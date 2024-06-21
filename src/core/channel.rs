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
use crate::core::event;
use crate::core::list;
use crate::core::reactor::CustomEvented;
use crate::future::get_reactor;
use slab::Slab;
use std::cell::RefCell;
use std::collections::VecDeque;
use std::future::Future;
use std::mem;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::Arc;
use std::task::{Context, Poll};

pub struct Sender<T> {
    sender: Option<mpsc::SyncSender<T>>,
    read_set_readiness: event::SetReadiness,
    write_registration: event::Registration,
    cts: Option<Arc<AtomicBool>>,
}

impl<T> Sender<T> {
    // NOTE: only makes sense for rendezvous channels
    pub fn can_send(&self) -> bool {
        match &self.cts {
            Some(cts) => cts.load(Ordering::Relaxed),
            None => true,
        }
    }

    pub fn get_write_registration(&self) -> &event::Registration {
        &self.write_registration
    }

    pub fn try_send(&self, t: T) -> Result<(), mpsc::TrySendError<T>> {
        if let Some(cts) = &self.cts {
            if cts
                .compare_exchange(true, false, Ordering::Relaxed, Ordering::Relaxed)
                .is_err()
            {
                return Err(mpsc::TrySendError::Full(t));
            }

            // cts will only be true if a read was performed while the queue
            // was empty, and this function is the only place where the queue
            // is written to. this means the try_send call below will only
            // fail if the receiver disconnected
        }

        match self.sender.as_ref().unwrap().try_send(t) {
            Ok(_) => {
                self.read_set_readiness
                    .set_readiness(mio::Interest::READABLE)
                    .unwrap();

                Ok(())
            }
            Err(e) => Err(e),
        }
    }

    pub fn send(&self, t: T) -> Result<(), mpsc::SendError<T>> {
        if self.cts.is_some() {
            panic!("blocking send with rendezvous channel not supported")
        }

        match self.sender.as_ref().unwrap().send(t) {
            Ok(_) => {
                self.read_set_readiness
                    .set_readiness(mio::Interest::READABLE)
                    .unwrap();

                Ok(())
            }
            Err(e) => Err(e),
        }
    }
}

impl<T> Drop for Sender<T> {
    fn drop(&mut self) {
        mem::drop(self.sender.take().unwrap());

        self.read_set_readiness
            .set_readiness(mio::Interest::READABLE)
            .unwrap();
    }
}

pub struct Receiver<T> {
    receiver: mpsc::Receiver<T>,
    read_registration: event::Registration,
    write_set_readiness: event::SetReadiness,
    cts: Option<Arc<AtomicBool>>,
}

impl<T> Receiver<T> {
    pub fn get_read_registration(&self) -> &event::Registration {
        &self.read_registration
    }

    pub fn try_recv(&self) -> Result<T, mpsc::TryRecvError> {
        match self.receiver.try_recv() {
            Ok(t) => {
                if self.cts.is_none() {
                    self.write_set_readiness
                        .set_readiness(mio::Interest::WRITABLE)
                        .unwrap();
                }

                Ok(t)
            }
            Err(mpsc::TryRecvError::Empty) if self.cts.is_some() => {
                let cts = self.cts.as_ref().unwrap();

                if cts
                    .compare_exchange(false, true, Ordering::Relaxed, Ordering::Relaxed)
                    .is_ok()
                {
                    self.write_set_readiness
                        .set_readiness(mio::Interest::WRITABLE)
                        .unwrap();
                }

                Err(mpsc::TryRecvError::Empty)
            }
            Err(e) => Err(e),
        }
    }

    pub fn recv(&self) -> Result<T, mpsc::RecvError> {
        let t = self.receiver.recv()?;

        if self.cts.is_none() {
            self.write_set_readiness
                .set_readiness(mio::Interest::WRITABLE)
                .unwrap();
        }

        Ok(t)
    }
}

