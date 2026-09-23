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
use crate::core::http1::{self, server, BodySize, RecvStatus};
use crate::core::io::io_split;
use crate::core::io::{AsyncRead, AsyncWrite};
use crate::core::net::{AsyncNetListener, AsyncTcpStream, AsyncUnixStream, NetListener, NetStream};
use crate::core::reactor::Reactor;
use crate::core::select::{select_2, Select2};
use crate::core::task::{CancellationSender, CancellationToken};
use log::{debug, error, warn};
use std::cell::RefCell;
use std::error::Error;
use std::pin::pin;
use std::rc::Rc;
use std::sync::mpsc;
use std::thread;

const REACTOR_BUDGET: u32 = 100;
const HEADERS_MAX: usize = 64;

pub struct Config {
    pub connections_max: usize,
    pub headers_size_max: usize,
    pub body_size_max: usize,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            connections_max: 50,
            headers_size_max: 16_384,
            body_size_max: 1_000_000,
        }
    }
}

pub struct Request {
    pub method: String,
    pub uri: String,
    pub headers: Vec<(String, Vec<u8>)>,
    pub body: Vec<u8>,
}

pub struct Response {
    pub code: u16,
    pub reason: String,
    pub headers: Vec<(String, Vec<u8>)>,
    pub body: Vec<u8>,
}

pub struct Server {
    thread: Option<thread::JoinHandle<()>>,
    stop: Option<channel::Sender<()>>,
}

impl Server {
    pub fn new<H>(listener: NetListener, config: Config, handler: H) -> Self
    where
        H: Fn(Request) -> Response + Send + 'static,
    {
        let (stop_s, stop_r) = channel::channel(1);

        let thread = thread::Builder::new()
            .name("server".to_string())
            .spawn(move || {
                let reactor = Reactor::new(config.connections_max * 4 + 10); // 4 per client plus extra
                let executor = Executor::new(config.connections_max + 2); // clients plus stop and server tasks

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
                    .expect("failed to spawn simple http server stop watcher");

                executor
                    .spawn(run_server(listener, cancel_t, config, handler))
                    .expect("failed to spawn simple http server task");

                executor
                    .run(|timeout| reactor.poll(timeout))
                    .expect("simple http server error");
            })
            .expect("failed to spawn server thread");

        Self {
            thread: Some(thread),
            stop: Some(stop_s),
        }
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        // Signal the thread to stop.
        self.stop = None;

        // Wait for the thread to exit.
        self.thread.take().unwrap().join().unwrap();

        debug!("simple http server stopped");
    }
}

struct Client {
    done: channel::LocalReceiver<()>,
    _cancel: CancellationSender,
}

async fn run_server<H>(listener: NetListener, stop: CancellationToken, config: Config, handler: H)
where
    H: Fn(Request) -> Response + Send + 'static,
{
    let listener = AsyncNetListener::new(listener);

    let reactor = Reactor::current().unwrap();
    let executor = Executor::current().unwrap();
    let config = Rc::new(config);
    let handler = Rc::new(handler);
    let mut clients: Vec<Client> = Vec::new();

    debug!("simple http server started");

    // Loop to serve connections. When the loop ends, the `clients` Vec is dropped, which causes
    // all the client tasks to end as well.

    loop {
        let stream = match select_2(pin!(listener.accept()), pin!(stop.cancelled())).await {
            Select2::R1(Ok((stream, _peer_addr))) => stream,
            Select2::R1(Err(e)) => {
                error!("simple http server accept error: {}", e);
                continue;
            }
            Select2::R2(_) => break,
        };

        // Clear finished clients. With a low connections_max this should be relatively cheap.
        clients.retain(|c| !matches!(c.done.try_recv(), Err(mpsc::TryRecvError::Disconnected)));

        if clients.len() >= config.connections_max {
            // Drop the stream to close the connection immediately.
            warn!("too many simple http server connections, rejecting");
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
                    config.clone(),
                    handler.clone(),
                ))
                .expect("failed to spawn simple http server connection task"),
            NetStream::Unix(s) => executor
                .spawn(run_connection(
                    AsyncUnixStream::new(s),
                    token,
                    s_done,
                    config.clone(),
                    handler.clone(),
                ))
                .expect("failed to spawn simple http server connection task"),
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
    config: Rc<Config>,
    handler: Rc<dyn Fn(Request) -> Response>,
) {
    let result = match select_2(
        pin!(handle_connection(stream, &config, &*handler)),
        pin!(token.cancelled()),
    )
    .await
    {
        Select2::R1(r) => r,
        Select2::R2(()) => return,
    };

    if let Err(e) = result {
        debug!("simple http server connection error: {e}");
    }
}

async fn handle_connection<S: AsyncRead + AsyncWrite>(
    stream: S,
    config: &Config,
    handler: &dyn Fn(Request) -> Response,
) -> Result<(), Box<dyn Error>> {
    let stream = RefCell::new(stream);

    let buffer_size = config.headers_size_max;

    let rb_tmp = Rc::new(TmpBuffer::new(buffer_size));
    let mut buf1 = VecRingBuffer::new(buffer_size, &rb_tmp);
    let mut buf2 = VecRingBuffer::new(buffer_size, &rb_tmp);
    let body_buf = ContiguousBuffer::new(config.body_size_max);

    let mut resp_state = server::ResponseState::default();

    let (resp_body, mut body_buf) = {
        let (req, mut resp) = server::Request::new(io_split(&stream), &mut buf1, &mut buf2);

        let resp_data = process_request(req, &mut resp, body_buf, handler).await?;

        let headers: Vec<http1::Header> = resp_data
            .headers
            .iter()
            .map(|(name, value)| http1::Header {
                name: name.as_str(),
                value,
            })
            .collect();

        let body_len = resp_data.body.len();
        let body_buf: ContiguousBuffer = resp_data.body.into();

        let (resp_header, prepare_body) = resp.prepare_header(
            resp_data.code,
            &resp_data.reason,
            &headers,
            http1::BodySize::Known(body_len),
            &mut resp_state,
        )?;

        (resp_header.send().await?.start_body(prepare_body), body_buf)
    };

    loop {
        // Fill the buffer as much as possible
        let size = resp_body.prepare(Buffer::read_buf(&body_buf), true)?;
        body_buf.read_commit(size);

        match resp_body.send().await {
            http1::SendStatus::Complete(_) => break,
            http1::SendStatus::EarlyResponse(_) => unreachable!(), // For requests only
            http1::SendStatus::Partial((), _) => {}
            http1::SendStatus::Error((), e) => return Err(e.into()),
        }
    }

    Ok(())
}

