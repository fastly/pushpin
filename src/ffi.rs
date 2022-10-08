/*
 * Copyright (C) 2021-2022 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
 *
 * Pushpin is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

use crate::jwt;
use crate::timer::TimerWheel;
use libc;
use std::collections::HashSet;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;
use std::slice;

#[repr(C)]
pub struct ExpiredTimer {
    key: libc::c_int,
    user_data: libc::size_t,
}

#[no_mangle]
pub unsafe extern "C" fn timer_wheel_create(capacity: libc::c_uint) -> *mut TimerWheel {
    let wheel = TimerWheel::new(capacity as usize);

    Box::into_raw(Box::new(wheel))
}

#[no_mangle]
pub unsafe extern "C" fn timer_wheel_destroy(wheel: *mut TimerWheel) {
    if !wheel.is_null() {
        drop(Box::from_raw(wheel));
    }
}

#[no_mangle]
pub unsafe extern "C" fn timer_add(
    wheel: *mut TimerWheel,
    expires: u64,
    user_data: libc::size_t,
) -> libc::c_int {
    match TimerWheel::add(&mut *wheel, expires, user_data) {
        Ok(key) => key as libc::c_int,
        Err(_) => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn timer_remove(wheel: *mut TimerWheel, key: libc::c_int) {
    TimerWheel::remove(&mut *wheel, key as usize);
}

#[no_mangle]
pub unsafe extern "C" fn timer_wheel_timeout(wheel: *mut TimerWheel) -> i64 {
    match TimerWheel::timeout(&*wheel) {
        Some(timeout) => timeout as i64,
        None => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn timer_wheel_update(wheel: *mut TimerWheel, curtime: u64) {
    TimerWheel::update(&mut *wheel, curtime);
}

#[no_mangle]
pub unsafe extern "C" fn timer_wheel_take_expired(wheel: *mut TimerWheel) -> ExpiredTimer {
    match TimerWheel::take_expired(&mut *wheel) {
        Some((key, user_data)) => ExpiredTimer {
            key: key as libc::c_int,
            user_data: user_data as libc::size_t,
        },
        None => ExpiredTimer {
            key: -1,
            user_data: 0,
        },
    }
}

#[no_mangle]
pub unsafe extern "C" fn jwt_encoding_key_from_secret(
    data: *const u8,
    len: libc::size_t,
) -> *mut jsonwebtoken::EncodingKey {
    let key = jsonwebtoken::EncodingKey::from_secret(slice::from_raw_parts(data, len));

    Box::into_raw(Box::new(key))
}

#[no_mangle]
pub unsafe extern "C" fn jwt_encoding_key_from_ec_pem(
    data: *const u8,
    len: libc::size_t,
) -> *mut jsonwebtoken::EncodingKey {
    match jsonwebtoken::EncodingKey::from_ec_pem(slice::from_raw_parts(data, len)) {
        Ok(key) => Box::into_raw(Box::new(key)),
        Err(_) => ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn jwt_encoding_key_destroy(key: *mut jsonwebtoken::EncodingKey) {
    if !key.is_null() {
        drop(Box::from_raw(key));
    }
}

#[no_mangle]
pub unsafe extern "C" fn jwt_decoding_key_from_secret(
    data: *const u8,
    len: libc::size_t,
) -> *mut jsonwebtoken::DecodingKey {
    let key = jsonwebtoken::DecodingKey::from_secret(slice::from_raw_parts(data, len));

    Box::into_raw(Box::new(key))
}

#[no_mangle]
pub unsafe extern "C" fn jwt_decoding_key_from_ec_pem(
    data: *const u8,
    len: libc::size_t,
) -> *mut jsonwebtoken::DecodingKey {
    match jsonwebtoken::DecodingKey::from_ec_pem(slice::from_raw_parts(data, len)) {
        Ok(key) => Box::into_raw(Box::new(key)),
        Err(_) => ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn jwt_decoding_key_destroy(key: *mut jsonwebtoken::DecodingKey) {
    if !key.is_null() {
        drop(Box::from_raw(key));
    }
}

#[no_mangle]
pub unsafe extern "C" fn jwt_str_destroy(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

#[no_mangle]
pub unsafe extern "C" fn jwt_encode(
    alg: libc::c_int,
    claim: *const c_char,
    key: *const jsonwebtoken::EncodingKey,
    out_token: *mut *const c_char,
) -> libc::c_int {
    if claim.is_null() || out_token.is_null() {
        return 1; // null pointers
    }

    let key = match key.as_ref() {
        Some(r) => r,
        None => return 1, // null pointer
    };

    let header = match alg {
        0 => jsonwebtoken::Header::new(jsonwebtoken::Algorithm::HS256),
        1 => jsonwebtoken::Header::new(jsonwebtoken::Algorithm::ES256),
        2 => jsonwebtoken::Header::new(jsonwebtoken::Algorithm::RS256),
        _ => return 1, // unsupported algorithm
    };

    let claim = match CStr::from_ptr(claim).to_str() {
        Ok(s) => s,
        Err(_) => return 1, // claim is a JSON string which will be valid UTF-8
    };

    let token = match jwt::encode(&header, &claim, key) {
        Ok(token) => token,
        Err(_) => return 1, // failed to sign
    };

    let token = match CString::new(token) {
        Ok(s) => s,
        Err(_) => return 1, // unexpected token string format
    };

    *out_token = token.into_raw();

    0
}

#[no_mangle]
pub unsafe extern "C" fn jwt_decode(
    alg: libc::c_int,
    token: *const c_char,
    key: *const jsonwebtoken::DecodingKey,
    out_claim: *mut *const c_char,
) -> libc::c_int {
    if token.is_null() || out_claim.is_null() {
        return 1; // null pointers
    }

    let key = match key.as_ref() {
        Some(r) => r,
        None => return 1, // null pointer
    };

    let mut validation = match alg {
        0 => jsonwebtoken::Validation::new(jsonwebtoken::Algorithm::HS256),
        1 => jsonwebtoken::Validation::new(jsonwebtoken::Algorithm::ES256),
        2 => jsonwebtoken::Validation::new(jsonwebtoken::Algorithm::RS256),
        _ => return 1, // unsupported algorithm
    };

    // don't check exp or anything. that's left to the caller
    validation.required_spec_claims = HashSet::new();

    let token = match CStr::from_ptr(token).to_str() {
        Ok(s) => s,
        Err(_) => return 1, // token string will be valid UTF-8
    };

    let claim = match jwt::decode(&token, key, &validation) {
        Ok(claim) => claim,
        Err(_) => return 1, // failed to validate
    };

    let claim = match CString::new(claim) {
        Ok(s) => s,
        Err(_) => return 1, // unexpected claim string format
    };

    *out_claim = claim.into_raw();

    0
}
