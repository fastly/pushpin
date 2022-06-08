#![allow(dead_code)]

//! # Exponential Back-off with Jitter
//!
//! This module implements the "Full Jitter" variant described
//! [here](https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/)
//! with the addition of a lower bound for the calculated delay.

mod backoff;
mod errors;
mod fixed_backoff;
mod future;
/// [`Backoff`] unit tests.
#[cfg(test)]
mod tests;

pub use backoff::*;
pub use errors::*;
pub use fixed_backoff::*;
pub use future::*;
