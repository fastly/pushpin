/*
 * Copyright (C) 2020 Fanout, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

use condure::connection::testutil::{
    BenchServerReqConnection, BenchServerReqHandler, BenchServerStreamConnection,
    BenchServerStreamHandler,
};
use condure::server::TestServer;
use criterion::{criterion_group, criterion_main, Criterion};
use std::io::{Read, Write};
use std::net::SocketAddr;
use std::str;

const REQS_PER_ITER: usize = 10;

fn req(addr: SocketAddr) {
    let mut clients = Vec::new();

    for _ in 0..REQS_PER_ITER {
        let mut client = std::net::TcpStream::connect(&addr).unwrap();
        client
            .write(b"GET /hello HTTP/1.0\r\nHost: example.com\r\n\r\n")
            .unwrap();

        clients.push(client);
    }

    for client in clients.iter_mut() {
        let mut buf = Vec::new();
        client.read_to_end(&mut buf).unwrap();

        assert_eq!(
            str::from_utf8(&buf).unwrap(),
            "HTTP/1.0 200 OK\r\nContent-Length: 6\r\n\r\nworld\n"
        );
    }
}

fn criterion_benchmark(c: &mut Criterion) {
    {
        let t = BenchServerReqHandler::new();

        c.bench_function("req_handler", |b| {
            b.iter_batched_ref(|| t.init(), |i| t.run(i), criterion::BatchSize::SmallInput)
        });
    }

    {
        let t = BenchServerStreamHandler::new();

        c.bench_function("stream_handler", |b| {
            b.iter_batched_ref(|| t.init(), |i| t.run(i), criterion::BatchSize::SmallInput)
        });
    }

    {
        let t = BenchServerReqConnection::new();

        c.bench_function("req_connection", |b| {
            b.iter_batched_ref(|| t.init(), |i| t.run(i), criterion::BatchSize::SmallInput)
        });
    }

    {
        let t = BenchServerStreamConnection::new();

        c.bench_function("stream_connection", |b| {
            b.iter_batched_ref(|| t.init(), |i| t.run(i), criterion::BatchSize::SmallInput)
        });
    }

    {
        let server = TestServer::new(1);
        let req_addr = server.req_addr();
        let stream_addr = server.stream_addr();

        c.bench_function("req_server workers=1", |b| b.iter(|| req(req_addr)));
        c.bench_function("stream_server workers=1", |b| b.iter(|| req(stream_addr)));
    }

    {
        let server = TestServer::new(2);
        let req_addr = server.req_addr();
        let stream_addr = server.stream_addr();

        c.bench_function("req_server workers=2", |b| b.iter(|| req(req_addr)));
        c.bench_function("stream_server workers=2", |b| b.iter(|| req(stream_addr)));
    }
}

criterion_group!(benches, criterion_benchmark);
criterion_main!(benches);
