/*
 * Copyright (C) 2020-2023 Fanout, Inc.
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

// Note: Always Be Receiving (ABR)
//
// Connection handlers are expected to read ZHTTP messages as fast as
// possible. If they don't, the whole thread could stall. This is by design,
// to limit the number of to-be-processed messages in memory. They either
// need to do something immediately with the messages, or discard them.
//
// Every await point must ensure messages keep getting read/processed, by
// doing one of:
//
// - Directly awaiting a message.
// - Awaiting a select that is awaiting a message.
// - Wrapping other activity with discard_while().
// - Calling handle_other(), which itself will read messages.
// - Awaiting something known to not block.

use crate::arena;
use crate::buffer::{
    BaseRingBuffer, Buffer, LimitBufs, RefRead, RingBuffer, SliceRingBuffer, TmpBuffer,
    VECTORED_MAX,
};
use crate::future::{
    io_split, poll_async, select_2, select_3, select_4, select_option, AsyncLocalReceiver,
    AsyncLocalSender, AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt, CancellationToken,
    ReadHalf, Select2, Select3, Select4, StdWriteWrapper, Timeout, WriteHalf,
};
use crate::http1;
use crate::net::SocketAddr;
use crate::pin;
use crate::reactor::Reactor;
use crate::websocket;
use crate::zhttppacket;
use arrayvec::{ArrayString, ArrayVec};
use log::{debug, warn};
use sha1::{Digest, Sha1};
use std::cell::{Ref, RefCell};
use std::cmp;
use std::collections::VecDeque;
use std::future::Future;
use std::io;
use std::io::Write;
use std::pin::Pin;
use std::rc::Rc;
use std::str;
use std::str::FromStr;
use std::sync::mpsc;
use std::task::Context;
use std::task::Poll;
use std::time::Duration;

const URI_SIZE_MAX: usize = 4096;
const HEADERS_MAX: usize = 64;
const WS_HASH_INPUT_MAX: usize = 256;
const WS_ACCEPT_MAX: usize = 28; // base64_encode(sha1_hash) = 28 bytes
const ZHTTP_SESSION_TIMEOUT: Duration = Duration::from_secs(60);

pub trait CidProvider {
    fn get_new_assigned_cid(&mut self) -> ArrayString<32>;
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

fn calculate_ws_accept(key: &[u8]) -> Result<ArrayString<WS_ACCEPT_MAX>, ()> {
    let input_len = key.len() + websocket::WS_GUID.len();

    if input_len > WS_HASH_INPUT_MAX {
        return Err(());
    }

    let mut input = [0; WS_HASH_INPUT_MAX];

    input[..key.len()].copy_from_slice(key);
    input[key.len()..input_len].copy_from_slice(&websocket::WS_GUID.as_bytes());

    let input = &input[..input_len];

    let mut hasher = Sha1::new();
    hasher.update(input);
    let digest = hasher.finalize();

    let mut output = [0; WS_ACCEPT_MAX];

    let size = base64::encode_config_slice(&digest, base64::STANDARD, &mut output);

    let output = match str::from_utf8(&output[..size]) {
        Ok(s) => s,
        Err(_) => return Err(()),
    };

    Ok(ArrayString::from_str(output).unwrap())
}

fn validate_ws_request(
    req: &http1::Request,
    ws_version: Option<&[u8]>,
    ws_key: Option<&[u8]>,
) -> Result<ArrayString<WS_ACCEPT_MAX>, ()> {
    // a websocket request must not have a body.
    // some clients send "Content-Length: 0", which we'll allow.
    // chunked encoding will be rejected.
    if req.method == "GET"
        && (req.body_size == http1::BodySize::NoBody || req.body_size == http1::BodySize::Known(0))
        && ws_version == Some(b"13")
        && ws_key.is_some()
    {
        return calculate_ws_accept(&ws_key.unwrap());
    }

    Err(())
}

fn write_ws_ext_header_value<W: Write>(
    config: &websocket::PerMessageDeflateConfig,
    dest: &mut W,
) -> Result<(), io::Error> {
    write!(dest, "permessage-deflate")?;

    config.serialize(dest)
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
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    packet_buf: &mut [u8],
) -> Result<zmq::Message, io::Error> {
    let mut data = zhttppacket::RequestData::new();

    data.method = method;

    let host = get_host(headers);

    let mut zheaders = [zhttppacket::EMPTY_HEADER; HEADERS_MAX];
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

    if let Some(SocketAddr::Ip(peer_addr)) = peer_addr {
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

#[derive(Debug)]
enum ServerError {
    Io(io::Error),
    Utf8(str::Utf8Error),
    Http(http1::ServerError),
    WebSocket(websocket::Error),
    InvalidWebSocketRequest,
    CompressionError,
    BadMessage,
    HandlerError,
    HandlerCancel,
    BufferExceeded,
    BadFrame,
    Timeout,
    Stopped,
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

impl<T> From<mpsc::SendError<T>> for ServerError {
    fn from(_e: mpsc::SendError<T>) -> Self {
        Self::Io(io::Error::from(io::ErrorKind::BrokenPipe))
    }
}

impl<T> From<mpsc::TrySendError<T>> for ServerError {
    fn from(e: mpsc::TrySendError<T>) -> Self {
        let kind = match e {
            mpsc::TrySendError::Full(_) => io::ErrorKind::WriteZero,
            mpsc::TrySendError::Disconnected(_) => io::ErrorKind::BrokenPipe,
        };

        Self::Io(io::Error::from(kind))
    }
}

impl From<mpsc::RecvError> for ServerError {
    fn from(_e: mpsc::RecvError) -> Self {
        Self::Io(io::Error::from(io::ErrorKind::UnexpectedEof))
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

struct ServerStreamSharedDataInner {
    to_addr: Option<ArrayVec<u8, 64>>,
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

    fn set_to_addr(&self, addr: Option<ArrayVec<u8, 64>>) {
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

async fn recv_nonzero<R: AsyncRead>(r: &mut R, buf: &mut RingBuffer) -> Result<(), io::Error> {
    if buf.write_avail() == 0 {
        return Err(io::Error::from(io::ErrorKind::WriteZero));
    }

    let size = match r.read(buf.write_buf()).await {
        Ok(size) => size,
        Err(e) => return Err(e),
    };

    buf.write_commit(size);

    if size == 0 {
        return Err(io::Error::from(io::ErrorKind::UnexpectedEof));
    }

    Ok(())
}

struct LimitedRingBuffer<'a> {
    inner: &'a mut RingBuffer,
    limit: usize,
}

impl AsRef<[u8]> for LimitedRingBuffer<'_> {
    fn as_ref(&self) -> &[u8] {
        let buf = self.inner.read_buf();
        let limit = cmp::min(buf.len(), self.limit);

        &buf[..limit]
    }
}

struct HttpRead<'a, R: AsyncRead> {
    stream: ReadHalf<'a, R>,
    buf1: &'a mut RingBuffer,
    buf2: &'a mut RingBuffer,
}

struct HttpWrite<'a, W: AsyncWrite> {
    stream: WriteHalf<'a, W>,
}

struct RequestHandler<'a, R: AsyncRead, W: AsyncWrite> {
    r: HttpRead<'a, R>,
    w: HttpWrite<'a, W>,
}

impl<'a, R: AsyncRead, W: AsyncWrite> RequestHandler<'a, R, W> {
    fn new(
        stream: (ReadHalf<'a, R>, WriteHalf<'a, W>),
        buf1: &'a mut RingBuffer,
        buf2: &'a mut RingBuffer,
    ) -> Self {
        buf1.align();
        buf2.clear();

        Self {
            r: HttpRead {
                stream: stream.0,
                buf1,
                buf2,
            },
            w: HttpWrite { stream: stream.1 },
        }
    }

    // read from stream into buf, and parse buf as a request header
    async fn recv_request<'b: 'c, 'c, const N: usize>(
        mut self,
        mut scratch: &'b mut http1::RequestScratch<N>,
        req_mem: &'c mut Option<http1::OwnedRequest<'b, N>>,
    ) -> Result<RequestHeader<'a, 'b, 'c, R, W, N>, ServerError> {
        let mut protocol = http1::ServerProtocol::new();

        assert_eq!(protocol.state(), http1::ServerState::ReceivingRequest);

        loop {
            {
                let hbuf = self.r.buf1.take_inner();

                match protocol.recv_request_owned(hbuf, scratch) {
                    http1::RecvRequestStatus::Complete(req) => {
                        assert!([
                            http1::ServerState::ReceivingBody,
                            http1::ServerState::AwaitingResponse
                        ]
                        .contains(&protocol.state()));

                        *req_mem = Some(req);

                        break Ok(RequestHeader {
                            r: self.r,
                            w: self.w,
                            protocol,
                            req_mem,
                        });
                    }
                    http1::RecvRequestStatus::Incomplete(hbuf, ret_scratch) => {
                        // NOTE: after polonius it may not be necessary for
                        // scratch to be returned
                        scratch = ret_scratch;
                        self.r.buf1.set_inner(hbuf);
                    }
                    http1::RecvRequestStatus::Error(e, hbuf, _) => {
                        self.r.buf1.set_inner(hbuf);

                        return Err(e.into());
                    }
                }
            }

            if let Err(e) = recv_nonzero(&mut self.r.stream, self.r.buf1).await {
                if e.kind() == io::ErrorKind::WriteZero {
                    return Err(ServerError::BufferExceeded);
                }

                return Err(e.into());
            }
        }
    }
}

struct RequestHeader<'a, 'b, 'c, R: AsyncRead, W: AsyncWrite, const N: usize> {
    r: HttpRead<'a, R>,
    w: HttpWrite<'a, W>,
    protocol: http1::ServerProtocol,
    req_mem: &'c mut Option<http1::OwnedRequest<'b, N>>,
}

impl<'a, 'b, 'c, R: AsyncRead, W: AsyncWrite, const N: usize> RequestHeader<'a, 'b, 'c, R, W, N> {
    fn request(&self) -> http1::Request {
        self.req_mem.as_ref().unwrap().get()
    }

    async fn start_recv_body(mut self) -> Result<RequestRecvBody<'a, R, W>, ServerError> {
        self.handle_expect().await?;

        // restore the read ringbuffer
        self.discard_request();

        Ok(self.into_recv_body().0)
    }

    async fn start_recv_body_and_keep_header(
        mut self,
    ) -> Result<RequestRecvBodyKeepHeader<'a, 'b, 'c, R, W, N>, ServerError> {
        self.handle_expect().await?;

        // we're keeping the request, so put any remaining bytes into buf2
        // and swap the inner buffers. those bytes will then become readable
        // from buf1. we'll plan to give the request's inner buffer to buf2
        // after the request is no longer needed
        let req = self.req_mem.as_ref().unwrap();
        self.r.buf2.write(req.remaining_bytes())?;
        self.r.buf1.swap_inner(self.r.buf2);

        let (recv_body, req_mem) = self.into_recv_body();

        Ok(RequestRecvBodyKeepHeader {
            inner: recv_body,
            req_mem,
        })
    }

    fn recv_done(mut self) -> Result<RequestStartResponse<'a, R, W>, ServerError> {
        // restore the read ringbuffer
        self.discard_request();

        Ok(RequestStartResponse::new(self.r, self.w, self.protocol))
    }

    // this method requires the request to exist
    async fn handle_expect(&mut self) -> Result<(), ServerError> {
        if !self.request().expect_100 {
            return Ok(());
        }

        let mut cont = [0; 32];

        let cont = {
            let mut c = io::Cursor::new(&mut cont[..]);

            if let Err(e) = self.protocol.send_100_continue(&mut c) {
                return Err(e.into());
            }

            let size = c.position() as usize;

            &cont[..size]
        };

        let mut left = cont.len();

        while left > 0 {
            let pos = cont.len() - left;

            let size = match self.w.stream.write(&cont[pos..]).await {
                Ok(size) => size,
                Err(e) => return Err(e.into()),
            };

            left -= size;
        }

        Ok(())
    }

    // consumes request and gives the inner buffer back to buf1
    fn discard_request(&mut self) {
        let req = self.req_mem.take().unwrap();

        let remaining_len = req.remaining_bytes().len();
        let inner_buf = req.into_buf();
        let hsize = inner_buf.filled_len() - remaining_len;

        self.r.buf1.set_inner(inner_buf);
        self.r.buf1.read_commit(hsize);
    }

    fn into_recv_body(
        self,
    ) -> (
        RequestRecvBody<'a, R, W>,
        &'c mut Option<http1::OwnedRequest<'b, N>>,
    ) {
        (
            RequestRecvBody {
                r: RefCell::new(RecvBodyRead {
                    stream: self.r.stream,
                    buf: self.r.buf1,
                }),
                wstream: self.w.stream,
                buf2: self.r.buf2,
                protocol: RefCell::new(self.protocol),
            },
            self.req_mem,
        )
    }
}

struct RecvBodyRead<'a, R: AsyncRead> {
    stream: ReadHalf<'a, R>,
    buf: &'a mut RingBuffer,
}

struct RequestRecvBody<'a, R: AsyncRead, W: AsyncWrite> {
    r: RefCell<RecvBodyRead<'a, R>>,
    wstream: WriteHalf<'a, W>,
    buf2: &'a mut RingBuffer,
    protocol: RefCell<http1::ServerProtocol>,
}

impl<'a, R: AsyncRead, W: AsyncWrite> RequestRecvBody<'a, R, W> {
    fn more(&self) -> bool {
        self.protocol.borrow().state() == http1::ServerState::ReceivingBody
    }

    async fn add_to_recv_buffer(&self) -> Result<(), ServerError> {
        let r = &mut *self.r.borrow_mut();

        if let Err(e) = recv_nonzero(&mut r.stream, r.buf).await {
            if e.kind() == io::ErrorKind::WriteZero {
                return Err(ServerError::BufferExceeded);
            }

            return Err(e.into());
        }

        Ok(())
    }

    fn try_recv_body(&self, dest: &mut [u8]) -> Option<Result<usize, ServerError>> {
        let r = &mut *self.r.borrow_mut();
        let protocol = &mut *self.protocol.borrow_mut();

        if protocol.state() == http1::ServerState::ReceivingBody {
            loop {
                let (size, read_size) = {
                    let mut buf = io::Cursor::new(r.buf.read_buf());

                    let mut headers = [httparse::EMPTY_HEADER; HEADERS_MAX];

                    let (size, _) = match protocol.recv_body(&mut buf, dest, &mut headers) {
                        Ok(ret) => ret,
                        Err(e) => return Some(Err(e.into())),
                    };

                    let read_size = buf.position() as usize;

                    (size, read_size)
                };

                if protocol.state() == http1::ServerState::ReceivingBody && read_size == 0 {
                    if !r.buf.is_readable_contiguous() {
                        r.buf.align();
                        continue;
                    }

                    return None;
                }

                r.buf.read_commit(read_size);

                return Some(Ok(size));
            }
        }

        assert_eq!(protocol.state(), http1::ServerState::AwaitingResponse);

        Some(Ok(0))
    }

    async fn recv_body(&self, dest: &mut [u8]) -> Result<usize, ServerError> {
        loop {
            if let Some(ret) = self.try_recv_body(dest) {
                return ret;
            }

            self.add_to_recv_buffer().await?;
        }
    }

    fn recv_done(self) -> RequestStartResponse<'a, R, W> {
        let r = self.r.into_inner();

        RequestStartResponse::new(
            HttpRead {
                stream: r.stream,
                buf1: r.buf,
                buf2: self.buf2,
            },
            HttpWrite {
                stream: self.wstream,
            },
            self.protocol.into_inner(),
        )
    }
}

struct RequestRecvBodyKeepHeader<'a, 'b, 'c, R: AsyncRead, W: AsyncWrite, const N: usize> {
    inner: RequestRecvBody<'a, R, W>,
    req_mem: &'c mut Option<http1::OwnedRequest<'b, N>>,
}

impl<'a, 'b, 'c, R: AsyncRead, W: AsyncWrite, const N: usize>
    RequestRecvBodyKeepHeader<'a, 'b, 'c, R, W, N>
{
    fn request(&self) -> http1::Request {
        self.req_mem.as_ref().unwrap().get()
    }

    async fn recv_body(&self, dest: &mut [u8]) -> Result<usize, ServerError> {
        self.inner.recv_body(dest).await
    }

    fn recv_done(self) -> RequestStartResponse<'a, R, W> {
        // the request is no longer needed, so give its inner buffer to buf2
        // and clear it
        let buf = self.req_mem.take().unwrap().into_buf();
        self.inner.buf2.set_inner(buf);
        self.inner.buf2.clear();

        self.inner.recv_done()
    }
}

struct RequestStartResponse<'a, R: AsyncRead, W: AsyncWrite> {
    r: HttpRead<'a, R>,
    w: HttpWrite<'a, W>,
    protocol: http1::ServerProtocol,
}

impl<'a, R: AsyncRead, W: AsyncWrite> RequestStartResponse<'a, R, W> {
    fn new(r: HttpRead<'a, R>, w: HttpWrite<'a, W>, protocol: http1::ServerProtocol) -> Self {
        Self { r, w, protocol }
    }

    async fn fill_recv_buffer(&mut self) -> ServerError {
        loop {
            if let Err(e) = recv_nonzero(&mut self.r.stream, self.r.buf1).await {
                if e.kind() == io::ErrorKind::WriteZero {
                    // if there's no more space, suspend forever
                    let () = std::future::pending().await;
                }

                return e.into();
            }
        }
    }

    fn prepare_response(
        mut self,
        code: u32,
        reason: &str,
        headers: &[http1::Header<'_>],
        body_size: http1::BodySize,
    ) -> Result<RequestSendHeader<'a, R, W>, ServerError> {
        self.r.buf2.clear();

        let mut hbuf = io::Cursor::new(self.r.buf2.write_buf());

        if let Err(e) = self
            .protocol
            .send_response(&mut hbuf, code, reason, headers, body_size)
        {
            return Err(e.into());
        }

        let size = hbuf.position() as usize;
        self.r.buf2.write_commit(size);

        let (stream, buf1, buf2) = ((self.r.stream, self.w.stream), self.r.buf1, self.r.buf2);

        Ok(RequestSendHeader::new(
            stream,
            buf1,
            buf2,
            self.protocol,
            size,
        ))
    }
}

struct SendHeaderRead<'a, R: AsyncRead> {
    stream: ReadHalf<'a, R>,
    buf: &'a mut RingBuffer,
}

struct EarlyBody {
    overflow: Option<Buffer>,
    done: bool,
}

struct RequestSendHeader<'a, R: AsyncRead, W: AsyncWrite> {
    r: RefCell<SendHeaderRead<'a, R>>,
    wstream: RefCell<WriteHalf<'a, W>>,
    wbuf: RefCell<LimitedRingBuffer<'a>>,
    protocol: http1::ServerProtocol,
    early_body: RefCell<EarlyBody>,
}

impl<'a, R: AsyncRead, W: AsyncWrite> RequestSendHeader<'a, R, W> {
    fn new(
        stream: (ReadHalf<'a, R>, WriteHalf<'a, W>),
        buf1: &'a mut RingBuffer,
        buf2: &'a mut RingBuffer,
        protocol: http1::ServerProtocol,
        header_size: usize,
    ) -> Self {
        Self {
            r: RefCell::new(SendHeaderRead {
                stream: stream.0,
                buf: buf1,
            }),
            wstream: RefCell::new(stream.1),
            wbuf: RefCell::new(LimitedRingBuffer {
                inner: buf2,
                limit: header_size,
            }),
            protocol,
            early_body: RefCell::new(EarlyBody {
                overflow: None,
                done: false,
            }),
        }
    }

    async fn send_header(&self) -> Result<(), ServerError> {
        let mut stream = self.wstream.borrow_mut();

        // limit = header bytes left
        while self.wbuf.borrow().limit > 0 {
            let size = stream.write_shared(&self.wbuf).await?;

            let mut wbuf = self.wbuf.borrow_mut();

            wbuf.inner.read_commit(size);
            wbuf.limit -= size;
        }

        let mut wbuf = self.wbuf.borrow_mut();
        let mut early_body = self.early_body.borrow_mut();

        if let Some(overflow) = &mut early_body.overflow {
            wbuf.inner.write(overflow.read_buf())?;

            early_body.overflow = None;
        }

        Ok(())
    }

    fn append_body(&self, body: &[u8], more: bool, id: &str) -> Result<(), ServerError> {
        let mut wbuf = self.wbuf.borrow_mut();
        let mut early_body = self.early_body.borrow_mut();

        // limit = header bytes left
        if wbuf.limit > 0 {
            // if there are still header bytes in the buffer, then we may
            // need to overflow into a separate buffer if there's not enough
            // room

            let accepted = if early_body.overflow.is_none() {
                wbuf.inner.write(body)?
            } else {
                0
            };

            if accepted < body.len() {
                debug!("conn {}: overflowing {} bytes", id, body.len() - accepted);

                if early_body.overflow.is_none() {
                    // only allow overflowing as much as there are header
                    // bytes left
                    early_body.overflow = Some(Buffer::new(wbuf.limit));
                }

                let overflow = early_body.overflow.as_mut().unwrap();

                overflow.write_all(&body[accepted..])?;
            }
        } else {
            // if the header has been fully cleared from the buffer, then
            // always write directly to the buffer
            wbuf.inner.write_all(body)?;
        }

        early_body.done = !more;

        Ok(())
    }

    fn send_header_done(self) -> RequestSendBody<'a, R, W> {
        let r = self.r.into_inner();
        let wstream = self.wstream.into_inner();
        let wbuf = self.wbuf.into_inner();
        let early_body = self.early_body.borrow();

        assert_eq!(wbuf.limit, 0);
        assert_eq!(early_body.overflow.is_none(), true);

        let (stream, buf1, buf2) = { ((r.stream, wstream), r.buf, wbuf.inner) };

        RequestSendBody {
            r: RefCell::new(HttpSendBodyRead {
                stream: stream.0,
                buf: buf1,
            }),
            w: RefCell::new(HttpSendBodyWrite {
                stream: stream.1,
                buf: buf2,
                body_done: early_body.done,
            }),
            protocol: RefCell::new(self.protocol),
        }
    }
}

struct HttpSendBodyRead<'a, R: AsyncRead> {
    stream: ReadHalf<'a, R>,
    buf: &'a mut RingBuffer,
}

struct HttpSendBodyWrite<'a, W: AsyncWrite> {
    stream: WriteHalf<'a, W>,
    buf: &'a mut RingBuffer,
    body_done: bool,
}

struct SendBodyFuture<'a, 'b, W: AsyncWrite> {
    w: &'a RefCell<HttpSendBodyWrite<'b, W>>,
    protocol: &'a RefCell<http1::ServerProtocol>,
}

impl<'a, 'b, W: AsyncWrite> Future for SendBodyFuture<'a, 'b, W> {
    type Output = Result<usize, ServerError>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &*self;

        let w = &mut *f.w.borrow_mut();

        let stream = &mut w.stream;

        if !stream.is_writable() {
            return Poll::Pending;
        }

        let protocol = &mut *f.protocol.borrow_mut();

        let mut buf_arr = [&b""[..]; VECTORED_MAX - 2];
        let bufs = w.buf.get_ref_vectored(&mut buf_arr);

        match protocol.send_body(
            &mut StdWriteWrapper::new(Pin::new(&mut w.stream), cx),
            bufs,
            w.body_done,
            None,
        ) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(http1::ServerError::Io(e)) if e.kind() == io::ErrorKind::WouldBlock => {
                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e.into())),
        }
    }
}

impl<W: AsyncWrite> Drop for SendBodyFuture<'_, '_, W> {
    fn drop(&mut self) {
        self.w.borrow_mut().stream.cancel();
    }
}

struct RequestSendBody<'a, R: AsyncRead, W: AsyncWrite> {
    r: RefCell<HttpSendBodyRead<'a, R>>,
    w: RefCell<HttpSendBodyWrite<'a, W>>,
    protocol: RefCell<http1::ServerProtocol>,
}

impl<'a, R: AsyncRead, W: AsyncWrite> RequestSendBody<'a, R, W> {
    fn append_body(&self, body: &[u8], more: bool) -> Result<(), ServerError> {
        let w = &mut *self.w.borrow_mut();

        w.buf.write_all(body)?;
        w.body_done = !more;

        Ok(())
    }

    fn can_flush(&self) -> bool {
        let w = &*self.w.borrow();

        w.buf.read_avail() > 0 || w.body_done
    }

    async fn flush_body(&self) -> Result<(usize, bool), ServerError> {
        {
            let protocol = &*self.protocol.borrow();

            assert_eq!(protocol.state(), http1::ServerState::SendingBody);

            let w = &*self.w.borrow();

            if w.buf.read_avail() == 0 && !w.body_done {
                return Ok((0, false));
            }
        }

        let size = SendBodyFuture {
            w: &self.w,
            protocol: &self.protocol,
        }
        .await?;

        let w = &mut *self.w.borrow_mut();
        let protocol = &*self.protocol.borrow();

        w.buf.read_commit(size);

        if w.buf.read_avail() > 0
            || !w.body_done
            || protocol.state() == http1::ServerState::SendingBody
        {
            return Ok((size, false));
        }

        assert_eq!(protocol.state(), http1::ServerState::Finished);

        Ok((size, true))
    }

    async fn send_body(&self, body: &[u8], more: bool) -> Result<usize, ServerError> {
        let w = &mut *self.w.borrow_mut();
        let protocol = &mut *self.protocol.borrow_mut();

        assert_eq!(protocol.state(), http1::ServerState::SendingBody);

        Ok(protocol
            .send_body_async(&mut w.stream, &[body], !more, None)
            .await?)
    }

    async fn fill_recv_buffer(&self) -> ServerError {
        let r = &mut *self.r.borrow_mut();

        loop {
            if let Err(e) = recv_nonzero(&mut r.stream, r.buf).await {
                if e.kind() == io::ErrorKind::WriteZero {
                    // if there's no more space, suspend forever
                    let () = std::future::pending().await;
                }

                return e.into();
            }
        }
    }

    fn finish(self) -> bool {
        self.protocol.borrow().is_persistent()
    }
}

struct WebSocketRead<'a, R: AsyncRead> {
    stream: ReadHalf<'a, R>,
    buf: &'a mut RingBuffer,
}

struct WebSocketWrite<'a, W: AsyncWrite, M> {
    stream: WriteHalf<'a, W>,
    buf: &'a mut BaseRingBuffer<M>,
}

struct SendMessageContentFuture<'a, 'b, W: AsyncWrite, M> {
    w: &'a RefCell<WebSocketWrite<'b, W, M>>,
    protocol: &'a websocket::Protocol<M>,
    avail: usize,
    done: bool,
}

impl<'a, 'b, W: AsyncWrite, M: AsRef<[u8]> + AsMut<[u8]>> Future
    for SendMessageContentFuture<'a, 'b, W, M>
{
    type Output = Result<(usize, bool), ServerError>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &*self;

        let w = &mut *f.w.borrow_mut();

        let stream = &mut w.stream;

        if !stream.is_writable() {
            return Poll::Pending;
        }

        // protocol.send_message_content may add 1 element to vector
        let mut buf_arr = [&b""[..]; VECTORED_MAX - 1];
        let bufs = w.buf.get_ref_vectored(&mut buf_arr).limit(f.avail);

        match f.protocol.send_message_content(
            &mut StdWriteWrapper::new(Pin::new(&mut w.stream), cx),
            bufs,
            f.done,
        ) {
            Ok(ret) => Poll::Ready(Ok(ret)),
            Err(websocket::Error::Io(e)) if e.kind() == io::ErrorKind::WouldBlock => Poll::Pending,
            Err(e) => Poll::Ready(Err(e.into())),
        }
    }
}

impl<W: AsyncWrite, M> Drop for SendMessageContentFuture<'_, '_, W, M> {
    fn drop(&mut self) {
        self.w.borrow_mut().stream.cancel();
    }
}

struct WebSocketHandler<'a, R: AsyncRead, W: AsyncWrite> {
    r: RefCell<WebSocketRead<'a, R>>,
    w: RefCell<WebSocketWrite<'a, W, &'a mut [u8]>>,
    protocol: websocket::Protocol<&'a mut [u8]>,
}

impl<'a, R: AsyncRead, W: AsyncWrite> WebSocketHandler<'a, R, W> {
    fn new(
        stream: (ReadHalf<'a, R>, WriteHalf<'a, W>),
        buf1: &'a mut RingBuffer,
        buf2: &'a mut SliceRingBuffer<'a>,
        deflate_config: Option<(websocket::PerMessageDeflateConfig, SliceRingBuffer<'a>)>,
    ) -> Self {
        buf2.clear();

        Self {
            r: RefCell::new(WebSocketRead {
                stream: stream.0,
                buf: buf1,
            }),
            w: RefCell::new(WebSocketWrite {
                stream: stream.1,
                buf: buf2,
            }),
            protocol: websocket::Protocol::new(deflate_config),
        }
    }

    fn state(&self) -> websocket::State {
        self.protocol.state()
    }

    async fn add_to_recv_buffer(&self) -> Result<(), ServerError> {
        let r = &mut *self.r.borrow_mut();

        if let Err(e) = recv_nonzero(&mut r.stream, r.buf).await {
            if e.kind() == io::ErrorKind::WriteZero {
                return Err(ServerError::BufferExceeded);
            }

            return Err(e.into());
        }

        Ok(())
    }

    fn try_recv_message_content<'b>(
        &self,
        dest: &mut [u8],
    ) -> Option<Result<(u8, usize, bool), ServerError>> {
        let r = &mut *self.r.borrow_mut();

        loop {
            match self.protocol.recv_message_content(r.buf, dest) {
                Some(Ok(ret)) => return Some(Ok(ret)),
                Some(Err(e)) => return Some(Err(e.into())),
                None => {
                    if !r.buf.is_readable_contiguous() {
                        r.buf.align();
                        continue;
                    }

                    return None;
                }
            }
        }
    }

    fn accept_avail(&self) -> usize {
        self.w.borrow().buf.write_avail()
    }

    fn accept_body(&self, body: &[u8]) -> Result<(), ServerError> {
        let w = &mut *self.w.borrow_mut();

        w.buf.write_all(body)?;

        Ok(())
    }

    fn is_sending_message(&self) -> bool {
        self.protocol.is_sending_message()
    }

    fn send_message_start(&self, opcode: u8, mask: Option<[u8; 4]>) {
        self.protocol.send_message_start(opcode, mask);
    }

    async fn send_message_content<F>(
        &self,
        avail: usize,
        done: bool,
        bytes_sent: &F,
    ) -> Result<(usize, bool), ServerError>
    where
        F: Fn(),
    {
        loop {
            let (size, done) = SendMessageContentFuture {
                w: &self.w,
                protocol: &self.protocol,
                avail,
                done,
            }
            .await?;

            let w = &mut *self.w.borrow_mut();

            if size == 0 && !done {
                continue;
            }

            w.buf.read_commit(size);

            bytes_sent();

            return Ok((size, done));
        }
    }
}

struct ZhttpStreamSessionOut<'a> {
    instance_id: &'a str,
    id: &'a str,
    packet_buf: &'a RefCell<Vec<u8>>,
    sender_stream: &'a AsyncLocalSender<(ArrayVec<u8, 64>, zmq::Message)>,
    shared: &'a ServerStreamSharedData,
}

impl<'a> ZhttpStreamSessionOut<'a> {
    fn new(
        instance_id: &'a str,
        id: &'a str,
        packet_buf: &'a RefCell<Vec<u8>>,
        sender_stream: &'a AsyncLocalSender<(ArrayVec<u8, 64>, zmq::Message)>,
        shared: &'a ServerStreamSharedData,
    ) -> Self {
        Self {
            instance_id,
            id,
            packet_buf,
            sender_stream,
            shared,
        }
    }

    async fn check_send(&self) {
        self.sender_stream.check_send().await
    }

    // this method is non-blocking, in order to increment the sequence number
    // and send the message in one shot, without concurrent activity
    // interfering with the sequencing. to send asynchronously, first await
    // on check_send and then call this method
    fn try_send_msg(&self, zreq: zhttppacket::Request) -> Result<(), ServerError> {
        let msg = {
            let mut zreq = zreq;

            let ids = [zhttppacket::Id {
                id: self.id.as_bytes(),
                seq: Some(self.shared.out_seq()),
            }];

            zreq.from = self.instance_id.as_bytes();
            zreq.ids = &ids;
            zreq.multi = true;

            let packet_buf = &mut *self.packet_buf.borrow_mut();

            let size = zreq.serialize(packet_buf)?;

            zmq::Message::from(&packet_buf[..size])
        };

        let mut addr = ArrayVec::new();
        if addr
            .try_extend_from_slice(self.shared.to_addr().get().unwrap())
            .is_err()
        {
            return Err(io::Error::from(io::ErrorKind::InvalidInput).into());
        }

        self.sender_stream.try_send((addr, msg))?;

        self.shared.inc_out_seq();

        Ok(())
    }
}

struct ZhttpStreamSessionIn<'a, R> {
    id: &'a str,
    send_buf_size: usize,
    websocket: bool,
    receiver: &'a AsyncLocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    shared: &'a ServerStreamSharedData,
    msg_read: &'a R,
    next: Option<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    seq: u32,
    credits: u32,
    first_data: bool,
}

impl<'a, R> ZhttpStreamSessionIn<'a, R>
where
    R: Fn(),
{
    fn new(
        id: &'a str,
        send_buf_size: usize,
        websocket: bool,
        receiver: &'a AsyncLocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
        shared: &'a ServerStreamSharedData,
        msg_read: &'a R,
    ) -> Self {
        Self {
            id,
            send_buf_size,
            websocket,
            receiver,
            shared,
            msg_read,
            next: None,
            seq: 0,
            credits: 0,
            first_data: true,
        }
    }

    fn credits(&self) -> u32 {
        self.credits
    }

    fn subtract_credits(&mut self, amount: u32) {
        self.credits -= amount;
    }

    async fn peek_msg(&mut self) -> Result<&arena::Rc<zhttppacket::OwnedResponse>, ServerError> {
        if self.next.is_none() {
            let (r, id_index) = loop {
                let (r, id_index) = self.receiver.recv().await?;

                let zresp = r.get().get();

                if zresp.ids[id_index].id != self.id.as_bytes() {
                    // skip messages addressed to old ids
                    continue;
                }

                break (r, id_index);
            };

            let zresp = r.get().get();

            if !zresp.ptype_str.is_empty() {
                debug!("conn {}: handle packet: {}", self.id, zresp.ptype_str);
            } else {
                debug!("conn {}: handle packet: (data)", self.id);
            }

            if zresp.ids.len() == 0 {
                return Err(ServerError::BadMessage);
            }

            if let Some(seq) = zresp.ids[id_index].seq {
                if seq != self.seq {
                    debug!(
                        "conn {}: bad seq (expected {}, got {}), skipping",
                        self.id, self.seq, seq
                    );
                    return Err(ServerError::BadMessage);
                }

                self.seq += 1;
            }

            let mut addr = ArrayVec::new();
            if addr.try_extend_from_slice(zresp.from).is_err() {
                return Err(ServerError::BadMessage);
            }

            self.shared.set_to_addr(Some(addr));

            (self.msg_read)();

            match &zresp.ptype {
                zhttppacket::ResponsePacket::Data(rdata) => {
                    let mut credits = rdata.credits;

                    if self.first_data {
                        self.first_data = false;

                        if self.websocket && credits == 0 {
                            // workaround for pushpin-proxy, which doesn't
                            //   send credits on websocket accept
                            credits = self.send_buf_size as u32;
                            debug!(
                                "conn {}: no credits in websocket accept, assuming {}",
                                self.id, credits
                            );
                        }
                    }

                    self.credits += credits;
                }
                zhttppacket::ResponsePacket::Error(edata) => {
                    debug!(
                        "conn {}: zhttp error condition={}",
                        self.id, edata.condition
                    );
                }
                zhttppacket::ResponsePacket::Credit(cdata) => {
                    self.credits += cdata.credits;
                }
                zhttppacket::ResponsePacket::Ping(pdata) => {
                    self.credits += pdata.credits;
                }
                zhttppacket::ResponsePacket::Pong(pdata) => {
                    self.credits += pdata.credits;
                }
                _ => {}
            }

            self.next = Some((r, id_index));
        }

        Ok(&self.next.as_ref().unwrap().0)
    }

    async fn recv_msg(&mut self) -> Result<arena::Rc<zhttppacket::OwnedResponse>, ServerError> {
        self.peek_msg().await?;

        Ok(self.next.take().unwrap().0)
    }
}

async fn send_msg(
    sender: &AsyncLocalSender<zmq::Message>,
    msg: zmq::Message,
) -> Result<(), ServerError> {
    Ok(sender.send(msg).await?)
}

async fn discard_while<F, T>(
    receiver: &AsyncLocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    fut: F,
) -> F::Output
where
    F: Future<Output = Result<T, ServerError>> + Unpin,
{
    loop {
        match select_2(fut, receiver.recv()).await {
            Select2::R1(v) => break v,
            Select2::R2(_) => {
                // unexpected message in current state
                return Err(ServerError::BadMessage);
            }
        }
    }
}

// return true if persistent
async fn server_req_handler<S: AsyncRead + AsyncWrite>(
    id: &str,
    stream: &mut S,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    buf1: &mut RingBuffer,
    buf2: &mut RingBuffer,
    body_buf: &mut Buffer,
    packet_buf: &RefCell<Vec<u8>>,
    zsender: &AsyncLocalSender<zmq::Message>,
    zreceiver: &AsyncLocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
) -> Result<bool, ServerError> {
    let stream = RefCell::new(stream);

    let handler = RequestHandler::new(io_split(&stream), buf1, buf2);
    let mut scratch = http1::RequestScratch::<HEADERS_MAX>::new();
    let mut req_mem = None;

    // receive request header

    // ABR: discard_while
    let handler = {
        match discard_while(
            zreceiver,
            pin!(handler.recv_request(&mut scratch, &mut req_mem)),
        )
        .await
        {
            Ok(handler) => handler,
            Err(ServerError::Io(e)) if e.kind() == io::ErrorKind::UnexpectedEof => {
                return Ok(false)
            }
            Err(e) => return Err(e),
        }
    };

    // log request

    {
        let req = handler.request();
        let host = get_host(req.headers);
        let scheme = if secure { "https" } else { "http" };

        debug!(
            "conn {}: request: {} {}://{}{}",
            id, req.method, scheme, host, req.uri
        );
    }

    // receive request body

    // ABR: discard_while
    let handler = discard_while(zreceiver, pin!(handler.start_recv_body_and_keep_header())).await?;

    loop {
        // ABR: discard_while
        let size = discard_while(zreceiver, pin!(handler.recv_body(body_buf.write_buf()))).await?;

        if size == 0 {
            break;
        }

        body_buf.write_commit(size);
    }

    // determine how to respond

    let msg = {
        let req = handler.request();

        let mut websocket = false;

        for h in req.headers.iter() {
            if h.name.eq_ignore_ascii_case("Upgrade") && h.value == b"websocket" {
                websocket = true;
                break;
            }
        }

        if websocket {
            // websocket requests are not supported in req mode

            // toss the request body
            body_buf.clear();

            None
        } else {
            // regular http requests we can handle

            // prepare zmq message

            let ids = [zhttppacket::Id {
                id: id.as_bytes(),
                seq: None,
            }];

            let msg = make_zhttp_request(
                "",
                &ids,
                req.method,
                req.uri,
                req.headers,
                body_buf.read_buf(),
                false,
                Mode::HttpReq,
                0,
                peer_addr,
                secure,
                &mut *packet_buf.borrow_mut(),
            )?;

            // body consumed
            body_buf.clear();

            Some(msg)
        }
    };

    let (handler, websocket) = if let Some(msg) = msg {
        // handle as http

        let handler = handler.recv_done();

        // send message

        // ABR: discard_while
        discard_while(zreceiver, pin!(send_msg(&zsender, msg))).await?;

        // receive message

        let zresp = loop {
            // ABR: direct read
            let (zresp, id_index) = zreceiver.recv().await?;

            let zresp_ref = zresp.get().get();

            if zresp_ref.ids[id_index].id != id.as_bytes() {
                // skip messages addressed to old ids
                continue;
            }

            if !zresp_ref.ptype_str.is_empty() {
                debug!("conn {}: handle packet: {}", id, zresp_ref.ptype_str);
            } else {
                debug!("conn {}: handle packet: (data)", id);
            }

            // skip non-data messages

            match &zresp_ref.ptype {
                zhttppacket::ResponsePacket::Data(_) => break zresp,
                _ => debug!(
                    "conn {}: unexpected packet in req mode: {}",
                    id, zresp_ref.ptype_str
                ),
            }
        };

        let zresp = zresp.get().get();

        let rdata = match &zresp.ptype {
            zhttppacket::ResponsePacket::Data(rdata) => rdata,
            _ => unreachable!(), // we confirmed the type above
        };

        // send response header

        let handler = {
            let mut headers = [http1::EMPTY_HEADER; HEADERS_MAX];
            let mut headers_len = 0;

            for h in rdata.headers.iter() {
                if headers_len >= headers.len() {
                    return Err(ServerError::BadMessage);
                }

                headers[headers_len] = http1::Header {
                    name: h.name,
                    value: h.value,
                };

                headers_len += 1;
            }

            let headers = &headers[..headers_len];

            handler.prepare_response(
                rdata.code,
                rdata.reason,
                headers,
                http1::BodySize::Known(rdata.body.len()),
            )?
        };

        // ABR: discard_while
        discard_while(zreceiver, pin!(handler.send_header())).await?;

        let handler = handler.send_header_done();

        body_buf.write_all(&rdata.body)?;

        (handler, false)
    } else {
        // handle as websocket

        // send response header

        let headers = &[http1::Header {
            name: "Content-Type",
            value: b"text/plain",
        }];

        let body = "WebSockets not supported on req mode interface.\n";

        let handler = handler.recv_done();

        let handler = handler.prepare_response(
            400,
            "Bad Request",
            headers,
            http1::BodySize::Known(body.len()),
        )?;

        // ABR: discard_while
        discard_while(zreceiver, pin!(handler.send_header())).await?;

        let handler = handler.send_header_done();

        body_buf.write_all(body.as_bytes())?;

        (handler, true)
    };

    // send response body

    while body_buf.read_avail() > 0 {
        // ABR: discard_while
        let size = discard_while(
            zreceiver,
            pin!(handler.send_body(body_buf.read_buf(), false)),
        )
        .await?;

        body_buf.read_commit(size);
    }

    let persistent = handler.finish();

    if websocket {
        return Ok(false);
    }

    Ok(persistent)
}

async fn server_req_connection_inner<P: CidProvider, S: AsyncRead + AsyncWrite + Identify>(
    token: CancellationToken,
    cid: &mut ArrayString<32>,
    cid_provider: &mut P,
    mut stream: S,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    buffer_size: usize,
    body_buffer_size: usize,
    rb_tmp: &Rc<TmpBuffer>,
    packet_buf: Rc<RefCell<Vec<u8>>>,
    timeout: Duration,
    zsender: AsyncLocalSender<zmq::Message>,
    zreceiver: &AsyncLocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
) -> Result<(), ServerError> {
    let reactor = Reactor::current().unwrap();

    let mut buf1 = RingBuffer::new(buffer_size, rb_tmp);
    let mut buf2 = RingBuffer::new(buffer_size, rb_tmp);
    let mut body_buf = Buffer::new(body_buffer_size);

    loop {
        stream.set_id(cid);

        // this was originally logged when starting the non-async state
        // machine, so we'll keep doing that
        debug!("conn {}: assigning id", cid);

        let reuse = {
            let handler = server_req_handler(
                cid.as_ref(),
                &mut stream,
                peer_addr,
                secure,
                &mut buf1,
                &mut buf2,
                &mut body_buf,
                &packet_buf,
                &zsender,
                zreceiver,
            );

            let timeout = Timeout::new(reactor.now() + timeout);

            match select_3(pin!(handler), timeout.elapsed(), token.cancelled()).await {
                Select3::R1(ret) => ret?,
                Select3::R2(_) => return Err(ServerError::Timeout),
                Select3::R3(_) => return Err(ServerError::Stopped),
            }
        };

        if !reuse {
            break;
        }

        // note: buf1 is not cleared as there may be data to read

        buf2.clear();
        body_buf.clear();

        *cid = cid_provider.get_new_assigned_cid();
    }

    stream.close().await?;

    Ok(())
}

pub async fn server_req_connection<P: CidProvider, S: AsyncRead + AsyncWrite + Identify>(
    token: CancellationToken,
    mut cid: ArrayString<32>,
    cid_provider: &mut P,
    stream: S,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    buffer_size: usize,
    body_buffer_size: usize,
    rb_tmp: &Rc<TmpBuffer>,
    packet_buf: Rc<RefCell<Vec<u8>>>,
    timeout: Duration,
    zsender: AsyncLocalSender<zmq::Message>,
    zreceiver: AsyncLocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
) {
    match server_req_connection_inner(
        token,
        &mut cid,
        cid_provider,
        stream,
        peer_addr,
        secure,
        buffer_size,
        body_buffer_size,
        rb_tmp,
        packet_buf,
        timeout,
        zsender,
        &zreceiver,
    )
    .await
    {
        Ok(()) => debug!("conn {}: finished", cid),
        Err(e) => debug!("conn {}: process error: {:?}", cid, e),
    }
}

// this function will either return immediately or await messages
async fn handle_other<R>(
    zresp: &zhttppacket::Response<'_, '_, '_>,
    zsess_in: &mut ZhttpStreamSessionIn<'_, R>,
    zsess_out: &ZhttpStreamSessionOut<'_>,
) -> Result<(), ServerError>
where
    R: Fn(),
{
    match &zresp.ptype {
        zhttppacket::ResponsePacket::KeepAlive => Ok(()),
        zhttppacket::ResponsePacket::Credit(_) => Ok(()),
        zhttppacket::ResponsePacket::HandoffStart => {
            // discarding here is fine. the sender should cease sending
            // messages until we've replied with proceed
            discard_while(
                &zsess_in.receiver,
                pin!(async { Ok(zsess_out.check_send().await) }),
            )
            .await?;

            let zreq = zhttppacket::Request::new_handoff_proceed(b"", &[]);

            // check_send just finished, so this should succeed
            zsess_out.try_send_msg(zreq)?;

            // pause until we get a msg
            zsess_in.peek_msg().await?;

            Ok(())
        }
        zhttppacket::ResponsePacket::Error(_) => Err(ServerError::HandlerError),
        zhttppacket::ResponsePacket::Cancel => Err(ServerError::HandlerCancel),
        _ => Err(ServerError::BadMessage), // unexpected type
    }
}

async fn stream_recv_body<'a, 'b, 'c, R1, R2, R, W, const N: usize>(
    tmp_buf: &RefCell<Vec<u8>>,
    bytes_read: &R1,
    handler: RequestHeader<'a, 'b, 'c, R, W, N>,
    zsess_in: &mut ZhttpStreamSessionIn<'_, R2>,
    zsess_out: &ZhttpStreamSessionOut<'_>,
) -> Result<RequestStartResponse<'a, R, W>, ServerError>
where
    R1: Fn(),
    R2: Fn(),
    R: AsyncRead,
    W: AsyncWrite,
{
    let handler = {
        let mut start_recv_body = pin!(handler.start_recv_body());

        // ABR: poll_async doesn't block
        match poll_async(start_recv_body.as_mut()).await {
            Poll::Ready(ret) => ret?,
            Poll::Pending => {
                // if we get here, then the send buffer with the client is full

                // keep trying to process while reading messages
                loop {
                    // ABR: select contains read
                    let ret = select_2(start_recv_body.as_mut(), pin!(zsess_in.recv_msg())).await;

                    match ret {
                        Select2::R1(ret) => break ret?,
                        Select2::R2(ret) => {
                            let r = ret?;

                            let zresp_ref = r.get().get();

                            // note: if we get a data message, handle_other will
                            // error out. technically a data message should be
                            // allowed here, but we're not in a position to do
                            // anything with it, so we error.
                            //
                            // fortunately, the conditions to hit this are unusual:
                            //   * we need to receive a subsequent request over
                            //     a persistent connection
                            //   * that request needs to be one for which a body
                            //     would be expected, and the request needs to
                            //     include an expect header
                            //   * the send buffer to that connection needs to be
                            //     full
                            //   * the handler needs to provide an early response
                            //     before receiving the request body
                            //
                            // in other words, a client needs to send a large
                            // pipelined POST over a reused connection, before it
                            // has read the previous response, and the handler
                            // needs to reject the request

                            // ABR: handle_other
                            handle_other(zresp_ref, zsess_in, zsess_out).await?;
                        }
                    }
                }
            }
        }
    };

    {
        let mut check_send = pin!(None);
        let mut add_to_recv_buffer = pin!(None);

        loop {
            if zsess_in.credits() > 0 && add_to_recv_buffer.is_none() && check_send.is_none() {
                check_send.set(Some(zsess_out.check_send()));
            }

            // ABR: select contains read
            let ret = select_3(
                select_option(check_send.as_mut().as_pin_mut()),
                select_option(add_to_recv_buffer.as_mut().as_pin_mut()),
                pin!(zsess_in.peek_msg()),
            )
            .await;

            match ret {
                Select3::R1(()) => {
                    check_send.set(None);

                    assert!(zsess_in.credits() > 0);
                    assert_eq!(add_to_recv_buffer.is_none(), true);

                    let tmp_buf = &mut *tmp_buf.borrow_mut();
                    let max_read = cmp::min(tmp_buf.len(), zsess_in.credits() as usize);

                    let size = match handler.try_recv_body(&mut tmp_buf[..max_read]) {
                        Some(ret) => ret?,
                        None => {
                            add_to_recv_buffer.set(Some(handler.add_to_recv_buffer()));
                            continue;
                        }
                    };

                    bytes_read();

                    let body = &tmp_buf[..size];

                    zsess_in.subtract_credits(size as u32);

                    let mut rdata = zhttppacket::RequestData::new();
                    rdata.body = body;
                    rdata.more = handler.more();

                    let zreq = zhttppacket::Request::new_data(b"", &[], rdata);

                    // check_send just finished, so this should succeed
                    zsess_out.try_send_msg(zreq)?;

                    if !handler.more() {
                        break;
                    }
                }
                Select3::R2(ret) => {
                    ret?;

                    add_to_recv_buffer.set(None);
                }
                Select3::R3(ret) => {
                    let r = ret?;

                    let zresp_ref = r.get().get();

                    match &zresp_ref.ptype {
                        zhttppacket::ResponsePacket::Data(_) => break,
                        _ => {
                            // ABR: direct read
                            let r = zsess_in.recv_msg().await?;

                            let zresp_ref = r.get().get();

                            // ABR: handle_other
                            handle_other(zresp_ref, zsess_in, zsess_out).await?;
                        }
                    }
                }
            }
        }
    }

    Ok(handler.recv_done())
}

async fn stream_send_body<'a, R1, R2, R, W>(
    bytes_read: &R1,
    handler: &RequestSendBody<'a, R, W>,
    zsess_in: &mut ZhttpStreamSessionIn<'_, R2>,
    zsess_out: &ZhttpStreamSessionOut<'_>,
) -> Result<(), ServerError>
where
    R1: Fn(),
    R2: Fn(),
    R: AsyncRead,
    W: AsyncWrite,
{
    let mut out_credits = 0;

    let mut flush_body = pin!(None);
    let mut check_send = pin!(None);

    'main: loop {
        let ret = {
            if flush_body.is_none() && handler.can_flush() {
                flush_body.set(Some(handler.flush_body()));
            }

            if out_credits > 0 && check_send.is_none() {
                check_send.set(Some(zsess_out.check_send()));
            }

            // ABR: select contains read
            select_4(
                select_option(flush_body.as_mut().as_pin_mut()),
                select_option(check_send.as_mut().as_pin_mut()),
                pin!(zsess_in.recv_msg()),
                pin!(handler.fill_recv_buffer()),
            )
            .await
        };

        match ret {
            Select4::R1(ret) => {
                flush_body.set(None);

                let (size, done) = ret?;

                if done {
                    break;
                }

                out_credits += size as u32;

                if size > 0 {
                    bytes_read();
                }
            }
            Select4::R2(()) => {
                check_send.set(None);

                let zreq = zhttppacket::Request::new_credit(b"", &[], out_credits);
                out_credits = 0;

                // check_send just finished, so this should succeed
                zsess_out.try_send_msg(zreq)?;
            }
            Select4::R3(ret) => {
                let r = ret?;

                let zresp_ref = r.get().get();

                match &zresp_ref.ptype {
                    zhttppacket::ResponsePacket::Data(rdata) => {
                        handler.append_body(rdata.body, rdata.more)?;
                    }
                    _ => {
                        // if handoff requested, flush send buffer responding
                        match &zresp_ref.ptype {
                            zhttppacket::ResponsePacket::HandoffStart => {
                                if flush_body.is_none() && handler.can_flush() {
                                    flush_body.set(Some(handler.flush_body()));
                                }

                                while let Some(fut) = flush_body.as_mut().as_pin_mut() {
                                    let ret = discard_while(zsess_in.receiver, fut).await;
                                    flush_body.set(None);

                                    let (size, done) = ret?;

                                    if done {
                                        break 'main;
                                    }

                                    out_credits += size as u32;

                                    if size > 0 {
                                        bytes_read();
                                    }

                                    if handler.can_flush() {
                                        flush_body.set(Some(handler.flush_body()));
                                    }
                                }
                            }
                            _ => {}
                        }

                        // ABR: handle_other
                        handle_other(zresp_ref, zsess_in, zsess_out).await?;
                    }
                }
            }
            Select4::R4(e) => return Err(e),
        }
    }

    Ok(())
}

async fn stream_websocket<S, R1, R2>(
    id: &str,
    stream: RefCell<&mut S>,
    buf1: &mut RingBuffer,
    buf2: &mut RingBuffer,
    messages_max: usize,
    tmp_buf: &RefCell<Vec<u8>>,
    bytes_read: &R1,
    deflate_config: Option<(websocket::PerMessageDeflateConfig, usize)>,
    zsess_in: &mut ZhttpStreamSessionIn<'_, R2>,
    zsess_out: &ZhttpStreamSessionOut<'_>,
) -> Result<(), ServerError>
where
    S: AsyncRead + AsyncWrite,
    R1: Fn(),
    R2: Fn(),
{
    // buf2 must be empty since we will repurpose the memory
    assert_eq!(buf2.read_avail(), 0);
    let rb_tmp = buf2.get_tmp().clone();
    let mut wbuf = buf2.take_inner().into_inner();

    let (mut wbuf, deflate_config) = match deflate_config {
        Some((config, write_buf_size)) => {
            let (wbuf, ebuf) = wbuf.split_at_mut(write_buf_size);

            let wbuf = SliceRingBuffer::new(wbuf, &rb_tmp);
            let ebuf = SliceRingBuffer::new(ebuf, &rb_tmp);

            (wbuf, Some((config, ebuf)))
        }
        None => (SliceRingBuffer::new(&mut wbuf, &rb_tmp), None),
    };

    let handler = WebSocketHandler::new(io_split(&stream), buf1, &mut wbuf, deflate_config);
    let mut ws_in_tracker = MessageTracker::new(messages_max);

    let mut out_credits = 0;

    let mut check_send = pin!(None);
    let mut add_to_recv_buffer = pin!(None);
    let mut send_content = pin!(None);

    loop {
        let (do_send, do_recv) = match handler.state() {
            websocket::State::Connected => (true, true),
            websocket::State::PeerClosed => (true, false),
            websocket::State::Closing => (false, true),
            websocket::State::Finished => break,
        };

        if out_credits > 0
            || (do_recv && zsess_in.credits() > 0 && add_to_recv_buffer.is_none())
                && check_send.is_none()
        {
            check_send.set(Some(zsess_out.check_send()));
        }

        if do_send && send_content.is_none() {
            if let Some((mtype, avail, done)) = ws_in_tracker.current() {
                if !handler.is_sending_message() {
                    handler.send_message_start(mtype, None);
                }

                if avail > 0 || done {
                    send_content.set(Some(handler.send_message_content(avail, done, bytes_read)));
                }
            }
        }

        // ABR: select contains read
        let ret = select_4(
            select_option(check_send.as_mut().as_pin_mut()),
            select_option(add_to_recv_buffer.as_mut().as_pin_mut()),
            select_option(send_content.as_mut().as_pin_mut()),
            pin!(zsess_in.recv_msg()),
        )
        .await;

        match ret {
            Select4::R1(()) => {
                check_send.set(None);

                if out_credits > 0 {
                    let zreq = zhttppacket::Request::new_credit(b"", &[], out_credits as u32);
                    out_credits = 0;

                    // check_send just finished, so this should succeed
                    zsess_out.try_send_msg(zreq)?;
                    continue;
                }

                assert!(zsess_in.credits() > 0);
                assert_eq!(add_to_recv_buffer.is_none(), true);

                let tmp_buf = &mut *tmp_buf.borrow_mut();
                let max_read = cmp::min(tmp_buf.len(), zsess_in.credits() as usize);

                let (opcode, size, end) =
                    match handler.try_recv_message_content(&mut tmp_buf[..max_read]) {
                        Some(ret) => ret?,
                        None => {
                            add_to_recv_buffer.set(Some(handler.add_to_recv_buffer()));
                            continue;
                        }
                    };

                bytes_read();

                let body = &tmp_buf[..size];

                let zreq = match opcode {
                    websocket::OPCODE_TEXT | websocket::OPCODE_BINARY => {
                        if body.is_empty() && !end {
                            // don't bother sending empty message
                            continue;
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
                                Err(e) => return Err(e.into()),
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
                        debug!("conn {}: unsupported websocket opcode: {}", id, opcode);
                        return Err(ServerError::BadFrame);
                    }
                };

                zsess_in.subtract_credits(size as u32);

                // check_send just finished, so this should succeed
                zsess_out.try_send_msg(zreq)?;
            }
            Select4::R2(ret) => {
                ret?;

                add_to_recv_buffer.set(None);
            }
            Select4::R3(ret) => {
                send_content.set(None);

                let (size, done) = ret?;

                ws_in_tracker.consumed(size, done);

                if handler.state() == websocket::State::Connected
                    || handler.state() == websocket::State::PeerClosed
                {
                    out_credits += size as u32;
                }
            }
            Select4::R4(ret) => {
                let r = ret?;

                let zresp_ref = r.get().get();

                match &zresp_ref.ptype {
                    zhttppacket::ResponsePacket::Data(rdata) => match handler.state() {
                        websocket::State::Connected | websocket::State::PeerClosed => {
                            let avail = handler.accept_avail();

                            if let Err(e) = handler.accept_body(rdata.body) {
                                warn!(
                                    "received too much data from handler (size={}, credits={})",
                                    rdata.body.len(),
                                    avail,
                                );

                                return Err(e.into());
                            }

                            let opcode = match &rdata.content_type {
                                Some(zhttppacket::ContentType::Binary) => websocket::OPCODE_BINARY,
                                _ => websocket::OPCODE_TEXT,
                            };

                            if !ws_in_tracker.in_progress() {
                                if ws_in_tracker.start(opcode).is_err() {
                                    return Err(ServerError::BufferExceeded);
                                }
                            }

                            ws_in_tracker.extend(rdata.body.len());

                            if !rdata.more {
                                ws_in_tracker.done();
                            }
                        }
                        _ => {}
                    },
                    zhttppacket::ResponsePacket::Close(cdata) => match handler.state() {
                        websocket::State::Connected | websocket::State::PeerClosed => {
                            let (code, reason) = match cdata.status {
                                Some(v) => v,
                                None => (1000, ""),
                            };

                            let arr: [u8; 2] = code.to_be_bytes();

                            // close content isn't limited by credits. if we
                            // don't have space for it, just error out
                            handler.accept_body(&arr)?;
                            handler.accept_body(reason.as_bytes())?;

                            if ws_in_tracker.start(websocket::OPCODE_CLOSE).is_err() {
                                return Err(ServerError::BadFrame);
                            }

                            ws_in_tracker.extend(arr.len() + reason.len());
                            ws_in_tracker.done();
                        }
                        _ => {}
                    },
                    zhttppacket::ResponsePacket::Ping(pdata) => match handler.state() {
                        websocket::State::Connected | websocket::State::PeerClosed => {
                            let avail = handler.accept_avail();

                            if let Err(e) = handler.accept_body(pdata.body) {
                                warn!(
                                    "received too much data from handler (size={}, credits={})",
                                    pdata.body.len(),
                                    avail,
                                );

                                return Err(e.into());
                            }

                            if ws_in_tracker.start(websocket::OPCODE_PING).is_err() {
                                return Err(ServerError::BadFrame);
                            }

                            ws_in_tracker.extend(pdata.body.len());
                            ws_in_tracker.done();
                        }
                        _ => {}
                    },
                    zhttppacket::ResponsePacket::Pong(pdata) => match handler.state() {
                        websocket::State::Connected | websocket::State::PeerClosed => {
                            let avail = handler.accept_avail();

                            if let Err(e) = handler.accept_body(pdata.body) {
                                warn!(
                                    "received too much data from handler (size={}, credits={})",
                                    pdata.body.len(),
                                    avail,
                                );

                                return Err(e.into());
                            }

                            if ws_in_tracker.start(websocket::OPCODE_PONG).is_err() {
                                return Err(ServerError::BadFrame);
                            }

                            ws_in_tracker.extend(pdata.body.len());
                            ws_in_tracker.done();
                        }
                        _ => {}
                    },
                    _ => {
                        // if handoff requested, flush send buffer responding
                        match &zresp_ref.ptype {
                            zhttppacket::ResponsePacket::HandoffStart => loop {
                                if send_content.is_none() {
                                    if let Some((mtype, avail, done)) = ws_in_tracker.current() {
                                        if !handler.is_sending_message() {
                                            handler.send_message_start(mtype, None);
                                        }

                                        if avail > 0 || done {
                                            send_content
                                                .set(Some(handler.send_message_content(
                                                    avail, done, bytes_read,
                                                )));
                                        }
                                    }
                                }

                                if let Some(fut) = send_content.as_mut().as_pin_mut() {
                                    let ret = discard_while(zsess_in.receiver, fut).await;
                                    send_content.set(None);

                                    let (size, done) = ret?;

                                    ws_in_tracker.consumed(size, done);

                                    if handler.state() == websocket::State::Connected
                                        || handler.state() == websocket::State::PeerClosed
                                    {
                                        out_credits += size as u32;
                                    }
                                } else {
                                    break;
                                }
                            },
                            _ => {}
                        }

                        // ABR: handle_other
                        handle_other(zresp_ref, zsess_in, zsess_out).await?;
                    }
                }
            }
        }
    }

    Ok(())
}

// return true if persistent
async fn server_stream_handler<S, R1, R2>(
    id: &str,
    stream: &mut S,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    buf1: &mut RingBuffer,
    buf2: &mut RingBuffer,
    messages_max: usize,
    allow_compression: bool,
    packet_buf: &RefCell<Vec<u8>>,
    tmp_buf: &RefCell<Vec<u8>>,
    instance_id: &str,
    zsender: &AsyncLocalSender<zmq::Message>,
    zsender_stream: &AsyncLocalSender<(ArrayVec<u8, 64>, zmq::Message)>,
    zreceiver: &AsyncLocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    shared: &ServerStreamSharedData,
    refresh_stream_timeout: &R1,
    refresh_session_timeout: &R2,
) -> Result<bool, ServerError>
where
    S: AsyncRead + AsyncWrite,
    R1: Fn(),
    R2: Fn(),
{
    let stream = RefCell::new(stream);

    let send_buf_size = buf1.capacity();
    let recv_buf_size = buf2.capacity();

    let handler = RequestHandler::new(io_split(&stream), buf1, buf2);
    let mut scratch = http1::RequestScratch::<HEADERS_MAX>::new();
    let mut req_mem = None;

    let zsess_out = ZhttpStreamSessionOut::new(instance_id, id, packet_buf, zsender_stream, shared);

    // receive request header

    // ABR: discard_while
    let handler = match discard_while(
        zreceiver,
        pin!(handler.recv_request(&mut scratch, &mut req_mem)),
    )
    .await
    {
        Ok(handler) => handler,
        Err(ServerError::Io(e)) if e.kind() == io::ErrorKind::UnexpectedEof => return Ok(false),
        Err(e) => return Err(e),
    };

    refresh_stream_timeout();

    let (body_size, ws_config, msg) = {
        let req = handler.request();

        let mut websocket = false;
        let mut ws_version = None;
        let mut ws_key = None;
        let mut ws_deflate_config = None;

        for h in req.headers.iter() {
            if h.name.eq_ignore_ascii_case("Upgrade") && h.value == b"websocket" {
                websocket = true;
            }

            if h.name.eq_ignore_ascii_case("Sec-WebSocket-Version") {
                ws_version = Some(h.value);
            }

            if h.name.eq_ignore_ascii_case("Sec-WebSocket-Key") {
                ws_key = Some(h.value);
            }

            if h.name.eq_ignore_ascii_case("Sec-WebSocket-Extensions") {
                for value in http1::parse_header_value(h.value) {
                    let (name, params) = match value {
                        Ok(v) => v,
                        Err(_) => return Err(ServerError::InvalidWebSocketRequest),
                    };

                    match name {
                        "permessage-deflate" => {
                            // the client can present multiple offers. take
                            // the first that works. if none work, it's not
                            // an error. we'll just not use compression
                            if allow_compression && ws_deflate_config.is_none() {
                                if let Ok(config) =
                                    websocket::PerMessageDeflateConfig::from_params(params)
                                {
                                    if let Ok(resp_config) = config.create_response() {
                                        // split the original write buffer memory:
                                        // 75% for a new write buffer, 25% for an encode buffer
                                        let write_buf_size = recv_buf_size * 3 / 4;

                                        ws_deflate_config = Some((resp_config, write_buf_size));
                                    }
                                }
                            }
                        }
                        name => {
                            debug!("ignoring unsupported websocket extension: {}", name);
                            continue;
                        }
                    }
                }
            }
        }

        // log request

        let host = get_host(req.headers);

        let scheme = if websocket {
            if secure {
                "wss"
            } else {
                "ws"
            }
        } else {
            if secure {
                "https"
            } else {
                "http"
            }
        };

        debug!(
            "conn {}: request: {} {}://{}{}",
            id, req.method, scheme, host, req.uri
        );

        let ws_config: Option<(
            ArrayString<WS_ACCEPT_MAX>,
            Option<(websocket::PerMessageDeflateConfig, usize)>,
        )> = if websocket {
            let accept = match validate_ws_request(&req, ws_version, ws_key) {
                Ok(s) => s,
                Err(_) => return Err(ServerError::InvalidWebSocketRequest),
            };

            Some((accept, ws_deflate_config))
        } else {
            None
        };

        let ids = [zhttppacket::Id {
            id: id.as_bytes(),
            seq: Some(shared.out_seq()),
        }];

        let (mode, more) = if websocket {
            (Mode::WebSocket, false)
        } else {
            let more = match req.body_size {
                http1::BodySize::NoBody => false,
                http1::BodySize::Known(x) => x > 0,
                http1::BodySize::Unknown => true,
            };

            (Mode::HttpStream, more)
        };

        let credits = if let Some((_, Some((_, write_buf_size)))) = &ws_config {
            *write_buf_size
        } else {
            recv_buf_size
        };

        let msg = make_zhttp_request(
            instance_id,
            &ids,
            req.method,
            req.uri,
            req.headers,
            b"",
            more,
            mode,
            credits as u32,
            peer_addr,
            secure,
            &mut *packet_buf.borrow_mut(),
        )?;

        shared.inc_out_seq();

        (req.body_size, ws_config, msg)
    };

    // send request message

    // ABR: discard_while
    discard_while(zreceiver, pin!(send_msg(&zsender, msg))).await?;

    let mut zsess_in = ZhttpStreamSessionIn::new(
        id,
        send_buf_size,
        ws_config.is_some(),
        zreceiver,
        shared,
        refresh_session_timeout,
    );

    // receive any message, in order to get a handler address
    // ABR: direct read
    zsess_in.peek_msg().await?;

    let mut handler = if body_size != http1::BodySize::NoBody {
        // receive request body and send to handler

        stream_recv_body(
            tmp_buf,
            refresh_stream_timeout,
            handler,
            &mut zsess_in,
            &zsess_out,
        )
        .await?
    } else {
        handler.recv_done()?
    };

    // receive response message

    let zresp = loop {
        // ABR: select contains read
        let ret = select_2(pin!(zsess_in.recv_msg()), pin!(handler.fill_recv_buffer())).await;

        match ret {
            Select2::R1(ret) => {
                let r = ret?;

                let zresp_ref = r.get().get();

                match &zresp_ref.ptype {
                    zhttppacket::ResponsePacket::Data(_)
                    | zhttppacket::ResponsePacket::Error(_) => break r,
                    _ => {
                        // ABR: handle_other
                        handle_other(zresp_ref, &mut zsess_in, &zsess_out).await?;
                    }
                }
            }
            Select2::R2(e) => return Err(e),
        }
    };

    // determine how to respond

    let (handler, ws_config) = {
        let zresp = zresp.get().get();

        let rdata = match &zresp.ptype {
            zhttppacket::ResponsePacket::Data(rdata) => rdata,
            zhttppacket::ResponsePacket::Error(edata) => {
                if ws_config.is_some() && edata.condition == "rejected" {
                    // send websocket rejection

                    let rdata = edata.rejected_info.as_ref().unwrap();

                    let handler = {
                        let mut headers = [http1::EMPTY_HEADER; HEADERS_MAX];
                        let mut headers_len = 0;

                        for h in rdata.headers.iter() {
                            // don't send these headers
                            if h.name.eq_ignore_ascii_case("Upgrade")
                                || h.name.eq_ignore_ascii_case("Connection")
                                || h.name.eq_ignore_ascii_case("Sec-WebSocket-Accept")
                                || h.name.eq_ignore_ascii_case("Sec-WebSocket-Extensions")
                            {
                                continue;
                            }

                            if headers_len >= headers.len() {
                                return Err(ServerError::BadMessage);
                            }

                            headers[headers_len] = http1::Header {
                                name: h.name,
                                value: h.value,
                            };

                            headers_len += 1;
                        }

                        let headers = &headers[..headers_len];

                        handler.prepare_response(
                            rdata.code,
                            rdata.reason,
                            headers,
                            http1::BodySize::Known(rdata.body.len()),
                        )?
                    };

                    // ABR: discard_while
                    discard_while(zreceiver, pin!(handler.send_header())).await?;

                    let handler = handler.send_header_done();

                    handler.append_body(rdata.body, false)?;

                    loop {
                        // ABR: discard_while
                        let (_, done) =
                            discard_while(zreceiver, pin!(handler.flush_body())).await?;

                        if done {
                            break;
                        }
                    }

                    return Ok(false);
                } else {
                    // ABR: handle_other
                    return Err(handle_other(zresp, &mut zsess_in, &zsess_out)
                        .await
                        .unwrap_err());
                }
            }
            _ => unreachable!(), // we confirmed the type above
        };

        // send response header

        let handler = {
            let mut headers = [http1::EMPTY_HEADER; HEADERS_MAX];
            let mut headers_len = 0;

            let mut body_size = http1::BodySize::Unknown;

            for h in rdata.headers.iter() {
                if ws_config.is_some() {
                    // don't send these headers
                    if h.name.eq_ignore_ascii_case("Upgrade")
                        || h.name.eq_ignore_ascii_case("Connection")
                        || h.name.eq_ignore_ascii_case("Sec-WebSocket-Accept")
                        || h.name.eq_ignore_ascii_case("Sec-WebSocket-Extensions")
                    {
                        continue;
                    }
                } else {
                    if h.name.eq_ignore_ascii_case("Content-Length") {
                        let s = str::from_utf8(h.value)?;

                        let clen: usize = match s.parse() {
                            Ok(clen) => clen,
                            Err(_) => {
                                return Err(io::Error::from(io::ErrorKind::InvalidInput).into())
                            }
                        };

                        body_size = http1::BodySize::Known(clen);
                    }
                }

                if headers_len >= headers.len() {
                    return Err(ServerError::BadMessage);
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

            let mut ws_ext = ArrayVec::<u8, 512>::new();

            if let Some(ws_config) = &ws_config {
                let accept_data = &ws_config.0;

                if headers_len + 4 > headers.len() {
                    return Err(ServerError::BadMessage);
                }

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
                    value: accept_data.as_bytes(),
                };
                headers_len += 1;

                if let Some((config, _)) = &ws_config.1 {
                    if write_ws_ext_header_value(config, &mut ws_ext).is_err() {
                        return Err(ServerError::CompressionError);
                    }

                    headers[headers_len] = http1::Header {
                        name: "Sec-WebSocket-Extensions",
                        value: ws_ext.as_ref(),
                    };
                    headers_len += 1;
                }
            }

            let headers = &headers[..headers_len];

            handler.prepare_response(rdata.code, rdata.reason, headers, body_size)?
        };

        handler.append_body(rdata.body, rdata.more, id)?;

        {
            let mut send_header = pin!(handler.send_header());

            loop {
                // ABR: select contains read
                let ret = select_2(send_header.as_mut(), pin!(zsess_in.recv_msg())).await;

                match ret {
                    Select2::R1(ret) => {
                        ret?;

                        break;
                    }
                    Select2::R2(ret) => {
                        let r = ret?;

                        let zresp_ref = r.get().get();

                        match &zresp_ref.ptype {
                            zhttppacket::ResponsePacket::Data(rdata) => {
                                handler.append_body(rdata.body, rdata.more, id)?;
                            }
                            _ => {
                                // ABR: handle_other
                                handle_other(zresp_ref, &mut zsess_in, &zsess_out).await?;
                            }
                        }
                    }
                }
            }
        }

        let handler = handler.send_header_done();

        refresh_stream_timeout();

        let ws_config = if let Some((_, ws_deflate_config)) = ws_config {
            Some(ws_deflate_config)
        } else {
            None
        };

        (handler, ws_config)
    };

    if let Some(deflate_config) = ws_config {
        drop(handler);

        // handle as websocket connection

        stream_websocket(
            id,
            stream,
            buf1,
            buf2,
            messages_max,
            tmp_buf,
            refresh_stream_timeout,
            deflate_config,
            &mut zsess_in,
            &zsess_out,
        )
        .await?;

        Ok(false)
    } else {
        // send response body

        stream_send_body(refresh_stream_timeout, &handler, &mut zsess_in, &zsess_out).await?;

        let persistent = handler.finish();

        Ok(persistent)
    }
}

async fn server_stream_connection_inner<P: CidProvider, S: AsyncRead + AsyncWrite + Identify>(
    token: CancellationToken,
    cid: &mut ArrayString<32>,
    cid_provider: &mut P,
    mut stream: S,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    buffer_size: usize,
    messages_max: usize,
    rb_tmp: &Rc<TmpBuffer>,
    packet_buf: Rc<RefCell<Vec<u8>>>,
    tmp_buf: Rc<RefCell<Vec<u8>>>,
    stream_timeout: Duration,
    allow_compression: bool,
    instance_id: &str,
    zsender: AsyncLocalSender<zmq::Message>,
    zsender_stream: AsyncLocalSender<(ArrayVec<u8, 64>, zmq::Message)>,
    zreceiver: &AsyncLocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    shared: arena::Rc<ServerStreamSharedData>,
) -> Result<(), ServerError> {
    let reactor = Reactor::current().unwrap();

    let mut buf1 = RingBuffer::new(buffer_size, rb_tmp);
    let mut buf2 = RingBuffer::new(buffer_size, rb_tmp);

    loop {
        stream.set_id(cid);

        // this was originally logged when starting the non-async state
        // machine, so we'll keep doing that
        debug!("conn {}: assigning id", cid);

        let reuse = {
            let stream_timeout_time = RefCell::new(reactor.now() + stream_timeout);
            let session_timeout_time = RefCell::new(reactor.now() + ZHTTP_SESSION_TIMEOUT);

            let soonest_time = || {
                cmp::min(
                    *stream_timeout_time.borrow(),
                    *session_timeout_time.borrow(),
                )
            };

            let timeout = Timeout::new(soonest_time());

            let refresh_stream_timeout = || {
                stream_timeout_time.replace(reactor.now() + stream_timeout);
                timeout.set_deadline(soonest_time());
            };

            let refresh_session_timeout = || {
                session_timeout_time.replace(reactor.now() + ZHTTP_SESSION_TIMEOUT);
                timeout.set_deadline(soonest_time());
            };

            let handler = pin!(server_stream_handler(
                cid.as_ref(),
                &mut stream,
                peer_addr,
                secure,
                &mut buf1,
                &mut buf2,
                messages_max,
                allow_compression,
                &packet_buf,
                &tmp_buf,
                instance_id,
                &zsender,
                &zsender_stream,
                zreceiver,
                shared.get(),
                &refresh_stream_timeout,
                &refresh_session_timeout,
            ));

            match select_3(handler, timeout.elapsed(), token.cancelled()).await {
                Select3::R1(ret) => match ret {
                    Ok(reuse) => reuse,
                    Err(e) => {
                        let handler_caused = match &e {
                            ServerError::BadMessage
                            | ServerError::HandlerError
                            | ServerError::HandlerCancel => true,
                            _ => false,
                        };

                        let shared = shared.get();

                        if !handler_caused && shared.to_addr().get().is_some() {
                            let id = cid.as_ref();

                            let msg = {
                                let mut zreq = zhttppacket::Request::new_cancel(b"", &[]);

                                let ids = [zhttppacket::Id {
                                    id: id.as_bytes(),
                                    seq: Some(shared.out_seq()),
                                }];

                                zreq.from = instance_id.as_bytes();
                                zreq.ids = &ids;
                                zreq.multi = true;

                                let packet_buf = &mut *packet_buf.borrow_mut();

                                let size = zreq.serialize(packet_buf)?;

                                shared.inc_out_seq();

                                zmq::Message::from(&packet_buf[..size])
                            };

                            let mut addr = ArrayVec::new();
                            if addr
                                .try_extend_from_slice(shared.to_addr().get().unwrap())
                                .is_err()
                            {
                                return Err(io::Error::from(io::ErrorKind::InvalidInput).into());
                            }

                            // best effort
                            let _ = zsender_stream.try_send((addr, msg));
                        }

                        return Err(e);
                    }
                },
                Select3::R2(_) => return Err(ServerError::Timeout),
                Select3::R3(_) => return Err(ServerError::Stopped),
            }
        };

        if !reuse {
            break;
        }

        // note: buf1 is not cleared as there may be data to read

        buf2.clear();
        shared.get().reset();

        *cid = cid_provider.get_new_assigned_cid();
    }

    discard_while(zreceiver, pin!(async { Ok(stream.close().await?) })).await?;

    Ok(())
}

pub async fn server_stream_connection<P: CidProvider, S: AsyncRead + AsyncWrite + Identify>(
    token: CancellationToken,
    mut cid: ArrayString<32>,
    cid_provider: &mut P,
    stream: S,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    buffer_size: usize,
    messages_max: usize,
    rb_tmp: &Rc<TmpBuffer>,
    packet_buf: Rc<RefCell<Vec<u8>>>,
    tmp_buf: Rc<RefCell<Vec<u8>>>,
    timeout: Duration,
    allow_compression: bool,
    instance_id: &str,
    zsender: AsyncLocalSender<zmq::Message>,
    zsender_stream: AsyncLocalSender<(ArrayVec<u8, 64>, zmq::Message)>,
    zreceiver: AsyncLocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    shared: arena::Rc<ServerStreamSharedData>,
) {
    match server_stream_connection_inner(
        token,
        &mut cid,
        cid_provider,
        stream,
        peer_addr,
        secure,
        buffer_size,
        messages_max,
        rb_tmp,
        packet_buf,
        tmp_buf,
        timeout,
        allow_compression,
        instance_id,
        zsender,
        zsender_stream,
        &zreceiver,
        shared,
    )
    .await
    {
        Ok(()) => debug!("conn {}: finished", cid),
        Err(e) => debug!("conn {}: process error: {:?}", cid, e),
    }
}

pub mod testutil {
    use super::*;
    use crate::buffer::TmpBuffer;
    use crate::channel;
    use crate::waker;
    use std::fmt;
    use std::future::Future;
    use std::io::Read;
    use std::mem;
    use std::rc::Rc;
    use std::sync::Arc;
    use std::task::{Context, Poll, Waker};
    use std::time::Instant;

    pub struct NoopWaker {}

    impl NoopWaker {
        pub fn new() -> Self {
            Self {}
        }

        pub fn into_std(self: Rc<NoopWaker>) -> Waker {
            waker::into_std(self)
        }
    }

    impl waker::RcWake for NoopWaker {
        fn wake(self: Rc<Self>) {}
    }

    pub struct StepExecutor<'a, F> {
        reactor: &'a Reactor,
        fut: Pin<Box<F>>,
    }

    impl<'a, F> StepExecutor<'a, F>
    where
        F: Future,
    {
        pub fn new(reactor: &'a Reactor, fut: F) -> Self {
            Self {
                reactor,
                fut: Box::pin(fut),
            }
        }

        pub fn step(&mut self) -> Poll<F::Output> {
            self.reactor.poll_nonblocking(self.reactor.now()).unwrap();

            let waker = Rc::new(NoopWaker::new()).into_std();
            let mut cx = Context::from_waker(&waker);

            self.fut.as_mut().poll(&mut cx)
        }

        pub fn advance_time(&mut self, now: Instant) {
            self.reactor.poll_nonblocking(now).unwrap();
        }
    }

    #[track_caller]
    pub fn check_poll<T, E>(p: Poll<Result<T, E>>) -> Option<T>
    where
        E: fmt::Debug,
    {
        match p {
            Poll::Ready(v) => match v {
                Ok(t) => Some(t),
                Err(e) => panic!("check_poll error: {:?}", e),
            },
            Poll::Pending => None,
        }
    }

    pub struct FakeSock {
        inbuf: Vec<u8>,
        outbuf: Vec<u8>,
        out_allow: usize,
    }

    impl FakeSock {
        pub fn new() -> Self {
            Self {
                inbuf: Vec::with_capacity(16384),
                outbuf: Vec::with_capacity(16384),
                out_allow: 0,
            }
        }

        pub fn add_readable(&mut self, buf: &[u8]) {
            self.inbuf.extend_from_slice(buf);
        }

        pub fn take_writable(&mut self) -> Vec<u8> {
            mem::take(&mut self.outbuf)
        }

        pub fn allow_write(&mut self, size: usize) {
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

    pub struct AsyncFakeSock {
        pub inner: Rc<RefCell<FakeSock>>,
    }

    impl AsyncFakeSock {
        pub fn new(sock: Rc<RefCell<FakeSock>>) -> Self {
            Self { inner: sock }
        }
    }

    impl AsyncRead for AsyncFakeSock {
        fn poll_read(
            self: Pin<&mut Self>,
            _cx: &mut Context,
            buf: &mut [u8],
        ) -> Poll<Result<usize, io::Error>> {
            let inner = &mut *self.inner.borrow_mut();

            match inner.read(buf) {
                Ok(usize) => Poll::Ready(Ok(usize)),
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => Poll::Pending,
                Err(e) => Poll::Ready(Err(e)),
            }
        }

        fn cancel(&mut self) {}
    }

    impl AsyncWrite for AsyncFakeSock {
        fn poll_write(
            self: Pin<&mut Self>,
            _cx: &mut Context,
            buf: &[u8],
        ) -> Poll<Result<usize, io::Error>> {
            let inner = &mut *self.inner.borrow_mut();

            match inner.write(buf) {
                Ok(usize) => Poll::Ready(Ok(usize)),
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => Poll::Pending,
                Err(e) => Poll::Ready(Err(e)),
            }
        }

        fn poll_close(self: Pin<&mut Self>, _cx: &mut Context) -> Poll<Result<(), io::Error>> {
            Poll::Ready(Ok(()))
        }

        fn is_writable(&self) -> bool {
            true
        }

        fn cancel(&mut self) {}
    }

    impl Identify for AsyncFakeSock {
        fn set_id(&mut self, _id: &str) {
            // do nothing
        }
    }

    pub struct SimpleCidProvider {
        pub cid: ArrayString<32>,
    }

    impl CidProvider for SimpleCidProvider {
        fn get_new_assigned_cid(&mut self) -> ArrayString<32> {
            self.cid
        }
    }

    async fn server_req_handler_fut(
        sock: Rc<RefCell<FakeSock>>,
        secure: bool,
        s_from_conn: channel::LocalSender<zmq::Message>,
        r_to_conn: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
        packet_buf: Rc<RefCell<Vec<u8>>>,
        buf1: &mut RingBuffer,
        buf2: &mut RingBuffer,
        body_buf: &mut Buffer,
    ) -> Result<bool, ServerError> {
        let mut sock = AsyncFakeSock::new(sock);

        let r_to_conn = AsyncLocalReceiver::new(r_to_conn);
        let s_from_conn = AsyncLocalSender::new(s_from_conn);

        server_req_handler(
            "1",
            &mut sock,
            None,
            secure,
            buf1,
            buf2,
            body_buf,
            &packet_buf,
            &s_from_conn,
            &r_to_conn,
        )
        .await
    }

    pub struct BenchServerReqHandlerArgs {
        sock: Rc<RefCell<FakeSock>>,
        buf1: RingBuffer,
        buf2: RingBuffer,
        body_buf: Buffer,
    }

    pub struct BenchServerReqHandler {
        reactor: Reactor,
        msg_mem: Arc<arena::ArcMemory<zmq::Message>>,
        scratch_mem: Rc<arena::RcMemory<RefCell<zhttppacket::ParseScratch<'static>>>>,
        resp_mem: Rc<arena::RcMemory<zhttppacket::OwnedResponse>>,
        rb_tmp: Rc<TmpBuffer>,
        packet_buf: Rc<RefCell<Vec<u8>>>,
    }

    impl BenchServerReqHandler {
        pub fn new() -> Self {
            Self {
                reactor: Reactor::new(100),
                msg_mem: Arc::new(arena::ArcMemory::new(1)),
                scratch_mem: Rc::new(arena::RcMemory::new(1)),
                resp_mem: Rc::new(arena::RcMemory::new(1)),
                rb_tmp: Rc::new(TmpBuffer::new(1024)),
                packet_buf: Rc::new(RefCell::new(vec![0; 2048])),
            }
        }

        pub fn init(&self) -> BenchServerReqHandlerArgs {
            let buffer_size = 1024;

            BenchServerReqHandlerArgs {
                sock: Rc::new(RefCell::new(FakeSock::new())),
                buf1: RingBuffer::new(buffer_size, &self.rb_tmp),
                buf2: RingBuffer::new(buffer_size, &self.rb_tmp),
                body_buf: Buffer::new(buffer_size),
            }
        }

        pub fn run(&self, args: &mut BenchServerReqHandlerArgs) {
            let reactor = &self.reactor;
            let msg_mem = &self.msg_mem;
            let scratch_mem = &self.scratch_mem;
            let resp_mem = &self.resp_mem;
            let packet_buf = &self.packet_buf;
            let sock = &args.sock;

            let (s_to_conn, r_to_conn) =
                channel::local_channel(1, 1, &reactor.local_registration_memory());
            let (s_from_conn, r_from_conn) =
                channel::local_channel(1, 2, &reactor.local_registration_memory());

            let fut = {
                let sock = args.sock.clone();
                let s_from_conn = s_from_conn
                    .try_clone(&reactor.local_registration_memory())
                    .unwrap();

                server_req_handler_fut(
                    sock,
                    false,
                    s_from_conn,
                    r_to_conn,
                    packet_buf.clone(),
                    &mut args.buf1,
                    &mut args.buf2,
                    &mut args.body_buf,
                )
            };

            let mut executor = StepExecutor::new(reactor, fut);

            assert_eq!(check_poll(executor.step()), None);

            let req_data = concat!(
                "GET /path HTTP/1.1\r\n",
                "Host: example.com\r\n",
                "Connection: close\r\n",
                "\r\n"
            )
            .as_bytes();

            sock.borrow_mut().add_readable(req_data);
            sock.borrow_mut().allow_write(1024);

            assert_eq!(check_poll(executor.step()), None);

            // read message
            let _ = r_from_conn.try_recv().unwrap();

            let msg = concat!(
                "T100:2:id,1:1,4:code,3:200#6:reason,2:OK,7:h",
                "eaders,34:30:12:Content-Type,10:text/plain,]]4:body,6:hell",
                "o\n,}",
            );

            let msg = zmq::Message::from(msg.as_bytes());
            let msg = arena::Arc::new(msg, msg_mem).unwrap();

            let scratch =
                arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), scratch_mem)
                    .unwrap();

            let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
            let resp = arena::Rc::new(resp, resp_mem).unwrap();

            assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

            assert_eq!(check_poll(executor.step()), Some(false));

            let data = sock.borrow_mut().take_writable();

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
    }

    async fn server_req_connection_inner_fut(
        token: CancellationToken,
        sock: Rc<RefCell<FakeSock>>,
        secure: bool,
        s_from_conn: channel::LocalSender<zmq::Message>,
        r_to_conn: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
        rb_tmp: Rc<TmpBuffer>,
        packet_buf: Rc<RefCell<Vec<u8>>>,
    ) -> Result<(), ServerError> {
        let mut cid = ArrayString::from_str("1").unwrap();
        let mut cid_provider = SimpleCidProvider { cid };

        let sock = AsyncFakeSock::new(sock);

        let r_to_conn = AsyncLocalReceiver::new(r_to_conn);
        let s_from_conn = AsyncLocalSender::new(s_from_conn);
        let buffer_size = 1024;

        let timeout = Duration::from_millis(5_000);

        server_req_connection_inner(
            token,
            &mut cid,
            &mut cid_provider,
            sock,
            None,
            secure,
            buffer_size,
            buffer_size,
            &rb_tmp,
            packet_buf,
            timeout,
            s_from_conn,
            &r_to_conn,
        )
        .await
    }

    pub struct BenchServerReqConnection {
        reactor: Reactor,
        msg_mem: Arc<arena::ArcMemory<zmq::Message>>,
        scratch_mem: Rc<arena::RcMemory<RefCell<zhttppacket::ParseScratch<'static>>>>,
        resp_mem: Rc<arena::RcMemory<zhttppacket::OwnedResponse>>,
        rb_tmp: Rc<TmpBuffer>,
        packet_buf: Rc<RefCell<Vec<u8>>>,
    }

    impl BenchServerReqConnection {
        pub fn new() -> Self {
            Self {
                reactor: Reactor::new(100),
                msg_mem: Arc::new(arena::ArcMemory::new(1)),
                scratch_mem: Rc::new(arena::RcMemory::new(1)),
                resp_mem: Rc::new(arena::RcMemory::new(1)),
                rb_tmp: Rc::new(TmpBuffer::new(1024)),
                packet_buf: Rc::new(RefCell::new(vec![0; 2048])),
            }
        }

        pub fn init(&self) -> Rc<RefCell<FakeSock>> {
            Rc::new(RefCell::new(FakeSock::new()))
        }

        pub fn run(&self, sock: &Rc<RefCell<FakeSock>>) {
            let reactor = &self.reactor;
            let msg_mem = &self.msg_mem;
            let scratch_mem = &self.scratch_mem;
            let resp_mem = &self.resp_mem;
            let rb_tmp = &self.rb_tmp;
            let packet_buf = &self.packet_buf;

            let (s_to_conn, r_to_conn) =
                channel::local_channel(1, 1, &reactor.local_registration_memory());
            let (s_from_conn, r_from_conn) =
                channel::local_channel(1, 2, &reactor.local_registration_memory());
            let (_cancel, token) = CancellationToken::new(&reactor.local_registration_memory());

            let fut = {
                let sock = sock.clone();
                let s_from_conn = s_from_conn
                    .try_clone(&reactor.local_registration_memory())
                    .unwrap();

                server_req_connection_inner_fut(
                    token,
                    sock,
                    false,
                    s_from_conn,
                    r_to_conn,
                    rb_tmp.clone(),
                    packet_buf.clone(),
                )
            };

            let mut executor = StepExecutor::new(reactor, fut);

            assert_eq!(check_poll(executor.step()), None);

            let req_data = concat!(
                "GET /path HTTP/1.1\r\n",
                "Host: example.com\r\n",
                "Connection: close\r\n",
                "\r\n"
            )
            .as_bytes();

            sock.borrow_mut().add_readable(req_data);
            sock.borrow_mut().allow_write(1024);

            assert_eq!(check_poll(executor.step()), None);

            // read message
            let _ = r_from_conn.try_recv().unwrap();

            let msg = concat!(
                "T100:2:id,1:1,4:code,3:200#6:reason,2:OK,7:h",
                "eaders,34:30:12:Content-Type,10:text/plain,]]4:body,6:hell",
                "o\n,}",
            );

            let msg = zmq::Message::from(msg.as_bytes());
            let msg = arena::Arc::new(msg, msg_mem).unwrap();

            let scratch =
                arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), scratch_mem)
                    .unwrap();

            let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
            let resp = arena::Rc::new(resp, resp_mem).unwrap();

            assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

            assert_eq!(check_poll(executor.step()), Some(()));

            let data = sock.borrow_mut().take_writable();

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
    }

    async fn server_stream_handler_fut(
        sock: Rc<RefCell<FakeSock>>,
        secure: bool,
        s_from_conn: channel::LocalSender<zmq::Message>,
        s_stream_from_conn: channel::LocalSender<(ArrayVec<u8, 64>, zmq::Message)>,
        r_to_conn: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
        packet_buf: Rc<RefCell<Vec<u8>>>,
        tmp_buf: Rc<RefCell<Vec<u8>>>,
        buf1: &mut RingBuffer,
        buf2: &mut RingBuffer,
        shared: arena::Rc<ServerStreamSharedData>,
    ) -> Result<bool, ServerError> {
        let mut sock = AsyncFakeSock::new(sock);

        let r_to_conn = AsyncLocalReceiver::new(r_to_conn);
        let s_from_conn = AsyncLocalSender::new(s_from_conn);
        let s_stream_from_conn = AsyncLocalSender::new(s_stream_from_conn);

        server_stream_handler(
            "1",
            &mut sock,
            None,
            secure,
            buf1,
            buf2,
            10,
            false,
            &packet_buf,
            &tmp_buf,
            "test",
            &s_from_conn,
            &s_stream_from_conn,
            &r_to_conn,
            shared.get(),
            &|| {},
            &|| {},
        )
        .await
    }

    pub struct BenchServerStreamHandlerArgs {
        sock: Rc<RefCell<FakeSock>>,
        buf1: RingBuffer,
        buf2: RingBuffer,
    }

    pub struct BenchServerStreamHandler {
        reactor: Reactor,
        msg_mem: Arc<arena::ArcMemory<zmq::Message>>,
        scratch_mem: Rc<arena::RcMemory<RefCell<zhttppacket::ParseScratch<'static>>>>,
        resp_mem: Rc<arena::RcMemory<zhttppacket::OwnedResponse>>,
        shared_mem: Rc<arena::RcMemory<ServerStreamSharedData>>,
        rb_tmp: Rc<TmpBuffer>,
        packet_buf: Rc<RefCell<Vec<u8>>>,
        tmp_buf: Rc<RefCell<Vec<u8>>>,
    }

    impl BenchServerStreamHandler {
        pub fn new() -> Self {
            Self {
                reactor: Reactor::new(100),
                msg_mem: Arc::new(arena::ArcMemory::new(1)),
                scratch_mem: Rc::new(arena::RcMemory::new(1)),
                resp_mem: Rc::new(arena::RcMemory::new(1)),
                shared_mem: Rc::new(arena::RcMemory::new(1)),
                rb_tmp: Rc::new(TmpBuffer::new(1024)),
                packet_buf: Rc::new(RefCell::new(vec![0; 2048])),
                tmp_buf: Rc::new(RefCell::new(vec![0; 1024])),
            }
        }

        pub fn init(&self) -> BenchServerStreamHandlerArgs {
            let buffer_size = 1024;

            BenchServerStreamHandlerArgs {
                sock: Rc::new(RefCell::new(FakeSock::new())),
                buf1: RingBuffer::new(buffer_size, &self.rb_tmp),
                buf2: RingBuffer::new(buffer_size, &self.rb_tmp),
            }
        }

        pub fn run(&self, args: &mut BenchServerStreamHandlerArgs) {
            let reactor = &self.reactor;
            let msg_mem = &self.msg_mem;
            let scratch_mem = &self.scratch_mem;
            let resp_mem = &self.resp_mem;
            let shared_mem = &self.shared_mem;
            let packet_buf = &self.packet_buf;
            let tmp_buf = &self.tmp_buf;
            let sock = &args.sock;

            let (s_to_conn, r_to_conn) =
                channel::local_channel(1, 1, &reactor.local_registration_memory());
            let (s_from_conn, r_from_conn) =
                channel::local_channel(1, 2, &reactor.local_registration_memory());
            let (s_stream_from_conn, _r_stream_from_conn) =
                channel::local_channel(1, 2, &reactor.local_registration_memory());

            let fut = {
                let sock = args.sock.clone();
                let s_from_conn = s_from_conn
                    .try_clone(&reactor.local_registration_memory())
                    .unwrap();
                let shared = arena::Rc::new(ServerStreamSharedData::new(), &shared_mem).unwrap();

                server_stream_handler_fut(
                    sock,
                    false,
                    s_from_conn,
                    s_stream_from_conn,
                    r_to_conn,
                    packet_buf.clone(),
                    tmp_buf.clone(),
                    &mut args.buf1,
                    &mut args.buf2,
                    shared,
                )
            };

            let mut executor = StepExecutor::new(&reactor, fut);

            assert_eq!(check_poll(executor.step()), None);

            let req_data =
                concat!("GET /path HTTP/1.1\r\n", "Host: example.com\r\n", "\r\n").as_bytes();

            sock.borrow_mut().add_readable(req_data);
            sock.borrow_mut().allow_write(1024);

            assert_eq!(check_poll(executor.step()), None);

            // read message
            let _ = r_from_conn.try_recv().unwrap();

            let msg = concat!(
                "T127:2:id,1:1,6:reason,2:OK,7:headers,34:30:12:Content-Typ",
                "e,10:text/plain,]]3:seq,1:0#4:from,7:handler,4:code,3:200#",
                "4:body,6:hello\n,}",
            );

            let msg = zmq::Message::from(msg.as_bytes());
            let msg = arena::Arc::new(msg, &msg_mem).unwrap();

            let scratch =
                arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem)
                    .unwrap();

            let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
            let resp = arena::Rc::new(resp, &resp_mem).unwrap();

            assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

            assert_eq!(check_poll(executor.step()), Some(true));

            let data = sock.borrow_mut().take_writable();

            let expected = concat!(
                "HTTP/1.1 200 OK\r\n",
                "Content-Type: text/plain\r\n",
                "Content-Length: 6\r\n",
                "\r\n",
                "hello\n",
            );

            assert_eq!(str::from_utf8(&data).unwrap(), expected);
        }
    }

    async fn server_stream_connection_inner_fut(
        token: CancellationToken,
        sock: Rc<RefCell<FakeSock>>,
        secure: bool,
        s_from_conn: channel::LocalSender<zmq::Message>,
        s_stream_from_conn: channel::LocalSender<(ArrayVec<u8, 64>, zmq::Message)>,
        r_to_conn: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
        rb_tmp: Rc<TmpBuffer>,
        packet_buf: Rc<RefCell<Vec<u8>>>,
        tmp_buf: Rc<RefCell<Vec<u8>>>,
        shared: arena::Rc<ServerStreamSharedData>,
    ) -> Result<(), ServerError> {
        let mut cid = ArrayString::from_str("1").unwrap();
        let mut cid_provider = SimpleCidProvider { cid };

        let sock = AsyncFakeSock::new(sock);

        let r_to_conn = AsyncLocalReceiver::new(r_to_conn);
        let s_from_conn = AsyncLocalSender::new(s_from_conn);
        let s_stream_from_conn = AsyncLocalSender::new(s_stream_from_conn);
        let buffer_size = 1024;

        let timeout = Duration::from_millis(5_000);

        server_stream_connection_inner(
            token,
            &mut cid,
            &mut cid_provider,
            sock,
            None,
            secure,
            buffer_size,
            10,
            &rb_tmp,
            packet_buf,
            tmp_buf,
            timeout,
            false,
            "test",
            s_from_conn,
            s_stream_from_conn,
            &r_to_conn,
            shared,
        )
        .await
    }

    pub struct BenchServerStreamConnection {
        reactor: Reactor,
        msg_mem: Arc<arena::ArcMemory<zmq::Message>>,
        scratch_mem: Rc<arena::RcMemory<RefCell<zhttppacket::ParseScratch<'static>>>>,
        resp_mem: Rc<arena::RcMemory<zhttppacket::OwnedResponse>>,
        shared_mem: Rc<arena::RcMemory<ServerStreamSharedData>>,
        rb_tmp: Rc<TmpBuffer>,
        packet_buf: Rc<RefCell<Vec<u8>>>,
        tmp_buf: Rc<RefCell<Vec<u8>>>,
    }

    impl BenchServerStreamConnection {
        pub fn new() -> Self {
            Self {
                reactor: Reactor::new(100),
                msg_mem: Arc::new(arena::ArcMemory::new(1)),
                scratch_mem: Rc::new(arena::RcMemory::new(1)),
                resp_mem: Rc::new(arena::RcMemory::new(1)),
                shared_mem: Rc::new(arena::RcMemory::new(1)),
                rb_tmp: Rc::new(TmpBuffer::new(1024)),
                packet_buf: Rc::new(RefCell::new(vec![0; 2048])),
                tmp_buf: Rc::new(RefCell::new(vec![0; 1024])),
            }
        }

        pub fn init(&self) -> Rc<RefCell<FakeSock>> {
            Rc::new(RefCell::new(FakeSock::new()))
        }

        pub fn run(&self, sock: &Rc<RefCell<FakeSock>>) {
            let reactor = &self.reactor;
            let msg_mem = &self.msg_mem;
            let scratch_mem = &self.scratch_mem;
            let resp_mem = &self.resp_mem;
            let shared_mem = &self.shared_mem;
            let rb_tmp = &self.rb_tmp;
            let packet_buf = &self.packet_buf;
            let tmp_buf = &self.tmp_buf;

            let (s_to_conn, r_to_conn) =
                channel::local_channel(1, 1, &reactor.local_registration_memory());
            let (s_from_conn, r_from_conn) =
                channel::local_channel(1, 2, &reactor.local_registration_memory());
            let (s_stream_from_conn, _r_stream_from_conn) =
                channel::local_channel(1, 2, &reactor.local_registration_memory());
            let (_cancel, token) = CancellationToken::new(&reactor.local_registration_memory());

            let fut = {
                let sock = sock.clone();
                let s_from_conn = s_from_conn
                    .try_clone(&reactor.local_registration_memory())
                    .unwrap();
                let shared = arena::Rc::new(ServerStreamSharedData::new(), &shared_mem).unwrap();

                server_stream_connection_inner_fut(
                    token,
                    sock,
                    false,
                    s_from_conn,
                    s_stream_from_conn,
                    r_to_conn,
                    rb_tmp.clone(),
                    packet_buf.clone(),
                    tmp_buf.clone(),
                    shared,
                )
            };

            let mut executor = StepExecutor::new(&reactor, fut);

            assert_eq!(check_poll(executor.step()), None);

            let req_data =
                concat!("GET /path HTTP/1.1\r\n", "Host: example.com\r\n", "\r\n").as_bytes();

            sock.borrow_mut().add_readable(req_data);
            sock.borrow_mut().allow_write(1024);

            assert_eq!(check_poll(executor.step()), None);

            // read message
            let _ = r_from_conn.try_recv().unwrap();

            let msg = concat!(
                "T127:2:id,1:1,6:reason,2:OK,7:headers,34:30:12:Content-Typ",
                "e,10:text/plain,]]3:seq,1:0#4:from,7:handler,4:code,3:200#",
                "4:body,6:hello\n,}",
            );

            let msg = zmq::Message::from(msg.as_bytes());
            let msg = arena::Arc::new(msg, &msg_mem).unwrap();

            let scratch =
                arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem)
                    .unwrap();

            let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
            let resp = arena::Rc::new(resp, &resp_mem).unwrap();

            assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

            // connection reusable
            assert_eq!(check_poll(executor.step()), None);

            let data = sock.borrow_mut().take_writable();

            let expected = concat!(
                "HTTP/1.1 200 OK\r\n",
                "Content-Type: text/plain\r\n",
                "Content-Length: 6\r\n",
                "\r\n",
                "hello\n",
            );

            assert_eq!(str::from_utf8(&data).unwrap(), expected);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::testutil::*;
    use super::*;
    use crate::buffer::TmpBuffer;
    use crate::channel;
    use crate::websocket::Decoder;
    use std::rc::Rc;
    use std::sync::Arc;
    use std::task::Poll;
    use std::time::Instant;

    #[test]
    fn ws_ext_header() {
        let config = websocket::PerMessageDeflateConfig::default();
        let mut dest = ArrayVec::<u8, 512>::new();
        write_ws_ext_header_value(&config, &mut dest).unwrap();
        let expected = "permessage-deflate";
        assert_eq!(str::from_utf8(&dest).unwrap(), expected);

        let mut config = websocket::PerMessageDeflateConfig::default();
        config.client_no_context_takeover = true;
        let mut dest = ArrayVec::<u8, 512>::new();
        write_ws_ext_header_value(&config, &mut dest).unwrap();
        let expected = "permessage-deflate; client_no_context_takeover";
        assert_eq!(str::from_utf8(&dest).unwrap(), expected);
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
    fn early_body() {
        let reactor = Reactor::new(100);

        let sock = Rc::new(RefCell::new(FakeSock::new()));
        sock.borrow_mut().allow_write(1024);

        let sock = RefCell::new(AsyncFakeSock::new(sock));

        let rb_tmp = Rc::new(TmpBuffer::new(12));

        let mut buf1 = RingBuffer::new(12, &rb_tmp);
        let mut buf2 = RingBuffer::new(12, &rb_tmp);

        buf2.write(b"foo").unwrap();

        let handler = RequestSendHeader::new(
            io_split(&sock),
            &mut buf1,
            &mut buf2,
            http1::ServerProtocol::new(),
            3,
        );
        assert_eq!(handler.early_body.borrow().overflow.is_none(), true);

        handler.append_body(b"hello", false, "").unwrap();
        assert_eq!(handler.early_body.borrow().overflow.is_none(), true);

        handler.append_body(b" world", false, "").unwrap();
        assert_eq!(handler.early_body.borrow().overflow.is_some(), true);

        handler.append_body(b"!", false, "").unwrap();

        handler.append_body(b"!", false, "").unwrap_err();

        {
            let mut executor = StepExecutor::new(&reactor, handler.send_header());
            assert_eq!(check_poll(executor.step()), Some(()));
        }

        assert_eq!(handler.early_body.borrow().overflow.is_none(), true);

        let handler = handler.send_header_done();
        let header = sock.borrow_mut().inner.borrow_mut().take_writable();
        assert_eq!(header, b"foo");

        let w = handler.w.borrow();
        let mut buf_arr = [&b""[..]; VECTORED_MAX - 2];
        let bufs = w.buf.get_ref_vectored(&mut buf_arr);
        assert_eq!(bufs[0], b"hello wor");
        assert_eq!(bufs[1], b"ld!");
    }

    async fn req_fut(
        token: CancellationToken,
        sock: Rc<RefCell<FakeSock>>,
        secure: bool,
        s_from_conn: channel::LocalSender<zmq::Message>,
        r_to_conn: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    ) -> Result<(), ServerError> {
        let mut cid = ArrayString::from_str("1").unwrap();
        let mut cid_provider = SimpleCidProvider { cid };

        let sock = AsyncFakeSock::new(sock);

        let r_to_conn = AsyncLocalReceiver::new(r_to_conn);
        let s_from_conn = AsyncLocalSender::new(s_from_conn);
        let buffer_size = 1024;

        let rb_tmp = Rc::new(TmpBuffer::new(1024));
        let packet_buf = Rc::new(RefCell::new(vec![0; 2048]));

        let timeout = Duration::from_millis(5_000);

        server_req_connection_inner(
            token,
            &mut cid,
            &mut cid_provider,
            sock,
            None,
            secure,
            buffer_size,
            buffer_size,
            &rb_tmp,
            packet_buf,
            timeout,
            s_from_conn,
            &r_to_conn,
        )
        .await
    }

    #[test]
    fn server_req_without_body() {
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(1));
        let scratch_mem = Rc::new(arena::RcMemory::new(1));
        let resp_mem = Rc::new(arena::RcMemory::new(1));

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (_cancel, token) = CancellationToken::new(&reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();
            let s_from_conn = s_from_conn
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            req_fut(token, sock, false, s_from_conn, r_to_conn)
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        assert_eq!(check_poll(executor.step()), None);

        // no messages yet
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // fill the connection's outbound message queue
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_ok(), true);
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_err(), true);
        drop(s_from_conn);

        let req_data = concat!(
            "GET /path HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Connection: close\r\n",
            "\r\n"
        )
        .as_bytes();

        sock.borrow_mut().add_readable(req_data);

        // connection won't be able to send a message yet
        assert_eq!(check_poll(executor.step()), None);

        // read bogus message
        let msg = r_from_conn.try_recv().unwrap();
        assert_eq!(msg.is_empty(), true);

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // now connection will be able to send a message
        assert_eq!(check_poll(executor.step()), None);

        // read real message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "T148:2:id,1:1,3:ext,15:5:multi,4:true!}6:method,3:GET,3:ur",
            "i,23:http://example.com/path,7:headers,52:22:4:Host,11:exa",
            "mple.com,]22:10:Connection,5:close,]]}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let msg = concat!(
            "T100:2:id,1:1,4:code,3:200#6:reason,2:OK,7:h",
            "eaders,34:30:12:Content-Type,10:text/plain,]]4:body,6:hell",
            "o\n,}",
        );

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();
        assert_eq!(data.is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        assert_eq!(check_poll(executor.step()), Some(()));

        let data = sock.borrow_mut().take_writable();

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
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(1));
        let scratch_mem = Rc::new(arena::RcMemory::new(1));
        let resp_mem = Rc::new(arena::RcMemory::new(1));

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (_cancel, token) = CancellationToken::new(&reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();
            let s_from_conn = s_from_conn
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            req_fut(token, sock, false, s_from_conn, r_to_conn)
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        assert_eq!(check_poll(executor.step()), None);

        // no messages yet
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // fill the connection's outbound message queue
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_ok(), true);
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_err(), true);
        drop(s_from_conn);

        let req_data = concat!(
            "POST /path HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Content-Length: 6\r\n",
            "Connection: close\r\n",
            "\r\n",
            "hello\n"
        )
        .as_bytes();

        sock.borrow_mut().add_readable(req_data);

        // connection won't be able to send a message yet
        assert_eq!(check_poll(executor.step()), None);

        // read bogus message
        let msg = r_from_conn.try_recv().unwrap();
        assert_eq!(msg.is_empty(), true);

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // now connection will be able to send a message
        assert_eq!(check_poll(executor.step()), None);

        // read real message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "T191:2:id,1:1,3:ext,15:5:multi,4:true!}6:method,4:POST,3:u",
            "ri,23:http://example.com/path,7:headers,78:22:4:Host,11:ex",
            "ample.com,]22:14:Content-Length,1:6,]22:10:Connection,5:cl",
            "ose,]]4:body,6:hello\n,}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let msg = concat!(
            "T100:2:id,1:1,4:code,3:200#6:reason,2:OK,7:h",
            "eaders,34:30:12:Content-Type,10:text/plain,]]4:body,6:hell",
            "o\n,}",
        );

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();
        assert_eq!(data.is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        assert_eq!(check_poll(executor.step()), Some(()));

        let data = sock.borrow_mut().take_writable();

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
        let now = Instant::now();
        let reactor = Reactor::new_with_time(100, now);

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (_s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, _r_from_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (_cancel, token) = CancellationToken::new(&reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();

            req_fut(token, sock, false, s_from_conn, r_to_conn)
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        assert_eq!(check_poll(executor.step()), None);

        executor.advance_time(now + Duration::from_millis(5_000));

        match executor.step() {
            Poll::Ready(Err(ServerError::Timeout)) => {}
            _ => panic!("unexpected state"),
        }
    }

    #[test]
    fn server_req_pipeline() {
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(1));
        let scratch_mem = Rc::new(arena::RcMemory::new(1));
        let resp_mem = Rc::new(arena::RcMemory::new(1));

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (_cancel, token) = CancellationToken::new(&reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();
            let s_from_conn = s_from_conn
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            req_fut(token, sock, false, s_from_conn, r_to_conn)
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        assert_eq!(check_poll(executor.step()), None);

        // no messages yet
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // fill the connection's outbound message queue
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_ok(), true);
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_err(), true);
        drop(s_from_conn);

        let req_data = concat!(
            "GET /path1 HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "\r\n",
            "GET /path2 HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "\r\n",
        )
        .as_bytes();

        sock.borrow_mut().add_readable(req_data);

        // connection won't be able to send a message yet
        assert_eq!(check_poll(executor.step()), None);

        // read bogus message
        let msg = r_from_conn.try_recv().unwrap();
        assert_eq!(msg.is_empty(), true);

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // now connection will be able to send a message
        assert_eq!(check_poll(executor.step()), None);

        // read real message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "T123:2:id,1:1,3:ext,15:5:multi,4:true!}6:method,3:GET,3:ur",
            "i,24:http://example.com/path1,7:headers,26:22:4:Host,11:ex",
            "ample.com,]]}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let msg = concat!(
            "T100:2:id,1:1,4:code,3:200#6:reason,2:OK,7:h",
            "eaders,34:30:12:Content-Type,10:text/plain,]]4:body,6:hell",
            "o\n,}",
        );

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();
        assert_eq!(data.is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();

        let expected = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n",
        );

        assert_eq!(str::from_utf8(&data).unwrap(), expected);

        // read real message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "T123:2:id,1:1,3:ext,15:5:multi,4:true!}6:method,3:GET,3:ur",
            "i,24:http://example.com/path2,7:headers,26:22:4:Host,11:ex",
            "ample.com,]]}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let msg = concat!(
            "T100:2:id,1:1,4:code,3:200#6:reason,2:OK,7:h",
            "eaders,34:30:12:Content-Type,10:text/plain,]]4:body,6:hell",
            "o\n,}",
        );

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();

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
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(1));
        let scratch_mem = Rc::new(arena::RcMemory::new(1));
        let resp_mem = Rc::new(arena::RcMemory::new(1));

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (_cancel, token) = CancellationToken::new(&reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();
            let s_from_conn = s_from_conn
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            req_fut(token, sock, true, s_from_conn, r_to_conn)
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        assert_eq!(check_poll(executor.step()), None);

        // no messages yet
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // fill the connection's outbound message queue
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_ok(), true);
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_err(), true);
        drop(s_from_conn);

        let req_data = concat!(
            "GET /path HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Connection: close\r\n",
            "\r\n"
        )
        .as_bytes();

        sock.borrow_mut().add_readable(req_data);

        // connection won't be able to send a message yet
        assert_eq!(check_poll(executor.step()), None);

        // read bogus message
        let msg = r_from_conn.try_recv().unwrap();
        assert_eq!(msg.is_empty(), true);

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // now connection will be able to send a message
        assert_eq!(check_poll(executor.step()), None);

        // read real message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "T149:2:id,1:1,3:ext,15:5:multi,4:true!}6:method,3:GET,3:ur",
            "i,24:https://example.com/path,7:headers,52:22:4:Host,11:ex",
            "ample.com,]22:10:Connection,5:close,]]}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let msg = concat!(
            "T100:2:id,1:1,4:code,3:200#6:reason,2:OK,7:h",
            "eaders,34:30:12:Content-Type,10:text/plain,]]4:body,6:hell",
            "o\n,}",
        );

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();
        assert_eq!(data.is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        assert_eq!(check_poll(executor.step()), Some(()));

        let data = sock.borrow_mut().take_writable();

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

    async fn stream_fut(
        token: CancellationToken,
        sock: Rc<RefCell<FakeSock>>,
        secure: bool,
        allow_compression: bool,
        s_from_conn: channel::LocalSender<zmq::Message>,
        s_stream_from_conn: channel::LocalSender<(ArrayVec<u8, 64>, zmq::Message)>,
        r_to_conn: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    ) -> Result<(), ServerError> {
        let mut cid = ArrayString::from_str("1").unwrap();
        let mut cid_provider = SimpleCidProvider { cid };

        let sock = AsyncFakeSock::new(sock);

        let r_to_conn = AsyncLocalReceiver::new(r_to_conn);
        let s_from_conn = AsyncLocalSender::new(s_from_conn);
        let s_stream_from_conn = AsyncLocalSender::new(s_stream_from_conn);
        let buffer_size = 1024;

        let rb_tmp = Rc::new(TmpBuffer::new(1024));
        let packet_buf = Rc::new(RefCell::new(vec![0; 2048]));
        let tmp_buf = Rc::new(RefCell::new(vec![0; buffer_size]));

        let timeout = Duration::from_millis(5_000);

        let shared_mem = Rc::new(arena::RcMemory::new(1));
        let shared = arena::Rc::new(ServerStreamSharedData::new(), &shared_mem).unwrap();

        server_stream_connection_inner(
            token,
            &mut cid,
            &mut cid_provider,
            sock,
            None,
            secure,
            buffer_size,
            10,
            &rb_tmp,
            packet_buf,
            tmp_buf,
            timeout,
            allow_compression,
            "test",
            s_from_conn,
            s_stream_from_conn,
            &r_to_conn,
            shared,
        )
        .await
    }

    #[test]
    fn server_stream_without_body() {
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(1));
        let scratch_mem = Rc::new(arena::RcMemory::new(1));
        let resp_mem = Rc::new(arena::RcMemory::new(1));

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (s_stream_from_conn, _r_stream_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (_cancel, token) = CancellationToken::new(&reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();
            let s_from_conn = s_from_conn
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            stream_fut(
                token,
                sock,
                false,
                false,
                s_from_conn,
                s_stream_from_conn,
                r_to_conn,
            )
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        assert_eq!(check_poll(executor.step()), None);

        // no messages yet
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // fill the connection's outbound message queue
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_ok(), true);
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_err(), true);
        drop(s_from_conn);

        let req_data =
            concat!("GET /path HTTP/1.1\r\n", "Host: example.com\r\n", "\r\n").as_bytes();

        sock.borrow_mut().add_readable(req_data);

        // connection won't be able to send a message yet
        assert_eq!(check_poll(executor.step()), None);

        // read bogus message
        let msg = r_from_conn.try_recv().unwrap();
        assert_eq!(msg.is_empty(), true);

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // now connection will be able to send a message
        assert_eq!(check_poll(executor.step()), None);

        // read real message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "T179:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:multi,4:t",
            "rue!}6:method,3:GET,3:uri,23:http://example.com/path,7:hea",
            "ders,26:22:4:Host,11:example.com,]]7:credits,4:1024#6:stre",
            "am,4:true!}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let msg = concat!(
            "T127:2:id,1:1,6:reason,2:OK,7:headers,34:30:12:Content-Typ",
            "e,10:text/plain,]]3:seq,1:0#4:from,7:handler,4:code,3:200#",
            "4:body,6:hello\n,}",
        );

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();
        assert_eq!(data.is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        // connection reusable
        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();

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
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(1));
        let scratch_mem = Rc::new(arena::RcMemory::new(1));
        let resp_mem = Rc::new(arena::RcMemory::new(1));

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (s_stream_from_conn, r_stream_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (_cancel, token) = CancellationToken::new(&reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();
            let s_from_conn = s_from_conn
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            stream_fut(
                token,
                sock,
                false,
                false,
                s_from_conn,
                s_stream_from_conn,
                r_to_conn,
            )
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        assert_eq!(check_poll(executor.step()), None);

        // no messages yet
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // fill the connection's outbound message queue
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_ok(), true);
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_err(), true);
        drop(s_from_conn);

        let req_data = concat!(
            "POST /path HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n"
        )
        .as_bytes();

        sock.borrow_mut().add_readable(req_data);

        // connection won't be able to send a message yet
        assert_eq!(check_poll(executor.step()), None);

        // read bogus message
        let msg = r_from_conn.try_recv().unwrap();
        assert_eq!(msg.is_empty(), true);

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // now connection will be able to send a message
        assert_eq!(check_poll(executor.step()), None);

        // read real message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "T220:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:multi,4:t",
            "rue!}6:method,4:POST,3:uri,23:http://example.com/path,7:he",
            "aders,52:22:4:Host,11:example.com,]22:14:Content-Length,1:",
            "6,]]7:credits,4:1024#4:more,4:true!6:stream,4:true!}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let msg =
            concat!("T69:7:credits,4:1024#3:seq,1:0#2:id,1:1,4:from,7:handler,4:type,6:credit,}",);

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        // read message
        let (addr, msg) = r_stream_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        assert_eq!(addr.as_ref(), "handler".as_bytes());

        let buf = &msg[..];

        let expected = concat!(
            "T74:4:from,4:test,2:id,1:1,3:seq,1:1#3:ext,15:5:multi,4:tr",
            "ue!}4:body,6:hello\n,}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let msg = concat!(
            "T127:2:id,1:1,6:reason,2:OK,7:headers,34:30:12:Content-Typ",
            "e,10:text/plain,]]3:seq,1:1#4:from,7:handler,4:code,3:200#",
            "4:body,6:hello\n,}",
        );

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();
        assert_eq!(data.is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        // connection reusable
        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();

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
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(2));
        let scratch_mem = Rc::new(arena::RcMemory::new(2));
        let resp_mem = Rc::new(arena::RcMemory::new(2));

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (s_stream_from_conn, _r_stream_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (_cancel, token) = CancellationToken::new(&reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();
            let s_from_conn = s_from_conn
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            stream_fut(
                token,
                sock,
                false,
                false,
                s_from_conn,
                s_stream_from_conn,
                r_to_conn,
            )
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        assert_eq!(check_poll(executor.step()), None);

        // no messages yet
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // fill the connection's outbound message queue
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_ok(), true);
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_err(), true);
        drop(s_from_conn);

        let req_data =
            concat!("GET /path HTTP/1.1\r\n", "Host: example.com\r\n", "\r\n").as_bytes();

        sock.borrow_mut().add_readable(req_data);

        // connection won't be able to send a message yet
        assert_eq!(check_poll(executor.step()), None);

        // read bogus message
        let msg = r_from_conn.try_recv().unwrap();
        assert_eq!(msg.is_empty(), true);

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // now connection will be able to send a message
        assert_eq!(check_poll(executor.step()), None);

        // read real message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "T179:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:multi,4:t",
            "rue!}6:method,3:GET,3:uri,23:http://example.com/path,7:hea",
            "ders,26:22:4:Host,11:example.com,]]7:credits,4:1024#6:stre",
            "am,4:true!}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let msg = concat!(
            "T125:4:more,4:true!2:id,1:1,6:reason,2:OK,7:headers,34:30:",
            "12:Content-Type,10:text/plain,]]3:seq,1:0#4:from,7:handler",
            ",4:code,3:200#}",
        );

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let msg = concat!("T52:3:seq,1:1#2:id,1:1,4:from,7:handler,4:body,6:hello\n,}");

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();
        assert_eq!(data.is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        // connection reusable
        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();

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
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(1));
        let scratch_mem = Rc::new(arena::RcMemory::new(1));
        let resp_mem = Rc::new(arena::RcMemory::new(1));

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (s_stream_from_conn, _r_stream_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (_cancel, token) = CancellationToken::new(&reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();
            let s_from_conn = s_from_conn
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            stream_fut(
                token,
                sock,
                false,
                false,
                s_from_conn,
                s_stream_from_conn,
                r_to_conn,
            )
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        assert_eq!(check_poll(executor.step()), None);

        // no messages yet
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // fill the connection's outbound message queue
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_ok(), true);
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_err(), true);
        drop(s_from_conn);

        let req_data = concat!(
            "POST /path HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
        )
        .as_bytes();

        sock.borrow_mut().add_readable(req_data);

        // connection won't be able to send a message yet
        assert_eq!(check_poll(executor.step()), None);

        // read bogus message
        let msg = r_from_conn.try_recv().unwrap();
        assert_eq!(msg.is_empty(), true);

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // now connection will be able to send a message
        assert_eq!(check_poll(executor.step()), None);

        // read real message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "T220:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:multi,4:t",
            "rue!}6:method,4:POST,3:uri,23:http://example.com/path,7:he",
            "aders,52:22:4:Host,11:example.com,]22:14:Content-Length,1:",
            "6,]]7:credits,4:1024#4:more,4:true!6:stream,4:true!}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let msg = concat!(
            "T150:2:id,1:1,6:reason,11:Bad Request,7:headers,34:30:12:C",
            "ontent-Type,10:text/plain,]]3:seq,1:0#4:from,7:handler,4:c",
            "ode,3:400#4:body,18:stopping this now\n,}",
        );

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();
        assert_eq!(data.is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        assert_eq!(check_poll(executor.step()), Some(()));

        let data = sock.borrow_mut().take_writable();

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

    #[test]
    fn server_websocket() {
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(2));
        let scratch_mem = Rc::new(arena::RcMemory::new(2));
        let resp_mem = Rc::new(arena::RcMemory::new(2));

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (s_stream_from_conn, r_stream_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (_cancel, token) = CancellationToken::new(&reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();

            stream_fut(
                token,
                sock,
                false,
                false,
                s_from_conn,
                s_stream_from_conn,
                r_to_conn,
            )
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        let req_data = concat!(
            "GET /path HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Upgrade: websocket\r\n",
            "Sec-WebSocket-Version: 13\r\n",
            "Sec-WebSocket-Key: abcde\r\n",
            "\r\n"
        )
        .as_bytes();

        sock.borrow_mut().add_readable(req_data);

        assert_eq!(check_poll(executor.step()), None);

        // read message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "T255:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:multi,4:t",
            "rue!}6:method,3:GET,3:uri,21:ws://example.com/path,7:heade",
            "rs,119:22:4:Host,11:example.com,]22:7:Upgrade,9:websocket,",
            "]30:21:Sec-WebSocket-Version,2:13,]29:17:Sec-WebSocket-Key",
            ",5:abcde,]]7:credits,4:1024#}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let msg = concat!(
            "T98:2:id,1:1,6:reason,19:Switching Protocols,3:seq,1:0#4:f",
            "rom,7:handler,4:code,3:101#7:credits,4:1024#}",
        );

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();
        assert_eq!(data.is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();

        let expected = concat!(
            "HTTP/1.1 101 Switching Protocols\r\n",
            "Upgrade: websocket\r\n",
            "Connection: Upgrade\r\n",
            "Sec-WebSocket-Accept: 8m4i+0BpIKblsbf+VgYANfQKX4w=\r\n",
            "\r\n",
        );

        assert_eq!(str::from_utf8(&data).unwrap(), expected);

        // send message

        let mut data = vec![0; 1024];
        let body = b"hello";
        let size = websocket::write_header(
            true,
            false,
            websocket::OPCODE_TEXT,
            body.len(),
            None,
            &mut data,
        )
        .unwrap();
        data[size..(size + body.len())].copy_from_slice(body);
        let data = &data[..(size + body.len())];

        sock.borrow_mut().add_readable(data);

        assert_eq!(check_poll(executor.step()), None);

        // read message
        let (addr, msg) = r_stream_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        assert_eq!(addr.as_ref(), "handler".as_bytes());

        let buf = &msg[..];

        let expected = concat!(
            "T96:4:from,4:test,2:id,1:1,3:seq,1:1#3:ext,15:5:multi,4:tr",
            "ue!}12:content-type,4:text,4:body,5:hello,}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        // recv message

        let msg = concat!(
            "T99:4:from,7:handler,2:id,1:1,3:seq,1:1#3:ext,15:5:multi,4",
            ":true!}12:content-type,4:text,4:body,5:world,}",
        );

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();

        let fi = websocket::read_header(&data).unwrap();
        assert_eq!(fi.fin, true);
        assert_eq!(fi.opcode, websocket::OPCODE_TEXT);
        assert!(data.len() >= fi.payload_offset + fi.payload_size);

        let content = &data[fi.payload_offset..(fi.payload_offset + fi.payload_size)];
        assert_eq!(str::from_utf8(content).unwrap(), "world");
    }

    #[test]
    fn server_websocket_with_deflate() {
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(2));
        let scratch_mem = Rc::new(arena::RcMemory::new(2));
        let resp_mem = Rc::new(arena::RcMemory::new(2));

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (s_stream_from_conn, r_stream_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());
        let (_cancel, token) = CancellationToken::new(&reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();

            stream_fut(
                token,
                sock,
                false,
                true,
                s_from_conn,
                s_stream_from_conn,
                r_to_conn,
            )
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        let req_data = concat!(
            "GET /path HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Upgrade: websocket\r\n",
            "Sec-WebSocket-Version: 13\r\n",
            "Sec-WebSocket-Key: abcde\r\n",
            "Sec-WebSocket-Extensions: permessage-deflate\r\n",
            "\r\n"
        )
        .as_bytes();

        sock.borrow_mut().add_readable(req_data);

        assert_eq!(check_poll(executor.step()), None);

        // read message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "T308:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:multi,4:t",
            "rue!}6:method,3:GET,3:uri,21:ws://example.com/path,7:heade",
            "rs,173:22:4:Host,11:example.com,]22:7:Upgrade,9:websocket,",
            "]30:21:Sec-WebSocket-Version,2:13,]29:17:Sec-WebSocket-Key",
            ",5:abcde,]50:24:Sec-WebSocket-Extensions,18:permessage-def",
            "late,]]7:credits,3:768#}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let msg = concat!(
            "T98:2:id,1:1,6:reason,19:Switching Protocols,3:seq,1:0#4:f",
            "rom,7:handler,4:code,3:101#7:credits,4:1024#}",
        );

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();
        assert_eq!(data.is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();

        let expected = concat!(
            "HTTP/1.1 101 Switching Protocols\r\n",
            "Upgrade: websocket\r\n",
            "Connection: Upgrade\r\n",
            "Sec-WebSocket-Accept: 8m4i+0BpIKblsbf+VgYANfQKX4w=\r\n",
            "Sec-WebSocket-Extensions: permessage-deflate\r\n",
            "\r\n",
        );

        assert_eq!(str::from_utf8(&data).unwrap(), expected);

        // send message

        let mut data = vec![0; 1024];
        let body = {
            let src = b"hello";
            let mut enc = websocket::DeflateEncoder::new();
            let mut dest = vec![0; 1024];
            let (read, written, output_end) = enc.encode(src, true, &mut dest).unwrap();
            assert_eq!(read, 5);
            assert_eq!(output_end, true);
            dest.truncate(written);

            dest
        };
        let size = websocket::write_header(
            true,
            true,
            websocket::OPCODE_TEXT,
            body.len(),
            None,
            &mut data,
        )
        .unwrap();
        data[size..(size + body.len())].copy_from_slice(&body);
        let data = &data[..(size + body.len())];

        sock.borrow_mut().add_readable(data);

        assert_eq!(check_poll(executor.step()), None);

        // read message
        let (addr, msg) = r_stream_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        assert_eq!(addr.as_ref(), "handler".as_bytes());

        let buf = &msg[..];

        let expected = concat!(
            "T96:4:from,4:test,2:id,1:1,3:seq,1:1#3:ext,15:5:multi,4:tr",
            "ue!}12:content-type,4:text,4:body,5:hello,}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        // recv message

        let msg = concat!(
            "T99:4:from,7:handler,2:id,1:1,3:seq,1:1#3:ext,15:5:multi,4",
            ":true!}12:content-type,4:text,4:body,5:world,}",
        );

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert_eq!(s_to_conn.try_send((resp, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();

        let fi = websocket::read_header(&data).unwrap();
        assert_eq!(fi.fin, true);
        assert_eq!(fi.opcode, websocket::OPCODE_TEXT);
        assert!(data.len() >= fi.payload_offset + fi.payload_size);

        let content = {
            let src = &data[fi.payload_offset..(fi.payload_offset + fi.payload_size)];
            let mut dec = websocket::DeflateDecoder::new();
            let mut dest = vec![0; 1024];
            let (read, written, output_end) = dec.decode(src, true, &mut dest).unwrap();
            assert_eq!(read, src.len());
            assert_eq!(output_end, true);
            dest.truncate(written);

            dest
        };
        assert_eq!(str::from_utf8(&content).unwrap(), "world");
    }

    #[test]
    fn bench_server_req_handler() {
        let t = BenchServerReqHandler::new();
        t.run(&mut t.init());
    }

    #[test]
    fn bench_server_req_connection() {
        let t = BenchServerReqConnection::new();
        t.run(&mut t.init());
    }

    #[test]
    fn bench_server_stream_handler() {
        let t = BenchServerStreamHandler::new();
        t.run(&mut t.init());
    }

    #[test]
    fn bench_server_stream_connection() {
        let t = BenchServerStreamConnection::new();
        t.run(&mut t.init());
    }
}
