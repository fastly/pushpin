/*
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

//! URL utilities that replace Qt's QUrl with Rust's url crate via FFI

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::panic;
use std::str::FromStr;
use std::sync::Arc;
use url::Url;

// Option<Arc> wrapper for implicit sharing like QUrl
// Each SharedUrl has independent validity, but shares underlying URL data when both valid
#[derive(Clone, Default)]
pub struct SharedUrl(Option<Arc<Url>>);

impl SharedUrl {
    pub fn new() -> Self {
        SharedUrl(None)
    }

    pub fn from_encoded(encoded: &[u8], strict: bool) -> Self {
        let input = match std::str::from_utf8(encoded) {
            Ok(s) => s,
            Err(_) => return SharedUrl(None),
        };

        match Url::parse(input) {
            Ok(url) => SharedUrl(Some(Arc::new(url))),
            Err(_) => {
                if !strict {
                    // Try to be more permissive in non-strict mode
                    // For now, just fail - we could add heuristics later
                }
                SharedUrl(None)
            }
        }
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_none()
    }

    pub fn is_valid(&self) -> bool {
        self.0.is_some()
    }

    pub fn clear(&mut self) {
        // Just set this instance to None - doesn't affect other SharedUrl instances
        // that may share the same underlying URL data
        self.0 = None;
    }

    pub fn set_scheme(&mut self, scheme: &str) {
        if let Some(ref mut url_arc) = self.0 {
            // Use Arc::make_mut for copy-on-write: only clones the URL data
            // if there are multiple references to it
            let url_mut = Arc::make_mut(url_arc);
            let _ = url_mut.set_scheme(scheme);
        }
    }

    pub fn scheme(&self) -> String {
        match self.0.as_ref() {
            Some(url) => url.scheme().to_string(),
            None => String::new(),
        }
    }

    pub fn path(&self, _encoding: UrlEncoding) -> String {
        match self.0.as_ref() {
            Some(url) => url.path().to_string(),
            None => String::new(),
        }
    }

    pub fn query(&self, _encoding: UrlEncoding) -> String {
        match self.0.as_ref() {
            Some(url) => url.query().unwrap_or("").to_string(),
            None => String::new(),
        }
    }

    pub fn has_query(&self) -> bool {
        match self.0.as_ref() {
            Some(url) => url.query().is_some(),
            None => false,
        }
    }

    pub fn host(&self) -> String {
        match self.0.as_ref() {
            Some(url) => url.host_str().unwrap_or("").to_string(),
            None => String::new(),
        }
    }

    pub fn port(&self) -> i32 {
        match self.0.as_ref() {
            Some(url) => url.port().map(|p| p as i32).unwrap_or(-1),
            None => -1,
        }
    }

    pub fn authority(&self) -> String {
        match self.0.as_ref() {
            Some(url) => {
                if let Some(host) = url.host_str() {
                    if let Some(port) = url.port() {
                        format!("{}:{}", host, port)
                    } else {
                        host.to_string()
                    }
                } else {
                    String::new()
                }
            }
            None => String::new(),
        }
    }

    pub fn set_host(&mut self, host: &str) {
        if let Some(ref mut url_arc) = self.0 {
            let url_mut = Arc::make_mut(url_arc);
            let _ = url_mut.set_host(Some(host));
        }
    }

    pub fn set_port(&mut self, port: i32) {
        if let Some(ref mut url_arc) = self.0 {
            let url_mut = Arc::make_mut(url_arc);
            if port <= 0 || port > 65535 {
                let _ = url_mut.set_port(None);
            } else {
                let _ = url_mut.set_port(Some(port as u16));
            }
        }
    }

    pub fn set_path(&mut self, path: &str) {
        if let Some(ref mut url_arc) = self.0 {
            let url_mut = Arc::make_mut(url_arc);
            url_mut.set_path(path);
        }
    }

    pub fn set_query(&mut self, query: Option<&str>) {
        if let Some(ref mut url_arc) = self.0 {
            let url_mut = Arc::make_mut(url_arc);
            url_mut.set_query(query);
        }
    }

    pub fn to_string(&self, _encoding: UrlEncoding) -> String {
        match self.0.as_ref() {
            Some(url) => url.to_string(),
            None => String::new(),
        }
    }

    pub fn to_encoded(&self) -> Vec<u8> {
        match self.0.as_ref() {
            Some(url) => url.to_string().into_bytes(),
            None => Vec::new(),
        }
    }

    pub fn join(&self, relative: &str) -> Self {
        match self.0.as_ref() {
            Some(base_url) => match base_url.join(relative) {
                Ok(resolved_url) => SharedUrl(Some(Arc::new(resolved_url))),
                Err(_) => SharedUrl(None),
            },
            None => SharedUrl(None),
        }
    }
}

impl FromStr for SharedUrl {
    type Err = ();

    fn from_str(input: &str) -> Result<Self, Self::Err> {
        match Url::parse(input) {
            Ok(url) => Ok(SharedUrl(Some(Arc::new(url)))),
            Err(_) => Ok(SharedUrl(None)), // Return invalid URL instead of error
        }
    }
}

// Encoding modes (QUrl compatibility)
#[repr(C)]
pub enum UrlEncoding {
    FullyEncoded = 0,
}

#[repr(C)]
pub enum UrlParsingMode {
    StrictMode = 0,
}

// FFI interface
pub mod ffi {
    use super::*;

    // Opaque handle for C++
    pub type CUrlHandle = *mut SharedUrl;

    /// Creates a new empty URL handle.
    ///
    /// # Safety
    ///
    /// The returned handle must be freed with `url_delete` to avoid memory leaks.
    #[no_mangle]
    pub unsafe extern "C" fn url_new() -> CUrlHandle {
        Box::into_raw(Box::new(SharedUrl::new()))
    }

    /// Creates a URL handle from a C string.
    ///
    /// # Safety
    ///
    /// - `input` must be a valid null-terminated C string or null
    /// - The returned handle must be freed with `url_delete` to avoid memory leaks
    /// - The caller must ensure `input` remains valid for the duration of this call
    #[no_mangle]
    pub unsafe extern "C" fn url_from_string(input: *const c_char) -> CUrlHandle {
        let result = panic::catch_unwind(|| {
            if input.is_null() {
                return Box::into_raw(Box::new(SharedUrl::new()));
            }

            let c_str = CStr::from_ptr(input);
            let input_str = match c_str.to_str() {
                Ok(s) => s,
                Err(_) => return Box::into_raw(Box::new(SharedUrl::new())),
            };

            Box::into_raw(Box::new(SharedUrl::from_str(input_str).unwrap_or_default()))
        });

        match result {
            Ok(handle) => handle,
            Err(_) => Box::into_raw(Box::new(SharedUrl::new())), // Return invalid URL on panic
        }
    }

    /// Creates a URL handle from encoded bytes.
    ///
    /// # Safety
    ///
    /// - `encoded` must point to valid memory of at least `len` bytes, or be null if `len` is 0
    /// - The memory pointed to by `encoded` must remain valid for the duration of this call
    /// - The returned handle must be freed with `url_delete` to avoid memory leaks
    #[no_mangle]
    pub unsafe extern "C" fn url_from_encoded(
        encoded: *const u8,
        len: usize,
        mode: UrlParsingMode,
    ) -> CUrlHandle {
        let result = panic::catch_unwind(|| {
            if encoded.is_null() || len == 0 {
                return Box::into_raw(Box::new(SharedUrl::new()));
            }

            let slice = std::slice::from_raw_parts(encoded, len);
            let strict = match mode {
                UrlParsingMode::StrictMode => true,
            };

            Box::into_raw(Box::new(SharedUrl::from_encoded(slice, strict)))
        });

        match result {
            Ok(handle) => handle,
            Err(_) => Box::into_raw(Box::new(SharedUrl::new())), // Return invalid URL on panic
        }
    }

    /// Creates a clone of the given URL handle.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    /// - The returned handle must be freed with `url_delete` to avoid memory leaks
    #[no_mangle]
    pub unsafe extern "C" fn url_clone(handle: CUrlHandle) -> CUrlHandle {
        if handle.is_null() {
            return Box::into_raw(Box::new(SharedUrl::new()));
        }

        let url = &*handle;
        Box::into_raw(Box::new(url.clone()))
    }

    /// Frees a URL handle.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    /// - After calling this function, `handle` becomes invalid and must not be used
    #[no_mangle]
    pub unsafe extern "C" fn url_delete(handle: CUrlHandle) {
        if !handle.is_null() {
            let _ = Box::from_raw(handle);
        }
    }

    /// Checks if a URL handle represents an empty URL.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    #[no_mangle]
    pub unsafe extern "C" fn url_is_empty(handle: CUrlHandle) -> bool {
        if handle.is_null() {
            return true;
        }
        (*handle).is_empty()
    }

    /// Checks if a URL handle represents a valid URL.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    #[no_mangle]
    pub unsafe extern "C" fn url_is_valid(handle: CUrlHandle) -> bool {
        if handle.is_null() {
            return false;
        }
        (*handle).is_valid()
    }

    /// Clears a URL handle, making it empty.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    #[no_mangle]
    pub unsafe extern "C" fn url_clear(handle: CUrlHandle) {
        if !handle.is_null() {
            (*handle).clear();
        }
    }

    /// Sets the scheme of a URL.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    /// - `scheme` must be a valid null-terminated C string or null
    /// - The caller must ensure `scheme` remains valid for the duration of this call
    #[no_mangle]
    pub unsafe extern "C" fn url_set_scheme(handle: CUrlHandle, scheme: *const c_char) {
        if handle.is_null() || scheme.is_null() {
            return;
        }

        let c_str = CStr::from_ptr(scheme);
        if let Ok(scheme_str) = c_str.to_str() {
            (*handle).set_scheme(scheme_str);
        }
    }

    /// Gets the scheme of a URL as a C string.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    /// - The returned string must be freed with `url_string_delete` to avoid memory leaks
    /// - Returns null if the handle is null or if string allocation fails
    #[no_mangle]
    pub unsafe extern "C" fn url_scheme(handle: CUrlHandle) -> *mut c_char {
        if handle.is_null() {
            return std::ptr::null_mut();
        }

        let scheme = (*handle).scheme();
        match CString::new(scheme) {
            Ok(c_string) => c_string.into_raw(),
            Err(_) => std::ptr::null_mut(),
        }
    }

    /// Gets the path of a URL as a C string.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    /// - The returned string must be freed with `url_string_delete` to avoid memory leaks
    /// - Returns null if the handle is null or if string allocation fails
    #[no_mangle]
    pub unsafe extern "C" fn url_path(handle: CUrlHandle, encoding: UrlEncoding) -> *mut c_char {
        if handle.is_null() {
            return std::ptr::null_mut();
        }

        let path = (*handle).path(encoding);
        match CString::new(path) {
            Ok(c_string) => c_string.into_raw(),
            Err(_) => std::ptr::null_mut(),
        }
    }

    /// Gets the query string of a URL as a C string.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    /// - The returned string must be freed with `url_string_delete` to avoid memory leaks
    /// - Returns null if the handle is null or if string allocation fails
    #[no_mangle]
    pub unsafe extern "C" fn url_query(handle: CUrlHandle, encoding: UrlEncoding) -> *mut c_char {
        if handle.is_null() {
            return std::ptr::null_mut();
        }

        let query = (*handle).query(encoding);
        match CString::new(query) {
            Ok(c_string) => c_string.into_raw(),
            Err(_) => std::ptr::null_mut(),
        }
    }

    /// Checks if a URL has a query string.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    #[no_mangle]
    pub unsafe extern "C" fn url_has_query(handle: CUrlHandle) -> bool {
        if handle.is_null() {
            return false;
        }
        (*handle).has_query()
    }

    /// Gets the host of a URL as a C string.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    /// - The returned string must be freed with `url_string_delete` to avoid memory leaks
    /// - Returns null if the handle is null or if string allocation fails
    #[no_mangle]
    pub unsafe extern "C" fn url_host(handle: CUrlHandle) -> *mut c_char {
        if handle.is_null() {
            return std::ptr::null_mut();
        }

        let host = (*handle).host();
        match CString::new(host) {
            Ok(c_string) => c_string.into_raw(),
            Err(_) => std::ptr::null_mut(),
        }
    }

    /// Gets the port of a URL.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    /// - Returns -1 if the handle is null or if no port is specified
    #[no_mangle]
    pub unsafe extern "C" fn url_port(handle: CUrlHandle) -> i32 {
        if handle.is_null() {
            return -1;
        }
        (*handle).port()
    }

    /// Gets the authority (host:port) of a URL as a C string.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    /// - The returned string must be freed with `url_string_delete` to avoid memory leaks
    /// - Returns null if the handle is null or if string allocation fails
    #[no_mangle]
    pub unsafe extern "C" fn url_authority(handle: CUrlHandle) -> *mut c_char {
        if handle.is_null() {
            return std::ptr::null_mut();
        }

        let authority = (*handle).authority();
        match CString::new(authority) {
            Ok(c_string) => c_string.into_raw(),
            Err(_) => std::ptr::null_mut(),
        }
    }

    /// Sets the host of a URL.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    /// - `host` must be a valid null-terminated C string or null
    /// - The caller must ensure `host` remains valid for the duration of this call
    #[no_mangle]
    pub unsafe extern "C" fn url_set_host(handle: CUrlHandle, host: *const c_char) {
        if handle.is_null() || host.is_null() {
            return;
        }

        let c_str = CStr::from_ptr(host);
        if let Ok(host_str) = c_str.to_str() {
            (*handle).set_host(host_str);
        }
    }

    /// Sets the port of a URL.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    #[no_mangle]
    pub unsafe extern "C" fn url_set_port(handle: CUrlHandle, port: i32) {
        if !handle.is_null() {
            (*handle).set_port(port);
        }
    }

    /// Sets the path of a URL.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    /// - `path` must be a valid null-terminated C string or null
    /// - The caller must ensure `path` remains valid for the duration of this call
    #[no_mangle]
    pub unsafe extern "C" fn url_set_path(handle: CUrlHandle, path: *const c_char) {
        if handle.is_null() || path.is_null() {
            return;
        }

        let c_str = CStr::from_ptr(path);
        if let Ok(path_str) = c_str.to_str() {
            (*handle).set_path(path_str);
        }
    }

    /// Sets the query string of a URL.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    /// - `query` must be a valid null-terminated C string or null (null clears the query)
    /// - The caller must ensure `query` remains valid for the duration of this call
    #[no_mangle]
    pub unsafe extern "C" fn url_set_query(handle: CUrlHandle, query: *const c_char) {
        if handle.is_null() {
            return;
        }

        if query.is_null() {
            (*handle).set_query(None);
        } else {
            let c_str = CStr::from_ptr(query);
            if let Ok(query_str) = c_str.to_str() {
                if query_str.is_empty() {
                    (*handle).set_query(None);
                } else {
                    (*handle).set_query(Some(query_str));
                }
            }
        }
    }

    /// Converts a URL to a string representation.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    /// - The returned string must be freed with `url_string_delete` to avoid memory leaks
    /// - Returns null if the handle is null or if string allocation fails
    #[no_mangle]
    pub unsafe extern "C" fn url_to_string(
        handle: CUrlHandle,
        encoding: UrlEncoding,
    ) -> *mut c_char {
        let result = panic::catch_unwind(|| {
            if handle.is_null() {
                return std::ptr::null_mut();
            }

            let url_str = (*handle).to_string(encoding);
            match CString::new(url_str) {
                Ok(c_string) => c_string.into_raw(),
                Err(_) => std::ptr::null_mut(),
            }
        });

        match result {
            Ok(ptr) => ptr,
            Err(_) => std::ptr::null_mut(), // Return null on panic
        }
    }

    /// Converts a URL to encoded bytes.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle returned by a url_* function or null
    /// - `out_len` must point to a valid usize that will receive the length of the returned data
    /// - The returned bytes must be freed with `url_bytes_delete` to avoid memory leaks
    /// - Returns null if the handle is null, out_len is null, or if allocation fails
    #[no_mangle]
    pub unsafe extern "C" fn url_to_encoded(handle: CUrlHandle, out_len: *mut usize) -> *mut u8 {
        if handle.is_null() || out_len.is_null() {
            if !out_len.is_null() {
                *out_len = 0;
            }
            return std::ptr::null_mut();
        }

        let encoded = (*handle).to_encoded();
        let len = encoded.len();
        *out_len = len;

        if encoded.is_empty() {
            return std::ptr::null_mut();
        }

        // Allocate memory that includes the length header for proper deallocation
        let total_size = std::mem::size_of::<usize>() + len;
        let layout =
            match std::alloc::Layout::from_size_align(total_size, std::mem::align_of::<usize>()) {
                Ok(layout) => layout,
                Err(_) => return std::ptr::null_mut(),
            };
        let ptr = std::alloc::alloc(layout);

        if ptr.is_null() {
            return std::ptr::null_mut();
        }

        // Store the length at the beginning
        *(ptr as *mut usize) = len;

        // Copy the data after the length
        let data_ptr = ptr.add(std::mem::size_of::<usize>());
        std::ptr::copy_nonoverlapping(encoded.as_ptr(), data_ptr, len);

        data_ptr
    }

    /// Frees a C string returned by URL functions.
    ///
    /// # Safety
    ///
    /// - `ptr` must be a string returned by a url_* function or null
    /// - After calling this function, `ptr` becomes invalid and must not be used
    #[no_mangle]
    pub unsafe extern "C" fn url_string_delete(ptr: *mut c_char) {
        if !ptr.is_null() {
            let _ = CString::from_raw(ptr);
        }
    }

    /// Frees bytes returned by URL functions.
    ///
    /// # Safety
    ///
    /// - `ptr` must be bytes returned by a url_* function or null
    /// - After calling this function, `ptr` becomes invalid and must not be used
    #[no_mangle]
    pub unsafe extern "C" fn url_bytes_delete(ptr: *mut u8) {
        if !ptr.is_null() {
            // The pointer points to the data, but the length is stored just before it
            let len_ptr = ptr.sub(std::mem::size_of::<usize>()) as *mut usize;
            let len = *len_ptr;

            let total_size = std::mem::size_of::<usize>() + len;
            let layout = match std::alloc::Layout::from_size_align(
                total_size,
                std::mem::align_of::<usize>(),
            ) {
                Ok(layout) => layout,
                Err(_) => return, // Can't deallocate if layout creation fails
            };

            std::alloc::dealloc(len_ptr as *mut u8, layout);
        }
    }

    /// Joins a base URL with a relative URL using RFC 3986 URL resolution.
    ///
    /// # Safety
    ///
    /// - `base_handle` must be a valid handle returned by a url_* function or null
    /// - `relative` must be a valid null-terminated C string or null
    /// - The caller must ensure `relative` remains valid for the duration of this call
    /// - The returned handle must be freed with `url_delete` to avoid memory leaks
    #[no_mangle]
    pub unsafe extern "C" fn url_join(
        base_handle: CUrlHandle,
        relative: *const c_char,
    ) -> CUrlHandle {
        let result = panic::catch_unwind(|| {
            if base_handle.is_null() || relative.is_null() {
                return Box::into_raw(Box::new(SharedUrl::new()));
            }

            let c_str = CStr::from_ptr(relative);
            let relative_str = match c_str.to_str() {
                Ok(s) => s,
                Err(_) => return Box::into_raw(Box::new(SharedUrl::new())),
            };

            let joined = (*base_handle).join(relative_str);
            Box::into_raw(Box::new(joined))
        });

        match result {
            Ok(handle) => handle,
            Err(_) => Box::into_raw(Box::new(SharedUrl::new())), // Return invalid URL on panic
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_basic_url_operations() {
        let url = SharedUrl::from_str("https://example.com/path?query=value").unwrap();
        assert!(url.is_valid());
        assert!(!url.is_empty());
        assert_eq!(url.scheme(), "https");
        assert_eq!(url.path(UrlEncoding::FullyEncoded), "/path");
        assert_eq!(url.query(UrlEncoding::FullyEncoded), "query=value");
        assert!(url.has_query());
    }

    #[test]
    fn test_invalid_url() {
        let url = SharedUrl::from_str("not a url").unwrap();
        assert!(!url.is_valid());
        assert!(url.is_empty());
    }

    #[test]
    fn test_empty_url() {
        let url = SharedUrl::new();
        assert!(!url.is_valid());
        assert!(url.is_empty());
        assert_eq!(url.scheme(), "");
    }

    #[test]
    fn test_implicit_sharing() {
        let url1 = SharedUrl::from_str("https://example.com").unwrap();
        let url2 = url1.clone();

        // Both should work and share the same Arc
        assert_eq!(url1.scheme(), "https");
        assert_eq!(url2.scheme(), "https");
    }

    #[test]
    fn test_copy_on_write() {
        let url1 = SharedUrl::from_str("https://example.com").unwrap();
        let mut url2 = url1.clone();

        // Both start with same scheme
        assert_eq!(url1.scheme(), "https");
        assert_eq!(url2.scheme(), "https");

        // Mutating url2 should not affect url1 (copy-on-write)
        url2.set_scheme("http");

        assert_eq!(url1.scheme(), "https"); // unchanged
        assert_eq!(url2.scheme(), "http"); // changed

        // Test independent validity - clearing one doesn't affect the other
        let mut url3 = url1.clone();
        url3.clear();

        assert!(url1.is_valid()); // original still valid
        assert!(!url3.is_valid()); // cleared copy is invalid
    }

    #[test]
    fn test_independent_validity() {
        // This test demonstrates why Option<Arc<T>> is better than Arc<Option<T>>
        // Each SharedUrl has independent validity state
        let url1 = SharedUrl::from_str("https://example.com").unwrap();
        let url2 = url1.clone(); // shares the same URL data
        let mut url3 = url1.clone(); // also shares the same URL data

        // All three share the same underlying URL
        assert_eq!(url1.scheme(), "https");
        assert_eq!(url2.scheme(), "https");
        assert_eq!(url3.scheme(), "https");

        // Clearing url3 only affects url3, not url1 or url2
        url3.clear();

        assert!(url1.is_valid()); // still valid
        assert!(url2.is_valid()); // still valid
        assert!(!url3.is_valid()); // invalid

        // url1 and url2 still work normally
        assert_eq!(url1.scheme(), "https");
        assert_eq!(url2.scheme(), "https");
        assert_eq!(url3.scheme(), ""); // empty for invalid URL
    }

    #[test]
    fn test_single_reference_mutation() {
        let mut url = SharedUrl::from_str("https://example.com").unwrap();

        // Should be able to mutate in-place since there's only one reference
        url.set_scheme("http");
        assert_eq!(url.scheme(), "http");

        url.clear();
        assert!(!url.is_valid());
    }
}
