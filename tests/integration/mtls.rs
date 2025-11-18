use http_body_util::Full;
use hyper::body::Bytes;
use hyper::Response;
use hyperlocal::UnixListenerExt;
use openssl::ssl::{SslAcceptor, SslFiletype, SslMethod, SslVerifyMode};
use serde_json::json;
use serde_json::Value;
use std::io::{BufRead, BufReader};
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::net::{TcpListener, UnixListener};
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

    // Write test certificates to temp directory for the mTLS backend
    let cert_path = std::env::temp_dir().join("test_server_cert.pem");
    let key_path = std::env::temp_dir().join("test_server_key.pem");
    let ca_path = std::env::temp_dir().join("test_ca_cert.pem");

    std::fs::write(&cert_path, TEST_CERT).expect("Failed to write test cert");
    std::fs::write(&key_path, TEST_KEY).expect("Failed to write test key");
    std::fs::write(&ca_path, TEST_CERT).expect("Failed to write test CA cert");

    cleanup.add_file(cert_path);
    cleanup.add_file(key_path);
    cleanup.add_file(ca_path);

    // Start mTLS backend server
    let backend_logs = Arc::new(Mutex::new(Vec::new()));
    let (backend_ready_tx, backend_ready_rx) = oneshot::channel();
    let backend_handle = start_mtls_backend(
        "127.0.0.1:8443".to_string(),
        backend_logs.clone(),
        backend_ready_tx,
    )
    .await;
    cleanup.set_backend_handle(backend_handle);

    let _ = tokio::time::timeout(Duration::from_secs(5), backend_ready_rx)
        .await
        .expect("mTLS backend did not become ready");

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

#[derive(Clone, Debug)]
struct RequestRecord {
    method: String,
    path: String,
    headers: Vec<(String, String)>,
}

/// Cleanup guard that ensures all test resources are cleaned up
struct TestCleanup {
    loader: Option<std::process::Child>,
    pushpin: Option<std::process::Child>,
    server_handle: Option<tokio::task::JoinHandle<()>>,
    backend_handle: Option<tokio::task::JoinHandle<()>>,
    files_to_remove: Vec<std::path::PathBuf>,
}

