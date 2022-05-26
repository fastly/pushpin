//! # Understanding the Tests
//!
//! These tests for `xqd-stats-emitter` use the `wiremock` library in order to
//! simulate us making connections with the http endpoint that we send metrics
//! and stats to from `xqd`. The important part to know here is that while
//! `wiremock` will randomize which port it can use we need to set individual
//! ports so that we don't have tests that sometimes fail. The reason is some of
//! these tests simulate not being able to connect to the service and when a
//! `MessageAggregator` is spawned it can only use one URL. If we bring down the
//! `wiremock` server and spin it up again it might be on a different port and
//! the test can fail, or it spins up on a port for another test. It's a problem
//! laden with race conditions due to Rust running tests in parallel. The
//! solution is to just make the server for each test use one port. If you need
//! to add a test the next available port is:
//!
//! 3012
//!
//! Make sure to bump this number if you add a test so the next person who comes
//! here knows what port to use!

use crate::stats_emitter::heavenly::uuid::ServiceID;
use crate::stats_emitter::xqd_config::MutualTlsConfig;
use crate::stats_emitter::{
    data_types::{ChannelMessage, DataCenter, Emitter, Message, SchemaName, Server, Service},
    message_aggregator::MessageAggregator,
    options::{RawAggregatorConfig, RawMessageSenderMode},
};
use hyper::client::HttpConnector;
use maplit::hashmap;
use std::{
    borrow::Cow,
    collections::{HashMap, HashSet},
    fs::{self, File},
    net::TcpListener,
    process::{Command, Stdio},
    sync::{Arc, Mutex},
};
use tempdir::TempDir;
use tokio::time::{sleep, Duration, Sleep};
use wiremock::{
    http::Method,
    matchers::{method, path},
    Mock, MockServer, Request, ResponseTemplate,
};
type Metrics = HashMap<ServiceID, HashMap<&'static str, u64>>;

#[tokio::test]
async fn can_send() {
    let demo = ServiceID::from_static("demo");
    let uuid = ServiceID::from_static("0");
    let mock_server = ok_server(3000).await;
    let opts = opts(mock_server.uri());
    let tx = MessageAggregator::spawn(opts, HttpConnector::new()).unwrap();
    tx.try_send(ChannelMessage::new(uuid, "test-metric", 5).unwrap())
        .unwrap();

    let reqs = wait_on_message(&mock_server).await;
    assert_req(
        &reqs[0],
        hashmap! {
            uuid => hashmap! {
                "test-metric" => 5
            },
            demo => hashmap! {
                "test-metric" => 5
            }
        },
        1,
    );
}

#[tokio::test]
async fn can_send_with_many_metrics() {
    let demo = ServiceID::from_static("demo");
    let uuid = ServiceID::from_static("0");
    let mock_server = ok_server(3005).await;
    let opts = opts(mock_server.uri());
    let tx = MessageAggregator::spawn(opts, HttpConnector::new()).unwrap();
    tx.try_send(ChannelMessage::new(uuid, "test-metric-0", 1).unwrap())
        .unwrap();
    tx.try_send(ChannelMessage::new(uuid, "test-metric-1", 1).unwrap())
        .unwrap();

    let reqs = wait_on_message(&mock_server).await;
    assert_req(
        &reqs[0],
        hashmap! {
            uuid => hashmap! {
                "test-metric-0" => 1,
                "test-metric-1" => 1
            },
            demo => hashmap! {
                "test-metric-0" => 1,
                "test-metric-1" => 1
            }
        },
        2,
    );
}

