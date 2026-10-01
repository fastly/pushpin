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

use crate::api::item::{validate_item, InPublishItems};
use crate::api::ListenSpec;
use crate::core::fs::{set_group, set_user};
use crate::core::net::NetListener;
use crate::core::net::SocketAddr;
use crate::core::simplehttpserver::{self, Request, Response};
use crate::core::zmq::{SpecInfo, ZmqSocket};
use log::{debug, error, info};
use mio::net::{TcpListener, UnixListener};
use std::fs;
use std::io;
use std::os::unix::fs::PermissionsExt;
use std::sync::Mutex;

fn empty_ok() -> Response {
    Response {
        code: 200,
        reason: "OK".to_string(),
        headers: Vec::new(),
        body: Vec::new(),
    }
}

fn ok<T: AsRef<str>>(message: T) -> Response {
    Response {
        code: 200,
        reason: "OK".to_string(),
        headers: vec![(
            "Content-Type".to_string(),
            "text/plain".to_string().into_bytes(),
        )],
        body: format!("{}\n", message.as_ref()).into_bytes(),
    }
}

fn bad_request<T: AsRef<str>>(message: T) -> Response {
    Response {
        code: 400,
        reason: "Bad Request".to_string(),
        headers: vec![(
            "Content-Type".to_string(),
            "text/plain".to_string().into_bytes(),
        )],
        body: format!("{}\n", message.as_ref()).into_bytes(),
    }
}

fn not_found<T: AsRef<str>>(message: T) -> Response {
    Response {
        code: 404,
        reason: "Not Found".to_string(),
        headers: vec![(
            "Content-Type".to_string(),
            "text/plain".to_string().into_bytes(),
        )],
        body: format!("{}\n", message.as_ref()).into_bytes(),
    }
}

fn method_not_allowed<T: AsRef<str>>(methods: T) -> Response {
    Response {
        code: 405,
        reason: "Method Not Allowed".to_string(),
        headers: vec![(
            "Allow".to_string(),
            methods.as_ref().to_string().into_bytes(),
        )],
        body: Vec::new(),
    }
}

struct Context<'a> {
    content_max: usize,
    item_out_sock: &'a Mutex<ZmqSocket>,
}

async fn publish(ctx: &Context<'_>, req: Request) -> Response {
    match req.method.as_str() {
        "OPTIONS" => return empty_ok(),
        "POST" => {}
        _ => return method_not_allowed("OPTIONS, POST"),
    }

    let items: InPublishItems = match serde_json::from_slice(&req.body) {
        Ok(items) => items,
        Err(e) => return bad_request(format!("JSON parse/schema error: {}", e)),
    };

    for (n, item) in items.items.iter().enumerate() {
        let (item, _) = match validate_item(item, ctx.content_max, false) {
            Ok(ret) => ret,
            Err(e) => return bad_request(format!("item {}: {}", n + 1, e)),
        };

        let payload = match item.serialize() {
            Ok(item) => item,
            Err(_) => return bad_request(format!("item {}: failed to serialize", n + 1)),
        };

        let msgs = [
            zmq::Message::from(item.channel.into_bytes()),
            zmq::Message::from(payload),
        ];

        let item_out_sock = ctx.item_out_sock.lock().unwrap();

        if let Err(e) = item_out_sock.send_multipart(msgs, 0) {
            error!("failed to send item: {e}");
        }
    }

    debug!(
        "control: {} {} code=200 items={}",
        req.method,
        req.uri,
        items.items.len()
    );

    ok("Published")
}

fn publish_noslash() -> Response {
    not_found("Publish endpoint needs trailing slash: publish/")
}

struct State {
    content_max: usize,
    item_out_sock: Mutex<ZmqSocket>,
}

async fn handle_request(state: &State, req: Request) -> Response {
    let ctx = Context {
        content_max: state.content_max,
        item_out_sock: &state.item_out_sock,
    };

    match req.uri.as_str() {
        "/publish" => publish_noslash(),
        "/publish/" => publish(&ctx, req).await,
        _ => not_found("Not found"),
    }
}

pub struct Server {
    // Used by tests
    #[allow(dead_code)]
    addr: SocketAddr,

    _server: simplehttpserver::Server,
}

