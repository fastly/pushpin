use http_body_util::Full;
use hyper::body::Bytes;
use hyper::Response;
use hyperlocal::UnixListenerExt;
use serde_json::json;
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
                println!("Incoming request: {:?}", request);
                if request.uri().path() == "/v1/config/service" {
                    let services = serde_json::to_vec(&json!({
                        "service": {
                            "mock-service-123": {
                                "service": "mock-service-123",
                                "version": "1",
                                "features": {
                                    "loader-pushpin-enabled": "1"
                                }
                            }
                        }
                    }))
                    .unwrap();
                    let response: Response<Full<Bytes>> = Response::builder()
                        .status(200)
                        .header("Content-Type", "application/json")
                        .body(Full::from(Bytes::from(services)))
                        .unwrap();

                    println!("Outgoing response: {:?}", response);
                    Ok::<_, hyper::Error>(response)
                } else if request.uri().path() == "/v1/config/service/{}/version/{}/backends" {
                    unimplemented!();
                } else {
                    let response: Response<Full<Bytes>> = Response::builder()
                        .status(404)
                        .body(Full::from(Bytes::from_static(b"Test404")))
                        .unwrap();

                    println!("Outgoing response: {:?}", response);
                    Ok::<_, hyper::Error>(response)
                }
            }
        })
        .await
        .expect("Failed to serve connection.");

    Ok(())
}
