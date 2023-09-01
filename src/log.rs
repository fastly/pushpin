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
use std::io;
use std::mem;
use std::str;
use std::sync::Once;
use time::macros::format_description;
use time::{OffsetDateTime, UtcOffset};

struct SimpleLogger {
    local_offset: UtcOffset,
}

impl Log for SimpleLogger {
    fn enabled(&self, metadata: &Metadata) -> bool {
        metadata.level() <= Level::Trace
    }

    fn log(&self, record: &Record) {
        if !self.enabled(record.metadata()) {
            return;
        }

        let now = OffsetDateTime::now_utc().to_offset(self.local_offset);

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

        println!("[{}] {} [{}] {}", lname, ts, record.target(), record.args());
    }

    fn flush(&self) {}
}

static mut LOGGER: mem::MaybeUninit<SimpleLogger> = mem::MaybeUninit::uninit();

pub fn get_simple_logger() -> &'static impl Log {
    static INIT: Once = Once::new();

    unsafe {
        INIT.call_once(|| {
            let local_offset =
                UtcOffset::current_local_offset().expect("failed to get local time offset");

            LOGGER.write(SimpleLogger { local_offset });
        });

        LOGGER.as_ptr().as_ref().unwrap()
    }
}
