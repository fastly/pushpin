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
use log::{info, LevelFilter};
use pushpin::log::get_simple_logger;
use pushpin::runner::{ArgsData, CliArgs, Settings};
use pushpin::service::start_services;
use std::error::Error;
use std::process;

fn process_args_and_run(args: CliArgs) -> Result<(), Box<dyn Error>> {
    let args_data = ArgsData::new(args)?;
    let settings = Settings::new(args_data)?;

    let settings = Settings::new(args_data, &config_file)?;
    info!("using config: {:?}", settings.config_file.display());
    start_services(settings);
    //To be implemented in the next PR

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
