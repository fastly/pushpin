/*
 * Copyright (C) 2023 Fanout, Inc.
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

use condure::channel;
use condure::client::TestClient;
use condure::executor::Executor;
use condure::future::{AsyncReadExt, AsyncSender, AsyncTcpListener, AsyncTcpStream, AsyncWriteExt};
use condure::reactor::Reactor;
use criterion::{criterion_group, criterion_main, Criterion};
use mio::net::TcpListener;
use std::net::SocketAddr;
use std::rc::Rc;
use std::str;

const REQS_PER_ITER: usize = 10;

fn req<F1, F2>(listener: TcpListener, start: F1, wait: F2) -> TcpListener
where
    F1: Fn(SocketAddr) + 'static,
    F2: Fn() + 'static,
{
    let executor = Executor::new(REQS_PER_ITER + 1);

    let addr = listener.local_addr().unwrap();

    let (s, r) = channel::channel(1);

    for _ in 0..REQS_PER_ITER {
        start(addr);
    }

    let spawner = executor.spawner();

    executor
        .spawn(async move {
            let s = AsyncSender::new(s);
            let listener = AsyncTcpListener::new(listener);

            for _ in 0..REQS_PER_ITER {
                let (stream, _) = listener.accept().await.unwrap();
                let mut stream = AsyncTcpStream::new(stream);

                spawner
                    .spawn(async move {
                        let mut buf = Vec::new();
                        let mut req_end = 0;

                        while req_end == 0 {
                            let mut chunk = [0; 1024];
                            let size = stream.read(&mut chunk).await.unwrap();
                            buf.extend_from_slice(&chunk[..size]);

                            for i in 0..(buf.len() - 3) {
                                if &buf[i..(i + 4)] == b"\r\n\r\n" {
                                    req_end = i + 4;
                                    break;
                                }
                            }
                        }

                        let expected = format!(
                            concat!("GET /path HTTP/1.1\r\n", "Host: {}\r\n", "\r\n"),
                            addr
                        );

                        assert_eq!(str::from_utf8(&buf[..req_end]).unwrap(), expected);

                        stream
                            .write(
                                b"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: 6\r\n\r\nhello\n",
                            ).await
                            .unwrap();
                    })
                    .unwrap();
            }

            s.send(listener.into_inner()).await.unwrap();
        })
        .unwrap();

    executor
        .run(|timeout| Reactor::current().unwrap().poll(timeout))
        .unwrap();

    for _ in 0..REQS_PER_ITER {
        wait();
    }

    let listener = r.recv().unwrap();

    listener
}

fn criterion_benchmark(c: &mut Criterion) {
    let mut req_listener = Some(TcpListener::bind("127.0.0.1:0".parse().unwrap()).unwrap());
    let mut stream_listener = Some(TcpListener::bind("127.0.0.1:0".parse().unwrap()).unwrap());
    let _reactor = Reactor::new(REQS_PER_ITER * 10);

    {
        let client = Rc::new(TestClient::new(1));

        c.bench_function("req_client workers=1", |b| {
            b.iter(|| {
                let c1 = Rc::clone(&client);
                let c2 = Rc::clone(&client);

                req_listener = Some(req(
                    req_listener.take().unwrap(),
                    move |addr| c1.do_req(addr),
                    move || c2.wait_req(),
                ))
            })
        });
        c.bench_function("stream_client workers=1", |b| {
            b.iter(|| {
                let c1 = Rc::clone(&client);
                let c2 = Rc::clone(&client);

                stream_listener = Some(req(
                    stream_listener.take().unwrap(),
                    move |addr| c1.do_stream_http(addr),
                    move || c2.wait_stream(),
                ))
            })
        });
    }

    {
        let client = Rc::new(TestClient::new(2));

        c.bench_function("req_client workers=2", |b| {
            b.iter(|| {
                let c1 = Rc::clone(&client);
                let c2 = Rc::clone(&client);

                req_listener = Some(req(
                    req_listener.take().unwrap(),
                    move |addr| c1.do_req(addr),
                    move || c2.wait_req(),
                ))
            })
        });
        c.bench_function("stream_client workers=2", |b| {
            b.iter(|| {
                let c1 = Rc::clone(&client);
                let c2 = Rc::clone(&client);

                stream_listener = Some(req(
                    stream_listener.take().unwrap(),
                    move |addr| c1.do_stream_http(addr),
                    move || c2.wait_stream(),
                ))
            })
        });
    }
}

criterion_group!(benches, criterion_benchmark);
criterion_main!(benches);
