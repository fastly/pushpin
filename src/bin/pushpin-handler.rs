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

use clap::builder::Str;
use clap::{Parser, arg};
use pushpin::core::{call_c_main, version};
use pushpin::import_cpp;
use std::env;
use std::ffi::CString;
use std::process::ExitCode;

// import_cpp! {
//     fn handler_main(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
// }

import_cpp! {
    fn handler_main(args: *const CCliArgs) -> libc::c_int;
}


// Struct to hold the command line arguments
#[derive(Parser, Debug)]
#[command(
    name= "Pushpin Handler",
    version = version(),
    about = "Pushpin handler component."
)]
pub struct CliArgs {
    /// Set path to the configuration file
    #[arg(short, long, value_name = "file", default_value = "pushpin.conf")]
    pub config_file: String,

    /// Set path to the log file
    #[arg(short = 'l', long, value_name = "file")]
    pub log_file: Option<String>,

    /// Set log level
    #[arg(short = 'L', long, value_name = "x", default_value = "2")]
    pub log_level: String,

    /// Override ipc_prefix config option, which is used to add a prefix to all ZeroMQ IPC filenames
    #[arg(long, value_name = "prefix")]
    pub ipc_prefix: Option<String>,

    /// Override port_offset config option, which is used to increment all ZeroMQ TCP ports and the HTTP control server port
    #[arg(long, value_name = "offset")]
    pub port_offset: Option<String>,

    /// Enable verbose output. Same as --loglevel=3.
    #[arg(short, long, action = clap::ArgAction::SetTrue, default_value_t = false)]
    pub verbose: bool,
}

impl CliArgs {
    /// Verifies the command line arguments and returns a new instance of `CliArgs`.
    pub fn verify(self) -> Self {
        // Check if the configuration file exists and is readable
        // Check if log file is specified and valid
        // Check if log level is within the valid range
        // Check if ipc_prefix is a valid string
        // Check if port_offset is within the valid range
        self
    }

    pub fn into_c_struct(self) -> CArgsData {
        CArgsData {
            config_file: CString::new(self.config_file).unwrap().into_raw(),
            log_file: self.log_file.as_ref().map_or_else(
                || CString::new("").unwrap().into_raw(),
                |s| CString::new(s).unwrap().into_raw(),
            ),
            log_level: self.log_level.parse::<libc::c_int>().unwrap_or(2),
            ipc_prefix: self.ipc_prefix.as_ref().map_or_else(
                || CString::new("").unwrap().into_raw(),
                |s| CString::new(s).unwrap().into_raw(),
            ),
            port_offset: self.port_offset
                .as_ref()
                .and_then(|s| s.parse::<libc::c_int>().ok())
                .unwrap_or(-1),
            verbose: self.verbose,
        }
    }
}

impl IntoIterator for CliArgs {
    type Item = (String, String);
    type IntoIter = std::vec::IntoIter<Self::Item>;

    fn into_iter(self) -> Self::IntoIter {
        let mut args: Vec<(String, String)> = vec![];

        args.push(("config-file".to_string(), self.config_file));

        if let Some(log_file) = self.log_file {
            args.push(("log-file".to_string(), log_file));
        }

        args.push(("log-level".to_string(), self.log_level));

        if let Some(ipc_prefix) = self.ipc_prefix {
            args.push(("ipc-prefix".to_string(), ipc_prefix));
        }

        if let Some(port_offset) = self.port_offset {
            args.push(("port-offset".to_string(), port_offset));
        }

        if self.verbose {
            args.push(("verbose".to_string(), "true".to_string()));
        }

        args.into_iter()
    }
}

// C-compatible struct that matches CliArgs in handlerapp.cpp
#[repr(C)]
pub struct CCliArgs {
    pub config_file: *const libc::c_char,
    pub log_file: *const libc::c_char,
    pub log_level: libc::c_int,
    pub ipc_prefix: *const libc::c_char,
    pub port_offset: libc::c_int,
    pub verbose: bool,
}

// Modify the handler_main function signature to accept our struct
// import_cpp! {
//     fn handler_main_with_args(args: *const CArgsData) -> libc::c_int;
// }

fn main() -> ExitCode {
    let cli_args = CliArgs::parse().verify();

    // Call the C++ function with our struct
    unsafe { 
        // We need to keep the CStrings alive until the C++ function returns
        let result = handler_main(&cli_args.into_c_struct());
        ExitCode::from(result)
    }

    // unsafe { ExitCode::from(call_c_main(handler_main, env::args_os())) }
}
