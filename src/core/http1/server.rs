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

use crate::buffer::{Buffer, ContiguousBuffer, VecRingBuffer, VECTORED_MAX};
use crate::core::http1::error::Error;
use crate::core::http1::protocol::{self, BodySize, Header, ParseScratch, ParseStatus};
use crate::core::http1::util::*;
use crate::future::{
    select_2, AsyncRead, AsyncWrite, AsyncWriteExt, ReadHalf, Select2, StdWriteWrapper, WriteHalf,
};
use crate::pin;
use std::cell::{Cell, RefCell};
use std::io::{self, Write};
use std::pin::Pin;
use std::str;

struct RequestInner<'a, R: AsyncRead, W: AsyncWrite> {
    r: ReadHalf<'a, R>,
    w: WriteHalf<'a, W>,
    buf1: &'a mut VecRingBuffer,
    buf2: &'a mut VecRingBuffer,
    protocol: protocol::ServerProtocol,
    send_is_dirty: bool,
}

pub struct Request;

impl Request {
    pub fn new<'a: 'b, 'b, R: AsyncRead, W: AsyncWrite>(
        stream: (ReadHalf<'a, R>, WriteHalf<'a, W>),
        buf1: &'a mut VecRingBuffer,
        buf2: &'a mut VecRingBuffer,
    ) -> (Self, Response<'a, R, W>) {
        (
            Self,
            Response {
                inner: Some(RequestInner {
                    r: stream.0,
                    w: stream.1,
                    buf1,
                    buf2,
                    protocol: protocol::ServerProtocol::new(),
                    send_is_dirty: false,
                }),
            },
        )
    }

    pub fn recv_header<'a: 'b, 'b, R: AsyncRead, W: AsyncWrite>(
        self,
        resp: &'b mut Response<'a, R, W>,
    ) -> RequestHeader<'a, 'b, R, W> {
        RequestHeader {
            inner: resp.inner.as_mut().unwrap(),
        }
    }
}

pub struct RequestHeader<'a, 'b, R: AsyncRead, W: AsyncWrite> {
    inner: &'b mut RequestInner<'a, R, W>,
}

impl<'a: 'b, 'b, R: AsyncRead, W: AsyncWrite> RequestHeader<'a, 'b, R, W> {
    // read from stream into buf, and parse buf as a request header
    pub async fn recv<'c, const N: usize>(
        self,
        mut scratch: &'c mut ParseScratch<N>,
    ) -> Result<
        (
            protocol::OwnedRequest<'c, N>,
            RequestBodyKeepHeader<'a, 'b, R, W>,
        ),
        Error,
    > {
        assert_eq!(
            self.inner.protocol.state(),
            protocol::ServerState::ReceivingRequest
        );

        let size_limit = self.inner.buf1.remaining_capacity();

        let req = loop {
            {
                let hbuf = self.inner.buf1.take_inner();

                match self.inner.protocol.recv_request_owned(hbuf, scratch) {
                    ParseStatus::Complete(req) => break req,
                    ParseStatus::Incomplete((), hbuf, ret_scratch) => {
                        // NOTE: after polonius it may not be necessary for
                        // scratch to be returned
                        scratch = ret_scratch;
                        self.inner.buf1.set_inner(hbuf);
                    }
                    ParseStatus::Error(e, hbuf, _) => {
                        self.inner.buf1.set_inner(hbuf);

                        return Err(e.into());
                    }
                }
            }

            if let Err(e) = recv_nonzero(&mut self.inner.r, self.inner.buf1).await {
                if e.kind() == io::ErrorKind::WriteZero {
                    return Err(Error::RequestTooLarge(size_limit));
                }

                return Err(e.into());
            }
        };

        assert!([
            protocol::ServerState::ReceivingBody,
            protocol::ServerState::AwaitingResponse
        ]
        .contains(&self.inner.protocol.state()));

        // at this point, req has taken buf1's inner buffer, such that
        // buf1 has no inner buffer

        // put remaining readable bytes in buf2
        self.inner.buf2.write_all(req.remaining_bytes())?;

        // swap inner buffers, such that buf1 now contains the remaining
        // readable bytes, and buf2 is now the one with no inner buffer
        self.inner.buf1.swap_inner(self.inner.buf2);

        let need_send_100 = req.get().expect_100;

        Ok((
            req,
            RequestBodyKeepHeader {
                inner: Some(RequestBodyKeepHeaderInner {
                    inner: RequestBody {
                        inner: RefCell::new(Some(RequestBodyInner {
                            r: &mut self.inner.r,
                            w: &mut self.inner.w,
                            buf1: self.inner.buf1,
                            protocol: &mut self.inner.protocol,
                            need_send_100,
                            send_is_dirty: &mut self.inner.send_is_dirty,
                        })),
                    },
                    buf2: self.inner.buf2,
                }),
            },
        ))
    }
}