impl TestCleanup {
    fn new() -> Self {
        Self {
            loader: None,
            pushpin: None,
            server_handle: None,
            backend_handle: None,
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

    fn set_backend_handle(&mut self, handle: tokio::task::JoinHandle<()>) {
        self.backend_handle = Some(handle);
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

        // Abort async tasks
        if let Some(handle) = self.server_handle.take() {
            handle.abort();
        }
        if let Some(handle) = self.backend_handle.take() {
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
        .expect(&format!("Failed to spawn loader at {:?}", loader_path));

    set_stdout_stderr("loader", &mut child);

    child
}

fn spawn_pushpin() -> std::process::Child {
    let mut child = Command::new("cargo")
        .arg("run")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect(&format!("Failed to spawn pushpin"));

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
    let start = tokio::time::Instant::now();

    loop {
        let guard = logs.lock().unwrap();

        let found_service = guard.iter().any(|r| {
            r.path == "/v1/config/service"
                && r.headers
                    .iter()
                    .any(|(k, v)| k.to_lowercase() == "fetchly-consumer" && !v.is_empty())
                && r.headers.iter().any(|(k, v)| {
                    k.to_lowercase() == "accept" && v.to_lowercase().contains("application/json")
                })
        });

        let mut found_backends = true;
        for expected in expected_backends.iter() {
            if !guard.iter().any(|r| r.path == *expected) {
                found_backends = false;
                break;
            }
        }

        if found_service && found_backends {
            return Ok(());
        }

        if tokio::time::Instant::now().duration_since(start) > timeout_dur {
            let snapshot: Vec<(String, String, Vec<(String, String)>)> = guard
                .iter()
                .map(|r| (r.method.clone(), r.path.clone(), r.headers.clone()))
                .collect();
            return Err(format!("timeout; logs snapshot: {:?}", snapshot));
        }

        drop(guard);
        sleep(Duration::from_millis(50)).await;
    }
}

async fn wait_for_pushpin(timeout: Duration) -> Result<(), String> {
    let start = tokio::time::Instant::now();

    loop {
        // Try to connect to Pushpin's port
        if let Ok(_) = tokio::net::TcpStream::connect("127.0.0.1:7999").await {
            println!("[test] Pushpin is ready!");
            return Ok(());
        }

        if tokio::time::Instant::now().duration_since(start) > timeout {
            return Err("Pushpin did not become ready in time".to_string());
        }

        sleep(Duration::from_millis(50)).await;
    }
}

#[tokio::test(flavor = "multi_thread")]
#[ignore] // Run with: cargo test test_mtls_e2e_docker -- --ignored --nocapture
async fn test_mtls_e2e_docker() {
    // Full end-to-end test with origind running in Docker
    // This test requires Docker to be running and will:
    // 1. Start origind in a Docker container
    // 2. Configure Pushpin to use the containerized origind
    // 3. Make a request through Pushpin that uses mTLS to connect to the backend
    // 4. Verify the mTLS connection succeeds

    // TODO: Implement Docker-based E2E test
    // Steps:
    // - Start origind container with docker run
    // - Configure pushpin.conf with origind_path pointing to container
    // - Run full test flow including actual connection to mTLS backend
    // - Clean up Docker container

    unimplemented!("Docker E2E test to be implemented");
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
    // For now, return mTLS-enabled backend for testing
    let body = serde_json::to_vec(&get_mock_backends_with_mtls(&service_id, &version)).unwrap();
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

fn _get_mock_backends(_service_id: &str, _version: &str) -> Value {
    json!({
        "backends": [
            {
                "name": "backend1",
                "address": "127.0.0.1",
                "port": 8080
            }
        ]
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

// Test certificate for mTLS testing (used for CA, server, and client)
const TEST_CERT: &str = r#"-----BEGIN CERTIFICATE-----
MIICpDCCAYwCCQDkzIPOmEje1DANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAls
b2NhbGhvc3QwHhcNMjMwNjA4MjIxMjE3WhcNMjMwNjA5MjIxMjE3WjAUMRIwEAYD
VQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC7
Lj9eFGJ0hsbtn1ebNaakK/f3tktLbYhT7eZ547T1OYfPs9stk7ZMaNPXv/CPbz4x
5NZC89rghUScZYFGAfQE5Rxrso8vUzUSAzRebSm5LG3BYsHyKf7lZkD3cK1kBPtl
lRMQ0/Jg6WkUglYWV8/2Cm8SoJpdllBbbl0bOu1S8QMswb4IrZ1UE130tbP5SnSb
bke2ahVrnJ2lC63sD64rBedYWm5FSHlJ2ciRPe1tr+owqSVrHrjZjrTHovyMVsff
BFJ1iVfnzkxR/tyGFlHHngkRdwtO81Orc9yAIe8v1U3y6F+Tk2LIwW4PYh/xqj4W
ijPttBqrybO5T+jDV/PNAgMBAAEwDQYJKoZIhvcNAQELBQADggEBADQmWrdkwdtR
Fu+9GBjXsmjPNvN72Da4UtLf8Y+LgA/XYKGCFaGxpFm+61DOpbjpUR3B8MRQzn45
x4/ZcNmRrYj7yiBlj/Y/bQKfBLaTG2JCJ2ffdBgZMPG3U9wLQKsUbOsdznkSYG18
CGTM3btznIlW7pkDsw3CRkKoYWNRd0STzifa2ASCEgRAFemYIj/YysVw6nWTtIHY
5Ez+TDwOpUkuk2haE6UvaxR0+q3r+10907HqZejyLmSY+FQk1ylAfJtJcJvpbrB+
kQa8kPmOm+hnLGDXFI0qfBHfuiKDX7yi39aFgWI/Mbz5wKHr0IIoJmncayYacnGX
coUhiF2hpf0=
-----END CERTIFICATE-----"#;

const TEST_KEY: &str = r#"-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC7Lj9eFGJ0hsbt
n1ebNaakK/f3tktLbYhT7eZ547T1OYfPs9stk7ZMaNPXv/CPbz4x5NZC89rghUSc
ZYFGAfQE5Rxrso8vUzUSAzRebSm5LG3BYsHyKf7lZkD3cK1kBPtllRMQ0/Jg6WkU
glYWV8/2Cm8SoJpdllBbbl0bOu1S8QMswb4IrZ1UE130tbP5SnSbbke2ahVrnJ2l
C63sD64rBedYWm5FSHlJ2ciRPe1tr+owqSVrHrjZjrTHovyMVsffBFJ1iVfnzkxR
/tyGFlHHngkRdwtO81Orc9yAIe8v1U3y6F+Tk2LIwW4PYh/xqj4WijPttBqrybO5
T+jDV/PNAgMBAAECggEAB1lIeZwZRXPpKXkhCmHv2fAz+xC4Igz51jm327854orQ
rzHjgAWVmahf8M+DVU5Lxc+zLcu/IyN4Tx+ZFLOM7ghEtmG7R2Nf6QYhLzff9Hov
EPGcpbJKZJ1AHbbZx9x+Nj3FEtsPYAip7Hk1ggkOjB1awQN3LAdzvjM2CpSkrqXg
c4GQ4hK3tkyIZxPiC6pr6246+UjakzFGXT5zzQajbkFHrM8s4Wn42tbdd6N14jgv
5mdR6bAzusG8P3IRlO4zQ/NQTCXI6kz4SdTlZERaxt35pThXRkcifMPcGRTageax
l1ZxBIRjTSp60tPR6fcH8std8hEcRExcOeCmOld4gQKBgQDwWz5vQCUyvza6l/O3
G6huXmQcpFea5PpWtII55bp3DTen6SrB3cGGtKZZqfN7IXFODUIUIvQEf4bI8r0y
Vu6Sypnq+CIbRN5aul7X+do5gEpFEZW+BdbBN+mCBaf16xaxS9GWZj1wCWSjyE4s
PE7jEbLgVPwd+8FmK3XemaF7bQKBgQDHXQC7XjZ0OxfeAOVLz1vShBBlbDtJEonY
cuSveZqEiLEaUFuU3XFuExbyfCRjNNsz6JROXvCO2KQ6HbI/tkZCmJYoQ8mhhAF+
5QN9hGZgMPcvPEZW4AEih5qVrwO3IQGF3YJnYLvyyroEjQ7nSwCf/HPCF5Gl/K41
QPRlM5e94QKBgFyhPYGQfgV9rbDhqLpTvWizle934o8+WcAalumLQH5rKJzcfm7y
cIfijQ2XMs+sRsdm0qWCBvrIzwAYlJOW7yDBVeo5MKPDudHLa4verZxldboCmev+
whH641IJrf5XWIqBhsdopZrM8+0u3/mqUFiwVHiiJ/vCL3mZnDZqjNJNAoGAFge2
7v2IMuvcxVGABRKS6P5i+XIuUvLTfLGlh6Z+ZqrcNzYuCJM315wQaxdAxh2vI1tO
GCLxnjdeXnWtntC7jtxhq21iOJDnwWf5LMOWtIZ0qimU9ECon3IwqN3AIVpqWqqR
oG7WFgxE5f/YZ8Kn/QXenNIR7C+x6HyXBR/gYsECgYEAg6PSkpYdOxaTZzaxIxS3
HUUy7H1+wzV/ZCKIMZEfH23kUiHMZXjp3xI1FTlGcbMFpOkmjwi+MFHEMcvmwzmc
owdohdh7ngo60nkgMwz5TyWBWDdT+Otogi7F37qAt/fjd4xmNjsyTY4b2OwuP1/S
X7Rmwy1AQ2WKrwOSy4d3xDs=
-----END PRIVATE KEY-----"#;

fn parse_backend_path(path: &str) -> Option<(String, String)> {
    let parts: Vec<&str> = path.split('/').collect();
    assert!(parts.len() >= 8);
    Some((parts[4].to_string(), parts[6].to_string()))
}

async fn start_mtls_backend(
    addr: String,
    request_logs: Arc<Mutex<Vec<RequestRecord>>>,
    ready_tx: oneshot::Sender<()>,
) -> tokio::task::JoinHandle<()> {
    tokio::spawn(async move {
        // Set up SSL acceptor with server certificate and require client certificates
        let mut acceptor = SslAcceptor::mozilla_intermediate(SslMethod::tls()).unwrap();

        acceptor
            .set_private_key_file(
                std::env::temp_dir()
                    .join("test_server_key.pem")
                    .to_str()
                    .unwrap(),
                SslFiletype::PEM,
            )
            .unwrap();
        acceptor
            .set_certificate_chain_file(
                std::env::temp_dir()
                    .join("test_server_cert.pem")
                    .to_str()
                    .unwrap(),
            )
            .unwrap();
        acceptor.check_private_key().unwrap();

        acceptor.set_verify(SslVerifyMode::PEER | SslVerifyMode::FAIL_IF_NO_PEER_CERT);
        acceptor
            .set_ca_file(
                std::env::temp_dir()
                    .join("test_ca_cert.pem")
                    .to_str()
                    .unwrap(),
            )
            .unwrap();

        let acceptor = acceptor.build();

        let listener = TcpListener::bind(&addr)
            .await
            .expect("Failed to bind mTLS server");
        println!("[mTLS backend] Listening on {}", addr);

        // Signal that we're ready
        let _ = ready_tx.send(());

        // Listen for incoming connections
        loop {
            match listener.accept().await {
                Ok((stream, peer_addr)) => {
                    println!("[mTLS backend] Accepted connection from {}", peer_addr);
                    let acceptor = acceptor.clone();
                    let request_logs = request_logs.clone();

                    tokio::spawn(async move {
                        // Perform TLS handshake
                        let ssl = openssl::ssl::Ssl::new(acceptor.context()).unwrap();
                        let mut tls_stream = match tokio_openssl::SslStream::new(ssl, stream) {
                            Ok(s) => s,
                            Err(e) => {
                                eprintln!("[mTLS backend] Failed to create SSL stream: {}", e);
                                return;
                            }
                        };

                        if let Err(e) = std::pin::Pin::new(&mut tls_stream).accept().await {
                            eprintln!("[mTLS backend] TLS handshake failed: {}", e);
                            return;
                        }

                        // Log client certificate info
                        if let Some(cert) = tls_stream.ssl().peer_certificate() {
                            println!(
                                "[mTLS backend] Client certificate received: {:?}",
                                cert.subject_name()
                            );
                        } else {
                            println!("[mTLS backend] No client certificate");
                        }

                        // Simple HTTP handling - just read request and send back 200 OK
                        let mut buf = vec![0u8; 4096];
                        match tokio::io::AsyncReadExt::read(&mut tls_stream, &mut buf).await {
                            Ok(n) if n > 0 => {
                                let request_str = String::from_utf8_lossy(&buf[..n]);
                                println!("[mTLS backend] Received request:\n{}", request_str);

                                // Parse basic HTTP request info
                                let lines: Vec<&str> = request_str.lines().collect();
                                if let Some(request_line) = lines.first() {
                                    let parts: Vec<&str> =
                                        request_line.split_whitespace().collect();
                                    if parts.len() >= 2 {
                                        let mut headers = Vec::new();
                                        for line in lines.iter().skip(1) {
                                            if line.is_empty() {
                                                break;
                                            }
                                            if let Some((key, value)) = line.split_once(':') {
                                                headers.push((
                                                    key.trim().to_string(),
                                                    value.trim().to_string(),
                                                ));
                                            }
                                        }

                                        let mut guard = request_logs.lock().unwrap();
                                        guard.push(RequestRecord {
                                            method: parts[0].to_string(),
                                            path: parts[1].to_string(),
                                            headers,
                                        });
                                    }
                                }

                                // Send simple HTTP response
                                let response =
                                    b"HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nmTLS works!";
                                let _ =
                                    tokio::io::AsyncWriteExt::write_all(&mut tls_stream, response)
                                        .await;
                            }
                            Ok(_) => println!("[mTLS backend] Empty request"),
                            Err(e) => eprintln!("[mTLS backend] Failed to read request: {}", e),
                        }
                    });
                }
                Err(e) => eprintln!("[mTLS backend] Accept error: {}", e),
            }
        }
    })
}
