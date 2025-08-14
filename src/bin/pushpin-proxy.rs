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
use pushpin::import_cpp;
use pushpin::proxy::proxycliargs::ffi::ProxyCliArgs;
use pushpin::proxy::proxycliargs::{destroy_proxy_cli_args, CliArgs};
use std::process::ExitCode;

import_cpp! {
    fn proxy_main(args: *const ProxyCliArgs) -> libc::c_int;
}

fn main() -> ExitCode {
    let cli_args = CliArgs::parse().verify();
    let cli_args_ffi = cli_args.to_ffi();

    let exit_code = unsafe { proxy_main(&cli_args_ffi) };

    // Clean up the allocated memory
    unsafe { destroy_proxy_cli_args(cli_args_ffi) };

    ExitCode::from(exit_code as u8)
}
