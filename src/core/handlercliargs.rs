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

use crate::core::config::get_config_file;
use crate::core::version;
use clap::{arg, Parser};
use std::env;
use std::ffi::CString;
use std::path::PathBuf;

// Struct to hold the command line arguments
#[derive(Parser, Debug)]
#[command(
    name= "Pushpin Handler",
    version = version(),
    about = "Pushpin handler component."
)]
pub struct CliArgs {
    /// Set path to the configuration file
    #[arg(short, long = "config", value_name = "file")]
    pub config_file: Option<String>,

    /// Set path to the log file
    #[arg(short = 'l', long = "logfile", value_name = "file")]
    pub log_file: Option<String>,

    /// Set log level (0=error, 1=warn, 2=info, 3=debug, 4=trace)
    #[arg(short = 'L', long = "loglevel", value_name = "x", default_value_t = 2, value_parser = clap::value_parser!(u32).range(1..=4))]
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
        let work_dir = env::current_dir().unwrap_or_default();
        let config_path: Option<PathBuf> = self.config_file.as_ref().map(PathBuf::from);

        // Resolve the config file path using get_config_file
        self.config_file = match get_config_file(&work_dir, config_path) {
            Ok(path) => Some(path.to_string_lossy().to_string()),
            Err(e) => {
                eprintln!("error: failed to find configuration file: {}", e);
                std::process::exit(1);
            }
        };

        self
    }

    pub fn to_ffi(&self) -> ffi::HandlerCliArgs {
        let config_file = self
            .config_file
            .as_ref()
            .map_or_else(
                || {
                    let work_dir = std::env::current_dir().unwrap_or_default();
                    let default_config = get_config_file(&work_dir, None)
                        .unwrap_or_else(|_| "examples/config/pushpin.conf".into());
                    CString::new(default_config.to_string_lossy().to_string()).unwrap()
                },
                |s| CString::new(s.as_str()).unwrap(),
            )
            .into_raw();

        let log_file = self
            .log_file
            .as_ref()
            .map_or_else(
                || CString::new("").unwrap(),
                |s| CString::new(s.as_str()).unwrap(),
            )
            .into_raw();

        let ipc_prefix = self
            .ipc_prefix
            .as_ref()
            .map_or_else(
                || CString::new("").unwrap(),
                |s| CString::new(s.as_str()).unwrap(),
            )
            .into_raw();

        ffi::HandlerCliArgs {
            config_file,
            log_file,
            log_level: self.log_level,
            ipc_prefix,
            port_offset: self.port_offset.map_or(-1, |p| p as i32),
        }
    }
}

pub mod ffi {
    #[repr(C)]
    pub struct HandlerCliArgs {
        pub config_file: *mut libc::c_char,
        pub log_file: *mut libc::c_char,
        pub log_level: libc::c_uint,
        pub ipc_prefix: *mut libc::c_char,
        pub port_offset: libc::c_int,
    }
}

/// Frees the memory allocated by handler_cli_args_to_ffi
/// MUST be called by C++ code when done with the HandlerCliArgs struct
///
/// # Safety
///
/// This function is unsafe because it takes ownership of raw pointers and frees their memory.
/// The caller must ensure that:
/// - The `ffi_args` struct was created by `handler_cli_args_to_ffi`
/// - Each pointer field in `ffi_args` is either null or points to valid memory allocated by `CString::into_raw()`
/// - No pointer in `ffi_args` is used after this function is called (double-free protection)
/// - This function is called exactly once per `HandlerCliArgs` instance
#[no_mangle]
pub unsafe extern "C" fn destroy_handler_cli_args(ffi_args: ffi::HandlerCliArgs) {
    if !ffi_args.config_file.is_null() {
        let _ = CString::from_raw(ffi_args.config_file);
    }
    if !ffi_args.log_file.is_null() {
        let _ = CString::from_raw(ffi_args.log_file);
    }
    if !ffi_args.ipc_prefix.is_null() {
        let _ = CString::from_raw(ffi_args.ipc_prefix);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::NamedTempFile;

    #[test]
    fn test_ccli_args() {
        // Create mock values
        let file = NamedTempFile::new().unwrap();
        let config_test_file = file.path().to_str().unwrap().to_string();

        let args = CliArgs {
            config_file: Some(config_test_file.clone()),
            log_file: Some("pushpin.log".to_string()),
            log_level: 3,
            ipc_prefix: Some("ipc".to_string()),
            port_offset: Some(8080),
        };

        let args_ffi = args.to_ffi();

        // Test verify() method
        let verified_args = args.verify();
        assert_eq!(verified_args.config_file, Some(config_test_file.clone()));
        assert_eq!(verified_args.log_file, Some("pushpin.log".to_string()));
        assert_eq!(verified_args.log_level, 3);
        assert_eq!(verified_args.ipc_prefix, Some("ipc".to_string()));
        assert_eq!(verified_args.port_offset, Some(8080));

        // Test conversion to C++-compatible struct
        unsafe {
            assert_eq!(
                std::ffi::CStr::from_ptr(args_ffi.config_file)
                    .to_str()
                    .unwrap(),
                config_test_file
            );
            assert_eq!(
                std::ffi::CStr::from_ptr(args_ffi.log_file)
                    .to_str()
                    .unwrap(),
                "pushpin.log"
            );
            assert_eq!(
                std::ffi::CStr::from_ptr(args_ffi.ipc_prefix)
                    .to_str()
                    .unwrap(),
                "ipc"
            );
        }
        assert_eq!(args_ffi.log_level, 3);
        assert_eq!(args_ffi.port_offset, 8080);

        // Test cleanup - this should not crash
        unsafe {
            destroy_handler_cli_args(args_ffi);
        }

        // Test with empty/default values
        let empty_args = CliArgs {
            config_file: None,
            log_file: None,
            log_level: 2,
            ipc_prefix: None,
            port_offset: None,
        };

        let empty_args_ffi = empty_args.to_ffi();

        // Test verify() with empty args
        let verified_empty_args = empty_args.verify();
        let default_config_file = get_config_file(&env::current_dir().unwrap(), None)
            .unwrap()
            .to_string_lossy()
            .to_string();
        assert_eq!(
            verified_empty_args.config_file,
            Some(default_config_file.clone())
        );
        assert_eq!(verified_empty_args.log_file, None);
        assert_eq!(verified_empty_args.log_level, 2);
        assert_eq!(verified_empty_args.ipc_prefix, None);
        assert_eq!(verified_empty_args.port_offset, None);

        // Test conversion to C++-compatible struct
        unsafe {
            assert_eq!(
                std::ffi::CStr::from_ptr(empty_args_ffi.config_file)
                    .to_str()
                    .unwrap(),
                default_config_file
            );
            assert_eq!(
                std::ffi::CStr::from_ptr(empty_args_ffi.log_file)
                    .to_str()
                    .unwrap(),
                ""
            );
            assert_eq!(
                std::ffi::CStr::from_ptr(empty_args_ffi.ipc_prefix)
                    .to_str()
                    .unwrap(),
                ""
            );
            assert_eq!(empty_args_ffi.log_level, 2);
            assert_eq!(empty_args_ffi.port_offset, -1);
        }

        // Test cleanup for empty args - this should not crash
        unsafe {
            destroy_handler_cli_args(empty_args_ffi);
        }
    }
}
