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

use crate::connmgr::tls::{TlsStream, TlsStreamError, VerifyMode};
use crate::core::arena;
use crate::core::event::{self, ReadinessExt};
use crate::core::net::AsyncTcpStream;
use crate::core::reactor::{CustomEvented, Reactor, Registration, TimerEvented};
use crate::core::waker::{RefWake, RefWaker, RefWakerData};
use mio::net::TcpStream;
use openssl::ssl;
use std::cell::{Ref, RefCell};
use std::future::Future;
use std::io::{self, Read, Write};
use std::mem;
use std::os::fd::{FromRawFd, IntoRawFd};
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Context, Poll, Waker};
use std::time::Instant;

pub struct PollFuture<F> {
    fut: F,
}

impl<F> Future for PollFuture<F>
where
    F: Future + Unpin,
{
    type Output = Poll<F::Output>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let s = &mut *self;

        Poll::Ready(Pin::new(&mut s.fut).poll(cx))
    }
}

pub fn poll_async<F>(fut: F) -> PollFuture<F>
where
    F: Future + Unpin,
{
    PollFuture { fut }
}

pub trait AsyncRead: Unpin {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut [u8],
    ) -> Poll<Result<usize, io::Error>>;

    fn cancel(&mut self);
}

pub trait AsyncWrite: Unpin {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>>;

    fn poll_write_vectored(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bufs: &[io::IoSlice<'_>],
    ) -> Poll<Result<usize, io::Error>> {
        for b in bufs {
            if !b.is_empty() {
                return self.poll_write(cx, b);
            }
        }

        self.poll_write(cx, &[])
    }

    fn poll_close(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>>;

    // for use with std Write
    fn is_writable(&self) -> bool;

    fn cancel(&mut self);
}

impl<T: ?Sized + AsyncRead> AsyncRead for &mut T {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut [u8],
    ) -> Poll<Result<usize, io::Error>> {
        Pin::new(&mut **self).poll_read(cx, buf)
    }

    fn cancel(&mut self) {
        AsyncRead::cancel(&mut **self)
    }
}

impl<T: ?Sized + AsyncWrite> AsyncWrite for &mut T {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        Pin::new(&mut **self).poll_write(cx, buf)
    }

    fn poll_write_vectored(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bufs: &[io::IoSlice<'_>],
    ) -> Poll<Result<usize, io::Error>> {
        Pin::new(&mut **self).poll_write_vectored(cx, bufs)
    }

    fn poll_close(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        Pin::new(&mut **self).poll_close(cx)
    }

    fn is_writable(&self) -> bool {
        AsyncWrite::is_writable(&**self)
    }

    fn cancel(&mut self) {
        AsyncWrite::cancel(&mut **self)
    }
}

pub trait AsyncReadExt: AsyncRead {
    fn read<'a>(&'a mut self, buf: &'a mut [u8]) -> ReadFuture<'a, Self> {
        ReadFuture { r: self, buf }
    }
}

pub trait AsyncWriteExt: AsyncWrite {
    fn write<'a>(&'a mut self, buf: &'a [u8]) -> WriteFuture<'a, Self> {
        WriteFuture {
            w: self,
            buf,
            pos: 0,
        }
    }

    fn write_vectored<'a>(
        &'a mut self,
        bufs: &'a [io::IoSlice<'a>],
    ) -> WriteVectoredFuture<'a, Self> {
        WriteVectoredFuture {
            w: self,
            bufs,
            pos: 0,
        }
    }

    fn close(&mut self) -> CloseFuture<'_, Self> {
        CloseFuture { w: self }
    }

    fn write_shared<'a, B>(&'a mut self, buf: &'a RefCell<B>) -> WriteSharedFuture<'a, Self, B>
    where
        B: AsRef<[u8]>,
    {
        WriteSharedFuture { w: self, buf }
    }
}

impl<R: AsyncRead + ?Sized> AsyncReadExt for R {}
impl<W: AsyncWrite + ?Sized> AsyncWriteExt for W {}

pub struct StdWriteWrapper<'a, 'b, W> {
    w: Pin<&'a mut W>,
    cx: &'a mut Context<'b>,
}

impl<'a, 'b, W: AsyncWrite> StdWriteWrapper<'a, 'b, W> {
    pub fn new(w: Pin<&'a mut W>, cx: &'a mut Context<'b>) -> Self {
        StdWriteWrapper { w, cx }
    }
}

