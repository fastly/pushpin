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

use crate::core::arena;
use crate::core::event::{self, ReadinessExt};
use crate::core::reactor::{CustomEvented, Reactor, Registration, TimerEvented};
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Context, Poll};
use std::time::Instant;

pub struct PollFuture<F> {
    fut: F,
}

impl<F> Future for PollFuture<F>
where
    F: Future + Unpin,
{
    type Output = Poll<F::Output>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let s = &mut *self;

        Poll::Ready(Pin::new(&mut s.fut).poll(cx))
    }
}

pub fn poll_async<F>(fut: F) -> PollFuture<F>
where
    F: Future + Unpin,
{
    PollFuture { fut }
}

#[track_caller]
pub fn get_reactor() -> Reactor {
    Reactor::current().expect("no reactor in thread")
}

pub struct Timeout {
    evented: TimerEvented,
}

impl Timeout {
    pub fn new(deadline: Instant) -> Self {
        let evented = TimerEvented::new(deadline, &get_reactor()).unwrap();

        evented.registration().set_ready(true);

        Self { evented }
    }

    pub fn set_deadline(&self, deadline: Instant) {
        self.evented.set_expires(deadline).unwrap();

        self.evented.registration().set_ready(true);
    }

    pub fn elapsed(&self) -> TimeoutFuture<'_> {
        TimeoutFuture { t: self }
    }
}

pub struct CancellationSender {
    read_set_readiness: event::LocalSetReadiness,
}

impl CancellationSender {
    fn cancel(&self) {
        self.read_set_readiness
            .set_readiness(mio::Interest::READABLE)
            .unwrap();
    }
}

impl Drop for CancellationSender {
    fn drop(&mut self) {
        self.cancel();
    }
}

pub struct CancellationToken {
    evented: CustomEvented,
    _read_registration: event::LocalRegistration,
}

impl CancellationToken {
    pub fn new(
        memory: &Rc<arena::RcMemory<event::LocalRegistrationEntry>>,
    ) -> (CancellationSender, Self) {
        let (read_reg, read_sr) = event::LocalRegistration::new(memory);

        let evented =
            CustomEvented::new_local(&read_reg, mio::Interest::READABLE, &get_reactor()).unwrap();

        evented.registration().set_ready(false);

        let sender = CancellationSender {
            read_set_readiness: read_sr,
        };

        let token = Self {
            evented,
            _read_registration: read_reg,
        };

        (sender, token)
    }

    pub fn cancelled(&self) -> CancelledFuture<'_> {
        CancelledFuture { t: self }
    }
}

pub struct EventWaiter<'a> {
    registration: &'a Registration,
}

impl<'a> EventWaiter<'a> {
    pub fn new(registration: &'a Registration) -> Self {
        Self { registration }
    }

    pub fn wait(&'a self, interest: mio::Interest) -> WaitFuture<'a> {
        WaitFuture { w: self, interest }
    }
}

pub struct TimeoutFuture<'a> {
    t: &'a Timeout,
}

impl Future for TimeoutFuture<'_> {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let evented = &self.t.evented;

        evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        if !evented.registration().is_ready() {
            return Poll::Pending;
        }

        let now = get_reactor().now();

        if now >= evented.expires() {
            Poll::Ready(())
        } else {
            evented.registration().set_ready(false);

            Poll::Pending
        }
    }
}

impl Drop for TimeoutFuture<'_> {
    fn drop(&mut self) {
        self.t.evented.registration().clear_waker();
    }
}

pub struct CancelledFuture<'a> {
    t: &'a CancellationToken,
}

impl Future for CancelledFuture<'_> {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        if f.t.evented.registration().is_ready() {
            return Poll::Ready(());
        }

        f.t.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        Poll::Pending
    }
}

impl Drop for CancelledFuture<'_> {
    fn drop(&mut self) {
        self.t.evented.registration().clear_waker();
    }
}

pub struct WaitFuture<'a> {
    w: &'a EventWaiter<'a>,
    interest: mio::Interest,
}

impl Future for WaitFuture<'_> {
    type Output = mio::Interest;

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &*self;

        f.w.registration.set_waker(cx.waker(), f.interest);

        if !f.w.registration.readiness().contains_any(f.interest) {
            return Poll::Pending;
        }

        let readiness = f.w.registration.readiness().unwrap();

        // mask with the interest
        let readable = readiness.is_readable() && f.interest.is_readable();
        let writable = readiness.is_writable() && f.interest.is_writable();
        let readiness = if readable && writable {
            mio::Interest::READABLE | mio::Interest::WRITABLE
        } else if readable {
            mio::Interest::READABLE
        } else {
            mio::Interest::WRITABLE
        };

        Poll::Ready(readiness)
    }
}

impl Drop for WaitFuture<'_> {
    fn drop(&mut self) {
        self.w.registration.clear_waker();
    }
}

pub async fn event_wait(registration: &Registration, interest: mio::Interest) -> mio::Interest {
    EventWaiter::new(registration).wait(interest).await
}

pub struct YieldFuture {
    done: bool,
}

impl Future for YieldFuture {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        if !f.done {
            f.done = true;
            cx.waker().wake_by_ref();

            Poll::Pending
        } else {
            Poll::Ready(())
        }
    }
}

pub fn yield_task() -> YieldFuture {
    YieldFuture { done: false }
}

pub struct YieldToLocalEvents {
    started: bool,
    evented: CustomEvented,
    _registration: event::LocalRegistration,
    set_readiness: event::LocalSetReadiness,
}

