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

pub mod item;
pub mod server;

use self::server::Server;
use crate::core::zmq::SpecInfo;
use log::{debug, info};
use signal_hook;
use signal_hook::consts::TERM_SIGNALS;
use signal_hook::iterator::Signals;
use std::error::Error;
use std::path::PathBuf;
use std::sync::atomic::AtomicBool;
use std::sync::Arc;

pub enum ListenSpec {
    Tcp {
        addr: std::net::SocketAddr,
    },
    Local {
        path: PathBuf,
        mode: Option<u32>,
        user: Option<String>,
        group: Option<String>,
    },
}

pub struct Config {
    pub maxconn: usize,
    pub buffer_size: usize,
    pub body_buffer_size: usize,
    pub content_max: usize,
    pub listen: Vec<ListenSpec>,
    pub item_out: Vec<String>,
    pub item_out_bind: bool,
    pub ipc_file_mode: u32,
}

pub struct App {
    _server: Server,
    _zmq_context: zmq::Context,
}

impl App {
    pub fn new(config: &Config) -> Result<Self, String> {
        if config.maxconn < 1 {
            return Err("maxconn must be >= 1".into());
        }

        if config.listen.len() != 1 {
            return Err("exactly one listen config must be specified".into());
        }

        let mut item_out_specs = Vec::new();

        for spec in config.item_out.iter() {
            item_out_specs.push(SpecInfo {
                spec: spec.clone(),
                bind: config.item_out_bind,
                ipc_file_mode: config.ipc_file_mode,
            });
        }

        let zmq_context = zmq::Context::new();

        let server = Server::new(
            config.maxconn,
            config.buffer_size,
            config.body_buffer_size,
            config.content_max,
            &config.listen[0],
            &zmq_context,
            &item_out_specs,
        )?;

        Ok(Self {
            _server: server,
            _zmq_context: zmq_context,
        })
    }

    pub fn wait_for_term(&self) {
        let mut signals = Signals::new(TERM_SIGNALS).unwrap();

        let term_now = Arc::new(AtomicBool::new(false));

        // Ensure two term signals in a row causes the app to immediately exit
        for signal_type in TERM_SIGNALS {
            signal_hook::flag::register_conditional_shutdown(
                *signal_type,
                1, // Exit code
                Arc::clone(&term_now),
            )
            .unwrap();

            signal_hook::flag::register(*signal_type, Arc::clone(&term_now)).unwrap();
        }

        // Wait for termination
        let signal_type = signals.into_iter().next().unwrap();
        assert!(TERM_SIGNALS.contains(&signal_type));
    }
}

pub fn run(config: &Config) -> Result<(), Box<dyn Error>> {
    debug!("starting...");

    {
        let a = match App::new(config) {
            Ok(a) => a,
            Err(e) => {
                return Err(e.into());
            }
        };

        info!("started");

        a.wait_for_term();

        info!("stopping...");
    }

    debug!("stopped");

    Ok(())
}
