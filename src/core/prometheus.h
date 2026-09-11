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

#ifndef PROMETHEUS_H
#define PROMETHEUS_H

#include "rust/bindings.h"
#include <QString>
#include <memory>

/// RAII wrapper around the Rust-backed prometheus HTTP server. Destroying this object blocks
/// until the server thread stops.
class PrometheusServer {
public:
    ~PrometheusServer();

    PrometheusServer(const PrometheusServer &) = delete;
    PrometheusServer &operator=(const PrometheusServer &) = delete;

    /// Create and start a prometheus HTTP server listening on `addr`. The registry is cloned
    /// internally so the server is independent of the registry's lifetime. On failure returns
    /// nullptr and writes a description to `*error` if `error` is non-null.
    static std::unique_ptr<PrometheusServer>
    create(const QString &addr, const ffi::PrometheusRegistry *registry, QString *error = nullptr);

private:
    explicit PrometheusServer(ffi::PrometheusServer *handle);
    ffi::PrometheusServer *inner_;
};

#endif
