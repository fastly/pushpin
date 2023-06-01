/*
 * Copyright (C) 2020-2023 Fanout, Inc.
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

use clap::{crate_version, Arg, ArgAction, Command};
use condure::app;
use log::{error, Level, LevelFilter, Metadata, Record};
use std::error::Error;
use std::io;
use std::mem;
use std::path::PathBuf;
use std::process;
use std::str;
use std::sync::Once;
use std::time::Duration;
use time::macros::format_description;
use time::{OffsetDateTime, UtcOffset};

// safety values
const WORKERS_MAX: usize = 1024;
const CONNS_MAX: usize = 10_000_000;

const PRIVATE_SUBNETS: &[&str] = &[
    "127.0.0.0/8",
    "10.0.0.0/8",
    "172.16.0.0/12",
    "192.168.0.0/16",
    "169.254.0.0/16",
    "::1/128",
    "fc00::/7",
    "fe80::/10",
];

struct SimpleLogger {
    local_offset: UtcOffset,
}

impl log::Log for SimpleLogger {
    fn enabled(&self, metadata: &Metadata) -> bool {
        metadata.level() <= Level::Trace
    }

    fn log(&self, record: &Record) {
        if !self.enabled(record.metadata()) {
            return;
        }

        let now = OffsetDateTime::now_utc().to_offset(self.local_offset);

        let format = format_description!(
            "[year]-[month]-[day] [hour]:[minute]:[second].[subsecond digits:3]"
        );

        let mut ts = [0u8; 64];

        let size = {
            let mut ts = io::Cursor::new(&mut ts[..]);

            now.format_into(&mut ts, &format)
                .expect("failed to write timestamp");

            ts.position() as usize
        };

        let ts = str::from_utf8(&ts[..size]).expect("timestamp is not utf-8");

        let lname = match record.level() {
            log::Level::Error => "ERR",
            log::Level::Warn => "WARN",
            log::Level::Info => "INFO",
            log::Level::Debug => "DEBUG",
            log::Level::Trace => "TRACE",
        };

        println!("[{}] {} [{}] {}", lname, ts, record.target(), record.args());
    }

    fn flush(&self) {}
}

static mut LOGGER: mem::MaybeUninit<SimpleLogger> = mem::MaybeUninit::uninit();

fn get_simple_logger() -> &'static SimpleLogger {
    static INIT: Once = Once::new();

    unsafe {
        INIT.call_once(|| {
            let local_offset =
                UtcOffset::current_local_offset().expect("failed to get local time offset");

            LOGGER.write(SimpleLogger { local_offset });
        });

        LOGGER.as_ptr().as_ref().unwrap()
    }
}

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
    zserver_req_specs: Vec<String>,
    zserver_stream_specs: Vec<String>,
    zserver_connect: bool,
    ipc_file_mode: u32,
    tls_identities_dir: String,
    allow_compression: bool,
    deny_out_internal: bool,
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
        zserver_req: args.zserver_req_specs,
        zserver_stream: args.zserver_stream_specs,
        zserver_connect: args.zserver_connect,
        ipc_file_mode: args.ipc_file_mode,
        certs_dir: PathBuf::from(args.tls_identities_dir),
        allow_compression: args.allow_compression,
        deny: Vec::new(),
    };

    for v in args.listen.iter() {
        let mut parts = v.split(',');

        // there's always a first part
        let part1 = parts.next().unwrap();

        let mut stream = true;
        let mut tls = false;
        let mut default_cert = None;
        let mut local = false;
        let mut mode = None;
        let mut user = None;
        let mut group = None;

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
                "local" => local = true,
                "mode" => match u32::from_str_radix(v, 8) {
                    Ok(x) => mode = Some(x),
                    Err(e) => return Err(format!("failed to parse mode: {}", e).into()),
                },
                "user" => user = Some(String::from(v)),
                "group" => group = Some(String::from(v)),
                _ => return Err(format!("failed to parse listen: invalid param: {}", part).into()),
            }
        }

        let spec = if local {
            app::ListenSpec::Local {
                path: PathBuf::from(part1),
                mode,
                user,
                group,
            }
        } else {
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

            app::ListenSpec::Tcp {
                addr,
                tls,
                default_cert,
            }
        };

        config.listen.push(app::ListenConfig { spec, stream });
    }

    if args.deny_out_internal {
        for s in PRIVATE_SUBNETS.iter() {
            config.deny.push(s.parse().unwrap());
        }
    }

    condure::run(&config)
}

fn main() {
    let matches = Command::new("condure")
        .version(crate_version!())
        .about("HTTP/WebSocket connection manager")
        .arg(
            Arg::new("log-level")
                .long("log-level")
                .num_args(1)
                .value_name("N")
                .help("Log level")
                .default_value("2"),
        )
        .arg(
            Arg::new("id")
                .long("id")
                .num_args(1)
                .value_name("ID")
                .help("Instance ID")
                .default_value("condure"),
        )
        .arg(
            Arg::new("workers")
                .long("workers")
                .num_args(1)
                .value_name("N")
                .help("Number of worker threads")
                .default_value("2"),
        )
        .arg(
            Arg::new("req-maxconn")
                .long("req-maxconn")
                .num_args(1)
                .value_name("N")
                .help("Maximum number of concurrent connections in req mode")
                .default_value("100"),
        )
        .arg(
            Arg::new("stream-maxconn")
                .long("stream-maxconn")
                .num_args(1)
                .value_name("N")
                .help("Maximum number of concurrent connections in stream mode")
                .default_value("10000"),
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
                .help("Body buffer size for connections in req mode")
                .default_value("100000"),
        )
        .arg(
            Arg::new("messages-max")
                .long("messages-max")
                .num_args(1)
                .value_name("N")
                .help("Maximum number of queued WebSocket messages per connection")
                .default_value("100"),
        )
        .arg(
            Arg::new("req-timeout")
                .long("req-timeout")
                .num_args(1)
                .value_name("N")
                .help("Connection timeout in req mode (seconds)")
                .default_value("30"),
        )
        .arg(
            Arg::new("stream-timeout")
                .long("stream-timeout")
                .num_args(1)
                .value_name("N")
                .help("Connection timeout in stream mode (seconds)")
                .default_value("1800"),
        )
        .arg(
            Arg::new("listen")
                .long("listen")
                .num_args(1)
                .value_name("[addr:]port[,params...]")
                .action(ArgAction::Append)
                .help("Port to listen on"),
        )
        .arg(
            Arg::new("zclient-req")
                .long("zclient-req")
                .num_args(1)
                .value_name("spec")
                .action(ArgAction::Append)
                .help("ZeroMQ client REQ spec")
                .default_value("ipc://client"),
        )
        .arg(
            Arg::new("zclient-stream")
                .long("zclient-stream")
                .num_args(1)
                .value_name("spec-base")
                .action(ArgAction::Append)
                .help("ZeroMQ client PUSH/ROUTER/SUB spec base")
                .default_value("ipc://client"),
        )
        .arg(
            Arg::new("zclient-connect")
                .long("zclient-connect")
                .action(ArgAction::SetTrue)
                .help("ZeroMQ client sockets should connect instead of bind"),
        )
        .arg(
            Arg::new("zserver-req")
                .long("zserver-req")
                .num_args(1)
                .value_name("spec")
                .action(ArgAction::Append)
                .help("ZeroMQ server REQ spec"),
        )
        .arg(
            Arg::new("zserver-stream")
                .long("zserver-stream")
                .num_args(1)
                .value_name("spec-base")
                .action(ArgAction::Append)
                .help("ZeroMQ server PULL/ROUTER/PUB spec base"),
        )
        .arg(
            Arg::new("zserver-connect")
                .long("zserver-connect")
                .action(ArgAction::SetTrue)
                .help("ZeroMQ server sockets should connect instead of bind"),
        )
        .arg(
            Arg::new("ipc-file-mode")
                .long("ipc-file-mode")
                .num_args(1)
                .value_name("octal")
                .help("Permissions for ZeroMQ IPC binds"),
        )
        .arg(
            Arg::new("tls-identities-dir")
                .long("tls-identities-dir")
                .num_args(1)
                .value_name("directory")
                .help("Directory containing certificates and private keys")
                .default_value("."),
        )
        .arg(
            Arg::new("compression")
                .long("compression")
                .action(ArgAction::SetTrue)
                .help("Allow compression to be used"),
        )
        .arg(
            Arg::new("deny-out-internal")
                .long("deny-out-internal")
                .action(ArgAction::SetTrue)
                .help("Block outbound connections to local/internal IP address ranges"),
        )
        .arg(
            Arg::new("sizes")
                .long("sizes")
                .action(ArgAction::SetTrue)
                .help("Prints sizes of tasks and other objects"),
        )
        .get_matches();

    log::set_logger(get_simple_logger()).unwrap();

    log::set_max_level(LevelFilter::Info);

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
        4..=core::usize::MAX => LevelFilter::Trace,
        _ => unreachable!(),
    };

    log::set_max_level(level);

    if *matches.get_one("sizes").unwrap() {
        for (name, size) in condure::app::App::sizes() {
            println!("{}: {} bytes", name, size);
        }
        process::exit(0);
    }

    let id = matches.get_one::<String>("id").unwrap();

    let workers = matches.get_one::<String>("workers").unwrap();

    let workers: usize = match workers.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse workers: {}", e);
            process::exit(1);
        }
    };

    let req_maxconn = matches.get_one::<String>("req-maxconn").unwrap();

    let req_maxconn: usize = match req_maxconn.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse req-maxconn: {}", e);
            process::exit(1);
        }
    };

    let stream_maxconn = matches.get_one::<String>("stream-maxconn").unwrap();

    let stream_maxconn: usize = match stream_maxconn.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse stream-maxconn: {}", e);
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

    let messages_max = matches.get_one::<String>("messages-max").unwrap();

    let messages_max: usize = match messages_max.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse messages-max: {}", e);
            process::exit(1);
        }
    };

    let req_timeout = matches.get_one::<String>("req-timeout").unwrap();

    let req_timeout: usize = match req_timeout.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse req-timeout: {}", e);
            process::exit(1);
        }
    };

    let stream_timeout = matches.get_one::<String>("stream-timeout").unwrap();

    let stream_timeout: usize = match stream_timeout.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse stream-timeout: {}", e);
            process::exit(1);
        }
    };

    let mut listen: Vec<String> = matches
        .get_many::<String>("listen")
        .unwrap_or_default()
        .map(|v| v.to_owned())
        .collect();

    let zclient_req_specs: Vec<String> = matches
        .get_many::<String>("zclient-req")
        .unwrap()
        .map(|v| v.to_owned())
        .collect();

    let zclient_stream_specs: Vec<String> = matches
        .get_many::<String>("zclient-stream")
        .unwrap()
        .map(|v| v.to_owned())
        .collect();

    let zclient_connect = *matches.get_one("zclient-connect").unwrap();

    let zserver_req_specs: Vec<String> = matches
        .get_many::<String>("zserver-req")
        .unwrap_or_default()
        .map(|v| v.to_owned())
        .collect();

    let zserver_stream_specs: Vec<String> = matches
        .get_many::<String>("zserver-stream")
        .unwrap_or_default()
        .map(|v| v.to_owned())
        .collect();

    let zserver_connect = *matches.get_one("zserver-connect").unwrap();

    let ipc_file_mode = matches
        .get_one::<String>("ipc-file-mode")
        .cloned()
        .unwrap_or(String::from("0"));

    let ipc_file_mode = match u32::from_str_radix(&ipc_file_mode, 8) {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse ipc-file-mode: {}", e);
            process::exit(1);
        }
    };

    let tls_identities_dir = matches.get_one::<String>("tls-identities-dir").unwrap();

    let allow_compression = *matches.get_one("compression").unwrap();

    let deny_out_internal = *matches.get_one("deny-out-internal").unwrap();

    // if no zmq server specs are set (needed by client mode), specify
    // default listen configuration in order to enable server mode. this
    // means if zmq server specs are set, then server mode won't be enabled
    // by default
    if listen.is_empty() && zserver_req_specs.is_empty() && zserver_stream_specs.is_empty() {
        listen.push("0.0.0.0:8000,stream".to_string());
    }

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
        zserver_req_specs,
        zserver_stream_specs,
        zserver_connect,
        ipc_file_mode,
        tls_identities_dir: tls_identities_dir.to_string(),
        allow_compression,
        deny_out_internal,
    };

    if let Err(e) = process_args_and_run(args) {
        error!("{}", e);
        process::exit(1);
    }
}
