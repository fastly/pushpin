mod attachments;
mod bulleit;
mod geolocation;

pub use attachments::*;
pub use bulleit::*;
pub use geolocation::*;

use serde::Deserialize;
use std::path::PathBuf;

#[derive(Clone, Debug, Deserialize)]
pub struct RawGlobalConfig {
    /// The number of this xqd instance.
    pub instance: Option<u64>,
    /// The version of this xqd daemon.
    pub version: String,
    /// The Cypress version used by this xqd daemon. This version is used by the loader to request
    /// code, compiled by Cypress, from Heavenly.
    pub cypress_version: String,
    pub cli_socket_path: PathBuf,
    pub cli_socket_chmod: Option<u32>,
    pub cli_socket_chgrp: Option<String>,
    pub cli_threads: usize,
    pub core_threads: usize,
    pub max_threads: usize,
    pub tokio_thread_stack_size: Option<usize>,
    pub services_dir: PathBuf,
    pub customers_dir: PathBuf,
    pub api_keys_dir: PathBuf,
    pub max_concurrent_backend_reqs: usize,
    pub backend_acls_dir: PathBuf,
    pub backend_block_list: Vec<String>,
    pub guest_timeout_s: Option<u64>,
    pub guest_max_wasm_ms: Option<u64>,
    pub edge_configly_path: PathBuf,
    pub id_key_path: PathBuf,

    pub enable_seccomp: bool,
    pub seccomp_testing_clock_getres_state: Option<SeccompState>,
    pub enable_dynamic_session_sizing: bool,
    pub max_dynamic_backends: usize,
    pub max_sessions: usize,
    pub service_max_code_size: u64,
    pub num_worklogs: u32,
    pub worklog_socket_path: PathBuf,

    pub default_send_session_error_details: bool,
    pub default_max_instances_percent_per_service: u8,
    pub default_max_instance_waiters_per_service: usize,
    pub default_instance_queue_timeout_ms: u64,
    pub default_max_concurrent_backend_reqs_per_service: usize,
    pub default_max_backend_reqs_per_instance: u32,
    pub default_max_dictionary_lookups_per_instance: u32,

    pub sidecar_request_timeout_ms: u64,

    pub attachments: RawAttachmentsConfig,
    pub bulleit: Option<BulleitConfig>,
    pub geolocation: RawGeolocationConfig,
    pub billing_stats: Option<BillingStatsConfig>,

    pub prometheus_threads: Option<usize>,
    pub prometheus: Option<Vec<RawPrometheusConfig>>,

    pub panic_behavior: PanicBehavior,

    pub cached: Option<RawCachedConfig>,

    pub cdn_loop_current_secret: String,
    pub cdn_loop_other_secrets: Vec<String>,

    pub cdn_loop_max_hops: usize,
    pub cdn_loop_max_services: usize,
    pub use_fastly_tcp_info: bool,

    pub powderhorn_socket_path: Option<PathBuf>,
    pub csec_agent_socket_path: Option<PathBuf>,
    pub object_store_edge_app_hostname: Option<String>,
    pub object_store_edge_app_private_key_path: Option<PathBuf>,
    pub object_store_edge_app_bypass_local_route_tables: Option<bool>,

    pub memory_tier_populations: MemoryTierPopulationConfig,
    pub mlockall_xqd_binary: bool,

    pub wasmtime: Option<RawWasmtimeConfig>,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum SeccompState {
    Allow,
    Warn,
    Deny,
}

#[derive(Clone, Debug, Deserialize)]
pub struct RawWasmtimeConfig {
    pub wasm_memory_size_limit: u64,
    pub wasm_stack_size: usize,
    pub wasm_op_slice: u64,
    pub epoch_interruption_us: u64,
    pub enforce_version_match: Option<bool>,
    pub module_loading_mlock_modules: bool,
    pub module_loading_deserialize_file: bool,
}

#[derive(Clone, Debug, Deserialize)]
pub struct BillingStatsConfig {
    pub emitter: BillingEmitter,
    pub nsq: NsqConfig,
    pub json: RawAggregatorConfig,
}

#[derive(Copy, Clone, Debug, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum BillingEmitter {
    Nsq,
    Json,
    Both,
    Neither,
}

impl BillingEmitter {
    pub fn is_nsq(&self) -> bool {
        &Self::Nsq == self
    }

    pub fn is_json(&self) -> bool {
        &Self::Json == self
    }

    pub fn is_both(&self) -> bool {
        &Self::Both == self
    }

    pub fn is_neither(&self) -> bool {
        &Self::Neither == self
    }
}

#[derive(Clone, Debug, Deserialize)]
pub struct NsqConfig {
    pub dump_file: Option<String>,
    pub client_cert_path: Option<String>,
    pub client_key_path: Option<String>,
    pub ca_cert_path: Option<String>,
    pub max_buffered_backlog: Option<usize>,
    pub pub_topic: Option<String>,
    pub pub_port: Option<u16>,
    pub pub_path: Option<String>,
}

/// Options for what to send in messages, how to send them, and for what purpose
#[derive(Debug, Clone, Deserialize)]
pub struct RawAggregatorConfig {
    // These options can only be determined at runtime and are optional only
    // because we need to be able to deserialize the configuration and set them
    // later
    /// The POP we send messages from
    pub datacenter: Option<String>,
    /// Who is emitting the messages e.g. xqd-3
    pub emitter: Option<String>,
    /// Which server in the POP is the message from
    pub server: Option<String>,

    // These can be known ahead of time and exist in the configuration
    /// The size of the queue of messages the `MessageAggregator` will hold
    pub queue_size: usize,
    /// The schema name of the messages we're sending. Can be anything like
    /// `xqd-billing-stats` or `xqd-custom-metrics`
    pub schema_name: String,

    #[serde(flatten)]
    /// What mode the message sender should operate under
    pub mode: RawMessageSenderMode,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(untagged)]
/// The mode the message sender will work in. If set to `Json` it will send the
/// messages to a url over http or if `mtls` is set then via https. This is the
/// mode we want to use in production. If set to `DumpFile` then messages will
/// be written to the given path on disk. This is primarily used for integration
/// testing, but could be used locally for debugging.
pub enum RawMessageSenderMode {
    Json {
        url: String,
        mtls: Option<MutualTlsConfig>,
    },
    DumpFile {
        dump_file: PathBuf,
    },
}

#[derive(Debug, Clone, Deserialize)]
pub struct MutualTlsConfig {
    pub cert_path: PathBuf,
    pub key_path: PathBuf,
    pub ca_path: PathBuf,
    #[serde(default)] // Defaults to false
    pub dangerous_no_peer_verification: bool,
}

#[derive(Copy, Clone, Debug, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PanicBehavior {
    Abort,
    Unwind,
}

#[derive(Clone, Debug, Deserialize)]
pub struct RawCachedConfig {
    pub socket_path: PathBuf,
    pub max_connections: usize,
    pub frontmatter_format_version: Option<u8>,
}

#[derive(Clone, Debug, Deserialize)]
pub struct RawPrometheusConfig {
    pub listen: String,
    pub encoding: String,
    pub listen_socket_chmod: Option<u32>,
    pub listen_socket_chgrp: Option<String>,
}

#[derive(Clone, Debug, Deserialize)]
pub struct MemoryTierPopulationConfig {
    pub tier_2mb: usize,
    pub tier_4mb: usize,
    pub tier_8mb: usize,
    pub tier_16mb: usize,
    pub tier_32mb: usize,
    pub tier_64mb: usize,
    pub tier_128mb: usize,
}
