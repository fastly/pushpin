/*
 * Copyright (C) 2020-2023 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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

use crate::core::reactor::TimerEvented;
use crate::core::task::get_reactor;
use std::future::Future;
use std::pin::Pin;
use std::task::{Context, Poll};
use std::time::Instant;

pub struct Timeout {
    evented: TimerEvented,
}

impl Timeout {
    pub fn new(deadline: Instant) -> Self {
        let evented = TimerEvented::new(deadline, &get_reactor()).unwrap();

        Self { evented }
    }

    pub fn set_deadline(&self, deadline: Instant) {
        // in case a previous timeout had completed, set the registration
        // readiness to false to ensure we get notified again
        self.evented.registration().set_ready(false);

        self.evented.set_expires(deadline).unwrap();
    }

    pub fn elapsed(&self) -> ElapsedFuture<'_> {
        ElapsedFuture { t: self }
    }
}

pub struct ElapsedFuture<'a> {
    t: &'a Timeout,
}

impl Future for ElapsedFuture<'_> {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let evented = &self.t.evented;

        let now = evented.registration().reactor().now();
        let expired = now >= evented.expires();

        // if registration ready, reactor guarantees enough time has passed
        assert!(!evented.registration().is_ready() || expired);

        // even if the registration is not ready, we can still return ready
        // if the timeout has elapsed. notably, this enables a timeout of
        // zero to poll as ready immediately
        if expired {
            return Poll::Ready(());
        }

        evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        Poll::Pending
    }
}

impl Drop for ElapsedFuture<'_> {
    fn drop(&mut self) {
        self.t.evented.registration().clear_waker();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::executor::Executor;
    use crate::core::reactor::Reactor;
    use crate::core::task::poll_async;
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
}
