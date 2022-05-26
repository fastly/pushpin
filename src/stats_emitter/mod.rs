mod data_types;
mod errors;
mod heavenly;
mod message_aggregator;
mod message_sender;
mod options;
#[cfg(test)]
mod tests;
pub mod xqd_backoff;
mod xqd_config;

// What is actually exposed to users
pub use data_types::ChannelMessage;
pub use errors::EmitterError;
pub use message_aggregator::{MessageAggregator, MessageAggregatorSender};
