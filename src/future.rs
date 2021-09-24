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
use crate::event::ReadinessExt;
use crate::reactor::{CustomEvented, FdEvented, IoEvented, Reactor, Registration, TimerEvented};
use crate::shuffle::shuffle;
use crate::tls::TlsStream;
use crate::zmq::{MultipartHeader, ZmqSocket};
use mio;
use mio::net::{TcpListener, TcpStream};
use paste::paste;
use std::cell::{Cell, RefCell};
use std::future::Future;
use std::io;
use std::io::{Read, Write};
use std::net::SocketAddr;
use std::pin::Pin;
use std::sync::mpsc;
use std::task::{Context, Poll};
use std::time::{Duration, Instant};

pub const REGISTRATIONS_PER_CHANNEL: usize = 1;

// 1 for the zmq fd, and potentially 1 for the retry timer
pub const REGISTRATIONS_PER_ZMQSOCKET: usize = 2;

fn range_unordered(dest: &mut [usize]) -> &[usize] {
    for i in 0..dest.len() {
        dest[i] = i;
    }

    shuffle(dest);

    dest
}

fn map_poll<F, W, V>(cx: &mut Context, fut: &mut F, wrap_func: W) -> Poll<V>
where
    F: Future + Unpin,
    W: FnOnce(F::Output) -> V,
{
    match Pin::new(fut).poll(cx) {
        Poll::Ready(v) => Poll::Ready(wrap_func(v)),
        Poll::Pending => Poll::Pending,
    }
}

macro_rules! declare_select {
    ($count: literal, ( $($num:literal),* )) => {
        paste! {
            pub enum [<Select $count>]<$([<O $num>], )*> {
                $(
                    [<R $num>]([<O $num>]),
                )*
            }

            pub struct [<Select $count Future>]<$([<F $num>], )*> {
                $(
                    [<f $num>]: [<F $num>],
                )*
            }

            impl<$([<F $num>], )*> Future for [<Select $count Future>]<$([<F $num>], )*>
            where
                $(
                    [<F $num>]: Future + Unpin,
                )*
            {
                type Output = [<Select $count>]<$([<F $num>]::Output, )*>;

                fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
                    let mut indexes = [0; $count];

                    for i in range_unordered(&mut indexes) {
                        let s = &mut *self;

                        let p = match i + 1 {
                            $(
                                $num => map_poll(cx, &mut s.[<f $num>], |v| [<Select $count>]::[<R $num>](v)),
                            )*
                            _ => unreachable!(),
                        };

                        if p.is_ready() {
                            return p;
                        }
                    }

                    Poll::Pending
                }
            }

            pub fn [<select_ $count>]<$([<F $num>], )*>(
                $(
                    [<f $num>]: [<F $num>],
                )*
            ) -> [<Select $count Future>]<$([<F $num>], )*>
            where
                $(
                    [<F $num>]: Future + Unpin,
                )*
            {
                [<Select $count Future>] {
                    $(
                        [<f $num>],
                    )*
                }
            }
        }
    }
}

declare_select!(2, (1, 2));
declare_select!(3, (1, 2, 3));
declare_select!(4, (1, 2, 3, 4));
declare_select!(6, (1, 2, 3, 4, 5, 6));
declare_select!(9, (1, 2, 3, 4, 5, 6, 7, 8, 9));

pub struct SelectSliceFuture<'a, F> {
    futures: &'a mut [F],
    scratch: &'a mut Vec<usize>,
}

impl<F, O> Future for SelectSliceFuture<'_, F>
where
    F: Future<Output = O> + Unpin,
{
    type Output = (usize, F::Output);

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let s = &mut *self;

        let indexes = &mut s.scratch;
        indexes.resize(s.futures.len(), 0);

        for i in range_unordered(&mut indexes[..s.futures.len()]) {
            if let Poll::Ready(v) = Pin::new(&mut s.futures[*i]).poll(cx) {
                return Poll::Ready((*i, v));
            }
        }

        Poll::Pending
    }
}

pub fn select_slice<'a, F, O>(
    futures: &'a mut [F],
    scratch: &'a mut Vec<usize>,
) -> SelectSliceFuture<'a, F>
where
    F: Future<Output = O> + Unpin,
{
    if futures.len() > scratch.capacity() {
        panic!(
            "select_slice scratch is not large enough: {}, need {}",
            scratch.capacity(),
            futures.len()
        );
    }

    SelectSliceFuture { futures, scratch }
}

pub struct SelectOptionFuture<F> {
    fut: Option<F>,
}

