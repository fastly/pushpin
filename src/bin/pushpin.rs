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

use clap::Parser;
use log::{error, info, LevelFilter};
use pushpin::log::{ensure_init_simple_logger, get_simple_logger, local_offset_check};
use pushpin::runner::{open_log_file, ArgsData, CliArgs, Settings};
use pushpin::service::start_services;
use std::env;
use std::error::Error;
use std::process;

fn process_args_and_run(args: CliArgs) -> Result<(), Box<dyn Error>> {
    let args_data = ArgsData::new(args)?;
    let settings = Settings::new(&env::current_dir()?, args_data)?;

    let log_file = match settings.log_file.clone() {
        Some(x) => match open_log_file(x) {
            Ok(x) => Some(x),
            Err(_) => {
                error!("unable to open log file. logging to standard out.");
                None
            }
        },
        None => None,
    };
    ensure_init_simple_logger(log_file, true);
    log::set_logger(get_simple_logger()).unwrap();
    let ll = settings
        .log_levels
        .get("")
        .unwrap_or(settings.log_levels.get("default").unwrap());
    let level = match ll {
        0 => LevelFilter::Error,
        1 => LevelFilter::Warn,
        2 => LevelFilter::Info,
        3 => LevelFilter::Debug,
        4..=u8::MAX => LevelFilter::Trace,
    };
    log::set_max_level(level);

    local_offset_check();

    info!("using config: {:?}", settings.config_file.display());
    start_services(settings);

    Ok(())
}

fn main() {
    let args = CliArgs::parse();
    info!("starting...");

    if let Err(e) = process_args_and_run(args) {
        eprintln!("Error: {}", e);
        process::exit(1);
    }
}
