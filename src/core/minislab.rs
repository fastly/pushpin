/*
 * Copyright (C) 2026 Fastly, Inc.
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

// A slab that supports drop-in-place removals.
// Adapted from https://github.com/tokio-rs/slab (MIT licensed).
//
// Changes:
// * try_remove() returns a Result<(), RemoveError> instead of Option<T>.
// * remove() returns nothing.
// * All methods/types we don't use are removed.

#[derive(Debug)]
pub struct RemoveError;

pub struct MiniSlab<T> {
    entries: Vec<Entry<T>>,
    len: usize,
    next: usize,
}

#[derive(Clone)]
enum Entry<T> {
    Vacant(usize),
    Occupied(T),
}

impl<T> MiniSlab<T> {
    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            entries: Vec::with_capacity(capacity),
            next: 0,
            len: 0,
        }
    }

    pub fn capacity(&self) -> usize {
        self.entries.capacity()
    }

    pub fn clear(&mut self) {
        self.entries.clear();
        self.len = 0;
        self.next = 0;
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn get(&self, key: usize) -> Option<&T> {
        match self.entries.get(key) {
            Some(Entry::Occupied(val)) => Some(val),
            _ => None,
        }
    }

    /// # Safety
    ///
    /// The key must be within bounds.
    pub unsafe fn get_unchecked(&self, key: usize) -> &T {
        match *self.entries.get_unchecked(key) {
            Entry::Occupied(ref val) => val,
            _ => unreachable!(),
        }
    }

    pub fn insert(&mut self, val: T) -> usize {
        let key = self.next;

        self.insert_at(key, val);

        key
    }

    fn insert_at(&mut self, key: usize, val: T) {
        self.len += 1;

        if key == self.entries.len() {
            self.entries.push(Entry::Occupied(val));
            self.next = key + 1;
        } else {
            self.next = match self.entries.get(key) {
                Some(&Entry::Vacant(next)) => next,
                _ => unreachable!(),
            };
            self.entries[key] = Entry::Occupied(val);
        }
    }

    pub fn try_remove(&mut self, key: usize) -> Result<(), RemoveError> {
        if let Some(entry) = self.entries.get_mut(key) {
            if let Entry::Occupied(_) = entry {
                *entry = Entry::Vacant(self.next);
                self.len -= 1;
                self.next = key;
                return Ok(());
            }
        }

        Err(RemoveError)
    }

    #[track_caller]
    pub fn remove(&mut self, key: usize) {
        self.try_remove(key).expect("invalid key");
    }
}
