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
use crate::core::event::ReadinessExt;
use crate::core::timer::TimerWheel;
use slab::Slab;
use std::cell::{Cell, RefCell};
use std::cmp;
use std::io;
use std::os::unix::io::RawFd;
use std::rc::{Rc, Weak};
use std::task::Waker;
use std::time::{Duration, Instant};

const TICK_DURATION_MS: u64 = 10;
const EXPIRE_MAX: usize = 100;

thread_local! {
    static REACTOR: RefCell<Option<Weak<ReactorData>>> = const { RefCell::new(None) };
}

fn duration_to_ticks_round_down(d: Duration) -> u64 {
    (d.as_millis() / (TICK_DURATION_MS as u128)) as u64
}

fn duration_to_ticks_round_up(d: Duration) -> u64 {
    ((d.as_millis() + (TICK_DURATION_MS as u128) - 1) / (TICK_DURATION_MS as u128)) as u64
}

fn ticks_to_duration(t: u64) -> Duration {
    Duration::from_millis(t * TICK_DURATION_MS)
}

enum WakerInterest {
    Single(Waker, mio::Interest),
    Separate(Waker, Waker),
}

impl WakerInterest {
    fn interest(&self) -> mio::Interest {
        match self {
            Self::Single(_, interest) => *interest,
            Self::Separate(_, _) => mio::Interest::READABLE | mio::Interest::WRITABLE,
        }
    }

    fn change(self, waker: &Waker, interest: mio::Interest) -> Self {
        match self {
            Self::Single(current_waker, current_interest) => {
                if (interest.is_readable() && interest.is_writable())
                    || current_interest == interest
                {
                    // all interest or interest unchanged. stay using a
                    // single waker

                    let waker = if current_waker.will_wake(waker) {
                        // keep the current waker
                        current_waker
                    } else {
                        // switch to the new waker
                        waker.clone()
                    };

                    Self::Single(waker, interest)
                } else {
                    assert!(interest.is_readable() != interest.is_writable());

                    // one interest was specified when we had at least the
                    // opposite interest. switch to separate

                    match (interest.is_readable(), interest.is_writable()) {
                        (true, false) => Self::Separate(waker.clone(), current_waker),
                        (false, true) => Self::Separate(current_waker, waker.clone()),
                        _ => unreachable!(),
                    }
                }
            }
            Self::Separate(read_waker, write_waker) => {
                match (interest.is_readable(), interest.is_writable()) {
                    (true, true) => {
                        // if multiple interests on one waker, switch to single

                        let waker = if read_waker.will_wake(waker) {
                            read_waker
                        } else if write_waker.will_wake(waker) {
                            write_waker
                        } else {
                            waker.clone()
                        };

                        Self::Single(waker, interest)
                    }
                    (true, false) => {
                        let read_waker = if read_waker.will_wake(waker) {
                            // keep the current waker
                            read_waker
                        } else {
                            // switch to the new waker
                            waker.clone()
                        };

                        Self::Separate(read_waker, write_waker)
                    }
                    (false, true) => {
                        let write_waker = if write_waker.will_wake(waker) {
                            // keep the current waker
                            write_waker
                        } else {
                            // switch to the new waker
                            waker.clone()
                        };

                        Self::Separate(read_waker, write_waker)
                    }
                    (false, false) => unreachable!(), // interest always has a value
                }
            }
        }
    }

    fn merge(self, other: Self) -> Self {
        match self {
            Self::Single(waker, interest) => {
                if (interest.is_readable() && interest.is_writable())
                    || interest == other.interest()
                {
                    // there is already a single waker of both interests or
                    // of one interest that is the same interest as the
                    // other. leave alone
                    Self::Single(waker, interest)
                } else {
                    assert!(interest.is_readable() != interest.is_writable());

                    // there is a single waker of one interest, and the other
                    // has at least the opposite interest. switch to separate

                    match (interest.is_readable(), interest.is_writable()) {
                        (true, false) => {
                            let other_waker = match other {
                                Self::Single(waker, _) => waker,
                                Self::Separate(_, waker) => waker,
                            };

                            Self::Separate(waker, other_waker)
                        }
                        (false, true) => {
                            let other_waker = match other {
                                Self::Single(waker, _) => waker,
                                Self::Separate(waker, _) => waker,
                            };

                            Self::Separate(other_waker, waker)
                        }
                        _ => unreachable!(),
                    }
                }
            }
            separate => {
                // there are already separate wakers for both interests.
                // leave alone
                separate
            }
        }
    }

