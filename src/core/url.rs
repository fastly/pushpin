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

use crate::core::cow::CowArc;
use std::ops::{Deref, DerefMut};
use std::str::FromStr;
use url::{ParseError, Url};

#[derive(Clone)]
pub struct CowUrl(CowArc<Url>);

impl FromStr for CowUrl {
    type Err = ParseError;

    fn from_str(input: &str) -> Result<Self, Self::Err> {
        Ok(Self(CowArc::new(Url::parse(input)?)))
    }
}

impl Deref for CowUrl {
    type Target = Url;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl DerefMut for CowUrl {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
    }
}

// FFI interface
mod ffi {
    use super::*;
    use std::ffi::CStr;
    use std::os::raw::{c_char, c_int};
    use std::ptr;

    // The FFI handle is a *mut Url backed by Arc<Url>. Mutating functions consume the pointer
    // and return a (potentially new) pointer — callers must always update their stored pointer
    // with the return value, as Arc::make_mut may reallocate when the refcount is > 1.

    use std::mem::ManuallyDrop;
    use std::sync::Arc;

    unsafe fn arc_into_ptr(arc: Arc<Url>) -> *mut Url {
        Arc::into_raw(arc) as *mut Url
    }

    unsafe fn arc_from_ptr(ptr: *mut Url) -> Arc<Url> {
        assert!(!ptr.is_null());

        Arc::from_raw(ptr)
    }

    unsafe fn arc_borrow(ptr: *const Url) -> ManuallyDrop<Arc<Url>> {
        assert!(!ptr.is_null());

        ManuallyDrop::new(Arc::from_raw(ptr))
    }

    unsafe fn with_arc_mut<F, R>(url: *mut *mut Url, f: F) -> R
    where
        F: FnOnce(&mut Url) -> R,
    {
        let mut arc = arc_from_ptr(*url);
        let result = f(Arc::make_mut(&mut arc));
        *url = arc_into_ptr(arc);
        result
    }

    /// FFI-compatible owned string. Does not free its memory on drop; callers must pass it to
    /// `cow_url_data_delete` when done.
    #[repr(C)]
    pub struct CowUrlData {
        pub data: *mut c_char,
        pub len: usize,
    }

    impl CowUrlData {
        pub fn null() -> Self {
            Self {
                data: ptr::null_mut(),
                len: 0,
            }
        }

        pub fn is_null(&self) -> bool {
            self.data.is_null()
        }

        /// # Safety
        ///
        /// `data` and `len` must be populated from an earlier call to `from`.
        pub unsafe fn into_boxed_slice(self) -> Box<[u8]> {
            assert!(!self.data.is_null());

            Box::from_raw(std::ptr::slice_from_raw_parts_mut(
                self.data as *mut u8,
                self.len,
            ))
        }
    }

    impl From<String> for CowUrlData {
        fn from(s: String) -> Self {
            let s = s.into_bytes().into_boxed_slice();
            let len = s.len();

            Self {
                data: Box::into_raw(s) as *mut c_char,
                len,
            }
        }
    }

    /// Creates a CowUrl from a C string.
    ///
    /// # Safety
    ///
    /// - `input` must be a valid null-terminated C string in UTF-8 format
    /// - The returned pointer must be freed with `cow_url_destroy` to avoid memory leaks
    /// - The caller must ensure `input` remains valid for the duration of this call
    /// - Returns null on failure
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_from_string(input: *const c_char) -> *mut Url {
        assert!(!input.is_null());

        let input = match CStr::from_ptr(input).to_str() {
            Ok(s) => s,
            Err(_) => return ptr::null_mut(),
        };

        match Url::parse(input) {
            Ok(url) => arc_into_ptr(Arc::new(url)),
            Err(_) => ptr::null_mut(),
        }
    }

    /// Creates a clone of the given CowUrl. The returned pointer may equal the input pointer,
    /// as cloning only increments the Arc refcount without reallocating.
    ///
    /// # Safety
    ///
    /// - `url` must be a valid non-null pointer returned by a cow_url_* function
    /// - The returned pointer must be freed with `cow_url_destroy` to avoid memory leaks
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_clone(url: *const Url) -> *mut Url {
        let arc = arc_borrow(url);

        arc_into_ptr(Arc::clone(&*arc))
    }

