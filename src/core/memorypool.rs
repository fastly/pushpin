/*
 * Copyright (C) 2020-2023 Fanout, Inc.
 * Copyright (C) 2026 Fastly, Inc.
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

use crate::core::minislab::MiniSlab;
use std::cell::{Cell, RefCell};
use std::fmt;
use std::marker::PhantomData;
use std::mem;
use std::ops::{Deref, DerefMut};
use std::process::abort;
use std::ptr::NonNull;

#[derive(Debug)]
pub struct AllocError;

pub struct InsertError<T>(pub T);

impl<T> fmt::Debug for InsertError<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("InsertError").finish_non_exhaustive()
    }
}

// This is essentially a sharable slab for use within a single thread.
// Operations are protected by a RefCell, however lookup operations return
// pointers that can be used without RefCell protection.
pub struct Memory<T> {
    entries: RefCell<MiniSlab<T>>,
}

impl<T> Memory<T> {
    pub fn new(capacity: usize) -> Self {
        // Allocate the slab with fixed capacity
        let s = MiniSlab::with_capacity(capacity);

        Self {
            entries: RefCell::new(s),
        }
    }

    #[cfg(test)]
    pub fn len(&self) -> usize {
        self.entries.borrow().len()
    }

    // Returns a key and a pointer to the inserted entry.
    //
    // SAFETY: The returned pointer is guaranteed to be valid until the entry
    // is removed or the Memory is dropped.
    fn insert(&self, e: T) -> Result<(usize, *const T), InsertError<T>> {
        let mut entries = self.entries.borrow_mut();

        // Out of capacity. By preventing inserts beyond the capacity, we
        // ensure the underlying memory won't get moved due to a realloc.
        if entries.len() == entries.capacity() {
            return Err(InsertError(e));
        }

        let key = entries.insert(e);

        // SAFETY: The element was inserted above
        let entry = unsafe { entries.get_unchecked(key) };

        // Slab element addresses are guaranteed to be stable once created,
        // therefore we can return a pointer to the element and guarantee
        // its validity until the element is removed.

        Ok((key, entry as *const T))
    }

    #[allow(clippy::let_unit_value)]
    fn remove(&self, key: usize) {
        // Ensure remove() method doesn't return a value
        let _: () = self.entries.borrow_mut().remove(key);
    }

    // Returns a pointer to an entry if it exists.
    //
    // SAFETY: The returned pointer is guaranteed to be valid until the entry
    // is removed or the Memory is dropped.
    #[cfg(test)]
    fn addr_of(&self, key: usize) -> Option<*const T> {
        let entries = self.entries.borrow();

        let entry = entries.get(key)?;

        // Slab element addresses are guaranteed to be stable once created,
        // therefore we can return a pointer to the element and guarantee
        // its validity until the element is removed.

        Some(entry as *const T)
    }
}

#[cold]
fn unlikely_abort() {
    abort();
}

pub struct RcInner<T> {
    refs: Cell<usize>,
    memory: Cell<Option<std::rc::Rc<RcMemory<T>>>>,
    key: Cell<usize>,
    value: T,
}

impl<T> RcInner<T> {
    #[inline]
    fn inc(&self) {
        // Optimized checked increment adapted from std::rc::Rc

        let refs = self.refs.get();

        // We insert an `assume` here to hint LLVM at an otherwise
        // missed optimization.
        // SAFETY: The reference count will never be zero when this is
        // called.
        // NOTE: Commented out since our MSRV is too low.
        //unsafe {
        //    std::hint::assert_unchecked(refs != 0);
        //}

        let refs = refs.wrapping_add(1);
        self.refs.set(refs);

        // We want to abort on overflow instead of dropping the value.
        // Checking for overflow after the store instead of before
        // allows for slightly better code generation.
        if refs == 0 {
            unlikely_abort();
        }
    }

    #[inline]
    fn dec(&self) {
        self.refs.set(self.refs.get() - 1);
    }
}

pub type RcMemory<T> = Memory<RcInner<T>>;

pub struct Rc<T> {
    ptr: NonNull<RcInner<T>>,
    phantom: PhantomData<RcInner<T>>,
}

impl<T> Rc<T> {
    pub fn new(v: T) -> Self {
        let ptr = Box::leak(Box::new(RcInner {
            refs: Cell::new(1),
            memory: Cell::new(None),
            key: Cell::new(0),
            value: v,
        }));

        Self {
            ptr: ptr.into(),
            phantom: PhantomData,
        }
    }

    pub fn try_new_in(v: T, memory: &std::rc::Rc<RcMemory<T>>) -> Result<Self, AllocError> {
        let (key, ptr) = memory
            .insert(RcInner {
                refs: Cell::new(1),
                memory: Cell::new(Some(std::rc::Rc::clone(memory))),
                key: Cell::new(0),
                value: v,
            })
            .map_err(|_| AllocError)?;

        // SAFETY: ptr is not null and we promise to only use it immutably
        // despite casting it to *mut in order to construct NonNull
        let ptr = unsafe { NonNull::new_unchecked(ptr as *mut RcInner<T>) };

        // SAFETY: ptr is convertible to a reference
        unsafe { ptr.as_ref().key.set(key) };

        Ok(Self {
            ptr,
            phantom: PhantomData,
        })
    }

    #[inline(always)]
    fn inner(&self) -> &RcInner<T> {
        // SAFETY: ptr points to a slab element, and slab element addresses
        // are guaranteed to be stable once created, and the only place we
        // remove the pointed-to element is in Rc's drop method and only if
        // no other Rc instances are referencing it. Therefore, while this Rc
        // is alive, ptr is always valid.
        unsafe { self.ptr.as_ref() }
    }

    #[inline(never)]
    fn drop_slow(&mut self) {
        if let Some(memory) = self.inner().memory.take() {
            // Slab allocations contain a std::rc::Rc to the Memory they are
            // contained in, and we need to be careful the Memory is not
            // dropped while an entry is being removed. To ensure this, the
            // Rc is moved out of the entry above, and it is dropped only
            // after the entry is removed.

            let key = self.inner().key.get();

            memory.remove(key);
        } else {
            // If there is no reference to slab memory, then the memory is
            // managed by the system allocator. To free it, we simply convert
            // the pointer back to a Box and drop it.

            // SAFETY: While this Rc is alive, ptr is always valid
            unsafe { drop(Box::from_raw(self.ptr.as_mut())) };
        }
    }
}

impl<T> Clone for Rc<T> {
    #[inline]
    fn clone(&self) -> Self {
        self.inner().inc();

        Self {
            ptr: self.ptr,
            phantom: self.phantom,
        }
    }
}

impl<T> Deref for Rc<T> {
    type Target = T;

    #[inline(always)]
    fn deref(&self) -> &Self::Target {
        &self.inner().value
    }
}

impl<T> Drop for Rc<T> {
    #[inline]
    fn drop(&mut self) {
        self.inner().dec();
        if self.inner().refs.get() == 0 {
            self.drop_slow()
        }
    }
}

// Adapted from https://github.com/rust-lang/rfcs/pull/2802
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
    vec: Vec<u8>,
    size: usize,
    align: usize,
}

impl ReusableVec {
    pub fn new<T>(capacity: usize) -> Self {
        let size = mem::size_of::<T>();
        let align = mem::align_of::<T>();

        let vec: Vec<T> = Vec::with_capacity(capacity);

        // Safety: we must cast to Vec<U> before using, where U has the same
        // size and alignment as T
        let vec: Vec<u8> = unsafe { mem::transmute(vec) };

        Self { vec, size, align }
    }

    pub fn get_as_new<U>(&mut self) -> ReusableVecHandle<'_, U> {
        let size = mem::size_of::<U>();
        let align = mem::align_of::<U>();

        // If these don't match, panic. It's up the user to ensure the type
        // is acceptable
        assert_eq!(self.size, size);
        assert_eq!(self.align, align);

        let vec: &mut Vec<u8> = &mut self.vec;

        // Safety: U has the expected size and alignment
        let vec: &mut Vec<U> = unsafe { mem::transmute(vec) };

        // The vec starts empty, and is always cleared when the handle drops.
        // get_as_new() borrows self mutably, so it's not possible to create
        // a handle when one already exists
        assert!(vec.is_empty());

        ReusableVecHandle { vec }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct FlagOnDrop {
        dropped: std::rc::Rc<Cell<bool>>,
    }

    impl Drop for FlagOnDrop {
        fn drop(&mut self) {
            self.dropped.set(true);
        }
    }

    #[test]
    fn test_rc() {
        let dropped = std::rc::Rc::new(Cell::new(false));

        let f = FlagOnDrop {
            dropped: std::rc::Rc::clone(&dropped),
        };

        let a = Rc::new(f);
        assert_eq!(a.inner().refs.get(), 1);

        let b = Rc::clone(&a);
        assert_eq!(a.inner().refs.get(), 2);

        drop(a);
        assert_eq!(b.inner().refs.get(), 1);
        assert!(!dropped.get());

        drop(b);
        assert!(dropped.get());
    }

    #[test]
    fn test_rc_in() {
        let memory = std::rc::Rc::new(RcMemory::new(2));
        assert_eq!(memory.len(), 0);

        let e0a = Rc::try_new_in(123 as i32, &memory).unwrap();
        assert_eq!(memory.len(), 1);
        let p = memory.addr_of(0).unwrap();

        let e0b = Rc::clone(&e0a);
        assert_eq!(memory.len(), 1);
        assert_eq!(memory.addr_of(0).unwrap(), p);

        let e1a = Rc::try_new_in(456 as i32, &memory).unwrap();
        assert_eq!(memory.len(), 2);
        assert_eq!(memory.addr_of(0).unwrap(), p);

        // No room
        assert!(Rc::try_new_in(789 as i32, &memory).is_err());

        assert_eq!(*e0a, 123);
        assert_eq!(*e0b, 123);
        assert_eq!(*e1a, 456);

        mem::drop(e0b);
        assert_eq!(memory.len(), 2);
        assert_eq!(memory.addr_of(0).unwrap(), p);

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
