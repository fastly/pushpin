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

use crate::arena;
use crate::buffer::{Buffer, LimitBufs, RefRead, RingBuffer, TmpBuffer, VECTORED_MAX};
use crate::channel;
use crate::future::{event_wait, select_6, select_option, CancellationToken, Select6, Timeout};
use crate::http1;
use crate::pin_mut;
use crate::reactor;
use crate::reactor::Reactor;
use crate::websocket;
use crate::zhttppacket;
use crate::zhttpsocket;
use arrayvec::{ArrayString, ArrayVec};
use log::debug;
use std::cell::{Cell, Ref, RefCell};
use std::cmp;
use std::collections::VecDeque;
use std::io;
use std::io::{Read, Write};
use std::net::SocketAddr;
use std::pin::Pin;
use std::rc::Rc;
use std::str;
use std::str::FromStr;
use std::sync::mpsc;
use std::time::{Duration, Instant};

const URI_SIZE_MAX: usize = 4096;
const WS_HASH_INPUT_MAX: usize = 256;
const ZHTTP_SESSION_TIMEOUT: Duration = Duration::from_secs(60);

pub trait Shutdown {
    fn shutdown(&mut self) -> Result<(), io::Error>;
}

pub trait ZhttpSender {
    fn can_send_to(&self) -> bool;
    fn send(&mut self, message: zmq::Message) -> Result<(), zhttpsocket::SendError>;
    fn send_to(&mut self, addr: &[u8], message: zmq::Message)
        -> Result<(), zhttpsocket::SendError>;
}

pub trait CidProvider {
    fn get_new_assigned_cid(&mut self) -> ArrayString<[u8; 32]>;
}

pub trait Identify {
    fn set_id(&mut self, id: &str);
}

#[derive(PartialEq)]
enum Mode {
    HttpReq,
    HttpStream,
    WebSocket,
}

fn get_host<'a>(headers: &'a [httparse::Header]) -> &'a str {
    for h in headers.iter() {
        if h.name.eq_ignore_ascii_case("Host") {
            match str::from_utf8(h.value) {
                Ok(s) => return s,
                Err(_) => break,
            }
        }
    }

    "localhost"
}

fn make_zhttp_request(
    instance: &str,
    ids: &[zhttppacket::Id],
    method: &str,
    path: &str,
    headers: &[httparse::Header],
    body: &[u8],
    more: bool,
    mode: Mode,
    credits: u32,
    peer_addr: Option<SocketAddr>,
    secure: bool,
    packet_buf: &mut [u8],
) -> Result<zmq::Message, io::Error> {
    let mut data = zhttppacket::RequestData::new();

    data.method = method;

    let host = get_host(headers);

    let mut zheaders = [zhttppacket::EMPTY_HEADER; http1::HEADERS_MAX];
    let mut zheaders_len = 0;

    for h in headers.iter() {
        zheaders[zheaders_len] = zhttppacket::Header {
            name: h.name,
            value: h.value,
        };
        zheaders_len += 1;
    }
    data.headers = &zheaders[..zheaders_len];

    let scheme = match mode {
        Mode::HttpReq | Mode::HttpStream => {
            if secure {
                "https"
            } else {
                "http"
            }
        }
        Mode::WebSocket => {
            if secure {
                "wss"
            } else {
                "ws"
            }
        }
    };

    let mut uri = [0; URI_SIZE_MAX];
    let mut c = io::Cursor::new(&mut uri[..]);

    write!(&mut c, "{}://{}{}", scheme, host, path)?;

    let size = c.position() as usize;

    data.uri = match str::from_utf8(&uri[..size]) {
        Ok(s) => s,
        Err(_) => return Err(io::Error::from(io::ErrorKind::InvalidData)),
    };

    data.body = body;
    data.more = more;

    if mode == Mode::HttpStream {
        data.stream = true;
    }

    data.credits = credits;

    let mut addr = [0; 128];

    if let Some(peer_addr) = &peer_addr {
        let mut c = io::Cursor::new(&mut addr[..]);
        write!(&mut c, "{}", peer_addr.ip()).unwrap();
        let size = c.position() as usize;

        data.peer_address = str::from_utf8(&addr[..size]).unwrap();
        data.peer_port = peer_addr.port();
    }

    let mut zreq = zhttppacket::Request::new_data(instance.as_bytes(), &ids, data);
    zreq.multi = true;

    let size = zreq.serialize(packet_buf)?;

    Ok(zmq::Message::from(&packet_buf[..size]))
}

struct Want {
    sock_read: bool,
    sock_write: bool,
    zhttp_read: bool,
    zhttp_write: bool,
    zhttp_write_to: bool,
    timeout: Option<Instant>,
}

impl Want {
    fn nothing() -> Self {
        Self {
            sock_read: false,
            sock_write: false,
            zhttp_read: false,
            zhttp_write: false,
            zhttp_write_to: false,
            timeout: None,
        }
    }

    fn merge(&self, other: &Want) -> Want {
        let timeout = if self.timeout.is_some() && other.timeout.is_some() {
            let a = self.timeout.unwrap();
            let b = other.timeout.unwrap();
            Some(cmp::min(a, b))
        } else if self.timeout.is_some() && other.timeout.is_none() {
            self.timeout
        } else if self.timeout.is_none() && other.timeout.is_some() {
            other.timeout
        } else {
            // both none
            None
        };

        Want {
            sock_read: self.sock_read || other.sock_read,
            sock_write: self.sock_write || other.sock_write,
            zhttp_read: self.zhttp_read || other.zhttp_read,
            zhttp_write: self.zhttp_write || other.zhttp_write,
            zhttp_write_to: self.zhttp_write_to || other.zhttp_write_to,
            timeout,
        }
    }
}

#[derive(Debug, PartialEq, Clone, Copy)]
enum ServerState {
    // call: start
    Ready,

    // call: process, apply_zhttp_response
    // next: Connected, Ready, Finished
    Connected,

    // connection should be closed
    Finished,
}

#[derive(Debug)]
enum ServerError {
    Io(io::Error),
    Utf8(str::Utf8Error),
    Http(http1::ServerError),
    WebSocket(websocket::Error),
    InvalidWebSocketRequest,
    BadMessage,
    BufferExceeded,
    BadFrame,
}

impl From<io::Error> for ServerError {
    fn from(e: io::Error) -> Self {
        Self::Io(e)
    }
}

impl From<str::Utf8Error> for ServerError {
    fn from(e: str::Utf8Error) -> Self {
        Self::Utf8(e)
    }
}

impl From<http1::ServerError> for ServerError {
    fn from(e: http1::ServerError) -> Self {
        Self::Http(e)
    }
}

impl From<websocket::Error> for ServerError {
    fn from(e: websocket::Error) -> Self {
        Self::WebSocket(e)
    }
}

// our own range-like struct that supports copying
#[derive(Clone, Copy)]
struct Range {
    start: usize,
    end: usize,
}

fn slice_to_range<T: AsRef<[u8]>>(base: &[u8], s: T) -> Range {
    let sref = s.as_ref();
    let start = (sref.as_ptr() as usize) - (base.as_ptr() as usize);

    Range {
        start,
        end: start + sref.len(),
    }
}

fn range_to_slice(base: &[u8], range: Range) -> &[u8] {
    &base[range.start..range.end]
}

unsafe fn range_to_str_unchecked(base: &[u8], range: Range) -> &str {
    str::from_utf8_unchecked(&base[range.start..range.end])
}

#[derive(Clone, Copy)]
struct HeaderRanges {
    name: Range,
    value: Range,
}

const EMPTY_HEADER_RANGES: HeaderRanges = HeaderRanges {
    name: Range { start: 0, end: 0 },
    value: Range { start: 0, end: 0 },
};

#[derive(Clone, Copy)]
struct RequestHeaderRanges {
    method: Range,
    uri: Range,
    headers: [HeaderRanges; http1::HEADERS_MAX],
    headers_count: usize,
}

#[derive(Clone, Copy)]
struct MessageItem {
    mtype: u8,
    avail: usize,
}

struct MessageTracker {
    items: VecDeque<MessageItem>,
    last_partial: bool,
}

impl MessageTracker {
    fn new(max_messages: usize) -> Self {
        Self {
            items: VecDeque::with_capacity(max_messages),
            last_partial: false,
        }
    }

    fn clear(&mut self) {
        self.items.clear();
        self.last_partial = false;
    }

    fn in_progress(&self) -> bool {
        self.last_partial
    }

    fn start(&mut self, mtype: u8) -> Result<(), ()> {
        if self.last_partial || self.items.len() == self.items.capacity() {
            return Err(());
        }

        self.items.push_back(MessageItem { mtype, avail: 0 });

        self.last_partial = true;

        Ok(())
    }

    fn extend(&mut self, amt: usize) {
        assert_eq!(self.last_partial, true);

        self.items.back_mut().unwrap().avail += amt;
    }

    fn done(&mut self) {
        self.last_partial = false;
    }

    // type, avail, done
    fn current(&self) -> Option<(u8, usize, bool)> {
        if self.items.len() > 1 {
            let m = self.items.front().unwrap();
            Some((m.mtype, m.avail, true))
        } else if self.items.len() == 1 {
            let m = self.items.front().unwrap();
            Some((m.mtype, m.avail, !self.last_partial))
        } else {
            None
        }
    }

    fn consumed(&mut self, amt: usize, done: bool) {
        assert!(amt <= self.items[0].avail);

        self.items[0].avail -= amt;

        if done {
            assert_eq!(self.items[0].avail, 0);

            self.items.pop_front().unwrap();
        }
    }
}

struct ServerProcessArgs<'a, S, Z>
where
    S: Read + Write + Shutdown,
    Z: ZhttpSender,
{
    now: Instant,
    instance: &'a str,
    sock: &'a mut S,
    zsender: &'a mut Z,
    packet_buf: &'a mut [u8],
    tmp_buf: &'a mut [u8],
}

#[derive(Debug, PartialEq)]
enum ServerReqState {
    Ready,
    Active,
    ShuttingDown,
    Finishing,
    Finished,
}

struct ServerReqConnection {
    id: ArrayString<[u8; 32]>,
    peer_addr: Option<SocketAddr>,
    secure: bool,
    timeout: Duration,
    state: ServerReqState,
    protocol: http1::ServerProtocol,
    exp_time: Option<Instant>,
    req: Option<RequestHeaderRanges>,
    buf1: RingBuffer,
    buf2: RingBuffer,
    body_buf: Buffer,
    cont: [u8; 32],
    cont_len: usize,
    cont_left: usize,
    pending_msg: Option<zmq::Message>,
    sock_readable: bool,
}

impl ServerReqConnection {
    fn new(
        now: Instant,
        peer_addr: Option<SocketAddr>,
        secure: bool,
        buffer_size: usize,
        body_buffer_size: usize,
        rb_tmp: &Rc<TmpBuffer>,
        timeout: Duration,
    ) -> Self {
        let buf1 = RingBuffer::new(buffer_size, rb_tmp);
        let buf2 = RingBuffer::new(buffer_size, rb_tmp);
        let body_buf = Buffer::new(body_buffer_size);

        Self {
            id: ArrayString::new(),
            peer_addr,
            secure,
            timeout,
            state: ServerReqState::Ready,
            protocol: http1::ServerProtocol::new(),
            exp_time: Some(now + timeout),
            req: None,
            buf1,
            buf2,
            body_buf,
            cont: [0; 32],
            cont_len: 0,
            cont_left: 0,
            pending_msg: None,
            sock_readable: true,
        }
    }

    fn reset(&mut self, now: Instant) {
        // note: buf1 is not cleared as there may be data to read

        self.state = ServerReqState::Ready;
        self.protocol = http1::ServerProtocol::new();
        self.exp_time = Some(now + self.timeout);
        self.req = None;
        self.buf2.clear();
        self.body_buf.clear();
        self.pending_msg = None;
        self.sock_readable = true;
    }

    fn state(&self) -> ServerState {
        match self.state {
            ServerReqState::Ready => ServerState::Ready,
            ServerReqState::Finished => ServerState::Finished,
            _ => ServerState::Connected,
        }
    }

    fn start(&mut self, id: &str) {
        self.id = ArrayString::from_str(id).unwrap();
        self.state = ServerReqState::Active;
    }

    fn set_sock_readable(&mut self) {
        self.sock_readable = true;
    }

    fn process<S, Z>(
        &mut self,
        now: Instant,
        sock: &mut S,
        zsender: &mut Z,
        packet_buf: &mut [u8],
    ) -> Result<Want, ServerError>
    where
        S: Read + Write + Shutdown,
        Z: ZhttpSender,
    {
        loop {
            let args = ServerProcessArgs {
                now,
                instance: "",
                sock,
                zsender,
                packet_buf,
                tmp_buf: &mut [0; 0],
            };

            if let Some(r) = self.process_step(args) {
                if let Err(e) = &r {
                    match self.state {
                        ServerReqState::Finishing | ServerReqState::Finished => {}
                        _ => {
                            debug!("conn {}: error: {:?}", self.id, e);
                            self.state = ServerReqState::Finishing;
                            continue;
                        }
                    }
                }

                return r;
            }
        }
    }

