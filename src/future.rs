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
use crate::zmq::{MultipartHeader, ZmqSocket};
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

fn map_poll<T, F, M, W, V>(p: Pin<&mut T>, cx: &mut Context, map_func: M, wrap_func: W) -> Poll<V>
where
    F: Future,
    M: FnOnce(&mut T) -> &mut F,
    W: FnOnce(F::Output) -> V,
{
    let f = unsafe { p.map_unchecked_mut(map_func) };

    match f.poll(cx) {
        Poll::Ready(v) => Poll::Ready(wrap_func(v)),
        Poll::Pending => Poll::Pending,
    }
}

pub enum Select2<O1, O2> {
    R1(O1),
    R2(O2),
}

pub struct Select2Future<F1, F2> {
    f1: F1,
    f2: F2,
}

impl<F1, F2> Future for Select2Future<F1, F2>
where
    F1: Future,
    F2: Future,
{
    type Output = Select2<F1::Output, F2::Output>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let mut indexes = [0; 2];

        for i in range_unordered(&mut indexes) {
            let s = self.as_mut();

            let p = match i {
                0 => map_poll(s, cx, |s| &mut s.f1, |v| Select2::R1(v)),
                1 => map_poll(s, cx, |s| &mut s.f2, |v| Select2::R2(v)),
                _ => unreachable!(),
            };

            if p.is_ready() {
                return p;
            }
        }

        Poll::Pending
    }
}

pub enum Select3<O1, O2, O3> {
    R1(O1),
    R2(O2),
    R3(O3),
}

pub struct Select3Future<F1, F2, F3> {
    f1: F1,
    f2: F2,
    f3: F3,
}

impl<F1, F2, F3> Future for Select3Future<F1, F2, F3>
where
    F1: Future,
    F2: Future,
    F3: Future,
{
    type Output = Select3<F1::Output, F2::Output, F3::Output>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let mut indexes = [0; 3];

        for i in range_unordered(&mut indexes) {
            let s = self.as_mut();

            let p = match i {
                0 => map_poll(s, cx, |s| &mut s.f1, |v| Select3::R1(v)),
                1 => map_poll(s, cx, |s| &mut s.f2, |v| Select3::R2(v)),
                2 => map_poll(s, cx, |s| &mut s.f3, |v| Select3::R3(v)),
                _ => unreachable!(),
            };

            if p.is_ready() {
                return p;
            }
        }

        Poll::Pending
    }
}

pub enum Select4<O1, O2, O3, O4> {
    R1(O1),
    R2(O2),
    R3(O3),
    R4(O4),
}

pub struct Select4Future<F1, F2, F3, F4> {
    f1: F1,
    f2: F2,
    f3: F3,
    f4: F4,
}

impl<F1, F2, F3, F4> Future for Select4Future<F1, F2, F3, F4>
where
    F1: Future,
    F2: Future,
    F3: Future,
    F4: Future,
{
    type Output = Select4<F1::Output, F2::Output, F3::Output, F4::Output>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let mut indexes = [0; 4];

        for i in range_unordered(&mut indexes) {
            let s = self.as_mut();

            let p = match i {
                0 => map_poll(s, cx, |s| &mut s.f1, |v| Select4::R1(v)),
                1 => map_poll(s, cx, |s| &mut s.f2, |v| Select4::R2(v)),
                2 => map_poll(s, cx, |s| &mut s.f3, |v| Select4::R3(v)),
                3 => map_poll(s, cx, |s| &mut s.f4, |v| Select4::R4(v)),
                _ => unreachable!(),
            };

            if p.is_ready() {
                return p;
            }
        }

        Poll::Pending
    }
}

pub enum Select5<O1, O2, O3, O4, O5> {
    R1(O1),
    R2(O2),
    R3(O3),
    R4(O4),
    R5(O5),
}

pub struct Select5Future<F1, F2, F3, F4, F5> {
    f1: F1,
    f2: F2,
    f3: F3,
    f4: F4,
    f5: F5,
}

impl<F1, F2, F3, F4, F5> Future for Select5Future<F1, F2, F3, F4, F5>
where
    F1: Future,
    F2: Future,
    F3: Future,
    F4: Future,
    F5: Future,
{
    type Output = Select5<F1::Output, F2::Output, F3::Output, F4::Output, F5::Output>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let mut indexes = [0; 5];

        for i in range_unordered(&mut indexes) {
            let s = self.as_mut();

            let p = match i {
                0 => map_poll(s, cx, |s| &mut s.f1, |v| Select5::R1(v)),
                1 => map_poll(s, cx, |s| &mut s.f2, |v| Select5::R2(v)),
                2 => map_poll(s, cx, |s| &mut s.f3, |v| Select5::R3(v)),
                3 => map_poll(s, cx, |s| &mut s.f4, |v| Select5::R4(v)),
                4 => map_poll(s, cx, |s| &mut s.f5, |v| Select5::R5(v)),
                _ => unreachable!(),
            };

            if p.is_ready() {
                return p;
            }
        }

        Poll::Pending
    }
}