    fn clear_interest(self, interest: mio::Interest) -> Option<Self> {
        match self {
            Self::Single(waker, cur) => cur.remove(interest).map(|i| Self::Single(waker, i)),
            Self::Separate(read_waker, write_waker) => {
                match (interest.is_readable(), interest.is_writable()) {
                    (true, true) => None, // clear all
                    (true, false) => Some(Self::Single(write_waker, mio::Interest::WRITABLE)),
                    (false, true) => Some(Self::Single(read_waker, mio::Interest::READABLE)),
                    (false, false) => unreachable!(), // interest always has a value
                }
            }
        }
    }

    fn wake(self, readiness: mio::Interest) -> Option<Self> {
        match self {
            Self::Single(waker, interest) => {
                if (interest.is_readable() && readiness.is_readable())
                    || (interest.is_writable() && readiness.is_writable())
                {
                    waker.wake();

                    None
                } else {
                    Some(Self::Single(waker, interest))
                }
            }
            Self::Separate(read_waker, write_waker) => {
                match (readiness.is_readable(), readiness.is_writable()) {
                    (true, true) => {
                        read_waker.wake();
                        write_waker.wake();

                        None
                    }
                    (true, false) => {
                        read_waker.wake();

                        Some(Self::Single(write_waker, mio::Interest::WRITABLE))
                    }
                    (false, true) => {
                        write_waker.wake();

                        Some(Self::Single(read_waker, mio::Interest::READABLE))
                    }
                    (false, false) => unreachable!(), // interest always has a value
                }
            }
        }
    }

    fn wake_by_ref(&self, readiness: mio::Interest) {
        match self {
            Self::Single(waker, interest) => {
                if (interest.is_readable() && readiness.is_readable())
                    || (interest.is_writable() && readiness.is_writable())
                {
                    waker.wake_by_ref();
                }
            }
            Self::Separate(read_waker, write_waker) => {
                if readiness.is_readable() {
                    read_waker.wake_by_ref();
                }

                if readiness.is_writable() {
                    write_waker.wake_by_ref();
                }
            }
        }
    }
}

pub struct Registration {
    reactor: Weak<ReactorData>,
    key: usize,
}

impl Registration {
    pub fn reactor(&self) -> Reactor {
        let reactor = self.reactor.upgrade().expect("reactor is gone");

        Reactor { inner: reactor }
    }

    pub fn set_waker_persistent(&self, enabled: bool) {
        let reactor = self.reactor.upgrade().expect("reactor is gone");
        let registrations = &mut *reactor.registrations.borrow_mut();

        let reg_data = &mut registrations[self.key];

        reg_data.waker_persistent = enabled;
    }

    pub fn readiness(&self) -> event::Readiness {
        let reactor = self.reactor.upgrade().expect("reactor is gone");
        let registrations = &*reactor.registrations.borrow();

        let reg_data = &registrations[self.key];

        reg_data.readiness
    }

    pub fn set_readiness(&self, readiness: event::Readiness) {
        let reactor = self.reactor.upgrade().expect("reactor is gone");
        let registrations = &mut *reactor.registrations.borrow_mut();

        let reg_data = &mut registrations[self.key];

        reg_data.readiness = readiness;
    }

    pub fn clear_readiness(&self, readiness: mio::Interest) {
        let reactor = self.reactor.upgrade().expect("reactor is gone");
        let registrations = &mut *reactor.registrations.borrow_mut();

        let reg_data = &mut registrations[self.key];

        if let Some(cur) = reg_data.readiness.take() {
            reg_data.readiness = cur.remove(readiness);
        }
    }