#[tokio::test]
async fn can_send_with_many_metrics_and_uuids() {
    let demo = ServiceID::from_static("demo");
    let uuid_0 = ServiceID::from_static("0");
    let uuid_1 = ServiceID::from_static("1");
    let mock_server = ok_server(3006).await;
    let opts = opts(mock_server.uri());
    let tx = MessageAggregator::spawn(opts, HttpConnector::new()).unwrap();

    tx.try_send(ChannelMessage::new(uuid_0, "test-metric-0", 1).unwrap())
        .unwrap();
    tx.try_send(ChannelMessage::new(uuid_0, "test-metric-1", 1).unwrap())
        .unwrap();
    tx.try_send(ChannelMessage::new(uuid_1, "test-metric-0", 1).unwrap())
        .unwrap();
    tx.try_send(ChannelMessage::new(uuid_1, "test-metric-1", 1).unwrap())
        .unwrap();

    let reqs = wait_on_message(&mock_server).await;

    assert_req(
        &reqs[0],
        hashmap! {
            uuid_0 => hashmap! {
                "test-metric-0" => 1,
                "test-metric-1" => 1
            },
            uuid_1 => hashmap! {
                "test-metric-0" => 1,
                "test-metric-1" => 1
            },
            demo => hashmap! {
                "test-metric-0" => 2,
                "test-metric-1" => 2
            }
        },
        2,
    );
}

#[tokio::test]
async fn can_send_2_in_less_than_a_second() {
    let demo = ServiceID::from_static("demo");
    let uuid_0 = ServiceID::from_static("0");
    let uuid_1 = ServiceID::from_static("1");
    let mock_server = ok_server(3001).await;
    let opts = opts(mock_server.uri());
    let tx = MessageAggregator::spawn(opts, HttpConnector::new()).unwrap();

    tx.try_send(ChannelMessage::new(uuid_0, "test-metric", 5).unwrap())
        .unwrap();
    tx.try_send(ChannelMessage::new(uuid_1, "testing", 6).unwrap())
        .unwrap();

    let reqs = wait_on_message(&mock_server).await;
    assert_req(
        &reqs[0],
        hashmap! {
            uuid_0 => hashmap! {
                "test-metric" => 5
            },
            uuid_1 => hashmap! {
                "testing" => 6
            },
            demo => hashmap! {
                "test-metric" => 5,
                "testing" => 6
            }
        },
        2,
    );
}

#[tokio::test]
async fn can_send_2_wait_more_than_a_second() {
    let demo = ServiceID::from_static("demo");
    let uuid_0 = ServiceID::from_static("0");
    let uuid_1 = ServiceID::from_static("1");
    let mock_server = ok_server(3002).await;
    let opts = opts(mock_server.uri());
    let tx = MessageAggregator::spawn(opts, HttpConnector::new()).unwrap();

    tx.try_send(ChannelMessage::new(uuid_0, "test-metric", 5).unwrap())
        .unwrap();
    sleep_secs(2).await;
    tx.try_send(ChannelMessage::new(uuid_1, "testing", 6).unwrap())
        .unwrap();
    sleep_secs(2).await;

    let reqs = wait_on_message(&mock_server).await;
    assert_eq!(reqs.len(), 2);
    assert_req(
        &reqs[0],
        hashmap! {
            uuid_0 => hashmap! {
                "test-metric" => 5
            },
            demo => hashmap! {
                "test-metric" => 5,
            }
        },
        1,
    );
    assert_req(
        &reqs[1],
        hashmap! {
            uuid_1 => hashmap! {
                "testing" => 6
            },
            demo => hashmap! {
                "testing" => 6,
            }
        },
        1,
    );
}

#[tokio::test]
async fn can_handle_non_200_server() {
    let demo = ServiceID::from_static("demo");
    let uuid_0 = ServiceID::from_static("0");
    let mut mock_server = bad_server(3003).await;
    let opts = opts(mock_server.uri());
    let tx = MessageAggregator::spawn(opts, HttpConnector::new()).unwrap();

    tx.try_send(ChannelMessage::new(uuid_0, "test-metric", 5).unwrap())
        .unwrap();
    sleep_secs(3).await;

    let reqs = wait_on_message(&mock_server).await;

    // Make sure that it has tried to send a message more than once and that
    // the message sent is exactly the same, which means the retry logic is working.
    assert!(reqs.len() >= 2);
    assert_eq!(&reqs[0].body, &reqs[1].body);

    make_good(&mut mock_server).await;

    // Wait till we get a message and respond with 200
    wait_on_message(&mock_server).await;

    // Wait a bit longer to make sure no other message is sent
    sleep_secs(3).await;

    // Make sure we only get the one message since we respond with a 200
    // response now.
    let reqs = get_reqs(&mock_server).await;
    assert!(reqs.len() == 1);

    assert_req(
        &reqs[0],
        hashmap! {
            uuid_0 => hashmap! {
                "test-metric" => 5
            },
            demo => hashmap! {
                "test-metric" => 5,
            }
        },
        1,
    );
}

