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

use std::ffi::CString;
use std::io;
use std::mem;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::ptr;

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
            buf.as_mut_ptr() as *mut libc::c_char,
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
            buf.as_mut_ptr() as *mut libc::c_char,
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