    pub fn is_ready(&self) -> bool {
        self.readiness().is_some()
    }

    pub fn set_ready(&self, ready: bool) {
        let readiness = if ready {
            Some(mio::Interest::READABLE)
        } else {
            None
        };

        self.set_readiness(readiness);
    }

    pub fn set_waker(&self, waker: &Waker, interest: mio::Interest) {
        let reactor = self.reactor.upgrade().expect("reactor is gone");
        let registrations = &mut *reactor.registrations.borrow_mut();

        let reg_data = &mut registrations[self.key];

        match reg_data.waker.take() {
            Some(wi) => reg_data.waker = Some(wi.change(waker, interest)),
            None => reg_data.waker = Some(WakerInterest::Single(waker.clone(), interest)),
        }
    }

    pub fn clear_waker(&self) {
        let reactor = self.reactor.upgrade().expect("reactor is gone");
        let registrations = &mut *reactor.registrations.borrow_mut();

        let reg_data = &mut registrations[self.key];

        reg_data.waker = None;
    }

    pub fn clear_waker_interest(&self, interest: mio::Interest) {
        let reactor = self.reactor.upgrade().expect("reactor is gone");
        let registrations = &mut *reactor.registrations.borrow_mut();

        let reg_data = &mut registrations[self.key];

        if let Some(wi) = reg_data.waker.take() {
            reg_data.waker = wi.clear_interest(interest);
        }
    }

    pub fn deregister_io<S: mio::event::Source>(&self, source: &mut S) -> Result<(), io::Error> {
        let reactor = self.reactor.upgrade().expect("reactor is gone");
        let poll = &reactor.poll.borrow();

        poll.deregister(source)
    }

    pub fn deregister_custom(&self, handle: &event::Registration) -> Result<(), io::Error> {
        let reactor = self.reactor.upgrade().expect("reactor is gone");
        let poll = &reactor.poll.borrow();

        poll.deregister_custom(handle)
    }

    pub fn deregister_custom_local(
        &self,
        handle: &event::LocalRegistration,
    ) -> Result<(), io::Error> {
        let reactor = self.reactor.upgrade().expect("reactor is gone");
        let poll = &reactor.poll.borrow();

        poll.deregister_custom_local(handle)
    }

    pub fn pull_from_budget(&self) -> bool {
        let reactor = self.reactor.upgrade().expect("reactor is gone");

        let mut registrations = reactor.registrations.borrow_mut();
        let reg_data = &mut registrations[self.key];

        if reg_data.waker.is_none() {
            panic!("pull_from_budget requires a waker to be set");
        }

        let ok = self.pull_from_budget_inner();

        if !ok {
            let wi = reg_data.waker.take().unwrap();
            let persistent = reg_data.waker_persistent;
            drop(registrations);

            let wi_remaining = if persistent {
                wi.wake_by_ref(mio::Interest::READABLE | mio::Interest::WRITABLE);

                Some(wi)
            } else {
                wi.wake(mio::Interest::READABLE | mio::Interest::WRITABLE)
            };

            if let Some(wi_remaining) = wi_remaining {
                let mut registrations = reactor.registrations.borrow_mut();

                if let Some(event_reg) = registrations.get_mut(self.key) {
                    match event_reg.waker.take() {
                        Some(wi) => event_reg.waker = Some(wi.merge(wi_remaining)),
                        None => event_reg.waker = Some(wi_remaining),
                    }
                }
            }
        }

        ok
    }

    pub fn pull_from_budget_with_waker(&self, waker: &Waker) -> bool {
        let ok = self.pull_from_budget_inner();

        if !ok {
            waker.wake_by_ref();
        }

        ok
    }

