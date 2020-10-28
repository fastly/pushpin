/*
 * Copyright (C) 2020 Fanout, Inc.
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
use crate::buffer::{TmpBuffer, WriteVectored, VECTORED_MAX};
use crate::channel;
use crate::connection::{
    ServerReqConnection, ServerState, ServerStreamConnection, Shutdown, Want, ZhttpSender,
};
use crate::list;
use crate::listener::Listener;
use crate::timer;
use crate::tls::{IdentityCache, TlsAcceptor, TlsStream};
use crate::tnetstring;
use crate::zhttppacket;
use crate::zhttpsocket;
use crate::zmq::SpecInfo;
use arrayvec::{ArrayString, ArrayVec};
use log::{debug, error, info, warn};
use mio;
use mio::net::{TcpListener, TcpStream};
use mio::unix::EventedFd;
use slab::Slab;
use std::cmp;
use std::collections::VecDeque;
use std::convert::TryFrom;
use std::io;
use std::io::{Read, Write};
use std::net::SocketAddr;
use std::path::Path;
use std::rc::Rc;
use std::str;
use std::str::FromStr;
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

// we read and process each message one at a time, dropping each message before reading the next
pub const MSG_RETAINED_MAX: usize = 1;

const HANDLE_BASE: usize = 4;
const CONN_BASE: usize = 16;
const ACCEPT_PER_LOOP_MAX: usize = 100;
const TICK_DURATION_MS: u64 = 10;
const POLL_TIMEOUT_MAX: Duration = Duration::from_millis(100);
const TCP_KEEPALIVE: Duration = Duration::from_secs(3_600); // 1 hour

const KEEP_ALIVE_TIMEOUT_MS: usize = 45_000;
const KEEP_ALIVE_BATCH_MS: usize = 100;
const KEEP_ALIVE_INTERVAL: Duration = Duration::from_millis(KEEP_ALIVE_BATCH_MS as u64);
const KEEP_ALIVE_BATCHES: usize = KEEP_ALIVE_TIMEOUT_MS / KEEP_ALIVE_BATCH_MS;
const BULK_PACKET_SIZE_MAX: usize = 65_000;

fn duration_to_ticks(d: Duration) -> u64 {
    (d.as_millis() / (TICK_DURATION_MS as u128)) as u64
}

fn ticks_to_duration(t: u64) -> Duration {
    Duration::from_millis(t * TICK_DURATION_MS)
}

fn get_addr_and_buf(msg: &[u8]) -> Result<(&str, &[u8]), ()> {
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

    Ok((addr, &msg[(pos + 1)..]))
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

fn send_batched<'buf, 'ids>(
    mut zreq: zhttppacket::Request<'buf, 'ids, '_>,
    ids: &'ids [zhttppacket::Id<'buf>],
    handle: &mut ClientStreamHandle,
    to_addr: &[u8],
) {
    zreq.multi = true;

    let ids_per_msg = zhttppacket::IDS_MAX;
    let msg_count = (ids.len() + (ids_per_msg - 1)) / ids_per_msg;

    for i in 0..msg_count {
        let start = i * ids_per_msg;
        let len = cmp::min(ids_per_msg, ids.len() - start);

        zreq.ids = &ids[start..(start + len)];

        let mut data = [0; BULK_PACKET_SIZE_MAX];

        let size = match zreq.serialize(&mut data) {
            Ok(size) => size,
            Err(e) => {
                error!(
                    "failed to serialize keep-alive packet with {} ids: {}",
                    zreq.ids.len(),
                    e
                );
                break;
            }
        };

        let buf = &data[..size];
        let msg = zmq::Message::from(buf);

        if let Err(e) = handle.send_to(to_addr, msg) {
            let e = match e {
                zhttpsocket::SendError::Full(_) => io::Error::from(io::ErrorKind::WriteZero),
                zhttpsocket::SendError::Io(e) => e,
            };

            error!("zhttp write error: {:?}", e);
            break;
        }
    }
}

impl WriteVectored for TcpStream {
    fn write_vectored(&mut self, bufs: &[io::IoSlice]) -> Result<usize, io::Error> {
        // IoVec does not allow 0-byte slices, so initialize using 1 byte
        let mut arr = [(&b"X"[..]).into(); VECTORED_MAX];
        let mut arr_len = 0;

        for buf in bufs {
            if buf.len() > 0 {
                arr[arr_len] = buf.as_ref().into();
                arr_len += 1;
            }
        }

        TcpStream::write_bufs(self, &arr[..arr_len])
    }
}

impl Shutdown for TcpStream {
    fn shutdown(&mut self) -> Result<(), io::Error> {
        Ok(())
    }
}

impl WriteVectored for TlsStream {
    fn write_vectored(&mut self, bufs: &[io::IoSlice]) -> Result<usize, io::Error> {
        let mut total = 0;

        for buf in bufs {
            match self.write(&*buf) {
                Ok(size) => total += size,
                Err(e) => return Err(e),
            }
        }

        Ok(total)
    }
}

impl Shutdown for TlsStream {
    fn shutdown(&mut self) -> Result<(), io::Error> {
        self.shutdown()
    }
}

impl ZhttpSender for zhttpsocket::ClientReqHandle {
    fn can_send_to(&self) -> bool {
        // req mode doesn't use this
        unimplemented!();
    }

    fn send(&mut self, message: zmq::Message) -> Result<(), zhttpsocket::SendError> {
        self.send(message)
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

struct ClientStreamHandle {
    inner: zhttpsocket::ClientStreamHandle,
    out: VecDeque<(ArrayVec<[u8; 64]>, zmq::Message)>,
    send_to_allowed: bool,
}

impl ClientStreamHandle {
    fn new(handle: zhttpsocket::ClientStreamHandle, queue_size: usize) -> Self {
        Self {
            inner: handle,
            out: VecDeque::with_capacity(queue_size),
            send_to_allowed: false,
        }
    }

    fn pending_send_to(&self) -> usize {
        self.out.len()
    }

    fn set_send_to_allowed(&mut self, allowed: bool) {
        self.send_to_allowed = allowed;
    }

    fn flush_send_to(&mut self) -> Result<(), io::Error> {
        while let Some((addr, msg)) = self.out.pop_front() {
            match self.inner.send_to_addr(addr.as_ref(), msg) {
                Ok(_) => {}
                Err(zhttpsocket::SendError::Full(msg)) => {
                    self.out.push_front((addr, msg));
                    return Err(io::Error::from(io::ErrorKind::WouldBlock));
                }
                Err(zhttpsocket::SendError::Io(e)) => return Err(e),
            }
        }

        Ok(())
    }
}

impl ZhttpSender for ClientStreamHandle {
    fn can_send_to(&self) -> bool {
        self.out.len() < self.out.capacity() && self.send_to_allowed
    }

    fn send(&mut self, message: zmq::Message) -> Result<(), zhttpsocket::SendError> {
        self.inner.send_to_any(message)
    }

    fn send_to(
        &mut self,
        addr: &[u8],
        message: zmq::Message,
    ) -> Result<(), zhttpsocket::SendError> {
        if !self.can_send_to() {
            return Err(zhttpsocket::SendError::Full(message));
        }

        let mut a = ArrayVec::new();
        if a.try_extend_from_slice(addr).is_err() {
            return Err(zhttpsocket::SendError::Io(io::Error::from(
                io::ErrorKind::InvalidInput,
            )));
        }

        self.out.push_back((a, message));

        Ok(())
    }
}

enum ServerConnection {
    Req(ServerReqConnection),
    Stream(ServerStreamConnection),
}

#[derive(Copy, Clone, PartialEq)]
pub enum ZhttpMode {
    Req,
    Stream,
}

#[derive(Copy, Clone)]
enum ZWriteKey {
    Req(usize),
    Stream(usize),
    StreamTo(usize),
}

enum Stream {
    Plain(TcpStream),
    Tls(TlsStream),
}

impl Stream {
    fn get_tcp(&self) -> Option<&TcpStream> {
        match self {
            Stream::Plain(stream) => Some(stream),
            Stream::Tls(stream) => stream.get_tcp(),
        }
    }
}

struct Connection {
    id: ArrayString<[u8; 32]>,
    stream: Stream,
    conn: ServerConnection,
    want: Want,
    timer: Option<(usize, u64)>, // timer id, exp time
    zwrite_key: Option<ZWriteKey>,
}

impl Connection {
    fn new(
        stream: Stream,
        peer_addr: SocketAddr,
        zmode: ZhttpMode,
        buffer_size: usize,
        body_buffer_size: usize,
        messages_max: usize,
        rb_tmp: &Rc<TmpBuffer>,
        timeout: Duration,
    ) -> Self {
        if let Err(e) = stream.get_tcp().unwrap().set_nodelay(true) {
            error!("set nodelay failed: {:?}", e);
        }

        if let Err(e) = stream.get_tcp().unwrap().set_keepalive(Some(TCP_KEEPALIVE)) {
            error!("set keepalive failed: {:?}", e);
        }

        let conn = match zmode {
            ZhttpMode::Req => ServerConnection::Req(ServerReqConnection::new(
                Instant::now(),
                Some(peer_addr),
                buffer_size,
                body_buffer_size,
                rb_tmp,
                timeout,
            )),
            ZhttpMode::Stream => ServerConnection::Stream(ServerStreamConnection::new(
                Instant::now(),
                Some(peer_addr),
                buffer_size,
                messages_max,
                rb_tmp,
                timeout,
            )),
        };

        Self {
            id: ArrayString::new(),
            stream,
            conn,
            want: Want::nothing(),
            timer: None,
            zwrite_key: None,
        }
    }

    fn mode(&self) -> ZhttpMode {
        match &self.conn {
            ServerConnection::Req(_) => ZhttpMode::Req,
            ServerConnection::Stream(_) => ZhttpMode::Stream,
        }
    }

    fn state(&self) -> ServerState {
        match &self.conn {
            ServerConnection::Req(conn) => conn.state(),
            ServerConnection::Stream(conn) => conn.state(),
        }
    }

    fn get_tcp(&self) -> Option<&TcpStream> {
        self.stream.get_tcp()
    }

    fn start(&mut self, id: &str) {
        self.id = ArrayString::from_str(id).unwrap();

        if let Stream::Tls(stream) = &mut self.stream {
            stream.set_id(id);
        }

        debug!("conn {}: assigning id", self.id);

        match &mut self.conn {
            ServerConnection::Req(conn) => conn.start(self.id.as_ref()),
            ServerConnection::Stream(conn) => conn.start(self.id.as_ref()),
        }
    }

    fn set_sock_readable(&mut self) {
        match &mut self.conn {
            ServerConnection::Req(conn) => conn.set_sock_readable(),
            ServerConnection::Stream(conn) => conn.set_sock_readable(),
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
            ServerConnection::Req(conn) => {
                if let Err(e) = conn.apply_zhttp_response(zresp) {
                    debug!("conn {}: apply error {:?}", self.id, e);
                    return Err(());
                }
            }
            ServerConnection::Stream(conn) => {
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
        req_handle: &mut zhttpsocket::ClientReqHandle,
        stream_handle: &mut ClientStreamHandle,
        packet_buf: &mut [u8],
        tmp_buf: &mut [u8],
    ) -> bool {
        match &mut self.stream {
            Stream::Plain(stream) => Self::process_with_stream(
                &self.id,
                &mut self.conn,
                &mut self.want,
                stream,
                now,
                instance_id,
                req_handle,
                stream_handle,
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
                    req_handle,
                    stream_handle,
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

    fn process_with_stream<S: Read + Write + WriteVectored + Shutdown>(
        id: &ArrayString<[u8; 32]>,
        conn: &mut ServerConnection,
        want: &mut Want,
        stream: &mut S,
        now: Instant,
        instance_id: &str,
        req_handle: &mut zhttpsocket::ClientReqHandle,
        stream_handle: &mut ClientStreamHandle,
        packet_buf: &mut [u8],
        tmp_buf: &mut [u8],
    ) -> bool {
        match conn {
            ServerConnection::Req(conn) => {
                match conn.process(now, stream, req_handle, packet_buf) {
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
            ServerConnection::Stream(conn) => {
                match conn.process(now, instance_id, stream, stream_handle, packet_buf, tmp_buf) {
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

struct KeySet {
    index: Vec<bool>,
    queue: VecDeque<u32>,
}

impl KeySet {
    fn new(capacity: usize) -> Self {
        u32::try_from(capacity).unwrap();

        let mut index = Vec::with_capacity(capacity);
        index.resize(capacity, false);

        let queue = VecDeque::with_capacity(capacity);

        Self { index, queue }
    }

    fn add(&mut self, key: usize) {
        let k = u32::try_from(key).unwrap();

        if self.index[key] {
            return;
        }

        self.queue.push_back(k);
        self.index[key] = true;
    }

    fn take(&mut self) -> Option<usize> {
        match self.queue.pop_front() {
            Some(k) => {
                let key = k as usize;

                self.index[key] = false;

                Some(key)
            }
            None => None,
        }
    }
}

enum ZWrite {
    Server,
    Connection(usize),
}

struct Worker {
    thread: Option<thread::JoinHandle<()>>,
    stop: channel::Sender<()>,
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
    ) -> Self {
        debug!("worker {}: starting", id);

        let (s, r) = channel::channel(1);
        let (rs, rr) = channel::channel(1);

        let instance_id = String::from(instance_id);
        let req_acceptor_tls = req_acceptor_tls.clone();
        let stream_acceptor_tls = stream_acceptor_tls.clone();
        let identities = Arc::clone(identities);
        let zsockman = Arc::clone(zsockman);

        let thread = thread::spawn(move || {
            Self::run(
                instance_id,
                id,
                req_maxconn,
                stream_maxconn,
                buffer_size,
                body_buffer_size,
                messages_max,
                req_timeout,
                stream_timeout,
                r,
                req_acceptor,
                stream_acceptor,
                &req_acceptor_tls,
                &stream_acceptor_tls,
                identities,
                zsockman,
                rs,
            );
        });

        rr.recv().unwrap();

        Self {
            thread: Some(thread),
            stop: s,
        }
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

    fn flush_send_to(
        can_zstream_out_stream_write: &mut bool,
        stream_handle: &mut ClientStreamHandle,
    ) {
        match stream_handle.flush_send_to() {
            Ok(_) => {}
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                *can_zstream_out_stream_write = false
            }
            Err(e) => error!("zhttp write error: {:?}", e),
        }
    }

    fn zwrite_queue_next(
        can_zreq_write: bool,
        can_zstream_out_write: bool,
        zwrite_nodes: &Slab<list::Node<ZWrite>>,
        zwrite_req: &list::List,
        zwrite_stream: &list::List,
        zwrite_stream_to: &list::List,
        stream_handle: &ClientStreamHandle,
        needs_process: &mut KeySet,
    ) {
        if can_zreq_write {
            if let Some(nkey) = zwrite_req.head {
                let n = &zwrite_nodes[nkey];
                match n.value {
                    ZWrite::Connection(key) => needs_process.add(key),
                    ZWrite::Server => {}
                }
            }
        }

        if can_zstream_out_write {
            if let Some(nkey) = zwrite_stream.head {
                let n = &zwrite_nodes[nkey];
                match n.value {
                    ZWrite::Connection(key) => needs_process.add(key),
                    ZWrite::Server => {}
                }
            }
        }

        if stream_handle.pending_send_to() == 0 {
            if let Some(nkey) = zwrite_stream_to.head {
                let n = &zwrite_nodes[nkey];
                match n.value {
                    ZWrite::Connection(key) => needs_process.add(key),
                    ZWrite::Server => {}
                }
            }
        }
    }

    fn zwrite_remove(
        k: ZWriteKey,
        zwrite_nodes: &mut Slab<list::Node<ZWrite>>,
        zwrite_req: &mut list::List,
        zwrite_stream: &mut list::List,
        zwrite_stream_to: &mut list::List,
    ) {
        match k {
            ZWriteKey::Req(nkey) => {
                zwrite_req.remove(zwrite_nodes, nkey);
                zwrite_nodes.remove(nkey);
            }
            ZWriteKey::Stream(nkey) => {
                zwrite_stream.remove(zwrite_nodes, nkey);
                zwrite_nodes.remove(nkey);
            }
            ZWriteKey::StreamTo(nkey) => {
                zwrite_stream_to.remove(zwrite_nodes, nkey);
                zwrite_nodes.remove(nkey);
            }
        }
    }

    fn run(
        instance_id: String,
        id: usize,
        req_maxconn: usize,
        stream_maxconn: usize,
        buffer_size: usize,
        body_buffer_size: usize,
        messages_max: usize,
        req_timeout: Duration,
        stream_timeout: Duration,
        stop: channel::Receiver<()>,
        req_acceptor: channel::Receiver<(usize, TcpStream, SocketAddr)>,
        stream_acceptor: channel::Receiver<(usize, TcpStream, SocketAddr)>,
        req_acceptor_tls: &[(bool, Option<String>)],
        stream_acceptor_tls: &[(bool, Option<String>)],
        identities: Arc<IdentityCache>,
        zsockman: Arc<zhttpsocket::SocketManager>,
        ready_sender: channel::Sender<()>,
    ) {
        let maxconn = req_maxconn + stream_maxconn;

        let mut req_count = 0;
        let mut stream_count = 0;

        let mut next_cid = 0;

        debug!("worker {}: allocating buffers", id);

        let rb_tmp = Rc::new(TmpBuffer::new(buffer_size));

        // large enough to fit anything
        let mut packet_buf = vec![0; buffer_size + body_buffer_size + 4096];

        // same size as working buffers
        let mut tmp_buf = vec![0; buffer_size];

        let mut conns: Slab<Connection> = Slab::with_capacity(maxconn);
        let mut needs_process = KeySet::new(maxconn);
        let mut timers = timer::TimerWheel::new(maxconn);

        let ka_batch = (stream_maxconn + (KEEP_ALIVE_BATCHES - 1)) / KEEP_ALIVE_BATCHES;

        let mut ka_nodes: Slab<list::Node<usize>> = Slab::with_capacity(ka_batch);
        let mut ka_addrs: Vec<(ArrayVec<[u8; 64]>, list::List)> = Vec::with_capacity(ka_batch);
        let mut ka_ids_mem: Vec<zhttppacket::Id> = Vec::with_capacity(ka_batch);

        let mut zwrite_nodes: Slab<list::Node<ZWrite>> = Slab::with_capacity(maxconn + 1);
        let mut zwrite_req = list::List::default();
        let mut zwrite_stream = list::List::default();
        let mut zwrite_stream_to = list::List::default();
        let mut server_zwrite_key = None;

        let mut req_tls_acceptors = Vec::new();

        for config in req_acceptor_tls {
            if config.0 {
                let default_cert = config.1.as_ref().map(|s| s.as_str());
                req_tls_acceptors.push(Some(TlsAcceptor::new(&identities, default_cert)));
            } else {
                req_tls_acceptors.push(None);
            }
        }

        let mut stream_tls_acceptors = Vec::new();

        for config in stream_acceptor_tls {
            if config.0 {
                let default_cert = config.1.as_ref().map(|s| s.as_str());
                stream_tls_acceptors.push(Some(TlsAcceptor::new(&identities, default_cert)));
            } else {
                stream_tls_acceptors.push(None);
            }
        }

        debug!("worker {}: allocating done", id);

        let poll = mio::Poll::new().unwrap();

        poll.register(
            stop.get_read_registration(),
            mio::Token(0),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            req_acceptor.get_read_registration(),
            mio::Token(1),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            stream_acceptor.get_read_registration(),
            mio::Token(2),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        let mut req_handle = zsockman.client_req_handle(format!("{}-", id).as_bytes());
        let stream_handle = zsockman.client_stream_handle(format!("{}-", id).as_bytes());

        poll.register(
            req_handle.get_read_registration(),
            mio::Token(HANDLE_BASE + 0),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            req_handle.get_write_registration(),
            mio::Token(HANDLE_BASE + 1),
            mio::Ready::writable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            stream_handle.get_read_registration(),
            mio::Token(HANDLE_BASE + 2),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            stream_handle.get_write_any_registration(),
            mio::Token(HANDLE_BASE + 3),
            mio::Ready::writable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            stream_handle.get_write_addr_registration(),
            mio::Token(HANDLE_BASE + 4),
            mio::Ready::writable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        let mut stream_handle = ClientStreamHandle::new(stream_handle, ka_batch);

        let mut can_req_accept = true;
        let mut can_stream_accept = true;
        let mut can_zreq_read = true;
        let mut can_zreq_write = true;
        let mut can_zstream_in_read = true;
        let mut can_zstream_out_write = true;
        let mut can_zstream_out_stream_write = true;

        let mut last_keep_alive_time = Instant::now();
        let mut next_keep_alive_index = 0;

        let mut events = mio::Events::with_capacity(1024);

        let start_time = Instant::now();

        debug!("worker {}: started", id);

        ready_sender.send(()).unwrap();
        drop(ready_sender);

        loop {
            let now = Instant::now();
            let now_ticks = duration_to_ticks(now - start_time);

            timers.update(now_ticks);

            while let Some((_, key)) = timers.take_expired() {
                let c = &mut conns[key];
                c.timer = None;

                needs_process.add(key);
            }

            for _ in 0..ACCEPT_PER_LOOP_MAX {
                if !can_req_accept || req_count >= req_maxconn {
                    break;
                }

                let (pos, stream, peer_addr) = match req_acceptor.try_recv() {
                    Ok(stream) => stream,
                    Err(_) => {
                        can_req_accept = false;
                        break;
                    }
                };

                let stream = match &req_tls_acceptors[pos] {
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
                    None => Stream::Plain(stream),
                };

                req_count += 1;

                assert!(conns.len() < conns.capacity());

                let entry = conns.vacant_entry();
                let key = entry.key();

                let c = Connection::new(
                    stream,
                    peer_addr,
                    ZhttpMode::Req,
                    buffer_size,
                    body_buffer_size,
                    messages_max,
                    &rb_tmp,
                    req_timeout,
                );

                entry.insert(c);

                let c = &mut conns[key];

                debug!(
                    "worker {}: req conn starting {} {}/{}",
                    id, key, req_count, req_maxconn
                );

                let id = Self::gen_id(id, key, &mut next_cid);
                c.start(id.as_ref());

                let ready_flags = mio::Ready::readable() | mio::Ready::writable();

                poll.register(
                    c.get_tcp().unwrap(),
                    mio::Token(CONN_BASE + (key * 4) + 0),
                    ready_flags,
                    mio::PollOpt::edge(),
                )
                .unwrap();

                needs_process.add(key);
            }

            for _ in 0..ACCEPT_PER_LOOP_MAX {
                if !can_stream_accept || stream_count >= stream_maxconn {
                    break;
                }

                let (pos, stream, peer_addr) = match stream_acceptor.try_recv() {
                    Ok(stream) => stream,
                    Err(_) => {
                        can_stream_accept = false;
                        break;
                    }
                };

                let stream = match &stream_tls_acceptors[pos] {
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
                    None => Stream::Plain(stream),
                };

                stream_count += 1;

                assert!(conns.len() < conns.capacity());

                let entry = conns.vacant_entry();
                let key = entry.key();

                let c = Connection::new(
                    stream,
                    peer_addr,
                    ZhttpMode::Stream,
                    buffer_size,
                    body_buffer_size,
                    messages_max,
                    &rb_tmp,
                    stream_timeout,
                );

                entry.insert(c);

                let c = &mut conns[key];

                debug!(
                    "worker {}: stream conn starting {} {}/{}",
                    id, key, stream_count, stream_maxconn
                );

                let id = Self::gen_id(id, key, &mut next_cid);
                c.start(id.as_ref());

                let ready_flags = mio::Ready::readable() | mio::Ready::writable();

                poll.register(
                    c.get_tcp().unwrap(),
                    mio::Token(CONN_BASE + (key * 4) + 0),
                    ready_flags,
                    mio::PollOpt::edge(),
                )
                .unwrap();

                needs_process.add(key);
            }

            if can_zreq_read {
                // here we try to read and process packets as fast as
                //   possible. we should really only copy buffers or
                //   flag things to do later in c.process()

                loop {
                    let msg = match req_handle.recv() {
                        Ok(msg) => msg,
                        Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                            break;
                        }
                        Err(e) => {
                            debug!("worker {}: handle read error {}", id, e);
                            break;
                        }
                    };

                    let msg = &msg.get()[..];

                    let mut scratch = zhttppacket::ResponseScratch::new();
                    let zresp = match zhttppacket::Response::parse(msg, &mut scratch) {
                        Ok(zresp) => zresp,
                        Err(e) => {
                            warn!("worker {}: zhttp parse error: {}", id, e);
                            continue;
                        }
                    };

                    let mut handled = 0;

                    for id in zresp.ids {
                        let key = match get_key(&id.id) {
                            Ok(key) => key,
                            Err(_) => continue,
                        };

                        let c = match conns.get_mut(key) {
                            Some(c) => c,
                            None => continue,
                        };

                        if c.id.as_ref().as_bytes() != id.id {
                            // key found but cid mismatch
                            continue;
                        }

                        handled += 1;

                        if c.handle_packet(now, &zresp, None).is_err() {
                            continue;
                        }

                        if c.mode() == ZhttpMode::Req && c.want.zhttp_read {
                            needs_process.add(key);
                        }
                    }

                    if handled == 0 {
                        debug!("worker {}: no conn for zmq message", id);
                    }
                }

                can_zreq_read = false;
            }

            if can_zstream_in_read {
                // here we try to read and process packets as fast as
                //   possible. we should really only copy buffers or
                //   flag things to do later in c.process()

                loop {
                    let msg = match stream_handle.inner.recv() {
                        Ok(msg) => msg,
                        Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                            break;
                        }
                        Err(e) => {
                            debug!("worker {}: handle read error {}", id, e);
                            break;
                        }
                    };

                    let msg = &msg.get()[..];

                    let (addr, buf) = match get_addr_and_buf(&msg) {
                        Ok(ret) => ret,
                        Err(_) => {
                            warn!("worker {}: packet has unexpected format", id);
                            continue;
                        }
                    };

                    if addr != instance_id {
                        warn!("worker {}: packet not for us", id);
                        continue;
                    }

                    let mut scratch = zhttppacket::ResponseScratch::new();
                    let zresp = match zhttppacket::Response::parse(&buf, &mut scratch) {
                        Ok(zresp) => zresp,
                        Err(e) => {
                            warn!("worker {}: zhttp parse error: {}", id, e);
                            continue;
                        }
                    };

                    let mut handled = 0;

                    for id in zresp.ids {
                        let key = match get_key(&id.id) {
                            Ok(key) => key,
                            Err(_) => continue,
                        };

                        let c = match conns.get_mut(key) {
                            Some(c) => c,
                            None => continue,
                        };

                        if c.id.as_ref().as_bytes() != id.id {
                            // key found but cid mismatch
                            continue;
                        }

                        handled += 1;

                        if c.handle_packet(now, &zresp, id.seq).is_err() {
                            continue;
                        }

                        if c.mode() == ZhttpMode::Stream && c.want.zhttp_read {
                            needs_process.add(key);
                        }
                    }

                    if handled == 0 {
                        debug!("worker {}: no conn for zmq message", id);
                    }
                }

                can_zstream_in_read = false;
            }

            Self::flush_send_to(&mut can_zstream_out_stream_write, &mut stream_handle);

            Self::zwrite_queue_next(
                can_zreq_write,
                can_zstream_out_write,
                &zwrite_nodes,
                &zwrite_req,
                &zwrite_stream,
                &zwrite_stream_to,
                &stream_handle,
                &mut needs_process,
            );

            while let Some(key) = needs_process.take() {
                let c = &mut conns[key];

                stream_handle.set_send_to_allowed(false);

                if let Some(k) = c.zwrite_key {
                    let (at_head, is_stream_to) = match k {
                        ZWriteKey::Req(nkey) => (zwrite_req.head == Some(nkey), false),
                        ZWriteKey::Stream(nkey) => (zwrite_stream.head == Some(nkey), false),
                        ZWriteKey::StreamTo(nkey) => (zwrite_stream_to.head == Some(nkey), true),
                    };

                    // remove from zwrite before processing. if it's still
                    //   needed it will be re-added
                    if at_head {
                        if is_stream_to {
                            stream_handle.set_send_to_allowed(true);
                        }

                        c.zwrite_key = None;

                        Self::zwrite_remove(
                            k,
                            &mut zwrite_nodes,
                            &mut zwrite_req,
                            &mut zwrite_stream,
                            &mut zwrite_stream_to,
                        );
                    }
                }

                if c.process(
                    now,
                    &instance_id,
                    &mut req_handle,
                    &mut stream_handle,
                    &mut packet_buf,
                    &mut tmp_buf,
                ) {
                    debug!("conn {}: destroying", c.id);

                    if let Some(stream) = c.get_tcp() {
                        poll.deregister(stream).unwrap();
                    }

                    if let Some(k) = c.zwrite_key.take() {
                        Self::zwrite_remove(
                            k,
                            &mut zwrite_nodes,
                            &mut zwrite_req,
                            &mut zwrite_stream,
                            &mut zwrite_stream_to,
                        );
                    }

                    if let Some((timer_id, _)) = c.timer {
                        timers.remove(timer_id);
                    }

                    match c.mode() {
                        ZhttpMode::Req => req_count -= 1,
                        ZhttpMode::Stream => stream_count -= 1,
                    }

                    conns.remove(key);
                    continue;
                }

                if c.state() == ServerState::Ready {
                    let id = Self::gen_id(id, key, &mut next_cid);
                    c.start(id.as_ref());

                    needs_process.add(key);
                    continue;
                }

                if c.want.zhttp_write {
                    match c.mode() {
                        ZhttpMode::Req => {
                            can_zreq_write = false;
                        }
                        ZhttpMode::Stream => {
                            can_zstream_out_write = false;
                        }
                    }
                }

                if c.zwrite_key.is_none() && (c.want.zhttp_write || c.want.zhttp_write_to) {
                    let nkey = zwrite_nodes.insert(list::Node::new(ZWrite::Connection(key)));

                    match c.mode() {
                        ZhttpMode::Req => {
                            c.zwrite_key = Some(ZWriteKey::Req(nkey));
                            zwrite_req.push_back(&mut zwrite_nodes, nkey);
                        }
                        ZhttpMode::Stream => {
                            if c.want.zhttp_write_to {
                                c.zwrite_key = Some(ZWriteKey::StreamTo(nkey));
                                zwrite_stream_to.push_back(&mut zwrite_nodes, nkey);
                            } else {
                                c.zwrite_key = Some(ZWriteKey::Stream(nkey));
                                zwrite_stream.push_back(&mut zwrite_nodes, nkey);
                            }
                        }
                    }
                }

                Self::flush_send_to(&mut can_zstream_out_stream_write, &mut stream_handle);

                Self::zwrite_queue_next(
                    can_zreq_write,
                    can_zstream_out_write,
                    &zwrite_nodes,
                    &zwrite_req,
                    &zwrite_stream,
                    &zwrite_stream_to,
                    &stream_handle,
                    &mut needs_process,
                );

                if let Some(want_exp_time) = c.want.timeout {
                    // convert to ticks
                    let want_exp_time = duration_to_ticks(want_exp_time - start_time);

                    let mut add = false;

                    if let Some((timer_id, exp_time)) = c.timer {
                        if want_exp_time != exp_time {
                            timers.remove(timer_id);
                            add = true;
                        }
                    } else {
                        add = true;
                    }

                    if add {
                        let timer_id = timers.add(want_exp_time, key).unwrap();
                        c.timer = Some((timer_id, want_exp_time));
                    }
                } else {
                    if let Some((timer_id, _)) = c.timer {
                        timers.remove(timer_id);
                        c.timer = None;
                    }
                }
            }

            if now >= last_keep_alive_time + KEEP_ALIVE_INTERVAL && server_zwrite_key.is_none() {
                let nkey = zwrite_nodes.insert(list::Node::new(ZWrite::Server));
                server_zwrite_key = Some(nkey);
                zwrite_stream_to.push_back(&mut zwrite_nodes, nkey);
            }

            let mut do_keep_alives = false;

            stream_handle.set_send_to_allowed(true);

            // is there room to write and the server is up next?
            if stream_handle.pending_send_to() == 0 {
                if let Some(nkey) = zwrite_stream_to.head {
                    let n = &zwrite_nodes[nkey];
                    match n.value {
                        ZWrite::Connection(_) => {}
                        ZWrite::Server => {
                            zwrite_stream_to.remove(&mut zwrite_nodes, nkey);

                            server_zwrite_key = None;
                            zwrite_nodes.remove(nkey);

                            do_keep_alives = true;
                        }
                    }
                }
            }

            if do_keep_alives {
                ka_nodes.clear();
                ka_addrs.clear();

                for _ in 0..ka_nodes.capacity() {
                    let key = next_keep_alive_index;
                    if key == conns.capacity() {
                        next_keep_alive_index = 0;
                        break;
                    }

                    next_keep_alive_index += 1;

                    if let Some(c) = conns.get(key) {
                        // only send keep-alives to stream connections
                        let conn = match &c.conn {
                            ServerConnection::Stream(conn) => conn,
                            _ => continue,
                        };

                        // only send keep-alives to connections with known handler addresses
                        let addr = match conn.to_addr() {
                            Some(addr) => addr,
                            None => continue,
                        };

                        let mut pos = ka_addrs.len();

                        for (i, a) in ka_addrs.iter().enumerate() {
                            if a.0.as_ref() == addr {
                                pos = i;
                            }
                        }

                        if pos == ka_addrs.len() {
                            // connection limits to_addr to 64 so this is guaranteed to succeed
                            let mut a = ArrayVec::new();
                            a.try_extend_from_slice(addr).unwrap();

                            ka_addrs.push((a, list::List::default()));
                        }

                        let node = ka_nodes.insert(list::Node::new(key));
                        ka_addrs[pos].1.push_back(&mut ka_nodes, node);
                    }
                }

                for (addr, keys) in ka_addrs.iter() {
                    let addr = addr.as_ref();

                    let mut ka_ids = arena::recycle_vec(ka_ids_mem);

                    // get ids/seqs
                    let mut next = keys.head;
                    while let Some(nkey) = next {
                        let n = &ka_nodes[nkey];

                        let c = &conns[n.value];

                        // this must succeed since we checked it earlier
                        let conn = match &c.conn {
                            ServerConnection::Stream(conn) => conn,
                            _ => unreachable!(),
                        };

                        ka_ids.push(zhttppacket::Id {
                            id: c.id.as_bytes(),
                            seq: Some(conn.out_seq()),
                        });

                        next = n.next;
                    }

                    debug!(
                        "worker {}: sending keep alives for {} sessions",
                        id,
                        ka_ids.len()
                    );

                    let zreq = zhttppacket::Request::new_keep_alive(instance_id.as_bytes(), &[]);

                    send_batched(zreq, &ka_ids, &mut stream_handle, addr);

                    ka_ids_mem = arena::recycle_vec(ka_ids);

                    // inc seqs
                    let mut next = keys.head;
                    while let Some(nkey) = next {
                        let n = &ka_nodes[nkey];

                        let c = &mut conns[n.value];

                        // this must succeed since we checked it earlier
                        let conn = match &mut c.conn {
                            ServerConnection::Stream(conn) => conn,
                            _ => unreachable!(),
                        };

                        conn.inc_out_seq();

                        next = n.next;
                    }
                }

                if now - last_keep_alive_time >= KEEP_ALIVE_INTERVAL * 2 {
                    // got really behind somehow. just skip ahead
                    last_keep_alive_time = now;
                } else {
                    // keep steady pace
                    last_keep_alive_time += KEEP_ALIVE_INTERVAL;
                }
            }

            let timeout = if (can_req_accept && req_count < req_maxconn)
                || (can_stream_accept && stream_count < stream_maxconn)
                || (can_zreq_write && !zwrite_req.is_empty())
                || (can_zstream_out_write && !zwrite_stream.is_empty())
                || (stream_handle.pending_send_to() == 0 && !zwrite_stream_to.is_empty())
                || (can_zstream_out_stream_write && stream_handle.pending_send_to() > 0)
            {
                Duration::from_millis(0)
            } else if let Some(t) = timers.timeout() {
                cmp::min(ticks_to_duration(t), POLL_TIMEOUT_MAX)
            } else {
                POLL_TIMEOUT_MAX
            };

            poll.poll(&mut events, Some(timeout)).unwrap();

            let mut done = false;

            for event in events.iter() {
                match event.token() {
                    mio::Token(0) => {
                        if stop.try_recv().is_ok() {
                            done = true;
                            break;
                        }
                    }
                    mio::Token(1) => {
                        debug!("worker {}: req accept event", id);
                        can_req_accept = true;
                    }
                    mio::Token(2) => {
                        debug!("worker {}: stream accept event", id);
                        can_stream_accept = true;
                    }
                    mio::Token(4) => {
                        debug!("worker {}: zhttp req read event", id);
                        can_zreq_read = true;
                    }
                    mio::Token(5) => {
                        debug!("worker {}: zhttp req write event", id);
                        can_zreq_write = true;
                    }
                    mio::Token(6) => {
                        debug!("worker {}: zhttp stream in read event", id);
                        can_zstream_in_read = true;
                    }
                    mio::Token(7) => {
                        debug!("worker {}: zhttp stream out write event", id);
                        can_zstream_out_write = true;
                    }
                    mio::Token(8) => {
                        debug!("worker {}: zhttp stream out stream write event", id);
                        can_zstream_out_stream_write = true;
                    }
                    token => {
                        let key = (usize::from(token) - CONN_BASE) / 4;

                        let c = &mut conns[key];

                        let using_tls = match &c.stream {
                            Stream::Tls(_) => true,
                            _ => false,
                        };

                        let readable = event.readiness().is_readable();
                        let writable = event.readiness().is_writable();

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
                            needs_process.add(key);
                        }
                    }
                }
            }

            if done {
                break;
            }
        }

        // reuse ka_* vars to send cancels

        let mut next_cancel_index = 0;

        while next_cancel_index < conns.capacity() {
            ka_nodes.clear();
            ka_addrs.clear();

            while ka_nodes.len() < ka_nodes.capacity() {
                let key = next_cancel_index;

                next_cancel_index += 1;

                if next_cancel_index == conns.capacity() {
                    break;
                }

                if let Some(c) = conns.get(key) {
                    // only send cancels to stream connections
                    let conn = match &c.conn {
                        ServerConnection::Stream(conn) => conn,
                        _ => continue,
                    };

                    // only send cancels to connections with known handler addresses
                    let addr = match conn.to_addr() {
                        Some(addr) => addr,
                        None => continue,
                    };

                    let mut pos = ka_addrs.len();

                    for (i, a) in ka_addrs.iter().enumerate() {
                        if a.0.as_ref() == addr {
                            pos = i;
                        }
                    }

                    if pos == ka_addrs.len() {
                        // connection limits to_addr to 64 so this is guaranteed to succeed
                        let mut a = ArrayVec::new();
                        a.try_extend_from_slice(addr).unwrap();

                        ka_addrs.push((a, list::List::default()));
                    }

                    let node = ka_nodes.insert(list::Node::new(key));
                    ka_addrs[pos].1.push_back(&mut ka_nodes, node);
                }
            }

            for (addr, keys) in ka_addrs.iter() {
                let addr = addr.as_ref();

                let mut ka_ids = arena::recycle_vec(ka_ids_mem);

                // get ids/seqs
                let mut next = keys.head;
                while let Some(nkey) = next {
                    let n = &ka_nodes[nkey];

                    let c = &conns[n.value];

                    // this must succeed since we checked it earlier
                    let conn = match &c.conn {
                        ServerConnection::Stream(conn) => conn,
                        _ => unreachable!(),
                    };

                    ka_ids.push(zhttppacket::Id {
                        id: c.id.as_bytes(),
                        seq: Some(conn.out_seq()),
                    });

                    next = n.next;
                }

                debug!(
                    "worker {}: sending cancels for {} sessions",
                    id,
                    ka_ids.len()
                );

                let zreq = zhttppacket::Request::new_cancel(instance_id.as_bytes(), &[]);

                send_batched(zreq, &ka_ids, &mut stream_handle, addr);

                ka_ids_mem = arena::recycle_vec(ka_ids);

                // inc seqs
                let mut next = keys.head;
                while let Some(nkey) = next {
                    let n = &ka_nodes[nkey];

                    let c = &mut conns[n.value];

                    // this must succeed since we checked it earlier
                    let conn = match &mut c.conn {
                        ServerConnection::Stream(conn) => conn,
                        _ => unreachable!(),
                    };

                    conn.inc_out_seq();

                    next = n.next;
                }
            }

            Self::flush_send_to(&mut can_zstream_out_stream_write, &mut stream_handle);

            // give zsockman some time to process pending messages
            thread::sleep(Duration::from_millis(10));
        }

        debug!("worker: {} stopped", id);
    }
}

impl Drop for Worker {
    fn drop(&mut self) {
        self.stop.try_send(()).unwrap();

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
    ) -> Result<Self, String> {
        let identities = Arc::new(IdentityCache::new(certs_dir));

        let mut req_tcp_listeners = Vec::new();
        let mut stream_tcp_listeners = Vec::new();

        let mut req_acceptor_tls = Vec::new();
        let mut stream_acceptor_tls = Vec::new();

        let zsockman = Arc::new(zsockman);

        let mut addrs = Vec::new();

        for lc in listen_addrs.iter() {
            let l = match TcpListener::bind(&lc.addr) {
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

        let mut zsockman = zhttpsocket::SocketManager::new(
            Arc::clone(&zmq_context),
            "test",
            MSG_RETAINED_MAX * workers,
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
            100,
            100,
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
        )
        .unwrap();

        let (s, r) = channel::channel(1);

        let thread = thread::spawn(move || {
            Self::run(r, zmq_context);
        });

        Self {
            server,
            thread: Some(thread),
            stop: s,
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

    fn run(stop: channel::Receiver<()>, zmq_context: Arc<zmq::Context>) {
        let rep_sock = zmq_context.socket(zmq::REP).unwrap();
        rep_sock.connect("inproc://server-test").unwrap();

        let in_sock = zmq_context.socket(zmq::PULL).unwrap();
        in_sock.connect("inproc://server-test-out").unwrap();

        let in_stream_sock = zmq_context.socket(zmq::ROUTER).unwrap();
        in_stream_sock.set_identity(b"handler").unwrap();
        in_stream_sock
            .connect("inproc://server-test-out-stream")
            .unwrap();

        let out_sock = zmq_context.socket(zmq::PUB).unwrap();
        out_sock.connect("inproc://server-test-in").unwrap();

        let poll = mio::Poll::new().unwrap();

        poll.register(
            stop.get_read_registration(),
            mio::Token(0),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            &EventedFd(&rep_sock.get_fd().unwrap()),
            mio::Token(1),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            &EventedFd(&in_sock.get_fd().unwrap()),
            mio::Token(2),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            &EventedFd(&in_stream_sock.get_fd().unwrap()),
            mio::Token(3),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        let mut events = mio::Events::with_capacity(1024);

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

            poll.poll(&mut events, None).unwrap();

            let mut done = false;

            for event in events.iter() {
                match event.token() {
                    mio::Token(0) => {
                        if stop.try_recv().is_ok() {
                            done = true;
                            break;
                        }
                    }
                    mio::Token(1) => {
                        rep_events = rep_sock.get_events().unwrap();
                    }
                    mio::Token(2) => {
                        in_events = in_sock.get_events().unwrap();
                    }
                    mio::Token(3) => {
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
        &mut data[size..(size + body.len())].copy_from_slice(body);
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
        &mut data[size..(size + body.len())].copy_from_slice(body);
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
        &mut data[size..(size + body.len())].copy_from_slice(body);
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
