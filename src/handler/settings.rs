/*
 * Copyright (C) 2026 Fastly, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

use crate::core::settings::{CommonSettings, Settings, SettingsError};
use std::convert::TryFrom;

const DEFAULT_HTTP_MAX_HEADERS_SIZE: i32 = 10000;
const DEFAULT_HTTP_MAX_BODY_SIZE: i32 = 1000000;
const UNSET: i32 = -1;

/// cbindgen:ignore
#[allow(clippy::missing_safety_doc)]
#[cxx::bridge(namespace = "ffi")]
pub mod ffi {
    // -1 or empty means unset
    struct HandlerSettings {
        stats_spec: String,
        command_spec: String,
        state_spec: String,
        push_in_spec: String,
        push_in_sub_specs: Vec<String>,
        push_in_sub_connect: bool,
        push_in_http_addr: String,
        push_in_http_port: i32,
        push_in_http_max_headers_size: i32,
        push_in_http_max_body_size: i32,
        ipc_file_mode: i32,
        share_all: bool,
        message_rate: i32,
        message_hwm: i32,
        message_block_size: i32,
        message_wait: i32,
        id_cache_ttl: i32,
        update_on_first_subscription: bool,
        connections_max: i32,
        connection_subscription_max: i32,
        subscription_linger: i32,
        stats_connection_send: bool,
        stats_connection_ttl: i32,
        stats_subscription_ttl: i32,
        stats_report_interval: i32,
        stats_format: String,
        debug_socket: String,
        prometheus_port: String,
        prometheus_prefix: String,
    }

    unsafe extern "C++" {
        include!("pushpin/src/handler/handlerinit.h");

        type HandlerCliArgs;

        unsafe fn handler_init(args: *const HandlerCliArgs, settings: &HandlerSettings) -> i32;
    }
}

impl ffi::HandlerSettings {
    pub fn from_settings(settings: &Settings) -> Result<Self, SettingsError> {
        let common = CommonSettings::from_settings(settings)?;
        let has_service = |name: &str| common.services.iter().any(|s| s == name);

        let push_in_http_addr = settings.ip_addr("handler/push_in_http_addr")?;
        let push_in_http_port = settings.port("handler/push_in_http_port")?;

        // Only serve the API here if the api component is not enabled
        let (push_in_http_addr, push_in_http_port) = if has_service("api") {
            (None, None)
        } else {
            (push_in_http_addr, push_in_http_port)
        };

        Ok(Self {
            stats_spec: string(settings, "handler/stats_spec")?,
            command_spec: string(settings, "handler/command_spec")?,
            state_spec: string(settings, "handler/state_spec")?,
            push_in_spec: string(settings, "handler/push_in_spec")?,
            push_in_sub_specs: combined_specs(
                settings,
                "handler/push_in_sub_specs",
                "handler/push_in_sub_spec",
            )?,
            push_in_sub_connect: settings
                .bool("handler/push_in_sub_connect")?
                .unwrap_or(false),
            push_in_http_addr: push_in_http_addr.map_or_else(String::new, |a| a.to_string()),
            push_in_http_port: push_in_http_port.map_or(UNSET, i32::from),
            push_in_http_max_headers_size: int_or(
                settings,
                "handler/push_in_http_max_headers_size",
                DEFAULT_HTTP_MAX_HEADERS_SIZE,
            )?,
            push_in_http_max_body_size: int_or(
                settings,
                "handler/push_in_http_max_body_size",
                DEFAULT_HTTP_MAX_BODY_SIZE,
            )?,
            ipc_file_mode: settings
                .octal("handler/ipc_file_mode")?
                .map_or(Ok(UNSET), |mode| int("handler/ipc_file_mode", mode))?,
            share_all: settings.bool("handler/share_all")?.unwrap_or(false),
            message_rate: int_or(settings, "handler/message_rate", UNSET)?,
            message_hwm: int_or(settings, "handler/message_hwm", UNSET)?,
            message_block_size: int_or(settings, "handler/message_block_size", UNSET)?,
            message_wait: int_or(settings, "handler/message_wait", 5000)?,
            id_cache_ttl: int_or(settings, "handler/id_cache_ttl", 0)?,
            update_on_first_subscription: settings
                .bool("handler/update_on_first_subscription")?
                .unwrap_or(true),
            connections_max: int("runner/client_maxconn", common.client_maxconn)?,
            connection_subscription_max: int_or(
                settings,
                "handler/connection_subscription_max",
                20,
            )?,
            subscription_linger: int_or(settings, "handler/subscription_linger", 60)?,
            stats_connection_send: common.stats_connection_send,
            stats_connection_ttl: int("global/stats_connection_ttl", common.stats_connection_ttl)?,
            stats_subscription_ttl: int_or(settings, "handler/stats_subscription_ttl", 60)?,
            stats_report_interval: int_or(settings, "handler/stats_report_interval", 10)?,
            stats_format: string(settings, "handler/stats_format")?,
            // Read as a list and rejoined, like the C++, so params after commas are kept
            debug_socket: settings.list("handler/debug_socket")?.join(","),
            prometheus_port: string(settings, "handler/prometheus_port")?,
            prometheus_prefix: string(settings, "handler/prometheus_prefix")?,
        })
    }
}

/// Converts a number to C++'s int, erroring if it doesn't fit.
fn int(key: &str, value: u32) -> Result<i32, SettingsError> {
    i32::try_from(value).map_err(|_| SettingsError::InvalidValue {
        key: key.to_string(),
        value: value.to_string(),
        expected: "at most 2147483647",
    })
}

/// Reads the number at `key` as a C++ int, or returns `default` if it's unset.
fn int_or(settings: &Settings, key: &str, default: i32) -> Result<i32, SettingsError> {
    match settings.number(key)? {
        Some(value) => int(key, value),
        None => Ok(default),
    }
}

/// Reads the string at `key`, or an empty one if it's unset.
fn string(settings: &Settings, key: &str) -> Result<String, SettingsError> {
    Ok(settings.string(key)?.unwrap_or_default())
}

fn combined_specs(
    settings: &Settings,
    list_key: &str,
    single_key: &str,
) -> Result<Vec<String>, SettingsError> {
    let mut specs = settings.list(list_key)?;
    specs.extend(settings.string(single_key)?.filter(|s| !s.is_empty()));

    Ok(specs)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    mod helpers {
        use super::*;
        use crate::core::settings::parse;

        // Pulls in the shipped internal.conf, which has the socket specs the handler requires,
        // and puts the sockets under run/
        pub(super) const BASE_CONF: &str = "[global]\ninclude=internal.conf\nrundir=run\n";

        pub(super) fn load_conf(text: &str) -> Result<ffi::HandlerSettings, SettingsError> {
            ffi::HandlerSettings::from_settings(&parse(text)?)
        }
    }

    use self::helpers::*;

    #[test]
    fn example_config_loads() {
        let settings = Settings::load(Path::new("examples/config/pushpin.conf")).unwrap();
        let s = ffi::HandlerSettings::from_settings(&settings).unwrap();

        assert_eq!(s.stats_spec, "ipc://run/pushpin-stats");
        assert_eq!(s.command_spec, "tcp://127.0.0.1:5563");
        assert_eq!(s.state_spec, "");
        assert_eq!(s.push_in_spec, "tcp://127.0.0.1:5560");
        assert_eq!(s.push_in_sub_specs, vec!["tcp://127.0.0.1:5562"]);
        assert!(!s.push_in_sub_connect);
        assert_eq!(s.push_in_http_addr, "127.0.0.1");
        assert_eq!(s.push_in_http_port, 5561);
        assert_eq!(s.ipc_file_mode, -1);
        assert_eq!(s.message_rate, 2500);
        assert_eq!(s.message_hwm, 25000);
        assert_eq!(s.message_block_size, -1);
        assert_eq!(s.id_cache_ttl, 60);
        assert_eq!(s.stats_format, "tnetstring");
        assert_eq!(s.debug_socket, "");
        assert_eq!(s.connections_max, 50000);
        assert_eq!(s.stats_connection_ttl, 120);
    }

    #[test]
    fn invalid_config_fails() {
        let cases = [
            (
                format!("{BASE_CONF}[handler]\npush_in_http_addr=127.0.0.l\n"),
                "handler/push_in_http_addr must be an IP address: 127.0.0.l",
            ),
            (
                format!("{BASE_CONF}[handler]\nmessage_wait=3000000000\n"),
                "handler/message_wait must be at most 2147483647: 3000000000",
            ),
        ];

        for (conf, expected) in cases {
            let err = load_conf(&conf).err().map(|e| e.to_string());
            assert_eq!(err.as_deref(), Some(expected), "{}", conf);
        }
    }

    mod values {
        use super::helpers::*;

        #[test]
        fn defaults_when_unset() {
            let s = load_conf(BASE_CONF).unwrap();

            assert_eq!(s.connections_max, 50000);
            assert!(s.stats_connection_send);
            assert_eq!(s.stats_connection_ttl, 120);
            assert_eq!(s.push_in_http_port, -1);
            assert_eq!(s.push_in_http_max_headers_size, 10000);
            assert_eq!(s.push_in_http_max_body_size, 1000000);
            assert_eq!(s.message_rate, -1);
            assert_eq!(s.message_wait, 5000);
            assert_eq!(s.id_cache_ttl, 0);
            assert!(s.update_on_first_subscription);
            assert_eq!(s.connection_subscription_max, 20);
            assert_eq!(s.subscription_linger, 60);
            assert_eq!(s.stats_subscription_ttl, 60);
            assert_eq!(s.stats_report_interval, 10);
        }

        #[test]
        fn unset_is_minus_one_or_empty() {
            let f = load_conf(BASE_CONF).unwrap();

            assert_eq!(f.push_in_http_addr, "");
            assert_eq!(f.push_in_http_port, -1);
            assert_eq!(f.ipc_file_mode, -1);
            assert_eq!(f.message_rate, -1);
            assert_eq!(f.message_hwm, -1);
            assert_eq!(f.message_block_size, -1);
            assert_eq!(f.state_spec, "");
            assert_eq!(f.stats_format, "");
            assert_eq!(f.debug_socket, "");
        }

        #[test]
        fn read_as_written() {
            let f = load_conf(&format!(
                    "{BASE_CONF}stats_connection_send=false\nstats_connection_ttl=30\n[runner]\nclient_maxconn=100\n[handler]\npush_in_http_addr=127.0.0.1\npush_in_http_port=5561\nipc_file_mode=770\nmessage_rate=2500\nstats_format=json\ndebug_socket=run/pushpin-handler-debug,mode=777\n"
                ),
            )
            .unwrap();

            assert_eq!(f.push_in_http_addr, "127.0.0.1");
            assert_eq!(f.push_in_http_port, 5561);
            assert_eq!(f.ipc_file_mode, 0o770);
            assert_eq!(f.message_rate, 2500);
            assert_eq!(f.stats_format, "json");
            assert_eq!(f.debug_socket, "run/pushpin-handler-debug,mode=777");
            assert_eq!(f.connections_max, 100);
            assert!(!f.stats_connection_send);
            assert_eq!(f.stats_connection_ttl, 30);
        }
    }

    mod specs {
        use super::helpers::*;

        #[test]
        fn empty_single_spec_ignored() {
            let s = load_conf(&format!(
                "{BASE_CONF}[handler]\npush_in_sub_specs=tcp://127.0.0.1:5562\npush_in_sub_spec=\n"
            ))
            .unwrap();

            assert_eq!(s.push_in_sub_specs, vec!["tcp://127.0.0.1:5562"]);
        }
    }

    mod services {
        use super::helpers::*;

        #[test]
        fn api_disables_push_http() {
            let s = load_conf(&format!(
                    "{BASE_CONF}[runner]\nservices=connmgr,proxy,handler,api\n[handler]\npush_in_http_addr=127.0.0.1\npush_in_http_port=5561\n"
                ),
            )
            .unwrap();

            assert_eq!(s.push_in_http_addr, "");
            assert_eq!(s.push_in_http_port, -1);
        }
    }
}
