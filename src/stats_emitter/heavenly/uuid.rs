//! Control plane identifiers.
//!
//! See the crate-level documentation for more info.

/// Defines [`common_traits!`] and [`uuid_type!`] macros for UUID types.
#[macro_use]
mod macros;
/// Parsing errors and related types.
mod error;

/// Unit tests for Heavenly identifiers.
#[cfg(tests)]
mod tests;

pub use self::error::{InvalidID, ParseError};

use serde::de::{self, Visitor};
use serde::{Deserialize, Deserializer, Serialize, Serializer};
use std::convert::{TryFrom, TryInto};
use std::ops::Deref;

#[inline]
const fn is_valid_char(c: u8) -> bool {
    matches!(c, b'0'..=b'9' | b'A'..=b'Z' | b'a'..=b'z')
}

#[derive(Copy, Clone, PartialEq, Eq, Hash)]
pub struct HeavenlyUuid {
    len: u8,
    b: [u8; 22],
}

common_traits!(HeavenlyUuid);

// Define a UUID type that represents a Fastly service.
uuid_type!(ServiceID);

// Define a UUID type that represents a Fastly customer.
uuid_type!(CustomerID);

// Define a UUID type that represents an attachment.
uuid_type!(AttachmentID);

/// [`HeavenlyUuids`] are sorted like the [`Strings`] they aim to represent.
impl Ord for HeavenlyUuid {
    #[inline]
    fn cmp(&self, other: &HeavenlyUuid) -> std::cmp::Ordering {
        self.as_str().cmp(other.as_str())
    }
}

/// [`HeavenlyUuids`] are sorted like the [`Strings`] they aim to represent.
impl PartialOrd for HeavenlyUuid {
    #[inline]
    fn partial_cmp(&self, other: &HeavenlyUuid) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl HeavenlyUuid {
    /// `GLOBAL` is the singleton key used by `backend_acls.rs's instance of `ModuleLoader`.
    pub const GLOBAL: Self = Self::from_static("global");

    /// Create a [`HeavenlyUuid`] from a slice of bytes.
    fn from_bytes(bytes: &[u8]) -> Result<Self, ParseError> {
        let n = bytes.len();
        if n == 0 {
            return Err(ParseError::HeavenlyUuidEmpty);
        } else if n > 22 {
            return Err(ParseError::HeavenlyUuidTooLong(bytes.into()));
        }

        // A valid heavenly-generated uuid consists of 1 to 22 base62 characters.
        // Recently created service ids are exactly 22 characters long.
        // A special service id like "healthcheck", and service ids
        // generated before padding was introduced, can be shorter.
        // Tests also use shorter service ids like "service1" for
        // convenience.

        for c in bytes.iter() {
            if !is_valid_char(*c) {
                return Err(ParseError::HeavenlyUuidInvalid(bytes.into()));
            }
        }

        let mut b = [0u8; 22];
        (&mut b[..n]).copy_from_slice(bytes);
        Ok(HeavenlyUuid { len: n as u8, b })
    }

    /// Create a [`HeavenlyUuid`] from a static string.
    ///
    /// # Panics
    ///
    /// This is meant to create compile-time constants. It will cause compilation failures
    /// (or panic) if fed an invalid UUID.
    pub const fn from_static(id: &'static str) -> Self {
        let id = id.as_bytes();
        let mut ret = Self {
            len: id.len() as u8,
            b: [0; 22],
        };
        let mut i = 0;
        loop {
            // There must be at least one byte, and no more than 22. An invalid character becomes a
            // compile-time error by using usize::MAX as the destination index.
            let p = if is_valid_char(id[i]) { i } else { usize::MAX };
            ret.b[p] = id[i];
            i += 1;
            if i >= id.len() {
                break;
            }
        }
        ret
    }

    /// Reference a UUID as a slice of bytes.
    #[inline]
    pub fn as_bytes(&self) -> &[u8] {
        &self.b[..self.len as usize]
    }

    /// Reference a UUID as a string.
    #[inline]
    pub fn as_str(&self) -> &str {
        // Safety: Every manner there is to create a HeavenlyUuid enforces that all the bytes are
        // ASCII; '0'-'9', 'A'-'Z', or 'a'-'z'.
        unsafe { std::str::from_utf8_unchecked(self.as_bytes()) }
    }
}

// A HeavenlyUuid is meant to work just like a very compact String, as it is constructed as a length
// and some bytes, but specialized because its length and contents are restricted.  We want it
// conveyed in JSON as a String, so we must roll our own Deserialize.
impl<'de> Deserialize<'de> for HeavenlyUuid {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct JsonVisitor;
        impl<'de> Visitor<'de> for JsonVisitor {
            type Value = HeavenlyUuid;

            fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
                // There is only one way to create a ServiceID: `TryFrom<&[u8]> for HeavenlyUuid`,
                // which enforces that all the bytes are ASCII; '0'-'9', 'a'-'z', or 'A'-'Z'
                // and that the string is at most 22 bytes long.
                f.write_str("string matching /^[a-zA-Z0-9]{1,22}$/")
            }

            fn visit_borrowed_str<E>(self, v: &str) -> Result<Self::Value, E>
            where
                E: de::Error,
            {
                v.parse::<Self::Value>()
                    .map_err(|e| de::Error::custom(format!("{:?}", e)))
            }
        }

        if deserializer.is_human_readable() {
            deserializer.deserialize_str(JsonVisitor)
        } else {
            let raw: [u8; 23] = Deserialize::deserialize(deserializer)?;

            let len = raw[0];
            if !(1..=22).contains(&len) {
                return Err(de::Error::custom(format!(
                    "First byte is outside of range: {:?}",
                    raw
                )));
            }

            let b: [u8; 22] = (&raw[1..]).try_into().unwrap();

            let (uuid, zeros) = (&b[..]).split_at(len as usize);
            if !uuid.iter().all(|&c| is_valid_char(c)) {
                return Err(de::Error::custom(format!(
                    "Heavenly UUID contains a character outside of the base64 set: {:?}",
                    raw
                )));
            } else if !zeros.iter().all(|&c| c == 0) {
                return Err(de::Error::custom(format!(
                    "Trailing bytes are non-zero: {:?}",
                    raw
                )));
            }

            Ok(Self { len, b })
        }
    }
}

// When serializing a HeavenlyUuid, the serde data type we want to use depends on the serialization
// format. For JSON (human readable), a string is ideal. For bincode, we'll recreate what would
// have been done with #[derive(Serialize,Deserialize)], which is to simply treat the thing as a
// sequence of 23 bytes.
//
// Obviously the Serialize and Deserialize formats must agree for anything to be useful, but the
// most important reason to have a custom implementation of Deserialize is to ensure that we can never
// deserialize something the same shape (ie, {u8, [u8]}) that includes illegal characters.
impl Serialize for HeavenlyUuid {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        if serializer.is_human_readable() {
            serializer.serialize_str(self.as_str())
        } else {
            // For bincode (and others), this is what #[derive(Serialize)] would do, but with more
            // steps, and more lines of code. Ultimately what ends up "on the wire" looks like an
            // array of 23 bytes.
            let mut tmp = [0u8; 23];
            tmp[0] = self.len;
            (&mut tmp[1..(1 + self.len as usize)]).copy_from_slice(self.as_bytes());
            tmp.serialize(serializer)
        }
    }
}
