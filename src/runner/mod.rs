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

pub mod service;

use clap::{ArgAction, Parser};
use log::{error, warn};
use serde::Deserialize;
use std::collections::HashMap;
use std::error::Error;
use std::fs::{self, File, OpenOptions};
use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::path::{Path, PathBuf};
use std::str;
use std::string::String;
use url::Url;

use crate::core::config::{get_config_file, CustomConfig};
use crate::core::version;

#[derive(Parser, Clone)]
#[command(
    name = "pushpin",
    version = version(),
    about = "Reverse proxy for realtime web services."
)]
pub struct CliArgs {
    #[arg(long, value_name = "file", help = "Config file.")]
    pub config: Option<PathBuf>,

    #[arg(long, value_name = "file", help = "File to log to.")]
    pub logfile: Option<PathBuf>,

    #[arg(
        long,
        value_name = "x",
        default_value = "2",
        help = "Log level (default: 2)."
    )]
    pub loglevel: Option<String>,

    #[arg(long, action=ArgAction::SetTrue, help = "Verbose output. Same as --loglevel=3.")]
    pub verbose: bool,

    #[arg(
        long,
        value_name = "[addr:]port",
        help = "Run a single HTTP server instance."
    )]
    pub port: Option<String>,

    #[arg(
        long,
        value_name = "x",
        help = "Set instance ID (needed to run multiple instances)."
    )]
    pub id: Option<i32>,

    #[arg(long, value_name = "line", help = "Add route (overrides routes file).")]
    pub route: Option<Vec<String>>,
}

#[derive(Eq, PartialEq, Debug, Clone)]
pub struct ArgsData {
    id: Option<u32>,
    pub config_file: Option<PathBuf>,
    log_file: Option<PathBuf>,
    route_lines: Vec<String>,
    log_levels: HashMap<String, u8>,
    socket: Option<SocketAddr>,
}

impl ArgsData {
    pub fn new(cli_args: CliArgs) -> Result<Self, Box<dyn Error>> {
        Ok(Self {
            id: Self::get_id(cli_args.id)?,
            config_file: cli_args.config,
            log_file: cli_args.logfile,
            route_lines: Self::get_route_lines(cli_args.route.as_deref()),
            log_levels: Self::get_log_levels(cli_args.loglevel.as_deref(), cli_args.verbose)?,
            socket: Self::get_socket(cli_args.port.as_deref())?,
        })
    }

    fn get_id(id: Option<i32>) -> Result<Option<u32>, Box<dyn Error>> {
        let id = match id {
            Some(x) => x,
            _ => return Ok(None),
        };

        if id >= 0 {
            Ok(Some(id as u32))
        } else {
            Err("id must be greater than or equal to 0".into())
        }
    }

    fn get_route_lines(route_lines: Option<&[String]>) -> Vec<String> {
        match route_lines {
            Some(x) => x.to_vec(),
            _ => vec![],
        }
    }

    fn get_log_levels(
        levels: Option<&str>,
        verbose: bool,
    ) -> Result<HashMap<String, u8>, Box<dyn Error>> {
        if verbose {
            return Ok(HashMap::from([(String::new(), 3)]));
        }

        let parts = match levels {
            Some(x) => x.split(','),
            None => {
                // default log level imposed
                return Ok(HashMap::from([(String::new(), 2)]));
            }
        };

        let mut levels: HashMap<String, u8> = HashMap::new();
        for part in parts {
            if part.is_empty() {
                return Err("log level component cannot be empty".into());
            }

            match part.find(':') {
                None => {
                    let level: u8 = match part.trim().parse() {
                        Ok(x) => x,
                        Err(_) => return Err("log level must be greater than or equal to 0".into()),
                    };

                    levels.insert(String::new(), level);
                }
                Some(indx) => {
                    if indx == 0 {
                        return Err("log level component name cannot be empty".into());
                    }

                    let name = &part[..indx];
                    let level: u8 = match part[indx + 1..].trim().parse() {
                        Ok(x) => x,
                        Err(_) => {
                            return Err(format!(
                                "log level for service {} must be greater than or equal to 0",
                                name
                            )
                            .into())
                        }
                    };

                    levels.insert(String::from(name), level);
                }
            }
        }

        Ok(levels)
    }

