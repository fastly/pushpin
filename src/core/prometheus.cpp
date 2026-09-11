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

#include "prometheus.h"

PrometheusServer::PrometheusServer(ffi::PrometheusServer *handle) : inner_(handle) {}

PrometheusServer::~PrometheusServer() { ffi::prometheus_server_destroy(inner_); }

// static
std::unique_ptr<PrometheusServer> PrometheusServer::create(const QString &addr,
                                                           const ffi::PrometheusRegistry *registry,
                                                           QString *error) {
    const char *errStr = nullptr;
    ffi::PrometheusServer *handle =
        ffi::prometheus_server_create(addr.toUtf8().data(), registry, &errStr);
    if (!handle) {
        if (error && errStr)
            *error = QString::fromUtf8(errStr);
        ffi::prometheus_server_error_destroy(errStr);
        return nullptr;
    }
    return std::unique_ptr<PrometheusServer>(new PrometheusServer(handle));
}
