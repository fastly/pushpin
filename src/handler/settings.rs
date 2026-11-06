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
use std::net::IpAddr;

const DEFAULT_HTTP_MAX_HEADERS_SIZE: u32 = 10000;
const DEFAULT_HTTP_MAX_BODY_SIZE: u32 = 1000000;

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
    fn ipc_file_mode_octal() {
        let s = load_conf("mode", &format!("{GLOBAL}[handler]\nipc_file_mode=777\n")).unwrap();
        assert_eq!(s.ipc_file_mode, Some(0o777));
    }
}