impl<'a, 'b, W: AsyncWrite> Write for StdWriteWrapper<'a, 'b, W> {
    fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
        match self.w.as_mut().poll_write(self.cx, buf) {
            Poll::Ready(ret) => ret,
            Poll::Pending => Err(io::Error::from(io::ErrorKind::WouldBlock)),
        }
    }

    fn write_vectored(&mut self, bufs: &[io::IoSlice]) -> Result<usize, io::Error> {
        match self.w.as_mut().poll_write_vectored(self.cx, bufs) {
            Poll::Ready(ret) => ret,
            Poll::Pending => Err(io::Error::from(io::ErrorKind::WouldBlock)),
        }
    }

    fn flush(&mut self) -> Result<(), io::Error> {
        Ok(())
    }
}

pub struct ReadHalf<'a, T: AsyncRead> {
    handle: &'a RefCell<T>,
}

impl<T: AsyncRead> AsyncRead for ReadHalf<'_, T> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut [u8],
    ) -> Poll<Result<usize, io::Error>> {
        let mut handle = self.handle.borrow_mut();

        Pin::new(&mut *handle).poll_read(cx, buf)
    }

    fn cancel(&mut self) {
        self.handle.borrow_mut().cancel();
    }
}

pub struct WriteHalf<'a, T: AsyncWrite> {
    handle: &'a RefCell<T>,
}

impl<T: AsyncWrite> AsyncWrite for WriteHalf<'_, T> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        let mut handle = self.handle.borrow_mut();

        Pin::new(&mut *handle).poll_write(cx, buf)
    }

    fn poll_write_vectored(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bufs: &[io::IoSlice<'_>],
    ) -> Poll<Result<usize, io::Error>> {
        let mut handle = self.handle.borrow_mut();

        Pin::new(&mut *handle).poll_write_vectored(cx, bufs)
    }

    fn poll_close(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        let mut handle = self.handle.borrow_mut();

        Pin::new(&mut *handle).poll_close(cx)
    }

    fn is_writable(&self) -> bool {
        self.handle.borrow().is_writable()
    }

    fn cancel(&mut self) {
        self.handle.borrow_mut().cancel();
    }
}

pub fn io_split<T: AsyncRead + AsyncWrite>(handle: &RefCell<T>) -> (ReadHalf<T>, WriteHalf<T>) {
    (ReadHalf { handle }, WriteHalf { handle })
}

#[track_caller]
pub fn get_reactor() -> Reactor {
    Reactor::current().expect("no reactor in thread")
}

struct TlsOpInner {
    readiness: event::Readiness,
    waker: Option<(Waker, mio::Interest)>,
}

struct TlsOp {
    inner: RefCell<TlsOpInner>,
}

impl TlsOp {
    fn new() -> Self {
        Self {
            inner: RefCell::new(TlsOpInner {
                readiness: None,
                waker: None,
            }),
        }
    }

    fn readiness(&self) -> event::Readiness {
        self.inner.borrow().readiness
    }

    pub fn set_readiness(&self, readiness: event::Readiness) {
        self.inner.borrow_mut().readiness = readiness;
    }

    pub fn clear_readiness(&self, readiness: mio::Interest) {
        let inner = &mut *self.inner.borrow_mut();

        if let Some(cur) = inner.readiness.take() {
            inner.readiness = cur.remove(readiness);
        }
    }

    fn set_waker(&self, waker: &Waker, interest: mio::Interest) {
        let inner = &mut *self.inner.borrow_mut();

        let waker = if let Some((current_waker, _)) = inner.waker.take() {
            if current_waker.will_wake(waker) {
                // keep the current waker
                current_waker
            } else {
                // switch to the new waker
                waker.clone()
            }
        } else {
            // we didn't have a waker yet, so we'll use this one
            waker.clone()
        };

        inner.waker = Some((waker, interest));
    }

    fn clear_waker(&self) {
        let inner = &mut *self.inner.borrow_mut();

        inner.waker = None;
    }

    fn apply_readiness(&self, readiness: mio::Interest) {
        let inner = &mut *self.inner.borrow_mut();

        let (became_readable, became_writable) = {
            let prev_readiness = inner.readiness;

            inner.readiness.merge(readiness);

            (
                !prev_readiness.contains_any(mio::Interest::READABLE)
                    && inner.readiness.contains_any(mio::Interest::READABLE),
                !prev_readiness.contains_any(mio::Interest::WRITABLE)
                    && inner.readiness.contains_any(mio::Interest::WRITABLE),
            )
        };

        if became_readable || became_writable {
            if let Some((_, interest)) = &inner.waker {
                if (became_readable && interest.is_readable())
                    || (became_writable && interest.is_writable())
                {
                    let (waker, _) = inner.waker.take().unwrap();
                    waker.wake();
                }
            }
        }
    }
}

pub struct TlsWaker {
    registration: RefCell<Option<Registration>>,
    handshake: TlsOp,
    shutdown: TlsOp,
    read: TlsOp,
    write: TlsOp,
}

