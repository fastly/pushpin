use crate::stats_emitter::xqd_backoff::{BackoffFuture, Error};
use rand::Rng;
use serde::{de, Deserialize, Deserializer};
use std::{cmp::min, sync::Arc};
use tokio::time::Duration;

#[derive(Clone, Debug)]
pub struct Backoff {
    /// The minimum possible delay, in milliseconds.
    pub(crate) min_ms: u32,
    /// The maximum possible delay, in milliseconds.
    pub(crate) max_ms: u32,
    /// The initial delay, in milliseconds.
    pub(crate) base_ms: u32,
    /// The limit to how many times a retry will be made
    max_retries: Option<usize>,
    /// The place in which this backoff is being used for. This allows logging
    /// more contextual information since a backoff can be used in many places
    ctx: Arc<String>,
    /// The current backoff "step."
    step: usize,
    /// Total duration of all [`BackoffFutures`][crate::future::BackoffFuture]
    /// made so far.
    total_duration: Duration,
}

impl Backoff {
    /// Build a [`Backoff`] and provide context for where it will be used.
    pub fn build() -> BackoffBuilder {
        BackoffBuilder {
            min_ms: Ok(None),
            max_ms: Ok(None),
            base_ms: Ok(None),
            max_retries: None,
            ctx: None,
        }
    }

    /// Reset the current exponential backoff step.
    pub fn reset(&mut self) {
        self.step = 0;
        self.total_duration = Duration::default();
    }
}

impl Iterator for Backoff {
    type Item = BackoffFuture;
    fn next(&mut self) -> Option<Self::Item> {
        if self
            .max_retries
            .map(|max| self.step >= max)
            .unwrap_or(false)
        {
            return None;
        }

        // In order to produce some level of jitter and randomness we pass a range to `gen_range`
        // that uses it to create a random number value. This is clamped by calling min with
        // `max_ms - min_ms` so that we can't go above that max time value.
        let r = rand::thread_rng().gen_range(
            0..min(
                self.max_ms - self.min_ms,
                self.base_ms
                    // saturating_mul will multiply base_ms by some number and
                    // make a number that will not overflow. Essentially if the
                    // value times base_ms >= 2^32 then it will be set to 2^32 - 1.
                    // We pass it 1 that will shift left the current step value
                    // up to 31 bits at most to avoid overflow.
                    .saturating_mul(1u32 << min(31, self.step as u8)),
            ),
        );

        let duration =
            Duration::from_millis(min(self.max_ms as u64, self.min_ms as u64 + r as u64));
        self.total_duration += duration;
        self.step += 1;

        Some(BackoffFuture::new(
            self.ctx.clone(),
            duration,
            self.total_duration,
            self.step,
        ))
    }
}

/// A builder-pattern initializer for [`Backoff`].
///
/// Acquire a new builder with [`Backoff::build`].
#[derive(Debug, Deserialize)]
pub struct BackoffBuilder {
    /// The minimum possible delay, if given, in milliseconds.
    #[serde(deserialize_with = "to_ms", rename = "min_s")]
    min_ms: Result<Option<u32>, Error>,
    /// The maximum possible delay, if given, in milliseconds.
    #[serde(deserialize_with = "to_ms", rename = "max_s")]
    max_ms: Result<Option<u32>, Error>,
    /// The initial delay, if given, in milliseconds.
    #[serde(deserialize_with = "to_ms", rename = "base_s")]
    base_ms: Result<Option<u32>, Error>,
    /// The limit to how many times a retry will be made
    #[serde(skip_deserializing)]
    // value will default to None if deserializing.
    max_retries: Option<usize>,
    /// Context in which the backoff will be used
    #[serde(skip_deserializing)]
    // value will default to None and must be set `with_context` if deserializing.
    ctx: Option<Arc<String>>,
}

fn to_ms<'de, D>(input: D) -> Result<Result<Option<u32>, Error>, D::Error>
where
    D: Deserializer<'de>,
{
    Ok(Ok(Some(
        secs_to_millisecs(f32::deserialize(input)?).map_err(de::Error::custom)?,
    )))
}

fn secs_to_millisecs(val: f32) -> Result<u32, Error> {
    let val = val * 1000.0;
    if val < 0.0 || val.is_infinite() || val as f64 > u32::MAX as f64 {
        Err(Error::OutOfBounds(val))
    } else {
        Ok(val as u32)
    }
}

impl BackoffBuilder {
    /// Set the base backoff delay, in seconds.
    pub fn with_base_s(self, base_s: f32) -> Self {
        Self {
            base_ms: Some(secs_to_millisecs(base_s)).transpose(),
            ..self
        }
    }

    /// Set the minimum backoff delay, in seconds.
    pub fn with_min_s(self, min_s: f32) -> Self {
        Self {
            min_ms: Some(secs_to_millisecs(min_s)).transpose(),
            ..self
        }
    }

    /// Set the maximum backoff delay, in seconds.
    pub fn with_max_s(self, max_s: f32) -> Self {
        Self {
            max_ms: Some(secs_to_millisecs(max_s)).transpose(),
            ..self
        }
    }

    /// Set the maximum amount of retries a [`Backoff`] can make before being
    /// reset.
    pub fn with_max_retries(self, max: usize) -> Self {
        Self {
            max_retries: Some(max),
            ..self
        }
    }

    /// Set's the context of where this [`Backoff`] will be used
    pub fn with_context(self, ctx: impl ToString) -> Self {
        Self {
            ctx: Some(Arc::new(ctx.to_string())),
            ..self
        }
    }

    /// Initialize a [`Backoff`] using the given settings.
    ///
    /// Returns an [`Error`] if an invalid configuration was used.
    pub fn init(self) -> Result<Backoff, Error> {
        let min_ms = self.min_ms?.ok_or(Error::MissingSetting("min_s"))?;
        let max_ms = self.max_ms?.ok_or(Error::MissingSetting("max_s"))?;
        let base_ms = self.base_ms?.ok_or(Error::MissingSetting("base_s"))?;
        let ctx = self.ctx.ok_or(Error::MissingSetting("context"))?;
        let max_retries = self.max_retries;

        if min_ms >= max_ms {
            return Err(Error::MaxNotAboveMin {
                min: min_ms,
                max: max_ms,
            });
        }

        Ok(Backoff {
            min_ms,
            max_ms,
            base_ms,
            max_retries,
            ctx,
            step: 0,
            total_duration: Duration::default(),
        })
    }
}
