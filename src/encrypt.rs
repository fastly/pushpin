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

#[cfg(test)]
mod tests {
    use super::*;
    use aegis::aegis128l::Aegis128L;

    #[test]
    fn decrypt() {
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

        let plain = decrypt_message(&out, key).unwrap();
        assert_eq!(plain, b"hello world");
    }
}
