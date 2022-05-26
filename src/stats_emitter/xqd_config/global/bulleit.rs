use serde::Deserialize;
use std::path::PathBuf;

/// The maximum log line length that we will allow.
///
/// Bulleit has a hard cap of 64KiB, since it uses a `bufio.NewScanner` to read newlines, and Go's
/// `bufio.Scanner` defaults to a cap of `bufio.MaxScanTokenSize` until it errors if it cannot find
/// the terminating token (a newline). `bufio.MaxScanTokenSize` is 64 * 1024.
///
/// Bulleit requires an header of `<service id><space><endpoint name><space>::<space>`. <service id>
/// can be up to 24 characters [1], <endpoint name> can be up to 255 characters [2], plus the
/// spacing, and a terminating newline. These are all counted against the data customers can pass
/// through bulleit per line. Customers with longer endpoint names will be able to send a few less
/// bytes per line, customers with shorter endpoint names will be able to send a few more bytes per
/// line. Ex: Given a customer with an id of "service1", and an endpoint name of "other_endpoint",
/// they would be able to send 65507 bytes ((16 << 12) - 1 - len("service1") - len("other_endpoint")
/// - len("::") - count(<header spaces>) - len("\n")).
///
/// Reserving 1 byte here to make sure handling the end of a 64KiB buffer isn't an issue.
/// Another 1 byte is reserved in Line's format method for the newline.
///
/// 1 -
/// https://github.com/fastly/Heavenly/blob/5528bd1bbf7943b1de962ff29c962a768e2fc012/db/20220119.sql#L86
/// 2 -
/// https://github.com/fastly/Heavenly/blob/5528bd1bbf7943b1de962ff29c962a768e2fc012/db/20220119.sql#L964
pub const LOG_MAX_LENGTH: usize = (16 << 12) - 1;

#[derive(Clone, Debug, Deserialize)]
pub struct BulleitConfig {
    pub socket_path: Option<PathBuf>,
    pub socket_reconnect_interval_millis: u64,
    pub num_reusable_entries: usize,
    pub num_nonreusable_entries: Option<usize>,
    pub log_bytes_buffer_capacity: usize,
}

impl Default for BulleitConfig {
    /// Returns the default [`BulleitConfig`] configurations.
    ///
    /// See the documentation on [`LOG_MAX_LENGTH`] for more information about these values.
    fn default() -> Self {
        Self {
            // No socket path is provided by default; this will discard log events.
            socket_path: None,
            socket_reconnect_interval_millis: 250,
            num_reusable_entries: 64 << 10,
            num_nonreusable_entries: Some(64 << 10),
            log_bytes_buffer_capacity: 1 << 20,
        }
    }
}
