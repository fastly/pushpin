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
use pushpin::config::get_config_file;
use pushpin::runner::{ArgsData, CliArgs, Settings};
use std::error::Error;
use std::process;

fn process_args_and_run(args: CliArgs) -> Result<(), Box<dyn Error>> {
    let args_data = ArgsData::new(args)?;
    let config_file = get_config_file(args_data.config_file.as_ref())?;
    println!("using config: {:?}", config_file.as_os_str());
    let settings = Settings::new(args_data, &config_file)?;
    println!("settings: {:?}", settings);
    //To be implemented in the next PR

    Ok(())
}

fn main() {
    let args = CliArgs::parse();
    println!("starting...");

    if let Err(e) = process_args_and_run(args) {
        eprintln!("Error: {}", e);
        process::exit(1);
    }
}
