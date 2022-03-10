use hyper::{Body, Client, Request};
use hyperlocal::{UnixClientExt, Uri};
use std::{error::Error, process};

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error + Send + Sync>> {
    let uri = Uri::new("/tmp/pushpin.sock", "/");
    let req = Request::builder()
        .uri(uri)
        .header("host", "pushpin.healthcheck.test")
        .body(Body::empty())
        .unwrap();

    let client = Client::unix();

    let response = client.request(req).await?;

    if response.status().is_success() {
        process::exit(0);
    } else {
        process::exit(1);
    }
}
