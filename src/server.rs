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
    ServerReqConnection, ServerState, ServerStreamConnection, Shutdown, Want, ZhttpSender,
};
use crate::event;
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
use mio::net::{TcpListener, TcpSocket, TcpStream};
use mio::unix::SourceFd;
use slab::Slab;
use std::cell::Cell;
use std::cmp;
use std::collections::VecDeque;
use std::convert::TryFrom;
use std::io;
use std::io::{Read, Write};
use std::net::SocketAddr;
use std::os::unix::io::{FromRawFd, IntoRawFd};
use std::path::Path;
use std::rc::Rc;
use std::str;
use std::str::FromStr;
use std::sync::mpsc;
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

// we read and process each message one at a time, dropping each message before reading the next
pub const MSG_RETAINED_MAX: usize = 1;

const STOP_TOKEN: mio::Token = mio::Token(1);
const REQ_ACCEPTOR_TOKEN: mio::Token = mio::Token(2);
const STREAM_ACCEPTOR_TOKEN: mio::Token = mio::Token(3);
const REQ_HANDLE_READ_TOKEN: mio::Token = mio::Token(4);
const REQ_HANDLE_WRITE_TOKEN: mio::Token = mio::Token(5);
const STREAM_HANDLE_READ_TOKEN: mio::Token = mio::Token(6);
const STREAM_HANDLE_WRITE_ANY_TOKEN: mio::Token = mio::Token(7);
const STREAM_HANDLE_WRITE_ADDR_TOKEN: mio::Token = mio::Token(8);
const ZREQ_RECEIVER_TOKEN: mio::Token = mio::Token(9);
const ZSTREAM_OUT_RECEIVER_TOKEN: mio::Token = mio::Token(10);
const ZSTREAM_OUT_STREAM_RECEIVER_TOKEN: mio::Token = mio::Token(11);
const ZSTREAM_OUT_STREAM_SENDER_TOKEN: mio::Token = mio::Token(12);

const BASE_TOKENS: usize = 12;
const CONN_BASE: usize = 16;
const TOKENS_PER_CONN: usize = 8;
const ACCEPT_PER_LOOP_MAX: usize = 100;
const TICK_DURATION_MS: u64 = 10;
const POLL_TIMEOUT_MAX: Duration = Duration::from_millis(100);

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
    handle: &mut zhttpsocket::ClientStreamHandle,
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

        if let Err(e) = handle.send_to_addr(to_addr, msg) {
            let e = match e {
                zhttpsocket::SendError::Full(_) => io::Error::from(io::ErrorKind::WriteZero),
                zhttpsocket::SendError::Io(e) => e,
            };

            error!("zhttp write error: {:?}", e);
            break;
        }
    }
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

#[derive(Copy, Clone, PartialEq)]
pub enum ZhttpMode {
    Req,
    Stream,
}

enum Stream {
    Plain(TcpStream),
    Tls(TlsStream),
}

impl Stream {
    fn get_tcp(&mut self) -> Option<&mut TcpStream> {
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
}

impl Connection {
    fn new_req(
        stream: Stream,
        peer_addr: SocketAddr,
        buffer_size: usize,
        body_buffer_size: usize,
        rb_tmp: &Rc<TmpBuffer>,
        timeout: Duration,
        sender: channel::LocalSender<zmq::Message>,
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
                    Instant::now(),
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
        }
    }