struct RequestBodyInner<'a, 'b, R: AsyncRead, W: AsyncWrite> {
    r: &'b mut ReadHalf<'a, R>,
    w: &'b mut WriteHalf<'a, W>,
    buf1: &'b mut VecRingBuffer,
    protocol: &'b mut protocol::ServerProtocol,
    need_send_100: bool,
    send_is_dirty: &'b mut bool,
}

pub struct RequestBody<'a, 'b, R: AsyncRead, W: AsyncWrite> {
    inner: RefCell<Option<RequestBodyInner<'a, 'b, R, W>>>,
}

impl<'a: 'b, 'b, R: AsyncRead, W: AsyncWrite> RequestBody<'a, 'b, R, W> {
    // on EOF and any subsequent calls, return success
    #[allow(clippy::await_holding_refcell_ref)]
    pub async fn add_to_buffer(&self) -> Result<(), Error> {
        let mut b_inner = self.inner.borrow_mut();

        if let Some(mut inner) = b_inner.take() {
            Self::handle_expect(&mut inner).await?;

            if let Err(e) = recv_nonzero(inner.r, inner.buf1).await {
                if e.kind() == io::ErrorKind::WriteZero {
                    return Err(Error::BufferExceeded);
                }

                return Err(e.into());
            }

            *b_inner = Some(inner);

            Ok(())
        } else {
            Err(Error::Unusable)
        }
    }

    pub fn try_recv(&self, dest: &mut [u8]) -> Result<RecvStatus<(), ()>, Error> {
        loop {
            let mut b_inner = self.inner.borrow_mut();

            if let Some(inner) = b_inner.take() {
                let (read, written, done) =
                    if inner.protocol.state() == protocol::ServerState::ReceivingBody {
                        let mut buf = io::Cursor::new(Buffer::read_buf(inner.buf1));

                        let mut headers = [httparse::EMPTY_HEADER; HEADERS_MAX];

                        let (written, _) =
                            match inner.protocol.recv_body(&mut buf, dest, &mut headers) {
                                Ok(ret) => ret,
                                Err(e) => return Err(e.into()),
                            };

                        let read = buf.position() as usize;

                        (
                            read,
                            written,
                            inner.protocol.state() == protocol::ServerState::AwaitingResponse,
                        )
                    } else {
                        (0, 0, true)
                    };

                if done {
                    inner.buf1.read_commit(read);
                    assert_eq!(
                        inner.protocol.state(),
                        protocol::ServerState::AwaitingResponse
                    );

                    *b_inner = None;

                    break Ok(RecvStatus::Complete((), written));
                } else {
                    *b_inner = Some(RequestBodyInner {
                        r: inner.r,
                        w: inner.w,
                        buf1: inner.buf1,
                        protocol: inner.protocol,
                        need_send_100: inner.need_send_100,
                        send_is_dirty: inner.send_is_dirty,
                    });

                    let inner = b_inner.as_mut().unwrap();

                    if read == 0 && written == 0 && !inner.buf1.is_readable_contiguous() {
                        inner.buf1.align();
                        continue;
                    }

                    inner.buf1.read_commit(read);

                    break Ok(RecvStatus::Read((), written));
                }
            } else {
                return Err(Error::Unusable);
            }
        }
    }

    async fn handle_expect(inner: &mut RequestBodyInner<'a, 'b, R, W>) -> Result<(), Error> {
        if !inner.need_send_100 {
            return Ok(());
        }

        let mut cont = [0; 32];

        let cont = {
            let mut c = io::Cursor::new(&mut cont[..]);

            inner.protocol.send_100_continue(&mut c).unwrap();

            let size = c.position() as usize;

            &cont[..size]
        };

        let mut left = cont.len();

        while left > 0 {
            let pos = cont.len() - left;

            let size = match inner.w.write(&cont[pos..]).await {
                Ok(size) => size,
                Err(e) => return Err(e.into()),
            };

            *inner.send_is_dirty = true;

            left -= size;
        }

        inner.need_send_100 = false;
        *inner.send_is_dirty = false;

        Ok(())
    }
}

