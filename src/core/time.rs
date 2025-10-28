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
use std::cell::RefCell;
use std::future::Future;
use std::pin::Pin;
use std::task::{Context, Poll};
use std::time::Instant;

pub struct Timeout {
    evented: RefCell<Option<TimerEvented>>,
}

impl Timeout {
    pub fn new(deadline: Instant) -> Self {
        let reactor = get_reactor();

        let evented = if deadline > reactor.now() {
            Some(TimerEvented::new(deadline, &reactor).unwrap())
        } else {
            None
        };

        Self {
            evented: RefCell::new(evented),
        }
    }

    pub fn set_deadline(&self, deadline: Instant) {
        let reactor = get_reactor();

        if deadline > reactor.now() {
            if let Some(evented) = self.evented.borrow().as_ref() {
                evented.set_expires(deadline).unwrap();
                evented.registration().set_ready(false);
            } else {
                self.evented
                    .replace(Some(TimerEvented::new(deadline, &reactor).unwrap()));
            }
        } else {
            self.evented.replace(None);
        }
    }

    pub fn elapsed(&self) -> TimeoutFuture<'_> {
        TimeoutFuture { t: self }
    }
}

pub struct TimeoutFuture<'a> {
    t: &'a Timeout,
}

impl Future for TimeoutFuture<'_> {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let evented = self.t.evented.borrow();

        let Some(evented) = evented.as_ref() else {
            // no registration means we are ready immediately
            return Poll::Ready(());
        };

        if evented.registration().is_ready() {
            let now = evented.registration().reactor().now();

            // reactor guarantees enough time has passed
            assert!(now >= evented.expires());

            return Poll::Ready(());
        }

        evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        Poll::Pending
    }
}

impl Drop for TimeoutFuture<'_> {
    fn drop(&mut self) {
        if let Some(evented) = self.t.evented.borrow().as_ref() {
            evented.registration().clear_waker();
        }
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