    fn try_recv(&mut self, sock: &mut dyn io::Read) -> Result<bool, io::Error> {
        if self.buf1.write_avail() == 0 {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        if !self.sock_readable {
            return Err(io::Error::from(io::ErrorKind::WouldBlock));
        }

        let size = match self.buf1.write_from(sock) {
            Ok(size) => size,
            Err(e) => {
                if e.kind() == io::ErrorKind::WouldBlock {
                    self.sock_readable = false;
                }

                return Err(e);
            }
        };

        let closed = size == 0;

        if closed {
            self.state = ServerReqState::ShuttingDown;
        }

        Ok(closed)
    }

    fn after_request<S, Z>(&mut self, args: ServerProcessArgs<'_, S, Z>) -> Result<(), ServerError>
    where
        S: Read + Write + Shutdown,
        Z: ZhttpSender,
    {
        let proto = &mut self.protocol;

        let hbuf = self.buf2.read_buf();
        let ranges = self.req.unwrap();

        let method = unsafe { range_to_str_unchecked(hbuf, ranges.method) };
        let path = unsafe { range_to_str_unchecked(hbuf, ranges.uri) };

        let mut headers = [httparse::EMPTY_HEADER; http1::HEADERS_MAX];

        for (i, h) in ranges.headers.iter().enumerate() {
            headers[i].name = unsafe { range_to_str_unchecked(hbuf, h.name) };
            headers[i].value = range_to_slice(hbuf, h.value);
        }

        let headers = &headers[..ranges.headers_count];

        let mut websocket = false;

        for h in headers.iter() {
            if h.name.eq_ignore_ascii_case("Upgrade") && h.value == b"websocket" {
                websocket = true;
                break;
            }
        }

        if websocket {
            // header consumed
            self.buf2.clear();

            // body sent
            self.body_buf.clear();

            let mut hbuf = io::Cursor::new(self.buf2.write_buf());

            let headers = &[http1::Header {
                name: "Content-Type",
                value: b"text/plain",
            }];

            let body = "WebSockets not supported on req mode interface.\n";

            if let Err(e) = proto.send_response(
                &mut hbuf,
                400,
                "Bad Request",
                headers,
                http1::BodySize::Known(body.len()),
            ) {
                return Err(e.into());
            }

            let size = hbuf.position() as usize;
            self.buf2.write_commit(size);

            if let Err(e) = self.body_buf.write_all(body.as_bytes()) {
                return Err(ServerError::Io(e));
            }

            return Ok(());
        }

        let ids = [zhttppacket::Id {
            id: self.id.as_bytes(),
            seq: None,
        }];

        let msg = match make_zhttp_request(
            "",
            &ids,
            method,
            path,
            headers,
            self.body_buf.read_buf(),
            false,
            Mode::HttpReq,
            0,
            self.peer_addr,
            self.secure,
            args.packet_buf,
        ) {
            Ok(msg) => msg,
            Err(e) => return Err(e.into()),
        };

        // header and body consumed
        self.buf2.clear();
        self.body_buf.clear();

        self.pending_msg = Some(msg);

        Ok(())
    }

    fn process_step<S, Z>(
        &mut self,
        args: ServerProcessArgs<'_, S, Z>,
    ) -> Option<Result<Want, ServerError>>
    where
        S: Read + Write + Shutdown,
        Z: ZhttpSender,
    {
        // check expiration if not already shutting down
        match self.state {
            ServerReqState::Finishing | ServerReqState::Finished => {}
            _ => {
                if self.exp_time.is_some() && args.now >= self.exp_time.unwrap() {
                    self.state = ServerReqState::Finishing;
                }
            }
        }

        match self.state {
            ServerReqState::Active => {
                return self.process_http(args);
            }
            ServerReqState::ShuttingDown => {
                match args.sock.shutdown() {
                    Ok(()) => {}
                    Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                        let mut want = Want::nothing();
                        want.sock_read = true;
                        want.sock_write = true;
                        want.timeout = self.exp_time;
                        return Some(Ok(want));
                    }
                    Err(e) => return Some(Err(e.into())),
                }

                self.state = ServerReqState::Finishing;

                return None;
            }
            ServerReqState::Finishing => {
                self.state = ServerReqState::Finished;

                return None;
            }
            ServerReqState::Ready | ServerReqState::Finished => {
                return Some(Ok(Want::nothing()));
            }
        }
    }

    fn process_http<S, Z>(
        &mut self,
        args: ServerProcessArgs<'_, S, Z>,
    ) -> Option<Result<Want, ServerError>>
    where
        S: Read + Write + Shutdown,
        Z: ZhttpSender,
    {
        let mut want = Want::nothing();
        want.sock_read = true;
        want.timeout = self.exp_time;

        // always read if possible, to detect disconnects
        match self.try_recv(args.sock) {
            Ok(closed) => {
                if closed {
                    return None;
                }
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {} // ok
            Err(e) if e.kind() == io::ErrorKind::WriteZero => want.sock_read = false,
            Err(e) => return Some(Err(e.into())),
        }

        let proto = &mut self.protocol;

        match proto.state() {
            http1::ServerState::ReceivingRequest => {
                self.buf1.align();

                let mut hbuf = io::Cursor::new(self.buf1.read_buf());

                let mut headers = [httparse::EMPTY_HEADER; http1::HEADERS_MAX];

                let req = match proto.recv_request(&mut hbuf, &mut headers) {
                    Some(Ok(req)) => req,
                    Some(Err(e)) => return Some(Err(e.into())),
                    None => match self.try_recv(args.sock) {
                        Ok(_) => return None,
                        Err(e) if e.kind() == io::ErrorKind::WriteZero => {
                            return Some(Err(ServerError::BufferExceeded));
                        }
                        Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                            want.sock_read = true;
                            return Some(Ok(want));
                        }
                        Err(e) => return Some(Err(e.into())),
                    },
                };

                let hsize = hbuf.position() as usize;

                let host = get_host(req.headers);

                let scheme = if self.secure { "https" } else { "http" };

                debug!(
                    "conn {}: request: {} {}://{}{}",
                    self.id, req.method, scheme, host, req.uri
                );

                let hbuf = self.buf1.read_buf();

                let mut ranges = RequestHeaderRanges {
                    method: slice_to_range(hbuf, req.method),
                    uri: slice_to_range(hbuf, req.uri),
                    headers: [EMPTY_HEADER_RANGES; http1::HEADERS_MAX],
                    headers_count: req.headers.len(),
                };

                for (i, h) in req.headers.iter().enumerate() {
                    ranges.headers[i].name = slice_to_range(hbuf, h.name);
                    ranges.headers[i].value = slice_to_range(hbuf, h.value);
                }

                self.req = Some(ranges);

                // move header data to buf2
                if let Err(e) = self.buf2.write_all(&hbuf[..hsize]) {
                    return Some(Err(e.into()));
                }

                if req.expect_100 {
                    let mut cont = io::Cursor::new(&mut self.cont[..]);

                    if let Err(e) = proto.send_100_continue(&mut cont) {
                        return Some(Err(e.into()));
                    }

                    self.cont_len = cont.position() as usize;
                    self.cont_left = self.cont_len;
                }

                self.buf1.read_commit(hsize);

                if proto.state() == http1::ServerState::AwaitingResponse {
                    if let Err(e) = self.after_request(args) {
                        return Some(Err(e));
                    }
                }
            }
            http1::ServerState::ReceivingBody => {
                if self.cont_left > 0 {
                    let pos = self.cont_len - self.cont_left;

                    let size = match args.sock.write(&self.cont[pos..self.cont_len]) {
                        Ok(size) => size,
                        Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                            want.sock_write = true;
                            return Some(Ok(want));
                        }
                        Err(e) => return Some(Err(e.into())),
                    };

                    self.cont_left -= size;

                    return None;
                }

                self.buf1.align();

                let mut buf = io::Cursor::new(self.buf1.read_buf());

                let mut headers = [httparse::EMPTY_HEADER; http1::HEADERS_MAX];

                let (size, _) =
                    match proto.recv_body(&mut buf, self.body_buf.write_buf(), &mut headers) {
                        Ok((size, headers)) => (size, headers),
                        Err(e) => return Some(Err(e.into())),
                    };

                let read_size = buf.position() as usize;

                if proto.state() == http1::ServerState::ReceivingBody && read_size == 0 {
                    match self.try_recv(args.sock) {
                        Ok(_) => return None,
                        Err(e) if e.kind() == io::ErrorKind::WriteZero => {
                            return Some(Err(ServerError::BufferExceeded));
                        }
                        Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                            want.sock_read = true;
                            return Some(Ok(want));
                        }
                        Err(e) => return Some(Err(e.into())),
                    }
                }

                self.buf1.read_commit(read_size);

                self.body_buf.write_commit(size);

                if proto.state() == http1::ServerState::ReceivingBody
                    && self.body_buf.write_avail() == 0
                {
                    return Some(Err(ServerError::BufferExceeded));
                }

                if proto.state() == http1::ServerState::AwaitingResponse {
                    if let Err(e) = self.after_request(args) {
                        return Some(Err(e));
                    }
                }
            }
            http1::ServerState::AwaitingResponse => {
                if let Some(msg) = self.pending_msg.take() {
                    match args.zsender.send(msg) {
                        Ok(()) => {}
                        Err(zhttpsocket::SendError::Full(msg)) => {
                            self.pending_msg = Some(msg);
                            want.zhttp_write = true;
                            return Some(Ok(want));
                        }
                        Err(zhttpsocket::SendError::Io(e)) => return Some(Err(e.into())),
                    }
                } else {
                    want.zhttp_read = true;
                    return Some(Ok(want));
                }
            }
            http1::ServerState::SendingBody => {
                if self.buf2.read_avail() > 0 {
                    let size = match args.sock.write(self.buf2.read_buf()) {
                        Ok(size) => size,
                        Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                            want.sock_write = true;
                            return Some(Ok(want));
                        }
                        Err(e) => return Some(Err(e.into())),
                    };

                    self.buf2.read_commit(size);

                    return None;
                }

                let size = match proto.send_body(args.sock, &[self.body_buf.read_buf()], true, None)
                {
                    Ok(size) => size,
                    Err(http1::ServerError::Io(e)) if e.kind() == io::ErrorKind::WouldBlock => {
                        want.sock_write = true;
                        return Some(Ok(want));
                    }
                    Err(e) => return Some(Err(e.into())),
                };

                self.body_buf.read_commit(size);
            }
            http1::ServerState::Finished => {
                debug!("conn {}: finished", self.id);

                if proto.is_persistent() {
                    self.reset(args.now);
                } else {
                    self.state = ServerReqState::ShuttingDown;
                }
            }
        }

        None
    }

    fn apply_zhttp_response(&mut self, zresp: &zhttppacket::Response) -> Result<(), ServerError> {
        let proto = &mut self.protocol;

        if proto.state() != http1::ServerState::AwaitingResponse || self.pending_msg.is_some() {
            // not expecting anything
            return Ok(());
        }

        match &zresp.ptype {
            zhttppacket::ResponsePacket::Data(rdata) => {
                let mut hbuf = io::Cursor::new(self.buf2.write_buf());

                let mut headers = [http1::EMPTY_HEADER; http1::HEADERS_MAX];
                let mut headers_len = 0;

                for h in rdata.headers.iter() {
                    headers[headers_len] = http1::Header {
                        name: h.name,
                        value: h.value,
                    };
                    headers_len += 1;
                }

                if let Err(e) = proto.send_response(
                    &mut hbuf,
                    rdata.code,
                    rdata.reason,
                    &headers[..headers_len],
                    http1::BodySize::Known(rdata.body.len()),
                ) {
                    self.state = ServerReqState::Finishing;
                    return Err(e.into());
                }

                let size = hbuf.position() as usize;
                self.buf2.write_commit(size);

                if let Err(e) = self.body_buf.write_all(&rdata.body) {
                    self.state = ServerReqState::Finishing;
                    return Err(ServerError::Io(e));
                }
            }
            _ => debug!(
                "conn {}: unexpected packet in req mode: {}",
                self.id, zresp.ptype_str
            ),
        }

        Ok(())
    }
}

enum ServerProtocol {
    Http(http1::ServerProtocol),
    WebSocket(websocket::Protocol),
}

#[derive(Debug, PartialEq)]
enum ServerStreamState {
    Ready,
    Active,
    Paused,
    ShuttingDown,
    Finishing,
    Finished,
}

struct ServerStreamSharedDataInner {
    to_addr: Option<ArrayVec<[u8; 64]>>,
    out_seq: u32,
}

pub struct AddrRef<'a> {
    s: Ref<'a, ServerStreamSharedDataInner>,
}

impl<'a> AddrRef<'a> {
    pub fn get(&self) -> Option<&[u8]> {
        match &self.s.to_addr {
            Some(addr) => Some(addr.as_ref()),
            None => None,
        }
    }
}

pub struct ServerStreamSharedData {
    inner: RefCell<ServerStreamSharedDataInner>,
}