    /// Frees a CowUrl.
    ///
    /// # Safety
    ///
    /// - `url` must be a valid pointer returned by a cow_url_* function or null
    /// - After calling this function, the pointer becomes invalid and must not be used
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_destroy(url: *mut Url) {
        if !url.is_null() {
            drop(arc_from_ptr(url));
        }
    }

    /// Gets the scheme of a URL.
    ///
    /// # Safety
    ///
    /// - `url` must be a valid non-null pointer returned by a cow_url_* function
    /// - The returned string must be freed with `cow_url_data_delete` to avoid memory leaks
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_scheme(url: *const Url) -> CowUrlData {
        arc_borrow(url).scheme().to_string().into()
    }

    /// Sets the scheme of a URL. Updates `*url` with the (potentially new) pointer. Returns 0
    /// on success, -1 on failure. `*url` is always updated when the pointer was consumed.
    ///
    /// # Safety
    ///
    /// - `url` must be a non-null pointer to a non-null pointer returned by a cow_url_* function
    /// - `scheme` must be a valid non-null null-terminated C string in UTF-8 format
    /// - The caller must ensure `scheme` remains valid for the duration of this call
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_set_scheme(
        url: *mut *mut Url,
        scheme: *const c_char,
    ) -> c_int {
        assert!(!url.is_null());
        assert!(!scheme.is_null());

        let scheme = match CStr::from_ptr(scheme).to_str() {
            Ok(s) => s,
            Err(_) => return -1,
        };

        if with_arc_mut(url, |u| u.set_scheme(scheme)).is_err() {
            return -1;
        }

        0
    }

    /// Gets the path of a URL.
    ///
    /// # Safety
    ///
    /// - `url` must be a valid non-null pointer returned by a cow_url_* function
    /// - The returned string must be freed with `cow_url_data_delete` to avoid memory leaks
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_path(url: *const Url) -> CowUrlData {
        arc_borrow(url).path().to_string().into()
    }

    /// Sets the path of a URL. Updates `*url` with the (potentially new) pointer. Returns 0
    /// on success, -1 on failure. On failure, `*url` is left unchanged.
    ///
    /// # Safety
    ///
    /// - `url` must be a non-null pointer to a non-null pointer returned by a cow_url_* function
    /// - `path` must be a valid non-null null-terminated C string in UTF-8 format
    /// - The caller must ensure `path` remains valid for the duration of this call
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_set_path(url: *mut *mut Url, path: *const c_char) -> c_int {
        assert!(!url.is_null());
        assert!(!path.is_null());

        let path = match CStr::from_ptr(path).to_str() {
            Ok(s) => s,
            Err(_) => return -1,
        };

        with_arc_mut(url, |u| u.set_path(path));

        0
    }

    /// Checks if a URL has a query string. Returns 1 if true, 0 if false.
    ///
    /// # Safety
    ///
    /// - `url` must be a valid non-null pointer returned by a cow_url_* function
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_has_query(url: *const Url) -> c_int {
        if arc_borrow(url).query().is_some() {
            1
        } else {
            0
        }
    }

    /// Gets the query string of a URL.
    ///
    /// # Safety
    ///
    /// - `url` must be a valid non-null pointer returned by a cow_url_* function
    /// - The returned string must be freed with `cow_url_data_delete` to avoid memory leaks
    /// - Returns null data if the URL has no query string
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_query(url: *const Url) -> CowUrlData {
        match arc_borrow(url).query() {
            Some(q) => q.to_string().into(),
            None => CowUrlData::null(),
        }
    }

    /// Sets the query string of a URL. Updates `*url` with the (potentially new) pointer.
    /// Returns 0 on success, -1 on failure. On failure, `*url` is left unchanged. Pass null
    /// for `query` to clear the query string.
    ///
    /// # Safety
    ///
    /// - `url` must be a non-null pointer to a non-null pointer returned by a cow_url_* function
    /// - `query` must be a valid null-terminated C string in UTF-8 format, or null
    /// - The caller must ensure `query` remains valid for the duration of this call
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_set_query(url: *mut *mut Url, query: *const c_char) -> c_int {
        assert!(!url.is_null());

        let query = if !query.is_null() {
            match CStr::from_ptr(query).to_str() {
                Ok(s) => s,
                Err(_) => return -1,
            }
        } else {
            ""
        };

        with_arc_mut(url, |u| {
            u.set_query(if !query.is_empty() { Some(query) } else { None })
        });

        0
    }

