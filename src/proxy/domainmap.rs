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

use crate::core::log::ffi::int_to_optional_level_filter;
use std::ffi::CString;
use std::ptr;

/// Route parameters returned by DomainMap lookups
#[derive(Debug, Clone)]
pub struct RouteParams {
    pub log_level: Option<log::LevelFilter>,
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
    pub fn lookup(&self, route_id: &str, path: Option<&str>) -> Option<RouteParams> {
        let c_route_id = match CString::new(route_id) {
            Ok(s) => s,
            Err(_) => return None,
        };

        let c_path = if let Some(path) = path {
            match CString::new(path) {
                Ok(s) => Some(s),
                Err(_) => return None,
            }
        } else {
            None
        };

        let c_path_ptr = match c_path {
            Some(s) => s.as_ptr(),
            None => ptr::null(),
        };

        let mut ffi_params = crate::ffi::DomainMapRouteParams { log_level: -1 };

        let result = unsafe {
            crate::ffi::domainmap_entry_params(
                self.handle,
                c_route_id.as_ptr(),
                c_path_ptr,
                &mut ffi_params,
            )
        };

        if result != 0 {
            return None;
        }

        let log_level = int_to_optional_level_filter(ffi_params.log_level);

        Some(RouteParams { log_level })
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

// Ensure the handle type is Send and Sync since DomainMap uses internal locking.
// The C++ DomainMap is thread-safe with internal QMutex locking
unsafe impl Send for DomainMap {}
unsafe impl Sync for DomainMap {}