impl<F, O> Future for SelectOptionFuture<F>
where
    F: Future<Output = O> + Unpin,
{
    type Output = O;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let s = &mut *self;

        match Pin::new(&mut s.fut).as_pin_mut() {
            Some(f) => f.poll(cx),
            None => Poll::Pending,
        }
    }
}

pub fn select_option<F, O>(fut: Option<F>) -> SelectOptionFuture<F>
where
    F: Future<Output = O>,
{
    SelectOptionFuture { fut }
}

pub trait AsyncRead: Unpin {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut [u8],
    ) -> Poll<Result<usize, io::Error>>;

    fn cancel(&mut self) {}
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

    fn cancel(&mut self) {}
}

pub trait AsyncShutdown: Unpin {
    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>>;

    fn cancel(&mut self) {}
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
}

pub trait AsyncShutdownExt: AsyncShutdown {
    fn shutdown<'a>(&'a mut self) -> ShutdownFuture<'a, Self> {
        ShutdownFuture { s: self }
    }
}

impl<R: AsyncRead + ?Sized> AsyncReadExt for R {}
impl<W: AsyncWrite + ?Sized> AsyncWriteExt for W {}
impl<S: AsyncShutdown + ?Sized> AsyncShutdownExt for S {}

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
}

pub fn io_split<T: AsyncRead + AsyncWrite>(handle: &RefCell<T>) -> (ReadHalf<T>, WriteHalf<T>) {
    (ReadHalf { handle }, WriteHalf { handle })
}

#[track_caller]
fn get_reactor() -> Reactor {
    Reactor::current().expect("no reactor in thread")
}

pub struct AsyncSender<T> {
    evented: CustomEvented,
    inner: channel::Sender<T>,
}

impl<T> AsyncSender<T> {
    pub fn new(s: channel::Sender<T>) -> Self {
        let evented = CustomEvented::new(
            s.get_write_registration(),
            mio::Interest::WRITABLE,
            &get_reactor(),
        )
        .unwrap();

        // assume we can write, unless can_send() returns false. note that
        // if can_send() returns true, it doesn't mean we can actually write
        evented.registration().set_ready(s.can_send());

        Self { evented, inner: s }
    }

    pub fn is_writable(&self) -> bool {
        self.evented.registration().is_ready()
    }

    pub fn wait_writable<'a>(&'a self) -> WaitWritableFuture<'a, T> {
        WaitWritableFuture { s: self }
    }

    pub fn try_send(&self, t: T) -> Result<(), mpsc::TrySendError<T>> {
        match self.inner.try_send(t) {
            Ok(_) => {
                // if can_send() returns false, then we know we can't write
                if !self.inner.can_send() {
                    self.evented.registration().set_ready(false);
                }

                Ok(())
            }
            Err(mpsc::TrySendError::Full(t)) => {
                self.evented.registration().set_ready(false);

                Err(mpsc::TrySendError::Full(t))
            }
            Err(mpsc::TrySendError::Disconnected(t)) => Err(mpsc::TrySendError::Disconnected(t)),
        }
    }

    pub fn send<'a>(&'a self, t: T) -> SendFuture<'a, T> {
        SendFuture {
            s: self,
            t: Some(t),
        }
    }
}

pub struct AsyncReceiver<T> {
    evented: CustomEvented,
    inner: channel::Receiver<T>,
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

        Self { evented, inner: r }
    }

    pub fn recv<'a>(&'a self) -> RecvFuture<'a, T> {
        RecvFuture { r: self }
    }
}

pub struct AsyncLocalSender<T> {
    evented: CustomEvented,
    inner: channel::LocalSender<T>,
}

impl<T> AsyncLocalSender<T> {
    pub fn new(s: channel::LocalSender<T>) -> Self {
        let evented = CustomEvented::new_local(
            s.get_write_registration(),
            mio::Interest::WRITABLE,
            &get_reactor(),
        )
        .unwrap();

        evented.registration().set_ready(true);

        Self { evented, inner: s }
    }

    pub fn send<'a>(&'a self, t: T) -> LocalSendFuture<'a, T> {
        LocalSendFuture {
            s: self,
            t: Some(t),
        }
    }
}

pub struct AsyncLocalReceiver<T> {
    evented: CustomEvented,
    inner: channel::LocalReceiver<T>,
}

impl<T> AsyncLocalReceiver<T> {
    pub fn new(r: channel::LocalReceiver<T>) -> Self {
        let evented = CustomEvented::new_local(
            r.get_read_registration(),
            mio::Interest::READABLE,
            &get_reactor(),
        )
        .unwrap();

        evented.registration().set_ready(true);

        Self { evented, inner: r }
    }