#[allow(clippy::new_without_default)]
impl TlsWaker {
    pub fn new() -> Self {
        Self {
            registration: RefCell::new(None),
            handshake: TlsOp::new(),
            shutdown: TlsOp::new(),
            read: TlsOp::new(),
            write: TlsOp::new(),
        }
    }

    fn registration(&self) -> Ref<'_, Registration> {
        Ref::map(self.registration.borrow(), |b| b.as_ref().unwrap())
    }

    fn set_registration(&self, registration: Registration) {
        let readiness = registration.readiness();

        registration.clear_readiness(mio::Interest::READABLE | mio::Interest::WRITABLE);

        for op in [&self.handshake, &self.shutdown, &self.read, &self.write] {
            op.set_readiness(readiness);
        }

        *self.registration.borrow_mut() = Some(registration);
    }

    fn take_registration(&self) -> Registration {
        self.registration.borrow_mut().take().unwrap()
    }
}

impl RefWake for TlsWaker {
    fn wake(&self) {
        if let Some(readiness) = self.registration().readiness() {
            self.registration()
                .clear_readiness(mio::Interest::READABLE | mio::Interest::WRITABLE);

            for op in [&self.handshake, &self.shutdown, &self.read, &self.write] {
                op.apply_readiness(readiness);
            }
        }
    }
}

pub struct AsyncTlsStream<'a> {
    waker: RefWaker<'a, TlsWaker>,
    stream: Option<TlsStream<TcpStream>>,
}

impl<'a: 'b, 'b> AsyncTlsStream<'a> {
    pub fn new(mut s: TlsStream<TcpStream>, waker_data: &'a RefWakerData<TlsWaker>) -> Self {
        let registration = get_reactor()
            .register_io(
                s.get_inner(),
                mio::Interest::READABLE | mio::Interest::WRITABLE,
            )
            .unwrap();

        // assume I/O operations are ready to be attempted
        registration.set_readiness(Some(mio::Interest::READABLE | mio::Interest::WRITABLE));

        Self::new_with_registration(s, waker_data, registration)
    }

    pub fn connect(
        domain: &str,
        stream: AsyncTcpStream,
        verify_mode: VerifyMode,
        waker_data: &'a RefWakerData<TlsWaker>,
    ) -> Result<Self, ssl::Error> {
        let (registration, stream) = stream.into_evented().into_parts();

        let stream = match TlsStream::connect(domain, stream, verify_mode) {
            Ok(stream) => stream,
            Err((mut stream, e)) => {
                registration.deregister_io(&mut stream).unwrap();

                return Err(e);
            }
        };

        Ok(Self::new_with_registration(
            stream,
            waker_data,
            registration,
        ))
    }

    pub fn ensure_handshake(&'b mut self) -> EnsureHandshakeFuture<'a, 'b> {
        EnsureHandshakeFuture { s: self }
    }

    pub fn inner(&mut self) -> &mut TlsStream<TcpStream> {
        self.stream.as_mut().unwrap()
    }

    pub fn into_inner(mut self) -> TlsStream<TcpStream> {
        let mut stream = self.stream.take().unwrap();

        self.waker
            .registration()
            .deregister_io(stream.get_inner())
            .unwrap();

        stream
    }

    pub fn into_std(mut self) -> TlsStream<std::net::TcpStream> {
        let mut stream = self.stream.take().unwrap();

        self.waker
            .registration()
            .deregister_io(stream.get_inner())
            .unwrap();

        stream.change_inner(|stream| unsafe {
            std::net::TcpStream::from_raw_fd(stream.into_raw_fd())
        })
    }

    // assumes stream is in non-blocking mode
    pub fn from_std(
        stream: TlsStream<std::net::TcpStream>,
        waker_data: &'a RefWakerData<TlsWaker>,
    ) -> Self {
        let stream = stream.change_inner(TcpStream::from_std);

        Self::new(stream, waker_data)
    }

    fn new_with_registration(
        s: TlsStream<TcpStream>,
        waker_data: &'a RefWakerData<TlsWaker>,
        registration: Registration,
    ) -> Self {
        let waker = RefWaker::new(waker_data);
        waker.set_registration(registration);

        waker.registration().set_waker_persistent(true);
        waker.registration().set_waker(
            waker.as_std(&mut mem::MaybeUninit::uninit()),
            mio::Interest::READABLE | mio::Interest::WRITABLE,
        );

        Self {
            waker,
            stream: Some(s),
        }
    }
}

impl<'a> Drop for AsyncTlsStream<'a> {
    fn drop(&mut self) {
        let registration = self.waker.take_registration();

        if let Some(stream) = &mut self.stream {
            registration.deregister_io(stream.get_inner()).unwrap();
        }
    }
}

