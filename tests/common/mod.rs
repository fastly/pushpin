use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::time::sleep;

/// Environment variable name used to tell pushpin-loader to connect to a mock Fetchly server
#[allow(dead_code)]
pub const LOADER_TEST_ENV: &str = "LOADER_TEST";

#[allow(dead_code)]
#[derive(Clone, Debug)]
pub struct RequestRecord {
    pub method: String,
    pub path: String,
    pub headers: Vec<(String, String)>,
}

#[allow(dead_code)]
pub async fn wait_for_manifest_and_backends(
    logs: Arc<Mutex<Vec<RequestRecord>>>,
    expected_backends: Vec<String>,
    timeout_dur: Duration,
) -> Result<(), String> {
    type RequestSnapshot = (String, String, Vec<(String, String)>);
    let start = tokio::time::Instant::now();

    loop {
        let (found_service, found_backends, should_timeout, snapshot) = {
            let guard = logs.lock().unwrap();

            let found_service = guard.iter().any(|r| {
                r.path == "/v1/config/service"
                    && r.headers
                        .iter()
                        .any(|(k, v)| k.to_lowercase() == "fetchly-consumer" && !v.is_empty())
                    && r.headers.iter().any(|(k, v)| {
                        k.to_lowercase() == "accept"
                            && v.to_lowercase().contains("application/json")
                    })
            });

            let mut found_backends = true;
            for expected in expected_backends.iter() {
                if !guard.iter().any(|r| r.path == *expected) {
                    found_backends = false;
                    break;
                }
            }

            let should_timeout = tokio::time::Instant::now().duration_since(start) > timeout_dur;

            let snapshot: Vec<RequestSnapshot> = if should_timeout {
                guard
                    .iter()
                    .map(|r| (r.method.clone(), r.path.clone(), r.headers.clone()))
                    .collect()
            } else {
                Vec::new()
            };

            (found_service, found_backends, should_timeout, snapshot)
        };

        if found_service && found_backends {
            return Ok(());
        }

        if should_timeout {
            return Err(format!("timeout; logs snapshot: {:?}", snapshot));
        }
        sleep(Duration::from_millis(50)).await;
    }
}

/// Test certificate for mTLS testing (used for CA, server, and client)
#[allow(dead_code)]
pub const TEST_CERT: &str = r#"-----BEGIN CERTIFICATE-----
MIIC+zCCAeOgAwIBAgIJAK5NJY1Pm0F8MA0GCSqGSIb3DQEBCwUAMBQxEjAQBgNV
BAMMCWxvY2FsaG9zdDAeFw0yNTExMTkwMDMwNDNaFw0zNTExMTcwMDMwNDNaMBQx
EjAQBgNVBAMMCWxvY2FsaG9zdDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoC
ggEBAOv3oepDZd5TzVporS2kLM22w2GPnUNRS2MC6j+Bhuh1AprMERrYG0AngVNm
HA9bi1XYfuducns/yvwueOOJcsrL1+CCQ4SuaCOFQ6T6c2OL7FFKbhWZwF6N89mQ
A6DIMx7vREFARggkAaeCNcR+L4ew+DvG4joBj+FFDU4gVW2fvrFLGIJsvYaN0Pdz
FLeJWywxPqZJqbdnmIXlSxYuEngA+yDCfVMMv9gglMS0miG2OrecP+POV7izczqV
0mwKqS9O9cjeZMim/vvZXrMMmH+tBJNEKLd7k04pE8mUQNl5BJ1X8IdkPLXcAZj7
eN7OsuJXKE8xVDcDcdy1d5Ka8DcCAwEAAaNQME4wHQYDVR0OBBYEFGzsnaE7NZLi
PH2IN4D6OR5Z4xTeMB8GA1UdIwQYMBaAFGzsnaE7NZLiPH2IN4D6OR5Z4xTeMAwG
A1UdEwQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEBABhWBMg9/g5xsmUYP1ciuFzP
zbknvB8Iijwb9rP3rkNJ4i07ZYbFvdh5AedDEk8slCGezr7MoB00C5SXgJK0gGuR
msuL19sovre2H1KzvMbIPgk0yL3qwpY2HE0crCFR/9lu+g9HMvhPkaT/CpIlfGm8
bdtCUz8wwRfEUoO/3QUxu+AjVwqQRh6sai7HT9PvEKzcxar6puNBdAgSbdOMml7B
5oQghwb+uDbqF7oqDdCjgu9zo+CDZV/u3OLvlXUccB+cvRgbLuslj8SFNIpioJAi
FLHzY9d3EuR064DlJrVBEs3yrcRmiwnohzHTNGvbkR6rkiiZBfOxaWxLuWWR4i4=
-----END CERTIFICATE-----"#;

