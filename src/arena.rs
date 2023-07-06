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

use slab::Slab;
use std::cell::{RefCell, RefMut};
use std::mem;
use std::ops::{Deref, DerefMut};
use std::sync::{Mutex, MutexGuard};

pub struct EntryGuard<'a, T> {
    entries: RefMut<'a, Slab<T>>,
    entry: &'a mut T,
    key: usize,
}

impl<T> EntryGuard<'_, T> {
    fn remove(mut self) {
        self.entries.remove(self.key);
    }
}

impl<T> Deref for EntryGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        self.entry
    }
}

impl<T> DerefMut for EntryGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.entry
    }
}

// this is essentially a sharable slab for use within a single thread.
//   operations are protected by a RefCell. when an element is retrieved for
//   reading or modification, it is wrapped in a EntryGuard which keeps the
//   entire slab borrowed until the caller is done working with the element
pub struct Memory<T> {
    entries: RefCell<Slab<T>>,
}

impl<T> Memory<T> {
    pub fn new(capacity: usize) -> Self {
        // allocate the slab with fixed capacity
        let s = Slab::with_capacity(capacity);

        Self {
            entries: RefCell::new(s),
        }
    }

    #[cfg(test)]
    pub fn len(&self) -> usize {
        let entries = self.entries.borrow();

        entries.len()
    }

    fn insert(&self, e: T) -> Result<usize, ()> {
        let mut entries = self.entries.borrow_mut();

        // out of capacity. by preventing inserts beyond the capacity, we
        //   ensure the underlying memory won't get moved due to a realloc
        if entries.len() == entries.capacity() {
            return Err(());
        }

        Ok(entries.insert(e))
    }

    fn get<'a>(&'a self, key: usize) -> Option<EntryGuard<'a, T>> {
        let mut entries = self.entries.borrow_mut();

        let entry = entries.get_mut(key)?;

        // slab element addresses are guaranteed to be stable once created,
        //   and the only place we remove the element is in EntryGuard's
        //   remove method which consumes itself, therefore it is safe to
        //   assume the element will live at least as long as the EntryGuard
        //   and we can extend the lifetime of the reference beyond the
        //   RefMut
        let entry = unsafe { mem::transmute::<&mut T, &'a mut T>(entry) };

        Some(EntryGuard {
            entries,
            entry,
            key,
        })
    }

    // for tests, as a way to confirm the memory isn't moving. be careful
    //   with this. the very first element inserted will be at index 0, but
    //   if the slab has been used and cleared, then the next element
    //   inserted may not be at index 0 and calling this method afterward
    //   will panic
    #[cfg(test)]
    fn entry0_ptr(&self) -> *const T {
        let entries = self.entries.borrow();

        entries.get(0).unwrap() as *const T
    }
}

pub struct SyncEntryGuard<'a, T> {
    entries: MutexGuard<'a, Slab<T>>,
    entry: &'a mut T,
    key: usize,
}

impl<T> SyncEntryGuard<'_, T> {
    fn remove(mut self) {
        self.entries.remove(self.key);
    }
}

impl<T> Deref for SyncEntryGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        self.entry
    }
}

impl<T> DerefMut for SyncEntryGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.entry
    }
}

// this is essentially a thread-safe slab. operations are protected by a
//   mutex. when an element is retrieved for reading or modification, it is
//   wrapped in a EntryGuard which keeps the entire slab locked until the
//   caller is done working with the element
pub struct SyncMemory<T> {
    entries: Mutex<Slab<T>>,
}

impl<T> SyncMemory<T> {
    pub fn new(capacity: usize) -> Self {
        // allocate the slab with fixed capacity
        let s = Slab::with_capacity(capacity);

        Self {
            entries: Mutex::new(s),
        }
    }

    #[cfg(test)]
    pub fn len(&self) -> usize {
        let entries = self.entries.lock().unwrap();

        entries.len()
    }

    fn insert(&self, e: T) -> Result<usize, ()> {
        let mut entries = self.entries.lock().unwrap();

        // out of capacity. by preventing inserts beyond the capacity, we
        //   ensure the underlying memory won't get moved due to a realloc
        if entries.len() == entries.capacity() {
            return Err(());
        }

        Ok(entries.insert(e))
    }

