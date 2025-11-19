/*
 * Copyright (C) 2022 Fanout, Inc.
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

use jsonwebtoken::{DecodingKey, EncodingKey, Header, TokenData, Validation};

pub fn encode(
    header: &Header,
    claim: &str,
    key: &EncodingKey,
) -> Result<String, jsonwebtoken::errors::Error> {
    // Claim is already serialized, but the jsonwebtoken crate requires a
    // serializable object. So we'll deserialize to a generic value first
    let claim: serde_json::Value = serde_json::from_str(claim)?;

    jsonwebtoken::encode(header, &claim, key)
}

pub fn decode(
    token: &str,
    key: &DecodingKey,
    validation: &Validation,
) -> Result<String, jsonwebtoken::errors::Error> {
    let token_data: TokenData<serde_json::Value> = jsonwebtoken::decode(token, key, validation)?;

    Ok(serde_json::to_string(&token_data.claims)?)
}

mod ffi {
    use super::*;
    use std::collections::HashSet;
    use std::ffi::{CStr, CString};
    use std::os::raw::c_char;
    use std::ptr;
    use std::slice;

    pub const JWT_KEYTYPE_SECRET: libc::c_int = 0;
    pub const JWT_KEYTYPE_EC: libc::c_int = 1;
    pub const JWT_KEYTYPE_RSA: libc::c_int = 2;
    pub const JWT_ALGORITHM_HS256: libc::c_int = 0;
    pub const JWT_ALGORITHM_ES256: libc::c_int = 1;
    pub const JWT_ALGORITHM_RS256: libc::c_int = 2;

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
        // Pem data includes the key type, however the jsonwebtoken crate
        // requires specifying the expected type when decoding. We'll just try
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
        // Pem data includes the key type, however the jsonwebtoken crate
        // requires specifying the expected type when decoding. We'll just try
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
        out_token: *mut *mut c_char,
    ) -> libc::c_int {
        if claim.is_null() || out_token.is_null() {
            return 1; // Null pointers
        }

        let key = match key.as_ref() {
            Some(r) => r,
            None => return 1, // Null pointer
        };

        let header = match alg {
            JWT_ALGORITHM_HS256 => jsonwebtoken::Header::new(jsonwebtoken::Algorithm::HS256),
            JWT_ALGORITHM_ES256 => jsonwebtoken::Header::new(jsonwebtoken::Algorithm::ES256),
            JWT_ALGORITHM_RS256 => jsonwebtoken::Header::new(jsonwebtoken::Algorithm::RS256),
            _ => return 1, // Unsupported algorithm
        };

        let claim = match CStr::from_ptr(claim).to_str() {
            Ok(s) => s,
            Err(_) => return 1, // Claim is a JSON string which will be valid UTF-8
        };

        let token = match encode(&header, claim, key) {
            Ok(token) => token,
            Err(_) => return 1, // Failed to sign
        };

        let token = match CString::new(token) {
            Ok(s) => s,
            Err(_) => return 1, // Unexpected token string format
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
        out_claim: *mut *mut c_char,
    ) -> libc::c_int {
        if token.is_null() || out_claim.is_null() {
            return 1; // Null pointers
        }

        let key = match key.as_ref() {
            Some(r) => r,
            None => return 1, // Null pointer
        };

        let mut validation = match alg {
            JWT_ALGORITHM_HS256 => jsonwebtoken::Validation::new(jsonwebtoken::Algorithm::HS256),
            JWT_ALGORITHM_ES256 => jsonwebtoken::Validation::new(jsonwebtoken::Algorithm::ES256),
            JWT_ALGORITHM_RS256 => jsonwebtoken::Validation::new(jsonwebtoken::Algorithm::RS256),
            _ => return 1, // Unsupported algorithm
        };

        // Don't check exp or anything. That's left to the caller
        validation.required_spec_claims = HashSet::new();

        let token = match CStr::from_ptr(token).to_str() {
            Ok(s) => s,
            Err(_) => return 1, // Token string will be valid UTF-8
        };

        let claim = match decode(token, key, &validation) {
            Ok(claim) => claim,
            Err(_) => return 1, // Failed to validate
        };

        let claim = match CString::new(claim) {
            Ok(s) => s,
            Err(_) => return 1, // Unexpected claim string format
        };

        *out_claim = claim.into_raw();

        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use jsonwebtoken::Algorithm;
    use serde::Deserialize;
    use serde::Serialize;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn encode_decode() {
        #[derive(Debug, Serialize, Deserialize)]
        struct Claim {
            iss: String,
            exp: u64,
        }

        let claim = Claim {
            iss: "nobody".to_string(),
            exp: SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_secs(),
        };

        let claim = serde_json::to_string(&claim).unwrap();

        let token = encode(
            &Header::new(Algorithm::HS256),
            &claim,
            &EncodingKey::from_secret(b"secret"),
        )
        .unwrap();

        let claim = decode(
            &token,
            &DecodingKey::from_secret(b"secret"),
            &Validation::new(Algorithm::HS256),
        )
        .unwrap();

        let claim: Claim = serde_json::from_str(&claim).unwrap();
        assert_eq!(claim.iss, "nobody");
    }
}
