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
use pushpin::core::memorypool;
use std::cell::RefCell;
use std::mem;
use std::rc::Rc;

fn bench_memorypool_rc_new<const N: usize>(c: &mut Criterion, op_kcount: usize) {
    let op_count = op_kcount * 1000;
    let bytes = mem::size_of::<[u64; N]>();
    let memory = Rc::new(memorypool::RcMemory::new(op_count));
    let instances = RefCell::new(Vec::with_capacity(op_count));

    c.bench_function(&format!("mp-rc-new-{bytes}b-x{op_kcount}k"), |b| {
        b.iter_batched_ref(
            || instances.borrow_mut().clear(),
            |_| {
                let instances = &mut *instances.borrow_mut();
                let mut next_value: [u64; N] = [0; N];
                while next_value[0] < op_count as u64 {
                    let n = memorypool::Rc::try_new_in(next_value, &memory).unwrap();
                    instances.push(n);
                    next_value[0] += 1;
                }
            },
            BatchSize::PerIteration,
        )
    });
}

fn bench_system_rc_new<const N: usize>(c: &mut Criterion, op_kcount: usize) {
    let op_count = op_kcount * 1000;
    let bytes = mem::size_of::<[u64; N]>();
    let instances = RefCell::new(Vec::with_capacity(op_count));

    c.bench_function(&format!("sys-rc-new-{bytes}b-x{op_kcount}k"), |b| {
        b.iter_batched_ref(
            || instances.borrow_mut().clear(),
            |_| {
                let instances = &mut *instances.borrow_mut();
                let mut next_value: [u64; N] = [0; N];
                while next_value[0] < op_count as u64 {
                    let n = memorypool::Rc::new(next_value);
                    instances.push(n);
                    next_value[0] += 1;
                }
            },
            BatchSize::PerIteration,
        )
    });
}

fn bench_std_rc_new<const N: usize>(c: &mut Criterion, op_kcount: usize) {
    let op_count = op_kcount * 1000;
    let bytes = mem::size_of::<[u64; N]>();
    let instances = RefCell::new(Vec::with_capacity(op_count));

    c.bench_function(&format!("std-rc-new-{bytes}b-x{op_kcount}k"), |b| {
        b.iter_batched_ref(
            || instances.borrow_mut().clear(),
            |_| {
                let instances = &mut *instances.borrow_mut();
                let mut next_value: [u64; N] = [0; N];
                while next_value[0] < op_count as u64 {
                    let n = Rc::new(next_value);
                    instances.push(n);
                    next_value[0] += 1;
                }
            },
            BatchSize::PerIteration,
        )
    });
}

fn bench_memorypool_rc_drop<const N: usize>(c: &mut Criterion, op_kcount: usize) {
    let op_count = op_kcount * 1000;
    let bytes = mem::size_of::<[u64; N]>();
    let memory = Rc::new(memorypool::RcMemory::new(op_count));
    let instances = RefCell::new(Vec::with_capacity(op_count));

    c.bench_function(&format!("mp-rc-drp-{bytes}b-x{op_kcount}k"), |b| {
        b.iter_batched_ref(
            || {
                let instances = &mut *instances.borrow_mut();
                let mut next_value: [u64; N] = [0; N];
                while next_value[0] < op_count as u64 {
                    let n = memorypool::Rc::try_new_in(next_value, &memory).unwrap();
                    instances.push(n);
                    next_value[0] += 1;
                }
            },
            |_| instances.borrow_mut().clear(),
            BatchSize::PerIteration,
        )
    });
}

fn bench_system_rc_drop<const N: usize>(c: &mut Criterion, op_kcount: usize) {
    let op_count = op_kcount * 1000;
    let bytes = mem::size_of::<[u64; N]>();
    let instances = RefCell::new(Vec::with_capacity(op_count));

    c.bench_function(&format!("sys-rc-drp-{bytes}b-x{op_kcount}k"), |b| {
        b.iter_batched_ref(
            || {
                let instances = &mut *instances.borrow_mut();
                let mut next_value: [u64; N] = [0; N];
                while next_value[0] < op_count as u64 {
                    let n = memorypool::Rc::new(next_value);
                    instances.push(n);
                    next_value[0] += 1;
                }
            },
            |_| instances.borrow_mut().clear(),
            BatchSize::PerIteration,
        )
    });
}