    fn get<'a>(&'a self, key: usize) -> Option<SyncEntryGuard<'a, T>> {
        let mut entries = self.entries.lock().unwrap();

        let entry = entries.get_mut(key)?;

        // slab element addresses are guaranteed to be stable once created,
        //   and the only place we remove the element is in SyncEntryGuard's
        //   remove method which consumes itself, therefore it is safe to
        //   assume the element will live at least as long as the SyncEntryGuard
        //   and we can extend the lifetime of the reference beyond the
        //   MutexGuard
        let entry = unsafe { mem::transmute::<&mut T, &'a mut T>(entry) };

        Some(SyncEntryGuard {
            entries,
            entry,
            key,
        })
    }

    // for tests, as a way to confirm the memory isn't moving. be careful
    //   with this. the very first element inserted will be at index 0, but
    //   if the slab has been used and cleared, then the next element
    //   inserted may not be at index 0 and calling this method afterward
    //   will panic
    #[cfg(test)]
    fn entry0_ptr(&self) -> *const T {
        let entries = self.entries.lock().unwrap();

        entries.get(0).unwrap() as *const T
    }
}

pub struct ReusableValue<T> {
    reusable: std::sync::Arc<Reusable<T>>,
    value: *mut T,
    key: usize,
}

impl<T> ReusableValue<T> {
    // vec element addresses are guaranteed to be stable once created,
    //   and elements are only removed when the Reusable is dropped, and
    //   the Arc'd Reusable is guaranteed to live as long as
    //   ReusableValue, therefore it is safe to assume the element will
    //   live at least as long as the ReusableValue

    fn get(&self) -> &T {
        unsafe { &*self.value }
    }

    fn get_mut(&mut self) -> &mut T {
        unsafe { &mut *self.value }
    }
}

impl<T> Drop for ReusableValue<T> {
    fn drop(&mut self) {
        let mut entries = self.reusable.entries.lock().unwrap();

        entries.0.remove(self.key);
    }
}

impl<T> Deref for ReusableValue<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        self.get()
    }
}

impl<T> DerefMut for ReusableValue<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.get_mut()
    }
}

// like Memory, but for preinitializing each value and reusing
pub struct Reusable<T> {
    entries: Mutex<(Slab<()>, Vec<T>)>,
}

impl<T> Reusable<T> {
    pub fn new<F>(capacity: usize, init_fn: F) -> Self
    where
        F: Fn() -> T,
    {
        let mut values = Vec::with_capacity(capacity);

        for _ in 0..capacity {
            values.push(init_fn());
        }

        // allocate the slab with fixed capacity
        let s = Slab::with_capacity(capacity);

        Self {
            entries: Mutex::new((s, values)),
        }
    }

    #[cfg(test)]
    pub fn len(&self) -> usize {
        let entries = self.entries.lock().unwrap();

        entries.0.len()
    }

    #[allow(clippy::result_unit_err)]
    pub fn reserve(self: &std::sync::Arc<Self>) -> Result<ReusableValue<T>, ()> {
        let mut entries = self.entries.lock().unwrap();

        // out of capacity. the number of buffers is fixed
        if entries.0.len() == entries.0.capacity() {
            return Err(());
        }

        let key = entries.0.insert(());

        let value = &mut entries.1[key] as *mut T;

        Ok(ReusableValue {
            reusable: self.clone(),
            value,
            key,
        })
    }
}

pub struct RcEntry<T> {
    value: T,
    refs: usize,
}

pub type RcMemory<T> = Memory<RcEntry<T>>;

pub struct Rc<T> {
    memory: std::rc::Rc<RcMemory<T>>,
    key: usize,
}

impl<T> Rc<T> {
    #[allow(clippy::result_unit_err)]
    pub fn new(v: T, memory: &std::rc::Rc<RcMemory<T>>) -> Result<Self, ()> {
        let key = memory.insert(RcEntry { value: v, refs: 1 })?;

        Ok(Self {
            memory: std::rc::Rc::clone(memory),
            key,
        })
    }

    #[allow(clippy::should_implement_trait)]
    pub fn clone(rc: &Rc<T>) -> Self {
        let mut e = rc.memory.get(rc.key).unwrap();

        e.refs += 1;

        Self {
            memory: rc.memory.clone(),
            key: rc.key,
        }
    }

