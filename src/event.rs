/*
 * Copyright (C) 2021 Fanout, Inc.
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

use crate::list;
use mio::event::Source;
use mio::{Events, Interest, Poll, Token, Waker};
use slab::Slab;
use std::io;
use std::sync::{Arc, Mutex};
use std::time::Duration;

const EVENTS_MAX: usize = 1024;

type Readiness = Option<Interest>;

trait MergeReadiness {
    fn merge(&mut self, readiness: Interest);
}

impl MergeReadiness for Readiness {
    fn merge(&mut self, readiness: Interest) {
        match *self {
            Some(cur) => *self = Some(cur.add(readiness)),
            None => *self = Some(readiness),
        }
    }
}

struct SourceItem {
    subtoken: Token,
    interests: Interest,
    readiness: Readiness,
}

struct RegisteredSources {
    nodes: Slab<list::Node<SourceItem>>,
    ready: list::List,
}

struct CustomSourcesInner {
    registered_sources: Mutex<RegisteredSources>,
    waker: Waker,
}

impl CustomSourcesInner {
    fn new(max_sources: usize, waker: Waker) -> Self {
        Self {
            registered_sources: Mutex::new(RegisteredSources {
                nodes: Slab::with_capacity(max_sources),
                ready: list::List::default(),
            }),
            waker,
        }
    }

    fn register(&self, subtoken: Token, interests: Interest) -> Result<usize, io::Error> {
        let sources = &mut *self.registered_sources.lock().unwrap();

        if sources.nodes.len() == sources.nodes.capacity() {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        Ok(sources.nodes.insert(list::Node::new(SourceItem {
            subtoken,
            interests,
            readiness: None,
        })))
    }

    fn deregister(&self, key: usize) -> Result<(), io::Error> {
        let sources = &mut *self.registered_sources.lock().unwrap();

        if sources.nodes.contains(key) {
            sources.ready.remove(&mut sources.nodes, key);
            sources.nodes.remove(key);
        }

        Ok(())
    }

    fn set_readiness(&self, key: usize, readiness: Interest) -> Result<(), io::Error> {
        let sources = &mut *self.registered_sources.lock().unwrap();

        if !sources.nodes.contains(key) {
            return Err(io::Error::from(io::ErrorKind::NotFound));
        }

        let item = &mut sources.nodes[key].value;

        if !(item.interests.is_readable() && readiness.is_readable())
            && !(item.interests.is_writable() && readiness.is_writable())
        {
            // not of interest
            return Ok(());
        }

        let orig = item.readiness;

        item.readiness.merge(readiness);

        if item.readiness != orig {
            sources.ready.remove(&mut sources.nodes, key);
            sources.ready.push_back(&mut sources.nodes, key);

            self.waker.wake()?
        }

        Ok(())
    }

    fn has_events(&self) -> bool {
        let sources = &mut *self.registered_sources.lock().unwrap();

        !sources.ready.is_empty()
    }

    fn next_event(&self) -> Option<(Token, Interest)> {
        let sources = &mut *self.registered_sources.lock().unwrap();

        match sources.ready.pop_front(&mut sources.nodes) {
            Some(key) => {
                let item = &mut sources.nodes[key].value;

                let readiness = item.readiness.take().unwrap();

                Some((item.subtoken, readiness))
            }
            None => None,
        }
    }
}

pub struct CustomSources {
    inner: Arc<CustomSourcesInner>,
}

impl CustomSources {
    pub fn new(poll: &Poll, token: Token, max_sources: usize) -> Result<Self, io::Error> {
        let waker = Waker::new(poll.registry(), token)?;

        Ok(Self {
            inner: Arc::new(CustomSourcesInner::new(max_sources, waker)),
        })
    }

    pub fn register(
        &self,
        registration: &Registration,
        subtoken: Token,
        interests: Interest,
    ) -> Result<(), io::Error> {
        let mut reg = registration.inner.lock().unwrap();

        if reg.data.is_none() {
            let key = self.inner.register(subtoken, interests)?;

            reg.data = Some((key, self.inner.clone()));

            if let Some(readiness) = reg.readiness {
                self.inner.set_readiness(key, readiness).unwrap();

                reg.readiness = None;
            }
        }

        Ok(())
    }

    pub fn deregister(&self, registration: &Registration) -> Result<(), io::Error> {
        let mut reg = registration.inner.lock().unwrap();

        if let Some((key, _)) = reg.data {
            self.inner.deregister(key)?;

            reg.data = None;
        }

        Ok(())
    }

    fn has_events(&self) -> bool {
        self.inner.has_events()
    }

    pub fn next_event(&self) -> Option<(Token, Interest)> {
        self.inner.next_event()
    }
}

struct RegistrationInner {
    data: Option<(usize, Arc<CustomSourcesInner>)>,
    readiness: Readiness,
}

pub struct Registration {
    inner: Arc<Mutex<RegistrationInner>>,
}

impl Registration {
    pub fn new() -> (Self, SetReadiness) {
        let reg = Arc::new(Mutex::new(RegistrationInner {
            data: None,
            readiness: None,
        }));

        let registration = Self { inner: reg.clone() };

        let set_readiness = SetReadiness { inner: reg };

        (registration, set_readiness)
    }
}

impl Drop for Registration {
    fn drop(&mut self) {
        let mut reg = self.inner.lock().unwrap();

        if let Some((key, sources)) = &reg.data {
            sources.deregister(*key).unwrap();

            reg.data = None;
        }
    }
}

pub struct SetReadiness {
    inner: Arc<Mutex<RegistrationInner>>,
}

impl SetReadiness {
    pub fn set_readiness(&self, readiness: Interest) -> Result<(), io::Error> {
        let mut reg = self.inner.lock().unwrap();

        match &reg.data {
            Some((key, sources)) => sources.set_readiness(*key, readiness)?,
            None => reg.readiness.merge(readiness),
        }

        Ok(())
    }
}

#[derive(Debug, PartialEq)]
pub struct Event {
    token: Token,
    readiness: Interest,
}

impl Event {
    pub fn token(&self) -> Token {
        self.token
    }

    pub fn is_readable(&self) -> bool {
        self.readiness.is_readable()
    }

    pub fn is_writable(&self) -> bool {
        self.readiness.is_writable()
    }
}

pub struct Poller {
    poll: Poll,
    events: Events,
    custom_sources: CustomSources,
}

impl Poller {
    pub fn new(max_custom_sources: usize) -> Result<Self, io::Error> {
        let poll = Poll::new()?;
        let events = Events::with_capacity(EVENTS_MAX);
        let custom_sources = CustomSources::new(&poll, Token(0), max_custom_sources)?;

        Ok(Self {
            poll,
            events,
            custom_sources,
        })
    }

    pub fn register<S>(
        &self,
        source: &mut S,
        token: Token,
        interests: Interest,
    ) -> Result<(), io::Error>
    where
        S: Source + ?Sized,
    {
        if token == Token(0) {
            return Err(io::Error::from(io::ErrorKind::InvalidInput));
        }

        self.poll.registry().register(source, token, interests)
    }

    pub fn deregister<S>(&self, source: &mut S) -> Result<(), io::Error>
    where
        S: Source + ?Sized,
    {
        self.poll.registry().deregister(source)
    }

    pub fn register_custom(
        &self,
        registration: &Registration,
        token: Token,
        interests: Interest,
    ) -> Result<(), io::Error> {
        if token == Token(0) {
            return Err(io::Error::from(io::ErrorKind::InvalidInput));
        }

        self.custom_sources.register(registration, token, interests)
    }

    pub fn deregister_custom(&self, registration: &Registration) -> Result<(), io::Error> {
        self.custom_sources.deregister(registration)
    }

    pub fn poll(&mut self, timeout: Option<Duration>) -> Result<(), io::Error> {
        let timeout = if self.custom_sources.has_events() {
            Some(Duration::from_millis(0))
        } else {
            timeout
        };

        self.poll.poll(&mut self.events, timeout)
    }

    pub fn iter_events(&self) -> EventsIterator<'_, '_> {
        EventsIterator {
            events: self.events.iter(),
            custom_sources: &self.custom_sources,
            custom_left: EVENTS_MAX,
        }
    }
}

pub struct EventsIterator<'a, 'b> {
    events: mio::event::Iter<'b>,
    custom_sources: &'a CustomSources,
    custom_left: usize,
}

impl Iterator for EventsIterator<'_, '_> {
    type Item = Event;

    fn next(&mut self) -> Option<Self::Item> {
        while let Some(event) = self.events.next() {
            if event.token() == Token(0) {
                continue;
            }

            let mut readiness = None;

            if event.is_readable() {
                readiness.merge(Interest::READABLE);
            }

            if event.is_writable() {
                readiness.merge(Interest::WRITABLE);
            }

            if let Some(readiness) = readiness {
                return Some(Event {
                    token: event.token(),
                    readiness,
                });
            }
        }

        if self.custom_left > 0 {
            self.custom_left -= 1;

            if let Some((token, readiness)) = self.custom_sources.next_event() {
                return Some(Event { token, readiness });
            }
        }

        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn test_readiness() {
        let token = Token(123);
        let subtoken = Token(456);

        let mut poll = Poll::new().unwrap();

        let sources = CustomSources::new(&poll, token, 1).unwrap();

        assert_eq!(sources.has_events(), false);
        assert_eq!(sources.next_event(), None);

        let (reg, sr) = Registration::new();

        sources
            .register(&reg, subtoken, Interest::READABLE)
            .unwrap();

        let mut events = Events::with_capacity(1024);

        poll.poll(&mut events, Some(Duration::from_millis(0)))
            .unwrap();

        assert!(events.is_empty());

        sr.set_readiness(Interest::READABLE).unwrap();

        'poll: loop {
            poll.poll(&mut events, None).unwrap();

            for event in &events {
                if event.token() == token {
                    break 'poll;
                }
            }
        }

        assert_eq!(sources.has_events(), true);
        assert_eq!(sources.next_event(), Some((subtoken, Interest::READABLE)));

        assert_eq!(sources.has_events(), false);
        assert_eq!(sources.next_event(), None);
    }

    #[test]
    fn test_readiness_early() {
        let token = Token(123);
        let subtoken = Token(456);

        let mut poll = Poll::new().unwrap();

        let sources = CustomSources::new(&poll, token, 1).unwrap();

        assert_eq!(sources.has_events(), false);
        assert_eq!(sources.next_event(), None);

        let (reg, sr) = Registration::new();

        sr.set_readiness(Interest::READABLE).unwrap();

        sources
            .register(&reg, subtoken, Interest::READABLE)
            .unwrap();

        let mut events = Events::with_capacity(1024);

        poll.poll(&mut events, Some(Duration::from_millis(0)))
            .unwrap();

        let event = events.iter().next();
        assert!(event.is_some());

        let event = event.unwrap();
        assert_eq!(event.token(), token);

        assert_eq!(sources.has_events(), true);
        assert_eq!(sources.next_event(), Some((subtoken, Interest::READABLE)));

        assert_eq!(sources.has_events(), false);
        assert_eq!(sources.next_event(), None);
    }

    #[test]
    fn test_poller() {
        let token = Token(123);

        let mut poller = Poller::new(1).unwrap();

        assert_eq!(poller.iter_events().next(), None);

        let (reg, sr) = Registration::new();

        poller
            .register_custom(&reg, token, Interest::READABLE)
            .unwrap();

        poller.poll(Some(Duration::from_millis(0))).unwrap();

        assert_eq!(poller.iter_events().next(), None);

        sr.set_readiness(Interest::READABLE).unwrap();

        poller.poll(None).unwrap();

        let mut it = poller.iter_events();

        let event = it.next().unwrap();
        assert_eq!(event.token(), token);
        assert_eq!(event.is_readable(), true);
        assert_eq!(it.next(), None);
    }
}