    fn get_socket(port: Option<&str>) -> Result<Option<SocketAddr>, Box<dyn Error>> {
        let socket = match port {
            Some(x) => x,
            None => return Ok(None),
        };
        let (socket, port) = match socket.find(':') {
            Some(x) => (Some(socket), &socket[(x + 1)..]),
            None => (None, socket),
        };
        let port = match port.parse::<u16>() {
            Ok(x) => x,
            Err(_) => return Err("port must be greater than or equal to 1".into()),
        };
        if socket.is_none() {
            return Ok(Some(SocketAddr::new(
                IpAddr::V4(Ipv4Addr::new(0, 0, 0, 0)),
                port,
            )));
        }
        let socket = match socket {
            Some(x) => x,
            None => return Err("error parsing port.".into()),
        };
        match socket.parse::<SocketAddr>() {
            Ok(x) => Ok(Some(x)),
            Err(e) => Err(format!("error parsing port. {:?}", e).into()),
        }
    }
}

#[derive(Debug, Deserialize, PartialEq, Eq, Clone)]
pub struct Settings {
    pub service_names: Vec<String>,
    pub config_file: PathBuf,
    pub run_dir: PathBuf,
    pub log_file: Option<PathBuf>,
    pub certs_dir: PathBuf,
    pub connmgr_bin: PathBuf,
    pub proxy_bin: PathBuf,
    pub handler_bin: PathBuf,
    pub ipc_prefix: String,
    pub ports: Vec<ListenPort>,
    pub port_offset: u32,
    pub client_buffer_size: i32,
    pub client_max_connections: i32,
    pub allow_compression: bool,
    pub file_prefix: String,
    pub log_levels: HashMap<String, u8>,
    pub route_lines: Vec<String>,
}

