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

use log::warn;
use notify::Watcher;
use std::ffi::CString;
use std::io;
use std::mem;
use std::os::fd::{AsRawFd, RawFd};
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::{Arc, Mutex};

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

#[cfg(target_os = "macos")]
fn get_errno() -> libc::c_int {
    // SAFETY: always safe to call
    unsafe { *libc::__error() }
}

#[cfg(not(target_os = "macos"))]
fn get_errno() -> libc::c_int {
    // SAFETY: always safe to call
    unsafe { *libc::__errno_location() }
}

fn set_fd_nonblocking(fd: RawFd) -> Result<(), io::Error> {
    // SAFETY: always safe to call
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFL, 0) };
    if flags < 0 {
        return Err(io::Error::last_os_error());
    }

    // SAFETY: always safe to call
    let ret = unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) };
    if ret != 0 {
        return Err(io::Error::last_os_error());
    }

    Ok(())
}

struct WatchState {
    _watcher: notify::RecommendedWatcher,
    changed: bool,
}

struct WatchData {
    file: PathBuf,
    read_fd: RawFd,
    write_fd: RawFd,
    state: Mutex<Option<WatchState>>,
}

pub struct Watch {
    data: Arc<WatchData>,
}

#[derive(Debug)]
pub struct WatchError;

impl Watch {
    pub fn new<P: AsRef<Path>>(file_path: P) -> Result<Self, WatchError> {
        let file = file_path.as_ref();

        let dir = match file.parent() {
            Some(p) => p,
            None => return Err(WatchError),
        };

        let mut fds = [0; 2];

        // SAFETY: fds pointer is valid
        let ret = unsafe { libc::pipe(fds.as_mut_ptr()) };
        assert_eq!(ret, 0);

        for fd in &fds {
            assert!(set_fd_nonblocking(*fd).is_ok());
        }

        let data = Arc::new(WatchData {
            file: file.to_owned(),
            read_fd: fds[0],
            write_fd: fds[1],
            state: Mutex::new(None),
        });

        let mut watcher = {
            let data = Arc::clone(&data);

            notify::recommended_watcher(move |event: Result<notify::Event, notify::Error>| {
                let event = match event {
                    Ok(event) => event,
                    Err(e) => {
                        warn!("file watcher error: {:?}", e);
                        return;
                    }
                };

                if !event.paths.into_iter().any(|p| p == data.file) {
                    // skip unrelated events
                    return;
                }

                if let Some(state) = data.state.lock().unwrap().as_mut() {
                    if !state.changed {
                        state.changed = true;

                        // non-blocking write to wake up the other side

                        let buf: [u8; 1] = [0; 1];

                        // SAFETY: buf pointer and size are valid
                        let ret = unsafe {
                            libc::write(data.write_fd, buf.as_ptr() as *const libc::c_void, 1)
                        };
                        assert!(ret == 1 || get_errno() == libc::EAGAIN);
                    }
                }
            })
            .expect("failed to create file watcher")
        };

        // watch the dir instead of the file, so we can detect file creates
        if let Err(e) = watcher.watch(dir, notify::RecursiveMode::NonRecursive) {
            warn!("failed to watch {}: {:?}", dir.display(), e);
        }

        {
            let mut state = data.state.lock().unwrap();

            *state = Some(WatchState {
                _watcher: watcher,
                changed: false,
            });
        }

        Ok(Self { data })
    }

    pub fn changed(&self) -> bool {
        let mut changed = false;

        if let Some(state) = self.data.state.lock().unwrap().as_mut() {
            // non-blocking read to clear

            let mut buf = [0u8; 128];

            // SAFETY: buf pointer and size are valid
            let ret = unsafe {
                libc::read(
                    self.data.read_fd,
                    buf.as_mut_ptr() as *mut libc::c_void,
                    buf.len(),
                )
            };
            assert!(ret >= 0 || get_errno() == libc::EAGAIN);

            changed = state.changed;
            state.changed = false;
        }

        changed
    }
}

impl Drop for Watch {
    fn drop(&mut self) {
        let mut state = self.data.state.lock().unwrap();
        *state = None;

        for fd in [self.data.write_fd, self.data.read_fd] {
            // SAFETY: always safe to call
            unsafe { libc::close(fd) };
        }
    }
}

impl AsRawFd for Watch {
    fn as_raw_fd(&self) -> RawFd {
        self.data.read_fd
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::event::Poller;
    use crate::core::test_dir;
    use std::fs;

    #[test]
    fn watch() {
        let file = test_dir().join("watch-file");

        match fs::remove_file(&file) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::NotFound => {}
            _ => panic!("failed to remove {}", file.display()),
        }

        let mut poller = Poller::new(1).unwrap();
        let token = mio::Token(1);

        let watcher = Watch::new(&file).unwrap();
        poller
            .register(
                &mut mio::unix::SourceFd(&watcher.as_raw_fd()),
                token,
                mio::Interest::READABLE,
            )
            .unwrap();
        assert_eq!(poller.iter_events().next(), None);
        assert!(!watcher.changed());

        // detect create

        fs::write(&file, "hello").unwrap();
        poller.poll(None).unwrap();

        let event = poller.iter_events().next().unwrap();
        assert_eq!(event.token(), token);
        assert_eq!(event.is_readable(), true);
        assert!(watcher.changed());
        assert!(!watcher.changed());

        // detect modify

        fs::write(&file, "world").unwrap();
        poller.poll(None).unwrap();

        let event = poller.iter_events().next().unwrap();
        assert_eq!(event.token(), token);
        assert_eq!(event.is_readable(), true);
        assert!(watcher.changed());
        assert!(!watcher.changed());

        // detect remove

        fs::remove_file(&file).unwrap();
        poller.poll(None).unwrap();

        let event = poller.iter_events().next().unwrap();
        assert_eq!(event.token(), token);
        assert_eq!(event.is_readable(), true);
        assert!(watcher.changed());
        assert!(!watcher.changed());
    }
}
