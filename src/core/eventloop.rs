/*
 * Copyright (C) 2025 Fastly, Inc.
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

use crate::core::list;
use crate::core::reactor;
use crate::core::waker;
use slab::Slab;
use std::cell::{Cell, RefCell};
use std::os::fd::RawFd;
use std::rc::{Rc, Weak};
use std::task::Waker;
use std::time::Duration;

pub const READABLE: u8 = 0x01;
pub const WRITABLE: u8 = 0x02;

pub trait Callback {
    fn call(&mut self);
}

impl Callback for Box<dyn Callback> {
    fn call(&mut self) {
        (**self).call();
    }
}

pub struct FnCallback<T>(T);

impl<T: FnMut()> Callback for FnCallback<T> {
    fn call(&mut self) {
        self.0();
    }
}

enum Evented {
    Fd(reactor::FdEvented),
    Timer(reactor::TimerEvented),
}

impl Evented {
    fn registration(&self) -> &reactor::Registration {
        match self {
            Self::Fd(e) => e.registration(),
            Self::Timer(e) => e.registration(),
        }
    }
}

struct Registration<C> {
    _evented: Evented,
    activated: bool,
    callback: Option<C>,
}

struct RegistrationsData<C> {
    nodes: Slab<list::Node<Registration<C>>>,
    activated: list::List,
}

#[derive(Debug)]
struct RegistrationsError;

struct Registrations<C> {
    data: RefCell<RegistrationsData<C>>,
}

impl<C: Callback> Registrations<C> {
    fn new(capacity: usize) -> Self {
        Self {
            data: RefCell::new(RegistrationsData {
                nodes: Slab::with_capacity(capacity),
                activated: list::List::default(),
            }),
        }
    }

    fn add<W>(
        &self,
        evented: Evented,
        interest: mio::Interest,
        get_waker: W,
        callback: C,
    ) -> Result<usize, RegistrationsError>
    where
        W: FnOnce(usize) -> Waker,
        C: Callback,
    {
        let data = &mut *self.data.borrow_mut();

        if data.nodes.len() == data.nodes.capacity() {
            return Err(RegistrationsError);
        }

        let entry = data.nodes.vacant_entry();
        let nkey = entry.key();

        evented.registration().set_waker(&get_waker(nkey), interest);

        let reg = Registration {
            _evented: evented,
            activated: false,
            callback: Some(callback),
        };

        entry.insert(list::Node::new(reg));

        Ok(nkey)
    }

    fn remove(&self, reg_id: usize) {
        let nkey = reg_id;

        let data = &mut *self.data.borrow_mut();

        data.activated.remove(&mut data.nodes, nkey);
        data.nodes.remove(nkey);
    }

    fn activate(&self, reg_id: usize) {
        let nkey = reg_id;

        let data = &mut *self.data.borrow_mut();

        let reg = &mut data.nodes[nkey].value;

        if reg.activated {
            return;
        }

        reg.activated = true;

        data.activated.push_back(&mut data.nodes, nkey);
    }

    fn dispatch_activated(&self) {
        // move the current list aside so we only process registrations that
        // have been activated up to this point, and not registrations that
        // might get activated during the course of calling callbacks
        let mut activated = {
            let data = &mut *self.data.borrow_mut();

            let mut l = list::List::default();
            l.concat(&mut data.nodes, &mut data.activated);

            l
        };

        // call the callback of each activated registration, ensuring we
        // release borrows before each call. this way, callbacks can access
        // the eventloop, for example to add registrations
        loop {
            let (nkey, mut callback) = {
                let data = &mut *self.data.borrow_mut();

                let nkey = match activated.pop_front(&mut data.nodes) {
                    Some(nkey) => nkey,
                    None => break,
                };

                let reg = &mut data.nodes[nkey].value;
                reg.activated = false;

                let callback = reg
                    .callback
                    .take()
                    .expect("registration should have a callback");

                (nkey, callback)
            };

            callback.call();

            let data = &mut *self.data.borrow_mut();

            let reg = &mut data.nodes[nkey].value;

            reg.callback = Some(callback);
        }
    }
}

struct Activator<C> {
    regs: Weak<Registrations<C>>,
    reg_id: usize,
}

impl<C: Callback> waker::RcWake for Activator<C> {
    fn wake(self: Rc<Self>) {
        if let Some(regs) = self.regs.upgrade() {
            regs.activate(self.reg_id);
        }
    }
}

#[derive(Debug)]
pub struct EventLoopError;

pub struct EventLoop<C> {
    reactor: reactor::Reactor,
    exit_code: Cell<Option<i32>>,
    regs: Rc<Registrations<C>>,
}

impl<C: Callback> EventLoop<C> {
    pub fn new(registrations_max: usize) -> Self {
        Self {
            reactor: reactor::Reactor::new(registrations_max),
            exit_code: Cell::new(None),
            regs: Rc::new(Registrations::new(registrations_max)),
        }
    }

    pub fn step(&self) -> Option<i32> {
        self.poll_and_dispatch(Some(Duration::from_millis(0)))
    }

    pub fn exec(&self) -> i32 {
        loop {
            if let Some(code) = self.poll_and_dispatch(None) {
                break code;
            }
        }
    }

    pub fn exit(&self, code: i32) {
        self.exit_code.set(Some(code));
    }

    // SAFETY: `cb` must be safe to call with the provided `ctx` until the
    // registration is removed with `deregister` or the `EventLoop` is
    // dropped.
    pub fn register_fd(
        &self,
        fd: RawFd,
        interest: u8,
        callback: C,
    ) -> Result<usize, EventLoopError> {
        let interest = if interest & READABLE != 0 && interest & WRITABLE != 0 {
            mio::Interest::READABLE | mio::Interest::WRITABLE
        } else if interest & READABLE != 0 {
            mio::Interest::READABLE
        } else if interest & WRITABLE != 0 {
            mio::Interest::WRITABLE
        } else {
            // must specify at least one of READABLE or WRITABLE
            return Err(EventLoopError);
        };

        let evented = match reactor::FdEvented::new(fd, interest, &self.reactor) {
            Ok(evented) => evented,
            Err(_) => return Err(EventLoopError),
        };

        let regs = Rc::downgrade(&self.regs);

        let get_waker = |reg_id| {
            let activator = Rc::new(Activator { regs, reg_id });

            waker::into_std(activator)
        };

        Ok(self
            .regs
            .add(Evented::Fd(evented), interest, get_waker, callback)
            .expect("slab should have capacity"))
    }

    // SAFETY: `cb` must be safe to call with the provided `ctx` until the
    // registration is removed with `deregister` or the `EventLoop` is
    // dropped.
    pub fn register_timer(&self, timeout: Duration, callback: C) -> Result<usize, EventLoopError> {
        let expires = self.reactor.now() + timeout;

        let evented = match reactor::TimerEvented::new(expires, &self.reactor) {
            Ok(evented) => evented,
            Err(_) => return Err(EventLoopError),
        };

        let regs = Rc::downgrade(&self.regs);

        let get_waker = |reg_id| {
            let activator = Rc::new(Activator { regs, reg_id });

            waker::into_std(activator)
        };

        Ok(self
            .regs
            .add(
                Evented::Timer(evented),
                mio::Interest::READABLE,
                get_waker,
                callback,
            )
            .expect("slab should have capacity"))
    }

    pub fn deregister(&self, id: usize) {
        self.regs.remove(id);
    }

    fn poll_and_dispatch(&self, timeout: Option<Duration>) -> Option<i32> {
        // if exit code set, do a non-blocking poll
        let timeout = if self.exit_code.get().is_some() {
            Some(Duration::from_millis(0))
        } else {
            timeout
        };

        self.reactor.poll(timeout).unwrap();
        self.regs.dispatch_activated();

        self.exit_code.get()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::fd::AsRawFd;
    use std::rc::Rc;

    struct NoopCallback;

    impl Callback for NoopCallback {
        fn call(&mut self) {}
    }

    #[test]
    fn exec() {
        {
            let l = EventLoop::<NoopCallback>::new(1);
            assert_eq!(l.step(), None);

            l.exit(123);
            assert_eq!(l.step(), Some(123));
        }

        {
            let l = EventLoop::<NoopCallback>::new(1);
            l.exit(124);
            assert_eq!(l.exec(), 124);
        }
    }

    #[test]
    fn fd() {
        let l = Rc::new(EventLoop::<Box<dyn Callback>>::new(1));

        let listener = Rc::new(std::net::TcpListener::bind("127.0.0.1:0").unwrap());
        listener.set_nonblocking(true).unwrap();

        let addr = listener.local_addr().unwrap();
        let fd = listener.as_raw_fd();

        let cb = {
            let l = Rc::clone(&l);
            let listener = Rc::clone(&listener);

            Box::new(FnCallback(move || {
                let _stream = listener.accept().unwrap();
                l.exit(0);
            }))
        };

        let id = l.register_fd(fd, READABLE, cb).unwrap();

        // non-blocking connect attempt to trigger listener
        let _stream = mio::net::TcpStream::connect(addr);

        assert_eq!(l.exec(), 0);

        l.deregister(id);
    }

    #[test]
    fn timer() {
        let l = Rc::new(EventLoop::<Box<dyn Callback>>::new(1));

        let cb = {
            let l = Rc::clone(&l);

            Box::new(FnCallback(move || l.exit(0)))
        };

        let id = l.register_timer(Duration::from_millis(0), cb).unwrap();

        // no space
        assert!(l
            .register_timer(Duration::from_millis(0), Box::new(NoopCallback))
            .is_err());

        assert_eq!(l.exec(), 0);

        l.deregister(id);

        assert!(l
            .register_timer(Duration::from_millis(0), Box::new(NoopCallback))
            .is_ok());
    }
}
