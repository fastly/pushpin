/// Configuration errors.
///
/// These errors represent different reasons a [`Backoff`][crate::backoff::Backoff]
/// configuration might not be valid.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("The upper bound {max}ms is not larger than the lower bound {min}ms")]
    MaxNotAboveMin { min: u32, max: u32 },

    #[error("Parameter {0} in seconds cannot be represented as u32 milliseconds")]
    OutOfBounds(f32),

    #[error("A value for {0} was not provided")]
    MissingSetting(&'static str),
}
