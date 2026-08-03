/*
 * Copyright (C) 2026 Fastly, Inc.
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

use crate::core::buffer::{Buffer, ContiguousBuffer, TmpBuffer, VecRingBuffer};
use crate::core::channel;
use crate::core::executor::Executor;
use crate::core::http1::{self, server};
use crate::core::io::io_split;
use crate::core::io::{AsyncRead, AsyncWrite};
use crate::core::net::{AsyncNetListener, AsyncTcpStream, AsyncUnixStream, NetListener, NetStream};
use crate::core::reactor::Reactor;
use crate::core::select::{select_2, Select2};
use crate::core::task::{CancellationSender, CancellationToken};
use log::{debug, error, warn};
use prometheus::{Encoder, TextEncoder};
use std::cell::RefCell;
use std::error::Error;
use std::io::Write;
use std::pin::pin;
use std::rc::Rc;
use std::sync::mpsc;
use std::thread;

const CONNS_MAX: usize = 10;
const REACTOR_BUDGET: u32 = 100;
const BUFFER_SIZE: usize = 4096;

pub struct PrometheusServer {
    thread: Option<thread::JoinHandle<()>>,
    stop: Option<channel::Sender<()>>,
}

impl PrometheusServer {
    pub fn new(listener: NetListener, prefix: &str, registry: prometheus::Registry) -> Self {
        let (stop_s, stop_r) = channel::channel(1);

        let prefix = prefix.to_string();

        let thread = thread::Builder::new()
            .name("prometheus".to_string())
            .spawn(move || {
                let reactor = Reactor::new(CONNS_MAX * 4 + 10); // 4 per client plus extra
                let executor = Executor::new(CONNS_MAX + 2); // clients plus stop and server tasks

                {
                    let reactor = reactor.clone();
                    executor.set_pre_poll(move || {
                        reactor.set_budget(Some(REACTOR_BUDGET));
                    });
                }

                let (cancel_s, cancel_t) =
                    CancellationToken::new(&reactor.local_registration_memory());

                // Watch for stop signal and cancel the token when triggered.
                executor
                    .spawn(async move {
                        let stop_r = channel::AsyncReceiver::new(stop_r);
                        let _ = stop_r.recv().await;
                        drop(cancel_s);
                    })
                    .expect("failed to spawn prometheus stop watcher");

                executor
                    .spawn(run_server(listener, cancel_t, prefix, registry))
                    .expect("failed to spawn prometheus server task");

                executor
                    .run(|timeout| reactor.poll(timeout))
                    .expect("prometheus server error");
            })
            .expect("failed to spawn prometheus thread");

        Self {
            thread: Some(thread),
            stop: Some(stop_s),
        }
    }
}

impl Drop for PrometheusServer {
    fn drop(&mut self) {
        // Signal the thread to stop.
        self.stop = None;

        // Wait for the thread to exit.
        self.thread.take().unwrap().join().unwrap();

        debug!("prometheus server stopped");
    }
}

struct Client {
    done: channel::LocalReceiver<()>,
    _cancel: CancellationSender,
}

async fn run_server(
    listener: NetListener,
    stop: CancellationToken,
    prefix: String,
    registry: prometheus::Registry,
) {
    let listener = AsyncNetListener::new(listener);

    let reactor = Reactor::current().unwrap();
    let executor = Executor::current().unwrap();
    let mut clients: Vec<Client> = Vec::new();

    debug!("prometheus server started");

    // Loop to serve connections. When the loop ends, the `clients` Vec is dropped, which causes
    // causes all the client tasks to end as well.

    loop {
        let stream = match select_2(pin!(listener.accept()), pin!(stop.cancelled())).await {
            Select2::R1(Ok((stream, _peer_addr))) => stream,
            Select2::R1(Err(e)) => {
                error!("prometheus: accept error: {}", e);
                continue;
            }
            Select2::R2(_) => break,
        };

        // Clear finished clients. With a low CONNS_MAX this should be relatively cheap.
        clients.retain(|c| !matches!(c.done.try_recv(), Err(mpsc::TryRecvError::Disconnected)));

        if clients.len() >= CONNS_MAX {
            // Drop the stream to close the connection immediately.
            warn!("too many prometheus connections, rejecting");
            continue;
        }

        let (s_done, r_done) = channel::local_channel(1, 1, &reactor.local_registration_memory());

        let (cancel, token) = CancellationToken::new(&reactor.local_registration_memory());

        match stream {
            NetStream::Tcp(s) => executor
                .spawn(run_connection(
                    AsyncTcpStream::new(s),
                    token,
                    s_done,
                    prefix.clone(),
                    registry.clone(),
                ))
                .expect("failed to spawn prometheus connection task"),
            NetStream::Unix(s) => executor
                .spawn(run_connection(
                    AsyncUnixStream::new(s),
                    token,
                    s_done,
                    prefix.clone(),
                    registry.clone(),
                ))
                .expect("failed to spawn prometheus connection task"),
        };

        clients.push(Client {
            done: r_done,
            _cancel: cancel,
        })
    }
}

async fn run_connection<S: AsyncRead + AsyncWrite + 'static>(
    stream: S,
    token: CancellationToken,
    _done: channel::LocalSender<()>, // dropped when function returns, indicating done
    prefix: String,
    registry: prometheus::Registry,
) {
    let result = match select_2(
        pin!(handle_connection(stream, &prefix, &registry)),
        pin!(token.cancelled()),
    )
    .await
    {
        Select2::R1(r) => r,
        Select2::R2(()) => return,
    };

    if let Err(e) = result {
        debug!("prometheus connection error: {e}");
    }
}

async fn handle_connection<S: AsyncRead + AsyncWrite>(
    stream: S,
    prefix: &str,
    registry: &prometheus::Registry,
) -> Result<(), Box<dyn Error>> {
    let stream = RefCell::new(stream);

    let rb_tmp = Rc::new(TmpBuffer::new(BUFFER_SIZE));
    let mut buf1 = VecRingBuffer::new(BUFFER_SIZE, &rb_tmp);
    let mut buf2 = VecRingBuffer::new(BUFFER_SIZE, &rb_tmp);

    let mut metric_families = registry.gather();
    if !prefix.is_empty() {
        for mf in &mut metric_families {
            mf.set_name(format!("{}{}", prefix, mf.get_name()));
        }
    }

    let encoder = TextEncoder::new();

    let mut output = {
        let mut output = Vec::new();
        encoder.encode(&metric_families, &mut output)?;

        let mut output_buffer = ContiguousBuffer::new(output.len());
        output_buffer
            .write_all(&output)
            .expect("output didn't fit in appropriately-sized buffer");

        output_buffer
    };

    let content_type = encoder.format_type();

    let mut resp_state = server::ResponseState::default();

    let resp_body = {
        let (req, mut resp) = server::Request::new(io_split(&stream), &mut buf1, &mut buf2);

        let mut scratch = http1::ParseScratch::<64>::new();
        let (owned_req, req_body) = req.recv_header(&mut resp).recv(&mut scratch, None).await?;
        let _ = req_body.discard_header(owned_req);

        let headers = [http1::Header {
            name: "Content-Type",
            value: content_type.as_bytes(),
        }];

        let (resp_header, prepare_body) = resp.prepare_header(
            200,
            "OK",
            &headers,
            http1::BodySize::Known(output.len()),
            &mut resp_state,
        )?;

        resp_header.send().await?.start_body(prepare_body)
    };

    loop {
        // Fill the buffer as much as possible
        let size = resp_body.prepare(Buffer::read_buf(&output), true)?;
        output.read_commit(size);

        match resp_body.send().await {
            http1::SendStatus::Complete(_) => break,
            http1::SendStatus::EarlyResponse(_) => unreachable!(), // For requests only
            http1::SendStatus::Partial((), _) => {}
            http1::SendStatus::Error((), e) => return Err(e.into()),
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use mio::net::TcpListener;
    use std::io::{Read, Write};

    #[test]
    fn request_metrics() {
        let registry = prometheus::Registry::new();
        let counter = prometheus::Counter::new("test_counter", "a test counter").unwrap();
        registry.register(Box::new(counter.clone())).unwrap();
        counter.inc();

        let listener = TcpListener::bind("127.0.0.1:0".parse().unwrap()).unwrap();
        let addr = listener.local_addr().unwrap();

        let server = PrometheusServer::new(NetListener::Tcp(listener), "myprefix_", registry);

        let mut stream = std::net::TcpStream::connect(addr).unwrap();
        stream
            .write_all(b"GET /metrics HTTP/1.0\r\nHost: localhost\r\n\r\n")
            .unwrap();

        let mut response = String::new();
        stream.read_to_string(&mut response).unwrap();

        drop(server);

        assert!(
            response.starts_with("HTTP/1.0 200 OK\r\n"),
            "unexpected response: {}",
            response
        );

        let (headers, body) = response.split_once("\r\n\r\n").unwrap();
        assert!(
            headers.contains("Content-Type: text/plain"),
            "missing Content-Type header"
        );

        // Every non-comment, non-empty line must carry the prefix.
        let unprefixed: Vec<&str> = body
            .lines()
            .filter(|l| !l.is_empty() && !l.starts_with('#') && !l.starts_with("myprefix_"))
            .collect();
        assert!(
            unprefixed.is_empty(),
            "body contains unprefixed metric names: {:?}",
            unprefixed
        );

        // The prefixed counter must appear in the output.
        assert!(
            body.contains("myprefix_test_counter"),
            "expected metric not found in body:\n{}",
            body
        );
    }
}
