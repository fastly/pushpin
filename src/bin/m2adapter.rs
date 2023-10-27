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

use pushpin::call_c_main;
use std::process::ExitCode;

#[cfg(target_os = "macos")]
#[link(name = "pushpin-cpp")]
#[link(name = "QtCore", kind = "framework")]
#[link(name = "QtNetwork", kind = "framework")]
extern "C" {
    fn m2adapter_main(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
}

#[cfg(not(target_os = "macos"))]
#[link(name = "pushpin-cpp")]
#[link(name = "Qt5Core")]
#[link(name = "Qt5Network")]
extern "C" {
    fn m2adapter_main(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
}

fn main() -> ExitCode {
    unsafe { ExitCode::from(call_c_main(m2adapter_main)) }
}
