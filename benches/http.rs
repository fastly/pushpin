// ============================================================================
// http1 benchmark
// ============================================================================
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
const REQUEST_PATH: &str = "/api/test";
const REQUEST_HOST: &[u8] = b"example.com";

// Small payload (~50 bytes)
const SMALL_BODY: &[u8] = br#"{"ok":true,"id":1,"ts":"2000-01-01T00:00:00Z"}"#;

// Medium payload (~600 bytes)
const MEDIUM_BODY: &[u8] = br#"{"data":{"id":1,"msg":"test","ts":"2000-01-01T00:00:00Z","meta":{"a":"x","b":["y","z"],"c":"abc-123"},"content":{"t":"test","b":"test test test test test test test test test test test test test test test test test test","links":[{"r":"self","h":"/a/1"},{"r":"x","h":"/b/2"}],"f":{"x":false,"y":false,"z":true}},"d":{"ch":["a","b","c"],"ts":"2000-01-01T00:00:01Z","n":0},"v":"1"}}"#;

// Large payload (~6KB)
const LARGE_BODY: &[u8] = br#"{"items":[{"id":1,"u":10001,"m":"test","ts":"2000-01-01T00:00:00Z","meta":{"p":"a","t":["x"],"s":"s-001"},"c":{"t":"test","b":"test test test test test"}},{"id":2,"u":10002,"m":"test","ts":"2000-01-01T00:00:01Z","meta":{"p":"b","t":["y"],"s":"s-002"},"c":{"t":"test","b":"test test test test test"}},{"id":3,"u":10003,"m":"test","ts":"2000-01-01T00:00:02Z","meta":{"p":"c","t":["z"],"s":"s-003"},"c":{"t":"test","b":"test test test test test"}},{"id":4,"u":10004,"m":"test","ts":"2000-01-01T00:00:03Z","meta":{"p":"a","t":["x"],"s":"s-004"},"c":{"t":"test","b":"test test test test test"}},{"id":5,"u":10005,"m":"test","ts":"2000-01-01T00:00:04Z","meta":{"p":"b","t":["y"],"s":"s-005"},"c":{"t":"test","b":"test test test test test"}},{"id":6,"u":10006,"m":"test","ts":"2000-01-01T00:00:05Z","meta":{"p":"a","t":["z"],"s":"s-006"},"c":{"t":"test","b":"test test test test test"}},{"id":7,"u":10007,"m":"test","ts":"2000-01-01T00:00:06Z","meta":{"p":"c","t":["x"],"s":"s-007"},"c":{"t":"test","b":"test test test test test"}},{"id":8,"u":10008,"m":"test","ts":"2000-01-01T00:00:07Z","meta":{"p":"b","t":["y"],"s":"s-008"},"c":{"t":"test","b":"test test test test test"}},{"id":9,"u":10009,"m":"test","ts":"2000-01-01T00:00:08Z","meta":{"p":"a","t":["x"],"s":"s-009"},"c":{"t":"test","b":"test test test test test"}},{"id":10,"u":10010,"m":"test","ts":"2000-01-01T00:00:09Z","meta":{"p":"b","t":["y"],"s":"s-010"},"c":{"t":"test","b":"test test test test test"}},{"id":11,"u":10011,"m":"test","ts":"2000-01-01T00:00:10Z","meta":{"p":"c","t":["z"],"s":"s-011"},"c":{"t":"test","b":"test test test test test"}},{"id":12,"u":10012,"m":"test","ts":"2000-01-01T00:00:11Z","meta":{"p":"a","t":["x"],"s":"s-012"},"c":{"t":"test","b":"test test test test test"}},{"id":13,"u":10013,"m":"test","ts":"2000-01-01T00:00:12Z","meta":{"p":"b","t":["y"],"s":"s-013"},"c":{"t":"test","b":"test test test test test"}},{"id":14,"u":10014,"m":"test","ts":"2000-01-01T00:00:13Z","meta":{"p":"a","t":["z"],"s":"s-014"},"c":{"t":"test","b":"test test test test test"}},{"id":15,"u":10015,"m":"test","ts":"2000-01-01T00:00:14Z","meta":{"p":"c","t":["x"],"s":"s-015"},"c":{"t":"test","b":"test test test test test"}},{"id":16,"u":10016,"m":"test","ts":"2000-01-01T00:00:15Z","meta":{"p":"b","t":["y"],"s":"s-016"},"c":{"t":"test","b":"test test test test test"}},{"id":17,"u":10017,"m":"test","ts":"2000-01-01T00:00:16Z","meta":{"p":"a","t":["x"],"s":"s-017"},"c":{"t":"test","b":"test test test test test"}},{"id":18,"u":10018,"m":"test","ts":"2000-01-01T00:00:17Z","meta":{"p":"b","t":["y"],"s":"s-018"},"c":{"t":"test","b":"test test test test test"}},{"id":19,"u":10019,"m":"test","ts":"2000-01-01T00:00:18Z","meta":{"p":"c","t":["z"],"s":"s-019"},"c":{"t":"test","b":"test test test test test"}},{"id":20,"u":10020,"m":"test","ts":"2000-01-01T00:00:19Z","meta":{"p":"a","t":["x"],"s":"s-020"},"c":{"t":"test","b":"test test test test test"}}],"n":20}"#;

