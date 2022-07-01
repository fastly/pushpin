mod data_types;
mod errors;
mod heavenly;
mod message_aggregator;
mod message_sender;
mod options;
#[cfg(test)]
mod tests;
pub mod xqd_backoff;
mod xqd_config;

use data_types::ChannelMessage;
use heavenly::uuid::ServiceID;
use hyper::client::HttpConnector;
use message_aggregator::{MessageAggregator, MessageAggregatorSender};
use serde::Deserialize;
use std::error::Error;
use std::fs;
use std::path::PathBuf;
use std::str;
use tokio::runtime::Runtime;
use tokio::sync::mpsc::error::SendError;
use tracing::{debug, error, info, warn};

const SCHEMA_NAME: &'static str = "billing";
const EMITTER_NAME: &'static str = "pushpin";
const EDGE_CONFIGLY_PATH: &'static str = "/etc/fastly/edge-configly.json";

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
    pub received: u64,

    #[serde(default)]
    pub sent: u64,

    #[serde(rename(deserialize = "http-response-sent"), default)]
    #[allow(dead_code)]
    pub http_response_sent: u64,
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

fn send_count(
    sender: &MessageAggregatorSender,
    service_id: &ServiceID,
    metric: &'static str,
    count: u64,
) -> Result<(), Box<dyn Error>> {
    let msg = ChannelMessage {
        id: service_id.clone(),
        metric: metric.into(),
        count,
    };

    match sender.blocking_send(msg) {
        Ok(()) => {}
        Err(SendError(msg)) => {
            return Err(format!("failed to send to aggregator: {:?}", msg).into())
        }
    }

    Ok(())
}

fn process_report(
    service_id: &ServiceID,
    r: &Report,
    sender: &MessageAggregatorSender,
) -> Result<(), Box<dyn Error>> {
    debug!("report: {:?}", r);

    if r.minutes > 0 {
        send_count(
            &sender,
            service_id,
            "pushpin_conn_time_ms",
            r.minutes * 60_000,
        )?;
    }

    if r.received > 0 {
        send_count(&sender, service_id, "pushpin_recv_publishes", r.received)?;
    }

    if r.sent > 0 {
        send_count(&sender, service_id, "pushpin_send_publishes", r.sent)?;
    }

    Ok(())
}

fn process_stats(spec: &str, sender: MessageAggregatorSender) -> Result<(), Box<dyn Error>> {
    let context = zmq::Context::new();

    let sock = context.socket(zmq::SUB)?;
    sock.set_subscribe(b"report ")?;
    sock.connect(spec)?;

    loop {
        let parts = match sock.recv_multipart(0) {
            Ok(parts) => parts,
            Err(zmq::Error::EINTR) => continue,
            Err(e) => {
                error!("zmq recv error: {}", e);
                continue;
            }
        };

        if parts.len() != 1 {
            warn!("received message with parts != 1, skipping");
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

        let pos = match report.route.find(":") {
            Some(pos) => pos,
            None => continue, // skip routes that don't begin with "{service-id}:"
        };

        let service_id = &report.route[..pos];

        let service_id = match ServiceID::from_bytes(service_id.as_bytes()) {
            Ok(s) => s,
            Err(e) => {
                warn!("invalid service ID {}: {}", service_id, e);
                continue;
            }
        };

        if let Err(e) = process_report(&service_id, &report, &sender) {
            error!("failed to process report: {}", e);
        }
    }
}

pub fn start_aggregator(config: &Config) -> Result<MessageAggregatorSender, Box<dyn Error>> {
    let mtls = if !config.cert.is_empty() {
        Some(xqd_config::MutualTlsConfig {
            cert_path: PathBuf::from(&config.cert),
            key_path: PathBuf::from(&config.key),
            ca_path: PathBuf::from(&config.ca),
            dangerous_no_peer_verification: !config.verify,
        })
    } else {
        None
    };

    let opts = xqd_config::RawAggregatorConfig {
        schema_name: SCHEMA_NAME.to_string(),
        emitter: Some(EMITTER_NAME.to_string()),
        datacenter: Some(config.datacenter.clone()),
        server: Some(config.server.clone()),
        queue_size: config.queue_size as usize,
        mode: xqd_config::RawMessageSenderMode::Json {
            url: config.endpoint.clone(),
            mtls,
        },
    };

    let mut connector = HttpConnector::new();
    connector.enforce_http(false);

    Ok(MessageAggregator::spawn(opts, connector)?)
}

// this function never exits cleanly, due to xqd-stats-emitter behavior.
//
// the process_stats_task may complete if it encounters an unrecoverable
// error. however, the aggregator tasks never complete normally and panic if
// they encounter an unrecoverable error. when the process_stats_task
// completes, its sender will be dropped, causing the aggregator tasks to
// panic. the app is configured with panic = "abort", so the app will exit
// in that case.
pub fn run(config: &Config) -> Result<(), Box<dyn Error>> {
    info!("starting...");

    let rt = Runtime::new()?;

    rt.block_on(async {
        // start the aggregator in the background
        let sender = start_aggregator(&config).unwrap();

        let process_stats_task = {
            let spec = config.spec.clone();

            tokio::task::spawn_blocking(move || {
                if let Err(e) = process_stats(&spec, sender) {
                    error!("process_stats failed: {}", e);
                }
            })
        };

        info!("started");

        process_stats_task
            .await
            .expect("process_stats_task exited uncleanly");
    });

    Ok(())
}
