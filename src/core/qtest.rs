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

use crate::core::test_dir;
use std::env;
use std::ffi::{CString, OsStr, OsString};
use std::fs::File;
use std::io::{self, BufRead, BufReader, Read};
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::sync::{mpsc, Mutex, OnceLock};
use std::thread;

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

        let thread = thread::Builder::new()
            .name("qtest-log".to_string())
            .spawn(move || {
                // this will block until the other side opens the file for writing
                let f = File::open(&f).unwrap();

                // forward the output until EOF or error
                if let Err(e) = read_and_print_all(f) {
                    eprintln!("failed to read log line: {}", e);
                }
            })
            .unwrap();

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

pub fn run<F>(test_fn: F) -> bool
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
        thread::Builder::new()
            .name("qtest-run".to_string())
            .spawn(move || {
                for t in r {
                    let ret = call_qtest(t.f, output_file.as_deref());

                    // if receiver is gone, keep going
                    let _ = t.ret.send(ret);
                }
                unreachable!();
            })
            .unwrap();

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
