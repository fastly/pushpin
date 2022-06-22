use std::{io, path::PathBuf};
use thiserror::Error;

#[derive(Error, Debug)]
pub enum EmitterError {
    #[error("IO error involving {path}. error: {error}")]
    Io { error: io::Error, path: PathBuf },
    #[error("error deserializing JSON from {path}. error: {error}")]
    JsonDeserialize {
        error: serde_json::error::Error,
        path: PathBuf,
    },
    #[error("error validating configuration. error: {0}")]
    ConfigError(#[from] ConfigError),
}

#[derive(Error, Debug)]
#[error("error setting up message sender: {0}")]
pub struct MessageSenderError(#[from] openssl::error::ErrorStack);

#[derive(Error, Debug)]
#[error("the count must be greater than 0")]
pub struct ZeroCount;

#[derive(Error, Debug)]
pub enum ConfigError {
    #[error("missing field '{0}' in configuration")]
    MissingField(&'static str),
    #[error("path for {cert_type} does not exist '{path}' in configuration")]
    InvalidCertPath {
        path: PathBuf,
        cert_type: &'static str,
    },
    #[error("invalid url: '{0}'")]
    InvalidUrl(#[from] http::uri::InvalidUri),
}
