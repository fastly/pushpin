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

use crate::core::buffer::{Buffer, VecRingBuffer, VECTORED_MAX};
use crate::core::http1::error::Error;
use crate::core::http1::protocol::{self, BodySize, Header, ParseScratch, ParseStatus};
use crate::core::http1::util::*;
use crate::core::io::{AsyncRead, AsyncWrite, AsyncWriteExt, ReadHalf, StdWriteWrapper, WriteHalf};
use crate::core::select::{select_2, Select2};
use std::cell::RefCell;
use std::io::{self, Write};
use std::mem;
use std::pin::pin;
use std::pin::Pin;
use std::str;

pub struct Request<'a, R: AsyncRead, W: AsyncWrite> {
    r: ReadHalf<'a, R>,
    w: WriteHalf<'a, W>,
    hbuf: &'a mut VecRingBuffer,
    bbuf: &'a mut VecRingBuffer,
}

impl<'a, R: AsyncRead, W: AsyncWrite> Request<'a, R, W> {
    pub fn new(
        stream: (ReadHalf<'a, R>, WriteHalf<'a, W>),
        buf1: &'a mut VecRingBuffer,
        buf2: &'a mut VecRingBuffer,
    ) -> Self {
        Self {
            r: stream.0,
            w: stream.1,
            hbuf: buf1,
            bbuf: buf2,
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub fn prepare_header(
        self,
        method: &str,
        uri: &str,
        headers: &[Header<'_>],
        body_size: BodySize,
        websocket: bool,
        initial_body: &[u8],
        end: bool,
    ) -> Result<RequestHeader<'a, R, W>, Error> {
        let req = protocol::ClientRequest::new();

        let size_limit = self.hbuf.capacity();

        let req_body = match req.send_header(self.hbuf, method, uri, headers, body_size, websocket)
        {
            Ok(ret) => ret,
            Err(_) => return Err(Error::RequestTooLarge(size_limit)),
        };

        if self.bbuf.write_all(initial_body).is_err() {
            return Err(Error::BufferExceeded);
        }

        Ok(RequestHeader {
            r: self.r,
            w: self.w,
            hbuf: self.hbuf,
            bbuf: self.bbuf,
            req_body,
            end,
        })
    }
}

pub struct RequestHeader<'a, R: AsyncRead, W: AsyncWrite> {
    r: ReadHalf<'a, R>,
    w: WriteHalf<'a, W>,
    hbuf: &'a mut VecRingBuffer,
    bbuf: &'a mut VecRingBuffer,
    req_body: protocol::ClientRequestBody,
    end: bool,
}

impl<'a, R: AsyncRead, W: AsyncWrite> RequestHeader<'a, R, W> {
    pub async fn send(mut self) -> Result<RequestBody<'a, R, W>, Error> {
        while self.hbuf.len() > 0 {
            let size = self.w.write(Buffer::read_buf(self.hbuf)).await?;
            self.hbuf.read_commit(size);
        }

        let block_size = self.bbuf.capacity();

        Ok(RequestBody {
            inner: RefCell::new(Some(RequestBodyInner {
                r: RefCell::new(RequestBodyRead {
                    stream: self.r,
                    buf: self.hbuf,
                }),
                w: RefCell::new(RequestBodyWrite {
                    stream: self.w,
                    buf: self.bbuf,
                    req_body: Some(self.req_body),
                    end: self.end,
                    block_size,
                }),
            })),
        })
    }
}

struct RequestBodyRead<'a, R: AsyncRead> {
    stream: ReadHalf<'a, R>,
    buf: &'a mut VecRingBuffer,
}

struct RequestBodyWrite<'a, W: AsyncWrite> {
    stream: WriteHalf<'a, W>,
    buf: &'a mut VecRingBuffer,
    req_body: Option<protocol::ClientRequestBody>,
    end: bool,
    block_size: usize,
}

struct RequestBodyInner<'a, R: AsyncRead, W: AsyncWrite> {
    r: RefCell<RequestBodyRead<'a, R>>,
    w: RefCell<RequestBodyWrite<'a, W>>,
}

pub struct RequestBody<'a, R: AsyncRead, W: AsyncWrite> {
    inner: RefCell<Option<RequestBodyInner<'a, R, W>>>,
}

