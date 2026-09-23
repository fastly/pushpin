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

use clap::{Arg, Command};
use log::{error, LevelFilter};
use pushpin::api::{run, Config};
use pushpin::core::log::{get_simple_logger, local_offset_check};
use pushpin::core::version;
use std::error::Error;
use std::process;

// Safety value
const WORKERS_MAX: usize = 1024;

struct Args {
    workers: usize,
}

fn process_args_and_run(args: Args) -> Result<(), Box<dyn Error>> {
    if args.workers > WORKERS_MAX {
        return Err("failed to parse workers: value too large".into());
    }

    let config = Config {
        _workers: args.workers,
    };

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
            Arg::new("workers")
                .long("workers")
                .num_args(1)
                .value_name("N")
                .help("Number of worker threads")
                .default_value("2"),
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

    let workers = matches.get_one::<String>("workers").unwrap();

    let workers: usize = match workers.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse workers: {}", e);
            process::exit(1);
        }
    };

    let args = Args { workers };

    if let Err(e) = process_args_and_run(args) {
        error!("{}", e);
        process::exit(1);
    }
}
