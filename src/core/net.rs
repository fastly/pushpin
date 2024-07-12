/*
 * Copyright (C) 2022 Fanout, Inc.
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

use crate::core::event::ReadinessExt;
use crate::core::io::{AsyncRead, AsyncWrite};
use crate::core::reactor::IoEvented;
use crate::core::task::get_reactor;
use log::error;
use mio::net::{TcpListener, TcpStream, UnixListener, UnixStream};
use socket2::Socket;
use std::fmt;
use std::future::Future;
use std::io::{self, Read, Write};
use std::os::unix::io::{FromRawFd, IntoRawFd};
use std::path::Path;
use std::pin::Pin;
use std::ptr;
use std::task::{Context, Poll};

pub fn set_socket_opts(stream: &mut TcpStream) {
    if let Err(e) = stream.set_nodelay(true) {
        error!("set nodelay failed: {:?}", e);
    }

    // safety: we move the value out of stream and replace it at the end
    let ret = unsafe {
        let s = ptr::read(stream);
        let socket = Socket::from_raw_fd(s.into_raw_fd());
        let ret = socket.set_keepalive(true);
        ptr::write(stream, TcpStream::from_raw_fd(socket.into_raw_fd()));

        ret
    };

    if let Err(e) = ret {
        error!("set keepalive failed: {:?}", e);
    }
}

#[derive(Debug)]
pub enum SocketAddr {
    Ip(std::net::SocketAddr),
    Unix(std::os::unix::net::SocketAddr),
}

impl fmt::Display for SocketAddr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Ip(a) => write!(f, "{}", a),
            Self::Unix(a) => write!(f, "{:?}", a),
        }
    }
}

#[derive(Debug)]
pub enum NetListener {
    Tcp(TcpListener),
    Unix(UnixListener),
}

#[derive(Debug)]
pub enum NetStream {
    Tcp(TcpStream),
    Unix(UnixStream),
}

pub struct AsyncTcpListener {
    evented: IoEvented<TcpListener>,
}

impl AsyncTcpListener {
    pub fn new(l: TcpListener) -> Self {
        let evented = IoEvented::new(l, mio::Interest::READABLE, &get_reactor()).unwrap();

        evented.registration().set_ready(true);

        Self { evented }
    }

    pub fn bind(addr: std::net::SocketAddr) -> Result<Self, io::Error> {
        let listener = TcpListener::bind(addr)?;

        Ok(Self::new(listener))
    }

    pub fn local_addr(&self) -> Result<std::net::SocketAddr, io::Error> {
        self.evented.io().local_addr()
    }

    pub fn accept(&self) -> AcceptFuture<'_> {
        AcceptFuture { l: self }
    }

    pub fn into_inner(self) -> TcpListener {
        self.evented.into_inner()
    }
}

pub struct AsyncUnixListener {
    evented: IoEvented<UnixListener>,
}

impl AsyncUnixListener {
    pub fn new(l: UnixListener) -> Self {
        let evented = IoEvented::new(l, mio::Interest::READABLE, &get_reactor()).unwrap();

        evented.registration().set_ready(true);

        Self { evented }
    }

    pub fn bind<P: AsRef<Path>>(path: P) -> Result<Self, io::Error> {
        let listener = UnixListener::bind(path)?;

        Ok(Self::new(listener))
    }

    pub fn local_addr(&self) -> Result<std::os::unix::net::SocketAddr, io::Error> {
        self.evented.io().local_addr()
    }

    pub fn accept(&self) -> UnixAcceptFuture<'_> {
        UnixAcceptFuture { l: self }
    }
}

pub enum AsyncNetListener {
    Tcp(AsyncTcpListener),
    Unix(AsyncUnixListener),
}

impl AsyncNetListener {
    pub fn new(l: NetListener) -> Self {
        match l {
            NetListener::Tcp(l) => Self::Tcp(AsyncTcpListener::new(l)),
            NetListener::Unix(l) => Self::Unix(AsyncUnixListener::new(l)),
        }
    }

    pub fn accept(&self) -> NetAcceptFuture<'_> {
        match self {
            Self::Tcp(l) => NetAcceptFuture::Tcp(l.accept()),
            Self::Unix(l) => NetAcceptFuture::Unix(l.accept()),
        }
    }
}

pub struct AsyncTcpStream {
    evented: IoEvented<TcpStream>,
}

impl AsyncTcpStream {
    pub fn new(s: TcpStream) -> Self {
        let evented = IoEvented::new(
            s,
            mio::Interest::READABLE | mio::Interest::WRITABLE,
            &get_reactor(),
        )
        .unwrap();

        // when constructing via new(), assume I/O operations are ready to be
        // attempted
        evented
            .registration()
            .set_readiness(Some(mio::Interest::READABLE | mio::Interest::WRITABLE));

        Self { evented }
    }

    pub async fn connect(addrs: &[std::net::SocketAddr]) -> Result<Self, io::Error> {
        let mut last_err = None;

        for addr in addrs {
            let stream = match TcpStream::connect(*addr) {
                Ok(stream) => stream,
                Err(e) => {
                    last_err = Some(e);
                    continue;
                }
            };

            let mut stream = Self::new(stream);

            // when constructing via connect(), the ready state should start out
            // false because we need to wait for a writability indication
            stream.evented.registration().set_ready(false);

            let fut = TcpConnectFuture { s: &mut stream };

            if let Err(e) = fut.await {
                last_err = Some(e);
                continue;
            }

            return Ok(stream);
        }

        Err(last_err.unwrap_or_else(|| io::Error::from(io::ErrorKind::InvalidInput)))
    }

    pub fn peer_addr(&self) -> Result<std::net::SocketAddr, io::Error> {
        self.evented.io().peer_addr()
    }

    pub fn into_inner(self) -> TcpStream {
        self.evented.into_inner()
    }

    pub fn into_std(self) -> std::net::TcpStream {
        unsafe { std::net::TcpStream::from_raw_fd(self.evented.into_inner().into_raw_fd()) }
    }

    // assumes stream is in non-blocking mode
    pub fn from_std(stream: std::net::TcpStream) -> Self {
        Self::new(TcpStream::from_std(stream))
    }

    pub fn into_evented(self) -> IoEvented<TcpStream> {
        self.evented
    }
}

pub struct AsyncUnixStream {
    evented: IoEvented<UnixStream>,
}

impl AsyncUnixStream {
    pub fn new(s: UnixStream) -> Self {
        let evented = IoEvented::new(
            s,
            mio::Interest::READABLE | mio::Interest::WRITABLE,
            &get_reactor(),
        )
        .unwrap();

        // when constructing via new(), assume I/O operations are ready to be
        // attempted
        evented
            .registration()
            .set_readiness(Some(mio::Interest::READABLE | mio::Interest::WRITABLE));

        Self { evented }
    }

    pub async fn connect<P: AsRef<Path>>(path: P) -> Result<Self, io::Error> {
        let stream = UnixStream::connect(path)?;
        let mut stream = Self::new(stream);

        // when constructing via connect(), the ready state should start out
        // false because we need to wait for a writability indication
        stream.evented.registration().set_ready(false);

        let fut = UnixConnectFuture { s: &mut stream };
        fut.await?;

        Ok(stream)
    }
}

pub struct AcceptFuture<'a> {
    l: &'a AsyncTcpListener,
}

impl Future for AcceptFuture<'_> {
    type Output = Result<(TcpStream, std::net::SocketAddr), io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.l.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        if !f.l.evented.registration().is_ready() {
            return Poll::Pending;
        }

        if !f.l.evented.registration().pull_from_budget() {
            return Poll::Pending;
        }

        match f.l.evented.io().accept() {
            Ok((stream, peer_addr)) => Poll::Ready(Ok((stream, peer_addr))),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.l.evented.registration().set_ready(false);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for AcceptFuture<'_> {
    fn drop(&mut self) {
        self.l.evented.registration().clear_waker();
    }
}

pub struct UnixAcceptFuture<'a> {
    l: &'a AsyncUnixListener,
}

impl Future for UnixAcceptFuture<'_> {
    type Output = Result<(UnixStream, std::os::unix::net::SocketAddr), io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.l.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        if !f.l.evented.registration().is_ready() {
            return Poll::Pending;
        }

        if !f.l.evented.registration().pull_from_budget() {
            return Poll::Pending;
        }

        match f.l.evented.io().accept() {
            Ok((stream, peer_addr)) => Poll::Ready(Ok((stream, peer_addr))),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.l.evented.registration().set_ready(false);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for UnixAcceptFuture<'_> {
    fn drop(&mut self) {
        self.l.evented.registration().clear_waker();
    }
}

pub enum NetAcceptFuture<'a> {
    Tcp(AcceptFuture<'a>),
    Unix(UnixAcceptFuture<'a>),
}

impl Future for NetAcceptFuture<'_> {
    type Output = Result<(NetStream, SocketAddr), io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let ret = match &mut *self {
            Self::Tcp(fut) => match Pin::new(fut).poll(cx) {
                Poll::Ready(ret) => match ret {
                    Ok((stream, peer_addr)) => {
                        Ok((NetStream::Tcp(stream), SocketAddr::Ip(peer_addr)))
                    }
                    Err(e) => Err(e),
                },
                Poll::Pending => return Poll::Pending,
            },
            Self::Unix(fut) => match Pin::new(fut).poll(cx) {
                Poll::Ready(ret) => match ret {
                    Ok((stream, peer_addr)) => {
                        Ok((NetStream::Unix(stream), SocketAddr::Unix(peer_addr)))
                    }
                    Err(e) => Err(e),
                },
                Poll::Pending => return Poll::Pending,
            },
        };

        Poll::Ready(ret)
    }
}

pub struct TcpConnectFuture<'a> {
    s: &'a mut AsyncTcpStream,
}

impl Future for TcpConnectFuture<'_> {
    type Output = Result<(), io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::WRITABLE);

        if !f.s.evented.registration().is_ready() {
            return Poll::Pending;
        }

        // mio documentation says to use take_error() and peer_addr() to
        // check for connected

        if let Ok(Some(e)) | Err(e) = f.s.evented.io().take_error() {
            return Poll::Ready(Err(e));
        }

        match f.s.evented.io().peer_addr() {
            Ok(_) => Poll::Ready(Ok(())),
            Err(e) if e.kind() == io::ErrorKind::NotConnected => {
                f.s.evented
                    .registration()
                    .clear_readiness(mio::Interest::WRITABLE);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for TcpConnectFuture<'_> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub struct UnixConnectFuture<'a> {
    s: &'a mut AsyncUnixStream,
}

impl Future for UnixConnectFuture<'_> {
    type Output = Result<(), io::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::WRITABLE);

        if !f.s.evented.registration().is_ready() {
            return Poll::Pending;
        }

        // mio documentation says to use take_error() and peer_addr() to
        // check for connected

        if let Ok(Some(e)) | Err(e) = f.s.evented.io().take_error() {
            return Poll::Ready(Err(e));
        }

        match f.s.evented.io().peer_addr() {
            Ok(_) => Poll::Ready(Ok(())),
            Err(e) if e.kind() == io::ErrorKind::NotConnected => {
                f.s.evented
                    .registration()
                    .clear_readiness(mio::Interest::WRITABLE);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for UnixConnectFuture<'_> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

impl AsyncRead for AsyncTcpStream {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &mut [u8],
    ) -> Poll<Result<usize, io::Error>> {
        let f = &mut *self;

        f.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        if !f
            .evented
            .registration()
            .readiness()
            .contains_any(mio::Interest::READABLE)
        {
            return Poll::Pending;
        }

        if !f.evented.registration().pull_from_budget() {
            return Poll::Pending;
        }

        match f.evented.io().read(buf) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.evented
                    .registration()
                    .clear_readiness(mio::Interest::READABLE);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn cancel(&mut self) {
        self.evented
            .registration()
            .clear_waker_interest(mio::Interest::READABLE);
    }
}

impl AsyncWrite for AsyncTcpStream {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        let f = &mut *self;

        f.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::WRITABLE);

        if !f
            .evented
            .registration()
            .readiness()
            .contains_any(mio::Interest::WRITABLE)
        {
            return Poll::Pending;
        }

        if !f.evented.registration().pull_from_budget() {
            return Poll::Pending;
        }

        match f.evented.io().write(buf) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.evented
                    .registration()
                    .clear_readiness(mio::Interest::WRITABLE);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn poll_write_vectored(
        mut self: Pin<&mut Self>,
        cx: &mut Context,
        bufs: &[io::IoSlice<'_>],
    ) -> Poll<Result<usize, io::Error>> {
        let f = &mut *self;

        f.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::WRITABLE);

        if !f
            .evented
            .registration()
            .readiness()
            .contains_any(mio::Interest::WRITABLE)
        {
            return Poll::Pending;
        }

        if !f.evented.registration().pull_from_budget() {
            return Poll::Pending;
        }

        match f.evented.io().write_vectored(bufs) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.evented
                    .registration()
                    .clear_readiness(mio::Interest::WRITABLE);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn poll_close(self: Pin<&mut Self>, _cx: &mut Context) -> Poll<Result<(), io::Error>> {
        Poll::Ready(Ok(()))
    }

    fn is_writable(&self) -> bool {
        self.evented
            .registration()
            .readiness()
            .contains_any(mio::Interest::WRITABLE)
    }

    fn cancel(&mut self) {
        self.evented
            .registration()
            .clear_waker_interest(mio::Interest::WRITABLE);
    }
}

impl AsyncRead for AsyncUnixStream {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &mut [u8],
    ) -> Poll<Result<usize, io::Error>> {
        let f = &mut *self;

        f.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        if !f
            .evented
            .registration()
            .readiness()
            .contains_any(mio::Interest::READABLE)
        {
            return Poll::Pending;
        }

        if !f.evented.registration().pull_from_budget() {
            return Poll::Pending;
        }

        match f.evented.io().read(buf) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.evented
                    .registration()
                    .clear_readiness(mio::Interest::READABLE);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn cancel(&mut self) {
        self.evented
            .registration()
            .clear_waker_interest(mio::Interest::READABLE);
    }
}

impl AsyncWrite for AsyncUnixStream {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        let f = &mut *self;

        f.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::WRITABLE);

        if !f
            .evented
            .registration()
            .readiness()
            .contains_any(mio::Interest::WRITABLE)
        {
            return Poll::Pending;
        }

        if !f.evented.registration().pull_from_budget() {
            return Poll::Pending;
        }

        match f.evented.io().write(buf) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.evented
                    .registration()
                    .clear_readiness(mio::Interest::WRITABLE);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn poll_write_vectored(
        mut self: Pin<&mut Self>,
        cx: &mut Context,
        bufs: &[io::IoSlice<'_>],
    ) -> Poll<Result<usize, io::Error>> {
        let f = &mut *self;

        f.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::WRITABLE);

        if !f
            .evented
            .registration()
            .readiness()
            .contains_any(mio::Interest::WRITABLE)
        {
            return Poll::Pending;
        }

        if !f.evented.registration().pull_from_budget() {
            return Poll::Pending;
        }

        match f.evented.io().write_vectored(bufs) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                f.evented
                    .registration()
                    .clear_readiness(mio::Interest::WRITABLE);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn poll_close(self: Pin<&mut Self>, _cx: &mut Context) -> Poll<Result<(), io::Error>> {
        Poll::Ready(Ok(()))
    }

    fn is_writable(&self) -> bool {
        self.evented
            .registration()
            .readiness()
            .contains_any(mio::Interest::WRITABLE)
    }

    fn cancel(&mut self) {
        self.evented
            .registration()
            .clear_waker_interest(mio::Interest::WRITABLE);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::executor::Executor;
    use crate::core::io::{AsyncReadExt, AsyncWriteExt};
    use crate::core::reactor::Reactor;
    use std::fs;
    use std::str;

    #[test]
    fn async_tcpstream() {
        let reactor = Reactor::new(3); // 3 registrations
        let executor = Executor::new(2); // 2 tasks

        let spawner = executor.spawner();

        executor
            .spawn(async move {
                let addr = "127.0.0.1:0".parse().unwrap();
                let listener = AsyncTcpListener::bind(addr).expect("failed to bind");
                let addr = listener.local_addr().unwrap();

                spawner
                    .spawn(async move {
                        let mut stream = AsyncTcpStream::connect(&[addr]).await.unwrap();

                        let size = stream.write("hello".as_bytes()).await.unwrap();
                        assert_eq!(size, 5);
                    })
                    .unwrap();

                let (stream, _) = listener.accept().await.unwrap();
                let mut stream = AsyncTcpStream::new(stream);

                let mut resp = [0u8; 1024];
                let mut resp = io::Cursor::new(&mut resp[..]);

                loop {
                    let mut buf = [0; 1024];

                    let size = stream.read(&mut buf).await.unwrap();
                    if size == 0 {
                        break;
                    }

                    resp.write(&buf[..size]).unwrap();
                }

                let size = resp.position() as usize;
                let resp = str::from_utf8(&resp.get_ref()[..size]).unwrap();

                assert_eq!(resp, "hello");
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn async_unixstream() {
        // ensure pipe file doesn't exist
        match fs::remove_file("test-unixstream") {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::NotFound => {}
            Err(e) => panic!("{}", e),
        }

        let reactor = Reactor::new(3); // 3 registrations
        let executor = Executor::new(2); // 2 tasks

        let spawner = executor.spawner();

        executor
            .spawn(async move {
                let listener = AsyncUnixListener::bind("test-unixstream").expect("failed to bind");

                spawner
                    .spawn(async move {
                        let mut stream = AsyncUnixStream::connect("test-unixstream").await.unwrap();

                        let size = stream.write("hello".as_bytes()).await.unwrap();
                        assert_eq!(size, 5);
                    })
                    .unwrap();

                let (stream, _) = listener.accept().await.unwrap();
                let mut stream = AsyncUnixStream::new(stream);

                let mut resp = Vec::new();

                loop {
                    let mut buf = [0; 1024];

                    let size = stream.read(&mut buf).await.unwrap();
                    if size == 0 {
                        break;
                    }

                    resp.extend(&buf[..size]);
                }

                let resp = str::from_utf8(&resp).unwrap();

                assert_eq!(resp, "hello");
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();

        fs::remove_file("test-unixstream").unwrap();
    }
}
