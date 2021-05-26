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

use crate::channel;
use crate::reactor::{CustomEvented, IoEvented, Reactor, TimerEvented};
use mio;
use mio::net::{TcpListener, TcpStream};
use std::cell::Cell;
use std::future::Future;
use std::io;
use std::io::{Read, Write};
use std::net::SocketAddr;
use std::pin::Pin;
use std::sync::mpsc;
use std::task::{Context, Poll};
use std::time::{Duration, Instant};

pub struct SelectFromSliceFuture<'a, F> {
    futures: &'a mut [F],
}

impl<F, O> Future for SelectFromSliceFuture<'_, F>
where
    F: Future<Output = O>,
{
    type Output = (usize, F::Output);

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        for (i, f) in self.futures.iter_mut().enumerate() {
            let p = unsafe { Pin::new_unchecked(f) };

            if let Poll::Ready(v) = p.poll(cx) {
                return Poll::Ready((i, v));
            }
        }

        Poll::Pending
    }
}

pub fn select_from_slice<'a, F, O>(futures: &'a mut [F]) -> SelectFromSliceFuture<'a, F>
where
    F: Future<Output = O>,
{
    SelectFromSliceFuture { futures }
}

pub struct SelectFromPairFuture<F1, F2> {
    f1: F1,
    f2: F2,
}

impl<F1, F2, O1, O2> Future for SelectFromPairFuture<F1, F2>
where
    F1: Future<Output = O1>,
    F2: Future<Output = O2>,
{
    type Output = (Option<F1::Output>, Option<F2::Output>);

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f1 = unsafe { self.as_mut().map_unchecked_mut(|s| &mut s.f1) };

        if let Poll::Ready(v) = f1.poll(cx) {
            return Poll::Ready((Some(v), None));
        }

        let f2 = unsafe { self.as_mut().map_unchecked_mut(|s| &mut s.f2) };

        if let Poll::Ready(v) = f2.poll(cx) {
            return Poll::Ready((None, Some(v)));
        }

        Poll::Pending
    }
}

pub fn select_from_pair<F1, F2, O1, O2>(f1: F1, f2: F2) -> SelectFromPairFuture<F1, F2>
where
    F1: Future<Output = O1>,
    F2: Future<Output = O2>,
{
    SelectFromPairFuture { f1, f2 }
}

#[track_caller]
fn get_reactor() -> Reactor {
    Reactor::current().expect("no reactor in thread")
}

pub struct AsyncSender<T> {
    inner: channel::Sender<T>,
    evented: CustomEvented,
    writable: Cell<bool>,
}

impl<T> AsyncSender<T> {
    pub fn new(s: channel::Sender<T>) -> Self {
        let evented = CustomEvented::new(
            s.get_write_registration(),
            mio::Interest::WRITABLE,
            &get_reactor(),
        )
        .unwrap();

        let writable = s.can_send();

        // we know the state up front, so ready can start out unset
        evented.registration().set_ready(false);

        Self {
            inner: s,
            evented,
            writable: Cell::new(writable),
        }
    }

    pub fn is_writable(&self) -> bool {
        let dirty = self.evented.registration().is_ready();

        if dirty {
            self.writable.set(self.inner.can_send());
            self.evented.registration().set_ready(false);
        }

        self.writable.get()
    }

    pub fn wait_writable<'a>(&'a mut self) -> WaitWritableFuture<'a, T> {
        WaitWritableFuture { s: self }
    }

    pub fn try_send(&mut self, t: T) -> Result<(), mpsc::TrySendError<T>> {
        match self.inner.try_send(t) {
            Ok(_) => {
                self.writable.set(self.inner.can_send());

                Ok(())
            }
            Err(mpsc::TrySendError::Full(t)) => {
                self.writable.set(self.inner.can_send());

                Err(mpsc::TrySendError::Full(t))
            }
            Err(mpsc::TrySendError::Disconnected(t)) => {
                self.writable.set(false);

                Err(mpsc::TrySendError::Disconnected(t))
            }
        }
    }
}

pub struct AsyncReceiver<T> {
    inner: channel::Receiver<T>,
    evented: CustomEvented,
}

impl<T> AsyncReceiver<T> {
    pub fn new(r: channel::Receiver<T>) -> Self {
        let evented = CustomEvented::new(
            r.get_read_registration(),
            mio::Interest::READABLE,
            &get_reactor(),
        )
        .unwrap();

        evented.registration().set_ready(true);

        Self { inner: r, evented }
    }

    pub fn recv<'a>(&'a mut self) -> RecvFuture<'a, T> {
        RecvFuture { r: self }
    }
}

pub struct AsyncTcpListener {
    evented: IoEvented<TcpListener>,
}

