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

use prometheus::{opts, Counter, CounterVec};
use std::sync::OnceLock;

static TOTAL_REQUESTS: OnceLock<Counter> = OnceLock::new();
static TOTAL_CONNECTS: OnceLock<CounterVec> = OnceLock::new();

pub fn total_requests() -> &'static Counter {
    TOTAL_REQUESTS.get_or_init(|| {
        prometheus::register_counter!(
            "connmgr_requests_total",
            "Total number of requests processed by connmgr"
        )
        .expect("failed to register total_requests counter")
    })
}

pub fn total_connects() -> &'static CounterVec {
    TOTAL_CONNECTS.get_or_init(|| {
        prometheus::register_counter_vec!(
            opts!(
                "connmgr_connects_total",
                "Total number of outbound connections attempted by connmgr"
            ),
            &["status"]
        )
        .expect("failed to register connmgr_connects counter")
    })
}

pub fn init() {
    // Pre-initialize metrics so they appear even before any requests arrive.

    let _ = total_requests();

    for status in ["success", "error"] {
        let _ = total_connects().with_label_values(&[status]);
    }
}
