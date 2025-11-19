/// End-to-end mTLS testing with mock origind
///
/// This test verifies the complete mTLS flow:
/// 1. Mock Fetchly provides backend with mTLS config
/// 2. pushpin-loader processes it and generates routes
/// 3. Pushpin forwards requests to mock origind
/// 4. Mock origind performs mTLS handshake with backend
/// 5. Backend verifies client certificate and responds
///
/// Run with: cargo test test_mtls_e2e -- --ignored --nocapture
use crate::common::{
    expected_backends_from_manifest, remove_packaging_cache, set_stdout_stderr, spawn_loader,
    start_mock_fetchly_server, wait_for_manifest_and_backends, TEST_CERT, TEST_KEY,
};

use openssl::ssl::{SslAcceptor, SslConnector, SslFiletype, SslMethod, SslVerifyMode};
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;
use tokio::sync::oneshot;
use tokio::time::sleep;

#[tokio::test(flavor = "multi_thread")]
#[ignore]
async fn test_mtls_e2e() {
    println!("[e2e] Starting mTLS end-to-end test");

    let mut cleanup = TestCleanup::new();

    // Set up mock Fetchly server
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
        .expect("Fetchly server did not become ready");
    println!("[e2e] Mock Fetchly server started");

    // Start pushpin-loader
    let loader_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("fastly-build/packaging/pushpin-loader");
    let loader_cwd = loader_path.parent().expect("Loader path has no parent");
    remove_packaging_cache(loader_cwd);

    let loader_guard = spawn_loader(&loader_path, loader_cwd, &socket_path);
    cleanup.set_loader(loader_guard);
    println!("[e2e] pushpin-loader started");

    let expected = expected_backends_from_manifest();
    wait_for_manifest_and_backends(logs.clone(), expected, Duration::from_secs(10))
        .await
        .expect("Loader failed to fetch backends");
    println!("[e2e] Loader fetched backends");

    // Copy routes to pushpin location
    let generated_routes = loader_cwd.join("routes");
    let pushpin_routes =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/config/routes");

    // Test route that routes all traffic to mock origind
    let mut routes_content =
        std::fs::read_to_string(&generated_routes).expect("Failed to read generated routes");
    routes_content.push_str("* 127.0.0.1:50051\n");

    std::fs::write(&pushpin_routes, routes_content).expect("Failed to write routes file");
    println!("[e2e] Routes copied and test route added");

    write_test_certs();
    cleanup.set_cleanup_certs();

    // Start mock origind server
    let temp_dir = std::env::temp_dir();
    let origind_handle = start_mock_origind(
        "127.0.0.1:50051".to_string(),
        "127.0.0.1:8443".to_string(),
        temp_dir
            .join("test_server_cert.pem")
            .to_str()
            .unwrap()
            .to_string(),
        temp_dir
            .join("test_server_key.pem")
            .to_str()
            .unwrap()
            .to_string(),
    )
    .await;
    cleanup.set_origind_handle(origind_handle);
    println!("[e2e] Mock origind started on 127.0.0.1:50051");

    // Give origind time to start
    sleep(Duration::from_secs(1)).await;

    // Start mTLS backend server
    let (backend_ready_tx, backend_ready_rx) = oneshot::channel();
    let backend_handle = start_mtls_backend("127.0.0.1:8443".to_string(), backend_ready_tx).await;
    cleanup.set_backend_handle(backend_handle);

    let _ = tokio::time::timeout(Duration::from_secs(5), backend_ready_rx)
        .await
        .expect("mTLS backend did not become ready");
    println!("[e2e] mTLS backend server started on 127.0.0.1:8443");

    // Start Pushpin
    let pushpin_guard = spawn_pushpin();
    cleanup.set_pushpin(pushpin_guard);
    println!("[e2e] Pushpin started");

    wait_for_pushpin()
        .await
        .expect("Pushpin did not become ready");
    println!("[e2e] Pushpin is ready");

    // Give Pushpin time to load routes
    sleep(Duration::from_secs(2)).await;

    // Make request
    println!("[e2e] Making HTTP request...");
    let client = reqwest::Client::new();
    let response = client
        .get("http://127.0.0.1:7999/test")
        .header("Grip-Sig", "grip-sig-placeholder")
        .header("Cookie", "ck:mock-service-123:session=test")
        .timeout(Duration::from_secs(10))
        .send()
        .await
        .expect("Failed to make HTTP request");

    // Verify success
    println!("[e2e] Response status: {}", response.status());
    assert_eq!(response.status(), 200, "Should receive 200 OK");

    let body = response.text().await.expect("Failed to read response body");
    println!("[e2e] Response body: {}", body);
    assert_eq!(body, "mTLS works", "Backend should return success message");

    // Cleanup happens automatically via Drop
}