pub fn channel<T>(bound: usize) -> (Sender<T>, Receiver<T>) {
    let (read_reg, read_sr) = event::Registration::new();
    let (write_reg, write_sr) = event::Registration::new();

    // rendezvous channel
    if bound == 0 {
        let (s, r) = mpsc::sync_channel::<T>(1);

        let cts = Arc::new(AtomicBool::new(false));

        let sender = Sender {
            sender: Some(s),
            read_set_readiness: read_sr,
            write_registration: write_reg,
            cts: Some(Arc::clone(&cts)),
        };

        let receiver = Receiver {
            receiver: r,
            read_registration: read_reg,
            write_set_readiness: write_sr,
            cts: Some(Arc::clone(&cts)),
        };

        (sender, receiver)
    } else {
        let (s, r) = mpsc::sync_channel::<T>(bound);

        let sender = Sender {
            sender: Some(s),
            read_set_readiness: read_sr,
            write_registration: write_reg,
            cts: None,
        };

        let receiver = Receiver {
            receiver: r,
            read_registration: read_reg,
            write_set_readiness: write_sr,
            cts: None,
        };

        // channel is immediately writable
        receiver
            .write_set_readiness
            .set_readiness(mio::Interest::WRITABLE)
            .unwrap();

        (sender, receiver)
    }
}

struct LocalSenderData {
    notified: bool,
    write_set_readiness: event::LocalSetReadiness,
}

struct LocalSenders {
    nodes: Slab<list::Node<LocalSenderData>>,
    waiting: list::List,
}

struct LocalChannel<T> {
    queue: RefCell<VecDeque<T>>,
    senders: RefCell<LocalSenders>,
    read_set_readiness: RefCell<Option<event::LocalSetReadiness>>,
}

impl<T> LocalChannel<T> {
    fn senders_is_empty(&self) -> bool {
        self.senders.borrow().nodes.is_empty()
    }

    fn add_sender(&self, write_sr: event::LocalSetReadiness) -> Result<usize, ()> {
        let mut senders = self.senders.borrow_mut();

        if senders.nodes.len() == senders.nodes.capacity() {
            return Err(());
        }

        let key = senders.nodes.insert(list::Node::new(LocalSenderData {
            notified: false,
            write_set_readiness: write_sr,
        }));

        Ok(key)
    }

    fn remove_sender(&self, key: usize) {
        let senders = &mut *self.senders.borrow_mut();

        senders.waiting.remove(&mut senders.nodes, key);
        senders.nodes.remove(key);

        if senders.nodes.is_empty() {
            if let Some(read_sr) = &*self.read_set_readiness.borrow() {
                // notify for disconnect
                read_sr.set_readiness(mio::Interest::READABLE).unwrap();
            }
        }
    }

    fn set_sender_waiting(&self, key: usize) {
        let senders = &mut *self.senders.borrow_mut();

        // add if not already present
        if senders.nodes[key].prev.is_none() && senders.waiting.head != Some(key) {
            senders.waiting.push_back(&mut senders.nodes, key);
        }
    }

    fn notify_one_sender(&self) {
        let senders = &mut *self.senders.borrow_mut();

        // notify next waiting sender, if any
        if let Some(key) = senders.waiting.pop_front(&mut senders.nodes) {
            let sender = &mut senders.nodes[key].value;

            sender.notified = true;
            sender
                .write_set_readiness
                .set_readiness(mio::Interest::WRITABLE)
                .unwrap();
        }
    }

    fn sender_is_notified(&self, key: usize) -> bool {
        self.senders.borrow().nodes[key].value.notified
    }

    fn clear_sender_notified(&self, key: usize) {
        self.senders.borrow_mut().nodes[key].value.notified = false;
    }
}

pub struct LocalSender<T> {
    channel: Rc<LocalChannel<T>>,
    key: usize,
    write_registration: event::LocalRegistration,
}

impl<T> LocalSender<T> {
    pub fn get_write_registration(&self) -> &event::LocalRegistration {
        &self.write_registration
    }

    // if this returns true, then the next call to try_send() by any sender
    // is guaranteed to not return TrySendError::Full.
    // if this returns false, the sender is added to the wait list
    pub fn check_send(&self) -> bool {
        let queue = self.channel.queue.borrow();

        let can_send = queue.len() < queue.capacity();

        if !can_send {
            self.channel.set_sender_waiting(self.key);
        }

        can_send
    }

