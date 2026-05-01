/*
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

use std::thread;

mod ffi {
    use super::*;
    use std::ffi::{c_char, CStr};
    use std::ptr;

    mod sendable_pointer {
        pub struct SendablePointer<T>(*mut T);

        impl<T> SendablePointer<T> {
            // SAFETY: `p` must be safe to move to another thread.
            pub unsafe fn new(p: *mut T) -> Self {
                Self(p)
            }

            pub fn get(&self) -> *mut T {
                self.0
            }
        }

        unsafe impl<T> Send for SendablePointer<T> {}
    }

    use sendable_pointer::SendablePointer;

    pub struct ThreadJoinHandle(thread::JoinHandle<()>);

    /// `name` can be used to supply an optional name for the thread. If it is null, no explicit
    /// name will be set on the thread.
    //
    /// SAFETY: `ctx` must be safe to use from another thread, and `f` must be safe to call from
    /// another thread with `ctx` as its argument. If `name` is non-null, it must point to a valid
    /// C string.
    #[no_mangle]
    pub unsafe extern "C" fn thread_spawn(
        f: unsafe extern "C" fn(*mut libc::c_void),
        ctx: *mut libc::c_void,
        name: *const c_char,
    ) -> *mut ThreadJoinHandle {
        let name = if !name.is_null() {
            // SAFETY: We assume `name` is valid.
            unsafe {
                match CStr::from_ptr(name).to_str() {
                    Ok(s) => Some(s),
                    Err(_) => return ptr::null_mut(), // Invalid UTF-8
                }
            }
        } else {
            None
        };

        // SAFETY: `ctx` is safe to move to another thread.
        let ctx = unsafe { SendablePointer::new(ctx) };

        let builder = thread::Builder::new();

        let builder = if let Some(name) = name {
            builder.name(name.to_string())
        } else {
            builder
        };

        let thread = builder.spawn(move || unsafe { f(ctx.get()) }).unwrap();

        Box::into_raw(Box::new(ThreadJoinHandle(thread)))
    }

    /// Blocks until the thread completes and consumes the handle, invaliding the pointer.
    ///
    /// SAFETY: `handle` must point to a valid handle returned by `thread_spawn` that has not yet
    /// been invalidated.
    #[no_mangle]
    pub unsafe extern "C" fn thread_join(handle: *mut ThreadJoinHandle) {
        assert!(!handle.is_null());

        // SAFETY: We assume `handle` is valid.
        let thread = unsafe { Box::from_raw(handle).0 };

        thread.join().unwrap();
    }

    /// Discards the handle and leaves the thread running, invaliding the pointer.
    ///
    /// SAFETY: `handle` must point to a valid handle returned by `thread_spawn` that has not yet
    /// been invalidated.
    #[no_mangle]
    pub unsafe extern "C" fn thread_forget(handle: *mut ThreadJoinHandle) {
        assert!(!handle.is_null());

        // SAFETY: We assume `handle` is valid.
        let handle = unsafe { Box::from_raw(handle).0 };

        drop(handle);
    }
}
