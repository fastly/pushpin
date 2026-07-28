/*
 * Copyright (C) 2023 Fastly, Inc.
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

use config::{Config, ConfigError};
use serde::Deserialize;
use std::collections::HashMap;
use std::env;
use std::error::Error;
use std::net::{IpAddr, Ipv4Addr};
use std::path::{Path, PathBuf};
use std::str::FromStr;

#[cfg(not(test))]
use config::File;

#[derive(Debug, Deserialize, Default)]
pub struct Global {
    pub include: String,
    pub rundir: String,
    pub libdir: String,
    pub ipc_prefix: String,
    pub port_offset: i32,
    pub stats_connection_ttl: i32,
    pub stats_connection_send: bool,
}

impl From<Global> for config::ValueKind {
    fn from(global: Global) -> Self {
        let mut properties = std::collections::HashMap::new();
        properties.insert("include".to_string(), config::Value::from(global.include));
        properties.insert("rundir".to_string(), config::Value::from(global.rundir));
        properties.insert("libdir".to_string(), config::Value::from(global.libdir));
        properties.insert(
            "ipc_prefix".to_string(),
            config::Value::from(global.ipc_prefix),
        );
        properties.insert(
            "port_offset".to_string(),
            config::Value::from(global.port_offset),
        );
        properties.insert(
            "stats_connection_ttl".to_string(),
            config::Value::from(global.stats_connection_ttl),
        );
        properties.insert(
            "stats_connection_send".to_string(),
            config::Value::from(global.stats_connection_send),
        );

        Self::Table(properties)
    }
}

#[derive(serde::Deserialize, Eq, PartialEq, Debug, Default)]
pub struct Runner {
    //runner rundir is deprecated
    pub rundir: String,
    pub services: String,
    pub http_port: String,
    pub https_ports: String,
    pub local_ports: String,
    pub logdir: String,
    pub log_level: String,
    pub client_buffer_size: i32,
    pub client_maxconn: i32,
    pub allow_compression: bool,
}

impl From<Runner> for config::ValueKind {
    fn from(runner: Runner) -> Self {
        let mut properties = std::collections::HashMap::new();
        properties.insert("rundir".to_string(), config::Value::from(runner.rundir));
        properties.insert("services".to_string(), config::Value::from(runner.services));
        properties.insert(
            "http_port".to_string(),
            config::Value::from(runner.http_port),
        );
        properties.insert(
            "https_ports".to_string(),
            config::Value::from(runner.https_ports),
        );
        properties.insert(
            "local_ports".to_string(),
            config::Value::from(runner.local_ports),
        );
        properties.insert("logdir".to_string(), config::Value::from(runner.logdir));
        properties.insert(
            "log_level".to_string(),
            config::Value::from(runner.log_level),
        );
        properties.insert("client_buffer_size".to_string(), config::Value::from(8192));
        properties.insert("client_maxconn".to_string(), config::Value::from(50000));
        properties.insert(
            "allow_compression".to_string(),
            config::Value::from(runner.allow_compression),
        );

        Self::Table(properties)
    }
}

#[derive(Debug, Deserialize, Default)]
pub struct Proxy {
    pub routesfile: String,
    pub debug: bool,
    pub auto_cross_origin: bool,
    pub accept_x_forwarded_protocol: bool,
    pub set_x_forwarded_protocol: String,
    pub x_forwarded_for: String,
    pub x_forwarded_for_trusted: String,
    pub orig_headers_need_mark: String,
    pub accept_pushpin_route: bool,
    pub cdn_loop: String,
    pub log_from: bool,
    pub log_user_agent: bool,
    pub sig_iss: String,
    pub sig_key: String,
    pub upstream_key: String,
    pub sockjs_url: String,
    pub updates_check: String,
    pub organization_name: String,
}

impl From<Proxy> for config::ValueKind {
    fn from(proxy: Proxy) -> Self {
        let mut properties = std::collections::HashMap::new();
        properties.insert(
            "routesfile".to_string(),
            config::Value::from(proxy.routesfile),
        );
        properties.insert("debug".to_string(), config::Value::from(proxy.debug));
        properties.insert(
            "auto_cross_origin".to_string(),
            config::Value::from(proxy.auto_cross_origin),
        );
        properties.insert(
            "accept_x_forwarded_protocol".to_string(),
            config::Value::from(proxy.accept_x_forwarded_protocol),
        );
        properties.insert(
            "set_x_forwarded_protocol".to_string(),
            config::Value::from(proxy.set_x_forwarded_protocol),
        );
        properties.insert(
            "x_forwarded_for".to_string(),
            config::Value::from(proxy.x_forwarded_for),
        );
        properties.insert(
            "x_forwarded_for_trusted".to_string(),
            config::Value::from(proxy.x_forwarded_for_trusted),
        );
        properties.insert(
            "orig_headers_need_mark".to_string(),
            config::Value::from(proxy.orig_headers_need_mark),
        );
        properties.insert(
            "accept_pushpin_route".to_string(),
            config::Value::from(proxy.accept_pushpin_route),
        );
        properties.insert("cdn_loop".to_string(), config::Value::from(proxy.cdn_loop));
        properties.insert("log_from".to_string(), config::Value::from(proxy.log_from));
        properties.insert(
            "log_user_agent".to_string(),
            config::Value::from(proxy.log_user_agent),
        );
        properties.insert("sig_iss".to_string(), config::Value::from(proxy.sig_iss));
        properties.insert("sig_key".to_string(), config::Value::from(proxy.sig_key));
        properties.insert(
            "upstream_key".to_string(),
            config::Value::from(proxy.upstream_key),
        );
        properties.insert(
            "sockjs_url".to_string(),
            config::Value::from(proxy.sockjs_url),
        );
        properties.insert(
            "updates_check".to_string(),
            config::Value::from(proxy.updates_check),
        );
        properties.insert(
            "organization_name".to_string(),
            config::Value::from(proxy.organization_name),
        );

        Self::Table(properties)
    }
}

#[derive(Debug, Deserialize, Default)]
pub struct Handler {
    pub ipc_file_mode: u16,
    pub push_in_spec: String,
    pub push_in_sub_specs: String,
    pub push_in_sub_connect: bool,
    pub push_in_http_addr: String,
    pub push_in_http_port: u16,
    pub push_in_http_max_headers_size: u32,
    pub push_in_http_max_body_size: u32,
    pub stats_spec: String,
    pub command_spec: String,
    pub message_rate: u32,
    pub message_hwm: u32,
    pub message_block_size: u32,
    pub message_wait: u32,
    pub id_cache_ttl: u32,
    pub connection_subscription_max: u32,
    pub subscription_linger: u32,
    pub stats_subscription_ttl: u32,
    pub stats_report_interval: u32,
    pub stats_format: String,
    pub prometheus_port: String,
    pub prometheus_prefix: String,
}

impl From<Handler> for config::ValueKind {
    fn from(handler: Handler) -> Self {
        let mut properties = std::collections::HashMap::new();
        properties.insert(
            "ipc_file_mode".to_string(),
            config::Value::from(handler.ipc_file_mode),
        );
        properties.insert(
            "push_in_spec".to_string(),
            config::Value::from(handler.push_in_spec),
        );
        properties.insert(
            "push_in_sub_specs".to_string(),
            config::Value::from(handler.push_in_sub_specs),
        );
        properties.insert(
            "push_in_sub_connect".to_string(),
            config::Value::from(handler.push_in_sub_connect),
        );
        properties.insert(
            "push_in_http_addr".to_string(),
            config::Value::from(handler.push_in_http_addr),
        );
        properties.insert(
            "push_in_http_port".to_string(),
            config::Value::from(handler.push_in_http_port),
        );
        properties.insert(
            "push_in_http_max_headers_size".to_string(),
            config::Value::from(handler.push_in_http_max_headers_size),
        );
        properties.insert(
            "push_in_http_max_body_size".to_string(),
            config::Value::from(handler.push_in_http_max_body_size),
        );
        properties.insert(
            "stats_spec".to_string(),
            config::Value::from(handler.stats_spec),
        );
        properties.insert(
            "command_spec".to_string(),
            config::Value::from(handler.command_spec),
        );
        properties.insert(
            "message_rate".to_string(),
            config::Value::from(handler.message_rate),
        );
        properties.insert(
            "message_hwm".to_string(),
            config::Value::from(handler.message_hwm),
        );
        properties.insert(
            "message_block_size".to_string(),
            config::Value::from(handler.message_block_size),
        );
        properties.insert(
            "message_wait".to_string(),
            config::Value::from(handler.message_wait),
        );
        properties.insert(
            "id_cache_ttl".to_string(),
            config::Value::from(handler.id_cache_ttl),
        );
        properties.insert(
            "connection_subscription_max".to_string(),
            config::Value::from(handler.connection_subscription_max),
        );
        properties.insert(
            "subscription_linger".to_string(),
            config::Value::from(handler.subscription_linger),
        );
        properties.insert(
            "stats_subscription_ttl".to_string(),
            config::Value::from(handler.stats_subscription_ttl),
        );
        properties.insert(
            "stats_report_interval".to_string(),
            config::Value::from(handler.stats_report_interval),
        );
        properties.insert(
            "stats_format".to_string(),
            config::Value::from(handler.stats_format),
        );
        properties.insert(
            "prometheus_port".to_string(),
            config::Value::from(handler.prometheus_port),
        );
        properties.insert(
            "prometheus_prefix".to_string(),
            config::Value::from(handler.prometheus_prefix),
        );

        Self::Table(properties)
    }
}

#[derive(Debug, Deserialize, Default)]
pub struct CustomConfig {
    pub global: Global,
    pub runner: Runner,
    pub proxy: Proxy,
    pub handler: Handler,
}

impl CustomConfig {
    #[cfg(not(test))]
    pub fn new(config_file: &str) -> Result<CustomConfig, ConfigError> {
        let config = Config::builder()
            .add_source(File::with_name(config_file).format(config::FileFormat::Ini))
            .set_default("global", Global::default())?
            .set_default("runner", Runner::default())?
            .set_default("proxy", Proxy::default())?
            .set_default("handler", Handler::default())?
            .build()?;

        config.try_deserialize()
    }

    #[cfg(test)]
    pub fn new(_config_file: &str) -> Result<CustomConfig, ConfigError> {
        let config = Config::builder()
            .set_default(
                "global",
                Global {
                    include: String::from("{libdir}/internal.conf"),
                    rundir: String::from("run"),
                    ipc_prefix: String::from("pushpin-"),
                    port_offset: 0,
                    stats_connection_ttl: 120,
                    stats_connection_send: true,
                    libdir: String::new(),
                },
            )?
            .set_default(
                "runner",
                Runner {
                    rundir: String::new(),
                    services: String::from("connmgr,proxy,handler"),
                    http_port: String::from("7999"),
                    https_ports: String::from("443"),
                    local_ports: String::from("{rundir}/{ipc_prefix}server"),
                    logdir: String::from("log"),
                    log_level: String::from("2"),
                    client_buffer_size: 8192,
                    client_maxconn: 50000,
                    allow_compression: false,
                },
            )?
            .set_default(
                "proxy",
                Proxy {
                    routesfile: String::from("routes"),
                    debug: false,
                    auto_cross_origin: false,
                    accept_x_forwarded_protocol: false,
                    set_x_forwarded_protocol: String::from("proto-only"),
                    x_forwarded_for: String::new(),
                    x_forwarded_for_trusted: String::new(),
                    orig_headers_need_mark: String::new(),
                    accept_pushpin_route: false,
                    cdn_loop: String::new(),
                    log_from: false,
                    log_user_agent: false,
                    sig_iss: String::from("pushpin"),
                    sig_key: String::from("changeme"),
                    upstream_key: String::new(),
                    sockjs_url: String::from("http://cdn.jsdelivr.net/sockjs/0.3.4/sockjs.min.js"),
                    updates_check: String::from("report"),
                    organization_name: String::new(),
                },
            )?
            .set_default(
                "handler",
                Handler {
                    ipc_file_mode: 777,
                    push_in_spec: String::from("tcp://127.0.0.1:5560"),
                    push_in_sub_specs: String::from("tcp://127.0.0.1:5562"),
                    push_in_sub_connect: false,
                    push_in_http_addr: String::from("127.0.0.1"),
                    push_in_http_port: 5561,
                    push_in_http_max_headers_size: 10000,
                    push_in_http_max_body_size: 1000000,
                    stats_spec: String::from("ipc://{rundir}/{ipc_prefix}stats"),
                    command_spec: String::from("tcp://127.0.0.1:5563"),
                    message_rate: 2500,
                    message_hwm: 25000,
                    message_block_size: 0,
                    message_wait: 5000,
                    id_cache_ttl: 60,
                    connection_subscription_max: 20,
                    subscription_linger: 60,
                    stats_subscription_ttl: 60,
                    stats_report_interval: 10,
                    stats_format: String::from("tnetstring"),
                    prometheus_port: String::new(),
                    prometheus_prefix: String::new(),
                },
            )?
            .build()?;

        config.try_deserialize()
    }
}

pub fn default_config_file() -> PathBuf {
    PathBuf::from(env!("CONFIG_DIR")).join("pushpin.conf")
}

pub fn get_config_file(
    work_dir: &Path,
    arg_config: Option<PathBuf>,
) -> Result<PathBuf, Box<dyn Error>> {
    let mut config_files: Vec<PathBuf> = vec![];
    match arg_config {
        Some(x) => config_files.push(x),
        None => {
            // ./config
            config_files.push(work_dir.join("config").join("pushpin.conf"));
            // Same dir as executable (NOTE: deprecated)
            config_files.push(work_dir.join("pushpin.conf"));
            // ./examples/config
            config_files.push(
                work_dir
                    .join("examples")
                    .join("config")
                    .join("pushpin.conf"),
            );
            // Default
            config_files.push(default_config_file());
        }
    }

    let mut config_file = "";
    for cf in config_files.iter() {
        if cf.is_file() {
            config_file = cf.to_str().unwrap_or("");
            break;
        }
    }

    if config_file.is_empty() {
        return Err(format!(
            "no configuration file found. Tried: {}",
            config_files
                .iter()
                .map(|path_buf| path_buf.display().to_string())
                .collect::<Vec<String>>()
                .join(" ")
        )
        .into());
    }

    match Path::new(config_file).try_exists() {
        Ok(true) => {}
        Ok(false) => {
            return Err(format!("failed to open {}", config_file).into());
        }
        Err(e) => {
            return Err(format!("failed to open {}, with error: {:?}", config_file, e).into());
        }
    }

    Ok(config_file.into())
}

mod ffi {
    use crate::core::version;
    use std::env;
    use std::ffi::CString;

    #[repr(C)]
    pub struct BuildConfig {
        version: *mut libc::c_char,
        config_dir: *mut libc::c_char,
        lib_dir: *mut libc::c_char,
    }

    #[no_mangle]
    pub extern "C" fn build_config_new() -> *mut BuildConfig {
        let lib_dir = env!("LIB_DIR");
        let config_dir = env!("CONFIG_DIR");

        let c = BuildConfig {
            version: CString::new(version()).unwrap().into_raw(),
            config_dir: CString::new(config_dir).unwrap().into_raw(),
            lib_dir: CString::new(lib_dir).unwrap().into_raw(),
        };

        Box::into_raw(Box::new(c))
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn build_config_destroy(c: *mut BuildConfig) {
        let c = match c.as_mut() {
            Some(c) => Box::from_raw(c),
            None => return,
        };

        drop(CString::from_raw(c.version));
        drop(CString::from_raw(c.config_dir));
        drop(CString::from_raw(c.lib_dir));
    }
}

/// Address specification for a TCP listener. Supports parsing from a comma-separated string: the
/// first field is an address — a bare port number (e.g. `"9001"`) expands to `0.0.0.0:9001`, and
/// a `host:port` pair is used as-is. Remaining fields are `key` or `key=value` pairs; unrecognized
/// ones are collected into `params`.
pub struct TcpListenConfig {
    pub addr: std::net::SocketAddr,
    pub params: HashMap<String, String>,
}

impl FromStr for TcpListenConfig {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let mut parts = s.split(',');

        // There is always a first part.
        let first = parts.next().unwrap();

        let addr: std::net::SocketAddr = if let Ok(addr) = first.parse() {
            addr
        } else if let Ok(port) = first.parse::<u16>() {
            (IpAddr::V4(Ipv4Addr::UNSPECIFIED), port).into()
        } else {
            return Err(format!("invalid TCP address or port: {}", first));
        };

        let mut params = HashMap::new();

        for part in parts {
            let (k, v) = match part.find('=') {
                Some(pos) => (&part[..pos], &part[(pos + 1)..]),
                None => (part, ""),
            };
            params.insert(k.to_string(), v.to_string());
        }

        Ok(Self { addr, params })
    }
}

/// Address specification for a Unix socket listener. Supports parsing from a comma-separated
/// string: the first field is the socket path, and remaining fields are `key` or `key=value`
/// pairs. Recognized keys are `mode` (octal permissions), `user`, and `group`. Unrecognized ones
/// are collected into `params`.
pub struct UnixListenConfig {
    pub path: PathBuf,
    pub mode: Option<u32>,
    pub user: Option<String>,
    pub group: Option<String>,
    pub params: HashMap<String, String>,
}

impl FromStr for UnixListenConfig {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let mut parts = s.split(',');

        // There is always a first part.
        let path = PathBuf::from(parts.next().unwrap());

        let mut mode = None;
        let mut user = None;
        let mut group = None;
        let mut params = HashMap::new();

        for part in parts {
            let (k, v) = match part.find('=') {
                Some(pos) => (&part[..pos], &part[(pos + 1)..]),
                None => (part, ""),
            };

            match k {
                "mode" => match u32::from_str_radix(v, 8) {
                    Ok(x) => mode = Some(x),
                    Err(e) => return Err(format!("failed to parse mode: {}", e)),
                },
                "user" => user = Some(String::from(v)),
                "group" => group = Some(String::from(v)),
                _ => {
                    params.insert(k.to_string(), v.to_string());
                }
            }
        }

        Ok(Self {
            path,
            mode,
            user,
            group,
            params,
        })
    }
}

pub enum NetListenConfig {
    Tcp(TcpListenConfig),
    Unix(UnixListenConfig),
}

impl FromStr for NetListenConfig {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let mut parts = s.split(',');

        // There is always a first part.
        let first = parts.next().unwrap();

        let mut local = false;
        let mut rest: Vec<&str> = Vec::new();

        for part in parts {
            let k = match part.find('=') {
                Some(pos) => &part[..pos],
                None => part,
            };
            if k == "local" {
                local = true;
            } else {
                rest.push(part);
            }
        }

        // Reconstruct a string for the inner parser: first field + remaining params.
        let inner = if rest.is_empty() {
            first.to_string()
        } else {
            format!("{},{}", first, rest.join(","))
        };

        if local {
            Ok(Self::Unix(inner.parse()?))
        } else {
            Ok(Self::Tcp(inner.parse()?))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::{ensure_example_config, test_dir};
    use std::error::Error;
    use std::net::{Ipv4Addr, Ipv6Addr, SocketAddr};
    use std::path::PathBuf;

    #[test]
    fn tcp_listen_config_port_only() {
        let c: TcpListenConfig = "9001".parse().unwrap();
        assert_eq!(c.addr, SocketAddr::new(Ipv4Addr::UNSPECIFIED.into(), 9001));
        assert!(c.params.is_empty());
    }

    #[test]
    fn tcp_listen_config_host_port() {
        let c: TcpListenConfig = "127.0.0.1:9001".parse().unwrap();
        assert_eq!(c.addr, "127.0.0.1:9001".parse().unwrap());
        assert!(c.params.is_empty());
    }

    #[test]
    fn tcp_listen_config_ipv6() {
        let c: TcpListenConfig = "[::1]:9001".parse().unwrap();
        assert_eq!(c.addr, SocketAddr::new(Ipv6Addr::LOCALHOST.into(), 9001));
        assert!(c.params.is_empty());
    }

    #[test]
    fn tcp_listen_config_params() {
        let c: TcpListenConfig = "9001,tls,default-cert=mycert".parse().unwrap();
        assert_eq!(c.addr, SocketAddr::new(Ipv4Addr::UNSPECIFIED.into(), 9001));
        assert_eq!(c.params["tls"], "");
        assert_eq!(c.params["default-cert"], "mycert");
    }

    #[test]
    fn tcp_listen_config_invalid() {
        assert!("notanaddr".parse::<TcpListenConfig>().is_err());
    }

    #[test]
    fn unix_listen_config_path_only() {
        let c: UnixListenConfig = "/tmp/foo.sock".parse().unwrap();
        assert_eq!(c.path, PathBuf::from("/tmp/foo.sock"));
        assert_eq!(c.mode, None);
        assert_eq!(c.user, None);
        assert_eq!(c.group, None);
        assert!(c.params.is_empty());
    }

    #[test]
    fn unix_listen_config_all_known_params() {
        let c: UnixListenConfig = "/tmp/foo.sock,mode=0600,user=www,group=www"
            .parse()
            .unwrap();
        assert_eq!(c.path, PathBuf::from("/tmp/foo.sock"));
        assert_eq!(c.mode, Some(0o600));
        assert_eq!(c.user.as_deref(), Some("www"));
        assert_eq!(c.group.as_deref(), Some("www"));
        assert!(c.params.is_empty());
    }

    #[test]
    fn unix_listen_config_unknown_params() {
        let c: UnixListenConfig = "/tmp/foo.sock,req".parse().unwrap();
        assert_eq!(c.params["req"], "");
    }

    #[test]
    fn unix_listen_config_bad_mode() {
        assert!("/tmp/foo.sock,mode=notoctal"
            .parse::<UnixListenConfig>()
            .is_err());
    }

    #[test]
    fn net_listen_config_tcp_port_only() {
        let NetListenConfig::Tcp(c) = "9001".parse().unwrap() else {
            panic!("expected Tcp")
        };
        assert_eq!(c.addr, SocketAddr::new(Ipv4Addr::UNSPECIFIED.into(), 9001));
        assert!(c.params.is_empty());
    }

    #[test]
    fn net_listen_config_tcp_with_params() {
        let NetListenConfig::Tcp(c) = "9001,tls,default-cert=mycert".parse().unwrap() else {
            panic!("expected Tcp")
        };
        assert_eq!(c.addr, SocketAddr::new(Ipv4Addr::UNSPECIFIED.into(), 9001));
        assert_eq!(c.params["tls"], "");
        assert_eq!(c.params["default-cert"], "mycert");
    }

    #[test]
    fn net_listen_config_unix_via_local() {
        let NetListenConfig::Unix(c) = "/tmp/foo.sock,local,mode=0600".parse().unwrap() else {
            panic!("expected Unix")
        };
        assert_eq!(c.path, PathBuf::from("/tmp/foo.sock"));
        assert_eq!(c.mode, Some(0o600));
        assert!(c.params.is_empty());
    }

    #[test]
    fn net_listen_config_unix_unknown_params() {
        let NetListenConfig::Unix(c) = "/tmp/foo.sock,local,req".parse().unwrap() else {
            panic!("expected Unix")
        };
        assert_eq!(c.params["req"], "");
    }

    struct TestArgs {
        name: &'static str,
        work_dir: PathBuf,
        input: Option<PathBuf>,
        output: Result<PathBuf, Box<dyn Error>>,
    }

    #[test]
    fn it_works() {
        let test_dir = test_dir();
        ensure_example_config(&test_dir);

        let test_args: Vec<TestArgs> = vec![TestArgs {
            name: "no input",
            work_dir: test_dir.clone(),
            input: None,
            output: Ok(test_dir
                .join("examples")
                .join("config")
                .join("pushpin.conf")),
        }];

        for test_arg in test_args.iter() {
            assert_eq!(
                get_config_file(&test_arg.work_dir, test_arg.input.clone()).unwrap(),
                test_arg.output.as_deref().unwrap(),
                "{}",
                test_arg.name
            );
        }
    }

    #[test]
    fn it_fails() {
        let test_args: Vec<TestArgs> = vec![TestArgs {
            name: "invalid config file",
            work_dir: test_dir(),
            input: Some(PathBuf::from("no/such/file")),
            output: Err("no configuration file found. Tried: no/such/file".into()),
        }];

        for test_arg in test_args.iter() {
            match get_config_file(&test_arg.work_dir, test_arg.input.clone()) {
                Ok(x) => panic!(
                    "Test case {} should fail, but its passing with this output {:?}",
                    test_arg.name, x
                ),
                Err(e) => {
                    assert_eq!(
                        e.to_string(),
                        test_arg.output.as_deref().unwrap_err().to_string()
                    );
                }
            }
        }
    }
}