pub struct Timeout {
    evented: TimerEvented,
}

impl Timeout {
    pub fn new(deadline: Instant) -> Self {
        let evented = TimerEvented::new(deadline, &get_reactor()).unwrap();

        evented.registration().set_ready(true);

        Self { evented }
    }

    pub fn set_deadline(&self, deadline: Instant) {
        self.evented.set_expires(deadline).unwrap();

        self.evented.registration().set_ready(true);
    }

    pub fn elapsed(&self) -> TimeoutFuture<'_> {
        TimeoutFuture { t: self }
    }
}

pub struct CancellationSender {
    read_set_readiness: event::LocalSetReadiness,
}

impl CancellationSender {
    fn cancel(&self) {
        self.read_set_readiness
            .set_readiness(mio::Interest::READABLE)
            .unwrap();
    }
}

impl Drop for CancellationSender {
    fn drop(&mut self) {
        self.cancel();
    }
}

pub struct CancellationToken {
    evented: CustomEvented,
    _read_registration: event::LocalRegistration,
}

impl CancellationToken {
    pub fn new(
        memory: &Rc<arena::RcMemory<event::LocalRegistrationEntry>>,
    ) -> (CancellationSender, Self) {
        let (read_reg, read_sr) = event::LocalRegistration::new(memory);

        let evented =
            CustomEvented::new_local(&read_reg, mio::Interest::READABLE, &get_reactor()).unwrap();

        evented.registration().set_ready(false);

        let sender = CancellationSender {
            read_set_readiness: read_sr,
        };

        let token = Self {
            evented,
            _read_registration: read_reg,
        };

        (sender, token)
    }

    pub fn cancelled(&self) -> CancelledFuture<'_> {
        CancelledFuture { t: self }
    }
}

pub struct EventWaiter<'a> {
    registration: &'a Registration,
}

impl<'a> EventWaiter<'a> {
    pub fn new(registration: &'a Registration) -> Self {
        Self { registration }
    }

    pub fn wait(&'a self, interest: mio::Interest) -> WaitFuture<'a> {
        WaitFuture { w: self, interest }
    }
}

pub struct ReadFuture<'a, R: AsyncRead + ?Sized> {
    r: &'a mut R,
    buf: &'a mut [u8],
}

impl<'a, R: AsyncRead + ?Sized> Future for ReadFuture<'a, R> {
    type Output = Result<usize, io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        let r: Pin<&mut R> = Pin::new(f.r);

        r.poll_read(cx, f.buf)
    }
}

impl<'a, R: AsyncRead + ?Sized> Drop for ReadFuture<'a, R> {
    fn drop(&mut self) {
        self.r.cancel();
    }
}

pub struct WriteFuture<'a, W: AsyncWrite + ?Sized + Unpin> {
    w: &'a mut W,
    buf: &'a [u8],
    pos: usize,
}

impl<'a, W: AsyncWrite + ?Sized> Future for WriteFuture<'a, W> {
    type Output = Result<usize, io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        let mut w: Pin<&mut W> = Pin::new(f.w);

        // try to write all the data before producing a result
        while f.pos < f.buf.len() {
            match w.as_mut().poll_write(cx, &f.buf[f.pos..]) {
                Poll::Ready(result) => match result {
                    Ok(size) => f.pos += size,
                    Err(e) => return Poll::Ready(Err(e)),
                },
                Poll::Pending => return Poll::Pending,
            }
        }

        Poll::Ready(Ok(f.buf.len()))
    }
}

impl<'a, W: AsyncWrite + ?Sized> Drop for WriteFuture<'a, W> {
    fn drop(&mut self) {
        self.w.cancel();
    }
}

pub struct CloseFuture<'a, W: AsyncWrite + ?Sized> {
    w: &'a mut W,
}

impl<'a, W: AsyncWrite + ?Sized> Future for CloseFuture<'a, W> {
    type Output = Result<(), io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        let w: Pin<&mut W> = Pin::new(f.w);

        w.poll_close(cx)
    }
}

impl<'a, W: AsyncWrite + ?Sized> Drop for CloseFuture<'a, W> {
    fn drop(&mut self) {
        self.w.cancel();
    }
}

fn get_start_offset(bufs: &[io::IoSlice], pos: usize) -> (usize, usize) {
    let mut start = 0;
    let mut offset = pos;

    for buf in bufs {
        if offset < buf.len() {
            break;
        }

        start += 1;
        offset -= buf.len();
    }

    (start, offset)
}

pub struct WriteVectoredFuture<'a, W: AsyncWrite + ?Sized + Unpin> {
    w: &'a mut W,
    bufs: &'a [io::IoSlice<'a>],
    pos: usize,
}

