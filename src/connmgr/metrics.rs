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

use prometheus::Counter;
use std::sync::OnceLock;

static TOTAL_REQUESTS: OnceLock<Counter> = OnceLock::new();

pub fn total_requests() -> &'static Counter {
    TOTAL_REQUESTS.get_or_init(|| {
        Counter::new(
            "requests_total",
            "Total number of requests processed by connmgr",
        )
        .expect("failed to create total_requests counter")
    })
}

pub fn init(registry: &prometheus::Registry) {
    // Pre-initialize metrics so they appear even before any requests arrive,
    // and register them with the provided registry.
    let counter = total_requests();
    registry
        .register(Box::new(counter.clone()))
        .expect("failed to register total_requests counter");
}
