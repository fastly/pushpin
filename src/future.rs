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
use std::sync::mpsc;
use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};

pub trait Reactor {
    fn poll(&self) -> Result<(), io::Error>;
}

pub struct RegistrationHandle<'a> {
    reactor: &'a MioReactor,
    key: usize,
}

impl RegistrationHandle<'_> {
    fn is_ready(&self) -> bool {
        let data = &*self.reactor.data.borrow();

        let event_reg = &data.registrations[self.key];

        event_reg.ready
    }

    fn set_ready(&self, ready: bool) {
        let data = &mut *self.reactor.data.borrow_mut();

        let event_reg = &mut data.registrations[self.key];

        event_reg.ready = ready;
    }

    fn bind_waker(&self, waker: Waker) {
        let data = &mut *self.reactor.data.borrow_mut();

        let event_reg = &mut data.registrations[self.key];

        event_reg.waker = Some(waker);
    }

    fn unbind_waker(&self) {
        let data = &mut *self.reactor.data.borrow_mut();

        let event_reg = &mut data.registrations[self.key];

        event_reg.waker = None;
    }
}

impl Drop for RegistrationHandle<'_> {
    fn drop(&mut self) {
        let data = &mut *self.reactor.data.borrow_mut();

        data.registrations.remove(self.key);
    }
}

struct EventRegistration {
    ready: bool,
    waker: Option<Waker>,
}

struct MioReactorData {
    registrations: Slab<EventRegistration>,
    events: mio::Events,
}

pub struct MioReactor {
    data: RefCell<MioReactorData>,
    poll: mio::Poll,
}

impl MioReactor {
    pub fn new(registrations_max: usize) -> Self {
        let data = MioReactorData {
            registrations: Slab::with_capacity(registrations_max),
            events: mio::Events::with_capacity(1024),
        };

        Self {
            data: RefCell::new(data),
            poll: mio::Poll::new().unwrap(),
        }
    }

    fn register<E>(&self, handle: &E, interest: mio::Ready) -> Result<RegistrationHandle, io::Error>
    where
        E: mio::Evented + ?Sized,
    {
        let data = &mut *self.data.borrow_mut();

        if data.registrations.len() == data.registrations.capacity() {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        let key = data.registrations.insert(EventRegistration {
            ready: false,
            waker: None,
        });

        if let Err(e) = self
            .poll
            .register(handle, mio::Token(key), interest, mio::PollOpt::edge())
        {
            data.registrations.remove(key);

            return Err(e);
        }

        Ok(RegistrationHandle { reactor: self, key })
    }
}

impl Reactor for MioReactor {
    fn poll(&self) -> Result<(), io::Error> {
        let data = &mut *self.data.borrow_mut();

        self.poll.poll(&mut data.events, None)?;

        for event in data.events.iter() {
            let key = usize::from(event.token());

            if let Some(event_reg) = data.registrations.get_mut(key) {
                event_reg.ready = true;

                if let Some(waker) = event_reg.waker.take() {
                    waker.wake();
                }
            }
        }

        Ok(())
    }
}

struct SharedWaker<'f> {
    executor: *const Executor<'f>,
    task_id: usize,
}

impl SharedWaker<'_> {
    fn as_std_waker(&self) -> Waker {
        let executor = unsafe { self.executor.as_ref().unwrap() };

        executor.add_waker_ref(self.task_id);

        let rw = RawWaker::new(self as *const Self as *const (), Self::vtable());

        unsafe { Waker::from_raw(rw) }
    }

    unsafe fn clone(data: *const ()) -> RawWaker {
        let s = (data as *const Self).as_ref().unwrap();

        let executor = s.executor.as_ref().unwrap();

        executor.add_waker_ref(s.task_id);

        RawWaker::new(data, &Self::vtable())
    }

    unsafe fn wake(data: *const ()) {
        Self::wake_by_ref(data);

        Self::drop(data);
    }

    unsafe fn wake_by_ref(data: *const ()) {
        let s = (data as *const Self).as_ref().unwrap();

        let executor = s.executor.as_ref().unwrap();

        executor.wake(s.task_id);
    }

    unsafe fn drop(data: *const ()) {
        let s = (data as *const Self).as_ref().unwrap();

        let executor = s.executor.as_ref().unwrap();

        executor.remove_waker_ref(s.task_id);
    }

    fn vtable() -> &'static RawWakerVTable {
        &RawWakerVTable::new(Self::clone, Self::wake, Self::wake_by_ref, Self::drop)
    }
}

struct Task<'f> {
    fut: Option<Pin<Box<dyn Future<Output = ()> + 'f>>>,
    waker: SharedWaker<'f>,
    waker_refs: usize,
    awake: bool,
}

struct Tasks<'f> {
    nodes: Slab<list::Node<Task<'f>>>,
    next: list::List,
}

pub struct Executor<'f> {
    tasks: RefCell<Tasks<'f>>,
}

impl<'f> Executor<'f> {
    pub fn new(tasks_max: usize) -> Self {
        Self {
            tasks: RefCell::new(Tasks {
                nodes: Slab::with_capacity(tasks_max),
                next: list::List::default(),
            }),
        }
    }

