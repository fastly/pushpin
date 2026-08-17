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
use opentelemetry::trace::{SpanId, Status, TraceId, TracerProvider};
use opentelemetry::KeyValue;
use opentelemetry_otlp::WithExportConfig;
use opentelemetry_sdk::propagation::TraceContextPropagator;
use opentelemetry_sdk::trace::{BatchSpanProcessor, SdkTracerProvider, SpanData, SpanProcessor};
use opentelemetry_sdk::Resource;
use std::collections::{HashMap, HashSet, VecDeque};
use std::error::Error;
use std::sync::mpsc;
use std::sync::OnceLock;
use std::time::{Duration, Instant};
use tracing_subscriber::layer::SubscriberExt;

static TRACE_PROVIDER: OnceLock<SdkTracerProvider> = OnceLock::new();

const TRACE_BUFFER_TIMEOUT_SECS: usize = 10;
#[cfg(not(test))]
const MAX_BUFFERED_TRACES: usize = 50_000;
struct BufferedTrace {
    spans: Vec<SpanData>,
    has_error: bool,
    has_root: bool,
}

/// Data sent from the SpanProcessor to the tail sampling thread
enum ProcessorData {
    Span(SpanData),
    Resource(Resource),
    Flush,
}

/// Span processor that launches a background thread, that buffers complete
/// traces and exports only those containing an error span
///
/// Implementation details inspired by OpenTelemetry Collector's tail sampling processor:
/// https://github.com/open-telemetry/opentelemetry-collector-contrib/blob/main/processor/tailsamplingprocessor
#[derive(Debug)]
struct TailSamplingProcessor {
    sender: mpsc::Sender<ProcessorData>,
}

impl SpanProcessor for TailSamplingProcessor {
    fn on_start(&self, _span: &mut opentelemetry_sdk::trace::Span, _cx: &opentelemetry::Context) {}

    fn on_end(&self, span: SpanData) {
        let _ = self.sender.send(ProcessorData::Span(span));
    }

    fn force_flush(&self) -> opentelemetry_sdk::error::OTelSdkResult {
        let _ = self.sender.send(ProcessorData::Flush);
        Ok(())
    }

    // Shutdown is handled by TailSampler::drain() when the sender drops
    fn shutdown_with_timeout(&self, _timeout: Duration) -> opentelemetry_sdk::error::OTelSdkResult {
        Ok(())
    }

    fn set_resource(&mut self, resource: &opentelemetry_sdk::Resource) {
        let _ = self.sender.send(ProcessorData::Resource(resource.clone()));
    }
}

impl TailSamplingProcessor {
    fn new(inner: BatchSpanProcessor) -> Self {
        let (sender, receiver) = mpsc::channel::<ProcessorData>();

        std::thread::Builder::new()
            .name("otel-tail-sampler".into())
            .spawn(move || Self::run_buffer_loop(receiver, inner))
            .expect("failed to spawn tail sampling thread");

        Self { sender }
    }

    fn run_buffer_loop(receiver: mpsc::Receiver<ProcessorData>, inner: BatchSpanProcessor) {
        let mut state = TailSampler::new(inner);

        loop {
            match receiver.recv_timeout(Duration::from_secs(1)) {
                Ok(ProcessorData::Span(span)) => state.handle_span(span),
                Ok(ProcessorData::Flush) => state.flush(),
                Ok(ProcessorData::Resource(r)) => state.set_resource(&r),
                Err(mpsc::RecvTimeoutError::Timeout) => {}
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    state.drain();
                    return;
                }
            }
            state.try_tick();
        }
    }
}

/// Internal state for the tail sampling thread
///
/// - `traces`: map of trace ID to its spans pending export/drop
/// - `time_wheel`: circular buffer of trace ID sets (one slot/second) for expiry
/// - `current_slot`:`time_wheel` index
/// - `insertion_order`: FIFO queue for eviction when at capacity
/// - `inner`: processor that sends spans to the collector
/// - `last_tick`: time-wheel last advanced timestamp
struct TailSampler {
    traces: HashMap<TraceId, BufferedTrace>,
    time_wheel: Vec<HashSet<TraceId>>,
    current_slot: usize,
    insertion_order: VecDeque<TraceId>,
    inner: BatchSpanProcessor,
    last_tick: Instant,
}

impl TailSampler {
    fn new(inner: BatchSpanProcessor) -> Self {
        Self {
            traces: HashMap::new(),
            time_wheel: (0..TRACE_BUFFER_TIMEOUT_SECS)
                .map(|_| HashSet::new())
                .collect(),
            current_slot: 0,
            insertion_order: VecDeque::new(),
            inner,
            last_tick: Instant::now(),
        }
    }

    fn resolve_trace(&mut self, trace: BufferedTrace) {
        if trace.has_error {
            for s in trace.spans {
                self.inner.on_end(s);
            }
        }
    }

