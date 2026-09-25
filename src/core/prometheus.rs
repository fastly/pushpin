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

use crate::core::net::NetListener;
use crate::core::simplehttpserver;
use prometheus::{Encoder, TextEncoder};

const CONNS_MAX: usize = 10;

#[cfg(target_os = "linux")]
fn try_register_process_collector_inner(
    registry: &prometheus::Registry,
) -> Result<(), prometheus::Error> {
    let pc = prometheus::process_collector::ProcessCollector::for_self();

    registry.register(Box::new(pc))
}

pub fn try_register_process_collector(
    _registry: &prometheus::Registry,
) -> Result<(), prometheus::Error> {
    #[cfg(target_os = "linux")]
    try_register_process_collector_inner(_registry)?;

    Ok(())
}

pub struct PrometheusServer {
    _server: simplehttpserver::Server,
}

impl PrometheusServer {
    pub fn new(listener: NetListener, registry: prometheus::Registry) -> Self {
        Self {
            _server: simplehttpserver::Server::new(
                listener,
                simplehttpserver::Config {
                    connections_max: CONNS_MAX,
                    headers_size_max: 4_096,
                    body_size_max: 0,
                },
                simplehttpserver::handler_fn(registry, move |state, req| {
                    Box::pin(async move { process_request(req, state) })
                }),
            ),
        }
    }
}

fn process_request(
    _req: simplehttpserver::Request,
    registry: &prometheus::Registry,
) -> simplehttpserver::Response {
    let metric_families = registry.gather();

    let encoder = TextEncoder::new();

    let mut body = Vec::new();

    if let Err(e) = encoder.encode(&metric_families, &mut body) {
        return simplehttpserver::Response {
            code: 500,
            reason: "Internal Server Error".to_string(),
            headers: vec![(
                "Content-Type".to_string(),
                "text/plain".to_string().into_bytes(),
            )],
            body: format!("Failed to encode metrics: {e}\n").into_bytes(),
        };
    }

    let content_type = encoder.format_type();

    simplehttpserver::Response {
        code: 200,
        reason: "OK".to_string(),
        headers: vec![(
            "Content-Type".to_string(),
            content_type.to_string().into_bytes(),
        )],
        body,
    }
}

pub mod ffi {
    use super::*;
    use crate::core::config::NetListenConfig;
    use libc::c_char;
    use std::ffi::{CStr, CString};

    /// Opaque handle to a `prometheus::Registry`, for use across the FFI boundary.
    pub enum PrometheusRegistry {}

    /// Create and start a prometheus HTTP server listening on `addr`. The provided `registry` is
    /// cloned internally so the server is independent of the registry's lifetime. Returns an opaque
    /// handle; call `prometheus_server_destroy` when done. On failure, returns null and writes a
    /// heap-allocated error string to `*error`; call `prometheus_server_error_destroy` to release
    /// it.
    ///
    /// # Safety
    ///
    /// `addr` must be a valid null-terminated C string. `registry` must be a valid non-null pointer.
    /// `error` must be a valid non-null pointer to a `*const c_char`.
    #[no_mangle]
    pub unsafe extern "C" fn prometheus_server_create(
        addr: *const c_char,
        registry: *const PrometheusRegistry,
        error: *mut *const c_char,
    ) -> *mut PrometheusServer {
        let addr = match CStr::from_ptr(addr).to_str() {
            Ok(addr) => addr,
            Err(e) => {
                *error = CString::new(format!("invalid listen address: {e}"))
                    .unwrap_or_default()
                    .into_raw();
                return std::ptr::null_mut();
            }
        };

        let registry = &*(registry as *const prometheus::Registry);

        let config = match NetListenConfig::from_prometheus_port_str(addr) {
            Ok(c) => c,
            Err(e) => {
                *error = CString::new(e).unwrap_or_default().into_raw();
                return std::ptr::null_mut();
            }
        };

        let listener = match NetListener::bind_config(&config) {
            Ok(l) => l,
            Err(e) => {
                *error = CString::new(e).unwrap_or_default().into_raw();
                return std::ptr::null_mut();
            }
        };

        *error = std::ptr::null();
        Box::into_raw(Box::new(PrometheusServer::new(listener, registry.clone())))
    }

    /// Destroy an error string returned by `prometheus_server_create`.
    ///
    /// # Safety
    ///
    /// `error` must be a pointer previously written by `prometheus_server_create`, or null.
    #[no_mangle]
    pub unsafe extern "C" fn prometheus_server_error_destroy(error: *const c_char) {
        if !error.is_null() {
            drop(CString::from_raw(error as *mut c_char));
        }
    }

    /// Destroy a prometheus server handle returned by `prometheus_server_create`. Blocks until the
    /// server thread has stopped.
    ///
    /// # Safety
    ///
    /// `server` must be a valid pointer returned by `prometheus_server_create`, or null.
    #[no_mangle]
    pub unsafe extern "C" fn prometheus_server_destroy(server: *mut PrometheusServer) {
        if !server.is_null() {
            drop(Box::from_raw(server));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mio::net::TcpListener;
    use std::io::{Read, Write};

    #[test]
    fn request_metrics() {
        let registry =
            prometheus::Registry::new_custom(Some("myprefix".to_string()), None).unwrap();
        let counter = prometheus::Counter::new("test_counter", "a test counter").unwrap();
        registry.register(Box::new(counter.clone())).unwrap();
        counter.inc();

        let listener = TcpListener::bind("127.0.0.1:0".parse().unwrap()).unwrap();
        let addr = listener.local_addr().unwrap();

        let server = PrometheusServer::new(NetListener::Tcp(listener), registry);

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
