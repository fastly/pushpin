use pin_project::pin_project;
use std::{
    future::Future,
    pin::Pin,
    sync::Arc,
    task::{Context, Poll},
};
use tokio::time::{sleep, Duration, Sleep};

#[pin_project]
#[derive(Debug)]
pub struct BackoffFuture {
    // Sleep must be pinned since we will poll it according to the tokio docs
    #[pin]
    sleep: Option<Sleep>,
    logged_ctx: bool,
    ctx: Arc<String>,
    future_duration: Duration,
    total_duration: Duration,
    retry: usize,
}

impl BackoffFuture {
    pub(crate) fn new(
        ctx: Arc<String>,
        future_duration: Duration,
        total_duration: Duration,
        retry: usize,
    ) -> Self {
        Self {
            sleep: None,
            ctx,
            future_duration,
            total_duration,
            logged_ctx: false,
            retry,
        }
    }

    /// Total time for the future to run in as milliseconds
    pub fn time_ms(&self) -> u128 {
        self.future_duration.as_millis()
    }

    /// Total time for the future to run in as seconds
    pub fn time_s(&self) -> f32 {
        self.future_duration.as_secs_f32()
    }
}

impl Future for BackoffFuture {
    type Output = ();
    fn poll(self: Pin<&mut Self>, future_ctx: &mut Context<'_>) -> Poll<Self::Output> {
        let mut this = self.project();
        // Create sleep on first poll so that the sleep only starts the first
        // time it is awaited
        if this.sleep.is_none() {
            this.sleep.set(Some(sleep(*this.future_duration)));
        }

        if !*this.logged_ctx {
            tracing::warn!(
                "Backoff occuring. Context: {}. Retry #{}. Time: {}ms. Total Retry Time: {}ms",
                this.ctx,
                this.retry,
                this.future_duration.as_millis(),
                this.total_duration.as_millis()
            );
            *this.logged_ctx = true;
        }

        this.sleep
            .as_pin_mut()
            .expect("sleep future was made on first poll")
            .poll(future_ctx)
    }
}