    pub fn recv<'a>(&'a self) -> LocalRecvFuture<'a, T> {
        LocalRecvFuture { r: self }
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

    pub fn accept<'a>(&'a self) -> AcceptFuture<'a> {
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
        evented
            .registration()
            .set_readiness(Some(mio::Interest::READABLE | mio::Interest::WRITABLE));

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
}

pub struct AsyncTlsStream {
    registration: Registration,
    stream: TlsStream,
}

impl AsyncTlsStream {
    pub fn new(mut s: TlsStream) -> Self {
        let registration = get_reactor()
            .register_io(
                s.get_tcp(),
                mio::Interest::READABLE | mio::Interest::WRITABLE,
            )
            .unwrap();

        // assume I/O operations are ready to be attempted
        registration.set_readiness(Some(mio::Interest::READABLE | mio::Interest::WRITABLE));

        // process TLS reads/writes after any socket event
        registration.set_any_as_all(true);

        Self {
            registration,
            stream: s,
        }
    }
}

impl Drop for AsyncTlsStream {
    fn drop(&mut self) {
        self.registration
            .deregister_io(self.stream.get_tcp())
            .unwrap();
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

pub struct AsyncZmqSocket {
    evented: FdEvented,
    inner: ZmqSocket,
    timeout: Cell<Option<Duration>>,
}

impl AsyncZmqSocket {
    pub fn new(s: ZmqSocket) -> Self {
        let evented = FdEvented::new(
            s.inner().get_fd().unwrap(),
            mio::Interest::READABLE,
            &get_reactor(),
        )
        .unwrap();

        // zmq events are used for readiness, and registration readiness is
        // used to tell us when to call update_events(). we'll call that
        // below, so registration readiness can start out false
        evented.registration().set_ready(false);

        s.update_events();

        Self {
            evented,
            inner: s,
            timeout: Cell::new(None),
        }
    }

    pub fn inner(&self) -> &ZmqSocket {
        &self.inner
    }

    pub fn set_retry_timeout(&self, timeout: Option<Duration>) {
        self.timeout.set(timeout);
    }

    pub fn send<'a>(&'a self, msg: zmq::Message) -> ZmqSendFuture<'a> {
        ZmqSendFuture { s: self, msg }
    }

    pub fn send_to<'a>(
        &'a self,
        header: MultipartHeader,
        content: zmq::Message,
    ) -> ZmqSendToFuture<'a> {
        ZmqSendToFuture {
            s: self,
            header,
            content,
            timer_evented: None,
        }
    }

    pub fn recv<'a>(&'a self) -> ZmqRecvFuture<'a> {
        ZmqRecvFuture { s: self }
    }

    pub fn recv_routed<'a>(&'a self) -> ZmqRecvRoutedFuture<'a> {
        ZmqRecvRoutedFuture { s: self }
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

pub struct WaitWritableFuture<'a, T> {
    s: &'a AsyncSender<T>,
}

impl<T> Future for WaitWritableFuture<'_, T> {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &*self;

        f.s.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::WRITABLE);

        // if can_send() returns false, then we know we can't write. this
        // check prevents spurious wakups of a rendezvous channel from
        // indicating writability when the channel is not actually writable
        if !f.s.inner.can_send() {
            f.s.evented.registration().set_ready(false);
        }

        if !f.s.evented.registration().is_ready() {
            return Poll::Pending;
        }

        Poll::Ready(())
    }
}

impl<T> Drop for WaitWritableFuture<'_, T> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub struct SendFuture<'a, T> {
    s: &'a AsyncSender<T>,
    t: Option<T>,
}

impl<T> Future for SendFuture<'_, T>
where
    T: Unpin,
{
    type Output = Result<(), mpsc::SendError<T>>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::WRITABLE);

        if !f.s.evented.registration().is_ready() {
            return Poll::Pending;
        }

        if !f.s.evented.registration().pull_from_budget() {
            return Poll::Pending;
        }

        let t = f.t.take().unwrap();

        // try_send will update the registration readiness, so we don't need
        // to do that here
        match f.s.try_send(t) {
            Ok(()) => Poll::Ready(Ok(())),
            Err(mpsc::TrySendError::Full(t)) => {
                f.t = Some(t);

                Poll::Pending
            }
            Err(mpsc::TrySendError::Disconnected(t)) => Poll::Ready(Err(mpsc::SendError(t))),
        }
    }
}

impl<T> Drop for SendFuture<'_, T> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub struct RecvFuture<'a, T> {
    r: &'a AsyncReceiver<T>,
}

