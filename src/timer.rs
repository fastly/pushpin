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

// adapted from http://25thandclement.com/~william/projects/timeout.c.html (MIT licensed)

use crate::list;
use slab::Slab;
use std::cmp;

const WHEEL_BITS: usize = 6;
const WHEEL_NUM: usize = 4;
const WHEEL_LEN: usize = 1 << WHEEL_BITS;
const WHEEL_MAX: usize = WHEEL_LEN - 1;
const WHEEL_MASK: u64 = (WHEEL_LEN as u64) - 1;
const TIMEOUT_MAX: u64 = (1 << (WHEEL_BITS * WHEEL_NUM)) - 1;

// find last set
fn fls64(x: u64) -> u32 {
    64 - x.leading_zeros()
}

fn need_resched(curtime: u64, newtime: u64) -> [u64; WHEEL_NUM] {
    let mut result = [0; WHEEL_NUM];

    // no time elapsed
    if newtime <= curtime {
        return result;
    }

    let mut elapsed = newtime - curtime;

    for wheel in 0..WHEEL_NUM {
        // we only care about the highest bits
        let trunc_bits = (wheel * WHEEL_BITS) as u64;

        let pending;

        if (elapsed >> trunc_bits) > (WHEEL_MAX as u64) {
            // all slots need processing
            pending = !0;
        } else {
            let old_slot = (curtime >> trunc_bits) & WHEEL_MASK;
            let new_slot = (newtime >> trunc_bits) & WHEEL_MASK;

            let d = if new_slot > old_slot {
                new_slot - old_slot
            } else {
                (WHEEL_LEN as u64) - old_slot + new_slot
            };

            pending = if d >= WHEEL_LEN as u64 {
                !0
            } else if wheel > 0 {
                ((1 << d) - 1u64).rotate_left(old_slot as u32)
            } else {
                ((1 << d) - 1u64).rotate_left((old_slot + 1) as u32)
            };
        }

        result[wheel] = pending;

        let finished_bit = if wheel > 0 {
            // higher wheels have completed a full rotation when slot 63 is processed
            1 << (WHEEL_LEN - 1)
        } else {
            // lowest wheel has completed a full rotation when slot 0 is processed
            1
        };

        // if the current wheel didn't finish a full rotation then we don't need to look
        //   at higher wheels
        if pending & finished_bit == 0 {
            break;
        }

        // ensure the elapsed time includes the current slot of the next wheel
        elapsed = cmp::max(elapsed, (WHEEL_LEN << (wheel * WHEEL_BITS)) as u64);
    }

    result
}

#[cfg(test)]
fn need_resched_simple(curtime: u64, newtime: u64) -> [u64; WHEEL_NUM] {
    let mut result = [0; WHEEL_NUM];

    // no time elapsed
    if newtime <= curtime {
        return result;
    }

    for curtime in curtime..newtime {
        for wheel in 0..WHEEL_NUM {
            let trunc_bits = (wheel * WHEEL_BITS) as u64;

            let old_slot = (curtime >> trunc_bits) & WHEEL_MASK;
            let new_slot = ((curtime + 1) >> trunc_bits) & WHEEL_MASK;

            if old_slot != new_slot {
                if wheel > 0 {
                    result[wheel] |= 1 << old_slot;
                } else {
                    result[wheel] |= 1 << new_slot;
                }
            }
        }
    }

    result
}

enum InList {
    Wheel(usize, usize),
    Expired,
}

struct Timer {
    expires: u64,
    list: Option<InList>,
    user_data: usize,
}

pub struct TimerWheel {
    nodes: Slab<list::Node<Timer>>,
    wheel: [[list::List; WHEEL_LEN]; WHEEL_NUM],
    expired: list::List,
    pending: [u64; WHEEL_NUM],
    curtime: u64,
}

impl TimerWheel {
    pub fn new(capacity: usize) -> Self {
        Self {
            nodes: Slab::with_capacity(capacity),
            wheel: [[list::List::default(); WHEEL_LEN]; WHEEL_NUM],
            expired: list::List::default(),
            pending: [0; WHEEL_NUM],
            curtime: 0,
        }
    }