impl AsyncTcpListener {
    pub fn new(l: TcpListener) -> Self {
        let evented = IoEvented::new(l, mio::Interest::READABLE, &get_reactor()).unwrap();

        evented.registration().set_ready(true);

        Self { evented }
    }

    pub fn bind(addr: SocketAddr) -> Result<Self, io::Error> {
        let listener = TcpListener::bind(addr)?;

        Ok(Self::new(listener))
    }

    pub fn local_addr(&self) -> Result<SocketAddr, io::Error> {
        self.evented.io().local_addr()
    }

    pub fn accept<'a>(&'a mut self) -> AcceptFuture<'a> {
        AcceptFuture { l: self }
    }
}

pub struct AsyncTcpStream {
    evented: IoEvented<TcpStream>,
}

impl AsyncTcpStream {
    pub fn new(s: TcpStream) -> Self {
        let evented = IoEvented::new(
            s,
            mio::Interest::READABLE | mio::Interest::WRITABLE,
            &get_reactor(),
        )
        .unwrap();

        // when constructing via new(), assume I/O operations are ready to be
        // attempted
        evented.registration().set_ready(true);

        Self { evented }
    }

    pub async fn connect<'a>(addr: SocketAddr) -> Result<Self, io::Error> {
        let stream = TcpStream::connect(addr)?;
        let mut stream = Self::new(stream);

        // when constructing via connect(), the ready state should start out
        // false because we need to wait for a writability indication
        stream.evented.registration().set_ready(false);

        let fut = TcpConnectFuture { s: &mut stream };
        fut.await?;

        Ok(stream)
    }

    pub fn read<'a>(&'a mut self, buf: &'a mut [u8]) -> TcpReadFuture<'a> {
        TcpReadFuture { s: self, buf }
    }

    pub fn write<'a>(&'a mut self, buf: &'a [u8]) -> TcpWriteFuture<'a> {
        TcpWriteFuture {
            s: self,
            buf,
            pos: 0,
        }
    }
}

pub struct AsyncSleep {
    evented: TimerEvented,
}

impl AsyncSleep {
    pub fn new(expires: Instant) -> Self {
        let evented = TimerEvented::new(expires, &get_reactor()).unwrap();

        evented.registration().set_ready(true);

        Self { evented }
    }

    pub fn sleep<'a>(&'a mut self) -> SleepFuture<'a> {
        SleepFuture { s: self }
    }
}

pub struct WaitWritableFuture<'a, T> {
    s: &'a mut AsyncSender<T>,
}

impl<T> Future for WaitWritableFuture<'_, T> {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented.registration().set_waker(cx.waker().clone());

        let dirty = f.s.evented.registration().is_ready();

        if dirty {
            f.s.writable.set(f.s.inner.can_send());
            f.s.evented.registration().set_ready(false);
        }

        if f.s.writable.get() {
            Poll::Ready(())
        } else {
            Poll::Pending
        }
    }
}

impl<T> Drop for WaitWritableFuture<'_, T> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub struct RecvFuture<'a, T> {
    r: &'a mut AsyncReceiver<T>,
}

impl<T> Future for RecvFuture<'_, T> {
    type Output = Result<T, mpsc::RecvError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.r.evented.registration().set_waker(cx.waker().clone());

        if !f.r.evented.registration().is_ready() {
            return Poll::Pending;
        }

        match f.r.inner.try_recv() {
            Ok(v) => Poll::Ready(Ok(v)),
            Err(mpsc::TryRecvError::Empty) => {
                f.r.evented.registration().set_ready(false);

                Poll::Pending
            }
            Err(mpsc::TryRecvError::Disconnected) => Poll::Ready(Err(mpsc::RecvError)),
        }
    }
}

impl<T> Drop for RecvFuture<'_, T> {
    fn drop(&mut self) {
        self.r.evented.registration().clear_waker();
    }
}

pub struct AcceptFuture<'a> {
    l: &'a mut AsyncTcpListener,
}

impl Future for AcceptFuture<'_> {
    type Output = Result<(TcpStream, SocketAddr), io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.l.evented.registration().set_waker(cx.waker().clone());

        if !f.l.evented.registration().is_ready() {
            return Poll::Pending;
        }

        match f.l.evented.io().accept() {
            Ok((stream, peer_addr)) => Poll::Ready(Ok((stream, peer_addr))),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.l.evented.registration().set_ready(false);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for AcceptFuture<'_> {
    fn drop(&mut self) {
        self.l.evented.registration().clear_waker();
    }
}

pub struct TcpConnectFuture<'a> {
    s: &'a mut AsyncTcpStream,
}