/// Start a mock origind server that forwards to the mTLS backend
///
/// This is a simplified version that just forwards requests to backends with mTLS.
/// Real origind does connection pooling, health checks, etc. - we just need forwarding for tests.
async fn start_mock_origind(
    listen_addr: String,
    backend_addr: String,
    cert_path: String,
    key_path: String,
) -> tokio::task::JoinHandle<()> {
    tokio::spawn(async move {
        let listener = TcpListener::bind(&listen_addr)
            .await
            .expect("Failed to bind mock origind");

        println!("[mock-origind] Listening on {}", listen_addr);

        // Share these across all connection handlers to avoid cloning on every connection
        let backend_addr = Arc::new(backend_addr);
        let cert_path = Arc::new(cert_path);
        let key_path = Arc::new(key_path);

        loop {
            match listener.accept().await {
                Ok((mut client_stream, client_addr)) => {
                    println!("[mock-origind] Connection from {}", client_addr);
                    let backend_addr = Arc::clone(&backend_addr);
                    let cert_path = Arc::clone(&cert_path);
                    let key_path = Arc::clone(&key_path);

                    tokio::spawn(async move {
                        // Read request from client
                        let mut buf = vec![0u8; 4096];
                        let n = match client_stream.read(&mut buf).await {
                            Ok(n) if n > 0 => n,
                            Ok(_) => {
                                println!("[mock-origind] Client closed connection");
                                return;
                            }
                            Err(e) => {
                                eprintln!("[mock-origind] Failed to read from client: {}", e);
                                return;
                            }
                        };

                        let request = &buf[..n];
                        println!("[mock-origind] Received {} bytes from client", n);

                        // Connect to backend with mTLS
                        let mut ssl_builder = SslConnector::builder(SslMethod::tls()).unwrap();
                        ssl_builder
                            .set_certificate_file(cert_path.as_ref(), SslFiletype::PEM)
                            .unwrap();
                        ssl_builder
                            .set_private_key_file(key_path.as_ref(), SslFiletype::PEM)
                            .unwrap();
                        ssl_builder.set_verify(SslVerifyMode::NONE); // For testing, disable certificate verification

                        let connector = ssl_builder.build();

                        let tcp_stream =
                            match tokio::net::TcpStream::connect(backend_addr.as_ref()).await {
                                Ok(s) => s,
                                Err(e) => {
                                    eprintln!("[mock-origind] Failed to connect to backend: {}", e);
                                    return;
                                }
                            };

                        println!(
                            "[mock-origind] Connected to backend {}",
                            backend_addr.as_ref()
                        );

                        let ssl = connector
                            .configure()
                            .unwrap()
                            .into_ssl("localhost")
                            .unwrap();

                        let mut tls_stream = match tokio_openssl::SslStream::new(ssl, tcp_stream) {
                            Ok(s) => s,
                            Err(e) => {
                                eprintln!("[mock-origind] Failed to create SSL stream: {}", e);
                                return;
                            }
                        };

                        if let Err(e) = std::pin::Pin::new(&mut tls_stream).connect().await {
                            eprintln!("[mock-origind] TLS handshake failed: {}", e);
                            return;
                        }

                        println!("[mock-origind] TLS handshake successful");

                        // Forward client request to backend
                        if let Err(e) = tls_stream.write_all(request).await {
                            eprintln!("[mock-origind] Failed to write to backend: {}", e);
                            return;
                        }

                        // Read response from backend
                        let mut response_buf = vec![0u8; 4096];
                        match tls_stream.read(&mut response_buf).await {
                            Ok(n) => {
                                println!("[mock-origind] Read {} bytes from backend", n);
                                response_buf.truncate(n);
                            }
                            Err(e) => {
                                eprintln!("[mock-origind] Failed to read from backend: {}", e);
                                return;
                            }
                        }

                        println!(
                            "[mock-origind] Received {} bytes from backend",
                            response_buf.len()
                        );

                        // Forward response to client
                        if let Err(e) = client_stream.write_all(&response_buf).await {
                            eprintln!("[mock-origind] Failed to write to client: {}", e);
                            return;
                        }

                        println!("[mock-origind] Request forwarded successfully");
                    });
                }
                Err(e) => {
                    eprintln!("[mock-origind] Accept error: {}", e);
                }
            }
        }
    })
}

