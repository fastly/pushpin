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
use mio;
use slab::Slab;
use std::cell::RefCell;
use std::io;
use std::rc::Rc;
use std::task::Waker;

pub struct RegistrationHandle {
    reactor: Rc<MioReactor>,
    key: usize,
}

impl RegistrationHandle {
    pub fn is_ready(&self) -> bool {
        let data = &*self.reactor.data.borrow();

        let event_reg = &data.registrations[self.key];

        event_reg.ready
    }

    pub fn set_ready(&self, ready: bool) {
        let data = &mut *self.reactor.data.borrow_mut();

        let event_reg = &mut data.registrations[self.key];

        event_reg.ready = ready;
    }

    pub fn bind_waker(&self, waker: Waker) {
        let data = &mut *self.reactor.data.borrow_mut();

        let event_reg = &mut data.registrations[self.key];

        event_reg.waker = Some(waker);
    }

    pub fn unbind_waker(&self) {
        let data = &mut *self.reactor.data.borrow_mut();

        let event_reg = &mut data.registrations[self.key];

        event_reg.waker = None;
    }
}

impl Drop for RegistrationHandle {
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
}

pub struct MioReactor {
    data: RefCell<MioReactorData>,
    poll: RefCell<event::Poller>,
}

impl MioReactor {
    pub fn new(registrations_max: usize) -> Self {
        let data = MioReactorData {
            registrations: Slab::with_capacity(registrations_max),
        };

        Self {
            data: RefCell::new(data),
            poll: RefCell::new(event::Poller::new(registrations_max).unwrap()),
        }
    }

    pub fn register<E>(
        reactor: &Rc<MioReactor>,
        handle: &mut E,
        interest: mio::Interest,
    ) -> Result<RegistrationHandle, io::Error>
    where
        E: mio::event::Source + ?Sized,
    {
        let data = &mut *reactor.data.borrow_mut();

        if data.registrations.len() == data.registrations.capacity() {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        let key = data.registrations.insert(EventRegistration {
            ready: false,
            waker: None,
        });

        if let Err(e) = reactor
            .poll
            .borrow()
            .register(handle, mio::Token(key + 1), interest)
        {
            data.registrations.remove(key);

            return Err(e);
        }

        Ok(RegistrationHandle {
            reactor: Rc::clone(reactor),
            key,
        })
    }

    pub fn register_custom(
        reactor: &Rc<MioReactor>,
        handle: &event::Registration,
        interest: mio::Interest,
    ) -> Result<RegistrationHandle, io::Error> {
        let data = &mut *reactor.data.borrow_mut();

        if data.registrations.len() == data.registrations.capacity() {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        let key = data.registrations.insert(EventRegistration {
            ready: false,
            waker: None,
        });

        if let Err(e) = reactor
            .poll
            .borrow()
            .register_custom(handle, mio::Token(key + 1), interest)
        {
            data.registrations.remove(key);

            return Err(e);
        }

        Ok(RegistrationHandle {
            reactor: Rc::clone(reactor),
            key,
        })
    }

    pub fn poll(&self) -> Result<(), io::Error> {
        let data = &mut *self.data.borrow_mut();

        let poll = &mut *self.poll.borrow_mut();

        poll.poll(None)?;

        for event in poll.iter_events() {
            let key = usize::from(event.token());

            assert!(key > 0);

            let key = key - 1;

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
