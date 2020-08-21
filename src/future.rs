/*
 * Copyright (C) 2020 Fanout, Inc.
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
use crate::list;
use mio;
use mio::net::{TcpListener, TcpStream};
use slab::Slab;
use std::cell::{Cell, RefCell};
use std::future::Future;
use std::io;
use std::net::SocketAddr;
use std::pin::Pin;
use std::ptr;
use std::sync::mpsc;
use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};

const EVENT_REGISTRATIONS_MAX: usize = 128;

struct EventRegistration {
    ready: bool,
    enabled: bool,
}

struct ExecutorData {
    registrations_nodes: Slab<list::Node<EventRegistration>>,
    registrations: list::List,
}

pub struct Executor {
    data: RefCell<ExecutorData>,
    poll: mio::Poll,
}

impl Executor {
    pub fn new() -> Self {
        let data = ExecutorData {
            registrations_nodes: Slab::with_capacity(EVENT_REGISTRATIONS_MAX),
            registrations: list::List::default(),
        };

        Self {
            data: RefCell::new(data),
            poll: mio::Poll::new().unwrap(),
        }
    }

    fn register(&self) -> usize {
        let data = &mut *self.data.borrow_mut();

        let key = data
            .registrations_nodes
            .insert(list::Node::new(EventRegistration {
                ready: false,
                enabled: false,
            }));

        data.registrations
            .push_back(&mut data.registrations_nodes, key);

        key
    }

    fn unregister(&self, key: usize) {
        let data = &mut *self.data.borrow_mut();

        data.registrations
            .remove(&mut data.registrations_nodes, key);
        data.registrations_nodes.remove(key);
    }

    fn is_ready(&self, key: usize) -> bool {
        let event_reg = &self.data.borrow().registrations_nodes[key].value;

        event_reg.ready
    }

    fn set_ready(&self, key: usize, ready: bool) {
        let event_reg = &mut self.data.borrow_mut().registrations_nodes[key].value;

        event_reg.ready = ready;
    }

    fn set_enabled(&self, key: usize, enabled: bool) {
        let event_reg = &mut self.data.borrow_mut().registrations_nodes[key].value;

        event_reg.enabled = enabled;
    }

    pub fn block_on<F>(&self, mut f: F)
    where
        F: Future<Output = ()>,
    {
        let mut p = unsafe { Pin::new_unchecked(&mut f) };

        let mut events = mio::Events::with_capacity(1024);

        let mut need_poll = true;

        loop {
            if need_poll {
                need_poll = false;

                let w = NoopWaker {}.into_task_waker();

                let mut cx = Context::from_waker(&w);

                match p.as_mut().poll(&mut cx) {
                    Poll::Ready(_) => break,
                    Poll::Pending => {}
                }
            }

            self.poll.poll(&mut events, None).unwrap();

            for event in events.iter() {
                let key = usize::from(event.token());

                let data = &mut *self.data.borrow_mut();

                if let Some(event_reg) = data.registrations_nodes.get_mut(key) {
                    event_reg.value.ready = true;

                    if event_reg.value.enabled {
                        need_poll = true;
                    }
                }
            }
        }
    }
}

static VTABLE: RawWakerVTable = RawWakerVTable::new(vt_clone, vt_wake, vt_wake_by_ref, vt_drop);

struct NoopWaker {}

impl NoopWaker {
    fn into_task_waker(self) -> Waker {
        let rw = RawWaker::new(ptr::null_mut(), &VTABLE);
        unsafe { Waker::from_raw(rw) }
    }
}

unsafe fn vt_clone(_data: *const ()) -> RawWaker {
    RawWaker::new(ptr::null_mut(), &VTABLE)
}

unsafe fn vt_wake(_data: *const ()) {}

unsafe fn vt_wake_by_ref(_data: *const ()) {}

unsafe fn vt_drop(_data: *const ()) {}

pub struct SelectFromSliceFuture<'a, F> {
    futures: &'a mut [F],
}

impl<F, O> Future for SelectFromSliceFuture<'_, F>
where
    F: Future<Output = O>,
{
    type Output = (usize, F::Output);

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let s = unsafe { self.get_unchecked_mut() };

        for (i, f) in s.futures.iter_mut().enumerate() {
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

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let s = unsafe { self.get_unchecked_mut() };

        let p = unsafe { Pin::new_unchecked((&mut s.f1 as *mut F1).as_mut().unwrap()) };
        if let Poll::Ready(v) = p.poll(cx) {
            return Poll::Ready((Some(v), None));
        }

        let p = unsafe { Pin::new_unchecked((&mut s.f2 as *mut F2).as_mut().unwrap()) };
        if let Poll::Ready(v) = p.poll(cx) {
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

pub struct WaitWritableFuture<'a, 'ex, T> {
    s: &'a mut AsyncSender<'ex, T>,
}

impl<T> Future for WaitWritableFuture<'_, '_, T> {
    type Output = ();

    fn poll(self: Pin<&mut Self>, _cx: &mut Context) -> Poll<Self::Output> {
        let f = unsafe { self.get_unchecked_mut() };

        f.s.executor.set_enabled(f.s.reg_key, true);

        let dirty = f.s.executor.is_ready(f.s.reg_key);

        if dirty {
            f.s.writable.set(f.s.inner.can_send());
            f.s.executor.set_ready(f.s.reg_key, false);
        }

        if f.s.writable.get() {
            Poll::Ready(())
        } else {
            Poll::Pending
        }
    }
}

impl<T> Drop for WaitWritableFuture<'_, '_, T> {
    fn drop(&mut self) {
        self.s.executor.set_enabled(self.s.reg_key, false);
    }
}

pub struct RecvFuture<'a, 'ex, T> {
    r: &'a mut AsyncReceiver<'ex, T>,
}

impl<T> Future for RecvFuture<'_, '_, T> {
    type Output = Result<T, mpsc::RecvError>;

    fn poll(self: Pin<&mut Self>, _cx: &mut Context) -> Poll<Self::Output> {
        let f = unsafe { self.get_unchecked_mut() };

        f.r.executor.set_enabled(f.r.reg_key, true);

        let ready = f.r.executor.is_ready(f.r.reg_key);

        if !ready {
            return Poll::Pending;
        }

        match f.r.inner.try_recv() {
            Ok(v) => Poll::Ready(Ok(v)),
            Err(mpsc::TryRecvError::Empty) => {
                f.r.executor.set_ready(f.r.reg_key, false);

                Poll::Pending
            }
            Err(mpsc::TryRecvError::Disconnected) => Poll::Ready(Err(mpsc::RecvError)),
        }
    }
}

impl<T> Drop for RecvFuture<'_, '_, T> {
    fn drop(&mut self) {
        self.r.executor.set_enabled(self.r.reg_key, false);
    }
}

pub struct AcceptFuture<'a, 'ex> {
    l: &'a mut AsyncTcpListener<'ex>,
}

impl Future for AcceptFuture<'_, '_> {
    type Output = Result<(TcpStream, SocketAddr), io::Error>;

    fn poll(self: Pin<&mut Self>, _cx: &mut Context) -> Poll<Self::Output> {
        let f = unsafe { self.get_unchecked_mut() };

        f.l.executor.set_enabled(f.l.reg_key, true);

        let ready = f.l.executor.is_ready(f.l.reg_key);

        if !ready {
            return Poll::Pending;
        }

        match f.l.inner.accept() {
            Ok((stream, peer_addr)) => Poll::Ready(Ok((stream, peer_addr))),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.l.executor.set_ready(f.l.reg_key, false);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for AcceptFuture<'_, '_> {
    fn drop(&mut self) {
        self.l.executor.set_enabled(self.l.reg_key, false);
    }
}

pub struct AsyncSender<'ex, T> {
    executor: &'ex Executor,
    inner: channel::Sender<T>,
    reg_key: usize,
    writable: Cell<bool>,
}

impl<'ex, T> AsyncSender<'ex, T> {
    pub fn new(s: channel::Sender<T>, executor: &'ex Executor) -> Self {
        let e = executor;

        let key = e.register();

        e.poll
            .register(
                s.get_write_registration(),
                mio::Token(key),
                mio::Ready::writable(),
                mio::PollOpt::edge(),
            )
            .unwrap();

        let writable = s.can_send();

        // we know the state up front, so ready can start out unset
        e.set_ready(key, false);

        Self {
            executor,
            inner: s,
            reg_key: key,
            writable: Cell::new(writable),
        }
    }

    pub fn is_writable(&self) -> bool {
        let dirty = self.executor.is_ready(self.reg_key);

        if dirty {
            self.writable.set(self.inner.can_send());
            self.executor.set_ready(self.reg_key, false);
        }

        self.writable.get()
    }

    pub fn wait_writable<'a>(&'a mut self) -> WaitWritableFuture<'a, 'ex, T> {
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

impl<T> Drop for AsyncSender<'_, T> {
    fn drop(&mut self) {
        self.executor.unregister(self.reg_key);
    }
}

pub struct AsyncReceiver<'ex, T> {
    executor: &'ex Executor,
    inner: channel::Receiver<T>,
    reg_key: usize,
}

impl<'ex, T> AsyncReceiver<'ex, T> {
    pub fn new(r: channel::Receiver<T>, executor: &'ex Executor) -> Self {
        let e = executor;

        let key = e.register();

        e.poll
            .register(
                r.get_read_registration(),
                mio::Token(key),
                mio::Ready::readable(),
                mio::PollOpt::edge(),
            )
            .unwrap();

        e.set_ready(key, true);

        Self {
            executor,
            inner: r,
            reg_key: key,
        }
    }

    pub fn recv<'a>(&'a mut self) -> RecvFuture<'a, 'ex, T> {
        RecvFuture { r: self }
    }
}

impl<T> Drop for AsyncReceiver<'_, T> {
    fn drop(&mut self) {
        self.executor.unregister(self.reg_key);
    }
}

pub struct AsyncTcpListener<'ex> {
    executor: &'ex Executor,
    inner: TcpListener,
    reg_key: usize,
}

impl<'ex> AsyncTcpListener<'ex> {
    pub fn new(l: TcpListener, executor: &'ex Executor) -> Self {
        let e = executor;

        let key = e.register();

        e.poll
            .register(
                &l,
                mio::Token(key),
                mio::Ready::readable(),
                mio::PollOpt::edge(),
            )
            .unwrap();

        e.set_ready(key, true);

        Self {
            executor,
            inner: l,
            reg_key: key,
        }
    }

    pub fn accept<'a>(&'a mut self) -> AcceptFuture<'a, 'ex> {
        AcceptFuture { l: self }
    }
}

impl Drop for AsyncTcpListener<'_> {
    fn drop(&mut self) {
        self.executor.unregister(self.reg_key);
    }
}