    /// Gets the host of a URL.
    ///
    /// # Safety
    ///
    /// - `url` must be a valid non-null pointer returned by a cow_url_* function
    /// - The returned string must be freed with `cow_url_data_delete` to avoid memory leaks
    /// - Returns null data if the URL has no host
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_host(url: *const Url) -> CowUrlData {
        match arc_borrow(url).host_str() {
            Some(h) => h.to_string().into(),
            None => CowUrlData::null(),
        }
    }

    /// Sets the host of a URL. Updates `*url` with the (potentially new) pointer. Returns 0
    /// on success, -1 on failure. `*url` is always updated when the pointer was consumed.
    ///
    /// # Safety
    ///
    /// - `url` must be a non-null pointer to a non-null pointer returned by a cow_url_* function
    /// - `host` must be a valid non-null null-terminated C string in UTF-8 format
    /// - The caller must ensure `host` remains valid for the duration of this call
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_set_host(url: *mut *mut Url, host: *const c_char) -> c_int {
        assert!(!url.is_null());
        assert!(!host.is_null());

        let host = match CStr::from_ptr(host).to_str() {
            Ok(s) => s,
            Err(_) => return -1,
        };

        if with_arc_mut(url, |u| u.set_host(Some(host))).is_err() {
            return -1;
        }

        0
    }

    /// Gets the port of a URL.
    ///
    /// # Safety
    ///
    /// - `url` must be a valid non-null pointer returned by a cow_url_* function
    /// - Returns -1 if no port is specified
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_port(url: *const Url) -> i32 {
        arc_borrow(url).port().map_or(-1, |p| p as i32)
    }

    /// Sets the port of a URL. Updates `*url` with the (potentially new) pointer. Returns 0
    /// on success, -1 on failure. `*url` is always updated. Pass a value outside 1–65535 to
    /// clear the port.
    ///
    /// # Safety
    ///
    /// - `url` must be a non-null pointer to a non-null pointer returned by a cow_url_* function
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_set_port(url: *mut *mut Url, port: i32) -> c_int {
        assert!(!url.is_null());

        let p = if port > 0 && port <= 65535 {
            Some(port as u16)
        } else {
            None
        };

        if with_arc_mut(url, |u| u.set_port(p)).is_err() {
            return -1;
        }

        0
    }

    /// Converts a URL to a string representation.
    ///
    /// # Safety
    ///
    /// - `url` must be a valid non-null pointer returned by a cow_url_* function
    /// - The returned string must be freed with `cow_url_data_delete` to avoid memory leaks
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_to_string(url: *const Url) -> CowUrlData {
        arc_borrow(url).to_string().into()
    }

    /// Frees a `CowUrlData` returned by URL functions. Safe to call on null data.
    ///
    /// # Safety
    ///
    /// - `data` must be a `CowUrlData` returned by a cow_url_* function, or null data
    /// - After calling this function, `data` becomes invalid and must not be used
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_data_delete(data: CowUrlData) {
        if !data.is_null() {
            drop(data.into_boxed_slice());
        }
    }

    /// Joins a base URL with a relative URL using RFC 3986 URL resolution.
    ///
    /// # Safety
    ///
    /// - `base` must be a valid non-null pointer returned by a cow_url_* function
    /// - `relative` must be a valid null-terminated C string in UTF-8 format
    /// - The caller must ensure `relative` remains valid for the duration of this call
    /// - The returned pointer must be freed with `cow_url_destroy` to avoid memory leaks
    #[no_mangle]
    pub unsafe extern "C" fn cow_url_join(base: *const Url, relative: *const c_char) -> *mut Url {
        assert!(!relative.is_null());

        let relative = match CStr::from_ptr(relative).to_str() {
            Ok(s) => s,
            Err(_) => return ptr::null_mut(),
        };

        let resolved = match arc_borrow(base).join(relative) {
            Ok(u) => u,
            Err(_) => return ptr::null_mut(),
        };

        arc_into_ptr(Arc::new(resolved))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_basic_url_operations() {
        let url: CowUrl = "https://example.com/path?query=value".parse().unwrap();
        assert_eq!(url.scheme(), "https");
        assert_eq!(url.path(), "/path");
        assert_eq!(url.query(), Some("query=value"));
        assert!(url.query().is_some());
    }

    #[test]
    fn test_invalid_url() {
        assert!("not a url".parse::<CowUrl>().is_err());
    }
}