impl<'a, W: AsyncWrite + ?Sized> Future for WriteVectoredFuture<'a, W> {
    type Output = Result<usize, io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        let mut w: Pin<&mut W> = Pin::new(f.w);

        // try to write all the data before producing a result
        loop {
            let (start, offset) = get_start_offset(f.bufs, f.pos);

            if start >= f.bufs.len() {
                break;
            }

            if offset == 0 {
                match w.as_mut().poll_write_vectored(cx, &f.bufs[start..]) {
                    Poll::Ready(result) => match result {
                        Ok(size) => f.pos += size,
                        Err(e) => return Poll::Ready(Err(e)),
                    },
                    Poll::Pending => return Poll::Pending,
                }
            } else {
                match w.as_mut().poll_write(cx, &f.bufs[start][offset..]) {
                    Poll::Ready(result) => match result {
                        Ok(size) => f.pos += size,
                        Err(e) => return Poll::Ready(Err(e)),
                    },
                    Poll::Pending => return Poll::Pending,
                }
            }
        }

        Poll::Ready(Ok(f.pos))
    }
}

impl<'a, W: AsyncWrite + ?Sized> Drop for WriteVectoredFuture<'a, W> {
    fn drop(&mut self) {
        self.w.cancel();
    }
}

pub struct WriteSharedFuture<'a, W: AsyncWrite + ?Sized + Unpin, B: AsRef<[u8]>> {
    w: &'a mut W,
    buf: &'a RefCell<B>,
}

impl<'a, W: AsyncWrite + ?Sized, B: AsRef<[u8]>> Future for WriteSharedFuture<'a, W, B> {
    type Output = Result<usize, io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        let w: Pin<&mut W> = Pin::new(f.w);

        w.poll_write(cx, f.buf.borrow().as_ref())
    }
}

impl<'a, W: AsyncWrite + ?Sized, B: AsRef<[u8]>> Drop for WriteSharedFuture<'a, W, B> {
    fn drop(&mut self) {
        self.w.cancel();
    }
}

impl AsyncRead for AsyncTlsStream<'_> {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &mut [u8],
    ) -> Poll<Result<usize, io::Error>> {
        let f = &mut *self;

        let registration = f.waker.registration();
        let op = &f.waker.read;
        let stream = f.stream.as_mut().unwrap();

        let interests = stream.interests_for_read();

        if let Some(interests) = interests {
            if !op.readiness().contains_any(interests) {
                op.set_waker(cx.waker(), interests);

                return Poll::Pending;
            }
        }

        if !registration.pull_from_budget_with_waker(cx.waker()) {
            return Poll::Pending;
        }

        match stream.read(buf) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                let interests = stream.interests_for_read().unwrap();
                op.clear_readiness(interests);
                op.set_waker(cx.waker(), interests);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn cancel(&mut self) {
        let op = &self.waker.read;

        op.clear_waker();
    }
}

impl AsyncWrite for AsyncTlsStream<'_> {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        let f = &mut *self;

        let registration = f.waker.registration();
        let op = &f.waker.write;
        let stream = f.stream.as_mut().unwrap();

        let interests = stream.interests_for_write();

        if let Some(interests) = interests {
            if !op.readiness().contains_any(interests) {
                op.set_waker(cx.waker(), interests);

                return Poll::Pending;
            }
        }

        if !registration.pull_from_budget_with_waker(cx.waker()) {
            return Poll::Pending;
        }

        match stream.write(buf) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                let interests = stream.interests_for_write().unwrap();
                op.clear_readiness(interests);
                op.set_waker(cx.waker(), interests);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn poll_close(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Result<(), io::Error>> {
        let f = &mut *self;

        let registration = f.waker.registration();
        let op = &f.waker.shutdown;
        let stream = f.stream.as_mut().unwrap();

        let interests = stream.interests_for_shutdown();

        if let Some(interests) = interests {
            if !op.readiness().contains_any(interests) {
                op.set_waker(cx.waker(), interests);

                return Poll::Pending;
            }
        }

        if !registration.pull_from_budget_with_waker(cx.waker()) {
            return Poll::Pending;
        }

        match stream.shutdown() {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                let interests = stream.interests_for_shutdown().unwrap();
                op.clear_readiness(interests);
                op.set_waker(cx.waker(), interests);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn is_writable(&self) -> bool {
        let op = &self.waker.write;
        let stream = self.stream.as_ref().unwrap();

        if let Some(interests) = stream.interests_for_write() {
            op.readiness().contains_any(interests)
        } else {
            true
        }
    }

    fn cancel(&mut self) {
        let write_op = &self.waker.write;
        let shutdown_op = &self.waker.shutdown;

        write_op.clear_waker();
        shutdown_op.clear_waker();
    }
}

pub struct EnsureHandshakeFuture<'a, 'b> {
    s: &'b mut AsyncTlsStream<'a>,
}

impl Future for EnsureHandshakeFuture<'_, '_> {
    type Output = Result<(), TlsStreamError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        let registration = f.s.waker.registration();
        let op = &f.s.waker.handshake;
        let stream = f.s.stream.as_mut().unwrap();

        let interests = stream.interests_for_handshake();

        if let Some(interests) = interests {
            if !op.readiness().contains_any(interests) {
                op.set_waker(cx.waker(), interests);

                return Poll::Pending;
            }
        }

        if !registration.pull_from_budget_with_waker(cx.waker()) {
            return Poll::Pending;
        }

        match stream.ensure_handshake() {
            Ok(()) => Poll::Ready(Ok(())),
            Err(TlsStreamError::Io(e)) if e.kind() == io::ErrorKind::WouldBlock => {
                let interests = stream.interests_for_handshake().unwrap();
                op.clear_readiness(interests);
                op.set_waker(cx.waker(), interests);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for EnsureHandshakeFuture<'_, '_> {
    fn drop(&mut self) {
        let op = &self.s.waker.handshake;

        op.clear_waker();
    }
}

pub struct TimeoutFuture<'a> {
    t: &'a Timeout,
}

impl Future for TimeoutFuture<'_> {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let evented = &self.t.evented;

        evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        if !evented.registration().is_ready() {
            return Poll::Pending;
        }

        let now = get_reactor().now();

        if now >= evented.expires() {
            Poll::Ready(())
        } else {
            evented.registration().set_ready(false);

            Poll::Pending
        }
    }
}

impl Drop for TimeoutFuture<'_> {
    fn drop(&mut self) {
        self.t.evented.registration().clear_waker();
    }
}

pub struct CancelledFuture<'a> {
    t: &'a CancellationToken,
}

