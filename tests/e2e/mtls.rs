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
    create_test_config, create_test_dir, setup_loader_with_mock_server, spawn_pushpin_process,
    wait_for_pushpin_ready, TestCleanup, TEST_CERT, TEST_KEY,
};

use openssl::ssl::{SslAcceptor, SslConnector, SslFiletype, SslMethod, SslVerifyMode};
use std::sync::Arc;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;
use tokio::sync::oneshot;
use tokio::time::sleep;

#[tokio::test(flavor = "multi_thread")]
#[ignore]
async fn test_mtls_e2e() {
    println!("[e2e] Starting mTLS end-to-end test");

    let test_dir = create_test_dir().expect("Failed to create test directory");
    let mut cleanup = TestCleanup::new();
    cleanup.set_test_dir(test_dir.clone());

    // Set up loader with mock Fetchly server
    let (generated_routes, _logs) = setup_loader_with_mock_server(&test_dir, &mut cleanup)
        .await
        .expect("Failed to setup loader");
    println!("[e2e] Loader and mock Fetchly server set up");

    // Create test config with extra route for mock origind
    let test_config = create_test_config(&test_dir, &generated_routes, Some("* 127.0.0.1:50051\n"))
        .expect("Failed to create test config");
    println!("[e2e] Test config created with routes to mock origind");

    write_test_certs(&test_dir);

    // Start mock origind server
    let origind_handle = start_mock_origind(
        "127.0.0.1:50051".to_string(),
        "127.0.0.1:8443".to_string(),
        test_dir
            .join("test_server_cert.pem")
            .to_str()
            .unwrap()
            .to_string(),
        test_dir
            .join("test_server_key.pem")
            .to_str()
            .unwrap()
            .to_string(),
    )
    .await;
    cleanup.add_task(origind_handle);
    println!("[e2e] Mock origind started on 127.0.0.1:50051");

    // Give origind time to start
    sleep(Duration::from_secs(1)).await;

    // Start mTLS backend server
    let (backend_ready_tx, backend_ready_rx) = oneshot::channel();
    let backend_handle = start_mtls_backend(
        "127.0.0.1:8443".to_string(),
        test_dir.clone(),
        backend_ready_tx,
    )
    .await;
    cleanup.add_task(backend_handle);

    let _ = tokio::time::timeout(Duration::from_secs(5), backend_ready_rx)
        .await
        .expect("mTLS backend did not become ready");
    println!("[e2e] mTLS backend server started on 127.0.0.1:8443");

    // Start Pushpin with test config
    let pushpin_guard = spawn_pushpin_process(&test_config);
    cleanup.add_process(pushpin_guard);
    println!("[e2e] Pushpin started");

    wait_for_pushpin_ready(Duration::from_secs(10))
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

fn write_test_certs(test_dir: &std::path::Path) {
    std::fs::write(test_dir.join("test_server_cert.pem"), TEST_CERT)
        .expect("Failed to write server cert");
    std::fs::write(test_dir.join("test_server_key.pem"), TEST_KEY)
        .expect("Failed to write server key");
    std::fs::write(test_dir.join("test_ca_cert.pem"), TEST_CERT).expect("Failed to write CA cert");
}

async fn start_mtls_backend(
    addr: String,
    test_dir: std::path::PathBuf,
    ready_tx: oneshot::Sender<()>,
) -> tokio::task::JoinHandle<()> {
    tokio::spawn(async move {
        // Set up SSL acceptor with server certificate and require client certificates
        let mut acceptor = SslAcceptor::mozilla_intermediate(SslMethod::tls()).unwrap();

        acceptor
            .set_private_key_file(
                test_dir.join("test_server_key.pem").to_str().unwrap(),
                SslFiletype::PEM,
            )
            .unwrap();
        acceptor
            .set_certificate_chain_file(test_dir.join("test_server_cert.pem").to_str().unwrap())
            .unwrap();
        acceptor.check_private_key().unwrap();

        acceptor.set_verify(SslVerifyMode::PEER | SslVerifyMode::FAIL_IF_NO_PEER_CERT);
        acceptor
            .set_ca_file(test_dir.join("test_ca_cert.pem").to_str().unwrap())
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
