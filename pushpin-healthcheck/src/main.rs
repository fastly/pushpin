use hyper::Client;
use hyperlocal::{UnixClientExt, Uri};
use std::error::Error;
use std::process;

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error + Send + Sync>> {
    let url = Uri::new("/tmp/pushpin.sock", "/").into();

    let client = Client::unix();

    let response = client.get(url).await?;

    if !response.status().is_success() {
        process::exit(1);
    } else {
        process::exit(0);
    }
}
