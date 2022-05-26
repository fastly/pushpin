//! Geolocation configuration options for ExecuteD.

use serde::{Deserialize, Serialize};
use std::path::PathBuf;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum RawGeolocationConfig {
    Notary(RawNotaryConfig),
    Varnish,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct RawNotaryConfig {
    pub uds_path: PathBuf,
    pub lookup_timeout_ms: u64,
    pub varnish_fallback: bool,
}