#[tokio::test]
async fn can_handle_no_connection_to_server() {
    let demo = ServiceID::from_static("demo");
    let uuid_0 = ServiceID::from_static("0");
    let opts = opts("http://127.0.0.1:3004");
    let tx = MessageAggregator::spawn(opts, HttpConnector::new()).unwrap();

    tx.try_send(ChannelMessage::new(uuid_0, "test-metric", 5).unwrap())
        .unwrap();

    // Give it time to actually let it send the message a few times even
    // though the server is not up
    sleep_secs(3).await;

    // Start the server up so we can receive the message
    let mock_server = ok_server(3004).await;
    wait_on_message(&mock_server).await;

    // Wait a bit longer to make sure no other message is sent
    sleep_secs(3).await;

    // Make sure we only get the one message since we respond with a 200
    // response now.
    let reqs = get_reqs(&mock_server).await;
    assert!(reqs.len() == 1);
    assert_req(
        &reqs[0],
        hashmap! {
            uuid_0 => hashmap! {
                "test-metric" => 5
            },
            demo => hashmap! {
                "test-metric" => 5,
            }
        },
        1,
    );
}

#[tokio::test]
async fn can_handle_losing_connection_to_server_and_non_200() {
    let demo = ServiceID::from_static("demo");
    let uuid_0 = ServiceID::from_static("0");
    let opts = opts("http://127.0.0.1:3007");
    let tx = MessageAggregator::spawn(opts, HttpConnector::new()).unwrap();

    // Handle server working
    {
        let mock_server = ok_server(3007).await;
        tx.try_send(ChannelMessage::new(uuid_0, "test-metric", 5).unwrap())
            .unwrap();
        let reqs = wait_on_message(&mock_server).await;
        assert_req(
            &reqs[0],
            hashmap! {
                uuid_0 => hashmap! {
                    "test-metric" => 5
                },
                demo => hashmap! {
                    "test-metric" => 5,
                }
            },
            1,
        );

        std::mem::drop(mock_server);
    }

    tx.try_send(ChannelMessage::new(uuid_0, "test-metric", 6).unwrap())
        .unwrap();

    // Give it time to actually let it send the message a few times even
    // though the server is not up and that it will not crash the process
    sleep_secs(2).await;

    // Start the server up again so we can receive the message a few times
    let mut mock_server = bad_server(3007).await;
    wait_on_n_messages(&mock_server, 3).await;
    make_good(&mut mock_server).await;
    let reqs = wait_on_message(&mock_server).await;
    assert_eq!(reqs.len(), 1);

    // Wait a bit longer to make sure other messages are not sent
    sleep_secs(6).await;

    // Make sure we didn't get any more messages sent
    let reqs = get_reqs(&mock_server).await;
    assert_eq!(reqs.len(), 1);
    assert_req(
        &reqs[0],
        hashmap! {
            uuid_0 => hashmap! {
                "test-metric" => 6
            },
            demo => hashmap! {
                "test-metric" => 6
            }
        },
        1,
    );
}

#[tokio::test]
async fn can_handle_going_from_200_to_non_200_and_back() {
    let demo = ServiceID::from_static("demo");
    let uuid_0 = ServiceID::from_static("0");
    let opts = opts("http://127.0.0.1:3008");
    let tx = MessageAggregator::spawn(opts, HttpConnector::new()).unwrap();

    let mut mock_server = ok_server(3008).await;
    tx.try_send(ChannelMessage::new(uuid_0, "test-metric", 5).unwrap())
        .unwrap();
    let reqs = wait_on_message(&mock_server).await;
    assert_req(
        &reqs[0],
        hashmap! {
            uuid_0 => hashmap! {
                "test-metric" => 5
            },
            demo => hashmap! {
                "test-metric" => 5
            }
        },
        1,
    );

    make_bad(&mut mock_server).await;
    tx.try_send(ChannelMessage::new(uuid_0, "test-metric", 6).unwrap())
        .unwrap();
    wait_on_n_messages(&mock_server, 3).await;

    make_good(&mut mock_server).await;
    let reqs = wait_on_message(&mock_server).await;
    assert_eq!(reqs.len(), 1);
    assert_req(
        &reqs[0],
        hashmap! {
            uuid_0 => hashmap! {
                "test-metric" => 6
            },
            demo => hashmap! {
                "test-metric" => 6
            }
        },
        1,
    );
}

