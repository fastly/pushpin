use aegis::aegis128l::Aegis128L;

const ALGORITHM_NAME: &[u8] = b"aegis128l";

#[derive(Debug)]
pub enum DecryptError {
    UnsupportedAlgorithm,
    BadFormat,
    InvalidData,
}

pub fn decrypt_message(data: &[u8], key: &[u8; 16]) -> Result<Vec<u8>, DecryptError> {
    let data = match data.strip_prefix(b"E:") {
        Some(v) => v,
        None => return Err(DecryptError::BadFormat),
    };

    // find end of key ID
    let pos = match data.iter().position(|&b| b == b':') {
        Some(x) => x,
        None => return Err(DecryptError::BadFormat),
    };

    // skip over key ID
    let data = &data[(pos + 1)..];

    // find end of algorithm name
    let pos = match data.iter().position(|&b| b == b':') {
        Some(x) => x,
        None => return Err(DecryptError::BadFormat),
    };

    let algo = &data[..pos];
    let data = &data[(pos + 1)..];

    if algo != ALGORITHM_NAME {
        return Err(DecryptError::UnsupportedAlgorithm);
    }

    // data must be prefixed with a 16 byte nonce and suffixed with a 32 byte tag
    if data.len() < 48 {
        return Err(DecryptError::BadFormat);
    }

    let mut nonce = [0; 16];
    nonce.copy_from_slice(&data[..16]);

    let encrypted = &data[16..(data.len() - 32)];

    let mut tag = [0; 32];
    tag.copy_from_slice(&data[(data.len() - 32)..]);

    let cipher = Aegis128L::new(key, &nonce);

    let plain = match cipher.decrypt(encrypted, &tag, &[]) {
        Ok(v) => v,
        Err(_) => return Err(DecryptError::InvalidData),
    };

    Ok(plain)
}

mod ffi {
    use super::*;
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

        let plain = match decrypt_message(data, &key) {
            Ok(v) => v,
            Err(DecryptError::UnsupportedAlgorithm) => return 2,
            Err(DecryptError::BadFormat) => return 3,
            Err(DecryptError::InvalidData) => return 4,
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
}

#[cfg(test)]
mod tests {
    use super::*;
    use aegis::aegis128l::Aegis128L;

    #[test]
    fn decrypt() {
        // doesn't start with E:
        assert!(matches!(
            decrypt_message(b"", &[0; 16]),
            Err(DecryptError::BadFormat)
        ));

        // no key ID end marker
        assert!(matches!(
            decrypt_message(b"E:abc", &[0; 16]),
            Err(DecryptError::BadFormat)
        ));

        // no algorithm end marker
        assert!(matches!(
            decrypt_message(b"E:abc:rot26", &[0; 16]),
            Err(DecryptError::BadFormat)
        ));

        assert!(matches!(
            decrypt_message(b"E:abc:rot26:", &[0; 16]),
            Err(DecryptError::UnsupportedAlgorithm)
        ));

        // no nonce/tag
        assert!(matches!(
            decrypt_message(b"E:abc:aegis128l:12345678", &[0; 16]),
            Err(DecryptError::BadFormat)
        ));

        let data = b"hello world";
        let key = b"abababababababab";
        let nonce = b"cdcdcdcdcdcdcdcd";

        let cipher = Aegis128L::<32>::new(key, nonce);
        let (encrypted, tag) = cipher.encrypt(data, &[]);

        let mut out = Vec::new();

        out.extend(b"E:key1:");
        out.extend(ALGORITHM_NAME);
        out.extend(b":");
        out.extend(nonce);
        out.extend(&encrypted);
        out.extend(&tag);

        // wrong key
        assert!(matches!(
            decrypt_message(&out, b"aaaaaaaaaaaaaaaa"),
            Err(DecryptError::InvalidData),
        ));

        let plain = decrypt_message(&out, key).unwrap();
        assert_eq!(plain, b"hello world");
    }
}
