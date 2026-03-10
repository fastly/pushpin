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

use criterion::{criterion_group, criterion_main, Criterion};
use pushpin::core::list;
use pushpin::core::memorypool;
use slab::Slab;
use std::rc::Rc;

fn criterion_benchmark(c: &mut Criterion) {
    const NODE_KCOUNT: usize = 10;

    {
        // Preallocate the nodes memory
        let mut nodes_slab = Some(Slab::with_capacity(NODE_KCOUNT * 1000));

        c.bench_function(&format!("slab-push-pop-x{NODE_KCOUNT}k"), |b| {
            b.iter(|| {
                let mut nodes = nodes_slab.take().unwrap();
                let mut l = list::List::default();

                let mut next_value: u64 = 0;
                while nodes.len() < nodes.capacity() {
                    let n = nodes.insert(list::Node::new(next_value));
                    l.push_back(&mut nodes, n);
                    next_value += 1;
                }

                let mut expected_value = 0;
                while !nodes.is_empty() {
                    let n = l.pop_front(&mut nodes).unwrap();
                    assert_eq!(nodes[n].value, expected_value);
                    nodes.remove(n);
                    expected_value += 1;
                }

                nodes_slab = Some(nodes);
            })
        });
    }

    {
        // Preallocate the nodes memory
        let mut nodes_slab = Some(Slab::with_capacity(NODE_KCOUNT));

        c.bench_function(&format!("gen-slab-push-pop-x{NODE_KCOUNT}k"), |b| {
            b.iter(|| {
                let mut nodes = nodes_slab.take().unwrap();
                let mut l = list::SlabList::default();

                let mut next_value: u64 = 0;
                while nodes.len() < nodes.capacity() {
                    let n = nodes.insert(list::SlabNode::new(next_value));
                    l.push_back(&mut nodes, n);
                    next_value += 1;
                }

                let mut expected_value = 0;
                while !nodes.is_empty() {
                    let n = l.pop_front(&mut nodes).unwrap();
                    assert_eq!(nodes[n].value, expected_value);
                    nodes.remove(n);
                    expected_value += 1;
                }

                nodes_slab = Some(nodes);
            })
        });
    }

    {
        let node_count = NODE_KCOUNT * 1000;

        // Preallocate the nodes memory
        let nodes_memory = Rc::new(memorypool::RcMemory::new(node_count));

        c.bench_function(&format!("mp-push-pop-x{NODE_KCOUNT}k"), |b| {
            b.iter(|| {
                let mut l = list::RcList::default();

                let mut next_value: u64 = 0;
                while next_value < node_count as u64 {
                    let n = list::RcNode::new(next_value, Some(&nodes_memory));
                    l.push_back(n);
                    next_value += 1;
                }

                let mut expected_value = 0;
                while expected_value < node_count as u64 {
                    let n = l.pop_front().unwrap();
                    assert_eq!(*n.value(), expected_value);
                    expected_value += 1;
                }
            })
        });
    }

    {
        let node_count = NODE_KCOUNT * 1000;

        c.bench_function(&format!("sys-push-pop-x{NODE_KCOUNT}k"), |b| {
            b.iter(|| {
                let mut l = list::RcList::default();

                let mut next_value: u64 = 0;
                while next_value < node_count as u64 {
                    let n = list::RcNode::new(next_value, None);
                    l.push_back(n);
                    next_value += 1;
                }

                let mut expected_value = 0;
                while expected_value < node_count as u64 {
                    let n = l.pop_front().unwrap();
                    assert_eq!(*n.value(), expected_value);
                    expected_value += 1;
                }
            })
        });
    }

    {
        let node_count = NODE_KCOUNT * 1000;

        // Preallocate the nodes
        let nodes_memory = Rc::new(memorypool::RcMemory::new(node_count));
        let mut nodes = Vec::new();
        let mut next_value: u64 = 0;
        while nodes_memory.len() < nodes_memory.capacity() {
            let n = list::RcNode::new(next_value, Some(&nodes_memory));
            nodes.push(n);
            next_value += 1;
        }

        c.bench_function(&format!("pre-mp-push-pop-x{NODE_KCOUNT}k"), |b| {
            b.iter(|| {
                let mut l = list::RcList::default();

                for n in &nodes {
                    l.push_back(n.clone());
                }

                let mut expected_value = 0;
                while expected_value < node_count as u64 {
                    let n = l.pop_front().unwrap();
                    assert_eq!(*n.value(), expected_value);
                    expected_value += 1;
                }
            })
        });
    }

    {
        let node_count = NODE_KCOUNT * 1000;

        // Preallocate the nodes
        let mut nodes = Vec::new();
        let mut next_value: u64 = 0;
        while next_value < node_count as u64 {
            let n = list::RcNode::new(next_value, None);
            nodes.push(n);
            next_value += 1;
        }

        c.bench_function(&format!("pre-sys-push-pop-x{NODE_KCOUNT}k"), |b| {
            b.iter(|| {
                let mut l = list::RcList::default();

                for n in &nodes {
                    l.push_back(n.clone());
                }

                let mut expected_value = 0;
                while expected_value < node_count as u64 {
                    let n = l.pop_front().unwrap();
                    assert_eq!(*n.value(), expected_value);
                    expected_value += 1;
                }
            })
        });
    }
}

criterion_group!(benches, criterion_benchmark);
criterion_main!(benches);