/// Cleanup guard for e2e test resources
struct TestCleanup {
    loader: Option<std::process::Child>,
    pushpin: Option<std::process::Child>,
    backend_handle: Option<tokio::task::JoinHandle<()>>,
    server_handle: Option<tokio::task::JoinHandle<()>>,
    origind_handle: Option<tokio::task::JoinHandle<()>>,
    cleanup_certs: bool,
    files_to_remove: Vec<std::path::PathBuf>,
}

impl TestCleanup {
    fn new() -> Self {
        Self {
            loader: None,
            pushpin: None,
            backend_handle: None,
            server_handle: None,
            origind_handle: None,
            cleanup_certs: false,
            files_to_remove: Vec::new(),
        }
    }

    fn set_loader(&mut self, child: std::process::Child) {
        self.loader = Some(child);
    }

    fn set_pushpin(&mut self, child: std::process::Child) {
        self.pushpin = Some(child);
    }

    fn set_backend_handle(&mut self, handle: tokio::task::JoinHandle<()>) {
        self.backend_handle = Some(handle);
    }

    fn set_server_handle(&mut self, handle: tokio::task::JoinHandle<()>) {
        self.server_handle = Some(handle);
    }

    fn set_origind_handle(&mut self, handle: tokio::task::JoinHandle<()>) {
        self.origind_handle = Some(handle);
    }

    fn set_cleanup_certs(&mut self) {
        self.cleanup_certs = true;
    }

    fn add_file(&mut self, path: std::path::PathBuf) {
        self.files_to_remove.push(path);
    }
}

impl Drop for TestCleanup {
    fn drop(&mut self) {
        println!("[e2e] Cleaning up test resources...");

        if let Some(mut loader) = self.loader.take() {
            let _ = loader.kill();
            let _ = loader.wait();
        }
        if let Some(mut pushpin) = self.pushpin.take() {
            let _ = pushpin.kill();
            let _ = pushpin.wait();
        }

        if let Some(handle) = self.backend_handle.take() {
            handle.abort();
        }
        if let Some(handle) = self.server_handle.take() {
            handle.abort();
        }
        if let Some(handle) = self.origind_handle.take() {
            handle.abort();
        }

        // Give the OS time to reclaim ports after aborting tasks
        std::thread::sleep(std::time::Duration::from_millis(100));

        if self.cleanup_certs {
            cleanup_test_certs();
        }

        for path in &self.files_to_remove {
            let _ = std::fs::remove_file(path);
        }
    }
}

async fn wait_for_pushpin() -> Result<(), String> {
    let start = tokio::time::Instant::now();
    let timeout = Duration::from_secs(15);

    loop {
        if tokio::net::TcpStream::connect("127.0.0.1:7999")
            .await
            .is_ok()
        {
            return Ok(());
        }

        if tokio::time::Instant::now().duration_since(start) > timeout {
            return Err("Pushpin did not become ready in time".to_string());
        }

        sleep(Duration::from_millis(50)).await;
    }
}

fn spawn_pushpin() -> std::process::Child {
    let mut child = Command::new("cargo")
        .arg("run")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("Failed to spawn pushpin");

    set_stdout_stderr("pushpin-e2e", &mut child);
    child
}

fn write_test_certs() {
    let temp_dir = std::env::temp_dir();

    std::fs::write(temp_dir.join("test_server_cert.pem"), TEST_CERT)
        .expect("Failed to write server cert");
    std::fs::write(temp_dir.join("test_server_key.pem"), TEST_KEY)
        .expect("Failed to write server key");
    std::fs::write(temp_dir.join("test_ca_cert.pem"), TEST_CERT).expect("Failed to write CA cert");
}

fn cleanup_test_certs() {
    let temp_dir = std::env::temp_dir();
    let _ = std::fs::remove_file(temp_dir.join("test_server_cert.pem"));
    let _ = std::fs::remove_file(temp_dir.join("test_server_key.pem"));
    let _ = std::fs::remove_file(temp_dir.join("test_ca_cert.pem"));
}

async fn start_mtls_backend(
    addr: String,
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

                        // Read HTTP request and reply with 200 OK
                        let mut buf = vec![0u8; 4096];
                        match tokio::io::AsyncReadExt::read(&mut tls_stream, &mut buf).await {
                            Ok(n) if n > 0 => {
                                println!("[mTLS backend] Received {} bytes", n);
                                let response =
                                    b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nmTLS works";
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