#[tokio::test]
/// This is a long running test as we must fill up the buffer with 120 messages
/// and one is sent every second. However, timing is hard and so we sleep 2
/// seconds for each message we send to make sure we do actually have a reliable
/// test. We want to make sure that we can clear the buffer after we can connect
/// and so this tests a worst case scenario where we have to drop messages.
async fn will_drop_messages() {
    let demo = ServiceID::from_static("demo");
    let uuid = ServiceID::from_static("0");
    let opts = opts("http://127.0.0.1:3009");
    let tx = MessageAggregator::spawn(opts, HttpConnector::new()).unwrap();
    for _ in 0..11 {
        tx.try_send(ChannelMessage::new(uuid, "test-metric", 5).unwrap())
            .unwrap();
        sleep_secs(2).await;
    }
    tx.try_send(ChannelMessage::new(uuid, "test-drop", 5).unwrap())
        .unwrap();
    sleep_secs(2).await;
    let mock_server = ok_server(3009).await;
    let reqs = wait_on_n_messages(&mock_server, 11).await;
    // Make sure we don't have anything else
    sleep_secs(2).await;

    // Our buffer is 10 messages for this test and 120 for prod. The first
    // message is pulled out of the queue and held onto if it fails to send. So
    // when everything is all said and done we send 11 messages actually that we
    // need to test.
    assert_eq!(reqs.len(), 11);
    for req in reqs {
        assert_req(
            &req,
            hashmap! {
                uuid => hashmap! {
                    "test-metric" => 5
                },
                demo => hashmap! {
                    "test-metric" => 5
                }
            },
            1,
        );
    }
}

const STATS_TOOL: &str = "/opt/fst-stats/bin/fst-stats-tool";

#[test]
fn output_is_compliant() {
    let mut msg = Message::new(
        Arc::new(SchemaName::new("billing")),
        Arc::new(DataCenter::new("data")),
        Arc::new(Server::new("server")),
        Arc::new(Emitter::new("time")),
    );
    let test_0: Cow<'static, str> = "test-metric-0".into();
    let test_1: Cow<'static, str> = "test-metric-1".into();
    let test_2: Cow<'static, str> = "test-metric-2".into();
    let test_3: Cow<'static, str> = "test-metric-3".into();
    let test_4: Cow<'static, str> = "test-metric-4".into();
    msg.schema = {
        let mut set = HashSet::new();
        set.insert(test_0.to_owned());
        set.insert(test_1.to_owned());
        set.insert(test_2.to_owned());
        set.insert(test_3.to_owned());
        set.insert(test_4.to_owned());
        set
    };
    let uuid_0 = ServiceID::from_static("0abcdefghijklmnopqrstu");
    let uuid_1 = ServiceID::from_static("1abcdefghijklmnopqrstu");
    let uuid_2 = ServiceID::from_static("2abcdefghijklmnopqrstu");
    let uuid_3 = ServiceID::from_static("3abcdefghijklmnopqrstu");
    let uuid_4 = ServiceID::from_static("4abcdefghijklmnopqrstu");
    let demo = ServiceID::from_static("demo");
    msg.services = hashmap! {
      uuid_0 => Service {
        id: uuid_0,
        counters: hashmap!{
          test_0.to_owned() => 5,
          test_4.to_owned() => 4,
        },
      },
      uuid_1 => Service {
        id: uuid_1,
        counters: hashmap!{
          test_1.to_owned() => 1,
        },
      },
      uuid_2 => Service {
        id: uuid_2,
        counters: hashmap!{
          test_0.to_owned() => 5,
          test_2.to_owned() => 2,
          test_3.to_owned() => 3,
          test_1.to_owned() => 1,
          test_4.to_owned() => 4,
        },
      },
      uuid_3 => Service {
        id: uuid_3,
        counters: hashmap!{
          test_2.to_owned() => 2,
          test_1.to_owned() => 1,
        },
      },
      uuid_4 => Service {
        id: uuid_4,
        counters: hashmap!{
          test_1.to_owned() => 1,
          test_3.to_owned() => 3,
        },
      },
      demo => Service {
        id: demo,
        counters: hashmap!{
          test_0.to_owned() => 10,
          test_1.to_owned() => 4,
          test_2.to_owned() => 4,
          test_3.to_owned() => 6,
          test_4.to_owned() => 8,
        },
      },
    };

    let dir = TempDir::new("stats-compliance-test").unwrap();
    let c_path = dir.path().join("compliance");
    fs::write(&c_path, serde_json::to_string(&msg).unwrap()).unwrap();
    let command = Command::new(STATS_TOOL)
        .arg("check-json")
        .arg(c_path)
        .output()
        .unwrap();

    if !command.status.success() {
        panic!("{}", String::from_utf8(command.stderr).unwrap());
    }
}

