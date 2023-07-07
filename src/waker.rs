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

use std::cell::Cell;
use std::mem;
use std::ops::Deref;
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

pub trait RefWake {
    fn wake(&self);
}

pub struct RefWakerData<W> {
    refs: Cell<usize>,
    w: W,
}

impl<W: RefWake> RefWakerData<W> {
    pub fn new(w: W) -> Self {
        Self {
            refs: Cell::new(1),
            w,
        }
    }
}

// a waker that borrows its inner data and panics on drop if there are any
// active clones. this makes it possible to create wakers without heap
// allocations, as long as all clones are cleaned up before the RefWaker is
// dropped
pub struct RefWaker<'a, W: RefWake + 'a> {
    data: &'a RefWakerData<W>,
}

impl<'a, W: RefWake + 'a> RefWaker<'a, W> {
    pub fn new(data: &'a RefWakerData<W>) -> Self {
        Self { data }
    }

    pub fn ref_count(&self) -> usize {
        self.data.refs.get()
    }

    pub fn as_std<'b>(&self, scratch: &'b mut mem::MaybeUninit<Waker>) -> &'b Waker {
        let rw = RawWaker::new(
            self.data as *const RefWakerData<W> as *const (),
            &RawWakerVTable::new(Self::clone, Self::wake, Self::wake_by_ref, Self::drop),
        );

        // SAFETY: the inner Waker data is part of self, and our Drop impl
        // guarantees any Waker instances cannot outlive self
        let waker = unsafe { Waker::from_raw(rw) };

        scratch.write(waker);

        // SAFETY: scratch is initialized above
        unsafe { scratch.assume_init_ref() }
    }

    unsafe fn clone(data: *const ()) -> RawWaker {
        let data_ref = (data as *const RefWakerData<W>).as_ref().unwrap();

        data_ref.refs.set(data_ref.refs.get() + 1);

        RawWaker::new(
            data,
            &RawWakerVTable::new(Self::clone, Self::wake, Self::wake_by_ref, Self::drop),
        )
    }

    unsafe fn wake(data: *const ()) {
        Self::wake_by_ref(data);

        Self::drop(data);
    }

    unsafe fn wake_by_ref(data: *const ()) {
        let data_ref = (data as *const RefWakerData<W>).as_ref().unwrap();

        data_ref.w.wake();
    }

    unsafe fn drop(data: *const ()) {
        let data_ref = (data as *const RefWakerData<W>).as_ref().unwrap();

        let refs = data_ref.refs.get();

        assert!(refs > 1);

        data_ref.refs.set(refs - 1);
    }
}

impl<'a, W: RefWake + 'a> Drop for RefWaker<'a, W> {
    fn drop(&mut self) {
        assert_eq!(self.data.refs.get(), 1);
    }
}

impl<'a, W: RefWake + 'a> Deref for RefWaker<'a, W> {
    type Target = W;

    fn deref(&self) -> &Self::Target {
        &self.data.w
    }
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

    impl RefWake for TestWaker {
        fn wake(&self) {
            self.waked.set(self.waked.get() + 1);
        }
    }

    #[test]
    fn test_rc_waker() {
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

    #[test]
    fn test_ref_waker() {
        let data = RefWakerData::new(TestWaker::new());
        let data = RefWaker::new(&data);

        assert_eq!(RefWaker::ref_count(&data), 1);

        let waker = data.as_std(&mut mem::MaybeUninit::uninit()).clone();

        assert_eq!(RefWaker::ref_count(&data), 2);

        let waker2 = waker.clone();

        assert_eq!(RefWaker::ref_count(&data), 3);
        assert_eq!(data.waked(), 0);

        waker2.wake();

        assert_eq!(RefWaker::ref_count(&data), 2);
        assert_eq!(data.waked(), 1);

        waker.wake_by_ref();

        assert_eq!(RefWaker::ref_count(&data), 2);
        assert_eq!(data.waked(), 2);

        mem::drop(waker);

        assert_eq!(RefWaker::ref_count(&data), 1);
    }
}