impl Server {
    pub fn new(
        maxconn: usize,
        headers_size_max: usize,
        body_size_max: usize,
        content_max: usize,
        listen: &ListenSpec,
        zmq_context: &zmq::Context,
        item_out_specs: &[SpecInfo],
    ) -> Result<Self, String> {
        let item_out_sock = ZmqSocket::new(zmq_context, zmq::PUB);

        if let Err(e) = item_out_sock.apply_specs(item_out_specs) {
            return Err(format!("failed to set item out specs: {}", e));
        }

        let (listener, addr) = match listen {
            ListenSpec::Tcp { addr } => {
                let l = match TcpListener::bind(*addr) {
                    Ok(l) => l,
                    Err(e) => return Err(format!("failed to bind {}: {}", addr, e)),
                };

                let addr = l.local_addr().unwrap();

                info!("listening on {}", addr);

                (NetListener::Tcp(l), SocketAddr::Ip(addr))
            }
            ListenSpec::Local {
                path,
                mode,
                user,
                group,
            } => {
                // Ensure pipe file doesn't exist
                match fs::remove_file(path) {
                    Ok(()) => {}
                    Err(e) if e.kind() == io::ErrorKind::NotFound => {}
                    Err(e) => panic!("{}", e),
                }

                let l = match UnixListener::bind(path) {
                    Ok(l) => l,
                    Err(e) => return Err(format!("failed to bind {:?}: {}", path, e)),
                };

                if let Some(mode) = mode {
                    let perms = fs::Permissions::from_mode(*mode);

                    if let Err(e) = fs::set_permissions(path, perms) {
                        return Err(format!("failed to set mode on {:?}: {}", path, e));
                    }
                }

                if let Some(user) = user {
                    if let Err(e) = set_user(path, user) {
                        return Err(format!(
                            "failed to set user {:?} on {:?}: {}",
                            user, path, e
                        ));
                    }
                }

                if let Some(group) = group {
                    if let Err(e) = set_group(path, group) {
                        return Err(format!(
                            "failed to set group {:?} on {:?}: {}",
                            group, path, e
                        ));
                    }
                }

                let addr = l.local_addr().unwrap();

                info!("listening on {:?}", addr);

                (NetListener::Unix(l), SocketAddr::Unix(addr))
            }
        };

        let state = State {
            content_max,
            item_out_sock: Mutex::new(item_out_sock),
        };

        Ok(Self {
            addr,
            _server: simplehttpserver::Server::new(
                listener,
                simplehttpserver::Config {
                    connections_max: maxconn,
                    headers_size_max,
                    body_size_max,
                },
                simplehttpserver::handler_fn(state, move |state, req| {
                    Box::pin(handle_request(state, req))
                }),
            ),
        })
    }

    #[cfg(test)]
    pub fn addr(&self) -> &SocketAddr {
        &self.addr
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::str;

    #[test]
    fn publish() {
        let zmq_context = zmq::Context::new();

        let item_in_sock = zmq_context.socket(zmq::SUB).unwrap();
        item_in_sock.set_subscribe(&[]).unwrap();
        item_in_sock.bind("inproc://test-api-publish-item").unwrap();

        let server = Server::new(
            1,
            1_024,
            100_000,
            10_000,
            &ListenSpec::Tcp {
                addr: "127.0.0.1:0".parse().unwrap(),
            },
            &zmq_context,
            &[SpecInfo {
                spec: "inproc://test-api-publish-item".to_string(),
                bind: false,
                ipc_file_mode: 0,
            }],
        )
        .unwrap();

        // Activate the subscription without receiving
        zmq::poll(&mut [item_in_sock.as_poll_item(zmq::POLLIN)], 1).unwrap();

        // Ensure we are subscribed
        std::thread::sleep(std::time::Duration::from_millis(100));

        let SocketAddr::Ip(addr) = server.addr() else {
            panic!("expected tcp listen address");
        };

        let mut stream = std::net::TcpStream::connect(addr).unwrap();

        let items = "{\"items\":[{\"channel\":\"test\",\"formats\":{\"http-stream\":{\"content\":\"hello world\"}}}]}";

        let data = format!(
            concat!(
                "POST /publish/ HTTP/1.0\r\n",
                "Host: localhost\r\n",
                "Content-Length: {}\r\n",
                "\r\n",
                "{}",
            ),
            items.len(),
            items
        );

        stream.write_all(data.as_bytes()).unwrap();

        let mut response = String::new();
        stream.read_to_string(&mut response).unwrap();

        drop(server);

        assert!(
            response.starts_with("HTTP/1.0 200 OK\r\n"),
            "unexpected response: {}",
            response
        );

        let expected =
            "74:7:formats,60:11:http-stream,41:6:action,4:send,7:content,11:hello world,}}}";

        let msgs = item_in_sock.recv_multipart(0).unwrap();
        assert_eq!(msgs.len(), 2);
        assert_eq!(str::from_utf8(&*msgs[0]).unwrap(), "test");
        assert_eq!(str::from_utf8(&*msgs[1]).unwrap(), expected);
    }

    #[test]
    fn publish_content_too_large() {
        const SMALL_CONTENT_MAX: usize = 10;

        let zmq_context = zmq::Context::new();

        let server = Server::new(
            1,
            1_024,
            100_000,
            SMALL_CONTENT_MAX,
            &ListenSpec::Tcp {
                addr: "127.0.0.1:0".parse().unwrap(),
            },
            &zmq_context,
            &[SpecInfo {
                spec: "inproc://test-api-publish-content-too-large-item".to_string(),
                bind: false,
                ipc_file_mode: 0,
            }],
        )
        .unwrap();

        let SocketAddr::Ip(addr) = server.addr() else {
            panic!("expected tcp listen address");
        };

        let mut stream = std::net::TcpStream::connect(addr).unwrap();

        let items = "{\"items\":[{\"channel\":\"test\",\"formats\":{\"http-stream\":{\"content\":\"hello world\"}}}]}";

        let data = format!(
            concat!(
                "POST /publish/ HTTP/1.0\r\n",
                "Host: localhost\r\n",
                "Content-Length: {}\r\n",
                "\r\n",
                "{}",
            ),
            items.len(),
            items
        );

        stream.write_all(data.as_bytes()).unwrap();

        let mut response = String::new();
        stream.read_to_string(&mut response).unwrap();

        drop(server);

        assert!(
            response.starts_with("HTTP/1.0 400 Bad Request\r\n"),
            "unexpected response: {}",
            response
        );
    }
}