impl Future for CancelledFuture<'_> {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        if f.t.evented.registration().is_ready() {
            return Poll::Ready(());
        }

        f.t.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        Poll::Pending
    }
}

impl Drop for CancelledFuture<'_> {
    fn drop(&mut self) {
        self.t.evented.registration().clear_waker();
    }
}

pub struct WaitFuture<'a> {
    w: &'a EventWaiter<'a>,
    interest: mio::Interest,
}

impl Future for WaitFuture<'_> {
    type Output = mio::Interest;

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &*self;

        f.w.registration.set_waker(cx.waker(), f.interest);

        if !f.w.registration.readiness().contains_any(f.interest) {
            return Poll::Pending;
        }

        let readiness = f.w.registration.readiness().unwrap();

        // mask with the interest
        let readable = readiness.is_readable() && f.interest.is_readable();
        let writable = readiness.is_writable() && f.interest.is_writable();
        let readiness = if readable && writable {
            mio::Interest::READABLE | mio::Interest::WRITABLE
        } else if readable {
            mio::Interest::READABLE
        } else {
            mio::Interest::WRITABLE
        };

        Poll::Ready(readiness)
    }
}

impl Drop for WaitFuture<'_> {
    fn drop(&mut self) {
        self.w.registration.clear_waker();
    }
}

pub async fn event_wait(registration: &Registration, interest: mio::Interest) -> mio::Interest {
    EventWaiter::new(registration).wait(interest).await
}

pub struct YieldFuture {
    done: bool,
}

impl Future for YieldFuture {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        if !f.done {
            f.done = true;
            cx.waker().wake_by_ref();

            Poll::Pending
        } else {
            Poll::Ready(())
        }
    }
}

pub fn yield_task() -> YieldFuture {
    YieldFuture { done: false }
}

pub struct YieldToLocalEvents {
    started: bool,
    evented: CustomEvented,
    _registration: event::LocalRegistration,
    set_readiness: event::LocalSetReadiness,
}

impl YieldToLocalEvents {
    fn new() -> Self {
        let reactor = get_reactor();

        let (reg, sr) = event::LocalRegistration::new(&reactor.local_registration_memory());

        let evented = CustomEvented::new_local(&reg, mio::Interest::READABLE, &reactor).unwrap();

        evented.registration().set_ready(false);

        Self {
            started: false,
            evented,
            _registration: reg,
            set_readiness: sr,
        }
    }
}

impl Future for YieldToLocalEvents {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        if f.evented.registration().is_ready() {
            return Poll::Ready(());
        }

        f.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        if !f.started {
            f.started = true;

            // this will wake us up after all local events before it have been processed
            f.set_readiness
                .set_readiness(mio::Interest::READABLE)
                .unwrap();
        }

        Poll::Pending
    }
}

