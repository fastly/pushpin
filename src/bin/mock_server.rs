use std::io::Read;
use std::os::unix::net::UnixListener;

fn main() -> Result<(), std::io::Error> {
    let socket_path = "./fastly-build/packaging/mock_server.sock";

    if std::fs::metadata(socket_path).is_ok() {
        println!("A socket is already present. Deleting...");
        std::fs::remove_file(socket_path).expect("Failed to delete previous socket");
    }

    let unix_listener = UnixListener::bind(socket_path).expect("Failed to bind to unix socket");

    println!("Mock server ready and listening on {}", socket_path);

    loop {
        let (mut unix_stream, socket_address) = unix_listener
            .accept()
            .expect("Failed at accepting a connection on the unix listener");

        let mut message = String::new();
        unix_stream
            .read_to_string(&mut message)
            .expect("Failed at reading the unix stream");

        if !message.is_empty() {
            println!("{}", message);
            return Ok(());
        } else {
            return Err(std::io::Error::new(
                std::io::ErrorKind::Other,
                "No message received",
            ));
        }
    }
}
