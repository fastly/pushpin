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
    #[arg(short, long, value_name = "file")]
    pub config_file: Option<String>,

    /// Set path to the log file
    #[arg(short = 'l', long, value_name = "file")]
    pub log_file: Option<String>,

    /// Set log level (0=error, 1=warn, 2=info, 3=debug, 4=trace)
    #[arg(short = 'L', long, value_name = "x", default_value_t = 2, value_parser = clap::value_parser!(u32).range(1..=4))]
    pub log_level: u32,

    /// Override ipc_prefix config option, which is used to add a prefix to all ZeroMQ IPC filenames
    #[arg(long, value_name = "prefix")]
    pub ipc_prefix: Option<String>,

    /// Override port_offset config option, which is used to increment all ZeroMQ TCP ports and the HTTP control server port
    #[arg(long, value_name = "offset", value_parser = clap::value_parser!(u32))]
    pub port_offset: Option<u32>,

    /// Add routes (overrides routes file)
    #[arg(long, value_name = "routes")]
    pub routes: Option<Vec<String>>,

    /// Log update checks in Zurl as debug level
    #[arg(long, value_name = "quiet-check", default_value_t = false)]
    pub quiet_check: bool,
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

    pub fn to_ffi(&self) -> ffi::CliArgsFfi {
        ffi::handler_cli_args_to_ffi(self)
    }
}

pub mod ffi {
    use std::ffi::CString;

    #[repr(C)]
    pub struct CliArgsFfi {
        pub config_file: *mut libc::c_char,
        pub log_file: *mut libc::c_char,
        pub log_level: libc::c_uint,
        pub ipc_prefix: *mut libc::c_char,
        pub port_offset: libc::c_int,
        pub routes: *mut *mut libc::c_char,
        pub routes_count: libc::c_uint,
        pub quiet_check: libc::c_int,
    }

    // Converts CliArgs to a C++-compatible struct
    #[no_mangle]
    pub extern "C" fn handler_cli_args_to_ffi(args: &super::CliArgs) -> CliArgsFfi {
        let config_file = args
            .config_file
            .as_ref()
            .map_or_else(
                || {
                    let work_dir = std::env::current_dir().unwrap_or_default();
                    let default_config = super::get_config_file(&work_dir, None)
                        .unwrap_or_else(|_| "examples/config/pushpin.conf".into());
                    CString::new(default_config.to_string_lossy().to_string()).unwrap()
                },
                |s| CString::new(s.as_str()).unwrap(),
            )
            .into_raw();

        let log_file = args
            .log_file
            .as_ref()
            .map_or_else(
                || CString::new("").unwrap(),
                |s| CString::new(s.as_str()).unwrap(),
            )
            .into_raw();

        let ipc_prefix = args
            .ipc_prefix
            .as_ref()
            .map_or_else(
                || CString::new("").unwrap(),
                |s| CString::new(s.as_str()).unwrap(),
            )
            .into_raw();

        let (routes, routes_count) = match &args.routes {
            Some(routes_vec) if !routes_vec.is_empty() => {
                // Allocate array of string pointers
                let routes_array = unsafe {
                    libc::malloc(routes_vec.len() * std::mem::size_of::<*mut libc::c_char>())
                        as *mut *mut libc::c_char
                };

                // Convert each route to CString and store pointer in array
                for i in 0..routes_vec.len() {
                    let c_string = CString::new(routes_vec[i].to_string()).unwrap().into_raw();
                    unsafe {
                        *routes_array.add(i) = c_string;
                    }
                }

                (routes_array, routes_vec.len() as libc::c_uint)
            }
            _ => {
                let routes_array = unsafe { libc::malloc(0) as *mut *mut libc::c_char };
                (routes_array, 0)
            }
        };

        CliArgsFfi {
            config_file,
            log_file,
            log_level: args.log_level,
            ipc_prefix,
            port_offset: args.port_offset.map_or(-1, |p| p as i32),
            routes,
            routes_count,
            quiet_check: if args.quiet_check { 1 } else { 0 },
        }
    }