struct RequestBodyKeepHeaderInner<'a, 'b, R: AsyncRead, W: AsyncWrite> {
    inner: RequestBody<'a, 'b, R, W>,
    buf2: &'b mut VecRingBuffer,
}

pub struct RequestBodyKeepHeader<'a, 'b, R: AsyncRead, W: AsyncWrite> {
    inner: Option<RequestBodyKeepHeaderInner<'a, 'b, R, W>>,
}

impl<'a: 'b, 'b, R: AsyncRead, W: AsyncWrite> RequestBodyKeepHeader<'a, 'b, R, W> {
    pub fn discard_header<const N: usize>(
        mut self,
        req: protocol::OwnedRequest<N>,
    ) -> RequestBody<'a, 'b, R, W> {
        let inner = self.inner.take().unwrap();

        inner.buf2.set_inner(req.into_buf());
        inner.buf2.clear();

        inner.inner
    }

    pub async fn add_to_buffer(&self) -> Result<(), Error> {
        let inner = self.inner.as_ref().unwrap();

        inner.inner.add_to_buffer().await
    }

    pub fn try_recv(&self, dest: &mut [u8]) -> Result<RecvStatus<(), ()>, Error> {
        let inner = self.inner.as_ref().unwrap();

        match inner.inner.try_recv(dest)? {
            RecvStatus::Complete((), written) => Ok(RecvStatus::Complete((), written)),
            RecvStatus::Read((), written) => Ok(RecvStatus::Read((), written)),
        }
    }
}

impl<R: AsyncRead, W: AsyncWrite> Drop for RequestBodyKeepHeader<'_, '_, R, W> {
    fn drop(&mut self) {
        if self.inner.is_some() {
            panic!("RequestBodyKeepHeader must be consumed by discard_header() instead of dropped");
        }
    }
}

pub struct Response<'a, R: AsyncRead, W: AsyncWrite> {
    inner: Option<RequestInner<'a, R, W>>,
}

impl<'a, R: AsyncRead, W: AsyncWrite> Response<'a, R, W> {
    pub async fn fill_recv_buffer(&mut self) -> Error {
        if let Some(inner) = &mut self.inner {
            loop {
                if let Err(e) = recv_nonzero(&mut inner.r, inner.buf1).await {
                    if e.kind() == io::ErrorKind::WriteZero {
                        // if there's no more space, suspend forever
                        std::future::pending::<()>().await;
                    }

                    return e.into();
                }
            }
        } else {
            Error::Unusable
        }
    }

    #[allow(clippy::type_complexity)]
    pub fn prepare_header<'b>(
        &mut self,
        code: u16,
        reason: &str,
        headers: &[Header<'_>],
        body_size: BodySize,
        state: &'b mut ResponseState<'a, R, W>,
    ) -> Result<
        (
            ResponseHeader<'a, 'b, R, W>,
            ResponsePrepareBody<'a, 'b, R, W>,
        ),
        Error,
    > {
        let inner = match &mut self.inner {
            Some(inner) => inner,
            None => return Err(Error::Unusable),
        };

        if inner.protocol.state() == protocol::ServerState::ReceivingRequest {
            inner.protocol.skip_recv_request();
        }

        inner.buf2.clear();
        let size_limit = inner.buf2.capacity();

        let mut hbuf = io::Cursor::new(inner.buf2.write_buf());

        if inner
            .protocol
            .send_response(&mut hbuf, code, reason, headers, body_size)
            .is_err()
        {
            // enable prepare_header to be called again
            inner.buf2.clear();

            return Err(Error::ResponseTooLarge(size_limit));
        }

        let header_size = hbuf.position() as usize;
        inner.buf2.write_commit(header_size);

        let inner = self.inner.take().unwrap();

        *state.inner.borrow_mut() = Some(ResponseStateInner {
            r: inner.r,
            w: RefCell::new(inner.w),
            buf1: inner.buf1,
            buf2: RefCell::new(LimitedRingBuffer {
                inner: inner.buf2,
                limit: header_size,
            }),
            protocol: inner.protocol,
            overflow: RefCell::new(None),
            end: Cell::new(false),
        });

        let state = &state.inner;

        Ok((ResponseHeader { state }, ResponsePrepareBody { state }))
    }
}

