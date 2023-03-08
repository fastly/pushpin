/*
 * Copyright (C) 2022 Fanout, Inc.
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
use crate::list;
use arrayvec::{ArrayString, ArrayVec};
use mio::Interest;
use slab::Slab;
use std::collections::VecDeque;
use std::io;
use std::net::{IpAddr, ToSocketAddrs};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread;

pub const REGISTRATIONS_PER_QUERY: usize = 1;

pub const ADDRS_MAX: usize = 16;

pub type Hostname = ArrayString<255>;
pub type Addrs = ArrayVec<IpAddr, ADDRS_MAX>;

fn std_resolve(host: &str) -> Result<Addrs, io::Error> {
    match (host, 0).to_socket_addrs() {
        Ok(addrs) => Ok(addrs.take(ADDRS_MAX).map(|addr| addr.ip()).collect()),
        Err(e) => Err(e),
    }
}

struct QueryItem {
    host: Hostname,
    result: Option<Result<Addrs, io::Error>>,
    set_readiness: event::SetReadiness,
    invalidated: Option<Arc<AtomicBool>>,
}

struct QueriesInner {
    stop: bool,
    nodes: Slab<list::Node<QueryItem>>,
    next: list::List,
    registrations: VecDeque<(event::Registration, event::SetReadiness)>,
    invalidated_count: u32,
}

#[derive(Clone)]
struct Queries {
    inner: Arc<(Mutex<QueriesInner>, Condvar)>,
}

impl Queries {
    fn new(queries_max: usize) -> Self {
        let mut registrations = VecDeque::with_capacity(queries_max);

        for _ in 0..registrations.capacity() {
            registrations.push_back(event::Registration::new());
        }

        let inner = QueriesInner {
            stop: false,
            nodes: Slab::with_capacity(queries_max),
            next: list::List::default(),
            registrations,
            invalidated_count: 0,
        };

        Self {
            inner: Arc::new((Mutex::new(inner), Condvar::new())),
        }
    }

    fn set_stop_flag(&self) {
        let (lock, cvar) = &*self.inner;

        let mut queries = lock.lock().unwrap();
        queries.stop = true;

        cvar.notify_all();
    }

    fn add(&self, host: &str) -> Result<(usize, event::Registration), ()> {
        let (lock, cvar) = &*self.inner;

        let queries = &mut *lock.lock().unwrap();

        if queries.nodes.len() == queries.nodes.capacity() {
            return Err(());
        }

        let (reg, sr) = queries.registrations.pop_back().unwrap();

        let nkey = match Hostname::from(host) {
            Ok(host) => {
                let nkey = queries.nodes.insert(list::Node::new(QueryItem {
                    host,
                    result: None,
                    set_readiness: sr,
                    invalidated: None,
                }));

                queries.next.push_back(&mut queries.nodes, nkey);

                cvar.notify_one();

                nkey
            }
            Err(_) => {
                sr.set_readiness(Interest::READABLE).unwrap();

                let nkey = queries.nodes.insert(list::Node::new(QueryItem {
                    host: Hostname::new(),
                    result: Some(Err(io::Error::from(io::ErrorKind::InvalidInput))),
                    set_readiness: sr,
                    invalidated: None,
                }));

                nkey
            }
        };

        Ok((nkey, reg))
    }

    // block until a query is available, or stopped
    fn get_next(&self, invalidated: &Arc<AtomicBool>) -> Option<(usize, Hostname)> {
        let (lock, cvar) = &*self.inner;

        let mut queries_guard = lock.lock().unwrap();

        loop {
            let queries = &mut *queries_guard;

            if queries.stop {
                return None;
            }

            if let Some(nkey) = queries.next.pop_front(&mut queries.nodes) {
                let qi = &mut queries.nodes[nkey].value;

                invalidated.store(false, Ordering::Relaxed);
                qi.invalidated = Some(invalidated.clone());

                return Some((nkey, qi.host.clone()));
            }

            queries_guard = cvar.wait(queries_guard).unwrap();
        }
    }

    fn set_result(
        &self,
        item_key: usize,
        result: Result<Addrs, io::Error>,
        invalidated: &AtomicBool,
    ) {
        let mut queries = self.inner.0.lock().unwrap();

        if !invalidated.load(Ordering::Relaxed) {
            let qi = &mut queries.nodes[item_key].value;

            qi.result = Some(result);
            qi.set_readiness.set_readiness(Interest::READABLE).unwrap();
        } else {
            queries.invalidated_count += 1;
        }
    }

    fn take_result(&self, item_key: usize) -> Option<Result<Addrs, io::Error>> {
        let queries = &mut *self.inner.0.lock().unwrap();

        queries.nodes[item_key].value.result.take()
    }

    fn remove(&self, item_key: usize, registration: event::Registration) {
        let queries = &mut *self.inner.0.lock().unwrap();

        // remove from next list if present
        queries.next.remove(&mut queries.nodes, item_key);

        let qi = queries.nodes.remove(item_key).value;

        if let Some(invalidated) = &qi.invalidated {
            invalidated.store(true, Ordering::Relaxed);
        }

        queries
            .registrations
            .push_back((registration, qi.set_readiness));
    }

    #[cfg(test)]
    fn invalidated_count(&self) -> u32 {
        let queries = &mut *self.inner.0.lock().unwrap();

        queries.invalidated_count
    }
}

struct ResolverInner {
    workers: Vec<thread::JoinHandle<()>>,
    queries: Queries,
}

impl ResolverInner {
    fn new<F>(num_threads: usize, queries_max: usize, resolve_fn: Arc<F>) -> Self
    where
        F: Fn(&str) -> Result<Addrs, io::Error> + Send + Sync + 'static,
    {
        let mut workers = Vec::with_capacity(num_threads);
        let queries = Queries::new(queries_max);

        for _ in 0..workers.capacity() {
            let queries = queries.clone();
            let resolve_fn = resolve_fn.clone();

            let thread = thread::Builder::new()
                .name("resolver".to_string())
                .spawn(move || {
                    let invalidated = Arc::new(AtomicBool::new(false));

                    loop {
                        let (item_key, host) = match queries.get_next(&invalidated) {
                            Some(ret) => ret,
                            None => break,
                        };

                        let ret = resolve_fn(host.as_str());

                        queries.set_result(item_key, ret, &invalidated);
                    }
                })
                .unwrap();

            workers.push(thread);
        }

        Self { workers, queries }
    }

    fn resolve(&self, host: &str) -> Result<Query, ()> {
        let (item_key, reg) = self.queries.add(host)?;

        Ok(Query {
            queries: self.queries.clone(),
            item_key,
            registration: Some(reg),
        })
    }

    fn stop(&mut self) {
        self.queries.set_stop_flag();

        for worker in self.workers.drain(..) {
            worker.join().unwrap();
        }
    }
}

impl Drop for ResolverInner {
    fn drop(&mut self) {
        self.stop();
    }
}

pub struct Resolver {
    inner: ResolverInner,
}

impl Resolver {
    pub fn new(num_threads: usize, queries_max: usize) -> Self {
        let inner = ResolverInner::new(num_threads, queries_max, Arc::new(std_resolve));

        Self { inner }
    }

    pub fn resolve(&self, host: &str) -> Result<Query, ()> {
        self.inner.resolve(host)
    }
}

pub struct Query {
    queries: Queries,
    item_key: usize,
    registration: Option<event::Registration>,
}

impl Query {
    pub fn get_read_registration(&self) -> &event::Registration {
        self.registration.as_ref().unwrap()
    }

    pub fn process(&self) -> Option<Result<Addrs, io::Error>> {
        self.queries.take_result(self.item_key)
    }
}

impl Drop for Query {
    fn drop(&mut self) {
        let reg = self.registration.take().unwrap();

        self.queries.remove(self.item_key, reg);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resolve() {
        let mut poller = event::Poller::new(1).unwrap();

        let resolver = Resolver::new(1, 1);

        let query = resolver.resolve("127.0.0.1").unwrap();

        // queries_max is 1, so this should error
        assert_eq!(resolver.resolve("127.0.0.1").is_err(), true);

        // register query interest with poller
        poller
            .register_custom(
                query.get_read_registration(),
                mio::Token(1),
                Interest::READABLE,
            )
            .unwrap();

        // wait for completion
        let result = loop {
            if let Some(result) = query.process() {
                break result;
            }

            poller.poll(None).unwrap();

            for _ in poller.iter_events() {}
        };

        // deregister query interest
        poller
            .deregister_custom(query.get_read_registration())
            .unwrap();

        assert_eq!(result.unwrap().as_slice(), &[IpAddr::from([127, 0, 0, 1])]);
    }

    #[test]
    fn invalidate_query() {
        let mut inner = {
            let cond = Arc::new((Mutex::new(false), Condvar::new()));

            let resolve_fn = {
                let cond = cond.clone();

                Arc::new(move |_: &str| {
                    let (lock, cvar) = &*cond;

                    let guard = lock.lock().unwrap();

                    // let main thread know we've started
                    cvar.notify_one();

                    // wait for query to be removed
                    let _guard = cvar.wait(guard).unwrap();

                    Ok(Addrs::new())
                })
            };

            let (lock, cvar) = &*cond;
            let guard = lock.lock().unwrap();

            let inner = ResolverInner::new(1, 1, resolve_fn);

            let query = inner.resolve("127.0.0.1").unwrap();

            // wait for resolve_fn to start
            let _guard = cvar.wait(guard).unwrap();

            drop(query);

            // let worker know the query has been removed
            cvar.notify_one();

            inner
        };

        inner.stop();

        assert_eq!(inner.queries.invalidated_count(), 1);
    }
}
