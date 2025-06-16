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

use pushpin::core::call_c_main;
use pushpin::import_cpp;
use std::env;
use std::process::ExitCode;
use pushpin::core::ccliargs::CCliArgs;
use clap::Parser;

import_cpp! {
    fn proxy_main(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
}

fn main() -> ExitCode {
    let c_cli_args = CCliArgs::parse().verify();
    
    unsafe { ExitCode::from(call_c_main(proxy_main, env::args_os())) }
}
