/// Errors that can arise while parsing a [`HeavenlyUuid`][super::HeavenlyUuid].
#[derive(Debug, thiserror::Error)]
pub enum ParseError {
    #[error("Empty Heavenly UUID")]
    HeavenlyUuidEmpty,
    #[error("Heavenly UUID longer than the 22 character limit: {0:?}")]
    HeavenlyUuidTooLong(InvalidID),
    #[error("Heavenly UUID contains a character outside of the base62 set: {0:?}")]
    HeavenlyUuidInvalid(InvalidID),
}

/// An invalid [`HeavenlyUuid`][super::HeavenlyUuid].
pub struct InvalidID(Vec<u8>);

impl From<&[u8]> for InvalidID {
    fn from(b: &[u8]) -> Self {
        InvalidID(b.to_vec())
    }
}

impl std::fmt::Debug for InvalidID {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        let b = self.0.as_slice();
        match std::str::from_utf8(b) {
            Ok(s) => write!(f, "{:?}", s),
            Err(e) => write!(f, "({:?}: {})", b, e),
        }
    }
}
