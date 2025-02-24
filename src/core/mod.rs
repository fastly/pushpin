/*
 * Copyright (C) 2024 Fastly, Inc.
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

pub mod arena;
pub mod buffer;
pub mod channel;
pub mod config;
pub mod defer;
pub mod event;
pub mod eventloop;
pub mod executor;
pub mod fs;
pub mod http1;
pub mod io;
pub mod jwt;
pub mod list;
pub mod log;
pub mod net;
pub mod reactor;
pub mod select;
pub mod shuffle;
pub mod task;
pub mod time;
pub mod timer;
pub mod tnetstring;
pub mod waker;
pub mod zmq;

#[cfg(test)]
pub mod qtest;

use std::env;
use std::ffi::{CString, OsStr};
use std::os::unix::ffi::OsStrExt;

#[cfg(test)]
use std::path::{Path, PathBuf};

pub fn version() -> &'static str {
    env!("APP_VERSION")
}

/// # Safety
///
/// * `main_fn` must be safe to call.
pub unsafe fn call_c_main<I, S>(
    main_fn: unsafe extern "C" fn(libc::c_int, *const *const libc::c_char) -> libc::c_int,
    args: I,
) -> u8
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    let args: Vec<CString> = args
        .into_iter()
        .map(|s| CString::new(s.as_ref().as_bytes()).unwrap())
        .collect();
    let args: Vec<*const libc::c_char> = args.iter().map(|s| s.as_ptr()).collect();

    main_fn(args.len() as libc::c_int, args.as_ptr()) as u8
}

#[cfg(test)]
pub fn test_dir() -> PathBuf {
    // "cargo test" ensures this is present
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());

    out_dir.join("test-work")
}

#[cfg(test)]
pub fn ensure_example_config(dest: &Path) {
    use std::fs;
    use std::sync::Once;

    static INIT: Once = Once::new();

    INIT.call_once(|| {
        let src_dir = Path::new("examples").join("config");
        let dest_dir = dest.join("examples").join("config");

        fs::create_dir_all(&dest_dir).unwrap();
        fs::copy(src_dir.join("pushpin.conf"), dest_dir.join("pushpin.conf")).unwrap();
        fs::copy(src_dir.join("routes"), dest_dir.join("routes")).unwrap();
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::call_c_main;
    use crate::ffi;
    use std::ffi::OsStr;

    fn httpheaders_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::httpheaders_test, args) as u8 }
    }

    fn jwt_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::jwt_test, args) as u8 }
    }

    fn eventloop_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::eventloop_test, args) as u8 }
    }

    #[test]
    fn httpheaders() {
        assert!(qtest::run(httpheaders_test));
    }

    #[test]
    fn jwt() {
        assert!(qtest::run(jwt_test));
    }

    #[test]
    fn eventloop() {
        assert!(qtest::run(eventloop_test));
    }
}
