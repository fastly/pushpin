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
use std::net::IpAddr;

const DEFAULT_HTTP_MAX_HEADERS_SIZE: u32 = 10000;
const DEFAULT_HTTP_MAX_BODY_SIZE: u32 = 1000000;

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
        prometheus_port: String,
        prometheus_prefix: String,
    }

    unsafe extern "C++" {
        include!("pushpin/src/handler/handlerinit.h");

        type HandlerCliArgs;

        unsafe fn handler_init(args: *const HandlerCliArgs, settings: &HandlerSettings) -> i32;
    }
}

pub struct HandlerSettings {
    pub common: CommonSettings,
    pub server_in_stream_specs: Vec<String>,
    pub server_out_specs: Vec<String>,
    pub client_out_specs: Vec<String>,
    pub client_out_stream_specs: Vec<String>,
    pub client_in_specs: Vec<String>,
    pub inspect_specs: Vec<String>,
    pub accept_specs: Vec<String>,
    pub retry_out_specs: Vec<String>,
    pub ws_control_init_specs: Vec<String>,
    pub ws_control_stream_specs: Vec<String>,
    pub stats_spec: Option<String>,
    pub command_spec: Option<String>,
    pub state_spec: Option<String>,
    pub proxy_stats_specs: Vec<String>,
    pub proxy_command_spec: Option<String>,
    pub push_in_spec: Option<String>,
    pub push_in_sub_specs: Vec<String>,
    pub push_in_sub_connect: bool,
    pub push_in_http_addr: Option<IpAddr>,
    pub push_in_http_port: Option<u16>,
    pub push_in_http_max_headers_size: u32,
    pub push_in_http_max_body_size: u32,
    pub ipc_file_mode: Option<u32>,
    pub share_all: bool,
    pub message_rate: Option<u32>,
    pub message_hwm: Option<u32>,
    pub message_block_size: Option<u32>,
    pub message_wait: u32,
    pub id_cache_ttl: u32,
    pub update_on_first_subscription: bool,
    pub connection_subscription_max: u32,
    pub subscription_linger: u32,
    pub stats_subscription_ttl: u32,
    pub stats_report_interval: u32,
    pub stats_format: Option<String>,
    pub prometheus_port: Option<String>,
    pub prometheus_prefix: Option<String>,
}

impl HandlerSettings {
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
        let push_in_http_port = settings.adjust_port("handler/push_in_http_port")?;

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
            stats_spec: settings.string("handler/stats_spec")?,
            command_spec: settings.string("handler/command_spec")?,
            state_spec: settings.string("handler/state_spec")?,
            proxy_stats_specs: expand_specs(
                combined_specs(
                    settings,
                    "handler/proxy_stats_specs",
                    "handler/proxy_stats_spec",
                )?,
                workers,
            ),
            proxy_command_spec: settings.string("handler/proxy_command_spec")?.map(|spec| {
                if spec.starts_with("ipc:") && workers > 1 {
                    format!("{spec}-0")
                } else {
                    spec
                }
            }),
            push_in_spec: settings.string("handler/push_in_spec")?,
            push_in_sub_specs: combined_specs(
                settings,
                "handler/push_in_sub_specs",
                "handler/push_in_sub_spec",
            )?,
            push_in_sub_connect: settings
                .bool("handler/push_in_sub_connect")?
                .unwrap_or(false),
            push_in_http_addr,
            push_in_http_port,
            push_in_http_max_headers_size: settings
                .number("handler/push_in_http_max_headers_size")?
                .unwrap_or(DEFAULT_HTTP_MAX_HEADERS_SIZE),
            push_in_http_max_body_size: settings
                .number("handler/push_in_http_max_body_size")?
                .unwrap_or(DEFAULT_HTTP_MAX_BODY_SIZE),
            ipc_file_mode: settings.octal("handler/ipc_file_mode")?,
            share_all: settings.bool("handler/share_all")?.unwrap_or(false),
            message_rate: settings.number("handler/message_rate")?,
            message_hwm: settings.number("handler/message_hwm")?,
            message_block_size: settings.number("handler/message_block_size")?,
            message_wait: settings.number("handler/message_wait")?.unwrap_or(5000),
            id_cache_ttl: settings.number("handler/id_cache_ttl")?.unwrap_or(0),
            update_on_first_subscription: settings
                .bool("handler/update_on_first_subscription")?
                .unwrap_or(true),
            connection_subscription_max: settings
                .number("handler/connection_subscription_max")?
                .unwrap_or(20),
            subscription_linger: settings
                .number("handler/subscription_linger")?
                .unwrap_or(60),
            stats_subscription_ttl: settings
                .number("handler/stats_subscription_ttl")?
                .unwrap_or(60),
            stats_report_interval: settings
                .number("handler/stats_report_interval")?
                .unwrap_or(10),
            stats_format: settings.string("handler/stats_format")?,
            prometheus_port: settings.string("handler/prometheus_port")?,
            prometheus_prefix: settings.string("handler/prometheus_prefix")?,
            common,
        })
    }
}