#[tokio::test]
/// The cert test is used to test that we can actually connect to the https
/// server and do peer validation with the https endpoint. If it's successful
/// then it will dump a varnish message to disk and if not we will not see that
/// file. We check to see if the file is there and pass the test if it is. We
/// know from previous tests whether a connection can actually send anything. If
/// this test alone fails then it's a cert issue or something related, but if
/// this fails in conjunction with other tests such as `can_send` then it's not
/// just a cert issue as those tests use http to test the underlying messages
/// and mechanisms by which they are sent.
async fn cert_test() {
    // Define all the paths that we need in a spawned temporary directory and
    // the address we'll use
    const ADDR: &str = "127.0.0.1:3010";
    let dir = TempDir::new("cert-connection-test").unwrap();
    let dir_path = dir.path();
    let server_out = dir_path.join("server-out");
    let server_err = dir_path.join("server-err");
    let varnish = dir_path.join("varnish");
    let certs = dir_path;

    // Spawn the fst-stats-tool server
    let command = Arc::new(Mutex::new(
        Command::new(STATS_TOOL)
            .arg("server")
            .arg("-addr")
            .arg(ADDR)
            .arg("-certdir")
            .arg(&certs)
            .arg("-varnish-log")
            .arg(&varnish)
            .stdin(Stdio::null())
            .stdout(File::create(server_out).unwrap())
            .stderr(File::create(server_err).unwrap())
            .spawn()
            .unwrap(),
    ));

    // Create a ping function we can use to poll the server until it's up
    let ping = || {
        Command::new(STATS_TOOL)
            .arg("server")
            .arg("-addr")
            .arg(ADDR)
            .arg("-ping=true")
            .arg("-certdir")
            .arg(&certs)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap()
    };

    // Get the options for the aggregator and set the TLS config so that we use
    // https with the correct certs
    let mut opts = opts(format!("https://{}", ADDR));
    match &mut opts.mode {
        RawMessageSenderMode::Json { mtls, .. } => {
            *mtls = Some(MutualTlsConfig {
                ca_path: certs.join("nsq-ca-cert.pem"),
                cert_path: certs.join("nsq-client-cert.pem"),
                key_path: certs.join("nsq-client-key.pem"),
                dangerous_no_peer_verification: false,
            });
        }
        _ => panic!("Message sending mode was not JSON"),
    }

    // Poll the server and block execution until it's ready to receive messages
    while !ping().wait().unwrap().success() {}

    // Spawn our aggregator and send a single message
    let uuid = ServiceID::from_static("0abcdefghijklmnopqrstu");
    let tx = MessageAggregator::spawn(opts, {
        let mut connector = HttpConnector::new();
        connector.enforce_http(false);
        connector
    })
    .unwrap();
    tx.try_send(ChannelMessage::new(uuid, "test-metric", 5).unwrap())
        .unwrap();

    // Wait till the message is sent
    sleep_secs(2).await;

    // Kill the server now that we're done
    command.lock().unwrap().kill().unwrap();

    // Assert that the message went through to the server and has logged it as a
    // varnish message on disk indicating successful peer verification.
    assert!(varnish.exists())
}

