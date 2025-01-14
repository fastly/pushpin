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
    local_offset: Option<UtcOffset>,
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

        let now = OffsetDateTime::now_utc().to_offset(self.local_offset.unwrap_or(UtcOffset::UTC));

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

// SAFETY: this method is unsound on platforms where another thread may
// modify environment vars
unsafe fn get_offset() -> Option<UtcOffset> {
    time::util::local_offset::set_soundness(time::util::local_offset::Soundness::Unsound);

    let offset = UtcOffset::current_local_offset().ok();

    time::util::local_offset::set_soundness(time::util::local_offset::Soundness::Sound);

    offset
}

static LOGGER: OnceLock<SimpleLogger> = OnceLock::new();

pub fn ensure_init_simple_logger(output_file: Option<File>, runner_mode: bool) {
    LOGGER.get_or_init(|| {
        // SAFETY: we accept that this call is unsound. on some platforms it
        // is the only way to know the time zone, with a chance of UB if
        // another thread modifies environment vars during the call. the risk
        // is low, as this call will happen very early in the program, and
        // only once. we would rather accept this low risk and know the time
        // zone than not know the time zone
        let local_offset = unsafe { get_offset() };

        SimpleLogger {
            local_offset,
            output_file: output_file.map(Mutex::new),
            runner_mode,
        }
    });
}

pub fn get_simple_logger() -> &'static SimpleLogger {
    ensure_init_simple_logger(None, false);

    // logger is guaranteed to have been initialized
    LOGGER.get().expect("logger should be initialized")
}

pub fn local_offset_check() {
    if get_simple_logger().local_offset.is_none() {
        log::warn!("Failed to determine local time offset. Log timestamps will be in UTC.");
    }
}