#[allow(dead_code)]
pub const TEST_KEY: &str = r#"-----BEGIN PRIVATE KEY-----
MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQDr96HqQ2XeU81a
aK0tpCzNtsNhj51DUUtjAuo/gYbodQKazBEa2BtAJ4FTZhwPW4tV2H7nbnJ7P8r8
LnjjiXLKy9fggkOErmgjhUOk+nNji+xRSm4VmcBejfPZkAOgyDMe70RBQEYIJAGn
gjXEfi+HsPg7xuI6AY/hRQ1OIFVtn76xSxiCbL2GjdD3cxS3iVssMT6mSam3Z5iF
5UsWLhJ4APsgwn1TDL/YIJTEtJohtjq3nD/jzle4s3M6ldJsCqkvTvXI3mTIpv77
2V6zDJh/rQSTRCi3e5NOKRPJlEDZeQSdV/CHZDy13AGY+3jezrLiVyhPMVQ3A3Hc
tXeSmvA3AgMBAAECggEBAMqB9x9JQD+RxAb3Fsi4wQq68urjiZ+gQQZ0FfzyeIk/
AkE/KN7U4G4qeT7Nvv8fHXFLH34Wt4E2ukm6yFzeNPn9/wLaSH+x/gWd9PO9TRH9
n44URm2bShDb2o64naR6rAS8auNk+bU60YCkqug7MydMpX0HrlcByC0YSdbmwOoW
LFD+vtm2g1Ial4rnlGvu8CJo1temHnoQWDhbG5/pUEHBOvGpctzj3ltvKjpQBUkS
tq1YPJVeyoZ661fpXLbIjEY8zNdz8hmr74LEn3Xl3UjbPgCo2JH+JhTQNSp1dCWx
vMXOvOqXvzHt3CikdidMwseu+dUj7c7yURli7p30c8ECgYEA/Sz/8STYWVR3eoTr
hiJfZwDKPvxkuA9SL2uv8mImYx7gF+woFqBq8KPfiRSdvZpXo5NKntcT7IM0lShu
99HiR46kZW79NeWV2AAsA9nfp1nXN50+SNAjs+j437edXdUgr6m83QmOaUyLIrVt
+XB6zfxZmFHRTOkue8xAQM5fS18CgYEA7pl9dW60pf2gPlqARwimYDyMXuYnL/U/
Zvy8PNoYZcxw4hmVQ7sak2Y5Q5Mhg5XqmUDoZ9aDLluBAeyysb8TFYxzyPjICHbS
Hct3TuS7B0GUTJtASxcIF4fAdjhU398DbJqPgK/4/g+zAwRk69U3iY4QWDRxAG1Y
uHky9fvy4ikCgYAEElszZ6JLHX6ma8jV6WubXnniNXDxPN3XgWXdAhMX+QEloVir
tHzDzf7kV36J9BeAu/exQkhBv4Jy+6l9NtTqzbJ7JyPPYBfe13Hdxf1A1cMOU+sS
hLZuSEroXi2burfB23r8GxE9REvgjQZtwo9XDL3sIzS4he+Hcit+X1NNBQKBgQCD
jl/tau/yAgjjuHS4Nd2+fEQNYytCNyARQdLsXnhLUNEA6YgygyyhDyEN0EcPTY6/
l1lNP6qu2cck74SLmsYEAKAqtHleqV0rDm+nMViqJI+HOabWgEl25+PHi0HI+ibi
L8pl8yD/vFpAlKCso7BuiMUcTTXtPV1IwZLGrHd8GQKBgQDGFdbfwZrrnTEADh7t
BER82elqOZ+/OFWD3uNVFmFGpFPEHBMEMA8T356c8etjreWgMVO1ZlHyeGtpdzD8
4MZfp3axDIH6qajBsjsc3j/X8PHFmebONuCEAbtvkGwdULRNM8oCVcf+00n4vEGk
D+5Yx6F3xobZlkCo9JRPeIbMMg==
-----END PRIVATE KEY-----"#;