#[tokio::test]
async fn dump_file_test() {
    let demo = ServiceID::from_static("demo");
    let uuid = ServiceID::from_static("0");
    let mock_server = ok_server(3011).await;
    let mut opts = opts(mock_server.uri());
    let dir = TempDir::new("stats-dump-test").unwrap();
    let output_path = dir.path().join("output");
    opts.mode = RawMessageSenderMode::DumpFile {
        dump_file: output_path.clone(),
    };
    let tx = MessageAggregator::spawn(opts, HttpConnector::new()).unwrap();
    tx.send(ChannelMessage::new(uuid, "test-metric-1", 5).unwrap())
        .await
        .unwrap();
    tx.send(ChannelMessage::new(uuid, "test-metric-2", 5).unwrap())
        .await
        .unwrap();
    sleep_secs(2).await;
    tx.send(ChannelMessage::new(uuid, "test-metric-2", 5).unwrap())
        .await
        .unwrap();
    sleep_secs(2).await;
    assert!(&output_path.exists());
    let message_raw = fs::read_to_string(&output_path).unwrap();
    let mut message = None;

    // Merge all the messages
    for line in message_raw.lines() {
        let temp_message = serde_json::from_str::<Message>(&line).unwrap();
        match message {
            None => message = Some(temp_message),
            Some(ref mut message) => {
                message.schema = temp_message
                    .schema
                    .union(&message.schema)
                    .cloned()
                    .collect();
                for (sid, service) in temp_message.services {
                    for (metric, count) in service.counters {
                        message
                            .services
                            .entry(sid)
                            .and_modify(|service| {
                                service
                                    .counters
                                    .entry(metric.clone())
                                    .and_modify(|c| *c += count)
                                    .or_insert(count);
                            })
                            .or_insert(Service {
                                id: sid,
                                counters: {
                                    let mut map = HashMap::new();
                                    map.insert(metric, count);
                                    map
                                },
                            });
                    }
                }
            }
        }
    }

    let message = message.unwrap();
    assert_msg(&message);
    assert_metric(&message, uuid, "test-metric-1", 5);
    assert_metric(&message, uuid, "test-metric-2", 10);
    assert_metric(&message, demo, "test-metric-1", 5);
    assert_metric(&message, demo, "test-metric-2", 10);
}

/// Checks that a given request contains all the metrics we expect it to have
/// and that it is the right length. For example if we use a metric 'foo',
/// 'bar', and 'baz' and one `ServiceID` uses 'foo' and 'bar' and the other ID
/// uses 'bar' and 'baz' then the schema_len should be 3 as their only 3
/// uniquely named metrics.
///
/// This is the method you really want to use to test out that everything about
/// the `Request` is as we expect it to be as it also calls the below assertion
/// functions in order to test that it's formed in the way we expect it to be
/// and that it has the correct values. The test `output_is_compliant` also does
/// this but does so using the data team's tool to verify the output is what
/// they expect. `assert_req` here is more to make sure the internals of our
/// application work as expected.
fn assert_req(req: &Request, metrics: Metrics, schema_len: usize) {
    assert_eq!(req.method, Method::Post);
    let msg = serde_json::from_slice(&req.body).unwrap();
    assert_msg(&msg);
    for (uuid, metric_map) in metrics {
        for (metric, count) in metric_map {
            assert_metric(&msg, uuid, metric, count);
        }
    }
    assert_eq!(msg.schema.len(), schema_len);
}

/// Assert that the message contains the data we expect it to have based off the
/// options we've set. This convenience method lets us check this easily in each
/// test rather than doing it by hand each time.
fn assert_msg(msg: &Message) {
    assert_eq!(*msg.datacenter, DataCenter::new("xqd-testing-datacenter"));
    assert_eq!(*msg.server, Server::new("xqd-testing-server"));
    assert_eq!(*msg.emitter, Emitter::new("xqd-stats-emitter-test"));
    assert_eq!(*msg.schema_name, SchemaName::new("xqd-billing"));
}

