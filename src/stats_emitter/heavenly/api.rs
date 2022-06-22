//! Data models for Fastly API key information.
//!
//! Heavenly provides a collection of all active Fastly user tokens to authenticate purges at the
//! edge.

use crate::stats_emitter::heavenly::uuid::{CustomerID, ServiceID};
use serde::{
    de::{self, Deserializer, Visitor},
    ser::Serializer,
    Deserialize, Serialize,
};
use std::convert::TryInto;
use std::fmt::{self, Debug, Display};

#[derive(Serialize, Deserialize, Debug)]
pub struct ApiJson<'json> {
    #[serde(borrow = "'json")]
    pub user_keys: Vec<UserKeyJson<'json>>,
    pub fastly_customer_id: CustomerID,
}

#[derive(Serialize, Deserialize, Debug)]
pub struct UserKeyJson<'json> {
    #[serde(borrow = "'json")]
    pub key_hash: KeyHashJson<'json>,
    pub customer_id: CustomerID,
    pub services: Option<Vec<ServiceID>>,
}

#[derive(Copy, Clone, PartialEq, Eq)]
pub struct KeyHashJson<'json>(&'json [u8; 64]);

impl KeyHashJson<'_> {
    pub fn as_str(&self) -> &str {
        // Safety: Deserialize::deserialize will have ensured the bytes in the array are only ASCII
        // bytes for hex digits.
        unsafe { std::str::from_utf8_unchecked(&self.0[..]) }
    }
    pub fn as_bytes(&self) -> &[u8; 64] {
        &self.0
    }
    pub fn to_array(&self) -> [u8; 32] {
        #[inline]
        fn from_hex(h: u8) -> u8 {
            match h {
                b'0'..=b'9' => h - b'0',
                b'A'..=b'F' => h - b'A' + 0xA,
                b'a'..=b'f' => h - b'a' + 0xA,
                _ => unreachable!("bytes will have been valiated to be only [a-fA-F0-9]"),
            }
        }
        let mut out = [0; 32];
        for (i, byte) in out.iter_mut().enumerate() {
            *byte = (from_hex(self.0[i * 2]) << 4) | from_hex(self.0[i * 2 + 1])
        }
        out
    }
}

impl From<&KeyHashJson<'_>> for [u8; 32] {
    fn from(k: &KeyHashJson<'_>) -> Self {
        k.to_array()
    }
}

impl Display for KeyHashJson<'_> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

impl Debug for KeyHashJson<'_> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.debug_tuple("KeyHashJson").field(&self.as_str()).finish()
    }
}

impl Serialize for KeyHashJson<'_> {
    fn serialize<S>(&self, s: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        s.serialize_str(self.as_str())
    }
}

