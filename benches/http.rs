use criterion::{criterion_group, criterion_main, Criterion};
use mio::net::TcpListener;
use pushpin::core::buffer::{TmpBuffer, VecRingBuffer};
use pushpin::core::channel;
use pushpin::core::executor::Executor;
use pushpin::core::http1::{BodySize, Header, ParseScratch, RecvStatus};
use pushpin::core::io::io_split;
use pushpin::core::net::{AsyncTcpListener, AsyncTcpStream};
use pushpin::core::reactor::Reactor;
use std::cell::RefCell;
use std::net::SocketAddr;
use std::rc::Rc;

// Buffer sizes
const TMP_BUF_SIZE: usize = 8192;
const RING_BUF_SIZE: usize = 8192;
const BODY_READ_BUF_SIZE: usize = 2048;

// Request config
const REQUEST_METHOD: &str = "POST";
const REQUEST_PATH: &str = "/api/notify";
const REQUEST_HOST: &[u8] = b"example.com";

// Medium-sized JSON payload (~1KB)
const MEDIUM_BODY: &[u8] = br#"{"type":"notification","data":{"user_id":12345,"message":"Hello World","timestamp":"2026-02-06T10:00:00Z","metadata":{"priority":"high","tags":["urgent","customer"],"session_id":"abc-123-def-456"},"content":{"title":"Important Update","body":"This is a test notification with medium sized content to simulate realistic API payloads","links":[{"rel":"self","href":"/api/v1/notifications/123"},{"rel":"user","href":"/api/v1/users/12345"}],"attachments":[],"flags":{"read":false,"archived":false,"starred":true}},"delivery":{"channels":["email","push","websocket"],"sent_at":"2026-02-06T10:00:01Z","retry_count":0},"version":"1.0"}}"#;

// Response config
const RESPONSE_CODE: u16 = 200;
const RESPONSE_REASON: &str = "OK";
const RESPONSE_BODY: &[u8] = b"OK";