impl<T> Future for RecvFuture<'_, T> {
    type Output = Result<T, mpsc::RecvError>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &*self;

        f.r.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::READABLE);

        if !f.r.evented.registration().is_ready() {
            return Poll::Pending;
        }

        if !f.r.evented.registration().pull_from_budget() {
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

pub struct LocalSendFuture<'a, T> {
    s: &'a AsyncLocalSender<T>,
    t: Option<T>,
}

impl<T> Future for LocalSendFuture<'_, T>
where
    T: Unpin,
{
    type Output = Result<(), mpsc::SendError<T>>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::WRITABLE);

        if !f.s.evented.registration().is_ready() {
            return Poll::Pending;
        }

        if !f.s.evented.registration().pull_from_budget() {
            return Poll::Pending;
        }

        let t = f.t.take().unwrap();

        match f.s.inner.try_send(t) {
            Ok(()) => Poll::Ready(Ok(())),
            Err(mpsc::TrySendError::Full(t)) => {
                f.s.evented.registration().set_ready(false);
                f.t = Some(t);

                Poll::Pending
            }
            Err(mpsc::TrySendError::Disconnected(t)) => Poll::Ready(Err(mpsc::SendError(t))),
        }
    }
}

impl<T> Drop for LocalSendFuture<'_, T> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub struct LocalRecvFuture<'a, T> {
    r: &'a AsyncLocalReceiver<T>,
}

impl<T> Future for LocalRecvFuture<'_, T> {
    type Output = Result<T, mpsc::RecvError>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &*self;

        f.r.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::READABLE);

        if !f.r.evented.registration().is_ready() {
            return Poll::Pending;
        }

        if !f.r.evented.registration().pull_from_budget() {
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

impl<T> Drop for LocalRecvFuture<'_, T> {
    fn drop(&mut self) {
        self.r.evented.registration().clear_waker();
    }
}

pub struct AcceptFuture<'a> {
    l: &'a AsyncTcpListener,
}

impl Future for AcceptFuture<'_> {
    type Output = Result<(TcpStream, SocketAddr), io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.l.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::READABLE);

        if !f.l.evented.registration().is_ready() {
            return Poll::Pending;
        }

        if !f.l.evented.registration().pull_from_budget() {
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

pub struct ShutdownFuture<'a, S: AsyncShutdown + ?Sized> {
    s: &'a mut S,
}

impl<'a, S: AsyncShutdown + ?Sized> Future for ShutdownFuture<'a, S> {
    type Output = Result<(), io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        let s: Pin<&mut S> = Pin::new(f.s);

        s.poll_shutdown(cx)
    }
}

impl<'a, S: AsyncShutdown + ?Sized> Drop for ShutdownFuture<'a, S> {
    fn drop(&mut self) {
        self.s.cancel();
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

pub struct TcpConnectFuture<'a> {
    s: &'a mut AsyncTcpStream,
}

impl Future for TcpConnectFuture<'_> {
    type Output = Result<(), io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::WRITABLE);

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

impl AsyncRead for AsyncTcpStream {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &mut [u8],
    ) -> Poll<Result<usize, io::Error>> {
        let f = &mut *self;

        f.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::READABLE);

        if !f
            .evented
            .registration()
            .readiness()
            .contains_any(mio::Interest::READABLE)
        {
            return Poll::Pending;
        }

        if !f.evented.registration().pull_from_budget() {
            return Poll::Pending;
        }

        match f.evented.io().read(buf) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.evented
                    .registration()
                    .clear_readiness(mio::Interest::READABLE);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn cancel(&mut self) {
        self.evented
            .registration()
            .clear_waker_interest(mio::Interest::READABLE);
    }
}

impl AsyncWrite for AsyncTcpStream {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        let f = &mut *self;

        f.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::WRITABLE);

        if !f
            .evented
            .registration()
            .readiness()
            .contains_any(mio::Interest::WRITABLE)
        {
            return Poll::Pending;
        }

        if !f.evented.registration().pull_from_budget() {
            return Poll::Pending;
        }

        match f.evented.io().write(buf) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.evented
                    .registration()
                    .clear_readiness(mio::Interest::WRITABLE);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn cancel(&mut self) {
        self.evented
            .registration()
            .clear_waker_interest(mio::Interest::WRITABLE);
    }
}

impl AsyncShutdown for AsyncTcpStream {
    fn poll_shutdown(self: Pin<&mut Self>, _cx: &mut Context) -> Poll<Result<(), io::Error>> {
        Poll::Ready(Ok(()))
    }
}