    /// Frees the memory allocated by handler_cli_args_to_ffi
    /// MUST be called by C++ code when done with the CliArgsFfi struct
    #[no_mangle]
    pub unsafe extern "C" fn destroy_handler_cli_args(ffi_args: CliArgsFfi) {
        if !ffi_args.config_file.is_null() {
            let _ = CString::from_raw(ffi_args.config_file);
        }
        if !ffi_args.log_file.is_null() {
            let _ = CString::from_raw(ffi_args.log_file);
        }
        if !ffi_args.ipc_prefix.is_null() {
            let _ = CString::from_raw(ffi_args.ipc_prefix);
        }
        if !ffi_args.routes.is_null() {
            // Free each individual route string
            for i in 0..ffi_args.routes_count {
                let route_ptr = *ffi_args.routes.add(i as usize);
                if !route_ptr.is_null() {
                    let _ = CString::from_raw(route_ptr);
                }
            }

            libc::free(ffi_args.routes as *mut libc::c_void);
        }
    }
}

impl IntoIterator for CliArgs {
    type Item = (String, String);
    type IntoIter = std::vec::IntoIter<Self::Item>;

    fn into_iter(self) -> Self::IntoIter {
        let args: Vec<(String, String)> = vec![
            (
                "config-file".to_string(),
                self.config_file.unwrap_or_default(),
            ),
            ("log-file".to_string(), self.log_file.unwrap_or_default()),
            ("log-level".to_string(), self.log_level.to_string()),
            (
                "ipc-prefix".to_string(),
                self.ipc_prefix.unwrap_or_default(),
            ),
            (
                "port-offset".to_string(),
                self.port_offset.map_or("".to_string(), |p| p.to_string()),
            ),
            (
                "routes".to_string(),
                self.routes.map_or("".to_string(), |r| r.join(",")),
            ),
            ("quiet-check".to_string(), self.quiet_check.to_string()),
        ];

        args.into_iter()
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
            routes: Some(vec!["route1".to_string(), "route2".to_string()]),
            quiet_check: true,
        };

        let args_ffi = ffi::handler_cli_args_to_ffi(&args);

        // Test verify() method
        let verified_args = args.verify();
        assert_eq!(verified_args.config_file, Some(config_test_file.clone()));
        assert_eq!(verified_args.log_file, Some("pushpin.log".to_string()));
        assert_eq!(verified_args.log_level, 3);
        assert_eq!(verified_args.ipc_prefix, Some("ipc".to_string()));
        assert_eq!(verified_args.port_offset, Some(8080));
        assert_eq!(
            verified_args.routes,
            Some(vec!["route1".to_string(), "route2".to_string()])
        );
        assert_eq!(verified_args.quiet_check, true);

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

            // Test routes array
            assert_eq!(args_ffi.routes_count, 2);
            let routes_array = args_ffi.routes;
            assert_eq!(
                std::ffi::CStr::from_ptr(*routes_array.add(0))
                    .to_str()
                    .unwrap(),
                "route1"
            );
            assert_eq!(
                std::ffi::CStr::from_ptr(*routes_array.add(1))
                    .to_str()
                    .unwrap(),
                "route2"
            );
        }
        assert_eq!(args_ffi.log_level, 3);
        assert_eq!(args_ffi.port_offset, 8080);
        assert_eq!(args_ffi.quiet_check, 1);

        // Test with empty/default values
        let empty_args = CliArgs {
            config_file: None,
            log_file: None,
            log_level: 2,
            ipc_prefix: None,
            port_offset: None,
            routes: None,
            quiet_check: false,
        };

        let empty_args_ffi = ffi::handler_cli_args_to_ffi(&empty_args);

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
        assert_eq!(verified_empty_args.routes, None);
        assert_eq!(verified_empty_args.quiet_check, false);

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
            assert_eq!(empty_args_ffi.routes_count, 0);
            assert_eq!(empty_args_ffi.log_level, 2);
            assert_eq!(empty_args_ffi.port_offset, -1);
            assert_eq!(empty_args_ffi.quiet_check, 0);
        }
    }
}
