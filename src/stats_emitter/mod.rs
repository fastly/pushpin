use serde::Deserialize;
use stats_emitter::aggregator::AggregatorConfig;
use stats_emitter::aggregator::BillingMessageAggregator;
use stats_emitter::aggregator::OriginMessageAggregator;
use stats_emitter::data_types::DataCenter;
use stats_emitter::data_types::Emitter;
use stats_emitter::data_types::Metric;
use stats_emitter::data_types::SchemaName;
use stats_emitter::data_types::Server;
use stats_emitter::heavenly::region::ComplianceRegion;
use stats_emitter::heavenly::uuid::ServiceID;
use stats_emitter::metrics::PrefixedMetrics;
use stats_emitter::transport::HttpJson;
use stats_emitter::transport::MutualTlsConfig;
use stats_emitter::BillingKey;
use std::convert::TryInto;
use std::error::Error;
use std::fs;
use std::path::PathBuf;
use std::str;
use std::time::Duration;
use tokio;
use tracing::{debug, error, info, warn};

const BILLING_ENDPOINT_SUFFIX: &'static str = "fst-stats-json";
const BILLING_SCHEMA_NAME: &'static str = "billing";
const ORIGIN_ENDPOINT_SUFFIX: &'static str = "ori-stats-json";
const ORIGIN_SCHEMA_NAME: &'static str = "origin";
const EMITTER_NAME: &'static str = "pushpin";
const EDGE_CONFIGLY_PATH: &'static str = "/etc/fastly/edge-configly.json";

const QUEUE_SIZE: usize = 120;

lazy_static::lazy_static! {
    static ref PROM_METRICS: PrefixedMetrics = PrefixedMetrics::new(EMITTER_NAME);
}

pub struct Config {
    pub spec: String,
    pub endpoint: String,
    pub datacenter: String,
    pub server: String,
    pub queue_size: u32,
    pub cert: String,
    pub key: String,
    pub ca: String,
    pub verify: bool,
}

#[derive(serde::Deserialize)]
struct EdgeConfigly {
    cluster: String,
    hostname: String,
}

#[derive(Default)]
pub struct HostInfo {
    pub pop: String,
    pub hostname: String,
}

pub fn get_host_info() -> Result<HostInfo, Box<dyn Error>> {
    let data = match fs::read_to_string(EDGE_CONFIGLY_PATH) {
        Ok(data) => data,
        Err(e) => return Err(format!("{}: {}", e, EDGE_CONFIGLY_PATH).into()),
    };

    let data: EdgeConfigly = match serde_json::from_str(&data) {
        Ok(data) => data,
        Err(e) => return Err(format!("{}: {}", e, EDGE_CONFIGLY_PATH).into()),
    };

    Ok(HostInfo {
        pop: data.cluster,
        hostname: data.hostname,
    })
}

#[derive(Debug, Deserialize)]
struct Report {
    #[serde(default)]
    pub route: String,

    #[serde(default)]
    #[allow(dead_code)]
    pub duration: u64,

    #[serde(default)]
    #[allow(dead_code)]
    pub connections: u64,

    #[serde(default)]
    pub minutes: u64,

    #[serde(default)]
    #[allow(dead_code)]
    pub received: u64,

    #[serde(default)]
    pub sent: u64,

    #[serde(rename(deserialize = "http-response-sent"), default)]
    #[allow(dead_code)]
    pub http_response_sent: u64,

    #[serde(rename(deserialize = "client-header-bytes-received"), default)]
    pub client_header_bytes_received: u64,

    #[serde(rename(deserialize = "client-header-bytes-sent"), default)]
    pub client_header_bytes_sent: u64,

    #[serde(rename(deserialize = "client-content-bytes-received"), default)]
    pub client_content_bytes_received: u64,

    #[serde(rename(deserialize = "client-content-bytes-sent"), default)]
    pub client_content_bytes_sent: u64,

    #[serde(rename(deserialize = "server-header-bytes-received"), default)]
    pub server_header_bytes_received: u64,

    #[serde(rename(deserialize = "server-header-bytes-sent"), default)]
    pub server_header_bytes_sent: u64,

    #[serde(rename(deserialize = "server-content-bytes-received"), default)]
    pub server_content_bytes_received: u64,