impl<'a, R: AsyncRead, W: AsyncWrite> RequestBody<'a, R, W> {
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
        F: FnMut() -> bool,
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

    pub async fn send(&self) -> SendStatus<Response<'a, R>, (), Error> {
        if self.inner.borrow().is_none() {
            return SendStatus::Error((), Error::Unusable);
        }

        let status = loop {
            if let Some(inner) = self.take_inner_if_early_response() {
                let r = inner.r.into_inner();
                let w = inner.w.into_inner();
                let resp = w.req_body.unwrap().into_early_response();

                w.buf.clear();

                return SendStatus::EarlyResponse(Response {
                    r: r.stream,
                    rbuf: r.buf,
                    wbuf: w.buf,
                    inner: resp,
                });
            }

            match self.process().await {
                Some(Ok(status)) => break status,
                Some(Err(e)) => return SendStatus::Error((), e),
                None => {} // received data. loop and check for early response
            }
        };

        let mut inner = self.inner.borrow_mut();
        assert!(inner.is_some());

        match status {
            protocol::SendStatus::Complete(resp, size) => {
                let inner = inner.take().unwrap();

                let r = inner.r.into_inner();
                let w = inner.w.into_inner();

                w.buf.read_commit(size);

                assert_eq!(w.buf.len(), 0);

                SendStatus::Complete(Response {
                    r: r.stream,
                    rbuf: r.buf,
                    wbuf: w.buf,
                    inner: resp,
                })
            }
            protocol::SendStatus::Partial(req_body, size) => {
                let inner = inner.as_ref().unwrap();

                let mut w = inner.w.borrow_mut();

                w.req_body = Some(req_body);
                w.buf.read_commit(size);

                SendStatus::Partial((), size)
            }
            protocol::SendStatus::Error(req_body, e) => {
                let inner = inner.as_ref().unwrap();

                inner.w.borrow_mut().req_body = Some(req_body);

                SendStatus::Error((), e.into())
            }
        }
    }

    #[allow(clippy::await_holding_refcell_ref)]
    pub async fn fill_recv_buffer(&self) -> Error {
        if let Some(inner) = &*self.inner.borrow() {
            let r = &mut *inner.r.borrow_mut();

            loop {
                if let Err(e) = recv_nonzero(&mut r.stream, r.buf).await {
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

    // assumes self.inner is Some
    #[allow(clippy::await_holding_refcell_ref)]
    async fn process(
        &self,
    ) -> Option<
        Result<
            protocol::SendStatus<
                protocol::ClientResponse,
                protocol::ClientRequestBody,
                protocol::Error,
            >,
            Error,
        >,
    > {
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

                    let req_body = w.req_body.take().unwrap();

                    // req_body.send() expects the input to leave room for at
                    // least two more buffers in case chunked encoding is
                    // used (for chunked header and footer)
                    let mut buf_arr = [&b""[..]; VECTORED_MAX - 2];
                    let bufs = w.buf.read_bufs(&mut buf_arr);

                    match req_body.send(
                        &mut StdWriteWrapper::new(Pin::new(&mut w.stream), cx),
                        bufs,
                        w.end,
                        None,
                    ) {
                        protocol::SendStatus::Error(req_body, protocol::Error::Io(e))
                            if e.kind() == io::ErrorKind::WouldBlock =>
                        {
                            w.req_body = Some(req_body);

                            None
                        }
                        ret => Some(ret),
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
            Select2::R1(ret) => match ret {
                protocol::SendStatus::Error(req_body, protocol::Error::Io(e))
                    if e.kind() == io::ErrorKind::BrokenPipe =>
                {
                    // if we get an error when trying to send, it could be
                    // due to the server closing the connection after sending
                    // an early response. here we'll check if the server left
                    // us any data to read

                    let w = &mut *inner.w.borrow_mut();

                    w.req_body = Some(req_body);

                    if r.buf.len() == 0 {
                        let r = &mut *r;

                        match recv_nonzero(&mut r.stream, r.buf).await {
                            Ok(()) => None,                // received data
                            Err(e) => Some(Err(e.into())), // error while receiving data
                        }
                    } else {
                        None // we already received data
                    }
                }
                ret => Some(Ok(ret)),
            },
            Select2::R2(ret) => match ret {
                Ok(()) => None,         // received data
                Err(e) => Some(Err(e)), // error while receiving data
            },
        }
    }

    // assumes self.inner is Some
    fn take_inner_if_early_response(&self) -> Option<RequestBodyInner<'a, R, W>> {
        let mut inner = self.inner.borrow_mut();
        let inner_mut = inner.as_mut().unwrap();

        if inner_mut.r.borrow().buf.len() > 0 {
            Some(inner.take().unwrap())
        } else {
            None
        }
    }
}

pub struct Response<'a, R: AsyncRead> {
    r: ReadHalf<'a, R>,
    rbuf: &'a mut VecRingBuffer,
    wbuf: &'a mut VecRingBuffer,
    inner: protocol::ClientResponse,
}

impl<'a, R: AsyncRead> Response<'a, R> {
    pub async fn recv_header<'b, const N: usize>(
        mut self,
        mut scratch: &'b mut ParseScratch<N>,
    ) -> Result<
        (
            protocol::OwnedResponse<'b, N>,
            ResponseBodyKeepHeader<'a, R>,
        ),
        Error,
    > {
        let mut resp = self.inner;

        let (resp, resp_body) = loop {
            {
                let buf = self.rbuf.take_inner();

                resp = match resp.recv_header(buf, scratch) {
                    ParseStatus::Complete(ret) => break ret,
                    ParseStatus::Incomplete(resp, buf, ret_scratch) => {
                        // NOTE: after polonius it may not be necessary for
                        // scratch to be returned
                        scratch = ret_scratch;

                        self.rbuf.set_inner(buf);

                        resp
                    }
                    ParseStatus::Error(e, buf, _) => {
                        self.rbuf.set_inner(buf);

                        return Err(e.into());
                    }
                }
            }

            // take_inner aligns
            assert!(self.rbuf.is_readable_contiguous());

            if let Err(e) = recv_nonzero(&mut self.r, self.rbuf).await {
                if e.kind() == io::ErrorKind::WriteZero {
                    return Err(Error::BufferExceeded);
                }

                return Err(e.into());
            }
        };

        // at this point, resp has taken rbuf's inner buffer, such that
        // rbuf has no inner buffer

        // put remaining readable bytes in wbuf
        self.wbuf.write_all(resp.remaining_bytes())?;

        // swap inner buffers, such that rbuf now contains the remaining
        // readable bytes, and wbuf is now the one with no inner buffer
        self.rbuf.swap_inner(self.wbuf);

        Ok((
            resp,
            ResponseBodyKeepHeader {
                inner: ResponseBody {
                    inner: RefCell::new(Some(ResponseBodyInner {
                        r: self.r,
                        closed: false,
                        rbuf: self.rbuf,
                        resp_body,
                    })),
                },
                wbuf: RefCell::new(Some(self.wbuf)),
            },
        ))
    }
}

