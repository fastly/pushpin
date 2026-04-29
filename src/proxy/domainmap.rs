/*
 * Copyright (C) 2026 Fastly, Inc.
 *
 * This file is part of Pushpin.
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

use std::ffi::CString;
use std::os::raw::c_int;
use std::ptr;

/// Route parameters returned by DomainMap lookups
#[repr(C)]
#[derive(Debug, Clone)]
pub struct RouteParams {
    pub log_level: c_int,
}

/// Rust wrapper for DomainMap FFI
pub struct DomainMap {
    handle: *mut crate::ffi::DomainMap,
}

impl DomainMap {
    /// Create a new DomainMap instance
    pub fn new(filename: &str) -> Self {
        let c_filename = CString::new(filename).expect("filename contains null byte");

        let handle = unsafe { crate::ffi::domainmap_create(c_filename.as_ptr()) };

        assert!(
            !handle.is_null(),
            "domainmap_create should never return null"
        );

        DomainMap { handle }
    }

    /// Look up route parameters by route ID
    pub fn lookup(&self, route_id: &str) -> Option<RouteParams> {
        let c_route_id = match CString::new(route_id) {
            Ok(s) => s,
            Err(_) => return None,
        };

        let mut ffi_params = crate::ffi::DomainMapRouteParams { log_level: 0 };

        let result = unsafe {
            crate::ffi::domainmap_entry_params(self.handle, c_route_id.as_ptr(), &mut ffi_params)
        };

        if result == 0 {
            Some(RouteParams {
                log_level: ffi_params.log_level,
            })
        } else {
            None
        }
    }
}

impl Drop for DomainMap {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe {
                crate::ffi::domainmap_destroy(self.handle);
            }
            self.handle = ptr::null_mut();
        }
    }
}

// Ensure the handle type is Send and Sync since DomainMap uses internal locking
// The C++ DomainMap is thread-safe with internal QMutex locking
unsafe impl Send for DomainMap {}
unsafe impl Sync for DomainMap {}
