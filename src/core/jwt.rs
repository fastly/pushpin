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
    // claim is already serialized, but the jsonwebtoken crate requires a
    // serializable object. so we'll deserialize to a generic value first
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
