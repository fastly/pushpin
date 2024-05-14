/*
 * Copyright (C) 2020-2023 Fanout, Inc.
 * Copyright (C) 2023-2024 Fastly, Inc.
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

#![allow(clippy::collapsible_if)]
#![allow(clippy::collapsible_else_if)]

use crate::arena;
use crate::buffer::{
    Buffer, ContiguousBuffer, LimitBufsMut, TmpBuffer, VecRingBuffer, VECTORED_MAX,
};
use crate::core::http1::Error as CoreHttpError;
use crate::core::http1::{client, server};
use crate::core::http1::{RecvStatus, SendStatus};
use crate::counter::Counter;
use crate::future::{
    io_split, poll_async, select_2, select_3, select_4, select_option, AsyncLocalReceiver,
    AsyncLocalSender, AsyncRead, AsyncReadExt, AsyncResolver, AsyncTcpStream, AsyncTlsStream,
    AsyncWrite, AsyncWriteExt, CancellationToken, ReadHalf, Select2, Select3, Select4,
    StdWriteWrapper, Timeout, TlsWaker, WriteHalf,
};
use crate::http1;
use crate::net::SocketAddr;
use crate::pool::Pool;
use crate::reactor::Reactor;
use crate::resolver;
use crate::shuffle::random;
use crate::tls::{TlsStream, VerifyMode};
use crate::track::{
    self, track_future, Track, TrackFlag, TrackedAsyncLocalReceiver, ValueActiveError,
};
use crate::waker::RefWakerData;
use crate::websocket;
use crate::zhttppacket;
use crate::zmq::MultipartHeader;
use crate::{pin, Defer};
use arrayvec::{ArrayString, ArrayVec};
use ipnet::IpNet;
use log::{debug, log, warn, Level};
use sha1::{Digest, Sha1};
use std::cell::{Ref, RefCell};
use std::cmp;
use std::collections::VecDeque;
use std::convert::TryFrom;
use std::future::Future;
use std::io::{self, Read, Write};
use std::mem;
use std::net::IpAddr;
use std::pin::Pin;
use std::rc::Rc;
use std::str;
use std::str::FromStr;
use std::sync::{mpsc, Arc, Mutex};
use std::task::Context;
use std::task::Poll;
use std::thread;
use std::time::{Duration, Instant};

const URI_SIZE_MAX: usize = 4096;
const HEADERS_MAX: usize = 64;
const WS_HASH_INPUT_MAX: usize = 256;
const WS_KEY_MAX: usize = 24; // base64_encode([16 bytes]) = 24 bytes
const WS_ACCEPT_MAX: usize = 28; // base64_encode(sha1_hash) = 28 bytes
const REDIRECTS_MAX: usize = 8;
const ZHTTP_SESSION_TIMEOUT: Duration = Duration::from_secs(60);
const CONNECTION_POOL_TTL: Duration = Duration::from_secs(55);

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

fn gen_ws_key() -> ArrayString<WS_KEY_MAX> {
    let mut nonce = [0; 16];
    for b in nonce.iter_mut() {
        *b = (random() % 256) as u8;
    }

    let mut output = [0; WS_KEY_MAX];

    let size = base64::encode_config_slice(nonce, base64::STANDARD, &mut output);

    let output = str::from_utf8(&output[..size]).unwrap();

    ArrayString::from_str(output).unwrap()
}

#[allow(clippy::result_unit_err)]
pub fn calculate_ws_accept(key: &[u8]) -> Result<ArrayString<WS_ACCEPT_MAX>, ()> {
    let input_len = key.len() + websocket::WS_GUID.len();

    if input_len > WS_HASH_INPUT_MAX {
        return Err(());
    }

    let mut input = [0; WS_HASH_INPUT_MAX];

    input[..key.len()].copy_from_slice(key);
    input[key.len()..input_len].copy_from_slice(websocket::WS_GUID.as_bytes());

    let input = &input[..input_len];

    let mut hasher = Sha1::new();
    hasher.update(input);
    let digest = hasher.finalize();

    let mut output = [0; WS_ACCEPT_MAX];

    let size = base64::encode_config_slice(digest, base64::STANDARD, &mut output);

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
    {
        if let Some(ws_key) = ws_key {
            return calculate_ws_accept(ws_key);
        }
    }

    Err(())
}

fn validate_ws_response(ws_key: &[u8], ws_accept: Option<&[u8]>) -> Result<(), ()> {
    if let Some(ws_accept) = ws_accept {
        if calculate_ws_accept(ws_key)?.as_bytes() == ws_accept {
            return Ok(());
        }
    }

    Err(())
}

fn gen_mask() -> [u8; 4] {
    let mut out = [0; 4];
    for b in out.iter_mut() {
        *b = (random() % 256) as u8;
    }

    out
}

fn write_ws_ext_header_value<W: Write>(
    config: &websocket::PerMessageDeflateConfig,
    dest: &mut W,
) -> Result<(), io::Error> {
    write!(dest, "permessage-deflate")?;

    config.serialize(dest)
}

#[allow(clippy::too_many_arguments)]
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

    let mut zreq = zhttppacket::Request::new_data(instance.as_bytes(), ids, data);
    zreq.multi = true;

    let size = zreq.serialize(packet_buf)?;

    Ok(zmq::Message::from(&packet_buf[..size]))
}

// return the capacity increase
fn resize_write_buffer_if_full(
    buf: &mut VecRingBuffer,
    block_size: usize,
    blocks_max: usize,
    blocks_avail: &Counter,
) -> usize {
    assert!(blocks_max >= 2);

    // all but one block can be used for writing
    let allowed = blocks_max - 1;

    if buf.remaining_capacity() == 0
        && buf.capacity() < block_size * allowed
        && blocks_avail.dec(1).is_ok()
    {
        buf.resize(buf.capacity() + block_size);

        block_size
    } else {
        0
    }
}

#[derive(Debug)]
enum Error {
    Io(io::Error),
    #[allow(dead_code)]
    Utf8(str::Utf8Error),
    #[allow(dead_code)]
    Http(http1::Error),
    #[allow(dead_code)]
    CoreHttp(CoreHttpError),
    #[allow(dead_code)]
    WebSocket(websocket::Error),
    ReqModeWebSocket,
    InvalidWebSocketRequest,
    InvalidWebSocketResponse,
    #[allow(dead_code)]
    WebSocketRejectionTooLarge(usize),
    Compression,
    BadMessage,
    Handler,
    HandlerCancel,
    BufferExceeded,
    BadFrame,
    BadRequest,
    Tls,
    PolicyViolation,
    TooManyRedirects,
    ValueActive,
    StreamTimeout,
    SessionTimeout,
    Stopped,
}

impl Error {
    // returns true if the error represents a logic error (a bug in the code)
    // that could warrant a panic or high severity log level
    fn is_logical(&self) -> bool {
        matches!(self, Error::ValueActive)
    }

    fn log_level(&self) -> Level {
        if self.is_logical() {
            Level::Error
        } else {
            Level::Debug
        }
    }

    fn to_condition(&self) -> &'static str {
        match self {
            Error::Io(e) if e.kind() == io::ErrorKind::ConnectionRefused => {
                "remote-connection-failed"
            }
            Error::Io(e) if e.kind() == io::ErrorKind::TimedOut => "connection-timeout",
            Error::BadRequest => "bad-request",
            Error::StreamTimeout => "connection-timeout",
            Error::Tls => "tls-error",
            Error::PolicyViolation => "policy-violation",
            Error::TooManyRedirects => "too-many-redirects",
            Error::WebSocketRejectionTooLarge(_) => "rejection-too-large",
            _ => "undefined-condition",
        }
    }
}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Self {
        Self::Io(e)
    }
}

impl From<str::Utf8Error> for Error {
    fn from(e: str::Utf8Error) -> Self {
        Self::Utf8(e)
    }
}

impl<T> From<mpsc::SendError<T>> for Error {
    fn from(_e: mpsc::SendError<T>) -> Self {
        Self::Io(io::Error::from(io::ErrorKind::BrokenPipe))
    }
}

impl<T> From<mpsc::TrySendError<T>> for Error {
    fn from(e: mpsc::TrySendError<T>) -> Self {
        let kind = match e {
            mpsc::TrySendError::Full(_) => io::ErrorKind::WriteZero,
            mpsc::TrySendError::Disconnected(_) => io::ErrorKind::BrokenPipe,
        };

        Self::Io(io::Error::from(kind))
    }
}

impl From<http1::Error> for Error {
    fn from(e: http1::Error) -> Self {
        Self::Http(e)
    }
}

impl From<CoreHttpError> for Error {
    fn from(e: CoreHttpError) -> Self {
        Self::CoreHttp(e)
    }
}

impl From<websocket::Error> for Error {
    fn from(e: websocket::Error) -> Self {
        Self::WebSocket(e)
    }
}

impl From<ValueActiveError> for Error {
    fn from(_e: ValueActiveError) -> Self {
        Self::ValueActive
    }
}

impl From<track::RecvError> for Error {
    fn from(e: track::RecvError) -> Self {
        match e {
            track::RecvError::Disconnected => {
                Self::Io(io::Error::from(io::ErrorKind::UnexpectedEof))
            }
            track::RecvError::ValueActive => Self::ValueActive,
        }
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
        assert!(self.last_partial);

        self.items.back_mut().unwrap().avail += amt;
    }

    fn done(&mut self) {
        self.last_partial = false;
    }

    // type, avail, done
    fn current(&self) -> Option<(u8, usize, bool)> {
        #[allow(clippy::comparison_chain)]
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

pub struct AddrRef<'a> {
    s: Ref<'a, Option<ArrayVec<u8, 64>>>,
}

impl<'a> AddrRef<'a> {
    pub fn get(&self) -> Option<&[u8]> {
        match &*self.s {
            Some(s) => Some(s.as_slice()),
            None => None,
        }
    }
}

struct StreamSharedDataInner {
    to_addr: Option<ArrayVec<u8, 64>>,
    out_seq: u32,
}

pub struct StreamSharedData {
    inner: RefCell<StreamSharedDataInner>,
}

#[allow(clippy::new_without_default)]
impl StreamSharedData {
    pub fn new() -> Self {
        Self {
            inner: RefCell::new(StreamSharedDataInner {
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
            s: Ref::map(self.inner.borrow(), |s| &s.to_addr),
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

fn make_zhttp_req_response(
    id: Option<&[u8]>,
    ptype: zhttppacket::ResponsePacket,
    scratch: &mut [u8],
) -> Result<zmq::Message, io::Error> {
    let mut ids_mem = [zhttppacket::Id { id: b"", seq: None }];

    let ids = if let Some(id) = id {
        ids_mem[0].id = id;

        ids_mem.as_slice()
    } else {
        &[]
    };

    let zresp = zhttppacket::Response {
        from: b"",
        ids,
        multi: false,
        ptype,
        ptype_str: "",
    };

    let size = zresp.serialize(scratch)?;
    let payload = &scratch[..size];

    Ok(zmq::Message::from(payload))
}

fn make_zhttp_response(
    addr: &[u8],
    zresp: zhttppacket::Response,
    scratch: &mut [u8],
) -> Result<zmq::Message, io::Error> {
    let size = zresp.serialize(scratch)?;
    let payload = &scratch[..size];

    let mut v = vec![0; addr.len() + 1 + payload.len()];

    v[..addr.len()].copy_from_slice(addr);
    v[addr.len()] = b' ';
    let pos = addr.len() + 1;
    v[pos..(pos + payload.len())].copy_from_slice(payload);

    // this takes over the vec's memory without copying
    Ok(zmq::Message::from(v))
}

async fn recv_nonzero<R: AsyncRead>(r: &mut R, buf: &mut VecRingBuffer) -> Result<(), io::Error> {
    if buf.remaining_capacity() == 0 {
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

struct WebSocketRead<'a, R: AsyncRead> {
    stream: ReadHalf<'a, R>,
    buf: &'a mut VecRingBuffer,
}

struct WebSocketWrite<'a, W: AsyncWrite> {
    stream: WriteHalf<'a, W>,
    buf: &'a mut VecRingBuffer,
    block_size: usize,
}

struct SendMessageContentFuture<'a, 'b, W: AsyncWrite, M> {
    w: &'a RefCell<WebSocketWrite<'b, W>>,
    protocol: &'a websocket::Protocol<M>,
    avail: usize,
    done: bool,
}

impl<'a, 'b, W: AsyncWrite, M: AsRef<[u8]> + AsMut<[u8]>> Future
    for SendMessageContentFuture<'a, 'b, W, M>
{
    type Output = Result<(usize, bool), Error>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &*self;

        let w = &mut *f.w.borrow_mut();

        let stream = &mut w.stream;

        if !stream.is_writable() {
            return Poll::Pending;
        }

        // protocol.send_message_content may add 1 element to vector
        let mut buf_arr = mem::MaybeUninit::<[&mut [u8]; VECTORED_MAX - 1]>::uninit();
        let mut bufs = w.buf.read_bufs_mut(&mut buf_arr).limit(f.avail);

        match f.protocol.send_message_content(
            &mut StdWriteWrapper::new(Pin::new(&mut w.stream), cx),
            bufs.as_slice(),
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
    w: RefCell<WebSocketWrite<'a, W>>,
    protocol: websocket::Protocol<Vec<u8>>,
}

impl<'a, R: AsyncRead, W: AsyncWrite> WebSocketHandler<'a, R, W> {
    fn new(
        stream: (ReadHalf<'a, R>, WriteHalf<'a, W>),
        buf1: &'a mut VecRingBuffer,
        buf2: &'a mut VecRingBuffer,
        deflate_config: Option<(bool, VecRingBuffer)>,
    ) -> Self {
        buf2.clear();

        let block_size = buf2.capacity();

        Self {
            r: RefCell::new(WebSocketRead {
                stream: stream.0,
                buf: buf1,
            }),
            w: RefCell::new(WebSocketWrite {
                stream: stream.1,
                buf: buf2,
                block_size,
            }),
            protocol: websocket::Protocol::new(deflate_config),
        }
    }

    fn state(&self) -> websocket::State {
        self.protocol.state()
    }

    #[allow(clippy::await_holding_refcell_ref)]
    async fn add_to_recv_buffer(&self) -> Result<(), Error> {
        let r = &mut *self.r.borrow_mut();

        if let Err(e) = recv_nonzero(&mut r.stream, r.buf).await {
            if e.kind() == io::ErrorKind::WriteZero {
                return Err(Error::BufferExceeded);
            }

            return Err(e.into());
        }

        Ok(())
    }

    fn try_recv_message_content(
        &self,
        dest: &mut [u8],
    ) -> Option<Result<(u8, usize, bool), Error>> {
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
        self.w.borrow().buf.remaining_capacity()
    }

    fn accept_body(&self, body: &[u8]) -> Result<(), Error> {
        let w = &mut *self.w.borrow_mut();

        w.buf.write_all(body)?;

        Ok(())
    }

    fn expand_write_buffer(&self, blocks_max: usize, blocks_avail: &Counter) -> usize {
        let w = &mut *self.w.borrow_mut();

        resize_write_buffer_if_full(w.buf, w.block_size, blocks_max, blocks_avail)
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
    ) -> Result<(usize, bool), Error>
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
    shared: &'a StreamSharedData,
}

impl<'a> ZhttpStreamSessionOut<'a> {
    fn new(
        instance_id: &'a str,
        id: &'a str,
        packet_buf: &'a RefCell<Vec<u8>>,
        sender_stream: &'a AsyncLocalSender<(ArrayVec<u8, 64>, zmq::Message)>,
        shared: &'a StreamSharedData,
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

    fn cancel_send(&self) {
        self.sender_stream.cancel();
    }

    // this method is non-blocking, in order to increment the sequence number
    // and send the message in one shot, without concurrent activity
    // interfering with the sequencing. to send asynchronously, first await
    // on check_send and then call this method
    fn try_send_msg(&self, zreq: zhttppacket::Request) -> Result<(), Error> {
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

struct ZhttpServerStreamSessionOut<'a> {
    instance_id: &'a str,
    id: &'a [u8],
    packet_buf: &'a RefCell<Vec<u8>>,
    sender: &'a AsyncLocalSender<zmq::Message>,
    shared: &'a StreamSharedData,
}

impl<'a> ZhttpServerStreamSessionOut<'a> {
    fn new(
        instance_id: &'a str,
        id: &'a [u8],
        packet_buf: &'a RefCell<Vec<u8>>,
        sender: &'a AsyncLocalSender<zmq::Message>,
        shared: &'a StreamSharedData,
    ) -> Self {
        Self {
            instance_id,
            id,
            packet_buf,
            sender,
            shared,
        }
    }

    async fn check_send(&self) {
        self.sender.check_send().await
    }

    fn cancel_send(&self) {
        self.sender.cancel();
    }

    // this method is non-blocking, in order to increment the sequence number
    // and send the message in one shot, without concurrent activity
    // interfering with the sequencing. to send asynchronously, first await
    // on check_send and then call this method
    fn try_send_msg(&self, zresp: zhttppacket::Response) -> Result<(), Error> {
        let msg = {
            let mut zresp = zresp;

            let ids = [zhttppacket::Id {
                id: self.id,
                seq: Some(self.shared.out_seq()),
            }];

            zresp.from = self.instance_id.as_bytes();
            zresp.ids = &ids;
            zresp.multi = true;

            let addr = self.shared.to_addr();
            let addr = addr.get().unwrap();

            let packet_buf = &mut *self.packet_buf.borrow_mut();

            make_zhttp_response(addr, zresp, packet_buf)?
        };

        self.sender.try_send(msg)?;

        self.shared.inc_out_seq();

        Ok(())
    }
}

struct ZhttpStreamSessionIn<'a, 'b, R> {
    id: &'a str,
    send_buf_size: usize,
    websocket: bool,
    receiver: &'a TrackedAsyncLocalReceiver<'b, (arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    shared: &'a StreamSharedData,
    msg_read: &'a R,
    next: Option<(Track<'b, arena::Rc<zhttppacket::OwnedResponse>>, usize)>,
    seq: u32,
    credits: u32,
    first_data: bool,
}

impl<'a, 'b: 'a, R> ZhttpStreamSessionIn<'a, 'b, R>
where
    R: Fn(),
{
    fn new(
        id: &'a str,
        send_buf_size: usize,
        websocket: bool,
        receiver: &'a TrackedAsyncLocalReceiver<'b, (arena::Rc<zhttppacket::OwnedResponse>, usize)>,
        shared: &'a StreamSharedData,
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

    async fn peek_msg(&mut self) -> Result<&arena::Rc<zhttppacket::OwnedResponse>, Error> {
        if self.next.is_none() {
            let (r, id_index) = loop {
                let (r, id_index) = Track::map_first(self.receiver.recv().await?);

                let zresp = r.get().get();

                if zresp.ids[id_index].id != self.id.as_bytes() {
                    // skip messages addressed to old ids
                    continue;
                }

                break (r, id_index);
            };

            let zresp = r.get().get();

            if !zresp.ptype_str.is_empty() {
                debug!(
                    "server-conn {}: handle packet: {}",
                    self.id, zresp.ptype_str
                );
            } else {
                debug!("server-conn {}: handle packet: (data)", self.id);
            }

            if zresp.ids.is_empty() {
                return Err(Error::BadMessage);
            }

            if let Some(seq) = zresp.ids[id_index].seq {
                if seq != self.seq {
                    debug!(
                        "server-conn {}: bad seq (expected {}, got {}), skipping",
                        self.id, self.seq, seq
                    );
                    return Err(Error::BadMessage);
                }

                self.seq += 1;
            }

            let mut addr = ArrayVec::new();
            if addr.try_extend_from_slice(zresp.from).is_err() {
                return Err(Error::BadMessage);
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
                                "server-conn {}: no credits in websocket accept, assuming {}",
                                self.id, credits
                            );
                        }
                    }

                    self.credits += credits;
                }
                zhttppacket::ResponsePacket::Error(edata) => {
                    debug!(
                        "server-conn {}: zhttp error condition={}",
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

    async fn recv_msg(
        &mut self,
    ) -> Result<Track<'b, arena::Rc<zhttppacket::OwnedResponse>>, Error> {
        self.peek_msg().await?;

        Ok(self.next.take().unwrap().0)
    }
}

struct ZhttpServerStreamSessionIn<'a, 'b, R> {
    log_id: &'a str,
    id: &'a [u8],
    receiver: &'a TrackedAsyncLocalReceiver<'b, (arena::Rc<zhttppacket::OwnedRequest>, usize)>,
    shared: &'a StreamSharedData,
    msg_read: &'a R,
    next: Option<(Track<'b, arena::Rc<zhttppacket::OwnedRequest>>, usize)>,
    seq: u32,
    credits: u32,
}

impl<'a, 'b: 'a, R> ZhttpServerStreamSessionIn<'a, 'b, R>
where
    R: Fn(),
{
    fn new(
        log_id: &'a str,
        id: &'a [u8],
        credits: u32,
        receiver: &'a TrackedAsyncLocalReceiver<'b, (arena::Rc<zhttppacket::OwnedRequest>, usize)>,
        shared: &'a StreamSharedData,
        msg_read: &'a R,
    ) -> Self {
        Self {
            log_id,
            id,
            receiver,
            shared,
            msg_read,
            next: None,
            seq: 1,
            credits,
        }
    }

    fn credits(&self) -> u32 {
        self.credits
    }

    fn subtract_credits(&mut self, amount: u32) {
        self.credits -= amount;
    }

    async fn peek_msg(&mut self) -> Result<&arena::Rc<zhttppacket::OwnedRequest>, Error> {
        if self.next.is_none() {
            let (r, id_index) = loop {
                let (r, id_index) = Track::map_first(self.receiver.recv().await?);

                let zreq = r.get().get();

                if zreq.ids[id_index].id != self.id {
                    // skip messages addressed to old ids
                    continue;
                }

                break (r, id_index);
            };

            let zreq = r.get().get();

            if !zreq.ptype_str.is_empty() {
                debug!(
                    "client-conn {}: handle packet: {}",
                    self.log_id, zreq.ptype_str
                );
            } else {
                debug!("client-conn {}: handle packet: (data)", self.log_id);
            }

            if zreq.ids.is_empty() {
                return Err(Error::BadMessage);
            }

            if let Some(seq) = zreq.ids[id_index].seq {
                if seq != self.seq {
                    debug!(
                        "client-conn {}: bad seq (expected {}, got {}), skipping",
                        self.log_id, self.seq, seq
                    );
                    return Err(Error::BadMessage);
                }

                self.seq += 1;
            }

            let mut addr = ArrayVec::new();
            if addr.try_extend_from_slice(zreq.from).is_err() {
                return Err(Error::BadMessage);
            }

            self.shared.set_to_addr(Some(addr));

            (self.msg_read)();

            match &zreq.ptype {
                zhttppacket::RequestPacket::Data(rdata) => {
                    self.credits += rdata.credits;
                }
                zhttppacket::RequestPacket::Error(edata) => {
                    debug!(
                        "client-conn {}: zhttp error condition={}",
                        self.log_id, edata.condition
                    );
                }
                zhttppacket::RequestPacket::Credit(cdata) => {
                    self.credits += cdata.credits;
                }
                zhttppacket::RequestPacket::Ping(pdata) => {
                    self.credits += pdata.credits;
                }
                zhttppacket::RequestPacket::Pong(pdata) => {
                    self.credits += pdata.credits;
                }
                _ => {}
            }

            self.next = Some((r, id_index));
        }

        Ok(&self.next.as_ref().unwrap().0)
    }

    async fn recv_msg(&mut self) -> Result<Track<'b, arena::Rc<zhttppacket::OwnedRequest>>, Error> {
        self.peek_msg().await?;

        Ok(self.next.take().unwrap().0)
    }
}

async fn send_msg(sender: &AsyncLocalSender<zmq::Message>, msg: zmq::Message) -> Result<(), Error> {
    Ok(sender.send(msg).await?)
}

async fn discard_while<F, T, E>(
    receiver: &TrackedAsyncLocalReceiver<'_, (arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    fut: F,
) -> Result<T, Error>
where
    F: Future<Output = Result<T, E>> + Unpin,
    Error: From<E>,
{
    match select_2(fut, pin!(receiver.recv())).await {
        Select2::R1(v) => Ok(v?),
        Select2::R2(ret) => {
            ret?;

            // unexpected message in current state
            Err(Error::BadMessage)
        }
    }
}

async fn server_discard_while<F, T>(
    receiver: &TrackedAsyncLocalReceiver<'_, (arena::Rc<zhttppacket::OwnedRequest>, usize)>,
    fut: F,
) -> F::Output
where
    F: Future<Output = Result<T, Error>> + Unpin,
{
    match select_2(fut, pin!(receiver.recv())).await {
        Select2::R1(v) => v,
        Select2::R2(_) => Err(Error::BadMessage), // unexpected message in current state
    }
}

// read request body and prepare outgoing zmq message
#[allow(clippy::too_many_arguments)]
async fn server_req_read_body<R: AsyncRead, W: AsyncWrite>(
    id: &str,
    req: &http1::Request<'_, '_>,
    req_body: &mut server::RequestBodyKeepHeader<'_, '_, R, W>,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    body_buf: &mut ContiguousBuffer,
    packet_buf: &RefCell<Vec<u8>>,
    zreceiver: &TrackedAsyncLocalReceiver<'_, (arena::Rc<zhttppacket::OwnedResponse>, usize)>,
) -> Result<zmq::Message, Error> {
    // receive request body

    loop {
        match req_body.try_recv(body_buf.write_buf())? {
            RecvStatus::Complete((), written) => {
                body_buf.write_commit(written);
                break;
            }
            RecvStatus::Read((), written) => {
                body_buf.write_commit(written);

                if written == 0 {
                    // ABR: discard_while
                    discard_while(zreceiver, pin!(req_body.add_to_buffer())).await?;
                }
            }
        }
    }

    // determine how to respond

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

        return Err(Error::ReqModeWebSocket);
    }

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
        Buffer::read_buf(body_buf),
        false,
        Mode::HttpReq,
        0,
        peer_addr,
        secure,
        &mut packet_buf.borrow_mut(),
    )?;

    // body consumed
    body_buf.clear();

    Ok(msg)
}

// read full request and prepare outgoing zmq message.
// return Ok(None) if client disconnects before providing a complete request header
async fn server_req_read_header_and_body<R: AsyncRead, W: AsyncWrite>(
    id: &str,
    req_header: server::RequestHeader<'_, '_, R, W>,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    body_buf: &mut ContiguousBuffer,
    packet_buf: &RefCell<Vec<u8>>,
    zreceiver: &TrackedAsyncLocalReceiver<'_, (arena::Rc<zhttppacket::OwnedResponse>, usize)>,
) -> Result<Option<zmq::Message>, Error> {
    let mut scratch = http1::ParseScratch::<HEADERS_MAX>::new();

    // receive request header

    // WARNING: the returned req_header must not be dropped and instead must
    // be consumed by discard_header(). be careful with early returns from
    // this function and do not use the ?-operator
    let (req_header, mut req_body) = {
        // ABR: discard_while
        match discard_while(zreceiver, pin!(req_header.recv(&mut scratch))).await {
            Ok(ret) => ret,
            Err(Error::Io(e)) if e.kind() == io::ErrorKind::UnexpectedEof => return Ok(None),
            Err(e) => return Err(e),
        }
    };

    let req_ref = req_header.get();

    // log request

    {
        let host = get_host(req_ref.headers);
        let scheme = if secure { "https" } else { "http" };

        debug!(
            "server-conn {}: request: {} {}://{}{}",
            id, req_ref.method, scheme, host, req_ref.uri
        );
    }

    let result = server_req_read_body(
        id,
        &req_ref,
        &mut req_body,
        peer_addr,
        secure,
        body_buf,
        packet_buf,
        zreceiver,
    )
    .await;

    // whether success or fail, toss req_header so we are able to respond
    req_body.discard_header(req_header);

    // NOTE: req_header is now consumed and we don't need to worry about it from here

    Ok(Some(result?))
}

struct ReqRespond<'buf, 'st, R: AsyncRead, W: AsyncWrite> {
    header: server::ResponseHeader<'buf, 'st, R, W>,
    prepare_body: server::ResponsePrepareBody<'buf, 'st, R, W>,
}

// consumes resp if successful
#[allow(clippy::too_many_arguments)]
async fn server_req_respond<'buf, 'st, R: AsyncRead, W: AsyncWrite>(
    id: &str,
    req: server::Request,
    resp: &mut Option<server::Response<'buf, R, W>>,
    resp_state: &'st mut server::ResponseState<'buf, R, W>,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    body_buf: &mut ContiguousBuffer,
    packet_buf: &RefCell<Vec<u8>>,
    zsender: &AsyncLocalSender<zmq::Message>,
    zreceiver: &TrackedAsyncLocalReceiver<'_, (arena::Rc<zhttppacket::OwnedResponse>, usize)>,
) -> Result<Option<ReqRespond<'buf, 'st, R, W>>, Error> {
    let msg = {
        let req_header = req.recv_header(resp.as_mut().unwrap());

        match server_req_read_header_and_body(
            id, req_header, peer_addr, secure, body_buf, packet_buf, zreceiver,
        )
        .await?
        {
            Some(msg) => msg,
            None => return Ok(None),
        }
    };

    // send message

    // ABR: discard_while
    discard_while(zreceiver, pin!(send_msg(zsender, msg))).await?;

    // receive message

    let zresp = loop {
        // ABR: direct read
        let (zresp, id_index) = Track::map_first(zreceiver.recv().await?);

        let zresp_ref = zresp.get().get();

        if zresp_ref.ids[id_index].id != id.as_bytes() {
            // skip messages addressed to old ids
            continue;
        }

        if !zresp_ref.ptype_str.is_empty() {
            debug!("server-conn {}: handle packet: {}", id, zresp_ref.ptype_str);
        } else {
            debug!("server-conn {}: handle packet: (data)", id);
        }

        // skip non-data messages

        match &zresp_ref.ptype {
            zhttppacket::ResponsePacket::Data(_) => break zresp,
            _ => debug!(
                "server-conn {}: unexpected packet in req mode: {}",
                id, zresp_ref.ptype_str
            ),
        }
    };

    let (header, prepare_body) = {
        let zresp = zresp.get().get();

        let rdata = match &zresp.ptype {
            zhttppacket::ResponsePacket::Data(rdata) => rdata,
            _ => unreachable!(), // we confirmed the type above
        };

        if body_buf.write_all(rdata.body).is_err() {
            return Err(Error::BufferExceeded);
        }

        // send response header

        let mut headers = [http1::EMPTY_HEADER; HEADERS_MAX];
        let mut headers_len = 0;

        for h in rdata.headers.iter() {
            if headers_len >= headers.len() {
                return Err(Error::BadMessage);
            }

            headers[headers_len] = http1::Header {
                name: h.name,
                value: h.value,
            };

            headers_len += 1;
        }

        let headers = &headers[..headers_len];

        let mut resp_take = resp.take().unwrap();

        let (header, prepare_body) = match resp_take.prepare_header(
            rdata.code,
            rdata.reason,
            headers,
            http1::BodySize::Known(rdata.body.len()),
            resp_state,
        ) {
            Ok(ret) => ret,
            Err(e) => {
                *resp = Some(resp_take);
                return Err(e.into());
            }
        };

        (header, prepare_body)
    };

    Ok(Some(ReqRespond {
        header,
        prepare_body,
    }))
}

// return true if persistent
#[allow(clippy::too_many_arguments)]
async fn server_req_handler<S: AsyncRead + AsyncWrite>(
    id: &str,
    stream: &mut S,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    buf1: &mut VecRingBuffer,
    buf2: &mut VecRingBuffer,
    body_buf: &mut ContiguousBuffer,
    packet_buf: &RefCell<Vec<u8>>,
    zsender: &AsyncLocalSender<zmq::Message>,
    zreceiver: &TrackedAsyncLocalReceiver<'_, (arena::Rc<zhttppacket::OwnedResponse>, usize)>,
) -> Result<bool, Error> {
    let stream = RefCell::new(stream);

    let mut resp_state = server::ResponseState::default();

    let r = {
        let (req, resp) = server::Request::new(io_split(&stream), buf1, buf2);
        let mut resp = Some(resp);

        let ret = match server_req_respond(
            id,
            req,
            &mut resp,
            &mut resp_state,
            peer_addr,
            secure,
            body_buf,
            packet_buf,
            zsender,
            zreceiver,
        )
        .await
        {
            Ok(Some(ret)) => ret,
            Ok(None) => return Ok(false), // no request
            Err(e) => return Err(e),
        };

        assert!(resp.is_none());

        ret
    };

    // ABR: discard_while
    let header_sent = discard_while(zreceiver, pin!(r.header.send())).await?;

    let resp_body = header_sent.start_body(r.prepare_body);

    // send response body

    let finished = loop {
        // fill the buffer as much as possible
        let size = resp_body.prepare(Buffer::read_buf(body_buf), true)?;
        body_buf.read_commit(size);

        // send the buffer
        let send = pin!(async {
            match resp_body.send().await {
                SendStatus::Complete(finished) => Ok(Some(finished)),
                SendStatus::EarlyResponse(_) => unreachable!(), // for requests only
                SendStatus::Partial((), _) => Ok(None),
                SendStatus::Error((), e) => Err(e),
            }
        });

        // ABR: discard_while
        if let Some(finished) = discard_while(zreceiver, send).await? {
            break finished;
        }
    };

    assert_eq!(body_buf.len(), 0);

    Ok(finished.is_persistent())
}

#[allow(clippy::too_many_arguments)]
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
    zreceiver: &TrackedAsyncLocalReceiver<'_, (arena::Rc<zhttppacket::OwnedResponse>, usize)>,
) -> Result<(), Error> {
    let reactor = Reactor::current().unwrap();

    let mut buf1 = VecRingBuffer::new(buffer_size, rb_tmp);
    let mut buf2 = VecRingBuffer::new(buffer_size, rb_tmp);
    let mut body_buf = ContiguousBuffer::new(body_buffer_size);

    loop {
        stream.set_id(cid);

        // this was originally logged when starting the non-async state
        // machine, so we'll keep doing that
        debug!("server-conn {}: assigning id", cid);

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
                Select3::R2(_) => return Err(Error::StreamTimeout),
                Select3::R3(_) => return Err(Error::Stopped),
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

    // ABR: discard_while
    discard_while(zreceiver, pin!(stream.close())).await?;

    Ok(())
}

#[allow(clippy::too_many_arguments)]
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
    let value_active = TrackFlag::default();

    let zreceiver = TrackedAsyncLocalReceiver::new(zreceiver, &value_active);

    match track_future(
        server_req_connection_inner(
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
        ),
        &value_active,
    )
    .await
    {
        Ok(()) => debug!("server-conn {}: finished", cid),
        Err(e) => log!(e.log_level(), "server-conn {}: process error: {:?}", cid, e),
    }
}

async fn accept_handoff<R>(
    zsess_in: &mut ZhttpStreamSessionIn<'_, '_, R>,
    zsess_out: &ZhttpStreamSessionOut<'_>,
) -> Result<(), Error>
where
    R: Fn(),
{
    // discarding here is fine. the sender should cease sending
    // messages until we've replied with proceed
    discard_while(
        zsess_in.receiver,
        pin!(async {
            zsess_out.check_send().await;
            Ok::<(), Error>(())
        }),
    )
    .await?;

    let zreq = zhttppacket::Request::new_handoff_proceed(b"", &[]);

    // check_send just finished, so this should succeed
    zsess_out.try_send_msg(zreq)?;

    // pause until we get a msg
    zsess_in.peek_msg().await?;

    Ok(())
}

async fn server_accept_handoff<R>(
    zsess_in: &mut ZhttpServerStreamSessionIn<'_, '_, R>,
    zsess_out: &ZhttpServerStreamSessionOut<'_>,
) -> Result<(), Error>
where
    R: Fn(),
{
    // discarding here is fine. the sender should cease sending
    // messages until we've replied with proceed
    server_discard_while(
        zsess_in.receiver,
        pin!(async {
            zsess_out.check_send().await;
            Ok(())
        }),
    )
    .await?;

    let zresp = zhttppacket::Response::new_handoff_proceed(b"", &[]);

    // check_send just finished, so this should succeed
    zsess_out.try_send_msg(zresp)?;

    // pause until we get a msg
    zsess_in.peek_msg().await?;

    Ok(())
}

// this function will either return immediately or await messages
async fn handle_other<R>(
    zresp: Track<'_, arena::Rc<zhttppacket::OwnedResponse>>,
    zsess_in: &mut ZhttpStreamSessionIn<'_, '_, R>,
    zsess_out: &ZhttpStreamSessionOut<'_>,
) -> Result<(), Error>
where
    R: Fn(),
{
    match &zresp.get().get().ptype {
        zhttppacket::ResponsePacket::KeepAlive => Ok(()),
        zhttppacket::ResponsePacket::Credit(_) => Ok(()),
        zhttppacket::ResponsePacket::HandoffStart => {
            drop(zresp);

            accept_handoff(zsess_in, zsess_out).await?;

            Ok(())
        }
        zhttppacket::ResponsePacket::Error(_) => Err(Error::Handler),
        zhttppacket::ResponsePacket::Cancel => Err(Error::HandlerCancel),
        _ => Err(Error::BadMessage), // unexpected type
    }
}

// this function will either return immediately or await messages
async fn server_handle_other<R>(
    zreq: Track<'_, arena::Rc<zhttppacket::OwnedRequest>>,
    zsess_in: &mut ZhttpServerStreamSessionIn<'_, '_, R>,
    zsess_out: &ZhttpServerStreamSessionOut<'_>,
) -> Result<(), Error>
where
    R: Fn(),
{
    match &zreq.get().get().ptype {
        zhttppacket::RequestPacket::KeepAlive => Ok(()),
        zhttppacket::RequestPacket::Credit(_) => Ok(()),
        zhttppacket::RequestPacket::HandoffStart => {
            drop(zreq);

            server_accept_handoff(zsess_in, zsess_out).await?;

            Ok(())
        }
        zhttppacket::RequestPacket::Error(_) => Err(Error::Handler),
        zhttppacket::RequestPacket::Cancel => Err(Error::HandlerCancel),
        _ => Err(Error::BadMessage), // unexpected type
    }
}

async fn stream_recv_body<R1, R2, R, W>(
    tmp_buf: &RefCell<Vec<u8>>,
    bytes_read: &R1,
    req_body: server::RequestBody<'_, '_, R, W>,
    zsess_in: &mut ZhttpStreamSessionIn<'_, '_, R2>,
    zsess_out: &ZhttpStreamSessionOut<'_>,
) -> Result<(), Error>
where
    R1: Fn(),
    R2: Fn(),
    R: AsyncRead,
    W: AsyncWrite,
{
    let mut check_send = pin!(None);
    let mut add_to_buffer = pin!(None);

    loop {
        if zsess_in.credits() > 0 && add_to_buffer.is_none() && check_send.is_none() {
            check_send.set(Some(zsess_out.check_send()));
        }

        // ABR: select contains read
        let ret = select_3(
            select_option(check_send.as_mut().as_pin_mut()),
            select_option(add_to_buffer.as_mut().as_pin_mut()),
            pin!(zsess_in.peek_msg()),
        )
        .await;

        match ret {
            Select3::R1(()) => {
                check_send.set(None);

                let _defer = Defer::new(|| zsess_out.cancel_send());

                assert!(zsess_in.credits() > 0);
                assert!(add_to_buffer.is_none());

                let tmp_buf = &mut *tmp_buf.borrow_mut();
                let max_read = cmp::min(tmp_buf.len(), zsess_in.credits() as usize);

                let (size, done) = match req_body.try_recv(&mut tmp_buf[..max_read])? {
                    RecvStatus::Complete((), written) => (written, true),
                    RecvStatus::Read((), written) => {
                        if written == 0 {
                            add_to_buffer.set(Some(req_body.add_to_buffer()));
                            continue;
                        }

                        (written, false)
                    }
                };

                bytes_read();

                let body = &tmp_buf[..size];

                zsess_in.subtract_credits(size as u32);

                let mut rdata = zhttppacket::RequestData::new();
                rdata.body = body;
                rdata.more = !done;

                let zresp = zhttppacket::Request::new_data(b"", &[], rdata);

                // check_send just finished, so this should succeed
                zsess_out.try_send_msg(zresp)?;

                if done {
                    break;
                }
            }
            Select3::R2(ret) => {
                ret?;

                add_to_buffer.set(None);
            }
            Select3::R3(ret) => {
                let r = ret?;

                let zresp_ref = r.get().get();

                match &zresp_ref.ptype {
                    zhttppacket::ResponsePacket::Data(_) => break,
                    _ => {
                        // ABR: direct read
                        let zresp = zsess_in.recv_msg().await?;

                        // ABR: handle_other
                        handle_other(zresp, zsess_in, zsess_out).await?;
                    }
                }
            }
        }
    }

    Ok(())
}

async fn server_stream_recv_body<R1, R2, R>(
    tmp_buf: &RefCell<Vec<u8>>,
    bytes_read: &R1,
    resp_body: client::ResponseBody<'_, R>,
    zsess_in: &mut ZhttpServerStreamSessionIn<'_, '_, R2>,
    zsess_out: &ZhttpServerStreamSessionOut<'_>,
) -> Result<client::Finished, Error>
where
    R1: Fn(),
    R2: Fn(),
    R: AsyncRead,
{
    let mut check_send = pin!(None);
    let mut add_to_buffer = pin!(None);

    loop {
        if zsess_in.credits() > 0 && add_to_buffer.is_none() && check_send.is_none() {
            check_send.set(Some(zsess_out.check_send()));
        }

        // ABR: select contains read
        let ret = select_3(
            select_option(check_send.as_mut().as_pin_mut()),
            select_option(add_to_buffer.as_mut().as_pin_mut()),
            pin!(zsess_in.recv_msg()),
        )
        .await;

        match ret {
            Select3::R1(()) => {
                check_send.set(None);

                let _defer = Defer::new(|| zsess_out.cancel_send());

                assert!(zsess_in.credits() > 0);
                assert!(add_to_buffer.is_none());

                let tmp_buf = &mut *tmp_buf.borrow_mut();
                let max_read = cmp::min(tmp_buf.len(), zsess_in.credits() as usize);

                let (size, mut finished) = match resp_body.try_recv(&mut tmp_buf[..max_read])? {
                    RecvStatus::Complete(finished, written) => (written, Some(finished)),
                    RecvStatus::Read((), written) => {
                        if written == 0 {
                            add_to_buffer.set(Some(resp_body.add_to_buffer()));
                            continue;
                        }

                        (written, None)
                    }
                };

                bytes_read();

                let body = &tmp_buf[..size];

                zsess_in.subtract_credits(size as u32);

                let mut rdata = zhttppacket::ResponseData::new();
                rdata.body = body;
                rdata.more = finished.is_none();

                let zresp = zhttppacket::Response::new_data(b"", &[], rdata);

                // check_send just finished, so this should succeed
                zsess_out.try_send_msg(zresp)?;

                if let Some(finished) = finished.take() {
                    return Ok(finished);
                }
            }
            Select3::R2(ret) => {
                ret?;

                add_to_buffer.set(None);
            }
            Select3::R3(ret) => {
                let zreq = ret?;

                // ABR: handle_other
                server_handle_other(zreq, zsess_in, zsess_out).await?;
            }
        }
    }
}

async fn stream_send_body<R1, R2, R, W>(
    bytes_read: &R1,
    resp_body: server::ResponseBody<'_, R, W>,
    zsess_in: &mut ZhttpStreamSessionIn<'_, '_, R2>,
    zsess_out: &ZhttpStreamSessionOut<'_>,
    blocks_max: usize,
    blocks_avail: &Counter,
) -> Result<server::Finished, Error>
where
    R1: Fn(),
    R2: Fn(),
    R: AsyncRead,
    W: AsyncWrite,
{
    let mut out_credits = 0;

    let mut send = pin!(None);
    let mut check_send = pin!(None);

    let mut prepare_done = false;

    let finished = 'main: loop {
        let ret = {
            if send.is_none() && resp_body.can_send() {
                send.set(Some(resp_body.send()));
            }

            if !prepare_done && out_credits > 0 && check_send.is_none() {
                check_send.set(Some(zsess_out.check_send()));
            }

            // ABR: select contains read
            select_3(
                select_option(send.as_mut().as_pin_mut()),
                select_option(check_send.as_mut().as_pin_mut()),
                pin!(zsess_in.recv_msg()),
            )
            .await
        };

        match ret {
            Select3::R1(ret) => {
                send.set(None);

                match ret {
                    SendStatus::Complete(finished) => break finished,
                    SendStatus::EarlyResponse(_) => unreachable!(), // for requests only
                    SendStatus::Partial((), size) => {
                        out_credits += size as u32;

                        if size > 0 {
                            bytes_read();
                        }
                    }
                    SendStatus::Error(_, e) => return Err(e.into()),
                }
            }
            Select3::R2(()) => {
                check_send.set(None);

                let zreq = zhttppacket::Request::new_credit(b"", &[], out_credits);
                out_credits = 0;

                // check_send just finished, so this should succeed
                zsess_out.try_send_msg(zreq)?;
            }
            Select3::R3(ret) => {
                let zresp = ret?;

                match &zresp.get().get().ptype {
                    zhttppacket::ResponsePacket::Data(rdata) => {
                        let size = resp_body.prepare(rdata.body, !rdata.more)?;

                        if size < rdata.body.len() {
                            return Err(Error::BufferExceeded);
                        }

                        if rdata.more {
                            out_credits += resp_body
                                .expand_write_buffer(blocks_max, || blocks_avail.dec(1).is_ok())?
                                as u32;
                        } else {
                            prepare_done = true;
                        }
                    }
                    zhttppacket::ResponsePacket::HandoffStart => {
                        drop(zresp);

                        // if handoff requested, flush what we can before accepting
                        // so that the data is not delayed while we wait

                        if send.is_none() && resp_body.can_send() {
                            send.set(Some(resp_body.send()));
                        }

                        while let Some(fut) = send.as_mut().as_pin_mut() {
                            // ABR: poll_async doesn't block
                            let ret = match poll_async(fut).await {
                                Poll::Ready(ret) => ret,
                                Poll::Pending => break,
                            };

                            send.set(None);

                            match ret {
                                SendStatus::Complete(resp) => break 'main resp,
                                SendStatus::EarlyResponse(_) => unreachable!(), // for requests only
                                SendStatus::Partial((), size) => {
                                    out_credits += size as u32;

                                    if size > 0 {
                                        bytes_read();
                                    }
                                }
                                SendStatus::Error((), e) => return Err(e.into()),
                            }

                            if resp_body.can_send() {
                                send.set(Some(resp_body.send()));
                            }
                        }

                        // ABR: function contains read
                        accept_handoff(zsess_in, zsess_out).await?;
                    }
                    _ => {
                        // ABR: handle_other
                        handle_other(zresp, zsess_in, zsess_out).await?;
                    }
                }
            }
        }
    };

    Ok(finished)
}

struct Overflow {
    buf: ContiguousBuffer,
    end: bool,
}

#[allow(clippy::too_many_arguments)]
async fn server_stream_send_body<'a, R1, R2, R, W>(
    bytes_read: &R1,
    req_body: client::RequestBody<'a, R, W>,
    mut overflow: Option<Overflow>,
    recv_buf_size: usize,
    zsess_in: &mut ZhttpServerStreamSessionIn<'_, '_, R2>,
    zsess_out: &ZhttpServerStreamSessionOut<'_>,
    blocks_max: usize,
    blocks_avail: &Counter,
) -> Result<client::Response<'a, R>, Error>
where
    R1: Fn(),
    R2: Fn(),
    R: AsyncRead,
    W: AsyncWrite,
{
    // send initial body, including overflow, before offering credits

    let mut send = pin!(None);

    while send.is_some() || req_body.can_send() {
        if send.is_none() {
            send.set(Some(req_body.send()));
        }

        // ABR: select contains read
        let result = select_2(
            select_option(send.as_mut().as_pin_mut()),
            pin!(zsess_in.recv_msg()),
        )
        .await;

        match result {
            Select2::R1(ret) => {
                send.set(None);

                match ret {
                    SendStatus::Complete(resp) => return Ok(resp),
                    SendStatus::EarlyResponse(resp) => return Ok(resp),
                    SendStatus::Partial((), _) => {
                        if !req_body.can_send() {
                            if let Some(overflow) = &mut overflow {
                                let size =
                                    req_body.prepare(overflow.buf.read_buf(), overflow.end)?;
                                overflow.buf.read_commit(size);
                            }
                        }
                    }
                    SendStatus::Error((), e) => return Err(e.into()),
                }
            }
            Select2::R2(ret) => {
                let zreq = ret?;

                // ABR: handle_other
                server_handle_other(zreq, zsess_in, zsess_out).await?;
            }
        }
    }

    assert!(!req_body.can_send());

    let mut out_credits = recv_buf_size as u32;

    let mut send = pin!(None);
    let mut check_send = pin!(None);

    let mut prepare_done = false;

    let resp = 'main: loop {
        let ret = {
            if send.is_none() && req_body.can_send() {
                send.set(Some(req_body.send()));
            }

            if !prepare_done && out_credits > 0 && check_send.is_none() {
                check_send.set(Some(zsess_out.check_send()));
            }

            // ABR: select contains read
            select_3(
                select_option(send.as_mut().as_pin_mut()),
                select_option(check_send.as_mut().as_pin_mut()),
                pin!(zsess_in.recv_msg()),
            )
            .await
        };

        match ret {
            Select3::R1(ret) => {
                send.set(None);

                match ret {
                    SendStatus::Complete(resp) => break resp,
                    SendStatus::EarlyResponse(resp) => break resp,
                    SendStatus::Partial((), size) => {
                        out_credits += size as u32;

                        if size > 0 {
                            bytes_read();
                        }
                    }
                    SendStatus::Error(_, e) => return Err(e.into()),
                }
            }
            Select3::R2(()) => {
                check_send.set(None);

                let zresp = zhttppacket::Response::new_credit(b"", &[], out_credits);
                out_credits = 0;

                // check_send just finished, so this should succeed
                zsess_out.try_send_msg(zresp)?;
            }
            Select3::R3(ret) => {
                let zreq = ret?;

                match &zreq.get().get().ptype {
                    zhttppacket::RequestPacket::Data(rdata) => {
                        let size = req_body.prepare(rdata.body, !rdata.more)?;

                        if size < rdata.body.len() {
                            return Err(Error::BufferExceeded);
                        }

                        if rdata.more {
                            out_credits += req_body
                                .expand_write_buffer(blocks_max, || blocks_avail.dec(1).is_ok())?
                                as u32;
                        } else {
                            prepare_done = true;
                        }
                    }
                    zhttppacket::RequestPacket::HandoffStart => {
                        drop(zreq);

                        // if handoff requested, flush what we can before accepting
                        // so that the data is not delayed while we wait

                        if send.is_none() && req_body.can_send() {
                            send.set(Some(req_body.send()));
                        }

                        while let Some(fut) = send.as_mut().as_pin_mut() {
                            // ABR: poll_async doesn't block
                            let ret = match poll_async(fut).await {
                                Poll::Ready(ret) => ret,
                                Poll::Pending => break,
                            };

                            send.set(None);

                            match ret {
                                SendStatus::Complete(resp) => break 'main resp,
                                SendStatus::EarlyResponse(resp) => break 'main resp,
                                SendStatus::Partial((), size) => {
                                    out_credits += size as u32;

                                    if size > 0 {
                                        bytes_read();
                                    }
                                }
                                SendStatus::Error((), e) => return Err(e.into()),
                            }

                            if req_body.can_send() {
                                send.set(Some(req_body.send()));
                            }
                        }

                        // ABR: function contains read
                        server_accept_handoff(zsess_in, zsess_out).await?;
                    }
                    _ => {
                        // ABR: handle_other
                        server_handle_other(zreq, zsess_in, zsess_out).await?;
                    }
                }
            }
        }
    };

    Ok(resp)
}

#[allow(clippy::too_many_arguments)]
async fn stream_websocket<S, R1, R2>(
    log_id: &str,
    stream: RefCell<&mut S>,
    buf1: &mut VecRingBuffer,
    buf2: &mut VecRingBuffer,
    blocks_max: usize,
    blocks_avail: &Counter,
    messages_max: usize,
    tmp_buf: &RefCell<Vec<u8>>,
    bytes_read: &R1,
    deflate_config: Option<(websocket::PerMessageDeflateConfig, usize)>,
    zsess_in: &mut ZhttpStreamSessionIn<'_, '_, R2>,
    zsess_out: &ZhttpStreamSessionOut<'_>,
) -> Result<(), Error>
where
    S: AsyncRead + AsyncWrite,
    R1: Fn(),
    R2: Fn(),
{
    let deflate_config = match deflate_config {
        Some((config, enc_buf_size)) => {
            let ebuf = VecRingBuffer::new(enc_buf_size, buf2.get_tmp());

            Some((!config.server_no_context_takeover, ebuf))
        }
        None => None,
    };

    let handler = WebSocketHandler::new(io_split(&stream), buf1, buf2, deflate_config);
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

                let _defer = Defer::new(|| zsess_out.cancel_send());

                if out_credits > 0 {
                    let zreq = zhttppacket::Request::new_credit(b"", &[], out_credits);
                    out_credits = 0;

                    // check_send just finished, so this should succeed
                    zsess_out.try_send_msg(zreq)?;
                    continue;
                }

                assert!(zsess_in.credits() > 0);
                assert!(add_to_recv_buffer.is_none());

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
                        debug!(
                            "server-conn {}: unsupported websocket opcode: {}",
                            log_id, opcode
                        );
                        return Err(Error::BadFrame);
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
                let zresp = ret?;

                match &zresp.get().get().ptype {
                    zhttppacket::ResponsePacket::Data(rdata) => match handler.state() {
                        websocket::State::Connected | websocket::State::PeerClosed => {
                            let avail = handler.accept_avail();

                            if let Err(e) = handler.accept_body(rdata.body) {
                                warn!(
                                    "received too much data from handler (size={}, credits={})",
                                    rdata.body.len(),
                                    avail,
                                );

                                return Err(e);
                            }

                            out_credits +=
                                handler.expand_write_buffer(blocks_max, blocks_avail) as u32;

                            let opcode = match &rdata.content_type {
                                Some(zhttppacket::ContentType::Binary) => websocket::OPCODE_BINARY,
                                _ => websocket::OPCODE_TEXT,
                            };

                            if !ws_in_tracker.in_progress() {
                                if ws_in_tracker.start(opcode).is_err() {
                                    return Err(Error::BufferExceeded);
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
                            let (code, reason) = cdata.status.unwrap_or((1000, ""));

                            let arr: [u8; 2] = code.to_be_bytes();

                            // close content isn't limited by credits. if we
                            // don't have space for it, just error out
                            handler.accept_body(&arr)?;
                            handler.accept_body(reason.as_bytes())?;

                            if ws_in_tracker.start(websocket::OPCODE_CLOSE).is_err() {
                                return Err(Error::BadFrame);
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

                                return Err(e);
                            }

                            if ws_in_tracker.start(websocket::OPCODE_PING).is_err() {
                                return Err(Error::BadFrame);
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

                                return Err(e);
                            }

                            if ws_in_tracker.start(websocket::OPCODE_PONG).is_err() {
                                return Err(Error::BadFrame);
                            }

                            ws_in_tracker.extend(pdata.body.len());
                            ws_in_tracker.done();
                        }
                        _ => {}
                    },
                    zhttppacket::ResponsePacket::HandoffStart => {
                        drop(zresp);

                        // if handoff requested, flush what we can before accepting
                        // so that the data is not delayed while we wait
                        loop {
                            if send_content.is_none() {
                                if let Some((mtype, avail, done)) = ws_in_tracker.current() {
                                    if !handler.is_sending_message() {
                                        handler.send_message_start(mtype, None);
                                    }

                                    if avail > 0 || done {
                                        send_content.set(Some(
                                            handler.send_message_content(avail, done, bytes_read),
                                        ));
                                    }
                                }
                            }

                            if let Some(fut) = send_content.as_mut().as_pin_mut() {
                                // ABR: poll_async doesn't block
                                let ret = match poll_async(fut).await {
                                    Poll::Ready(ret) => ret,
                                    Poll::Pending => break,
                                };

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
                        }

                        // ABR: function contains read
                        accept_handoff(zsess_in, zsess_out).await?;
                    }
                    _ => {
                        // ABR: handle_other
                        handle_other(zresp, zsess_in, zsess_out).await?;
                    }
                }
            }
        }
    }

    Ok(())
}

#[allow(clippy::too_many_arguments)]
async fn server_stream_websocket<S, R1, R2>(
    log_id: &str,
    stream: RefCell<&mut S>,
    buf1: &mut VecRingBuffer,
    buf2: &mut VecRingBuffer,
    blocks_max: usize,
    blocks_avail: &Counter,
    messages_max: usize,
    tmp_buf: &RefCell<Vec<u8>>,
    bytes_read: &R1,
    deflate_config: Option<(websocket::PerMessageDeflateConfig, usize)>,
    zsess_in: &mut ZhttpServerStreamSessionIn<'_, '_, R2>,
    zsess_out: &ZhttpServerStreamSessionOut<'_>,
) -> Result<(), Error>
where
    S: AsyncRead + AsyncWrite,
    R1: Fn(),
    R2: Fn(),
{
    let deflate_config = match deflate_config {
        Some((config, enc_buf_size)) => {
            let ebuf = VecRingBuffer::new(enc_buf_size, buf2.get_tmp());

            Some((!config.client_no_context_takeover, ebuf))
        }
        None => None,
    };

    let handler = WebSocketHandler::new(io_split(&stream), buf1, buf2, deflate_config);
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
                    handler.send_message_start(mtype, Some(gen_mask()));
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

                let _defer = Defer::new(|| zsess_out.cancel_send());

                if out_credits > 0 {
                    let zresp = zhttppacket::Response::new_credit(b"", &[], out_credits);
                    out_credits = 0;

                    // check_send just finished, so this should succeed
                    zsess_out.try_send_msg(zresp)?;
                    continue;
                }

                assert!(zsess_in.credits() > 0);
                assert!(add_to_recv_buffer.is_none());

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

                let zresp = match opcode {
                    websocket::OPCODE_TEXT | websocket::OPCODE_BINARY => {
                        if body.is_empty() && !end {
                            // don't bother sending empty message
                            continue;
                        }

                        let mut data = zhttppacket::ResponseData::new();

                        data.body = body;

                        data.content_type = if opcode == websocket::OPCODE_TEXT {
                            Some(zhttppacket::ContentType::Text)
                        } else {
                            Some(zhttppacket::ContentType::Binary)
                        };

                        data.more = !end;

                        zhttppacket::Response::new_data(b"", &[], data)
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

                        zhttppacket::Response::new_close(b"", &[], status)
                    }
                    websocket::OPCODE_PING => zhttppacket::Response::new_ping(b"", &[], body),
                    websocket::OPCODE_PONG => zhttppacket::Response::new_pong(b"", &[], body),
                    opcode => {
                        debug!(
                            "client-conn {}: unsupported websocket opcode: {}",
                            log_id, opcode
                        );
                        return Err(Error::BadFrame);
                    }
                };

                zsess_in.subtract_credits(size as u32);

                // check_send just finished, so this should succeed
                zsess_out.try_send_msg(zresp)?;
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
                let zreq = ret?;

                match &zreq.get().get().ptype {
                    zhttppacket::RequestPacket::Data(rdata) => match handler.state() {
                        websocket::State::Connected | websocket::State::PeerClosed => {
                            let avail = handler.accept_avail();

                            if let Err(e) = handler.accept_body(rdata.body) {
                                warn!(
                                    "received too much data from handler (size={}, credits={})",
                                    rdata.body.len(),
                                    avail,
                                );

                                return Err(e);
                            }

                            out_credits +=
                                handler.expand_write_buffer(blocks_max, blocks_avail) as u32;

                            let opcode = match &rdata.content_type {
                                Some(zhttppacket::ContentType::Binary) => websocket::OPCODE_BINARY,
                                _ => websocket::OPCODE_TEXT,
                            };

                            if !ws_in_tracker.in_progress() {
                                if ws_in_tracker.start(opcode).is_err() {
                                    return Err(Error::BufferExceeded);
                                }
                            }

                            ws_in_tracker.extend(rdata.body.len());

                            if !rdata.more {
                                ws_in_tracker.done();
                            }
                        }
                        _ => {}
                    },
                    zhttppacket::RequestPacket::Close(cdata) => match handler.state() {
                        websocket::State::Connected | websocket::State::PeerClosed => {
                            let (code, reason) = cdata.status.unwrap_or((1000, ""));

                            let arr: [u8; 2] = code.to_be_bytes();

                            // close content isn't limited by credits. if we
                            // don't have space for it, just error out
                            handler.accept_body(&arr)?;
                            handler.accept_body(reason.as_bytes())?;

                            if ws_in_tracker.start(websocket::OPCODE_CLOSE).is_err() {
                                return Err(Error::BadFrame);
                            }

                            ws_in_tracker.extend(arr.len() + reason.len());
                            ws_in_tracker.done();
                        }
                        _ => {}
                    },
                    zhttppacket::RequestPacket::Ping(pdata) => match handler.state() {
                        websocket::State::Connected | websocket::State::PeerClosed => {
                            let avail = handler.accept_avail();

                            if let Err(e) = handler.accept_body(pdata.body) {
                                warn!(
                                    "received too much data from handler (size={}, credits={})",
                                    pdata.body.len(),
                                    avail,
                                );

                                return Err(e);
                            }

                            if ws_in_tracker.start(websocket::OPCODE_PING).is_err() {
                                return Err(Error::BadFrame);
                            }

                            ws_in_tracker.extend(pdata.body.len());
                            ws_in_tracker.done();
                        }
                        _ => {}
                    },
                    zhttppacket::RequestPacket::Pong(pdata) => match handler.state() {
                        websocket::State::Connected | websocket::State::PeerClosed => {
                            let avail = handler.accept_avail();

                            if let Err(e) = handler.accept_body(pdata.body) {
                                warn!(
                                    "received too much data from handler (size={}, credits={})",
                                    pdata.body.len(),
                                    avail,
                                );

                                return Err(e);
                            }

                            if ws_in_tracker.start(websocket::OPCODE_PONG).is_err() {
                                return Err(Error::BadFrame);
                            }

                            ws_in_tracker.extend(pdata.body.len());
                            ws_in_tracker.done();
                        }
                        _ => {}
                    },
                    zhttppacket::RequestPacket::HandoffStart => {
                        drop(zreq);

                        // if handoff requested, flush what we can before accepting
                        // so that the data is not delayed while we wait
                        loop {
                            if send_content.is_none() {
                                if let Some((mtype, avail, done)) = ws_in_tracker.current() {
                                    if !handler.is_sending_message() {
                                        handler.send_message_start(mtype, Some(gen_mask()));
                                    }

                                    if avail > 0 || done {
                                        send_content.set(Some(
                                            handler.send_message_content(avail, done, bytes_read),
                                        ));
                                    }
                                }
                            }

                            if let Some(fut) = send_content.as_mut().as_pin_mut() {
                                // ABR: poll_async doesn't block
                                let ret = match poll_async(fut).await {
                                    Poll::Ready(ret) => ret,
                                    Poll::Pending => break,
                                };

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
                        }

                        // ABR: function contains read
                        server_accept_handoff(zsess_in, zsess_out).await?;
                    }
                    _ => {
                        // ABR: handle_other
                        server_handle_other(zreq, zsess_in, zsess_out).await?;
                    }
                }
            }
        }
    }

    Ok(())
}

struct WsReqData {
    accept: ArrayString<WS_ACCEPT_MAX>,
    deflate_config: Option<(websocket::PerMessageDeflateConfig, usize)>,
}

#[allow(clippy::too_many_arguments)]
fn server_stream_process_req_header(
    id: &str,
    req: &http1::Request<'_, '_>,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    allow_compression: bool,
    packet_buf: &RefCell<Vec<u8>>,
    instance_id: &str,
    shared: &StreamSharedData,
    recv_buf_size: usize,
) -> Result<(zmq::Message, Option<WsReqData>), Error> {
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
                    Err(_) => return Err(Error::InvalidWebSocketRequest),
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
                                    // set the encoded buffer to be 25% the size of the
                                    // recv buffer
                                    let enc_buf_size = recv_buf_size / 4;

                                    ws_deflate_config = Some((resp_config, enc_buf_size));
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
        "server-conn {}: request: {} {}://{}{}",
        id, req.method, scheme, host, req.uri
    );

    let ws_req_data: Option<WsReqData> = if websocket {
        let accept = match validate_ws_request(req, ws_version, ws_key) {
            Ok(s) => s,
            Err(_) => return Err(Error::InvalidWebSocketRequest),
        };

        Some(WsReqData {
            accept,
            deflate_config: ws_deflate_config,
        })
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

    let msg = make_zhttp_request(
        instance_id,
        &ids,
        req.method,
        req.uri,
        req.headers,
        b"",
        more,
        mode,
        recv_buf_size as u32,
        peer_addr,
        secure,
        &mut packet_buf.borrow_mut(),
    )?;

    shared.inc_out_seq();

    Ok((msg, ws_req_data))
}

// read request header and prepare outgoing zmq message.
// return Ok(None) if client disconnects before providing a complete request header
#[allow(clippy::too_many_arguments)]
async fn server_stream_read_header<'a: 'b, 'b, R: AsyncRead, W: AsyncWrite>(
    id: &str,
    req_header: server::RequestHeader<'a, 'b, R, W>,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    allow_compression: bool,
    packet_buf: &RefCell<Vec<u8>>,
    instance_id: &str,
    zreceiver: &TrackedAsyncLocalReceiver<'_, (arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    shared: &StreamSharedData,
    recv_buf_size: usize,
) -> Result<
    Option<(
        zmq::Message,
        http1::BodySize,
        Option<WsReqData>,
        server::RequestBody<'a, 'b, R, W>,
    )>,
    Error,
> {
    let mut scratch = http1::ParseScratch::<HEADERS_MAX>::new();

    // receive request header

    // WARNING: the returned req_header must not be dropped and instead must
    // be consumed by discard_header(). be careful with early returns from
    // this function and do not use the ?-operator
    let (req_header, req_body) = {
        // ABR: discard_while
        match discard_while(zreceiver, pin!(req_header.recv(&mut scratch))).await {
            Ok(ret) => ret,
            Err(Error::Io(e)) if e.kind() == io::ErrorKind::UnexpectedEof => return Ok(None),
            Err(e) => return Err(e),
        }
    };

    let req_ref = req_header.get();

    let result = server_stream_process_req_header(
        id,
        &req_ref,
        peer_addr,
        secure,
        allow_compression,
        packet_buf,
        instance_id,
        shared,
        recv_buf_size,
    );

    let body_size = req_ref.body_size;

    // whether success or fail, toss req_header so we are able to respond
    let req_body = req_body.discard_header(req_header);

    // NOTE: req_header is now consumed and we don't need to worry about it from here

    let (msg, ws_req_data) = result?;

    Ok(Some((msg, body_size, ws_req_data, req_body)))
}

struct StreamRespondProceed<'buf, 'st, 'zs, 'tr, R: AsyncRead, W: AsyncWrite, R2> {
    header: server::ResponseHeader<'buf, 'st, R, W>,
    prepare_body: server::ResponsePrepareBody<'buf, 'st, R, W>,
    zsess_in: ZhttpStreamSessionIn<'zs, 'tr, R2>,
    ws_config: Option<Option<(websocket::PerMessageDeflateConfig, usize)>>,
}

struct StreamRespondWebSocketRejected<'buf, 'st, R: AsyncRead, W: AsyncWrite> {
    header: server::ResponseHeader<'buf, 'st, R, W>,
    prepare_body: server::ResponsePrepareBody<'buf, 'st, R, W>,
}

enum StreamRespond<'buf, 'st, 'zs, 'tr, R: AsyncRead, W: AsyncWrite, R2> {
    Proceed(StreamRespondProceed<'buf, 'st, 'zs, 'tr, R, W, R2>),
    WebSocketRejected(StreamRespondWebSocketRejected<'buf, 'st, R, W>),
}

// consumes resp if successful
#[allow(clippy::too_many_arguments)]
async fn server_stream_respond<'buf, 'st, 'zs, 'tr, R, W, R1, R2>(
    id: &'zs str,
    req: server::Request,
    resp: &mut Option<server::Response<'buf, R, W>>,
    resp_state: &'st mut server::ResponseState<'buf, R, W>,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    send_buf_size: usize,
    recv_buf_size: usize,
    allow_compression: bool,
    packet_buf: &RefCell<Vec<u8>>,
    tmp_buf: &RefCell<Vec<u8>>,
    instance_id: &str,
    zsender: &AsyncLocalSender<zmq::Message>,
    zsess_out: &ZhttpStreamSessionOut<'_>,
    zreceiver: &'zs TrackedAsyncLocalReceiver<'tr, (arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    shared: &'zs StreamSharedData,
    refresh_stream_timeout: &R1,
    refresh_session_timeout: &'zs R2,
) -> Result<Option<StreamRespond<'buf, 'st, 'zs, 'tr, R, W, R2>>, Error>
where
    R: AsyncRead,
    W: AsyncWrite,
    R1: Fn(),
    R2: Fn(),
{
    let req_header = req.recv_header(resp.as_mut().unwrap());

    // receive request header

    let result = server_stream_read_header(
        id,
        req_header,
        peer_addr,
        secure,
        allow_compression,
        packet_buf,
        instance_id,
        zreceiver,
        shared,
        recv_buf_size,
    )
    .await?;

    let (msg, body_size, ws_req_data, req_body) = match result {
        Some(ret) => ret,
        None => return Ok(None),
    };

    refresh_stream_timeout();

    // send request message

    // ABR: discard_while
    discard_while(zreceiver, pin!(send_msg(zsender, msg))).await?;

    let mut zsess_in = ZhttpStreamSessionIn::new(
        id,
        send_buf_size,
        ws_req_data.is_some(),
        zreceiver,
        shared,
        refresh_session_timeout,
    );

    // receive any message, in order to get a handler address
    // ABR: direct read
    zsess_in.peek_msg().await?;

    if body_size != http1::BodySize::NoBody {
        // receive request body and send to handler

        // ABR: function contains read
        stream_recv_body(
            tmp_buf,
            refresh_stream_timeout,
            req_body,
            &mut zsess_in,
            zsess_out,
        )
        .await?;
    }

    // receive response message

    let zresp = loop {
        let mut resp_take = resp.take().unwrap();

        // ABR: select contains read
        let ret = select_2(
            pin!(zsess_in.recv_msg()),
            pin!(resp_take.fill_recv_buffer()),
        )
        .await;

        *resp = Some(resp_take);

        match ret {
            Select2::R1(ret) => {
                let zresp = ret?;

                match zresp.get().get().ptype {
                    zhttppacket::ResponsePacket::Data(_)
                    | zhttppacket::ResponsePacket::Error(_) => break zresp,
                    _ => {
                        // ABR: handle_other
                        handle_other(zresp, &mut zsess_in, zsess_out).await?;
                    }
                }
            }
            Select2::R2(e) => return Err(e.into()),
        }
    };

    // determine how to respond

    let rdata = match &zresp.get().get().ptype {
        zhttppacket::ResponsePacket::Data(rdata) => rdata,
        zhttppacket::ResponsePacket::Error(edata) => {
            if ws_req_data.is_some() && edata.condition == "rejected" {
                // send websocket rejection

                let rdata = edata.rejected_info.as_ref().unwrap();

                if rdata.body.len() > recv_buf_size {
                    return Err(Error::WebSocketRejectionTooLarge(recv_buf_size));
                }

                let (header, mut prepare_body) = {
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
                            return Err(Error::BadMessage);
                        }

                        headers[headers_len] = http1::Header {
                            name: h.name,
                            value: h.value,
                        };

                        headers_len += 1;
                    }

                    let headers = &headers[..headers_len];

                    let mut resp_take = resp.take().unwrap();

                    match resp_take.prepare_header(
                        rdata.code,
                        rdata.reason,
                        headers,
                        http1::BodySize::Known(rdata.body.len()),
                        resp_state,
                    ) {
                        Ok(ret) => ret,
                        Err(e) => {
                            *resp = Some(resp_take);
                            return Err(e.into());
                        }
                    }
                };

                // first call can't fail
                let (size, overflowed) = prepare_body
                    .prepare(rdata.body, true)
                    .expect("infallible prepare call failed");

                if overflowed > 0 {
                    debug!("server-conn {}: overflowing {} bytes", id, overflowed);
                }

                // we confirmed above that the data will fit in the buffer
                assert!(size == rdata.body.len());

                return Ok(Some(StreamRespond::WebSocketRejected(
                    StreamRespondWebSocketRejected {
                        header,
                        prepare_body,
                    },
                )));
            } else {
                // ABR: handle_other
                return Err(handle_other(zresp, &mut zsess_in, zsess_out)
                    .await
                    .unwrap_err());
            }
        }
        _ => unreachable!(), // we confirmed the type above
    };

    if rdata.body.len() > recv_buf_size {
        return Err(Error::BufferExceeded);
    }

    // send response header

    let (header, mut prepare_body) = {
        let mut headers = [http1::EMPTY_HEADER; HEADERS_MAX];
        let mut headers_len = 0;

        let mut body_size = http1::BodySize::Unknown;

        for h in rdata.headers.iter() {
            if ws_req_data.is_some() {
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
                        Err(_) => return Err(io::Error::from(io::ErrorKind::InvalidInput).into()),
                    };

                    body_size = http1::BodySize::Known(clen);
                }
            }

            if headers_len >= headers.len() {
                return Err(Error::BadMessage);
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

        if let Some(ws_req_data) = &ws_req_data {
            let accept_data = &ws_req_data.accept;

            if headers_len + 4 > headers.len() {
                return Err(Error::BadMessage);
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

            if let Some((config, _)) = &ws_req_data.deflate_config {
                if write_ws_ext_header_value(config, &mut ws_ext).is_err() {
                    return Err(Error::Compression);
                }

                headers[headers_len] = http1::Header {
                    name: "Sec-WebSocket-Extensions",
                    value: ws_ext.as_ref(),
                };
                headers_len += 1;
            }
        }

        let headers = &headers[..headers_len];

        let mut resp_take = resp.take().unwrap();

        match resp_take.prepare_header(rdata.code, rdata.reason, headers, body_size, resp_state) {
            Ok(ret) => ret,
            Err(e) => {
                *resp = Some(resp_take);
                return Err(e.into());
            }
        }
    };

    // first call can't fail
    let (size, overflowed) = prepare_body
        .prepare(rdata.body, !rdata.more)
        .expect("infallible prepare call failed");

    if overflowed > 0 {
        debug!("server-conn {}: overflowing {} bytes", id, overflowed);
    }

    // we confirmed above that the data will fit in the buffer
    assert!(size == rdata.body.len());

    let ws_config = if let Some(ws_req_data) = ws_req_data {
        Some(ws_req_data.deflate_config)
    } else {
        None
    };

    Ok(Some(StreamRespond::Proceed(StreamRespondProceed {
        header,
        prepare_body,
        ws_config,
        zsess_in,
    })))
}

// return true if persistent
#[allow(clippy::too_many_arguments)]
async fn server_stream_handler<S, R1, R2>(
    id: &str,
    stream: &mut S,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    buf1: &mut VecRingBuffer,
    buf2: &mut VecRingBuffer,
    blocks_max: usize,
    blocks_avail: &Counter,
    messages_max: usize,
    allow_compression: bool,
    packet_buf: &RefCell<Vec<u8>>,
    tmp_buf: &RefCell<Vec<u8>>,
    instance_id: &str,
    zsender: &AsyncLocalSender<zmq::Message>,
    zsender_stream: &AsyncLocalSender<(ArrayVec<u8, 64>, zmq::Message)>,
    zreceiver: &TrackedAsyncLocalReceiver<'_, (arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    shared: &StreamSharedData,
    refresh_stream_timeout: &R1,
    refresh_session_timeout: &R2,
) -> Result<bool, Error>
where
    S: AsyncRead + AsyncWrite,
    R1: Fn(),
    R2: Fn(),
{
    let stream = RefCell::new(stream);

    let send_buf_size = buf1.capacity(); // for sending to handler
    let recv_buf_size = buf2.capacity(); // for receiving from handler

    let zsess_out = ZhttpStreamSessionOut::new(instance_id, id, packet_buf, zsender_stream, shared);

    let mut resp_state = server::ResponseState::default();

    let respond = {
        let (req, resp) = server::Request::new(io_split(&stream), buf1, buf2);
        let mut resp = Some(resp);

        let ret = match server_stream_respond(
            id,
            req,
            &mut resp,
            &mut resp_state,
            peer_addr,
            secure,
            send_buf_size,
            recv_buf_size,
            allow_compression,
            packet_buf,
            tmp_buf,
            instance_id,
            zsender,
            &zsess_out,
            zreceiver,
            shared,
            refresh_stream_timeout,
            refresh_session_timeout,
        )
        .await
        {
            Ok(Some(ret)) => ret,
            Ok(None) => return Ok(false), // no request
            Err(e) => return Err(e),
        };

        assert!(resp.is_none());

        ret
    };

    let (header, mut prepare_body, ws_config, mut zsess_in) = match respond {
        StreamRespond::Proceed(p) => (p.header, p.prepare_body, p.ws_config, p.zsess_in),
        StreamRespond::WebSocketRejected(r) => {
            // ABR: discard_while
            let header_sent = discard_while(zreceiver, pin!(r.header.send())).await?;

            let resp_body = header_sent.start_body(r.prepare_body);

            loop {
                // send the buffer
                let send = async {
                    match resp_body.send().await {
                        SendStatus::Complete(finished) => Ok(Some(finished)),
                        SendStatus::EarlyResponse(_) => unreachable!(), // for requests only
                        SendStatus::Partial((), _) => Ok(None),
                        SendStatus::Error((), e) => Err(e),
                    }
                };

                // ABR: discard_while
                if let Some(_finished) = discard_while(zreceiver, pin!(send)).await? {
                    break;
                }
            }

            return Ok(false);
        }
    };

    let header_sent = {
        let mut send = pin!(header.send());

        loop {
            // ABR: select contains read
            let ret = select_2(send.as_mut(), pin!(zsess_in.recv_msg())).await;

            match ret {
                Select2::R1(ret) => break ret?,
                Select2::R2(ret) => {
                    let zresp = ret?;

                    match &zresp.get().get().ptype {
                        zhttppacket::ResponsePacket::Data(rdata) => {
                            let (size, overflowed) =
                                prepare_body.prepare(rdata.body, !rdata.more)?;

                            if overflowed > 0 {
                                debug!("server-conn {}: overflowing {} bytes", id, overflowed);
                            }

                            if size < rdata.body.len() {
                                return Err(Error::BufferExceeded);
                            }
                        }
                        _ => {
                            // ABR: handle_other
                            handle_other(zresp, &mut zsess_in, &zsess_out).await?;
                        }
                    }
                }
            }
        }
    };

    let resp_body = header_sent.start_body(prepare_body);

    refresh_stream_timeout();

    if let Some(deflate_config) = ws_config {
        // reduce size of future
        #[allow(clippy::drop_non_drop)]
        drop(resp_body);

        // handle as websocket connection

        // ABR: function contains read
        stream_websocket(
            id,
            stream,
            buf1,
            buf2,
            blocks_max,
            blocks_avail,
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

        // ABR: function contains read
        let finished = stream_send_body(
            refresh_stream_timeout,
            resp_body,
            &mut zsess_in,
            &zsess_out,
            blocks_max,
            blocks_avail,
        )
        .await?;

        Ok(finished.is_persistent())
    }
}

#[allow(clippy::too_many_arguments)]
async fn server_stream_connection_inner<P: CidProvider, S: AsyncRead + AsyncWrite + Identify>(
    token: CancellationToken,
    cid: &mut ArrayString<32>,
    cid_provider: &mut P,
    mut stream: S,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    buffer_size: usize,
    blocks_max: usize,
    blocks_avail: &Counter,
    messages_max: usize,
    rb_tmp: &Rc<TmpBuffer>,
    packet_buf: Rc<RefCell<Vec<u8>>>,
    tmp_buf: Rc<RefCell<Vec<u8>>>,
    stream_timeout_duration: Duration,
    allow_compression: bool,
    instance_id: &str,
    zsender: AsyncLocalSender<zmq::Message>,
    zsender_stream: AsyncLocalSender<(ArrayVec<u8, 64>, zmq::Message)>,
    zreceiver: &TrackedAsyncLocalReceiver<'_, (arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    shared: arena::Rc<StreamSharedData>,
) -> Result<(), Error> {
    let reactor = Reactor::current().unwrap();

    let mut buf1 = VecRingBuffer::new(buffer_size, rb_tmp);
    let mut buf2 = VecRingBuffer::new(buffer_size, rb_tmp);

    loop {
        stream.set_id(cid);

        // this was originally logged when starting the non-async state
        // machine, so we'll keep doing that
        debug!("server-conn {}: assigning id", cid);

        let reuse = {
            let stream_timeout = Timeout::new(reactor.now() + stream_timeout_duration);
            let session_timeout = Timeout::new(reactor.now() + ZHTTP_SESSION_TIMEOUT);

            let refresh_stream_timeout = || {
                stream_timeout.set_deadline(reactor.now() + stream_timeout_duration);
            };

            let refresh_session_timeout = || {
                session_timeout.set_deadline(reactor.now() + ZHTTP_SESSION_TIMEOUT);
            };

            let handler = pin!(server_stream_handler(
                cid.as_ref(),
                &mut stream,
                peer_addr,
                secure,
                &mut buf1,
                &mut buf2,
                blocks_max,
                blocks_avail,
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

            let ret = match select_4(
                handler,
                stream_timeout.elapsed(),
                session_timeout.elapsed(),
                token.cancelled(),
            )
            .await
            {
                Select4::R1(ret) => ret,
                Select4::R2(_) => Err(Error::StreamTimeout),
                Select4::R3(_) => return Err(Error::SessionTimeout),
                Select4::R4(_) => return Err(Error::Stopped),
            };

            match ret {
                Ok(reuse) => reuse,
                Err(e) => {
                    let handler_caused = matches!(
                        &e,
                        Error::BadMessage | Error::Handler | Error::HandlerCancel
                    );

                    if !handler_caused {
                        let shared = shared.get();

                        let msg = if let Some(addr) = shared.to_addr().get() {
                            let id = cid.as_ref();

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

                            let msg = zmq::Message::from(&packet_buf[..size]);

                            let addr = match ArrayVec::try_from(addr) {
                                Ok(v) => v,
                                Err(_) => {
                                    return Err(io::Error::from(io::ErrorKind::InvalidInput).into())
                                }
                            };

                            Some((addr, msg))
                        } else {
                            None
                        };

                        if let Some((addr, msg)) = msg {
                            // best effort
                            let _ = zsender_stream.try_send((addr, msg));

                            shared.inc_out_seq();
                        }
                    }

                    return Err(e);
                }
            }
        };

        if !reuse {
            break;
        }

        // note: buf1 is not cleared as there may be data to read

        let additional_blocks = (buf2.capacity() / buffer_size) - 1;

        buf2.clear();
        buf2.resize(buffer_size);
        shared.get().reset();

        blocks_avail.inc(additional_blocks).unwrap();

        *cid = cid_provider.get_new_assigned_cid();
    }

    // ABR: discard_while
    discard_while(zreceiver, pin!(stream.close())).await?;

    Ok(())
}

#[allow(clippy::too_many_arguments)]
pub async fn server_stream_connection<P: CidProvider, S: AsyncRead + AsyncWrite + Identify>(
    token: CancellationToken,
    mut cid: ArrayString<32>,
    cid_provider: &mut P,
    stream: S,
    peer_addr: Option<&SocketAddr>,
    secure: bool,
    buffer_size: usize,
    blocks_max: usize,
    blocks_avail: &Counter,
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
    shared: arena::Rc<StreamSharedData>,
) {
    let value_active = TrackFlag::default();

    let zreceiver = TrackedAsyncLocalReceiver::new(zreceiver, &value_active);

    match track_future(
        server_stream_connection_inner(
            token,
            &mut cid,
            cid_provider,
            stream,
            peer_addr,
            secure,
            buffer_size,
            blocks_max,
            blocks_avail,
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
        ),
        &value_active,
    )
    .await
    {
        Ok(()) => debug!("server-conn {}: finished", cid),
        Err(e) => log!(e.log_level(), "server-conn {}: process error: {:?}", cid, e),
    }
}

enum Stream {
    Plain(std::net::TcpStream),
    Tls(TlsStream<std::net::TcpStream>),
}

impl Read for Stream {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, io::Error> {
        match self {
            Self::Plain(stream) => stream.read(buf),
            Self::Tls(stream) => stream.read(buf),
        }
    }
}

enum AsyncStream<'a> {
    Plain(AsyncTcpStream),
    Tls(AsyncTlsStream<'a>),
}

impl<'a> AsyncStream<'a> {
    fn into_inner(self) -> Stream {
        match self {
            Self::Plain(stream) => Stream::Plain(stream.into_std()),
            Self::Tls(stream) => Stream::Tls(stream.into_std()),
        }
    }
}

#[derive(Clone, Eq, Hash, PartialEq)]
struct ConnectionPoolKey {
    addr: std::net::SocketAddr,
    tls: bool,
    host: String,
}

impl ConnectionPoolKey {
    fn new(addr: std::net::SocketAddr, tls: bool, host: String) -> Self {
        Self { addr, tls, host }
    }
}

pub struct ConnectionPool {
    inner: Arc<Mutex<Pool<ConnectionPoolKey, Stream>>>,
    thread: Option<thread::JoinHandle<()>>,
    done: Option<mpsc::SyncSender<()>>,
}

impl ConnectionPool {
    pub fn new(capacity: usize) -> Self {
        let inner = Arc::new(Mutex::new(Pool::<ConnectionPoolKey, Stream>::new(capacity)));

        let (s, r) = mpsc::sync_channel(1);

        let thread = {
            let inner = Arc::clone(&inner);

            thread::Builder::new()
                .name("connection-pool".into())
                .spawn(move || {
                    while let Err(mpsc::RecvTimeoutError::Timeout) =
                        r.recv_timeout(Duration::from_secs(1))
                    {
                        let now = Instant::now();

                        while let Some((key, _)) = inner.lock().unwrap().expire(now) {
                            debug!("closing idle connection to {:?} for {}", key.addr, key.host);
                        }
                    }
                })
                .unwrap()
        };

        Self {
            inner,
            thread: Some(thread),
            done: Some(s),
        }
    }

    #[allow(clippy::result_large_err)]
    fn push(
        &self,
        addr: std::net::SocketAddr,
        tls: bool,
        host: String,
        stream: Stream,
        ttl: Duration,
    ) -> Result<(), Stream> {
        self.inner.lock().unwrap().add(
            ConnectionPoolKey::new(addr, tls, host),
            stream,
            Instant::now() + ttl,
        )
    }

    fn take(&self, addr: std::net::SocketAddr, tls: bool, host: &str) -> Option<Stream> {
        let key = ConnectionPoolKey::new(addr, tls, host.to_string());

        // take the first connection that returns WouldBlock when attempting a read.
        // anything else is considered an error and the connection is discarded
        while let Some(mut stream) = self.inner.lock().unwrap().take(&key) {
            match stream.read(&mut [0]) {
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => return Some(stream),
                _ => {}
            }

            debug!(
                "discarding broken connection to {:?} for {}",
                key.addr, key.host
            );
        }

        None
    }
}

impl Drop for ConnectionPool {
    fn drop(&mut self) {
        self.done = None;

        let thread = self.thread.take().unwrap();
        thread.join().unwrap();
    }
}

fn is_allowed(addr: &IpAddr, deny: &[IpNet]) -> bool {
    for net in deny {
        if net.contains(addr) {
            return false;
        }
    }

    true
}

async fn client_connect<'a>(
    log_id: &str,
    rdata: &zhttppacket::RequestData<'_, '_>,
    uri: &url::Url,
    resolver: &resolver::Resolver,
    deny: &[IpNet],
    pool: &ConnectionPool,
    tls_waker_data: &'a RefWakerData<TlsWaker>,
) -> Result<(std::net::SocketAddr, bool, AsyncStream<'a>), Error> {
    let use_tls = ["https", "wss"].contains(&uri.scheme());

    let uri_host = match uri.host_str() {
        Some(s) => s,
        None => return Err(Error::BadRequest),
    };

    let default_port = if use_tls { 443 } else { 80 };

    let (connect_host, connect_port) = if !rdata.connect_host.is_empty() {
        (rdata.connect_host, rdata.connect_port)
    } else {
        (uri_host, uri.port().unwrap_or(default_port))
    };

    let resolver = AsyncResolver::new(resolver);

    debug!("client-conn {}: resolving: [{}]", log_id, connect_host);

    let resolver_results = resolver.resolve(connect_host).await?;

    let mut addrs = ArrayVec::<std::net::SocketAddr, { resolver::ADDRS_MAX }>::new();
    let mut denied = false;
    let mut reuse_stream = None;

    for addr in resolver_results {
        if !is_allowed(&addr, deny) {
            denied = true;
            continue;
        }

        let addr = std::net::SocketAddr::new(addr, connect_port);

        if let Some(stream) = pool.take(addr, use_tls, uri_host) {
            reuse_stream = Some((addr, stream));
            break;
        }

        addrs.push(addr);
    }

    let (peer_addr, mut stream, is_new) = if let Some((peer_addr, stream)) = reuse_stream {
        debug!(
            "client-conn {}: reusing connection to {:?}",
            log_id, peer_addr,
        );

        let stream = match stream {
            Stream::Plain(stream) => AsyncStream::Plain(AsyncTcpStream::from_std(stream)),
            Stream::Tls(stream) => {
                AsyncStream::Tls(AsyncTlsStream::from_std(stream, tls_waker_data))
            }
        };

        (peer_addr, stream, false)
    } else {
        if addrs.is_empty() && denied {
            return Err(Error::PolicyViolation);
        }

        debug!("client-conn {}: connecting to one of {:?}", log_id, addrs);

        let stream = AsyncTcpStream::connect(&addrs).await?;

        let peer_addr = stream.peer_addr()?;

        debug!("client-conn {}: connected to {}", log_id, peer_addr);

        let stream = if use_tls {
            let host = if rdata.trust_connect_host {
                connect_host
            } else {
                uri_host
            };

            let verify_mode = if rdata.ignore_tls_errors {
                VerifyMode::None
            } else {
                VerifyMode::Full
            };

            let stream = match AsyncTlsStream::connect(host, stream, verify_mode, tls_waker_data) {
                Ok(stream) => stream,
                Err(e) => {
                    debug!("client-conn {}: tls connect error: {}", log_id, e);

                    return Err(Error::Tls);
                }
            };

            AsyncStream::Tls(stream)
        } else {
            AsyncStream::Plain(stream)
        };

        (peer_addr, stream, true)
    };

    if let AsyncStream::Tls(stream) = &mut stream {
        if stream.inner().set_id(log_id).is_err() {
            warn!("client-conn {}: log id too long for TlsStream", log_id);

            return Err(Error::BadRequest);
        }

        if is_new {
            if let Err(e) = stream.ensure_handshake().await {
                debug!("client-conn {}: tls handshake error: {:?}", log_id, e);

                return Err(Error::Tls);
            }
        }
    }

    Ok((peer_addr, use_tls, stream))
}

// return Some if fully valid redirect response, else return None.
fn check_redirect(
    method: &str,
    base_url: &url::Url,
    resp: &http1::Response,
    schemes: &[&str],
) -> Option<(url::Url, bool)> {
    if resp.code >= 300 && resp.code <= 399 {
        let mut location = None;

        for h in resp.headers.iter() {
            if h.name.eq_ignore_ascii_case("Location") {
                location = Some(h.value);
                break;
            }
        }

        // must have location header
        if let Some(s) = location {
            // must be UTF-8
            if let Ok(s) = str::from_utf8(s) {
                // must be valid URL
                if let Ok(url) = base_url.join(s) {
                    // must have an acceptable scheme
                    if schemes.contains(&url.scheme()) {
                        let use_get = resp.code >= 301 && resp.code <= 303 && method == "POST";

                        // all is well!
                        return Some((url, use_get));
                    }
                }
            }
        }
    }

    None
}

enum ClientHandlerDone<T> {
    Complete(T, bool),
    Redirect(bool, url::Url, bool), // rare alloc
}

impl<T> ClientHandlerDone<T> {
    fn is_persistent(&self) -> bool {
        match self {
            ClientHandlerDone::Complete(_, persistent) => *persistent,
            ClientHandlerDone::Redirect(persistent, _, _) => *persistent,
        }
    }
}

// return (_, true) if persistent
#[allow(clippy::too_many_arguments)]
async fn client_req_handler<S>(
    log_id: &str,
    id: Option<&[u8]>,
    stream: &mut S,
    zreq: &zhttppacket::Request<'_, '_, '_>,
    method: &str,
    url: &url::Url,
    include_body: bool,
    follow_redirects: bool,
    buf1: &mut VecRingBuffer,
    buf2: &mut VecRingBuffer,
    body_buf: &mut ContiguousBuffer,
    packet_buf: &RefCell<Vec<u8>>,
) -> Result<ClientHandlerDone<zmq::Message>, Error>
where
    S: AsyncRead + AsyncWrite,
{
    let stream = RefCell::new(stream);
    let req = client::Request::new(io_split(&stream), buf1, buf2);

    let req_header = {
        let rdata = match &zreq.ptype {
            zhttppacket::RequestPacket::Data(data) => data,
            _ => return Err(Error::BadRequest),
        };

        let host_port = &url[url::Position::BeforeHost..url::Position::AfterPort];

        let mut headers = ArrayVec::<http1::Header, HEADERS_MAX>::new();

        headers.push(http1::Header {
            name: "Host",
            value: host_port.as_bytes(),
        });

        for h in rdata.headers.iter() {
            if headers.remaining_capacity() == 0 {
                return Err(Error::BadRequest);
            }

            // host comes from the uri
            if h.name.eq_ignore_ascii_case("Host") {
                continue;
            }

            headers.push(http1::Header {
                name: h.name,
                value: h.value,
            });
        }

        let path = &url[url::Position::BeforePath..];

        let body_size = if include_body {
            body_buf.write_all(rdata.body)?;

            http1::BodySize::Known(rdata.body.len())
        } else {
            http1::BodySize::NoBody
        };

        req.prepare_header(method, path, &headers, body_size, false, &[], false)?
    };

    let resp = {
        // send request header

        let req_body = req_header.send().await?;

        // send request body

        loop {
            // fill the buffer as much as possible
            let size = req_body.prepare(Buffer::read_buf(body_buf), true)?;
            body_buf.read_commit(size);

            // send the buffer
            match req_body.send().await {
                SendStatus::Complete(resp) => break resp,
                SendStatus::EarlyResponse(resp) => {
                    body_buf.clear();
                    break resp;
                }
                SendStatus::Partial((), _) => {}
                SendStatus::Error((), e) => return Err(e.into()),
            }
        }
    };

    assert_eq!(body_buf.len(), 0);

    // receive response header

    let mut scratch = http1::ParseScratch::<HEADERS_MAX>::new();

    let (resp, resp_body) = resp.recv_header(&mut scratch).await?;

    let (zresp, finished) = {
        let resp_ref = resp.get();

        debug!(
            "client-conn {}: response: {} {}",
            log_id, resp_ref.code, resp_ref.reason
        );

        // receive response body

        let finished = {
            loop {
                match resp_body.try_recv(body_buf.write_buf())? {
                    RecvStatus::Complete(finished, written) => {
                        body_buf.write_commit(written);

                        break finished;
                    }
                    RecvStatus::Read((), written) => {
                        body_buf.write_commit(written);

                        if written == 0 {
                            resp_body.add_to_buffer().await?;
                        }
                    }
                }
            }
        };

        if follow_redirects {
            if let Some((url, use_get)) = check_redirect(method, url, &resp_ref, &["http", "https"])
            {
                let finished = finished.discard_header(resp);

                debug!("client-conn {}: redirecting to {}", log_id, url);

                return Ok(ClientHandlerDone::Redirect(
                    finished.is_persistent(),
                    url,
                    use_get,
                ));
            }
        }

        let mut zheaders = ArrayVec::<zhttppacket::Header, HEADERS_MAX>::new();

        for h in resp_ref.headers {
            zheaders.push(zhttppacket::Header {
                name: h.name,
                value: h.value,
            });
        }

        let rdata = zhttppacket::ResponseData {
            credits: 0,
            more: false,
            code: resp_ref.code,
            reason: resp_ref.reason,
            headers: &zheaders,
            content_type: None,
            body: Buffer::read_buf(body_buf),
        };

        let zresp = make_zhttp_req_response(
            id,
            zhttppacket::ResponsePacket::Data(rdata),
            &mut packet_buf.borrow_mut(),
        )?;

        (zresp, finished)
    };

    let finished = finished.discard_header(resp);

    Ok(ClientHandlerDone::Complete(zresp, finished.is_persistent()))
}

#[allow(clippy::too_many_arguments)]
async fn client_req_connect(
    log_id: &str,
    id: Option<&[u8]>,
    zreq: arena::Rc<zhttppacket::OwnedRequest>,
    buf1: &mut VecRingBuffer,
    buf2: &mut VecRingBuffer,
    body_buf: &mut ContiguousBuffer,
    packet_buf: &RefCell<Vec<u8>>,
    deny: &[IpNet],
    resolver: &resolver::Resolver,
    pool: &ConnectionPool,
) -> Result<zmq::Message, Error> {
    let zreq = zreq.get().get();

    let rdata = match &zreq.ptype {
        zhttppacket::RequestPacket::Data(data) => data,
        _ => return Err(Error::BadRequest),
    };

    let initial_url = match url::Url::parse(rdata.uri) {
        Ok(url) => url,
        Err(_) => return Err(Error::BadRequest),
    };

    // must be an http url
    if !["http", "https"].contains(&initial_url.scheme()) {
        return Err(Error::BadRequest);
    }

    // must have a method
    if rdata.method.is_empty() {
        return Err(Error::BadRequest);
    }

    debug!(
        "client-conn {}: request: {} {}",
        log_id, rdata.method, rdata.uri,
    );

    let deny = if rdata.ignore_policies { &[] } else { deny };

    let mut last_redirect: Option<(url::Url, bool)> = None;
    let mut redirect_count = 0;

    let zresp = loop {
        let (method, url, include_body) = match &last_redirect {
            Some((url, use_get)) => {
                let (method, include_body) = if *use_get {
                    ("GET", false)
                } else {
                    (rdata.method, true)
                };

                (method, url, include_body)
            }
            None => (rdata.method, &initial_url, true),
        };

        let url_host = match url.host_str() {
            Some(s) => s,
            None => return Err(Error::BadRequest),
        };

        let tls_waker_data = RefWakerData::new(TlsWaker::new());

        let (peer_addr, using_tls, mut stream) =
            client_connect(log_id, rdata, url, resolver, deny, pool, &tls_waker_data).await?;

        let done = match &mut stream {
            AsyncStream::Plain(stream) => {
                client_req_handler(
                    log_id,
                    id,
                    stream,
                    zreq,
                    method,
                    url,
                    include_body,
                    rdata.follow_redirects,
                    buf1,
                    buf2,
                    body_buf,
                    packet_buf,
                )
                .await?
            }
            AsyncStream::Tls(stream) => {
                client_req_handler(
                    log_id,
                    id,
                    stream,
                    zreq,
                    method,
                    url,
                    include_body,
                    rdata.follow_redirects,
                    buf1,
                    buf2,
                    body_buf,
                    packet_buf,
                )
                .await?
            }
        };

        if done.is_persistent() {
            if pool
                .push(
                    peer_addr,
                    using_tls,
                    url_host.to_string(),
                    stream.into_inner(),
                    CONNECTION_POOL_TTL,
                )
                .is_ok()
            {
                debug!("client-conn {}: leaving connection intact", log_id);
            }
        }

        match done {
            ClientHandlerDone::Complete(zresp, _) => break zresp,
            ClientHandlerDone::Redirect(_, url, mut use_get) => {
                if redirect_count >= REDIRECTS_MAX {
                    return Err(Error::TooManyRedirects);
                }

                redirect_count += 1;

                if let Some((_, b)) = &last_redirect {
                    use_get = use_get || *b;
                }

                last_redirect = Some((url, use_get));
            }
        }
    };

    Ok(zresp)
}

#[allow(clippy::too_many_arguments)]
async fn client_req_connection_inner(
    token: CancellationToken,
    log_id: &str,
    id: Option<&[u8]>,
    zreq: (MultipartHeader, arena::Rc<zhttppacket::OwnedRequest>),
    buffer_size: usize,
    body_buffer_size: usize,
    rb_tmp: &Rc<TmpBuffer>,
    packet_buf: Rc<RefCell<Vec<u8>>>,
    timeout: Duration,
    deny: &[IpNet],
    resolver: &resolver::Resolver,
    pool: &ConnectionPool,
    zsender: AsyncLocalSender<(MultipartHeader, zmq::Message)>,
) -> Result<(), Error> {
    let reactor = Reactor::current().unwrap();

    let (zheader, zreq) = zreq;

    let mut buf1 = VecRingBuffer::new(buffer_size, rb_tmp);
    let mut buf2 = VecRingBuffer::new(buffer_size, rb_tmp);
    let mut body_buf = ContiguousBuffer::new(body_buffer_size);

    let handler = client_req_connect(
        log_id,
        id,
        zreq,
        &mut buf1,
        &mut buf2,
        &mut body_buf,
        &packet_buf,
        deny,
        resolver,
        pool,
    );

    let timeout = Timeout::new(reactor.now() + timeout);

    let ret = match select_3(pin!(handler), timeout.elapsed(), token.cancelled()).await {
        Select3::R1(ret) => ret,
        Select3::R2(_) => Err(Error::StreamTimeout),
        Select3::R3(_) => return Err(Error::Stopped),
    };

    match ret {
        Ok(zresp) => zsender.send((zheader, zresp)).await?,
        Err(e) => {
            let zresp = make_zhttp_req_response(
                id,
                zhttppacket::ResponsePacket::Error(zhttppacket::ResponseErrorData {
                    condition: e.to_condition(),
                    rejected_info: None,
                }),
                &mut packet_buf.borrow_mut(),
            )?;

            zsender.send((zheader, zresp)).await?;

            return Err(e);
        }
    }

    Ok(())
}

#[allow(clippy::too_many_arguments)]
pub async fn client_req_connection(
    token: CancellationToken,
    log_id: &str,
    id: Option<&[u8]>,
    zreq: (MultipartHeader, arena::Rc<zhttppacket::OwnedRequest>),
    buffer_size: usize,
    body_buffer_size: usize,
    rb_tmp: &Rc<TmpBuffer>,
    packet_buf: Rc<RefCell<Vec<u8>>>,
    timeout: Duration,
    deny: &[IpNet],
    resolver: &resolver::Resolver,
    pool: &ConnectionPool,
    zsender: AsyncLocalSender<(MultipartHeader, zmq::Message)>,
) {
    match client_req_connection_inner(
        token,
        log_id,
        id,
        zreq,
        buffer_size,
        body_buffer_size,
        rb_tmp,
        packet_buf,
        timeout,
        deny,
        resolver,
        pool,
        zsender,
    )
    .await
    {
        Ok(()) => debug!("client-conn {}: finished", log_id),
        Err(e) => log!(
            e.log_level(),
            "client-conn {}: process error: {:?}",
            log_id,
            e
        ),
    }
}

// return true if persistent
#[allow(clippy::too_many_arguments)]
async fn client_stream_handler<S, R1, R2>(
    log_id: &str,
    stream: &mut S,
    zreq: &zhttppacket::Request<'_, '_, '_>,
    method: &str,
    url: &url::Url,
    include_body: bool,
    mut follow_redirects: bool,
    buf1: &mut VecRingBuffer,
    buf2: &mut VecRingBuffer,
    blocks_max: usize,
    blocks_avail: &Counter,
    messages_max: usize,
    allow_compression: bool,
    tmp_buf: &RefCell<Vec<u8>>,
    zsess_in: &mut ZhttpServerStreamSessionIn<'_, '_, R2>,
    zsess_out: &ZhttpServerStreamSessionOut<'_>,
    response_received: &mut bool,
    refresh_stream_timeout: &R1,
) -> Result<ClientHandlerDone<()>, Error>
where
    S: AsyncRead + AsyncWrite,
    R1: Fn(),
    R2: Fn(),
{
    let stream = RefCell::new(stream);

    let send_buf_size = buf1.capacity(); // for sending to handler
    let recv_buf_size = buf2.capacity(); // for receiving from handler

    let req = client::Request::new(io_split(&stream), buf1, buf2);

    let (req_header, ws_key, overflow) = {
        let rdata = match &zreq.ptype {
            zhttppacket::RequestPacket::Data(data) => data,
            _ => return Err(Error::BadRequest),
        };

        let websocket = ["wss", "ws"].contains(&url.scheme());

        let host_port = &url[url::Position::BeforeHost..url::Position::AfterPort];

        let ws_key = if websocket { Some(gen_ws_key()) } else { None };

        if !websocket && rdata.more {
            follow_redirects = false;
        }

        let mut ws_ext = ArrayVec::<u8, 512>::new();

        let mut headers = ArrayVec::<http1::Header, HEADERS_MAX>::new();

        headers.push(http1::Header {
            name: "Host",
            value: host_port.as_bytes(),
        });

        if let Some(ws_key) = &ws_key {
            headers.push(http1::Header {
                name: "Upgrade",
                value: b"websocket",
            });

            headers.push(http1::Header {
                name: "Connection",
                value: b"Upgrade",
            });

            headers.push(http1::Header {
                name: "Sec-WebSocket-Version",
                value: b"13",
            });

            headers.push(http1::Header {
                name: "Sec-WebSocket-Key",
                value: ws_key.as_bytes(),
            });

            if allow_compression {
                if write_ws_ext_header_value(
                    &websocket::PerMessageDeflateConfig::default(),
                    &mut ws_ext,
                )
                .is_err()
                {
                    return Err(Error::Compression);
                }

                headers.push(http1::Header {
                    name: "Sec-WebSocket-Extensions",
                    value: ws_ext.as_slice(),
                });
            }
        }

        let mut body_size = if websocket || !include_body {
            http1::BodySize::NoBody
        } else {
            http1::BodySize::Unknown
        };

        for h in rdata.headers.iter() {
            // host comes from the uri
            if h.name.eq_ignore_ascii_case("Host") {
                continue;
            }

            if websocket {
                // don't send these headers
                if h.name.eq_ignore_ascii_case("Connection")
                    || h.name.eq_ignore_ascii_case("Upgrade")
                    || h.name.eq_ignore_ascii_case("Sec-WebSocket-Version")
                    || h.name.eq_ignore_ascii_case("Sec-WebSocket-Key")
                {
                    continue;
                }
            } else {
                if h.name.eq_ignore_ascii_case("Content-Length") {
                    let s = str::from_utf8(h.value)?;

                    let clen: usize = match s.parse() {
                        Ok(clen) => clen,
                        Err(_) => return Err(io::Error::from(io::ErrorKind::InvalidInput).into()),
                    };

                    body_size = http1::BodySize::Known(clen);
                }
            }

            if headers.remaining_capacity() == 0 {
                return Err(Error::BadRequest);
            }

            headers.push(http1::Header {
                name: h.name,
                value: h.value,
            });
        }

        let method = if websocket { "GET" } else { method };

        let path = &url[url::Position::BeforePath..];

        if body_size == http1::BodySize::Unknown && !rdata.more {
            body_size = http1::BodySize::Known(rdata.body.len());
        }

        let mut overflow = None;

        let req_header = if websocket {
            req.prepare_header(method, path, &headers, body_size, true, &[], true)?
        } else {
            let (initial_body, end) = if include_body {
                if rdata.body.len() > recv_buf_size {
                    let body = &rdata.body[..recv_buf_size];

                    let mut remainder = ContiguousBuffer::new(rdata.body.len() - body.len());
                    remainder.write_all(&rdata.body[body.len()..])?;

                    debug!(
                        "initial={} overflow={} end={}",
                        body.len(),
                        remainder.len(),
                        !rdata.more
                    );

                    overflow = Some(Overflow {
                        buf: remainder,
                        end: !rdata.more,
                    });

                    (body, false)
                } else {
                    (rdata.body, !rdata.more)
                }
            } else {
                (&[][..], true)
            };

            req.prepare_header(method, path, &headers, body_size, false, initial_body, end)?
        };

        (req_header, ws_key, overflow)
    };

    // send request header

    let req_body = {
        let mut send_header = pin!(req_header.send());

        loop {
            // ABR: select contains read
            let result = select_2(send_header.as_mut(), pin!(zsess_in.recv_msg())).await;

            match result {
                Select2::R1(ret) => break ret?,
                Select2::R2(ret) => {
                    let zreq = ret?;

                    // ABR: handle_other
                    server_handle_other(zreq, zsess_in, zsess_out).await?;
                }
            }
        }
    };

    refresh_stream_timeout();

    // send request body

    // ABR: function contains read
    let resp = server_stream_send_body(
        refresh_stream_timeout,
        req_body,
        overflow,
        recv_buf_size,
        zsess_in,
        zsess_out,
        blocks_max,
        blocks_avail,
    )
    .await?;

    // receive response header

    let (resp_body, ws_config) = {
        let mut scratch = http1::ParseScratch::<HEADERS_MAX>::new();
        let mut recv_header = pin!(resp.recv_header(&mut scratch));

        let (resp, resp_body) = loop {
            // ABR: select contains read
            let result = select_2(recv_header.as_mut(), pin!(zsess_in.recv_msg())).await;

            match result {
                Select2::R1(ret) => break ret?,
                Select2::R2(ret) => {
                    let zreq = ret?;

                    // ABR: handle_other
                    server_handle_other(zreq, zsess_in, zsess_out).await?;
                }
            }
        };

        let ws_config = {
            let resp_ref = resp.get();

            debug!(
                "client-conn {}: response: {} {}",
                log_id, resp_ref.code, resp_ref.reason
            );

            loop {
                // ABR: select contains read
                let result =
                    select_2(pin!(zsess_out.check_send()), pin!(zsess_in.recv_msg())).await;

                match result {
                    Select2::R1(()) => break,
                    Select2::R2(ret) => {
                        let zreq = ret?;

                        // ABR: handle_other
                        server_handle_other(zreq, zsess_in, zsess_out).await?;
                    }
                }
            }

            if follow_redirects {
                let schemes = if ws_key.is_some() {
                    ["ws", "wss"]
                } else {
                    ["http", "https"]
                };

                if let Some((url, use_get)) = check_redirect(method, url, &resp_ref, &schemes) {
                    // eat response body
                    let finished = loop {
                        let ret = {
                            let mut buf = [0; 4_096];
                            resp_body.try_recv(&mut buf)?
                        };

                        match ret {
                            RecvStatus::Complete(finished, _) => break finished,
                            RecvStatus::Read((), written) => {
                                if written == 0 {
                                    let mut add_to_buffer = pin!(resp_body.add_to_buffer());

                                    loop {
                                        // ABR: select contains read
                                        let result = select_2(
                                            add_to_buffer.as_mut(),
                                            pin!(zsess_in.recv_msg()),
                                        )
                                        .await;

                                        match result {
                                            Select2::R1(ret) => {
                                                ret?;
                                                break;
                                            }
                                            Select2::R2(ret) => {
                                                let zreq = ret?;

                                                // ABR: handle_other
                                                server_handle_other(zreq, zsess_in, zsess_out)
                                                    .await?;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    };

                    let finished = finished.discard_header(resp);

                    debug!("client-conn {}: redirecting to {}", log_id, url);

                    return Ok(ClientHandlerDone::Redirect(
                        finished.is_persistent(),
                        url,
                        use_get,
                    ));
                }
            }

            let mut zheaders = ArrayVec::<zhttppacket::Header, HEADERS_MAX>::new();

            let mut ws_accept = None;
            let mut ws_deflate_config = None;

            for h in resp_ref.headers {
                if ws_key.is_some() {
                    if h.name.eq_ignore_ascii_case("Sec-WebSocket-Accept") {
                        ws_accept = Some(h.value);
                    }

                    if h.name.eq_ignore_ascii_case("Sec-WebSocket-Extensions") {
                        for value in http1::parse_header_value(h.value) {
                            let (name, params) = match value {
                                Ok(v) => v,
                                Err(_) => return Err(Error::InvalidWebSocketResponse),
                            };

                            match name {
                                "permessage-deflate" => {
                                    // we must have offered, and server must
                                    // provide one response at most
                                    if !allow_compression || ws_deflate_config.is_some() {
                                        return Err(Error::InvalidWebSocketResponse);
                                    }

                                    if let Ok(config) =
                                        websocket::PerMessageDeflateConfig::from_params(params)
                                    {
                                        if config.check_response().is_ok() {
                                            // set the encoded buffer to be 25% the size of the
                                            // recv buffer
                                            let enc_buf_size = recv_buf_size / 4;

                                            ws_deflate_config = Some((config, enc_buf_size));
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

                zheaders.push(zhttppacket::Header {
                    name: h.name,
                    value: h.value,
                });
            }

            if let Some(ws_key) = &ws_key {
                if resp_ref.code == 101 {
                    if validate_ws_response(ws_key.as_bytes(), ws_accept).is_err() {
                        return Err(Error::InvalidWebSocketResponse);
                    }
                } else {
                    // websocket request rejected

                    // we need to allocate to collect the response body,
                    // since buf1 holds bytes read from the socket, and
                    // resp is using buf2's inner buffer
                    let mut body_buf = ContiguousBuffer::new(send_buf_size);

                    // receive response body

                    let finished = loop {
                        match resp_body.try_recv(body_buf.write_buf())? {
                            RecvStatus::Complete(finished, written) => {
                                body_buf.write_commit(written);
                                break finished;
                            }
                            RecvStatus::Read((), written) => {
                                body_buf.write_commit(written);

                                if written == 0 {
                                    let mut add_to_buffer = pin!(resp_body.add_to_buffer());

                                    loop {
                                        // ABR: select contains read
                                        let result = select_2(
                                            add_to_buffer.as_mut(),
                                            pin!(zsess_in.recv_msg()),
                                        )
                                        .await;

                                        match result {
                                            Select2::R1(ret) => match ret {
                                                Ok(()) => break,
                                                Err(CoreHttpError::BufferExceeded) => {
                                                    return Err(Error::WebSocketRejectionTooLarge(
                                                        send_buf_size,
                                                    ));
                                                }
                                                Err(e) => return Err(e.into()),
                                            },
                                            Select2::R2(ret) => {
                                                let zreq = ret?;

                                                // ABR: handle_other
                                                server_handle_other(zreq, zsess_in, zsess_out)
                                                    .await?;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    };

                    let edata = zhttppacket::ResponseErrorData {
                        condition: "rejected",
                        rejected_info: Some(zhttppacket::RejectedInfo {
                            code: resp_ref.code,
                            reason: resp_ref.reason,
                            headers: &zheaders,
                            body: body_buf.read_buf(),
                        }),
                    };

                    let zresp = zhttppacket::Response::new_error(b"", &[], edata);

                    // check_send just finished, so this should succeed
                    zsess_out.try_send_msg(zresp)?;

                    drop(zheaders);

                    let finished = finished.discard_header(resp);

                    return Ok(ClientHandlerDone::Complete((), finished.is_persistent()));
                }
            }

            let credits = if ws_key.is_some() {
                // for websockets, provide credits when sending response to handler
                recv_buf_size as u32
            } else {
                // for http, it is not necessary to provide credits when responding
                0
            };

            let rdata = zhttppacket::ResponseData {
                credits,
                more: ws_key.is_none(),
                code: resp_ref.code,
                reason: resp_ref.reason,
                headers: &zheaders,
                content_type: None,
                body: b"",
            };

            let zresp = zhttppacket::Response::new_data(b"", &[], rdata);

            // check_send just finished, so this should succeed
            zsess_out.try_send_msg(zresp)?;

            if ws_key.is_some() {
                Some(ws_deflate_config)
            } else {
                None
            }
        };

        let resp_body = resp_body.discard_header(resp)?;

        (resp_body, ws_config)
    };

    *response_received = true;

    if let Some(deflate_config) = ws_config {
        // handle as websocket connection

        // ABR: function contains read
        server_stream_websocket(
            log_id,
            stream,
            buf1,
            buf2,
            blocks_max,
            blocks_avail,
            messages_max,
            tmp_buf,
            refresh_stream_timeout,
            deflate_config,
            zsess_in,
            zsess_out,
        )
        .await?;

        Ok(ClientHandlerDone::Complete((), false))
    } else {
        // receive response body

        // ABR: function contains read
        let finished = server_stream_recv_body(
            tmp_buf,
            refresh_stream_timeout,
            resp_body,
            zsess_in,
            zsess_out,
        )
        .await?;

        Ok(ClientHandlerDone::Complete((), finished.is_persistent()))
    }
}

#[allow(clippy::too_many_arguments)]
async fn client_stream_connect<E, R1, R2>(
    log_id: &str,
    id: &[u8],
    zreq: arena::Rc<zhttppacket::OwnedRequest>,
    buf1: &mut VecRingBuffer,
    buf2: &mut VecRingBuffer,
    buffer_size: usize,
    blocks_max: usize,
    blocks_avail: &Counter,
    messages_max: usize,
    allow_compression: bool,
    packet_buf: &RefCell<Vec<u8>>,
    tmp_buf: &RefCell<Vec<u8>>,
    deny: &[IpNet],
    instance_id: &str,
    resolver: &resolver::Resolver,
    pool: &ConnectionPool,
    zreceiver: &TrackedAsyncLocalReceiver<'_, (arena::Rc<zhttppacket::OwnedRequest>, usize)>,
    zsender: &AsyncLocalSender<zmq::Message>,
    shared: &StreamSharedData,
    enable_routing: &E,
    response_received: &mut bool,
    refresh_stream_timeout: &R1,
    refresh_session_timeout: &R2,
) -> Result<(), Error>
where
    E: Fn(),
    R1: Fn(),
    R2: Fn(),
{
    let zreq = zreq.get().get();

    // assign address so we can send replies
    let addr: ArrayVec<u8, 64> = match ArrayVec::try_from(zreq.from) {
        Ok(v) => v,
        Err(_) => return Err(Error::BadRequest),
    };

    shared.set_to_addr(Some(addr));

    let rdata = match &zreq.ptype {
        zhttppacket::RequestPacket::Data(data) => data,
        _ => return Err(Error::BadRequest),
    };

    let initial_url = match url::Url::parse(rdata.uri) {
        Ok(url) => url,
        Err(_) => return Err(Error::BadRequest),
    };

    // must be an http or websocket url
    if !["http", "https", "ws", "wss"].contains(&initial_url.scheme()) {
        return Err(Error::BadRequest);
    }

    // http requests must have a method
    if ["http", "https"].contains(&initial_url.scheme()) && rdata.method.is_empty() {
        return Err(Error::BadRequest);
    }

    let method = if !rdata.method.is_empty() {
        rdata.method
    } else {
        "_"
    };

    debug!("client-conn {}: request: {} {}", log_id, method, rdata.uri);

    let zsess_out = ZhttpServerStreamSessionOut::new(instance_id, id, packet_buf, zsender, shared);

    // ack request

    // ABR: discard_while
    server_discard_while(
        zreceiver,
        pin!(async {
            zsess_out.check_send().await;
            Ok(())
        }),
    )
    .await?;

    zsess_out.try_send_msg(zhttppacket::Response::new_keep_alive(b"", &[]))?;

    let mut zsess_in = ZhttpServerStreamSessionIn::new(
        log_id,
        id,
        rdata.credits,
        zreceiver,
        shared,
        refresh_session_timeout,
    );

    // allow receiving subsequent messages
    enable_routing();

    let deny = if rdata.ignore_policies { &[] } else { deny };

    let mut last_redirect: Option<(url::Url, bool)> = None;
    let mut redirect_count = 0;

    loop {
        let (method, url, include_body) = match &last_redirect {
            Some((url, use_get)) => {
                let (method, include_body) = if *use_get {
                    ("GET", false)
                } else {
                    (rdata.method, true)
                };

                (method, url, include_body)
            }
            None => (rdata.method, &initial_url, true),
        };

        let url_host = match url.host_str() {
            Some(s) => s,
            None => return Err(Error::BadRequest),
        };

        let tls_waker_data = RefWakerData::new(TlsWaker::new());

        let (peer_addr, using_tls, mut stream) = {
            let mut client_connect = pin!(client_connect(
                log_id,
                rdata,
                url,
                resolver,
                deny,
                pool,
                &tls_waker_data
            ));

            loop {
                // ABR: select contains read
                let ret = select_2(client_connect.as_mut(), pin!(zsess_in.recv_msg())).await;

                match ret {
                    Select2::R1(ret) => break ret?,
                    Select2::R2(ret) => {
                        let zreq = ret?;

                        // ABR: handle_other
                        server_handle_other(zreq, &mut zsess_in, &zsess_out).await?;
                    }
                }
            }
        };

        let done = match &mut stream {
            AsyncStream::Plain(stream) => {
                client_stream_handler(
                    log_id,
                    stream,
                    zreq,
                    method,
                    url,
                    include_body,
                    rdata.follow_redirects,
                    buf1,
                    buf2,
                    blocks_max,
                    blocks_avail,
                    messages_max,
                    allow_compression,
                    tmp_buf,
                    &mut zsess_in,
                    &zsess_out,
                    response_received,
                    refresh_stream_timeout,
                )
                .await?
            }
            AsyncStream::Tls(stream) => {
                client_stream_handler(
                    log_id,
                    stream,
                    zreq,
                    method,
                    url,
                    include_body,
                    rdata.follow_redirects,
                    buf1,
                    buf2,
                    blocks_max,
                    blocks_avail,
                    messages_max,
                    allow_compression,
                    tmp_buf,
                    &mut zsess_in,
                    &zsess_out,
                    response_received,
                    refresh_stream_timeout,
                )
                .await?
            }
        };

        if done.is_persistent() {
            let additional_blocks = (buf2.capacity() / buffer_size) - 1;

            buf2.resize(buffer_size);

            blocks_avail.inc(additional_blocks).unwrap();

            if pool
                .push(
                    peer_addr,
                    using_tls,
                    url_host.to_string(),
                    stream.into_inner(),
                    CONNECTION_POOL_TTL,
                )
                .is_ok()
            {
                debug!("client-conn {}: leaving connection intact", log_id);
            }
        }

        match done {
            ClientHandlerDone::Complete((), _) => break,
            ClientHandlerDone::Redirect(_, url, mut use_get) => {
                if redirect_count >= REDIRECTS_MAX {
                    return Err(Error::TooManyRedirects);
                }

                redirect_count += 1;

                if let Some((_, b)) = &last_redirect {
                    use_get = use_get || *b;
                }

                last_redirect = Some((url, use_get));
            }
        }
    }

    Ok(())
}

#[allow(clippy::too_many_arguments)]
async fn client_stream_connection_inner<E>(
    token: CancellationToken,
    log_id: &str,
    id: &[u8],
    zreq: arena::Rc<zhttppacket::OwnedRequest>,
    buffer_size: usize,
    blocks_max: usize,
    blocks_avail: &Counter,
    messages_max: usize,
    rb_tmp: &Rc<TmpBuffer>,
    packet_buf: Rc<RefCell<Vec<u8>>>,
    tmp_buf: Rc<RefCell<Vec<u8>>>,
    stream_timeout_duration: Duration,
    allow_compression: bool,
    deny: &[IpNet],
    instance_id: &str,
    resolver: &resolver::Resolver,
    pool: &ConnectionPool,
    zreceiver: &TrackedAsyncLocalReceiver<'_, (arena::Rc<zhttppacket::OwnedRequest>, usize)>,
    zsender: AsyncLocalSender<zmq::Message>,
    shared: arena::Rc<StreamSharedData>,
    enable_routing: &E,
) -> Result<(), Error>
where
    E: Fn(),
{
    let reactor = Reactor::current().unwrap();

    let mut buf1 = VecRingBuffer::new(buffer_size, rb_tmp);
    let mut buf2 = VecRingBuffer::new(buffer_size, rb_tmp);

    let stream_timeout = Timeout::new(reactor.now() + stream_timeout_duration);
    let session_timeout = Timeout::new(reactor.now() + ZHTTP_SESSION_TIMEOUT);

    let refresh_stream_timeout = || {
        stream_timeout.set_deadline(reactor.now() + stream_timeout_duration);
    };

    let refresh_session_timeout = || {
        session_timeout.set_deadline(reactor.now() + ZHTTP_SESSION_TIMEOUT);
    };

    let mut response_received = false;

    let ret = {
        let handler = pin!(client_stream_connect(
            log_id,
            id,
            zreq,
            &mut buf1,
            &mut buf2,
            buffer_size,
            blocks_max,
            blocks_avail,
            messages_max,
            allow_compression,
            &packet_buf,
            &tmp_buf,
            deny,
            instance_id,
            resolver,
            pool,
            zreceiver,
            &zsender,
            shared.get(),
            enable_routing,
            &mut response_received,
            &refresh_stream_timeout,
            &refresh_session_timeout,
        ));

        match select_4(
            handler,
            stream_timeout.elapsed(),
            session_timeout.elapsed(),
            token.cancelled(),
        )
        .await
        {
            Select4::R1(ret) => ret,
            Select4::R2(_) => Err(Error::StreamTimeout),
            Select4::R3(_) => return Err(Error::SessionTimeout),
            Select4::R4(_) => return Err(Error::Stopped),
        }
    };

    match ret {
        Ok(()) => {}
        Err(e) => {
            let handler_caused = matches!(
                &e,
                Error::BadMessage | Error::Handler | Error::HandlerCancel
            );

            if !handler_caused {
                let shared = shared.get();

                let msg = if let Some(addr) = shared.to_addr().get() {
                    let mut zresp = if response_received {
                        zhttppacket::Response::new_cancel(b"", &[])
                    } else {
                        zhttppacket::Response::new_error(
                            b"",
                            &[],
                            zhttppacket::ResponseErrorData {
                                condition: e.to_condition(),
                                rejected_info: None,
                            },
                        )
                    };

                    let ids = [zhttppacket::Id {
                        id,
                        seq: Some(shared.out_seq()),
                    }];

                    zresp.from = instance_id.as_bytes();
                    zresp.ids = &ids;
                    zresp.multi = true;

                    let packet_buf = &mut *packet_buf.borrow_mut();

                    let msg = make_zhttp_response(addr, zresp, packet_buf)?;

                    Some(msg)
                } else {
                    None
                };

                if let Some(msg) = msg {
                    // best effort
                    let _ = zsender.try_send(msg);

                    shared.inc_out_seq();
                }
            }

            return Err(e);
        }
    }

    Ok(())
}

#[allow(clippy::too_many_arguments)]
pub async fn client_stream_connection<E>(
    token: CancellationToken,
    log_id: &str,
    id: &[u8],
    zreq: arena::Rc<zhttppacket::OwnedRequest>,
    buffer_size: usize,
    blocks_max: usize,
    blocks_avail: &Counter,
    messages_max: usize,
    rb_tmp: &Rc<TmpBuffer>,
    packet_buf: Rc<RefCell<Vec<u8>>>,
    tmp_buf: Rc<RefCell<Vec<u8>>>,
    timeout: Duration,
    allow_compression: bool,
    deny: &[IpNet],
    instance_id: &str,
    resolver: &resolver::Resolver,
    pool: &ConnectionPool,
    zreceiver: AsyncLocalReceiver<(arena::Rc<zhttppacket::OwnedRequest>, usize)>,
    zsender: AsyncLocalSender<zmq::Message>,
    shared: arena::Rc<StreamSharedData>,
    enable_routing: &E,
) where
    E: Fn(),
{
    let value_active = TrackFlag::default();

    let zreceiver = TrackedAsyncLocalReceiver::new(zreceiver, &value_active);

    match track_future(
        client_stream_connection_inner(
            token,
            log_id,
            id,
            zreq,
            buffer_size,
            blocks_max,
            blocks_avail,
            messages_max,
            rb_tmp,
            packet_buf,
            tmp_buf,
            timeout,
            allow_compression,
            deny,
            instance_id,
            resolver,
            pool,
            &zreceiver,
            zsender,
            shared,
            enable_routing,
        ),
        &value_active,
    )
    .await
    {
        Ok(()) => debug!("client-conn {}: finished", log_id),
        Err(e) => log!(
            e.log_level(),
            "client-conn {}: process error: {:?}",
            log_id,
            e
        ),
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
    use std::rc::Rc;
    use std::sync::Arc;
    use std::task::{Context, Poll, Waker};
    use std::time::Instant;

    pub struct NoopWaker {}

    #[allow(clippy::new_without_default)]
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

    #[allow(clippy::new_without_default)]
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

        pub fn clear_write_allowed(&mut self) {
            self.out_allow = 0;
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
            if !buf.is_empty() && self.out_allow == 0 {
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
                if !buf.is_empty() && self.out_allow == 0 {
                    if total == 0 {
                        return Err(io::Error::from(io::ErrorKind::WouldBlock));
                    }

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

        fn poll_write_vectored(
            self: Pin<&mut Self>,
            _cx: &mut Context,
            bufs: &[io::IoSlice],
        ) -> Poll<Result<usize, io::Error>> {
            let inner = &mut *self.inner.borrow_mut();

            match inner.write_vectored(bufs) {
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

    #[allow(clippy::too_many_arguments)]
    async fn server_req_handler_fut(
        sock: Rc<RefCell<FakeSock>>,
        secure: bool,
        s_from_conn: channel::LocalSender<zmq::Message>,
        r_to_conn: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
        packet_buf: Rc<RefCell<Vec<u8>>>,
        buf1: &mut VecRingBuffer,
        buf2: &mut VecRingBuffer,
        body_buf: &mut ContiguousBuffer,
    ) -> Result<bool, Error> {
        let mut sock = AsyncFakeSock::new(sock);

        let f = TrackFlag::default();

        let r_to_conn = TrackedAsyncLocalReceiver::new(AsyncLocalReceiver::new(r_to_conn), &f);
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
        buf1: VecRingBuffer,
        buf2: VecRingBuffer,
        body_buf: ContiguousBuffer,
    }

    pub struct BenchServerReqHandler {
        reactor: Reactor,
        msg_mem: Arc<arena::ArcMemory<zmq::Message>>,
        scratch_mem: Rc<arena::RcMemory<RefCell<zhttppacket::ParseScratch<'static>>>>,
        resp_mem: Rc<arena::RcMemory<zhttppacket::OwnedResponse>>,
        rb_tmp: Rc<TmpBuffer>,
        packet_buf: Rc<RefCell<Vec<u8>>>,
    }

    #[allow(clippy::new_without_default)]
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
                buf1: VecRingBuffer::new(buffer_size, &self.rb_tmp),
                buf2: VecRingBuffer::new(buffer_size, &self.rb_tmp),
                body_buf: ContiguousBuffer::new(buffer_size),
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

            assert!(s_to_conn.try_send((resp, 0)).is_ok());

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
    ) -> Result<(), Error> {
        let mut cid = ArrayString::from_str("1").unwrap();
        let mut cid_provider = SimpleCidProvider { cid };

        let sock = AsyncFakeSock::new(sock);

        let f = TrackFlag::default();

        let r_to_conn = TrackedAsyncLocalReceiver::new(AsyncLocalReceiver::new(r_to_conn), &f);
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

    #[allow(clippy::new_without_default)]
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

            assert!(s_to_conn.try_send((resp, 0)).is_ok());

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

    #[allow(clippy::too_many_arguments)]
    async fn server_stream_handler_fut(
        sock: Rc<RefCell<FakeSock>>,
        secure: bool,
        s_from_conn: channel::LocalSender<zmq::Message>,
        s_stream_from_conn: channel::LocalSender<(ArrayVec<u8, 64>, zmq::Message)>,
        r_to_conn: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
        packet_buf: Rc<RefCell<Vec<u8>>>,
        tmp_buf: Rc<RefCell<Vec<u8>>>,
        buf1: &mut VecRingBuffer,
        buf2: &mut VecRingBuffer,
        shared: arena::Rc<StreamSharedData>,
    ) -> Result<bool, Error> {
        let mut sock = AsyncFakeSock::new(sock);

        let f = TrackFlag::default();

        let r_to_conn = TrackedAsyncLocalReceiver::new(AsyncLocalReceiver::new(r_to_conn), &f);
        let s_from_conn = AsyncLocalSender::new(s_from_conn);
        let s_stream_from_conn = AsyncLocalSender::new(s_stream_from_conn);

        server_stream_handler(
            "1",
            &mut sock,
            None,
            secure,
            buf1,
            buf2,
            2,
            &Counter::new(0),
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
        buf1: VecRingBuffer,
        buf2: VecRingBuffer,
    }

    pub struct BenchServerStreamHandler {
        reactor: Reactor,
        msg_mem: Arc<arena::ArcMemory<zmq::Message>>,
        scratch_mem: Rc<arena::RcMemory<RefCell<zhttppacket::ParseScratch<'static>>>>,
        resp_mem: Rc<arena::RcMemory<zhttppacket::OwnedResponse>>,
        shared_mem: Rc<arena::RcMemory<StreamSharedData>>,
        rb_tmp: Rc<TmpBuffer>,
        packet_buf: Rc<RefCell<Vec<u8>>>,
        tmp_buf: Rc<RefCell<Vec<u8>>>,
    }

    #[allow(clippy::new_without_default)]
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
                buf1: VecRingBuffer::new(buffer_size, &self.rb_tmp),
                buf2: VecRingBuffer::new(buffer_size, &self.rb_tmp),
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
                let shared = arena::Rc::new(StreamSharedData::new(), shared_mem).unwrap();

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

            let mut executor = StepExecutor::new(reactor, fut);

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
            let msg = arena::Arc::new(msg, msg_mem).unwrap();

            let scratch =
                arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), scratch_mem)
                    .unwrap();

            let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
            let resp = arena::Rc::new(resp, resp_mem).unwrap();

            assert!(s_to_conn.try_send((resp, 0)).is_ok());

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

    #[allow(clippy::too_many_arguments)]
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
        shared: arena::Rc<StreamSharedData>,
    ) -> Result<(), Error> {
        let mut cid = ArrayString::from_str("1").unwrap();
        let mut cid_provider = SimpleCidProvider { cid };

        let sock = AsyncFakeSock::new(sock);

        let f = TrackFlag::default();

        let r_to_conn = TrackedAsyncLocalReceiver::new(AsyncLocalReceiver::new(r_to_conn), &f);
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
            2,
            &Counter::new(0),
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
        shared_mem: Rc<arena::RcMemory<StreamSharedData>>,
        rb_tmp: Rc<TmpBuffer>,
        packet_buf: Rc<RefCell<Vec<u8>>>,
        tmp_buf: Rc<RefCell<Vec<u8>>>,
    }

    #[allow(clippy::new_without_default)]
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
                let shared = arena::Rc::new(StreamSharedData::new(), shared_mem).unwrap();

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

            let mut executor = StepExecutor::new(reactor, fut);

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
            let msg = arena::Arc::new(msg, msg_mem).unwrap();

            let scratch =
                arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), scratch_mem)
                    .unwrap();

            let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
            let resp = arena::Rc::new(resp, resp_mem).unwrap();

            assert!(s_to_conn.try_send((resp, 0)).is_ok());

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
    use test_log::test;

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

        t.start(websocket::OPCODE_TEXT).unwrap();
        assert_eq!(t.in_progress(), true);
        assert_eq!(t.current(), Some((websocket::OPCODE_TEXT, 3, true)));

        t.consumed(3, false);
        assert_eq!(t.current(), Some((websocket::OPCODE_TEXT, 0, true)));

        t.consumed(0, true);
        assert_eq!(t.current(), Some((websocket::OPCODE_TEXT, 0, false)));

        t.done();
        assert_eq!(t.current(), Some((websocket::OPCODE_TEXT, 0, true)));

        t.consumed(0, true);
        assert_eq!(t.current(), None);

        for _ in 0..t.items.capacity() {
            t.start(websocket::OPCODE_TEXT).unwrap();
            t.done();
        }
        let r = t.start(websocket::OPCODE_TEXT);
        assert!(r.is_err());
    }

    async fn server_req_fut(
        token: CancellationToken,
        sock: Rc<RefCell<FakeSock>>,
        secure: bool,
        s_from_conn: channel::LocalSender<zmq::Message>,
        r_to_conn: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    ) -> Result<(), Error> {
        let mut cid = ArrayString::from_str("1").unwrap();
        let mut cid_provider = SimpleCidProvider { cid };

        let sock = AsyncFakeSock::new(sock);

        let f = TrackFlag::default();

        let r_to_conn = TrackedAsyncLocalReceiver::new(AsyncLocalReceiver::new(r_to_conn), &f);
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

            server_req_fut(token, sock, false, s_from_conn, r_to_conn)
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

            server_req_fut(token, sock, false, s_from_conn, r_to_conn)
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

            server_req_fut(token, sock, false, s_from_conn, r_to_conn)
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        assert_eq!(check_poll(executor.step()), None);

        executor.advance_time(now + Duration::from_millis(5_000));

        match executor.step() {
            Poll::Ready(Err(Error::StreamTimeout)) => {}
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

            server_req_fut(token, sock, false, s_from_conn, r_to_conn)
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

            server_req_fut(token, sock, true, s_from_conn, r_to_conn)
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

    async fn server_stream_fut(
        token: CancellationToken,
        sock: Rc<RefCell<FakeSock>>,
        secure: bool,
        allow_compression: bool,
        s_from_conn: channel::LocalSender<zmq::Message>,
        s_stream_from_conn: channel::LocalSender<(ArrayVec<u8, 64>, zmq::Message)>,
        r_to_conn: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedResponse>, usize)>,
    ) -> Result<(), Error> {
        let mut cid = ArrayString::from_str("1").unwrap();
        let mut cid_provider = SimpleCidProvider { cid };

        let sock = AsyncFakeSock::new(sock);

        let f = TrackFlag::default();

        let r_to_conn = TrackedAsyncLocalReceiver::new(AsyncLocalReceiver::new(r_to_conn), &f);
        let s_from_conn = AsyncLocalSender::new(s_from_conn);
        let s_stream_from_conn = AsyncLocalSender::new(s_stream_from_conn);
        let buffer_size = 1024;

        let rb_tmp = Rc::new(TmpBuffer::new(1024));
        let packet_buf = Rc::new(RefCell::new(vec![0; 2048]));
        let tmp_buf = Rc::new(RefCell::new(vec![0; buffer_size]));

        let timeout = Duration::from_millis(5_000);

        let shared_mem = Rc::new(arena::RcMemory::new(1));
        let shared = arena::Rc::new(StreamSharedData::new(), &shared_mem).unwrap();

        server_stream_connection_inner(
            token,
            &mut cid,
            &mut cid_provider,
            sock,
            None,
            secure,
            buffer_size,
            3,
            &Counter::new(1),
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

            server_stream_fut(
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

            server_stream_fut(
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

            server_stream_fut(
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

            server_stream_fut(
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
    fn server_stream_expand_write_buffer() {
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

            server_stream_fut(
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

        let req_data =
            concat!("GET /path HTTP/1.1\r\n", "Host: example.com\r\n", "\r\n").as_bytes();

        sock.borrow_mut().add_readable(req_data);

        assert_eq!(check_poll(executor.step()), None);

        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert!(r_from_conn.try_recv().is_err());

        let buf = &msg[..];

        let expected = concat!(
            "T179:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:multi,4:t",
            "rue!}6:method,3:GET,3:uri,23:http://example.com/path,7:hea",
            "ders,26:22:4:Host,11:example.com,]]7:credits,4:1024#6:stre",
            "am,4:true!}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        sock.borrow_mut().allow_write(1024);

        let msg = concat!(
            "T125:2:id,1:1,6:reason,2:OK,7:headers,34:30:12:Content-Typ",
            "e,10:text/plain,]]3:seq,1:0#4:from,7:handler,4:code,3:200#",
            "4:more,4:true!}",
        );

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert!(s_to_conn.try_send((resp, 0)).is_ok());

        assert_eq!(check_poll(executor.step()), None);

        let data = sock.borrow_mut().take_writable();

        let expected = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Connection: Transfer-Encoding\r\n",
            "Transfer-Encoding: chunked\r\n",
            "\r\n",
        );

        assert_eq!(str::from_utf8(&data).unwrap(), expected);

        // no more messages yet
        assert!(r_stream_from_conn.try_recv().is_err());

        sock.borrow_mut().clear_write_allowed();

        let body = vec![0; 1024];

        let mut rdata = zhttppacket::ResponseData::new();
        rdata.body = body.as_slice();
        rdata.more = true;

        let resp = zhttppacket::Response::new_data(
            b"handler",
            &[zhttppacket::Id {
                id: b"1",
                seq: Some(1),
            }],
            rdata,
        );

        let mut buf = [0; 2048];
        let size = resp.serialize(&mut buf).unwrap();

        let msg = zmq::Message::from(&buf[..size]);
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert!(s_to_conn.try_send((resp, 0)).is_ok());

        assert_eq!(check_poll(executor.step()), None);

        // read message
        let (_, msg) = r_stream_from_conn.try_recv().unwrap();

        // no other messages
        assert!(r_stream_from_conn.try_recv().is_err());

        let buf = &msg[..];

        let expected = concat!(
            "T91:4:from,4:test,2:id,1:1,3:seq,1:1#3:ext,15:5:multi,4:tr",
            "ue!}4:type,6:credit,7:credits,4:1024#}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);
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

            server_stream_fut(
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

            server_stream_fut(
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
            "T309:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:multi,4:t",
            "rue!}6:method,3:GET,3:uri,21:ws://example.com/path,7:heade",
            "rs,173:22:4:Host,11:example.com,]22:7:Upgrade,9:websocket,",
            "]30:21:Sec-WebSocket-Version,2:13,]29:17:Sec-WebSocket-Key",
            ",5:abcde,]50:24:Sec-WebSocket-Extensions,18:permessage-def",
            "late,]]7:credits,4:1024#}",
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
    fn server_websocket_expand_write_buffer() {
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

            server_stream_fut(
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

        sock.borrow_mut().clear_write_allowed();

        let body = vec![0; 1024];

        let mut rdata = zhttppacket::ResponseData::new();
        rdata.body = body.as_slice();
        rdata.content_type = Some(zhttppacket::ContentType::Text);

        let resp = zhttppacket::Response::new_data(
            b"handler",
            &[zhttppacket::Id {
                id: b"1",
                seq: Some(1),
            }],
            rdata,
        );

        let mut buf = [0; 2048];
        let size = resp.serialize(&mut buf).unwrap();

        let msg = zmq::Message::from(&buf[..size]);
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let resp = zhttppacket::OwnedResponse::parse(msg, 0, scratch).unwrap();
        let resp = arena::Rc::new(resp, &resp_mem).unwrap();

        assert!(s_to_conn.try_send((resp, 0)).is_ok());

        assert_eq!(check_poll(executor.step()), None);

        // read message
        let (_, msg) = r_stream_from_conn.try_recv().unwrap();

        // no other messages
        assert!(r_stream_from_conn.try_recv().is_err());

        let buf = &msg[..];

        let expected = concat!(
            "T91:4:from,4:test,2:id,1:1,3:seq,1:1#3:ext,15:5:multi,4:tr",
            "ue!}4:type,6:credit,7:credits,4:1024#}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);
    }

    async fn client_req_fut(
        id: Option<Vec<u8>>,
        zreq: arena::Rc<zhttppacket::OwnedRequest>,
        sock: Rc<RefCell<FakeSock>>,
        s_from_conn: channel::LocalSender<zmq::Message>,
    ) -> Result<(), Error> {
        let mut sock = AsyncFakeSock::new(sock);

        let s_from_conn = AsyncLocalSender::new(s_from_conn);
        let buffer_size = 1024;

        let rb_tmp = Rc::new(TmpBuffer::new(buffer_size));

        let mut buf1 = VecRingBuffer::new(buffer_size, &rb_tmp);
        let mut buf2 = VecRingBuffer::new(buffer_size, &rb_tmp);
        let mut body_buf = ContiguousBuffer::new(buffer_size);
        let packet_buf = RefCell::new(vec![0; 2048]);

        let zreq = zreq.get().get();

        let rdata = match &zreq.ptype {
            zhttppacket::RequestPacket::Data(rdata) => rdata,
            _ => panic!("unexpected init packet"),
        };

        let url = url::Url::parse(rdata.uri).unwrap();

        let msg = match client_req_handler(
            "test",
            id.as_deref(),
            &mut sock,
            zreq,
            rdata.method,
            &url,
            true,
            false,
            &mut buf1,
            &mut buf2,
            &mut body_buf,
            &packet_buf,
        )
        .await?
        {
            ClientHandlerDone::Complete(r, _) => r,
            ClientHandlerDone::Redirect(_, _, _) => panic!("unexpected redirect"),
        };

        s_from_conn.send(msg).await?;

        Ok(())
    }

    #[test]
    fn client_req_without_id() {
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(1));
        let scratch_mem = Rc::new(arena::RcMemory::new(1));
        let req_mem = Rc::new(arena::RcMemory::new(1));

        let data = concat!(
            "T74:7:headers,16:12:3:Foo,3:Bar,]]3:uri,19:https://example.co",
            "m,6:method,3:GET,}",
        )
        .as_bytes();

        let msg = zmq::Message::from(data);
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let zreq = zhttppacket::OwnedRequest::parse(msg, 0, scratch).unwrap();
        let zreq = arena::Rc::new(zreq, &req_mem).unwrap();

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();
            let s_from_conn = s_from_conn
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            client_req_fut(None, zreq, sock, s_from_conn)
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        assert_eq!(check_poll(executor.step()), None);

        // no messages yet
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // fill the handler's outbound message queue
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_ok(), true);
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_err(), true);
        drop(s_from_conn);

        // no data yet
        assert_eq!(sock.borrow_mut().take_writable().is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        assert_eq!(check_poll(executor.step()), None);

        let expected = concat!(
            "GET / HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Foo: Bar\r\n",
            "\r\n",
        );

        let buf = sock.borrow_mut().take_writable();

        assert_eq!(str::from_utf8(&buf).unwrap(), expected);

        // handler won't be able to send a message yet
        assert_eq!(check_poll(executor.step()), None);

        // read bogus message
        let msg = r_from_conn.try_recv().unwrap();
        assert_eq!(msg.is_empty(), true);

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let resp_data = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n",
        )
        .as_bytes();

        sock.borrow_mut().add_readable(resp_data);

        // now handler will be able to send a message and finish
        assert_eq!(check_poll(executor.step()), Some(()));

        // read real message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "T117:4:code,3:200#6:reason,2:OK,7:headers,60:30:12:Content",
            "-Type,10:text/plain,]22:14:Content-Length,1:6,]]4:body,6:h",
            "ello\n,}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);
    }

    #[test]
    fn client_req_with_id() {
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(1));
        let scratch_mem = Rc::new(arena::RcMemory::new(1));
        let req_mem = Rc::new(arena::RcMemory::new(1));

        let data = concat!(
            "T83:7:headers,16:12:3:Foo,3:Bar,]]3:uri,19:https://example.co",
            "m,6:method,3:GET,2:id,1:1,}",
        )
        .as_bytes();

        let msg = zmq::Message::from(data);
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let zreq = zhttppacket::OwnedRequest::parse(msg, 0, scratch).unwrap();
        let zreq = arena::Rc::new(zreq, &req_mem).unwrap();

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();
            let s_from_conn = s_from_conn
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            client_req_fut(Some(b"1".to_vec()), zreq, sock, s_from_conn)
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        assert_eq!(check_poll(executor.step()), None);

        // no messages yet
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // fill the handler's outbound message queue
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_ok(), true);
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_err(), true);
        drop(s_from_conn);

        // no data yet
        assert_eq!(sock.borrow_mut().take_writable().is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        assert_eq!(check_poll(executor.step()), None);

        let expected = concat!(
            "GET / HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Foo: Bar\r\n",
            "\r\n",
        );

        let buf = sock.borrow_mut().take_writable();

        assert_eq!(str::from_utf8(&buf).unwrap(), expected);

        // handler won't be able to send a message yet
        assert_eq!(check_poll(executor.step()), None);

        // read bogus message
        let msg = r_from_conn.try_recv().unwrap();
        assert_eq!(msg.is_empty(), true);

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let resp_data = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n",
        )
        .as_bytes();

        sock.borrow_mut().add_readable(resp_data);

        // now handler will be able to send a message and finish
        assert_eq!(check_poll(executor.step()), Some(()));

        // read real message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "T126:2:id,1:1,4:code,3:200#6:reason,2:OK,7:headers,60:30:1",
            "2:Content-Type,10:text/plain,]22:14:Content-Length,1:6,]]4",
            ":body,6:hello\n,}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);
    }

    async fn client_stream_fut(
        id: Vec<u8>,
        zreq: arena::Rc<zhttppacket::OwnedRequest>,
        sock: Rc<RefCell<FakeSock>>,
        allow_compression: bool,
        r_to_conn: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedRequest>, usize)>,
        s_from_conn: channel::LocalSender<zmq::Message>,
        shared: arena::Rc<StreamSharedData>,
    ) -> Result<(), Error> {
        let mut sock = AsyncFakeSock::new(sock);

        let f = TrackFlag::default();

        let r_to_conn = TrackedAsyncLocalReceiver::new(AsyncLocalReceiver::new(r_to_conn), &f);
        let s_from_conn = AsyncLocalSender::new(s_from_conn);
        let buffer_size = 1024;

        let rb_tmp = Rc::new(TmpBuffer::new(buffer_size));

        let mut buf1 = VecRingBuffer::new(buffer_size, &rb_tmp);
        let mut buf2 = VecRingBuffer::new(buffer_size, &rb_tmp);
        let packet_buf = RefCell::new(vec![0; 2048]);
        let tmp_buf = Rc::new(RefCell::new(vec![0; buffer_size]));

        let mut response_received = false;

        let refresh_stream_timeout = || {};
        let refresh_session_timeout = || {};

        let zreq = zreq.get().get();

        let rdata = match &zreq.ptype {
            zhttppacket::RequestPacket::Data(rdata) => rdata,
            _ => panic!("unexpected init packet"),
        };

        let url = url::Url::parse(rdata.uri).unwrap();

        let log_id = "test";
        let instance_id = "test";

        let zsess_out = ZhttpServerStreamSessionOut::new(
            instance_id,
            &id,
            &packet_buf,
            &s_from_conn,
            shared.get(),
        );

        zsess_out.check_send().await;

        zsess_out.try_send_msg(zhttppacket::Response::new_keep_alive(b"", &[]))?;

        let mut zsess_in = ZhttpServerStreamSessionIn::new(
            log_id,
            &id,
            rdata.credits,
            &r_to_conn,
            shared.get(),
            &refresh_session_timeout,
        );

        let _persistent = client_stream_handler(
            "test",
            &mut sock,
            zreq,
            rdata.method,
            &url,
            true,
            false,
            &mut buf1,
            &mut buf2,
            3,
            &Counter::new(1),
            10,
            allow_compression,
            &tmp_buf,
            &mut zsess_in,
            &zsess_out,
            &mut response_received,
            &refresh_stream_timeout,
        )
        .await?;

        Ok(())
    }

    #[test]
    fn client_stream() {
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(2));
        let scratch_mem = Rc::new(arena::RcMemory::new(2));
        let req_mem = Rc::new(arena::RcMemory::new(2));

        let data = concat!(
            "T165:7:credits,4:1024#4:more,4:true!7:headers,34:30:12:Conten",
            "t-Type,10:text/plain,]]3:uri,24:https://example.com/path,6:me",
            "thod,4:POST,3:seq,1:0#2:id,1:1,4:from,7:handler,}",
        )
        .as_bytes();

        let msg = zmq::Message::from(data);
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let zreq = zhttppacket::OwnedRequest::parse(msg, 0, scratch).unwrap();
        let zreq = arena::Rc::new(zreq, &req_mem).unwrap();

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();
            let s_from_conn = s_from_conn
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            let shared_mem = Rc::new(arena::RcMemory::new(1));
            let shared = arena::Rc::new(StreamSharedData::new(), &shared_mem).unwrap();
            let addr = ArrayVec::try_from(b"handler".as_slice()).unwrap();
            shared.get().set_to_addr(Some(addr));

            client_stream_fut(
                b"1".to_vec(),
                zreq,
                sock,
                false,
                r_to_conn,
                s_from_conn,
                shared,
            )
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        // fill the handler's outbound message queue
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_ok(), true);
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_err(), true);
        drop(s_from_conn);

        // handler won't be able to send a message yet
        assert_eq!(check_poll(executor.step()), None);

        // read bogus message
        let msg = r_from_conn.try_recv().unwrap();
        assert_eq!(msg.is_empty(), true);

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // now handler will be able to send a message
        assert_eq!(check_poll(executor.step()), None);

        // read real message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "handler T79:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:mu",
            "lti,4:true!}4:type,10:keep-alive,}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        // no data yet
        assert_eq!(sock.borrow_mut().take_writable().is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        assert_eq!(check_poll(executor.step()), None);

        let expected = concat!(
            "POST /path HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Content-Type: text/plain\r\n",
            "Connection: Transfer-Encoding\r\n",
            "Transfer-Encoding: chunked\r\n",
            "\r\n",
        );

        let buf = sock.borrow_mut().take_writable();

        assert_eq!(str::from_utf8(&buf).unwrap(), expected);

        // read message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "handler T91:4:from,4:test,2:id,1:1,3:seq,1:1#3:ext,15:5:mu",
            "lti,4:true!}4:type,6:credit,7:credits,4:1024#}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let msg = concat!("T52:3:seq,1:1#2:id,1:1,4:from,7:handler,4:body,6:hello\n,}");

        let msg = zmq::Message::from(msg.as_bytes());
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let req = zhttppacket::OwnedRequest::parse(msg, 0, scratch).unwrap();
        let req = arena::Rc::new(req, &req_mem).unwrap();

        assert_eq!(s_to_conn.try_send((req, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let expected = concat!("6\r\nhello\n\r\n0\r\n\r\n",);

        let buf = sock.borrow_mut().take_writable();

        assert_eq!(str::from_utf8(&buf).unwrap(), expected);

        assert_eq!(check_poll(executor.step()), None);

        // no more messages yet
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let resp_data = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n",
        )
        .as_bytes();

        sock.borrow_mut().add_readable(resp_data);

        assert_eq!(check_poll(executor.step()), None);

        // read message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "handler T173:4:from,4:test,2:id,1:1,3:seq,1:2#3:ext,15:5:m",
            "ulti,4:true!}4:code,3:200#6:reason,2:OK,7:headers,60:30:12",
            ":Content-Type,10:text/plain,]22:14:Content-Length,1:6,]]4:",
            "more,4:true!}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        assert_eq!(check_poll(executor.step()), Some(()));

        // read message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "handler T74:4:from,4:test,2:id,1:1,3:seq,1:3#3:ext,15:5:mu",
            "lti,4:true!}4:body,6:hello\n,}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);
    }

    #[test]
    fn client_stream_expand_write_buffer() {
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(2));
        let scratch_mem = Rc::new(arena::RcMemory::new(2));
        let req_mem = Rc::new(arena::RcMemory::new(2));

        let data = concat!(
            "T165:7:credits,4:1024#4:more,4:true!7:headers,34:30:12:Conten",
            "t-Type,10:text/plain,]]3:uri,24:https://example.com/path,6:me",
            "thod,4:POST,3:seq,1:0#2:id,1:1,4:from,7:handler,}",
        )
        .as_bytes();

        let msg = zmq::Message::from(data);
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let zreq = zhttppacket::OwnedRequest::parse(msg, 0, scratch).unwrap();
        let zreq = arena::Rc::new(zreq, &req_mem).unwrap();

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();
            let s_from_conn = s_from_conn
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            let shared_mem = Rc::new(arena::RcMemory::new(1));
            let shared = arena::Rc::new(StreamSharedData::new(), &shared_mem).unwrap();
            let addr = ArrayVec::try_from(b"handler".as_slice()).unwrap();
            shared.get().set_to_addr(Some(addr));

            client_stream_fut(
                b"1".to_vec(),
                zreq,
                sock,
                false,
                r_to_conn,
                s_from_conn,
                shared,
            )
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        assert_eq!(check_poll(executor.step()), None);

        // read message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert!(r_from_conn.try_recv().is_err());

        let buf = &msg[..];

        let expected = concat!(
            "handler T79:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:mu",
            "lti,4:true!}4:type,10:keep-alive,}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        // no data yet
        assert!(sock.borrow_mut().take_writable().is_empty());

        sock.borrow_mut().allow_write(1024);

        assert_eq!(check_poll(executor.step()), None);

        let expected = concat!(
            "POST /path HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Content-Type: text/plain\r\n",
            "Connection: Transfer-Encoding\r\n",
            "Transfer-Encoding: chunked\r\n",
            "\r\n",
        );

        let buf = sock.borrow_mut().take_writable();

        assert_eq!(str::from_utf8(&buf).unwrap(), expected);

        sock.borrow_mut().clear_write_allowed();

        // read message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert!(r_from_conn.try_recv().is_err());

        let buf = &msg[..];

        let expected = concat!(
            "handler T91:4:from,4:test,2:id,1:1,3:seq,1:1#3:ext,15:5:mu",
            "lti,4:true!}4:type,6:credit,7:credits,4:1024#}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        let body = vec![0; 1024];

        let mut rdata = zhttppacket::RequestData::new();
        rdata.body = body.as_slice();
        rdata.more = true;

        let req = zhttppacket::Request::new_data(
            b"handler",
            &[zhttppacket::Id {
                id: b"1",
                seq: Some(1),
            }],
            rdata,
        );

        let mut buf = [0; 2048];
        let size = req.serialize(&mut buf).unwrap();

        let msg = zmq::Message::from(&buf[..size]);
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let req = zhttppacket::OwnedRequest::parse(msg, 0, scratch).unwrap();
        let req = arena::Rc::new(req, &req_mem).unwrap();

        assert!(s_to_conn.try_send((req, 0)).is_ok());

        assert_eq!(check_poll(executor.step()), None);

        // read message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert!(r_from_conn.try_recv().is_err());

        let buf = &msg[..];

        let expected = concat!(
            "handler T91:4:from,4:test,2:id,1:1,3:seq,1:2#3:ext,15:5:mu",
            "lti,4:true!}4:type,6:credit,7:credits,4:1024#}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);
    }

    #[test]
    fn client_websocket() {
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(2));
        let scratch_mem = Rc::new(arena::RcMemory::new(2));
        let req_mem = Rc::new(arena::RcMemory::new(2));

        let data = concat!(
            "T115:7:credits,4:1024#7:headers,16:12:3:Foo,3:Bar,]]3:uri,22:",
            "wss://example.com/path,3:seq,1:0#2:id,1:1,4:from,7:handler,}",
        )
        .as_bytes();

        let msg = zmq::Message::from(data);
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let zreq = zhttppacket::OwnedRequest::parse(msg, 0, scratch).unwrap();
        let zreq = arena::Rc::new(zreq, &req_mem).unwrap();

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();
            let s_from_conn = s_from_conn
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            let shared_mem = Rc::new(arena::RcMemory::new(1));
            let shared = arena::Rc::new(StreamSharedData::new(), &shared_mem).unwrap();
            let addr = ArrayVec::try_from(b"handler".as_slice()).unwrap();
            shared.get().set_to_addr(Some(addr));

            client_stream_fut(
                b"1".to_vec(),
                zreq,
                sock,
                false,
                r_to_conn,
                s_from_conn,
                shared,
            )
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        // fill the handler's outbound message queue
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_ok(), true);
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_err(), true);
        drop(s_from_conn);

        // handler won't be able to send a message yet
        assert_eq!(check_poll(executor.step()), None);

        // read bogus message
        let msg = r_from_conn.try_recv().unwrap();
        assert_eq!(msg.is_empty(), true);

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // now handler will be able to send a message
        assert_eq!(check_poll(executor.step()), None);

        // read real message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "handler T79:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:mu",
            "lti,4:true!}4:type,10:keep-alive,}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        // no data yet
        assert_eq!(sock.borrow_mut().take_writable().is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        assert_eq!(check_poll(executor.step()), None);

        let buf = sock.borrow_mut().take_writable();

        // use httparse to fish out Sec-WebSocket-Key
        let ws_key = {
            let mut headers = [httparse::EMPTY_HEADER; HEADERS_MAX];

            let mut req = httparse::Request::new(&mut headers);

            match req.parse(&buf) {
                Ok(httparse::Status::Complete(_)) => {}
                _ => panic!("unexpected parse status"),
            }

            let mut ws_key = String::new();

            for h in req.headers {
                if h.name.eq_ignore_ascii_case("Sec-WebSocket-Key") {
                    ws_key = String::from_utf8(h.value.to_vec()).unwrap();
                }
            }

            ws_key
        };

        let expected = format!(
            concat!(
                "GET /path HTTP/1.1\r\n",
                "Host: example.com\r\n",
                "Upgrade: websocket\r\n",
                "Connection: Upgrade\r\n",
                "Sec-WebSocket-Version: 13\r\n",
                "Sec-WebSocket-Key: {}\r\n",
                "Foo: Bar\r\n",
                "\r\n",
            ),
            ws_key
        );

        assert_eq!(str::from_utf8(&buf).unwrap(), expected);

        // no more messages yet
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let ws_accept = calculate_ws_accept(ws_key.as_bytes()).unwrap();

        let resp_data = format!(
            concat!(
                "HTTP/1.1 101 Switching Protocols\r\n",
                "Upgrade: websocket\r\n",
                "Connection: Upgrade\r\n",
                "Sec-WebSocket-Accept: {}\r\n",
                "\r\n",
            ),
            ws_accept
        );

        sock.borrow_mut().add_readable(resp_data.as_bytes());

        assert_eq!(check_poll(executor.step()), None);

        // read message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = format!(
            concat!(
                "handler T249:4:from,4:test,2:id,1:1,3:seq,1:1#3:ext,15:5:m",
                "ulti,4:true!}}4:code,3:101#6:reason,19:Switching Protocols",
                ",7:headers,114:22:7:Upgrade,9:websocket,]24:10:Connection,",
                "7:Upgrade,]56:20:Sec-WebSocket-Accept,28:{},]]7:credits,4:",
                "1024#}}",
            ),
            ws_accept
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

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
        let msg = r_from_conn.try_recv().unwrap();

        let buf = &msg[..];

        let expected = concat!(
            "handler T96:4:from,4:test,2:id,1:1,3:seq,1:2#3:ext,15:5:mu",
            "lti,4:true!}12:content-type,4:text,4:body,5:hello,}",
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

        let req = zhttppacket::OwnedRequest::parse(msg, 0, scratch).unwrap();
        let req = arena::Rc::new(req, &req_mem).unwrap();

        assert_eq!(s_to_conn.try_send((req, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let mut data = sock.borrow_mut().take_writable();

        let fi = websocket::read_header(&data).unwrap();
        assert_eq!(fi.fin, true);
        assert_eq!(fi.opcode, websocket::OPCODE_TEXT);
        assert!(data.len() >= fi.payload_offset + fi.payload_size);

        let content = &mut data[fi.payload_offset..(fi.payload_offset + fi.payload_size)];
        websocket::apply_mask(content, fi.mask.unwrap(), 0);
        assert_eq!(str::from_utf8(content).unwrap(), "world");
    }

    #[test]
    fn client_websocket_with_deflate() {
        let reactor = Reactor::new(100);

        let msg_mem = Arc::new(arena::ArcMemory::new(2));
        let scratch_mem = Rc::new(arena::RcMemory::new(2));
        let req_mem = Rc::new(arena::RcMemory::new(2));

        let data = concat!(
            "T115:7:credits,4:1024#7:headers,16:12:3:Foo,3:Bar,]]3:uri,22:",
            "wss://example.com/path,3:seq,1:0#2:id,1:1,4:from,7:handler,}",
        )
        .as_bytes();

        let msg = zmq::Message::from(data);
        let msg = arena::Arc::new(msg, &msg_mem).unwrap();

        let scratch =
            arena::Rc::new(RefCell::new(zhttppacket::ParseScratch::new()), &scratch_mem).unwrap();

        let zreq = zhttppacket::OwnedRequest::parse(msg, 0, scratch).unwrap();
        let zreq = arena::Rc::new(zreq, &req_mem).unwrap();

        let sock = Rc::new(RefCell::new(FakeSock::new()));

        let (s_to_conn, r_to_conn) =
            channel::local_channel(1, 1, &reactor.local_registration_memory());
        let (s_from_conn, r_from_conn) =
            channel::local_channel(1, 2, &reactor.local_registration_memory());

        let fut = {
            let sock = sock.clone();
            let s_from_conn = s_from_conn
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            let shared_mem = Rc::new(arena::RcMemory::new(1));
            let shared = arena::Rc::new(StreamSharedData::new(), &shared_mem).unwrap();
            let addr = ArrayVec::try_from(b"handler".as_slice()).unwrap();
            shared.get().set_to_addr(Some(addr));

            client_stream_fut(
                b"1".to_vec(),
                zreq,
                sock,
                true,
                r_to_conn,
                s_from_conn,
                shared,
            )
        };

        let mut executor = StepExecutor::new(&reactor, fut);

        // fill the handler's outbound message queue
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_ok(), true);
        assert_eq!(s_from_conn.try_send(zmq::Message::new()).is_err(), true);
        drop(s_from_conn);

        // handler won't be able to send a message yet
        assert_eq!(check_poll(executor.step()), None);

        // read bogus message
        let msg = r_from_conn.try_recv().unwrap();
        assert_eq!(msg.is_empty(), true);

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        // now handler will be able to send a message
        assert_eq!(check_poll(executor.step()), None);

        // read real message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = concat!(
            "handler T79:4:from,4:test,2:id,1:1,3:seq,1:0#3:ext,15:5:mu",
            "lti,4:true!}4:type,10:keep-alive,}",
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

        // no data yet
        assert_eq!(sock.borrow_mut().take_writable().is_empty(), true);

        sock.borrow_mut().allow_write(1024);

        assert_eq!(check_poll(executor.step()), None);

        let buf = sock.borrow_mut().take_writable();

        // use httparse to fish out Sec-WebSocket-Key
        let ws_key = {
            let mut headers = [httparse::EMPTY_HEADER; HEADERS_MAX];

            let mut req = httparse::Request::new(&mut headers);

            match req.parse(&buf) {
                Ok(httparse::Status::Complete(_)) => {}
                _ => panic!("unexpected parse status"),
            }

            let mut ws_key = String::new();

            for h in req.headers {
                if h.name.eq_ignore_ascii_case("Sec-WebSocket-Key") {
                    ws_key = String::from_utf8(h.value.to_vec()).unwrap();
                }
            }

            ws_key
        };

        let expected = format!(
            concat!(
                "GET /path HTTP/1.1\r\n",
                "Host: example.com\r\n",
                "Upgrade: websocket\r\n",
                "Connection: Upgrade\r\n",
                "Sec-WebSocket-Version: 13\r\n",
                "Sec-WebSocket-Key: {}\r\n",
                "Sec-WebSocket-Extensions: permessage-deflate\r\n",
                "Foo: Bar\r\n",
                "\r\n",
            ),
            ws_key
        );

        assert_eq!(str::from_utf8(&buf).unwrap(), expected);

        // no more messages yet
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let ws_accept = calculate_ws_accept(ws_key.as_bytes()).unwrap();

        let resp_data = format!(
            concat!(
                "HTTP/1.1 101 Switching Protocols\r\n",
                "Upgrade: websocket\r\n",
                "Connection: Upgrade\r\n",
                "Sec-WebSocket-Accept: {}\r\n",
                "Sec-WebSocket-Extensions: permessage-deflate\r\n",
                "\r\n",
            ),
            ws_accept
        );

        sock.borrow_mut().add_readable(resp_data.as_bytes());

        assert_eq!(check_poll(executor.step()), None);

        // read message
        let msg = r_from_conn.try_recv().unwrap();

        // no other messages
        assert_eq!(r_from_conn.try_recv().is_err(), true);

        let buf = &msg[..];

        let expected = format!(
            concat!(
                "handler T303:4:from,4:test,2:id,1:1,3:seq,1:1#3:ext,15:5:m",
                "ulti,4:true!}}4:code,3:101#6:reason,19:Switching Protocols",
                ",7:headers,168:22:7:Upgrade,9:websocket,]24:10:Connection,",
                "7:Upgrade,]56:20:Sec-WebSocket-Accept,28:{},]50:24:Sec-Web",
                "Socket-Extensions,18:permessage-deflate,]]7:credits,4:1024",
                "#}}",
            ),
            ws_accept
        );

        assert_eq!(str::from_utf8(buf).unwrap(), expected);

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
        let msg = r_from_conn.try_recv().unwrap();

        let buf = &msg[..];

        let expected = concat!(
            "handler T96:4:from,4:test,2:id,1:1,3:seq,1:2#3:ext,15:5:mu",
            "lti,4:true!}12:content-type,4:text,4:body,5:hello,}",
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

        let req = zhttppacket::OwnedRequest::parse(msg, 0, scratch).unwrap();
        let req = arena::Rc::new(req, &req_mem).unwrap();

        assert_eq!(s_to_conn.try_send((req, 0)).is_ok(), true);

        assert_eq!(check_poll(executor.step()), None);

        let mut data = sock.borrow_mut().take_writable();

        let fi = websocket::read_header(&data).unwrap();
        assert_eq!(fi.fin, true);
        assert_eq!(fi.opcode, websocket::OPCODE_TEXT);
        assert!(data.len() >= fi.payload_offset + fi.payload_size);

        let content = {
            let src = &mut data[fi.payload_offset..(fi.payload_offset + fi.payload_size)];
            websocket::apply_mask(src, fi.mask.unwrap(), 0);

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