struct ResponseStateInner<'a, R: AsyncRead, W: AsyncWrite> {
    r: ReadHalf<'a, R>,
    w: RefCell<WriteHalf<'a, W>>,
    buf1: &'a mut VecRingBuffer,
    buf2: RefCell<LimitedRingBuffer<'a>>,
    protocol: protocol::ServerProtocol,
    overflow: RefCell<Option<ContiguousBuffer>>,
    end: Cell<bool>,
}

pub struct ResponseState<'a, R: AsyncRead, W: AsyncWrite> {
    inner: RefCell<Option<ResponseStateInner<'a, R, W>>>,
}

impl<'a, R: AsyncRead, W: AsyncWrite> Default for ResponseState<'a, R, W> {
    fn default() -> Self {
        Self {
            inner: RefCell::new(None),
        }
    }
}

pub struct ResponseHeader<'a, 'b, R: AsyncRead, W: AsyncWrite> {
    state: &'b RefCell<Option<ResponseStateInner<'a, R, W>>>,
}

impl<'a, 'b, R: AsyncRead, W: AsyncWrite> ResponseHeader<'a, 'b, R, W> {
    #[allow(clippy::await_holding_refcell_ref)]
    pub async fn send(self) -> Result<ResponseHeaderSent<'a, 'b, R, W>, Error> {
        // ok to hold across await as self.state is only ever immutably borrowed
        let state = self.state.borrow();
        let state = state.as_ref().unwrap();

        while state.buf2.borrow().limit > 0 {
            // ok to hold across await as this is the only place state.w is borrowed
            let mut w = state.w.borrow_mut();

            // TODO: vectored write
            let size = w.write_shared(&state.buf2).await?;

            let mut buf2 = state.buf2.borrow_mut();
            buf2.inner.read_commit(size);
            buf2.limit -= size;
        }

        let mut overflow = state.overflow.borrow_mut();

        if let Some(overflow_ref) = &mut *overflow {
            // overflow is guaranteed to fit
            let mut buf2 = state.buf2.borrow_mut();
            buf2.inner.write_all(overflow_ref.read_buf()).unwrap();
            *overflow = None;
        }

        Ok(ResponseHeaderSent { state: self.state })
    }
}

pub struct ResponsePrepareBody<'a, 'b, R: AsyncRead, W: AsyncWrite> {
    state: &'b RefCell<Option<ResponseStateInner<'a, R, W>>>,
}

impl<'a, 'b, R: AsyncRead, W: AsyncWrite> ResponsePrepareBody<'a, 'b, R, W> {
    // only returns an error on invalid input
    pub fn prepare(&mut self, src: &[u8], end: bool) -> Result<(usize, usize), Error> {
        let state = self.state.borrow();
        let state = state.as_ref().unwrap();

        // call not allowed if the end has already been indicated
        if state.end.get() {
            return Err(Error::FurtherInputNotAllowed);
        }

        let buf2 = &mut *state.buf2.borrow_mut();
        let overflow = &mut *state.overflow.borrow_mut();

        // workaround for rust 1.77
        #[allow(clippy::unused_io_amount)]
        let accepted = if overflow.is_none() {
            match buf2.inner.write(src) {
                Ok(size) => size,
                Err(e) if e.kind() == io::ErrorKind::WriteZero => 0,
                Err(e) => panic!("infallible buffer write failed: {}", e),
            }
        } else {
            0
        };

        let (size, overflowed) = if accepted < src.len() {
            // only allow overflowing as much as there are header bytes left
            let overflow = overflow.get_or_insert_with(|| ContiguousBuffer::new(buf2.limit));

            let remaining = &src[accepted..];
            let overflowed = match overflow.write(remaining) {
                Ok(size) => size,
                Err(e) if e.kind() == io::ErrorKind::WriteZero => 0,
                Err(e) => panic!("infallible buffer write failed: {}", e),
            };

            (accepted + overflowed, overflowed)
        } else {
            (accepted, 0)
        };

        assert!(size <= src.len());

        if size == src.len() && end {
            state.end.set(true);
        }

        Ok((size, overflowed))
    }
}