struct ResponseBodyInner<'a, R: AsyncRead> {
    r: ReadHalf<'a, R>,
    closed: bool,
    rbuf: &'a mut VecRingBuffer,
    resp_body: protocol::ClientResponseBody,
}

pub struct ResponseBody<'a, R: AsyncRead> {
    inner: RefCell<Option<ResponseBodyInner<'a, R>>>,
}

impl<'a, R: AsyncRead> ResponseBody<'a, R> {
    // on EOF and any subsequent calls, return success
    #[allow(clippy::await_holding_refcell_ref)]
    pub async fn add_to_buffer(&self) -> Result<(), Error> {
        if let Some(inner) = &mut *self.inner.borrow_mut() {
            if !inner.closed {
                match recv_nonzero(&mut inner.r, inner.rbuf).await {
                    Ok(()) => {}
                    Err(e) if e.kind() == io::ErrorKind::WriteZero => {
                        return Err(Error::BufferExceeded)
                    }
                    Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => inner.closed = true,
                    Err(e) => return Err(e.into()),
                }
            }

            Ok(())
        } else {
            Err(Error::Unusable)
        }
    }

    pub fn try_recv(&self, dest: &mut [u8]) -> Result<RecvStatus<(), Finished>, Error> {
        loop {
            let mut b_inner = self.inner.borrow_mut();

            if let Some(inner) = b_inner.take() {
                let mut scratch = mem::MaybeUninit::<[httparse::Header; HEADERS_MAX]>::uninit();

                let src = Buffer::read_buf(inner.rbuf);
                let end = src.len() == inner.rbuf.len() && inner.closed;

                match inner.resp_body.recv(src, dest, end, &mut scratch)? {
                    protocol::RecvStatus::NeedBytes(resp_body) => {
                        *b_inner = Some(ResponseBodyInner {
                            r: inner.r,
                            closed: inner.closed,
                            rbuf: inner.rbuf,
                            resp_body,
                        });

                        let inner = b_inner.as_mut().unwrap();

                        if !inner.rbuf.is_readable_contiguous() {
                            inner.rbuf.align();
                            continue;
                        }

                        return Ok(RecvStatus::NeedBytes(()));
                    }
                    protocol::RecvStatus::Complete(finished, read, written) => {
                        inner.rbuf.read_commit(read);

                        *b_inner = None;

                        return Ok(RecvStatus::Complete(Finished { inner: finished }, written));
                    }
                    protocol::RecvStatus::Read(resp_body, read, written) => {
                        *b_inner = Some(ResponseBodyInner {
                            r: inner.r,
                            closed: inner.closed,
                            rbuf: inner.rbuf,
                            resp_body,
                        });

                        let inner = b_inner.as_mut().unwrap();

                        inner.rbuf.read_commit(read);

                        if read > 0 && written == 0 {
                            // input consumed but no output produced, retry
                            continue;
                        }

                        // written is only zero here if read is also zero
                        assert!(written > 0 || read == 0);

                        return Ok(RecvStatus::Read((), written));
                    }
                }
            } else {
                return Err(Error::Unusable);
            }
        }
    }
}

