/*
 * Copyright (c) 2016 Alex Crichton
 * Copyright (c) 2017 The Tokio Authors
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

use std::{
    cell::Cell,
    collections::hash_map::DefaultHasher,
    hash::Hasher,
    num::Wrapping,
    sync::atomic::{AtomicUsize, Ordering},
};

// Based on [Fisher–Yates shuffle].
//
// [Fisher–Yates shuffle]: https://en.wikipedia.org/wiki/Fisher–Yates_shuffle
pub fn shuffle<T>(slice: &mut [T]) {
    for i in (1..slice.len()).rev() {
        slice.swap(i, gen_index(i + 1));
    }
}

/// Return a value from `0..n`.
fn gen_index(n: usize) -> usize {
    (random() % n as u64) as usize
}

/// Pseudorandom number generator based on [xorshift*].
///
/// [xorshift*]: https://en.wikipedia.org/wiki/Xorshift#xorshift*
pub fn random() -> u64 {
    thread_local! {
        static RNG: Cell<Wrapping<u64>> = Cell::new(Wrapping(prng_seed()));
    }

    fn prng_seed() -> u64 {
        static COUNTER: AtomicUsize = AtomicUsize::new(0);

        // Any non-zero seed will do
        let mut seed = 0;
        while seed == 0 {
            let mut hasher = DefaultHasher::new();
            hasher.write_usize(COUNTER.fetch_add(1, Ordering::Relaxed));
            seed = hasher.finish();
        }
        seed
    }

    RNG.with(|rng| {
        let mut x = rng.get();
        debug_assert_ne!(x.0, 0);
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        rng.set(x);
        x.0.wrapping_mul(0x2545_f491_4f6c_dd1d)
    })
}
