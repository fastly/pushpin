use http_body_util::Full;
use hyper::body::Bytes;
use hyper::Response;
use hyperlocal::UnixListenerExt;
use serde_json::{json, Value};
use std::io::{BufRead, BufReader};
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::net::UnixListener;
use tokio::sync::oneshot;
use tokio::time::sleep;

#[derive(Clone, Debug)]
pub struct RequestRecord {
    pub method: String,
    pub path: String,
    pub headers: Vec<(String, String)>,
}

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

/// Test certificate for mTLS testing (used for CA, server, and client
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

/// Test key for mTLS testing (used for CA, server, and client)
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

/// Remove packaging cache dir so the loader will fetch backends
pub fn remove_packaging_cache(packaging_dir: &std::path::Path) {
    let cache_dir = packaging_dir.join("cache");
    if cache_dir.exists() {
        let _ = std::fs::remove_dir_all(&cache_dir);
    }
}

/// Spawn pushpin-loader
pub fn spawn_loader(
    loader_path: &std::path::Path,
    loader_cwd: &std::path::Path,
    key_path: &std::path::Path,
    routes_output: &std::path::Path,
    backends_dir: &std::path::Path,
) -> std::process::Child {
    let mut child = Command::new(loader_path)
        .current_dir(loader_cwd)
        .arg("--key")
        .arg(key_path)
        .arg("--output")
        .arg(routes_output)
        .arg("--backends-dir")
        .arg(backends_dir)
        .arg("--test")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap_or_else(|e| panic!("Failed to spawn loader at {:?}: {}", loader_path, e));

    set_stdout_stderr("loader", &mut child);
    child
}

/// Redirect child process stdout/stderr to test output with prefix
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
                                let parts: Vec<&str> = path.split('/').collect();
                                assert!(parts.len() >= 8);

                                let (service_id, version) =
                                    (parts[4].to_string(), parts[6].to_string());
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

pub fn create_test_dir() -> std::io::Result<std::path::PathBuf> {
    let test_dir = std::env::temp_dir().join(format!("pushpin-test-{}", std::process::id()));
    std::fs::create_dir_all(&test_dir)?;
    Ok(test_dir)
}

/// Create pushpin config with correct routes file path
pub fn create_test_config(
    test_dir: &std::path::Path,
    routes_file: &std::path::Path,
    extra_routes: Option<&str>,
) -> std::io::Result<std::path::PathBuf> {
    let config_path = test_dir.join("pushpin.conf");

    // Read the base config
    let base_config_path =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/config/pushpin.conf");
    let mut config_content = std::fs::read_to_string(&base_config_path)?;

    // Replace routesfile= with absolute path to loader's routes
    let routes_file_str = routes_file.to_str().unwrap();
    config_content = config_content.replace(
        "routesfile=routes",
        &format!("routesfile={}", routes_file_str),
    );

    std::fs::write(&config_path, config_content)?;

    // If extra routes are provided, append them to the routes file
    if let Some(extra) = extra_routes {
        let mut routes_content = std::fs::read_to_string(routes_file)?;
        routes_content.push_str(extra);
        std::fs::write(routes_file, routes_content)?;
    }

    Ok(config_path)
}

/// Create API key file for testing
pub fn create_api_key_file(test_dir: &std::path::Path) -> std::io::Result<std::path::PathBuf> {
    let key_path = test_dir.join("key");
    let mut key_file = std::fs::File::create(&key_path)?;
    std::io::Write::write_all(&mut key_file, b"TestAPIKey1234567890\n")?;
    Ok(key_path)
}

/// Set up and start the loader with mock Fetchly server
/// Returns (loader_guard, logs) for test verification
pub async fn start_loader(
    test_dir: &std::path::Path,
    cleanup: &mut TestCleanup,
) -> std::io::Result<(std::path::PathBuf, Arc<Mutex<Vec<RequestRecord>>>)> {
    let key_path = create_api_key_file(test_dir)?;

    let loader_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("fastly-build/packaging/pushpin-loader");
    let loader_cwd = loader_path.parent().ok_or_else(|| {
        std::io::Error::new(std::io::ErrorKind::NotFound, "Loader path has no parent")
    })?;

    remove_packaging_cache(loader_cwd);
    cleanup.set_loader_dir(loader_cwd.to_path_buf());

    // Set up mock Fetchly server in loader_cwd
    let socket_path = loader_cwd.join("mock_server.sock");
    let logs = Arc::new(Mutex::new(Vec::new()));

    let (ready_tx, ready_rx) = oneshot::channel();
    let server_handle =
        start_mock_fetchly_server(socket_path.clone(), logs.clone(), ready_tx).await;
    cleanup.add_task(server_handle);

    let _ = tokio::time::timeout(Duration::from_secs(5), ready_rx)
        .await
        .map_err(|_| {
            std::io::Error::new(
                std::io::ErrorKind::TimedOut,
                "Fetchly server did not become ready",
            )
        })?;

    // Write routes to test directory instead of shared location
    let routes_path = test_dir.join("routes");
    let backends_dir = test_dir.join("backends");
    let loader_guard = spawn_loader(
        &loader_path,
        loader_cwd,
        &key_path,
        &routes_path,
        &backends_dir,
    );
    cleanup.add_process(loader_guard);

    // Wait for loader to fetch backends
    let expected = expected_backends_from_manifest();
    wait_for_manifest_and_backends(logs.clone(), expected, Duration::from_secs(10))
        .await
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::TimedOut, e))?;

    Ok((routes_path, logs))
}

