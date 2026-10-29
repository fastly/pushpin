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

use crate::api::distributor::MessageDistributor;
use crate::api::item::{validate_item, InPublishItems};
use crate::api::metrics;
use crate::api::ListenSpec;
use crate::core::fs::{set_group, set_user};
use crate::core::net::NetListener;
use crate::core::net::SocketAddr;
use crate::core::simplehttpserver::{self, Request, Response};
use log::{debug, error, info};
use mio::net::{TcpListener, UnixListener};
use std::fs;
use std::io;
use std::os::unix::fs::PermissionsExt;

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
    distributors: &'a [Box<dyn MessageDistributor>],
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

    let metadata = [];

    for (n, item) in items.items.iter().enumerate() {
        let (item, _) = match validate_item(item, ctx.content_max, false) {
            Ok(ret) => ret,
            Err(e) => return bad_request(format!("item {}: {}", n + 1, e)),
        };

        let payload = match item.serialize() {
            Ok(item) => item,
            Err(_) => return bad_request(format!("item {}: failed to serialize", n + 1)),
        };

        for d in ctx.distributors {
            match d.distribute(&metadata, &item.channel, payload.clone()) {
                Ok(()) => debug!("sent item via {}", d.name()),
                Err(e) => error!("failed to send item via {}: {}", d.name(), e),
            }
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
    distributors: Vec<Box<dyn MessageDistributor>>,
}

async fn handle_request(state: &State, req: Request) -> Response {
    let ctx = Context {
        content_max: state.content_max,
        distributors: &state.distributors,
    };

    metrics::total_requests().inc();

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
        distributors: Vec<Box<dyn MessageDistributor>>,
    ) -> Result<Self, String> {
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
            distributors,
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
    use crate::api::distributor::MetadataValue;
    use std::error::Error;
    use std::io::{Read, Write};
    use std::str;
    use std::sync::{Arc, Mutex};

    struct DistributeCall {
        metadata: Vec<(String, MetadataValue)>,
        channel: String,
        item: Vec<u8>,
    }

    #[derive(Default)]
    struct MockDistributor {
        calls: Arc<Mutex<Vec<DistributeCall>>>,
    }

    impl MessageDistributor for MockDistributor {
        fn name(&self) -> &str {
            "mock"
        }

        fn distribute(
            &self,
            metadata: &[(&str, MetadataValue)],
            channel: &str,
            item: Vec<u8>,
        ) -> Result<(), Box<dyn Error>> {
            let mut calls = self.calls.lock().unwrap();

            calls.push(DistributeCall {
                metadata: metadata
                    .iter()
                    .map(|(k, v)| (k.to_string(), v.clone()))
                    .collect(),
                channel: channel.to_owned(),
                item,
            });

            Ok(())
        }
    }

    #[test]
    fn publish() {
        let distributor = MockDistributor::default();
        let calls = distributor.calls.clone();

        let server = Server::new(
            1,
            1_024,
            100_000,
            10_000,
            &ListenSpec::Tcp {
                addr: "127.0.0.1:0".parse().unwrap(),
            },
            vec![Box::new(distributor)],
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
            response.starts_with("HTTP/1.0 200 OK\r\n"),
            "unexpected response: {}",
            response
        );

        let expected =
            "74:7:formats,60:11:http-stream,41:6:action,4:send,7:content,11:hello world,}}}";

        let calls = &*calls.lock().unwrap();
        assert_eq!(calls.len(), 1);
        assert!(calls[0].metadata.is_empty());
        assert_eq!(calls[0].channel, "test");
        assert_eq!(str::from_utf8(&calls[0].item).unwrap(), expected);
    }

    #[test]
    fn publish_content_too_large() {
        const SMALL_CONTENT_MAX: usize = 10;

        let server = Server::new(
            1,
            1_024,
            100_000,
            SMALL_CONTENT_MAX,
            &ListenSpec::Tcp {
                addr: "127.0.0.1:0".parse().unwrap(),
            },
            Vec::new(),
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
