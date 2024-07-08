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

use crate::core::arena;
use crate::core::event::{self, ReadinessExt};
use crate::core::reactor::{CustomEvented, Reactor, Registration, TimerEvented};
use std::cell::RefCell;
use std::future::Future;
use std::io::{self, Write};
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Context, Poll};
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
    use crate::core::channel;
    use crate::core::executor::Executor;
    use std::cell::Cell;
    use std::cmp;
    use std::rc::Rc;
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
