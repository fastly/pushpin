use crate::common::{RequestRecord, TEST_CERT, TEST_KEY};

use http_body_util::Full;
use hyper::body::Bytes;
use hyper::Response;
use hyperlocal::UnixListenerExt;
use serde_json::json;
use serde_json::Value;
use std::io::{BufRead, BufReader};
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::net::UnixListener;
use tokio::sync::oneshot;
use tokio::time::sleep;

/// This test verifies that mTLS configuration flows correctly through the system:
/// 1. Mock Fetchly provides backend with mTLS config
/// 2. pushpin-loader processes it and generates routes
/// 3. Backend info file is created with certificates
/// 4. Pushpin loads the routes
/// Note: This doesn't test actual mTLS connections. That is done through the dockerized E2E test.
#[tokio::test(flavor = "multi_thread")]
async fn test_mtls_configuration() {
    let mut cleanup = TestCleanup::new();

    // Set up and start the mock Fetchly server
    let socket_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("fastly-build/packaging/mock_server.sock");
    let _ = std::fs::remove_file(&socket_path);
    cleanup.add_file(socket_path.clone());

    let logs = Arc::new(Mutex::new(Vec::new()));

    let (ready_tx, ready_rx) = oneshot::channel();
    let server_handle =
        start_mock_fetchly_server(socket_path.clone(), logs.clone(), ready_tx).await;
    cleanup.set_server_handle(server_handle);

    let _ = tokio::time::timeout(Duration::from_secs(5), ready_rx)
        .await
        .expect("Server did not become ready");

    // Start pushpin-loader
    let loader_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("fastly-build/packaging/pushpin-loader");
    let loader_cwd = loader_path.parent().expect("Loader path has no parent");
    remove_packaging_cache(loader_cwd);

    let loader_guard = spawn_loader(&loader_path, loader_cwd, &socket_path);
    cleanup.set_loader(loader_guard);

    // Verify expected requests are made
    let expected = expected_backends_from_manifest();
    match wait_for_manifest_and_backends(logs.clone(), expected.clone(), Duration::from_secs(10))
        .await
    {
        Ok(()) => {}
        Err(e) => {
            eprintln!("Integration test failed: {}", e);
            panic!("Integration test timed out waiting for requests");
        }
    }

    // Copy the generated routes file to where pushpin expects it
    let generated_routes = loader_cwd.join("routes");
    let pushpin_routes =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/config/routes");
    std::fs::copy(&generated_routes, &pushpin_routes).expect("Failed to copy routes file");

    // Verify the routes file contains backendinfo reference
    let routes_content =
        std::fs::read_to_string(&generated_routes).expect("Failed to read generated routes file");
    assert!(
        routes_content.contains("backendinfo="),
        "Routes file should contain backendinfo path"
    );
    assert!(
        routes_content.contains("mtls-backend"),
        "Routes file should reference the mTLS backend"
    );

    // Start pushpin
    let pushpin_guard = spawn_pushpin();
    cleanup.set_pushpin(pushpin_guard);

    // Wait for pushpin to be ready
    println!("[test] Waiting for pushpin to be ready...");
    match wait_for_pushpin(Duration::from_secs(10)).await {
        Ok(()) => {}
        Err(e) => {
            eprintln!("Failed to start pushpin: {}", e);
            panic!("Pushpin did not become ready");
        }
    }

    // Give pushpin a moment to load routes
    sleep(Duration::from_secs(3)).await;

    // Verify the backend info file was created with correct mTLS configuration
    let backend_info_path =
        loader_cwd.join("backends/mock-service-123:mtls-backend-e76b.backendinfo");
    assert!(
        backend_info_path.exists(),
        "Backend info file should be created"
    );

    let backend_info_content =
        std::fs::read_to_string(&backend_info_path).expect("Failed to read backend info file");
    let backend_info: serde_json::Value =
        serde_json::from_str(&backend_info_content).expect("Failed to parse backend info");

    // Verify mTLS fields are present
    assert!(
        backend_info.get("ssl_client_cert").is_some(),
        "Backend info should contain ssl_client_cert"
    );
    assert!(
        backend_info.get("encrypted_ssl_client_key").is_some(),
        "Backend info should contain encrypted_ssl_client_key"
    );
    assert_eq!(
        backend_info.get("port").and_then(|v| v.as_u64()),
        Some(8443),
        "Backend should be configured for port 8443"
    );

    // Cleanup happens automatically with Drop trait
}