/// Spawn pushpin with the given config
pub fn spawn_pushpin(config_path: &std::path::Path) -> std::process::Child {
    let pushpin_bin = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("pushpin");

    let mut child = std::process::Command::new(&pushpin_bin)
        .arg("--config")
        .arg(config_path)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("Failed to spawn pushpin");

    set_stdout_stderr("pushpin", &mut child);
    child
}

/// Wait for pushpin to be ready by polling the port
pub async fn wait_for_pushpin_ready(timeout: Duration) -> Result<(), String> {
    let start = tokio::time::Instant::now();
    loop {
        if let Ok(Ok(_)) = tokio::time::timeout(
            Duration::from_millis(100),
            tokio::net::TcpStream::connect("127.0.0.1:7999"),
        )
        .await
        {
            return Ok(());
        }

        if tokio::time::Instant::now().duration_since(start) > timeout {
            return Err("Pushpin did not become ready in time".to_string());
        }

        tokio::time::sleep(Duration::from_millis(100)).await;
    }
}

/// Handles killing processes and cleaning up test artifacts
pub struct TestCleanup {
    processes: Vec<std::process::Child>,
    task_handles: Vec<tokio::task::JoinHandle<()>>,
    loader_dir: Option<std::path::PathBuf>,
    test_dir: Option<std::path::PathBuf>,
}

impl TestCleanup {
    pub fn new() -> Self {
        Self {
            processes: Vec::new(),
            task_handles: Vec::new(),
            loader_dir: None,
            test_dir: None,
        }
    }

    pub fn set_test_dir(&mut self, path: std::path::PathBuf) {
        self.test_dir = Some(path);
    }

    pub fn set_loader_dir(&mut self, path: std::path::PathBuf) {
        self.loader_dir = Some(path);
    }

    pub fn add_process(&mut self, child: std::process::Child) {
        self.processes.push(child);
    }

    pub fn add_task(&mut self, handle: tokio::task::JoinHandle<()>) {
        self.task_handles.push(handle);
    }

    fn cleanup_loader_artifacts(packaging_dir: &std::path::Path) {
        let routes_file = packaging_dir.join("routes");
        if routes_file.exists() {
            let _ = std::fs::remove_file(&routes_file);
        }

        let backends_dir = packaging_dir.join("backends");
        if backends_dir.exists() {
            let _ = std::fs::remove_dir_all(&backends_dir);
        }

        let cache_dir = packaging_dir.join("cache");
        if cache_dir.exists() {
            let _ = std::fs::remove_dir_all(&cache_dir);
        }

        let socket_file = packaging_dir.join("mock_server.sock");
        if socket_file.exists() {
            let _ = std::fs::remove_file(&socket_file);
        }
    }
}

impl Drop for TestCleanup {
    fn drop(&mut self) {
        // Kill all processes and their children
        // The pushpin runner spawns child processes (connmgr, proxy, handler)
        // so we have to kill them explicitly
        for mut process in self.processes.drain(..) {
            let pid = process.id();

            #[cfg(unix)]
            {
                use nix::sys::signal::{kill, Signal};
                use nix::unistd::Pid;

                let _ = kill(Pid::from_raw(pid as i32), Signal::SIGTERM);
                std::thread::sleep(std::time::Duration::from_millis(500));
                let _ = kill(Pid::from_raw(pid as i32), Signal::SIGKILL);

                let _ = std::process::Command::new("pkill")
                    .args(&["-9", "-f", "pushpin-connmgr"])
                    .output();
                let _ = std::process::Command::new("pkill")
                    .args(&["-9", "-f", "pushpin-proxy"])
                    .output();
                let _ = std::process::Command::new("pkill")
                    .args(&["-9", "-f", "pushpin-handler"])
                    .output();
            }

            #[cfg(windows)]
            {
                use std::process::Command as StdCommand;

                let _ = StdCommand::new("taskkill")
                    .args(&["/F", "/T", "/PID", &pid.to_string()])
                    .output();

                let _ = StdCommand::new("taskkill")
                    .args(&["/F", "/IM", "pushpin-connmgr.exe"])
                    .output();
                let _ = StdCommand::new("taskkill")
                    .args(&["/F", "/IM", "pushpin-proxy.exe"])
                    .output();
                let _ = StdCommand::new("taskkill")
                    .args(&["/F", "/IM", "pushpin-handler.exe"])
                    .output();
            }

            let _ = process.wait();
        }

        // Abort all tasks
        for handle in self.task_handles.drain(..) {
            handle.abort();
        }

        std::thread::sleep(std::time::Duration::from_millis(100));

        // Clean up loader artifacts
        if let Some(ref loader_dir) = self.loader_dir {
            Self::cleanup_loader_artifacts(loader_dir);
        }

        // Clean up test directory
        if let Some(ref test_dir) = self.test_dir {
            let _ = std::fs::remove_dir_all(test_dir);
        }
    }
}