// Response config
const RESPONSE_CODE: u16 = 200;
const RESPONSE_REASON: &str = "OK";
const RESPONSE_BODY: &[u8] = b"OK";

fn run_http1_roundtrip(
    listener: TcpListener,
    addr: SocketAddr,
    req_body_data: &'static [u8],
) -> TcpListener {
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

            // Read request body into buffer
            let req_body = req_body.discard_header(req_info);
            let mut body_buf = Vec::with_capacity(req_body_data.len());
            let mut chunk = [0u8; BODY_READ_BUF_SIZE];
            loop {
                match req_body.try_recv(&mut chunk).unwrap() {
                    RecvStatus::NeedBytes(()) => req_body.add_to_buffer().await.unwrap(),
                    RecvStatus::Read((), size) => body_buf.extend_from_slice(&chunk[..size]),
                    RecvStatus::Complete((), size) => {
                        body_buf.extend_from_slice(&chunk[..size]);
                        break;
                    }
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
            let content_length = req_body_data.len().to_string();
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
                    BodySize::Known(req_body_data.len()),
                    false,
                    req_body_data,
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

            // Read response body into buffer (matching production ContiguousBuffer usage)
            let resp_body = resp_body_keep_header.discard_header(resp_info).unwrap();
            let mut body_buf = Vec::with_capacity(RESPONSE_BODY.len());
            let mut chunk = [0u8; BODY_READ_BUF_SIZE];
            loop {
                match resp_body.try_recv(&mut chunk).unwrap() {
                    RecvStatus::NeedBytes(_) => resp_body.add_to_buffer().await.unwrap(),
                    RecvStatus::Read(_, size) => body_buf.extend_from_slice(&chunk[..size]),
                    RecvStatus::Complete(_, size) => {
                        body_buf.extend_from_slice(&chunk[..size]);
                        break;
                    }
                }
            }
            assert_eq!(&body_buf[..], RESPONSE_BODY);
        })
        .unwrap();

    executor
        .run(|timeout| Reactor::current().unwrap().poll(timeout))
        .unwrap();

    recv.recv().unwrap()
}

fn core_http1_small(c: &mut Criterion) {
    let mut listener = Some(TcpListener::bind("127.0.0.1:0".parse().unwrap()).unwrap());
    let addr = listener.as_ref().unwrap().local_addr().unwrap();

    c.bench_function("core_http1_small", |b| {
        b.iter(|| {
            listener = Some(run_http1_roundtrip(
                listener.take().unwrap(),
                addr,
                SMALL_BODY,
            ));
        });
    });
}

fn core_http1_medium(c: &mut Criterion) {
    let mut listener = Some(TcpListener::bind("127.0.0.1:0".parse().unwrap()).unwrap());
    let addr = listener.as_ref().unwrap().local_addr().unwrap();

    c.bench_function("core_http1_medium", |b| {
        b.iter(|| {
            listener = Some(run_http1_roundtrip(
                listener.take().unwrap(),
                addr,
                MEDIUM_BODY,
            ));
        });
    });
}