use http_body_util::Full;
use hyper::body::Bytes;
use hyper::Response;
use hyperlocal::UnixListenerExt;
use serde_json::{json, Value};
use std::io::{BufRead, BufReader};
use std::process::{Command, Stdio};
use tokio::net::UnixListener;
use tokio::sync::oneshot;

/// Remove packaging cache so the loader will fetch backends
#[allow(dead_code)]
pub fn remove_packaging_cache(packaging_dir: &std::path::Path) {
    let cache_dir = packaging_dir.join("cache");
    if cache_dir.exists() {
        let _ = std::fs::remove_dir_all(&cache_dir);
    }
}

/// Spawn pushpin-loader with the given configuration
#[allow(dead_code)]
pub fn spawn_loader(
    loader_path: &std::path::Path,
    loader_cwd: &std::path::Path,
    socket_path: &std::path::Path,
) -> std::process::Child {
    let mut child = Command::new(loader_path)
        .current_dir(loader_cwd)
        .env(LOADER_TEST_ENV, socket_path.to_str().unwrap())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap_or_else(|e| panic!("Failed to spawn loader at {:?}: {}", loader_path, e));

    set_stdout_stderr("loader", &mut child);
    child
}

/// Redirect child process stdout/stderr to test output with prefix
#[allow(dead_code)]
pub fn set_stdout_stderr(app_name: &str, child: &mut std::process::Child) {
    if let Some(out) = child.stdout.take() {
        let app_name = app_name.to_string();
        std::thread::spawn(move || {
            let reader = BufReader::new(out);
            for line in reader.lines() {
                match line {
                    Ok(l) => println!("[{} stdout] {}", app_name, l),
                    Err(_) => break,
                }
            }
        });
    }

    if let Some(err) = child.stderr.take() {
        let app_name = app_name.to_string();
        std::thread::spawn(move || {
            let reader = BufReader::new(err);
            for line in reader.lines() {
                match line {
                    Ok(l) => eprintln!("[{} stderr] {}", app_name, l),
                    Err(_) => break,
                }
            }
        });
    }
}