    #[serde(rename(deserialize = "server-content-bytes-sent"), default)]
    pub server_content_bytes_sent: u64,
}

struct FanoutMetric<'a> {
    ws: Option<&'a str>,
    fo: Option<&'a str>,
    value: u64,
}

#[allow(dead_code)]
struct Aggregators {
    billing_aggregator: BillingMessageAggregator,
    origin_aggregator: OriginMessageAggregator,
}

impl Aggregators {
    fn send_report(
        &self,
        report: &Report,
        service_id: &ServiceID,
        grip_enabled: bool,
    ) -> Result<(), Box<dyn Error>> {
        debug!("report: {:?}", report);

        let table = [
            FanoutMetric {
                ws: Some("websocket_req_header_bytes"),
                fo: Some("fanout_req_header_bytes"),
                value: report.client_header_bytes_received,
            },
            FanoutMetric {
                ws: Some("websocket_req_body_bytes"),
                fo: Some("fanout_req_body_bytes"),
                value: report.client_content_bytes_received,
            },
            FanoutMetric {
                ws: Some("websocket_resp_header_bytes"),
                fo: Some("fanout_resp_header_bytes"),
                value: report.client_header_bytes_sent,
            },
            FanoutMetric {
                ws: Some("websocket_resp_body_bytes"),
                fo: Some("fanout_resp_body_bytes"),
                value: report.client_content_bytes_sent,
            },
            FanoutMetric {
                ws: Some("websocket_bereq_header_bytes"),
                fo: Some("fanout_bereq_header_bytes"),
                value: report.server_header_bytes_sent,
            },
            FanoutMetric {
                ws: Some("websocket_bereq_body_bytes"),
                fo: Some("fanout_bereq_body_bytes"),
                value: report.server_content_bytes_sent,
            },
            FanoutMetric {
                ws: Some("websocket_beresp_header_bytes"),
                fo: Some("fanout_beresp_header_bytes"),
                value: report.server_header_bytes_received,
            },
            FanoutMetric {
                ws: Some("websocket_beresp_body_bytes"),
                fo: Some("fanout_beresp_body_bytes"),
                value: report.server_content_bytes_received,
            },
            FanoutMetric {
                ws: Some("websocket_conn_time_ms"),
                fo: Some("fanout_conn_time_ms"),
                value: report.minutes * 60_000,
            },
            FanoutMetric {
                ws: None,
                fo: Some("fanout_send_publishes"),
                value: report.sent,
            },
        ];

        for metric in table {
            let name = if grip_enabled { metric.fo } else { metric.ws };

            if let Some(name) = name {
                self.send_metric(service_id, name, metric.value)?;
            }
        }

        Ok(())
    }

    fn send_metric(
        &self,
        service_id: &ServiceID,
        metric_name: &'static str,
        count: u64,
    ) -> Result<(), Box<dyn Error>> {
        let key = BillingKey::new(service_id.clone(), ComplianceRegion::None);
        if let Some(metric) = Metric::new(&key, metric_name.into(), count) {
            self.billing_aggregator.increment_metric(&None, metric)
        }
        Ok(())
    }
}

