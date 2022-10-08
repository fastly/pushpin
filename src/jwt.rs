/*
 * Copyright (C) 2022 Fanout, Inc.
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
