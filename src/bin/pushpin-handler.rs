/*
 * Copyright (C) 2023-2026 Fastly, Inc.
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
use log::{debug, error};
use pushpin::core::log::ffi::{log_init, log_set_level};
use pushpin::core::settings::{Settings, SettingsError};
use pushpin::handler::cliargs::{destroy_handler_cli_args, CliArgs};
use pushpin::handler::settings::{ffi, HandlerSettings};
use std::ffi::CString;
use std::path::Path;
use std::process::ExitCode;
use std::ptr;

fn load_settings(args: &CliArgs, config_file: &str) -> Result<HandlerSettings, SettingsError> {
    let mut settings = Settings::load(Path::new(config_file))?;

    if let Some(ipc_prefix) = args.ipc_prefix.as_deref().filter(|p| !p.is_empty()) {
        settings.set_ipc_prefix(ipc_prefix);
    }

    if let Some(port_offset) = args.port_offset {
        settings.set_port_offset(port_offset as i32);
    }

    HandlerSettings::from_settings(&settings)
}

fn main() -> ExitCode {
    let cli_args = CliArgs::parse().verify();

    let log_file = cli_args
        .log_file
        .as_deref()
        .filter(|f| !f.is_empty())
        .map(|f| CString::new(f).unwrap());

    // SAFETY: the pointer is either null or a valid C string
    if unsafe { log_init(log_file.as_ref().map_or(ptr::null(), |f| f.as_ptr())) } != 0 {
        return ExitCode::from(1);
    }

    log_set_level(cli_args.log_level as i32);

    debug!("starting...");

    let config_file = cli_args
        .config_file
        .clone()
        .expect("verify sets the config file");

    let settings = match load_settings(&cli_args, &config_file) {
        Ok(settings) => settings.to_ffi(),
        Err(e) => {
            error!("failed to load config file {config_file}: {e}");
            return ExitCode::from(1);
        }
    };

    let cli_args_ffi = cli_args.to_ffi();

    let exit_code = unsafe {
        ffi::handler_init(
            &cli_args_ffi as *const _ as *const ffi::HandlerCliArgs,
            &settings,
        )
    };

    // Clean up the allocated memory
    unsafe { destroy_handler_cli_args(cli_args_ffi) };

    ExitCode::from(exit_code as u8)
}
