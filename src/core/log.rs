/*
 * Copyright (C) 2020-2023 Fanout, Inc.
 * Copyright (C) 2023-2026 Fastly, Inc.
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

use crate::core::channel;
use crate::core::executor::{Executor, Spawner};
use crate::core::io::AsyncWriteExt;
use crate::core::net::{AsyncUnixListener, AsyncUnixStream};
use crate::core::reactor::Reactor;
use crate::core::select::{select_2, Select2};
use crate::core::task::{get_reactor, CancellationSender, CancellationToken};
use log::{debug, error, warn, LevelFilter, Log, Metadata, Record};
use mio::net::UnixListener;
use std::fmt::{self, Write as _};
use std::fs::File;
use std::io::{self, Write as _};
use std::mem;
use std::pin::pin;
use std::ptr;
use std::rc::Rc;
use std::str;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex, OnceLock, Weak};
use std::thread;
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

fn write_record<W: fmt::Write>(record: &Record, dest: &mut W) {
    let now = OffsetDateTime::now_utc().to_offset(local_offset().unwrap_or(UtcOffset::UTC));

    let format =
        format_description!("[year]-[month]-[day] [hour]:[minute]:[second].[subsecond digits:3]");

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

    if record.level() <= log::Level::Info || record.target().is_empty() {
        writeln!(dest, "[{}] {} {}", lname, ts, record.args()).expect("failed to write log output");
    } else {
        writeln!(
            dest,
            "[{}] {} [{}] {}",
            lname,
            ts,
            record.target(),
            record.args()
        )
        .expect("failed to write log output");
    }
}

enum SharedOutput<'a> {
    Stdout(io::Stdout),
    File(&'a Mutex<Option<File>>),
}

impl fmt::Write for SharedOutput<'_> {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let ret = match self {
            Self::Stdout(o) => o.write_all(s.as_bytes()),
            Self::File(o) => match &mut *(*o).lock().unwrap() {
                Some(o) => o.write_all(s.as_bytes()),
                None => Ok(()),
            },
        };

        ret.map_err(|_| fmt::Error)
    }

    fn write_fmt(&mut self, args: fmt::Arguments) -> fmt::Result {
        let ret = match self {
            Self::Stdout(o) => o.write_fmt(args),
            Self::File(o) => match &mut *(*o).lock().unwrap() {
                Some(o) => o.write_fmt(args),
                None => Ok(()),
            },
        };

        ret.map_err(|_| fmt::Error)
    }
}

struct AtomicLevelFilter(AtomicUsize);

impl AtomicLevelFilter {
    fn set(&self, level: LevelFilter) {
        self.0.store(level as usize, Ordering::Relaxed);
    }

    fn get(&self) -> LevelFilter {
        // SAFETY: We store only valid enum values
        unsafe { mem::transmute(self.0.load(Ordering::Relaxed)) }
    }
}

impl Default for AtomicLevelFilter {
    fn default() -> Self {
        Self(AtomicUsize::new(LevelFilter::Off as usize))
    }
}

pub struct SimpleLogger {
    max_level: AtomicLevelFilter,

    // The outer Option is read-only, for quick detection of the output mode. The inner Option
    // should be Some unless a log rotation fails, in which case it can be set to None to disable
    // output.
    output_file: Option<Mutex<Option<File>>>,

    runner_mode: bool,
}

impl SimpleLogger {
    pub fn is_active(&self) -> bool {
        let logger = log::logger();

        ptr::eq(
            self as *const Self as *const (),
            logger as *const dyn Log as *const (),
        )
    }

    pub fn max_level(&self) -> LevelFilter {
        self.max_level.get()
    }

    pub fn set_max_level(&self, level: LevelFilter) {
        self.max_level.set(level);
    }
}

impl Log for SimpleLogger {
    fn enabled(&self, metadata: &Metadata) -> bool {
        if let Some(dl) = DebugLogger::instance() {
            if dl.enabled(metadata) {
                return true;
            }
        }

        metadata.level() <= self.max_level.get()
    }

    fn log(&self, record: &Record) {
        if !self.enabled(record.metadata()) {
            return;
        }

        if let Some(dl) = DebugLogger::instance() {
            dl.log(record);
        }

        if record.metadata().level() > self.max_level.get() {
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

        write_record(record, &mut output);
    }

    fn flush(&self) {}
}

static LOGGER: OnceLock<SimpleLogger> = OnceLock::new();

pub fn ensure_init_simple_logger(output_file: Option<File>, runner_mode: bool) {
    LOGGER.get_or_init(|| SimpleLogger {
        max_level: AtomicLevelFilter::default(),
        output_file: output_file.map(|f| Mutex::new(Some(f))),
        runner_mode,
    });
}

pub fn get_simple_logger() -> &'static SimpleLogger {
    ensure_init_simple_logger(None, false);

    // Logger is guaranteed to have been initialized
    LOGGER.get().expect("logger should be initialized")
}

#[derive(Clone)]
struct LogMessage {
    inner: String,
}

struct LogClient {
    sender: channel::LocalSender<(Rc<LogMessage>, u64)>,
    drop_count: u64,
    _cancel: CancellationSender,
}

enum ClientWriteError {
    Io(io::Error),
    Cancelled,
}

struct LogBroadcaster {
    thread: Option<thread::JoinHandle<()>>,
    msgs: Option<channel::Sender<(LogMessage, u64)>>,
    interest: Arc<AtomicBool>,
    drop_count: AtomicU64,
}

impl LogBroadcaster {
    fn new(listener: UnixListener, queue_max: usize) -> Self {
        let (msgs, r_msgs) = channel::channel(queue_max);

        let interest = Arc::new(AtomicBool::new(false));

        let thread = {
            let interest = Arc::clone(&interest);

            thread::Builder::new()
                .name("debug-logger".to_string())
                .spawn(move || {
                    let reactor = Reactor::new(100); // Plenty for 11 tasks
                    let executor = Executor::new(11); // Accept task + 10 clients

                    let spawner = executor.spawner();

                    executor
                        .spawn(Self::accept_task(
                            listener, queue_max, spawner, r_msgs, interest,
                        ))
                        .unwrap();

                    executor.run(|timeout| reactor.poll(timeout)).unwrap();
                })
                .unwrap()
        };

        Self {
            thread: Some(thread),
            msgs: Some(msgs),
            interest,
            drop_count: AtomicU64::new(0),
        }
    }

    fn has_interest(&self) -> bool {
        self.interest.load(Ordering::Relaxed)
    }

    fn send(&self, message: String) {
        if let Some(sender) = &self.msgs {
            let message = LogMessage { inner: message };

            // `send` can be called from multiple threads at the same time.
            // To handle the drop count accurately, we do an atomic swap to
            // take the current value for sending, and if we fail to send
            // then we add the value back.
            let drop_count = self.drop_count.swap(0, Ordering::Relaxed);

            // Send immediately if the queue isn't full, else drop
            match sender.try_send((message, drop_count)) {
                Ok(()) => {}
                Err(mpsc::TrySendError::Full(_)) => {
                    self.drop_count.fetch_add(drop_count + 1, Ordering::Relaxed);
                }
                Err(mpsc::TrySendError::Disconnected(_)) => unreachable!(),
            }
        }
    }

    async fn accept_task(
        listener: UnixListener,
        queue_max: usize,
        spawner: Spawner,
        msgs: channel::Receiver<(LogMessage, u64)>,
        interest: Arc<AtomicBool>,
    ) {
        let listener = AsyncUnixListener::new(listener);
        let msgs = channel::AsyncReceiver::new(msgs);

        let mut clients: Vec<LogClient> = Vec::new();

        debug!("debug logger started");

        // Loop to read messages from a channel and to listen for new
        // connections. Messages are broadcasted to all connections. If the
        // channel closes, the task ends. When the task ends, the `clients`
        // Vec is dropped, which causes all the client tasks to end as well.

        loop {
            let result = select_2(msgs.recv(), listener.accept()).await;

            match result {
                Select2::R1(ret) => {
                    let (msg, drop_count) = match ret {
                        Ok(ret) => ret,
                        Err(mpsc::RecvError) => break,
                    };

                    let msg = Rc::new(msg);

                    if !clients.is_empty() {
                        clients.retain_mut(|c| {
                            c.drop_count += drop_count;

                            // Send immediately if the client's queue isn't full, else drop
                            match c.sender.try_send((Rc::clone(&msg), c.drop_count)) {
                                Ok(()) => {
                                    c.drop_count = 0;

                                    true
                                }
                                Err(mpsc::TrySendError::Full(_)) => {
                                    c.drop_count += 1;

                                    true
                                }
                                Err(mpsc::TrySendError::Disconnected(_)) => false,
                            }
                        });

                        if clients.is_empty() {
                            interest.store(false, Ordering::Relaxed);
                        }
                    }
                }
                Select2::R2(ret) => {
                    let stream = match ret {
                        Ok((stream, _)) => stream,
                        Err(e) => {
                            warn!("debug log accept error: {e}");
                            continue;
                        }
                    };

                    let reactor = get_reactor();

                    let (s_msgs, r_msgs) =
                        channel::local_channel(queue_max, 1, &reactor.local_registration_memory());

                    let (cancel, token) =
                        CancellationToken::new(&reactor.local_registration_memory());

                    if spawner
                        .spawn(Self::handle_stream(r_msgs, stream, token))
                        .is_err()
                    {
                        warn!("too many debug log connections, rejecting");
                        continue;
                    }

                    if clients.is_empty() {
                        interest.store(true, Ordering::Relaxed);
                    }

                    clients.push(LogClient {
                        sender: s_msgs,
                        drop_count: 0,
                        _cancel: cancel,
                    });
                }
            }
        }
    }

    async fn handle_stream(
        msgs: channel::LocalReceiver<(Rc<LogMessage>, u64)>,
        stream: mio::net::UnixStream,
        token: CancellationToken,
    ) {
        let msgs = channel::AsyncLocalReceiver::new(msgs);
        let mut stream = AsyncUnixStream::new(stream);

        if let Err(ClientWriteError::Io(e)) = Self::relay_logs(&msgs, &mut stream, &token).await {
            debug!("failed to write to debug log connection: {e}");
        }
    }

    async fn relay_logs(
        msgs: &channel::AsyncLocalReceiver<(Rc<LogMessage>, u64)>,
        stream: &mut AsyncUnixStream,
        token: &CancellationToken,
    ) -> Result<(), ClientWriteError> {
        // Loop to read from the channel and write to the client socket. Exit
        // when the channel closes or the token is cancelled.
        while let Ok((msg, drop_count)) = msgs.recv().await {
            if drop_count > 0 {
                Self::write_to_stream(stream, &format!("** dropped {drop_count} logs"), token)
                    .await?;
            }

            Self::write_to_stream(stream, &msg.inner, token).await?;
        }

        Ok(())
    }

    async fn write_to_stream(
        stream: &mut AsyncUnixStream,
        message: &str,
        token: &CancellationToken,
    ) -> Result<(), ClientWriteError> {
        let result = select_2(
            pin!(stream.write_all(message.as_bytes())),
            token.cancelled(),
        )
        .await;

        match result {
            Select2::R1(ret) => match ret {
                Ok(()) => Ok(()),
                Err(e) => Err(ClientWriteError::Io(e)),
            },
            Select2::R2(()) => Err(ClientWriteError::Cancelled),
        }
    }
}

impl Drop for LogBroadcaster {
    fn drop(&mut self) {
        // Tell event loop to exit
        self.msgs = None;

        // Wait for thread to end
        let thread = self.thread.take().unwrap();
        thread.join().unwrap();

        debug!("debug logger stopped");
    }
}

static DEBUG_LOGGER: OnceLock<Mutex<Option<Weak<DebugLoggerInner>>>> = OnceLock::new();

struct DebugLoggerInner {
    broadcaster: LogBroadcaster,
}

impl DebugLoggerInner {
    pub fn new(listener: UnixListener, queue_max: usize) -> Arc<Self> {
        let inner = DEBUG_LOGGER.get_or_init(|| Mutex::new(None));
        let mut inner = inner.lock().unwrap();

        if inner.is_some() {
            panic!("debug logger already exists");
        }

        let new_inner = Arc::new(Self {
            broadcaster: LogBroadcaster::new(listener, queue_max),
        });

        *inner = Some(Arc::downgrade(&new_inner));

        new_inner
    }
}

impl Drop for DebugLoggerInner {
    fn drop(&mut self) {
        // Clear global
        let inner = DEBUG_LOGGER.get().unwrap();
        let mut inner = inner.lock().unwrap();
        *inner = None;
    }
}

pub struct DebugLogger {
    inner: Arc<DebugLoggerInner>,
}

impl DebugLogger {
    pub fn new(listener: UnixListener, queue_max: usize) -> Self {
        Self {
            inner: DebugLoggerInner::new(listener, queue_max),
        }
    }

    pub fn instance() -> Option<Self> {
        DEBUG_LOGGER.get().and_then(|inner| {
            let inner = inner.lock().unwrap();

            inner
                .as_ref()
                .and_then(|inner| inner.upgrade().map(|inner| Self { inner }))
        })
    }
}

impl Log for DebugLogger {
    fn enabled(&self, _metadata: &Metadata) -> bool {
        self.inner.broadcaster.has_interest()
    }

    fn log(&self, record: &Record) {
        if !self.enabled(record.metadata()) {
            return;
        }

        let mut output = String::new();

        write_record(record, &mut output);

        self.inner.broadcaster.send(output);
    }

    fn flush(&self) {}
}

mod ffi {
    use super::*;
    use std::ffi::{c_char, c_int, CStr};
    use std::fs::OpenOptions;

    pub const LOG_LEVEL_ERROR: c_int = 0;
    pub const LOG_LEVEL_WARN: c_int = 1;
    pub const LOG_LEVEL_INFO: c_int = 2;
    pub const LOG_LEVEL_DEBUG: c_int = 3;
    pub const LOG_LEVEL_TRACE: c_int = 4;

    fn int_to_level(level: c_int) -> log::Level {
        match level {
            core::i32::MIN..=0 => log::Level::Error,
            1 => log::Level::Warn,
            2 => log::Level::Info,
            3 => log::Level::Debug,
            4..=core::i32::MAX => log::Level::Trace,
        }
    }

    /// If `output_file` is non-null, attempt to configure logging to the
    /// specified file. If `output_file` is null, log to stdout.
    ///
    /// Returns 0 on success, or non-zero if there was a problem opening
    /// the log file. In the latter case, the logging system will still be
    /// initialized, but will be configured log to stdout instead of a file,
    /// and a file error message will be logged.
    ///
    /// SAFETY: `output_file` must be valid if non-null.
    #[no_mangle]
    pub unsafe extern "C" fn log_init(output_file: *const c_char) -> c_int {
        let (output_file, file_error) = if !output_file.is_null() {
            let f = unsafe {
                CStr::from_ptr(output_file)
                    .to_str()
                    .expect("output_file must be utf-8")
            };

            match OpenOptions::new().create(true).append(true).open(f) {
                Ok(f) => (Some(f), None),
                Err(e) => (None, Some((f, e))),
            }
        } else {
            (None, None)
        };

        ensure_init_simple_logger(output_file, false);

        // Allow all log levels globally so individual loggers can limit as they choose
        log::set_max_level(LevelFilter::Trace);

        let logger = get_simple_logger();

        log::set_logger(logger).unwrap();
        logger.set_max_level(LevelFilter::Trace);

        // Log any file opening error after initializing the logger
        if let Some((f, e)) = file_error {
            error!("failed to open log file {f}: {e}");
            return -1;
        }

        0
    }

    #[no_mangle]
    pub extern "C" fn log_get_level() -> c_int {
        let level = match LOGGER.get() {
            Some(logger) if logger.is_active() => logger.max_level(),
            _ => log::max_level(),
        };

        match level {
            log::LevelFilter::Off => -1,
            log::LevelFilter::Error => LOG_LEVEL_ERROR,
            log::LevelFilter::Warn => LOG_LEVEL_WARN,
            log::LevelFilter::Info => LOG_LEVEL_INFO,
            log::LevelFilter::Debug => LOG_LEVEL_DEBUG,
            log::LevelFilter::Trace => LOG_LEVEL_TRACE,
        }
    }

    #[no_mangle]
    pub extern "C" fn log_set_level(level: c_int) {
        get_simple_logger().set_max_level(int_to_level(level).to_level_filter());
    }

    /// Returns 0 on success or if file output was not configured.
    ///
    /// SAFETY: `output_file` must be valid.
    #[no_mangle]
    pub extern "C" fn log_rotate(output_file: *const c_char) -> c_int {
        assert!(!output_file.is_null());

        let f = unsafe {
            CStr::from_ptr(output_file)
                .to_str()
                .expect("output_file must be utf-8")
        };

        let logger = get_simple_logger();

        let Some(output_file) = &logger.output_file else {
            return 0;
        };

        let mut output_file = output_file.lock().unwrap();

        // Close the file, if any
        *output_file = None;

        let new_output_file = match OpenOptions::new().create(true).append(true).open(f) {
            Ok(f) => f,
            Err(e) => {
                // Log to standard output since we can't log to the file
                write_record(
                    &Record::builder()
                        .args(format_args!("failed to reopen log file {f}: {e}"))
                        .level(log::Level::Error)
                        .build(),
                    &mut SharedOutput::Stdout(io::stdout()),
                );

                return -1;
            }
        };

        *output_file = Some(new_output_file);

        0
    }

    /// SAFETY: `message` must be valid.
    #[no_mangle]
    pub unsafe extern "C" fn log_log(level: c_int, message: *const c_char) {
        assert!(!message.is_null());

        let message = unsafe {
            match CStr::from_ptr(message).to_str() {
                Ok(s) => s,
                Err(_) => return, // Invalid UTF-8, skip
            }
        };

        // The default log target is the current Rust module, but this isn't
        // meaningful when logging via FFI, so let's set an empty target.
        log::log!(target: "", int_to_level(level), "{}", message);
    }

    /// SAFETY: `message` must be valid.
    #[no_mangle]
    pub unsafe extern "C" fn log_log_raw(message: *const c_char) {
        assert!(!message.is_null());

        let message = unsafe {
            match CStr::from_ptr(message).to_str() {
                Ok(s) => s,
                Err(_) => return, // Invalid UTF-8, skip
            }
        };

        let logger = get_simple_logger();

        let mut output = match &logger.output_file {
            Some(f) => SharedOutput::File(f),
            None => SharedOutput::Stdout(io::stdout()),
        };

        // Write raw message directly to output (assuming it's already formatted)
        writeln!(&mut output, "{}", message).expect("failed to write log output");
    }
}