    fn pull_from_budget_inner(&self) -> bool {
        let reactor = self.reactor.upgrade().expect("reactor is gone");

        let budget = &mut *reactor.budget.borrow_mut();

        match budget {
            Some(budget) => {
                if *budget > 0 {
                    *budget -= 1;

                    true
                } else {
                    false
                }
            }
            None => true,
        }
    }

    pub fn reregister_timer(&self, expires: Instant) -> Result<(), io::Error> {
        let reactor = self.reactor.upgrade().expect("reactor is gone");

        let registrations = &mut *reactor.registrations.borrow_mut();
        let reg_data = &mut registrations[self.key];

        let timer = &mut *reactor.timer.borrow_mut();

        if let Some(timer_key) = reg_data.timer_key {
            timer.wheel.remove(timer_key);
        }

        let expires_ticks = duration_to_ticks_round_up(expires - timer.start);

        let timer_key = match timer.wheel.add(expires_ticks, self.key) {
            Ok(timer_key) => timer_key,
            Err(_) => return Err(io::Error::from(io::ErrorKind::Other)),
        };

        reg_data.timer_key = Some(timer_key);

        Ok(())
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
    readiness: event::Readiness,
    waker: Option<WakerInterest>,
    timer_key: Option<usize>,
    waker_persistent: bool,
}

struct TimerData {
    wheel: TimerWheel,
    start: Instant,
    current_ticks: u64,
}

struct ReactorData {
    registrations: RefCell<Slab<RegistrationData>>,
    poll: RefCell<event::Poller>,
    timer: RefCell<TimerData>,
    budget: RefCell<Option<u32>>,
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
            current_ticks: 0,
        };