/// Cleanup guard that ensures all test resources are cleaned up
struct TestCleanup {
    loader: Option<std::process::Child>,
    pushpin: Option<std::process::Child>,
    server_handle: Option<tokio::task::JoinHandle<()>>,
    files_to_remove: Vec<std::path::PathBuf>,
}

impl TestCleanup {
    fn new() -> Self {
        Self {
            loader: None,
            pushpin: None,
            server_handle: None,
            files_to_remove: Vec::new(),
        }
    }

    fn set_loader(&mut self, child: std::process::Child) {
        self.loader = Some(child);
    }

    fn set_pushpin(&mut self, child: std::process::Child) {
        self.pushpin = Some(child);
    }

    fn set_server_handle(&mut self, handle: tokio::task::JoinHandle<()>) {
        self.server_handle = Some(handle);
    }

    fn add_file(&mut self, path: std::path::PathBuf) {
        self.files_to_remove.push(path);
    }
}

impl Drop for TestCleanup {
    fn drop(&mut self) {
        // Kill processes
        if let Some(mut loader) = self.loader.take() {
            let _ = loader.kill();
            let _ = loader.wait();
        }
        if let Some(mut pushpin) = self.pushpin.take() {
            let _ = pushpin.kill();
            let _ = pushpin.wait();
        }

        // Abort async server task
        if let Some(handle) = self.server_handle.take() {
            handle.abort();
        }

        // Remove files
        for path in &self.files_to_remove {
            let _ = std::fs::remove_file(path);
        }
    }
}

// Remove any packaging cache so the loader will fetch backends.
fn remove_packaging_cache(packaging_dir: &std::path::Path) {
    let cache_dir = packaging_dir.join("cache");
    if cache_dir.exists() {
        let _ = std::fs::remove_dir_all(&cache_dir);
    }
}

fn spawn_loader(
    loader_path: &std::path::Path,
    loader_cwd: &std::path::Path,
    socket_path: &std::path::Path,
) -> std::process::Child {
    let mut child = Command::new(loader_path)
        .current_dir(loader_cwd)
        .env("LOADER_TEST", socket_path.to_str().unwrap())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap_or_else(|e| panic!("Failed to spawn loader at {:?}: {}", loader_path, e));

    set_stdout_stderr("loader", &mut child);

    child
}

fn spawn_pushpin() -> std::process::Child {
    let mut child = Command::new("cargo")
        .arg("run")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("Failed to spawn pushpin");

    set_stdout_stderr("pushpin", &mut child);

    child
}

fn set_stdout_stderr(app_name: &str, child: &mut std::process::Child) {
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

fn expected_backends_from_manifest() -> Vec<String> {
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

async fn wait_for_manifest_and_backends(
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

async fn wait_for_pushpin(timeout: Duration) -> Result<(), String> {
    let start = tokio::time::Instant::now();

    loop {
        // Try to connect to Pushpin's port
        if tokio::net::TcpStream::connect("127.0.0.1:7999")
            .await
            .is_ok()
        {
            println!("[test] Pushpin is ready!");
            return Ok(());
        }

        if tokio::time::Instant::now().duration_since(start) > timeout {
            return Err("Pushpin did not become ready in time".to_string());
        }

        sleep(Duration::from_millis(50)).await;
    }
}

async fn start_mock_fetchly_server(
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

fn parse_backend_path(path: &str) -> Option<(String, String)> {
    let parts: Vec<&str> = path.split('/').collect();
    assert!(parts.len() >= 8);
    Some((parts[4].to_string(), parts[6].to_string()))
}