    // Buffer a span, evicting the oldest trace if at capacity
    // Immediately flushes if we have both an error and the root span for this trace
    fn handle_span(&mut self, span: SpanData) {
        let trace_id = span.span_context.trace_id();
        let is_error = matches!(span.status, Status::Error { .. });
        let is_root = span.parent_span_id == SpanId::INVALID;

        let is_new = !self.traces.contains_key(&trace_id);

        // Evict the oldest trace if at capacity
        if is_new && self.traces.len() >= MAX_BUFFERED_TRACES {
            while let Some(oldest_id) = self.insertion_order.pop_front() {
                if let Some(evicted) = self.traces.remove(&oldest_id) {
                    self.resolve_trace(evicted);
                    break;
                }
            }
        }

        // Add span to its trace buffer
        let entry = self
            .traces
            .entry(trace_id)
            .or_insert_with(|| BufferedTrace {
                spans: Vec::new(),
                has_error: false,
                has_root: false,
            });

        entry.has_error |= is_error;
        entry.has_root |= is_root;
        entry.spans.push(span);

        // Register new trace
        if is_new {
            self.time_wheel[self.current_slot].insert(trace_id);
            self.insertion_order.push_back(trace_id);
        }

        // Export immediately if we have both an error and the root span
        if entry.has_error && entry.has_root {
            let trace = self.traces.remove(&trace_id).unwrap();
            self.resolve_trace(trace);
        }
    }

    // Advance the time-wheel if a second has elapsed, expiring traces in the oldest slot
    fn try_tick(&mut self) {
        if self.last_tick.elapsed() < Duration::from_secs(1) {
            return;
        }
        self.last_tick = Instant::now();

        // Rotate the wheel forward one slot
        let oldest_slot = (self.current_slot + 1) % TRACE_BUFFER_TIMEOUT_SECS;
        let expired_ids = std::mem::take(&mut self.time_wheel[oldest_slot]);

        // Resolve all traces in the expired slot
        for trace_id in expired_ids {
            if let Some(trace) = self.traces.remove(&trace_id) {
                self.resolve_trace(trace);
            }
        }

        self.current_slot = oldest_slot;
    }

    fn flush(&mut self) {
        let _ = self.inner.force_flush();
    }

    fn set_resource(&mut self, resource: &Resource) {
        self.inner.set_resource(resource);
    }

    // Shutdown: export any remaining error traces and shut down the inner processor
    fn drain(&mut self) {
        for (_, trace) in self.traces.drain() {
            if trace.has_error {
                for s in trace.spans {
                    self.inner.on_end(s);
                }
            }
        }
        let _ = self.inner.shutdown_with_timeout(Duration::from_secs(5));
    }
}

/// Record an HTTP status code as error if >= 500
pub fn trace_status_code(code: u16) {
    let span = tracing::Span::current();
    span.record("http.status_code", code as i64);
    if code >= 500 {
        span.record("otel.status_code", "ERROR");
    }
}

pub enum WsCloseSource {
    Client,
    Server,
}

/// Record a WebSocket close code as error if >= 1002
pub fn trace_ws_close_code(code: u16, source: WsCloseSource) {
    let span = tracing::Span::current();
    let field = match source {
        WsCloseSource::Client => "ws.client_close_code",
        WsCloseSource::Server => "ws.server_close_code",
    };
    span.record(field, code as i64);
    if code >= 1002 {
        span.record("otel.status_code", "ERROR");
    }
}

