use std::io::Read;
use std::os::unix::net::UnixStream;
use std::process::Command;
use std::thread;
use std::time::Duration;

#[test]
fn test_mtls() {
    // Start mock Fetchly server
    let mut mock_server = Command::new("cargo")
        .args(["run", "--bin", "mock_server"])
        .spawn()
        .expect(&format!("Failed to spawn loader at {:?}", loader_path));

    set_stdout_stderr("loader", &mut child);

    ChildKiller::new(child)
}

fn spawn_pushpin() -> ChildKiller {
    let mut child = Command::new("cargo")
        .arg("run")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect(&format!("Failed to spawn pushpin"));

    set_stdout_stderr("pushpin", &mut child);

    ChildKiller::new(child)
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
    let services_value = get_mock_services_json();
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
        thread::sleep(Duration::from_millis(100));
    }

    // Start pushpin-loader
    let loader_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("fastly-build/packaging/pushpin-loader");
    let loader_cwd = loader_path.parent().expect("Loader path has no parent");
    remove_packaging_cache(loader_cwd);

    let mut loader_guard = spawn_loader(&loader_path, loader_cwd, &socket_path);

    // Verify expected requests are made
    let expected = expected_backends_from_manifest();
    match wait_for_manifest_and_backends(logs.clone(), expected.clone(), Duration::from_secs(10))
        .await
    {
        Ok(()) => {}
        Err(e) => {
            eprintln!("Integration test failed: {}", e);
            loader_guard.kill();
            panic!("Integration test timed out waiting for requests");
        }
    }

    // Start pushpin
    let mut pushpin_guard = spawn_pushpin();

    // Clean up
    loader_guard.kill();
    pushpin_guard.kill();
    let _ = std::fs::remove_file(&socket_path);
    server_handle.abort();
}

async fn start_mock_server(
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
    let body = serde_json::to_vec(&get_mock_services_json()).unwrap();
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
    let body = serde_json::to_vec(&get_mock_backends_json(&service_id, &version)).unwrap();
    Response::builder()
        .status(200)
        .header("Content-Type", "application/json")
        .header("Content-Length", body.len().to_string())
        .header("ETag", "\"mock-backends-etag\"")
        .header("Connection", "close")
        .body(Full::from(Bytes::from(body)))
        .unwrap()
}

fn get_mock_services_json() -> Value {
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

fn get_mock_backends_json(_service_id: &str, _version: &str) -> Value {
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

fn parse_backend_path(path: &str) -> Option<(String, String)> {
    let parts: Vec<&str> = path.split('/').collect();
    assert!(parts.len() >= 8);
    Some((parts[4].to_string(), parts[6].to_string()))
}
