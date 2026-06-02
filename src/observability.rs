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

use opentelemetry::global;
use opentelemetry::trace::{Status, TracerProvider};
use opentelemetry::KeyValue;
use opentelemetry_otlp::WithExportConfig;
use opentelemetry_sdk::propagation::TraceContextPropagator;
use opentelemetry_sdk::trace::{BatchSpanProcessor, SdkTracerProvider, SpanData, SpanProcessor};
use opentelemetry_sdk::Resource;
use std::error::Error;
use std::sync::OnceLock;
use tracing_subscriber::layer::SubscriberExt;

static TRACE_PROVIDER: OnceLock<SdkTracerProvider> = OnceLock::new();

/// Span processor that only exports spans based on status code filters in batch thread.
#[derive(Debug)]
struct ErrorOnlyProcessor {
    inner: BatchSpanProcessor,
}

impl SpanProcessor for ErrorOnlyProcessor {
    fn on_start(&self, span: &mut opentelemetry_sdk::trace::Span, cx: &opentelemetry::Context) {
        self.inner.on_start(span, cx);
    }

    fn on_end(&self, span: SpanData) {
        if matches!(span.status, Status::Error { .. }) {
            self.inner.on_end(span);
        }
    }

    fn force_flush(&self) -> opentelemetry_sdk::error::OTelSdkResult {
        self.inner.force_flush()
    }

    fn shutdown_with_timeout(
        &self,
        timeout: std::time::Duration,
    ) -> opentelemetry_sdk::error::OTelSdkResult {
        self.inner.shutdown_with_timeout(timeout)
    }

    fn set_resource(&mut self, resource: &opentelemetry_sdk::Resource) {
        self.inner.set_resource(resource);
    }
}

/// Record an HTTP status code as error if >= 500.
pub fn trace_status_code(code: u16) {
    tracing::Span::current().record("http.status_code", code as i64);
    if code >= 500 {
        tracing::error!(http.status_code = code, "request failed");
    }
}

pub enum WsCloseSource {
    Client,
    Server,
}

/// Record a WebSocket close code as error if >= 1002.
pub fn trace_ws_close_code(code: u16, source: WsCloseSource) {
    match source {
        WsCloseSource::Client => {
            tracing::Span::current().record("ws.client_close_code", code as i64);
        }
        WsCloseSource::Server => {
            tracing::Span::current().record("ws.server_close_code", code as i64);
        }
    }
    if code >= 1002 {
        tracing::error!(ws.close_code = code, "websocket closed abnormally");
    }
}

/// Initialize OpenTelemetry tracing with an OTLP HTTP exporter.
///
/// Traces go to the collector's `/v1/traces` endpoint for Tempo.
/// The OTLP endpoint is read from the config file (`otel_endpoint` in `[global]`),
/// falling back to `http://localhost:4318` (the default OTLP HTTP port).
///
/// Always call `shutdown_tracing()` before process exit to flush any remaining spans.
pub fn init_tracing(service_name: &str, endpoint: Option<&str>) -> Result<(), Box<dyn Error>> {
    let endpoint = endpoint
        .filter(|s| !s.is_empty())
        .unwrap_or("http://localhost:4318");

    let resource = Resource::builder()
        .with_service_name(service_name.to_string())
        .with_attributes([
            KeyValue::new("service.version", env!("CARGO_PKG_VERSION")),
            KeyValue::new("__tenant", "fanout"),
        ])
        .build();

    let trace_exporter = opentelemetry_otlp::SpanExporter::builder()
        .with_http()
        .with_endpoint(format!("{}/v1/traces", endpoint))
        .build()?;

    let processor = ErrorOnlyProcessor {
        inner: BatchSpanProcessor::builder(trace_exporter).build(),
    };

    let trace_provider = SdkTracerProvider::builder()
        .with_span_processor(processor)
        .with_resource(resource)
        .build();

    let tracer = trace_provider.tracer(service_name.to_string());
    TRACE_PROVIDER
        .set(trace_provider)
        .map_err(|_| "tracer provider already initialized")?;

    let otel_trace_layer = tracing_opentelemetry::layer().with_tracer(tracer);

    let subscriber = tracing_subscriber::registry().with(otel_trace_layer);

    tracing::subscriber::set_global_default(subscriber)?;

    global::set_text_map_propagator(TraceContextPropagator::new());

    Ok(())
}

/// Flush and shut down the trace provider.
pub fn shutdown_tracing() {
    if let Some(provider) = TRACE_PROVIDER.get() {
        if let Err(e) = provider.shutdown() {
            eprintln!("failed to shutdown tracer provider: {}", e);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use opentelemetry::trace::{SpanContext, SpanId, SpanKind, TraceFlags, TraceId, TraceState};
    use opentelemetry::InstrumentationScope;
    use opentelemetry_sdk::trace::{
        BatchConfigBuilder, InMemorySpanExporterBuilder, SpanEvents, SpanLinks,
    };
    use std::borrow::Cow;
    use std::time::{Duration, SystemTime};

    fn new_test_span_data(status: Status) -> SpanData {
        SpanData {
            span_context: SpanContext::new(
                TraceId::from(1u128),
                SpanId::from(1u64),
                TraceFlags::SAMPLED,
                false,
                TraceState::default(),
            ),
            parent_span_id: SpanId::INVALID,
            parent_span_is_remote: false,
            span_kind: SpanKind::Internal,
            name: Cow::Borrowed("test"),
            start_time: SystemTime::now(),
            end_time: SystemTime::now(),
            attributes: vec![],
            dropped_attributes_count: 0,
            events: SpanEvents::default(),
            links: SpanLinks::default(),
            status,
            instrumentation_scope: InstrumentationScope::builder("test").build(),
        }
    }

    fn new_test_processor(
        exporter: opentelemetry_sdk::trace::InMemorySpanExporter,
    ) -> ErrorOnlyProcessor {
        let batch = BatchSpanProcessor::builder(exporter)
            .with_batch_config(
                BatchConfigBuilder::default()
                    .with_scheduled_delay(Duration::from_millis(1))
                    .build(),
            )
            .build();
        ErrorOnlyProcessor { inner: batch }
    }

    #[test]
    fn error_only_processor_exports_error_spans() {
        let exporter = InMemorySpanExporterBuilder::new().build();
        let processor = new_test_processor(exporter.clone());

        let span_data = new_test_span_data(Status::Error {
            description: Cow::Borrowed("request failed"),
        });
        processor.on_end(span_data.clone());
        processor.force_flush().unwrap();

        let exported = exporter.get_finished_spans().unwrap();
        assert_eq!(exported.len(), 1);
        assert_eq!(exported[0], span_data);
    }

    #[test]
    fn error_only_processor_drops_ok_spans() {
        let exporter = InMemorySpanExporterBuilder::new().build();
        let processor = new_test_processor(exporter.clone());

        processor.on_end(new_test_span_data(Status::Ok));
        processor.on_end(new_test_span_data(Status::Unset));
        processor.force_flush().unwrap();

        let exported = exporter.get_finished_spans().unwrap();
        assert_eq!(exported.len(), 0);
    }
}
