use http_body_util::Full;
use hyper::body::Bytes;
use hyper::Response;
use hyperlocal::UnixListenerExt;
use serde_json::{json, Value};
use tokio::net::UnixListener;

#[tokio::main]
async fn main() -> Result<(), std::io::Error> {
    let socket_path = "./fastly-build/packaging/mock_server.sock";

    if std::fs::metadata(socket_path).is_ok() {
        println!("A socket is already present. Deleting...");
        std::fs::remove_file(socket_path).expect("Failed to delete previous socket");
    }

    let listener = UnixListener::bind(socket_path).expect("Failed to bind to unix socket");

    println!("Mock server ready and listening on {}", socket_path);

    listener
        .serve(|| {
            println!("Accepted connection.");

            |request| async move {
                let path = request.uri().path();
                if path == "/v1/config/service" {
                    let body = serde_json::to_vec(&mock_services_json()).unwrap();
                    let response: Response<Full<Bytes>> = Response::builder()
                        .status(200)
                        .header("Content-Type", "application/json")
                        .body(Full::from(Bytes::from(body)))
                        .unwrap();
                    Ok::<_, hyper::Error>(response)
                } else if let Some((service_id, version)) = parse_backend_path(path) {
                    let body =
                        serde_json::to_vec(&mock_backends_json(&service_id, &version)).unwrap();
                    let response: Response<Full<Bytes>> = Response::builder()
                        .status(200)
                        .header("Content-Type", "application/json")
                        .body(Full::from(Bytes::from(body)))
                        .unwrap();
                    Ok::<_, hyper::Error>(response)
                } else {
                    let response: Response<Full<Bytes>> = Response::builder()
                        .status(404)
                        .body(Full::from(Bytes::from_static(b"Test404")))
                        .unwrap();
                    Ok::<_, hyper::Error>(response)
                }
            }
        })
        .await
        .expect("Failed to serve connection.");

    Ok(())
}

fn mock_services_json() -> Value {
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

fn mock_backends_json(_service_id: &str, _version: &str) -> Value {
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
    if path.starts_with("/v1/config/service/") && path.ends_with("/backends") {
        let parts: Vec<&str> = path.split('/').collect();
        if parts.len() >= 8 {
            return Some((parts[4].to_string(), parts[6].to_string()));
        }
    }
    None
}
