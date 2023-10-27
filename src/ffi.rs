/*
 * Copyright (C) 2021-2022 Fanout, Inc.
 * Copyright (C) 2023 Fastly, Inc.
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

// NOTE: must match values in wzmq.h
const WZMQ_PAIR: libc::c_int = 0;
const WZMQ_PUB: libc::c_int = 1;
const WZMQ_SUB: libc::c_int = 2;
const WZMQ_REQ: libc::c_int = 3;
const WZMQ_REP: libc::c_int = 4;
const WZMQ_DEALER: libc::c_int = 5;
const WZMQ_ROUTER: libc::c_int = 6;
const WZMQ_PULL: libc::c_int = 7;
const WZMQ_PUSH: libc::c_int = 8;
const WZMQ_XPUB: libc::c_int = 9;
const WZMQ_XSUB: libc::c_int = 10;
const WZMQ_STREAM: libc::c_int = 11;

// NOTE: must match values in wzmq.h
const WZMQ_FD: libc::c_int = 0;
const WZMQ_SUBSCRIBE: libc::c_int = 1;
const WZMQ_UNSUBSCRIBE: libc::c_int = 2;
const WZMQ_LINGER: libc::c_int = 3;
const WZMQ_IDENTITY: libc::c_int = 4;
const WZMQ_IMMEDIATE: libc::c_int = 5;
const WZMQ_RCVMORE: libc::c_int = 6;
const WZMQ_EVENTS: libc::c_int = 7;
const WZMQ_SNDHWM: libc::c_int = 8;
const WZMQ_RCVHWM: libc::c_int = 9;
const WZMQ_TCP_KEEPALIVE: libc::c_int = 10;
const WZMQ_TCP_KEEPALIVE_IDLE: libc::c_int = 11;
const WZMQ_TCP_KEEPALIVE_CNT: libc::c_int = 12;
const WZMQ_TCP_KEEPALIVE_INTVL: libc::c_int = 13;

// NOTE: must match values in wzmq.h
const WZMQ_DONTWAIT: libc::c_int = 0x01;
const WZMQ_SNDMORE: libc::c_int = 0x02;

// NOTE: must match values in wzmq.h
const WZMQ_POLLIN: libc::c_int = 0x01;
const WZMQ_POLLOUT: libc::c_int = 0x02;

#[repr(C)]
pub struct WZmqMessage {
    data: *mut zmq::Message,
}

fn convert_io_flags(flags: libc::c_int) -> i32 {
    let mut out = 0;

    if flags & WZMQ_DONTWAIT != 0 {
        out |= zmq::DONTWAIT;
    }

    if flags & WZMQ_SNDMORE != 0 {
        out |= zmq::SNDMORE;
    }

    out
}

fn convert_events(events: zmq::PollEvents) -> libc::c_int {
    let mut out = 0;

    if events.contains(zmq::POLLIN) {
        out |= WZMQ_POLLIN;
    }

    if events.contains(zmq::POLLOUT) {
        out |= WZMQ_POLLOUT;
    }

    out
}

#[cfg(target_os = "macos")]
fn set_errno(value: libc::c_int) {
    unsafe {
        *libc::__error() = value;
    }
}

#[cfg(not(target_os = "macos"))]
fn set_errno(value: libc::c_int) {
    unsafe {
        *libc::__errno_location() = value;
    }
}

#[no_mangle]
pub extern "C" fn wzmq_init(io_threads: libc::c_int) -> *mut zmq::Context {
    let ctx = zmq::Context::new();

    if ctx.set_io_threads(io_threads).is_err() {
        return ptr::null_mut();
    }

    Box::into_raw(Box::new(ctx))
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_term(context: *mut zmq::Context) -> libc::c_int {
    if !context.is_null() {
        drop(Box::from_raw(context));
    }

    0
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_socket(
    context: *mut zmq::Context,
    stype: libc::c_int,
) -> *mut zmq::Socket {
    let ctx = match context.as_ref() {
        Some(ctx) => ctx,
        None => return ptr::null_mut(),
    };

    let stype = match stype {
        WZMQ_PAIR => zmq::PAIR,
        WZMQ_PUB => zmq::PUB,
        WZMQ_SUB => zmq::SUB,
        WZMQ_REQ => zmq::REQ,
        WZMQ_REP => zmq::REP,
        WZMQ_DEALER => zmq::DEALER,
        WZMQ_ROUTER => zmq::ROUTER,
        WZMQ_PULL => zmq::PULL,
        WZMQ_PUSH => zmq::PUSH,
        WZMQ_XPUB => zmq::XPUB,
        WZMQ_XSUB => zmq::XSUB,
        WZMQ_STREAM => zmq::STREAM,
        _ => return ptr::null_mut(),
    };

    let sock = match ctx.socket(stype) {
        Ok(sock) => sock,
        Err(_) => return ptr::null_mut(),
    };

    Box::into_raw(Box::new(sock))
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_close(socket: *mut zmq::Socket) -> libc::c_int {
    if socket.is_null() {
        return -1;
    }

    drop(Box::from_raw(socket));

    0
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_getsockopt(
    socket: *mut zmq::Socket,
    option_name: libc::c_int,
    option_value: *mut libc::c_void,
    option_len: *mut libc::size_t,
) -> libc::c_int {
    let sock = match socket.as_ref() {
        Some(sock) => sock,
        None => return -1,
    };

    if option_value.is_null() {
        return -1;
    }

    let option_len = match option_len.as_mut() {
        Some(x) => x,
        None => return -1,
    };

    match option_name {
        WZMQ_FD => {
            if *option_len as u32 != libc::c_int::BITS / 8 {
                return -1;
            }

            let x = match (option_value as *mut libc::c_int).as_mut() {
                Some(x) => x,
                None => return -1,
            };

            let v = match sock.get_fd() {
                Ok(v) => v,
                Err(e) => {
                    println!("get_fd failed: {:?}", e);
                    set_errno(e.to_raw());
                    return -1;
                }
            };

            *x = v;
        }
        WZMQ_IDENTITY => {
            let identity = match sock.get_identity() {
                Ok(v) => v,
                Err(_) => return -1,
            };

            let s = slice::from_raw_parts_mut(option_value as *mut u8, *option_len);

            if s.len() < identity.len() {
                return -1;
            }

            s[..identity.len()].copy_from_slice(&identity);
        }
        WZMQ_RCVMORE => {
            if *option_len as u32 != libc::c_int::BITS / 8 {
                return -1;
            }

            let x = match (option_value as *mut libc::c_int).as_mut() {
                Some(x) => x,
                None => return -1,
            };

            let v = match sock.get_rcvmore() {
                Ok(v) => v,
                Err(e) => {
                    set_errno(e.to_raw());
                    return -1;
                }
            };

            if v {
                *x = 1;
            } else {
                *x = 0;
            }
        }
        WZMQ_EVENTS => {
            if *option_len as u32 != libc::c_int::BITS / 8 {
                return -1;
            }

            let x = match (option_value as *mut libc::c_int).as_mut() {
                Some(x) => x,
                None => return -1,
            };

            let v = match sock.get_events() {
                Ok(v) => v,
                Err(e) => {
                    set_errno(e.to_raw());
                    return -1;
                }
            };

            *x = convert_events(v);
        }
        WZMQ_SNDHWM => {
            if *option_len as u32 != libc::c_int::BITS / 8 {
                return -1;
            }

            let x = match (option_value as *mut libc::c_int).as_mut() {
                Some(x) => x,
                None => return -1,
            };

            let v = match sock.get_sndhwm() {
                Ok(v) => v,
                Err(e) => {
                    set_errno(e.to_raw());
                    return -1;
                }
            };

            *x = v;
        }
        WZMQ_RCVHWM => {
            if *option_len as u32 != libc::c_int::BITS / 8 {
                return -1;
            }

            let x = match (option_value as *mut libc::c_int).as_mut() {
                Some(x) => x,
                None => return -1,
            };

            let v = match sock.get_rcvhwm() {
                Ok(v) => v,
                Err(e) => {
                    set_errno(e.to_raw());
                    return -1;
                }
            };

            *x = v;
        }
        _ => return -1,
    }

    0
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_setsockopt(
    socket: *mut zmq::Socket,
    option_name: libc::c_int,
    option_value: *mut libc::c_void,
    option_len: libc::size_t,
) -> libc::c_int {
    let sock = match socket.as_ref() {
        Some(sock) => sock,
        None => return -1,
    };

    if option_value.is_null() {
        return -1;
    }

    match option_name {
        WZMQ_SUBSCRIBE => {
            let s = slice::from_raw_parts(option_value as *mut u8, option_len);

            if let Err(e) = sock.set_subscribe(s) {
                set_errno(e.to_raw());
                return -1;
            }
        }
        WZMQ_UNSUBSCRIBE => {
            let s = slice::from_raw_parts(option_value as *mut u8, option_len);

            if let Err(e) = sock.set_unsubscribe(s) {
                set_errno(e.to_raw());
                return -1;
            }
        }
        WZMQ_LINGER => {
            if option_len as u32 != libc::c_int::BITS / 8 {
                return -1;
            }

            let x = match (option_value as *mut libc::c_int).as_ref() {
                Some(x) => x,
                None => return -1,
            };

            if let Err(e) = sock.set_linger(*x) {
                set_errno(e.to_raw());
                return -1;
            }
        }
        WZMQ_IDENTITY => {
            let s = slice::from_raw_parts(option_value as *mut u8, option_len);

            if let Err(e) = sock.set_identity(s) {
                set_errno(e.to_raw());
                return -1;
            }
        }
        WZMQ_IMMEDIATE => {
            if option_len as u32 != libc::c_int::BITS / 8 {
                return -1;
            }

            let x = match (option_value as *mut libc::c_int).as_ref() {
                Some(x) => x,
                None => return -1,
            };

            if let Err(e) = sock.set_immediate(*x != 0) {
                set_errno(e.to_raw());
                return -1;
            }
        }
        WZMQ_SNDHWM => {
            if option_len as u32 != libc::c_int::BITS / 8 {
                return -1;
            }

            let x = match (option_value as *mut libc::c_int).as_ref() {
                Some(x) => x,
                None => return -1,
            };

            if let Err(e) = sock.set_sndhwm(*x) {
                set_errno(e.to_raw());
                return -1;
            }
        }
        WZMQ_RCVHWM => {
            if option_len as u32 != libc::c_int::BITS / 8 {
                return -1;
            }

            let x = match (option_value as *mut libc::c_int).as_ref() {
                Some(x) => x,
                None => return -1,
            };

            if let Err(e) = sock.set_rcvhwm(*x) {
                set_errno(e.to_raw());
                return -1;
            }
        }
        WZMQ_TCP_KEEPALIVE => {
            if option_len as u32 != libc::c_int::BITS / 8 {
                return -1;
            }

            let x = match (option_value as *mut libc::c_int).as_ref() {
                Some(x) => x,
                None => return -1,
            };

            if let Err(e) = sock.set_tcp_keepalive(*x) {
                set_errno(e.to_raw());
                return -1;
            }
        }
        WZMQ_TCP_KEEPALIVE_IDLE => {
            if option_len as u32 != libc::c_int::BITS / 8 {
                return -1;
            }

            let x = match (option_value as *mut libc::c_int).as_ref() {
                Some(x) => x,
                None => return -1,
            };

            if let Err(e) = sock.set_tcp_keepalive_idle(*x) {
                set_errno(e.to_raw());
                return -1;
            }
        }
        WZMQ_TCP_KEEPALIVE_CNT => {
            if option_len as u32 != libc::c_int::BITS / 8 {
                return -1;
            }

            let x = match (option_value as *mut libc::c_int).as_ref() {
                Some(x) => x,
                None => return -1,
            };

            if let Err(e) = sock.set_tcp_keepalive_cnt(*x) {
                set_errno(e.to_raw());
                return -1;
            }
        }
        WZMQ_TCP_KEEPALIVE_INTVL => {
            if option_len as u32 != libc::c_int::BITS / 8 {
                return -1;
            }

            let x = match (option_value as *mut libc::c_int).as_ref() {
                Some(x) => x,
                None => return -1,
            };

            if let Err(e) = sock.set_tcp_keepalive_intvl(*x) {
                set_errno(e.to_raw());
                return -1;
            }
        }
        _ => return -1,
    }

    0
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_connect(
    socket: *mut zmq::Socket,
    endpoint: *const libc::c_char,
) -> libc::c_int {
    let sock = match socket.as_ref() {
        Some(sock) => sock,
        None => return -1,
    };

    if endpoint.is_null() {
        return -1;
    }

    let endpoint = match CStr::from_ptr(endpoint).to_str() {
        Ok(s) => s,
        Err(_) => return -1,
    };

    if let Err(e) = sock.connect(endpoint) {
        set_errno(e.to_raw());
        return -1;
    }

    0
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_bind(
    socket: *mut zmq::Socket,
    endpoint: *const libc::c_char,
) -> libc::c_int {
    let sock = match socket.as_ref() {
        Some(sock) => sock,
        None => return -1,
    };

    if endpoint.is_null() {
        return -1;
    }

    let endpoint = match CStr::from_ptr(endpoint).to_str() {
        Ok(s) => s,
        Err(_) => return -1,
    };

    if let Err(e) = sock.bind(endpoint) {
        set_errno(e.to_raw());
        return -1;
    }

    0
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_send(
    socket: *mut zmq::Socket,
    buf: *const u8,
    len: libc::size_t,
    flags: libc::c_int,
) -> libc::c_int {
    let sock = match socket.as_ref() {
        Some(sock) => sock,
        None => return -1,
    };

    if buf.is_null() {
        return -1;
    }

    let buf = slice::from_raw_parts(buf, len);

    if let Err(e) = sock.send(buf, convert_io_flags(flags)) {
        set_errno(e.to_raw());
        return -1;
    }

    buf.len() as libc::c_int
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_recv(
    socket: *mut zmq::Socket,
    buf: *mut u8,
    len: libc::size_t,
    flags: libc::c_int,
) -> libc::c_int {
    let sock = match socket.as_ref() {
        Some(sock) => sock,
        None => return -1,
    };

    if buf.is_null() {
        return -1;
    }

    let buf = slice::from_raw_parts_mut(buf, len);

    let size = match sock.recv_into(buf, convert_io_flags(flags)) {
        Ok(size) => size,
        Err(e) => {
            set_errno(e.to_raw());
            return -1;
        }
    };

    size as libc::c_int
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_msg_init(msg: *mut WZmqMessage) -> libc::c_int {
    let msg = msg.as_mut().unwrap();
    msg.data = Box::into_raw(Box::new(zmq::Message::new()));

    0
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_msg_init_size(
    msg: *mut WZmqMessage,
    size: libc::size_t,
) -> libc::c_int {
    let msg = msg.as_mut().unwrap();
    msg.data = Box::into_raw(Box::new(zmq::Message::with_size(size)));

    0
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_msg_data(msg: *mut WZmqMessage) -> *mut libc::c_void {
    let msg = msg.as_mut().unwrap();
    let data = msg.data.as_mut().unwrap();

    data.as_mut_ptr() as *mut libc::c_void
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_msg_size(msg: *const WZmqMessage) -> libc::size_t {
    let msg = msg.as_ref().unwrap();
    let data = msg.data.as_ref().unwrap();

    data.len()
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_msg_close(msg: *mut WZmqMessage) -> libc::c_int {
    let msg = match msg.as_mut() {
        Some(msg) => msg,
        None => return -1,
    };

    if !msg.data.is_null() {
        drop(Box::from_raw(msg.data));
        msg.data = ptr::null_mut();
    }

    0
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_msg_send(
    msg: *mut WZmqMessage,
    socket: *mut zmq::Socket,
    flags: libc::c_int,
) -> libc::c_int {
    let msg = match msg.as_mut() {
        Some(msg) => msg,
        None => return -1,
    };

    if msg.data.is_null() {
        return -1;
    }

    let sock = match socket.as_ref() {
        Some(sock) => sock,
        None => return -1,
    };

    let data = Box::from_raw(msg.data);
    msg.data = ptr::null_mut();

    let size = data.len();

    if let Err(e) = sock.send(*data, convert_io_flags(flags)) {
        set_errno(e.to_raw());
        return -1;
    }

    size as libc::c_int
}

#[allow(clippy::missing_safety_doc)]
#[no_mangle]
pub unsafe extern "C" fn wzmq_msg_recv(
    msg: *mut WZmqMessage,
    socket: *mut zmq::Socket,
    flags: libc::c_int,
) -> libc::c_int {
    let msg = match msg.as_mut() {
        Some(msg) => msg,
        None => return -1,
    };

    let sock = match socket.as_ref() {
        Some(sock) => sock,
        None => return -1,
    };

    if !msg.data.is_null() {
        drop(Box::from_raw(msg.data));
        msg.data = ptr::null_mut();
    }

    let data = match sock.recv_msg(convert_io_flags(flags)) {
        Ok(msg) => msg,
        Err(e) => {
            set_errno(e.to_raw());
            return -1;
        }
    };

    let size = data.len();

    msg.data = Box::into_raw(Box::new(data));

    size as libc::c_int
}
