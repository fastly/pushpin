/*
 * Copyright (C) 2021-2022 Fanout, Inc.
 * Copyright (C) 2023-2024 Fastly, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:APACHE2$
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
 *
 * $FANOUT_END_LICENSE$
 */

/// cbindgen:ignore
pub mod connmgr;
pub mod core;
/// cbindgen:ignore
pub mod future;
/// cbindgen:ignore
pub mod publish;
/// cbindgen:ignore
pub mod runner;

use std::env;
use std::ffi::{CString, OsStr};
use std::os::unix::ffi::OsStrExt;

#[cfg(test)]
use std::path::{Path, PathBuf};

pub fn version() -> &'static str {
    env!("APP_VERSION")
}

#[macro_export]
macro_rules! import_cpp {
    ($($tt:tt)*) => {
        #[link(name = "pushpin-cpp")]
        #[cfg_attr(
            all(target_os = "macos", qt_lib_prefix = "Qt"),
            link(name = "QtCore", kind = "framework"),
            link(name = "QtNetwork", kind = "framework")
        )]
        #[cfg_attr(
            all(target_os = "macos", qt_lib_prefix = "Qt6"),
            link(name = "Qt6Core", kind = "framework"),
            link(name = "Qt6Network", kind = "framework")
        )]
        #[cfg_attr(
            all(target_os = "macos", qt_lib_prefix = "Qt5"),
            link(name = "Qt5Core", kind = "framework"),
            link(name = "Qt5Network", kind = "framework")
        )]
        #[cfg_attr(
            all(not(target_os = "macos"), qt_lib_prefix = "Qt"),
            link(name = "QtCore", kind = "dylib"),
            link(name = "QtNetwork", kind = "dylib")
        )]
        #[cfg_attr(
            all(not(target_os = "macos"), qt_lib_prefix = "Qt6"),
            link(name = "Qt6Core", kind = "dylib"),
            link(name = "Qt6Network", kind = "dylib")
        )]
        #[cfg_attr(
            all(not(target_os = "macos"), qt_lib_prefix = "Qt5"),
            link(name = "Qt5Core", kind = "dylib"),
            link(name = "Qt5Network", kind = "dylib")
        )]
        #[cfg_attr(target_os = "macos", link(name = "c++"))]
        #[cfg_attr(not(target_os = "macos"), link(name = "stdc++"))]
        extern "C" {
            $($tt)*
        }
    };
}

