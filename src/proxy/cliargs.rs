/*
 * Copyright (C) 2025 Fastly, Inc.
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

use crate::core::config::default_config_file;
use crate::core::version;
use clap::{arg, Parser};
use std::ffi::{c_char, CString};
use std::path::PathBuf;
use std::slice;

/// Struct to hold the command line arguments
#[derive(Parser, Debug)]
#[command(
    name= "pushpin-proxy",
    version = version(),
    about = "Pushpin proxy component."
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

    /// Set verbose output; same as --loglevel=3
    #[arg(long = "verbose")]
    pub verbose: bool,

    /// Add a prefix to all ZeroMQ IPC filenames
    #[arg(long, value_name = "prefix")]
    pub ipc_prefix: Option<String>,

    /// Add route (overrides routes file)
    #[arg(long, value_name = "route line")]
    pub route: Vec<String>,
}

impl CliArgs {
    /// Verifies the command line arguments.
    pub fn verify(mut self) -> Self {
        let config_file = self
            .config_file
            .map(PathBuf::from)
            .unwrap_or_else(default_config_file);

        // FIXME: don't put back as a string after resolving
        self.config_file = Some(
            config_file
                .to_str()
                .expect("path sourced from string should convert")
                .to_string(),
        );

        if self.verbose {
            self.log_level = 3;
        }

        self
    }

    pub fn to_ffi(&self) -> ffi::ProxyCliArgs {
        let config_file = self
            .config_file
            .as_ref()
            .map_or_else(
                || CString::new("").unwrap(),
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

        let mut routes: Vec<*mut c_char> = Vec::new();

        // Convert each route to CString and store pointer in array
        for item in &self.route {
            let item = item.as_bytes();

            // Ensure no trailing nul byte
            let end = item.iter().position(|b| *b == 0).unwrap_or(item.len());
            let item = &item[..end];

            let c_string = CString::new(item).unwrap().into_raw();
            routes.push(c_string);
        }

        let routes = routes.into_boxed_slice();

        let routes_count = routes.len();
        let routes: *mut [*mut c_char] = Box::into_raw(routes);

        ffi::ProxyCliArgs {
            config_file,
            log_file,
            log_level: self.log_level,
            ipc_prefix,
            routes: routes as *mut *mut c_char,
            routes_count,
        }
    }
}

pub mod ffi {
    use std::ffi::{c_char, c_uint};

    #[repr(C)]
    pub struct ProxyCliArgs {
        pub config_file: *mut c_char,
        pub log_file: *mut c_char,
        pub log_level: c_uint,
        pub ipc_prefix: *mut c_char,
        pub routes: *mut *mut c_char,
        pub routes_count: libc::size_t,
    }
}

/// Frees the memory allocated by proxy_cli_args_to_ffi
/// MUST be called by C++ code when done with the ProxyCliArgs struct
///
/// # Safety
///
/// This function is unsafe because it takes ownership of raw pointers and frees their memory.
/// The caller must ensure that:
/// - The `ffi_args` struct was created by `proxy_cli_args_to_ffi`
/// - Each pointer field in `ffi_args` is either null or points to valid memory allocated by `CString::into_raw()` or `libc::malloc()`
/// - The `routes` array and its individual string elements were allocated properly
/// - No pointer in `ffi_args` is used after this function is called (double-free protection)
/// - This function is called exactly once per `ProxyCliArgs` instance
#[no_mangle]
pub unsafe extern "C" fn destroy_proxy_cli_args(ffi_args: ffi::ProxyCliArgs) {
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
        // SAFETY: the raw parts were originally derived from a boxed slice
        let routes = unsafe {
            Box::from_raw(slice::from_raw_parts_mut(
                ffi_args.routes,
                ffi_args.routes_count,
            ))
        };

        // Free each individual route string
        for item in routes.iter() {
            let _ = CString::from_raw(*item);
        }
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
            verbose: false,
            ipc_prefix: Some("ipc".to_string()),
            route: vec!["route1".to_string(), "route2".to_string()],
        };

        // Test verify() method
        let verified_args = args.verify();
        assert_eq!(verified_args.config_file, Some(config_test_file.clone()));
        assert_eq!(verified_args.log_file, Some("pushpin.log".to_string()));
        assert_eq!(verified_args.log_level, 3);
        assert_eq!(verified_args.verbose, false);
        assert_eq!(verified_args.ipc_prefix, Some("ipc".to_string()));
        assert_eq!(
            verified_args.route,
            vec!["route1".to_string(), "route2".to_string()]
        );

        let args_ffi = verified_args.to_ffi();

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

        // Test cleanup - this should not crash
        unsafe {
            destroy_proxy_cli_args(args_ffi);
        }

        // Test with empty/default values
        let empty_args = CliArgs {
            config_file: None,
            log_file: None,
            log_level: 2,
            verbose: false,
            ipc_prefix: None,
            route: Vec::new(),
        };

        // Test verify() with empty args
        let verified_empty_args = empty_args.verify();
        let default_config_file = default_config_file().to_str().unwrap().to_string();
        assert_eq!(
            verified_empty_args.config_file,
            Some(default_config_file.clone())
        );
        assert_eq!(verified_empty_args.log_file, None);
        assert_eq!(verified_empty_args.log_level, 2);
        assert_eq!(verified_empty_args.verbose, false);
        assert_eq!(verified_empty_args.ipc_prefix, None);
        assert!(verified_empty_args.route.is_empty());

        let empty_args_ffi = verified_empty_args.to_ffi();

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
        }

        // Test cleanup for empty args - this should not crash
        unsafe {
            destroy_proxy_cli_args(empty_args_ffi);
        }

        // Test verbose
        let args = CliArgs {
            config_file: None,
            log_file: None,
            log_level: 2,
            verbose: true,
            ipc_prefix: None,
            route: Vec::new(),
        };

        // Test verify() method
        let verified_args = args.verify();
        assert_eq!(verified_args.log_level, 3);
    }
}