    pub fn try_send(&self, t: T) -> Result<(), mpsc::TrySendError<T>> {
        // we are acting, so clear the notified flag
        self.channel.clear_sender_notified(self.key);

        let read_sr = &*self.channel.read_set_readiness.borrow();

        let read_sr = match read_sr {
            Some(sr) => sr,
            None => {
                // receiver is disconnected
                return Err(mpsc::TrySendError::Disconnected(t));
            }
        };

        let mut queue = self.channel.queue.borrow_mut();

        if queue.len() < queue.capacity() {
            queue.push_back(t);

            read_sr.set_readiness(mio::Interest::READABLE).unwrap();

            Ok(())
        } else {
            self.channel.set_sender_waiting(self.key);

            Err(mpsc::TrySendError::Full(t))
        }
    }

    pub fn cancel(&self) {
        // if we were notified but never acted on it, notify the next waiting sender, if any
        if self.channel.sender_is_notified(self.key) {
            self.channel.clear_sender_notified(self.key);

            self.channel.notify_one_sender();
        }
    }

    // NOTE: if the receiver is dropped while there are multiple senders,
    // only one of the senders will be notified of the disconnect
    #[allow(clippy::result_unit_err)]
    pub fn try_clone(
        &self,
        memory: &Rc<arena::RcMemory<event::LocalRegistrationEntry>>,
    ) -> Result<Self, ()> {
        let (write_reg, write_sr) = event::LocalRegistration::new(memory);

        let key = self.channel.add_sender(write_sr)?;

        Ok(Self {
            channel: self.channel.clone(),
            key,
            write_registration: write_reg,
        })
    }

    // returns error if a receiver already exists
    #[allow(clippy::result_unit_err)]
    pub fn make_receiver(
        &self,
        memory: &Rc<arena::RcMemory<event::LocalRegistrationEntry>>,
    ) -> Result<LocalReceiver<T>, ()> {
        if self.channel.read_set_readiness.borrow().is_some() {
            return Err(());
        }

        let (read_reg, read_sr) = event::LocalRegistration::new(memory);

        *self.channel.read_set_readiness.borrow_mut() = Some(read_sr);

        Ok(LocalReceiver {
            channel: self.channel.clone(),
            read_registration: read_reg,
        })
    }
}

impl<T> Drop for LocalSender<T> {
    fn drop(&mut self) {
        self.cancel();

        self.channel.remove_sender(self.key);
    }
}

pub struct LocalReceiver<T> {
    channel: Rc<LocalChannel<T>>,
    read_registration: event::LocalRegistration,
}

impl<T> LocalReceiver<T> {
    pub fn get_read_registration(&self) -> &event::LocalRegistration {
        &self.read_registration
    }

    pub fn try_recv(&self) -> Result<T, mpsc::TryRecvError> {
        let mut queue = self.channel.queue.borrow_mut();

        if queue.is_empty() {
            if self.channel.senders_is_empty() {
                return Err(mpsc::TryRecvError::Disconnected);
            }

            return Err(mpsc::TryRecvError::Empty);
        }

        let value = queue.pop_front().unwrap();

        self.channel.notify_one_sender();

        Ok(value)
    }

    pub fn clear(&self) {
        // loop over try_recv() in order to notify senders
        while self.try_recv().is_ok() {}
    }
}

impl<T> Drop for LocalReceiver<T> {
    fn drop(&mut self) {
        *self.channel.read_set_readiness.borrow_mut() = None;

        self.channel.notify_one_sender();
    }
}

