//! ExecuteD configuration parsing.
//!
//! This crate defines types that can be used with [serde] to parse the `xqd.toml.in` TOML data
//! packaged with ExecuteD. Use [`RawConfig::from_string`] to deserialize the file.'
//!
//! **N.B.** This crate does not validate the configuration settings themselves, beyond ensuring
//! that the file contained valid TOML syntax that provides values for the required fields.
//!
//! [serde]: https://docs.rs/serde/latest

mod config_version;
mod downstream;
mod global;
mod upstream;

use anyhow::{bail, Error};
pub use config_version::*;
pub use downstream::*;
pub use global::*;
use serde::Deserialize;
pub use upstream::*;

#[derive(Clone, Debug, Deserialize)]
pub struct RawConfig {
    pub global: RawGlobalConfig,
    pub downstream: RawDownstreamConfig,
    pub upstream: RawUpstreamConfig,
    pub config_version: Option<RawConfigVersion>,
}

impl RawConfig {
    pub fn from_string(config: impl AsRef<str>) -> Result<RawConfig, Error> {
        let config_str = config.as_ref();
        let config: RawConfig = match toml::from_str(config_str) {
            Ok(config) => config,
            Err(toml_err) => {
                match serde_json::from_str(config_str) {
                    Ok(config) => config,
                    Err(json_err) => {
                        // for xqd, a json config document will always be an object, so the first
                        // character will be `{`. if it's not `{`, the only other possible
                        // interpretation of this document is as a toml file, which could start
                        // with `#`, `[`, an ascii character, quote, .. -- it's much easier to test
                        // "not json" than "is toml".
                        if config_str.trim().starts_with("{") {
                            bail!("config parsing error (json): {}", json_err);
                        } else {
                            bail!("config parsing error (toml): {}", toml_err);
                        };
                    }
                }
            }
        };
        if config.downstream.listen.is_empty() {
            bail!("Config error: no socket to listen to");
        }
        Ok(config)
    }

    pub fn with_instance(&mut self, instance: &str) -> &mut Self {
        match instance.parse::<u64>() {
            Ok(instance) => {
                self.global.instance = Some(instance);
                self
            }
            Err(e) => panic!(
                "RawConfig: Invalid `instance` setting \"{}\": {:?}",
                instance, e
            ),
        }
    }
}
