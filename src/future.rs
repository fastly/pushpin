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
use crate::reactor::{MioReactor, RegistrationHandle};
use mio;
use mio::net::{TcpListener, TcpStream};
use std::cell::Cell;
use std::future::Future;
use std::io;
use std::net::SocketAddr;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::mpsc;
use std::task::{Context, Poll};

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

pub struct AsyncSender<T> {
    inner: channel::Sender<T>,
    handle: RegistrationHandle,
    writable: Cell<bool>,
}

impl<T> AsyncSender<T> {
    pub fn new(s: channel::Sender<T>, reactor: &Rc<MioReactor>) -> Self {
        let handle = MioReactor::register_custom(
            reactor,
            s.get_write_registration(),
            mio::Interest::WRITABLE,
        )
        .unwrap();

        let writable = s.can_send();

        // we know the state up front, so ready can start out unset
        handle.set_ready(false);

        Self {
            inner: s,
            handle,
            writable: Cell::new(writable),
        }
    }

    pub fn is_writable(&self) -> bool {
        let dirty = self.handle.is_ready();

        if dirty {
            self.writable.set(self.inner.can_send());
            self.handle.set_ready(false);
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
    handle: RegistrationHandle,
}

impl<T> AsyncReceiver<T> {
    pub fn new(r: channel::Receiver<T>, reactor: &Rc<MioReactor>) -> Self {
        let handle = MioReactor::register_custom(
            reactor,
            r.get_read_registration(),
            mio::Interest::READABLE,
        )
        .unwrap();

        handle.set_ready(true);

        Self { inner: r, handle }
    }

    pub fn recv<'a>(&'a mut self) -> RecvFuture<'a, T> {
        RecvFuture { r: self }
    }
}

pub struct AsyncTcpListener {
    inner: TcpListener,
    handle: RegistrationHandle,
}

impl AsyncTcpListener {
    pub fn new(mut l: TcpListener, reactor: &Rc<MioReactor>) -> Self {
        let handle = MioReactor::register(reactor, &mut l, mio::Interest::READABLE).unwrap();

        handle.set_ready(true);

        Self { inner: l, handle }
    }

    pub fn accept<'a>(&'a mut self) -> AcceptFuture<'a> {
        AcceptFuture { l: self }
    }
}

pub struct WaitWritableFuture<'a, T> {
    s: &'a mut AsyncSender<T>,
}

impl<T> Future for WaitWritableFuture<'_, T> {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.handle.bind_waker(cx.waker().clone());

        let dirty = f.s.handle.is_ready();

        if dirty {
            f.s.writable.set(f.s.inner.can_send());
            f.s.handle.set_ready(false);
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
        self.s.handle.unbind_waker();
    }
}

pub struct RecvFuture<'a, T> {
    r: &'a mut AsyncReceiver<T>,
}

impl<T> Future for RecvFuture<'_, T> {
    type Output = Result<T, mpsc::RecvError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.r.handle.bind_waker(cx.waker().clone());

        if !f.r.handle.is_ready() {
            return Poll::Pending;
        }

        match f.r.inner.try_recv() {
            Ok(v) => Poll::Ready(Ok(v)),
            Err(mpsc::TryRecvError::Empty) => {
                f.r.handle.set_ready(false);

                Poll::Pending
            }
            Err(mpsc::TryRecvError::Disconnected) => Poll::Ready(Err(mpsc::RecvError)),
        }
    }
}

impl<T> Drop for RecvFuture<'_, T> {
    fn drop(&mut self) {
        self.r.handle.unbind_waker();
    }
}

pub struct AcceptFuture<'a> {
    l: &'a mut AsyncTcpListener,
}

impl Future for AcceptFuture<'_> {
    type Output = Result<(TcpStream, SocketAddr), io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.l.handle.bind_waker(cx.waker().clone());

        if !f.l.handle.is_ready() {
            return Poll::Pending;
        }

        match f.l.inner.accept() {
            Ok((stream, peer_addr)) => Poll::Ready(Ok((stream, peer_addr))),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.l.handle.set_ready(false);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for AcceptFuture<'_> {
    fn drop(&mut self) {
        self.l.handle.unbind_waker();
    }
}
