use std::env;
use std::process::{Command, ExitCode};

fn main() -> ExitCode {
    let components = ["connmgr-in", "connmgr-out"];

    let args: Vec<String> = env::args().collect();

    if args.len() < 2 || !components.contains(&args[1].as_str()) {
        println!("usage: {} [connmgr-in|connmgr-out]", args[0]);
        return 1.into();
    }

    let name = args[1].as_str();

    Command::new("logger")
        .arg(format!("pushpin-log invoked for component {name}"))
        .status()
        .unwrap();

    Command::new("socat")
        .arg("-U")
        .arg("-")
        .arg(format!("/var/run/pushpin/{name}-debug"))
        .status()
        .unwrap();

    ExitCode::SUCCESS
}