    pub fn spawn<F>(&self, f: F) -> Result<(), ()>
    where
        F: Future<Output = ()> + 'f,
    {
        let tasks = &mut *self.tasks.borrow_mut();

        if tasks.nodes.len() == tasks.nodes.capacity() {
            return Err(());
        }

        let entry = tasks.nodes.vacant_entry();
        let key = entry.key();

        let waker = SharedWaker {
            executor: self,
            task_id: key,
        };

        let task = Task {
            fut: Some(Box::pin(f)),
            waker,
            waker_refs: 0,
            awake: true,
        };

        entry.insert(list::Node::new(task));

        tasks.next.push_back(&mut tasks.nodes, key);

        Ok(())
    }

    pub fn exec(&self, reactor: &dyn Reactor) -> Result<(), io::Error> {
        loop {
            self.process_tasks();

            if !self.have_tasks() {
                break;
            }

            reactor.poll()?;
        }

        Ok(())
    }

    fn have_tasks(&self) -> bool {
        !self.tasks.borrow().nodes.is_empty()
    }

    fn process_tasks(&self) {
        loop {
            let (nkey, task_ptr) = {
                let tasks = &mut *self.tasks.borrow_mut();

                let nkey = match tasks.next.head {
                    Some(nkey) => nkey,
                    None => break,
                };

                tasks.next.remove(&mut tasks.nodes, nkey);

                let task = &mut tasks.nodes[nkey].value;

                task.awake = false;

                (nkey, task as *mut Task)
            };

            // task won't move/drop while this pointer is in use
            let task: &mut Task = unsafe { task_ptr.as_mut().unwrap() };

            let done = {
                let f: Pin<&mut dyn Future<Output = ()>> = task.fut.as_mut().unwrap().as_mut();

                let w = task.waker.as_std_waker();

                let mut cx = Context::from_waker(&w);

                match f.poll(&mut cx) {
                    Poll::Ready(_) => true,
                    Poll::Pending => false,
                }
            };

            if done {
                task.fut = None;

                let tasks = &mut *self.tasks.borrow_mut();

                let task = &mut tasks.nodes[nkey].value;

                assert_eq!(task.waker_refs, 0);

                tasks.next.remove(&mut tasks.nodes, nkey);
                tasks.nodes.remove(nkey);
            }
        }
    }

    fn add_waker_ref(&self, task_id: usize) {
        let tasks = &mut *self.tasks.borrow_mut();

        let task = &mut tasks.nodes[task_id].value;

        task.waker_refs += 1;
    }

    fn remove_waker_ref(&self, task_id: usize) {
        let tasks = &mut *self.tasks.borrow_mut();

        let task = &mut tasks.nodes[task_id].value;

        assert!(task.waker_refs > 0);

        task.waker_refs -= 1;
    }

    fn wake(&self, task_id: usize) {
        let tasks = &mut *self.tasks.borrow_mut();

        let task = &mut tasks.nodes[task_id].value;

        if !task.awake {
            task.awake = true;

            tasks.next.remove(&mut tasks.nodes, task_id);
            tasks.next.push_back(&mut tasks.nodes, task_id);
        }
    }
}

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

pub struct AsyncSender<'r, T> {
    inner: channel::Sender<T>,
    handle: RegistrationHandle<'r>,
    writable: Cell<bool>,
}

impl<'r, T> AsyncSender<'r, T> {
    pub fn new(s: channel::Sender<T>, reactor: &'r MioReactor) -> Self {
        let handle = reactor
            .register(s.get_write_registration(), mio::Ready::writable())
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

    pub fn wait_writable<'a>(&'a mut self) -> WaitWritableFuture<'a, 'r, T> {
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

pub struct AsyncReceiver<'r, T> {
    inner: channel::Receiver<T>,
    handle: RegistrationHandle<'r>,
}

impl<'r, T> AsyncReceiver<'r, T> {
    pub fn new(r: channel::Receiver<T>, reactor: &'r MioReactor) -> Self {
        let handle = reactor
            .register(r.get_read_registration(), mio::Ready::readable())
            .unwrap();

        handle.set_ready(true);

        Self { inner: r, handle }
    }

    pub fn recv<'a>(&'a mut self) -> RecvFuture<'a, 'r, T> {
        RecvFuture { r: self }
    }
}

pub struct AsyncTcpListener<'r> {
    inner: TcpListener,
    handle: RegistrationHandle<'r>,
}

impl<'r> AsyncTcpListener<'r> {
    pub fn new(l: TcpListener, reactor: &'r MioReactor) -> Self {
        let handle = reactor.register(&l, mio::Ready::readable()).unwrap();

        handle.set_ready(true);

        Self { inner: l, handle }
    }

    pub fn accept<'a>(&'a mut self) -> AcceptFuture<'a, 'r> {
        AcceptFuture { l: self }
    }
}

pub struct WaitWritableFuture<'a, 'r, T> {
    s: &'a mut AsyncSender<'r, T>,
}

impl<T> Future for WaitWritableFuture<'_, '_, T> {
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

impl<T> Drop for WaitWritableFuture<'_, '_, T> {
    fn drop(&mut self) {
        self.s.handle.unbind_waker();
    }
}

pub struct RecvFuture<'a, 'r, T> {
    r: &'a mut AsyncReceiver<'r, T>,
}

impl<T> Future for RecvFuture<'_, '_, T> {
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

impl<T> Drop for RecvFuture<'_, '_, T> {
    fn drop(&mut self) {
        self.r.handle.unbind_waker();
    }
}

pub struct AcceptFuture<'a, 'r> {
    l: &'a mut AsyncTcpListener<'r>,
}

impl Future for AcceptFuture<'_, '_> {
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

impl Drop for AcceptFuture<'_, '_> {
    fn drop(&mut self) {
        self.l.handle.unbind_waker();
    }
}
