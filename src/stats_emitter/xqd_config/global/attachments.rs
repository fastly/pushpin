//! Attachment configuration options for ExecuteD.

use serde::{Deserialize, Serialize};
use std::path::PathBuf;

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct RawAttachmentsConfig {
    pub customer_secrets: RawCustomerSecretsConfig,
    pub dictionaries: RawDictionaryConfig,
    pub object_stores: RawObjectStoresConfig,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct RawCustomerSecretsConfig {
    pub enabled: bool,
    pub path: PathBuf,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct RawDictionaryConfig {
    pub enabled: bool,
    pub path: PathBuf,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct RawObjectStoresConfig {
    pub enabled: bool,
    pub path: PathBuf,
}
