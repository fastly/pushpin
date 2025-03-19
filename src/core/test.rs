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

#[derive(Default)]
pub struct TestException {
    pub file: String,
    pub line: u32,
    pub message: String,
}

pub mod ffi {
    use super::*;
    use std::ffi::CStr;
    use std::os::raw::{c_char, c_int};

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn test_exception_set(
        f: *mut TestException,
        file: *const c_char,
        line: libc::c_uint,
        message: *const c_char,
    ) -> c_int {
        let f = f.as_mut().unwrap();

        let file = unsafe { CStr::from_ptr(file) };

        let file = match file.to_str() {
            Ok(s) => s,
            Err(_) => return -1,
        };

        let message = unsafe { CStr::from_ptr(message) };

        let message = match message.to_str() {
            Ok(s) => s,
            Err(_) => return -1,
        };

        f.file = file.to_string();
        f.line = line;
        f.message = message.to_string();

        0
    }
}