impl Future for TcpConnectFuture<'_> {
    type Output = Result<(), io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented.registration().set_waker(cx.waker().clone());

        if !f.s.evented.registration().is_ready() {
            return Poll::Pending;
        }

        let maybe_error = match f.s.evented.io().take_error() {
            Ok(me) => me,
            Err(e) => return Poll::Ready(Err(e)),
        };

        if let Some(e) = maybe_error {
            return Poll::Ready(Err(e));
        }

        Poll::Ready(Ok(()))
    }
}

impl Drop for TcpConnectFuture<'_> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub struct TcpReadFuture<'a> {
    s: &'a mut AsyncTcpStream,
    buf: &'a mut [u8],
}

impl Future for TcpReadFuture<'_> {
    type Output = Result<usize, io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented.registration().set_waker(cx.waker().clone());

        if !f.s.evented.registration().is_ready() {
            return Poll::Pending;
        }

        match f.s.evented.io().read(f.buf) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.s.evented.registration().set_ready(false);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for TcpReadFuture<'_> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub struct TcpWriteFuture<'a> {
    s: &'a mut AsyncTcpStream,
    buf: &'a [u8],
    pos: usize,
}

impl Future for TcpWriteFuture<'_> {
    type Output = Result<usize, io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented.registration().set_waker(cx.waker().clone());

        if !f.s.evented.registration().is_ready() {
            return Poll::Pending;
        }

        // try to write all the data before producing a result, the same as
        // what a blocking write would do
        loop {
            match f.s.evented.io().write(&f.buf[f.pos..]) {
                Ok(size) => {
                    f.pos += size;

                    if f.pos >= f.buf.len() {
                        break;
                    }
                }
                Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                    f.s.evented.registration().set_ready(false);

                    return Poll::Pending;
                }
                Err(e) => return Poll::Ready(Err(e)),
            }
        }

        Poll::Ready(Ok(f.buf.len()))
    }
}

impl Drop for TcpWriteFuture<'_> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub struct SleepFuture<'a> {
    s: &'a mut AsyncSleep,
}

impl Future for SleepFuture<'_> {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented.registration().set_waker(cx.waker().clone());

        if !f.s.evented.registration().is_ready() {
            return Poll::Pending;
        }

        let now = get_reactor().now();

        if now >= f.s.evented.expires() {
            Poll::Ready(())
        } else {
            f.s.evented.registration().set_ready(false);

            Poll::Pending
        }
    }
}

impl Drop for SleepFuture<'_> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub async fn sleep(duration: Duration) {
    let now = get_reactor().now();

    AsyncSleep::new(now + duration).sleep().await
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::executor::Executor;

    #[test]
    fn test_tcpstream() {
        let executor = Executor::new(2); // 2 tasks
        let reactor = Reactor::new(3); // 3 registrations

        let spawner = executor.spawner();

        executor
            .spawn(async move {
                let addr = "127.0.0.1:0".parse().unwrap();
                let mut listener = AsyncTcpListener::bind(addr).expect("failed to bind");
                let addr = listener.local_addr().unwrap();

                spawner
                    .spawn(async move {
                        let mut stream = AsyncTcpStream::connect(addr).await.unwrap();

                        let size = stream.write("hello".as_bytes()).await.unwrap();
                        assert_eq!(size, 5);
                    })
                    .unwrap();

                let (stream, _) = listener.accept().await.unwrap();
                let mut stream = AsyncTcpStream::new(stream);

                let mut resp: Vec<u8> = Vec::new();

                loop {
                    let mut buf = [0; 1024];

                    let size = stream.read(&mut buf).await.unwrap();
                    if size == 0 {
                        break;
                    }

                    let buf = &buf[..size];
                    resp.extend(buf);
                }

                let resp = String::from_utf8(resp).unwrap();

                assert_eq!(&resp, "hello");
            })
            .unwrap();

        executor.run(|| reactor.poll()).unwrap();
    }

    #[test]
    fn test_sleep() {
        let now = Instant::now();

        let executor = Executor::new(1);
        let reactor = Reactor::new_with_time(1, now);

        executor.spawn(sleep(Duration::from_millis(100))).unwrap();

        executor.run_until_stalled();

        reactor
            .poll_nonblocking(now + Duration::from_millis(200))
            .unwrap();

        executor.run(|| Ok(())).unwrap();
    }

    #[test]
    fn test_sleep_ready() {
        let now = Instant::now();

        let executor = Executor::new(1);
        let _reactor = Reactor::new_with_time(1, now);

        executor.spawn(sleep(Duration::from_millis(0))).unwrap();

        executor.run(|| Ok(())).unwrap();
    }
}