pub struct ResponseHeaderSent<'a, 'b, R: AsyncRead, W: AsyncWrite> {
    state: &'b RefCell<Option<ResponseStateInner<'a, R, W>>>,
}

impl<'a, 'b, R: AsyncRead, W: AsyncWrite> ResponseHeaderSent<'a, 'b, R, W> {
    pub fn start_body(
        self,
        _prepare_body: ResponsePrepareBody<'a, 'b, R, W>,
    ) -> ResponseBody<'a, R, W> {
        let state = self.state.take().unwrap();

        let buf2 = state.buf2.into_inner();
        let block_size = buf2.inner.capacity();

        ResponseBody {
            inner: RefCell::new(Some(ResponseBodyInner {
                r: RefCell::new(ResponseBodyRead {
                    stream: state.r,
                    buf: state.buf1,
                }),
                w: RefCell::new(ResponseBodyWrite {
                    stream: state.w.into_inner(),
                    buf: buf2.inner,
                    protocol: state.protocol,
                    end: state.end.get(),
                    block_size,
                }),
            })),
        }
    }
}

struct ResponseBodyRead<'a, R: AsyncRead> {
    stream: ReadHalf<'a, R>,
    buf: &'a mut VecRingBuffer,
}

struct ResponseBodyWrite<'a, W: AsyncWrite> {
    stream: WriteHalf<'a, W>,
    buf: &'a mut VecRingBuffer,
    protocol: protocol::ServerProtocol,
    end: bool,
    block_size: usize,
}

struct ResponseBodyInner<'a, R: AsyncRead, W: AsyncWrite> {
    r: RefCell<ResponseBodyRead<'a, R>>,
    w: RefCell<ResponseBodyWrite<'a, W>>,
}

pub struct ResponseBody<'a, R: AsyncRead, W: AsyncWrite> {
    inner: RefCell<Option<ResponseBodyInner<'a, R, W>>>,
}

impl<'a, R: AsyncRead, W: AsyncWrite> ResponseBody<'a, R, W> {
    pub fn prepare(&self, src: &[u8], end: bool) -> Result<usize, Error> {
        if let Some(inner) = &*self.inner.borrow() {
            let w = &mut *inner.w.borrow_mut();

            // call not allowed if the end has already been indicated
            if w.end {
                return Err(Error::FurtherInputNotAllowed);
            }

            let size = match w.buf.write(src) {
                Ok(size) => size,
                Err(e) if e.kind() == io::ErrorKind::WriteZero => 0,
                Err(e) => panic!("infallible buffer write failed: {}", e),
            };

            assert!(size <= src.len());

            if size == src.len() && end {
                w.end = true;
            }

            Ok(size)
        } else {
            Err(Error::Unusable)
        }
    }

    pub fn expand_write_buffer<F>(&self, blocks_max: usize, reserve: F) -> Result<usize, Error>
    where
        F: Fn() -> bool,
    {
        if let Some(inner) = &*self.inner.borrow() {
            let w = &mut *inner.w.borrow_mut();

            Ok(resize_write_buffer_if_full(
                w.buf,
                w.block_size,
                blocks_max,
                reserve,
            ))
        } else {
            Err(Error::Unusable)
        }
    }

    pub fn can_send(&self) -> bool {
        if let Some(inner) = &*self.inner.borrow() {
            let w = &*inner.w.borrow();

            w.buf.len() > 0 || w.end
        } else {
            false
        }
    }

    pub async fn send(&self) -> SendStatus<Finished, (), Error> {
        if self.inner.borrow().is_none() {
            return SendStatus::Error((), Error::Unusable);
        }

        let size = loop {
            match self.process().await {
                Some(Ok(size)) => break size,
                Some(Err(e)) => return SendStatus::Error((), e),
                None => {} // received data
            }
        };

        let mut inner = self.inner.borrow_mut();
        assert!(inner.is_some());

        let done = {
            let inner = inner.as_ref().unwrap();
            let mut w = inner.w.borrow_mut();

            w.buf.read_commit(size);

            w.protocol.state() == protocol::ServerState::Finished
        };

        if done {
            let inner = inner.take().unwrap();
            let w = inner.w.into_inner();

            assert_eq!(w.buf.len(), 0);

            SendStatus::Complete(Finished {
                protocol: w.protocol,
            })
        } else {
            SendStatus::Partial((), size)
        }
    }

