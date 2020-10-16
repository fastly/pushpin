/*
 * Copyright (C) 2020 Fanout, Inc.
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

use crate::server::{Server, MSG_RETAINED_MAX};
use crate::zhttppacket;
use crate::zhttpsocket;
use crate::zmq::SpecInfo;
use log::info;
use signal_hook;
use signal_hook::iterator::Signals;
use std::cmp;
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

fn make_specs(base: &str) -> Result<(String, String, String), String> {
    if base.starts_with("ipc:") {
        Ok((
            format!("{}-{}", base, "out"),
            format!("{}-{}", base, "out-stream"),
            format!("{}-{}", base, "in"),
        ))
    } else if base.starts_with("tcp:") {
        match base.rfind(':') {
            Some(pos) => match base[(pos + 1)..base.len()].parse::<u16>() {
                Ok(port) => Ok((
                    format!("{}:{}", &base[..pos], port),
                    format!("{}:{}", &base[..pos], port + 1),
                    format!("{}:{}", &base[..pos], port + 2),
                )),
                Err(e) => Err(format!("error parsing tcp port in base spec: {}", e)),
            },
            None => Err("tcp base spec must specify port".into()),
        }
    } else {
        Err("base spec must be ipc or tcp".into())
    }
}

pub struct Config {
    pub instance_id: String,
    pub workers: usize,
    pub req_maxconn: usize,
    pub stream_maxconn: usize,
    pub buffer_size: usize,
    pub body_buffer_size: usize,
    pub messages_max: usize,
    pub req_timeout: Duration,
    pub stream_timeout: Duration,
    pub listen: Vec<(SocketAddr, bool)>, // bool=stream
    pub zclient_req: Vec<String>,
    pub zclient_stream: Vec<String>,
    pub zclient_connect: bool,
    pub ipc_file_mode: usize,
}

pub struct App {
    _server: Server,
}

impl App {
    pub fn new(config: &Config) -> Result<Self, String> {
        // usage to avoid dead code warnings elsewhere
        zhttppacket::Request::new_error(b"", &[], "bogus");
        zhttppacket::Request::new_handoff_start(b"", &[]);

        let zmq_context = Arc::new(zmq::Context::new());

        // set hwm to 5% of maxconn
        let hwm = cmp::max((config.req_maxconn + config.stream_maxconn) / 20, 1);

        let mut zsockman = zhttpsocket::SocketManager::new(
            Arc::clone(&zmq_context),
            &config.instance_id,
            MSG_RETAINED_MAX * config.workers,
            hwm,
            cmp::max(hwm / config.workers, 1),
        );

        let mut any_req = false;
        let mut any_stream = false;

        for (_, stream) in config.listen.iter() {
            if *stream {
                any_stream = true;
            } else {
                any_req = true;
            }
        }

        if any_req {
            let mut specs = Vec::new();

            for spec in config.zclient_req.iter() {
                if config.zclient_connect {
                    info!("zhttp client connect {}", spec);
                } else {
                    info!("zhttp client bind {}", spec);
                }

                specs.push(SpecInfo {
                    spec: spec.clone(),
                    bind: !config.zclient_connect,
                    ipc_file_mode: config.ipc_file_mode,
                });
            }

            if let Err(e) = zsockman.set_client_req_specs(&specs) {
                return Err(format!("failed to set zhttp client req specs: {}", e));
            }
        }

        if any_stream {
            let mut out_specs = Vec::new();
            let mut out_stream_specs = Vec::new();
            let mut in_specs = Vec::new();

            for spec in config.zclient_stream.iter() {
                let (out_spec, out_stream_spec, in_spec) = make_specs(spec)?;

                if config.zclient_connect {
                    info!(
                        "zhttp client connect {} {} {}",
                        out_spec, out_stream_spec, in_spec
                    );
                } else {
                    info!(
                        "zhttp client bind {} {} {}",
                        out_spec, out_stream_spec, in_spec
                    );
                }

                out_specs.push(SpecInfo {
                    spec: out_spec,
                    bind: !config.zclient_connect,
                    ipc_file_mode: config.ipc_file_mode,
                });

                out_stream_specs.push(SpecInfo {
                    spec: out_stream_spec,
                    bind: !config.zclient_connect,
                    ipc_file_mode: config.ipc_file_mode,
                });

                in_specs.push(SpecInfo {
                    spec: in_spec,
                    bind: !config.zclient_connect,
                    ipc_file_mode: config.ipc_file_mode,
                });
            }

            if let Err(e) =
                zsockman.set_client_stream_specs(&out_specs, &out_stream_specs, &in_specs)
            {
                return Err(format!("failed to set zhttp client stream specs: {}", e));
            }
        }

        let server = Server::new(
            &config.instance_id,
            config.workers,
            config.req_maxconn,
            config.stream_maxconn,
            config.buffer_size,
            config.body_buffer_size,
            config.messages_max,
            config.req_timeout,
            config.stream_timeout,
            &config.listen,
            zsockman,
        )?;

        Ok(Self { _server: server })
    }

    pub fn wait_for_term(&self) {
        let signal_types = &[signal_hook::SIGTERM, signal_hook::SIGINT];

        let signals = Signals::new(signal_types).unwrap();

        // remove the signal handlers after the first invocation
        for s in signal_types.iter() {
            signal_hook::cleanup::register(*s, signal_types.to_vec()).unwrap();
        }

        // wait for termination
        for signal in &signals {
            match signal {
                signal_hook::SIGTERM | signal_hook::SIGINT => {
                    break;
                }
                _ => unreachable!(),
            }
        }
    }
}
