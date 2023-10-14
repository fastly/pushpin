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

use crate::runner::Settings;
use log::{error, LevelFilter};
use mpsc::{channel, Sender};
use signal_hook::consts::{SIGINT, SIGTERM, TERM_SIGNALS};
use signal_hook::iterator::Signals;
use std::io::{BufRead, BufReader};
use std::process::{ChildStderr, ChildStdout, Stdio};
use std::sync::mpsc;
use std::thread::JoinHandle;
use std::{process::Command, thread};
use url::Url;

pub enum ServiceError {
    TermSignal(String),
    ThreadError(String),
}

pub struct Service {
    pub name: String,
    pub log_level: u8,
}

pub fn start_services(settings: Settings) {
    let mut services: Vec<Box<dyn RunnerService>> = vec![];
    if settings.service_names.contains(&String::from("condure")) {
        services.push(Box::new(CondureService::new(&settings)));
    }
    if settings
        .service_names
        .contains(&String::from("pushpin-proxy"))
    {
        services.push(Box::new(PushpinProxyService::new(&settings)));
    }
    if settings
        .service_names
        .contains(&String::from("pushpin-handler"))
    {
        services.push(Box::new(PushpinHandlerService::new(&settings)));
    }

    let (sender, receiver) = channel();
    let mut threads: Vec<Option<JoinHandle<()>>> = vec![];
    for mut service in services {
        threads.extend(service.start(sender.clone()));
    }

    // Spawn a signal handling thread
    threads.push(Some(thread::spawn(move || {
        for signal in Signals::new(TERM_SIGNALS)
            .expect("Error creating signal iterator")
            .forever()
        {
            match signal {
                SIGINT | SIGTERM => {
                    sender
                        .send(Err(ServiceError::TermSignal(
                            "termination signal received".to_string(),
                        )))
                        .unwrap();
                    break;
                }
                _ => {}
            }
        }
    })));

    // Receive error messages from other threads.
    loop {
        match receiver.recv() {
            Ok(Err(ServiceError::ThreadError(error_message))) => {
                error!("error received: {}", error_message);
                break;
            }
            Ok(Err(ServiceError::TermSignal(error_message))) => {
                error!("signal received: {}", error_message);
                break;
            }
            Ok(_) => {}
            Err(_) => error!("Failed to receive error message from thread"),
        }
    }

    // Wait for all threads to finish.
    for thread in &mut threads {
        let thread = thread.take().unwrap();
        thread.join().unwrap();
    }
}

impl Service {
    fn new(name: String, log_level: u8) -> Self {
        Self { name, log_level }
    }
    pub fn start(
        &mut self,
        args: Vec<String>,
        sender: Sender<Result<(), ServiceError>>,
    ) -> Vec<Option<JoinHandle<()>>> {
        let name = self.name.clone();
        let name_str = self.name.clone();

        let level = match self.log_level {
            0 => LevelFilter::Error,
            1 => LevelFilter::Warn,
            2 => LevelFilter::Info,
            3 => LevelFilter::Debug,
            4..=u8::MAX => LevelFilter::Trace,
        };
        log::set_max_level(level);

        // Create a channel for sending thread handles back to main thread
        let (handle_sender, handle_receiver) = channel();

        let mut result: Vec<Option<JoinHandle<()>>> = Vec::new();

        result.push(Some(thread::spawn(move || {
            let mut command = Command::new(&args[0]);
            command.args(&args[1..]);

            // Capture stdout and stderr
            command.stdout(Stdio::piped());
            command.stderr(Stdio::piped());

            let mut child = command.spawn().expect("Failed to execute command");

            let stdout = child.stdout.take().unwrap();
            let stderr = child.stderr.take().unwrap();
            let handles = start_log_handler(stdout, stderr, name_str);

            // Send the handles back to main thread
            handle_sender.send(handles).unwrap();

            let status = child.wait().expect("Failed to wait for command");

            if status.success() {
                sender.send(Ok(())).unwrap();
            } else {
                let error_message = format!("Failed to start {} service.", name);
                sender
                    .send(Err(ServiceError::ThreadError(error_message)))
                    .unwrap();
            }
        })));
        // Receive the handles from the channel and add them to the result vector
        let received_handles: Vec<Option<JoinHandle<()>>> = handle_receiver.recv().unwrap();
        result.extend(received_handles);

        result
    }
}

pub trait RunnerService {
    fn start(&mut self, sender: Sender<Result<(), ServiceError>>) -> Vec<Option<JoinHandle<()>>>;
}

pub struct CondureService {
    args: Vec<String>,
    pub service: Service,
}