pub enum Select6<O1, O2, O3, O4, O5, O6> {
    R1(O1),
    R2(O2),
    R3(O3),
    R4(O4),
    R5(O5),
    R6(O6),
}

pub struct Select6Future<F1, F2, F3, F4, F5, F6> {
    f1: F1,
    f2: F2,
    f3: F3,
    f4: F4,
    f5: F5,
    f6: F6,
}

impl<F1, F2, F3, F4, F5, F6> Future for Select6Future<F1, F2, F3, F4, F5, F6>
where
    F1: Future,
    F2: Future,
    F3: Future,
    F4: Future,
    F5: Future,
    F6: Future,
{
    type Output = Select6<F1::Output, F2::Output, F3::Output, F4::Output, F5::Output, F6::Output>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let mut indexes = [0; 6];

        for i in range_unordered(&mut indexes) {
            let s = self.as_mut();

            let p = match i {
                0 => map_poll(s, cx, |s| &mut s.f1, |v| Select6::R1(v)),
                1 => map_poll(s, cx, |s| &mut s.f2, |v| Select6::R2(v)),
                2 => map_poll(s, cx, |s| &mut s.f3, |v| Select6::R3(v)),
                3 => map_poll(s, cx, |s| &mut s.f4, |v| Select6::R4(v)),
                4 => map_poll(s, cx, |s| &mut s.f5, |v| Select6::R5(v)),
                5 => map_poll(s, cx, |s| &mut s.f6, |v| Select6::R6(v)),
                _ => unreachable!(),
            };

            if p.is_ready() {
                return p;
            }
        }

        Poll::Pending
    }
}

pub enum Select9<O1, O2, O3, O4, O5, O6, O7, O8, O9> {
    R1(O1),
    R2(O2),
    R3(O3),
    R4(O4),
    R5(O5),
    R6(O6),
    R7(O7),
    R8(O8),
    R9(O9),
}

pub struct Select9Future<F1, F2, F3, F4, F5, F6, F7, F8, F9> {
    f1: F1,
    f2: F2,
    f3: F3,
    f4: F4,
    f5: F5,
    f6: F6,
    f7: F7,
    f8: F8,
    f9: F9,
}

impl<F1, F2, F3, F4, F5, F6, F7, F8, F9> Future
    for Select9Future<F1, F2, F3, F4, F5, F6, F7, F8, F9>
where
    F1: Future,
    F2: Future,
    F3: Future,
    F4: Future,
    F5: Future,
    F6: Future,
    F7: Future,
    F8: Future,
    F9: Future,
{
    type Output = Select9<
        F1::Output,
        F2::Output,
        F3::Output,
        F4::Output,
        F5::Output,
        F6::Output,
        F7::Output,
        F8::Output,
        F9::Output,
    >;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let mut indexes = [0; 9];

        for i in range_unordered(&mut indexes) {
            let s = self.as_mut();

            let p = match i {
                0 => map_poll(s, cx, |s| &mut s.f1, |v| Select9::R1(v)),
                1 => map_poll(s, cx, |s| &mut s.f2, |v| Select9::R2(v)),
                2 => map_poll(s, cx, |s| &mut s.f3, |v| Select9::R3(v)),
                3 => map_poll(s, cx, |s| &mut s.f4, |v| Select9::R4(v)),
                4 => map_poll(s, cx, |s| &mut s.f5, |v| Select9::R5(v)),
                5 => map_poll(s, cx, |s| &mut s.f6, |v| Select9::R6(v)),
                6 => map_poll(s, cx, |s| &mut s.f7, |v| Select9::R7(v)),
                7 => map_poll(s, cx, |s| &mut s.f8, |v| Select9::R8(v)),
                8 => map_poll(s, cx, |s| &mut s.f9, |v| Select9::R9(v)),
                _ => unreachable!(),
            };

            if p.is_ready() {
                return p;
            }
        }

        Poll::Pending
    }
}

pub fn select_2<F1, F2>(f1: F1, f2: F2) -> Select2Future<F1, F2>
where
    F1: Future,
    F2: Future,
{
    Select2Future { f1, f2 }
}

pub fn select_3<F1, F2, F3>(f1: F1, f2: F2, f3: F3) -> Select3Future<F1, F2, F3>
where
    F1: Future,
    F2: Future,
    F3: Future,
{
    Select3Future { f1, f2, f3 }
}

pub fn select_4<F1, F2, F3, F4>(f1: F1, f2: F2, f3: F3, f4: F4) -> Select4Future<F1, F2, F3, F4>
where
    F1: Future,
    F2: Future,
    F3: Future,
    F4: Future,
{
    Select4Future { f1, f2, f3, f4 }
}

pub fn select_5<F1, F2, F3, F4, F5>(
    f1: F1,
    f2: F2,
    f3: F3,
    f4: F4,
    f5: F5,
) -> Select5Future<F1, F2, F3, F4, F5>
where
    F1: Future,
    F2: Future,
    F3: Future,
    F4: Future,
    F5: Future,
{
    Select5Future { f1, f2, f3, f4, f5 }
}

