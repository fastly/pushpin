use crate::common::{
    create_test_config, create_test_dir, setup_loader_with_mock_server, spawn_pushpin_process,
    wait_for_pushpin_ready, TestCleanup,
};

use std::time::Duration;
use tokio::time::sleep;

/// This test verifies that mTLS configuration flows correctly through the system:
/// 1. Mock Fetchly provides backend with mTLS config
/// 2. pushpin-loader processes it and generates routes
/// 3. Backend info file is created with certificates
/// 4. Pushpin starts and loads the routes successfully
/// Note: This doesn't test actual mTLS connections. That is done through the dockerized E2E test.
#[tokio::test(flavor = "multi_thread")]
async fn test_mtls_configuration() {
    let test_dir = create_test_dir().expect("Failed to create test directory");
    let mut cleanup = TestCleanup::new();
    cleanup.set_test_dir(test_dir.clone());

    // Set up loader with mock Fetchly server
    let (generated_routes, _logs) = setup_loader_with_mock_server(&test_dir, &mut cleanup)
        .await
        .expect("Failed to setup loader");

    // Verify the routes file
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

    let loader_cwd = generated_routes
        .parent()
        .expect("Routes file has no parent");
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

    // Create test config pointing to loader's routes file
    let test_config = create_test_config(&test_dir, &generated_routes, None)
        .expect("Failed to create test config");

    // Start Pushpin with test config
    let pushpin_guard = spawn_pushpin_process(&test_config);
    cleanup.add_process(pushpin_guard);

    println!("[test] Waiting for pushpin to be ready...");
    wait_for_pushpin_ready(Duration::from_secs(10))
        .await
        .expect("Pushpin did not become ready - configuration may be invalid");
    println!("[test] Pushpin successfully loaded configuration");

    // Give pushpin a moment to fully load routes
    sleep(Duration::from_secs(2)).await;

    // Verify Pushpin is actually running and responsive by making a request
    // We expect 502 Bad Gateway since the backend isn't running
    let client = reqwest::Client::new();
    let response = client
        .get("http://127.0.0.1:7999/health-check")
        .timeout(Duration::from_secs(5))
        .send()
        .await
        .expect("Pushpin should be responsive");

    println!(
        "[test] Pushpin responded with status: {} (502 expected - backend not running)",
        response.status()
    );
    assert_eq!(
        response.status(),
        502,
        "Should get 502 Bad Gateway since backend isn't running"
    );

    // Cleanup happens automatically with Drop trait
}