/// Assert that a message contains the metric and count we expect for a given
/// `ServiceID`. This helper method makes the code simpler to read and should be
/// used over directly checking in a test.
fn assert_metric(msg: &Message, uuid: ServiceID, metric: &'static str, count: u64) {
    assert!(msg.schema.contains(metric));
    assert_eq!(
        msg.services.get(&uuid).and_then(|s| s.counters.get(metric)),
        Some(&count)
    );
}

/// Create the `AggregatorConfig` we'll use for every single test with the only
/// variation being which URL is in use given we want to point to different
/// ports.
fn opts(url: impl ToString) -> RawAggregatorConfig {
    RawAggregatorConfig {
        schema_name: "xqd-billing".into(),
        emitter: Some("xqd-stats-emitter-test".into()),
        datacenter: Some("xqd-testing-datacenter".into()),
        server: Some("xqd-testing-server".into()),
        queue_size: 10,
        mode: RawMessageSenderMode::Json {
            url: url.to_string(),
            mtls: None,
        },
    }
}

/// Create a `wiremock` server on a given port that will respond with a 200 code
/// when receiving a request
async fn ok_server(port: u16) -> MockServer {
    build_server(
        Mock::given(method("POST"))
            .and(path("/"))
            .respond_with(ResponseTemplate::new(200))
            .named("Good Response"),
        port,
    )
    .await
}

/// Create a `wiremock` server on a given port that will respond with a 500 code
/// when receiving a request
async fn bad_server(port: u16) -> MockServer {
    build_server(
        Mock::given(method("POST"))
            .and(path("/"))
            .respond_with(ResponseTemplate::new(500))
            .named("Bad Response"),
        port,
    )
    .await
}

/// Reset a `wiremock` server to clear out all the responses it has received and
/// change it so that it responds with 500 code now when receiving a request
async fn make_bad(mock_server: &mut MockServer) {
    mock_server.reset().await;
    Mock::given(method("POST"))
        .and(path("/"))
        .respond_with(ResponseTemplate::new(500))
        .named("Bad Response")
        .mount(&mock_server)
        .await;
}

/// Reset a `wiremock` server to clear out all the responses it has received and
/// change it so that it responds with 200 code now when receiving a request
async fn make_good(mock_server: &mut MockServer) {
    mock_server.reset().await;
    Mock::given(method("POST"))
        .and(path("/"))
        .respond_with(ResponseTemplate::new(200))
        .named("Good Response")
        .mount(&mock_server)
        .await;
    assert_eq!(mock_server.received_requests().await.unwrap().len(), 0);
}

/// Helper method to build a `wiremock` server. You want to use the above
/// `ok_server` or `bad_server` methods instead of this directly
async fn build_server(mock: Mock, port: u16) -> MockServer {
    let mock_server = MockServer::builder()
        .listener(TcpListener::bind(("127.0.0.1", port)).unwrap())
        .start()
        .await;
    mock.mount(&mock_server).await;
    mock_server
}

/// Create a sleep future to sleep a given amount of seconds from the instant
/// this function is called
fn sleep_secs(secs: u64) -> Sleep {
    sleep(Duration::from_secs(secs))
}

/// Get the requests that the server has received to look at and to test how
/// many it has gotten. This is a convenience method to avoid having to call the
/// internal code of this function every single time it's needed which is a lot.
async fn get_reqs(mock_server: &MockServer) -> Vec<Request> {
    mock_server.received_requests().await.unwrap()
}

/// Block the test so that it won't continue until a message is received since
/// it might be a bit of time till the `MessageAggregator` can send a request
async fn wait_on_message(mock_server: &MockServer) -> Vec<Request> {
    while let Some(reqs) = mock_server.received_requests().await {
        if reqs.len() > 0 {
            return reqs;
        }
    }

    unreachable!()
}

/// Block the test so that it won't continue until n messages are received since
/// it might be a bit of time till the `MessageAggregator` can send many
/// requests
async fn wait_on_n_messages(mock_server: &MockServer, n: usize) -> Vec<Request> {
    while let Some(reqs) = mock_server.received_requests().await {
        if reqs.len() >= n {
            return reqs;
        }
    }

    unreachable!()
}
