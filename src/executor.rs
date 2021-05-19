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
use std::mem;
use std::pin::Pin;
use std::rc::{Rc, Weak};
use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};

thread_local! {
    static EXECUTOR: RefCell<Option<Weak<Tasks>>> = RefCell::new(None);
}

struct WakerData {
    tasks: Weak<Tasks>,
    task_id: usize,
}

impl WakerData {
    fn as_std_waker(self: &Rc<WakerData>) -> Waker {
        unsafe { Waker::from_raw(raw_waker(Rc::clone(self))) }
    }
}

fn raw_waker(waker: Rc<WakerData>) -> RawWaker {
    unsafe fn clone_waker(data: *const ()) -> RawWaker {
        let waker = mem::ManuallyDrop::new(Rc::from_raw(data as *const WakerData));

        let waker = Rc::clone(&waker);

        RawWaker::new(
            Rc::into_raw(waker) as *const (),
            &RawWakerVTable::new(clone_waker, wake, wake_by_ref, drop_waker),
        )
    }

    unsafe fn wake(data: *const ()) {
        let waker = Rc::from_raw(data as *const WakerData);

        if let Some(tasks) = waker.tasks.upgrade() {
            tasks.wake(waker.task_id);
        }
    }

    unsafe fn wake_by_ref(data: *const ()) {
        let waker = mem::ManuallyDrop::new(Rc::from_raw(data as *const WakerData));

        if let Some(tasks) = waker.tasks.upgrade() {
            tasks.wake(waker.task_id);
        }
    }

    unsafe fn drop_waker(data: *const ()) {
        Rc::from_raw(data as *const WakerData);
    }

    RawWaker::new(
        Rc::into_raw(waker) as *const (),
        &RawWakerVTable::new(clone_waker, wake, wake_by_ref, drop_waker),
    )
}

fn poll_fut(fut: &mut Pin<Box<dyn Future<Output = ()>>>, waker: Waker) -> bool {
    // convert from Pin<Box> to Pin<&mut>
    let fut: Pin<&mut dyn Future<Output = ()>> = fut.as_mut();

    let mut cx = Context::from_waker(&waker);

    match fut.poll(&mut cx) {
        Poll::Ready(_) => true,
        Poll::Pending => false,
    }
}

struct Task {
    fut: Option<Pin<Box<dyn Future<Output = ()>>>>,
}

struct TasksData {
    nodes: Slab<list::Node<Task>>,
    next: list::List,
    wakers: Vec<Rc<WakerData>>,
}

struct Tasks {
    data: RefCell<TasksData>,
}

impl Tasks {
    fn new(max: usize) -> Rc<Self> {
        let data = TasksData {
            nodes: Slab::with_capacity(max),
            next: list::List::default(),
            wakers: Vec::with_capacity(max),
        };

        let tasks = Rc::new(Self {
            data: RefCell::new(data),
        });

        {
            let data = &mut *tasks.data.borrow_mut();

            for task_id in 0..data.nodes.capacity() {
                data.wakers.push(Rc::new(WakerData {
                    tasks: Rc::downgrade(&tasks),
                    task_id,
                }));
            }
        }

        tasks
    }

    fn is_empty(&self) -> bool {
        self.data.borrow().nodes.is_empty()
    }

    fn add<F>(&self, fut: F) -> Result<(), ()>
    where
        F: Future<Output = ()> + 'static,
    {
        let data = &mut *self.data.borrow_mut();

        if data.nodes.len() == data.nodes.capacity() {
            return Err(());
        }

        let entry = data.nodes.vacant_entry();
        let nkey = entry.key();

        let task = Task {
            fut: Some(Box::pin(fut)),
        };

        entry.insert(list::Node::new(task));

        data.next.push_back(&mut data.nodes, nkey);

        Ok(())
    }

    fn remove(&self, task_id: usize) {
        let nkey = task_id;

        let data = &mut *self.data.borrow_mut();

        let task = &mut data.nodes[nkey].value;

        // drop the future. this should cause it to drop any owned wakers
        task.fut = None;

        // at this point, we should be the only remaining owner
        assert_eq!(Rc::strong_count(&data.wakers[nkey]), 1);

        data.next.remove(&mut data.nodes, nkey);
        data.nodes.remove(nkey);
    }

    fn take_next(&self) -> Option<(usize, Pin<Box<dyn Future<Output = ()>>>, Waker)> {
        let data = &mut *self.data.borrow_mut();

        let nkey = match data.next.head {
            Some(nkey) => nkey,
            None => return None,
        };

        data.next.remove(&mut data.nodes, nkey);

        let task = &mut data.nodes[nkey].value;

        // both of these are cheap
        let fut = task.fut.take().unwrap();
        let waker = data.wakers[nkey].as_std_waker();

        Some((nkey, fut, waker))
    }

    fn set_fut(&self, task_id: usize, fut: Pin<Box<dyn Future<Output = ()>>>) {
        let nkey = task_id;

        let data = &mut *self.data.borrow_mut();

        let task = &mut data.nodes[nkey].value;

        task.fut = Some(fut);
    }

    fn wake(&self, task_id: usize) {
        let nkey = task_id;

        let data = &mut *self.data.borrow_mut();

        // add node to the list if it's not already present
        if data.nodes[nkey].prev.is_none() && data.next.head != Some(nkey) {
            data.next.push_back(&mut data.nodes, nkey);
        }
    }
}

pub struct Executor {
    tasks: Rc<Tasks>,
}