fn bench_std_rc_drop<const N: usize>(c: &mut Criterion, op_kcount: usize) {
    let op_count = op_kcount * 1000;
    let bytes = mem::size_of::<[u64; N]>();
    let instances = RefCell::new(Vec::with_capacity(op_count));

    c.bench_function(&format!("std-rc-drp-{bytes}b-x{op_kcount}k"), |b| {
        b.iter_batched_ref(
            || {
                let instances = &mut *instances.borrow_mut();
                let mut next_value: [u64; N] = [0; N];
                while next_value[0] < op_count as u64 {
                    let n = Rc::new(next_value);
                    instances.push(n);
                    next_value[0] += 1;
                }
            },
            |_| instances.borrow_mut().clear(),
            BatchSize::PerIteration,
        )
    });
}

fn criterion_benchmark(c: &mut Criterion) {
    const OP_KCOUNT: usize = 100;
    const OP_COUNT: usize = OP_KCOUNT * 1000;

    {
        // Preallocate the instances
        let memory = Rc::new(memorypool::RcMemory::new(OP_COUNT));
        let mut instances = Vec::new();
        let mut next_value: u64 = 0;
        while next_value < OP_COUNT as u64 {
            let n = memorypool::Rc::try_new_in(next_value, &memory).unwrap();
            instances.push(n);
            next_value += 1;
        }

        let clones = RefCell::new(Vec::with_capacity(instances.len()));

        c.bench_function(&format!("mp-rc-clone-x{OP_KCOUNT}k"), |b| {
            b.iter_batched_ref(
                || clones.borrow_mut().clear(),
                |_| {
                    let clones = &mut *clones.borrow_mut();
                    for r in &instances {
                        clones.push(memorypool::Rc::clone(&r));
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

        c.bench_function(&format!("std-rc-clone-x{OP_KCOUNT}k"), |b| {
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
        let memory = Rc::new(memorypool::RcMemory::new(OP_COUNT));
        let mut instances = Vec::new();
        let mut next_value: u64 = 0;
        while next_value < OP_COUNT as u64 {
            let n = memorypool::Rc::try_new_in(next_value, &memory).unwrap();
            instances.push(n);
            next_value += 1;
        }

        let clones = RefCell::new(Vec::with_capacity(instances.len()));

        c.bench_function(&format!("mp-rc-dec-x{OP_KCOUNT}k"), |b| {
            b.iter_batched_ref(
                || {
                    let clones = &mut *clones.borrow_mut();
                    for r in &instances {
                        clones.push(memorypool::Rc::clone(r))
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

        c.bench_function(&format!("std-rc-dec-x{OP_KCOUNT}k"), |b| {
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
        let memory = Rc::new(memorypool::RcMemory::new(OP_COUNT));
        let mut instances = Vec::new();
        let mut next_value: u64 = 0;
        while next_value < OP_COUNT as u64 {
            let n = memorypool::Rc::try_new_in(next_value, &memory).unwrap();
            instances.push(n);
            next_value += 1;
        }

        c.bench_function(&format!("mp-rc-deref-x{OP_KCOUNT}k"), |b| {
            b.iter(|| {
                for (expected_value, r) in instances.iter().enumerate() {
                    assert_eq!(**r, expected_value as u64);
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

        c.bench_function(&format!("std-rc-deref-x{OP_KCOUNT}k"), |b| {
            b.iter(|| {
                for (expected_value, r) in instances.iter().enumerate() {
                    assert_eq!(**r, expected_value as u64);
                }
            })
        });
    }

    bench_memorypool_rc_new::<1>(c, OP_KCOUNT);
    bench_system_rc_new::<1>(c, OP_KCOUNT);
    bench_std_rc_new::<1>(c, OP_KCOUNT);

    bench_memorypool_rc_drop::<1>(c, OP_KCOUNT);
    bench_system_rc_drop::<1>(c, OP_KCOUNT);
    bench_std_rc_drop::<1>(c, OP_KCOUNT);

    bench_memorypool_rc_new::<80>(c, OP_KCOUNT);
    bench_std_rc_new::<80>(c, OP_KCOUNT);

    bench_memorypool_rc_drop::<80>(c, OP_KCOUNT);
    bench_std_rc_drop::<80>(c, OP_KCOUNT);

    bench_memorypool_rc_new::<160>(c, OP_KCOUNT);
    bench_std_rc_new::<160>(c, OP_KCOUNT);

    bench_memorypool_rc_drop::<160>(c, OP_KCOUNT);
    bench_std_rc_drop::<160>(c, OP_KCOUNT);
}

criterion_group!(benches, criterion_benchmark);
criterion_main!(benches);
