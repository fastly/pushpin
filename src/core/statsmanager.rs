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

use crate::core::prometheus::try_register_process_collector;
use prometheus::{IntCounter, IntGauge};

/// Metrics needed by the `StatsManager` C++ class.
pub struct CommonMetrics {
    registry: prometheus::Registry,
    request_received: IntCounter,
    connection_connected: IntGauge,
    connection_minute: IntCounter,
    message_received: IntCounter,
    message_sent: IntCounter,
    prev_request_received: u32,
    prev_connection_minute: u32,
    prev_message_received: u32,
    prev_message_sent: u32,
}

impl CommonMetrics {
    fn new(prefix: &str) -> Self {
        let trimmed = prefix.trim_end_matches('_');
        let registry = prometheus::Registry::new_custom(
            (!trimmed.is_empty()).then(|| trimmed.to_string()),
            None,
        )
        .expect("failed to create prometheus registry");

        try_register_process_collector(&registry).expect("failed to register process collector");

        let request_received = IntCounter::new("request_received", "Number of requests received")
            .expect("failed to create request_received");
        registry
            .register(Box::new(request_received.clone()))
            .expect("failed to register request_received");

        let connection_connected =
            IntGauge::new("connection_connected", "Number of concurrent connections")
                .expect("failed to create connection_connected");
        registry
            .register(Box::new(connection_connected.clone()))
            .expect("failed to register connection_connected");

        let connection_minute = IntCounter::new(
            "connection_minute",
            "Number of minutes clients have been connected",
        )
        .expect("failed to create connection_minute");
        registry
            .register(Box::new(connection_minute.clone()))
            .expect("failed to register connection_minute");

        let message_received = IntCounter::new(
            "message_received",
            "Number of messages received by the publish API",
        )
        .expect("failed to create message_received");
        registry
            .register(Box::new(message_received.clone()))
            .expect("failed to register message_received");

        let message_sent = IntCounter::new("message_sent", "Number of messages sent to clients")
            .expect("failed to create message_sent");
        registry
            .register(Box::new(message_sent.clone()))
            .expect("failed to register message_sent");

        Self {
            registry,
            request_received,
            connection_connected,
            connection_minute,
            message_received,
            message_sent,
            prev_request_received: 0,
            prev_connection_minute: 0,
            prev_message_received: 0,
            prev_message_sent: 0,
        }
    }

    fn update(
        &mut self,
        request_received: u32,
        connection_connected: u32,
        connection_minute: u32,
        message_received: u32,
        message_sent: u32,
    ) {
        let delta = request_received.saturating_sub(self.prev_request_received);
        if delta > 0 {
            self.request_received.inc_by(delta as u64);
            self.prev_request_received = request_received;
        }

        self.connection_connected.set(connection_connected as i64);

        let delta = connection_minute.saturating_sub(self.prev_connection_minute);
        if delta > 0 {
            self.connection_minute.inc_by(delta as u64);
            self.prev_connection_minute = connection_minute;
        }

        let delta = message_received.saturating_sub(self.prev_message_received);
        if delta > 0 {
            self.message_received.inc_by(delta as u64);
            self.prev_message_received = message_received;
        }

        let delta = message_sent.saturating_sub(self.prev_message_sent);
        if delta > 0 {
            self.message_sent.inc_by(delta as u64);
            self.prev_message_sent = message_sent;
        }
    }
}

mod ffi {
    use super::*;
    use crate::core::prometheus::ffi::PrometheusRegistry;
    use libc::c_char;
    use std::ffi::CStr;

    /// Create a CommonMetrics instance with the given prefix. Any trailing underscore in the prefix
    /// is stripped before use since the underlying prometheus `Registry` will insert one
    /// automatically (e.g. "myinstance_" becomes "myinstance_*"). Returns an opaque handle; call
    /// `statsmanager_commonmetrics_destroy` when done.
    ///
    /// # Safety
    ///
    /// `prefix` must be a valid null-terminated C string in UTF-8 format, or null (treated as
    /// empty).
    #[no_mangle]
    pub unsafe extern "C" fn statsmanager_commonmetrics_create(
        prefix: *const c_char,
    ) -> *mut CommonMetrics {
        let prefix = if prefix.is_null() {
            ""
        } else {
            unsafe { CStr::from_ptr(prefix) }
                .to_str()
                .expect("prefix must be in utf-8 format")
        };

        Box::into_raw(Box::new(CommonMetrics::new(prefix)))
    }

    /// Destroy a metrics handle returned by `statsmanager_commonmetrics_create`.
    ///
    /// # Safety
    ///
    /// `m` must be a valid pointer returned by `statsmanager_commonmetrics_create`, or null.
    #[no_mangle]
    pub unsafe extern "C" fn statsmanager_commonmetrics_destroy(m: *mut CommonMetrics) {
        if !m.is_null() {
            drop(Box::from_raw(m));
        }
    }

    /// Return a pointer to the prometheus registry owned by this instance. The pointer is valid
    /// until `statsmanager_commonmetrics_destroy` is called.
    ///
    /// # Safety
    ///
    /// `m` must be a valid non-null pointer returned by `statsmanager_commonmetrics_create`.
    #[no_mangle]
    pub unsafe extern "C" fn statsmanager_commonmetrics_registry(
        m: *const CommonMetrics,
    ) -> *const PrometheusRegistry {
        let m = unsafe { m.as_ref().unwrap() };

        &m.registry as *const prometheus::Registry as *const PrometheusRegistry
    }

    /// Update all metrics to the current totals. For counters the delta since the last call is
    /// computed internally; `connection_connected` is a gauge and is set directly.
    ///
    /// # Safety
    ///
    /// `m` must be a valid non-null pointer returned by `statsmanager_commonmetrics_create`.
    #[no_mangle]
    pub unsafe extern "C" fn statsmanager_commonmetrics_update(
        m: *mut CommonMetrics,
        request_received: u32,
        connection_connected: u32,
        connection_minute: u32,
        message_received: u32,
        message_sent: u32,
    ) {
        let m = unsafe { m.as_mut().unwrap() };

        m.update(
            request_received,
            connection_connected,
            connection_minute,
            message_received,
            message_sent,
        );
    }
}
