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

use clap::{Parser, arg};
use pushpin::core::{call_c_main, version};
use pushpin::core::config::get_config_file;
use pushpin::import_cpp;
use std::env;
use std::ffi::{OsString};
use std::path::PathBuf;
use std::process::ExitCode;

import_cpp! {
    fn handler_main(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
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
    #[arg(short, long, value_name = "file")]
    pub config_file: Option<String>,

    /// Set path to the log file
    #[arg(short = 'l', long, value_name = "file")]
    pub log_file: Option<String>,

    /// Set log level (0=error, 1=warn, 2=info, 3=debug, 4=trace)
    #[arg(short = 'L', long, value_name = "x", default_value = "2", value_parser = clap::value_parser!(u32).range(1..=4))]
    pub log_level: u32,

    /// Override ipc_prefix config option, which is used to add a prefix to all ZeroMQ IPC filenames
    #[arg(long, value_name = "prefix")]
    pub ipc_prefix: Option<String>,

    /// Override port_offset config option, which is used to increment all ZeroMQ TCP ports and the HTTP control server port
    #[arg(long, value_name = "offset", value_parser = clap::value_parser!(u32))]
    pub port_offset: Option<u32>,
}
impl CliArgs {
    /// Verifies the command line arguments.
    pub fn verify(mut self) -> Self {
        // Get current working directory
        let work_dir = env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
        
        // Convert config_file Option<String> to Option<PathBuf>
        let config_path = self.config_file.as_ref().map(|s| PathBuf::from(s));
        
        // Use get_config_file to find the config file
        self.config_file = match get_config_file(&work_dir, config_path) {
            Ok(path) => Some(path.to_string_lossy().to_string()),
            Err(e) => {
                eprintln!("error: failed to find configuration file: {}", e);
                std::process::exit(1);
            }
        };

        self
    }

    pub fn into_osstring_vec(self) -> Vec<OsString> {
        self.into_iter()
            .map(|(_, value)| OsString::from(value))
            .collect()
    }
}

impl IntoIterator for CliArgs {
    type Item = (String, String);
    type IntoIter = std::vec::IntoIter<Self::Item>;

    fn into_iter(self) -> Self::IntoIter {
        let mut args: Vec<(String, String)> = vec![];

        args.push((
            "config-file".to_string(), 
            self.config_file.unwrap_or_else(|| "".to_string())
        ));

        args.push((
            "log-file".to_string(),
            self.log_file.as_ref().map(|s| s.to_string()).unwrap_or_else(|| "".to_string()),
        ));

        args.push(("log-level".to_string(), self.log_level.to_string()));

        args.push((
            "ipc-prefix".to_string(),
            self.ipc_prefix.as_ref().map(|s| s.to_string()).unwrap_or_else(|| "".to_string()),
        ));

        args.push((
            "port-offset".to_string(),
            self.port_offset.as_ref().map(|s| s.to_string()).unwrap_or_else(|| "".to_string()),
        ));

        args.into_iter()
    }
}

fn main() -> ExitCode {
    let cli_args = CliArgs::parse().verify();

    unsafe { 
        ExitCode::from(call_c_main(handler_main, cli_args.into_osstring_vec()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::NamedTempFile;

    #[test]
    fn test_cli_args() {
        // Create mock config file
        let file = NamedTempFile::new().unwrap();
        let config_test_file = file.path().to_str().unwrap().to_string();

        // Test the verification of command line arguments
        let args = CliArgs {
            config_file: Some(config_test_file.clone()),
            log_file: Some("pushpin.log".to_string()),
            log_level: 3,
            ipc_prefix: Some("ipc".to_string()),
            port_offset: Some(8080),
        };
        let verified_args = args.verify();
        assert_eq!(verified_args.config_file, Some(config_test_file.clone()));
        assert_eq!(verified_args.log_file, Some("pushpin.log".to_string()));
        assert_eq!(verified_args.log_level, 3);
        assert_eq!(verified_args.ipc_prefix, Some("ipc".to_string()));
        assert_eq!(verified_args.port_offset, Some(8080));
        
        // Test the conversion to OsString vector
        let osstring_vec = verified_args.into_osstring_vec();
        assert_eq!(osstring_vec.len(), 5);
        assert_eq!(osstring_vec[0], OsString::from(config_test_file));
        assert_eq!(osstring_vec[1], OsString::from("pushpin.log"));
        assert_eq!(osstring_vec[2], OsString::from("3"));
        assert_eq!(osstring_vec[3], OsString::from("ipc"));
        assert_eq!(osstring_vec[4], OsString::from("8080"));
        
        // Test empty command line arguments
        let empty_args = CliArgs {
            config_file: None,
            log_file: None,
            log_level: 2,
            ipc_prefix: None,
            port_offset: None,
        };
        let verified_empty_args = empty_args.verify();
        let default_config_file = get_config_file(&env::current_dir().unwrap(), None).unwrap().to_string_lossy().to_string();
        assert_eq!(verified_empty_args.config_file, Some(default_config_file.clone()));
        assert_eq!(verified_empty_args.log_file, None);
        assert_eq!(verified_empty_args.log_level, 2);
        assert_eq!(verified_empty_args.ipc_prefix, None);
        assert_eq!(verified_empty_args.port_offset, None);

        // Test the conversion to OsString vector
        let empty_osstring_vec = verified_empty_args.into_osstring_vec();
        assert_eq!(empty_osstring_vec.len(), 5);
        assert_eq!(empty_osstring_vec[0], OsString::from(default_config_file));
        assert_eq!(empty_osstring_vec[1], OsString::from(""));
        assert_eq!(empty_osstring_vec[2], OsString::from("2"));
        assert_eq!(empty_osstring_vec[3], OsString::from(""));
        assert_eq!(empty_osstring_vec[4], OsString::from(""));
    }
}