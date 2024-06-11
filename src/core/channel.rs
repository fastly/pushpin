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
use slab::Slab;
use std::cell::RefCell;
use std::collections::VecDeque;
use std::mem;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::Arc;

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

#[cfg(test)]
mod tests {
    use super::*;
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
}
