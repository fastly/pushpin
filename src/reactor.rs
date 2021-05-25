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

use crate::event;
use crate::timer::TimerWheel;
use mio;
use slab::Slab;
use std::cell::RefCell;
use std::io;
use std::rc::{Rc, Weak};
use std::task::Waker;
use std::time::{Duration, Instant};

const TICK_DURATION_MS: u64 = 10;

thread_local! {
    static REACTOR: RefCell<Option<Weak<ReactorData>>> = RefCell::new(None);
}

fn duration_to_ticks(d: Duration) -> u64 {
    (d.as_millis() / (TICK_DURATION_MS as u128)) as u64
}

fn ticks_to_duration(t: u64) -> Duration {
    Duration::from_millis(t * TICK_DURATION_MS)
}

pub struct Registration {
    reactor: Weak<ReactorData>,
    key: usize,
}

impl Registration {
    pub fn is_ready(&self) -> bool {
        let reactor = self.reactor.upgrade().unwrap();
        let registrations = &*reactor.registrations.borrow();

        let reg_data = &registrations[self.key];

        reg_data.ready
    }

    pub fn set_ready(&self, ready: bool) {
        let reactor = self.reactor.upgrade().unwrap();
        let registrations = &mut *reactor.registrations.borrow_mut();

        let reg_data = &mut registrations[self.key];

        reg_data.ready = ready;
    }

    pub fn set_waker(&self, waker: Waker) {
        let reactor = self.reactor.upgrade().unwrap();
        let registrations = &mut *reactor.registrations.borrow_mut();

        let reg_data = &mut registrations[self.key];

        reg_data.waker = Some(waker);
    }

    pub fn clear_waker(&self) {
        let reactor = self.reactor.upgrade().unwrap();
        let registrations = &mut *reactor.registrations.borrow_mut();

        let reg_data = &mut registrations[self.key];

        reg_data.waker = None;
    }

    pub fn deregister_io<S: mio::event::Source>(&self, source: &mut S) -> Result<(), io::Error> {
        let reactor = self.reactor.upgrade().unwrap();
        let poll = &reactor.poll.borrow();

        poll.deregister(source)
    }
}

impl Drop for Registration {
    fn drop(&mut self) {
        if let Some(reactor) = self.reactor.upgrade() {
            let registrations = &mut *reactor.registrations.borrow_mut();

            if let Some(timer_key) = registrations[self.key].timer_key {
                let timer = &mut *reactor.timer.borrow_mut();

                timer.wheel.remove(timer_key);
            }

            registrations.remove(self.key);
        }
    }
}

struct RegistrationData {
    ready: bool,
    waker: Option<Waker>,
    timer_key: Option<usize>,
}

struct TimerData {
    wheel: TimerWheel,
    start: Instant,
}

struct ReactorData {
    registrations: RefCell<Slab<RegistrationData>>,
    poll: RefCell<event::Poller>,
    timer: RefCell<TimerData>,
}

#[derive(Clone)]
pub struct Reactor {
    inner: Rc<ReactorData>,
}

impl Reactor {
    pub fn new(registrations_max: usize) -> Self {
        Self::new_with_time(registrations_max, Instant::now())
    }

    pub fn new_with_time(registrations_max: usize, start_time: Instant) -> Self {
        let timer_data = TimerData {
            wheel: TimerWheel::new(registrations_max),
            start: start_time,
        };

        let inner = Rc::new(ReactorData {
            registrations: RefCell::new(Slab::with_capacity(registrations_max)),
            poll: RefCell::new(event::Poller::new(registrations_max).unwrap()),
            timer: RefCell::new(timer_data),
        });

        REACTOR.with(|r| {
            if r.borrow().is_some() {
                panic!("thread already has a Reactor");
            }

            r.replace(Some(Rc::downgrade(&inner)));
        });

        Self { inner }
    }

