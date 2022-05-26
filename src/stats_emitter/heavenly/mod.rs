//! Control plane tools.
//!
//! # Identifiers
//!
//! The [`uuid`] module defines various identifier types.
//!
//! The control plane system [Heavenly] generates unique identifiers for customers, services, and
//! other objects in its data model. The unique identifiers for different types of objects have
//! the same format.
//!
//! [`HeavenlyUuid`] represents a generic identifier, and can be [`Deserialize`]d
//! from a slice of bytes using [serde], or from a static string using
//! [`from_static`][uuid::HeavenlyUuid::from_static].
//!
//! [`ServiceID`] and [`CustomerID`] are distinct unique identifier types for services and for
//! customers, respectively.  [`ServiceID`]s and [`CustomerID`]s can be used as a [`HeavenlyUuid`]
//! via the [`Deref`] trait, to make it easy to use either in a context that needs a
//! [`HeavenlyUuid`] without converting back and forth.
//!
//! [`CustomerID`]: uuid::CustomerID
//! [`Deserialize`]: serde::Deserialize
//! [Heavenly]: https://github.com/fastly/heavenly
//! [`HeavenlyUuid`]: uuid::HeavenlyUuid
//! [serde]: docs.rs/serde/latest
//! [`ServiceID`]: uuid::ServiceID

pub mod api;
pub mod uuid;
