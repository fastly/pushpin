/*
 * Copyright (C) 2023-2025 Fastly, Inc.
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

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

#[derive(Default)]
pub struct TestException {
    pub file: String,
    pub line: u32,
    pub message: String,
}

fn get_root_dir() -> &'static Path {
    static ROOT_DIR: OnceLock<PathBuf> = OnceLock::new();
    const VAR: &str = "CARGO_MANIFEST_DIR";

    ROOT_DIR.get_or_init(|| {
        let manifest_dir = env::var(VAR).unwrap_or_else(|_| panic!("{} should be set", VAR));

        fs::canonicalize(manifest_dir).unwrap_or_else(|_| panic!("{} should canonicalize", VAR))
    })
}

fn run_catchable<F>(test_fn: F) -> Result<(), TestException>
where
    F: FnOnce(&mut TestException) -> bool,
{
    let mut ex = TestException::default();

    if !test_fn(&mut ex) {
        return Err(ex);
    }

    Ok(())
}

#[track_caller]
pub fn run_cpp<F>(test_fn: F)
where
    F: FnOnce(&mut TestException) -> bool,
{
    let root_dir = get_root_dir();

    if let Err(ex) = run_catchable(test_fn) {
        let file = Path::new(&ex.file);
        let file = file.strip_prefix(root_dir).unwrap_or(file);

        panic!(
            "exception thrown at {}:{}:\n{}",
            file.display(),
            ex.line,
            ex.message
        );
    }
}

pub mod ffi {
    use super::*;
    use std::ffi::CStr;
    use std::os::raw::{c_char, c_int};

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn test_exception_set(
        f: *mut TestException,
        file: *const c_char,
        line: libc::c_uint,
        message: *const c_char,
    ) -> c_int {
        let f = f.as_mut().unwrap();

        let file = unsafe { CStr::from_ptr(file) };

        let file = match file.to_str() {
            Ok(s) => s,
            Err(_) => return -1,
        };

        let message = unsafe { CStr::from_ptr(message) };

        let message = match message.to_str() {
            Ok(s) => s,
            Err(_) => return -1,
        };

        f.file = file.to_string();
        f.line = line;
        f.message = message.to_string();

        0
    }
}