fn spawn_aggregators(config: &Config) -> Result<Aggregators, Box<dyn Error>> {
    let billing_config = AggregatorConfig {
        schema_name: SchemaName::new(BILLING_SCHEMA_NAME),
        emitter: Emitter::new(EMITTER_NAME),
        datacenter: DataCenter::new(config.datacenter.clone()),
        server: Server::new(config.server.clone()),
        queue_size: QUEUE_SIZE,
        frequency: Duration::from_secs(1),
    };
    let endpoint = config.endpoint.clone();
    let billing_endpoint = format!("{endpoint}{BILLING_ENDPOINT_SUFFIX}");
    let billing_uri = billing_endpoint.try_into().unwrap();
    let billing_mtls = MutualTlsConfig {
        cert_path: PathBuf::from(&config.cert),
        key_path: PathBuf::from(&config.key),
        ca_path: PathBuf::from(&config.ca),
        dangerous_no_peer_verification: !config.verify,
    };
    let billing_transport = HttpJson::new(billing_uri, Some(billing_mtls)).unwrap();
    let billing_aggregator =
        BillingMessageAggregator::spawn(billing_config, billing_transport, &*PROM_METRICS);

    let origin_config = AggregatorConfig {
        schema_name: SchemaName::new(ORIGIN_SCHEMA_NAME),
        emitter: Emitter::new(EMITTER_NAME),
        datacenter: DataCenter::new(config.datacenter.clone()),
        server: Server::new(config.server.clone()),
        queue_size: QUEUE_SIZE,
        frequency: Duration::from_secs(1),
    };
    let endpoint = config.endpoint.clone();
    let origin_endpoint = format!("{endpoint}{ORIGIN_ENDPOINT_SUFFIX}");
    let origin_uri = origin_endpoint.try_into().unwrap();
    let origin_mtls = MutualTlsConfig {
        cert_path: PathBuf::from(&config.cert),
        key_path: PathBuf::from(&config.key),
        ca_path: PathBuf::from(&config.ca),
        dangerous_no_peer_verification: !config.verify,
    };
    let origin_transport = HttpJson::new(origin_uri, Some(origin_mtls)).unwrap();
    let origin_aggregator =
        OriginMessageAggregator::spawn(origin_config, origin_transport, &*PROM_METRICS);

    Ok(Aggregators {
        billing_aggregator,
        origin_aggregator,
    })
}

fn process_stats(spec: &str, aggregators: Aggregators) -> Result<(), Box<dyn Error>> {
    let context = zmq::Context::new();
    let sock = context.socket(zmq::SUB)?;
    sock.set_subscribe(b"report ")?;
    sock.connect(spec)?;

    loop {
        let parts = match sock.recv_multipart(0) {
            Ok(parts) => parts,
            Err(zmq::Error::EINTR) => continue,
            Err(e) => {
                error!("zmq recv_multipart error {}", e);
                continue;
            }
        };

        if parts.len() != 1 {
            warn!("process_stats received message with parts > 1, skipping");
            continue;
        }

        let msg = &parts[0];

        let pos = match msg.iter().enumerate().find(|(_, &i)| i == b' ') {
            Some((pos, _)) => pos,
            None => {
                warn!("received message with invalid format, skipping");
                continue;
            }
        };

        let mtype = match str::from_utf8(&msg[..pos]) {
            Ok(s) => s,
            Err(_) => {
                warn!("received message with type that is not utf-8, skipping");
                continue;
            }
        };

        if mtype != "report" {
            continue;
        }

        let payload = &msg[(pos + 1)..];

        if payload.is_empty() || payload[0] != b'J' {
            warn!("received message with unsupported payload format, skipping");
            continue;
        }

        let report: Report = match serde_json::from_slice(&payload[1..]) {
            Ok(r) => r,
            Err(e) => {
                warn!("failed to parse payload as json: {}", e);
                continue;
            }
        };

        let route = report.route.as_str();

        // interpret gr: prefix
        let (route, grip_enabled) = if route.starts_with("gr:") {
            (&route[3..], true)
        } else {
            (route, false)
        };

        // discard transport prefix
        let route = if route.starts_with("ht:") || route.starts_with("ws:") {
            &route[3..]
        } else {
            route
        };

        let service_id = match route.find(":") {
            Some(pos) => &route[..pos],
            None => continue, // skip routes that don't begin with "{service-id}:"
        };

        let service_id = match ServiceID::from_bytes(service_id.as_bytes()) {
            Ok(s) => s,
            Err(e) => {
                warn!("invalid service ID {}: {}", service_id, e);
                continue;
            }
        };

        aggregators.send_report(&report, &service_id, grip_enabled)?
    }
}

pub fn run(config: &Config) -> Result<(), Box<dyn Error>> {
    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(async {
        let aggregators = spawn_aggregators(&config).unwrap();

        let process_stats_task = {
            let spec = config.spec.clone();

            tokio::task::spawn_blocking(move || {
                if let Err(e) = process_stats(&spec, aggregators) {
                    error!("process_stats failed {}", e);
                }
            })
        };

        info!("Stats emitter started");
        process_stats_task
            .await
            .expect("process_stats exited uncleanly");
    });
    Ok(())
}