pub fn local_channel<T>(
    bound: usize,
    max_senders: usize,
    memory: &Rc<arena::RcMemory<event::LocalRegistrationEntry>>,
) -> (LocalSender<T>, LocalReceiver<T>) {
    let (read_reg, read_sr) = event::LocalRegistration::new(memory);
    let (write_reg, write_sr) = event::LocalRegistration::new(memory);

    // no support for rendezvous channels
    assert!(bound > 0);

    // need to support at least one sender
    assert!(max_senders > 0);

    let channel = Rc::new(LocalChannel {
        queue: RefCell::new(VecDeque::with_capacity(bound)),
        senders: RefCell::new(LocalSenders {
            nodes: Slab::with_capacity(max_senders),
            waiting: list::List::default(),
        }),
        read_set_readiness: RefCell::new(Some(read_sr)),
    });

    let key = channel.add_sender(write_sr).unwrap();

    let sender = LocalSender {
        channel: channel.clone(),
        key,
        write_registration: write_reg,
    };

    let receiver = LocalReceiver {
        channel,
        read_registration: read_reg,
    };

    (sender, receiver)
}

pub struct AsyncSender<T> {
    evented: CustomEvented,
    inner: Sender<T>,
}

impl<T> AsyncSender<T> {
    pub fn new(s: Sender<T>) -> Self {
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

    pub fn wait_writable(&self) -> WaitWritableFuture<'_, T> {
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

    pub fn send(&self, t: T) -> SendFuture<'_, T> {
        SendFuture {
            s: self,
            t: Some(t),
        }
    }
}

pub struct AsyncReceiver<T> {
    evented: CustomEvented,
    inner: Receiver<T>,
}

impl<T> AsyncReceiver<T> {
    pub fn new(r: Receiver<T>) -> Self {
        let evented = CustomEvented::new(
            r.get_read_registration(),
            mio::Interest::READABLE,
            &get_reactor(),
        )
        .unwrap();

        evented.registration().set_ready(true);

        Self { evented, inner: r }
    }

    pub fn recv(&self) -> RecvFuture<'_, T> {
        RecvFuture { r: self }
    }
}

pub struct AsyncLocalSender<T> {
    evented: CustomEvented,
    inner: LocalSender<T>,
}

impl<T> AsyncLocalSender<T> {
    pub fn new(s: LocalSender<T>) -> Self {
        let evented = CustomEvented::new_local(
            s.get_write_registration(),
            mio::Interest::WRITABLE,
            &get_reactor(),
        )
        .unwrap();

        evented.registration().set_ready(true);

        Self { evented, inner: s }
    }

    pub fn into_inner(self) -> LocalSender<T> {
        // normally, the poll registration would be deregistered when the
        // sender drops, but here we are keeping the sender alive, so we need
        // to explicitly deregister
        self.evented
            .registration()
            .deregister_custom_local(self.inner.get_write_registration())
            .unwrap();

        self.inner
    }

    pub fn send(&self, t: T) -> LocalSendFuture<'_, T> {
        LocalSendFuture {
            s: self,
            t: Some(t),
        }
    }

    // it's okay to run multiple instances of this future within the same
    // task. see the comment on the CheckSendFuture struct
    pub fn check_send(&self) -> CheckSendFuture<'_, T> {
        CheckSendFuture { s: self }
    }

    pub fn try_send(&self, t: T) -> Result<(), mpsc::TrySendError<T>> {
        self.inner.try_send(t)
    }

    pub fn cancel(&self) {
        self.inner.cancel();
    }
}

pub struct AsyncLocalReceiver<T> {
    evented: CustomEvented,
    inner: LocalReceiver<T>,
}

impl<T> AsyncLocalReceiver<T> {
    pub fn new(r: LocalReceiver<T>) -> Self {
        let evented = CustomEvented::new_local(
            r.get_read_registration(),
            mio::Interest::READABLE,
            &get_reactor(),
        )
        .unwrap();

        evented.registration().set_ready(true);

        Self { evented, inner: r }
    }

    pub fn into_inner(self) -> LocalReceiver<T> {
        // normally, the poll registration would be deregistered when the
        // receiver drops, but here we are keeping the receiver alive, so we
        // need to explicitly deregister
        self.evented
            .registration()
            .deregister_custom_local(self.inner.get_read_registration())
            .unwrap();

        self.inner
    }

