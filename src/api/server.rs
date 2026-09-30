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

use crate::api::ListenSpec;
use crate::core::fs::{set_group, set_user};
use crate::core::net::NetListener;
use crate::core::net::SocketAddr;
use crate::core::simplehttpserver::{self, Request, Response};
use log::info;
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

fn not_found(msg: &str) -> Response {
    Response {
        code: 404,
        reason: "Not Found".to_string(),
        headers: vec![(
            "Content-Type".to_string(),
            "text/plain".to_string().into_bytes(),
        )],
        body: format!("{msg}\n").into_bytes(),
    }
}

fn method_not_allowed(methods: &str) -> Response {
    Response {
        code: 405,
        reason: "Method Not Allowed".to_string(),
        headers: vec![("Allow".to_string(), methods.to_string().into_bytes())],
        body: Vec::new(),
    }
}

async fn publish(req: Request) -> Response {
    match req.method.as_str() {
        "OPTIONS" => return empty_ok(),
        "POST" => {}
        _ => return method_not_allowed("OPTIONS, POST"),
    }

    Response {
        code: 501,
        reason: "Not Implemented".to_string(),
        headers: vec![(
            "Content-Type".to_string(),
            "text/plain".to_string().into_bytes(),
        )],
        body: "Not implemented\n".to_string().into_bytes(),
    }
}

fn publish_noslash() -> Response {
    not_found("Publish endpoint needs trailing slash: publish/")
}

async fn handle_request(req: Request) -> Response {
    match req.uri.as_str() {
        "/publish" => publish_noslash(),
        "/publish/" => publish(req).await,
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
        listen: &ListenSpec,
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

        Ok(Self {
            addr,
            _server: simplehttpserver::Server::new(
                listener,
                simplehttpserver::Config {
                    connections_max: maxconn,
                    headers_size_max,
                    body_size_max,
                },
                simplehttpserver::handler_fn((), move |_, req| Box::pin(handle_request(req))),
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

    #[test]
    fn publish() {
        let server = Server::new(
            1,
            1_024,
            100_000,
            &ListenSpec::Tcp {
                addr: "127.0.0.1:0".parse().unwrap(),
            },
        )
        .unwrap();

        let SocketAddr::Ip(addr) = server.addr() else {
            panic!("expected tcp listen address");
        };

        let mut stream = std::net::TcpStream::connect(addr).unwrap();
        stream
            .write_all(b"POST /publish/ HTTP/1.0\r\nHost: localhost\r\n\r\n")
            .unwrap();

        let mut response = String::new();
        stream.read_to_string(&mut response).unwrap();

        drop(server);

        assert!(
            response.starts_with("HTTP/1.0 501 Not Implemented\r\n"),
            "unexpected response: {}",
            response
        );
    }
}