async fn process_request<R: AsyncRead, W: AsyncWrite>(
    req: server::Request,
    resp: &mut server::Response<'_, R, W>,
    mut body_buf: ContiguousBuffer,
    handler: &dyn Fn(Request) -> Response,
) -> Result<Response, Box<dyn Error>> {
    let mut scratch = http1::ParseScratch::<HEADERS_MAX>::new();
    let (owned_req, req_body) = req.recv_header(resp).recv(&mut scratch, None).await?;

    let (method, uri, headers) = {
        let req = owned_req.get();

        if req.body_size == BodySize::Unknown {
            return Ok(Response {
                code: 411,
                reason: "Length Required".to_string(),
                headers: vec![(
                    "Content-Type".to_string(),
                    "text/plain".to_string().into_bytes(),
                )],
                body: "Request requires Content-Length.\n"
                    .to_string()
                    .into_bytes(),
            });
        }

        (
            req.method.to_string(),
            req.uri.to_string(),
            req.headers
                .iter()
                .map(|h| (h.name.to_string(), h.value.to_vec()))
                .collect(),
        )
    };

    let req_body = req_body.discard_header(owned_req);

    loop {
        match req_body.try_recv(body_buf.write_buf())? {
            RecvStatus::Complete((), size) => {
                body_buf.write_commit(size);
                break;
            }
            RecvStatus::Read((), size) => body_buf.write_commit(size),
            RecvStatus::NeedBytes(()) => req_body.add_to_buffer().await?,
        }
    }

    Ok(handler(Request {
        method,
        uri,
        headers,
        body: body_buf.into_inner(),
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use mio::net::TcpListener;
    use std::io::{Read, Write};
    use std::str;
    use std::sync::{Arc, Mutex};

    #[test]
    fn get() {
        let listener = TcpListener::bind("127.0.0.1:0".parse().unwrap()).unwrap();
        let addr = listener.local_addr().unwrap();

        let request = Arc::new(Mutex::new(None));

        let server = {
            let request = request.clone();

            Server::new(NetListener::Tcp(listener), Config::default(), move |req| {
                *request.lock().unwrap() = Some(req);

                Response {
                    code: 200,
                    reason: "OK".to_string(),
                    headers: vec![(
                        "Content-Type".to_string(),
                        "text/plain".to_string().into_bytes(),
                    )],
                    body: "hello world\n".to_string().into_bytes(),
                }
            })
        };

        let mut stream = std::net::TcpStream::connect(addr).unwrap();

        let data = concat!("GET /path HTTP/1.0\r\n", "Host: localhost\r\n", "\r\n").as_bytes();

        stream.write_all(&data).unwrap();

        let mut response = String::new();
        stream.read_to_string(&mut response).unwrap();

        drop(server);

        let request = request.lock().unwrap().take().unwrap();
        assert_eq!(request.method, "GET");
        assert_eq!(request.uri, "/path");
        assert_eq!(request.headers.len(), 1);
        assert_eq!(request.headers[0].0, "Host");
        assert_eq!(str::from_utf8(&request.headers[0].1).unwrap(), "localhost");
        assert!(request.body.is_empty());

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

        assert_eq!(body, "hello world\n");
    }

    #[test]
    fn post() {
        let listener = TcpListener::bind("127.0.0.1:0".parse().unwrap()).unwrap();
        let addr = listener.local_addr().unwrap();

        let request = Arc::new(Mutex::new(None));

        let server = {
            let request = request.clone();

            Server::new(NetListener::Tcp(listener), Config::default(), move |req| {
                *request.lock().unwrap() = Some(req);

                Response {
                    code: 200,
                    reason: "OK".to_string(),
                    headers: vec![(
                        "Content-Type".to_string(),
                        "text/plain".to_string().into_bytes(),
                    )],
                    body: "world\n".to_string().into_bytes(),
                }
            })
        };

        let mut stream = std::net::TcpStream::connect(addr).unwrap();

        let data = concat!(
            "POST /path HTTP/1.0\r\n",
            "Host: localhost\r\n",
            "Content-Length: 5\r\n",
            "\r\n",
            "hello"
        )
        .as_bytes();

        stream.write_all(&data).unwrap();

        let mut response = String::new();
        stream.read_to_string(&mut response).unwrap();

        drop(server);

        let request = request.lock().unwrap().take().unwrap();
        assert_eq!(request.method, "POST");
        assert_eq!(request.uri, "/path");
        assert_eq!(request.headers.len(), 2);
        assert_eq!(request.headers[0].0, "Host");
        assert_eq!(str::from_utf8(&request.headers[0].1).unwrap(), "localhost");
        assert_eq!(request.headers[1].0, "Content-Length");
        assert_eq!(str::from_utf8(&request.headers[1].1).unwrap(), "5");
        assert_eq!(str::from_utf8(&request.body).unwrap(), "hello");

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

        assert_eq!(body, "world\n");
    }
}
