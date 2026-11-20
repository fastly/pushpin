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
        server_in_stream_specs: Vec<String>,
        server_out_specs: Vec<String>,
        client_out_specs: Vec<String>,
        client_out_stream_specs: Vec<String>,
        client_in_specs: Vec<String>,
        inspect_specs: Vec<String>,
        accept_specs: Vec<String>,
        retry_out_specs: Vec<String>,
        ws_control_init_specs: Vec<String>,
        ws_control_stream_specs: Vec<String>,
        stats_spec: String,
        command_spec: String,
        state_spec: String,
        proxy_stats_specs: Vec<String>,
        proxy_command_spec: String,
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
        let workers = common.proxy_workers;
        let has_service = |name: &str| common.services.iter().any(|s| s == name);

        let mut connmgr_in_stream_specs = settings.list("proxy/connmgr_in_stream_specs")?;
        connmgr_in_stream_specs.extend(settings.list("proxy/condure_in_stream_specs")?);

        let mut connmgr_out_specs = settings.list("proxy/connmgr_out_specs")?;
        connmgr_out_specs.extend(settings.list("proxy/condure_out_specs")?);

        let m2a_in_stream_specs = settings.list("handler/m2a_in_stream_specs")?;
        let m2a_out_specs = settings.list("handler/m2a_out_specs")?;

        if m2a_in_stream_specs.is_empty() || m2a_out_specs.is_empty() {
            return Err(SettingsError::Missing(
                "m2a_in_stream_specs and m2a_out_specs",
            ));
        }

        let inspect_specs = combined_specs(
            settings,
            "handler/proxy_inspect_specs",
            "handler/proxy_inspect_spec",
        )?;
        let accept_specs = combined_specs(
            settings,
            "handler/proxy_accept_specs",
            "handler/proxy_accept_spec",
        )?;
        let retry_out_specs = combined_specs(
            settings,
            "handler/proxy_retry_out_specs",
            "handler/proxy_retry_out_spec",
        )?;

        if inspect_specs.is_empty() || accept_specs.is_empty() || retry_out_specs.is_empty() {
            return Err(SettingsError::Missing(
                "proxy_inspect_specs, proxy_accept_specs, and proxy_retry_out_specs",
            ));
        }

        let (server_in_stream_specs, server_out_specs) = if !has_service("mongrel2")
            && (!connmgr_in_stream_specs.is_empty() || !connmgr_out_specs.is_empty())
        {
            (connmgr_in_stream_specs, connmgr_out_specs)
        } else {
            (m2a_in_stream_specs, m2a_out_specs)
        };

        let push_in_http_addr = settings.ip_addr("handler/push_in_http_addr")?;
        let push_in_http_port = settings.port("handler/push_in_http_port")?;

        // Only serve the API here if the api component is not enabled
        let (push_in_http_addr, push_in_http_port) = if has_service("api") {
            (None, None)
        } else {
            (push_in_http_addr, push_in_http_port)
        };

        Ok(Self {
            server_in_stream_specs,
            server_out_specs,
            client_out_specs: expand_specs(
                settings.list("handler/proxy_intreq_out_specs")?,
                workers,
            ),
            client_out_stream_specs: expand_specs(
                settings.list("handler/proxy_intreq_out_stream_specs")?,
                workers,
            ),
            client_in_specs: expand_specs(settings.list("handler/proxy_intreq_in_specs")?, workers),
            inspect_specs: expand_specs(inspect_specs, workers),
            accept_specs: expand_specs(accept_specs, workers),
            retry_out_specs: expand_specs(retry_out_specs, workers),
            ws_control_init_specs: expand_specs(
                settings.list("handler/proxy_ws_control_init_specs")?,
                workers,
            ),
            ws_control_stream_specs: expand_specs(
                settings.list("handler/proxy_ws_control_stream_specs")?,
                workers,
            ),
            stats_spec: string(settings, "handler/stats_spec")?,
            command_spec: string(settings, "handler/command_spec")?,
            state_spec: string(settings, "handler/state_spec")?,
            proxy_stats_specs: expand_specs(
                combined_specs(
                    settings,
                    "handler/proxy_stats_specs",
                    "handler/proxy_stats_spec",
                )?,
                workers,
            ),
            proxy_command_spec: settings
                .string("handler/proxy_command_spec")?
                .map(|spec| {
                    if spec.starts_with("ipc:") && workers > 1 {
                        format!("{spec}-0")
                    } else {
                        spec
                    }
                })
                .unwrap_or_default(),
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
            debug_socket: string(settings, "handler/debug_socket")?,
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
    specs.extend(settings.string(single_key)?);

    Ok(specs)
}

fn expand_specs(specs: Vec<String>, workers: u32) -> Vec<String> {
    match specs.as_slice() {
        [base] if base.starts_with("ipc:") && workers > 1 => {
            (0..workers).map(|i| format!("{base}-{i}")).collect()
        }
        _ => specs,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::test_dir;
    use std::fs;
    use std::path::Path;

    fn load_conf(test: &str, contents: &str) -> Result<ffi::HandlerSettings, SettingsError> {
        let dir = test_dir().join("handler-settings").join(test);
        fs::create_dir_all(&dir).unwrap();

        let path = dir.join("pushpin.conf");
        fs::write(&path, contents).unwrap();

        ffi::HandlerSettings::from_settings(&Settings::load(&path)?)
    }

    const GLOBAL: &str = "[global]\ninclude=internal.conf\nrundir=run\n";

    #[test]
    fn example_config() {
        let settings = Settings::load(Path::new("examples/config/pushpin.conf")).unwrap();
        let s = ffi::HandlerSettings::from_settings(&settings).unwrap();

        assert_eq!(
            s.server_in_stream_specs,
            vec!["ipc://run/pushpin-connmgr-out-stream"]
        );
        assert_eq!(s.server_out_specs, vec!["ipc://run/pushpin-connmgr-in"]);
        assert_eq!(s.client_out_specs, vec!["ipc://run/pushpin-intreq-in"]);
        assert_eq!(s.inspect_specs, vec!["ipc://run/pushpin-inspect"]);
        assert_eq!(s.accept_specs, vec!["ipc://run/pushpin-accept"]);
        assert_eq!(s.retry_out_specs, vec!["ipc://run/pushpin-retry"]);
        assert_eq!(s.proxy_stats_specs, vec!["ipc://run/pushpin-proxy-stats"]);
        assert_eq!(s.proxy_command_spec, "ipc://run/pushpin-proxy-command");
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
    fn defaults_when_unset() {
        let s = load_conf("defaults", GLOBAL).unwrap();

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
    fn specs_expanded_per_proxy_worker() {
        let s = load_conf("workers", &format!("{GLOBAL}[proxy]\nworkers=3\n")).unwrap();

        assert_eq!(
            s.inspect_specs,
            vec![
                "ipc://run/pushpin-inspect-0",
                "ipc://run/pushpin-inspect-1",
                "ipc://run/pushpin-inspect-2"
            ]
        );
        assert_eq!(s.client_out_specs.len(), 3);
        assert_eq!(s.ws_control_init_specs.len(), 3);
        assert_eq!(s.proxy_command_spec, "ipc://run/pushpin-proxy-command-0");
        assert_eq!(
            s.server_in_stream_specs,
            vec!["ipc://run/pushpin-connmgr-out-stream"]
        );
    }

    #[test]
    fn condure_specs_merged() {
        let s = load_conf(
            "condure",
            &format!("{GLOBAL}[proxy]\ncondure_in_stream_specs=ipc://run/condure-out-stream\n"),
        )
        .unwrap();

        assert_eq!(
            s.server_in_stream_specs,
            vec![
                "ipc://run/pushpin-connmgr-out-stream",
                "ipc://run/condure-out-stream"
            ]
        );
    }

    #[test]
    fn single_spec_appended() {
        let s = load_conf(
            "single_spec",
            &format!("{GLOBAL}[handler]\nproxy_inspect_spec=ipc://run/extra-inspect\n"),
        )
        .unwrap();

        assert_eq!(
            s.inspect_specs,
            vec!["ipc://run/pushpin-inspect", "ipc://run/extra-inspect"]
        );
    }

    #[test]
    fn mongrel2_uses_m2a_specs() {
        let s = load_conf(
            "mongrel2",
            &format!("{GLOBAL}[runner]\nservices=mongrel2,m2adapter,proxy,handler\n"),
        )
        .unwrap();

        assert_eq!(
            s.server_in_stream_specs,
            vec![
                "ipc://run/pushpin-m2zhttp-out-stream",
                "ipc://run/pushpin-m2zws-out-stream"
            ]
        );
        assert_eq!(
            s.server_out_specs,
            vec!["ipc://run/pushpin-m2zhttp-in", "ipc://run/pushpin-m2zws-in"]
        );
    }

    #[test]
    fn api_service_disables_push_http() {
        let s = load_conf(
            "api",
            &format!(
                "{GLOBAL}[runner]\nservices=connmgr,proxy,handler,api\n[handler]\npush_in_http_addr=127.0.0.1\npush_in_http_port=5561\n"
            ),
        )
        .unwrap();

        assert_eq!(s.push_in_http_addr, "");
        assert_eq!(s.push_in_http_port, -1);
    }

    #[test]
    fn missing_m2a_specs_errors() {
        let r = load_conf("missing_m2a", "[global]\nrundir=run\n");
        assert!(
            matches!(r, Err(SettingsError::Missing(m)) if m.starts_with("m2a_in_stream_specs"))
        );
    }

    #[test]
    fn missing_proxy_specs_errors() {
        let r = load_conf(
            "missing_proxy",
            "[global]\nrundir=run\n[handler]\nm2a_in_stream_specs=ipc://run/a\nm2a_out_specs=ipc://run/b\n",
        );
        assert!(
            matches!(r, Err(SettingsError::Missing(m)) if m.starts_with("proxy_inspect_specs"))
        );
    }

    #[test]
    fn invalid_push_in_http_addr_errors() {
        let r = load_conf(
            "bad_addr",
            &format!("{GLOBAL}[handler]\npush_in_http_addr=127.0.0.l\n"),
        );
        assert!(matches!(
            r,
            Err(SettingsError::InvalidValue { key, .. }) if key == "handler/push_in_http_addr"
        ));
    }

    #[test]
    fn invalid_ipc_file_mode_errors() {
        let r = load_conf(
            "bad_mode",
            &format!("{GLOBAL}[handler]\nipc_file_mode=abc\n"),
        );
        assert!(matches!(
            r,
            Err(SettingsError::InvalidValue { key, .. }) if key == "handler/ipc_file_mode"
        ));
    }

    #[test]
    fn common_settings_read_from_config() {
        let s = load_conf(
            "common",
            &format!(
                "{GLOBAL}stats_connection_send=false\nstats_connection_ttl=30\n[runner]\nclient_maxconn=100\n"
            ),
        )
        .unwrap();

        assert_eq!(s.connections_max, 100);
        assert!(!s.stats_connection_send);
        assert_eq!(s.stats_connection_ttl, 30);
    }

    #[test]
    fn to_ffi_unset_values() {
        let f = load_conf("ffi_unset", GLOBAL).unwrap();

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
    fn to_ffi_set_values() {
        let f = load_conf(
            "ffi_set",
            &format!(
                "{GLOBAL}stats_connection_send=false\nstats_connection_ttl=30\n[runner]\nclient_maxconn=100\n[handler]\npush_in_http_addr=127.0.0.1\npush_in_http_port=5561\nipc_file_mode=770\nmessage_rate=2500\nstats_format=json\ndebug_socket=run/pushpin-handler-debug\n"
            ),
        )
        .unwrap();

        assert_eq!(f.push_in_http_addr, "127.0.0.1");
        assert_eq!(f.push_in_http_port, 5561);
        assert_eq!(f.ipc_file_mode, 0o770);
        assert_eq!(f.message_rate, 2500);
        assert_eq!(f.stats_format, "json");
        assert_eq!(f.debug_socket, "run/pushpin-handler-debug");
        assert_eq!(f.connections_max, 100);
        assert!(!f.stats_connection_send);
        assert_eq!(f.stats_connection_ttl, 30);
        assert_eq!(f.inspect_specs, vec!["ipc://run/pushpin-inspect"]);
    }

    #[test]
    fn number_too_big_for_int_errors() {
        let r = load_conf(
            "too_big",
            &format!("{GLOBAL}[handler]\nmessage_wait=3000000000\n"),
        );
        assert_eq!(
            r.err().unwrap().to_string(),
            "handler/message_wait must be at most 2147483647: 3000000000"
        );
    }

    #[test]
    fn ipc_file_mode_octal() {
        let s = load_conf("mode", &format!("{GLOBAL}[handler]\nipc_file_mode=777\n")).unwrap();
        assert_eq!(s.ipc_file_mode, 0o777);
    }
}