    pub fn get<'a>(&'a self) -> &'a T {
        let e = self.memory.get(self.key).unwrap();

        // get a reference to the inner value
        let value = &e.value;

        // entry addresses are guaranteed to be stable once created, and the
        //   entry managed by this Rc won't be dropped until this Rc drops,
        //   therefore it is safe to assume the entry managed by this Rc will
        //   live at least as long as this Rc, and we can extend the lifetime
        //   of the reference beyond the EntryGuard
        unsafe { mem::transmute::<&T, &'a T>(value) }
    }
}

impl<T> Drop for Rc<T> {
    fn drop(&mut self) {
        let mut e = self.memory.get(self.key).unwrap();

        if e.refs == 1 {
            e.remove();
            return;
        }

        e.refs -= 1;
    }
}

pub type ArcMemory<T> = SyncMemory<RcEntry<T>>;

pub struct Arc<T> {
    memory: std::sync::Arc<ArcMemory<T>>,
    key: usize,
}

impl<T> Arc<T> {
    #[allow(clippy::result_unit_err)]
    pub fn new(v: T, memory: &std::sync::Arc<ArcMemory<T>>) -> Result<Self, ()> {
        let key = memory.insert(RcEntry { value: v, refs: 1 })?;

        Ok(Self {
            memory: memory.clone(),
            key,
        })
    }

    #[allow(clippy::should_implement_trait)]
    pub fn clone(rc: &Arc<T>) -> Self {
        let mut e = rc.memory.get(rc.key).unwrap();

        e.refs += 1;

        Self {
            memory: rc.memory.clone(),
            key: rc.key,
        }
    }

    pub fn get<'a>(&'a self) -> &'a T {
        let e = self.memory.get(self.key).unwrap();

        // get a reference to the inner value
        let value = &e.value;

        // entry addresses are guaranteed to be stable once created, and the
        //   entry managed by this Arc won't be dropped until this Arc drops,
        //   therefore it is safe to assume the entry managed by this Arc will
        //   live at least as long as this Arc, and we can extend the lifetime
        //   of the reference beyond the SyncEntryGuard
        unsafe { mem::transmute::<&T, &'a T>(value) }
    }
}

impl<T> Drop for Arc<T> {
    fn drop(&mut self) {
        let mut e = self.memory.get(self.key).unwrap();

        if e.refs == 1 {
            e.remove();
            return;
        }

        e.refs -= 1;
    }
}

// adapted from https://github.com/rust-lang/rfcs/pull/2802
pub fn recycle_vec<T, U>(mut v: Vec<T>) -> Vec<U> {
    assert_eq!(core::mem::size_of::<T>(), core::mem::size_of::<U>());
    assert_eq!(core::mem::align_of::<T>(), core::mem::align_of::<U>());
    v.clear();
    let ptr = v.as_mut_ptr();
    let capacity = v.capacity();
    mem::forget(v);
    let ptr = ptr as *mut U;
    unsafe { Vec::from_raw_parts(ptr, 0, capacity) }
}

// ReusableVec inspired by recycle_vec

pub struct ReusableVecHandle<'a, T> {
    vec: &'a mut Vec<T>,
}

impl<T> ReusableVecHandle<'_, T> {
    pub fn get_ref(&self) -> &Vec<T> {
        self.vec
    }

    pub fn get_mut(&mut self) -> &mut Vec<T> {
        self.vec
    }
}

impl<T> Drop for ReusableVecHandle<'_, T> {
    fn drop(&mut self) {
        self.vec.clear();
    }
}

impl<T> Deref for ReusableVecHandle<'_, T> {
    type Target = Vec<T>;

    fn deref(&self) -> &Self::Target {
        self.get_ref()
    }
}

impl<T> DerefMut for ReusableVecHandle<'_, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.get_mut()
    }
}

pub struct ReusableVec {
    vec: Vec<()>,
    size: usize,
    align: usize,
}

impl ReusableVec {
    pub fn new<T>(capacity: usize) -> Self {
        let size = mem::size_of::<T>();
        let align = mem::align_of::<T>();

        let vec: Vec<T> = Vec::with_capacity(capacity);

        // safety: we must cast to Vec<U> before using, where U has the same
        // size and alignment as T
        let vec: Vec<()> = unsafe { mem::transmute(vec) };

        Self { vec, size, align }
    }

