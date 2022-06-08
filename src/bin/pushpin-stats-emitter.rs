use clap::{App, Arg};
use pushpin::stats_emitter::{get_host_info, run, Config, HostInfo};
use std::env;
use std::error::Error;
use std::process;
use std::str;
use tracing::{error, Level};
use tracing_subscriber::FmtSubscriber;

const PROGRAM_NAME: &str = "pushpin-stats-emitter";

struct Args {
    spec: String,
    endpoint: String,
    datacenter: String,
    server: String,
    queue_size: u32,
    nsq_cert: String,
    nsq_key: String,
    nsq_ca: String,
    verify: bool,
}

fn process_args_and_run(args: Args) -> Result<(), Box<dyn Error>> {
    let config = Config {
        spec: args.spec,
        endpoint: args.endpoint,
        datacenter: args.datacenter,
        server: args.server,
        queue_size: args.queue_size,
        cert: args.nsq_cert,
        key: args.nsq_key,
        ca: args.nsq_ca,
        verify: args.verify,
    };

    run(&config)
}

fn main() {
    let (defaults, defaults_err) = match get_host_info() {
        Ok(i) => (i, None),
        Err(e) => (HostInfo::default(), Some(e)),
    };

    let matches = App::new(PROGRAM_NAME)
        .version(env!("APP_VERSION"))
        .about("Read stats from Pushpin and emit to NSQ")
        .arg(
            Arg::with_name("spec")
                .required(true)
                .takes_value(true)
                .value_name("stats-spec")
                .help("ZeroMQ SUB spec to read from"),
        )
        .arg(
            Arg::with_name("endpoint")
                .required(true)
                .takes_value(true)
                .value_name("nsq-endpoint")
                .help("NSQ endpoint to send to"),
        )
        .arg(
            Arg::with_name("log-level")
                .long("log-level")
                .takes_value(true)
                .value_name("N")
                .help("Log level")
                .default_value("2"),
        )
        .arg(
            Arg::with_name("pop")
                .long("pop")
                .takes_value(true)
                .value_name("name")
                .help("The POP to send metrics for")
                .default_value(&defaults.pop),
        )
        .arg(
            Arg::with_name("hostname")
                .long("hostname")
                .takes_value(true)
                .value_name("name")
                .help("The server name to send metrics for")
                .default_value(&defaults.hostname),
        )
        .arg(
            Arg::with_name("queue-size")
                .long("queue-size")
                .takes_value(true)
                .value_name("N")
                .help("Output queue size")
                .default_value("120"), // same as xqd default
        )
        .arg(
            Arg::with_name("nsq-cert")
                .long("nsq-cert")
                .takes_value(true)
                .value_name("file")
                .help("NSQ client cert"),
        )
        .arg(
            Arg::with_name("nsq-key")
                .long("nsq-key")
                .takes_value(true)
                .value_name("file")
                .help("NSQ client key"),
        )
        .arg(
            Arg::with_name("nsq-ca")
                .long("nsq-ca")
                .takes_value(true)
                .value_name("file")
                .help("NSQ client CA"),
        )
        .arg(
            Arg::with_name("no-verify-peer")
                .long("no-verify-peer")
                .help("Disable peer cert verification"),
        )
        .get_matches();

    let level = matches.value_of("log-level").unwrap();

    let level: usize = match level.parse() {
        Ok(x) => x,
        Err(e) => {
            eprintln!("Error: failed to parse log-level: {}", e);
            process::exit(1);
        }
    };

    let level = match level {
        0 => Level::ERROR,
        1 => Level::WARN,
        2 => Level::INFO,
        3 => Level::DEBUG,
        5..=core::usize::MAX => Level::TRACE,
        _ => unreachable!(),
    };

    let subscriber = FmtSubscriber::builder().with_max_level(level).finish();

    tracing::subscriber::set_global_default(subscriber).expect("setting default subscriber failed");

    let spec = matches.value_of("spec").unwrap();
    let endpoint = matches.value_of("endpoint").unwrap();

    let datacenter = matches.value_of("pop").unwrap();
    let server = matches.value_of("hostname").unwrap();

    if datacenter.is_empty() || server.is_empty() {
        // if these are empty because we failed to read the defaults, explain this
        if let Some(e) = defaults_err {
            error!(
                "failed to get default host info (consider using --pop/--hostname): {}",
                e
            );
            process::exit(1);
        }
    }

    let queue_size = matches.value_of("queue-size").unwrap();

    let queue_size: u32 = match queue_size.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse queue-size: {}", e);
            process::exit(1);
        }
    };

    let nsq_cert = matches.value_of("nsq-cert").unwrap_or("");
    let nsq_key = matches.value_of("nsq-key").unwrap_or("");
    let nsq_ca = matches.value_of("nsq-ca").unwrap_or("");
    let verify = !matches.is_present("no-verify-peer");

    let args = Args {
        spec: spec.to_string(),
        endpoint: endpoint.to_string(),
        datacenter: datacenter.to_string(),
        server: server.to_string(),
        queue_size,
        nsq_cert: nsq_cert.to_string(),
        nsq_key: nsq_key.to_string(),
        nsq_ca: nsq_ca.to_string(),
        verify,
    };

    if let Err(e) = process_args_and_run(args) {
        eprintln!("Error: {}", e);
        process::exit(1);
    }
}
