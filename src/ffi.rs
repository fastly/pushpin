/*
 * Copyright (C) 2021-2022 Fanout, Inc.
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
pub extern "C" fn timer_wheel_create(capacity: libc::c_uint) -> *mut TimerWheel {
    let wheel = TimerWheel::new(capacity as usize);

    Box::into_raw(Box::new(wheel))
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn timer_wheel_destroy(wheel: *mut TimerWheel) {
    if !wheel.is_null() {
        drop(Box::from_raw(wheel));
    }
}

#[allow(clippy::missing_safety_doc)]
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

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn timer_remove(wheel: *mut TimerWheel, key: libc::c_int) {
    TimerWheel::remove(&mut *wheel, key as usize);
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn timer_wheel_timeout(wheel: *mut TimerWheel) -> i64 {
    match TimerWheel::timeout(&*wheel) {
        Some(timeout) => timeout as i64,
        None => -1,
    }
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn timer_wheel_update(wheel: *mut TimerWheel, curtime: u64) {
    TimerWheel::update(&mut *wheel, curtime);
}

#[allow(clippy::missing_safety_doc)]
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

const JWT_KEYTYPE_SECRET: libc::c_int = 0;
const JWT_KEYTYPE_EC: libc::c_int = 1;
const JWT_KEYTYPE_RSA: libc::c_int = 2;
const JWT_ALGORITHM_HS256: libc::c_int = 0;
const JWT_ALGORITHM_ES256: libc::c_int = 1;
const JWT_ALGORITHM_RS256: libc::c_int = 2;

#[repr(C)]
pub struct JwtEncodingKey {
    r#type: libc::c_int,
    key: *mut jsonwebtoken::EncodingKey,
}

#[repr(C)]
pub struct JwtDecodingKey {
    r#type: libc::c_int,
    key: *mut jsonwebtoken::DecodingKey,
}

type EncodingKeyFromPemFn =
    fn(&[u8]) -> Result<jsonwebtoken::EncodingKey, jsonwebtoken::errors::Error>;
type DecodingKeyFromPemFn =
    fn(&[u8]) -> Result<jsonwebtoken::DecodingKey, jsonwebtoken::errors::Error>;

fn load_encoding_key_pem(
    key: &[u8],
) -> Result<(libc::c_int, jsonwebtoken::EncodingKey), jsonwebtoken::errors::Error> {
    // pem data includes the key type, however the jsonwebtoken crate
    // requires specifying the expected type when decoding. we'll just try
    // the data against multiple possible types
    let decoders: [(libc::c_int, EncodingKeyFromPemFn); 2] = [
        (JWT_KEYTYPE_EC, jsonwebtoken::EncodingKey::from_ec_pem),
        (JWT_KEYTYPE_RSA, jsonwebtoken::EncodingKey::from_rsa_pem),
    ];

    let mut last_err = None;

    for (ktype, f) in decoders {
        match f(key) {
            Ok(key) => return Ok((ktype, key)),
            Err(e) => last_err = Some(e),
        }
    }

    Err(last_err.unwrap())
}

fn load_decoding_key_pem(
    key: &[u8],
) -> Result<(libc::c_int, jsonwebtoken::DecodingKey), jsonwebtoken::errors::Error> {
    // pem data includes the key type, however the jsonwebtoken crate
    // requires specifying the expected type when decoding. we'll just try
    // the data against multiple possible types
    let decoders: [(libc::c_int, DecodingKeyFromPemFn); 2] = [
        (JWT_KEYTYPE_EC, jsonwebtoken::DecodingKey::from_ec_pem),
        (JWT_KEYTYPE_RSA, jsonwebtoken::DecodingKey::from_rsa_pem),
    ];

    let mut last_err = None;

    for (ktype, f) in decoders {
        match f(key) {
            Ok(key) => return Ok((ktype, key)),
            Err(e) => last_err = Some(e),
        }
    }

    Err(last_err.unwrap())
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn jwt_encoding_key_from_secret(
    data: *const u8,
    len: libc::size_t,
) -> JwtEncodingKey {
    let key = jsonwebtoken::EncodingKey::from_secret(slice::from_raw_parts(data, len));

    JwtEncodingKey {
        r#type: JWT_KEYTYPE_SECRET,
        key: Box::into_raw(Box::new(key)),
    }
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn jwt_encoding_key_from_pem(
    data: *const u8,
    len: libc::size_t,
) -> JwtEncodingKey {
    match load_encoding_key_pem(slice::from_raw_parts(data, len)) {
        Ok((ktype, key)) => JwtEncodingKey {
            r#type: ktype,
            key: Box::into_raw(Box::new(key)),
        },
        Err(_) => JwtEncodingKey {
            r#type: -1,
            key: ptr::null_mut(),
        },
    }
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn jwt_encoding_key_destroy(key: *mut jsonwebtoken::EncodingKey) {
    if !key.is_null() {
        drop(Box::from_raw(key));
    }
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn jwt_decoding_key_from_secret(
    data: *const u8,
    len: libc::size_t,
) -> JwtDecodingKey {
    let key = jsonwebtoken::DecodingKey::from_secret(slice::from_raw_parts(data, len));

    JwtDecodingKey {
        r#type: JWT_KEYTYPE_SECRET,
        key: Box::into_raw(Box::new(key)),
    }
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn jwt_decoding_key_from_pem(
    data: *const u8,
    len: libc::size_t,
) -> JwtDecodingKey {
    match load_decoding_key_pem(slice::from_raw_parts(data, len)) {
        Ok((ktype, key)) => JwtDecodingKey {
            r#type: ktype,
            key: Box::into_raw(Box::new(key)),
        },
        Err(_) => JwtDecodingKey {
            r#type: -1,
            key: ptr::null_mut(),
        },
    }
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn jwt_decoding_key_destroy(key: *mut jsonwebtoken::DecodingKey) {
    if !key.is_null() {
        drop(Box::from_raw(key));
    }
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn jwt_str_destroy(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

#[allow(clippy::missing_safety_doc)]
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
        JWT_ALGORITHM_HS256 => jsonwebtoken::Header::new(jsonwebtoken::Algorithm::HS256),
        JWT_ALGORITHM_ES256 => jsonwebtoken::Header::new(jsonwebtoken::Algorithm::ES256),
        JWT_ALGORITHM_RS256 => jsonwebtoken::Header::new(jsonwebtoken::Algorithm::RS256),
        _ => return 1, // unsupported algorithm
    };

    let claim = match CStr::from_ptr(claim).to_str() {
        Ok(s) => s,
        Err(_) => return 1, // claim is a JSON string which will be valid UTF-8
    };

    let token = match jwt::encode(&header, claim, key) {
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

#[allow(clippy::missing_safety_doc)]
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
        JWT_ALGORITHM_HS256 => jsonwebtoken::Validation::new(jsonwebtoken::Algorithm::HS256),
        JWT_ALGORITHM_ES256 => jsonwebtoken::Validation::new(jsonwebtoken::Algorithm::ES256),
        JWT_ALGORITHM_RS256 => jsonwebtoken::Validation::new(jsonwebtoken::Algorithm::RS256),
        _ => return 1, // unsupported algorithm
    };

    // don't check exp or anything. that's left to the caller
    validation.required_spec_claims = HashSet::new();

    let token = match CStr::from_ptr(token).to_str() {
        Ok(s) => s,
        Err(_) => return 1, // token string will be valid UTF-8
    };

    let claim = match jwt::decode(token, key, &validation) {
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