pub fn select_6<F1, F2, F3, F4, F5, F6>(
    f1: F1,
    f2: F2,
    f3: F3,
    f4: F4,
    f5: F5,
    f6: F6,
) -> Select6Future<F1, F2, F3, F4, F5, F6>
where
    F1: Future,
    F2: Future,
    F3: Future,
    F4: Future,
    F5: Future,
    F6: Future,
{
    Select6Future {
        f1,
        f2,
        f3,
        f4,
        f5,
        f6,
    }
}

pub fn select_9<F1, F2, F3, F4, F5, F6, F7, F8, F9>(
    f1: F1,
    f2: F2,
    f3: F3,
    f4: F4,
    f5: F5,
    f6: F6,
    f7: F7,
    f8: F8,
    f9: F9,
) -> Select9Future<F1, F2, F3, F4, F5, F6, F7, F8, F9>
where
    F1: Future,
    F2: Future,
    F3: Future,
    F4: Future,
    F5: Future,
    F6: Future,
    F7: Future,
    F8: Future,
    F9: Future,
{
    Select9Future {
        f1,
        f2,
        f3,
        f4,
        f5,
        f6,
        f7,
        f8,
        f9,
    }
}

pub struct SelectSliceFuture<'a, F> {
    futures: &'a mut [F],
    scratch: &'a mut Vec<usize>,
}

impl<F, O> Future for SelectSliceFuture<'_, F>
where
    F: Future<Output = O>,
{
    type Output = (usize, F::Output);

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let s = &mut *self;

        let indexes = &mut s.scratch;
        indexes.resize(s.futures.len(), 0);

        for i in range_unordered(&mut indexes[..s.futures.len()]) {
            let f = unsafe { Pin::new_unchecked(&mut s.futures[*i]) };

            if let Poll::Ready(v) = f.poll(cx) {
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
    F: Future<Output = O>,
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
    F: Future<Output = O>,
{
    type Output = O;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = unsafe { self.as_mut().map_unchecked_mut(|s| &mut s.fut) };

        match f.as_pin_mut() {
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

pub struct SelectOptionRefFuture<'a, F> {
    fut: Option<&'a mut F>,
}

impl<'a, F, O> Future for SelectOptionRefFuture<'a, F>
where
    F: Future<Output = O>,
{
    type Output = O;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        match self.fut.as_mut() {
            Some(f) => {
                let f: Pin<&mut F> = unsafe { Pin::new_unchecked(*f) };

                f.poll(cx)
            }
            None => Poll::Pending,
        }
    }
}

pub fn select_option_ref<F, O>(fut: Option<&mut F>) -> SelectOptionRefFuture<F>
where
    F: Future<Output = O>,
{
    SelectOptionRefFuture { fut }
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

pub struct TcpReadFuture<'a> {
    s: &'a mut AsyncTcpStream,
    buf: &'a mut [u8],
}

impl Future for TcpReadFuture<'_> {
    type Output = Result<usize, io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::READABLE);

        if !f
            .s
            .evented
            .registration()
            .readiness()
            .contains_any(mio::Interest::READABLE)
        {
            return Poll::Pending;
        }

        if !f.s.evented.registration().pull_from_budget() {
            return Poll::Pending;
        }

        match f.s.evented.io().read(f.buf) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.s.evented
                    .registration()
                    .clear_readiness(mio::Interest::READABLE);

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

        f.s.evented
            .registration()
            .set_waker(cx.waker().clone(), mio::Interest::WRITABLE);

        if !f
            .s
            .evented
            .registration()
            .readiness()
            .contains_any(mio::Interest::WRITABLE)
        {
            return Poll::Pending;
        }

        // try to write all the data before producing a result, the same as
        // what a blocking write would do
        loop {
            if !f.s.evented.registration().pull_from_budget() {
                return Poll::Pending;
            }

            match f.s.evented.io().write(&f.buf[f.pos..]) {
                Ok(size) => {
                    f.pos += size;

                    if f.pos >= f.buf.len() {
                        break;
                    }
                }
                Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                    f.s.evented
                        .registration()
                        .clear_readiness(mio::Interest::WRITABLE);

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
    use std::mem;
    use std::rc::Rc;
    use std::str;
    use std::task::Context;
    use std::thread;

    struct PollFuture<'a, F> {
        fut: &'a mut F,
    }

    impl<F, O> Future for PollFuture<'_, F>
    where
        F: Future<Output = O>,
    {
        type Output = Poll<F::Output>;

        fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
            let f = unsafe { self.as_mut().map_unchecked_mut(|s| s.fut) };

            Poll::Ready(f.poll(cx))
        }
    }

    fn poll_fut_async<'a, F>(fut: &'a mut F) -> PollFuture<'a, F> {
        PollFuture { fut }
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