    pub fn add(&mut self, expires: u64, user_data: usize) -> Result<usize, ()> {
        if self.nodes.len() == self.nodes.capacity() {
            return Err(());
        }

        let t = Timer {
            expires,
            list: None,
            user_data,
        };

        let key = self.nodes.insert(list::Node::new(t));

        self.sched(key);

        Ok(key)
    }

    pub fn remove(&mut self, key: usize) {
        let n = match self.nodes.get(key) {
            Some(n) => n,
            None => return,
        };

        match n.value.list {
            Some(InList::Wheel(wheel, slot)) => {
                let l = &mut self.wheel[wheel][slot];
                l.remove(&mut self.nodes, key);

                if l.is_empty() {
                    self.pending[wheel] &= !(1 << slot);
                }
            }
            Some(InList::Expired) => {
                self.expired.remove(&mut self.nodes, key);
            }
            None => {}
        }

        self.nodes.remove(key);
    }

    pub fn timeout(&self) -> Option<u64> {
        if !self.expired.is_empty() {
            return Some(0);
        }

        let mut timeout = None;
        let mut relmask = 0;

        for wheel in 0..WHEEL_NUM {
            // we only care about the highest bits
            let trunc_bits = (wheel * WHEEL_BITS) as u64;

            if self.pending[wheel] != 0 {
                let slot = ((self.curtime >> trunc_bits) & WHEEL_MASK) as usize;

                let pending = self.pending[wheel].rotate_right(slot as u32);

                // for higher order wheels, timeouts are one step in the future
                let offset = if wheel > 0 { 1 } else { 0 };

                // pending is guaranteed to be non-zero
                let t = ((pending.trailing_zeros() as u64) + offset) << trunc_bits;

                // reduce by how much lower wheels have progressed
                let t = t - (relmask & self.curtime);

                timeout = Some(match timeout {
                    Some(best) => cmp::min(best, t),
                    None => t,
                });
            }

            relmask <<= WHEEL_BITS;
            relmask |= WHEEL_MASK;
        }

        timeout
    }

    pub fn update(&mut self, curtime: u64) {
        // time must go forward
        if curtime <= self.curtime {
            return;
        }

        let need = need_resched(self.curtime, curtime);

        let mut l = list::List::default();

        for wheel in 0..WHEEL_NUM {
            let pending = need[wheel];

            // loop as long as we still have slots to process
            while pending & self.pending[wheel] != 0 {
                // get rightmost (earliest) slot that needs processing
                let slot = (pending & self.pending[wheel]).trailing_zeros() as usize;

                // move the timers out
                l.concat(&mut self.nodes, &mut self.wheel[wheel][slot]);
                self.pending[wheel] &= !(1 << slot);
            }
        }

        self.curtime = curtime;

        while let Some(key) = l.head {
            l.remove(&mut self.nodes, key);

            let n = &mut self.nodes[key];
            n.value.list = None;

            self.sched(key);
        }
    }

    pub fn take_expired(&mut self) -> Option<(usize, usize)> {
        match self.expired.pop_front(&mut self.nodes) {
            Some(key) => {
                let n = &self.nodes[key];
                let user_data = n.value.user_data;

                self.nodes.remove(key);

                Some((key, user_data))
            }
            None => None,
        }
    }