        let inner = Rc::new(ReactorData {
            registrations: RefCell::new(Slab::with_capacity(registrations_max)),
            poll: RefCell::new(event::Poller::new(registrations_max).unwrap()),
            timer: RefCell::new(timer_data),
            budget: RefCell::new(None),
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
            readiness: None,
            waker: None,
            timer_key: None,
            waker_persistent: false,
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
            readiness: None,
            waker: None,
            timer_key: None,
            waker_persistent: false,
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

    pub fn register_custom_local(
        &self,
        handle: &event::LocalRegistration,
        interest: mio::Interest,
    ) -> Result<Registration, io::Error> {
        let registrations = &mut *self.inner.registrations.borrow_mut();

        if registrations.len() == registrations.capacity() {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        let key = registrations.insert(RegistrationData {
            readiness: None,
            waker: None,
            timer_key: None,
            waker_persistent: false,
        });

        if let Err(e) =
            self.inner
                .poll
                .borrow()
                .register_custom_local(handle, mio::Token(key + 1), interest)
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
            readiness: None,
            waker: None,
            timer_key: None,
            waker_persistent: false,
        });

        let timer = &mut *self.inner.timer.borrow_mut();

        let expires_ticks = duration_to_ticks_round_up(expires - timer.start);

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

    // we advance time after polling. this way, Reactor::now() is accurate
    // during task processing. we assume the actual time doesn't change much
    // between task processing and the next poll
    pub fn poll(&self, timeout: Option<Duration>) -> Result<(), io::Error> {
        self.poll_for_events(self.next_timeout(timeout))?;
        self.advance_time(Instant::now());
        self.process_events();

        Ok(())
    }

    // return the timeout that would have been used for a blocking poll
    pub fn poll_nonblocking(&self, current_time: Instant) -> Result<Option<Duration>, io::Error> {
        let timeout = self.next_timeout(None);

        self.poll_for_events(Some(Duration::from_millis(0)))?;
        self.advance_time(current_time);
        self.process_events();

        Ok(timeout)
    }

    pub fn now(&self) -> Instant {
        let timer = &*self.inner.timer.borrow();

        timer.start + ticks_to_duration(timer.current_ticks)
    }

    pub fn set_budget(&self, budget: Option<u32>) {
        *self.inner.budget.borrow_mut() = budget;
    }

    pub fn current() -> Option<Self> {
        REACTOR.with(|r| {
            (*r.borrow_mut()).as_mut().map(|inner| Self {
                inner: inner.upgrade().unwrap(),
            })
        })
    }

    pub fn local_registration_memory(&self) -> Rc<arena::RcMemory<event::LocalRegistrationEntry>> {
        self.inner.poll.borrow().local_registration_memory().clone()
    }

    fn next_timeout(&self, user_timeout: Option<Duration>) -> Option<Duration> {
        let timer = &mut *self.inner.timer.borrow_mut();

        let timer_timeout = timer.wheel.timeout().map(ticks_to_duration);

        match user_timeout {
            Some(user_timeout) => Some(match timer_timeout {
                Some(timer_timeout) => cmp::min(user_timeout, timer_timeout),
                None => user_timeout,
            }),
            None => timer_timeout,
        }
    }

    fn poll_for_events(&self, timeout: Option<Duration>) -> Result<(), io::Error> {
        let poll = &mut *self.inner.poll.borrow_mut();

        poll.poll(timeout)
    }

    fn advance_time(&self, current_time: Instant) {
        let timer = &mut *self.inner.timer.borrow_mut();

        timer.current_ticks = duration_to_ticks_round_down(current_time - timer.start);
        timer.wheel.update(timer.current_ticks);
    }

    fn process_events(&self) {
        let poll = &mut *self.inner.poll.borrow_mut();

        for event in poll.iter_events() {
            let key = usize::from(event.token());

            assert!(key > 0);

            let key = key - 1;

            let mut registrations = self.inner.registrations.borrow_mut();

            if let Some(event_reg) = registrations.get_mut(key) {
                let event_readiness = event.readiness();

                let (became_readable, became_writable) = {
                    let prev_readiness = event_reg.readiness;

                    event_reg.readiness.merge(event_readiness);

                    (
                        !prev_readiness.contains_any(mio::Interest::READABLE)
                            && event_reg.readiness.contains_any(mio::Interest::READABLE),
                        !prev_readiness.contains_any(mio::Interest::WRITABLE)
                            && event_reg.readiness.contains_any(mio::Interest::WRITABLE),
                    )
                };

                if became_readable || became_writable {
                    if let Some(wi) = event_reg.waker.take() {
                        let interest = wi.interest();

                        if (became_readable && interest.is_readable())
                            || (became_writable && interest.is_writable())
                        {
                            let persistent = event_reg.waker_persistent;
                            drop(registrations);

                            let wi_remaining = if persistent {
                                wi.wake_by_ref(event_readiness);

                                Some(wi)
                            } else {
                                wi.wake(event_readiness)
                            };

                            if let Some(wi_remaining) = wi_remaining {
                                let mut registrations = self.inner.registrations.borrow_mut();

                                if let Some(event_reg) = registrations.get_mut(key) {
                                    match event_reg.waker.take() {
                                        Some(wi) => event_reg.waker = Some(wi.merge(wi_remaining)),
                                        None => event_reg.waker = Some(wi_remaining),
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        let timer = &mut *self.inner.timer.borrow_mut();

        let mut expire_count = 0;

        while let Some((_, key)) = timer.wheel.take_expired() {
            let mut registrations = self.inner.registrations.borrow_mut();

            if let Some(event_reg) = registrations.get_mut(key) {
                event_reg.readiness = Some(mio::Interest::READABLE);
                event_reg.timer_key = None;

                if let Some(wi) = event_reg.waker.take() {
                    let persistent = event_reg.waker_persistent;
                    drop(registrations);

                    let wi_remaining = if persistent {
                        wi.wake_by_ref(mio::Interest::READABLE);

                        Some(wi)
                    } else {
                        wi.wake(mio::Interest::READABLE)
                    };

                    if let Some(wi_remaining) = wi_remaining {
                        let mut registrations = self.inner.registrations.borrow_mut();

                        if let Some(event_reg) = registrations.get_mut(key) {
                            match event_reg.waker.take() {
                                Some(wi) => event_reg.waker = Some(wi.merge(wi_remaining)),
                                None => event_reg.waker = Some(wi_remaining),
                            }
                        }
                    }
                }
            }

            expire_count += 1;

            if expire_count >= EXPIRE_MAX {
                break;
            }
        }
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

struct IoEventedInner<S: mio::event::Source> {
    registration: Registration,
    io: S,
}

pub struct IoEvented<S: mio::event::Source> {
    inner: Option<IoEventedInner<S>>,
}

impl<S: mio::event::Source> IoEvented<S> {
    pub fn new(mut io: S, interest: mio::Interest, reactor: &Reactor) -> Result<Self, io::Error> {
        let registration = reactor.register_io(&mut io, interest)?;

        Ok(Self {
            inner: Some(IoEventedInner { registration, io }),
        })
    }

    pub fn registration(&self) -> &Registration {
        &self.inner.as_ref().unwrap().registration
    }

    pub fn io(&self) -> &S {
        &self.inner.as_ref().unwrap().io
    }

    // return registration and io object, without deregistering it
    pub fn into_parts(mut self) -> (Registration, S) {
        let inner = self.inner.take().unwrap();

        (inner.registration, inner.io)
    }

    pub fn into_inner(mut self) -> S {
        let mut inner = self.inner.take().unwrap();
        inner.registration.deregister_io(&mut inner.io).unwrap();

        inner.io
    }
}

impl<S: mio::event::Source> Drop for IoEvented<S> {
    fn drop(&mut self) {
        if let Some(mut inner) = self.inner.take() {
            inner.registration.deregister_io(&mut inner.io).unwrap();
        }
    }
}

pub struct FdEvented {
    registration: Registration,
    fd: RawFd,
}

impl FdEvented {
    pub fn new(fd: RawFd, interest: mio::Interest, reactor: &Reactor) -> Result<Self, io::Error> {
        let registration = reactor.register_io(&mut mio::unix::SourceFd(&fd), interest)?;

        Ok(Self { registration, fd })
    }

    pub fn registration(&self) -> &Registration {
        &self.registration
    }

    pub fn fd(&self) -> &RawFd {
        &self.fd
    }
}

impl Drop for FdEvented {
    fn drop(&mut self) {
        self.registration()
            .deregister_io(&mut mio::unix::SourceFd(&self.fd))
            .unwrap();
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

    pub fn new_local(
        event_reg: &event::LocalRegistration,
        interest: mio::Interest,
        reactor: &Reactor,
    ) -> Result<Self, io::Error> {
        let registration = reactor.register_custom_local(event_reg, interest)?;

        Ok(Self { registration })
    }

    pub fn registration(&self) -> &Registration {
        &self.registration
    }
}

pub struct TimerEvented {
    registration: Registration,
    expires: Cell<Instant>,
}

impl TimerEvented {
    pub fn new(expires: Instant, reactor: &Reactor) -> Result<Self, io::Error> {
        let registration = reactor.register_timer(expires)?;

        Ok(Self {
            registration,
            expires: Cell::new(expires),
        })
    }

    pub fn registration(&self) -> &Registration {
        &self.registration
    }

    pub fn expires(&self) -> Instant {
        self.expires.get()
    }

    pub fn set_expires(&self, expires: Instant) -> Result<(), io::Error> {
        if self.expires.get() == expires {
            // no change
            return Ok(());
        }

        self.expires.set(expires);
        self.registration.reregister_timer(expires)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::waker;
    use std::cell::Cell;
    use std::mem;
    use std::os::unix::io::AsRawFd;
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

        evented
            .registration()
            .set_waker(&waker.clone().into_std(), mio::Interest::READABLE);

        let thread = thread::spawn(move || {
            std::net::TcpStream::connect(addr).unwrap();
        });

        assert_eq!(waker.was_waked(), false);

        reactor.poll(None).unwrap();

        assert_eq!(waker.was_waked(), true);

        thread.join().unwrap();
    }

    #[test]
    fn test_reactor_fd() {
        let reactor = Reactor::new(1);

        let addr: std::net::SocketAddr = "127.0.0.1:0".parse().unwrap();
        let listener = std::net::TcpListener::bind(addr).unwrap();

        let evented =
            FdEvented::new(listener.as_raw_fd(), mio::Interest::READABLE, &reactor).unwrap();

        let addr = listener.local_addr().unwrap();

        let waker = Rc::new(TestWaker::new());

        evented
            .registration()
            .set_waker(&waker.clone().into_std(), mio::Interest::READABLE);

        let thread = thread::spawn(move || {
            std::net::TcpStream::connect(addr).unwrap();
        });

        assert_eq!(waker.was_waked(), false);

        reactor.poll(None).unwrap();

        assert_eq!(waker.was_waked(), true);

        thread.join().unwrap();
    }

    #[test]
    fn test_reactor_custom() {
        let reactor = Reactor::new(1);

        let (reg, sr) = event::Registration::new();

        let evented = CustomEvented::new(&reg, mio::Interest::READABLE, &reactor).unwrap();

        let waker = Rc::new(TestWaker::new());

        evented
            .registration()
            .set_waker(&waker.clone().into_std(), mio::Interest::READABLE);

        let thread = thread::spawn(move || {
            sr.set_readiness(mio::Interest::READABLE).unwrap();
        });

        assert_eq!(waker.was_waked(), false);

        reactor.poll(None).unwrap();

        assert_eq!(waker.was_waked(), true);

        thread.join().unwrap();
    }

    #[test]
    fn test_reactor_timer() {
        let now = Instant::now();

        let reactor = Reactor::new_with_time(1, now);

        let evented = TimerEvented::new(now + Duration::from_millis(100), &reactor).unwrap();

        let waker = Rc::new(TestWaker::new());

        evented
            .registration()
            .set_waker(&waker.clone().into_std(), mio::Interest::READABLE);

        assert_eq!(waker.was_waked(), false);
        assert_eq!(reactor.now(), now);

        let timeout = reactor
            .poll_nonblocking(now + Duration::from_millis(20))
            .unwrap();

        assert_eq!(timeout, Some(Duration::from_millis(100)));
        assert_eq!(reactor.now(), now + Duration::from_millis(20));
        assert_eq!(waker.was_waked(), false);

        let timeout = reactor
            .poll_nonblocking(now + Duration::from_millis(40))
            .unwrap();

        assert_eq!(timeout, Some(Duration::from_millis(80)));
        assert_eq!(reactor.now(), now + Duration::from_millis(40));
        assert_eq!(waker.was_waked(), false);

        let timeout = reactor
            .poll_nonblocking(now + Duration::from_millis(100))
            .unwrap();

        assert_eq!(timeout, Some(Duration::from_millis(60)));
        assert_eq!(waker.was_waked(), true);
        assert_eq!(reactor.now(), now + Duration::from_millis(100));
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
    fn test_reactor_budget() {
        let reactor = Reactor::new(1);

        let (reg, _) = event::Registration::new();

        let evented = CustomEvented::new(&reg, mio::Interest::READABLE, &reactor).unwrap();

        let waker = Rc::new(TestWaker::new());

        evented
            .registration()
            .set_waker(&waker.clone().into_std(), mio::Interest::READABLE);

        assert_eq!(evented.registration().pull_from_budget(), true);
        assert_eq!(waker.was_waked(), false);

        reactor.set_budget(Some(0));

        assert_eq!(evented.registration().pull_from_budget(), false);
        assert_eq!(waker.was_waked(), true);

        let waker = Rc::new(TestWaker::new());

        reactor.set_budget(Some(1));
        evented
            .registration()
            .set_waker(&waker.clone().into_std(), mio::Interest::READABLE);

        assert_eq!(evented.registration().pull_from_budget(), true);
        assert_eq!(waker.was_waked(), false);

        assert_eq!(evented.registration().pull_from_budget(), false);
        assert_eq!(waker.was_waked(), true);
    }
}
