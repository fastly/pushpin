/*
 * Copyright (C) 2023 Fastly, Inc.
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

use std::sync::atomic::{AtomicUsize, Ordering};

#[derive(Debug)]
pub struct CounterError;

/// An unsigned integer that can be shared between threads. Counter is backed by an AtomicUsize
/// and performs operations with Relaxed memory ordering, so its value cannot be reliably assumed
/// to be in sync with other atomic values, including other Counter values.
pub struct Counter(AtomicUsize);

impl Counter {
    pub fn new(value: usize) -> Self {
        Self(AtomicUsize::new(value))
    }

    pub fn inc(&self, amount: usize) -> Result<(), CounterError> {
        if amount == 0 {
            return Ok(());
        }

        loop {
            let value = self.0.load(Ordering::Relaxed);

            if amount > usize::MAX - value {
                return Err(CounterError);
            }

            if self
                .0
                .compare_exchange(value, value + amount, Ordering::Relaxed, Ordering::Relaxed)
                .is_ok()
            {
                break;
            }
        }

        Ok(())
    }

    pub fn dec(&self, amount: usize) -> Result<(), CounterError> {
        if amount == 0 {
            return Ok(());
        }

        loop {
            let value = self.0.load(Ordering::Relaxed);

            if amount > value {
                return Err(CounterError);
            }

            if self
                .0
                .compare_exchange(value, value - amount, Ordering::Relaxed, Ordering::Relaxed)
                .is_ok()
            {
                break;
            }
        }

        Ok(())
    }
}

pub struct CounterDec<'a> {
    counter: &'a Counter,
    amount: usize,
}

impl<'a> CounterDec<'a> {
    pub fn new(counter: &'a Counter) -> Self {
        Self { counter, amount: 0 }
    }

    pub fn dec(&mut self, amount: usize) -> Result<(), CounterError> {
        self.counter.dec(amount)?;
        self.amount += amount;

        Ok(())
    }
}

impl Drop for CounterDec<'_> {
    fn drop(&mut self) {
        assert!(self.counter.inc(self.amount).is_ok());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn counter() {
        let c = Counter::new(2);

        assert!(c.dec(1).is_ok());
        assert!(c.dec(1).is_ok());
        assert!(c.dec(1).is_err());

        assert!(c.inc(1).is_ok());
        assert!(c.dec(2).is_err());
        assert!(c.dec(1).is_ok());

        assert!(c.inc(usize::MAX).is_ok());
        assert!(c.inc(1).is_err());
    }

    #[test]
    fn counter_dec() {
        let c = Counter::new(2);

        {
            let mut c = CounterDec::new(&c);
            assert!(c.dec(1).is_ok());
            assert!(c.dec(1).is_ok());
            assert!(c.dec(1).is_err());
        }

        assert!(c.dec(2).is_ok());
        assert!(c.dec(1).is_err());
    }
}