fn run_http1_roundtrip(listener: TcpListener, addr: SocketAddr) -> TcpListener {
    let _reactor = Reactor::new(100);
    let executor = Executor::new(2);

    let (send, recv) = channel::channel::<TcpListener>(1);

    // Server task
    executor
        .spawn(async move {
            let send = channel::AsyncSender::new(send);
            let listener = AsyncTcpListener::new(listener);

            // Set up split stream and read/write buffers for server
            let (stream, _) = listener.accept().await.unwrap();
            let stream = AsyncTcpStream::new(stream);
            let stream = RefCell::new(stream);
            let (r, w) = io_split(&stream);

            let tmp = Rc::new(TmpBuffer::new(TMP_BUF_SIZE));
            let mut rbuf = VecRingBuffer::new(RING_BUF_SIZE, &tmp);
            let mut wbuf = VecRingBuffer::new(RING_BUF_SIZE, &tmp);

            // Parse incoming request
            let (req, mut resp) =
                pushpin::core::http1::server::Request::new((r, w), &mut rbuf, &mut wbuf);
            let req_header = req.recv_header(&mut resp);
            let mut scratch = ParseScratch::<32>::new();
            let (req_info, req_body) = req_header.recv(&mut scratch).await.unwrap();

            // Read request body
            let req_body = req_body.discard_header(req_info);
            let mut buf = [0u8; BODY_READ_BUF_SIZE];
            loop {
                match req_body.try_recv(&mut buf).unwrap() {
                    RecvStatus::NeedBytes(()) => req_body.add_to_buffer().await.unwrap(),
                    RecvStatus::Read((), _) => continue,
                    RecvStatus::Complete((), _) => break,
                }
            }

            // Send response
            let resp_content_length = RESPONSE_BODY.len().to_string();
            let headers = [Header {
                name: "Content-Length",
                value: resp_content_length.as_bytes(),
            }];
            let mut state = pushpin::core::http1::server::ResponseState::default();
            let (resp_header, resp_prepare_body) = resp
                .prepare_header(
                    RESPONSE_CODE,
                    RESPONSE_REASON,
                    &headers,
                    BodySize::Known(RESPONSE_BODY.len()),
                    &mut state,
                )
                .unwrap();
            let resp_sent = resp_header.send().await.unwrap();
            let resp_body = resp_sent.start_body(resp_prepare_body);
            resp_body.prepare(RESPONSE_BODY, true).unwrap();

            loop {
                match resp_body.send().await {
                    pushpin::core::http1::SendStatus::Complete(_) => break,
                    pushpin::core::http1::SendStatus::Partial((), _) => continue,
                    pushpin::core::http1::SendStatus::EarlyResponse(_) => {
                        panic!("Unexpected EarlyResponse")
                    }
                    pushpin::core::http1::SendStatus::Error(_, e) => panic!("Send error: {:?}", e),
                }
            }

            // Return listener via channel
            send.send(listener.into_inner()).await.unwrap();
        })
        .unwrap();

    // Client task
    executor
        .spawn(async move {
            // Set up split stream and read/write buffers for client
            let stream = AsyncTcpStream::connect(&[addr]).await.unwrap();
            let stream = RefCell::new(stream);
            let (r, w) = io_split(&stream);

            let tmp = Rc::new(TmpBuffer::new(TMP_BUF_SIZE));
            let mut rbuf = VecRingBuffer::new(RING_BUF_SIZE, &tmp);
            let mut wbuf = VecRingBuffer::new(RING_BUF_SIZE, &tmp);

            // Send request
            let content_length = MEDIUM_BODY.len().to_string();
            let headers = [
                Header {
                    name: "Host",
                    value: REQUEST_HOST,
                },
                Header {
                    name: "Content-Type",
                    value: b"application/json",
                },
                Header {
                    name: "Content-Length",
                    value: content_length.as_bytes(),
                },
            ];

            let req = pushpin::core::http1::client::Request::new((r, w), &mut rbuf, &mut wbuf);
            let req_header = req
                .prepare_header(
                    REQUEST_METHOD,
                    REQUEST_PATH,
                    &headers,
                    BodySize::Known(MEDIUM_BODY.len()),
                    false,
                    MEDIUM_BODY,
                    true,
                )
                .unwrap();
            let req_body = req_header.send().await.unwrap();

            // Send request body and get response
            let resp = loop {
                match req_body.send().await {
                    pushpin::core::http1::SendStatus::Complete(r) => break r,
                    pushpin::core::http1::SendStatus::Partial((), _) => continue,
                    pushpin::core::http1::SendStatus::EarlyResponse(_) => {
                        panic!("Unexpected EarlyResponse")
                    }
                    pushpin::core::http1::SendStatus::Error(_, e) => panic!("Send error: {:?}", e),
                }
            };

            let mut scratch = ParseScratch::<32>::new();
            let (resp_info, resp_body_keep_header) = resp.recv_header(&mut scratch).await.unwrap();
            assert_eq!(resp_info.get().code, 200);

            // Read response body
            let resp_body = resp_body_keep_header.discard_header(resp_info).unwrap();
            let mut buf = [0u8; BODY_READ_BUF_SIZE];
            loop {
                match resp_body.try_recv(&mut buf).unwrap() {
                    RecvStatus::NeedBytes(_) => resp_body.add_to_buffer().await.unwrap(),
                    RecvStatus::Read(_, _) => continue,
                    RecvStatus::Complete(_, _) => break,
                }
            }
        })
        .unwrap();

    executor
        .run(|timeout| Reactor::current().unwrap().poll(timeout))
        .unwrap();

    recv.recv().unwrap()
}

fn core_http1_medium(c: &mut Criterion) {
    let mut listener = Some(TcpListener::bind("127.0.0.1:0".parse().unwrap()).unwrap());
    let addr = listener.as_ref().unwrap().local_addr().unwrap();

    c.bench_function("core_http1_medium", |b| {
        b.iter(|| {
            listener = Some(run_http1_roundtrip(listener.take().unwrap(), addr));
        });
    });
}

criterion_group!(benches, core_http1_medium);
criterion_main!(benches);
