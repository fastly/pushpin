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

use crate::list;
use slab::Slab;
use std::cell::RefCell;
use std::future::Future;
use std::io;
use std::pin::Pin;
use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};

struct SharedWaker<'f> {
    executor: *const Executor<'f>,
    task_id: usize,
}

impl SharedWaker<'_> {
    fn as_std_waker(&self) -> Waker {
        let executor = unsafe { self.executor.as_ref().unwrap() };

        executor.add_waker_ref(self.task_id);

        let rw = RawWaker::new(self as *const Self as *const (), Self::vtable());

        unsafe { Waker::from_raw(rw) }
    }

    unsafe fn clone(data: *const ()) -> RawWaker {
        let s = (data as *const Self).as_ref().unwrap();

        let executor = s.executor.as_ref().unwrap();

        executor.add_waker_ref(s.task_id);

        RawWaker::new(data, &Self::vtable())
    }

    unsafe fn wake(data: *const ()) {
        Self::wake_by_ref(data);

        Self::drop(data);
    }

    unsafe fn wake_by_ref(data: *const ()) {
        let s = (data as *const Self).as_ref().unwrap();

        let executor = s.executor.as_ref().unwrap();

        executor.wake(s.task_id);
    }

    unsafe fn drop(data: *const ()) {
        let s = (data as *const Self).as_ref().unwrap();

        let executor = s.executor.as_ref().unwrap();

        executor.remove_waker_ref(s.task_id);
    }

    fn vtable() -> &'static RawWakerVTable {
        &RawWakerVTable::new(Self::clone, Self::wake, Self::wake_by_ref, Self::drop)
    }
}

struct Task<'f> {
    fut: Option<Pin<Box<dyn Future<Output = ()> + 'f>>>,
    waker: SharedWaker<'f>,
    waker_refs: usize,
    awake: bool,
}

struct Tasks<'f> {
    nodes: Slab<list::Node<Task<'f>>>,
    next: list::List,
}

pub struct Executor<'f> {
    tasks: RefCell<Tasks<'f>>,
}

impl<'f> Executor<'f> {
    pub fn new(tasks_max: usize) -> Self {
        Self {
            tasks: RefCell::new(Tasks {
                nodes: Slab::with_capacity(tasks_max),
                next: list::List::default(),
            }),
        }
    }

    pub fn spawn<F>(&self, f: F) -> Result<(), ()>
    where
        F: Future<Output = ()> + 'f,
    {
        let tasks = &mut *self.tasks.borrow_mut();

        if tasks.nodes.len() == tasks.nodes.capacity() {
            return Err(());
        }

        let entry = tasks.nodes.vacant_entry();
        let key = entry.key();

        let waker = SharedWaker {
            executor: self,
            task_id: key,
        };

        let task = Task {
            fut: Some(Box::pin(f)),
            waker,
            waker_refs: 0,
            awake: true,
        };

        entry.insert(list::Node::new(task));

        tasks.next.push_back(&mut tasks.nodes, key);

        Ok(())
    }

    pub fn have_tasks(&self) -> bool {
        !self.tasks.borrow().nodes.is_empty()
    }

    pub fn run_until_stalled(&self) {
        loop {
            let (nkey, task_ptr) = {
                let tasks = &mut *self.tasks.borrow_mut();

                let nkey = match tasks.next.head {
                    Some(nkey) => nkey,
                    None => break,
                };

                tasks.next.remove(&mut tasks.nodes, nkey);

                let task = &mut tasks.nodes[nkey].value;

                task.awake = false;

                (nkey, task as *mut Task)
            };

            // task won't move/drop while this pointer is in use
            let task: &mut Task = unsafe { task_ptr.as_mut().unwrap() };

            let done = {
                let f: Pin<&mut dyn Future<Output = ()>> = task.fut.as_mut().unwrap().as_mut();

                let w = task.waker.as_std_waker();

                let mut cx = Context::from_waker(&w);

                match f.poll(&mut cx) {
                    Poll::Ready(_) => true,
                    Poll::Pending => false,
                }
            };

            if done {
                task.fut = None;

                let tasks = &mut *self.tasks.borrow_mut();

                let task = &mut tasks.nodes[nkey].value;

                assert_eq!(task.waker_refs, 0);

                tasks.next.remove(&mut tasks.nodes, nkey);
                tasks.nodes.remove(nkey);
            }
        }
    }

    pub fn run<F>(&self, mut park: F) -> Result<(), io::Error>
    where
        F: FnMut() -> Result<(), io::Error>,
    {
        loop {
            self.run_until_stalled();

            if !self.have_tasks() {
                break;
            }

            park()?;
        }

        Ok(())
    }

    fn add_waker_ref(&self, task_id: usize) {
        let tasks = &mut *self.tasks.borrow_mut();

        let task = &mut tasks.nodes[task_id].value;

        task.waker_refs += 1;
    }

    fn remove_waker_ref(&self, task_id: usize) {
        let tasks = &mut *self.tasks.borrow_mut();

        let task = &mut tasks.nodes[task_id].value;

        assert!(task.waker_refs > 0);

        task.waker_refs -= 1;
    }

    fn wake(&self, task_id: usize) {
        let tasks = &mut *self.tasks.borrow_mut();

        let task = &mut tasks.nodes[task_id].value;

        if !task.awake {
            task.awake = true;

            tasks.next.remove(&mut tasks.nodes, task_id);
            tasks.next.push_back(&mut tasks.nodes, task_id);
        }
    }
}