    pub fn recv(&self) -> LocalRecvFuture<'_, T> {
        LocalRecvFuture { r: self }
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
            .set_waker(cx.waker(), mio::Interest::WRITABLE);

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
            .set_waker(cx.waker(), mio::Interest::WRITABLE);

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
            .set_waker(cx.waker(), mio::Interest::READABLE);

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
            .set_waker(cx.waker(), mio::Interest::WRITABLE);

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
        self.s.inner.cancel();

        self.s.evented.registration().clear_waker();
    }
}

// it's okay to maintain multiple instances of this future at the same time
// within the same task. calling poll() won't negatively affect other
// instances. the drop() method clears the waker on the shared registration,
// which may look problematic. however, whenever any instance is (re-)polled,
// the waker will be reinstated.
//
// notably, these scenarios work:
//
// * creating two instances and awaiting them sequentially
// * creating two instances and selecting on them in a loop. both will
//   eventually complete
// * creating one instance, polling it to pending, then creating a second
//   instance and polling it to completion, then polling on the first
//   instance again
pub struct CheckSendFuture<'a, T> {
    s: &'a AsyncLocalSender<T>,
}

impl<T> Future for CheckSendFuture<'_, T>
where
    T: Unpin,
{
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::WRITABLE);

        if !f.s.inner.check_send() {
            f.s.evented.registration().set_ready(false);

            return Poll::Pending;
        }

        Poll::Ready(())
    }
}

