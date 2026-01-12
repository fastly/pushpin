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

use criterion::{criterion_group, criterion_main, BatchSize, Criterion};
use pushpin::core::arena;
use std::cell::RefCell;
use std::rc::Rc;

fn criterion_benchmark(c: &mut Criterion) {
    const OP_COUNT: usize = 100000;
    const MEDIUM_ALLOC_OP_COUNT: usize = 100000;

    {
        // Preallocate the instances
        let memory = Rc::new(arena::RcMemory::new(OP_COUNT));
        let mut instances = Vec::new();
        let mut next_value: u64 = 0;
        while next_value < OP_COUNT as u64 {
            let n = arena::Rc::new(next_value, &memory).unwrap();
            instances.push(n);
            next_value += 1;
        }

        let clones = RefCell::new(Vec::with_capacity(instances.len()));

        c.bench_function(&format!("arena rc clone x{OP_COUNT}"), |b| {
            b.iter_batched_ref(
                || clones.borrow_mut().clear(),
                |_| {
                    let clones = &mut *clones.borrow_mut();
                    for r in &instances {
                        clones.push(arena::Rc::clone(&r));
                    }
                },
                BatchSize::PerIteration,
            )
        });
    }

    {
        // Preallocate the instances
        let mut instances = Vec::new();
        let mut next_value: u64 = 0;
        while next_value < OP_COUNT as u64 {
            let n = Rc::new(next_value);
            instances.push(n);
            next_value += 1;
        }

        let clones = RefCell::new(Vec::with_capacity(instances.len()));

        c.bench_function(&format!("std rc clone x{OP_COUNT}"), |b| {
            b.iter_batched_ref(
                || clones.borrow_mut().clear(),
                |_| {
                    let clones = &mut *clones.borrow_mut();
                    for r in &instances {
                        clones.push(Rc::clone(&r));
                    }
                },
                BatchSize::PerIteration,
            )
        });
    }

    {
        // Preallocate the instances
        let memory = Rc::new(arena::RcMemory::new(OP_COUNT));
        let mut instances = Vec::new();
        let mut next_value: u64 = 0;
        while next_value < OP_COUNT as u64 {
            let n = arena::Rc::new(next_value, &memory).unwrap();
            instances.push(n);
            next_value += 1;
        }

        let clones = RefCell::new(Vec::with_capacity(instances.len()));

        c.bench_function(&format!("arena rc dec x{OP_COUNT}"), |b| {
            b.iter_batched_ref(
                || {
                    let clones = &mut *clones.borrow_mut();
                    for r in &instances {
                        clones.push(arena::Rc::clone(r))
                    }
                },
                |_| clones.borrow_mut().clear(),
                BatchSize::PerIteration,
            )
        });
    }

    {
        // Preallocate the instances
        let mut instances = Vec::new();
        let mut next_value: u64 = 0;
        while next_value < OP_COUNT as u64 {
            let n = Rc::new(next_value);
            instances.push(n);
            next_value += 1;
        }

        let clones = RefCell::new(Vec::with_capacity(instances.len()));

        c.bench_function(&format!("std rc dec x{OP_COUNT}"), |b| {
            b.iter_batched_ref(
                || {
                    let clones = &mut *clones.borrow_mut();
                    for r in &instances {
                        clones.push(Rc::clone(r))
                    }
                },
                |_| clones.borrow_mut().clear(),
                BatchSize::PerIteration,
            )
        });
    }

    {
        // Preallocate the instances
        let memory = Rc::new(arena::RcMemory::new(OP_COUNT));
        let mut instances = Vec::new();
        let mut next_value: u64 = 0;
        while next_value < OP_COUNT as u64 {
            let n = arena::Rc::new(next_value, &memory).unwrap();
            instances.push(n);
            next_value += 1;
        }

        c.bench_function(&format!("arena rc deref x{OP_COUNT}"), |b| {
            b.iter(|| {
                for (expected_value, r) in instances.iter().enumerate() {
                    assert_eq!(*r.get(), expected_value as u64);
                }
            })
        });
    }

    {
        // Preallocate the instances
        let mut instances = Vec::new();
        let mut next_value: u64 = 0;
        while next_value < OP_COUNT as u64 {
            let n = Rc::new(next_value);
            instances.push(n);
            next_value += 1;
        }

        c.bench_function(&format!("std rc deref x{OP_COUNT}"), |b| {
            b.iter(|| {
                for (expected_value, r) in instances.iter().enumerate() {
                    assert_eq!(**r, expected_value as u64);
                }
            })
        });
    }

    {
        let memory = Rc::new(arena::RcMemory::new(OP_COUNT));
        let mut instances = Vec::new();

        c.bench_function(&format!("arena rc new/drop x{OP_COUNT}"), |b| {
            b.iter(|| {
                let mut next_value: u64 = 0;
                while next_value < OP_COUNT as u64 {
                    let n = arena::Rc::new(next_value, &memory).unwrap();
                    instances.push(n);
                    next_value += 1;
                }

                instances.clear();
            })
        });
    }

    {
        let mut instances = Vec::new();

        c.bench_function(&format!("std rc new/drop x{OP_COUNT}"), |b| {
            b.iter(|| {
                let mut next_value: u64 = 0;
                while next_value < OP_COUNT as u64 {
                    let n = Rc::new(next_value);
                    instances.push(n);
                    next_value += 1;
                }

                instances.clear();
            })
        });
    }

    {
        let memory = Rc::new(arena::RcMemory::new(MEDIUM_ALLOC_OP_COUNT));
        let mut instances = Vec::with_capacity(MEDIUM_ALLOC_OP_COUNT);

        c.bench_function(
            &format!("arena rc new/drop medium x{MEDIUM_ALLOC_OP_COUNT}"),
            |b| {
                b.iter(|| {
                    let mut next_value: [u64; 80] = [0; 80];
                    while next_value[0] < MEDIUM_ALLOC_OP_COUNT as u64 {
                        let n = arena::Rc::new(next_value, &memory).unwrap();
                        instances.push(n);
                        for v in &mut next_value {
                            *v += 1;
                        }
                    }

                    instances.clear();
                })
            },
        );
    }

    {
        let mut instances = Vec::with_capacity(MEDIUM_ALLOC_OP_COUNT);

        c.bench_function(
            &format!("std rc new/drop medium x{MEDIUM_ALLOC_OP_COUNT}"),
            |b| {
                b.iter(|| {
                    let mut next_value: [u64; 80] = [0; 80];
                    while next_value[0] < MEDIUM_ALLOC_OP_COUNT as u64 {
                        let n = Rc::new(next_value);
                        instances.push(n);
                        for v in &mut next_value {
                            *v += 1;
                        }
                    }

                    instances.clear();
                })
            },
        );
    }
}

criterion_group!(benches, criterion_benchmark);
criterion_main!(benches);
