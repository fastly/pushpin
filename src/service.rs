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
use log::{error, info};
use mpsc::{channel, Sender};
use signal_hook::consts::{SIGINT, SIGTERM, TERM_SIGNALS};
use signal_hook::iterator::Signals;
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
}

pub fn start_services(settings: Settings) {
    let mut services: Vec<Box<dyn RunnerService>> = vec![];
    if settings.service_names.contains(&String::from("condure")) {
        services.push(Box::new(CondureService::new(&settings)));
    }

    let (sender, receiver) = channel();
    let mut threads: Vec<Option<JoinHandle<()>>> = vec![];
    for mut service in services {
        threads.push(service.start(sender.clone()));
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
    fn new(name: String) -> Self {
        Self { name }
    }

    pub fn start(
        &mut self,
        args: Vec<String>,
        sender: Sender<Result<(), ServiceError>>,
    ) -> Option<thread::JoinHandle<()>> {
        info!("starting {}", self.name);

        let name = self.name.clone();

        Some(thread::spawn(move || {
            let mut command = Command::new(args[0].clone());
            command.args(&args[1..]);

            let status = command.status().expect("Failed to execute command");

            if status.success() {
                sender.send(Ok(())).unwrap();
            } else {
                let error_message = format!("Failed to start {} service.", name);
                sender
                    .send(Err(ServiceError::ThreadError(error_message)))
                    .unwrap();
            }
        }))
    }
}

pub trait RunnerService {
    fn start(&mut self, sender: Sender<Result<(), ServiceError>>) -> Option<JoinHandle<()>>;
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

        let ll = settings
            .log_levels
            .get(service_name)
            .unwrap_or(settings.log_levels.get("default").unwrap());

        args.push(format!("--log-level={}", ll.to_owned()));
        args.push(format!("--buffer-size={}", settings.client_buffer_size));
        args.push(format!(
            "--stream-maxconn={}",
            settings.client_max_connections
        ));
        if settings.allow_compression {
            args.push("--compression".to_string());
        }
        if Self::has_client_mode(settings.condure_bin.display().to_string()) {
            // client mode
            args.push(format!(
                "--zserver-stream=ipc://{}/{}condure-client",
                settings.run_dir.display(),
                settings.ipc_prefix
            ));
            args.push("--deny-out-internal".to_string());
        }
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
            service: Service::new(String::from(service_name)),
            args,
        }
    }

    fn has_client_mode(condure_bin: String) -> bool {
        let result: Result<std::process::Output, std::io::Error> =
            Command::new(condure_bin).arg("--help").output();

        match result {
            Ok(output) => {
                if !output.status.success() {
                    error!("Condure returned non-zero status: {}", output.status);
                    return false;
                }
                let stdout = String::from_utf8_lossy(&output.stdout);
                stdout.contains("--zserver-stream")
            }
            Err(e) => {
                error!("Failed to run condure: process error: {}", e);
                false
            }
        }
    }
}

impl RunnerService for CondureService {
    fn start(&mut self, sender: Sender<Result<(), ServiceError>>) -> Option<JoinHandle<()>> {
        self.service.start(self.args.clone(), sender)
    }
}