    pub fn register_io<S>(
        &self,
        source: &mut S,
        interest: mio::Interest,
    ) -> Result<Registration, io::Error>
    where
        S: mio::event::Source + ?Sized,
    {
        let registrations = &mut *self.inner.registrations.borrow_mut();

        if registrations.len() == registrations.capacity() {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        let key = registrations.insert(RegistrationData {
            ready: false,
            waker: None,
            timer_key: None,
        });

        if let Err(e) = self
            .inner
            .poll
            .borrow()
            .register(source, mio::Token(key + 1), interest)
        {
            registrations.remove(key);

            return Err(e);
        }

        Ok(Registration {
            reactor: Rc::downgrade(&self.inner),
            key,
        })
    }

    pub fn register_custom(
        &self,
        handle: &event::Registration,
        interest: mio::Interest,
    ) -> Result<Registration, io::Error> {
        let registrations = &mut *self.inner.registrations.borrow_mut();

        if registrations.len() == registrations.capacity() {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        let key = registrations.insert(RegistrationData {
            ready: false,
            waker: None,
            timer_key: None,
        });

        if let Err(e) =
            self.inner
                .poll
                .borrow()
                .register_custom(handle, mio::Token(key + 1), interest)
        {
            registrations.remove(key);

            return Err(e);
        }

        Ok(Registration {
            reactor: Rc::downgrade(&self.inner),
            key,
        })
    }

    pub fn register_timer(&self, expires: Instant) -> Result<Registration, io::Error> {
        let registrations = &mut *self.inner.registrations.borrow_mut();

        if registrations.len() == registrations.capacity() {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        let key = registrations.insert(RegistrationData {
            ready: false,
            waker: None,
            timer_key: None,
        });

        let timer = &mut *self.inner.timer.borrow_mut();

        let expires_ticks = duration_to_ticks(expires - timer.start);

        let timer_key = match timer.wheel.add(expires_ticks, key) {
            Ok(timer_key) => timer_key,
            Err(_) => {
                registrations.remove(key);

                return Err(io::Error::from(io::ErrorKind::Other));
            }
        };

        registrations[key].timer_key = Some(timer_key);

        Ok(Registration {
            reactor: Rc::downgrade(&self.inner),
            key,
        })
    }

    pub fn poll(&self) -> Result<(), io::Error> {
        self.poll_with_time(Instant::now())
    }

    pub fn poll_with_time(&self, current_time: Instant) -> Result<(), io::Error> {
        self.poll_for_events(self.advance_timers(current_time))
    }

    pub fn current() -> Option<Self> {
        REACTOR.with(|r| match &mut *r.borrow_mut() {
            Some(inner) => Some(Self {
                inner: inner.upgrade().unwrap(),
            }),
            None => None,
        })
    }

    fn advance_timers(&self, current_time: Instant) -> Option<Duration> {
        let timer = &mut *self.inner.timer.borrow_mut();

        let current_ticks = duration_to_ticks(current_time - timer.start);

        timer.wheel.update(current_ticks);

        match timer.wheel.timeout() {
            Some(ticks) => Some(ticks_to_duration(ticks)),
            None => None,
        }
    }

    fn poll_for_events(&self, timeout: Option<Duration>) -> Result<(), io::Error> {
        let poll = &mut *self.inner.poll.borrow_mut();

        poll.poll(timeout)?;

        let registrations = &mut *self.inner.registrations.borrow_mut();

        for event in poll.iter_events() {
            let key = usize::from(event.token());

            assert!(key > 0);

            let key = key - 1;

            if let Some(event_reg) = registrations.get_mut(key) {
                event_reg.ready = true;

                if let Some(waker) = event_reg.waker.take() {
                    waker.wake();
                }
            }
        }

        let timer = &mut *self.inner.timer.borrow_mut();

        while let Some((_, key)) = timer.wheel.take_expired() {
            if let Some(event_reg) = registrations.get_mut(key) {
                event_reg.ready = true;
                event_reg.timer_key = None;

                if let Some(waker) = event_reg.waker.take() {
                    waker.wake();
                }
            }
        }

        Ok(())
    }
}

impl Drop for Reactor {
    fn drop(&mut self) {
        REACTOR.with(|r| {
            if Rc::strong_count(&self.inner) == 1 {
                r.replace(None);
            }
        });
    }
}

pub struct IoEvented<S: mio::event::Source> {
    registration: Registration,
    io: Option<S>,
}

impl<S: mio::event::Source> IoEvented<S> {
    pub fn new(mut io: S, interest: mio::Interest, reactor: &Reactor) -> Result<Self, io::Error> {
        let registration = reactor.register_io(&mut io, interest)?;

        Ok(Self {
            registration,
            io: Some(io),
        })
    }

    pub fn registration(&self) -> &Registration {
        &self.registration
    }

    pub fn io(&self) -> &S {
        &self.io.as_ref().unwrap()
    }
}

impl<S: mio::event::Source> Drop for IoEvented<S> {
    fn drop(&mut self) {
        if let Some(mut io) = self.io.take() {
            self.registration().deregister_io(&mut io).unwrap();
        }
    }
}

pub struct CustomEvented {
    registration: Registration,
}

impl CustomEvented {
    pub fn new(
        event_reg: &event::Registration,
        interest: mio::Interest,
        reactor: &Reactor,
    ) -> Result<Self, io::Error> {
        let registration = reactor.register_custom(event_reg, interest)?;

        Ok(Self { registration })
    }

    pub fn registration(&self) -> &Registration {
        &self.registration
    }
}

pub struct TimerEvented {
    registration: Registration,
}

impl TimerEvented {
    pub fn new(expires: Instant, reactor: &Reactor) -> Result<Self, io::Error> {
        let registration = reactor.register_timer(expires)?;

        Ok(Self { registration })
    }

    pub fn registration(&self) -> &Registration {
        &self.registration
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::waker;
    use std::cell::Cell;
    use std::mem;
    use std::rc::Rc;
    use std::thread;

    struct TestWaker {
        waked: Cell<bool>,
    }

    impl TestWaker {
        fn new() -> Self {
            Self {
                waked: Cell::new(false),
            }
        }

        fn into_std(self: Rc<TestWaker>) -> Waker {
            waker::into_std(self)
        }

        fn was_waked(&self) -> bool {
            self.waked.get()
        }
    }

    impl waker::RcWake for TestWaker {
        fn wake(self: Rc<Self>) {
            self.waked.set(true);
        }
    }

    #[test]
    fn test_reactor_io() {
        let reactor = Reactor::new(1);

        let addr = "127.0.0.1:0".parse().unwrap();
        let listener = mio::net::TcpListener::bind(addr).unwrap();

        let evented = IoEvented::new(listener, mio::Interest::READABLE, &reactor).unwrap();

        let addr = evented.io().local_addr().unwrap();

        let waker = Rc::new(TestWaker::new());

        evented.registration().set_waker(waker.clone().into_std());

        let thread = thread::spawn(move || {
            std::net::TcpStream::connect(addr).unwrap();
        });

        assert_eq!(waker.was_waked(), false);

        reactor.poll().unwrap();

        assert_eq!(waker.was_waked(), true);

        thread.join().unwrap();
    }

    #[test]
    fn test_reactor_current() {
        assert!(Reactor::current().is_none());

        let reactor = Reactor::new(1);

        let current = Reactor::current().unwrap();

        mem::drop(reactor);

        assert!(Reactor::current().is_some());

        mem::drop(current);

        assert!(Reactor::current().is_none());
    }

    #[test]
    fn test_reactor_custom() {
        let reactor = Reactor::new(1);

        let (reg, sr) = event::Registration::new();

        let evented = CustomEvented::new(&reg, mio::Interest::READABLE, &reactor).unwrap();

        let waker = Rc::new(TestWaker::new());

        evented.registration().set_waker(waker.clone().into_std());

        let thread = thread::spawn(move || {
            sr.set_readiness(mio::Interest::READABLE).unwrap();
        });

        assert_eq!(waker.was_waked(), false);

        reactor.poll().unwrap();

        assert_eq!(waker.was_waked(), true);

        thread.join().unwrap();
    }

    #[test]
    fn test_reactor_timer() {
        let now = Instant::now();

        let reactor = Reactor::new_with_time(1, now);

        let evented = TimerEvented::new(now + Duration::from_millis(100), &reactor).unwrap();

        let waker = Rc::new(TestWaker::new());

        evented.registration().set_waker(waker.clone().into_std());

        assert_eq!(waker.was_waked(), false);

        let timeout = reactor
            .advance_timers(now + Duration::from_millis(20))
            .unwrap();
        reactor
            .poll_for_events(Some(Duration::from_millis(0)))
            .unwrap();

        assert_eq!(Some(timeout), Some(Duration::from_millis(80)));
        assert_eq!(waker.was_waked(), false);

        reactor
            .poll_with_time(now + Duration::from_millis(100))
            .unwrap();

        assert_eq!(waker.was_waked(), true);
    }
}
