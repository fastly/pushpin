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

use crate::core::event::{self, ReadinessExt};
use crate::core::list;
use crate::core::reactor;
use crate::core::waker;
use slab::Slab;
use std::cell::{Cell, RefCell};
use std::future::Future;
use std::os::fd::RawFd;
use std::pin::Pin;
use std::rc::{Rc, Weak};
use std::task::{Context, Poll, Waker};
use std::time::Duration;

pub trait Callback {
    fn call(&mut self, readiness: event::Readiness);
}

impl Callback for Box<dyn Callback> {
    fn call(&mut self, readiness: event::Readiness) {
        (**self).call(readiness);
    }
}

pub struct FnCallback<T>(T);

impl<T: FnMut(event::Readiness)> Callback for FnCallback<T> {
    fn call(&mut self, readiness: event::Readiness) {
        self.0(readiness);
    }
}

enum Evented {
    Fd(reactor::FdEvented),
    Timer(reactor::TimerEvented),
    Custom {
        evented: reactor::CustomEvented,
        _reg: event::Registration,
    },
}

impl Evented {
    fn registration(&self) -> &reactor::Registration {
        match self {
            Self::Fd(e) => e.registration(),
            Self::Timer(e) => e.registration(),
            Self::Custom { evented, .. } => evented.registration(),
        }
    }
}

struct Registration<C> {
    evented: Evented,
    activated: bool,
    callback: Option<C>,
}

struct RegistrationsData<C> {
    nodes: Slab<list::Node<Registration<C>>>,
    activated: list::List,
    waker: Option<Waker>,
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
                waker: None,
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

        evented.registration().set_waker_persistent(true);
        evented.registration().set_waker(&get_waker(nkey), interest);

        let reg = Registration {
            evented,
            activated: false,
            callback: Some(callback),
        };

        entry.insert(list::Node::new(reg));