pub fn yield_to_local_events() -> YieldToLocalEvents {
    YieldToLocalEvents::new()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::connmgr::tls::TlsAcceptor;
    use crate::core::channel;
    use crate::core::executor::Executor;
    use crate::core::net::AsyncTcpListener;
    use std::cell::Cell;
    use std::cmp;
    use std::rc::Rc;
    use std::str;
    use std::task::Context;
    use std::time::Duration;

    struct TestBuffer {
        data: Vec<u8>,
    }

    impl TestBuffer {
        fn new() -> Self {
            Self { data: Vec::new() }
        }
    }

    impl AsyncRead for TestBuffer {
        fn poll_read(
            mut self: Pin<&mut Self>,
            _cx: &mut Context,
            buf: &mut [u8],
        ) -> Poll<Result<usize, io::Error>> {
            let size = cmp::min(buf.len(), self.data.len());

            let left = self.data.split_off(size);

            (&mut buf[..size]).copy_from_slice(&self.data);

            self.data = left;

            Poll::Ready(Ok(size))
        }

        fn cancel(&mut self) {}
    }

    impl AsyncWrite for TestBuffer {
        fn poll_write(
            mut self: Pin<&mut Self>,
            _cx: &mut Context,
            buf: &[u8],
        ) -> Poll<Result<usize, io::Error>> {
            let size = self.data.write(buf).unwrap();

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

    #[test]
    fn test_read_write() {
        let executor = Executor::new(1);

        executor
            .spawn(async {
                let mut buf = TestBuffer::new();

                let mut data = [0; 16];

                assert_eq!(buf.read(&mut data).await.unwrap(), 0);
                assert_eq!(buf.write(b"hello").await.unwrap(), 5);
                assert_eq!(buf.read(&mut data).await.unwrap(), 5);
                assert_eq!(&data[..5], b"hello");
            })
            .unwrap();

        executor.run(|_| Ok(())).unwrap();
    }

    #[test]
    fn test_read_write_concurrent() {
        let executor = Executor::new(1);

        executor
            .spawn(async {
                let buf = RefCell::new(TestBuffer::new());
                let (mut r, mut w) = io_split(&buf);

                let mut data = [0; 16];

                let write_fut = w.write(b"hello");
                let read_fut = r.read(&mut data);

                assert_eq!(write_fut.await.unwrap(), 5);
                assert_eq!(read_fut.await.unwrap(), 5);
                assert_eq!(&data[..5], b"hello");
            })
            .unwrap();

        executor.run(|_| Ok(())).unwrap();
    }

    #[test]
    fn test_write_vectored() {
        let executor = Executor::new(1);

        executor
            .spawn(async {
                let mut buf = TestBuffer::new();

                let mut data = [0; 16];

                assert_eq!(buf.read(&mut data).await.unwrap(), 0);
                assert_eq!(
                    buf.write_vectored(&[
                        io::IoSlice::new(b"he"),
                        io::IoSlice::new(b"l"),
                        io::IoSlice::new(b"lo")
                    ])
                    .await
                    .unwrap(),
                    5
                );
                assert_eq!(buf.read(&mut data).await.unwrap(), 5);
                assert_eq!(&data[..5], b"hello");
            })
            .unwrap();

        executor.run(|_| Ok(())).unwrap();
    }

    #[test]
    fn test_tlsstream() {
        let reactor = Reactor::new(3); // 3 registrations
        let executor = Executor::new(2); // 2 tasks

        let spawner = executor.spawner();

        executor
            .spawn(async move {
                let addr = "127.0.0.1:0".parse().unwrap();
                let listener = AsyncTcpListener::bind(addr).expect("failed to bind");
                let acceptor = TlsAcceptor::new_self_signed();
                let addr = listener.local_addr().unwrap();

                spawner
                    .spawn(async move {
                        let stream = AsyncTcpStream::connect(&[addr]).await.unwrap();
                        let tls_waker_data = RefWakerData::new(TlsWaker::new());
                        let mut stream = AsyncTlsStream::connect(
                            "localhost",
                            stream,
                            VerifyMode::None,
                            &tls_waker_data,
                        )
                        .unwrap();

                        stream.ensure_handshake().await.unwrap();

                        let size = stream.write("hello".as_bytes()).await.unwrap();
                        assert_eq!(size, 5);

                        stream.close().await.unwrap();
                    })
                    .unwrap();

                let (stream, _) = listener.accept().await.unwrap();
                let stream = acceptor.accept(stream).unwrap();

                let tls_waker_data = RefWakerData::new(TlsWaker::new());
                let mut stream = AsyncTlsStream::new(stream, &tls_waker_data);

                let mut resp = [0u8; 1024];
                let mut resp = io::Cursor::new(&mut resp[..]);

                loop {
                    let mut buf = [0; 1024];

                    let size = stream.read(&mut buf).await.unwrap();
                    if size == 0 {
                        break;
                    }

                    resp.write(&buf[..size]).unwrap();
                }

                let size = resp.position() as usize;
                let resp = str::from_utf8(&resp.get_ref()[..size]).unwrap();

                assert_eq!(resp, "hello");

                stream.close().await.unwrap();
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn test_timeout() {
        let now = Instant::now();

        let executor = Executor::new(1);
        let reactor = Reactor::new_with_time(1, now);

        executor
            .spawn(async {
                let timeout = Timeout::new(get_reactor().now() + Duration::from_millis(100));
                timeout.elapsed().await;
            })
            .unwrap();

        executor.run_until_stalled();

        reactor
            .poll_nonblocking(now + Duration::from_millis(200))
            .unwrap();

        executor.run(|_| Ok(())).unwrap();
    }

    #[test]
    fn test_timeout_ready() {
        let now = Instant::now();

        let executor = Executor::new(1);
        let _reactor = Reactor::new_with_time(1, now);

        executor
            .spawn(async {
                let timeout = Timeout::new(get_reactor().now());
                timeout.elapsed().await;
            })
            .unwrap();

        executor.run(|_| Ok(())).unwrap();
    }

    #[test]
    fn test_timeout_change_ready() {
        let now = Instant::now();

        let _reactor = Reactor::new_with_time(1, now);
        let executor = Executor::new(1);

        executor
            .spawn(async {
                let timeout = Timeout::new(get_reactor().now() + Duration::from_millis(100));

                let mut fut = timeout.elapsed();
                assert_eq!(poll_async(&mut fut).await, Poll::Pending);

                timeout.set_deadline(get_reactor().now());
                assert_eq!(poll_async(&mut fut).await, Poll::Ready(()));
            })
            .unwrap();

        executor.run(|_| Ok(())).unwrap();
    }

    #[test]
    fn test_cancellation_token() {
        let reactor = Reactor::new(1);
        let executor = Executor::new(1);

        executor
            .spawn(async {
                let (sender, token) =
                    CancellationToken::new(&get_reactor().local_registration_memory());

                let mut fut = token.cancelled();
                assert_eq!(poll_async(&mut fut).await, Poll::Pending);

                drop(sender);
                token.cancelled().await;
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn test_event_wait() {
        let reactor = Reactor::new(2);
        let executor = Executor::new(2);

        let (s, r) = channel::local_channel::<u32>(1, 1, &reactor.local_registration_memory());

        let s = channel::AsyncLocalSender::new(s);

        executor
            .spawn(async move {
                let reactor = Reactor::current().unwrap();

                let reg = reactor
                    .register_custom_local(r.get_read_registration(), mio::Interest::READABLE)
                    .unwrap();

                assert_eq!(
                    event_wait(&reg, mio::Interest::READABLE).await,
                    mio::Interest::READABLE
                );
            })
            .unwrap();

        executor.run_until_stalled();

        assert_eq!(executor.have_tasks(), true);

        executor
            .spawn(async move {
                s.send(1).await.unwrap();
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    struct PollCountFuture<F> {
        count: i32,
        fut: F,
    }

    impl<F> PollCountFuture<F>
    where
        F: Future<Output = ()> + Unpin,
    {
        fn new(fut: F) -> Self {
            Self { count: 0, fut }
        }
    }

    impl<F> Future for PollCountFuture<F>
    where
        F: Future<Output = ()> + Unpin,
    {
        type Output = i32;

        fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
            let f = &mut *self;

            f.count += 1;

            match Pin::new(&mut f.fut).poll(cx) {
                Poll::Ready(()) => Poll::Ready(f.count),
                Poll::Pending => Poll::Pending,
            }
        }
    }

    #[test]
    fn test_yield_task() {
        let executor = Executor::new(1);

        executor
            .spawn(async {
                assert_eq!(PollCountFuture::new(yield_task()).await, 2);
            })
            .unwrap();

        executor.run(|_| Ok(())).unwrap();
    }

    #[test]
    fn test_yield_to_local_events() {
        let reactor = Reactor::new(3);
        let executor = Executor::new(2);

        let (s, r) = channel::local_channel(1, 1, &reactor.local_registration_memory());

        let state = Rc::new(Cell::new(0));

        {
            let state = Rc::clone(&state);

            executor
                .spawn(async move {
                    let r = channel::AsyncLocalReceiver::new(r);
                    r.recv().await.unwrap();
                    state.set(1);
                })
                .unwrap();
        }

        executor.run_until_stalled();

        executor
            .spawn(async move {
                s.try_send(1).unwrap();
                yield_to_local_events().await;
                assert_eq!(state.get(), 1);
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }
}
