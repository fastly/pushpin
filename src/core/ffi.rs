/*
 * Copyright (C) 2021-2022 Fanout, Inc.
 * Copyright (C) 2023-2024 Fastly, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:APACHE2$
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
 *
 * $FANOUT_END_LICENSE$
 */

use crate::core::encrypt;
use crate::core::log::{ensure_init_simple_logger, get_simple_logger};
use std::ptr;
use std::slice;

#[repr(C)]
pub struct EncryptBuffer {
    data: *const u8,
    len: libc::size_t,
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn encrypt_decrypt_message(
    data: *const u8,
    len: libc::size_t,
    key: *const u8,
    out_plain: *mut EncryptBuffer,
) -> libc::c_int {
    if data.is_null() || key.is_null() {
        return 1; // null pointers
    }

    let data = slice::from_raw_parts(data, len);

    let key = {
        let key = slice::from_raw_parts(key, 16);

        let mut buf = [0; 16];
        buf.copy_from_slice(&key[..16]);

        buf
    };

    let out_plain = match out_plain.as_mut() {
        Some(r) => r,
        None => return 1, // null pointer
    };

    let plain = match encrypt::decrypt_message(data, &key) {
        Ok(v) => v,
        Err(encrypt::DecryptError::UnsupportedAlgorithm) => return 2,
        Err(encrypt::DecryptError::BadFormat) => return 3,
        Err(encrypt::DecryptError::InvalidData) => return 4,
    };

    let plain = plain.into_boxed_slice();

    out_plain.len = plain.len();
    out_plain.data = Box::into_raw(plain) as *const u8;

    return 0;
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn encrypt_buffer_deinit(buf: *mut EncryptBuffer) {
    let buf = match buf.as_mut() {
        Some(r) => r,
        None => return, // null pointer
    };

    if !buf.data.is_null() {
        let data = slice::from_raw_parts_mut(buf.data as *mut u8, buf.len);

        drop(Box::from_raw(data));

        buf.data = ptr::null();
        buf.len = 0;
    }
}

#[no_mangle]
pub extern "C" fn log_init() {
    ensure_init_simple_logger(None, false);

    log::set_logger(get_simple_logger()).unwrap();
}

#[no_mangle]
pub extern "C" fn log_set_level(level: libc::c_int) {
    let level = match level {
        core::i32::MIN..=0 => log::LevelFilter::Error,
        1 => log::LevelFilter::Warn,
        2 => log::LevelFilter::Info,
        3 => log::LevelFilter::Debug,
        4..=core::i32::MAX => log::LevelFilter::Trace,
    };

    log::set_max_level(level);
}

#[no_mangle]
pub extern "C" fn security_limit_permissions() {
    // for now all we do is set up seccomp if running on linux

    #[cfg(all(target_os = "linux", not(test)))]
    crate::core::seccomp::install_seccomp_connect_filter()
}

#[no_mangle]
pub extern "C" fn backtrace_setup_signal_handlers() {
    crate::core::backtrace::setup_signal_handlers()
}