impl YieldToLocalEvents {
    fn new() -> Self {
        let reactor = get_reactor();

        let (reg, sr) = event::LocalRegistration::new(&reactor.local_registration_memory());

        let evented = CustomEvented::new_local(&reg, mio::Interest::READABLE, &reactor).unwrap();

        evented.registration().set_ready(false);

        Self {
            started: false,
            evented,
            _registration: reg,
            set_readiness: sr,
        }
    }
}

impl Future for YieldToLocalEvents {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        if f.evented.registration().is_ready() {
            return Poll::Ready(());
        }

        f.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        if !f.started {
            f.started = true;

            // this will wake us up after all local events before it have been processed
            f.set_readiness
                .set_readiness(mio::Interest::READABLE)
                .unwrap();
        }

        Poll::Pending
    }
}

pub fn yield_to_local_events() -> YieldToLocalEvents {
    YieldToLocalEvents::new()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::channel;
    use crate::core::executor::Executor;
    use std::cell::Cell;
    use std::rc::Rc;
    use std::task::Context;
    use std::time::Duration;

    #[test]
    fn test_timeout() {
        let now = Instant::now();

        let executor = Executor::new(1);
        let reactor = Reactor::new_with_time(1, now);

        executor
            .spawn(async {
                let timeout = Timeout::new(get_reactor().now() + Duration::from_millis(100));
                timeout.elapsed().await;
            })
            .unwrap();

        executor.run_until_stalled();

        reactor
            .poll_nonblocking(now + Duration::from_millis(200))
            .unwrap();

        executor.run(|_| Ok(())).unwrap();
    }

    #[test]
    fn test_timeout_ready() {
        let now = Instant::now();

        let executor = Executor::new(1);
        let _reactor = Reactor::new_with_time(1, now);

        executor
            .spawn(async {
                let timeout = Timeout::new(get_reactor().now());
                timeout.elapsed().await;
            })
            .unwrap();

        executor.run(|_| Ok(())).unwrap();
    }

    #[test]
    fn test_timeout_change_ready() {
        let now = Instant::now();

        let _reactor = Reactor::new_with_time(1, now);
        let executor = Executor::new(1);

        executor
            .spawn(async {
                let timeout = Timeout::new(get_reactor().now() + Duration::from_millis(100));

                let mut fut = timeout.elapsed();
                assert_eq!(poll_async(&mut fut).await, Poll::Pending);

                timeout.set_deadline(get_reactor().now());
                assert_eq!(poll_async(&mut fut).await, Poll::Ready(()));
            })
            .unwrap();

        executor.run(|_| Ok(())).unwrap();
    }

    #[test]
    fn test_cancellation_token() {
        let reactor = Reactor::new(1);
        let executor = Executor::new(1);

        executor
            .spawn(async {
                let (sender, token) =
                    CancellationToken::new(&get_reactor().local_registration_memory());

                let mut fut = token.cancelled();
                assert_eq!(poll_async(&mut fut).await, Poll::Pending);

                drop(sender);
                token.cancelled().await;
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn test_event_wait() {
        let reactor = Reactor::new(2);
        let executor = Executor::new(2);

        let (s, r) = channel::local_channel::<u32>(1, 1, &reactor.local_registration_memory());

        let s = channel::AsyncLocalSender::new(s);

        executor
            .spawn(async move {
                let reactor = Reactor::current().unwrap();

                let reg = reactor
                    .register_custom_local(r.get_read_registration(), mio::Interest::READABLE)
                    .unwrap();

                assert_eq!(
                    event_wait(&reg, mio::Interest::READABLE).await,
                    mio::Interest::READABLE
                );
            })
            .unwrap();

        executor.run_until_stalled();

        assert_eq!(executor.have_tasks(), true);

        executor
            .spawn(async move {
                s.send(1).await.unwrap();
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    struct PollCountFuture<F> {
        count: i32,
        fut: F,
    }

    impl<F> PollCountFuture<F>
    where
        F: Future<Output = ()> + Unpin,
    {
        fn new(fut: F) -> Self {
            Self { count: 0, fut }
        }
    }

    impl<F> Future for PollCountFuture<F>
    where
        F: Future<Output = ()> + Unpin,
    {
        type Output = i32;

        fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
            let f = &mut *self;

            f.count += 1;

            match Pin::new(&mut f.fut).poll(cx) {
                Poll::Ready(()) => Poll::Ready(f.count),
                Poll::Pending => Poll::Pending,
            }
        }
    }

    #[test]
    fn test_yield_task() {
        let executor = Executor::new(1);

        executor
            .spawn(async {
                assert_eq!(PollCountFuture::new(yield_task()).await, 2);
            })
            .unwrap();

        executor.run(|_| Ok(())).unwrap();
    }

    #[test]
    fn test_yield_to_local_events() {
        let reactor = Reactor::new(3);
        let executor = Executor::new(2);

        let (s, r) = channel::local_channel(1, 1, &reactor.local_registration_memory());

        let state = Rc::new(Cell::new(0));

        {
            let state = Rc::clone(&state);

            executor
                .spawn(async move {
                    let r = channel::AsyncLocalReceiver::new(r);
                    r.recv().await.unwrap();
                    state.set(1);
                })
                .unwrap();
        }

        executor.run_until_stalled();

        executor
            .spawn(async move {
                s.try_send(1).unwrap();
                yield_to_local_events().await;
                assert_eq!(state.get(), 1);
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }
}
