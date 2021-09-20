/*
 * Copyright (C) 2020-2021 Fanout, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

use crate::app::ListenConfig;
use crate::arena;
use crate::buffer::TmpBuffer;
use crate::channel;
use crate::connection::{
    ServerReqConnection, ServerState, ServerStreamConnection, ServerStreamSharedData, Shutdown,
    Want, ZhttpSender,
};
use crate::event;
use crate::executor::{Executor, Spawner};
use crate::future::{
    event_wait, select_2, select_3, select_4, select_6, select_option, AsyncLocalReceiver,
    AsyncLocalSender, AsyncReceiver, AsyncSleep, Select2, Select3, Select4, Select6,
};
use crate::list;
use crate::listener::Listener;
use crate::pin_mut;
use crate::reactor::Reactor;
use crate::tls::{IdentityCache, TlsAcceptor, TlsStream};
use crate::tnetstring;
use crate::zhttppacket;
use crate::zhttpsocket;
use crate::zmq::SpecInfo;
use arrayvec::{ArrayString, ArrayVec};
use log::{debug, error, info, warn};
use mio;
use mio::net::{TcpListener, TcpSocket, TcpStream};
use mio::unix::SourceFd;
use slab::Slab;
use std::cell::{Cell, RefCell};
use std::io;
use std::io::{Read, Write};
use std::net::SocketAddr;
use std::os::unix::io::{FromRawFd, IntoRawFd};
use std::path::Path;
use std::pin::Pin;
use std::rc::Rc;
use std::str;
use std::str::FromStr;
use std::sync::mpsc;
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

const RESP_SENDER_BOUND: usize = 1;

// we read and process each response message one at a time, wrapping it in an
// rc, and sending it to connections via channels. on the other side of each
// channel, the message is received and processed immediately. this means the
// max number of messages retained per connection is the channel bound per
// connection
pub const MSG_RETAINED_PER_CONNECTION_MAX: usize = RESP_SENDER_BOUND;

// the max number of messages retained outside of connections is one per
// handle we read from (req and stream), in preparation for sending to any
// connections
pub const MSG_RETAINED_PER_WORKER_MAX: usize = 2;

// run x1
// accept_task x2
// req_handle_task x1
// stream_handle_task x1
// keep_alives_task x1
const WORKER_NON_CONNECTION_TASKS_MAX: usize = 10;

// note: individual tasks are not (and must not be) capped to this number.
// this is because accept_task makes a registration for every connection
// task, which means each instance of accept_task could end up making
// thousands of registrations. however, such registrations are associated
// with the spawning of connection_task, so we can still estimate
// registrations relative to the number of tasks
const REGISTRATIONS_PER_TASK_MAX: usize = 32;

const REACTOR_BUDGET: u32 = 100;

const KEEP_ALIVE_TIMEOUT_MS: usize = 45_000;
const KEEP_ALIVE_BATCH_MS: usize = 100;
const KEEP_ALIVE_INTERVAL: Duration = Duration::from_millis(KEEP_ALIVE_BATCH_MS as u64);
const KEEP_ALIVE_BATCHES: usize = KEEP_ALIVE_TIMEOUT_MS / KEEP_ALIVE_BATCH_MS;
const BULK_PACKET_SIZE_MAX: usize = 65_000;

fn get_addr_and_offset(msg: &[u8]) -> Result<(&str, usize), ()> {
    let mut pos = None;
    for (i, b) in msg.iter().enumerate() {
        if *b == b' ' {
            pos = Some(i);
            break;
        }
    }

    let pos = match pos {
        Some(pos) => pos,
        None => return Err(()),
    };

    let addr = match str::from_utf8(&msg[..pos]) {
        Ok(addr) => addr,
        Err(_) => return Err(()),
    };

    Ok((addr, pos + 1))
}

fn get_key(id: &[u8]) -> Result<usize, ()> {
    let mut start = None;
    let mut end = None;

    for (i, b) in id.iter().enumerate() {
        if *b == b'-' {
            if start.is_none() {
                start = Some(i + 1);
            } else {
                end = Some(i);
                break;
            }
        }
    }

    let start = match start {
        Some(start) => start,
        None => return Err(()),
    };

    let end = match end {
        Some(end) => end,
        None => return Err(()),
    };

    let key = match str::from_utf8(&id[start..end]) {
        Ok(key) => key,
        Err(_) => return Err(()),
    };

    let key = match key.parse() {
        Ok(key) => key,
        Err(_) => return Err(()),
    };

    Ok(key)
}

fn set_socket_opts(stream: TcpStream) -> TcpStream {
    if let Err(e) = stream.set_nodelay(true) {
        error!("set nodelay failed: {:?}", e);
    }

    let socket = unsafe { TcpSocket::from_raw_fd(stream.into_raw_fd()) };

    if let Err(e) = socket.set_keepalive(true) {
        error!("set keepalive failed: {:?}", e);
    }

    unsafe { TcpStream::from_raw_fd(socket.into_raw_fd()) }
}

fn local_channel<T>(
    bound: usize,
    max_senders: usize,
) -> (channel::LocalSender<T>, channel::LocalReceiver<T>) {
    let (s, r) = channel::local_channel(
        bound,
        max_senders,
        &Reactor::current().unwrap().local_registration_memory(),
    );

    (s, r)
}

fn async_local_channel<T>(
    bound: usize,
    max_senders: usize,
) -> (AsyncLocalSender<T>, AsyncLocalReceiver<T>) {
    let (s, r) = local_channel(bound, max_senders);

    let s = AsyncLocalSender::new(s);
    let r = AsyncLocalReceiver::new(r);

    (s, r)
}

fn gen_id(id: usize, ckey: usize, next_cid: &mut u32) -> ArrayString<[u8; 32]> {
    let mut buf = [0; 32];
    let mut c = io::Cursor::new(&mut buf[..]);

    write!(&mut c, "{}-{}-{:x}", id, ckey, next_cid).unwrap();

    let size = c.position() as usize;

    let s = str::from_utf8(&buf[..size]).unwrap();

    *next_cid += 1;

    ArrayString::from_str(s).unwrap()
}

impl Shutdown for TcpStream {
    fn shutdown(&mut self) -> Result<(), io::Error> {
        Ok(())
    }
}

impl Shutdown for TlsStream {
    fn shutdown(&mut self) -> Result<(), io::Error> {
        self.shutdown()
    }
}

impl ZhttpSender for channel::LocalSender<zmq::Message> {
    fn can_send_to(&self) -> bool {
        // req mode doesn't use this
        unimplemented!();
    }

    fn send(&mut self, message: zmq::Message) -> Result<(), zhttpsocket::SendError> {
        match self.try_send(message) {
            Ok(()) => Ok(()),
            Err(mpsc::TrySendError::Full(msg)) => Err(zhttpsocket::SendError::Full(msg)),
            Err(mpsc::TrySendError::Disconnected(_)) => Err(zhttpsocket::SendError::Io(
                io::Error::from(io::ErrorKind::BrokenPipe),
            )),
        }
    }

    fn send_to(
        &mut self,
        _addr: &[u8],
        _message: zmq::Message,
    ) -> Result<(), zhttpsocket::SendError> {
        // req mode doesn't use this
        unimplemented!();
    }
}

struct StreamLocalSenders {
    out: channel::LocalSender<zmq::Message>,
    out_stream: channel::LocalSender<(ArrayVec<[u8; 64]>, zmq::Message)>,
    out_stream_can_write: Cell<bool>,
}

impl StreamLocalSenders {
    fn new(
        out: channel::LocalSender<zmq::Message>,
        out_stream: channel::LocalSender<(ArrayVec<[u8; 64]>, zmq::Message)>,
    ) -> Self {
        Self {
            out,
            out_stream,
            out_stream_can_write: Cell::new(true),
        }
    }

    fn set_out_stream_can_write(&self) {
        self.out_stream_can_write.set(true);
    }
}

impl ZhttpSender for StreamLocalSenders {
    fn can_send_to(&self) -> bool {
        if self.out_stream_can_write.get() {
            if self.out_stream.check_send() {
                return true;
            }

            self.out_stream_can_write.set(false);
        }

        false
    }

    fn send(&mut self, message: zmq::Message) -> Result<(), zhttpsocket::SendError> {
        match self.out.try_send(message) {
            Ok(()) => Ok(()),
            Err(mpsc::TrySendError::Full(msg)) => Err(zhttpsocket::SendError::Full(msg)),
            Err(mpsc::TrySendError::Disconnected(_)) => Err(zhttpsocket::SendError::Io(
                io::Error::from(io::ErrorKind::BrokenPipe),
            )),
        }
    }

    fn send_to(
        &mut self,
        addr: &[u8],
        message: zmq::Message,
    ) -> Result<(), zhttpsocket::SendError> {
        let mut a = ArrayVec::new();
        if a.try_extend_from_slice(addr).is_err() {
            return Err(zhttpsocket::SendError::Io(io::Error::from(
                io::ErrorKind::InvalidInput,
            )));
        }

        match self.out_stream.try_send((a, message)) {
            Ok(()) => Ok(()),
            Err(mpsc::TrySendError::Full((_, msg))) => Err(zhttpsocket::SendError::Full(msg)),
            Err(mpsc::TrySendError::Disconnected(_)) => Err(zhttpsocket::SendError::Io(
                io::Error::from(io::ErrorKind::BrokenPipe),
            )),
        }
    }
}

enum ServerConnection {
    Req(ServerReqConnection, channel::LocalSender<zmq::Message>),
    Stream(ServerStreamConnection, StreamLocalSenders),
}

enum Stream {
    Plain(TcpStream),
    Tls(TlsStream),
}

impl Stream {
    fn get_tcp(&mut self) -> &mut TcpStream {
        match self {
            Stream::Plain(stream) => stream,
            Stream::Tls(stream) => stream.get_tcp(),
        }
    }
}

struct Connection {
    id: ArrayString<[u8; 32]>,
    stream: Stream,
    conn: ServerConnection,
    want: Want,
    timer: Option<Instant>,
    zreceiver: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, Option<u32>)>,
}

impl Connection {
    fn new_req(
        now: Instant,
        stream: Stream,
        peer_addr: SocketAddr,
        buffer_size: usize,
        body_buffer_size: usize,
        rb_tmp: &Rc<TmpBuffer>,
        timeout: Duration,
        sender: channel::LocalSender<zmq::Message>,
        zreceiver: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, Option<u32>)>,
    ) -> Self {
        let secure = match &stream {
            Stream::Plain(_) => false,
            Stream::Tls(_) => true,
        };

        Self {
            id: ArrayString::new(),
            stream,
            conn: ServerConnection::Req(
                ServerReqConnection::new(
                    now,
                    Some(peer_addr),
                    secure,
                    buffer_size,
                    body_buffer_size,
                    rb_tmp,
                    timeout,
                ),
                sender,
            ),
            want: Want::nothing(),
            timer: None,
            zreceiver,
        }
    }

    fn new_stream(
        now: Instant,
        stream: Stream,
        peer_addr: SocketAddr,
        buffer_size: usize,
        messages_max: usize,
        rb_tmp: &Rc<TmpBuffer>,
        timeout: Duration,
        senders: StreamLocalSenders,
        zreceiver: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, Option<u32>)>,
        shared: arena::Rc<ServerStreamSharedData>,
    ) -> Self {
        let secure = match &stream {
            Stream::Plain(_) => false,
            Stream::Tls(_) => true,
        };

        Self {
            id: ArrayString::new(),
            stream,
            conn: ServerConnection::Stream(
                ServerStreamConnection::new(
                    now,
                    Some(peer_addr),
                    secure,
                    buffer_size,
                    messages_max,
                    rb_tmp,
                    timeout,
                    shared,
                ),
                senders,
            ),
            want: Want::nothing(),
            timer: None,
            zreceiver,
        }
    }

    fn state(&self) -> ServerState {
        match &self.conn {
            ServerConnection::Req(conn, _) => conn.state(),
            ServerConnection::Stream(conn, _) => conn.state(),
        }
    }

    fn get_tcp(&mut self) -> &mut TcpStream {
        self.stream.get_tcp()
    }

    fn set_sock_readable(&mut self) {
        match &mut self.conn {
            ServerConnection::Req(conn, _) => conn.set_sock_readable(),
            ServerConnection::Stream(conn, _) => conn.set_sock_readable(),
        }
    }

    fn set_out_stream_can_write(&self) {
        match &self.conn {
            ServerConnection::Req(_, _) => panic!("not stream conn"),
            ServerConnection::Stream(_, senders) => senders.set_out_stream_can_write(),
        }
    }

    fn start(&mut self, id: &str) {
        self.id = ArrayString::from_str(id).unwrap();

        if let Stream::Tls(stream) = &mut self.stream {
            stream.set_id(id);
        }

        debug!("conn {}: assigning id", self.id);

        match &mut self.conn {
            ServerConnection::Req(conn, _) => conn.start(self.id.as_ref()),
            ServerConnection::Stream(conn, _) => conn.start(self.id.as_ref()),
        }
    }

    fn handle_packet(
        &mut self,
        now: Instant,
        zresp: &zhttppacket::Response,
        seq: Option<u32>,
    ) -> Result<(), ()> {
        if !zresp.ptype_str.is_empty() {
            debug!("conn {}: handle packet: {}", self.id, zresp.ptype_str);
        } else {
            debug!("conn {}: handle packet: (data)", self.id);
        }

        match &mut self.conn {
            ServerConnection::Req(conn, _) => {
                if let Err(e) = conn.apply_zhttp_response(zresp) {
                    debug!("conn {}: apply error {:?}", self.id, e);
                    return Err(());
                }
            }
            ServerConnection::Stream(conn, _) => {
                if let Err(e) = conn.apply_zhttp_response(now, zresp, seq) {
                    debug!("conn {}: apply error {:?}", self.id, e);
                    return Err(());
                }
            }
        }

        Ok(())
    }

    fn process(
        &mut self,
        now: Instant,
        instance_id: &str,
        packet_buf: &mut [u8],
        tmp_buf: &mut [u8],
    ) -> bool {
        while let Ok((resp, seq)) = self.zreceiver.try_recv() {
            // if error, keep going
            let _ = self.handle_packet(now, resp.get().get(), seq);
        }

        match &mut self.stream {
            Stream::Plain(stream) => Self::process_with_stream(
                &self.id,
                &mut self.conn,
                &mut self.want,
                stream,
                now,
                instance_id,
                packet_buf,
                tmp_buf,
            ),
            Stream::Tls(stream) => {
                let done = Self::process_with_stream(
                    &self.id,
                    &mut self.conn,
                    &mut self.want,
                    stream,
                    now,
                    instance_id,
                    packet_buf,
                    tmp_buf,
                );

                // for TLS, wake on all socket events
                if self.want.sock_read || self.want.sock_write {
                    self.want.sock_read = true;
                    self.want.sock_write = true;
                }

                done
            }
        }
    }

    fn process_with_stream<S: Read + Write + Shutdown>(
        id: &ArrayString<[u8; 32]>,
        conn: &mut ServerConnection,
        want: &mut Want,
        stream: &mut S,
        now: Instant,
        instance_id: &str,
        packet_buf: &mut [u8],
        tmp_buf: &mut [u8],
    ) -> bool {
        match conn {
            ServerConnection::Req(conn, sender) => {
                match conn.process(now, stream, sender, packet_buf) {
                    Ok(w) => *want = w,
                    Err(e) => {
                        debug!("conn {}: process error: {:?}", id, e);
                        return true;
                    }
                }

                if conn.state() == ServerState::Finished {
                    return true;
                }
            }
            ServerConnection::Stream(conn, senders) => {
                match conn.process(now, instance_id, stream, senders, packet_buf, tmp_buf) {
                    Ok(w) => *want = w,
                    Err(e) => {
                        debug!("conn {}: process error: {:?}", id, e);
                        return true;
                    }
                }

                if conn.state() == ServerState::Finished {
                    return true;
                }
            }
        }

        false
    }
}

struct BatchKey {
    addr_index: usize,
    nkey: usize,
}

struct BatchGroup<'a, 'b> {
    addr: &'b [u8],
    ids: arena::ReusableVecHandle<'b, zhttppacket::Id<'a>>,
}

impl<'a> BatchGroup<'a, '_> {
    fn addr(&self) -> &[u8] {
        self.addr
    }

    fn ids(&self) -> &[zhttppacket::Id<'a>] {
        &*self.ids
    }
}

struct Batch {
    nodes: Slab<list::Node<usize>>,
    addrs: Vec<(ArrayVec<[u8; 64]>, list::List)>,
    addr_index: usize,
    group_ids: arena::ReusableVec,
    last_group_ckeys: Vec<usize>,
}

impl Batch {
    fn new(capacity: usize) -> Self {
        Self {
            nodes: Slab::with_capacity(capacity),
            addrs: Vec::with_capacity(capacity),
            addr_index: 0,
            group_ids: arena::ReusableVec::new::<zhttppacket::Id>(capacity),
            last_group_ckeys: Vec::with_capacity(capacity),
        }
    }

    fn len(&self) -> usize {
        self.nodes.len()
    }

    fn capacity(&self) -> usize {
        self.nodes.capacity()
    }

    fn is_empty(&self) -> bool {
        self.nodes.is_empty()
    }

    fn clear(&mut self) {
        self.addrs.clear();
        self.nodes.clear();
        self.addr_index = 0;
    }

    fn add(&mut self, to_addr: &[u8], ckey: usize) -> Result<BatchKey, ()> {
        let mut pos = self.addrs.len();

        for (i, a) in self.addrs.iter().enumerate() {
            if a.0.as_ref() == to_addr {
                pos = i;
            }
        }

        if pos == self.addrs.len() {
            // connection limits to_addr to 64 so this is guaranteed to succeed
            let mut a = ArrayVec::new();
            a.try_extend_from_slice(to_addr).unwrap();

            self.addrs.push((a, list::List::default()));
        }

        if self.nodes.len() == self.nodes.capacity() {
            return Err(());
        }

        let nkey = self.nodes.insert(list::Node::new(ckey));
        self.addrs[pos].1.push_back(&mut self.nodes, nkey);

        Ok(BatchKey {
            addr_index: pos,
            nkey,
        })
    }

    fn remove(&mut self, key: BatchKey) {
        self.addrs[key.addr_index]
            .1
            .remove(&mut self.nodes, key.nkey);
        self.nodes.remove(key.nkey);
    }

    fn take_group<'a, 'b: 'a, F>(&'a mut self, get_ids: F) -> Option<BatchGroup>
    where
        F: Fn(usize) -> (&'b [u8], u32),
    {
        // find the next addr with items
        while self.addr_index < self.addrs.len() && self.addrs[self.addr_index].1.is_empty() {
            self.addr_index += 1;
        }

        // if all are empty, we're done
        if self.addr_index == self.addrs.len() {
            return None;
        }

        let (addr, keys) = &mut self.addrs[self.addr_index];

        self.last_group_ckeys.clear();

        let mut ids = self.group_ids.get_as_new();

        // get ids/seqs
        while ids.len() < zhttppacket::IDS_MAX {
            let nkey = match keys.pop_front(&mut self.nodes) {
                Some(nkey) => nkey,
                None => break,
            };

            let ckey = self.nodes[nkey].value;
            self.nodes.remove(nkey);

            let (id, seq) = get_ids(ckey);

            self.last_group_ckeys.push(ckey);
            ids.push(zhttppacket::Id { id, seq: Some(seq) });
        }

        Some(BatchGroup { addr, ids })
    }

    fn last_group_ckeys(&self) -> &[usize] {
        &self.last_group_ckeys
    }
}

enum BatchType {
    KeepAlive,
    Cancel,
}

struct ConnectionItem {
    id: ArrayString<[u8; 32]>,
    stop: Option<AsyncLocalSender<()>>,
    zreceiver_sender:
        Option<AsyncLocalSender<(arena::Rc<zhttppacket::OwnedResponse>, Option<u32>)>>,
    shared: Option<arena::Rc<ServerStreamSharedData>>,
    batch_key: Option<BatchKey>,
}

struct ConnectionItems {
    nodes: Slab<list::Node<ConnectionItem>>,
    next_cid: u32,
    batch: Batch,
}

impl ConnectionItems {
    fn new(capacity: usize, batch: Batch) -> Self {
        Self {
            nodes: Slab::with_capacity(capacity),
            next_cid: 0,
            batch,
        }
    }
}

struct ConnectionsInner {
    active: list::List,
    count: usize,
    max: usize,
}

struct Connections {
    items: Rc<RefCell<ConnectionItems>>,
    inner: RefCell<ConnectionsInner>,
}

impl Connections {
    fn new(items: Rc<RefCell<ConnectionItems>>, max: usize) -> Self {
        Self {
            items,
            inner: RefCell::new(ConnectionsInner {
                active: list::List::default(),
                count: 0,
                max,
            }),
        }
    }

    fn count(&self) -> usize {
        self.inner.borrow().count
    }

    fn max(&self) -> usize {
        self.inner.borrow().max
    }

    fn add(
        &self,
        worker_id: usize,
        stop: AsyncLocalSender<()>,
        zreceiver_sender: AsyncLocalSender<(arena::Rc<zhttppacket::OwnedResponse>, Option<u32>)>,
        shared: Option<arena::Rc<ServerStreamSharedData>>,
    ) -> Result<(usize, ArrayString<[u8; 32]>), ()> {
        let items = &mut *self.items.borrow_mut();
        let c = &mut *self.inner.borrow_mut();

        if items.nodes.len() == items.nodes.capacity() {
            return Err(());
        }

        let nkey = items.nodes.insert(list::Node::new(ConnectionItem {
            id: ArrayString::new(),
            stop: Some(stop),
            zreceiver_sender: Some(zreceiver_sender),
            shared,
            batch_key: None,
        }));

        items.nodes[nkey].value.id = gen_id(worker_id, nkey, &mut items.next_cid);

        c.active.push_back(&mut items.nodes, nkey);
        c.count += 1;

        Ok((nkey, items.nodes[nkey].value.id))
    }

    fn remove(&self, ckey: usize) {
        let nkey = ckey;

        let items = &mut *self.items.borrow_mut();
        let c = &mut *self.inner.borrow_mut();
        let ci = &mut items.nodes[nkey].value;

        // clear active keep alive
        if let Some(bkey) = ci.batch_key.take() {
            items.batch.remove(bkey);
        }

        c.active.remove(&mut items.nodes, nkey);
        c.count -= 1;

        items.nodes.remove(nkey);
    }

    fn regen_id(&self, worker_id: usize, ckey: usize) -> ArrayString<[u8; 32]> {
        let nkey = ckey;

        let items = &mut *self.items.borrow_mut();
        let ci = &mut items.nodes[nkey].value;

        // clear active keep alive
        if let Some(bkey) = ci.batch_key.take() {
            items.batch.remove(bkey);
        }

        ci.id = gen_id(worker_id, nkey, &mut items.next_cid);

        ci.id
    }

    fn check_id(&self, ckey: usize, id: &[u8]) -> bool {
        let nkey = ckey;

        let items = &*self.items.borrow();

        let ci = match items.nodes.get(nkey) {
            Some(n) => &n.value,
            None => return false,
        };

        ci.id.as_bytes() == id
    }

    fn take_zreceiver_sender(
        &self,
        ckey: usize,
    ) -> Option<AsyncLocalSender<(arena::Rc<zhttppacket::OwnedResponse>, Option<u32>)>> {
        let nkey = ckey;

        let items = &mut *self.items.borrow_mut();
        let ci = &mut items.nodes[nkey].value;

        ci.zreceiver_sender.take()
    }

    fn set_zreceiver_sender(
        &self,
        ckey: usize,
        sender: AsyncLocalSender<(arena::Rc<zhttppacket::OwnedResponse>, Option<u32>)>,
    ) {
        let nkey = ckey;

        let items = &mut *self.items.borrow_mut();
        let ci = &mut items.nodes[nkey].value;

        ci.zreceiver_sender = Some(sender);
    }

    fn stop_all<F>(&self, about_to_stop: F)
    where
        F: Fn(usize),
    {
        let items = &mut *self.items.borrow_mut();
        let cinner = &*self.inner.borrow_mut();

        let mut next = cinner.active.head;
        while let Some(nkey) = next {
            let n = &mut items.nodes[nkey];
            let ci = &mut n.value;

            about_to_stop(nkey);

            ci.stop = None;

            next = n.next;
        }
    }

    fn items_capacity(&self) -> usize {
        self.items.borrow().nodes.capacity()
    }

    fn is_item_stream(&self, ckey: usize) -> bool {
        let items = &*self.items.borrow();

        match items.nodes.get(ckey) {
            Some(n) => {
                let ci = &n.value;

                ci.shared.is_some()
            }
            None => false,
        }
    }

    fn batch_is_empty(&self) -> bool {
        let items = &*self.items.borrow();

        items.batch.is_empty()
    }

    fn batch_len(&self) -> usize {
        let items = &*self.items.borrow();

        items.batch.len()
    }

    fn batch_capacity(&self) -> usize {
        let items = &*self.items.borrow();

        items.batch.capacity()
    }

    fn batch_clear(&self) {
        let items = &mut *self.items.borrow_mut();

        items.batch.clear();
    }

    fn batch_add(&self, ckey: usize) -> Result<(), ()> {
        let items = &mut *self.items.borrow_mut();
        let ci = &mut items.nodes[ckey].value;
        let cshared = ci.shared.as_ref().unwrap().get();

        // only batch connections with known handler addresses
        let addr_ref = cshared.to_addr();
        let addr = match addr_ref.get() {
            Some(addr) => addr,
            None => return Err(()),
        };

        let bkey = items.batch.add(addr, ckey)?;

        ci.batch_key = Some(bkey);

        Ok(())
    }

    fn next_batch_message(
        &self,
        from: &str,
        btype: BatchType,
    ) -> Option<(usize, ArrayVec<[u8; 64]>, zmq::Message)> {
        let items = &mut *self.items.borrow_mut();
        let nodes = &mut items.nodes;
        let batch = &mut items.batch;

        while !batch.is_empty() {
            let group = batch
                .take_group(|ckey| {
                    let ci = &nodes[ckey].value;
                    let cshared = ci.shared.as_ref().unwrap().get();

                    (ci.id.as_bytes(), cshared.out_seq())
                })
                .unwrap();

            let count = group.ids().len();

            assert!(count <= zhttppacket::IDS_MAX);

            let zreq = zhttppacket::Request {
                from: from.as_bytes(),
                ids: group.ids(),
                multi: true,
                ptype: match btype {
                    BatchType::KeepAlive => zhttppacket::RequestPacket::KeepAlive,
                    BatchType::Cancel => zhttppacket::RequestPacket::Cancel,
                },
            };

            let mut data = [0; BULK_PACKET_SIZE_MAX];

            let size = match zreq.serialize(&mut data) {
                Ok(size) => size,
                Err(e) => {
                    error!(
                        "failed to serialize keep-alive packet with {} ids: {}",
                        zreq.ids.len(),
                        e
                    );
                    continue;
                }
            };

            let data = &data[..size];

            let mut addr = ArrayVec::<[u8; 64]>::new();
            if addr.try_extend_from_slice(group.addr()).is_err() {
                error!("failed to prepare addr");
                continue;
            }

            let msg = zmq::Message::from(data);

            drop(group);

            for &ckey in batch.last_group_ckeys() {
                let ci = &mut nodes[ckey].value;
                let cshared = ci.shared.as_ref().unwrap().get();

                cshared.inc_out_seq();
                ci.batch_key = None;
            }

            return Some((count, addr, msg));
        }

        None
    }
}

#[derive(Clone)]
struct ConnectionOpts {
    instance_id: Rc<String>,
    buffer_size: usize,
    timeout: Duration,
    rb_tmp: Rc<TmpBuffer>,
    packet_buf: Rc<RefCell<Vec<u8>>>,
    tmp_buf: Rc<RefCell<Vec<u8>>>,
}

struct ConnectionReqOpts {
    body_buffer_size: usize,
    sender: channel::LocalSender<zmq::Message>,
}

struct ConnectionStreamOpts {
    messages_max: usize,
    sender: channel::LocalSender<zmq::Message>,
    sender_stream: channel::LocalSender<(ArrayVec<[u8; 64]>, zmq::Message)>,
    stream_shared_mem: Rc<arena::RcMemory<ServerStreamSharedData>>,
}

enum ConnectionModeOpts {
    Req(ConnectionReqOpts),
    Stream(ConnectionStreamOpts),
}

struct Worker {
    thread: Option<thread::JoinHandle<()>>,
    stop: Option<channel::Sender<()>>,
}

impl Worker {
    fn new(
        instance_id: &str,
        id: usize,
        req_maxconn: usize,
        stream_maxconn: usize,
        buffer_size: usize,
        body_buffer_size: usize,
        messages_max: usize,
        req_timeout: Duration,
        stream_timeout: Duration,
        req_acceptor: channel::Receiver<(usize, TcpStream, SocketAddr)>,
        stream_acceptor: channel::Receiver<(usize, TcpStream, SocketAddr)>,
        req_acceptor_tls: &Vec<(bool, Option<String>)>,
        stream_acceptor_tls: &Vec<(bool, Option<String>)>,
        identities: &Arc<IdentityCache>,
        zsockman: &Arc<zhttpsocket::SocketManager>,
        handle_bound: usize,
    ) -> Self {
        debug!("worker {}: starting", id);

        let (stop, r_stop) = channel::channel(1);
        let (s_ready, ready) = channel::channel(1);

        let instance_id = String::from(instance_id);
        let req_acceptor_tls = req_acceptor_tls.clone();
        let stream_acceptor_tls = stream_acceptor_tls.clone();
        let identities = Arc::clone(identities);
        let zsockman = Arc::clone(zsockman);

        let thread = thread::spawn(move || {
            let maxconn = req_maxconn + stream_maxconn;

            // 1 task per connection, plus a handful of supporting tasks
            let tasks_max = maxconn + WORKER_NON_CONNECTION_TASKS_MAX;

            let registrations_max = REGISTRATIONS_PER_TASK_MAX * tasks_max;

            let reactor = Reactor::new(registrations_max);

            let executor = Executor::new(tasks_max);

            {
                let reactor = reactor.clone();

                executor.set_pre_poll(move || {
                    reactor.set_budget(Some(REACTOR_BUDGET));
                });
            }

            executor
                .spawn(Self::run(
                    r_stop,
                    s_ready,
                    instance_id,
                    id,
                    req_maxconn,
                    stream_maxconn,
                    buffer_size,
                    body_buffer_size,
                    messages_max,
                    req_timeout,
                    stream_timeout,
                    req_acceptor,
                    stream_acceptor,
                    req_acceptor_tls,
                    stream_acceptor_tls,
                    identities,
                    zsockman,
                    handle_bound,
                ))
                .unwrap();

            executor.run(|timeout| reactor.poll(timeout)).unwrap();

            debug!("worker {}: stopped", id);
        });

        ready.recv().unwrap();

        Self {
            thread: Some(thread),
            stop: Some(stop),
        }
    }

    async fn run(
        stop: channel::Receiver<()>,
        ready: channel::Sender<()>,
        instance_id: String,
        id: usize,
        req_maxconn: usize,
        stream_maxconn: usize,
        buffer_size: usize,
        body_buffer_size: usize,
        messages_max: usize,
        req_timeout: Duration,
        stream_timeout: Duration,
        req_acceptor: channel::Receiver<(usize, TcpStream, SocketAddr)>,
        stream_acceptor: channel::Receiver<(usize, TcpStream, SocketAddr)>,
        req_acceptor_tls: Vec<(bool, Option<String>)>,
        stream_acceptor_tls: Vec<(bool, Option<String>)>,
        identities: Arc<IdentityCache>,
        zsockman: Arc<zhttpsocket::SocketManager>,
        handle_bound: usize,
    ) {
        let executor = Executor::current().unwrap();
        let stop = AsyncReceiver::new(stop);
        let req_acceptor = AsyncReceiver::new(req_acceptor);
        let stream_acceptor = AsyncReceiver::new(stream_acceptor);

        debug!("worker {}: allocating buffers", id);

        let rb_tmp = Rc::new(TmpBuffer::new(buffer_size));

        // large enough to fit anything
        let packet_buf = Rc::new(RefCell::new(vec![0; buffer_size + body_buffer_size + 4096]));

        // same size as working buffers
        let tmp_buf = Rc::new(RefCell::new(vec![0; buffer_size]));

        let instance_id = Rc::new(instance_id);

        let ka_batch = (stream_maxconn + (KEEP_ALIVE_BATCHES - 1)) / KEEP_ALIVE_BATCHES;

        let batch = Batch::new(ka_batch);

        let conn_items = Rc::new(RefCell::new(ConnectionItems::new(
            req_maxconn + stream_maxconn,
            batch,
        )));

        let req_conns = Rc::new(Connections::new(conn_items.clone(), req_maxconn));
        let stream_conns = Rc::new(Connections::new(conn_items.clone(), stream_maxconn));

        let (req_accept_stop, r_req_accept_stop) = async_local_channel(1, 1);
        let (stream_accept_stop, r_stream_accept_stop) = async_local_channel(1, 1);
        let (req_handle_stop, r_req_handle_stop) = async_local_channel(1, 1);
        let (stream_handle_stop, r_stream_handle_stop) = async_local_channel(1, 1);
        let (keep_alives_stop, r_keep_alives_stop) = async_local_channel(1, 1);

        let (s_req_accept_done, req_accept_done) = async_local_channel(1, 1);
        let (s_stream_accept_done, stream_accept_done) = async_local_channel(1, 1);
        let (s_req_handle_done, req_handle_done) = async_local_channel(1, 1);
        let (s_stream_handle_done, stream_handle_done) = async_local_channel(1, 1);
        let (s_keep_alives_done, keep_alives_done) = async_local_channel(1, 1);

        // max_senders is 1 per connection + 1 for the accept task
        let (zreq_sender, zreq_receiver) = local_channel(handle_bound, req_maxconn + 1);

        // max_senders is 1 per connection + 1 for the accept task
        let (zstream_out_sender, zstream_out_receiver) =
            local_channel(handle_bound, stream_maxconn + 1);

        // max_senders is 1 per connection + 1 for the accept task + 1 for the handle task
        let (zstream_out_stream_sender, zstream_out_stream_receiver) =
            local_channel(handle_bound, stream_maxconn + 2);

        let zreq_receiver = AsyncLocalReceiver::new(zreq_receiver);
        let zstream_out_receiver = AsyncLocalReceiver::new(zstream_out_receiver);
        let zstream_out_stream_receiver = AsyncLocalReceiver::new(zstream_out_stream_receiver);

        let req_handle = zhttpsocket::AsyncClientReqHandle::new(
            zsockman.client_req_handle(format!("{}-", id).as_bytes()),
        );

        let stream_handle = zhttpsocket::AsyncClientStreamHandle::new(
            zsockman.client_stream_handle(format!("{}-", id).as_bytes()),
        );

        let stream_shared_mem = Rc::new(arena::RcMemory::new(stream_maxconn));

        executor
            .spawn(Self::accept_task(
                "req_accept",
                id,
                r_req_accept_stop,
                s_req_accept_done,
                req_acceptor,
                req_acceptor_tls,
                identities.clone(),
                executor.spawner(),
                req_conns.clone(),
                ConnectionOpts {
                    instance_id: instance_id.clone(),
                    buffer_size,
                    timeout: req_timeout,
                    rb_tmp: rb_tmp.clone(),
                    packet_buf: packet_buf.clone(),
                    tmp_buf: tmp_buf.clone(),
                },
                ConnectionModeOpts::Req(ConnectionReqOpts {
                    body_buffer_size,
                    sender: zreq_sender,
                }),
            ))
            .unwrap();

        {
            let zstream_out_stream_sender = zstream_out_stream_sender
                .try_clone(&Reactor::current().unwrap().local_registration_memory())
                .unwrap();

            executor
                .spawn(Self::accept_task(
                    "stream_accept",
                    id,
                    r_stream_accept_stop,
                    s_stream_accept_done,
                    stream_acceptor,
                    stream_acceptor_tls,
                    identities.clone(),
                    executor.spawner(),
                    stream_conns.clone(),
                    ConnectionOpts {
                        instance_id: instance_id.clone(),
                        buffer_size,
                        timeout: stream_timeout,
                        rb_tmp: rb_tmp.clone(),
                        packet_buf: packet_buf.clone(),
                        tmp_buf: tmp_buf.clone(),
                    },
                    ConnectionModeOpts::Stream(ConnectionStreamOpts {
                        messages_max,
                        sender: zstream_out_sender,
                        sender_stream: zstream_out_stream_sender,
                        stream_shared_mem,
                    }),
                ))
                .unwrap();
        }

        executor
            .spawn(Self::req_handle_task(
                id,
                r_req_handle_stop,
                s_req_handle_done,
                zreq_receiver,
                req_handle,
                req_maxconn,
                req_conns.clone(),
            ))
            .unwrap();

        executor
            .spawn(Self::stream_handle_task(
                id,
                r_stream_handle_stop,
                s_stream_handle_done,
                instance_id.clone(),
                zstream_out_receiver,
                zstream_out_stream_receiver,
                stream_handle,
                stream_maxconn,
                stream_conns.clone(),
            ))
            .unwrap();

        executor
            .spawn(Self::keep_alives_task(
                id,
                r_keep_alives_stop,
                s_keep_alives_done,
                instance_id.clone(),
                zstream_out_stream_sender,
                stream_conns.clone(),
            ))
            .unwrap();

        debug!("worker {}: started", id);

        ready.send(()).unwrap();
        drop(ready);

        // wait for stop
        let _ = stop.recv().await;

        // stop all tasks
        drop(req_accept_stop);
        drop(stream_accept_stop);
        drop(req_handle_stop);
        drop(stream_handle_stop);
        drop(keep_alives_stop);

        // wait for all to stop
        let _ = req_accept_done.recv().await;
        let _ = stream_accept_done.recv().await;
        let _ = req_handle_done.recv().await;
        let stream_handle = stream_handle_done.recv().await.unwrap();
        let _ = keep_alives_done.recv().await;

        // send cancels

        stream_conns.batch_clear();

        let mut next_cancel_index = 0;

        while next_cancel_index < stream_conns.items_capacity() {
            while stream_conns.batch_len() < stream_conns.batch_capacity()
                && next_cancel_index < stream_conns.items_capacity()
            {
                let key = next_cancel_index;

                next_cancel_index += 1;

                if stream_conns.is_item_stream(key) {
                    // ignore errors
                    let _ = stream_conns.batch_add(key);
                }
            }

            while let Some((count, addr, msg)) =
                stream_conns.next_batch_message(&instance_id, BatchType::Cancel)
            {
                debug!("worker {}: sending cancels for {} sessions", id, count);

                stream_handle.send_to_addr(addr, msg).await.unwrap();
            }

            stream_conns.batch_clear();
        }
    }

    async fn accept_task(
        name: &str,
        id: usize,
        stop: AsyncLocalReceiver<()>,
        _done: AsyncLocalSender<()>,
        acceptor: AsyncReceiver<(usize, TcpStream, SocketAddr)>,
        acceptor_tls: Vec<(bool, Option<String>)>,
        identities: Arc<IdentityCache>,
        spawner: Spawner,
        conns: Rc<Connections>,
        opts: ConnectionOpts,
        mode_opts: ConnectionModeOpts,
    ) {
        let mut tls_acceptors = Vec::new();

        for config in acceptor_tls {
            if config.0 {
                let default_cert = config.1.as_ref().map(|s| s.as_str());
                tls_acceptors.push(Some(TlsAcceptor::new(&identities, default_cert)));
            } else {
                tls_acceptors.push(None);
            }
        }

        let reactor = Reactor::current().unwrap();

        // bound is 1 per connection, so all connections can indicate done at once
        // max_senders is 1 per connection + 1 for this task
        let (s_cdone, cdone) = channel::local_channel(
            conns.max(),
            conns.max() + 1,
            &reactor.local_registration_memory(),
        );

        let cdone = AsyncLocalReceiver::new(cdone);

        debug!("worker {}: task started: {}", id, name);

        loop {
            let acceptor_recv = if conns.count() < conns.max() {
                Some(acceptor.recv())
            } else {
                None
            };

            let (pos, stream, peer_addr) =
                match select_3(stop.recv(), cdone.recv(), select_option(acceptor_recv)).await {
                    // stop.recv
                    Select3::R1(_) => break,
                    // cdone.recv
                    Select3::R2(result) => match result {
                        Ok(cid) => {
                            conns.remove(cid);
                            continue;
                        }
                        Err(e) => panic!("cdone channel error: {}", e),
                    },
                    // acceptor_recv
                    Select3::R3(result) => match result {
                        Ok(ret) => ret,
                        Err(_) => continue, // ignore errors
                    },
                };

            let stream = set_socket_opts(stream);

            let stream = match &tls_acceptors[pos] {
                Some(tls_acceptor) => match tls_acceptor.accept(stream) {
                    Ok(stream) => {
                        debug!("worker {}: tls accept", id);

                        Stream::Tls(stream)
                    }
                    Err(e) => {
                        error!("worker {}: tls accept: {}", id, e);
                        break;
                    }
                },
                None => {
                    debug!("worker {}: plain accept", id);

                    Stream::Plain(stream)
                }
            };

            let (cstop, r_cstop) = async_local_channel(1, 1);

            let s_cdone = s_cdone
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            let (ckey, conn_id, zreceiver, mode_opts, shared) = match &mode_opts {
                ConnectionModeOpts::Req(req_opts) => {
                    let zreq_sender = req_opts
                        .sender
                        .try_clone(&reactor.local_registration_memory())
                        .unwrap();

                    let (zreq_receiver_sender, zreq_receiver) = local_channel(RESP_SENDER_BOUND, 1);

                    let zreq_receiver_sender = AsyncLocalSender::new(zreq_receiver_sender);

                    let (ckey, conn_id) = conns.add(id, cstop, zreq_receiver_sender, None).unwrap();

                    debug!(
                        "worker {}: req conn starting {} {}/{}",
                        id,
                        ckey,
                        conns.count(),
                        conns.max(),
                    );

                    let mode_opts = ConnectionModeOpts::Req(ConnectionReqOpts {
                        body_buffer_size: req_opts.body_buffer_size,
                        sender: zreq_sender,
                    });

                    (ckey, conn_id, zreq_receiver, mode_opts, None)
                }
                ConnectionModeOpts::Stream(stream_opts) => {
                    let zstream_out_sender = stream_opts
                        .sender
                        .try_clone(&reactor.local_registration_memory())
                        .unwrap();
                    let zstream_out_stream_sender = stream_opts
                        .sender_stream
                        .try_clone(&reactor.local_registration_memory())
                        .unwrap();

                    let (zstream_receiver_sender, zstream_receiver) =
                        local_channel(RESP_SENDER_BOUND, 1);

                    let zstream_receiver_sender = AsyncLocalSender::new(zstream_receiver_sender);

                    let shared = arena::Rc::new(
                        ServerStreamSharedData::new(),
                        &stream_opts.stream_shared_mem,
                    )
                    .unwrap();

                    let (ckey, conn_id) = conns
                        .add(
                            id,
                            cstop,
                            zstream_receiver_sender,
                            Some(arena::Rc::clone(&shared)),
                        )
                        .unwrap();

                    debug!(
                        "worker {}: stream conn starting {} {}/{}",
                        id,
                        ckey,
                        conns.count(),
                        conns.max(),
                    );

                    let mode_opts = ConnectionModeOpts::Stream(ConnectionStreamOpts {
                        messages_max: stream_opts.messages_max,
                        sender: zstream_out_sender,
                        sender_stream: zstream_out_stream_sender,
                        stream_shared_mem: stream_opts.stream_shared_mem.clone(),
                    });

                    (ckey, conn_id, zstream_receiver, mode_opts, Some(shared))
                }
            };

            if spawner
                .spawn(Self::connection_task(
                    r_cstop,
                    s_cdone,
                    id,
                    ckey,
                    conn_id,
                    stream,
                    peer_addr,
                    zreceiver,
                    conns.clone(),
                    opts.clone(),
                    mode_opts,
                    shared,
                ))
                .is_err()
            {
                // this should never happen. we only accept a connection if
                // we know we can spawn
                panic!("failed to spawn connection_task");
            }
        }

        drop(s_cdone);

        conns.stop_all(|ckey| debug!("worker {}: stopping {}", id, ckey));

        while cdone.recv().await.is_ok() {}

        debug!("worker {}: task stopped: {}", id, name);
    }

    async fn req_handle_task(
        id: usize,
        stop: AsyncLocalReceiver<()>,
        _done: AsyncLocalSender<()>,
        zreq_receiver: AsyncLocalReceiver<zmq::Message>,
        req_handle: zhttpsocket::AsyncClientReqHandle,
        req_maxconn: usize,
        conns: Rc<Connections>,
    ) {
        let msg_retained_max = 1 + (MSG_RETAINED_PER_CONNECTION_MAX * req_maxconn);

        let req_scratch_mem = Rc::new(arena::RcMemory::new(msg_retained_max));
        let req_resp_mem = Rc::new(arena::RcMemory::new(msg_retained_max));

        debug!("worker {}: task started: req_handle", id);

        let handle_send = None;

        pin_mut!(handle_send);

        'main: loop {
            let receiver_recv = if handle_send.is_none() {
                Some(zreq_receiver.recv())
            } else {
                None
            };

            let handle_recv = req_handle.recv();

            pin_mut!(handle_recv);

            match select_4(
                stop.recv(),
                select_option(receiver_recv),
                select_option(handle_send.as_mut().as_pin_mut()),
                handle_recv,
            )
            .await
            {
                // stop.recv
                Select4::R1(_) => break,
                // receiver_recv
                Select4::R2(result) => match result {
                    Ok(msg) => handle_send.set(Some(req_handle.send(msg))),
                    Err(mpsc::RecvError) => break, // this can happen if accept+conns end first
                },
                // handle_send
                Select4::R3(result) => {
                    handle_send.set(None);

                    if let Err(e) = result {
                        error!("req send error: {}", e);
                    }
                }
                // req_handle.recv
                Select4::R4(result) => match result {
                    Ok(msg) => {
                        let scratch = arena::Rc::new(
                            RefCell::new(zhttppacket::ResponseScratch::new()),
                            &req_scratch_mem,
                        )
                        .unwrap();

                        let zresp = match zhttppacket::OwnedResponse::parse(msg, 0, scratch) {
                            Ok(zresp) => zresp,
                            Err(e) => {
                                warn!("worker {}: zhttp parse error: {}", id, e);
                                continue;
                            }
                        };

                        let zresp = arena::Rc::new(zresp, &req_resp_mem).unwrap();

                        let mut count = 0;

                        for id in zresp.get().get().ids {
                            let key = match get_key(&id.id) {
                                Ok(key) => key,
                                Err(_) => continue,
                            };

                            if !conns.check_id(key, id.id) {
                                // key found but cid mismatch
                                continue;
                            }

                            if let Some(sender) = conns.take_zreceiver_sender(key) {
                                match select_2(
                                    stop.recv(),
                                    sender.send((arena::Rc::clone(&zresp), None)),
                                )
                                .await
                                {
                                    Select2::R1(_) => break 'main,
                                    Select2::R2(result) => match result {
                                        Ok(()) => count += 1,
                                        Err(_) => {}
                                    },
                                }

                                // need to re-check for validity after await
                                if conns.check_id(key, id.id) {
                                    conns.set_zreceiver_sender(key, sender);
                                }
                            }
                        }

                        debug!("worker {}: queued zmq message for {} conns", id, count);
                    }
                    Err(e) => panic!("worker {}: handle read error {}", id, e),
                },
            }
        }

        debug!("worker {}: task stopped: req_handle", id);
    }

    async fn stream_handle_task(
        id: usize,
        stop: AsyncLocalReceiver<()>,
        done: AsyncLocalSender<zhttpsocket::AsyncClientStreamHandle>,
        instance_id: Rc<String>,
        zstream_out_receiver: AsyncLocalReceiver<zmq::Message>,
        zstream_out_stream_receiver: AsyncLocalReceiver<(ArrayVec<[u8; 64]>, zmq::Message)>,
        stream_handle: zhttpsocket::AsyncClientStreamHandle,
        stream_maxconn: usize,
        conns: Rc<Connections>,
    ) {
        let msg_retained_max = 1 + (MSG_RETAINED_PER_CONNECTION_MAX * stream_maxconn);

        let stream_scratch_mem = Rc::new(arena::RcMemory::new(msg_retained_max));
        let stream_resp_mem = Rc::new(arena::RcMemory::new(msg_retained_max));

        debug!("worker {}: task started: stream_handle", id);

        {
            let handle_send_to_any = None;
            let handle_send_to_addr = None;

            pin_mut!(handle_send_to_any, handle_send_to_addr);

            'main: loop {
                let receiver_recv = if handle_send_to_any.is_none() {
                    Some(zstream_out_receiver.recv())
                } else {
                    None
                };

                let stream_receiver_recv = if handle_send_to_addr.is_none() {
                    Some(zstream_out_stream_receiver.recv())
                } else {
                    None
                };

                let handle_recv = stream_handle.recv();

                pin_mut!(handle_recv);

                match select_6(
                    stop.recv(),
                    select_option(receiver_recv),
                    select_option(handle_send_to_any.as_mut().as_pin_mut()),
                    select_option(stream_receiver_recv),
                    select_option(handle_send_to_addr.as_mut().as_pin_mut()),
                    handle_recv,
                )
                .await
                {
                    // stop.recv
                    Select6::R1(_) => break,
                    // receiver_recv
                    Select6::R2(result) => match result {
                        Ok(msg) => handle_send_to_any.set(Some(stream_handle.send_to_any(msg))),
                        Err(mpsc::RecvError) => break, // this can happen if accept+conns end first
                    },
                    // handle_send_to_any
                    Select6::R3(result) => {
                        handle_send_to_any.set(None);

                        if let Err(e) = result {
                            error!("stream out send error: {}", e);
                        }
                    }
                    // stream_receiver_recv
                    Select6::R4(result) => match result {
                        Ok((addr, msg)) => {
                            handle_send_to_addr.set(Some(stream_handle.send_to_addr(addr, msg)))
                        }
                        Err(mpsc::RecvError) => break, // this can happen if accept+conns end first
                    },
                    // handle_send_to_addr
                    Select6::R5(result) => {
                        handle_send_to_addr.set(None);

                        if let Err(e) = result {
                            error!("stream out stream send error: {}", e);
                        }
                    }
                    // stream_handle.recv
                    Select6::R6(result) => match result {
                        Ok(msg) => {
                            let msg_data = &msg.get()[..];

                            let (addr, offset) = match get_addr_and_offset(msg_data) {
                                Ok(ret) => ret,
                                Err(_) => {
                                    warn!("worker {}: packet has unexpected format", id);
                                    continue;
                                }
                            };

                            if addr != &*instance_id {
                                warn!("worker {}: packet not for us", id);
                                continue;
                            }

                            let scratch = arena::Rc::new(
                                RefCell::new(zhttppacket::ResponseScratch::new()),
                                &stream_scratch_mem,
                            )
                            .unwrap();

                            let zresp =
                                match zhttppacket::OwnedResponse::parse(msg, offset, scratch) {
                                    Ok(zresp) => zresp,
                                    Err(e) => {
                                        warn!("worker {}: zhttp parse error: {}", id, e);
                                        continue;
                                    }
                                };

                            let zresp = arena::Rc::new(zresp, &stream_resp_mem).unwrap();

                            let mut count = 0;

                            for id in zresp.get().get().ids {
                                let key = match get_key(&id.id) {
                                    Ok(key) => key,
                                    Err(_) => continue,
                                };

                                if !conns.check_id(key, id.id) {
                                    // key found but cid mismatch
                                    continue;
                                }

                                if let Some(sender) = conns.take_zreceiver_sender(key) {
                                    match select_2(
                                        stop.recv(),
                                        sender.send((arena::Rc::clone(&zresp), id.seq)),
                                    )
                                    .await
                                    {
                                        Select2::R1(_) => break 'main,
                                        Select2::R2(result) => match result {
                                            Ok(()) => count += 1,
                                            Err(_) => {}
                                        },
                                    }

                                    // need to re-check for validity after await
                                    if conns.check_id(key, id.id) {
                                        conns.set_zreceiver_sender(key, sender);
                                    }
                                }
                            }

                            debug!("worker {}: queued zmq message for {} conns", id, count);
                        }
                        Err(e) => panic!("worker {}: handle read error {}", id, e),
                    },
                }
            }
        }

        // give the handle back
        done.send(stream_handle).await.unwrap();

        debug!("worker {}: task stopped: stream_handle", id);
    }

    async fn connection_task(
        stop: AsyncLocalReceiver<()>,
        done: channel::LocalSender<usize>,
        worker_id: usize,
        ckey: usize,
        mut cid: ArrayString<[u8; 32]>,
        mut stream: Stream,
        peer_addr: SocketAddr,
        zreceiver: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, Option<u32>)>,
        conns: Rc<Connections>,
        opts: ConnectionOpts,
        mode_opts: ConnectionModeOpts,
        shared: Option<arena::Rc<ServerStreamSharedData>>,
    ) {
        let done = AsyncLocalSender::new(done);

        debug!("worker {}: task started: connection-{}", worker_id, ckey);

        let reactor = Reactor::current().unwrap();

        let stream_registration = reactor
            .register_io(
                stream.get_tcp(),
                mio::Interest::READABLE | mio::Interest::WRITABLE,
            )
            .unwrap();

        let zreceiver_registration = reactor
            .register_custom_local(zreceiver.get_read_registration(), mio::Interest::READABLE)
            .unwrap();

        let (zsender1_registration, zsender2_registration, mut c) = {
            match mode_opts {
                ConnectionModeOpts::Req(req_opts) => {
                    let zsender_registration = reactor
                        .register_custom_local(
                            req_opts.sender.get_write_registration(),
                            mio::Interest::WRITABLE,
                        )
                        .unwrap();

                    let c = Connection::new_req(
                        reactor.now(),
                        stream,
                        peer_addr,
                        opts.buffer_size,
                        req_opts.body_buffer_size,
                        &opts.rb_tmp,
                        opts.timeout,
                        req_opts.sender,
                        zreceiver,
                    );

                    (zsender_registration, None, c)
                }
                ConnectionModeOpts::Stream(stream_opts) => {
                    let zsender_registration = reactor
                        .register_custom_local(
                            stream_opts.sender.get_write_registration(),
                            mio::Interest::WRITABLE,
                        )
                        .unwrap();

                    let zsender_stream_registration = reactor
                        .register_custom_local(
                            stream_opts.sender_stream.get_write_registration(),
                            mio::Interest::WRITABLE,
                        )
                        .unwrap();

                    let c = Connection::new_stream(
                        reactor.now(),
                        stream,
                        peer_addr,
                        opts.buffer_size,
                        stream_opts.messages_max,
                        &opts.rb_tmp,
                        opts.timeout,
                        StreamLocalSenders::new(stream_opts.sender, stream_opts.sender_stream),
                        zreceiver,
                        shared.unwrap(),
                    );

                    (zsender_registration, Some(zsender_stream_registration), c)
                }
            }
        };

        let using_tls = match &c.stream {
            Stream::Tls(_) => true,
            _ => false,
        };

        c.start(cid.as_ref());

        let mut sleep = None;

        'main: loop {
            debug!("conn {}: process", c.id);

            if c.process(
                reactor.now(),
                &opts.instance_id,
                &mut *opts.packet_buf.borrow_mut(),
                &mut *opts.tmp_buf.borrow_mut(),
            ) {
                break;
            }

            if c.state() == ServerState::Ready {
                cid = conns.regen_id(worker_id, ckey);
                c.start(cid.as_ref());
                continue;
            }

            if let Some(want_exp_time) = c.want.timeout {
                let mut add = false;

                if let Some(exp_time) = c.timer {
                    if want_exp_time != exp_time {
                        add = true;
                    }
                } else {
                    add = true;
                }

                if add {
                    sleep = Some(AsyncSleep::new(want_exp_time));
                    c.timer = Some(want_exp_time);
                }
            } else {
                if c.timer.is_some() {
                    sleep = None;
                    c.timer = None;
                }
            }

            loop {
                let stream_wait = if c.want.sock_read || c.want.sock_write {
                    let interest = if c.want.sock_read && c.want.sock_write {
                        mio::Interest::READABLE | mio::Interest::WRITABLE
                    } else if c.want.sock_read {
                        mio::Interest::READABLE
                    } else {
                        mio::Interest::WRITABLE
                    };

                    Some(event_wait(&stream_registration, interest))
                } else {
                    None
                };

                // always read zhttp response packets so they can be applied immediately,
                // even if c.want.zhttp_read is false
                let zreceiver_wait = event_wait(&zreceiver_registration, mio::Interest::READABLE);

                let zsender1_wait = if c.want.zhttp_write {
                    Some(event_wait(&zsender1_registration, mio::Interest::WRITABLE))
                } else {
                    None
                };

                let zsender2_wait = if let Some(reg) = &zsender2_registration {
                    if c.want.zhttp_write_to {
                        Some(event_wait(reg, mio::Interest::WRITABLE))
                    } else {
                        None
                    }
                } else {
                    None
                };

                let sleep = if let Some(sleep) = &mut sleep {
                    Some(sleep.sleep())
                } else {
                    None
                };

                pin_mut!(
                    stream_wait,
                    zreceiver_wait,
                    zsender1_wait,
                    zsender2_wait,
                    sleep
                );

                match select_6(
                    stop.recv(),
                    select_option(stream_wait.as_pin_mut()),
                    zreceiver_wait,
                    select_option(zsender1_wait.as_pin_mut()),
                    select_option(zsender2_wait.as_pin_mut()),
                    select_option(sleep.as_pin_mut()),
                )
                .await
                {
                    // stop.recv
                    Select6::R1(_) => break 'main,
                    // stream_wait
                    Select6::R2(readiness) => {
                        stream_registration.set_ready(false);

                        let readable = readiness.is_readable();
                        let writable = readiness.is_writable();

                        if readable {
                            debug!("conn {}: sock read event", c.id);
                        }

                        // for TLS, set readable on all events
                        if readable || using_tls {
                            c.set_sock_readable();
                        }

                        if writable {
                            debug!("conn {}: sock write event", c.id);
                        }

                        if (readable && c.want.sock_read) || (writable && c.want.sock_write) {
                            break;
                        }
                    }
                    // zreceiver_wait
                    Select6::R3(_) => {
                        debug!("conn {}: zreceiver event", c.id);
                        zreceiver_registration.set_ready(false);
                        break;
                    }
                    // zsender1_wait
                    Select6::R4(_) => {
                        debug!("conn {}: zsender1 event", c.id);
                        zsender1_registration.set_ready(false);
                        break;
                    }
                    // zsender2_wait
                    Select6::R5(_) => {
                        debug!("conn {}: zsender2 event", c.id);
                        zsender2_registration.as_ref().unwrap().set_ready(false);
                        c.set_out_stream_can_write();
                        break;
                    }
                    // sleep
                    Select6::R6(_) => {
                        debug!("conn {}: timeout", c.id);
                        break;
                    }
                }
            }
        }

        stream_registration.deregister_io(c.get_tcp()).unwrap();

        done.send(ckey).await.unwrap();

        debug!("worker {}: task stopped: connection-{}", worker_id, ckey);
    }

    async fn keep_alives_task(
        id: usize,
        stop: AsyncLocalReceiver<()>,
        _done: AsyncLocalSender<()>,
        instance_id: Rc<String>,
        sender: channel::LocalSender<(ArrayVec<[u8; 64]>, zmq::Message)>,
        conns: Rc<Connections>,
    ) {
        debug!("worker {}: task started: keep_alives", id);

        let reactor = Reactor::current().unwrap();

        let mut keep_alive_count = 0;
        let mut next_keep_alive_time = reactor.now() + KEEP_ALIVE_INTERVAL;
        let mut next_keep_alive = AsyncSleep::new(next_keep_alive_time);
        let mut next_keep_alive_index = 0;

        let sender_registration = reactor
            .register_custom_local(sender.get_write_registration(), mio::Interest::WRITABLE)
            .unwrap();

        sender_registration.set_readiness(Some(mio::Interest::WRITABLE));

        'main: loop {
            while conns.batch_is_empty() {
                // wait for next keep alive time
                match select_2(stop.recv(), next_keep_alive.sleep()).await {
                    Select2::R1(_) => break 'main,
                    Select2::R2(_) => {}
                }

                for _ in 0..conns.batch_capacity() {
                    if next_keep_alive_index >= conns.items_capacity() {
                        break;
                    }

                    let key = next_keep_alive_index;

                    next_keep_alive_index += 1;

                    if conns.is_item_stream(key) {
                        // ignore errors
                        let _ = conns.batch_add(key);
                    }
                }

                keep_alive_count += 1;

                if keep_alive_count >= KEEP_ALIVE_BATCHES {
                    keep_alive_count = 0;
                    next_keep_alive_index = 0;
                }

                // keep steady pace
                next_keep_alive_time += KEEP_ALIVE_INTERVAL;
                next_keep_alive = AsyncSleep::new(next_keep_alive_time);
            }

            let wait = event_wait(&sender_registration, mio::Interest::WRITABLE);
            pin_mut!(wait);

            match select_2(stop.recv(), wait).await {
                Select2::R1(_) => break,
                Select2::R2(_) => {}
            }

            if !sender.check_send() {
                // if check_send returns false, we'll be on the waitlist for a notification
                sender_registration.clear_readiness(mio::Interest::WRITABLE);
                continue;
            }

            // if check_send returns true, we are guaranteed to be able to send

            match conns.next_batch_message(&instance_id, BatchType::KeepAlive) {
                Some((count, addr, msg)) => {
                    debug!("worker {}: sending keep alives for {} sessions", id, count);

                    if let Err(e) = sender.try_send((addr, msg)) {
                        error!("zhttp write error: {}", e);
                    }
                }
                None => {
                    // this could happen if message construction failed
                    sender.cancel();
                }
            }

            if conns.batch_is_empty() {
                conns.batch_clear();

                let now = reactor.now();

                if now >= next_keep_alive_time + KEEP_ALIVE_INTERVAL {
                    // got really behind somehow. just skip ahead
                    next_keep_alive_time = now + KEEP_ALIVE_INTERVAL;
                    next_keep_alive = AsyncSleep::new(next_keep_alive_time);
                }
            }
        }

        debug!("worker {}: task stopped: keep_alives", id);
    }
}

impl Drop for Worker {
    fn drop(&mut self) {
        self.stop = None;

        let thread = self.thread.take().unwrap();
        thread.join().unwrap();
    }
}

pub struct Server {
    addrs: Vec<SocketAddr>,

    // underscore-prefixed because we never reference after construction
    _workers: Vec<Worker>,
    _req_listener: Listener,
    _stream_listener: Listener,
}

impl Server {
    pub fn new(
        instance_id: &str,
        worker_count: usize,
        req_maxconn: usize,
        stream_maxconn: usize,
        buffer_size: usize,
        body_buffer_size: usize,
        messages_max: usize,
        req_timeout: Duration,
        stream_timeout: Duration,
        listen_addrs: &[ListenConfig],
        certs_dir: &Path,
        zsockman: zhttpsocket::SocketManager,
        handle_bound: usize,
    ) -> Result<Self, String> {
        let identities = Arc::new(IdentityCache::new(certs_dir));

        let mut req_tcp_listeners = Vec::new();
        let mut stream_tcp_listeners = Vec::new();

        let mut req_acceptor_tls = Vec::new();
        let mut stream_acceptor_tls = Vec::new();

        let zsockman = Arc::new(zsockman);

        let mut addrs = Vec::new();

        for lc in listen_addrs.iter() {
            let l = match TcpListener::bind(lc.addr) {
                Ok(l) => l,
                Err(e) => return Err(format!("failed to bind {}: {}", lc.addr, e)),
            };

            let addr = l.local_addr().unwrap();

            info!("listening on {}", addr);

            addrs.push(addr);

            if lc.stream {
                stream_tcp_listeners.push(l);
                stream_acceptor_tls.push((lc.tls, lc.default_cert.clone()));
            } else {
                req_tcp_listeners.push(l);
                req_acceptor_tls.push((lc.tls, lc.default_cert.clone()));
            };
        }

        let mut workers = Vec::new();
        let mut req_lsenders = Vec::new();
        let mut stream_lsenders = Vec::new();

        for i in 0..worker_count {
            // rendezvous channels
            let (s, req_r) = channel::channel(0);
            req_lsenders.push(s);
            let (s, stream_r) = channel::channel(0);
            stream_lsenders.push(s);

            let w = Worker::new(
                instance_id,
                i,
                req_maxconn / worker_count,
                stream_maxconn / worker_count,
                buffer_size,
                body_buffer_size,
                messages_max,
                req_timeout,
                stream_timeout,
                req_r,
                stream_r,
                &req_acceptor_tls,
                &stream_acceptor_tls,
                &identities,
                &zsockman,
                handle_bound,
            );
            workers.push(w);
        }

        let req_listener = Listener::new(req_tcp_listeners, req_lsenders);
        let stream_listener = Listener::new(stream_tcp_listeners, stream_lsenders);

        Ok(Self {
            addrs: addrs,
            _workers: workers,
            _req_listener: req_listener,
            _stream_listener: stream_listener,
        })
    }

    pub fn addrs(&self) -> &[SocketAddr] {
        &self.addrs
    }
}

pub struct TestServer {
    server: Server,
    thread: Option<thread::JoinHandle<()>>,
    stop: channel::Sender<()>,
}

impl TestServer {
    pub fn new(workers: usize) -> Self {
        let zmq_context = Arc::new(zmq::Context::new());

        let req_maxconn = 100;
        let stream_maxconn = 100;

        let maxconn = req_maxconn + stream_maxconn;

        let mut zsockman = zhttpsocket::SocketManager::new(
            Arc::clone(&zmq_context),
            "test",
            (MSG_RETAINED_PER_CONNECTION_MAX * maxconn) + (MSG_RETAINED_PER_WORKER_MAX * workers),
            100,
            100,
        );

        zsockman
            .set_client_req_specs(&vec![SpecInfo {
                spec: String::from("inproc://server-test"),
                bind: true,
                ipc_file_mode: 0,
            }])
            .unwrap();

        zsockman
            .set_client_stream_specs(
                &vec![SpecInfo {
                    spec: String::from("inproc://server-test-out"),
                    bind: true,
                    ipc_file_mode: 0,
                }],
                &vec![SpecInfo {
                    spec: String::from("inproc://server-test-out-stream"),
                    bind: true,
                    ipc_file_mode: 0,
                }],
                &vec![SpecInfo {
                    spec: String::from("inproc://server-test-in"),
                    bind: true,
                    ipc_file_mode: 0,
                }],
            )
            .unwrap();

        let addr1 = "127.0.0.1:0".parse().unwrap();
        let addr2 = "127.0.0.1:0".parse().unwrap();

        let server = Server::new(
            "test",
            workers,
            req_maxconn,
            stream_maxconn,
            1024,
            1024,
            10,
            Duration::from_secs(5),
            Duration::from_secs(5),
            &vec![
                ListenConfig {
                    addr: addr1,
                    stream: false,
                    tls: false,
                    default_cert: None,
                },
                ListenConfig {
                    addr: addr2,
                    stream: true,
                    tls: false,
                    default_cert: None,
                },
            ],
            Path::new("."),
            zsockman,
            100,
        )
        .unwrap();

        let (started_s, started_r) = channel::channel(1);
        let (stop_s, stop_r) = channel::channel(1);

        let thread = thread::spawn(move || {
            Self::run(started_s, stop_r, zmq_context);
        });

        // wait for handler thread to start
        started_r.recv().unwrap();

        Self {
            server,
            thread: Some(thread),
            stop: stop_s,
        }
    }

    pub fn req_addr(&self) -> SocketAddr {
        self.server.addrs()[0]
    }

    pub fn stream_addr(&self) -> SocketAddr {
        self.server.addrs()[1]
    }

    fn respond(id: &[u8]) -> Result<zmq::Message, io::Error> {
        let mut dest = [0; 1024];

        let mut cursor = io::Cursor::new(&mut dest[..]);

        cursor.write(b"T")?;

        let mut w = tnetstring::Writer::new(&mut cursor);

        w.start_map()?;

        w.write_string(b"id")?;
        w.write_string(id)?;

        w.write_string(b"code")?;
        w.write_int(200)?;

        w.write_string(b"reason")?;
        w.write_string(b"OK")?;

        w.write_string(b"body")?;
        w.write_string(b"world\n")?;

        w.end_map()?;

        w.flush()?;

        let size = cursor.position() as usize;

        Ok(zmq::Message::from(&dest[..size]))
    }

    fn respond_stream(id: &[u8]) -> Result<zmq::Message, io::Error> {
        let mut dest = [0; 1024];

        let mut cursor = io::Cursor::new(&mut dest[..]);

        cursor.write(b"test T")?;

        let mut w = tnetstring::Writer::new(&mut cursor);

        w.start_map()?;

        w.write_string(b"from")?;
        w.write_string(b"handler")?;

        w.write_string(b"id")?;
        w.write_string(id)?;

        w.write_string(b"seq")?;
        w.write_int(0)?;

        w.write_string(b"code")?;
        w.write_int(200)?;

        w.write_string(b"reason")?;
        w.write_string(b"OK")?;

        w.write_string(b"headers")?;

        w.start_array()?;

        w.start_array()?;
        w.write_string(b"Content-Length")?;
        w.write_string(b"6")?;
        w.end_array()?;

        w.end_array()?;

        w.write_string(b"body")?;
        w.write_string(b"world\n")?;

        w.end_map()?;

        w.flush()?;

        let size = cursor.position() as usize;

        Ok(zmq::Message::from(&dest[..size]))
    }

    fn respond_ws(id: &[u8]) -> Result<zmq::Message, io::Error> {
        let mut dest = [0; 1024];

        let mut cursor = io::Cursor::new(&mut dest[..]);

        cursor.write(b"test T")?;

        let mut w = tnetstring::Writer::new(&mut cursor);

        w.start_map()?;

        w.write_string(b"from")?;
        w.write_string(b"handler")?;

        w.write_string(b"id")?;
        w.write_string(id)?;

        w.write_string(b"seq")?;
        w.write_int(0)?;

        w.write_string(b"code")?;
        w.write_int(101)?;

        w.write_string(b"reason")?;
        w.write_string(b"Switching Protocols")?;

        w.end_map()?;

        w.flush()?;

        let size = cursor.position() as usize;

        Ok(zmq::Message::from(&dest[..size]))
    }

    fn respond_msg(
        id: &[u8],
        seq: u32,
        ptype: &str,
        content_type: &str,
        body: &[u8],
        code: Option<u16>,
    ) -> Result<zmq::Message, io::Error> {
        let mut dest = [0; 1024];

        let mut cursor = io::Cursor::new(&mut dest[..]);

        cursor.write(b"test T")?;

        let mut w = tnetstring::Writer::new(&mut cursor);

        w.start_map()?;

        w.write_string(b"from")?;
        w.write_string(b"handler")?;

        w.write_string(b"id")?;
        w.write_string(id)?;

        w.write_string(b"seq")?;
        w.write_int(seq as isize)?;

        if ptype.is_empty() {
            w.write_string(b"content-type")?;
            w.write_string(content_type.as_bytes())?;
        } else {
            w.write_string(b"type")?;
            w.write_string(ptype.as_bytes())?;
        }

        if let Some(x) = code {
            w.write_string(b"code")?;
            w.write_int(x as isize)?;
        }

        w.write_string(b"body")?;
        w.write_string(body)?;

        w.end_map()?;

        w.flush()?;

        let size = cursor.position() as usize;

        Ok(zmq::Message::from(&dest[..size]))
    }

    fn run(
        started: channel::Sender<()>,
        stop: channel::Receiver<()>,
        zmq_context: Arc<zmq::Context>,
    ) {
        let rep_sock = zmq_context.socket(zmq::REP).unwrap();
        rep_sock.connect("inproc://server-test").unwrap();

        let in_sock = zmq_context.socket(zmq::PULL).unwrap();
        in_sock.connect("inproc://server-test-out").unwrap();

        let in_stream_sock = zmq_context.socket(zmq::ROUTER).unwrap();
        in_stream_sock.set_identity(b"handler").unwrap();
        in_stream_sock
            .connect("inproc://server-test-out-stream")
            .unwrap();

        let out_sock = zmq_context.socket(zmq::XPUB).unwrap();
        out_sock.connect("inproc://server-test-in").unwrap();

        // ensure zsockman is subscribed
        let msg = out_sock.recv_msg(0).unwrap();
        assert_eq!(&msg[..], b"\x01test ");

        started.send(()).unwrap();

        let mut poller = event::Poller::new(1).unwrap();

        poller
            .register_custom(
                stop.get_read_registration(),
                mio::Token(1),
                mio::Interest::READABLE,
            )
            .unwrap();

        poller
            .register(
                &mut SourceFd(&rep_sock.get_fd().unwrap()),
                mio::Token(2),
                mio::Interest::READABLE,
            )
            .unwrap();

        poller
            .register(
                &mut SourceFd(&in_sock.get_fd().unwrap()),
                mio::Token(3),
                mio::Interest::READABLE,
            )
            .unwrap();

        poller
            .register(
                &mut SourceFd(&in_stream_sock.get_fd().unwrap()),
                mio::Token(4),
                mio::Interest::READABLE,
            )
            .unwrap();

        let mut rep_events = rep_sock.get_events().unwrap();

        let mut in_events = in_sock.get_events().unwrap();
        let mut in_stream_events = in_stream_sock.get_events().unwrap();

        loop {
            while rep_events.contains(zmq::POLLIN) {
                let parts = match rep_sock.recv_multipart(zmq::DONTWAIT) {
                    Ok(parts) => parts,
                    Err(zmq::Error::EAGAIN) => {
                        break;
                    }
                    Err(e) => panic!("recv error: {:?}", e),
                };

                assert_eq!(parts.len(), 1);

                let msg = &parts[0];
                assert_eq!(msg[0], b'T');

                let mut id = "";
                let mut method = "";

                for f in tnetstring::parse_map(&msg[1..]).unwrap() {
                    let f = f.unwrap();

                    match f.key {
                        "id" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            id = str::from_utf8(s).unwrap();
                        }
                        "method" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            method = str::from_utf8(s).unwrap();
                        }
                        _ => {}
                    }
                }

                assert_eq!(method, "GET");

                let msg = Self::respond(id.as_bytes()).unwrap();

                rep_sock.send(msg, 0).unwrap();

                rep_events = rep_sock.get_events().unwrap();
            }

            while in_events.contains(zmq::POLLIN) {
                let parts = match in_sock.recv_multipart(zmq::DONTWAIT) {
                    Ok(parts) => parts,
                    Err(zmq::Error::EAGAIN) => {
                        break;
                    }
                    Err(e) => panic!("recv error: {:?}", e),
                };

                in_events = in_sock.get_events().unwrap();

                assert_eq!(parts.len(), 1);

                let msg = &parts[0];
                assert_eq!(msg[0], b'T');

                let mut id = "";
                let mut method = "";
                let mut uri = "";

                for f in tnetstring::parse_map(&msg[1..]).unwrap() {
                    let f = f.unwrap();

                    match f.key {
                        "id" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            id = str::from_utf8(s).unwrap();
                        }
                        "method" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            method = str::from_utf8(s).unwrap();
                        }
                        "uri" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            uri = str::from_utf8(s).unwrap();
                        }
                        _ => {}
                    }
                }

                assert_eq!(method, "GET");

                if uri.starts_with("ws:") {
                    let msg = Self::respond_ws(id.as_bytes()).unwrap();
                    out_sock.send(msg, 0).unwrap();
                } else {
                    let msg = Self::respond_stream(id.as_bytes()).unwrap();
                    out_sock.send(msg, 0).unwrap();
                }
            }

            while in_stream_events.contains(zmq::POLLIN) {
                let parts = match in_stream_sock.recv_multipart(zmq::DONTWAIT) {
                    Ok(parts) => parts,
                    Err(zmq::Error::EAGAIN) => {
                        break;
                    }
                    Err(e) => panic!("recv error: {:?}", e),
                };

                in_stream_events = in_stream_sock.get_events().unwrap();

                assert_eq!(parts.len(), 3);
                assert_eq!(parts[1].len(), 0);

                let msg = &parts[2];
                assert_eq!(msg[0], b'T');

                let mut id = "";
                let mut seq = None;
                let mut ptype = "";
                let mut content_type = "";
                let mut body = &b""[..];
                let mut code = None;

                for f in tnetstring::parse_map(&msg[1..]).unwrap() {
                    let f = f.unwrap();

                    match f.key {
                        "id" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            id = str::from_utf8(s).unwrap();
                        }
                        "seq" => {
                            seq = Some(tnetstring::parse_int(&f.data).unwrap() as u32);
                        }
                        "type" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            ptype = str::from_utf8(s).unwrap();
                        }
                        "content-type" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            content_type = str::from_utf8(s).unwrap();
                        }
                        "body" => {
                            body = tnetstring::parse_string(&f.data).unwrap();
                        }
                        "code" => {
                            code = Some(tnetstring::parse_int(&f.data).unwrap() as u16);
                        }
                        _ => {}
                    }
                }

                let seq = seq.unwrap();

                // as a hack to make the test server stateless, respond to every message
                //   using the received sequence number. for messages we don't care about,
                //   respond with keep-alive in order to keep the sequencing going
                if ptype.is_empty() || ptype == "ping" || ptype == "pong" || ptype == "close" {
                    if ptype == "ping" {
                        ptype = "pong";
                    }

                    let msg =
                        Self::respond_msg(id.as_bytes(), seq, ptype, content_type, body, code)
                            .unwrap();
                    out_sock.send(msg, 0).unwrap();
                } else {
                    let msg =
                        Self::respond_msg(id.as_bytes(), seq, "keep-alive", "", &b""[..], None)
                            .unwrap();
                    out_sock.send(msg, 0).unwrap();
                }
            }

            poller.poll(None).unwrap();

            let mut done = false;

            for event in poller.iter_events() {
                match event.token() {
                    mio::Token(1) => {
                        if stop.try_recv().is_ok() {
                            done = true;
                            break;
                        }
                    }
                    mio::Token(2) => {
                        rep_events = rep_sock.get_events().unwrap();
                    }
                    mio::Token(3) => {
                        in_events = in_sock.get_events().unwrap();
                    }
                    mio::Token(4) => {
                        in_stream_events = in_stream_sock.get_events().unwrap();
                    }
                    _ => unreachable!(),
                }
            }

            if done {
                break;
            }
        }
    }
}

impl Drop for TestServer {
    fn drop(&mut self) {
        self.stop.try_send(()).unwrap();

        let thread = self.thread.take().unwrap();
        thread.join().unwrap();
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use crate::websocket;
    use std::io::Read;

    #[test]
    fn test_batch() {
        let mut batch = Batch::new(3);

        assert_eq!(batch.capacity(), 3);
        assert_eq!(batch.len(), 0);
        assert_eq!(batch.last_group_ckeys(), &[]);

        assert!(batch.add(b"addr-a", 1).is_ok());
        assert!(batch.add(b"addr-a", 2).is_ok());
        assert!(batch.add(b"addr-b", 3).is_ok());
        assert_eq!(batch.len(), 3);

        assert!(batch.add(b"addr-c", 4).is_err());
        assert_eq!(batch.len(), 3);
        assert_eq!(batch.is_empty(), false);

        let ids = ["id-1", "id-2", "id-3"];

        let group = batch
            .take_group(|ckey| (ids[ckey - 1].as_bytes(), 0))
            .unwrap();
        assert_eq!(group.ids().len(), 2);
        assert_eq!(group.ids()[0].id, b"id-1");
        assert_eq!(group.ids()[0].seq, Some(0));
        assert_eq!(group.ids()[1].id, b"id-2");
        assert_eq!(group.ids()[1].seq, Some(0));
        assert_eq!(group.addr(), b"addr-a");
        drop(group);
        assert_eq!(batch.is_empty(), false);
        assert_eq!(batch.last_group_ckeys(), &[1, 2]);

        let group = batch
            .take_group(|ckey| (ids[ckey - 1].as_bytes(), 0))
            .unwrap();
        assert_eq!(group.ids().len(), 1);
        assert_eq!(group.ids()[0].id, b"id-3");
        assert_eq!(group.ids()[0].seq, Some(0));
        assert_eq!(group.addr(), b"addr-b");
        drop(group);
        assert_eq!(batch.is_empty(), true);
        assert_eq!(batch.last_group_ckeys(), &[3]);

        assert!(batch
            .take_group(|ckey| { (ids[ckey - 1].as_bytes(), 0) })
            .is_none());
        assert_eq!(batch.last_group_ckeys(), &[3]);
    }

    #[test]
    fn test_server() {
        let server = TestServer::new(1);

        // req

        let mut client = std::net::TcpStream::connect(&server.req_addr()).unwrap();
        client
            .write(b"GET /hello HTTP/1.0\r\nHost: example.com\r\n\r\n")
            .unwrap();

        let mut buf = Vec::new();
        client.read_to_end(&mut buf).unwrap();

        assert_eq!(
            str::from_utf8(&buf).unwrap(),
            "HTTP/1.0 200 OK\r\nContent-Length: 6\r\n\r\nworld\n"
        );

        // stream (http)

        let mut client = std::net::TcpStream::connect(&server.stream_addr()).unwrap();
        client
            .write(b"GET /hello HTTP/1.0\r\nHost: example.com\r\n\r\n")
            .unwrap();

        let mut buf = Vec::new();
        client.read_to_end(&mut buf).unwrap();

        assert_eq!(
            str::from_utf8(&buf).unwrap(),
            "HTTP/1.0 200 OK\r\nContent-Length: 6\r\n\r\nworld\n"
        );

        // stream (ws)

        let mut client = std::net::TcpStream::connect(&server.stream_addr()).unwrap();

        let req = concat!(
            "GET /hello HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Upgrade: websocket\r\n",
            "Sec-WebSocket-Key: abcde\r\n",
            "\r\n",
        );

        client.write(req.as_bytes()).unwrap();

        let mut buf = Vec::new();
        let mut resp_end = 0;

        loop {
            let mut chunk = [0; 1024];
            let size = client.read(&mut chunk).unwrap();
            buf.extend_from_slice(&chunk[..size]);

            for i in 0..(buf.len() - 3) {
                if &buf[i..(i + 4)] == b"\r\n\r\n" {
                    resp_end = i + 4;
                    break;
                }
            }

            if resp_end > 0 {
                break;
            }
        }

        let expected = concat!(
            "HTTP/1.1 101 Switching Protocols\r\n",
            "Upgrade: websocket\r\n",
            "Connection: Upgrade\r\n",
            "Sec-WebSocket-Accept: 8m4i+0BpIKblsbf+VgYANfQKX4w=\r\n",
            "\r\n",
        );

        assert_eq!(str::from_utf8(&buf[..resp_end]).unwrap(), expected);

        buf = buf.split_off(resp_end);

        // send message

        let mut data = vec![0; 1024];
        let body = &b"hello"[..];
        let size =
            websocket::write_header(true, websocket::OPCODE_TEXT, body.len(), None, &mut data)
                .unwrap();
        data[size..(size + body.len())].copy_from_slice(body);
        client.write(&data[..(size + body.len())]).unwrap();

        // recv message

        let mut msg = Vec::new();

        loop {
            let fi = match websocket::read_header(&buf) {
                Ok(fi) => fi,
                Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => {
                    let mut chunk = [0; 1024];
                    let size = client.read(&mut chunk).unwrap();
                    assert!(size > 0);
                    buf.extend_from_slice(&chunk[..size]);
                    continue;
                }
                Err(e) => panic!("{:?}", e),
            };

            assert_eq!(fi.fin, true);
            assert_eq!(fi.opcode, websocket::OPCODE_TEXT);

            msg.extend_from_slice(&buf[fi.payload_offset..(fi.payload_offset + fi.payload_size)]);
            break;
        }

        assert_eq!(str::from_utf8(&msg).unwrap(), "hello");
    }

    #[test]
    fn test_ws() {
        let server = TestServer::new(1);

        let mut client = std::net::TcpStream::connect(&server.stream_addr()).unwrap();

        let req = concat!(
            "GET /hello HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Upgrade: websocket\r\n",
            "Sec-WebSocket-Key: abcde\r\n",
            "\r\n",
        );

        client.write(req.as_bytes()).unwrap();

        let mut buf = Vec::new();
        let mut resp_end = 0;

        loop {
            let mut chunk = [0; 1024];
            let size = client.read(&mut chunk).unwrap();
            buf.extend_from_slice(&chunk[..size]);

            for i in 0..(buf.len() - 3) {
                if &buf[i..(i + 4)] == b"\r\n\r\n" {
                    resp_end = i + 4;
                    break;
                }
            }

            if resp_end > 0 {
                break;
            }
        }

        let expected = concat!(
            "HTTP/1.1 101 Switching Protocols\r\n",
            "Upgrade: websocket\r\n",
            "Connection: Upgrade\r\n",
            "Sec-WebSocket-Accept: 8m4i+0BpIKblsbf+VgYANfQKX4w=\r\n",
            "\r\n",
        );

        assert_eq!(str::from_utf8(&buf[..resp_end]).unwrap(), expected);

        buf = buf.split_off(resp_end);

        // send binary

        let mut data = vec![0; 1024];
        let body = &[1, 2, 3][..];
        let size =
            websocket::write_header(true, websocket::OPCODE_BINARY, body.len(), None, &mut data)
                .unwrap();
        data[size..(size + body.len())].copy_from_slice(body);
        client.write(&data[..(size + body.len())]).unwrap();

        // recv binary

        let mut msg = Vec::new();

        loop {
            let fi = match websocket::read_header(&buf) {
                Ok(fi) => fi,
                Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => {
                    let mut chunk = [0; 1024];
                    let size = client.read(&mut chunk).unwrap();
                    assert!(size > 0);
                    buf.extend_from_slice(&chunk[..size]);
                    continue;
                }
                Err(e) => panic!("{:?}", e),
            };

            assert_eq!(fi.fin, true);
            assert_eq!(fi.opcode, websocket::OPCODE_BINARY);

            msg.extend_from_slice(&buf[fi.payload_offset..(fi.payload_offset + fi.payload_size)]);
            break;
        }

        assert_eq!(msg, &[1, 2, 3][..]);

        buf.clear();

        // send ping

        let mut data = vec![0; 1024];
        let body = &b""[..];
        let size =
            websocket::write_header(true, websocket::OPCODE_PING, body.len(), None, &mut data)
                .unwrap();
        client.write(&data[..size]).unwrap();

        // recv pong

        let mut msg = Vec::new();

        loop {
            let fi = match websocket::read_header(&buf) {
                Ok(fi) => fi,
                Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => {
                    let mut chunk = [0; 1024];
                    let size = client.read(&mut chunk).unwrap();
                    assert!(size > 0);
                    buf.extend_from_slice(&chunk[..size]);
                    continue;
                }
                Err(e) => panic!("{:?}", e),
            };

            assert_eq!(fi.fin, true);
            assert_eq!(fi.opcode, websocket::OPCODE_PONG);

            msg.extend_from_slice(&buf[fi.payload_offset..(fi.payload_offset + fi.payload_size)]);
            break;
        }

        assert_eq!(str::from_utf8(&msg).unwrap(), "");

        buf.clear();

        // send close

        let mut data = vec![0; 1024];
        let body = &b"\x03\xf0gone"[..];
        let size =
            websocket::write_header(true, websocket::OPCODE_CLOSE, body.len(), None, &mut data)
                .unwrap();
        data[size..(size + body.len())].copy_from_slice(body);
        client.write(&data[..(size + body.len())]).unwrap();

        // recv close

        let mut msg = Vec::new();

        loop {
            let fi = match websocket::read_header(&buf) {
                Ok(fi) => fi,
                Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => {
                    let mut chunk = [0; 1024];
                    let size = client.read(&mut chunk).unwrap();
                    assert!(size > 0);
                    buf.extend_from_slice(&chunk[..size]);
                    continue;
                }
                Err(e) => panic!("{:?}", e),
            };

            assert_eq!(fi.fin, true);
            assert_eq!(fi.opcode, websocket::OPCODE_CLOSE);

            msg.extend_from_slice(&buf[fi.payload_offset..(fi.payload_offset + fi.payload_size)]);
            break;
        }

        assert_eq!(msg, &b"\x03\xf0gone"[..]);

        // expect tcp close

        let mut chunk = [0; 1024];
        let size = client.read(&mut chunk).unwrap();
        assert_eq!(size, 0);
    }
}
