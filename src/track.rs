/*
 * Copyright (C) 2023 Fanout, Inc.
 * Copyright (C) 2023 Fastly, Inc.
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

use crate::future::AsyncLocalReceiver;
use std::cell::Cell;
use std::future::Future;
use std::ops::Deref;
use std::pin::Pin;
use std::sync::mpsc;
use std::task::{Context, Poll};

#[derive(Default)]
pub struct TrackFlag(Cell<bool>);

impl TrackFlag {
    pub fn get(&self) -> bool {
        self.0.get()
    }

    pub fn set(&self, v: bool) {
        self.0.set(v);
    }
}

struct TrackInner<'a, T> {
    value: T,
    active: &'a TrackFlag,
}

// wrap a value and a shared flag representing the value's liveness. on init,
// the flag is set to true. on drop, the flag is set to false
pub struct Track<'a, T> {
    inner: Option<TrackInner<'a, T>>,
}

impl<'a, T> Track<'a, T> {
    pub fn new(value: T, active: &'a TrackFlag) -> Self {
        active.set(true);

        Self {
            inner: Some(TrackInner { value, active }),
        }
    }
}

impl<'a, A, B> Track<'a, (A, B)> {
    pub fn map_first(mut orig: Self) -> (Track<'a, A>, B) {
        let ((a, b), active) = {
            let inner = orig.inner.take().unwrap();
            drop(orig);

            (inner.value, inner.active)
        };

        (Track::new(a, active), b)
    }
}

impl<'a, T> Drop for Track<'a, T> {
    fn drop(&mut self) {
        if let Some(inner) = &self.inner {
            inner.active.set(false);
        }
    }
}

impl<'a, T> Deref for Track<'a, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        &self.inner.as_ref().unwrap().value
    }
}

#[derive(Debug, PartialEq)]
pub enum RecvError {
    Disconnected,
    ValueActive,
}

// wrap an AsyncLocalReceiver and a shared flag representing the liveness of
// one received value at a time. each received value is wrapped in Track and
// must be dropped before reading the next value
pub struct TrackedAsyncLocalReceiver<'a, T> {
    inner: AsyncLocalReceiver<T>,
    value_active: &'a TrackFlag,
}

impl<'a, T> TrackedAsyncLocalReceiver<'a, T> {
    pub fn new(r: AsyncLocalReceiver<T>, value_active: &'a TrackFlag) -> Self {
        value_active.set(false);

        Self {
            inner: r,
            value_active,
        }
    }

    // attempt to receive a value from the inner receiver. if a previously
    // received value has not been dropped, this method returns an error
    pub async fn recv(&self) -> Result<Track<'a, T>, RecvError> {
        if self.value_active.get() {
            return Err(RecvError::ValueActive);
        }

        let v = match self.inner.recv().await {
            Ok(v) => v,
            Err(mpsc::RecvError) => return Err(RecvError::Disconnected),
        };

        Ok(Track::new(v, self.value_active))
    }
}

#[derive(Debug, PartialEq)]
pub struct ValueActiveError;

pub struct TrackFuture<'a, F> {
    fut: F,
    value_active: &'a TrackFlag,
}

impl<'a, F, T, E> Future for TrackFuture<'a, F>
where
    F: Future<Output = Result<T, E>>,
    E: From<ValueActiveError>,
{
    type Output = Result<T, E>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        // SAFETY: pin projection
        let (fut, value_active) = unsafe {
            let s = self.get_unchecked_mut();
            let fut = Pin::new_unchecked(&mut s.fut);

            (fut, &s.value_active)
        };

        let result = fut.poll(cx);

        if value_active.get() {
            return Poll::Ready(Err(ValueActiveError.into()));
        }

        result
    }
}

// wrap a future and a shared flag representing the liveness of some value.
// if the value is true after polling the inner future, return an error
pub fn track_future<F>(fut: F, value_active: &TrackFlag) -> TrackFuture<'_, F>
where
    F: Future,
{
    TrackFuture { fut, value_active }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::channel;
    use crate::executor::Executor;
    use crate::future::yield_task;
    use crate::reactor::Reactor;

    #[test]
    fn track_value() {
        let f = TrackFlag::default();

        let v = Track::new(42, &f);
        assert!(f.get());
        assert_eq!(*v, 42);

        drop(v);
        assert!(!f.get());
    }

    #[test]
    fn track_async_local_receiver() {
        let reactor = Reactor::new(2);
        let executor = Executor::new(1);

        let (s, r) = channel::local_channel(2, 1, &reactor.local_registration_memory());

        s.try_send(1).unwrap();
        s.try_send(2).unwrap();
        drop(s);

        executor
            .spawn(async move {
                let f = TrackFlag::default();

                let r = TrackedAsyncLocalReceiver::new(AsyncLocalReceiver::new(r), &f);

                let v = r.recv().await.unwrap();
                assert_eq!(*v, 1);
                assert!(r.recv().await.is_err());

                drop(v);
                let v = r.recv().await.unwrap();
                assert_eq!(*v, 2);
                assert!(r.recv().await.is_err());

                // no values left
                drop(v);
                assert!(r.recv().await.is_err());
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn track_value_and_future() {
        let executor = Executor::new(1);

        executor
            .spawn(async move {
                let f = TrackFlag::default();

                // awaiting while the flag is active is an error
                let ret = track_future(
                    async {
                        let v = Track::new(1, &f);

                        // this line will cause the error
                        yield_task().await;

                        // this line never reached
                        drop(v);

                        Ok(())
                    },
                    &f,
                )
                .await;
                assert_eq!(ret, Err(ValueActiveError));

                // awaiting while the flag is not active is ok
                let ret: Result<_, ValueActiveError> = track_future(
                    async {
                        let v = Track::new(1, &f);
                        drop(v);

                        // this is ok
                        yield_task().await;

                        Ok(())
                    },
                    &f,
                )
                .await;
                assert_eq!(ret, Ok(()));
            })
            .unwrap();

        executor.run(|_| Ok(())).unwrap();
    }
}
