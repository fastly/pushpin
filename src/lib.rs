/*
 * Copyright (C) 2021-2022 Fanout, Inc.
 * Copyright (C) 2023 Fastly, Inc.
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

pub mod arena;
pub mod buffer;
pub mod channel;
pub mod client;
pub mod condure;
pub mod config;
pub mod connection;
pub mod counter;
pub mod event;
pub mod executor;
pub mod ffi;
pub mod future;
pub mod http1;
pub mod jwt;
pub mod list;
pub mod listener;
pub mod log;
pub mod net;
pub mod pool;
pub mod publish_cli;
pub mod reactor;
pub mod resolver;
pub mod runner;
pub mod server;
pub mod service;
pub mod shuffle;
pub mod timer;
pub mod tls;
pub mod tnetstring;
pub mod track;
pub mod waker;
pub mod websocket;
pub mod zhttppacket;
pub mod zhttpsocket;
pub mod zmq;

use std::env;
use std::ffi::{CString, OsStr};
use std::future::Future;
use std::io;
use std::mem;
use std::ops::Deref;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::ptr;
use std::task::{Context, Poll};

pub struct Defer<T: FnOnce()> {
    f: Option<T>,
}

impl<T: FnOnce()> Defer<T> {
    pub fn new(f: T) -> Self {
        Self { f: Some(f) }
    }
}

impl<T: FnOnce()> Drop for Defer<T> {
    fn drop(&mut self) {
        let f = self.f.take().unwrap();

        f();
    }
}

pub struct Pinner<'a, T> {
    pub unsafe_pointer: &'a mut T,
}

impl<'a, T> Pinner<'a, T> {
    pub fn as_mut(&mut self) -> Pin<&mut T> {
        // SAFETY: as long as Pinner is only ever constructed via the pin!()
        // macro and the unsafe_pointer field is never directly accessed,
        // then the value is safe to pin here. this is because the macro
        // ensures the input is turned into a borrowed anonymous temporary,
        // preventing any further access to the original value after Pinner
        // is constructed, and Pinner has no methods that enable moving out
        // of the reference. the word "unsafe" is used in the field name to
        // discourage direct access. this is the best we can do, since the
        // field must be public for the macro to work.
        unsafe { Pin::new_unchecked(self.unsafe_pointer) }
    }

    pub fn set(&mut self, value: T) {
        self.as_mut().set(value)
    }
}

impl<'a, T> Pinner<'a, Option<T>> {
    pub fn as_pin_mut(&mut self) -> Option<Pin<&mut T>> {
        self.as_mut().as_pin_mut()
    }
}

impl<'a, T> Deref for Pinner<'a, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        self.unsafe_pointer
    }
}

impl<'a, T> Future for Pinner<'a, T>
where
    T: Future,
{
    type Output = T::Output;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        T::poll(Pin::into_inner(self).as_mut(), cx)
    }
}

// NOTE: replace with std::pin::pin someday
#[macro_export]
macro_rules! pin {
    ($x:expr) => {
        $crate::Pinner {
            unsafe_pointer: &mut { $x },
        }
    };
}

fn try_with_increasing_buffer<T, U>(starting_size: usize, f: T) -> Result<U, io::Error>
where
    T: Fn(&mut [u8]) -> Result<U, io::Error>,
{
    let mut buf = vec![0; starting_size];

    loop {
        match f(&mut buf) {
            Ok(v) => return Ok(v),
            Err(e) if e.raw_os_error() == Some(libc::ERANGE) => buf.resize(buf.len() * 2, 0),
            Err(e) => return Err(e),
        }
    }
}

fn get_user_uid(name: &str) -> Result<libc::gid_t, io::Error> {
    let name = CString::new(name).unwrap();

    try_with_increasing_buffer(1024, |buf| unsafe {
        let mut pwd = mem::MaybeUninit::uninit();
        let mut passwd = ptr::null_mut();

        if libc::getpwnam_r(
            name.as_ptr(),
            pwd.as_mut_ptr(),
            buf.as_mut_ptr() as *mut i8,
            buf.len(),
            &mut passwd,
        ) != 0
        {
            return Err(io::Error::last_os_error());
        }

        let passwd = match passwd.as_ref() {
            Some(r) => r,
            None => return Err(io::Error::from(io::ErrorKind::NotFound)),
        };

        Ok(passwd.pw_uid)
    })
}

fn get_group_gid(name: &str) -> Result<libc::gid_t, io::Error> {
    let name = CString::new(name).unwrap();

    try_with_increasing_buffer(1024, |buf| unsafe {
        let mut grp = mem::MaybeUninit::uninit();
        let mut group = ptr::null_mut();

        if libc::getgrnam_r(
            name.as_ptr(),
            grp.as_mut_ptr(),
            buf.as_mut_ptr() as *mut i8,
            buf.len(),
            &mut group,
        ) != 0
        {
            return Err(io::Error::last_os_error());
        }

        let group = match group.as_ref() {
            Some(r) => r,
            None => return Err(io::Error::from(io::ErrorKind::NotFound)),
        };

        Ok(group.gr_gid)
    })
}

pub fn set_user(path: &Path, user: &str) -> Result<(), io::Error> {
    let uid = get_user_uid(user)?;

    unsafe {
        let path = CString::new(path.as_os_str().as_bytes()).unwrap();

        if libc::chown(path.as_ptr(), uid, u32::MAX) != 0 {
            return Err(io::Error::last_os_error());
        }
    }

    Ok(())
}

pub fn set_group(path: &Path, group: &str) -> Result<(), io::Error> {
    let gid = get_group_gid(group)?;

    unsafe {
        let path = CString::new(path.as_os_str().as_bytes()).unwrap();

        if libc::chown(path.as_ptr(), u32::MAX, gid) != 0 {
            return Err(io::Error::last_os_error());
        }
    }

    Ok(())
}

pub fn can_move_mio_sockets_between_threads() -> bool {
    // on unix platforms, mio always uses epoll or kqueue, which support
    // this. mio makes no guarantee about supporting this on non-unix
    // platforms
    cfg!(unix)
}

pub enum ListenSpec {
    Tcp {
        addr: std::net::SocketAddr,
        tls: bool,
        default_cert: Option<String>,
    },
    Local {
        path: PathBuf,
        mode: Option<u32>,
        user: Option<String>,
        group: Option<String>,
    },
}

pub struct ListenConfig {
    pub spec: ListenSpec,
    pub stream: bool,
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
        .map(|s| CString::new(s.as_ref().as_encoded_bytes()).unwrap())
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
    use std::ffi::OsString;
    use std::fs::File;
    use std::io::{BufRead, BufReader, Read};
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

    fn read_and_print_all<R: Read>(r: R) -> Result<(), io::Error> {
        let r = BufReader::new(r);

        for line in r.lines() {
            let line = line?;

            println!("{}", line);
        }

        Ok(())
    }

    fn run_qtest<F>(test_fn: F, output_file: Option<&Path>) -> bool
    where
        F: Fn(&[&OsStr]) -> u8,
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

        ret == 0
    }

    fn mkfifo<P: AsRef<Path>>(path: P) -> Result<(), io::Error> {
        let path = CString::new(path.as_ref().as_os_str().as_encoded_bytes()).unwrap();

        unsafe {
            if libc::mkfifo(path.as_ptr(), 0o600) != 0 {
                return Err(io::Error::last_os_error());
            }
        }

        Ok(())
    }

    #[test]
    fn cpp() {
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

        let output_file = output_file.as_deref();

        if let Some(f) = output_file {
            match mkfifo(f) {
                Ok(()) => {}
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => {} // ok
                Err(e) => panic!("{}", e),
            }
        }

        // NOTE: qt tests cannot be run concurrently within the same process,
        // so we run them serially in a single rust test
        assert!(run_qtest(httpheaders_test, output_file));
        assert!(run_qtest(jwt_test, output_file));
        assert!(run_qtest(routesfile_test, output_file));
        assert!(run_qtest(jsonpatch_test, output_file));
        assert!(run_qtest(instruct_test, output_file));
        assert!(run_qtest(idformat_test, output_file));
        assert!(run_qtest(publishformat_test, output_file));
        assert!(run_qtest(publishitem_test, output_file));
        assert!(run_qtest(template_test, output_file));
        assert!(run_qtest(proxyengine_test, output_file));
        assert!(run_qtest(handlerengine_test, output_file));
    }
}