impl Settings {
    pub fn new(work_dir: &Path, args_data: ArgsData) -> Result<Self, Box<dyn Error>> {
        let config_file_path = get_config_file(work_dir, args_data.config_file)?;
        let config = match CustomConfig::new(config_file_path.to_str().unwrap()) {
            Ok(x) => x,
            Err(e) => return Err(format!("error: parsing config. {:?}", e).into()),
        };

        let exec_dir = work_dir;

        let config_dir = config_file_path.parent().unwrap().join("runner");
        let certs_dir = config_dir.join("certs");

        let mut log_levels: HashMap<String, u8> = HashMap::new();
        let config_log_levels: Vec<String> = config
            .runner
            .log_level
            .split(',')
            .map(|s| s.to_string())
            .collect();
        if !config_log_levels.is_empty() {
            log_levels = parse_log_levels(config_log_levels)?;
            if log_levels.is_empty() {
                return Err("error: parsing config while parsing log levels".into());
            }
        }
        if !args_data.log_levels.is_empty() {
            log_levels.clone_from(&args_data.log_levels);
        }
        log_levels.insert(
            "default".to_string(),
            *args_data.log_levels.get("").unwrap_or(&2),
        );

        let mut run_dir = PathBuf::from(config.global.rundir.clone());
        if config.global.rundir.is_empty() {
            warn!("rundir in [runner] section is deprecated. put in [global]");
            run_dir = PathBuf::from(config.runner.rundir.clone());
        }
        run_dir = exec_dir.join(run_dir);
        ensure_dir(run_dir.as_ref())?;

        let mut port_offset = 0;
        let mut ipc_prefix = if !config.global.ipc_prefix.is_empty() {
            config.global.ipc_prefix.clone()
        } else {
            "pushpin-".to_string()
        };
        let mut file_prefix = String::new();
        if let Some(x) = args_data.id {
            ipc_prefix = format!("{:?}-", x);
            port_offset = x * 10;
            file_prefix.clone_from(&ipc_prefix);
        }

        let mut ports: Vec<ListenPort> = vec![];
        match args_data.socket {
            Some(x) => {
                ports.push(ListenPort::new(
                    Some(x.ip()),
                    Some(x.port()),
                    None,
                    None,
                    None,
                    None,
                    None,
                ));
            }
            None => {
                for port in config.runner.http_port.split(',') {
                    if !port.is_empty() {
                        let socket = get_socket(Some(port))?.unwrap();
                        ports.push(ListenPort::new(
                            Some(socket.ip()),
                            Some(socket.port()),
                            None,
                            None,
                            None,
                            None,
                            None,
                        ));
                    }
                }
                for port in config.runner.https_ports.split(',') {
                    if !port.is_empty() {
                        let socket = get_socket(Some(port))?.unwrap();
                        ports.push(ListenPort::new(
                            Some(socket.ip()),
                            Some(socket.port()),
                            Some(true),
                            None,
                            None,
                            None,
                            None,
                        ));
                    }
                }
                for port in config
                    .runner
                    .local_ports
                    .replace("{rundir}", config.global.rundir.as_str())
                    .replace("{ipc_prefix}", &ipc_prefix)
                    .split(',')
                {
                    if !port.is_empty() {
                        let uri = if port.starts_with("unix:/") {
                            port.to_string()
                        } else {
                            format!("unix:/{}", port)
                        };
                        let uri = match Url::parse(uri.as_str()) {
                            Ok(x) => x,
                            _ => {
                                error!("invalid local port: {:?}", port);
                                return Err(format!("invalid local port: {:?}", port).into());
                            }
                        };
                        let params = uri
                            .query()
                            .map(|v| {
                                url::form_urlencoded::parse(v.as_bytes())
                                    .into_owned()
                                    .collect()
                            })
                            .unwrap_or_else(HashMap::new);

                        let mut mode = None;
                        if params.contains_key("mode") {
                            let mode_string = match params.get("mode") {
                                Some(x) => x,
                                None => {
                                    error!("invalid uri: {:?}", uri);
                                    return Err(format!("invalid uri: {:?}", uri).into());
                                }
                            };
                            mode = match mode_string.parse::<i32>() {
                                Ok(x) => Some(x),
                                Err(_) => {
                                    error!("invalid mode: {:?}", mode_string);
                                    return Err(format!("invalid mode: {:?}", mode_string).into());
                                }
                            };
                        }
                        ports.push(ListenPort::new(
                            None,
                            Some(0),
                            Some(true),
                            Some(uri.path().into()),
                            mode,
                            params.get("user").cloned(),
                            params.get("group").cloned(),
                        ));
                    }
                }
            }
        }
        if ports.is_empty() {
            error!("no server ports configured");
            return Err("no server ports configured".into());
        }

        Ok(Self {
            service_names: config
                .runner
                .services
                .split(',')
                .map(|s| s.to_string())
                .collect(),
            config_file: config_file_path,
            run_dir,
            log_file: args_data.log_file,
            connmgr_bin: get_service_dir(
                exec_dir.into(),
                "pushpin-connmgr",
                "bin/pushpin-connmgr",
            )?,
            proxy_bin: get_service_dir(exec_dir.into(), "pushpin-proxy", "bin/pushpin-proxy")?,
            handler_bin: get_service_dir(
                exec_dir.into(),
                "pushpin-handler",
                "bin/pushpin-handler",
            )?,
            certs_dir,
            ipc_prefix,
            ports,
            client_buffer_size: config.runner.client_buffer_size,
            client_max_connections: config.runner.client_maxconn,
            allow_compression: config.runner.allow_compression,
            port_offset,
            file_prefix,
            log_levels,
            route_lines: args_data.route_lines,
        })
    }
}

#[derive(Debug, Deserialize, PartialEq, Eq, Clone)]
pub struct ListenPort {
    pub ip: IpAddr,
    pub port: u16,
    pub ssl: bool,
    pub local_path: String,
    pub mode: i32,
    pub user: String,
    pub group: String,
}

impl ListenPort {
    fn new(
        ip: Option<IpAddr>,
        port: Option<u16>,
        ssl: Option<bool>,
        local_path: Option<String>,
        mode: Option<i32>,
        user: Option<String>,
        group: Option<String>,
    ) -> ListenPort {
        Self {
            ip: ip.unwrap_or(IpAddr::V4(Ipv4Addr::new(0, 0, 0, 0))),
            port: port.unwrap_or_default(),
            ssl: ssl.unwrap_or(false),
            local_path: local_path.unwrap_or_default(),
            mode: mode.unwrap_or(-1),
            user: user.unwrap_or_default(),
            group: group.unwrap_or_default(),
        }
    }
}

fn ensure_dir(directory_path: &Path) -> Result<(), std::io::Error> {
    if !directory_path.exists() {
        fs::create_dir_all(directory_path)?;
    }
    Ok(())
}

fn get_service_dir(
    exec_dir: PathBuf,
    service_name: &str,
    service_dir: &str,
) -> Result<PathBuf, Box<dyn Error>> {
    let service_exec_dir = exec_dir.join(service_dir);
    if service_exec_dir.is_file() {
        return Ok(fs::canonicalize(service_exec_dir)?);
    }

    Ok(PathBuf::from(service_name))
}