impl ServerStreamSharedData {
    pub fn new() -> Self {
        Self {
            inner: RefCell::new(ServerStreamSharedDataInner {
                to_addr: None,
                out_seq: 0,
            }),
        }
    }

    fn reset(&self) {
        let s = &mut *self.inner.borrow_mut();

        s.to_addr = None;
        s.out_seq = 0;
    }

    fn set_to_addr(&self, addr: Option<ArrayVec<[u8; 64]>>) {
        let s = &mut *self.inner.borrow_mut();

        s.to_addr = addr;
    }

    pub fn to_addr(&self) -> AddrRef {
        AddrRef {
            s: self.inner.borrow(),
        }
    }

    pub fn out_seq(&self) -> u32 {
        self.inner.borrow().out_seq
    }

    pub fn inc_out_seq(&self) {
        let s = &mut *self.inner.borrow_mut();

        s.out_seq += 1;
    }
}

struct ServerStreamData {
    id: ArrayString<[u8; 32]>,
    peer_addr: Option<SocketAddr>,
    secure: bool,
    client_timeout: Duration,
    state: ServerStreamState,
    client_exp_time: Option<Instant>,
    zhttp_exp_time: Option<Instant>,
    expect_100: bool,
    cont: [u8; 32],
    cont_len: usize,
    cont_left: usize,
    websocket: bool,
    ws_accept: Option<ArrayString<[u8; 28]>>, // base64_encode(sha1_hash) = 28 bytes
    in_seq: u32,
    in_credits: u32,
    out_credits: u32,
    resp_header_left: usize,
    resp_body_done: bool,
    ws_in_tracker: MessageTracker,
    in_overflow_allow: usize,
    sock_readable: bool,
    pending_msg: Option<zmq::Message>,
    handoff_requested: bool,
}

pub struct ServerStreamConnection {
    d: ServerStreamData,
    shared: arena::Rc<ServerStreamSharedData>,
    protocol: ServerProtocol,
    buf1: RingBuffer,
    buf2: RingBuffer,
    in_overflow: Option<Buffer>,
}

impl ServerStreamConnection {
    fn new(
        now: Instant,
        peer_addr: Option<SocketAddr>,
        secure: bool,
        buffer_size: usize,
        messages_max: usize,
        rb_tmp: &Rc<TmpBuffer>,
        timeout: Duration,
        shared: arena::Rc<ServerStreamSharedData>,
    ) -> Self {
        let buf1 = RingBuffer::new(buffer_size, &rb_tmp);
        let buf2 = RingBuffer::new(buffer_size, &rb_tmp);
        let ws_in_tracker = MessageTracker::new(messages_max);

        let mut s = Self {
            d: ServerStreamData {
                id: ArrayString::new(),
                peer_addr,
                secure,
                client_timeout: timeout,
                state: ServerStreamState::Ready,
                client_exp_time: None,
                zhttp_exp_time: None,
                expect_100: false,
                cont: [0; 32],
                cont_len: 0,
                cont_left: 0,
                websocket: false,
                ws_accept: None,
                in_seq: 0,
                in_credits: 0,
                out_credits: 0,
                resp_header_left: 0,
                resp_body_done: false,
                ws_in_tracker,
                in_overflow_allow: 0,
                sock_readable: true,
                pending_msg: None,
                handoff_requested: false,
            },
            shared,
            protocol: ServerProtocol::Http(http1::ServerProtocol::new()),
            buf1,
            buf2,
            in_overflow: None,
        };

        Self::refresh_client_timeout(&mut s.d, now);

        s
    }

    fn reset(&mut self, now: Instant) {
        // note: buf1 is not cleared as there may be data to read

        self.d.state = ServerStreamState::Ready;
        self.d.zhttp_exp_time = None;
        self.d.websocket = false;
        self.d.ws_accept = None;
        self.d.in_seq = 0;
        self.d.in_credits = 0;
        self.d.out_credits = 0;
        self.d.resp_header_left = 0;
        self.d.resp_body_done = false;
        self.d.ws_in_tracker.clear();
        self.d.in_overflow_allow = 0;
        self.d.sock_readable = true;
        self.d.pending_msg = None;
        self.d.handoff_requested = false;

        self.shared.get().reset();

        Self::refresh_client_timeout(&mut self.d, now);

        self.protocol = ServerProtocol::Http(http1::ServerProtocol::new());

        self.buf2.clear();
    }

    fn state(&self) -> ServerState {
        match self.d.state {
            ServerStreamState::Ready => ServerState::Ready,
            ServerStreamState::Finished => ServerState::Finished,
            _ => ServerState::Connected,
        }
    }

    fn start(&mut self, id: &str) {
        self.d.id = ArrayString::from_str(id).unwrap();
        self.d.state = ServerStreamState::Active;
    }

    fn set_sock_readable(&mut self) {
        self.d.sock_readable = true;
    }

    fn process<S, Z>(
        &mut self,
        now: Instant,
        instance: &str,
        sock: &mut S,
        zsender: &mut Z,
        packet_buf: &mut [u8],
        tmp_buf: &mut [u8],
    ) -> Result<Want, ServerError>
    where
        S: Read + Write + Shutdown,
        Z: ZhttpSender,
    {
        loop {
            let args = ServerProcessArgs {
                now,
                instance,
                sock,
                zsender,
                packet_buf,
                tmp_buf,
            };

            if let Some(r) = self.process_step(args) {
                if let Err(e) = &r {
                    match self.d.state {
                        ServerStreamState::Finishing | ServerStreamState::Finished => {}
                        _ => {
                            debug!("conn {}: error: {:?}", self.d.id, e);
                            self.d.state = ServerStreamState::Finishing;
                            continue;
                        }
                    }
                }

                return r;
            }
        }
    }

    fn try_recv(&mut self, sock: &mut dyn io::Read) -> Result<bool, io::Error> {
        if self.buf1.write_avail() == 0 {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        if !self.d.sock_readable {
            return Err(io::Error::from(io::ErrorKind::WouldBlock));
        }

        let size = match self.buf1.write_from(sock) {
            Ok(size) => size,
            Err(e) => {
                if e.kind() == io::ErrorKind::WouldBlock {
                    self.d.sock_readable = false;
                }

                return Err(e);
            }
        };

        let closed = size == 0;

        if closed {
            self.d.state = ServerStreamState::ShuttingDown;
        }

        Ok(closed)
    }

    fn zsend<S, Z>(
        d: &mut ServerStreamData,
        shared: &ServerStreamSharedData,
        args: &mut ServerProcessArgs<'_, S, Z>,
        zreq: zhttppacket::Request,
    ) -> Result<(), io::Error>
    where
        S: Read + Write + Shutdown,
        Z: ZhttpSender,
    {
        if !args.zsender.can_send_to() {
            return Err(io::Error::from(io::ErrorKind::WouldBlock));
        }

        let msg = {
            let mut zreq = zreq;

            let ids = [zhttppacket::Id {
                id: d.id.as_bytes(),
                seq: Some(shared.out_seq()),
            }];

            zreq.from = args.instance.as_bytes();
            zreq.ids = &ids;
            zreq.multi = true;

            let size = zreq.serialize(args.packet_buf)?;

            zmq::Message::from(&args.packet_buf[..size])
        };

        match args.zsender.send_to(shared.to_addr().get().unwrap(), msg) {
            Ok(()) => {}
            Err(zhttpsocket::SendError::Full(_)) => {
                return Err(io::Error::from(io::ErrorKind::WriteZero));
            }
            Err(zhttpsocket::SendError::Io(e)) => return Err(e),
        }

        shared.inc_out_seq();

        Ok(())
    }

    fn refresh_client_timeout(d: &mut ServerStreamData, now: Instant) {
        d.client_exp_time = Some(now + d.client_timeout);
    }

    fn refresh_zhttp_timeout(d: &mut ServerStreamData, now: Instant) {
        d.zhttp_exp_time = Some(now + ZHTTP_SESSION_TIMEOUT);
    }

    fn timeout(d: &ServerStreamData) -> Option<Instant> {
        if d.client_exp_time.is_some() && d.zhttp_exp_time.is_some() {
            Some(cmp::min(
                d.client_exp_time.unwrap(),
                d.zhttp_exp_time.unwrap(),
            ))
        } else if d.client_exp_time.is_some() && d.zhttp_exp_time.is_none() {
            d.client_exp_time
        } else if d.client_exp_time.is_none() && d.zhttp_exp_time.is_some() {
            d.zhttp_exp_time
        } else {
            None
        }
    }

    fn send_resp_header<S, Z>(
        &mut self,
        args: ServerProcessArgs<'_, S, Z>,
        mut want: Want,
    ) -> Option<Result<Want, ServerError>>
    where
        S: Read + Write + Shutdown,
        Z: ZhttpSender,
    {
        let size = match args
            .sock
            .write(&self.buf2.read_buf()[..self.d.resp_header_left])
        {
            Ok(size) => size,
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                want.sock_write = true;
                return Some(Ok(want));
            }
            Err(e) => return Some(Err(e.into())),
        };

        self.buf2.read_commit(size);

        self.d.in_overflow_allow -= size;

        if let Some(overflow) = &mut self.in_overflow {
            let osize = match self.buf2.write(overflow.read_buf()) {
                Ok(size) => size,
                Err(e) => return Some(Err(e.into())),
            };

            overflow.read_commit(osize);
        }

        self.d.resp_header_left -= size;

        if self.d.resp_header_left == 0 {
            if let Some(overflow) = &self.in_overflow {
                assert_eq!(overflow.read_avail(), 0);

                self.in_overflow = None;
            }

            Self::refresh_client_timeout(&mut self.d, args.now);
            want.timeout = Self::timeout(&self.d);
        }

        None
    }

    fn accept_body(&mut self, body: &[u8]) -> Result<(), ServerError> {
        if self.d.resp_header_left > 0 {
            let have_overflow = if let Some(overflow) = &mut self.in_overflow {
                overflow.read_avail() > 0
            } else {
                false
            };

            let accepted = if !have_overflow {
                self.buf2.write(body)?
            } else {
                0
            };

            if accepted < body.len() {
                debug!(
                    "conn {}: overflowing {} bytes",
                    self.d.id,
                    body.len() - accepted
                );

                self.in_overflow = Some(Buffer::new(self.d.in_overflow_allow));
                let overflow = self.in_overflow.as_mut().unwrap();

                overflow.write_all(&body[accepted..])?;
            }
        } else {
            self.buf2.write_all(body)?;
        }

        Ok(())
    }