    fn sched(&mut self, key: usize) {
        let n = &self.nodes[key];
        let expires = n.value.expires;

        if expires > self.curtime {
            // get relative timeout, capped
            let t = cmp::min(expires - self.curtime, TIMEOUT_MAX);
            assert!(t > 0);

            // wheel is selected by relative time
            // t =    0 = not valid
            // t =    1 = 0b0_000000_000001 -> fls  1, wheel 0
            // t =   63 = 0b0_000000_111111 -> fls  6, wheel 0
            // t =   64 = 0b0_000001_000000 -> fls  7, wheel 1
            // t = 4032 = 0b0_111111_000000 -> fls 12, wheel 1
            // t = 4095 = 0b0_111111_111111 -> fls 12, wheel 1
            // t = 4096 = 0b1_000000_000000 -> fls 13, wheel 2
            let wheel = ((fls64(t) - 1) as usize) / WHEEL_BITS;
            assert!(wheel < WHEEL_NUM);

            // we only care about the highest bits
            let trunc_bits = (wheel * WHEEL_BITS) as u64;

            // for higher order wheels, schedule 1 slot early. this way, fractional
            //   time remaining can be rescheduled to a lower wheel
            let offset = if wheel > 0 { 1 } else { 0 };

            // slot is selected by absolute time
            let slot = (((expires >> trunc_bits) - offset) & WHEEL_MASK) as usize;

            self.wheel[wheel][slot].push_back(&mut self.nodes, key);
            self.pending[wheel] |= 1 << slot;

            let n = &mut self.nodes[key];
            n.value.list = Some(InList::Wheel(wheel, slot));
        } else {
            self.expired.push_back(&mut self.nodes, key);

            let n = &mut self.nodes[key];
            n.value.list = Some(InList::Expired);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // convert string time of the form "x:x:x:x", where each part is a number between 0-63
    fn ts(s: &str) -> u64 {
        let mut result = 0;

        for (i, part) in s.rsplit(":").enumerate() {
            let x: u64 = part.parse().unwrap();
            assert!(x <= (WHEEL_MAX as u64));

            result |= x << (i * WHEEL_BITS);
        }

        result
    }

    // convert string range to bits
    fn r2b(s: &str) -> u64 {
        let mut it = s.split("-");

        let start = it.next().unwrap();
        let end = it.next().unwrap();

        assert_eq!(it.next(), None);

        let mut pos: u64 = start.parse().unwrap();
        let end: u64 = end.parse().unwrap();

        let mut result = 0;

        loop {
            result |= 1 << pos;

            if pos == end {
                break;
            }

            pos = (pos + 1) & WHEEL_MASK;
        }

        result
    }

    // convert wheel ranges of the form "x:x:x:x", where each part is a range
    fn r2w(s: &str) -> [u64; WHEEL_NUM] {
        let mut result = [0; WHEEL_NUM];

        for (i, part) in s.rsplit(":").enumerate() {
            if !part.is_empty() {
                result[i] = r2b(part);
            }
        }

        result
    }

    #[test]
    fn test_fls() {
        assert_eq!(fls64(0), 0);
        assert_eq!(fls64(0b1), 1);
        assert_eq!(fls64(0b10), 2);
        assert_eq!(fls64(0x4000000000000000), 63);
        assert_eq!(fls64(0x8000000000000000), 64);
    }

    #[test]
    fn test_sched() {
        let mut w = TimerWheel::new(10);

        w.update(7);

        // expired
        let t1 = w.add(0b0_000000, 1).unwrap();

        // wheel 0 slot 8 (1 tick away)
        let t2 = w.add(0b0_001000, 1).unwrap();

        // wheel 0 slot 63 (56 ticks away)
        let t3 = w.add(0b0_111111, 1).unwrap();

        // wheel 0 slot 0 (57 ticks away)
        let t4 = w.add(0b1_000000, 1).unwrap();

        // wheel 1 slot 0
        let t5 = w.add(0b1_001000, 1).unwrap();

        // wheel 3 slot 63
        let t6 = w.add(0b1_000000_000000_000000_000000, 1).unwrap();

        assert_eq!(w.expired.head, Some(t1));
        assert_eq!(w.wheel[0][8].head, Some(t2));
        assert_eq!(w.wheel[0][63].head, Some(t3));
        assert_eq!(w.wheel[0][0].head, Some(t4));
        assert_eq!(w.wheel[1][0].head, Some(t5));
        assert_eq!(w.wheel[3][63].head, Some(t6));
    }

    #[test]
    fn test_need_resched() {
        struct Test {
            curtime: &'static str,
            newtime: &'static str,
            expected: &'static str,
        }

        fn t(curtime: &'static str, newtime: &'static str, expected: &'static str) -> Test {
            Test {
                curtime,
                newtime,
                expected,
            }
        }

        let table = [
            t("00:00", "00:00", ""),
            t("00:00", "00:01", "01-01"),
            t("00:01", "00:02", "02-02"),
            t("00:02", "00:63", "03-63"),
            t("00:63", "01:00", "00-00:00-00"),
            t("01:00", "01:02", "01-02"),
            t("01:02", "05:01", "01-04:00-63"),
            t("05:01", "05:02", "02-02"),
            t("05:02", "06:01", "05-05:03-01"),
            t("00:63:63", "01:00:00", "00-00:63-63:00-00"),
            t("08:00:00", "08:01:00", "00-00:00-63"),
            t("04:00:02", "05:01:00", "04-04:00-63:00-63"),
            t("04:01:02", "05:00:00", "04-04:01-63:00-63"),
            t("04:00:03", "05:00:00", "04-04:00-63:00-63"),
            t("04:00:02", "05:00:00", "04-04:00-63:00-63"),
            t("08:00:19", "08:62:63", "00-61:00-63"),
            t("08:00:19", "08:63:63", "00-62:00-63"),
            t("09:00:00", "09:63:62", "00-62:00-63"),
        ];

        for (row, t) in table.iter().enumerate() {
            let curtime = ts(t.curtime);
            let newtime = ts(t.newtime);
            let expected = r2w(t.expected);

            // ensure the simple algorithm returns what we expect
            assert_eq!(
                need_resched_simple(curtime, newtime),
                expected,
                "row={} curtime={} newtime={}",
                row,
                curtime,
                newtime
            );

            // ensure the optimized algorithm returns matching results
            assert_eq!(
                need_resched(curtime, newtime),
                expected,
                "row={} curtime={} newtime={}",
                row,
                curtime,
                newtime
            );
        }
    }

    #[test]
    fn test_rotate() {
        // test full rotations through wheels 0 and 1, and one step of wheel 2
        let count = (64 * 64) + 1;

        let mut w = TimerWheel::new(count);

        for i in 0..count {
            w.add(i as u64, i).unwrap();
        }

        for i in 0..count {
            let (_, v) = w.take_expired().unwrap();
            assert_eq!(v, i);
            assert_eq!(w.take_expired(), None);

            w.update((i + 1) as u64);
        }

        assert_eq!(w.take_expired(), None);
    }

    #[test]
    fn test_wheel() {
        let mut w = TimerWheel::new(10);

        assert_eq!(w.timeout(), None);
        assert_eq!(w.take_expired(), None);

        let t1 = w.add(4, 1).unwrap();
        assert_eq!(w.timeout(), Some(4));

        w.remove(t1);
        assert_eq!(w.timeout(), None);

        w.update(5);
        assert_eq!(w.take_expired(), None);

        let t2 = w.add(8, 2).unwrap();
        assert_eq!(w.timeout(), Some(3));

        w.update(7);
        assert_eq!(w.timeout(), Some(1));
        assert_eq!(w.take_expired(), None);

        w.update(8);
        assert_eq!(w.timeout(), Some(0));
        assert_eq!(w.take_expired(), Some((t2, 2)));
        assert_eq!(w.take_expired(), None);

        for i in 0..2 {
            let base = i * 20_000_000;

            let t1 = w.add(base + 1, 1).unwrap();
            let t2 = w.add(base + 10, 2).unwrap();
            let t3 = w.add(base + 1_000, 3).unwrap();
            let t4 = w.add(base + 100_000, 4).unwrap();
            let t5 = w.add(base + 10_000_000, 5).unwrap();

            w.update(base + 100);
            assert_eq!(w.timeout(), Some(0));
            assert_eq!(w.take_expired(), Some((t1, 1)));
            assert_eq!(w.take_expired(), Some((t2, 2)));
            assert_eq!(w.take_expired(), None);
            assert!(w.timeout().unwrap() <= 900);

            w.update(base + 2_000);
            assert_eq!(w.timeout(), Some(0));
            assert_eq!(w.take_expired(), Some((t3, 3)));
            assert_eq!(w.take_expired(), None);
            assert!(w.timeout().unwrap() <= 98_000);

            w.update(base + 200_000);
            assert_eq!(w.timeout(), Some(0));
            assert_eq!(w.take_expired(), Some((t4, 4)));
            assert_eq!(w.take_expired(), None);
            assert!(w.timeout().unwrap() <= 9_800_000);

            w.update(base + 12_000_000);
            assert_eq!(w.timeout(), Some(0));
            assert_eq!(w.take_expired(), Some((t5, 5)));
            assert_eq!(w.take_expired(), None);
            assert_eq!(w.timeout(), None);
        }
    }
}
