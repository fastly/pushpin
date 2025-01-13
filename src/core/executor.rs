/*
 * Copyright (C) 2020-2023 Fanout, Inc.
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

use crate::core::list;
use crate::core::waker;
use log::debug;
use slab::Slab;
use std::cell::RefCell;
use std::future::Future;
use std::io;
use std::mem;
use std::pin::Pin;
use std::rc::{Rc, Weak};
use std::task::{Context, Waker};
use std::time::Duration;

thread_local! {
    static EXECUTOR: RefCell<Option<Weak<Tasks>>> = const { RefCell::new(None) };
}

type BoxFuture = Pin<Box<dyn Future<Output = ()>>>;

struct TaskWaker {
    tasks: Weak<Tasks>,
    task_id: usize,
}

impl waker::RcWake for TaskWaker {
    fn wake(self: Rc<Self>) {
        if let Some(tasks) = self.tasks.upgrade() {
            tasks.wake(self.task_id, false);
        }
    }
}

struct TaskResumeWaker {
    tasks: Weak<Tasks>,
    task_id: usize,
}

impl waker::RcWake for TaskResumeWaker {
    fn wake(self: Rc<Self>) {
        if let Some(tasks) = self.tasks.upgrade() {
            tasks.wake(self.task_id, true);
        }
    }
}

fn poll_fut(fut: &mut BoxFuture, waker: Waker) -> bool {
    // convert from Pin<Box> to Pin<&mut>
    let fut: Pin<&mut dyn Future<Output = ()>> = fut.as_mut();

    let mut cx = Context::from_waker(&waker);

    fut.poll(&mut cx).is_ready()
}

struct Task {
    fut: Option<Pin<Box<dyn Future<Output = ()>>>>,
    wakeable: bool,
    low: bool,
}

struct TasksData {
    nodes: Slab<list::Node<Task>>,
    next: list::List,
    next_low: list::List,
    wakers: Vec<Rc<TaskWaker>>,
    current_task: Option<usize>,
}

struct Tasks {
    data: RefCell<TasksData>,
    pre_poll: RefCell<Option<Box<dyn FnMut()>>>,
}

impl Tasks {
    fn new(max: usize) -> Rc<Self> {
        let data = TasksData {
            nodes: Slab::with_capacity(max),
            next: list::List::default(),
            next_low: list::List::default(),
            wakers: Vec::with_capacity(max),
            current_task: None,
        };

        let tasks = Rc::new(Self {
            data: RefCell::new(data),
            pre_poll: RefCell::new(None),
        });

        {
            let data = &mut *tasks.data.borrow_mut();

            for task_id in 0..data.nodes.capacity() {
                data.wakers.push(Rc::new(TaskWaker {
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

    fn have_next(&self) -> bool {
        !self.data.borrow().next.is_empty() || !self.data.borrow().next_low.is_empty()
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
            wakeable: false,
            low: false,
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

        if task.low {
            data.next_low.remove(&mut data.nodes, nkey);
        } else {
            data.next.remove(&mut data.nodes, nkey);
        }

        data.nodes.remove(nkey);
    }

    fn current_task(&self) -> Option<usize> {
        self.data.borrow().current_task
    }

    fn set_current_task(&self, task_id: Option<usize>) {
        self.data.borrow_mut().current_task = task_id;
    }

    fn take_next_list(&self, low: bool) -> list::List {
        let data = &mut *self.data.borrow_mut();

        let mut l = list::List::default();

        if low {
            l.concat(&mut data.nodes, &mut data.next_low);
        } else {
            l.concat(&mut data.nodes, &mut data.next);
        }

        l
    }

    fn take_task(&self, l: &mut list::List) -> Option<(usize, BoxFuture, Waker)> {
        let nkey = l.head?;

        let data = &mut *self.data.borrow_mut();

        l.remove(&mut data.nodes, nkey);

        let task = &mut data.nodes[nkey].value;

        // both of these are cheap
        let fut = task.fut.take().unwrap();
        let waker = waker::into_std(data.wakers[nkey].clone());

        task.wakeable = true;

        Some((nkey, fut, waker))
    }

    fn process_next(&self, low: bool) {
        let mut l = self.take_next_list(low);

        while let Some((task_id, mut fut, waker)) = self.take_task(&mut l) {
            self.set_current_task(Some(task_id));

            self.pre_poll();

            let done = poll_fut(&mut fut, waker);

            self.set_current_task(None);

            // take_task() took the future out of the task, so we
            // could poll it without having to maintain a borrow of
            // the tasks set. we'll put it back now
            self.set_fut(task_id, fut);

            if done {
                self.remove(task_id);
            }
        }
    }

    fn set_fut(&self, task_id: usize, fut: BoxFuture) {
        let nkey = task_id;

        let data = &mut *self.data.borrow_mut();

        let task = &mut data.nodes[nkey].value;

        task.fut = Some(fut);
    }

    fn wake(&self, task_id: usize, resume: bool) {
        let nkey = task_id;

        let data = &mut *self.data.borrow_mut();

        let task = &mut data.nodes[nkey].value;

        if !task.wakeable && !resume {
            return;
        }

        task.wakeable = false;

        if data.current_task == Some(task_id) || resume {
            // if a task triggers its own waker, queue with low priority in
            // order to achieve a yielding effect. do the same when waking
            // with resume mode, to achieve a yielding effect even when the
            // wake occurs during events processing

            task.low = true;
            data.next_low.push_back(&mut data.nodes, nkey);
        } else {
            task.low = false;
            data.next.push_back(&mut data.nodes, nkey);
        }
    }

    fn ignore_wakes(&self, task_id: usize) {
        let nkey = task_id;

        let data = &mut *self.data.borrow_mut();

        // tasks other than the current task may be in a temporary list
        // during task processing, in which case removal to prevent wakes is
        // not possible
        assert_eq!(
            data.current_task,
            Some(task_id),
            "ignore_wakes can only be self-applied"
        );

        let task = &mut data.nodes[nkey].value;

        task.wakeable = false;

        if task.low {
            data.next_low.remove(&mut data.nodes, nkey);
        } else {
            data.next.remove(&mut data.nodes, nkey);
        }
    }

    fn set_pre_poll<F>(&self, pre_poll_fn: F)
    where
        F: FnMut() + 'static,
    {
        *self.pre_poll.borrow_mut() = Some(Box::new(pre_poll_fn));
    }

    fn pre_poll(&self) {
        let pre_poll = &mut *self.pre_poll.borrow_mut();

        if let Some(f) = pre_poll {
            f();
        }
    }
}

#[derive(Debug)]
pub struct CurrentTaskError;

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

    #[allow(clippy::result_unit_err)]
    pub fn spawn<F>(&self, fut: F) -> Result<(), ()>
    where
        F: Future<Output = ()> + 'static,
    {
        debug!("spawning future with size {}", mem::size_of::<F>());

        self.tasks.add(fut)
    }

    pub fn set_pre_poll<F>(&self, pre_poll_fn: F)
    where
        F: FnMut() + 'static,
    {
        self.tasks.set_pre_poll(pre_poll_fn);
    }

    pub fn have_tasks(&self) -> bool {
        !self.tasks.is_empty()
    }

    pub fn run_until_stalled(&self) {
        while self.tasks.have_next() {
            self.tasks.process_next(false);
            self.tasks.process_next(true);
        }
    }

    pub fn run<F>(&self, mut park: F) -> Result<(), io::Error>
    where
        F: FnMut(Option<Duration>) -> Result<(), io::Error>,
    {
        loop {
            // run normal priority only
            self.tasks.process_next(false);

            if !self.have_tasks() {
                break;
            }

            let timeout = if self.tasks.have_next() {
                // some tasks trigger their own waker and return Pending in
                // order to achieve a yielding effect. in that case they will
                // already be queued up for processing again. use a timeout
                // of 0 when parking so we can quickly resume them

                let timeout = Duration::from_millis(0);

                Some(timeout)
            } else {
                None
            };

            park(timeout)?;

            // run normal priority again, in case the park triggered wakers
            self.tasks.process_next(false);

            // finally, run low priority (mainly yielding tasks)
            self.tasks.process_next(true);
        }

        Ok(())
    }

    pub fn current() -> Option<Self> {
        EXECUTOR.with(|ex| {
            (*ex.borrow_mut()).as_mut().map(|tasks| Self {
                tasks: tasks.upgrade().unwrap(),
            })
        })
    }

    pub fn spawner(&self) -> Spawner {
        Spawner {
            tasks: Rc::downgrade(&self.tasks),
        }
    }

    pub fn create_resume_waker_for_current_task(&self) -> Result<Waker, CurrentTaskError> {
        match self.tasks.current_task() {
            Some(task_id) => {
                let waker = Rc::new(TaskResumeWaker {
                    tasks: Rc::downgrade(&self.tasks),
                    task_id,
                });

                Ok(waker::into_std(waker))
            }
            None => Err(CurrentTaskError),
        }
    }

    pub fn ignore_wakes_for_current_task(&self) -> Result<(), CurrentTaskError> {
        match self.tasks.current_task() {
            Some(task_id) => {
                self.tasks.ignore_wakes(task_id);

                Ok(())
            }
            None => Err(CurrentTaskError),
        }
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

pub struct Spawner {
    tasks: Weak<Tasks>,
}

impl Spawner {
    #[allow(clippy::result_unit_err)]
    pub fn spawn<F>(&self, fut: F) -> Result<(), ()>
    where
        F: Future<Output = ()> + 'static,
    {
        let tasks = match self.tasks.upgrade() {
            Some(tasks) => tasks,
            None => return Err(()),
        };

        let ex = Executor { tasks };

        ex.spawn(fut)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;
    use std::mem;
    use std::task::Poll;

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

    struct EarlyWakeFuture {
        done: bool,
    }

    impl EarlyWakeFuture {
        fn new() -> Self {
            Self { done: false }
        }
    }

    impl Future for EarlyWakeFuture {
        type Output = ();

        fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
            if !self.done {
                self.done = true;
                cx.waker().wake_by_ref();

                return Poll::Pending;
            }

            Poll::Ready(())
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
            .run(|_| {
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

        executor.run(|_| Ok(())).unwrap();

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

    #[test]
    fn test_executor_spawner() {
        let executor = Executor::new(2);

        let flag = Rc::new(Cell::new(false));

        {
            let flag = flag.clone();
            let spawner = executor.spawner();

            executor
                .spawn(async move {
                    spawner
                        .spawn(async move {
                            flag.set(true);
                        })
                        .unwrap();
                })
                .unwrap();
        }

        assert_eq!(flag.get(), false);

        executor.run(|_| Ok(())).unwrap();

        assert_eq!(flag.get(), true);
    }

    #[test]
    fn test_executor_early_wake() {
        let executor = Executor::new(1);

        let fut = EarlyWakeFuture::new();

        executor
            .spawn(async move {
                fut.await;
            })
            .unwrap();

        let mut park_count = 0;

        executor
            .run(|_| {
                park_count += 1;

                Ok(())
            })
            .unwrap();

        assert_eq!(park_count, 1);
    }

    #[test]
    fn test_executor_pre_poll() {
        let executor = Executor::new(1);

        let flag = Rc::new(Cell::new(false));

        {
            let flag = flag.clone();

            executor.set_pre_poll(move || {
                flag.set(true);
            });
        }

        executor.spawn(async {}).unwrap();

        assert_eq!(flag.get(), false);

        executor.run(|_| Ok(())).unwrap();

        assert_eq!(flag.get(), true);
    }

    #[test]
    fn test_executor_ignore_resume_wakes() {
        let executor = Executor::new(1);

        // can't create a resume waker or ignore wakes outside of task
        assert!(executor.create_resume_waker_for_current_task().is_err());
        assert!(executor.ignore_wakes_for_current_task().is_err());

        let resume_waker: Rc<Cell<Option<Waker>>> = Rc::new(Cell::new(None));

        let fut = TestFuture::new();
        let handle = fut.handle();

        {
            let resume_waker = Rc::clone(&resume_waker);

            executor
                .spawn(async move {
                    let executor = Executor::current().unwrap();
                    resume_waker.set(Some(
                        executor.create_resume_waker_for_current_task().unwrap(),
                    ));
                    executor.ignore_wakes_for_current_task().unwrap();
                    fut.await;
                })
                .unwrap();
        }

        executor.run_until_stalled();
        assert_eq!(executor.have_tasks(), true);

        handle.set_ready();
        executor.run_until_stalled();
        assert_eq!(executor.have_tasks(), true);

        resume_waker.take().take().unwrap().wake();

        executor.run_until_stalled();
        assert_eq!(executor.have_tasks(), false);
    }
}
