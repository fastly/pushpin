/*
 * Copyright (C) 2020-2022 Fanout, Inc.
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

use clap::{crate_version, App, Arg};
use condure::app;
use log::{error, Level, LevelFilter, Metadata, Record};
use std::error::Error;
use std::path::PathBuf;
use std::process;
use std::time::Duration;

// safety values
const WORKERS_MAX: usize = 1024;
const CONNS_MAX: usize = 10_000_000;

struct SimpleLogger;

impl log::Log for SimpleLogger {
    fn enabled(&self, metadata: &Metadata) -> bool {
        metadata.level() <= Level::Trace
    }

    fn log(&self, record: &Record) {
        if !self.enabled(record.metadata()) {
            return;
        }

        let now = time::now();
        let tm_fmt = now.strftime("%Y-%m-%d %H:%M:%S").unwrap();
        let millis = now.tm_nsec / 1_000_000;

        let lname = match record.level() {
            log::Level::Error => "ERR",
            log::Level::Warn => "WARN",
            log::Level::Info => "INFO",
            log::Level::Debug => "DEBUG",
            log::Level::Trace => "TRACE",
        };

        println!(
            "[{}] {}.{:03} [{}] {}",
            lname,
            tm_fmt,
            millis,
            record.target(),
            record.args()
        );
    }

    fn flush(&self) {}
}

static LOGGER: SimpleLogger = SimpleLogger;

struct Args {
    id: String,
    workers: usize,
    req_maxconn: usize,
    stream_maxconn: usize,
    buffer_size: usize,
    body_buffer_size: usize,
    messages_max: usize,
    req_timeout: usize,
    stream_timeout: usize,
    listen: Vec<String>,
    zclient_req_specs: Vec<String>,
    zclient_stream_specs: Vec<String>,
    zclient_connect: bool,
    ipc_file_mode: usize,
    tls_identities_dir: String,
}

fn process_args_and_run(args: Args) -> Result<(), Box<dyn Error>> {
    if args.id.is_empty() || args.id.contains(" ") {
        return Err("failed to parse id: value cannot be empty or contain a space".into());
    }

    if args.workers > WORKERS_MAX {
        return Err("failed to parse workers: value too large".into());
    }

    if args.req_maxconn + args.stream_maxconn > CONNS_MAX {
        return Err("total maxconn is too large".into());
    }

    let mut config = app::Config {
        instance_id: args.id,
        workers: args.workers,
        req_maxconn: args.req_maxconn,
        stream_maxconn: args.stream_maxconn,
        buffer_size: args.buffer_size,
        body_buffer_size: args.body_buffer_size,
        messages_max: args.messages_max,
        req_timeout: Duration::from_secs(args.req_timeout as u64),
        stream_timeout: Duration::from_secs(args.stream_timeout as u64),
        listen: Vec::new(),
        zclient_req: args.zclient_req_specs,
        zclient_stream: args.zclient_stream_specs,
        zclient_connect: args.zclient_connect,
        ipc_file_mode: args.ipc_file_mode,
        certs_dir: PathBuf::from(args.tls_identities_dir),
    };

    for v in args.listen.iter() {
        let mut parts = v.split(',');

        // there's always a first part
        let part1 = parts.next().unwrap();

        let port_pos = match part1.rfind(':') {
            Some(pos) => pos + 1,
            None => 0,
        };

        let port = &part1[port_pos..];
        if port.parse::<u16>().is_err() {
            return Err(format!("failed to parse listen: invalid port {}", port).into());
        }

        let addr = if port_pos > 0 {
            String::from(part1)
        } else {
            format!("0.0.0.0:{}", part1)
        };

        let addr = match addr.parse() {
            Ok(addr) => addr,
            Err(e) => {
                return Err(format!("failed to parse listen: {}", e).into());
            }
        };

        let mut stream = true;
        let mut tls = false;
        let mut default_cert = None;

        for part in parts {
            let (k, v) = match part.find('=') {
                Some(pos) => (&part[..pos], &part[(pos + 1)..]),
                None => (part, ""),
            };

            match k {
                "req" => stream = false,
                "stream" => stream = true,
                "tls" => tls = true,
                "default-cert" => default_cert = Some(String::from(v)),
                _ => return Err(format!("failed to parse listen: invalid param: {}", part).into()),
            }
        }

        config.listen.push(app::ListenConfig {
            spec: app::ListenSpec::Tcp {
                addr,
                tls,
                default_cert,
            },
            stream,
        });
    }

    condure::run(&config)
}

fn main() {
    let matches = App::new("condure")
        .version(crate_version!())
        .about("HTTP/WebSocket connection manager")
        .arg(
            Arg::with_name("log-level")
                .long("log-level")
                .takes_value(true)
                .value_name("N")
                .help("Log level")
                .default_value("2"),
        )
        .arg(
            Arg::with_name("id")
                .long("id")
                .takes_value(true)
                .value_name("ID")
                .help("Instance ID")
                .default_value("condure"),
        )
        .arg(
            Arg::with_name("workers")
                .long("workers")
                .takes_value(true)
                .value_name("N")
                .help("Number of worker threads")
                .default_value("2"),
        )
        .arg(
            Arg::with_name("req-maxconn")
                .long("req-maxconn")
                .takes_value(true)
                .value_name("N")
                .help("Maximum number of concurrent connections in req mode")
                .default_value("100"),
        )
        .arg(
            Arg::with_name("stream-maxconn")
                .long("stream-maxconn")
                .takes_value(true)
                .value_name("N")
                .help("Maximum number of concurrent connections in stream mode")
                .default_value("10000"),
        )
        .arg(
            Arg::with_name("buffer-size")
                .long("buffer-size")
                .takes_value(true)
                .value_name("N")
                .help("Connection buffer size (two buffers per connection)")
                .default_value("8192"),
        )
        .arg(
            Arg::with_name("body-buffer-size")
                .long("body-buffer-size")
                .takes_value(true)
                .value_name("N")
                .help("Body buffer size for connections in req mode")
                .default_value("100000"),
        )
        .arg(
            Arg::with_name("messages-max")
                .long("messages-max")
                .takes_value(true)
                .value_name("N")
                .help("Maximum number of queued WebSocket messages per connection")
                .default_value("100"),
        )
        .arg(
            Arg::with_name("req-timeout")
                .long("req-timeout")
                .takes_value(true)
                .value_name("N")
                .help("Client timeout in req mode (seconds)")
                .default_value("30"),
        )
        .arg(
            Arg::with_name("stream-timeout")
                .long("stream-timeout")
                .takes_value(true)
                .value_name("N")
                .help("Client timeout in stream mode (seconds)")
                .default_value("1800"),
        )
        .arg(
            Arg::with_name("listen")
                .long("listen")
                .takes_value(true)
                .value_name("[addr:]port[,params...]")
                .multiple(true)
                .help("Port to listen on")
                .default_value("0.0.0.0:8000,stream"),
        )
        .arg(
            Arg::with_name("zclient-req")
                .long("zclient-req")
                .takes_value(true)
                .value_name("spec")
                .multiple(true)
                .help("ZeroMQ client REQ spec")
                .default_value("ipc://client"),
        )
        .arg(
            Arg::with_name("zclient-stream")
                .long("zclient-stream")
                .takes_value(true)
                .value_name("spec-base")
                .multiple(true)
                .help("ZeroMQ client PUSH/ROUTER/SUB spec base")
                .default_value("ipc://client"),
        )
        .arg(
            Arg::with_name("zclient-connect")
                .long("zclient-connect")
                .help("ZeroMQ client sockets should connect instead of bind"),
        )
        .arg(
            Arg::with_name("ipc-file-mode")
                .long("ipc-file-mode")
                .takes_value(true)
                .value_name("octal")
                .help("Permissions for ZeroMQ IPC binds"),
        )
        .arg(
            Arg::with_name("tls-identities-dir")
                .long("tls-identities-dir")
                .takes_value(true)
                .value_name("directory")
                .help("Directory containing certificates and private keys")
                .default_value("."),
        )
        .get_matches();

    log::set_logger(&LOGGER).unwrap();

    log::set_max_level(LevelFilter::Info);

    let level = matches.value_of("log-level").unwrap();

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
        4..=core::usize::MAX => LevelFilter::Trace,
        _ => unreachable!(),
    };

    log::set_max_level(level);

    let id = matches.value_of("id").unwrap();

    let workers = matches.value_of("workers").unwrap();

    let workers: usize = match workers.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse workers: {}", e);
            process::exit(1);
        }
    };

    let req_maxconn = matches.value_of("req-maxconn").unwrap();

    let req_maxconn: usize = match req_maxconn.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse req-maxconn: {}", e);
            process::exit(1);
        }
    };

    let stream_maxconn = matches.value_of("stream-maxconn").unwrap();

    let stream_maxconn: usize = match stream_maxconn.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse stream-maxconn: {}", e);
            process::exit(1);
        }
    };

    let buffer_size = matches.value_of("buffer-size").unwrap();

    let buffer_size: usize = match buffer_size.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse buffer-size: {}", e);
            process::exit(1);
        }
    };

    let body_buffer_size = matches.value_of("body-buffer-size").unwrap();

    let body_buffer_size: usize = match body_buffer_size.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse body-buffer-size: {}", e);
            process::exit(1);
        }
    };

    let messages_max = matches.value_of("messages-max").unwrap();

    let messages_max: usize = match messages_max.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse messages-max: {}", e);
            process::exit(1);
        }
    };

    let req_timeout = matches.value_of("req-timeout").unwrap();

    let req_timeout: usize = match req_timeout.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse req-timeout: {}", e);
            process::exit(1);
        }
    };

    let stream_timeout = matches.value_of("stream-timeout").unwrap();

    let stream_timeout: usize = match stream_timeout.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse stream-timeout: {}", e);
            process::exit(1);
        }
    };

    let listen = matches
        .values_of("listen")
        .unwrap()
        .map(String::from)
        .collect();

    let zclient_req_specs = matches
        .values_of("zclient-req")
        .unwrap()
        .map(String::from)
        .collect();

    let zclient_stream_specs = matches
        .values_of("zclient-stream")
        .unwrap()
        .map(String::from)
        .collect();

    let zclient_connect = matches.is_present("zclient-connect");

    let ipc_file_mode = matches.value_of("ipc-file-mode").unwrap_or("0");

    let ipc_file_mode: usize = match ipc_file_mode.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse ipc-file-mode: {}", e);
            process::exit(1);
        }
    };

    let tls_identities_dir = matches.value_of("tls-identities-dir").unwrap();

    let args = Args {
        id: id.to_string(),
        workers,
        req_maxconn,
        stream_maxconn,
        buffer_size,
        body_buffer_size,
        messages_max,
        req_timeout,
        stream_timeout,
        listen,
        zclient_req_specs,
        zclient_stream_specs,
        zclient_connect,
        ipc_file_mode,
        tls_identities_dir: tls_identities_dir.to_string(),
    };

    if let Err(e) = process_args_and_run(args) {
        error!("{}", e);
        process::exit(1);
    }
}
