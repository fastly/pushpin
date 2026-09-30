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

use clap::{Arg, ArgAction, Command};
use log::{error, LevelFilter};
use pushpin::api::{run, Config, ListenSpec};
use pushpin::core::config::NetListenConfig;
use pushpin::core::log::{get_simple_logger, local_offset_check};
use pushpin::core::version;
use std::error::Error;
use std::process;

// Safety value
const CONNS_MAX: usize = 10_000_000;

struct Args {
    maxconn: usize,
    buffer_size: usize,
    body_buffer_size: usize,
    listen: Vec<String>,
}

fn process_args_and_run(args: Args) -> Result<(), Box<dyn Error>> {
    if args.maxconn > CONNS_MAX {
        return Err("maxconn is too large".into());
    }

    let mut config = Config {
        maxconn: args.maxconn,
        buffer_size: args.buffer_size,
        body_buffer_size: args.body_buffer_size,
        listen: Vec::new(),
    };

    for v in args.listen.iter() {
        let lc: NetListenConfig = v
            .parse()
            .map_err(|e| format!("failed to parse listen: {}", e))?;

        let spec = match lc {
            NetListenConfig::Tcp(c) => {
                if let Some(k) = c.params.keys().next() {
                    return Err(format!("failed to parse listen: invalid param: {}", k).into());
                }

                ListenSpec::Tcp { addr: c.addr }
            }
            NetListenConfig::Unix(c) => {
                if let Some(k) = c.params.keys().next() {
                    return Err(format!("failed to parse listen: invalid param: {}", k).into());
                }

                ListenSpec::Local {
                    path: c.path,
                    mode: c.mode,
                    user: c.user,
                    group: c.group,
                }
            }
        };

        config.listen.push(spec);
    }

    run(&config)
}

fn main() {
    let matches = Command::new("pushpin-api")
        .version(version())
        .about("Pushpin control API")
        .arg(
            Arg::new("log-level")
                .long("log-level")
                .num_args(1)
                .value_name("N")
                .help("Log level")
                .default_value("2"),
        )
        .arg(
            Arg::new("maxconn")
                .long("maxconn")
                .num_args(1)
                .value_name("N")
                .help("Maximum number of concurrent connections")
                .default_value("50"),
        )
        .arg(
            Arg::new("buffer-size")
                .long("buffer-size")
                .num_args(1)
                .value_name("N")
                .help("Connection buffer size (two buffers per connection)")
                .default_value("8192"),
        )
        .arg(
            Arg::new("body-buffer-size")
                .long("body-buffer-size")
                .num_args(1)
                .value_name("N")
                .help("Body buffer size")
                .default_value("100000"),
        )
        .arg(
            Arg::new("listen")
                .long("listen")
                .num_args(1)
                .value_name("[addr:]port[,params...]")
                .action(ArgAction::Append)
                .help("Port to listen on"),
        )
        .get_matches();

    // Allow all log levels globally so individual loggers can limit as they choose
    log::set_max_level(LevelFilter::Trace);

    log::set_logger(get_simple_logger()).unwrap();

    get_simple_logger().set_max_level(LevelFilter::Info);

    let level = matches.get_one::<String>("log-level").unwrap();

    let level: usize = match level.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse log-level: {}", e);
            process::exit(1);
        }
    };

    let level = match level {
        0 => LevelFilter::Error,
        1 => LevelFilter::Warn,
        2 => LevelFilter::Info,
        3 => LevelFilter::Debug,
        4..=usize::MAX => LevelFilter::Trace,
        _ => unreachable!(),
    };

    get_simple_logger().set_max_level(level);

    local_offset_check();

    let maxconn = matches.get_one::<String>("maxconn").unwrap();

    let maxconn: usize = match maxconn.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse maxconn: {}", e);
            process::exit(1);
        }
    };

    let buffer_size = matches.get_one::<String>("buffer-size").unwrap();

    let buffer_size: usize = match buffer_size.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse buffer-size: {}", e);
            process::exit(1);
        }
    };

    let body_buffer_size = matches.get_one::<String>("body-buffer-size").unwrap();

    let body_buffer_size: usize = match body_buffer_size.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse body-buffer-size: {}", e);
            process::exit(1);
        }
    };

    let mut listen: Vec<String> = matches
        .get_many::<String>("listen")
        .unwrap_or_default()
        .map(|v| v.to_owned())
        .collect();

    // Default listen configuration
    if listen.is_empty() {
        listen.push("0.0.0.0:5561".to_string());
    }

    let args = Args {
        maxconn,
        buffer_size,
        body_buffer_size,
        listen,
    };

    if let Err(e) = process_args_and_run(args) {
        error!("{}", e);
        process::exit(1);
    }
}