impl Executor {
    pub fn new(tasks_max: usize) -> Self {
        let tasks = Tasks::new(tasks_max);

        EXECUTOR.with(|ex| {
            if ex.borrow().is_some() {
                panic!("thread already has an Executor");
            }

            ex.replace(Some(Rc::downgrade(&tasks)));
        });

        Self { tasks }
    }

    pub fn spawn<F>(&self, fut: F) -> Result<(), ()>
    where
        F: Future<Output = ()> + 'static,
    {
        self.tasks.add(fut)
    }

    pub fn have_tasks(&self) -> bool {
        !self.tasks.is_empty()
    }

    pub fn run_until_stalled(&self) {
        loop {
            match self.tasks.take_next() {
                Some((task_id, mut fut, waker)) => {
                    let done = poll_fut(&mut fut, waker);

                    // take_next() took the future out of the task, so we
                    // could poll it without having to maintain a borrow of
                    // the tasks set. we'll put it back now
                    self.tasks.set_fut(task_id, fut);

                    if done {
                        self.tasks.remove(task_id);
                    }
                }
                None => break,
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

    pub fn current() -> Option<Self> {
        EXECUTOR.with(|ex| match &mut *ex.borrow_mut() {
            Some(tasks) => Some(Self {
                tasks: tasks.upgrade().unwrap(),
            }),
            None => None,
        })
    }
}

impl Drop for Executor {
    fn drop(&mut self) {
        EXECUTOR.with(|ex| {
            if Rc::strong_count(&self.tasks) == 1 {
                ex.replace(None);
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    struct TestFutureData {
        ready: bool,
        waker: Option<Waker>,
    }

    struct TestFuture {
        data: Rc<RefCell<TestFutureData>>,
    }

    impl TestFuture {
        fn new() -> Self {
            let data = TestFutureData {
                ready: false,
                waker: None,
            };

            Self {
                data: Rc::new(RefCell::new(data)),
            }
        }

        fn handle(&self) -> TestHandle {
            TestHandle {
                data: Rc::clone(&self.data),
            }
        }
    }

    impl Future for TestFuture {
        type Output = ();

        fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
            let mut data = self.data.borrow_mut();

            match data.ready {
                true => Poll::Ready(()),
                false => {
                    data.waker = Some(cx.waker().clone());

                    Poll::Pending
                }
            }
        }
    }

    struct TestHandle {
        data: Rc<RefCell<TestFutureData>>,
    }

    impl TestHandle {
        fn set_ready(&self) {
            let data = &mut *self.data.borrow_mut();

            data.ready = true;

            if let Some(waker) = data.waker.take() {
                waker.wake();
            }
        }
    }

    #[test]
    fn test_executor_step() {
        let executor = Executor::new(1);

        let fut1 = TestFuture::new();
        let fut2 = TestFuture::new();

        let handle1 = fut1.handle();
        let handle2 = fut2.handle();

        let started = Rc::new(Cell::new(false));
        let fut1_done = Rc::new(Cell::new(false));
        let finishing = Rc::new(Cell::new(false));

        {
            let started = Rc::clone(&started);
            let fut1_done = Rc::clone(&fut1_done);
            let finishing = Rc::clone(&finishing);

            executor
                .spawn(async move {
                    started.set(true);

                    fut1.await;

                    fut1_done.set(true);

                    fut2.await;

                    finishing.set(true);
                })
                .unwrap();
        }

        // not started yet, no progress
        assert_eq!(executor.have_tasks(), true);
        assert_eq!(started.get(), false);

        executor.run_until_stalled();

        // started, but fut1 not ready
        assert_eq!(executor.have_tasks(), true);
        assert_eq!(started.get(), true);
        assert_eq!(fut1_done.get(), false);

        handle1.set_ready();
        executor.run_until_stalled();

        // fut1 finished
        assert_eq!(executor.have_tasks(), true);
        assert_eq!(fut1_done.get(), true);
        assert_eq!(finishing.get(), false);

        handle2.set_ready();
        executor.run_until_stalled();

        // fut2 finished, and thus the task finished
        assert_eq!(finishing.get(), true);
        assert_eq!(executor.have_tasks(), false);
    }

    #[test]
    fn test_executor_run() {
        let executor = Executor::new(1);

        let fut = TestFuture::new();
        let handle = fut.handle();

        executor
            .spawn(async move {
                fut.await;
            })
            .unwrap();

        executor
            .run(|| {
                handle.set_ready();

                Ok(())
            })
            .unwrap();

        assert_eq!(executor.have_tasks(), false);
    }

    #[test]
    fn test_executor_spawn_error() {
        let executor = Executor::new(1);

        assert!(executor.spawn(async {}).is_ok());
        assert!(executor.spawn(async {}).is_err());
    }

    #[test]
    fn test_executor_current() {
        assert!(Executor::current().is_none());

        let executor = Executor::new(2);

        let flag = Rc::new(Cell::new(false));

        {
            let flag = flag.clone();

            executor
                .spawn(async move {
                    Executor::current()
                        .unwrap()
                        .spawn(async move {
                            flag.set(true);
                        })
                        .unwrap();
                })
                .unwrap();
        }

        assert_eq!(flag.get(), false);

        executor.run(|| Ok(())).unwrap();

        assert_eq!(flag.get(), true);

        let current = Executor::current().unwrap();

        assert_eq!(executor.have_tasks(), false);
        assert!(current.spawn(async {}).is_ok());
        assert_eq!(executor.have_tasks(), true);

        mem::drop(executor);

        assert!(Executor::current().is_some());

        mem::drop(current);

        assert!(Executor::current().is_none());
    }
}
