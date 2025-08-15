use crate::core::io::{AsyncRead, AsyncWrite};
use crate::core::net::AsyncUnixStream;
use std::cell::RefCell;
use std::io;
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::rc::Rc;
use std::task::{ready, Context, Poll};

pub struct ReadHalf<T: AsyncRead> {
    handle: Rc<RefCell<T>>,
}

// NOTE: waker interest is not cleared if operation is abandoned
impl<T: AsyncRead> tokio::io::AsyncRead for ReadHalf<T> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &mut tokio::io::ReadBuf,
    ) -> Poll<Result<(), io::Error>> {
        let mut handle = self.handle.borrow_mut();

        let size = match ready!(AsyncRead::poll_read(
            Pin::new(&mut *handle),
            cx,
            buf.initialize_unfilled()
        )) {
            Ok(size) => size,
            Err(e) => return Poll::Ready(Err(e)),
        };

        buf.set_filled(size);

        Poll::Ready(Ok(()))
    }
}

pub struct WriteHalf<T: AsyncWrite> {
    handle: Rc<RefCell<T>>,
}

// NOTE: waker interest is not cleared if operation is abandoned
impl<T: AsyncWrite> tokio::io::AsyncWrite for WriteHalf<T> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        let mut handle = self.handle.borrow_mut();

        AsyncWrite::poll_write(Pin::new(&mut *handle), cx, buf)
    }

    fn poll_flush(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        // nothing to do
        Poll::Ready(Ok(()))
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        let mut handle = self.handle.borrow_mut();

        AsyncWrite::poll_close(Pin::new(&mut *handle), cx)
    }
}

fn io_split<T: AsyncRead + AsyncWrite>(stream: T) -> (ReadHalf<T>, WriteHalf<T>) {
    let handle = Rc::new(RefCell::new(stream));

    let r = ReadHalf {
        handle: Rc::clone(&handle),
    };

    let w = WriteHalf { handle };

    (r, w)
}

pub struct OrigindStream {
    inner: origind_client::OrigindStream<ReadHalf<AsyncUnixStream>, WriteHalf<AsyncUnixStream>>,
    to_flush: Option<usize>,
}

impl AsyncRead for OrigindStream {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &mut [u8],
    ) -> Poll<Result<usize, io::Error>> {
        let s = Pin::into_inner(self);
        let inner = Pin::new(&mut s.inner);

        let mut buf = tokio::io::ReadBuf::new(buf);

        if let Err(e) = ready!(tokio::io::AsyncRead::poll_read(inner, cx, &mut buf)) {
            return Poll::Ready(Err(e));
        }

        Poll::Ready(Ok(buf.filled().len()))
    }

    fn cancel(&mut self) {
        // nothing to do
    }
}

impl AsyncWrite for OrigindStream {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        let s = Pin::into_inner(self);
        let mut inner = Pin::new(&mut s.inner);

        if s.to_flush.is_none() {
            match ready!(tokio::io::AsyncWrite::poll_write(inner.as_mut(), cx, buf)) {
                Ok(size) => s.to_flush = Some(size),
                Err(e) => return Poll::Ready(Err(e)),
            }
        }

        if let Err(e) = ready!(tokio::io::AsyncWrite::poll_flush(inner, cx)) {
            return Poll::Ready(Err(e));
        }

        let size = s.to_flush.take().unwrap();

        Poll::Ready(Ok(size))
    }

    fn poll_close(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Result<(), io::Error>> {
        let s = Pin::into_inner(self);
        let inner = Pin::new(&mut s.inner);

        tokio::io::AsyncWrite::poll_shutdown(inner, cx)
    }

    fn is_writable(&self) -> bool {
        // nowhere to get this so just say true all the time
        true
    }

    fn cancel(&mut self) {
        // nothing to do
    }
}

pub struct OrigindManager {
    path: PathBuf,
}

impl OrigindManager {
    pub fn new<P: AsRef<Path>>(path: P) -> Self {
        Self {
            path: path.as_ref().into(),
        }
    }