    // assumes self.inner is Some
    #[allow(clippy::await_holding_refcell_ref)]
    async fn process(&self) -> Option<Result<usize, Error>> {
        let inner = self.inner.borrow();
        let inner = inner.as_ref().unwrap();

        let mut r = inner.r.borrow_mut();

        let result = select_2(
            AsyncOperation::new(
                |cx| {
                    let w = &mut *inner.w.borrow_mut();

                    if !w.stream.is_writable() {
                        return None;
                    }

                    assert_eq!(w.protocol.state(), protocol::ServerState::SendingBody);

                    if w.buf.len() == 0 && !w.end {
                        return Some(Ok(0));
                    }

                    // protocol.send_body() expects the input to leave room
                    // for at least two more buffers in case chunked encoding
                    // is used (for chunked header and footer)
                    let mut buf_arr = [&b""[..]; VECTORED_MAX - 2];
                    let bufs = w.buf.read_bufs(&mut buf_arr);

                    match w.protocol.send_body(
                        &mut StdWriteWrapper::new(Pin::new(&mut w.stream), cx),
                        bufs,
                        w.end,
                        None,
                    ) {
                        Ok(size) => Some(Ok(size)),
                        Err(protocol::Error::Io(e)) if e.kind() == io::ErrorKind::WouldBlock => {
                            None
                        }
                        Err(e) => Some(Err(e.into())),
                    }
                },
                || inner.w.borrow_mut().stream.cancel(),
            ),
            pin!(async {
                let r = &mut *r;

                if let Err(e) = recv_nonzero(&mut r.stream, r.buf).await {
                    if e.kind() == io::ErrorKind::WriteZero {
                        // if there's no more space, suspend forever
                        std::future::pending::<()>().await;
                    }

                    return Err(Error::from(e));
                }

                Ok(())
            }),
        )
        .await;

        match result {
            Select2::R1(ret) => Some(ret),
            Select2::R2(ret) => match ret {
                Ok(()) => None,         // received data
                Err(e) => Some(Err(e)), // error while receiving data
            },
        }
    }
}

pub struct Finished {
    protocol: protocol::ServerProtocol,
}

