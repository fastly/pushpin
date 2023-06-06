use clap::{Arg, ArgAction, Command};
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

    let matches = Command::new(PROGRAM_NAME)
        .version(env!("APP_VERSION"))
        .about("Read stats from Pushpin and emit to NSQ")
        .arg(
            Arg::new("spec")
                .required(true)
                .num_args(1)
                .value_name("stats-spec")
                .help("ZeroMQ SUB spec to read from"),
        )
        .arg(
            Arg::new("endpoint")
                .required(true)
                .num_args(1)
                .value_name("nsq-endpoint")
                .help("NSQ endpoint to send to"),
        )
        .arg(
            Arg::new("log-level")
                .long("log-level")
                .num_args(1)
                .value_name("N")
                .help("Log level")
                .default_value("2"),
        )
        .arg(
            Arg::new("pop")
                .long("pop")
                .num_args(1)
                .value_name("name")
                .help("The POP to send metrics for")
                .default_value(&defaults.pop),
        )
        .arg(
            Arg::new("hostname")
                .long("hostname")
                .num_args(1)
                .value_name("name")
                .help("The server name to send metrics for")
                .default_value(&defaults.hostname),
        )
        .arg(
            Arg::new("queue-size")
                .long("queue-size")
                .num_args(1)
                .value_name("N")
                .help("Output queue size")
                .default_value("120"), // same as xqd default
        )
        .arg(
            Arg::new("nsq-cert")
                .long("nsq-cert")
                .num_args(1)
                .value_name("file")
                .help("NSQ client cert"),
        )
        .arg(
            Arg::new("nsq-key")
                .long("nsq-key")
                .num_args(1)
                .value_name("file")
                .help("NSQ client key"),
        )
        .arg(
            Arg::new("nsq-ca")
                .long("nsq-ca")
                .num_args(1)
                .value_name("file")
                .help("NSQ client CA"),
        )
        .arg(
            Arg::new("no-verify-peer")
                .long("no-verify-peer")
                .action(ArgAction::SetTrue)
                .help("Disable peer cert verification"),
        )
        .get_matches();

    let level = matches.get_one::<String>("log-level").unwrap();

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

    let spec = matches.get_one::<String>("spec").unwrap().clone();
    let endpoint = matches.get_one::<String>("endpoint").unwrap().clone();

    let datacenter = matches.get_one::<String>("pop").unwrap().clone();
    let server = matches.get_one::<String>("hostname").unwrap().clone();

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

    let queue_size = matches.get_one::<String>("queue-size").unwrap();

    let queue_size: u32 = match queue_size.parse() {
        Ok(x) => x,
        Err(e) => {
            error!("failed to parse queue-size: {}", e);
            process::exit(1);
        }
    };

    let nsq_cert = matches
        .get_one::<String>("nsq-cert")
        .cloned()
        .unwrap_or_default();
    let nsq_key = matches
        .get_one::<String>("nsq-key")
        .cloned()
        .unwrap_or_default();
    let nsq_ca = matches
        .get_one::<String>("nsq-ca")
        .cloned()
        .unwrap_or_default();
    let verify = !*matches.get_one::<bool>("no-verify-peer").unwrap();

    let args = Args {
        spec,
        endpoint,
        datacenter,
        server,
        queue_size,
        nsq_cert,
        nsq_key,
        nsq_ca,
        verify,
    };

    if let Err(e) = process_args_and_run(args) {
        eprintln!("Error: {}", e);
        process::exit(1);
    }
}
