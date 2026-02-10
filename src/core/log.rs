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
use log::{debug, warn, LevelFilter, Log, Metadata, Record};
use mio::net::UnixListener;
use std::fmt::Write as _;
use std::fs::File;
use std::io::{self, Write};
use std::mem;
use std::pin::pin;
use std::rc::Rc;
use std::str;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
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
    output_file: Option<Mutex<File>>,
    runner_mode: bool,
}

impl SimpleLogger {
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
        max_level: AtomicLevelFilter::default(),
        output_file: output_file.map(Mutex::new),
        runner_mode,
    });
}

pub fn get_simple_logger() -> &'static SimpleLogger {
    ensure_init_simple_logger(None, false);

    // Logger is guaranteed to have been initialized
    LOGGER.get().expect("logger should be initialized")
}

const QUEUE_MAX: usize = 1000;

#[derive(Clone)]
struct LogMessage {
    inner: String,
}

struct LogClient {
    sender: channel::LocalSender<Rc<LogMessage>>,
    _cancel: CancellationSender,
}

struct LogBroadcaster {
    thread: Option<thread::JoinHandle<()>>,
    msgs: Option<channel::Sender<LogMessage>>,
    interest: Arc<AtomicBool>,
}

impl LogBroadcaster {
    fn new(listener: UnixListener) -> Self {
        let (msgs, r_msgs) = channel::channel(QUEUE_MAX);

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
                        .spawn(Self::accept_task(listener, spawner, r_msgs, interest))
                        .unwrap();

                    executor.run(|timeout| reactor.poll(timeout)).unwrap();
                })
                .unwrap()
        };

        Self {
            thread: Some(thread),
            msgs: Some(msgs),
            interest,
        }
    }

    fn has_interest(&self) -> bool {
        self.interest.load(Ordering::Relaxed)
    }

    fn send(&self, message: String) {
        if let Some(sender) = &self.msgs {
            // Send immediately if the queue isn't full, else drop
            let _ = sender.try_send(LogMessage { inner: message });
        }
    }

    async fn accept_task(
        listener: UnixListener,
        spawner: Spawner,
        msgs: channel::Receiver<LogMessage>,
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
                    let msg = match ret {
                        Ok(msg) => Rc::new(msg),
                        Err(mpsc::RecvError) => break,
                    };

                    if !clients.is_empty() {
                        clients.retain(|c| {
                            // Send immediately if the client's queue isn't full, else drop
                            match c.sender.try_send(Rc::clone(&msg)) {
                                Err(mpsc::TrySendError::Disconnected(_)) => false,
                                _ => true, // Success or full
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
                        channel::local_channel(QUEUE_MAX, 1, &reactor.local_registration_memory());

                    let (cancel, token) =
                        CancellationToken::new(&reactor.local_registration_memory());

                    if spawner
                        .spawn(Self::handle_stream(r_msgs, token, stream))
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
                        _cancel: cancel,
                    });
                }
            }
        }
    }

    async fn handle_stream(
        msgs: channel::LocalReceiver<Rc<LogMessage>>,
        token: CancellationToken,
        stream: mio::net::UnixStream,
    ) {
        let msgs = channel::AsyncLocalReceiver::new(msgs);
        let mut stream = AsyncUnixStream::new(stream);

        // Loop to read from the channel and write to the client socket. If
        // the channel closes or the cancellation token is cancelled, then
        // the task ends.

        while let Ok(msg) = msgs.recv().await {
            let result = select_2(
                pin!(stream.write_all(msg.inner.as_bytes())),
                token.cancelled(),
            )
            .await;

            match result {
                Select2::R1(ret) => {
                    if let Err(e) = ret {
                        debug!("failed to write to debug log connection: {e}");
                        break;
                    }
                }
                Select2::R2(()) => break,
            }
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
    pub fn new(listener: UnixListener) -> Arc<Self> {
        let inner = DEBUG_LOGGER.get_or_init(|| Mutex::new(None));
        let mut inner = inner.lock().unwrap();

        if inner.is_some() {
            panic!("debug logger already exists");
        }

        let new_inner = Arc::new(Self {
            broadcaster: LogBroadcaster::new(listener),
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
    pub fn new(listener: UnixListener) -> Self {
        Self {
            inner: DebugLoggerInner::new(listener),
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

        let mut output = String::new();

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

        self.inner.broadcaster.send(output);
    }

    fn flush(&self) {}
}