    pub async fn connect_tls(
        &self,
        host: &str,
        port: u16,
        cert_hostname: &str,
        check_cert: bool,
    ) -> Result<OrigindStream, origind_client::OrigindError> {
        let backend = origind_client::BackendDef {
            sid: None, // NOTE: need valid sid for mtls
            name: format!("pushpin_{host}"),
            host: host.to_string(),
            port,
            max_connections: None,
            is_private: false,
            connect_timeout_ms: 10000,
            ssl_config: Some(origind_client::BackendSslConf {
                min_tls_version: None,
                max_tls_version: None,
                cert_hostname: cert_hostname.to_string(),
                ca_cert: None,
                client_certificate: None,
                ciphers: None,
                check_cert,
                sni_hostname: Some(cert_hostname.to_string()),
                alpns: Vec::new(),
            }),
        };

        let opts = origind_client::ConnectionOptions {
            tcp_nodelay: true,
            ..Default::default()
        };

        let stream = AsyncUnixStream::connect(&self.path).await?;

        let (reader, writer) = io_split(stream);

        let client = origind_client::OrigindClient::from_streams(
            reader,
            writer,
            origind_client::ClientConnectionConfig::default(),
        );

        let c = client.create_connection(backend, opts).await?;

        Ok(OrigindStream {
            inner: c.into_origind_stream(),
            to_flush: None,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::executor::Executor;
    use crate::core::io::{AsyncReadExt, AsyncWriteExt};
    use crate::core::net::AsyncUnixListener;
    use crate::core::reactor::Reactor;
    use crate::core::task::{poll_async, yield_task};
    use bytes::{Bytes, BytesMut};
    use std::fs;
    use std::path::PathBuf;
    use std::pin::pin;
    use tokio_util::codec::{Decoder, Encoder};

    #[test]
    fn connect() {
        let path = PathBuf::from("origind-test");

        // ensure pipe file doesn't exist
        match fs::remove_file(&path) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::NotFound => {}
            Err(e) => panic!("{}", e),
        }

        let reactor = Reactor::new(3);
        let executor = Executor::new(3);

        executor
            .spawn(async move {
                let l = AsyncUnixListener::bind(&path).unwrap();

                let m = OrigindManager::new(&path);
                let mut f = pin!(m.connect_tls("127.0.0.1", 80, "localhost", false));

                // initiate connection
                assert!(poll_async(f.as_mut()).await.is_pending());

                let (server, _) = l.accept().await.unwrap();
                let mut server = AsyncUnixStream::new(server);

                // process i/o events so unix connection can complete
                yield_task().await;

                // cause client to send a message
                assert!(poll_async(f.as_mut()).await.is_pending());

                // wait for message
                let mut inbuf = BytesMut::new();
                let mut codec = origind_common::codec::OrigindCodec::default();
                let msg = loop {
                    let mut buf = [0; 16_384];
                    let size = server.read(&mut buf).await.unwrap();
                    let buf = &buf[..size];
                    inbuf.extend_from_slice(&buf);

                    if let Some(msg) = codec.decode(&mut inbuf).unwrap() {
                        break msg;
                    }
                };

                println!("C->S {:?}", msg);

                let msg = origind_common::OrigindMsg::Result(origind_common::OrigindResult {
                    msg: None,
                    kind: origind_common::OrigindResultKind::ConnectionCreated(
                        origind_common::ConnectionCreated {
                            description: origind_common::ConnectionDescription {
                                backend_id: 0,
                                connection_id: 0,
                            },
                            info: origind_common::ConnectInfo {
                                connections: 0,
                                racing: false,
                                provider_label: String::new(),
                                provider_ip: None,
                                race_sources: Vec::new(),
                            },
                        },
                    ),
                });

                println!("S->C {:?}", msg);

                let mut outbuf = BytesMut::new();
                codec.encode(msg, &mut outbuf).unwrap();

                while !outbuf.is_empty() {
                    let size = server.write(&outbuf).await.unwrap();
                    let _ = outbuf.split_to(size);
                }

                let mut client = f.await.unwrap();

                // write data from client

                let data = b"hel";
                let mut outbuf = BytesMut::from(data.as_slice());

                while !outbuf.is_empty() {
                    let size = client.write(&outbuf).await.unwrap();
                    let _ = outbuf.split_to(size);
                }

                let mut inbuf = BytesMut::new();
                let mut codec = origind_common::codec::OrigindCodec::default();

                // wait for message
                let msg = loop {
                    let mut buf = [0; 16_384];
                    let size = server.read(&mut buf).await.unwrap();
                    let buf = &buf[..size];
                    inbuf.extend_from_slice(&buf);

                    if let Some(msg) = codec.decode(&mut inbuf).unwrap() {
                        break msg;
                    }
                };

                println!("C->S {:?}", msg);

                let expected = Bytes::from(data.to_vec());
                assert_eq!(msg, origind_common::OrigindMsg::Data(expected));

                // write data from client again

                let data = b"lo";
                let mut outbuf = BytesMut::from(data.as_slice());

                while !outbuf.is_empty() {
                    let size = client.write(&outbuf).await.unwrap();
                    let _ = outbuf.split_to(size);
                }

                // wait for message
                let msg = loop {
                    let mut buf = [0; 16_384];
                    let size = server.read(&mut buf).await.unwrap();
                    let buf = &buf[..size];
                    inbuf.extend_from_slice(&buf);

                    if let Some(msg) = codec.decode(&mut inbuf).unwrap() {
                        break msg;
                    }
                };

                println!("C->S {:?}", msg);

                let expected = Bytes::from(data.to_vec());
                assert_eq!(msg, origind_common::OrigindMsg::Data(expected));

                let data = b"world";
                let msg = origind_common::OrigindMsg::Data(Bytes::from(data.to_vec()));

                println!("S->C {:?}", msg);

                let mut outbuf = BytesMut::new();
                codec.encode(msg, &mut outbuf).unwrap();

                while !outbuf.is_empty() {
                    let size = server.write(&outbuf).await.unwrap();
                    let _ = outbuf.split_to(size);
                }

                let mut inbuf = BytesMut::new();
                while inbuf.len() < data.len() {
                    let mut buf = [0; 16_384];
                    let size = client.read(&mut buf).await.unwrap();
                    let buf = &buf[..size];
                    inbuf.extend_from_slice(&buf);
                }
                assert_eq!(inbuf, data.as_slice());
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }
}