/// Start a mock Fetchly server on a Unix socket
#[allow(dead_code)]
pub async fn start_mock_fetchly_server(
    socket_path: std::path::PathBuf,
    logs: Arc<Mutex<Vec<RequestRecord>>>,
    ready_tx: oneshot::Sender<()>,
) -> tokio::task::JoinHandle<()> {
    tokio::spawn(async move {
        if std::fs::metadata(&socket_path).is_ok() {
            let _ = std::fs::remove_file(&socket_path);
        }

        let listener = UnixListener::bind(&socket_path).expect("failed to bind unix socket");
        let _ = ready_tx.send(());

        listener
            .serve(move || {
                let logs = logs.clone();
                move |request| {
                    let logs = logs.clone();
                    async move {
                        let handler = async move {
                            let path = request.uri().path();
                            let response: Response<Full<Bytes>>;

                            if path == "/v1/config/service" {
                                response = get_service_response();
                            } else if path.starts_with("/v1/config/service/")
                                && path.ends_with("/backends")
                            {
                                let (service_id, version) = parse_backend_path(path).unwrap();
                                response = get_backend_response(&service_id, &version);
                            } else {
                                response = Response::builder()
                                    .status(404)
                                    .body(Full::from(Bytes::from_static(b"Test404")))
                                    .unwrap();
                            }

                            let mut guard = logs.lock().unwrap();
                            guard.push(RequestRecord {
                                method: request.method().to_string(),
                                path: request.uri().path().to_string(),
                                headers: request
                                    .headers()
                                    .iter()
                                    .map(|(k, v)| {
                                        (
                                            k.as_str().to_string(),
                                            v.to_str().unwrap_or("").to_string(),
                                        )
                                    })
                                    .collect(),
                            });

                            Ok::<_, hyper::Error>(response)
                        };

                        match tokio::spawn(handler).await {
                            Ok(resp) => resp,
                            Err(_) => {
                                let response = Response::builder()
                                    .status(500)
                                    .body(Full::from(Bytes::from_static(b"ServerError")))
                                    .unwrap();
                                Ok::<_, hyper::Error>(response)
                            }
                        }
                    }
                }
            })
            .await
            .expect("failed to serve");
    })
}

#[allow(dead_code)]
fn get_service_response() -> Response<Full<Bytes>> {
    let body = serde_json::to_vec(&get_mock_services()).unwrap();
    Response::builder()
        .status(200)
        .header("Content-Type", "application/json")
        .header("Content-Length", body.len().to_string())
        .header("ETag", "\"mock-etag\"")
        .header("Connection", "close")
        .body(Full::from(Bytes::from(body)))
        .unwrap()
}

#[allow(dead_code)]
fn get_backend_response(service_id: &str, version: &str) -> Response<Full<Bytes>> {
    let body = serde_json::to_vec(&get_mock_backends_with_mtls(service_id, version)).unwrap();
    Response::builder()
        .status(200)
        .header("Content-Type", "application/json")
        .header("Content-Length", body.len().to_string())
        .header("ETag", "\"mock-backends-etag\"")
        .header("Connection", "close")
        .body(Full::from(Bytes::from(body)))
        .unwrap()
}

#[allow(dead_code)]
fn get_mock_services() -> Value {
    json!({
        "service": {
            "mock-service-123": {
                "service": "mock-service-123",
                "version": "1",
                "features": {
                    "loader-pushpin-enabled": "1"
                }
            }
        }
    })
}

#[allow(dead_code)]
fn get_mock_backends_with_mtls(_service_id: &str, _version: &str) -> Value {
    json!({
        "backends": [
            {
                "name": "mtls-backend",
                "address": "127.0.0.1",
                "port": 8443,
                "service_id": "test-service-mtls",
                "loader-grip-enabled": "1",
                "ssl_client_cert": TEST_CERT,
                "encrypted_ssl_client_key": {
                    "secret_id": "test-key-id",
                    "secret_value": TEST_KEY
                }
            }
        ]
    })
}

#[allow(dead_code)]
fn parse_backend_path(path: &str) -> Option<(String, String)> {
    let parts: Vec<&str> = path.split('/').collect();
    assert!(parts.len() >= 8);
    Some((parts[4].to_string(), parts[6].to_string()))
}

#[allow(dead_code)]
pub fn expected_backends_from_manifest() -> Vec<String> {
    let services_value = get_mock_services();
    let services = services_value.get("service");
    let mut expected_backends = Vec::new();
    if let Some(map) = services.and_then(|v| v.as_object()) {
        for (sid, sobj) in map.iter() {
            if let Some(version) = sobj.get("version").and_then(|v| v.as_str()) {
                expected_backends.push(format!(
                    "/v1/config/service/{}/version/{}/backends",
                    sid, version
                ));
            }
        }
    }
    expected_backends
}
