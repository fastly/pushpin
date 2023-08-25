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

use clap::{ArgAction, Parser};
use std::collections::HashMap;
use std::error::Error;
use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::path::{Path, PathBuf};
use std::string::String;

#[derive(Parser, Clone)]
#[command(
    name = "Pushpin",
    version,
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

#[derive(Eq, PartialEq, Debug)]
pub struct ArgsData {
    id: Option<u32>,
    pub config_file: PathBuf,
    log_file: PathBuf,
    route_lines: Vec<String>,
    log_levels: HashMap<String, u8>,
    socket: Option<SocketAddr>,
}

impl ArgsData {
    pub fn new(cli_args: CliArgs) -> Result<Self, Box<dyn Error>> {
        Ok(Self {
            id: Self::get_id(cli_args.id)?,
            config_file: Self::get_config_file(cli_args.config.as_deref()),
            log_file: Self::get_log_file(cli_args.logfile.as_deref()),
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

    fn get_config_file(config_file: Option<&Path>) -> PathBuf {
        match config_file {
            Some(x) => x.to_path_buf(),
            _ => PathBuf::new(),
        }
    }

    fn get_log_file(log_file: Option<&Path>) -> PathBuf {
        match log_file {
            Some(x) => x.to_path_buf(),
            _ => PathBuf::new(),
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;
    use std::error::Error;
    use std::net::SocketAddr;
    use std::path::PathBuf;

    struct TestArgs {
        name: &'static str,
        input: CliArgs,
        output: Result<ArgsData, Box<dyn Error>>,
    }

    #[test]
    fn it_works() {
        let test_args: Vec<TestArgs> = vec![
            TestArgs {
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
                    config_file: PathBuf::new(),
                    log_file: PathBuf::new(),
                    route_lines: vec![],
                    log_levels: HashMap::from([(String::new(), 2)]),
                    socket: None,
                }),
            },
            TestArgs {
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
                    config_file: PathBuf::from("/cfg/path"),
                    log_file: PathBuf::from("/log/path"),
                    route_lines: vec![String::from("* test")],
                    log_levels: HashMap::from([(String::new(), 2)]),
                    socket: Some("0.0.0.0:1234".parse::<SocketAddr>().unwrap()),
                }),
            },
            TestArgs {
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
                    config_file: PathBuf::from("/cfg/path"),
                    log_file: PathBuf::from("/log/path"),
                    route_lines: vec![String::from("* test")],
                    log_levels: HashMap::from([(String::new(), 3)]),
                    socket: Some("0.0.0.0:1234".parse::<SocketAddr>().unwrap()),
                }),
            },
            TestArgs {
                name: "log level subservice",
                input: CliArgs {
                    id: Some(123),
                    config: Some(PathBuf::from("/cfg/path")),
                    logfile: Some(PathBuf::from("/log/path")),
                    loglevel: Some(String::from("2,condure:3")),
                    verbose: false,
                    route: Some(vec![String::from("* test")]),
                    port: Some(String::from("1234")),
                },
                output: Ok(ArgsData {
                    id: Some(123),
                    config_file: PathBuf::from("/cfg/path"),
                    log_file: PathBuf::from("/log/path"),
                    route_lines: vec![String::from("* test")],
                    log_levels: HashMap::from([
                        (String::new(), 2u8),
                        (String::from("condure"), 3u8),
                    ]),
                    socket: Some("0.0.0.0:1234".parse::<SocketAddr>().unwrap()),
                }),
            },
            TestArgs {
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
                    config_file: PathBuf::from("/cfg/path"),
                    log_file: PathBuf::from("/log/path"),
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
        let test_args: Vec<TestArgs> = vec![
            TestArgs {
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
            TestArgs {
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
            TestArgs {
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
            TestArgs {
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
            TestArgs {
                name: "neg log level for subservice",
                input: CliArgs {
                    id: None,
                    config: None,
                    logfile: None,
                    loglevel: Some(String::from("condure:-1")),
                    verbose: false,
                    route: None,
                    port: None,
                },
                output: Err(
                    "log level for service condure must be greater than or equal to 0".into(),
                ),
            },
            TestArgs {
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
            TestArgs {
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
            TestArgs {
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
}
