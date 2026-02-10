/*
 * Copyright (C) 2020-2023 Fanout, Inc.
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

use log::{Level, Log, Metadata, Record};
use std::fs::File;
use std::io::{self, Write};
use std::str;
use std::sync::{Mutex, OnceLock};
use time::macros::format_description;
use time::{OffsetDateTime, UtcOffset};

static LOCAL_OFFSET: OnceLock<Option<UtcOffset>> = OnceLock::new();

// Obtains the local offset and caches it forever. This call may fail if
// there are multiple threads running when it is called for the first time,
// so it should be called early in the program before spawning threads.
fn local_offset() -> Option<UtcOffset> {
    *LOCAL_OFFSET.get_or_init(|| UtcOffset::current_local_offset().ok())
}

pub fn local_offset_check() {
    if local_offset().is_none() {
        log::warn!("Failed to determine local time offset. Log timestamps will be in UTC.");
    }
}

enum SharedOutput<'a> {
    Stdout(io::Stdout),
    File(&'a Mutex<File>),
}

impl Write for SharedOutput<'_> {
    fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
        match self {
            Self::Stdout(g) => g.write(buf),
            Self::File(g) => (*g).lock().unwrap().write(buf),
        }
    }

    fn flush(&mut self) -> Result<(), io::Error> {
        match self {
            Self::Stdout(g) => g.flush(),
            Self::File(g) => (*g).lock().unwrap().flush(),
        }
    }
}

pub struct SimpleLogger {
    output_file: Option<Mutex<File>>,
    runner_mode: bool,
}

impl Log for SimpleLogger {
    fn enabled(&self, metadata: &Metadata) -> bool {
        metadata.level() <= Level::Trace
    }

    fn log(&self, record: &Record) {
        if !self.enabled(record.metadata()) {
            return;
        }

        let mut output = match &self.output_file {
            Some(f) => SharedOutput::File(f),
            None => SharedOutput::Stdout(io::stdout()),
        };

        if self.runner_mode {
            writeln!(&mut output, "{}", record.args()).expect("failed to write log output");
            return;
        }

        let now = OffsetDateTime::now_utc().to_offset(local_offset().unwrap_or(UtcOffset::UTC));

        let format = format_description!(
            "[year]-[month]-[day] [hour]:[minute]:[second].[subsecond digits:3]"
        );

        let mut ts = [0u8; 64];

        let size = {
            let mut ts = io::Cursor::new(&mut ts[..]);

            now.format_into(&mut ts, &format)
                .expect("failed to write timestamp");

            ts.position() as usize
        };

        let ts = str::from_utf8(&ts[..size]).expect("timestamp is not utf-8");

        let lname = match record.level() {
            log::Level::Error => "ERR",
            log::Level::Warn => "WARN",
            log::Level::Info => "INFO",
            log::Level::Debug => "DEBUG",
            log::Level::Trace => "TRACE",
        };

        if record.level() <= log::Level::Info {
            writeln!(&mut output, "[{}] {} {}", lname, ts, record.args())
                .expect("failed to write log output");
        } else {
            writeln!(
                &mut output,
                "[{}] {} [{}] {}",
                lname,
                ts,
                record.target(),
                record.args()
            )
            .expect("failed to write log output");
        }
    }

    fn flush(&self) {}
}

static LOGGER: OnceLock<SimpleLogger> = OnceLock::new();

pub fn ensure_init_simple_logger(output_file: Option<File>, runner_mode: bool) {
    LOGGER.get_or_init(|| SimpleLogger {
        output_file: output_file.map(Mutex::new),
        runner_mode,
    });
}

pub fn get_simple_logger() -> &'static SimpleLogger {
    ensure_init_simple_logger(None, false);

    // Logger is guaranteed to have been initialized
    LOGGER.get().expect("logger should be initialized")
}
