/// Implements a variety of helpful traits that happen to have identical implementations between
/// HeavenlyUuid, ServiceID, and CustomerID.
macro_rules! common_traits {
    ($type:ident) => {
        impl std::str::FromStr for $type {
            type Err = ParseError;
            fn from_str(s: &str) -> Result<Self, Self::Err> {
                s.as_bytes().try_into()
            }
        }

        impl TryFrom<&str> for $type {
            type Error = ParseError;
            fn try_from(s: &str) -> Result<Self, Self::Error> {
                s.as_bytes().try_into()
            }
        }

        impl TryFrom<&[u8]> for $type {
            type Error = ParseError;
            fn try_from(b: &[u8]) -> Result<Self, Self::Error> {
                $type::from_bytes(b)
            }
        }

        impl std::fmt::Display for $type {
            fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
                f.write_str(self.as_str())
            }
        }

        impl std::fmt::Debug for $type {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                f.debug_tuple(stringify!($type))
                    .field(&self.as_str())
                    .finish()
            }
        }

        impl PartialEq<str> for $type {
            #[inline]
            fn eq(&self, other: &str) -> bool {
                self.as_str().eq(other)
            }
        }

        impl PartialEq<$type> for str {
            #[inline]
            fn eq(&self, other: &$type) -> bool {
                self.eq(other.as_str())
            }
        }

        impl PartialEq<&str> for $type {
            #[inline]
            fn eq(&self, other: &&str) -> bool {
                self.as_str().eq(*other)
            }
        }

        impl PartialEq<$type> for &str {
            #[inline]
            fn eq(&self, other: &$type) -> bool {
                other.eq(*self)
            }
        }

        impl AsRef<HeavenlyUuid> for $type {
            #[inline]
            fn as_ref(&self) -> &HeavenlyUuid {
                self
            }
        }
    };
}

/// Defines a [`HeavenlyUuid`] wrapper type.
///
/// This is used to generate a common shared interface for entity-specific identifiers that *wrap*
/// an inner [`HeavenlyUuid`]. N.B. This also additionally invokes [`common_traits`] to define
/// interfaces that are shared with [`HeavenlyUuid`] itself.
macro_rules! uuid_type {
    ($type:ident) => {
        #[derive(
            Copy, Clone, PartialEq, Eq, Hash, Ord, PartialOrd, serde::Serialize, serde::Deserialize,
        )]
        #[repr(transparent)]
        pub struct $type(HeavenlyUuid);

        impl $type {
            /// Create an identifier from a slice of bytes.
            pub fn from_bytes(b: &[u8]) -> Result<Self, ParseError> {
                HeavenlyUuid::from_bytes(b).map(Self)
            }

            /// Create an identifier from a static string.
            ///
            /// # Panics
            ///
            /// This is meant to create compile-time constants. It will cause compilation failures
            /// (or panic) if fed an invalid UUID.
            pub const fn from_static(id: &'static str) -> Self {
                Self(HeavenlyUuid::from_static(id))
            }

            /// Reference an identifier as a slice of bytes.
            pub fn as_bytes(&self) -> &[u8] {
                self.0.as_bytes()
            }

            /// Reference an identifier as a string.
            pub fn as_str(&self) -> &str {
                self.0.as_str()
            }
        }

        // Implement the same interface as `HeavenlyUuid` for this wrapper type.
        common_traits!($type);

        impl Deref for $type {
            type Target = HeavenlyUuid;
            #[inline]
            fn deref(&self) -> &Self::Target {
                &self.0
            }
        }

        impl From<$type> for HeavenlyUuid {
            #[inline]
            fn from(t: $type) -> Self {
                t.0
            }
        }

        impl<'uuid> From<&'uuid HeavenlyUuid> for &'uuid $type {
            #[inline]
            fn from(h: &'uuid HeavenlyUuid) -> Self {
                // Safety: $type is repr(transparent), lifetimes are explicitly bounded.
                unsafe { &*(h as *const HeavenlyUuid as *const $type) }
            }
        }

        impl From<HeavenlyUuid> for $type {
            #[inline]
            fn from(u: HeavenlyUuid) -> Self {
                Self(u)
            }
        }

        impl PartialEq<HeavenlyUuid> for $type {
            #[inline]
            fn eq(&self, other: &HeavenlyUuid) -> bool {
                other == self.deref()
            }
        }

        impl PartialEq<$type> for HeavenlyUuid {
            #[inline]
            fn eq(&self, other: &$type) -> bool {
                self == other.deref()
            }
        }
    };
}