    fn process_step<S, Z>(
        &mut self,
        mut args: ServerProcessArgs<'_, S, Z>,
    ) -> Option<Result<Want, ServerError>>
    where
        S: Read + Write + Shutdown,
        Z: ZhttpSender,
    {
        // check expiration if not already shutting down
        match self.d.state {
            ServerStreamState::Finishing | ServerStreamState::Finished => {}
            _ => {
                let exp_time = Self::timeout(&self.d);
                if exp_time.is_some() && args.now >= exp_time.unwrap() {
                    debug!("conn {}: timed out", self.d.id);

                    // don't send cancel
                    self.shared.get().set_to_addr(None);

                    self.d.state = ServerStreamState::Finishing;
                }
            }
        }

        match self.d.state {
            ServerStreamState::Active => {
                if self.d.out_credits > 0 {
                    if !args.zsender.can_send_to() {
                        let mut want = Want::nothing();
                        want.zhttp_write_to = true;
                        return Some(Ok(want));
                    }

                    let zreq = zhttppacket::Request::new_credit(b"", &[], self.d.out_credits);

                    if let Err(e) = Self::zsend(&mut self.d, self.shared.get(), &mut args, zreq) {
                        return Some(Err(e.into()));
                    }

                    self.d.out_credits = 0;
                }

                if self.d.handoff_requested && self.buf2.read_avail() == 0 {
                    if !args.zsender.can_send_to() {
                        let mut want = Want::nothing();
                        want.zhttp_write_to = true;
                        return Some(Ok(want));
                    }

                    let zreq = zhttppacket::Request::new_handoff_proceed(b"", &[]);

                    if let Err(e) = Self::zsend(&mut self.d, self.shared.get(), &mut args, zreq) {
                        return Some(Err(e.into()));
                    }

                    self.d.state = ServerStreamState::Paused;
                    self.shared.get().set_to_addr(None);
                    self.d.handoff_requested = false;

                    return None;
                }

                match &self.protocol {
                    ServerProtocol::Http(_) => {
                        return self.process_http(args);
                    }
                    ServerProtocol::WebSocket(_) => {
                        return self.process_websocket(args);
                    }
                }
            }
            ServerStreamState::Paused => {
                let mut want = Want::nothing();
                want.zhttp_read = true;
                want.timeout = Self::timeout(&self.d);
                return Some(Ok(want));
            }
            ServerStreamState::ShuttingDown => {
                match args.sock.shutdown() {
                    Ok(()) => {}
                    Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                        let mut want = Want::nothing();
                        want.sock_read = true;
                        want.sock_write = true;
                        want.timeout = Self::timeout(&self.d);
                        return Some(Ok(want));
                    }
                    Err(e) => return Some(Err(e.into())),
                }

                self.d.state = ServerStreamState::Finishing;

                return None;
            }
            ServerStreamState::Finishing => {
                if self.shared.get().to_addr().get().is_some() {
                    if args.zsender.can_send_to() {
                        let zreq = zhttppacket::Request::new_cancel(b"", &[]);

                        if let Err(e) = Self::zsend(&mut self.d, self.shared.get(), &mut args, zreq)
                        {
                            return Some(Err(e.into()));
                        }
                    }
                }

                self.d.state = ServerStreamState::Finished;

                return None;
            }
            ServerStreamState::Ready | ServerStreamState::Finished => {
                return Some(Ok(Want::nothing()));
            }
        }
    }

    fn process_http<S, Z>(
        &mut self,
        mut args: ServerProcessArgs<'_, S, Z>,
    ) -> Option<Result<Want, ServerError>>
    where
        S: Read + Write + Shutdown,
        Z: ZhttpSender,
    {
        let mut want = Want::nothing();
        want.sock_read = true;
        want.zhttp_read = true;
        want.timeout = Self::timeout(&self.d);

        // always read if possible, to detect disconnects
        match self.try_recv(args.sock) {
            Ok(closed) => {
                if closed {
                    return None;
                }
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {} // ok
            Err(e) if e.kind() == io::ErrorKind::WriteZero => want.sock_read = false,
            Err(e) => return Some(Err(e.into())),
        }

        let proto = match &mut self.protocol {
            ServerProtocol::Http(proto) => proto,
            _ => unreachable!(),
        };

        if let Some(msg) = self.d.pending_msg.take() {
            match args.zsender.send(msg) {
                Ok(()) => {}
                Err(zhttpsocket::SendError::Full(msg)) => {
                    self.d.pending_msg = Some(msg);

                    want.zhttp_write = true;
                    return Some(Ok(want));
                }
                Err(zhttpsocket::SendError::Io(e)) => return Some(Err(e.into())),
            }

            self.shared.get().inc_out_seq();

            Self::refresh_zhttp_timeout(&mut self.d, args.now);

            if self.d.expect_100 {
                let mut cont = io::Cursor::new(&mut self.d.cont[..]);

                if let Err(e) = proto.send_100_continue(&mut cont) {
                    return Some(Err(e.into()));
                }

                self.d.cont_len = cont.position() as usize;
                self.d.cont_left = self.d.cont_len;
            }

            return None;
        }

        match proto.state() {
            http1::ServerState::ReceivingRequest => {
                self.buf1.align();

                let mut hbuf = io::Cursor::new(self.buf1.read_buf());

                let mut headers = [httparse::EMPTY_HEADER; http1::HEADERS_MAX];

                let req = match proto.recv_request(&mut hbuf, &mut headers) {
                    Some(Ok(req)) => req,
                    Some(Err(e)) => return Some(Err(e.into())),
                    None => match self.try_recv(args.sock) {
                        Ok(_) => return None,
                        Err(e) if e.kind() == io::ErrorKind::WriteZero => {
                            return Some(Err(ServerError::BufferExceeded));
                        }
                        Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                            want.sock_read = true;
                            return Some(Ok(want));
                        }
                        Err(e) => return Some(Err(e.into())),
                    },
                };

                let hsize = hbuf.position() as usize;

                Self::refresh_client_timeout(&mut self.d, args.now);
                want.timeout = Self::timeout(&self.d);

                let mut ws_key = None;

                for h in req.headers.iter() {
                    if h.name.eq_ignore_ascii_case("Upgrade") {
                        if str::from_utf8(h.value).unwrap() == "websocket" {
                            self.d.websocket = true;
                        }
                    }

                    if h.name.eq_ignore_ascii_case("Sec-WebSocket-Key") {
                        ws_key = Some(h.value);
                    }
                }

                let host = get_host(req.headers);

                let scheme = if self.d.websocket {
                    if self.d.secure {
                        "wss"
                    } else {
                        "ws"
                    }
                } else {
                    if self.d.secure {
                        "https"
                    } else {
                        "http"
                    }
                };

                debug!(
                    "conn {}: request: {} {}://{}{}",
                    self.d.id, req.method, scheme, host, req.uri
                );

                if self.d.websocket {
                    if req.method != "GET"
                        || req.body_size != http1::BodySize::NoBody
                        || ws_key.is_none()
                    {
                        return Some(Err(ServerError::InvalidWebSocketRequest));
                    }

                    let ws_key = ws_key.unwrap();

                    if self.d.ws_accept.is_none() {
                        let mut input = [0; WS_HASH_INPUT_MAX];

                        let input_len = ws_key.len() + websocket::WS_GUID.len();

                        if input_len > WS_HASH_INPUT_MAX {
                            return Some(Err(ServerError::InvalidWebSocketRequest));
                        }

                        input[..ws_key.len()].copy_from_slice(ws_key);
                        input[ws_key.len()..input_len]
                            .copy_from_slice(&websocket::WS_GUID.as_bytes());

                        let input = &input[..input_len];

                        let digest = sha1::Sha1::from(input).digest();

                        let mut output = [0; 28];

                        let size = base64::encode_config_slice(
                            &digest.bytes(),
                            base64::STANDARD,
                            &mut output,
                        );

                        let output = str::from_utf8(&output[..size]).unwrap();

                        self.d.ws_accept = Some(ArrayString::from_str(output).unwrap());
                    }
                }

                let ids = [zhttppacket::Id {
                    id: self.d.id.as_bytes(),
                    seq: Some(self.shared.get().out_seq()),
                }];

                let (mode, more) = if self.d.websocket {
                    (Mode::WebSocket, false)
                } else {
                    let more = match req.body_size {
                        http1::BodySize::NoBody => false,
                        http1::BodySize::Known(x) => x > 0,
                        http1::BodySize::Unknown => true,
                    };

                    (Mode::HttpStream, more)
                };

                let msg = match make_zhttp_request(
                    args.instance,
                    &ids,
                    req.method,
                    &req.uri,
                    &req.headers,
                    b"",
                    more,
                    mode,
                    self.buf2.capacity() as u32,
                    self.d.peer_addr,
                    self.d.secure,
                    args.packet_buf,
                ) {
                    Ok(msg) => msg,
                    Err(e) => return Some(Err(e.into())),
                };

                self.d.expect_100 = req.expect_100;

                self.buf1.read_commit(hsize);

                self.d.pending_msg = Some(msg);
            }
            http1::ServerState::ReceivingBody => {
                if self.d.cont_left > 0 {
                    let pos = self.d.cont_len - self.d.cont_left;

                    let size = match args.sock.write(&self.d.cont[pos..self.d.cont_len]) {
                        Ok(size) => size,
                        Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                            want.sock_write = true;
                            return Some(Ok(want));
                        }
                        Err(e) => return Some(Err(e.into())),
                    };

                    self.d.cont_left -= size;

                    return None;
                }

                if self.shared.get().to_addr().get().is_none() || self.d.in_credits == 0 {
                    return Some(Ok(want));
                }

                if !args.zsender.can_send_to() {
                    want.zhttp_write_to = true;
                    return Some(Ok(want));
                }

                self.buf1.align();

                let mut buf = io::Cursor::new(self.buf1.read_buf());

                let mut headers = [httparse::EMPTY_HEADER; http1::HEADERS_MAX];

                // pull tmp_buf out of args so we can borrow it and still pass args along
                let tmp_buf = args.tmp_buf;
                args.tmp_buf = &mut [0; 0];

                let max_read = cmp::min(self.d.in_credits as usize, tmp_buf.len());

                let (size, _) =
                    match proto.recv_body(&mut buf, &mut tmp_buf[..max_read], &mut headers) {
                        Ok((size, headers)) => (size, headers),
                        Err(e) => return Some(Err(e.into())),
                    };

                let read_size = buf.position() as usize;

                self.buf1.read_commit(read_size);

                if read_size > 0 {
                    Self::refresh_client_timeout(&mut self.d, args.now);
                    want.timeout = Self::timeout(&self.d);
                }

                if proto.state() == http1::ServerState::ReceivingBody && read_size == 0 {
                    match self.try_recv(args.sock) {
                        Ok(_) => return None,
                        Err(e) if e.kind() == io::ErrorKind::WriteZero => {
                            return Some(Err(ServerError::BufferExceeded));
                        }
                        Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                            want.sock_read = true;
                            return Some(Ok(want));
                        }
                        Err(e) => return Some(Err(e.into())),
                    }
                }

                let body = &tmp_buf[..size];

                self.d.in_credits -= size as u32;

                let mut rdata = zhttppacket::RequestData::new();
                rdata.body = body;

                if proto.state() == http1::ServerState::ReceivingBody {
                    rdata.more = true;
                }

                let zreq = zhttppacket::Request::new_data(b"", &[], rdata);

                if let Err(e) = Self::zsend(&mut self.d, self.shared.get(), &mut args, zreq) {
                    return Some(Err(e.into()));
                }
            }
            http1::ServerState::AwaitingResponse => {
                return Some(Ok(want));
            }
            http1::ServerState::SendingBody => {
                if self.d.resp_header_left > 0 {
                    return self.send_resp_header(args, want);
                }

                if self.buf2.read_avail() == 0 && !self.d.resp_body_done {
                    return Some(Ok(want));
                }

                let mut buf_arr = [&b""[..]; VECTORED_MAX - 2];
                let bufs = self.buf2.get_ref_vectored(&mut buf_arr);

                let size = match proto.send_body(args.sock, bufs, self.d.resp_body_done, None) {
                    Ok(size) => size,
                    Err(http1::ServerError::Io(e)) if e.kind() == io::ErrorKind::WouldBlock => {
                        want.sock_write = true;
                        return Some(Ok(want));
                    }
                    Err(e) => return Some(Err(e.into())),
                };

                self.buf2.read_commit(size);

                if size > 0 {
                    Self::refresh_client_timeout(&mut self.d, args.now);
                    want.timeout = Self::timeout(&self.d);
                }

                if size > 0 && !self.d.resp_body_done {
                    self.d.out_credits += size as u32;
                }
            }
            http1::ServerState::Finished => {
                debug!("conn {}: finished", self.d.id);

                if proto.is_persistent() {
                    self.reset(args.now);
                } else {
                    // don't send cancel
                    self.shared.get().set_to_addr(None);

                    self.d.state = ServerStreamState::ShuttingDown;
                }
            }
        }

        None
    }

    fn process_websocket<S, Z>(
        &mut self,
        mut args: ServerProcessArgs<'_, S, Z>,
    ) -> Option<Result<Want, ServerError>>
    where
        S: Read + Write + Shutdown,
        Z: ZhttpSender,
    {
        let mut want = Want::nothing();
        want.sock_read = true;
        want.zhttp_read = true;
        want.timeout = Self::timeout(&self.d);

        // always read if possible, to detect disconnects
        match self.try_recv(args.sock) {
            Ok(closed) => {
                if closed {
                    return None;
                }
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {} // ok
            Err(e) if e.kind() == io::ErrorKind::WriteZero => want.sock_read = false,
            Err(e) => return Some(Err(e.into())),
        }

        if self.d.resp_header_left > 0 {
            return self.send_resp_header(args, want);
        }

        let proto = match &mut self.protocol {
            ServerProtocol::WebSocket(proto) => proto,
            _ => unreachable!(),
        };

        let tmp_buf = args.tmp_buf;
        args.tmp_buf = &mut [0; 0];

        match proto.state() {
            websocket::State::Connected => {
                let r = self.recv_frames(&mut args, tmp_buf);
                if let Some(Err(e)) = r {
                    return Some(Err(e));
                }

                if self.d.state == ServerStreamState::Finished {
                    return r;
                }

                let w = self.send_frames(&mut args);
                if let Some(Err(e)) = w {
                    return Some(Err(e));
                }

                if r.is_none() || w.is_none() {
                    return None;
                }

                let r = r.unwrap().unwrap();
                let w = w.unwrap().unwrap();

                Some(Ok(r.merge(&w.merge(&want))))
            }
            websocket::State::PeerClosed => {
                let w = self.send_frames(&mut args);
                if let Some(Err(e)) = w {
                    return Some(Err(e));
                }

                if w.is_none() {
                    return None;
                }

                let w = w.unwrap().unwrap();

                Some(Ok(w.merge(&want)))
            }
            websocket::State::Closing => {
                let r = self.recv_frames(&mut args, tmp_buf);
                if let Some(Err(e)) = r {
                    return Some(Err(e));
                }

                if r.is_none() {
                    return None;
                }

                let r = r.unwrap().unwrap();

                Some(Ok(r.merge(&want)))
            }
            websocket::State::Finished => {
                // don't send cancel
                self.shared.get().set_to_addr(None);

                self.d.state = ServerStreamState::ShuttingDown;

                None
            }
        }
    }

    fn recv_frames<S, Z>(
        &mut self,
        args: &mut ServerProcessArgs<'_, S, Z>,
        tmp_buf: &mut [u8],
    ) -> Option<Result<Want, ServerError>>
    where
        S: Read + Write + Shutdown,
        Z: ZhttpSender,
    {
        let proto = match &mut self.protocol {
            ServerProtocol::WebSocket(proto) => proto,
            _ => unreachable!(),
        };

        let mut want = Want::nothing();
        want.zhttp_read = true;
        want.timeout = Self::timeout(&self.d);

        if self.d.in_credits == 0 {
            return Some(Ok(want));
        }

        if !args.zsender.can_send_to() {
            want.zhttp_write_to = true;
            return Some(Ok(want));
        }

        let max_read = cmp::min(self.d.in_credits as usize, tmp_buf.len());

        self.buf1.align();

        match proto.recv_message_content(&mut self.buf1, &mut tmp_buf[..max_read]) {
            Some(Ok((opcode, size, end))) => {
                let body = &tmp_buf[..size];

                let zreq = match opcode {
                    websocket::OPCODE_TEXT | websocket::OPCODE_BINARY => {
                        if body.is_empty() && !end {
                            // process again instead of sending empty message
                            return None;
                        }

                        let mut data = zhttppacket::RequestData::new();

                        data.body = body;

                        data.content_type = if opcode == websocket::OPCODE_TEXT {
                            Some(zhttppacket::ContentType::Text)
                        } else {
                            Some(zhttppacket::ContentType::Binary)
                        };

                        data.more = !end;

                        zhttppacket::Request::new_data(b"", &[], data)
                    }
                    websocket::OPCODE_CLOSE => {
                        let status = if body.len() >= 2 {
                            let mut arr = [0; 2];
                            arr[..].copy_from_slice(&body[..2]);

                            let code = u16::from_be_bytes(arr);

                            let reason = match str::from_utf8(&body[2..]) {
                                Ok(reason) => reason,
                                Err(e) => return Some(Err(e.into())),
                            };

                            Some((code, reason))
                        } else {
                            None
                        };

                        zhttppacket::Request::new_close(b"", &[], status)
                    }
                    websocket::OPCODE_PING => zhttppacket::Request::new_ping(b"", &[], body),
                    websocket::OPCODE_PONG => zhttppacket::Request::new_pong(b"", &[], body),
                    opcode => {
                        debug!(
                            "conn {}: unsupported websocket opcode: {}",
                            self.d.id, opcode
                        );
                        return Some(Err(ServerError::BadFrame));
                    }
                };

                self.d.in_credits -= size as u32;

                if let Err(e) = Self::zsend(&mut self.d, self.shared.get(), args, zreq) {
                    return Some(Err(e.into()));
                }

                Self::refresh_client_timeout(&mut self.d, args.now);
                want.timeout = Self::timeout(&self.d);
            }
            Some(Err(e)) => return Some(Err(e.into())),
            None => match self.try_recv(args.sock) {
                Ok(_) => return None,
                Err(e) if e.kind() == io::ErrorKind::WriteZero => {
                    return Some(Err(ServerError::BufferExceeded));
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                    want.sock_read = true;
                    return Some(Ok(want));
                }
                Err(e) => return Some(Err(e.into())),
            },
        }

        None
    }

    fn send_frames<S, Z>(
        &mut self,
        args: &mut ServerProcessArgs<'_, S, Z>,
    ) -> Option<Result<Want, ServerError>>
    where
        S: Read + Write + Shutdown,
        Z: ZhttpSender,
    {
        let proto = match &mut self.protocol {
            ServerProtocol::WebSocket(proto) => proto,
            _ => unreachable!(),
        };

        let mut want = Want::nothing();
        want.zhttp_read = true;
        want.timeout = Self::timeout(&self.d);

        if let Some((mtype, avail, done)) = self.d.ws_in_tracker.current() {
            if !proto.is_sending_message() {
                proto.send_message_start(mtype, None);
            }

            if avail == 0 && !done {
                return Some(Ok(want));
            }

            let mut buf_arr = [&b""[..]; VECTORED_MAX - 1];
            let bufs = self.buf2.get_ref_vectored(&mut buf_arr).limit(avail);

            let (size, done) = match proto.send_message_content(args.sock, bufs, done) {
                Ok(r) => r,
                Err(e) => return Some(Err(e.into())),
            };

            if size == 0 && !done {
                want.sock_write = true;
                return Some(Ok(want));
            }

            self.buf2.read_commit(size);
            self.d.ws_in_tracker.consumed(size, done);

            Self::refresh_client_timeout(&mut self.d, args.now);
            want.timeout = Self::timeout(&self.d);

            if proto.state() == websocket::State::Connected
                || proto.state() == websocket::State::PeerClosed
            {
                self.d.out_credits += size as u32;
            }

            None
        } else {
            Some(Ok(want))
        }
    }

    fn apply_zhttp_response(
        &mut self,
        now: Instant,
        zresp: &zhttppacket::Response,
        seq: Option<u32>,
    ) -> Result<(), ServerError> {
        if zresp.ids.len() == 0 {
            return Err(ServerError::BadMessage);
        }

        if let Some(seq) = seq {
            if seq != self.d.in_seq {
                debug!(
                    "conn {}: bad seq (expected {}, got {}), skipping",
                    self.d.id, self.d.in_seq, seq
                );
                return Err(ServerError::BadMessage);
            }

            self.d.in_seq += 1;
        }

        match self.d.state {
            ServerStreamState::Ready
            | ServerStreamState::ShuttingDown
            | ServerStreamState::Finishing
            | ServerStreamState::Finished => {
                debug!(
                    "conn {}: unexpected message while in state {:?}",
                    self.d.id, self.d.state
                );
                return Err(ServerError::BadMessage);
            }
            ServerStreamState::Active => {}
            ServerStreamState::Paused => self.d.state = ServerStreamState::Active,
        }

        if self.d.handoff_requested {
            debug!(
                "conn {}: unexpected message after handoff requested",
                self.d.id
            );
            return Err(ServerError::BadMessage);
        }

        let mut addr = ArrayVec::new();
        if addr.try_extend_from_slice(zresp.from).is_err() {
            return Err(ServerError::BadMessage);
        }

        self.shared.get().set_to_addr(Some(addr));

        Self::refresh_zhttp_timeout(&mut self.d, now);

        match &zresp.ptype {
            zhttppacket::ResponsePacket::Data(rdata) => {
                match &mut self.protocol {
                    ServerProtocol::Http(proto) => match proto.state() {
                        http1::ServerState::AwaitingResponse
                        | http1::ServerState::ReceivingBody => {
                            let mut hbuf = io::Cursor::new(self.buf2.write_buf());

                            let mut headers = [http1::EMPTY_HEADER; http1::HEADERS_MAX];
                            let mut headers_len = 0;

                            let mut body_size = http1::BodySize::Unknown;

                            for h in rdata.headers.iter() {
                                if self.d.websocket {
                                    // don't send these headers
                                    if h.name.eq_ignore_ascii_case("Upgrade")
                                        || h.name.eq_ignore_ascii_case("Connection")
                                        || h.name.eq_ignore_ascii_case("Sec-WebSocket-Accept")
                                    {
                                        continue;
                                    }
                                } else {
                                    if h.name.eq_ignore_ascii_case("Content-Length") {
                                        let s = match str::from_utf8(h.value) {
                                            Ok(s) => s,
                                            Err(e) => {
                                                self.d.state = ServerStreamState::Finishing;
                                                return Err(e.into());
                                            }
                                        };

                                        let clen: usize = match s.parse() {
                                            Ok(clen) => clen,
                                            Err(_) => {
                                                self.d.state = ServerStreamState::Finishing;
                                                return Err(io::Error::from(
                                                    io::ErrorKind::InvalidInput,
                                                )
                                                .into());
                                            }
                                        };

                                        body_size = http1::BodySize::Known(clen);
                                    }
                                }

                                headers[headers_len] = http1::Header {
                                    name: h.name,
                                    value: h.value,
                                };
                                headers_len += 1;
                            }

                            if body_size == http1::BodySize::Unknown && !rdata.more {
                                body_size = http1::BodySize::Known(rdata.body.len());
                            }

                            if self.d.websocket {
                                headers[headers_len] = http1::Header {
                                    name: "Upgrade",
                                    value: b"websocket",
                                };
                                headers_len += 1;

                                headers[headers_len] = http1::Header {
                                    name: "Connection",
                                    value: b"Upgrade",
                                };
                                headers_len += 1;

                                headers[headers_len] = http1::Header {
                                    name: "Sec-WebSocket-Accept",
                                    value: self.d.ws_accept.as_ref().unwrap().as_bytes(),
                                };
                                headers_len += 1;
                            }

                            if let Err(e) = proto.send_response(
                                &mut hbuf,
                                rdata.code,
                                rdata.reason,
                                &headers[..headers_len],
                                body_size,
                            ) {
                                self.d.state = ServerStreamState::Finishing;
                                return Err(e.into());
                            }

                            let pos = hbuf.position() as usize;

                            self.buf2.write_commit(pos);
                            self.d.resp_header_left = pos;
                            self.d.in_overflow_allow = pos;

                            if self.d.websocket && rdata.credits == 0 {
                                // workaround for pushpin-proxy, which doesn't
                                //   send credits on websocket accept
                                let credits = self.buf1.capacity() as u32;
                                debug!("no credits in websocket accept, assuming {}", credits);
                                self.d.in_credits = credits;
                            } else {
                                self.d.in_credits = rdata.credits;
                            }

                            if self.d.websocket {
                                self.protocol =
                                    ServerProtocol::WebSocket(websocket::Protocol::new());
                            } else {
                                if let Err(e) = self.accept_body(rdata.body) {
                                    self.d.state = ServerStreamState::Finishing;
                                    return Err(e);
                                }

                                self.d.resp_body_done = !rdata.more;
                            }
                        }
                        http1::ServerState::SendingBody => {
                            if let Err(e) = self.accept_body(rdata.body) {
                                self.d.state = ServerStreamState::Finishing;
                                return Err(e);
                            }

                            self.d.in_credits += rdata.credits;
                            self.d.resp_body_done = !rdata.more;
                        }
                        _ => {}
                    },
                    ServerProtocol::WebSocket(proto) => match proto.state() {
                        websocket::State::Connected | websocket::State::PeerClosed => {
                            if let Err(e) = self.accept_body(rdata.body) {
                                self.d.state = ServerStreamState::Finishing;
                                return Err(e);
                            }

                            self.d.in_credits += rdata.credits;

                            let opcode = match &rdata.content_type {
                                Some(zhttppacket::ContentType::Binary) => websocket::OPCODE_BINARY,
                                _ => websocket::OPCODE_TEXT,
                            };

                            if !self.d.ws_in_tracker.in_progress() {
                                if self.d.ws_in_tracker.start(opcode).is_err() {
                                    self.d.state = ServerStreamState::Finishing;
                                    return Err(ServerError::BufferExceeded);
                                }
                            }

                            self.d.ws_in_tracker.extend(rdata.body.len());

                            if !rdata.more {
                                self.d.ws_in_tracker.done();
                            }
                        }
                        _ => {}
                    },
                }
            }
            zhttppacket::ResponsePacket::Error(edata) => {
                debug!(
                    "conn {}: zhttp error condition={}",
                    self.d.id, edata.condition
                );

                match &mut self.protocol {
                    ServerProtocol::Http(proto) => match proto.state() {
                        http1::ServerState::AwaitingResponse => {
                            if self.d.websocket && edata.condition == "rejected" {
                                let rdata = edata.rejected_info.as_ref().unwrap();

                                let mut hbuf = io::Cursor::new(self.buf2.write_buf());

                                let mut headers = [http1::EMPTY_HEADER; http1::HEADERS_MAX];
                                let mut headers_len = 0;

                                for h in rdata.headers.iter() {
                                    if self.d.websocket {
                                        // don't send these headers
                                        if h.name.eq_ignore_ascii_case("Upgrade")
                                            || h.name.eq_ignore_ascii_case("Connection")
                                            || h.name.eq_ignore_ascii_case("Sec-WebSocket-Accept")
                                        {
                                            continue;
                                        }
                                    }

                                    headers[headers_len] = http1::Header {
                                        name: h.name,
                                        value: h.value,
                                    };
                                    headers_len += 1;
                                }

                                if let Err(e) = proto.send_response(
                                    &mut hbuf,
                                    rdata.code,
                                    rdata.reason,
                                    &headers[..headers_len],
                                    http1::BodySize::Known(rdata.body.len()),
                                ) {
                                    self.d.state = ServerStreamState::Finishing;
                                    return Err(e.into());
                                }

                                let pos = hbuf.position() as usize;

                                self.buf2.write_commit(pos);
                                self.d.resp_header_left = pos;
                                self.d.in_overflow_allow = pos;

                                if let Err(e) = self.accept_body(rdata.body) {
                                    self.d.state = ServerStreamState::Finishing;
                                    return Err(e);
                                }

                                self.d.resp_body_done = true;

                                return Ok(());
                            }
                        }
                        _ => {}
                    },
                    _ => {}
                }

                self.d.state = ServerStreamState::Finished;
            }
            zhttppacket::ResponsePacket::Credit(cdata) => {
                self.d.in_credits += cdata.credits;
            }
            zhttppacket::ResponsePacket::KeepAlive => {}
            zhttppacket::ResponsePacket::Cancel => {
                self.d.state = ServerStreamState::Finished;
            }
            zhttppacket::ResponsePacket::HandoffStart => {
                self.d.handoff_requested = true;
            }
            zhttppacket::ResponsePacket::Close(cdata) => match &mut self.protocol {
                ServerProtocol::WebSocket(proto) => match proto.state() {
                    websocket::State::Connected | websocket::State::PeerClosed => {
                        let (code, reason) = match cdata.status {
                            Some(v) => v,
                            None => (1000, ""),
                        };

                        let arr: [u8; 2] = code.to_be_bytes();

                        if let Err(e) = self.accept_body(&arr) {
                            self.d.state = ServerStreamState::Finishing;
                            return Err(e);
                        }

                        if let Err(e) = self.accept_body(reason.as_bytes()) {
                            self.d.state = ServerStreamState::Finishing;
                            return Err(e);
                        }

                        if self.d.ws_in_tracker.start(websocket::OPCODE_CLOSE).is_err() {
                            self.d.state = ServerStreamState::Finishing;
                            return Err(ServerError::BadFrame);
                        }

                        self.d.ws_in_tracker.extend(arr.len() + reason.len());
                        self.d.ws_in_tracker.done();
                    }
                    _ => {}
                },
                _ => {}
            },
            zhttppacket::ResponsePacket::Ping(pdata) => match &mut self.protocol {
                ServerProtocol::WebSocket(proto) => match proto.state() {
                    websocket::State::Connected | websocket::State::PeerClosed => {
                        if let Err(e) = self.accept_body(pdata.body) {
                            self.d.state = ServerStreamState::Finishing;
                            return Err(e);
                        }

                        self.d.in_credits += pdata.credits;

                        if self.d.ws_in_tracker.start(websocket::OPCODE_PING).is_err() {
                            self.d.state = ServerStreamState::Finishing;
                            return Err(ServerError::BadFrame);
                        }

                        self.d.ws_in_tracker.extend(pdata.body.len());
                        self.d.ws_in_tracker.done();
                    }
                    _ => {}
                },
                _ => {}
            },
            zhttppacket::ResponsePacket::Pong(pdata) => match &mut self.protocol {
                ServerProtocol::WebSocket(proto) => match proto.state() {
                    websocket::State::Connected | websocket::State::PeerClosed => {
                        if let Err(e) = self.accept_body(pdata.body) {
                            self.d.state = ServerStreamState::Finishing;
                            return Err(e);
                        }

                        self.d.in_credits += pdata.credits;

                        if self.d.ws_in_tracker.start(websocket::OPCODE_PONG).is_err() {
                            self.d.state = ServerStreamState::Finishing;
                            return Err(ServerError::BadFrame);
                        }

                        self.d.ws_in_tracker.extend(pdata.body.len());
                        self.d.ws_in_tracker.done();
                    }
                    _ => {}
                },
                _ => {}
            },
            _ => debug!("conn {}: unsupported type: {}", self.d.id, zresp.ptype_str),
        }

        Ok(())
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

struct Connection<'a, S> {
    id: ArrayString<[u8; 32]>,
    stream: &'a mut S,
    conn: ServerConnection,
    want: Want,
    timer: Option<Instant>,
    zreceiver: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, Option<u32>)>,
}

