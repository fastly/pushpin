use serde::Deserialize;
use std::path::PathBuf;

#[derive(Clone, Debug, Deserialize)]
pub struct RawUpstreamConfig {
    pub connect: Vec<String>,
    pub hcd: Option<RawHcdConfig>,
    pub connection_broker: RawConnectionBrokerConfig,
}

#[derive(Clone, Debug, Deserialize)]
pub struct RawHcdConfig {
    pub port: u16,
    pub healthcheck_duration_ms: u64,
    pub cache_duration_ms: u64,
    pub timeout_ms: u64,
    pub freud: Option<RawFreudConfig>,
}

#[derive(Clone, Debug, Deserialize)]
pub struct RawFreudConfig {
    pub port: u16,
    pub cache_duration_ms: Option<u64>,
}

#[derive(Clone, Debug, Deserialize)]
pub struct RawConnectionBrokerConfig {
    /// The path of the Unix socket over which XQD will connect to the broker.
    pub socket_path: PathBuf,
    /// The number of threads used by the broker's primary Tokio runtime. If unspecified, the
    /// default is 2.
    pub num_threads: Option<usize>,
    /// The path to the config file that will be generated before executing
    /// `xqd-connection-broker`. If unspecified, `xqd-connection-broker.json` within the same
    /// directory as `connection_broker_socket_path` will be used.
    pub config_path: Option<PathBuf>,
    /// The path to the `xqd-connection-broker` binary. If unspecified, an `xqd-connection-broker`
    /// binary in the same directory as `xqd` will be used.
    pub bin_path: Option<PathBuf>,

    pub privacy_proxy_socket_path: Option<PathBuf>,
}