impl AsyncRead for AsyncTlsStream {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &mut [u8],
    ) -> Poll<Result<usize, io::Error>> {
        let f = &mut *self;

        f.registration
            .set_waker(cx.waker().clone(), mio::Interest::READABLE);

        if !f
            .registration
            .readiness()
            .contains_any(mio::Interest::READABLE)
        {
            return Poll::Pending;
        }

        if !f.registration.pull_from_budget() {
            return Poll::Pending;
        }

        match f.stream.read(buf) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.registration.clear_readiness(mio::Interest::READABLE);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn cancel(&mut self) {
        self.registration
            .clear_waker_interest(mio::Interest::READABLE);
    }
}

impl AsyncWrite for AsyncTlsStream {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        let f = &mut *self;

        f.registration
            .set_waker(cx.waker().clone(), mio::Interest::WRITABLE);

        if !f
            .registration
            .readiness()
            .contains_any(mio::Interest::WRITABLE)
        {
            return Poll::Pending;
        }

        if !f.registration.pull_from_budget() {
            return Poll::Pending;
        }

        match f.stream.write(buf) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.registration.clear_readiness(mio::Interest::WRITABLE);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn cancel(&mut self) {
        self.registration
            .clear_waker_interest(mio::Interest::WRITABLE);
    }
}