fn core_http1_large(c: &mut Criterion) {
    let mut listener = Some(TcpListener::bind("127.0.0.1:0".parse().unwrap()).unwrap());
    let addr = listener.as_ref().unwrap().local_addr().unwrap();

    c.bench_function("core_http1_large", |b| {
        b.iter(|| {
            listener = Some(run_http1_roundtrip(
                listener.take().unwrap(),
                addr,
                LARGE_BODY,
            ));
        });
    });
}

// ============================================================================
// Hyper benchmark
// ============================================================================

use http_body_util::{BodyExt, Full};
use hyper::body::Bytes;
use hyper::server::conn::http1 as hyper_http1;
use hyper::service::service_fn;
use hyper_util::rt::TokioIo;
use tokio::net::{TcpListener as TokioTcpListener, TcpStream as TokioTcpStream};

async fn hyper_server_handler(
    req: hyper::Request<hyper::body::Incoming>,
) -> Result<hyper::Response<Full<Bytes>>, std::convert::Infallible> {
    // Read request body into memory (matching http1's ContiguousBuffer usage)
    let _body = req.into_body().collect().await.unwrap().to_bytes();

    Ok(hyper::Response::builder()
        .status(RESPONSE_CODE)
        .body(Full::new(Bytes::from_static(RESPONSE_BODY)))
        .unwrap())
}

async fn run_hyper_roundtrip(
    listener: TokioTcpListener,
    req_body_data: &'static [u8],
) -> TokioTcpListener {
    let addr = listener.local_addr().unwrap();

    // Start server
    let server_handle = tokio::spawn(async move {
        let (server_stream, _) = listener.accept().await.unwrap();
        let server_io = TokioIo::new(server_stream);
        hyper_http1::Builder::new()
            .serve_connection(server_io, service_fn(hyper_server_handler))
            .await
            .unwrap();
        listener
    });

    // Start client
    let stream = TokioTcpStream::connect(addr).await.unwrap();
    let io = TokioIo::new(stream);

    let (mut sender, conn) = hyper::client::conn::http1::handshake(io).await.unwrap();

    // Connection driver
    tokio::spawn(async move {
        if let Err(e) = conn.await {
            if !e.is_closed() {
                panic!("Connection error: {:?}", e);
            }
        }
    });

    let req = hyper::Request::builder()
        .method(REQUEST_METHOD)
        .uri(REQUEST_PATH)
        .header("Host", REQUEST_HOST)
        .header("Content-Type", "application/json")
        .header("Connection", "close")
        .body(Full::new(Bytes::from_static(req_body_data)))
        .unwrap();

    let resp = sender.send_request(req).await.unwrap();
    assert_eq!(resp.status(), RESPONSE_CODE);

    // Read response body into memory (matching http1's ContiguousBuffer usage)
    let body = resp.into_body().collect().await.unwrap().to_bytes();
    assert_eq!(&body[..], RESPONSE_BODY);

    server_handle.await.unwrap()
}

fn hyper_http1_small(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut listener =
        Some(rt.block_on(async { TokioTcpListener::bind("127.0.0.1:0").await.unwrap() }));

    c.bench_function("hyper_http1_small", |b| {
        b.iter(|| {
            listener = Some(rt.block_on(run_hyper_roundtrip(listener.take().unwrap(), SMALL_BODY)));
        });
    });
}

fn hyper_http1_medium(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut listener =
        Some(rt.block_on(async { TokioTcpListener::bind("127.0.0.1:0").await.unwrap() }));

    c.bench_function("hyper_http1_medium", |b| {
        b.iter(|| {
            listener =
                Some(rt.block_on(run_hyper_roundtrip(listener.take().unwrap(), MEDIUM_BODY)));
        });
    });
}

fn hyper_http1_large(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut listener =
        Some(rt.block_on(async { TokioTcpListener::bind("127.0.0.1:0").await.unwrap() }));

    c.bench_function("hyper_http1_large", |b| {
        b.iter(|| {
            listener = Some(rt.block_on(run_hyper_roundtrip(listener.take().unwrap(), LARGE_BODY)));
        });
    });
}

criterion_group!(
    benches,
    core_http1_small,
    core_http1_medium,
    core_http1_large,
    hyper_http1_small,
    hyper_http1_medium,
    hyper_http1_large
);
criterion_main!(benches);
