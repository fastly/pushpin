/*
 * Copyright (C) 2020-2023 Fanout, Inc.
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

pub mod app;
pub mod arena;
pub mod buffer;
pub mod channel;
pub mod client;
pub mod connection;
pub mod event;
pub mod executor;
pub mod future;
pub mod http1;
pub mod list;
pub mod listener;
pub mod net;
pub mod reactor;
pub mod resolver;
pub mod server;
pub mod shuffle;
pub mod timer;
pub mod tls;
pub mod tnetstring;
pub mod waker;
pub mod websocket;
pub mod zhttppacket;
pub mod zhttpsocket;
pub mod zmq;

use app::Config;
use log::info;
use std::error::Error;
use std::ffi::CString;
use std::future::Future;
use std::io;
use std::mem;
use std::ops::Deref;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
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
        crate::Pinner {
            unsafe_pointer: &mut { $x },
        }
    };
}

pub fn set_group(path: &Path, group: &str) -> Result<(), io::Error> {
    let gid = unsafe {
        let name = CString::new(group).unwrap();
        let mut buf = [0; 1024];
        let mut grp = mem::MaybeUninit::uninit();
        let mut group = ptr::null_mut();

        if libc::getgrnam_r(
            name.as_ptr(),
            grp.as_mut_ptr(),
            buf.as_mut_ptr(),
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

        group.gr_gid
    };

    unsafe {
        let path = CString::new(path.as_os_str().as_bytes()).unwrap();

        if libc::chown(path.as_ptr(), u32::MAX, gid) != 0 {
            return Err(io::Error::last_os_error());
        }
    }

    Ok(())
}

pub fn run(config: &Config) -> Result<(), Box<dyn Error>> {
    info!("starting...");

    {
        let a = match app::App::new(config) {
            Ok(a) => a,
            Err(e) => {
                return Err(e.into());
            }
        };

        info!("started");

        a.wait_for_term();

        info!("stopping...");
    }

    info!("stopped");

    Ok(())
}