impl<'de: 'json, 'json> Deserialize<'de> for KeyHashJson<'json> {
    fn deserialize<D>(d: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct JsonVisitor;
        impl<'de> Visitor<'de> for JsonVisitor {
            type Value = KeyHashJson<'de>;
            fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
                f.write_str("string with no escapes matching `^[0-9a-fA-F]{64}$`")
            }

            fn visit_borrowed_str<E>(self, s: &'de str) -> Result<Self::Value, E>
            where
                E: de::Error,
            {
                if s.len() != 64 {
                    return Err(de::Error::custom(format!(
                        "string is {} bytes long, must be 64",
                        s.len()
                    )));
                }
                let b = s.as_bytes();
                if let Some(c) = b
                    .iter()
                    .find(|c| !matches!(c, b'0'..=b'9' | b'A'..=b'F' | b'a'..=b'f'))
                {
                    return Err(de::Error::custom(if c.is_ascii_graphic() {
                        format!("hash contains invalid char '{}'", unsafe {
                            std::str::from_utf8_unchecked(std::slice::from_ref(c))
                        })
                    } else {
                        format!("hash contains invalid char {:#X}", c)
                    }));
                }

                Ok(KeyHashJson(
                    b.try_into()
                        .expect("string has been checked to be 64-bytes long"),
                ))
            }
        }

        d.deserialize_str(JsonVisitor)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_from_json() {
        static TEST_JSON: &str = r#"{
  "fastly_customer_id": "M4HCwJxJPGCIBSlRd5ETh",
  "user_keys": [
    {
      "customer_id": "M4HCwJxJPGCIBSlRd5ETh",
      "key_hash": "19e206f4ad87b5ed771487c6a5e565cbb7d41378751a3956b9940dbc58ba90ea"
    }, {
      "customer_id": "3L7L7BLCFmHL4LHfehyUi7",
      "key_hash": "25e1fe622bfc38d04c6371c54ec6950bde264a9576612018607cf3cf801ba0fc"
    }, {
      "customer_id": "NotPvtCustomer2958",
      "key_hash": "2fd9cdbb0e142b2a5171bf4497ad147c13df3f42976b04008c15024dc9f45549"
    }, {
      "customer_id": "M4HCwJxJPGCIBSlRd5ETh",
      "key_hash": "38ccd68ec15d83d101f75ed3eb6037413116a4e63626975f3b6b735a323d2d02"
    }, {
      "customer_id": "M4HCwJxJPGCIBSlRd5ETh",
      "key_hash": "750f06504e34ab8439835f306d493ea54ecc946bdcdd2660561637bd47d72e51"
    }, {
      "customer_id": "M4HCwJxJPGCIBSlRd5ETh",
      "key_hash": "90ac86d256c5b39be4926eec2157625f522ebdf7152ca224f9c87bbb75ed6307"
    }, {
      "customer_id": "M4HCwJxJPGCIBSlRd5ETh",
      "key_hash": "bdfc59a315199789cc81b6b32e8bca9d78bf3b11d4e81bff44bea8458ebc34d0"
    }, {
      "customer_id": "wocKaWockAE5wCi4XH4MJ",
      "key_hash": "c4fd54ee03b7b89173c637ead4c2ef7df41ef2ceefefe55de69cbdf8263cff0c",
      "services": [ "7OfMZIkZUwMflLWkNP3dTo" ]
    }, {
      "customer_id": "M4HCwJxJPGCIBSlRd5ETh",
      "key_hash": "d58a8088db3f5d01ba5c5ca4bf29082935b4f583fb9b42d565545d1c0d68b690"
    }, {
      "customer_id": "3L7L7BLCFmHL4LHfehyUi7",
      "key_hash": "dccd49e1c9ff6737c8d0144a681c35ed9bdf386e74e226a48281f8625761a6cc"
    }, {
      "customer_id": "M4HCwJxJPGCIBSlRd5ETh",
      "key_hash": "f88c0958dbf326227fc803db2e708ab34aa4c5f8e8576bd024fdd6ef57871bd7",
      "services": [ "67nc53aNztXRrejB6wYHYD" ]
    }
  ]
}"#;
        const FIRST_HASH: [u8; 32] = [
            0x19, 0xe2, 0x06, 0xf4, 0xad, 0x87, 0xb5, 0xed, 0x77, 0x14, 0x87, 0xc6, 0xa5, 0xe5,
            0x65, 0xcb, 0xb7, 0xd4, 0x13, 0x78, 0x75, 0x1a, 0x39, 0x56, 0xb9, 0x94, 0x0d, 0xbc,
            0x58, 0xba, 0x90, 0xea,
        ];

        let obj: ApiJson = serde_json::from_str(TEST_JSON).expect("json parses");
        assert_eq!(obj.fastly_customer_id, "M4HCwJxJPGCIBSlRd5ETh");
        assert_eq!(obj.user_keys[0].customer_id, "M4HCwJxJPGCIBSlRd5ETh");
        assert_eq!(obj.fastly_customer_id, obj.user_keys[0].customer_id);

        assert_eq!(obj.user_keys[0].key_hash.to_array(), FIRST_HASH);
        assert_eq!(
            FIRST_HASH,
            Into::<[u8; 32]>::into(&obj.user_keys[0].key_hash),
        );

        // ensure borrowing is actually happening like we hoped...
        assert_eq!(
            TEST_JSON.as_bytes().as_ptr() as usize
                + TEST_JSON
                    .find("19e206f4ad87b5ed771487c6a5e565cbb7d41378751a3956b9940dbc58ba90ea")
                    .unwrap(),
            obj.user_keys[0].key_hash.as_bytes().as_ptr() as usize
        );

        let key_hash: KeyHashJson = serde_json::from_str(
            "\"0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789ABCDEF\"",
        )
        .unwrap();
        assert_eq!(
            key_hash.to_array(),
            [
                0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB,
                0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67,
                0x89, 0xAB, 0xCD, 0xEF
            ]
        );
    }

    #[test]
    fn test_invalid_json() {
        for (s, e) in &[
            (
                "\"750f06504e34ab8439835f306d493ea54ecc946bdcdd2660561637bd47d72e5\"",
                "string is 63 bytes long, must be 64",
            ),
            ("\"\"", "string is 0 bytes long, must be 64"),
            (
                "\"750f06504e34ab8439835f306d493ea54ecc946bdcdd2660561637bd47d72e512\"",
                "string is 65 bytes long, must be 64",
            ),
            (
                "\"750f06504e34ab8439835f306d493ea54ecc946bdcdd2660561637bd47d72e5X\"",
                "hash contains invalid char 'X'",
            ),
            (
                "\"750f06504G34ab8439835f306d493ea54ecc946bdcdd2660561637bd47d72e5X\"",
                "hash contains invalid char 'G'",
            ),
            (
                "\"750f06504e34ab8439835f306d493ea54ecc946bdcdd2660561637bd47d72e5 \"",
                "hash contains invalid char 0x20",
            ),
            (
                "\"750f06504e34ab\\n439835f306d493ea54ecc946bdcdd2660561637bd47d72e5 \"",
                "string with no escapes matching `^[0-9a-fA-F]{64}$`",
            ),
        ] {
            let out_err = serde_json::from_str::<KeyHashJson>(s)
                .err()
                .unwrap_or_else(|| {
                    panic!("expected {:?} to fail when deserialized as KeyHashJson", s)
                })
                .to_string();
            assert!(
                out_err.contains(e),
                "When parsing {:?}, error {:?} did not contain {:?}",
                s,
                out_err,
                e
            );
        }
    }
}