impl AsyncShutdown for AsyncTlsStream {
    fn poll_shutdown(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Result<(), io::Error>> {
        let f = &mut *self;

        f.registration
            .set_waker(cx.waker().clone(), mio::Interest::WRITABLE);

        if !f
            .registration
            .readiness()
            .contains_any(mio::Interest::WRITABLE)
        {
            return Poll::Pending;
        }

        if !f.registration.pull_from_budget() {
            return Poll::Pending;
        }

        match f.stream.shutdown() {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.registration.clear_readiness(mio::Interest::WRITABLE);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn cancel(&mut self) {
        self.registration
            .clear_waker_interest(mio::Interest::WRITABLE);
    }
}

pub struct SleepFuture<'a> {
    s: &'a mut AsyncSleep,
}

impl Future for SleepFuture<'_> {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::READABLE);

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

pub struct ZmqSendFuture<'a> {
    s: &'a AsyncZmqSocket,
    msg: zmq::Message,
}

impl Future for ZmqSendFuture<'_> {
    type Output = Result<(), zmq::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::READABLE);

        if f.s.evented.registration().is_ready() {
            f.s.inner.update_events();
            f.s.evented.registration().set_ready(false);
        }

        if !f.s.inner.events().contains(zmq::POLLOUT) {
            return Poll::Pending;
        }

        // NOTE: when rust-zmq allows resending messages we can
        //   avoid this copy

        let msg = zmq::Message::from(&f.msg[..]);

        match f.s.inner.send(msg, zmq::DONTWAIT) {
            Ok(()) => Poll::Ready(Ok(())),
            Err(zmq::Error::EAGAIN) => Poll::Pending,
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for ZmqSendFuture<'_> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub struct ZmqSendToFuture<'a> {
    s: &'a AsyncZmqSocket,
    header: MultipartHeader,
    content: zmq::Message,
    timer_evented: Option<TimerEvented>,
}

impl Future for ZmqSendToFuture<'_> {
    type Output = Result<(), zmq::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        let reactor = f.s.evented.registration().reactor();

        if let Some(timer_evented) = &f.timer_evented {
            timer_evented
                .registration()
                .set_waker(cx.waker().clone(), mio::Interest::READABLE);

            if reactor.now() < timer_evented.expires() {
                timer_evented.registration().set_ready(false);

                return Poll::Pending;
            }

            f.timer_evented = None;
        }

        assert!(f.timer_evented.is_none());

        f.s.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::READABLE);

        if f.s.evented.registration().is_ready() {
            f.s.inner.update_events();
            f.s.evented.registration().set_ready(false);
        }

        if !f.s.inner.events().contains(zmq::POLLOUT) {
            return Poll::Pending;
        }

        // NOTE: when rust-zmq allows resending messages we can
        //   avoid this copy

        let content = zmq::Message::from(&f.content[..]);

        match f.s.inner.send_to(&f.header, content, zmq::DONTWAIT) {
            Ok(()) => Poll::Ready(Ok(())),
            Err(zmq::Error::EAGAIN) => {
                if let Some(timeout) = f.s.timeout.get() {
                    let expires = reactor.now() + timeout;
                    let timer_evented = TimerEvented::new(expires, &reactor).unwrap();

                    f.s.evented.registration().clear_waker();

                    timer_evented.registration().set_ready(true);
                    timer_evented
                        .registration()
                        .set_waker(cx.waker().clone(), mio::Interest::READABLE);

                    f.timer_evented = Some(timer_evented);
                }

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for ZmqSendToFuture<'_> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub struct ZmqRecvFuture<'a> {
    s: &'a AsyncZmqSocket,
}

impl Future for ZmqRecvFuture<'_> {
    type Output = Result<zmq::Message, zmq::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::READABLE);

        if f.s.evented.registration().is_ready() {
            f.s.inner.update_events();
            f.s.evented.registration().set_ready(false);
        }

        if !f.s.inner.events().contains(zmq::POLLIN) {
            return Poll::Pending;
        }

        match f.s.inner.recv(zmq::DONTWAIT) {
            Ok(msg) => Poll::Ready(Ok(msg)),
            Err(zmq::Error::EAGAIN) => Poll::Pending,
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for ZmqRecvFuture<'_> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub struct ZmqRecvRoutedFuture<'a> {
    s: &'a AsyncZmqSocket,
}

impl Future for ZmqRecvRoutedFuture<'_> {
    type Output = Result<zmq::Message, zmq::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::READABLE);

        if f.s.evented.registration().is_ready() {
            f.s.inner.update_events();
            f.s.evented.registration().set_ready(false);
        }

        if !f.s.inner.events().contains(zmq::POLLIN) {
            return Poll::Pending;
        }

        match f.s.inner.recv_routed(zmq::DONTWAIT) {
            Ok(msg) => Poll::Ready(Ok(msg)),
            Err(zmq::Error::EAGAIN) => Poll::Pending,
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for ZmqRecvRoutedFuture<'_> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
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

        f.w.registration.set_waker(cx.waker().clone(), f.interest);

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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::executor::Executor;
    use crate::zmq::SpecInfo;
    use std::cmp;
    use std::mem;
    use std::rc::Rc;
    use std::str;
    use std::task::Context;
    use std::thread;

    struct PollFuture<F> {
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

    fn poll_fut_async<F>(fut: F) -> PollFuture<F>
    where
        F: Future + Unpin,
    {
        PollFuture { fut }
    }

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
    }

    #[test]
    fn test_channel_send_bound0() {
        let executor = Executor::new(2);
        let reactor = Reactor::new(2);

        let (s, r) = channel::channel::<u32>(0);

        let s = AsyncSender::new(s);
        let r = AsyncReceiver::new(r);

        executor
            .spawn(async move {
                s.send(1).await.unwrap();

                assert_eq!(s.is_writable(), false);
            })
            .unwrap();

        executor.run_until_stalled();

        assert_eq!(executor.have_tasks(), true);

        executor
            .spawn(async move {
                assert_eq!(r.recv().await, Ok(1));
                assert_eq!(r.recv().await, Err(mpsc::RecvError));
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn test_channel_send_bound1() {
        let executor = Executor::new(1);
        let reactor = Reactor::new(2);

        let (s, r) = channel::channel::<u32>(1);

        let s = AsyncSender::new(s);
        let r = AsyncReceiver::new(r);

        executor
            .spawn(async move {
                s.send(1).await.unwrap();

                assert_eq!(s.is_writable(), true);
            })
            .unwrap();

        executor.run_until_stalled();

        assert_eq!(executor.have_tasks(), false);

        executor
            .spawn(async move {
                assert_eq!(r.recv().await, Ok(1));
                assert_eq!(r.recv().await, Err(mpsc::RecvError));
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn test_channel_recv() {
        let executor = Executor::new(2);
        let reactor = Reactor::new(2);

        let (s, r) = channel::channel::<u32>(0);

        let s = AsyncSender::new(s);
        let r = AsyncReceiver::new(r);

        executor
            .spawn(async move {
                assert_eq!(r.recv().await, Ok(1));
                assert_eq!(r.recv().await, Err(mpsc::RecvError));
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

    #[test]
    fn test_channel_writable() {
        let executor = Executor::new(1);
        let reactor = Reactor::new(1);

        let (s, r) = channel::channel::<u32>(0);

        let s = AsyncSender::new(s);

        executor
            .spawn(async move {
                assert_eq!(s.is_writable(), false);

                s.wait_writable().await;
            })
            .unwrap();

        executor.run_until_stalled();

        assert_eq!(executor.have_tasks(), true);

        // attempting to receive on a rendezvous channel will make the
        // sender writable
        assert_eq!(r.try_recv(), Err(mpsc::TryRecvError::Empty));

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn test_local_channel() {
        let executor = Executor::new(2);
        let reactor = Reactor::new(2);

        let (s, r) = channel::local_channel::<u32>(1, 1, &reactor.local_registration_memory());

        let s = AsyncLocalSender::new(s);
        let r = AsyncLocalReceiver::new(r);

        executor
            .spawn(async move {
                assert_eq!(r.recv().await, Ok(1));
                assert_eq!(r.recv().await, Err(mpsc::RecvError));
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
    fn test_tcpstream() {
        let executor = Executor::new(2); // 2 tasks
        let reactor = Reactor::new(3); // 3 registrations

        let spawner = executor.spawner();

        executor
            .spawn(async move {
                let addr = "127.0.0.1:0".parse().unwrap();
                let listener = AsyncTcpListener::bind(addr).expect("failed to bind");
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
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn test_zmq() {
        let executor = Executor::new(2);
        let reactor = Reactor::new(2);

        let spec = "inproc://futures::tests::test_zmq";

        let context = zmq::Context::new();

        let s = AsyncZmqSocket::new(ZmqSocket::new(&context, zmq::PUSH));
        let r = AsyncZmqSocket::new(ZmqSocket::new(&context, zmq::PULL));

        s.inner().inner().bind(spec).unwrap();
        s.inner().inner().set_sndhwm(1).unwrap();

        executor
            .spawn(async move {
                s.send(zmq::Message::from(&b"1"[..])).await.unwrap();
                s.send(zmq::Message::from(&b"2"[..])).await.unwrap();
            })
            .unwrap();

        executor.run_until_stalled();

        assert_eq!(executor.have_tasks(), true);

        r.inner().inner().connect(spec).unwrap();

        executor
            .spawn(async move {
                assert_eq!(r.recv().await, Ok(zmq::Message::from(&b"1"[..])));
                assert_eq!(r.recv().await, Ok(zmq::Message::from(&b"2"[..])));
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn test_zmq_routable() {
        let executor = Executor::new(2);
        let reactor = Reactor::new(2);

        let spec = "inproc://futures::tests::test_zmq_routable";

        let context = zmq::Context::new();

        let s = AsyncZmqSocket::new(ZmqSocket::new(&context, zmq::DEALER));
        let r = AsyncZmqSocket::new(ZmqSocket::new(&context, zmq::ROUTER));

        s.inner().inner().bind(spec).unwrap();
        s.inner().inner().set_sndhwm(1).unwrap();

        executor
            .spawn(async move {
                s.send_to(MultipartHeader::new(), zmq::Message::from(&b"1"[..]))
                    .await
                    .unwrap();
                s.send_to(MultipartHeader::new(), zmq::Message::from(&b"2"[..]))
                    .await
                    .unwrap();
            })
            .unwrap();

        executor.run_until_stalled();

        assert_eq!(executor.have_tasks(), true);

        r.inner().inner().connect(spec).unwrap();

        executor
            .spawn(async move {
                assert_eq!(r.recv_routed().await, Ok(zmq::Message::from(&b"1"[..])));
                assert_eq!(r.recv_routed().await, Ok(zmq::Message::from(&b"2"[..])));
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn test_zmq_retry_timeout() {
        let executor = Executor::new(2);
        let reactor = Reactor::new(2);

        let spec = "inproc://futures::tests::test_zmq_routable";

        let context = zmq::Context::new();

        let s = ZmqSocket::new(&context, zmq::ROUTER);
        let r1 = ZmqSocket::new(&context, zmq::ROUTER);
        let r2 = ZmqSocket::new(&context, zmq::ROUTER);

        s.inner().set_sndhwm(1).unwrap();
        s.inner().set_router_mandatory(true).unwrap();
        s.apply_specs(&[SpecInfo {
            spec: spec.to_string(),
            bind: true,
            ipc_file_mode: 0,
        }])
        .unwrap();

        r1.inner().set_rcvhwm(1).unwrap();
        r1.inner().set_identity(b"test1").unwrap();
        r1.apply_specs(&[SpecInfo {
            spec: spec.to_string(),
            bind: false,
            ipc_file_mode: 0,
        }])
        .unwrap();

        r2.inner().set_rcvhwm(1).unwrap();
        r2.inner().set_identity(b"test2").unwrap();
        r2.apply_specs(&[SpecInfo {
            spec: spec.to_string(),
            bind: false,
            ipc_file_mode: 0,
        }])
        .unwrap();

        // ensure both peers are connected

        loop {
            let mut h = MultipartHeader::new();
            h.push(zmq::Message::from(&b"test1"[..]));

            match s.send_to(&h, zmq::Message::from(&b"1"[..]), 0) {
                Ok(()) => break,
                Err(zmq::Error::EHOSTUNREACH) => thread::sleep(Duration::from_millis(10)),
                Err(e) => panic!("{}", e),
            }
        }

        loop {
            let mut h = MultipartHeader::new();
            h.push(zmq::Message::from(&b"test2"[..]));

            match s.send_to(&h, zmq::Message::from(&b"1"[..]), 0) {
                Ok(()) => break,
                Err(zmq::Error::EHOSTUNREACH) => thread::sleep(Duration::from_millis(10)),
                Err(e) => panic!("{}", e),
            }
        }

        // we can clear out r1
        assert_eq!(r1.recv_routed(0), Ok(zmq::Message::from(&b"1"[..])));

        // wrap in Rc so the inproc sender is not dropped until after the
        // messages have been received
        let s = Rc::new(AsyncZmqSocket::new(s));

        s.set_retry_timeout(Some(Duration::from_millis(0)));

        {
            let s = s.clone();

            executor
                .spawn(async move {
                    // second write will succeed immediately

                    let mut h = MultipartHeader::new();
                    h.push(zmq::Message::from(&b"test2"[..]));
                    s.send_to(h, zmq::Message::from(&b"2"[..])).await.unwrap();

                    // third write will block

                    let mut h = MultipartHeader::new();
                    h.push(zmq::Message::from(&b"test2"[..]));
                    let mut fut = s.send_to(h, zmq::Message::from(&b"3"[..]));

                    assert_eq!(poll_fut_async(&mut fut).await, Poll::Pending);
                    assert_eq!(fut.timer_evented.is_some(), true);

                    fut.await.unwrap();
                })
                .unwrap();
        }

        executor.run_until_stalled();

        assert_eq!(executor.have_tasks(), true);

        // this will allow the third write to go through
        assert_eq!(r2.recv_routed(0), Ok(zmq::Message::from(&b"1"[..])));

        executor.run(|timeout| reactor.poll(timeout)).unwrap();

        assert_eq!(r2.recv_routed(0), Ok(zmq::Message::from(&b"2"[..])));
        assert_eq!(r2.recv_routed(0), Ok(zmq::Message::from(&b"3"[..])));
    }

    #[test]
    fn test_budget_unlimited() {
        let executor = Executor::new(1);
        let reactor = Reactor::new(1);

        let (s, r) = channel::channel::<u32>(3);

        s.send(1).unwrap();
        s.send(2).unwrap();
        s.send(3).unwrap();
        mem::drop(s);

        let r = AsyncReceiver::new(r);

        executor
            .spawn(async move {
                assert_eq!(r.recv().await, Ok(1));
                assert_eq!(r.recv().await, Ok(2));
                assert_eq!(r.recv().await, Ok(3));
                assert_eq!(r.recv().await, Err(mpsc::RecvError));
            })
            .unwrap();

        let mut park_count = 0;

        executor
            .run(|timeout| {
                park_count += 1;

                reactor.poll(timeout)
            })
            .unwrap();

        assert_eq!(park_count, 0);
    }

    #[test]
    fn test_budget_1() {
        let executor = Executor::new(1);
        let reactor = Reactor::new(1);

        {
            let reactor = reactor.clone();

            executor.set_pre_poll(move || {
                reactor.set_budget(Some(1));
            });
        }

        let (s, r) = channel::channel::<u32>(3);

        s.send(1).unwrap();
        s.send(2).unwrap();
        s.send(3).unwrap();
        mem::drop(s);

        let r = AsyncReceiver::new(r);

        executor
            .spawn(async move {
                assert_eq!(r.recv().await, Ok(1));
                assert_eq!(r.recv().await, Ok(2));
                assert_eq!(r.recv().await, Ok(3));
                assert_eq!(r.recv().await, Err(mpsc::RecvError));
            })
            .unwrap();

        let mut park_count = 0;

        executor
            .run(|timeout| {
                park_count += 1;

                reactor.poll(timeout)
            })
            .unwrap();

        assert_eq!(park_count, 3);
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

        executor.run(|_| Ok(())).unwrap();
    }

    #[test]
    fn test_sleep_ready() {
        let now = Instant::now();

        let executor = Executor::new(1);
        let _reactor = Reactor::new_with_time(1, now);

        executor.spawn(sleep(Duration::from_millis(0))).unwrap();

        executor.run(|_| Ok(())).unwrap();
    }

    #[test]
    fn test_event_wait() {
        let executor = Executor::new(2);
        let reactor = Reactor::new(2);

        let (s, r) = channel::local_channel::<u32>(1, 1, &reactor.local_registration_memory());

        let s = AsyncLocalSender::new(s);

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
}
