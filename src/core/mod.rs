/*
 * Copyright (C) 2024-2025 Fastly, Inc.
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
pub mod test;
pub mod time;
pub mod timer;
pub mod tnetstring;
pub mod waker;
pub mod zmq;
pub mod ccliargs;

use std::env;
use std::ffi::{CString, OsStr};
use std::os::unix::ffi::OsStrExt;

#[cfg(test)]
use std::path::{Path, PathBuf};

pub fn is_debug_build() -> bool {
    cfg!(debug_assertions)
}

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

mod ffi {
    use std::os::raw::c_int;

    #[no_mangle]
    pub extern "C" fn is_debug_build() -> c_int {
        if super::is_debug_build() {
            1
        } else {
            0
        }
    }
}

#[cfg(test)]
mod tests {
    use crate::core::test::{run_serial, TestException};
    use crate::ffi;

    fn httpheaders_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::httpheaders_test(out_ex) == 0 }
    }

    fn jwt_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::jwt_test(out_ex) == 0 }
    }

    fn timer_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::timer_test(out_ex) == 0 }
    }

    fn defercall_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::defercall_test(out_ex) == 0 }
    }

    fn tcpstream_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::tcpstream_test(out_ex) == 0 }
    }

    fn unixstream_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::unixstream_test(out_ex) == 0 }
    }

    fn eventloop_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::eventloop_test(out_ex) == 0 }
    }


    #[test]
    fn httpheaders() {
        run_serial(httpheaders_test);
    }

    #[test]
    fn jwt() {
        run_serial(jwt_test);
    }

    #[test]
    fn timer() {
        run_serial(timer_test);
    }

    #[test]
    fn defercall() {
        run_serial(defercall_test);
    }

    #[test]
    fn tcpstream() {
        run_serial(tcpstream_test);
    }

    #[test]
    fn unixstream() {
        run_serial(unixstream_test);
    }

    #[test]
    fn eventloop() {
        run_serial(eventloop_test);
    }
}