    pub fn get_as_new<U>(&mut self) -> ReusableVecHandle<'_, U> {
        let size = mem::size_of::<U>();
        let align = mem::align_of::<U>();

        // if these don't match, panic. it's up the user to ensure the type
        // is acceptable
        assert_eq!(self.size, size);
        assert_eq!(self.align, align);

        let vec: &mut Vec<()> = &mut self.vec;

        // safety: U has the expected size and alignment
        let vec: &mut Vec<U> = unsafe { mem::transmute(vec) };

        // the vec starts empty, and is always cleared when the handle drops.
        // get_as_new() borrows self mutably, so it's not possible to create
        // a handle when one already exists
        assert!(vec.is_empty());

        ReusableVecHandle { vec }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_reusable() {
        let reusable = std::sync::Arc::new(Reusable::new(2, || vec![0; 128]));
        assert_eq!(reusable.len(), 0);

        let mut buf1 = reusable.reserve().unwrap();
        assert_eq!(reusable.len(), 1);

        let mut buf2 = reusable.reserve().unwrap();
        assert_eq!(reusable.len(), 2);

        // no room
        assert!(reusable.reserve().is_err());

        buf1[..5].copy_from_slice(b"hello");
        buf2[..5].copy_from_slice(b"world");

        assert_eq!(&buf1[..5], b"hello");
        assert_eq!(&buf2[..5], b"world");

        mem::drop(buf1);
        assert_eq!(reusable.len(), 1);

        mem::drop(buf2);
        assert_eq!(reusable.len(), 0);
    }

    #[test]
    fn test_rc() {
        let memory = std::rc::Rc::new(RcMemory::new(2));
        assert_eq!(memory.len(), 0);

        let e0a = Rc::new(123 as i32, &memory).unwrap();
        assert_eq!(memory.len(), 1);
        let p = memory.entry0_ptr();

        let e0b = Rc::clone(&e0a);
        assert_eq!(memory.len(), 1);
        assert_eq!(memory.entry0_ptr(), p);

        let e1a = Rc::new(456 as i32, &memory).unwrap();
        assert_eq!(memory.len(), 2);
        assert_eq!(memory.entry0_ptr(), p);

        // no room
        assert!(Rc::new(789 as i32, &memory).is_err());

        assert_eq!(*e0a.get(), 123);
        assert_eq!(*e0b.get(), 123);
        assert_eq!(*e1a.get(), 456);

        mem::drop(e0b);
        assert_eq!(memory.len(), 2);
        assert_eq!(memory.entry0_ptr(), p);

        mem::drop(e0a);
        assert_eq!(memory.len(), 1);

        mem::drop(e1a);
        assert_eq!(memory.len(), 0);
    }

    #[test]
    fn test_arc() {
        let memory = std::sync::Arc::new(ArcMemory::new(2));
        assert_eq!(memory.len(), 0);

        let e0a = Arc::new(123 as i32, &memory).unwrap();
        assert_eq!(memory.len(), 1);
        let p = memory.entry0_ptr();

        let e0b = Arc::clone(&e0a);
        assert_eq!(memory.len(), 1);
        assert_eq!(memory.entry0_ptr(), p);

        let e1a = Arc::new(456 as i32, &memory).unwrap();
        assert_eq!(memory.len(), 2);
        assert_eq!(memory.entry0_ptr(), p);

        // no room
        assert!(Arc::new(789 as i32, &memory).is_err());

        assert_eq!(*e0a.get(), 123);
        assert_eq!(*e0b.get(), 123);
        assert_eq!(*e1a.get(), 456);

        mem::drop(e0b);
        assert_eq!(memory.len(), 2);
        assert_eq!(memory.entry0_ptr(), p);

        mem::drop(e0a);
        assert_eq!(memory.len(), 1);

        mem::drop(e1a);
        assert_eq!(memory.len(), 0);
    }

    #[test]
    fn test_reusable_vec() {
        let mut vec_mem = ReusableVec::new::<u32>(100);

        let mut vec = vec_mem.get_as_new::<u32>();

        assert_eq!(vec.capacity(), 100);
        assert_eq!(vec.len(), 0);

        vec.push(1);
        assert_eq!(vec.len(), 1);

        mem::drop(vec);

        let vec = vec_mem.get_as_new::<u32>();

        assert_eq!(vec.capacity(), 100);
        assert_eq!(vec.len(), 0);
    }
}
