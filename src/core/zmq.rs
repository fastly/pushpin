/*
 * Copyright (C) 2020-2023 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

use crate::core::reactor::{FdEvented, TimerEvented};
use crate::future::get_reactor;
use arrayvec::ArrayVec;
use std::cell::Cell;
use std::cell::RefCell;
use std::fmt;
use std::fs;
use std::future::Future;
use std::io;
use std::os::unix::fs::PermissionsExt;
use std::pin::Pin;
use std::task::{Context, Poll};
use std::time::Duration;

const MULTIPART_HEADERS_MAX: usize = 8;

// 1 for the zmq fd, and potentially 1 for the retry timer
pub const REGISTRATIONS_PER_ZMQSOCKET: usize = 2;

fn trim_prefix<'a>(s: &'a str, prefix: &str) -> Result<&'a str, ()> {
    if let Some(s) = s.strip_prefix(prefix) {
        Ok(s)
    } else {
        Err(())
    }
}

#[derive(Clone)]
pub struct SpecInfo {
    pub spec: String,
    pub bind: bool,
    pub ipc_file_mode: u32,
}

impl fmt::Display for SpecInfo {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.bind {
            write!(f, "bind:{}", self.spec)
        } else {
            write!(f, "connect:{}", self.spec)
        }
    }
}

impl fmt::Debug for SpecInfo {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self)
    }
}

#[derive(Debug)]
pub enum ZmqSocketError {
    Connect(String, zmq::Error),
    Bind(String, zmq::Error),
    SetMode(String, io::Error),
}

impl fmt::Display for ZmqSocketError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ZmqSocketError::Connect(spec, e) => write!(f, "connect {}: {}", spec, e),
            ZmqSocketError::Bind(spec, e) => write!(f, "bind {}: {}", spec, e),
            ZmqSocketError::SetMode(spec, e) => write!(f, "set mode {}: {}", spec, e),
        }
    }
}

#[derive(Clone)]
struct ActiveSpec {
    pub spec: SpecInfo,
    pub endpoint: String,
}

fn unbind(sock: &zmq::Socket, endpoint: &str) -> zmq::Result<()> {
    // NOTE: use zmq_unbind instead when it becomes available in rust-zmq
    sock.disconnect(endpoint)
}

fn setup_spec(sock: &zmq::Socket, spec: &SpecInfo) -> Result<String, ZmqSocketError> {
    if spec.bind {
        match sock.bind(&spec.spec) {
            Ok(_) => {
                let endpoint = sock.get_last_endpoint().unwrap().unwrap();

                if let Ok(path) = trim_prefix(&spec.spec, "ipc://") {
                    if spec.ipc_file_mode > 0 {
                        let perms = fs::Permissions::from_mode(spec.ipc_file_mode);
                        if let Err(e) = fs::set_permissions(path, perms) {
                            // if setting perms fails, undo the bind
                            unbind(sock, &endpoint).unwrap();

                            return Err(ZmqSocketError::SetMode(spec.spec.clone(), e));
                        }
                    }
                }

                Ok(endpoint)
            }
            Err(e) => Err(ZmqSocketError::Bind(spec.spec.clone(), e)),
        }
    } else {
        match sock.connect(&spec.spec) {
            Ok(_) => Ok(spec.spec.clone()),
            Err(e) => Err(ZmqSocketError::Connect(spec.spec.clone(), e)),
        }
    }
}

fn unsetup_spec(sock: &zmq::Socket, spec: &ActiveSpec) {
    if spec.spec.bind {
        unbind(sock, &spec.endpoint).unwrap();

        if let Ok(path) = trim_prefix(&spec.endpoint, "ipc://") {
            if fs::remove_file(path).is_err() {
                // oh well, we tried
            }
        }
    } else {
        sock.disconnect(&spec.endpoint).unwrap();
    }
}

pub type MultipartHeader = Vec<zmq::Message>;

pub struct ZmqSocket {
    inner: zmq::Socket,
    events: Cell<zmq::PollEvents>,
    specs: RefCell<Vec<ActiveSpec>>,
}

impl ZmqSocket {
    pub fn new(ctx: &zmq::Context, socket_type: zmq::SocketType) -> Self {
        Self {
            inner: ctx.socket(socket_type).unwrap(),
            events: Cell::new(zmq::PollEvents::empty()),
            specs: RefCell::new(Vec::new()),
        }
    }

    pub fn inner(&self) -> &zmq::Socket {
        &self.inner
    }

    pub fn update_events(&self) {
        loop {
            match self.inner.get_events() {
                Ok(events) => {
                    self.events.set(events);
                    break;
                }
                Err(zmq::Error::EINTR) => continue,
                Err(e) => panic!("get events error: {}", e),
            }
        }
    }

    pub fn events(&self) -> zmq::PollEvents {
        self.events.get()
    }

    pub fn send(&self, msg: zmq::Message, flags: i32) -> Result<(), zmq::Error> {
        let flags = flags & zmq::DONTWAIT;

        if let Err(e) = self.inner.send(msg, flags) {
            self.update_events();
            return Err(e);
        }

        self.update_events();

        Ok(())
    }

    pub fn send_to(
        &self,
        header: &MultipartHeader,
        content: zmq::Message,
        flags: i32,
    ) -> Result<(), zmq::Error> {
        if header.len() > MULTIPART_HEADERS_MAX {
            return Err(zmq::Error::EINVAL);
        }

        let mut headers: ArrayVec<&[u8], MULTIPART_HEADERS_MAX> = ArrayVec::new();

        for part in header {
            headers.push(part);
        }

        let flags = flags & zmq::DONTWAIT;

        if let Err(e) = self.inner.send_multipart(&headers, flags | zmq::SNDMORE) {
            self.update_events();
            return Err(e);
        }

        if let Err(e) = self.inner.send(zmq::Message::new(), flags | zmq::SNDMORE) {
            self.update_events();
            return Err(e);
        }

        self.send(content, flags)
    }

    pub fn recv(&self, flags: i32) -> Result<zmq::Message, zmq::Error> {
        let flags = flags & zmq::DONTWAIT;

        // get the first part
        let msg = match self.inner.recv_msg(flags) {
            Ok(msg) => msg,
            Err(e) => {
                self.update_events();
                return Err(e);
            }
        };

        let flags = 0;

        // eat the rest of the parts
        while self.inner.get_rcvmore().unwrap() {
            self.inner.recv_msg(flags).unwrap();
        }

        self.update_events();

        Ok(msg)
    }

    pub fn recv_routed(&self, flags: i32) -> Result<(MultipartHeader, zmq::Message), zmq::Error> {
        let flags = flags & zmq::DONTWAIT;

        let mut header = MultipartHeader::new();

        loop {
            // read parts until we reach the separator
            match self.inner.recv_msg(flags) {
                Ok(msg) => {
                    if msg.is_empty() {
                        break;
                    }

                    if header.len() == MULTIPART_HEADERS_MAX {
                        // header too large

                        let flags = 0;

                        // eat the rest of the parts
                        while self.inner.get_rcvmore().unwrap() {
                            self.inner.recv_msg(flags).unwrap();
                        }

                        self.update_events();

                        return Err(zmq::Error::EINVAL);
                    }

                    header.push(msg);
                }
                Err(e) => {
                    self.update_events();
                    return Err(e);
                }
            }
        }

        let flags = 0;

        // if we get here, we've read the separator. content parts should follow

        if !self.inner.get_rcvmore().unwrap() {
            return Err(zmq::Error::EINVAL);
        }

        // get the first part of the content
        let msg = match self.inner.recv_msg(flags) {
            Ok(msg) => msg,
            Err(e) => {
                self.update_events();
                return Err(e);
            }
        };

        // eat the rest of the parts
        while self.inner.get_rcvmore().unwrap() {
            self.inner.recv_msg(flags).unwrap();
        }

        self.update_events();

        Ok((header, msg))
    }

    pub fn apply_specs(&self, new_specs: &[SpecInfo]) -> Result<(), ZmqSocketError> {
        let mut specs = self.specs.borrow_mut();

        let mut to_remove = Vec::new();
        for cur in specs.iter() {
            let mut found = false;
            for new in new_specs.iter() {
                if cur.spec.spec == new.spec && cur.spec.bind == new.bind {
                    found = true;
                    break;
                }
            }
            if !found {
                to_remove.push(cur.clone());
            }
        }

        let mut to_add = Vec::new();
        let mut to_update = Vec::new();
        for new in new_specs.iter() {
            let mut found = None;
            for (ci, cur) in specs.iter().enumerate() {
                if new.spec == cur.spec.spec && new.bind == cur.spec.bind {
                    found = Some(ci);
                    break;
                }
            }
            match found {
                Some(ci) => {
                    if new.ipc_file_mode != specs[ci].spec.ipc_file_mode {
                        to_update.push(new.clone());
                    }
                }
                None => {
                    to_add.push(new.clone());
                }
            }
        }

        let mut added = Vec::new();

        // add specs we dont have. on fail, undo them
        for spec in to_add.iter() {
            match setup_spec(&self.inner, spec) {
                Ok(endpoint) => {
                    added.push(ActiveSpec {
                        spec: spec.clone(),
                        endpoint,
                    });
                }
                Err(e) => {
                    // undo previous adds
                    for spec in added.iter().rev() {
                        unsetup_spec(&self.inner, spec);
                    }
                    return Err(e);
                }
            }
        }

        // update ipc file mode
        let mut prev_perms = Vec::new();
        for spec in to_update.iter() {
            let mut err = None;

            if let Ok(path) = trim_prefix(&spec.spec, "ipc://") {
                if spec.ipc_file_mode > 0 {
                    match fs::metadata(path) {
                        Ok(meta) => {
                            let perms = fs::Permissions::from_mode(spec.ipc_file_mode);
                            match fs::set_permissions(path, perms) {
                                Ok(_) => {
                                    prev_perms.push((String::from(path), meta.permissions()));
                                }
                                Err(e) => {
                                    err = Some(ZmqSocketError::SetMode(spec.spec.clone(), e));
                                }
                            }
                        }
                        Err(e) => {
                            err = Some(ZmqSocketError::SetMode(spec.spec.clone(), e));
                        }
                    }
                }
            }

            if let Some(err) = err {
                // undo previous perms changes
                for (path, perms) in prev_perms {
                    if fs::set_permissions(path, perms).is_err() {
                        // oh well, we tried
                    }
                }

                // undo previous adds
                for spec in added.iter().rev() {
                    unsetup_spec(&self.inner, spec);
                }

                return Err(err);
            }
        }

        for spec in to_remove.iter() {
            unsetup_spec(&self.inner, spec);
        }

        // move current specs aside
        let prev_specs = std::mem::take(&mut *specs);

        // recompute current specs
        for new in new_specs {
            let mut s = None;

            // is it one we added?
            for spec in added.iter() {
                if new.spec == spec.spec.spec && new.bind == spec.spec.bind {
                    s = Some(spec.clone());
                    break;
                }
            }

            // else, it must be one we had already
            if s.is_none() {
                for spec in prev_specs.iter() {
                    if new.spec == spec.spec.spec && new.bind == spec.spec.bind {
                        s = Some(spec.clone());
                        break;
                    }
                }
            }

            assert!(s.is_some());

            specs.push(s.unwrap());
        }

        Ok(())
    }
}

impl Drop for ZmqSocket {
    fn drop(&mut self) {
        let specs = self.specs.borrow();

        for spec in specs.iter() {
            unsetup_spec(&self.inner, spec);
        }
    }
}

pub struct AsyncZmqSocket {
    evented: FdEvented,
    inner: ZmqSocket,
    timeout: Cell<Option<Duration>>,
}

impl AsyncZmqSocket {
    pub fn new(s: ZmqSocket) -> Self {
        let evented = FdEvented::new(
            s.inner().get_fd().unwrap(),
            mio::Interest::READABLE,
            &get_reactor(),
        )
        .unwrap();

        // zmq events are used for readiness, and registration readiness is
        // used to tell us when to call update_events(). we'll call that
        // below, so registration readiness can start out false
        evented.registration().set_ready(false);

        s.update_events();

        Self {
            evented,
            inner: s,
            timeout: Cell::new(None),
        }
    }

    pub fn inner(&self) -> &ZmqSocket {
        &self.inner
    }

    pub fn set_retry_timeout(&self, timeout: Option<Duration>) {
        self.timeout.set(timeout);
    }

    pub fn send(&self, msg: zmq::Message) -> ZmqSendFuture<'_> {
        ZmqSendFuture { s: self, msg }
    }

    pub fn send_to(&self, header: MultipartHeader, content: zmq::Message) -> ZmqSendToFuture<'_> {
        ZmqSendToFuture {
            s: self,
            header,
            content,
            timer_evented: None,
        }
    }

    pub fn recv(&self) -> ZmqRecvFuture<'_> {
        ZmqRecvFuture { s: self }
    }

    pub fn recv_routed(&self) -> ZmqRecvRoutedFuture<'_> {
        ZmqRecvRoutedFuture { s: self }
    }
}

pub struct ZmqSendFuture<'a> {
    s: &'a AsyncZmqSocket,
    msg: zmq::Message,
}

impl Future for ZmqSendFuture<'_> {
    type Output = Result<(), zmq::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        if f.s.evented.registration().is_ready() {
            f.s.inner.update_events();
            f.s.evented.registration().set_ready(false);
        }

        if !f.s.inner.events().contains(zmq::POLLOUT) {
            return Poll::Pending;
        }

        // NOTE: when rust-zmq allows resending messages we can
        //   avoid this copy

        let msg = zmq::Message::from(&f.msg[..]);

        match f.s.inner.send(msg, zmq::DONTWAIT) {
            Ok(()) => Poll::Ready(Ok(())),
            Err(zmq::Error::EAGAIN) => Poll::Pending,
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for ZmqSendFuture<'_> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub struct ZmqSendToFuture<'a> {
    s: &'a AsyncZmqSocket,
    header: MultipartHeader,
    content: zmq::Message,
    timer_evented: Option<TimerEvented>,
}

impl Future for ZmqSendToFuture<'_> {
    type Output = Result<(), zmq::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        let reactor = f.s.evented.registration().reactor();

        if let Some(timer_evented) = &f.timer_evented {
            timer_evented
                .registration()
                .set_waker(cx.waker(), mio::Interest::READABLE);

            if reactor.now() < timer_evented.expires() {
                timer_evented.registration().set_ready(false);

                return Poll::Pending;
            }

            f.timer_evented = None;
        }

        assert!(f.timer_evented.is_none());

        f.s.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        if f.s.evented.registration().is_ready() {
            f.s.inner.update_events();
            f.s.evented.registration().set_ready(false);
        }

        if !f.s.inner.events().contains(zmq::POLLOUT) {
            return Poll::Pending;
        }

        // NOTE: when rust-zmq allows resending messages we can
        //   avoid this copy

        let content = zmq::Message::from(&f.content[..]);

        match f.s.inner.send_to(&f.header, content, zmq::DONTWAIT) {
            Ok(()) => Poll::Ready(Ok(())),
            Err(zmq::Error::EAGAIN) => {
                if let Some(timeout) = f.s.timeout.get() {
                    let expires = reactor.now() + timeout;
                    let timer_evented = TimerEvented::new(expires, &reactor).unwrap();

                    f.s.evented.registration().clear_waker();

                    timer_evented.registration().set_ready(true);
                    timer_evented
                        .registration()
                        .set_waker(cx.waker(), mio::Interest::READABLE);

                    f.timer_evented = Some(timer_evented);
                }

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for ZmqSendToFuture<'_> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub struct ZmqRecvFuture<'a> {
    s: &'a AsyncZmqSocket,
}

impl Future for ZmqRecvFuture<'_> {
    type Output = Result<zmq::Message, zmq::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        if f.s.evented.registration().is_ready() {
            f.s.inner.update_events();
            f.s.evented.registration().set_ready(false);
        }

        if !f.s.inner.events().contains(zmq::POLLIN) {
            return Poll::Pending;
        }

        match f.s.inner.recv(zmq::DONTWAIT) {
            Ok(msg) => Poll::Ready(Ok(msg)),
            Err(zmq::Error::EAGAIN) => Poll::Pending,
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for ZmqRecvFuture<'_> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

pub struct ZmqRecvRoutedFuture<'a> {
    s: &'a AsyncZmqSocket,
}

impl Future for ZmqRecvRoutedFuture<'_> {
    type Output = Result<(MultipartHeader, zmq::Message), zmq::Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        f.s.evented
            .registration()
            .set_waker(cx.waker(), mio::Interest::READABLE);

        if f.s.evented.registration().is_ready() {
            f.s.inner.update_events();
            f.s.evented.registration().set_ready(false);
        }

        if !f.s.inner.events().contains(zmq::POLLIN) {
            return Poll::Pending;
        }

        match f.s.inner.recv_routed(zmq::DONTWAIT) {
            Ok(ret) => Poll::Ready(Ok(ret)),
            Err(zmq::Error::EAGAIN) => Poll::Pending,
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for ZmqRecvRoutedFuture<'_> {
    fn drop(&mut self) {
        self.s.evented.registration().clear_waker();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::executor::Executor;
    use crate::core::reactor::Reactor;
    use crate::future::poll_async;
    use std::rc::Rc;
    use std::thread;

    #[test]
    fn send_after_disconnect() {
        let zmq_context = zmq::Context::new();

        let s = ZmqSocket::new(&zmq_context, zmq::REQ);
        s.apply_specs(&[SpecInfo {
            spec: String::from("inproc://send-test"),
            bind: true,
            ipc_file_mode: 0,
        }])
        .unwrap();

        assert_eq!(s.events().contains(zmq::POLLOUT), false);

        let r = ZmqSocket::new(&zmq_context, zmq::REP);
        r.apply_specs(&[SpecInfo {
            spec: String::from("inproc://send-test"),
            bind: false,
            ipc_file_mode: 0,
        }])
        .unwrap();

        s.update_events();

        assert_eq!(s.events().contains(zmq::POLLOUT), true);

        drop(r);

        assert_eq!(
            s.send((&b"test"[..]).into(), zmq::DONTWAIT),
            Err(zmq::Error::EAGAIN)
        );

        assert_eq!(s.events().contains(zmq::POLLOUT), false);
    }

    #[test]
    fn async_send_recv() {
        let reactor = Reactor::new(2);
        let executor = Executor::new(2);

        let spec = "inproc://futures::tests::test_zmq";

        let context = zmq::Context::new();

        let s = AsyncZmqSocket::new(ZmqSocket::new(&context, zmq::PUSH));
        let r = AsyncZmqSocket::new(ZmqSocket::new(&context, zmq::PULL));

        s.inner().inner().bind(spec).unwrap();
        s.inner().inner().set_sndhwm(1).unwrap();

        executor
            .spawn(async move {
                s.send(zmq::Message::from(&b"1"[..])).await.unwrap();
                s.send(zmq::Message::from(&b"2"[..])).await.unwrap();
            })
            .unwrap();

        executor.run_until_stalled();

        assert_eq!(executor.have_tasks(), true);

        r.inner().inner().connect(spec).unwrap();

        executor
            .spawn(async move {
                assert_eq!(r.recv().await, Ok(zmq::Message::from(&b"1"[..])));
                assert_eq!(r.recv().await, Ok(zmq::Message::from(&b"2"[..])));
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn async_routable() {
        let reactor = Reactor::new(2);
        let executor = Executor::new(2);

        let spec = "inproc://futures::tests::test_zmq_routable";

        let context = zmq::Context::new();

        let s = AsyncZmqSocket::new(ZmqSocket::new(&context, zmq::DEALER));
        let r = AsyncZmqSocket::new(ZmqSocket::new(&context, zmq::ROUTER));

        s.inner().inner().bind(spec).unwrap();
        s.inner().inner().set_sndhwm(1).unwrap();

        executor
            .spawn(async move {
                s.send_to(MultipartHeader::new(), zmq::Message::from(&b"1"[..]))
                    .await
                    .unwrap();
                s.send_to(MultipartHeader::new(), zmq::Message::from(&b"2"[..]))
                    .await
                    .unwrap();
            })
            .unwrap();

        executor.run_until_stalled();

        assert_eq!(executor.have_tasks(), true);

        r.inner().inner().connect(spec).unwrap();

        executor
            .spawn(async move {
                let (_, msg) = r.recv_routed().await.unwrap();
                assert_eq!(msg, zmq::Message::from(&b"1"[..]));
                let (_, msg) = r.recv_routed().await.unwrap();
                assert_eq!(msg, zmq::Message::from(&b"2"[..]));
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }

    #[test]
    fn async_retry_timeout() {
        let executor = Executor::new(2);
        let reactor = Reactor::new(2);

        let spec = "inproc://futures::tests::test_zmq_routable";

        let context = zmq::Context::new();

        let s = ZmqSocket::new(&context, zmq::ROUTER);
        let r1 = ZmqSocket::new(&context, zmq::ROUTER);
        let r2 = ZmqSocket::new(&context, zmq::ROUTER);

        s.inner().set_sndhwm(1).unwrap();
        s.inner().set_router_mandatory(true).unwrap();
        s.apply_specs(&[SpecInfo {
            spec: spec.to_string(),
            bind: true,
            ipc_file_mode: 0,
        }])
        .unwrap();

        r1.inner().set_rcvhwm(1).unwrap();
        r1.inner().set_identity(b"test1").unwrap();
        r1.apply_specs(&[SpecInfo {
            spec: spec.to_string(),
            bind: false,
            ipc_file_mode: 0,
        }])
        .unwrap();

        r2.inner().set_rcvhwm(1).unwrap();
        r2.inner().set_identity(b"test2").unwrap();
        r2.apply_specs(&[SpecInfo {
            spec: spec.to_string(),
            bind: false,
            ipc_file_mode: 0,
        }])
        .unwrap();

        // ensure both peers are connected

        loop {
            let mut h = MultipartHeader::new();
            h.push(zmq::Message::from(&b"test1"[..]));

            match s.send_to(&h, zmq::Message::from(&b"1"[..]), 0) {
                Ok(()) => break,
                Err(zmq::Error::EHOSTUNREACH) => thread::sleep(Duration::from_millis(10)),
                Err(e) => panic!("{}", e),
            }
        }

        loop {
            let mut h = MultipartHeader::new();
            h.push(zmq::Message::from(&b"test2"[..]));

            match s.send_to(&h, zmq::Message::from(&b"1"[..]), 0) {
                Ok(()) => break,
                Err(zmq::Error::EHOSTUNREACH) => thread::sleep(Duration::from_millis(10)),
                Err(e) => panic!("{}", e),
            }
        }

        // we can clear out r1
        let (_, msg) = r1.recv_routed(0).unwrap();
        assert_eq!(msg, zmq::Message::from(&b"1"[..]));

        // wrap in Rc so the inproc sender is not dropped until after the
        // messages have been received
        let s = Rc::new(AsyncZmqSocket::new(s));

        s.set_retry_timeout(Some(Duration::from_millis(0)));

        {
            let s = s.clone();

            executor
                .spawn(async move {
                    // second write will succeed immediately

                    let mut h = MultipartHeader::new();
                    h.push(zmq::Message::from(&b"test2"[..]));
                    s.send_to(h, zmq::Message::from(&b"2"[..])).await.unwrap();

                    // third write will block

                    let mut h = MultipartHeader::new();
                    h.push(zmq::Message::from(&b"test2"[..]));
                    let mut fut = s.send_to(h, zmq::Message::from(&b"3"[..]));

                    assert_eq!(poll_async(&mut fut).await, Poll::Pending);
                    assert_eq!(fut.timer_evented.is_some(), true);

                    fut.await.unwrap();
                })
                .unwrap();
        }

        executor.run_until_stalled();

        assert_eq!(executor.have_tasks(), true);

        // this will allow the third write to go through
        let (_, msg) = r2.recv_routed(0).unwrap();
        assert_eq!(msg, zmq::Message::from(&b"1"[..]));

        executor.run(|timeout| reactor.poll(timeout)).unwrap();

        let (_, msg) = r2.recv_routed(0).unwrap();
        assert_eq!(msg, zmq::Message::from(&b"2"[..]));
        let (_, msg) = r2.recv_routed(0).unwrap();
        assert_eq!(msg, zmq::Message::from(&b"3"[..]));
    }
}