impl<'a, S: Read + Write + Shutdown + Identify> Connection<'a, S> {
    fn new_req(
        now: Instant,
        stream: &'a mut S,
        peer_addr: SocketAddr,
        secure: bool,
        buffer_size: usize,
        body_buffer_size: usize,
        rb_tmp: &Rc<TmpBuffer>,
        timeout: Duration,
        sender: channel::LocalSender<zmq::Message>,
        zreceiver: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, Option<u32>)>,
    ) -> Self {
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
        stream: &'a mut S,
        peer_addr: SocketAddr,
        secure: bool,
        buffer_size: usize,
        messages_max: usize,
        rb_tmp: &Rc<TmpBuffer>,
        timeout: Duration,
        senders: StreamLocalSenders,
        zreceiver: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, Option<u32>)>,
        shared: arena::Rc<ServerStreamSharedData>,
    ) -> Self {
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

        self.stream.set_id(id);

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

        match &mut self.conn {
            ServerConnection::Req(conn, sender) => {
                match conn.process(now, self.stream, sender, packet_buf) {
                    Ok(w) => self.want = w,
                    Err(e) => {
                        debug!("conn {}: process error: {:?}", self.id, e);
                        return true;
                    }
                }

                if conn.state() == ServerState::Finished {
                    return true;
                }
            }
            ServerConnection::Stream(conn, senders) => {
                match conn.process(now, instance_id, self.stream, senders, packet_buf, tmp_buf) {
                    Ok(w) => self.want = w,
                    Err(e) => {
                        debug!("conn {}: process error: {:?}", self.id, e);
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

async fn connection_process<P: CidProvider, S: Read + Write + Shutdown + Identify>(
    token: CancellationToken,
    mut cid: ArrayString<[u8; 32]>,
    cid_provider: &mut P,
    mut c: Connection<'_, S>,
    secure: bool,
    stream_registration: &reactor::Registration,
    zsender1_registration: reactor::Registration,
    zsender2_registration: Option<reactor::Registration>,
    zreceiver_registration: reactor::Registration,
    packet_buf: Rc<RefCell<Vec<u8>>>,
    tmp_buf: Rc<RefCell<Vec<u8>>>,
    instance_id: &str,
    reactor: &Reactor,
) {
    c.start(cid.as_ref());

    let mut timeout = None;

    'main: loop {
        debug!("conn {}: process", c.id);

        if c.process(
            reactor.now(),
            instance_id,
            &mut *packet_buf.borrow_mut(),
            &mut *tmp_buf.borrow_mut(),
        ) {
            break;
        }

        // for TLS, wake on all socket events
        if secure && (c.want.sock_read || c.want.sock_write) {
            c.want.sock_read = true;
            c.want.sock_write = true;
        }

        if c.state() == ServerState::Ready {
            cid = cid_provider.get_new_assigned_cid();
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
                timeout = Some(Timeout::new(want_exp_time));
                c.timer = Some(want_exp_time);
            }
        } else {
            if c.timer.is_some() {
                timeout = None;
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

            let timeout_elapsed = if let Some(timeout) = &timeout {
                Some(timeout.elapsed())
            } else {
                None
            };

            pin_mut!(
                stream_wait,
                zreceiver_wait,
                zsender1_wait,
                zsender2_wait,
                timeout_elapsed,
            );

            match select_6(
                token.cancelled(),
                select_option(stream_wait.as_pin_mut()),
                zreceiver_wait,
                select_option(zsender1_wait.as_pin_mut()),
                select_option(zsender2_wait.as_pin_mut()),
                select_option(timeout_elapsed.as_pin_mut()),
            )
            .await
            {
                // token.cancelled
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
                    if readable || secure {
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
                // timeout_elapsed
                Select6::R6(_) => {
                    debug!("conn {}: timeout", c.id);
                    break;
                }
            }
        }
    }
}

pub async fn server_req_connection<P: CidProvider, S: Read + Write + Shutdown + Identify>(
    stop: CancellationToken,
    cid: ArrayString<[u8; 32]>,
    cid_provider: &mut P,
    stream: &mut S,
    stream_registration: &reactor::Registration,
    peer_addr: SocketAddr,
    secure: bool,
    buffer_size: usize,
    body_buffer_size: usize,
    rb_tmp: &Rc<TmpBuffer>,
    packet_buf: Rc<RefCell<Vec<u8>>>,
    tmp_buf: Rc<RefCell<Vec<u8>>>,
    timeout: Duration,
    instance_id: &str,
    zsender: channel::LocalSender<zmq::Message>,
    zreceiver: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, Option<u32>)>,
    reactor: &Reactor,
) {
    let zreceiver_registration = reactor
        .register_custom_local(zreceiver.get_read_registration(), mio::Interest::READABLE)
        .unwrap();

    let zsender_registration = reactor
        .register_custom_local(zsender.get_write_registration(), mio::Interest::WRITABLE)
        .unwrap();

    let c = Connection::new_req(
        reactor.now(),
        stream,
        peer_addr,
        secure,
        buffer_size,
        body_buffer_size,
        rb_tmp,
        timeout,
        zsender,
        zreceiver,
    );

    connection_process(
        stop,
        cid,
        cid_provider,
        c,
        secure,
        stream_registration,
        zsender_registration,
        None,
        zreceiver_registration,
        packet_buf,
        tmp_buf,
        instance_id,
        reactor,
    )
    .await;
}

pub async fn server_stream_connection<P: CidProvider, S: Read + Write + Shutdown + Identify>(
    stop: CancellationToken,
    cid: ArrayString<[u8; 32]>,
    cid_provider: &mut P,
    stream: &mut S,
    stream_registration: &reactor::Registration,
    peer_addr: SocketAddr,
    secure: bool,
    buffer_size: usize,
    messages_max: usize,
    rb_tmp: &Rc<TmpBuffer>,
    packet_buf: Rc<RefCell<Vec<u8>>>,
    tmp_buf: Rc<RefCell<Vec<u8>>>,
    timeout: Duration,
    instance_id: &str,
    zsender: channel::LocalSender<zmq::Message>,
    zsender_stream: channel::LocalSender<(ArrayVec<[u8; 64]>, zmq::Message)>,
    zreceiver: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, Option<u32>)>,
    shared: arena::Rc<ServerStreamSharedData>,
    reactor: &Reactor,
) {
    let zreceiver_registration = reactor
        .register_custom_local(zreceiver.get_read_registration(), mio::Interest::READABLE)
        .unwrap();

    let zsender_registration = reactor
        .register_custom_local(zsender.get_write_registration(), mio::Interest::WRITABLE)
        .unwrap();

    let zsender_stream_registration = reactor
        .register_custom_local(
            zsender_stream.get_write_registration(),
            mio::Interest::WRITABLE,
        )
        .unwrap();

    let c = Connection::new_stream(
        reactor.now(),
        stream,
        peer_addr,
        secure,
        buffer_size,
        messages_max,
        rb_tmp,
        timeout,
        StreamLocalSenders::new(zsender, zsender_stream),
        zreceiver,
        shared,
    );

    connection_process(
        stop,
        cid,
        cid_provider,
        c,
        secure,
        stream_registration,
        zsender_registration,
        Some(zsender_stream_registration),
        zreceiver_registration,
        packet_buf,
        tmp_buf,
        instance_id,
        reactor,
    )
    .await;
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::buffer::TmpBuffer;
    use std::mem;
    use std::rc::Rc;

    struct FakeSock {
        inbuf: Vec<u8>,
        outbuf: Vec<u8>,
        out_allow: usize,
    }

    impl FakeSock {
        fn new() -> Self {
            Self {
                inbuf: Vec::new(),
                outbuf: Vec::new(),
                out_allow: 0,
            }
        }

        fn add_readable(&mut self, buf: &[u8]) {
            self.inbuf.extend_from_slice(buf);
        }

        fn take_writable(&mut self) -> Vec<u8> {
            self.outbuf.split_off(0)
        }

        fn allow_write(&mut self, size: usize) {
            self.out_allow += size;
        }
    }

    impl Read for FakeSock {
        fn read(&mut self, buf: &mut [u8]) -> Result<usize, io::Error> {
            if self.inbuf.is_empty() {
                return Err(io::Error::from(io::ErrorKind::WouldBlock));
            }

            let size = cmp::min(buf.len(), self.inbuf.len());

            buf[..size].copy_from_slice(&self.inbuf[..size]);

            let mut rest = self.inbuf.split_off(size);
            mem::swap(&mut self.inbuf, &mut rest);

            Ok(size)
        }
    }

    impl Write for FakeSock {
        fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
            if buf.len() > 0 && self.out_allow == 0 {
                return Err(io::Error::from(io::ErrorKind::WouldBlock));
            }

            let size = cmp::min(buf.len(), self.out_allow);
            let buf = &buf[..size];

            self.outbuf.extend_from_slice(buf);
            self.out_allow -= size;

            Ok(buf.len())
        }

        fn write_vectored(&mut self, bufs: &[io::IoSlice]) -> Result<usize, io::Error> {
            let mut total = 0;

            for buf in bufs {
                if self.out_allow == 0 {
                    break;
                }

                let size = cmp::min(buf.len(), self.out_allow);
                let buf = &buf[..size];

                self.outbuf.extend_from_slice(buf.as_ref());
                self.out_allow -= size;

                total += buf.len();
            }

            Ok(total)
        }

        fn flush(&mut self) -> Result<(), io::Error> {
            Ok(())
        }
    }

    impl Shutdown for FakeSock {
        fn shutdown(&mut self) -> Result<(), io::Error> {
            Ok(())
        }
    }

    struct FakeSender {
        msgs: Vec<(Option<String>, zmq::Message)>,
        allow: usize,
    }

    impl FakeSender {
        fn new() -> Self {
            Self {
                msgs: Vec::new(),
                allow: 0,
            }
        }

        fn take(&mut self) -> (Option<String>, zmq::Message) {
            self.msgs.remove(0)
        }

        fn allow(&mut self, size: usize) {
            self.allow += size;
        }
    }

    impl ZhttpSender for FakeSender {
        fn can_send_to(&self) -> bool {
            self.allow > 0
        }

        fn send(&mut self, message: zmq::Message) -> Result<(), zhttpsocket::SendError> {
            if self.allow == 0 {
                return Err(zhttpsocket::SendError::Full(message));
            }

            self.msgs.push((None, message));
            self.allow -= 1;

            Ok(())
        }

        fn send_to(
            &mut self,
            addr: &[u8],
            message: zmq::Message,
        ) -> Result<(), zhttpsocket::SendError> {
            if self.allow == 0 {
                return Err(zhttpsocket::SendError::Full(message));
            }

            self.msgs
                .push((Some(String::from_utf8(addr.to_vec()).unwrap()), message));
            self.allow -= 1;

            Ok(())
        }
    }

    #[test]
    fn message_tracker() {
        let mut t = MessageTracker::new(2);

        assert_eq!(t.in_progress(), false);
        assert_eq!(t.current(), None);

        t.start(websocket::OPCODE_TEXT).unwrap();
        assert_eq!(t.in_progress(), true);
        assert_eq!(t.current(), Some((websocket::OPCODE_TEXT, 0, false)));

        t.extend(5);
        assert_eq!(t.in_progress(), true);
        assert_eq!(t.current(), Some((websocket::OPCODE_TEXT, 5, false)));

        t.consumed(2, false);
        assert_eq!(t.in_progress(), true);
        assert_eq!(t.current(), Some((websocket::OPCODE_TEXT, 3, false)));

        t.done();
        assert_eq!(t.in_progress(), false);
        assert_eq!(t.current(), Some((websocket::OPCODE_TEXT, 3, true)));

        t.consumed(3, true);
        assert_eq!(t.current(), None);

        for _ in 0..t.items.capacity() {
            t.start(websocket::OPCODE_TEXT).unwrap();
            t.done();
        }
        let r = t.start(websocket::OPCODE_TEXT);
        assert!(r.is_err());
    }

    #[test]
    fn server_req_without_body() {
        let mut sock = FakeSock::new();
        let mut sender = FakeSender::new();

        let buffer_size = 1024;
        let rb_tmp = Rc::new(TmpBuffer::new(1024));
        let mut packet_buf = vec![0; 2048];

        let timeout = Duration::from_millis(5_000);

        let mut c = ServerReqConnection::new(
            Instant::now(),
            None,
            false,
            buffer_size,
            buffer_size,
            &rb_tmp,
            timeout,
        );
        c.start("1");

        assert_eq!(c.state(), ServerState::Connected);

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_read, true);

        let req_data = concat!(
            "GET /path HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Connection: close\r\n",
            "\r\n"
        )
        .as_bytes();

        sock.add_readable(req_data);
        c.set_sock_readable();

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_write, true);
        assert_eq!(sender.msgs.len(), 0);

        sender.allow(1);

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_read, true);
        assert_eq!(sender.msgs.len(), 1);

        let (_, buf) = sender.take();
        let buf = &buf[..];

        let expected = concat!(
            "T148:2:id,1:1,3:ext,15:5:multi,4:true!}6:method,3:GET,3:ur",
            "i,23:http://example.com/path,7:headers,52:22:4:Host,11:exa",
            "mple.com,]22:10:Connection,5:close,]]}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let ids = [zhttppacket::Id {
            id: b"1",
            seq: None,
        }];

        let rdata = zhttppacket::ResponseData {
            credits: 0,
            more: false,
            code: 200,
            reason: "OK",
            headers: &[zhttppacket::Header {
                name: "Content-Type",
                value: b"text/plain",
            }],
            content_type: None,
            body: b"hello\n",
        };

        let zresp = zhttppacket::Response {
            from: b"",
            ids: &ids,
            multi: false,
            ptype: zhttppacket::ResponsePacket::Data(rdata),
            ptype_str: "",
        };

        c.apply_zhttp_response(&zresp).unwrap();

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_write, true);

        sock.allow_write(1024);

        c.process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Finished);

        let data = sock.take_writable();

        let expected = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Connection: close\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n",
        );

        assert_eq!(str::from_utf8(&data).unwrap(), expected);
    }

    #[test]
    fn server_req_with_body() {
        let mut sock = FakeSock::new();
        let mut sender = FakeSender::new();

        let buffer_size = 1024;
        let rb_tmp = Rc::new(TmpBuffer::new(1024));
        let mut packet_buf = vec![0; 2048];

        let timeout = Duration::from_millis(5_000);

        let mut c = ServerReqConnection::new(
            Instant::now(),
            None,
            false,
            buffer_size,
            buffer_size,
            &rb_tmp,
            timeout,
        );
        c.start("1");

        assert_eq!(c.state(), ServerState::Connected);

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_read, true);

        let req_data = concat!(
            "POST /path HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Content-Length: 6\r\n",
            "Connection: close\r\n",
            "\r\n",
            "hello\n"
        )
        .as_bytes();

        sock.add_readable(req_data);
        c.set_sock_readable();

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_write, true);
        assert_eq!(sender.msgs.len(), 0);

        sender.allow(1);

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_read, true);
        assert_eq!(sender.msgs.len(), 1);

        let (_, buf) = sender.take();
        let buf = &buf[..];

        let expected = concat!(
            "T191:2:id,1:1,3:ext,15:5:multi,4:true!}6:method,4:POST,3:u",
            "ri,23:http://example.com/path,7:headers,78:22:4:Host,11:ex",
            "ample.com,]22:14:Content-Length,1:6,]22:10:Connection,5:cl",
            "ose,]]4:body,6:hello\n,}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let ids = [zhttppacket::Id {
            id: b"1",
            seq: None,
        }];

        let rdata = zhttppacket::ResponseData {
            credits: 0,
            more: false,
            code: 200,
            reason: "OK",
            headers: &[zhttppacket::Header {
                name: "Content-Type",
                value: b"text/plain",
            }],
            content_type: None,
            body: b"hello\n",
        };

        let zresp = zhttppacket::Response {
            from: b"",
            ids: &ids,
            multi: false,
            ptype: zhttppacket::ResponsePacket::Data(rdata),
            ptype_str: "",
        };

        c.apply_zhttp_response(&zresp).unwrap();

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_write, true);

        sock.allow_write(1024);

        c.process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Finished);

        let data = sock.take_writable();

        let expected = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Connection: close\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n",
        );

        assert_eq!(str::from_utf8(&data).unwrap(), expected);
    }

    #[test]
    fn server_req_timeout() {
        let mut sock = FakeSock::new();
        let mut sender = FakeSender::new();

        let now = Instant::now();

        let buffer_size = 1024;
        let rb_tmp = Rc::new(TmpBuffer::new(1024));
        let mut packet_buf = vec![0; 2048];

        let timeout = Duration::from_millis(5_000);

        let mut c =
            ServerReqConnection::new(now, None, false, buffer_size, buffer_size, &rb_tmp, timeout);
        c.start("1");

        assert_eq!(c.state(), ServerState::Connected);

        let want = c
            .process(now, &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_read, true);
        assert_eq!(want.timeout, Some(now + timeout));

        c.process(now + timeout, &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Finished);
    }

    #[test]
    fn server_req_pipeline() {
        let mut sock = FakeSock::new();
        let mut sender = FakeSender::new();

        let buffer_size = 1024;
        let rb_tmp = Rc::new(TmpBuffer::new(1024));
        let mut packet_buf = vec![0; 2048];

        let timeout = Duration::from_millis(5_000);

        let mut c = ServerReqConnection::new(
            Instant::now(),
            None,
            false,
            buffer_size,
            buffer_size,
            &rb_tmp,
            timeout,
        );
        c.start("1");

        assert_eq!(c.state(), ServerState::Connected);

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_read, true);

        let req_data = concat!(
            "GET /path1 HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "\r\n",
            "GET /path2 HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "\r\n",
        )
        .as_bytes();

        sock.add_readable(req_data);
        c.set_sock_readable();

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_write, true);
        assert_eq!(sender.msgs.len(), 0);

        sender.allow(1);

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_read, true);
        assert_eq!(sender.msgs.len(), 1);

        let (_, buf) = sender.take();
        let buf = &buf[..];

        let expected = concat!(
            "T123:2:id,1:1,3:ext,15:5:multi,4:true!}6:method,3:GET,3:ur",
            "i,24:http://example.com/path1,7:headers,26:22:4:Host,11:ex",
            "ample.com,]]}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let ids = [zhttppacket::Id {
            id: b"1",
            seq: None,
        }];

        let rdata = zhttppacket::ResponseData {
            credits: 0,
            more: false,
            code: 200,
            reason: "OK",
            headers: &[zhttppacket::Header {
                name: "Content-Type",
                value: b"text/plain",
            }],
            content_type: None,
            body: b"hello\n",
        };

        let zresp = zhttppacket::Response {
            from: b"",
            ids: &ids,
            multi: false,
            ptype: zhttppacket::ResponsePacket::Data(rdata),
            ptype_str: "",
        };

        c.apply_zhttp_response(&zresp).unwrap();

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_write, true);

        sock.allow_write(1024);

        c.process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Ready);

        c.start("1");

        assert_eq!(c.state(), ServerState::Connected);

        let data = sock.take_writable();

        let expected = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n",
        );

        assert_eq!(str::from_utf8(&data).unwrap(), expected);

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_write, true);
        assert_eq!(sender.msgs.len(), 0);

        sender.allow(1);

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_read, true);
        assert_eq!(sender.msgs.len(), 1);

        let (_, buf) = sender.take();
        let buf = &buf[..];

        let expected = concat!(
            "T123:2:id,1:1,3:ext,15:5:multi,4:true!}6:method,3:GET,3:ur",
            "i,24:http://example.com/path2,7:headers,26:22:4:Host,11:ex",
            "ample.com,]]}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let ids = [zhttppacket::Id {
            id: b"1",
            seq: None,
        }];

        let rdata = zhttppacket::ResponseData {
            credits: 0,
            more: false,
            code: 200,
            reason: "OK",
            headers: &[zhttppacket::Header {
                name: "Content-Type",
                value: b"text/plain",
            }],
            content_type: None,
            body: b"hello\n",
        };

        let zresp = zhttppacket::Response {
            from: b"",
            ids: &ids,
            multi: false,
            ptype: zhttppacket::ResponsePacket::Data(rdata),
            ptype_str: "",
        };

        c.apply_zhttp_response(&zresp).unwrap();

        c.process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Ready);

        let data = sock.take_writable();

        let expected = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n",
        );

        assert_eq!(str::from_utf8(&data).unwrap(), expected);
    }

    #[test]
    fn server_req_secure() {
        let mut sock = FakeSock::new();
        let mut sender = FakeSender::new();

        let buffer_size = 1024;
        let rb_tmp = Rc::new(TmpBuffer::new(1024));
        let mut packet_buf = vec![0; 2048];

        let timeout = Duration::from_millis(5_000);

        let mut c = ServerReqConnection::new(
            Instant::now(),
            None,
            true,
            buffer_size,
            buffer_size,
            &rb_tmp,
            timeout,
        );
        c.start("1");

        assert_eq!(c.state(), ServerState::Connected);

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_read, true);

        let req_data = concat!(
            "GET /path HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Connection: close\r\n",
            "\r\n"
        )
        .as_bytes();

        sock.add_readable(req_data);
        c.set_sock_readable();

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_write, true);
        assert_eq!(sender.msgs.len(), 0);

        sender.allow(1);

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_read, true);
        assert_eq!(sender.msgs.len(), 1);

        let (_, buf) = sender.take();
        let buf = &buf[..];

        let expected = concat!(
            "T149:2:id,1:1,3:ext,15:5:multi,4:true!}6:method,3:GET,3:ur",
            "i,24:https://example.com/path,7:headers,52:22:4:Host,11:ex",
            "ample.com,]22:10:Connection,5:close,]]}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let ids = [zhttppacket::Id {
            id: b"1",
            seq: None,
        }];

        let rdata = zhttppacket::ResponseData {
            credits: 0,
            more: false,
            code: 200,
            reason: "OK",
            headers: &[zhttppacket::Header {
                name: "Content-Type",
                value: b"text/plain",
            }],
            content_type: None,
            body: b"hello\n",
        };

        let zresp = zhttppacket::Response {
            from: b"",
            ids: &ids,
            multi: false,
            ptype: zhttppacket::ResponsePacket::Data(rdata),
            ptype_str: "",
        };

        c.apply_zhttp_response(&zresp).unwrap();

        let want = c
            .process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_write, true);

        sock.allow_write(1024);

        c.process(Instant::now(), &mut sock, &mut sender, &mut packet_buf)
            .unwrap();

        assert_eq!(c.state(), ServerState::Finished);

        let data = sock.take_writable();

        let expected = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Connection: close\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n",
        );

        assert_eq!(str::from_utf8(&data).unwrap(), expected);
    }

    #[test]
    fn server_stream_without_body() {
        let mut sock = FakeSock::new();
        let mut sender = FakeSender::new();

        let buffer_size = 1024;
        let messages_max = 10;

        let rb_tmp = Rc::new(TmpBuffer::new(buffer_size));
        let mut packet_buf = vec![0; buffer_size * 2];
        let mut tmp_buf = vec![0; buffer_size];

        let timeout = Duration::from_millis(5_000);

        let shared_mem = Rc::new(arena::RcMemory::new(1));
        let shared = arena::Rc::new(ServerStreamSharedData::new(), &shared_mem).unwrap();

        let mut c = ServerStreamConnection::new(
            Instant::now(),
            None,
            false,
            buffer_size,
            messages_max,
            &rb_tmp,
            timeout,
            shared,
        );
        c.start("1");

        assert_eq!(c.state(), ServerState::Connected);

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_read, true);

        let req_data =
            concat!("GET /path HTTP/1.1\r\n", "Host: example.com\r\n", "\r\n").as_bytes();

        sock.add_readable(req_data);
        c.set_sock_readable();

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_write, true);
        assert_eq!(sender.msgs.len(), 0);

        sender.allow(1);

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_read, true);
        assert_eq!(sender.msgs.len(), 1);

        let (addr, buf) = sender.take();
        let buf = &buf[..];

        assert_eq!(addr, None);

        let expected = concat!(
            "T179:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:multi,4:t",
            "rue!}6:method,3:GET,3:uri,23:http://example.com/path,7:hea",
            "ders,26:22:4:Host,11:example.com,]]7:credits,4:1024#6:stre",
            "am,4:true!}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let ids = [zhttppacket::Id {
            id: b"1",
            seq: Some(0),
        }];

        let rdata = zhttppacket::ResponseData {
            credits: 0,
            more: false,
            code: 200,
            reason: "OK",
            headers: &[zhttppacket::Header {
                name: "Content-Type",
                value: b"text/plain",
            }],
            content_type: None,
            body: b"hello\n",
        };

        let zresp = zhttppacket::Response {
            from: b"handler",
            ids: &ids,
            multi: false,
            ptype: zhttppacket::ResponsePacket::Data(rdata),
            ptype_str: "",
        };

        c.apply_zhttp_response(Instant::now(), &zresp, ids[0].seq)
            .unwrap();

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_write, true);

        let data = sock.take_writable();
        assert!(data.is_empty());

        sock.allow_write(1024);

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Ready);
        assert_eq!(want.sock_write, false);

        let data = sock.take_writable();

        let expected = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n",
        );

        assert_eq!(str::from_utf8(&data).unwrap(), expected);
    }

    #[test]
    fn server_stream_with_body() {
        let mut sock = FakeSock::new();
        let mut sender = FakeSender::new();

        let buffer_size = 1024;
        let messages_max = 10;

        let rb_tmp = Rc::new(TmpBuffer::new(buffer_size));
        let mut packet_buf = vec![0; buffer_size * 2];
        let mut tmp_buf = vec![0; buffer_size];

        let timeout = Duration::from_millis(5_000);

        let shared_mem = Rc::new(arena::RcMemory::new(1));
        let shared = arena::Rc::new(ServerStreamSharedData::new(), &shared_mem).unwrap();

        let mut c = ServerStreamConnection::new(
            Instant::now(),
            None,
            false,
            buffer_size,
            messages_max,
            &rb_tmp,
            timeout,
            shared,
        );
        c.start("1");

        assert_eq!(c.state(), ServerState::Connected);

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_read, true);

        let req_data = concat!(
            "POST /path HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n"
        )
        .as_bytes();

        sock.add_readable(req_data);
        c.set_sock_readable();

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_write, true);
        assert_eq!(sender.msgs.len(), 0);

        sender.allow(2);

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_read, true);
        assert_eq!(sender.msgs.len(), 1);

        let (addr, buf) = sender.take();
        let buf = &buf[..];

        assert_eq!(addr, None);

        let expected = concat!(
            "T220:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:multi,4:t",
            "rue!}6:method,4:POST,3:uri,23:http://example.com/path,7:he",
            "aders,52:22:4:Host,11:example.com,]22:14:Content-Length,1:",
            "6,]]7:credits,4:1024#4:more,4:true!6:stream,4:true!}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let ids = [zhttppacket::Id {
            id: b"1",
            seq: Some(0),
        }];

        let zresp = zhttppacket::Response {
            from: b"handler",
            ids: &ids,
            multi: false,
            ptype: zhttppacket::ResponsePacket::Credit(zhttppacket::CreditData {
                credits: buffer_size as u32,
            }),
            ptype_str: "credit",
        };

        c.apply_zhttp_response(Instant::now(), &zresp, ids[0].seq)
            .unwrap();

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_read, true);
        assert_eq!(sender.msgs.len(), 1);

        let (addr, buf) = sender.take();
        let buf = &buf[..];

        assert_eq!(addr, Some(String::from("handler")));

        let expected = concat!(
            "T74:4:from,4:test,2:id,1:1,3:seq,1:1#3:ext,15:5:multi,4:tr",
            "ue!}4:body,6:hello\n,}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let ids = [zhttppacket::Id {
            id: b"1",
            seq: Some(1),
        }];

        let rdata = zhttppacket::ResponseData {
            credits: 0,
            more: false,
            code: 200,
            reason: "OK",
            headers: &[zhttppacket::Header {
                name: "Content-Type",
                value: b"text/plain",
            }],
            content_type: None,
            body: b"hello\n",
        };

        let zresp = zhttppacket::Response {
            from: b"handler",
            ids: &ids,
            multi: false,
            ptype: zhttppacket::ResponsePacket::Data(rdata),
            ptype_str: "",
        };

        c.apply_zhttp_response(Instant::now(), &zresp, ids[0].seq)
            .unwrap();

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_write, true);

        let data = sock.take_writable();
        assert!(data.is_empty());

        sock.allow_write(1024);

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Ready);
        assert_eq!(want.sock_write, false);

        let data = sock.take_writable();

        let expected = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n",
        );

        assert_eq!(str::from_utf8(&data).unwrap(), expected);
    }

    #[test]
    fn server_stream_chunked() {
        let mut sock = FakeSock::new();
        let mut sender = FakeSender::new();

        let buffer_size = 1024;
        let messages_max = 10;

        let rb_tmp = Rc::new(TmpBuffer::new(buffer_size));
        let mut packet_buf = vec![0; buffer_size * 2];
        let mut tmp_buf = vec![0; buffer_size];

        let timeout = Duration::from_millis(5_000);

        let shared_mem = Rc::new(arena::RcMemory::new(1));
        let shared = arena::Rc::new(ServerStreamSharedData::new(), &shared_mem).unwrap();

        let mut c = ServerStreamConnection::new(
            Instant::now(),
            None,
            false,
            buffer_size,
            messages_max,
            &rb_tmp,
            timeout,
            shared,
        );
        c.start("1");

        assert_eq!(c.state(), ServerState::Connected);

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_read, true);

        let req_data =
            concat!("GET /path HTTP/1.1\r\n", "Host: example.com\r\n", "\r\n").as_bytes();

        sock.add_readable(req_data);
        c.set_sock_readable();

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_write, true);
        assert_eq!(sender.msgs.len(), 0);

        sender.allow(1);

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_read, true);
        assert_eq!(sender.msgs.len(), 1);

        let (addr, buf) = sender.take();
        let buf = &buf[..];

        assert_eq!(addr, None);

        let expected = concat!(
            "T179:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:multi,4:t",
            "rue!}6:method,3:GET,3:uri,23:http://example.com/path,7:hea",
            "ders,26:22:4:Host,11:example.com,]]7:credits,4:1024#6:stre",
            "am,4:true!}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let ids = [zhttppacket::Id {
            id: b"1",
            seq: Some(0),
        }];

        let rdata = zhttppacket::ResponseData {
            credits: 0,
            more: true,
            code: 200,
            reason: "OK",
            headers: &[zhttppacket::Header {
                name: "Content-Type",
                value: b"text/plain",
            }],
            content_type: None,
            body: b"",
        };

        let zresp = zhttppacket::Response {
            from: b"handler",
            ids: &ids,
            multi: false,
            ptype: zhttppacket::ResponsePacket::Data(rdata),
            ptype_str: "",
        };

        c.apply_zhttp_response(Instant::now(), &zresp, ids[0].seq)
            .unwrap();

        let ids = [zhttppacket::Id {
            id: b"1",
            seq: Some(1),
        }];

        let rdata = zhttppacket::ResponseData {
            credits: 0,
            more: false,
            code: 200,
            reason: "OK",
            headers: &[],
            content_type: None,
            body: b"hello\n",
        };

        let zresp = zhttppacket::Response {
            from: b"handler",
            ids: &ids,
            multi: false,
            ptype: zhttppacket::ResponsePacket::Data(rdata),
            ptype_str: "",
        };

        c.apply_zhttp_response(Instant::now(), &zresp, ids[0].seq)
            .unwrap();

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_write, true);

        let data = sock.take_writable();
        assert!(data.is_empty());

        sock.allow_write(1024);

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Ready);
        assert_eq!(want.sock_write, false);

        let data = sock.take_writable();

        let expected = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Connection: Transfer-Encoding\r\n",
            "Transfer-Encoding: chunked\r\n",
            "\r\n",
            "6\r\n",
            "hello\n",
            "\r\n",
            "0\r\n",
            "\r\n",
        );

        assert_eq!(str::from_utf8(&data).unwrap(), expected);
    }

    #[test]
    fn server_stream_early_response() {
        let mut sock = FakeSock::new();
        let mut sender = FakeSender::new();

        let buffer_size = 1024;
        let messages_max = 10;

        let rb_tmp = Rc::new(TmpBuffer::new(buffer_size));
        let mut packet_buf = vec![0; buffer_size * 2];
        let mut tmp_buf = vec![0; buffer_size];

        let timeout = Duration::from_millis(5_000);

        let shared_mem = Rc::new(arena::RcMemory::new(1));
        let shared = arena::Rc::new(ServerStreamSharedData::new(), &shared_mem).unwrap();

        let mut c = ServerStreamConnection::new(
            Instant::now(),
            None,
            false,
            buffer_size,
            messages_max,
            &rb_tmp,
            timeout,
            shared,
        );
        c.start("1");

        assert_eq!(c.state(), ServerState::Connected);

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_read, true);

        let req_data = concat!(
            "POST /path HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n"
        )
        .as_bytes();

        sock.add_readable(req_data);
        c.set_sock_readable();

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_write, true);
        assert_eq!(sender.msgs.len(), 0);

        sender.allow(2);

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.zhttp_read, true);
        assert_eq!(sender.msgs.len(), 1);

        let (addr, buf) = sender.take();
        let buf = &buf[..];

        assert_eq!(addr, None);

        let expected = concat!(
            "T220:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:multi,4:t",
            "rue!}6:method,4:POST,3:uri,23:http://example.com/path,7:he",
            "aders,52:22:4:Host,11:example.com,]22:14:Content-Length,1:",
            "6,]]7:credits,4:1024#4:more,4:true!6:stream,4:true!}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let ids = [zhttppacket::Id {
            id: b"1",
            seq: Some(0),
        }];

        let rdata = zhttppacket::ResponseData {
            credits: 0,
            more: false,
            code: 400,
            reason: "Bad Request",
            headers: &[
                zhttppacket::Header {
                    name: "Content-Type",
                    value: b"text/plain",
                },
                zhttppacket::Header {
                    name: "Content-Length",
                    value: b"18",
                },
            ],
            content_type: None,
            body: b"stopping this now\n",
        };

        let zresp = zhttppacket::Response {
            from: b"handler",
            ids: &ids,
            multi: false,
            ptype: zhttppacket::ResponsePacket::Data(rdata),
            ptype_str: "",
        };

        c.apply_zhttp_response(Instant::now(), &zresp, ids[0].seq)
            .unwrap();

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Connected);
        assert_eq!(want.sock_write, true);

        let data = sock.take_writable();
        assert!(data.is_empty());

        sock.allow_write(1024);

        let want = c
            .process(
                Instant::now(),
                "test",
                &mut sock,
                &mut sender,
                &mut packet_buf,
                &mut tmp_buf,
            )
            .unwrap();

        assert_eq!(c.state(), ServerState::Finished);
        assert_eq!(want.sock_write, false);

        let data = sock.take_writable();

        let expected = concat!(
            "HTTP/1.1 400 Bad Request\r\n",
            "Content-Type: text/plain\r\n",
            "Connection: close\r\n",
            "Content-Length: 18\r\n",
            "\r\n",
            "stopping this now\n",
        );

        assert_eq!(str::from_utf8(&data).unwrap(), expected);
    }
}
