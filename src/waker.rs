/*
 * Copyright (C) 2021 Fanout, Inc.
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

use std::mem;
use std::rc::Rc;
use std::task::{RawWaker, RawWakerVTable, Waker};

// adapted from std::task::Wake

pub trait RcWake {
    fn wake(self: Rc<Self>);

    fn wake_by_ref(self: &Rc<Self>) {
        self.clone().wake();
    }
}

pub fn into_std<W: RcWake>(waker: Rc<W>) -> Waker {
    // SAFETY: This is safe because raw_waker safely constructs
    // a RawWaker from Rc<W>.
    unsafe { Waker::from_raw(raw_waker(waker)) }
}

#[inline(always)]
fn raw_waker<W: RcWake>(waker: Rc<W>) -> RawWaker {
    unsafe fn clone_waker<W: RcWake>(waker: *const ()) -> RawWaker {
        let waker = mem::ManuallyDrop::new(Rc::from_raw(waker as *const W));

        let waker = Rc::clone(&waker);

        RawWaker::new(
            Rc::into_raw(waker) as *const (),
            &RawWakerVTable::new(
                clone_waker::<W>,
                wake::<W>,
                wake_by_ref::<W>,
                drop_waker::<W>,
            ),
        )
    }

    unsafe fn wake<W: RcWake>(waker: *const ()) {
        let waker = Rc::from_raw(waker as *const W);
        <W as RcWake>::wake(waker);
    }

    unsafe fn wake_by_ref<W: RcWake>(waker: *const ()) {
        let waker = mem::ManuallyDrop::new(Rc::from_raw(waker as *const W));
        <W as RcWake>::wake_by_ref(&waker);
    }

    unsafe fn drop_waker<W: RcWake>(waker: *const ()) {
        Rc::from_raw(waker as *const W);
    }

    RawWaker::new(
        Rc::into_raw(waker) as *const (),
        &RawWakerVTable::new(
            clone_waker::<W>,
            wake::<W>,
            wake_by_ref::<W>,
            drop_waker::<W>,
        ),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    struct TestWaker {
        waked: Cell<u32>,
    }

    impl TestWaker {
        fn new() -> Self {
            TestWaker {
                waked: Cell::new(0),
            }
        }

        fn waked(&self) -> u32 {
            self.waked.get()
        }
    }

    impl RcWake for TestWaker {
        fn wake(self: Rc<Self>) {
            self.waked.set(self.waked.get() + 1);
        }
    }

    #[test]
    fn test_waker() {
        let data = Rc::new(TestWaker::new());

        assert_eq!(Rc::strong_count(&data), 1);

        let waker = into_std(data.clone());

        assert_eq!(Rc::strong_count(&data), 2);

        let waker2 = waker.clone();

        assert_eq!(Rc::strong_count(&data), 3);
        assert_eq!(data.waked(), 0);

        waker2.wake();

        assert_eq!(Rc::strong_count(&data), 2);
        assert_eq!(data.waked(), 1);

        waker.wake_by_ref();

        assert_eq!(Rc::strong_count(&data), 2);
        assert_eq!(data.waked(), 2);

        mem::drop(waker);

        assert_eq!(Rc::strong_count(&data), 1);
    }
}