fn get_socket(port: Option<&str>) -> Result<Option<SocketAddr>, Box<dyn Error>> {
    let socket = match port {
        Some(x) => x,
        None => return Ok(None),
    };
    if socket.is_empty() {
        return Ok(None);
    }
    let (socket, port) = match socket.find(':') {
        Some(x) => (Some(socket), &socket[(x + 1)..]),
        None => (None, socket),
    };
    let port = match port.parse::<u16>() {
        Ok(x) => x,
        Err(_) => return Err(format!("port {:?} must be greater than or equal to 1", port).into()),
    };
    if socket.is_none() {
        return Ok(Some(SocketAddr::new(
            IpAddr::V4(Ipv4Addr::new(0, 0, 0, 0)),
            port,
        )));
    }
    let socket = match socket {
        Some(x) => x,
        None => return Err("error parsing port.".into()),
    };
    match socket.parse::<SocketAddr>() {
        Ok(x) => Ok(Some(x)),
        Err(e) => Err(format!("error parsing port. {:?}", e).into()),
    }
}

fn parse_log_levels(log_levels: Vec<String>) -> Result<HashMap<String, u8>, Box<dyn Error>> {
    let mut levels: HashMap<String, u8> = HashMap::new();
    for log_level in log_levels {
        if log_level.is_empty() {
            return Err("log level component cannot be empty".into());
        }

        match log_level.find(':') {
            Some(indx) => {
                if indx == 0 {
                    return Err("log level component name cannot be empty".into());
                }
                let name = &log_level[..indx];
                let level: u8 = match log_level[indx + 1..].trim().parse() {
                    Ok(x) => x,
                    Err(_) => {
                        return Err(format!(
                            "log level for service {} must be greater than or equal to 0",
                            name
                        )
                        .into())
                    }
                };

                levels.insert(String::from(name), level);
            }
            None => {
                let level: u8 = match log_level.trim().parse() {
                    Ok(x) => x,
                    Err(_) => return Err("log level must be greater than or equal to 0".into()),
                };

                levels.insert(String::new(), level);
            }
        }
    }
    Ok(levels)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::{call_c_main, ensure_example_config, qtest, test_dir};
    use crate::ffi;
    use std::collections::HashMap;
    use std::error::Error;
    use std::ffi::OsStr;
    use std::net::SocketAddr;
    use std::net::{IpAddr, Ipv4Addr};
    use std::path::PathBuf;

    struct SettingsTestArgs {
        name: &'static str,
        input: ArgsData,
        output: Result<Settings, Box<dyn Error>>,
    }

    struct CliTestArgs {
        name: &'static str,
        input: CliArgs,
        output: Result<ArgsData, Box<dyn Error>>,
    }

    #[test]
    fn cli_it_works() {
        let test_args: Vec<CliTestArgs> = vec![
            CliTestArgs {
                name: "no input",
                input: CliArgs {
                    id: None,
                    config: None,
                    logfile: None,
                    loglevel: None,
                    verbose: false,
                    route: None,
                    port: None,
                },
                output: Ok(ArgsData {
                    id: None,
                    config_file: None,
                    log_file: None,
                    route_lines: vec![],
                    log_levels: HashMap::from([(String::new(), 2)]),
                    socket: None,
                }),
            },
            CliTestArgs {
                name: "basic input",
                input: CliArgs {
                    id: Some(123),
                    config: Some(PathBuf::from("/cfg/path")),
                    logfile: Some(PathBuf::from("/log/path")),
                    loglevel: Some(String::from("2")),
                    verbose: false,
                    route: Some(vec![String::from("* test")]),
                    port: Some(String::from("1234")),
                },
                output: Ok(ArgsData {
                    id: Some(123),
                    config_file: Some(PathBuf::from("/cfg/path")),
                    log_file: Some(PathBuf::from("/log/path")),
                    route_lines: vec![String::from("* test")],
                    log_levels: HashMap::from([(String::new(), 2)]),
                    socket: Some("0.0.0.0:1234".parse::<SocketAddr>().unwrap()),
                }),
            },
            CliTestArgs {
                name: "verbose",
                input: CliArgs {
                    id: Some(123),
                    config: Some(PathBuf::from("/cfg/path")),
                    logfile: Some(PathBuf::from("/log/path")),
                    loglevel: Some(String::from("2")),
                    verbose: true,
                    route: Some(vec![String::from("* test")]),
                    port: Some(String::from("1234")),
                },
                output: Ok(ArgsData {
                    id: Some(123),
                    config_file: Some(PathBuf::from("/cfg/path")),
                    log_file: Some(PathBuf::from("/log/path")),
                    route_lines: vec![String::from("* test")],
                    log_levels: HashMap::from([(String::new(), 3)]),
                    socket: Some("0.0.0.0:1234".parse::<SocketAddr>().unwrap()),
                }),
            },
            CliTestArgs {
                name: "log level subservice",
                input: CliArgs {
                    id: Some(123),
                    config: Some(PathBuf::from("/cfg/path")),
                    logfile: Some(PathBuf::from("/log/path")),
                    loglevel: Some(String::from("2,connmgr:3")),
                    verbose: false,
                    route: Some(vec![String::from("* test")]),
                    port: Some(String::from("1234")),
                },
                output: Ok(ArgsData {
                    id: Some(123),
                    config_file: Some(PathBuf::from("/cfg/path")),
                    log_file: Some(PathBuf::from("/log/path")),
                    route_lines: vec![String::from("* test")],
                    log_levels: HashMap::from([
                        (String::new(), 2u8),
                        (String::from("connmgr"), 3u8),
                    ]),
                    socket: Some("0.0.0.0:1234".parse::<SocketAddr>().unwrap()),
                }),
            },
            CliTestArgs {
                name: "port socket",
                input: CliArgs {
                    id: Some(123),
                    config: Some(PathBuf::from("/cfg/path")),
                    logfile: Some(PathBuf::from("/log/path")),
                    loglevel: Some(String::from("2")),
                    verbose: false,
                    route: Some(vec![String::from("* test")]),
                    port: Some(String::from("127.0.0.1:1234")),
                },
                output: Ok(ArgsData {
                    id: Some(123),
                    config_file: Some(PathBuf::from("/cfg/path")),
                    log_file: Some(PathBuf::from("/log/path")),
                    route_lines: vec![String::from("* test")],
                    log_levels: HashMap::from([(String::new(), 2u8)]),
                    socket: Some("127.0.0.1:1234".parse::<SocketAddr>().unwrap()),
                }),
            },
        ];

        for test_arg in test_args.iter() {
            let _output = &test_arg.output;
            assert!(
                matches!(ArgsData::new(test_arg.input.clone()), _output),
                "{}",
                test_arg.name
            );
        }
    }

    #[test]
    fn it_fails() {
        let test_args: Vec<CliTestArgs> = vec![
            CliTestArgs {
                name: "neg id",
                input: CliArgs {
                    id: Some(-123),
                    config: None,
                    logfile: None,
                    loglevel: None,
                    verbose: false,
                    route: None,
                    port: None,
                },
                output: Err("id must be greater than or equal to 0".into()),
            },
            CliTestArgs {
                name: "missing log level",
                input: CliArgs {
                    id: None,
                    config: None,
                    logfile: None,
                    loglevel: Some(String::from("2,")),
                    verbose: false,
                    route: None,
                    port: None,
                },
                output: Err("log level component cannot be empty".into()),
            },
            CliTestArgs {
                name: "neg log level",
                input: CliArgs {
                    id: None,
                    config: None,
                    logfile: None,
                    loglevel: Some(String::from("-2")),
                    verbose: false,
                    route: None,
                    port: None,
                },
                output: Err("log level must be greater than or equal to 0".into()),
            },
            CliTestArgs {
                name: "empty log name",
                input: CliArgs {
                    id: None,
                    config: None,
                    logfile: None,
                    loglevel: Some(String::from(":2,")),
                    verbose: false,
                    route: None,
                    port: None,
                },
                output: Err("log level component name cannot be empty".into()),
            },
            CliTestArgs {
                name: "neg log level for subservice",
                input: CliArgs {
                    id: None,
                    config: None,
                    logfile: None,
                    loglevel: Some(String::from("connmgr:-1")),
                    verbose: false,
                    route: None,
                    port: None,
                },
                output: Err(
                    "log level for service connmgr must be greater than or equal to 0".into(),
                ),
            },
            CliTestArgs {
                name: "neg port",
                input: CliArgs {
                    id: None,
                    config: None,
                    logfile: None,
                    loglevel: None,
                    verbose: false,
                    route: None,
                    port: Some(String::from("-1234")),
                },
                output: Err("port must be greater than or equal to 1".into()),
            },
            CliTestArgs {
                name: "empty host",
                input: CliArgs {
                    id: None,
                    config: None,
                    logfile: None,
                    loglevel: None,
                    verbose: false,
                    route: None,
                    port: Some(String::from(":1234")),
                },
                output: Err("error parsing port. AddrParseError(Socket)".into()),
            },
            CliTestArgs {
                name: "empty port",
                input: CliArgs {
                    id: None,
                    config: None,
                    logfile: None,
                    loglevel: None,
                    verbose: false,
                    route: None,
                    port: Some(String::from("test:")),
                },
                output: Err("port must be greater than or equal to 1".into()),
            },
        ];

        for test_arg in test_args.iter() {
            let _output = &test_arg.output;
            assert!(
                matches!(ArgsData::new(test_arg.input.clone()), _output),
                "{}",
                test_arg.name
            );
        }
    }

    #[test]
    fn it_works() {
        let test_dir = test_dir();
        ensure_example_config(&test_dir);

        let exec_dir = &test_dir;
        let mut log_map = HashMap::new();
        log_map.insert("".to_string(), 2);
        log_map.insert("default".to_string(), 2);
        let test_args: Vec<SettingsTestArgs> = vec![SettingsTestArgs {
            name: "no input",
            input: ArgsData {
                id: None,
                config_file: None,
                log_file: None,
                route_lines: vec![],
                log_levels: HashMap::from([(String::new(), 2)]),
                socket: None,
            },
            output: Ok(Settings {
                service_names: vec![
                    "connmgr".to_string(),
                    "proxy".to_string(),
                    "handler".to_string(),
                ],
                config_file: test_dir
                    .join("examples")
                    .join("config")
                    .join("pushpin.conf"),
                run_dir: exec_dir.clone().join("run"),
                log_file: None,
                certs_dir: test_dir
                    .join("examples")
                    .join("config")
                    .join("runner")
                    .join("certs"),
                connmgr_bin: if exec_dir.clone().join("bin/pushpin-connmgr").exists() {
                    exec_dir.clone().join("bin/pushpin-connmgr")
                } else {
                    PathBuf::from("pushpin-connmgr")
                },
                proxy_bin: if exec_dir.clone().join("bin/pushpin-proxy").exists() {
                    exec_dir.clone().join("bin/pushpin-proxy")
                } else {
                    PathBuf::from("pushpin-proxy")
                },
                handler_bin: if exec_dir.clone().join("bin/pushpin-handler").exists() {
                    exec_dir.clone().join("bin/pushpin-handler")
                } else {
                    PathBuf::from("pushpin-handler")
                },
                ipc_prefix: String::from("pushpin-"),
                ports: vec![
                    ListenPort {
                        ip: IpAddr::V4(Ipv4Addr::new(0, 0, 0, 0)),
                        port: 7999,
                        ssl: false,
                        local_path: String::new(),
                        mode: -1,
                        user: String::from(""),
                        group: String::from(""),
                    },
                    ListenPort {
                        ip: IpAddr::V4(Ipv4Addr::new(0, 0, 0, 0)),
                        port: 443,
                        ssl: true,
                        local_path: String::new(),
                        mode: -1,
                        user: String::from(""),
                        group: String::from(""),
                    },
                    ListenPort {
                        ip: IpAddr::V4(Ipv4Addr::new(0, 0, 0, 0)),
                        port: 0,
                        ssl: true,
                        local_path: "/run/pushpin-server".to_string(),
                        mode: -1,
                        user: String::from(""),
                        group: String::from(""),
                    },
                ],
                client_buffer_size: 8192,
                client_max_connections: 50000,
                allow_compression: false,
                port_offset: 0,
                file_prefix: String::new(),
                log_levels: log_map,
                route_lines: vec![],
            }),
        }];

        for test_arg in test_args.iter() {
            assert_eq!(
                Settings::new(&test_dir, test_arg.input.clone()).unwrap(),
                test_arg.output.as_ref().unwrap().clone(),
                "{}",
                test_arg.name
            );
        }
    }

    fn template_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::template_test, args) as u8 }
    }

    #[test]
    fn template() {
        assert!(qtest::run(template_test));
    }
}

pub fn open_log_file(log_file_path: PathBuf) -> Result<File, std::io::Error> {
    match ensure_dir(log_file_path.parent().unwrap()) {
        Ok(_) => (),
        Err(e) => return Err(e),
    }
    OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .open(log_file_path)
}