pub struct ResponseBodyKeepHeader<'a, R: AsyncRead> {
    inner: ResponseBody<'a, R>,
    wbuf: RefCell<Option<&'a mut VecRingBuffer>>,
}

impl<'a, R: AsyncRead> ResponseBodyKeepHeader<'a, R> {
    pub fn discard_header<const N: usize>(
        self,
        resp: protocol::OwnedResponse<N>,
    ) -> Result<ResponseBody<'a, R>, Error> {
        if let Some(wbuf) = self.wbuf.borrow_mut().take() {
            wbuf.set_inner(resp.into_buf());
            wbuf.clear();

            Ok(self.inner)
        } else {
            Err(Error::Unusable)
        }
    }

    pub async fn add_to_buffer(&self) -> Result<(), Error> {
        self.inner.add_to_buffer().await
    }

    pub fn try_recv(
        &self,
        dest: &mut [u8],
    ) -> Result<RecvStatus<(), FinishedKeepHeader<'a>>, Error> {
        if !self.wbuf.borrow().is_some() {
            return Err(Error::Unusable);
        }

        match self.inner.try_recv(dest)? {
            RecvStatus::Complete(finished, written) => Ok(RecvStatus::Complete(
                FinishedKeepHeader {
                    inner: finished,
                    wbuf: self.wbuf.borrow_mut().take().unwrap(),
                },
                written,
            )),
            RecvStatus::Read((), written) => Ok(RecvStatus::Read((), written)),
            RecvStatus::NeedBytes(()) => Ok(RecvStatus::NeedBytes(())),
        }
    }
}

pub struct Finished {
    inner: protocol::ClientFinished,
}

impl Finished {
    pub fn is_persistent(&self) -> bool {
        self.inner.persistent
    }
}

pub struct FinishedKeepHeader<'a> {
    inner: Finished,
    wbuf: &'a mut VecRingBuffer,
}

impl FinishedKeepHeader<'_> {
    pub fn discard_header<const N: usize>(self, resp: protocol::OwnedResponse<N>) -> Finished {
        self.wbuf.set_inner(resp.into_buf());
        self.wbuf.clear();

        self.inner
    }

    pub fn is_persistent(&self) -> bool {
        self.inner.is_persistent()
    }
}