impl CondureService {
    pub fn new(settings: &Settings) -> Self {
        let mut args: Vec<String> = vec![];
        let service_name = "condure";

        args.push(settings.condure_bin.display().to_string());

        let log_level = match settings.log_levels.get(service_name) {
            Some(&x) => x,
            None => settings.log_levels.get("default").unwrap().to_owned(),
        };
        args.push(format!("--log-level={}", log_level));

        args.push(format!("--buffer-size={}", settings.client_buffer_size));
        args.push(format!(
            "--stream-maxconn={}",
            settings.client_max_connections
        ));
        if settings.allow_compression {
            args.push("--compression".to_string());
        }
        args.push(format!(
            "--zserver-stream=ipc://{}/{}condure-client",
            settings.run_dir.display(),
            settings.ipc_prefix
        ));
        args.push("--deny-out-internal".to_string());

        if !settings.ports.is_empty() {
            //server mode
            let mut using_ssl = false;

            for port in &settings.ports {
                if !port.local_path.is_empty() {
                    let mut arg = format!("--listen={},local,stream", port.local_path);
                    if port.mode >= 0 {
                        arg = format!("{},mode={}", arg, port.mode);
                    }
                    if !port.user.is_empty() {
                        arg = format!("{},user={}", arg, port.user);
                    }
                    if !port.group.is_empty() {
                        arg = format!("{},group={}", arg, port.group);
                    }
                    args.push(arg);
                } else {
                    let url_string = format!("http://{}:{}", port.ip, port.port);
                    let url = Url::parse(&url_string).expect("Failed to parse Condure URL");

                    let mut arg = format!("--listen={},stream", url.authority());

                    if port.ssl {
                        using_ssl = true;
                        arg = format!("{},tls,default-cert=default_{}", arg, port.port);
                    }
                    args.push(arg);
                }
            }

            args.push(format!(
                "--zclient-stream=ipc://{}/{}condure",
                settings.run_dir.display(),
                settings.ipc_prefix
            ));

            if using_ssl {
                args.push(format!(
                    "--tls-identities-dir={}",
                    settings.certs_dir.display()
                ));
            }
        }

        Self {
            service: Service::new(String::from(service_name), log_level),
            args,
        }
    }
}

impl RunnerService for CondureService {
    fn start(&mut self, sender: Sender<Result<(), ServiceError>>) -> Vec<Option<JoinHandle<()>>> {
        self.service.start(self.args.clone(), sender)
    }
}

pub struct PushpinProxyService {
    args: Vec<String>,
    pub service: Service,
}

impl PushpinProxyService {
    pub fn new(settings: &Settings) -> Self {
        let mut args: Vec<String> = vec![];
        let service_name = "proxy";

        args.push(settings.proxy_bin.display().to_string());
        args.push(format!("--config={}", settings.config_file.display()));

        if !settings.ipc_prefix.is_empty() {
            args.push(format!("--ipc-prefix={}", settings.ipc_prefix));
        }
        let log_level = match settings.log_levels.get("pushpin-proxy") {
            Some(&x) => x,
            None => settings.log_levels.get("default").unwrap().to_owned(),
        };
        args.push(format!("--loglevel={}", log_level));

        for route in settings.route_lines.clone() {
            args.push(format!("--route={}", route));
        }

        Self {
            service: Service::new(String::from(service_name), log_level),
            args,
        }
    }
}

impl RunnerService for PushpinProxyService {
    fn start(&mut self, sender: Sender<Result<(), ServiceError>>) -> Vec<Option<JoinHandle<()>>> {
        self.service.start(self.args.clone(), sender)
    }
}

pub struct PushpinHandlerService {
    args: Vec<String>,
    pub service: Service,
}

impl PushpinHandlerService {
    pub fn new(settings: &Settings) -> Self {
        let mut args: Vec<String> = vec![];
        let service_name = "handler";

        args.push(settings.handler_bin.display().to_string());
        args.push(format!("--config={}", settings.config_file.display()));

        if settings.port_offset > 0 {
            args.push(format!("--port-offset={}", settings.port_offset));
        }
        if !settings.ipc_prefix.is_empty() {
            args.push(format!("--ipc-prefix={}", settings.ipc_prefix));
        }
        let log_level = match settings.log_levels.get("pushpin-handler") {
            Some(&x) => x,
            None => settings.log_levels.get("default").unwrap().to_owned(),
        };
        args.push(format!("--loglevel={}", log_level));

        Self {
            service: Service::new(String::from(service_name), log_level),
            args,
        }
    }
}

impl RunnerService for PushpinHandlerService {
    fn start(&mut self, sender: Sender<Result<(), ServiceError>>) -> Vec<Option<JoinHandle<()>>> {
        self.service.start(self.args.clone(), sender)
    }
}

fn start_log_handler(
    stdout: ChildStdout,
    stderr: ChildStderr,
    name: String,
) -> Vec<Option<JoinHandle<()>>> {
    let mut result: Vec<Option<JoinHandle<()>>> = Vec::new();

    let name_str = name.clone();
    result.push(Some(thread::spawn(move || {
        let reader = BufReader::new(stdout);
        for line in reader.lines() {
            match line {
                Ok(msg) => log_message(&name_str, log::Level::Info, &msg),
                Err(_) => log_message(
                    &name_str,
                    log::Level::Error,
                    "failed to capture log message.",
                ),
            }
        }
    })));

    result.push(Some(thread::spawn(move || {
        let reader_err = BufReader::new(stderr);
        for line in reader_err.lines() {
            match line {
                Ok(msg) => log_message(&name, log::Level::Error, &msg),
                Err(_) => log_message(&name, log::Level::Error, "failed to capture log message."),
            }
        }
    })));

    result
}

fn log_message(name: &str, level: log::Level, msg: &str) {
    log::logger().log(
        &log::Record::builder()
            .level(level)
            .target(name)
            .args(format_args!("{}", msg))
            .build(),
    );
}