/// Initialize OpenTelemetry tracing with an OTLP HTTP exporter
///
/// Traces go to the collector's `/v1/traces` endpoint for Tempo.
/// The OTLP endpoint is read from the config file (`otel_endpoint` in `[global]`),
/// falling back to `http://127.0.0.1:14318` (the default OTLP HTTP port on cache nodes)
///
/// Always call `shutdown_tracing()` before process exit to flush any remaining spans
pub fn init_tracing(
    service_name: &str,
    endpoint: Option<&str>,
    resource_attributes: Option<&str>,
) -> Result<(), Box<dyn Error>> {
    let env_endpoint = std::env::var("OTEL_EXPORTER_OTLP_ENDPOINT").ok();
    let endpoint = endpoint
        .filter(|s| !s.is_empty())
        .or(env_endpoint.as_deref())
        .unwrap_or("http://127.0.0.1:14318");

    let mut attributes = vec![
        KeyValue::new("service.version", crate::core::version()),
        KeyValue::new("__tenant", "fastly-edge"),
    ];

    if let Some(attrs) = resource_attributes.filter(|s| !s.is_empty()) {
        for pair in attrs.split(',') {
            if let Some((key, value)) = pair.split_once('=') {
                attributes.push(KeyValue::new(
                    key.trim().to_string(),
                    value.trim().to_string(),
                ));
            }
        }
    }

    let resource = Resource::builder()
        .with_service_name(service_name.to_string())
        .with_attributes(attributes)
        .build();

    let trace_exporter = opentelemetry_otlp::SpanExporter::builder()
        .with_http()
        .with_endpoint(format!("{}/v1/traces", endpoint))
        .build()?;

    let processor = TailSamplingProcessor::new(BatchSpanProcessor::builder(trace_exporter).build());

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

/// Flush and shut down the trace provider
pub fn shutdown_tracing() {
    if let Some(provider) = TRACE_PROVIDER.get() {
        if let Err(e) = provider.shutdown() {
            eprintln!("failed to shutdown tracer provider: {}", e);
        }
    }
}

#[cfg(test)]
const MAX_BUFFERED_TRACES: usize = 2;

#[cfg(test)]
mod tests {
    use super::*;
    use opentelemetry::trace::{SpanContext, SpanKind, TraceFlags, TraceState};
    use opentelemetry::InstrumentationScope;
    use opentelemetry_sdk::trace::{
        BatchConfigBuilder, InMemorySpanExporterBuilder, SpanEvents, SpanLinks,
    };
    use std::borrow::Cow;
    use std::time::SystemTime;

    fn new_test_span_data_with_ids(
        trace_id: TraceId,
        span_id: SpanId,
        parent_span_id: SpanId,
        status: Status,
    ) -> SpanData {
        SpanData {
            span_context: SpanContext::new(
                trace_id,
                span_id,
                TraceFlags::SAMPLED,
                false,
                TraceState::default(),
            ),
            parent_span_id,
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
    ) -> TailSamplingProcessor {
        let batch = BatchSpanProcessor::builder(exporter)
            .with_batch_config(
                BatchConfigBuilder::default()
                    .with_scheduled_delay(Duration::from_millis(1))
                    .build(),
            )
            .build();
        TailSamplingProcessor::new(batch)
    }

    #[test]
    fn tail_sampling_exports_full_trace_on_error() {
        let exporter = InMemorySpanExporterBuilder::new().build();
        let processor = new_test_processor(exporter.clone());

        let trace_id = TraceId::from(1u128);
        let root_span_id = SpanId::from(1u64);
        let child_span_id = SpanId::from(2u64);

        // Child span with error arrives first
        processor.on_end(new_test_span_data_with_ids(
            trace_id,
            child_span_id,
            root_span_id,
            Status::Error {
                description: Cow::Borrowed("request failed"),
            },
        ));

        // Root span arrives — triggers immediate flush
        processor.on_end(new_test_span_data_with_ids(
            trace_id,
            root_span_id,
            SpanId::INVALID,
            Status::Unset,
        ));

        std::thread::sleep(Duration::from_millis(50));

        let exported = exporter.get_finished_spans().unwrap();
        assert_eq!(exported.len(), 2);
    }

    #[test]
    fn tail_sampling_drops_ok_traces_on_disconnect() {
        let exporter = InMemorySpanExporterBuilder::new().build();
        let processor = new_test_processor(exporter.clone());

        let trace_id = TraceId::from(2u128);
        let root_span_id = SpanId::from(3u64);
        let child_span_id = SpanId::from(4u64);

        processor.on_end(new_test_span_data_with_ids(
            trace_id,
            child_span_id,
            root_span_id,
            Status::Ok,
        ));

        processor.on_end(new_test_span_data_with_ids(
            trace_id,
            root_span_id,
            SpanId::INVALID,
            Status::Ok,
        ));

        // Dropping the processor disconnects the channel, triggering the drain
        // loop which only exports traces with errors — OK traces get dropped
        drop(processor);
        std::thread::sleep(Duration::from_millis(50));

        let exported = exporter.get_finished_spans().unwrap();
        assert_eq!(exported.len(), 0);
    }

    #[test]
    fn tail_sampling_evicts_under_pressure() {
        let exporter = InMemorySpanExporterBuilder::new().build();
        // Capacity of 2 traces
        let processor = new_test_processor(exporter.clone());

        // Fill with 2 non-error traces
        for i in 1u128..=2 {
            let trace_id = TraceId::from(i);
            processor.on_end(new_test_span_data_with_ids(
                trace_id,
                SpanId::from(i as u64),
                SpanId::from((i + 100) as u64),
                Status::Ok,
            ));
        }

        std::thread::sleep(Duration::from_millis(50));

        // Third trace causes eviction of the oldest (trace 1)
        let trace_id_3 = TraceId::from(3u128);
        processor.on_end(new_test_span_data_with_ids(
            trace_id_3,
            SpanId::from(10u64),
            SpanId::from(11u64),
            Status::Error {
                description: Cow::Borrowed("err"),
            },
        ));
        // Send root to trigger immediate flush of trace 3
        processor.on_end(new_test_span_data_with_ids(
            trace_id_3,
            SpanId::from(11u64),
            SpanId::INVALID,
            Status::Unset,
        ));

        std::thread::sleep(Duration::from_millis(50));

        let exported = exporter.get_finished_spans().unwrap();
        // Only trace 3 (error trace) should be exported (2 spans)
        // Trace 1 was evicted with no error → dropped
        assert_eq!(exported.len(), 2);
    }
}
