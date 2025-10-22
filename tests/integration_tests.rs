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
        .expect("Failed to start mock server");

    // Wait for socket to be available
    for _ in 0..50 {
        if UnixStream::connect("./fastly-build/packaging/mock_server.sock").is_ok() {
            return;
        }
        thread::sleep(Duration::from_millis(100));
    }

    // Start pushpin-loader
    match Command::new("./pushpin-loader")
        .current_dir("fastly-build/packaging")
        .output()
    {
        Ok(output) => match output.status.success() {
            true => {
                println!(
                    "Loader output: {:?}",
                    String::from_utf8_lossy(&output.stdout)
                );
            }
            false => {
                panic!(
                    "pushpin-loader failed with stderr: {}",
                    String::from_utf8_lossy(&output.stderr)
                );
            }
        },
        Err(e) => {
            panic!("Failed to execute pushpin-loader: {}", e);
        }
    };

    assert!(false);

    println!(
        "Server output: {:?}",
        mock_server
            .stdout
            .take()
            .map(|mut s| {
                let mut output = String::new();
                s.read_to_string(&mut output)
                    .expect("Failed to read server output");
                output
            })
            .expect("Failed to get server output")
    );

    // Start pushpin

    // Send request

    // Verify mTLS connection
}