        Ok(nkey)
    }

    fn remove(&self, reg_id: usize) -> Result<(), RegistrationsError> {
        let nkey = reg_id;

        let data = &mut *self.data.borrow_mut();

        if !data.nodes.contains(nkey) {
            return Err(RegistrationsError);
        }

        data.activated.remove(&mut data.nodes, nkey);
        data.nodes.remove(nkey);

        Ok(())
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

        if let Some(waker) = data.waker.take() {
            waker.wake();
        }
    }

    fn dispatch_activated(&self) {
        // call the callback of each activated registration, ensuring we
        // release borrows before each call. this way, callbacks can access
        // the eventloop, for example to add or remove registrations
        loop {
            let (nkey, mut callback, readiness) = {
                let data = &mut *self.data.borrow_mut();

                let nkey = match data.activated.pop_front(&mut data.nodes) {
                    Some(nkey) => nkey,
                    None => break,
                };

                let reg = &mut data.nodes[nkey].value;

                let callback = reg
                    .callback
                    .take()
                    .expect("registration should have a callback");

                let readiness = reg.evented.registration().readiness();

                let nkey = if let Evented::Timer(_) = &reg.evented {
                    // remove timer registrations after activation
                    data.nodes.remove(nkey);

                    None
                } else {
                    reg.activated = false;
                    reg.evented
                        .registration()
                        .clear_readiness(mio::Interest::READABLE | mio::Interest::WRITABLE);

                    Some(nkey)
                };

                (nkey, callback, readiness)
            };

            callback.call(readiness);

            if let Some(nkey) = nkey {
                let data = &mut *self.data.borrow_mut();

                // if the registration still exists, restore its callback
                if let Some(n) = &mut data.nodes.get_mut(nkey) {
                    let reg = &mut n.value;

                    // only set the callback field on the registration if
                    // it's the same registration we took the callback from
                    // and not a new registration that happened to reuse the
                    // same slot. if the callback field is none, then it's
                    // the same registration.
                    if reg.callback.is_none() {
                        reg.callback = Some(callback);
                    }
                }
            }
        }
    }

    fn set_waker(&self, waker: &Waker) {
        let data = &mut *self.data.borrow_mut();

        if let Some(current_waker) = &data.waker {
            if !waker.will_wake(current_waker) {
                // replace
                data.waker = Some(waker.clone());
            }
        } else {
            // set
            data.waker = Some(waker.clone());
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
    // will create a reactor if one does not exist in the current thread. if
    // one already exists, registrations_max should be <= the max configured
    // in the reactor.
    pub fn new(registrations_max: usize) -> Self {
        let reactor = if let Some(reactor) = reactor::Reactor::current() {
            // use existing reactor if available
            reactor
        } else {
            reactor::Reactor::new(registrations_max)
        };

        Self {
            reactor,
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

    pub fn exec_async(&self) -> Exec<C> {
        Exec { l: self }
    }

    pub fn exit(&self, code: i32) {
        self.exit_code.set(Some(code));
    }

    pub fn register_fd(
        &self,
        fd: RawFd,
        interest: mio::Interest,
        callback: C,
    ) -> Result<usize, EventLoopError> {
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

    pub fn register_custom(
        &self,
        callback: C,
    ) -> Result<(usize, event::SetReadiness), EventLoopError> {
        let (reg, sr) = event::Registration::new();

        let evented = match reactor::CustomEvented::new(
            &reg,
            mio::Interest::READABLE | mio::Interest::WRITABLE,
            &self.reactor,
        ) {
            Ok(evented) => evented,
            Err(_) => return Err(EventLoopError),
        };

        let regs = Rc::downgrade(&self.regs);

        let get_waker = |reg_id| {
            let activator = Rc::new(Activator { regs, reg_id });

            waker::into_std(activator)
        };

        let id = self
            .regs
            .add(
                Evented::Custom { evented, _reg: reg },
                mio::Interest::READABLE | mio::Interest::WRITABLE,
                get_waker,
                callback,
            )
            .expect("slab should have capacity");

        Ok((id, sr))
    }

    pub fn deregister(&self, id: usize) -> Result<(), EventLoopError> {
        self.regs.remove(id).map_err(|_| EventLoopError)
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

pub struct Exec<'a, C> {
    l: &'a EventLoop<C>,
}

impl<C: Callback> Future for Exec<'_, C> {
    type Output = i32;

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let l = self.l;

        l.regs.dispatch_activated();

        if let Some(code) = l.exit_code.get() {
            return Poll::Ready(code);
        }

        l.regs.set_waker(cx.waker());

        Poll::Pending
    }
}

mod ffi {
    use super::*;
    use event::ffi::{interest_int_to_mio, READABLE, WRITABLE};
    use std::ops::Deref;

    pub struct RawCallback {
        // SAFETY: must be called with the associated ctx value
        f: unsafe extern "C" fn(*mut libc::c_void, u8),

        ctx: *mut libc::c_void,
    }

    impl RawCallback {
        // SAFETY: caller must ensure f is safe to call for the lifetime
        // of the registration
        pub unsafe fn new(
            f: unsafe extern "C" fn(*mut libc::c_void, u8),
            ctx: *mut libc::c_void,
        ) -> Self {
            Self { f, ctx }
        }
    }

    impl Callback for RawCallback {
        fn call(&mut self, readiness: event::Readiness) {
            let readiness = {
                let mut r = 0;

                if readiness.contains_any(mio::Interest::READABLE) {
                    r |= READABLE;
                }

                if readiness.contains_any(mio::Interest::WRITABLE) {
                    r |= WRITABLE;
                }

                r
            };

            // SAFETY: we are passing the ctx value that was provided
            unsafe {
                (self.f)(self.ctx, readiness);
            }
        }
    }

    pub struct EventLoopRaw(EventLoop<RawCallback>);

    impl Deref for EventLoopRaw {
        type Target = EventLoop<RawCallback>;

        fn deref(&self) -> &Self::Target {
            &self.0
        }
    }

    #[no_mangle]
    pub extern "C" fn event_loop_create(capacity: libc::c_uint) -> *mut EventLoopRaw {
        let l = EventLoopRaw(EventLoop::new(capacity as usize));

        Box::into_raw(Box::new(l))
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn event_loop_destroy(l: *mut EventLoopRaw) {
        if !l.is_null() {
            drop(Box::from_raw(l));
        }
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn event_loop_step(
        l: *mut EventLoopRaw,
        out_code: *mut libc::c_int,
    ) -> libc::c_int {
        let l = l.as_mut().unwrap();

        match l.step() {
            Some(code) => {
                unsafe { out_code.write(code) };

                0
            }
            None => -1,
        }
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn event_loop_exec(l: *mut EventLoopRaw) -> libc::c_int {
        let l = l.as_mut().unwrap();

        l.exec() as libc::c_int
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn event_loop_exit(l: *mut EventLoopRaw, code: libc::c_int) {
        let l = l.as_mut().unwrap();

        l.exit(code);
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn event_loop_register_fd(
        l: *mut EventLoopRaw,
        fd: std::os::raw::c_int,
        interest: u8,
        cb: unsafe extern "C" fn(*mut libc::c_void, u8),
        ctx: *mut libc::c_void,
        out_id: *mut libc::size_t,
    ) -> libc::c_int {
        let l = l.as_mut().unwrap();

        let Ok(interest) = interest_int_to_mio(interest) else {
            return -1;
        };

        // SAFETY: we assume caller guarantees that the callback is safe to
        // call for the lifetime of the registration
        let cb = unsafe { RawCallback::new(cb, ctx) };

        let id = match l.register_fd(fd, interest, cb) {
            Ok(id) => id,
            Err(_) => return -1,
        };

        out_id.write(id);

        0
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn event_loop_register_timer(
        l: *mut EventLoopRaw,
        timeout: u64,
        cb: unsafe extern "C" fn(*mut libc::c_void, u8),
        ctx: *mut libc::c_void,
        out_id: *mut libc::size_t,
    ) -> libc::c_int {
        let l = l.as_mut().unwrap();

        // SAFETY: we assume caller guarantees that the callback is safe to
        // call for the lifetime of the registration
        let cb = unsafe { RawCallback::new(cb, ctx) };

        let id = match l.register_timer(Duration::from_millis(timeout), cb) {
            Ok(id) => id,
            Err(_) => return -1,
        };

        out_id.write(id);

        0
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn event_loop_register_custom(
        l: *mut EventLoopRaw,
        cb: unsafe extern "C" fn(*mut libc::c_void, u8),
        ctx: *mut libc::c_void,
        out_id: *mut libc::size_t,
        out_set_readiness: *mut *mut event::ffi::SetReadiness,
    ) -> libc::c_int {
        let l = l.as_mut().unwrap();

        // SAFETY: we assume caller guarantees that the callback is safe to
        // call for the lifetime of the registration
        let cb = unsafe { RawCallback::new(cb, ctx) };

        let (id, sr) = match l.register_custom(cb) {
            Ok(id) => id,
            Err(_) => return -1,
        };

        out_id.write(id);
        out_set_readiness.write(Box::into_raw(Box::new(event::ffi::SetReadiness(sr))));

        0
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn event_loop_deregister(
        l: *mut EventLoopRaw,
        id: libc::size_t,
    ) -> libc::c_int {
        let l = l.as_mut().unwrap();

        if l.deregister(id).is_err() {
            return -1;
        }

        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::executor::Executor;
    use crate::core::reactor::Reactor;
    use std::cell::Cell;
    use std::io;
    use std::os::fd::AsRawFd;
    use std::rc::Rc;
    use std::thread;

    struct NoopCallback;

    impl Callback for NoopCallback {
        fn call(&mut self, _readiness: event::Readiness) {}
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

        let count = Rc::new(Cell::new(0));

        let cb = {
            let l = Rc::clone(&l);
            let listener = Rc::clone(&listener);
            let count = Rc::clone(&count);

            Box::new(FnCallback(move |readiness: event::Readiness| {
                assert!(readiness.contains_any(mio::Interest::READABLE));

                let _stream = listener.accept().unwrap();

                let e = listener.accept().unwrap_err();
                assert_eq!(e.kind(), io::ErrorKind::WouldBlock);

                count.set(count.get() + 1);
                if count.get() == 2 {
                    l.exit(0);
                }
            }))
        };

        let id = l.register_fd(fd, mio::Interest::READABLE, cb).unwrap();

        {
            // non-blocking connect attempt to trigger listener
            let _stream = mio::net::TcpStream::connect(addr);

            while count.get() < 1 {
                l.step();
                thread::sleep(Duration::from_millis(10));
            }
        }

        {
            // non-blocking connect attempt to trigger listener
            let _stream = mio::net::TcpStream::connect(addr);

            while count.get() < 2 {
                l.step();
                thread::sleep(Duration::from_millis(10));
            }
        }

        assert_eq!(l.exec(), 0);

        l.deregister(id).unwrap();
    }

    #[test]
    fn timer() {
        let l = Rc::new(EventLoop::<Box<dyn Callback>>::new(1));

        let cb = {
            let l = Rc::clone(&l);

            Box::new(FnCallback(move |readiness: event::Readiness| {
                assert!(readiness.contains_any(mio::Interest::READABLE));

                l.exit(0);
            }))
        };

        let id = l.register_timer(Duration::from_millis(0), cb).unwrap();

        // no space
        assert!(l
            .register_timer(Duration::from_millis(0), Box::new(NoopCallback))
            .is_err());

        assert_eq!(l.exec(), 0);

        // activated timers automatically deregister
        l.deregister(id).unwrap_err();

        let id = l
            .register_timer(Duration::from_millis(0), Box::new(NoopCallback))
            .unwrap();

        l.deregister(id).unwrap();
    }

    #[test]
    fn custom() {
        let l = Rc::new(EventLoop::<Box<dyn Callback>>::new(1));

        let cb = {
            let l = Rc::clone(&l);

            Box::new(FnCallback(move |readiness: event::Readiness| {
                assert!(readiness.contains_any(mio::Interest::READABLE));

                l.exit(0);
            }))
        };

        let (id, sr) = l.register_custom(cb).unwrap();

        sr.set_readiness(mio::Interest::READABLE).unwrap();
        assert_eq!(l.exec(), 0);

        l.deregister(id).unwrap();
    }

    #[test]
    fn deregister_within_callback() {
        let l = Rc::new(EventLoop::<Box<dyn Callback>>::new(1));

        let listener = Rc::new(std::net::TcpListener::bind("127.0.0.1:0").unwrap());
        listener.set_nonblocking(true).unwrap();

        let addr = listener.local_addr().unwrap();
        let fd = listener.as_raw_fd();

        let id = Rc::new(Cell::new(None));

        let cb = {
            let l = Rc::clone(&l);
            let listener = Rc::clone(&listener);
            let id = Rc::clone(&id);

            Box::new(FnCallback(move |readiness: event::Readiness| {
                assert!(readiness.contains_any(mio::Interest::READABLE));

                let _stream = listener.accept().unwrap();

                let e = listener.accept().unwrap_err();
                assert_eq!(e.kind(), io::ErrorKind::WouldBlock);

                // this is allowed
                l.deregister(id.get().unwrap()).unwrap();

                l.exit(0);
            }))
        };

        id.set(Some(
            l.register_fd(fd, mio::Interest::READABLE, cb).unwrap(),
        ));

        // non-blocking connect attempt to trigger listener
        let _stream = mio::net::TcpStream::connect(addr);

        assert_eq!(l.exec(), 0);
    }

    #[test]
    fn exec_async() {
        let reactor = Reactor::new(1);
        let executor = Executor::new(1);

        executor
            .spawn(async {
                let l = Rc::new(EventLoop::<Box<dyn Callback>>::new(1));

                let listener = Rc::new(std::net::TcpListener::bind("127.0.0.1:0").unwrap());
                listener.set_nonblocking(true).unwrap();

                let addr = listener.local_addr().unwrap();
                let fd = listener.as_raw_fd();

                let cb = {
                    let l = Rc::clone(&l);
                    let listener = Rc::clone(&listener);

                    Box::new(FnCallback(move |readiness: event::Readiness| {
                        assert!(readiness.contains_any(mio::Interest::READABLE));

                        let _stream = listener.accept().unwrap();
                        l.exit(0);
                    }))
                };

                let id = l.register_fd(fd, mio::Interest::READABLE, cb).unwrap();

                // non-blocking connect attempt to trigger listener
                let _stream = mio::net::TcpStream::connect(addr);

                assert_eq!(l.exec_async().await, 0);

                l.deregister(id).unwrap();
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }
}
