/*
 * Copyright (C) 2023 Fanout, Inc.
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
use crate::timer::TimerWheel;
use slab::Slab;
use std::borrow::Borrow;
use std::collections::HashMap;
use std::hash::Hash;
use std::time::{Duration, Instant};

const TICK_DURATION_MS: u64 = 10;

fn duration_to_ticks_round_down(d: Duration) -> u64 {
    (d.as_millis() / (TICK_DURATION_MS as u128)) as u64
}

struct PoolItem<K, V> {
    key: K,
    value: V,
    timer_id: usize,
}

pub struct Pool<K, V> {
    nodes: Slab<list::Node<PoolItem<K, V>>>,
    by_key: HashMap<K, list::List>,
    wheel: TimerWheel,
    start: Instant,
    current_ticks: u64,
}

impl<K, V> Pool<K, V>
where
    K: Clone + Eq + Hash + PartialEq,
{
    pub fn new(capacity: usize) -> Self {
        Self {
            nodes: Slab::with_capacity(capacity),
            by_key: HashMap::with_capacity(capacity),
            wheel: TimerWheel::new(capacity),
            start: Instant::now(),
            current_ticks: 0,
        }
    }

    pub fn add(&mut self, key: K, value: V, expires: Instant) -> Result<(), V> {
        if self.nodes.len() == self.nodes.capacity() {
            return Err(value);
        }

        let expires = self.get_ticks(expires);

        let nkey = {
            let entry = self.nodes.vacant_entry();
            let nkey = entry.key();

            let timer_id = self.wheel.add(expires, nkey).unwrap();

            entry.insert(list::Node::new(PoolItem {
                key: key.clone(),
                value,
                timer_id,
            }));

            nkey
        };

        let l = self.by_key.entry(key).or_default();

        l.push_back(&mut self.nodes, nkey);

        Ok(())
    }

    pub fn take<Q>(&mut self, key: &Q) -> Option<V>
    where
        K: Borrow<Q>,
        Q: Hash + Eq + ?Sized,
    {
        let l = self.by_key.get_mut(key)?;

        let nkey = l.pop_front(&mut self.nodes)?;

        if l.is_empty() {
            self.by_key.remove(&key);
        }

        let pi = self.nodes.remove(nkey).value;

        self.wheel.remove(pi.timer_id);

        Some(pi.value)
    }

    pub fn expire(&mut self, now: Instant) -> Option<(K, V)> {
        let ticks = self.get_ticks(now);

        if ticks > self.current_ticks {
            self.wheel.update(ticks);
            self.current_ticks = ticks;
        }

        let nkey = match self.wheel.take_expired() {
            Some((_, nkey)) => nkey,
            None => return None,
        };

        let pi = &self.nodes[nkey].value;

        let l = self.by_key.get_mut(&pi.key).unwrap();
        l.remove(&mut self.nodes, nkey);

        if l.is_empty() {
            let pi = &self.nodes[nkey].value;
            self.by_key.remove(&pi.key);
        }

        let pi = self.nodes.remove(nkey).value;

        Some((pi.key, pi.value))
    }

    fn get_ticks(&self, t: Instant) -> u64 {
        let d = if t > self.start {
            t - self.start
        } else {
            Duration::from_millis(0)
        };

        duration_to_ticks_round_down(d)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pool_add_take() {
        let mut pool = Pool::new(3);

        let now = Instant::now();
        pool.add(1, "a", now).unwrap();
        pool.add(1, "b", now).unwrap();
        pool.add(2, "c", now).unwrap();
        assert_eq!(pool.add(2, "d", now).is_ok(), false);

        assert_eq!(pool.take(&1), Some("a"));
        assert_eq!(pool.take(&2), Some("c"));
        assert_eq!(pool.take(&1), Some("b"));
        assert_eq!(pool.take(&2), None);
    }

    #[test]
    fn pool_expire() {
        let mut pool = Pool::new(3);

        let now = Instant::now();
        pool.add(1, "a", now + Duration::from_secs(1)).unwrap();
        pool.add(1, "b", now + Duration::from_secs(2)).unwrap();
        pool.add(2, "c", now + Duration::from_secs(3)).unwrap();

        assert_eq!(pool.expire(now), None);
        assert_eq!(pool.expire(now + Duration::from_secs(1)), Some((1, "a")));
        assert_eq!(pool.expire(now + Duration::from_secs(1)), None);
        assert_eq!(pool.expire(now + Duration::from_secs(5)), Some((1, "b")));
        assert_eq!(pool.expire(now + Duration::from_secs(5)), Some((2, "c")));
        assert_eq!(pool.expire(now + Duration::from_secs(5)), None);
    }
}
