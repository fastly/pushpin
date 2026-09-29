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

use std::ffi::CStr;

/// Declares both a &str and a &CStr from a single input string at compile time.
macro_rules! dual_str {
    ($lit:expr) => {{
        // Emit one instance of the string data that both types can use
        const BYTES: &'static [u8] = concat!($lit, "\0").as_bytes();

        // The &CStr will refer to the whole slice, including the nul byte
        const C: &'static CStr = match CStr::from_bytes_with_nul(BYTES) {
            Ok(c) => c,
            Err(_) => panic!("Invalid CStr"), // Could happen if input contains nul byte
        };

        // The &str will exclude the nul byte.
        const STR_BYTES: &'static [u8] = match BYTES.split_last() {
            Some((_, rest)) => rest,
            None => unreachable!(), // Input will always have a nul byte
        };

        const S: &'static str = match std::str::from_utf8(STR_BYTES) {
            Ok(s) => s,
            Err(_) => unreachable!(), // Input guaranteed to be in UTF-8
        };

        (S, C)
    }};
}

// IANA assignments
// http://www.iana.org/assignments/http-status-codes/http-status-codes.xml
const fn get_reason_inner(code: u16) -> (&'static str, &'static CStr) {
    match code {
        100 => dual_str!("Continue"),
        101 => dual_str!("Switching Protocols"),
        102 => dual_str!("Processing"),
        200 => dual_str!("OK"),
        201 => dual_str!("Created"),
        202 => dual_str!("Accepted"),
        203 => dual_str!("Non-Authoritative Information"),
        204 => dual_str!("No Content"),
        205 => dual_str!("Reset Content"),
        206 => dual_str!("Partial Content"),
        207 => dual_str!("Multi-Status"),
        208 => dual_str!("Already Reported"),
        226 => dual_str!("IM Used"),
        300 => dual_str!("Multiple Choices"),
        301 => dual_str!("Moved Permanently"),
        302 => dual_str!("Found"),
        303 => dual_str!("See Other"),
        304 => dual_str!("Not Modified"),
        305 => dual_str!("Use Proxy"),
        306 => dual_str!("Reserved"),
        307 => dual_str!("Temporary Redirect"),
        308 => dual_str!("Permanent Redirect"),
        400 => dual_str!("Bad Request"),
        401 => dual_str!("Unauthorized"),
        402 => dual_str!("Payment Required"),
        403 => dual_str!("Forbidden"),
        404 => dual_str!("Not Found"),
        405 => dual_str!("Method Not Allowed"),
        406 => dual_str!("Not Acceptable"),
        407 => dual_str!("Proxy Authentication Required"),
        408 => dual_str!("Request Timeout"),
        409 => dual_str!("Conflict"),
        410 => dual_str!("Gone"),
        411 => dual_str!("Length Required"),
        412 => dual_str!("Precondition Failed"),
        413 => dual_str!("Request Entity Too Large"),
        414 => dual_str!("Request-URI Too Long"),
        415 => dual_str!("Unsupported Media Type"),
        416 => dual_str!("Requested Range Not Satisfiable"),
        417 => dual_str!("Expectation Failed"),
        422 => dual_str!("Unprocessable Entity"),
        423 => dual_str!("Locked"),
        424 => dual_str!("Failed Dependency"),
        426 => dual_str!("Upgrade Required"),
        428 => dual_str!("Precondition Required"),
        429 => dual_str!("Too Many Requests"),
        431 => dual_str!("Request Header Fields Too Large"),
        500 => dual_str!("Internal Server Error"),
        501 => dual_str!("Not Implemented"),
        502 => dual_str!("Bad Gateway"),
        503 => dual_str!("Service Unavailable"),
        504 => dual_str!("Gateway Timeout"),
        505 => dual_str!("HTTP Version Not Supported"),
        506 => dual_str!("Variant Also Negotiates"),
        507 => dual_str!("Insufficient Storage"),
        508 => dual_str!("Loop Detected"),
        510 => dual_str!("Not Extended"),
        511 => dual_str!("Network Authentication Required"),
        _ => dual_str!("Undefined Reason"),
    }
}

pub fn get_reason(code: u16) -> &'static str {
    get_reason_inner(code).0
}

mod ffi {
    use super::*;
    use std::ffi::c_char;

    /// Returned pointer has a static lifetime.
    #[no_mangle]
    pub extern "C" fn statusreasons_get_reason(code: u16) -> *const c_char {
        get_reason_inner(code).1.as_ptr()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_get_reason() {
        // Known
        assert_eq!(get_reason(200), "OK");
        assert_eq!(get_reason(404), "Not Found");

        // Unknown
        assert_eq!(get_reason(0), "Undefined Reason");

        // FFI
        let r = ffi::statusreasons_get_reason(200);
        assert!(!r.is_null());
        // SAFETY: r is non-null and guaranteed to be nul-terminated
        let r = unsafe { CStr::from_ptr(r) };
        assert_eq!(r.to_str().unwrap(), get_reason(200));
    }
}