impl Finished {
    pub fn is_persistent(&self) -> bool {
        self.protocol.is_persistent()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::buffer::TmpBuffer;
    use crate::future::io_split;
    use std::cmp;
    use std::future::Future;
    use std::rc::Rc;
    use std::sync::Arc;
    use std::task::{Context, Poll, Wake};

    struct FakeStream {
        in_data: Vec<u8>,
        out_data: Vec<u8>,
    }

    impl FakeStream {
        fn new() -> Self {
            Self {
                in_data: Vec::new(),
                out_data: Vec::new(),
            }
        }
    }

    impl AsyncRead for FakeStream {
        fn poll_read(
            mut self: Pin<&mut Self>,
            _cx: &mut Context,
            buf: &mut [u8],
        ) -> Poll<Result<usize, io::Error>> {
            let size = cmp::min(buf.len(), self.in_data.len());

            if size == 0 {
                return Poll::Pending;
            }

            let left = self.in_data.split_off(size);

            (&mut buf[..size]).copy_from_slice(&self.in_data);

            self.in_data = left;

            Poll::Ready(Ok(size))
        }

        fn cancel(&mut self) {}
    }

    impl AsyncWrite for FakeStream {
        fn poll_write(
            mut self: Pin<&mut Self>,
            _cx: &mut Context,
            buf: &[u8],
        ) -> Poll<Result<usize, io::Error>> {
            let size = self.out_data.write(buf).unwrap();

            Poll::Ready(Ok(size))
        }

        fn poll_close(self: Pin<&mut Self>, _cx: &mut Context) -> Poll<Result<(), io::Error>> {
            Poll::Ready(Ok(()))
        }

        fn is_writable(&self) -> bool {
            true
        }

        fn cancel(&mut self) {}
    }

    struct NoopWaker;

    impl Wake for NoopWaker {
        fn wake(self: Arc<Self>) {}
    }

    #[test]
    fn request_response() {
        let mut fut = pin!(async {
            let mut stream = FakeStream::new();
            stream
                .in_data
                .write_all("POST /path HTTP/1.1\r\nContent-Length: 6\r\n\r\nhello\n".as_bytes())
                .unwrap();

            {
                let stream = RefCell::new(&mut stream);

                let tmp = Rc::new(TmpBuffer::new(1024));
                let mut buf1 = VecRingBuffer::new(1024, &tmp);
                let mut buf2 = VecRingBuffer::new(1024, &tmp);

                let (req, mut resp) = Request::new(io_split(&stream), &mut buf1, &mut buf2);

                let header = req.recv_header(&mut resp);

                let mut scratch = ParseScratch::<HEADERS_MAX>::new();
                let (req_header, req_body) = header.recv(&mut scratch).await.unwrap();

                let req_ref = req_header.get();
                assert_eq!(req_ref.method, "POST");
                assert_eq!(req_ref.uri, "/path");
                assert_eq!(req_ref.body_size, BodySize::Known(6));

                let req_body = req_body.discard_header(req_header);

                let mut buf = [0; 64];
                let size = match req_body.try_recv(&mut buf).unwrap() {
                    RecvStatus::Complete((), size) => size,
                    _ => unreachable!(),
                };

                drop(req_body);

                let buf = &buf[..size];
                assert_eq!(str::from_utf8(buf).unwrap(), "hello\n");

                let mut state = ResponseState::default();
                let (header, prepare_body) =
                    match resp.prepare_header(200, "OK", &[], BodySize::Known(6), &mut state) {
                        Ok(ret) => ret,
                        Err(_) => unreachable!(),
                    };

                let sent = header.send().await.unwrap();

                let resp_body = sent.start_body(prepare_body);
                assert_eq!(resp_body.prepare(b"world\n", true).unwrap(), 6);

                let finished = match resp_body.send().await {
                    SendStatus::Complete(finished) => finished,
                    _ => unreachable!(),
                };

                assert!(finished.is_persistent());
            }

            let expected = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nworld\n";

            assert_eq!(str::from_utf8(&stream.out_data).unwrap(), expected);
        });

        let waker = Arc::new(NoopWaker).into();
        let mut cx = Context::from_waker(&waker);
        assert!(fut.as_mut().poll(&mut cx).is_ready());
    }

    #[test]
    fn response_during_header() {
        let mut fut = pin!(async {
            let mut stream = FakeStream::new();
            stream
                .in_data
                .write_all("POST /path HTTP/1.1\r\nContent-Length: 6\r\n\r\nhello\n".as_bytes())
                .unwrap();

            {
                let stream = RefCell::new(&mut stream);

                let tmp = Rc::new(TmpBuffer::new(1024));
                let mut buf1 = VecRingBuffer::new(1024, &tmp);
                let mut buf2 = VecRingBuffer::new(1024, &tmp);

                let (_req, mut resp) = Request::new(io_split(&stream), &mut buf1, &mut buf2);

                let mut state = ResponseState::default();
                let (header, prepare_body) =
                    match resp.prepare_header(200, "OK", &[], BodySize::Known(6), &mut state) {
                        Ok(ret) => ret,
                        Err(_) => unreachable!(),
                    };

                let sent = header.send().await.unwrap();

                let resp_body = sent.start_body(prepare_body);
                assert_eq!(resp_body.prepare(b"world\n", true).unwrap(), 6);

                let finished = match resp_body.send().await {
                    SendStatus::Complete(finished) => finished,
                    _ => unreachable!(),
                };

                assert!(!finished.is_persistent());
            }

            let expected = "HTTP/1.0 200 OK\r\nContent-Length: 6\r\n\r\nworld\n";

            assert_eq!(str::from_utf8(&stream.out_data).unwrap(), expected);
        });

        let waker = Arc::new(NoopWaker).into();
        let mut cx = Context::from_waker(&waker);
        assert!(fut.as_mut().poll(&mut cx).is_ready());
    }

    #[test]
    fn response_during_body() {
        let mut fut = pin!(async {
            let mut stream = FakeStream::new();
            stream
                .in_data
                .write_all("POST /path HTTP/1.1\r\nContent-Length: 6\r\n\r\nhello\n".as_bytes())
                .unwrap();

            {
                let stream = RefCell::new(&mut stream);

                let tmp = Rc::new(TmpBuffer::new(1024));
                let mut buf1 = VecRingBuffer::new(1024, &tmp);
                let mut buf2 = VecRingBuffer::new(1024, &tmp);

                let (req, mut resp) = Request::new(io_split(&stream), &mut buf1, &mut buf2);

                let header = req.recv_header(&mut resp);

                let mut scratch = ParseScratch::<HEADERS_MAX>::new();
                let (req_header, req_body) = header.recv(&mut scratch).await.unwrap();

                let req_ref = req_header.get();
                assert_eq!(req_ref.method, "POST");
                assert_eq!(req_ref.uri, "/path");
                assert_eq!(req_ref.body_size, BodySize::Known(6));

                let req_body = req_body.discard_header(req_header);
                drop(req_body);

                let mut state = ResponseState::default();
                let (header, prepare_body) =
                    match resp.prepare_header(200, "OK", &[], BodySize::Known(6), &mut state) {
                        Ok(ret) => ret,
                        Err(_) => unreachable!(),
                    };

                let sent = header.send().await.unwrap();

                let resp_body = sent.start_body(prepare_body);
                assert_eq!(resp_body.prepare(b"world\n", true).unwrap(), 6);

                let finished = match resp_body.send().await {
                    SendStatus::Complete(finished) => finished,
                    _ => unreachable!(),
                };

                assert!(!finished.is_persistent());
            }

            let expected =
                "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 6\r\n\r\nworld\n";

            assert_eq!(str::from_utf8(&stream.out_data).unwrap(), expected);
        });

        let waker = Arc::new(NoopWaker).into();
        let mut cx = Context::from_waker(&waker);
        assert!(fut.as_mut().poll(&mut cx).is_ready());
    }

    #[test]
    fn response_overflow() {
        let mut fut = pin!(async {
            let mut stream = FakeStream::new();
            stream
                .in_data
                .write_all("GET /path HTTP/1.1\r\n\r\n".as_bytes())
                .unwrap();

            let mut body = [0; 100];
            for i in 0..body.len() {
                body[i] = b'a' + ((i as u8) % 26);
            }

            let attempted_body = str::from_utf8(&body).unwrap();
            let expected_body = &attempted_body[..64];

            {
                let stream = RefCell::new(&mut stream);

                let tmp = Rc::new(TmpBuffer::new(64));
                let mut buf1 = VecRingBuffer::new(64, &tmp);
                let mut buf2 = VecRingBuffer::new(64, &tmp);

                let (req, mut resp) = Request::new(io_split(&stream), &mut buf1, &mut buf2);

                let header = req.recv_header(&mut resp);

                let mut scratch = ParseScratch::<HEADERS_MAX>::new();
                let (req_header, req_body) = header.recv(&mut scratch).await.unwrap();
                let req_body = req_body.discard_header(req_header);
                drop(req_body);

                let mut state = ResponseState::default();

                // this will serialize to 39 bytes, leaving 25 bytes left
                let (header, mut prepare_body) =
                    match resp.prepare_header(200, "OK", &[], BodySize::Known(64), &mut state) {
                        Ok(ret) => ret,
                        Err(_) => unreachable!(),
                    };

                // only the first 64 bytes will fit
                assert_eq!(
                    prepare_body
                        .prepare(attempted_body.as_bytes(), true)
                        .unwrap(),
                    (64, 39)
                );

                // end is ignored if input doesn't fit, so set end again
                assert_eq!(prepare_body.prepare(&[], true).unwrap(), (0, 0));

                let sent = header.send().await.unwrap();

                let resp_body = sent.start_body(prepare_body);

                let size = match resp_body.send().await {
                    SendStatus::Partial(_, size) => size,
                    _ => unreachable!(),
                };
                assert_eq!(size, 25);

                let finished = match resp_body.send().await {
                    SendStatus::Complete(finished) => finished,
                    _ => unreachable!(),
                };

                assert!(finished.is_persistent());
            }

            let expected =
                "HTTP/1.1 200 OK\r\nContent-Length: 64\r\n\r\n".to_string() + expected_body;

            assert_eq!(str::from_utf8(&stream.out_data).unwrap(), expected);
        });

        let waker = Arc::new(NoopWaker).into();
        let mut cx = Context::from_waker(&waker);
        assert!(fut.as_mut().poll(&mut cx).is_ready());
    }
}