impl<T> Drop for CheckSendFuture<'_, T> {
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
            .set_waker(cx.waker(), mio::Interest::READABLE);

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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::executor::Executor;
    use crate::core::reactor::Reactor;
    use crate::future::poll_async;
    use std::cell::Cell;
    use std::time;

    #[test]
    fn test_send_recv_bound0() {
        let (sender, receiver) = channel(0);

        assert_eq!(sender.can_send(), false);

        let result = sender.try_send(42);
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TrySendError::Full(42));

        let result = receiver.try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Empty);

        assert_eq!(sender.can_send(), true);

        let result = sender.try_send(42);
        assert_eq!(result.is_ok(), true);
        assert_eq!(sender.can_send(), false);

        let result = receiver.try_recv();
        assert_eq!(result.is_ok(), true);

        let v = result.unwrap();
        assert_eq!(v, 42);

        let result = receiver.try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Empty);

        mem::drop(sender);

        let result = receiver.try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Disconnected);
    }

    #[test]
    fn test_send_recv_bound1() {
        let (sender, receiver) = channel(1);

        let result = receiver.try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Empty);

        let result = sender.try_send(42);
        assert_eq!(result.is_ok(), true);

        let result = sender.try_send(42);
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TrySendError::Full(42));

        let result = receiver.try_recv();
        assert_eq!(result.is_ok(), true);

        let v = result.unwrap();
        assert_eq!(v, 42);

        let result = receiver.try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Empty);

        mem::drop(sender);

        let result = receiver.try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Disconnected);
    }

    #[test]
    fn test_notify_bound0() {
        let (sender, receiver) = channel(0);

        let mut poller = event::Poller::new(2).unwrap();

        poller
            .register_custom(
                sender.get_write_registration(),
                mio::Token(1),
                mio::Interest::WRITABLE,
            )
            .unwrap();

        poller
            .register_custom(
                receiver.get_read_registration(),
                mio::Token(2),
                mio::Interest::READABLE,
            )
            .unwrap();

        assert_eq!(sender.can_send(), false);

        poller.poll(Some(time::Duration::from_millis(0))).unwrap();

        assert_eq!(poller.iter_events().next(), None);

        let result = receiver.try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Empty);

        poller.poll(None).unwrap();

        let mut it = poller.iter_events();

        let event = it.next().unwrap();
        assert_eq!(event.token(), mio::Token(1));
        assert_eq!(event.is_writable(), true);
        assert_eq!(it.next(), None);

        assert_eq!(sender.can_send(), true);

        sender.try_send(42).unwrap();

        poller.poll(None).unwrap();

        let mut it = poller.iter_events();

        let event = it.next().unwrap();
        assert_eq!(event.token(), mio::Token(2));
        assert_eq!(event.is_readable(), true);
        assert_eq!(it.next(), None);

        let v = receiver.try_recv().unwrap();

        assert_eq!(v, 42);

        mem::drop(sender);

        poller.poll(None).unwrap();

        let mut it = poller.iter_events();

        let event = it.next().unwrap();
        assert_eq!(event.token(), mio::Token(2));
        assert_eq!(event.is_readable(), true);
        assert_eq!(it.next(), None);

        let e = receiver.try_recv().unwrap_err();
        assert_eq!(e, mpsc::TryRecvError::Disconnected);
    }

    #[test]
    fn test_notify_bound1() {
        let (sender, receiver) = channel(1);

        let mut poller = event::Poller::new(2).unwrap();

        poller
            .register_custom(
                sender.get_write_registration(),
                mio::Token(1),
                mio::Interest::WRITABLE,
            )
            .unwrap();

        poller
            .register_custom(
                receiver.get_read_registration(),
                mio::Token(2),
                mio::Interest::READABLE,
            )
            .unwrap();

        poller.poll(Some(time::Duration::from_millis(0))).unwrap();

        let mut it = poller.iter_events();

        let event = it.next().unwrap();
        assert_eq!(event.token(), mio::Token(1));
        assert_eq!(event.is_writable(), true);
        assert_eq!(it.next(), None);

        sender.try_send(42).unwrap();

        poller.poll(None).unwrap();

        let mut it = poller.iter_events();

        let event = it.next().unwrap();
        assert_eq!(event.token(), mio::Token(2));
        assert_eq!(event.is_readable(), true);
        assert_eq!(it.next(), None);

        let v = receiver.try_recv().unwrap();

        assert_eq!(v, 42);

        mem::drop(sender);

        poller.poll(None).unwrap();

        let mut it = poller.iter_events();

        let event = it.next().unwrap();
        assert_eq!(event.token(), mio::Token(2));
        assert_eq!(event.is_readable(), true);
        assert_eq!(it.next(), None);

        let e = receiver.try_recv().unwrap_err();
        assert_eq!(e, mpsc::TryRecvError::Disconnected);
    }

    #[test]
    fn test_local_send_recv() {
        let poller = event::Poller::new(6).unwrap();

        let (sender1, receiver) = local_channel(1, 2, poller.local_registration_memory());

        assert_eq!(receiver.try_recv(), Err(mpsc::TryRecvError::Empty));

        assert_eq!(sender1.try_send(1), Ok(()));

        assert_eq!(receiver.try_recv(), Ok(1));

        let sender2 = sender1
            .try_clone(poller.local_registration_memory())
            .unwrap();

        assert_eq!(sender1.try_send(2), Ok(()));

        let channel = sender2.channel.clone();

        assert_eq!(channel.senders.borrow().waiting.is_empty(), true);
        assert_eq!(
            channel.senders.borrow().nodes[sender2.key].value.notified,
            false
        );

        assert_eq!(sender2.try_send(3), Err(mpsc::TrySendError::Full(3)));
        assert_eq!(channel.senders.borrow().waiting.is_empty(), false);
        assert_eq!(
            channel.senders.borrow().nodes[sender2.key].value.notified,
            false
        );

        assert_eq!(receiver.try_recv(), Ok(2));

        assert_eq!(channel.senders.borrow().waiting.is_empty(), true);
        assert_eq!(
            channel.senders.borrow().nodes[sender2.key].value.notified,
            true
        );

        assert_eq!(sender2.try_send(3), Ok(()));
        assert_eq!(
            channel.senders.borrow().nodes[sender2.key].value.notified,
            false
        );

        assert_eq!(receiver.try_recv(), Ok(3));

        mem::drop(sender1);
        mem::drop(sender2);

        assert_eq!(receiver.try_recv(), Err(mpsc::TryRecvError::Disconnected));
    }

    #[test]
    fn test_local_send_disc() {
        let poller = event::Poller::new(4).unwrap();

        let (sender, receiver) = local_channel(1, 1, poller.local_registration_memory());

        mem::drop(receiver);

        assert_eq!(sender.try_send(1), Err(mpsc::TrySendError::Disconnected(1)));
    }

    #[test]
    fn test_local_cancel() {
        let poller = event::Poller::new(6).unwrap();

        let (sender1, receiver) = local_channel(1, 2, poller.local_registration_memory());

        let sender2 = sender1
            .try_clone(poller.local_registration_memory())
            .unwrap();
        let channel = sender2.channel.clone();

        assert_eq!(sender1.try_send(1), Ok(()));

        assert_eq!(sender2.try_send(2), Err(mpsc::TrySendError::Full(2)));
        assert_eq!(sender1.try_send(3), Err(mpsc::TrySendError::Full(3)));
        assert_eq!(channel.senders.borrow().waiting.is_empty(), false);
        assert_eq!(
            channel.senders.borrow().nodes[sender1.key].value.notified,
            false
        );
        assert_eq!(
            channel.senders.borrow().nodes[sender2.key].value.notified,
            false
        );

        assert_eq!(receiver.try_recv(), Ok(1));
        assert_eq!(channel.senders.borrow().waiting.is_empty(), false);
        assert_eq!(
            channel.senders.borrow().nodes[sender1.key].value.notified,
            false
        );
        assert_eq!(
            channel.senders.borrow().nodes[sender2.key].value.notified,
            true
        );

        sender2.cancel();
        assert_eq!(channel.senders.borrow().waiting.is_empty(), true);
        assert_eq!(
            channel.senders.borrow().nodes[sender1.key].value.notified,
            true
        );
        assert_eq!(
            channel.senders.borrow().nodes[sender2.key].value.notified,
            false
        );

        assert_eq!(sender1.try_send(3), Ok(()));
        assert_eq!(
            channel.senders.borrow().nodes[sender1.key].value.notified,
            false
        );

        assert_eq!(receiver.try_recv(), Ok(3));
    }

    #[test]
    fn test_local_check_send() {
        let poller = event::Poller::new(4).unwrap();

        let (sender, receiver) = local_channel(1, 1, poller.local_registration_memory());

        assert_eq!(receiver.try_recv(), Err(mpsc::TryRecvError::Empty));

        let channel = sender.channel.clone();

        assert_eq!(sender.check_send(), true);
        assert_eq!(channel.senders.borrow().waiting.is_empty(), true);
        assert_eq!(
            channel.senders.borrow().nodes[sender.key].value.notified,
            false
        );

        assert_eq!(sender.try_send(1), Ok(()));
        assert_eq!(channel.senders.borrow().waiting.is_empty(), true);
        assert_eq!(
            channel.senders.borrow().nodes[sender.key].value.notified,
            false
        );

        assert_eq!(sender.check_send(), false);
        assert_eq!(channel.senders.borrow().waiting.is_empty(), false);
        assert_eq!(
            channel.senders.borrow().nodes[sender.key].value.notified,
            false
        );

        assert_eq!(receiver.try_recv(), Ok(1));
        assert_eq!(channel.senders.borrow().waiting.is_empty(), true);
        assert_eq!(
            channel.senders.borrow().nodes[sender.key].value.notified,
            true
        );

        assert_eq!(sender.try_send(2), Ok(()));
        assert_eq!(channel.senders.borrow().waiting.is_empty(), true);
        assert_eq!(
            channel.senders.borrow().nodes[sender.key].value.notified,
            false
        );

        assert_eq!(receiver.try_recv(), Ok(2));
    }

    #[test]
    fn test_async_send_bound0() {
        let reactor = Reactor::new(2);
        let executor = Executor::new(2);

        let (s, r) = channel::<u32>(0);

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
    fn test_async_send_bound1() {
        let reactor = Reactor::new(2);
        let executor = Executor::new(1);

        let (s, r) = channel::<u32>(1);

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
    fn test_async_recv() {
        let reactor = Reactor::new(2);
        let executor = Executor::new(2);

        let (s, r) = channel::<u32>(0);

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
    fn test_async_writable() {
        let reactor = Reactor::new(1);
        let executor = Executor::new(1);

        let (s, r) = channel::<u32>(0);

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
    fn test_async_local_channel() {
        let reactor = Reactor::new(2);
        let executor = Executor::new(2);

        let (s, r) = local_channel::<u32>(1, 1, &reactor.local_registration_memory());

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
    fn test_async_check_send_sequential() {
        // create two instances and await them sequentially

        let reactor = Reactor::new(2);
        let executor = Executor::new(2);

        let (s, r) = local_channel::<u32>(1, 1, &reactor.local_registration_memory());

        let state = Rc::new(Cell::new(0));

        {
            let state = state.clone();

            executor
                .spawn(async move {
                    let s = AsyncLocalSender::new(s);

                    // fill the queue
                    s.send(1).await.unwrap();
                    state.set(1);

                    // create two instances and await them sequentially

                    let fut1 = s.check_send();
                    let fut2 = s.check_send();

                    fut1.await;

                    s.send(2).await.unwrap();
                    state.set(2);

                    fut2.await;

                    state.set(3);
                })
                .unwrap();
        }

        reactor.poll_nonblocking(reactor.now()).unwrap();
        executor.run_until_stalled();
        assert_eq!(state.get(), 1);

        assert_eq!(r.try_recv(), Ok(1));
        assert_eq!(r.try_recv(), Err(mpsc::TryRecvError::Empty));
        reactor.poll_nonblocking(reactor.now()).unwrap();
        executor.run_until_stalled();
        assert_eq!(state.get(), 2);

        assert_eq!(r.try_recv(), Ok(2));
        assert_eq!(r.try_recv(), Err(mpsc::TryRecvError::Empty));
        reactor.poll_nonblocking(reactor.now()).unwrap();
        executor.run_until_stalled();
        assert_eq!(state.get(), 3);

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn test_async_check_send_alternating() {
        // create one instance, poll it to pending, then create a second
        // instance and poll it to completion, then poll the first again

        let reactor = Reactor::new(2);
        let executor = Executor::new(2);

        let (s, r) = local_channel::<u32>(1, 1, &reactor.local_registration_memory());

        let state = Rc::new(Cell::new(0));

        {
            let state = state.clone();

            executor
                .spawn(async move {
                    let s = AsyncLocalSender::new(s);

                    // fill the queue
                    s.send(1).await.unwrap();

                    // create one instance
                    let mut fut1 = s.check_send();

                    // poll it to pending
                    assert_eq!(poll_async(&mut fut1).await, Poll::Pending);
                    state.set(1);

                    // create a second instance and poll it to completion
                    s.check_send().await;

                    s.send(2).await.unwrap();
                    state.set(2);

                    // poll the first again
                    fut1.await;

                    state.set(3);
                })
                .unwrap();
        }

        reactor.poll_nonblocking(reactor.now()).unwrap();
        executor.run_until_stalled();
        assert_eq!(state.get(), 1);

        assert_eq!(r.try_recv(), Ok(1));
        assert_eq!(r.try_recv(), Err(mpsc::TryRecvError::Empty));
        reactor.poll_nonblocking(reactor.now()).unwrap();
        executor.run_until_stalled();
        assert_eq!(state.get(), 2);

        assert_eq!(r.try_recv(), Ok(2));
        assert_eq!(r.try_recv(), Err(mpsc::TryRecvError::Empty));
        reactor.poll_nonblocking(reactor.now()).unwrap();
        executor.run_until_stalled();
        assert_eq!(state.get(), 3);

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn test_budget_unlimited() {
        let reactor = Reactor::new(1);
        let executor = Executor::new(1);

        let (s, r) = channel::<u32>(3);

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
        let reactor = Reactor::new(1);
        let executor = Executor::new(1);

        {
            let reactor = reactor.clone();

            executor.set_pre_poll(move || {
                reactor.set_budget(Some(1));
            });
        }

        let (s, r) = channel::<u32>(3);

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
}
