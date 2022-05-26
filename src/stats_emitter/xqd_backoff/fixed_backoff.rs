use crate::stats_emitter::xqd_backoff::future::BackoffFuture;
use std::sync::Arc;
use tokio::time::Duration;

/// [`FixedBackoff`] is a type for when you want to specify a set of numbers for
/// backoff and do not need to worry about problems like the
/// [thundering herd](https://en.wikipedia.org/wiki/Thundering_herd_problem).
/// In these cases you can use a much simpler backoff that does not need to use
/// a random number generator in order to
/// ```
/// # #[tokio::main]
/// # async fn main() {
/// use pushpin::stats_emitter::xqd_backoff::FixedBackoff;
/// // Create a FixedBackoff
/// let mut fixed_backoff = FixedBackoff::new([0, 0, 1 , 2 ,3, 4], "testing context");
///
/// // Show that each step produces the exact numbers from the array
/// // until the end in which case it reuses the number until it is
/// // reset
/// assert_eq!(fixed_backoff.next().unwrap().time_ms(), 0);
/// assert_eq!(fixed_backoff.next().unwrap().time_ms(), 0);
/// assert_eq!(fixed_backoff.next().unwrap().time_ms(), 1);
/// assert_eq!(fixed_backoff.next().unwrap().time_ms(), 2);
/// assert_eq!(fixed_backoff.next().unwrap().time_ms(), 3);
/// assert_eq!(fixed_backoff.next().unwrap().time_ms(), 4);
/// assert_eq!(fixed_backoff.next().unwrap().time_ms(), 4);
/// fixed_backoff.reset();
/// assert_eq!(fixed_backoff.next().unwrap().time_ms(), 0);
///
/// // Can be used with any sized array
/// let mut iterator = FixedBackoff::new([0,1,2,4], "testing context two");
///
/// // Also implements Iterator
/// for (i, x) in iterator.take(5).enumerate() {
///   match i {
///     0 => assert_eq!(x.time_ms(), 0),
///     1 => assert_eq!(x.time_ms(), 1),
///     2 => assert_eq!(x.time_ms(), 2),
///     3 => assert_eq!(x.time_ms(), 4),
///     4 => assert_eq!(x.time_ms(), 4),
///     _ => break,
///   }
/// }
/// # }
/// ```
pub struct FixedBackoff<const N: usize> {
    backoff: [u64; N],
    ctx: Arc<String>,
    step: usize,
    total_duration: Duration,
    max_retries: Option<usize>,
}

impl<const N: usize> FixedBackoff<N> {
    /// A set of delays to use in milliseconds. Delays are used in the order
    /// they appear in the array passed in to this function.
    pub fn new(backoff: [u64; N], ctx: impl ToString) -> Self {
        Self {
            backoff,
            step: 0,
            ctx: Arc::new(ctx.to_string()),
            total_duration: Duration::default(),
            max_retries: None,
        }
    }

    /// Reset the backoff to the 1st value in the array and reset
    pub fn reset(&mut self) {
        self.step = 0;
        self.total_duration = Duration::default();
    }

    /// How many retries should be made past the length of the provided array
    ///
    /// ```
    /// use pushpin::stats_emitter::xqd_backoff::FixedBackoff;
    /// # #[tokio::main]
    /// # async fn main() {
    /// // Create a FixedBackoff with max retries
    /// let mut fixed_backoff = FixedBackoff::new([0, 1, 2 , 4 ,8], "testing context").with_max_retries(1);
    ///
    /// // Show that each step produces the exact numbers from the array
    /// // until the end in which case it reuses the number until it is
    /// // reset
    /// assert_eq!(fixed_backoff.next().unwrap().time_ms(), 0);
    /// assert_eq!(fixed_backoff.next().unwrap().time_ms(), 1);
    /// assert_eq!(fixed_backoff.next().unwrap().time_ms(), 2);
    /// assert_eq!(fixed_backoff.next().unwrap().time_ms(), 4);
    /// assert_eq!(fixed_backoff.next().unwrap().time_ms(), 8);
    /// assert_eq!(fixed_backoff.next().unwrap().time_ms(), 8);
    /// assert!(fixed_backoff.next().is_none());
    /// # }
    /// ```
    pub fn with_max_retries(mut self, max: usize) -> Self {
        self.max_retries = Some(self.backoff.len() + max);
        self
    }
}

impl<const N: usize> Iterator for FixedBackoff<N> {
    type Item = BackoffFuture;
    fn next(&mut self) -> Option<Self::Item> {
        let duration = if self
            .max_retries
            .map(|max| self.step >= max)
            .unwrap_or(false)
        {
            return None;
        } else if self.step >= (N - 1) {
            Duration::from_millis(self.backoff[N - 1])
        } else {
            Duration::from_millis(self.backoff[self.step])
        };
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