impl HandlerSettings {
    pub fn to_ffi(&self) -> ffi::HandlerSettings {
        let int = |x: u32| i32::try_from(x).unwrap_or(i32::MAX);
        let opt = |x: Option<u32>| x.map_or(-1, int);
        let string = |s: &Option<String>| s.clone().unwrap_or_default();

        ffi::HandlerSettings {
            server_in_stream_specs: self.server_in_stream_specs.clone(),
            server_out_specs: self.server_out_specs.clone(),
            client_out_specs: self.client_out_specs.clone(),
            client_out_stream_specs: self.client_out_stream_specs.clone(),
            client_in_specs: self.client_in_specs.clone(),
            inspect_specs: self.inspect_specs.clone(),
            accept_specs: self.accept_specs.clone(),
            retry_out_specs: self.retry_out_specs.clone(),
            ws_control_init_specs: self.ws_control_init_specs.clone(),
            ws_control_stream_specs: self.ws_control_stream_specs.clone(),
            stats_spec: string(&self.stats_spec),
            command_spec: string(&self.command_spec),
            state_spec: string(&self.state_spec),
            proxy_stats_specs: self.proxy_stats_specs.clone(),
            proxy_command_spec: string(&self.proxy_command_spec),
            push_in_spec: string(&self.push_in_spec),
            push_in_sub_specs: self.push_in_sub_specs.clone(),
            push_in_sub_connect: self.push_in_sub_connect,
            push_in_http_addr: self
                .push_in_http_addr
                .map_or_else(String::new, |a| a.to_string()),
            push_in_http_port: self.push_in_http_port.map_or(-1, i32::from),
            push_in_http_max_headers_size: int(self.push_in_http_max_headers_size),
            push_in_http_max_body_size: int(self.push_in_http_max_body_size),
            ipc_file_mode: opt(self.ipc_file_mode),
            share_all: self.share_all,
            message_rate: opt(self.message_rate),
            message_hwm: opt(self.message_hwm),
            message_block_size: opt(self.message_block_size),
            message_wait: int(self.message_wait),
            id_cache_ttl: int(self.id_cache_ttl),
            update_on_first_subscription: self.update_on_first_subscription,
            connections_max: int(self.common.client_maxconn),
            connection_subscription_max: int(self.connection_subscription_max),
            subscription_linger: int(self.subscription_linger),
            stats_connection_send: self.common.stats_connection_send,
            stats_connection_ttl: int(self.common.stats_connection_ttl),
            stats_subscription_ttl: int(self.stats_subscription_ttl),
            stats_report_interval: int(self.stats_report_interval),
            stats_format: string(&self.stats_format),
            prometheus_port: string(&self.prometheus_port),
            prometheus_prefix: string(&self.prometheus_prefix),
        }
    }
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
    use std::net::Ipv4Addr;
    use std::path::Path;

    fn load_conf(test: &str, contents: &str) -> Result<HandlerSettings, SettingsError> {
        let dir = test_dir().join("handler-settings").join(test);
        fs::create_dir_all(&dir).unwrap();

        let path = dir.join("pushpin.conf");
        fs::write(&path, contents).unwrap();

        HandlerSettings::from_settings(&Settings::load(&path)?)
    }

    const GLOBAL: &str = "[global]\ninclude=internal.conf\nrundir=run\n";

