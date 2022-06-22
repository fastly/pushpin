pub use crate::stats_emitter::xqd_config::{RawAggregatorConfig, RawMessageSenderMode};
use crate::stats_emitter::{
    data_types::{DataCenter, Emitter, SchemaName, Server},
    errors::ConfigError,
};
use hyper::Uri;
use std::convert::{TryFrom, TryInto};
use std::{path::PathBuf, sync::Arc};

impl TryFrom<RawAggregatorConfig> for AggregatorConfig {
    type Error = ConfigError;
    fn try_from(opts: RawAggregatorConfig) -> Result<Self, Self::Error> {
        Ok(Self {
            queue_size: opts.queue_size,
            schema_name: Arc::new(SchemaName::new(opts.schema_name)),
            emitter: Arc::new(Emitter::new(
                opts.emitter.ok_or(ConfigError::MissingField("emitter"))?,
            )),
            server: Arc::new(Server::new(
                opts.server.ok_or(ConfigError::MissingField("server"))?,
            )),
            datacenter: Arc::new(DataCenter::new(
                opts.datacenter
                    .ok_or(ConfigError::MissingField("datacenter"))?,
            )),
            mode: match opts.mode {
                RawMessageSenderMode::Json { url, mtls } => MessageSenderMode::Json {
                    url: url.try_into()?,
                    mtls: mtls
                        .map(|m| {
                            Ok(MutualTlsConfig {
                                cert_path: {
                                    if m.cert_path.exists() {
                                        m.cert_path
                                    } else {
                                        return Err(ConfigError::InvalidCertPath {
                                            path: m.cert_path,
                                            cert_type: "client cert",
                                        });
                                    }
                                },
                                key_path: {
                                    if m.key_path.exists() {
                                        m.key_path
                                    } else {
                                        return Err(ConfigError::InvalidCertPath {
                                            path: m.key_path,
                                            cert_type: "client key",
                                        });
                                    }
                                },
                                ca_path: {
                                    if m.ca_path.exists() {
                                        m.ca_path
                                    } else {
                                        return Err(ConfigError::InvalidCertPath {
                                            path: m.ca_path,
                                            cert_type: "certificate authority",
                                        });
                                    }
                                },
                                dangerous_no_peer_verification: m.dangerous_no_peer_verification,
                            })
                        })
                        .transpose()?,
                },
                RawMessageSenderMode::DumpFile { dump_file } => {
                    MessageSenderMode::DumpFile { dump_file }
                }
            },
        })
    }
}

/// Options for what url to send messages to and for what pipeline
#[derive(Debug, Clone)]
pub(crate) struct AggregatorConfig {
    pub(crate) schema_name: Arc<SchemaName>,
    pub(crate) emitter: Arc<Emitter>,
    pub(crate) datacenter: Arc<DataCenter>,
    pub(crate) server: Arc<Server>,
    pub(crate) mode: MessageSenderMode,
    pub(crate) queue_size: usize,
}

#[derive(Debug, Clone)]
pub(crate) struct MutualTlsConfig {
    pub(crate) cert_path: PathBuf,
    pub(crate) key_path: PathBuf,
    pub(crate) ca_path: PathBuf,
    pub(crate) dangerous_no_peer_verification: bool,
}

#[derive(Debug, Clone)]
/// The mode the message sender will work in. If set to `Json` it will send the
/// messages to a url over http or if `mtls` is set then via https. This is the
/// mode we want to use in production. If set to `DumpFile` then messages will
/// be written to the given path on disk. This is primarily used for integration
/// testing, but could be used locally for debugging.
pub(crate) enum MessageSenderMode {
    Json {
        url: Uri,
        mtls: Option<MutualTlsConfig>,
    },
    DumpFile {
        dump_file: PathBuf,
    },
}