    fn new_stream(
        stream: Stream,
        peer_addr: SocketAddr,
        buffer_size: usize,
        messages_max: usize,
        rb_tmp: &Rc<TmpBuffer>,
        timeout: Duration,
        senders: StreamLocalSenders,
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
                    Instant::now(),
                    Some(peer_addr),
                    secure,
                    buffer_size,
                    messages_max,
                    rb_tmp,
                    timeout,
                ),
                senders,
            ),
            want: Want::nothing(),
            timer: None,
        }
    }

    fn mode(&self) -> ZhttpMode {
        match &self.conn {
            ServerConnection::Req(_, _) => ZhttpMode::Req,
            ServerConnection::Stream(_, _) => ZhttpMode::Stream,
        }
    }

    fn state(&self) -> ServerState {
        match &self.conn {
            ServerConnection::Req(conn, _) => conn.state(),
            ServerConnection::Stream(conn, _) => conn.state(),
        }
    }

    fn get_tcp(&mut self) -> Option<&mut TcpStream> {
        self.stream.get_tcp()
    }

    fn get_zreq_sender(&self) -> &channel::LocalSender<zmq::Message> {
        match &self.conn {
            ServerConnection::Req(_, sender) => sender,
            ServerConnection::Stream(_, _) => panic!("not req conn"),
        }
    }

    fn get_zstream_senders(&self) -> &StreamLocalSenders {
        match &self.conn {
            ServerConnection::Req(_, _) => panic!("not stream conn"),
            ServerConnection::Stream(_, senders) => senders,
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

    fn set_sock_readable(&mut self) {
        match &mut self.conn {
            ServerConnection::Req(conn, _) => conn.set_sock_readable(),
            ServerConnection::Stream(conn, _) => conn.set_sock_readable(),
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

    fn deregister(&mut self, poller: &event::Poller) {
        if let Some(stream) = self.stream.get_tcp() {
            poller.deregister(stream).unwrap();
        }

        match &self.conn {
            ServerConnection::Req(_, sender) => {
                poller
                    .deregister_custom(sender.get_write_registration())
                    .unwrap();
            }
            ServerConnection::Stream(_, senders) => {
                poller
                    .deregister_custom(&senders.out.get_write_registration())
                    .unwrap();
                poller
                    .deregister_custom(&senders.out_stream.get_write_registration())
                    .unwrap();
            }
        }
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

        // BASE_TOKENS + 1 per req connection + 2 per stream connection
        let mut poller =
            event::Poller::new(BASE_TOKENS + req_maxconn + (stream_maxconn * 2)).unwrap();

        poller
            .register_custom(
                stop.get_read_registration(),
                STOP_TOKEN,
                mio::Interest::READABLE,
            )
            .unwrap();

        poller
            .register_custom(
                req_acceptor.get_read_registration(),
                REQ_ACCEPTOR_TOKEN,
                mio::Interest::READABLE,
            )
            .unwrap();

        poller
            .register_custom(
                stream_acceptor.get_read_registration(),
                STREAM_ACCEPTOR_TOKEN,
                mio::Interest::READABLE,
            )
            .unwrap();

        let mut req_handle = zsockman.client_req_handle(format!("{}-", id).as_bytes());
        let mut stream_handle = zsockman.client_stream_handle(format!("{}-", id).as_bytes());

        poller
            .register_custom(
                req_handle.get_read_registration(),
                REQ_HANDLE_READ_TOKEN,
                mio::Interest::READABLE,
            )
            .unwrap();

        poller
            .register_custom(
                req_handle.get_write_registration(),
                REQ_HANDLE_WRITE_TOKEN,
                mio::Interest::WRITABLE,
            )
            .unwrap();

        poller
            .register_custom(
                stream_handle.get_read_registration(),
                STREAM_HANDLE_READ_TOKEN,
                mio::Interest::READABLE,
            )
            .unwrap();

        poller
            .register_custom(
                stream_handle.get_write_any_registration(),
                STREAM_HANDLE_WRITE_ANY_TOKEN,
                mio::Interest::WRITABLE,
            )
            .unwrap();

        poller
            .register_custom(
                stream_handle.get_write_addr_registration(),
                STREAM_HANDLE_WRITE_ADDR_TOKEN,
                mio::Interest::WRITABLE,
            )
            .unwrap();

        // bound is 1, for fairness. sends from multiple connections will be interleaved
        // max_senders is 1 per connection + 1 for the worker itself
        let (zreq_sender, zreq_receiver) = channel::local_channel(1, req_maxconn + 1);
        let (zstream_out_sender, zstream_out_receiver) =
            channel::local_channel(1, stream_maxconn + 1);
        let (zstream_out_stream_sender, zstream_out_stream_receiver) =
            channel::local_channel(1, stream_maxconn + 1);

        poller
            .register_custom(
                zreq_receiver.get_read_registration(),
                ZREQ_RECEIVER_TOKEN,
                mio::Interest::READABLE,
            )
            .unwrap();

        poller
            .register_custom(
                zstream_out_receiver.get_read_registration(),
                ZSTREAM_OUT_RECEIVER_TOKEN,
                mio::Interest::READABLE,
            )
            .unwrap();

        poller
            .register_custom(
                zstream_out_stream_receiver.get_read_registration(),
                ZSTREAM_OUT_STREAM_RECEIVER_TOKEN,
                mio::Interest::READABLE,
            )
            .unwrap();

        poller
            .register_custom(
                zstream_out_stream_sender.get_write_registration(),
                ZSTREAM_OUT_STREAM_SENDER_TOKEN,
                mio::Interest::WRITABLE,
            )
            .unwrap();

        let mut zreq_receiver_ready = true;
        let mut zstream_out_receiver_ready = true;
        let mut zstream_out_stream_receiver_ready = true;
        let mut zstream_out_stream_sender_ready = true;
        let mut req_send_pending = None;
        let mut stream_out_send_pending = None;
        let mut stream_out_stream_send_pending = None;

        let mut can_req_accept = true;
        let mut can_stream_accept = true;
        let mut can_zreq_read = true;
        let mut can_zreq_write = true;
        let mut can_zstream_in_read = true;
        let mut can_zstream_out_write = true;
        let mut can_zstream_out_stream_write = true;

        let mut last_keep_alive_time = Instant::now();
        let mut next_keep_alive_index = 0;

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

                let stream = set_socket_opts(stream);

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

                let zreq_sender = zreq_sender.try_clone().unwrap();

                let entry = conns.vacant_entry();
                let key = entry.key();

                let c = Connection::new_req(
                    stream,
                    peer_addr,
                    buffer_size,
                    body_buffer_size,
                    &rb_tmp,
                    req_timeout,
                    zreq_sender,
                );

                entry.insert(c);

                let c = &mut conns[key];

                debug!(
                    "worker {}: req conn starting {} {}/{}",
                    id, key, req_count, req_maxconn
                );

                let id = Self::gen_id(id, key, &mut next_cid);
                c.start(id.as_ref());

                let ready_flags = mio::Interest::READABLE | mio::Interest::WRITABLE;

                poller
                    .register(
                        c.get_tcp().unwrap(),
                        mio::Token(CONN_BASE + (key * TOKENS_PER_CONN) + 0),
                        ready_flags,
                    )
                    .unwrap();

                poller
                    .register_custom(
                        c.get_zreq_sender().get_write_registration(),
                        mio::Token(CONN_BASE + (key * TOKENS_PER_CONN) + 1),
                        mio::Interest::WRITABLE,
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

                let stream = set_socket_opts(stream);

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

                let zstream_senders = StreamLocalSenders::new(
                    zstream_out_sender.try_clone().unwrap(),
                    zstream_out_stream_sender.try_clone().unwrap(),
                );

                let entry = conns.vacant_entry();
                let key = entry.key();

                let c = Connection::new_stream(
                    stream,
                    peer_addr,
                    buffer_size,
                    messages_max,
                    &rb_tmp,
                    stream_timeout,
                    zstream_senders,
                );

                entry.insert(c);

                let c = &mut conns[key];

                debug!(
                    "worker {}: stream conn starting {} {}/{}",
                    id, key, stream_count, stream_maxconn
                );

                let id = Self::gen_id(id, key, &mut next_cid);
                c.start(id.as_ref());

                let ready_flags = mio::Interest::READABLE | mio::Interest::WRITABLE;

                poller
                    .register(
                        c.get_tcp().unwrap(),
                        mio::Token(CONN_BASE + (key * TOKENS_PER_CONN) + 0),
                        ready_flags,
                    )
                    .unwrap();

                poller
                    .register_custom(
                        c.get_zstream_senders().out.get_write_registration(),
                        mio::Token(CONN_BASE + (key * TOKENS_PER_CONN) + 1),
                        mio::Interest::WRITABLE,
                    )
                    .unwrap();

                poller
                    .register_custom(
                        c.get_zstream_senders().out_stream.get_write_registration(),
                        mio::Token(CONN_BASE + (key * TOKENS_PER_CONN) + 2),
                        mio::Interest::WRITABLE,
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
                    let msg = match stream_handle.recv() {
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

            while let Some(key) = needs_process.take() {
                let c = &mut conns[key];

                if c.process(now, &instance_id, &mut packet_buf, &mut tmp_buf) {
                    debug!("conn {}: destroying", c.id);

                    c.deregister(&poller);

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

            let mut do_keep_alives = false;

            if now >= last_keep_alive_time + KEEP_ALIVE_INTERVAL && zstream_out_stream_sender_ready
            {
                if zstream_out_stream_sender.check_send() {
                    // if check_send returns true, we are guaranteed to be able to send
                    do_keep_alives = true;
                } else {
                    // if check_send returns false, we'll be on the waitlist for a notification
                    zstream_out_stream_sender_ready = false;
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
                            ServerConnection::Stream(conn, _) => conn,
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
                            ServerConnection::Stream(conn, _) => conn,
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
                            ServerConnection::Stream(conn, _) => conn,
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

            if req_send_pending.is_none() && zreq_receiver_ready {
                match zreq_receiver.try_recv() {
                    Ok(msg) => req_send_pending = Some(msg),
                    Err(mpsc::TryRecvError::Empty) => zreq_receiver_ready = false,
                    Err(mpsc::TryRecvError::Disconnected) => unreachable!(),
                }
            }

            if can_zreq_write {
                if let Some(msg) = req_send_pending.take() {
                    match req_handle.send(msg) {
                        Ok(()) => {}
                        Err(zhttpsocket::SendError::Full(msg)) => {
                            req_send_pending = Some(msg);

                            can_zreq_write = false;
                        }
                        Err(zhttpsocket::SendError::Io(e)) => error!("req send error: {}", e),
                    }
                }
            }

            if stream_out_send_pending.is_none() && zstream_out_receiver_ready {
                match zstream_out_receiver.try_recv() {
                    Ok(msg) => stream_out_send_pending = Some(msg),
                    Err(mpsc::TryRecvError::Empty) => zstream_out_receiver_ready = false,
                    Err(mpsc::TryRecvError::Disconnected) => unreachable!(),
                }
            }

            if can_zstream_out_write {
                if let Some(msg) = stream_out_send_pending.take() {
                    match stream_handle.send_to_any(msg) {
                        Ok(()) => {}
                        Err(zhttpsocket::SendError::Full(msg)) => {
                            stream_out_send_pending = Some(msg);

                            can_zstream_out_write = false;
                        }
                        Err(zhttpsocket::SendError::Io(e)) => {
                            error!("stream out send error: {}", e)
                        }
                    }
                }
            }

            if stream_out_stream_send_pending.is_none() && zstream_out_stream_receiver_ready {
                match zstream_out_stream_receiver.try_recv() {
                    Ok(msg) => stream_out_stream_send_pending = Some(msg),
                    Err(mpsc::TryRecvError::Empty) => zstream_out_stream_receiver_ready = false,
                    Err(mpsc::TryRecvError::Disconnected) => unreachable!(),
                }
            }

            if can_zstream_out_stream_write {
                if let Some((addr, msg)) = stream_out_stream_send_pending.take() {
                    match stream_handle.send_to_addr(&addr, msg) {
                        Ok(()) => {}
                        Err(zhttpsocket::SendError::Full(msg)) => {
                            stream_out_stream_send_pending = Some((addr, msg));

                            can_zstream_out_stream_write = false;
                        }
                        Err(zhttpsocket::SendError::Io(e)) => {
                            error!("stream out stream send error: {}", e)
                        }
                    }
                }
            }

            let timeout = if (can_req_accept && req_count < req_maxconn)
                || (can_stream_accept && stream_count < stream_maxconn)
                || (req_send_pending.is_none() && zreq_receiver_ready)
                || (can_zreq_write && req_send_pending.is_some())
                || (stream_out_send_pending.is_none() && zstream_out_receiver_ready)
                || (can_zstream_out_write && stream_out_send_pending.is_some())
                || (stream_out_stream_send_pending.is_none() && zstream_out_stream_receiver_ready)
                || (can_zstream_out_stream_write && stream_out_stream_send_pending.is_some())
            {
                Duration::from_millis(0)
            } else if let Some(t) = timers.timeout() {
                cmp::min(ticks_to_duration(t), POLL_TIMEOUT_MAX)
            } else {
                POLL_TIMEOUT_MAX
            };

            poller.poll(Some(timeout)).unwrap();

            let mut done = false;

            for event in poller.iter_events() {
                match event.token() {
                    STOP_TOKEN => {
                        if stop.try_recv().is_ok() {
                            done = true;
                            break;
                        }
                    }
                    REQ_ACCEPTOR_TOKEN => {
                        debug!("worker {}: req accept event", id);
                        can_req_accept = true;
                    }
                    STREAM_ACCEPTOR_TOKEN => {
                        debug!("worker {}: stream accept event", id);
                        can_stream_accept = true;
                    }
                    REQ_HANDLE_READ_TOKEN => {
                        debug!("worker {}: zhttp req read event", id);
                        can_zreq_read = true;
                    }
                    REQ_HANDLE_WRITE_TOKEN => {
                        debug!("worker {}: zhttp req write event", id);
                        can_zreq_write = true;
                    }
                    STREAM_HANDLE_READ_TOKEN => {
                        debug!("worker {}: zhttp stream in read event", id);
                        can_zstream_in_read = true;
                    }
                    STREAM_HANDLE_WRITE_ANY_TOKEN => {
                        debug!("worker {}: zhttp stream out write event", id);
                        can_zstream_out_write = true;
                    }
                    STREAM_HANDLE_WRITE_ADDR_TOKEN => {
                        debug!("worker {}: zhttp stream out stream write event", id);
                        can_zstream_out_stream_write = true;
                    }
                    ZREQ_RECEIVER_TOKEN => {
                        debug!("worker {}: zreq receiver ready", id);
                        zreq_receiver_ready = true;
                    }
                    ZSTREAM_OUT_RECEIVER_TOKEN => {
                        debug!("worker {}: zstream out receiver ready", id);
                        zstream_out_receiver_ready = true;
                    }
                    ZSTREAM_OUT_STREAM_RECEIVER_TOKEN => {
                        debug!("worker {}: zstream out stream receiver ready", id);
                        zstream_out_stream_receiver_ready = true;
                    }
                    ZSTREAM_OUT_STREAM_SENDER_TOKEN => {
                        debug!("worker {}: zstream out stream sender ready", id);
                        zstream_out_stream_sender_ready = true;
                    }
                    token => {
                        let key = (usize::from(token) - CONN_BASE) / TOKENS_PER_CONN;
                        let subkey = (usize::from(token) - CONN_BASE) % TOKENS_PER_CONN;

                        let c = &mut conns[key];

                        if subkey == 0 {
                            let using_tls = match &c.stream {
                                Stream::Tls(_) => true,
                                _ => false,
                            };

                            let readable = event.is_readable();
                            let writable = event.is_writable();

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
                        } else if subkey == 1 {
                            // zhttp sender req/out ready
                            if c.want.zhttp_write {
                                needs_process.add(key);
                            }
                        } else if subkey == 2 {
                            c.set_out_stream_can_write();

                            // zhttp sender out_stream ready
                            if c.want.zhttp_write_to {
                                needs_process.add(key);
                            }
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
                        ServerConnection::Stream(conn, _) => conn,
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
                        ServerConnection::Stream(conn, _) => conn,
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
                        ServerConnection::Stream(conn, _) => conn,
                        _ => unreachable!(),
                    };

                    conn.inc_out_seq();

                    next = n.next;
                }
            }

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