    #[test]
    fn example_config() {
        let settings = Settings::load(Path::new("examples/config/pushpin.conf")).unwrap();
        let s = HandlerSettings::from_settings(&settings).unwrap();

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
        assert_eq!(
            s.proxy_command_spec.as_deref(),
            Some("ipc://run/pushpin-proxy-command")
        );
        assert_eq!(s.stats_spec.as_deref(), Some("ipc://run/pushpin-stats"));
        assert_eq!(s.command_spec.as_deref(), Some("tcp://127.0.0.1:5563"));
        assert_eq!(s.state_spec, None);
        assert_eq!(s.push_in_spec.as_deref(), Some("tcp://127.0.0.1:5560"));
        assert_eq!(s.push_in_sub_specs, vec!["tcp://127.0.0.1:5562"]);
        assert!(!s.push_in_sub_connect);
        assert_eq!(
            s.push_in_http_addr,
            Some(IpAddr::V4(Ipv4Addr::new(127, 0, 0, 1)))
        );
        assert_eq!(s.push_in_http_port, Some(5561));
        assert_eq!(s.ipc_file_mode, None);
        assert_eq!(s.message_rate, Some(2500));
        assert_eq!(s.message_hwm, Some(25000));
        assert_eq!(s.message_block_size, None);
        assert_eq!(s.id_cache_ttl, 60);
        assert_eq!(s.stats_format.as_deref(), Some("tnetstring"));
        assert_eq!(s.common.services, vec!["connmgr", "proxy", "handler"]);
        assert_eq!(s.common.client_maxconn, 50000);
        assert_eq!(s.common.stats_connection_ttl, 120);
    }

    #[test]
    fn defaults_when_unset() {
        let s = load_conf("defaults", GLOBAL).unwrap();

        assert_eq!(s.common.proxy_workers, 1);
        assert_eq!(s.common.client_maxconn, 50000);
        assert!(s.common.stats_connection_send);
        assert_eq!(s.common.stats_connection_ttl, 120);
        assert_eq!(s.push_in_http_port, None);
        assert_eq!(s.push_in_http_max_headers_size, 10000);
        assert_eq!(s.push_in_http_max_body_size, 1000000);
        assert_eq!(s.message_rate, None);
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
        assert_eq!(
            s.proxy_command_spec.as_deref(),
            Some("ipc://run/pushpin-proxy-command-0")
        );
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

        assert_eq!(s.push_in_http_addr, None);
        assert_eq!(s.push_in_http_port, None);
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

        assert_eq!(s.common.client_maxconn, 100);
        assert!(!s.common.stats_connection_send);
        assert_eq!(s.common.stats_connection_ttl, 30);
    }

    #[test]
    fn to_ffi_unset_values() {
        let f = load_conf("ffi_unset", GLOBAL).unwrap().to_ffi();

        assert_eq!(f.push_in_http_addr, "");
        assert_eq!(f.push_in_http_port, -1);
        assert_eq!(f.ipc_file_mode, -1);
        assert_eq!(f.message_rate, -1);
        assert_eq!(f.message_hwm, -1);
        assert_eq!(f.message_block_size, -1);
        assert_eq!(f.state_spec, "");
        assert_eq!(f.stats_format, "");
    }

    #[test]
    fn to_ffi_set_values() {
        let f = load_conf(
            "ffi_set",
            &format!(
                "{GLOBAL}stats_connection_send=false\nstats_connection_ttl=30\n[runner]\nclient_maxconn=100\n[handler]\npush_in_http_addr=127.0.0.1\npush_in_http_port=5561\nipc_file_mode=770\nmessage_rate=2500\nmessage_wait=3000000000\nstats_format=json\n"
            ),
        )
        .unwrap()
        .to_ffi();

        assert_eq!(f.push_in_http_addr, "127.0.0.1");
        assert_eq!(f.push_in_http_port, 5561);
        assert_eq!(f.ipc_file_mode, 0o770);
        assert_eq!(f.message_rate, 2500);
        assert_eq!(f.message_wait, i32::MAX);
        assert_eq!(f.stats_format, "json");
        assert_eq!(f.connections_max, 100);
        assert!(!f.stats_connection_send);
        assert_eq!(f.stats_connection_ttl, 30);
        assert_eq!(f.inspect_specs, vec!["ipc://run/pushpin-inspect"]);
    }

    #[test]
    fn ipc_file_mode_octal() {
        let s = load_conf("mode", &format!("{GLOBAL}[handler]\nipc_file_mode=777\n")).unwrap();
        assert_eq!(s.ipc_file_mode, Some(0o777));
    }
}
