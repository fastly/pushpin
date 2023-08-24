/*
 * Copyright (C) 2021-2022 Fanout, Inc.
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
pub mod event;
pub mod executor;
pub mod ffi;
pub mod future;
pub mod http1;
pub mod jwt;
pub mod list;
pub mod listener;
pub mod net;
pub mod pool;
pub mod publish_cli;
pub mod reactor;
pub mod resolver;
pub mod runner;
pub mod server;
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

use std::ffi::CString;
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
    let mut buf = Vec::new();
    buf.resize(starting_size, 0);

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