#[macro_export]
macro_rules! import_cpptest {
    ($($tt:tt)*) => {
        #[link(name = "pushpin-cpptest")]
        #[link(name = "pushpin-cpp")]
        #[cfg_attr(
            all(target_os = "macos", qt_lib_prefix = "Qt"),
            link(name = "QtCore", kind = "framework"),
            link(name = "QtNetwork", kind = "framework"),
            link(name = "QtTest", kind = "framework")
        )]
        #[cfg_attr(
            all(target_os = "macos", qt_lib_prefix = "Qt6"),
            link(name = "Qt6Core", kind = "framework"),
            link(name = "Qt6Network", kind = "framework"),
            link(name = "Qt6Test", kind = "framework")
        )]
        #[cfg_attr(
            all(target_os = "macos", qt_lib_prefix = "Qt5"),
            link(name = "Qt5Core", kind = "framework"),
            link(name = "Qt5Network", kind = "framework"),
            link(name = "Qt5Test", kind = "framework")
        )]
        #[cfg_attr(
            all(not(target_os = "macos"), qt_lib_prefix = "Qt"),
            link(name = "QtCore", kind = "dylib"),
            link(name = "QtNetwork", kind = "dylib"),
            link(name = "QtTest", kind = "dylib")
        )]
        #[cfg_attr(
            all(not(target_os = "macos"), qt_lib_prefix = "Qt6"),
            link(name = "Qt6Core", kind = "dylib"),
            link(name = "Qt6Network", kind = "dylib"),
            link(name = "Qt6Test", kind = "dylib")
        )]
        #[cfg_attr(
            all(not(target_os = "macos"), qt_lib_prefix = "Qt5"),
            link(name = "Qt5Core", kind = "dylib"),
            link(name = "Qt5Network", kind = "dylib"),
            link(name = "Qt5Test", kind = "dylib")
        )]
        #[cfg_attr(target_os = "macos", link(name = "c++"))]
        #[cfg_attr(not(target_os = "macos"), link(name = "stdc++"))]
        extern "C" {
            $($tt)*
        }
    };
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
    #[cfg(test)]
    import_cpptest! {
        pub fn httpheaders_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn jwt_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn routesfile_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn proxyengine_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn jsonpatch_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn instruct_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn idformat_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn publishformat_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn publishitem_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn handlerengine_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn template_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::OsString;
    use std::fs::File;
    use std::io::{self, BufRead, BufReader, Read};
    use std::sync::{mpsc, Mutex, OnceLock};
    use std::thread;

    fn httpheaders_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::httpheaders_test, args) as u8 }
    }

    fn jwt_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::jwt_test, args) as u8 }
    }

    fn routesfile_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::routesfile_test, args) as u8 }
    }

    fn proxyengine_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::proxyengine_test, args) as u8 }
    }

    fn jsonpatch_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::jsonpatch_test, args) as u8 }
    }

    fn instruct_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::instruct_test, args) as u8 }
    }

    fn idformat_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::idformat_test, args) as u8 }
    }

    fn publishformat_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::publishformat_test, args) as u8 }
    }

    fn publishitem_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::publishitem_test, args) as u8 }
    }

    fn handlerengine_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::handlerengine_test, args) as u8 }
    }

    fn template_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::template_test, args) as u8 }
    }

    fn mkfifo<P: AsRef<Path>>(path: P) -> Result<(), io::Error> {
        let path = CString::new(path.as_ref().as_os_str().as_bytes()).unwrap();

        unsafe {
            if libc::mkfifo(path.as_ptr(), 0o600) != 0 {
                return Err(io::Error::last_os_error());
            }
        }

        Ok(())
    }

    fn read_and_print_all<R: Read>(r: R) -> Result<(), io::Error> {
        let r = BufReader::new(r);

        for line in r.lines() {
            let line = line?;

            println!("{}", line);
        }

        Ok(())
    }

    fn call_qtest<F>(test_fn: F, output_file: Option<&Path>) -> u8
    where
        F: FnOnce(&[&OsStr]) -> u8,
    {
        let thread = if let Some(f) = output_file {
            let f = f.to_owned();

            let thread = thread::spawn(move || {
                // this will block until the other side opens the file for writing
                let f = File::open(&f).unwrap();

                // forward the output until EOF or error
                if let Err(e) = read_and_print_all(f) {
                    eprintln!("failed to read log line: {}", e);
                }
            });

            Some(thread)
        } else {
            None
        };

        let mut args = vec![OsStr::new("qtest")];

        let output_arg = if let Some(f) = output_file {
            let mut arg = OsString::from(f);
            arg.push(",txt");

            Some(arg)
        } else {
            None
        };

        if let Some(arg) = &output_arg {
            args.push(OsStr::new("-o"));
            args.push(arg);
        }

        let ret = test_fn(&args);

        if let Some(thread) = thread {
            thread.join().unwrap();
        }

        ret
    }

    // return fifo path, if applicable
    fn setup_output_file() -> Option<PathBuf> {
        // when cargo runs tests, it normally captures their output. however,
        // it does not do this by capturing the actual stdout of the process.
        // instead, it tracks calls made to the print family of functions in
        // the rust standard library. this means any output that does not go
        // through those functions, such as the output of our c++ tests, will
        // not be captured. in order to capture the output of c++ tests, we
        // use a fifo as an output file, and then any data read from the
        // other side is passed to rust print functions

        // one caveat of relaying output from the fifo is that it is
        // asynchronous. if a c++ test crashes and immediately aborts the
        // program, then it is possible some of its output may not get
        // relayed. if you are investigating a crash, set OUTPUT_DIRECT=1 to
        // opt out of the relaying
        let output_direct = !env::var("OUTPUT_DIRECT").unwrap_or_default().is_empty();

        let output_file = if output_direct {
            None
        } else {
            Some(test_dir().join("output"))
        };

        if let Some(f) = &output_file {
            match mkfifo(f) {
                Ok(()) => {}
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => {} // ok
                Err(e) => panic!("{}", e),
            }
        }

        output_file
    }

    struct RunQTest {
        f: Box<dyn FnOnce(&[&OsStr]) -> u8 + Send>,
        ret: mpsc::SyncSender<u8>,
    }

    fn run_qtest<F>(test_fn: F) -> bool
    where
        F: FnOnce(&[&OsStr]) -> u8 + Send + 'static,
    {
        // qt tests cannot be run concurrently within the same process, and
        // qt also doesn't like it when QCoreApplication is recreated in
        // different threads, so this function sets up a background thread
        // to enable running tests serially and all from the same thread

        static SENDER: OnceLock<Mutex<mpsc::Sender<RunQTest>>> = OnceLock::new();

        let s_run = SENDER.get_or_init(|| {
            let output_file = setup_output_file();

            let (s, r) = mpsc::channel::<RunQTest>();

            // run in the background forever
            thread::spawn(move || {
                for t in r {
                    let ret = call_qtest(t.f, output_file.as_deref());

                    // if receiver is gone, keep going
                    let _ = t.ret.send(ret);
                }
                unreachable!();
            });

            Mutex::new(s)
        });

        let (s_ret, r_ret) = mpsc::sync_channel(1);

        s_run
            .lock()
            .unwrap()
            .send(RunQTest {
                f: Box::new(test_fn),
                ret: s_ret,
            })
            .unwrap();

        let ret = r_ret.recv().unwrap();

        ret == 0
    }

    #[test]
    fn httpheaders() {
        assert!(run_qtest(httpheaders_test));
    }

    #[test]
    fn jwt() {
        assert!(run_qtest(jwt_test));
    }

    #[test]
    fn routesfile() {
        assert!(run_qtest(routesfile_test));
    }

    #[test]
    fn jsonpatch() {
        assert!(run_qtest(jsonpatch_test));
    }

    #[test]
    fn instruct() {
        assert!(run_qtest(instruct_test));
    }

    #[test]
    fn idformat() {
        assert!(run_qtest(idformat_test));
    }

    #[test]
    fn publishformat() {
        assert!(run_qtest(publishformat_test));
    }

    #[test]
    fn publishitem() {
        assert!(run_qtest(publishitem_test));
    }

    #[test]
    fn template() {
        assert!(run_qtest(template_test));
    }

    #[test]
    fn proxyengine() {
        assert!(run_qtest(proxyengine_test));
    }

    #[test]
    fn handlerengine() {
        assert!(run_qtest(handlerengine_test));
    }
}
