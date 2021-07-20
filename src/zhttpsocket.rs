/*
 * Copyright (C) 2020-2021 Fanout, Inc.
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

use crate::arena;
use crate::channel;
use crate::event;
use crate::executor::Executor;
use crate::future::{
    select_9, select_option, select_slice, AsyncReceiver, AsyncSender, AsyncZmqSocket, RecvFuture,
    Select9, ZmqSendFuture, ZmqSendToFuture, REGISTRATIONS_PER_CHANNEL,
    REGISTRATIONS_PER_ZMQSOCKET,
};
use crate::list;
use crate::reactor::Reactor;
use crate::tnetstring;
use crate::zhttppacket::{Id, Response, ResponseScratch};
use crate::zmq::{MultipartHeader, SpecInfo, ZmqSocket};
use arrayvec::{ArrayString, ArrayVec};
use log::{debug, error, log_enabled, trace, warn};
use slab::Slab;
use std::cell::{Cell, RefCell};
use std::fmt;
use std::future::Future;
use std::io;
use std::marker;
use std::pin::Pin;
use std::str;
use std::str::FromStr;
use std::sync::{mpsc, Arc, Mutex};
use std::task::{Context, Poll};
use std::thread;
use std::time::Duration;

const HANDLES_MAX: usize = 1_024;
const STREAM_OUT_STREAM_DELAY: Duration = Duration::from_millis(50);
const LOG_METADATA_MAX: usize = 1_000;
const LOG_CONTENT_MAX: usize = 1_000;
const EXECUTOR_TASKS_MAX: usize = 1;

fn trim(s: &str, max: usize) -> String {
    // NOTE: O(n)
    let char_len = s.chars().count();

    if char_len > max && max >= 7 {
        let dist = max / 2;
        let mut left_end = 0;
        let mut right_start = 0;

        // NOTE: O(n)
        for (i, (pos, _)) in s.char_indices().enumerate() {
            // dist guaranteed to be < char_len
            if i == dist {
                left_end = pos;
            }

            // (char_len - dist + 3) guaranteed to be < char_len
            if i == char_len - dist + 3 {
                right_start = pos;
            }
        }

        let left = &s[..left_end];
        let right = &s[right_start..];

        format!("{}...{}", left, right)
    } else {
        s.to_owned()
    }
}

struct Packet<'a> {
    map_frame: tnetstring::Frame<'a>,
    content_field: Option<&'a str>,
}

impl Packet<'_> {
    fn fmt_metadata(&self, f: &mut dyn io::Write) -> Result<(), io::Error> {
        let it = tnetstring::MapIterator::new(self.map_frame.data);

        write!(f, "{{ ")?;

        let mut first = true;

        for mi in it {
            let mi = match mi {
                Ok(mi) => mi,
                Err(_) => return Ok(()),
            };

            if let Some(field) = self.content_field {
                if mi.key == field {
                    continue;
                }
            }

            // can't fail
            let (frame, _) = tnetstring::parse_frame(mi.data).unwrap();

            if !first {
                write!(f, ", ")?;
            }

            first = false;

            write!(f, "\"{}\": {}", mi.key, frame)?;
        }

        write!(f, " }}")
    }

    fn fmt_content(&self, f: &mut dyn io::Write) -> Result<Option<usize>, io::Error> {
        let field = match self.content_field {
            Some(field) => field,
            None => return Ok(None),
        };

        let it = tnetstring::MapIterator::new(self.map_frame.data);

        let mut ptype = &b""[..];
        let mut content = None;

        for mi in it {
            let mi = match mi {
                Ok(mi) => mi,
                Err(_) => return Ok(None),
            };

            if mi.key == "type" {
                ptype = match tnetstring::parse_string(mi.data) {
                    Ok(s) => s,
                    Err(_) => return Ok(None),
                };
            }

            // can't fail
            let (frame, _) = tnetstring::parse_frame(mi.data).unwrap();

            if mi.key == field {
                content = Some(frame);
            }
        }

        // only take content from data packets (ptype empty)
        if ptype.is_empty() {
            if let Some(frame) = content {
                write!(f, "{}", frame)?;
                return Ok(Some(frame.data.len()));
            } else {
                return Ok(Some(0));
            }
        }

        Ok(None)
    }
}

impl fmt::Display for Packet<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut meta = Vec::new();

        if self.fmt_metadata(&mut meta).is_err() {
            return Err(fmt::Error);
        }

        // formatted output is guaranteed to be utf8
        let meta = String::from_utf8(meta).unwrap();

        let meta = trim(&meta, LOG_METADATA_MAX);

        if self.content_field.is_some() {
            let mut content = Vec::new();

            let clen = match self.fmt_content(&mut content) {
                Ok(clen) => clen,
                Err(_) => return Err(fmt::Error),
            };

            if let Some(clen) = clen {
                // formatted output is guaranteed to be utf8
                let content = String::from_utf8(content).unwrap();

                let content = trim(&content, LOG_CONTENT_MAX);

                return write!(f, "{} {} {}", meta, clen, content);
            }
        }

        write!(f, "{}", meta)
    }
}

fn packet_to_string(data: &[u8]) -> String {
    if data.len() == 0 {
        return String::from("<packet is 0 bytes>");
    }

    if data[0] == b'T' {
        let (frame, _) = match tnetstring::parse_frame(&data[1..]) {
            Ok(frame) => frame,
            Err(e) => return format!("<parse failed: {:?}>", e),
        };

        if frame.ftype != tnetstring::FrameType::Map {
            return format!("<not a map>");
        }

        let p = Packet {
            map_frame: frame,
            content_field: Some("body"),
        };

        p.to_string()
    } else {
        // maybe it's addr-prefixed

        let mut pos = None;

        for i in 0..data.len() {
            if data[i] == b' ' {
                pos = Some(i);
                break;
            }
        }

        if pos.is_none() {
            return String::from("<unexpected format>");
        }

        let pos = pos.unwrap();

        let addr = match str::from_utf8(&data[..pos]) {
            Ok(addr) => addr,
            Err(e) => return format!("<addr parse failed: {:?}>", e),
        };

        let payload = &data[(pos + 1)..];

        if payload.len() == 0 {
            return String::from("<payload is 0 bytes>");
        }

        if payload[0] != b'T' {
            return String::from("<unexpected format>");
        }

        let payload = &data[(pos + 2)..];

        let (frame, _) = match tnetstring::parse_frame(payload) {
            Ok(frame) => frame,
            Err(e) => return format!("<parse failed: {:?}>", e),
        };

        if frame.ftype != tnetstring::FrameType::Map {
            return format!("<not a map>");
        }

        let p = Packet {
            map_frame: frame,
            content_field: Some("body"),
        };

        format!("{} {}", addr, p)
    }
}

struct ClientReqSockets {
    sock: AsyncZmqSocket,
}

struct ClientStreamSockets {
    out: AsyncZmqSocket,
    out_stream: AsyncZmqSocket,
    in_: AsyncZmqSocket,
}

struct ReqPipeEnd {
    sender: channel::Sender<arena::Arc<zmq::Message>>,
    receiver: channel::Receiver<zmq::Message>,
}

struct StreamPipeEnd {
    sender: channel::Sender<arena::Arc<zmq::Message>>,
    receiver_any: channel::Receiver<zmq::Message>,
    receiver_addr: channel::Receiver<(ArrayVec<[u8; 64]>, zmq::Message)>,
}

struct AsyncReqPipeEnd {
    sender: AsyncSender<arena::Arc<zmq::Message>>,
    receiver: AsyncReceiver<zmq::Message>,
}

struct AsyncStreamPipeEnd {
    sender: AsyncSender<arena::Arc<zmq::Message>>,
    receiver_any: AsyncReceiver<zmq::Message>,
    receiver_addr: AsyncReceiver<(ArrayVec<[u8; 64]>, zmq::Message)>,
}

enum ControlRequest {
    Stop,
    SetClientReq(Vec<SpecInfo>),
    SetClientStream(Vec<SpecInfo>, Vec<SpecInfo>, Vec<SpecInfo>),
    AddClientReqHandle(ReqPipeEnd, ArrayString<[u8; 8]>),
    AddClientStreamHandle(StreamPipeEnd, ArrayString<[u8; 8]>),
}

type ControlResponse = Result<(), String>;

struct ReqPipe {
    pe: AsyncReqPipeEnd,
    filter: ArrayString<[u8; 8]>,
    valid: Cell<bool>,
}

struct StreamPipe {
    pe: AsyncStreamPipeEnd,
    filter: ArrayString<[u8; 8]>,
    valid: Cell<bool>,
}

struct RecvWrapperFuture<'a, T> {
    fut: RecvFuture<'a, T>,
    nkey: usize,
}

impl<T> Future for RecvWrapperFuture<'_, T> {
    type Output = (usize, Result<T, mpsc::RecvError>);

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let nkey = self.nkey;
        let fut = unsafe { self.as_mut().map_unchecked_mut(|s| &mut s.fut) };

        match fut.poll(cx) {
            Poll::Ready(result) => match result {
                Ok(value) => Poll::Ready((nkey, Ok(value))),
                Err(mpsc::RecvError) => Poll::Ready((nkey, Err(mpsc::RecvError))),
            },
            Poll::Pending => Poll::Pending,
        }
    }
}

struct RecvScratch<T> {
    tasks: arena::ReusableVec,
    slice_scratch: Vec<usize>,
    _marker: marker::PhantomData<T>,
}

impl<T> RecvScratch<T> {
    fn new(capacity: usize) -> Self {
        Self {
            tasks: arena::ReusableVec::new::<RecvWrapperFuture<T>>(capacity),
            slice_scratch: Vec::with_capacity(capacity),
            _marker: marker::PhantomData,
        }
    }

    fn get<'a>(
        &mut self,
    ) -> (
        arena::ReusableVecHandle<RecvWrapperFuture<'a, T>>,
        &mut Vec<usize>,
    ) {
        (self.tasks.get_as_new(), &mut self.slice_scratch)
    }
}

struct ReqHandles {
    nodes: Slab<list::Node<ReqPipe>>,
    list: list::List,
    recv_scratch: RefCell<RecvScratch<zmq::Message>>,
    need_cleanup: Cell<bool>,
}

impl ReqHandles {
    fn new(capacity: usize) -> Self {
        Self {
            nodes: Slab::with_capacity(capacity),
            list: list::List::default(),
            recv_scratch: RefCell::new(RecvScratch::new(capacity)),
            need_cleanup: Cell::new(false),
        }
    }

    fn len(&self) -> usize {
        self.nodes.len()
    }

    fn add(&mut self, pe: AsyncReqPipeEnd, filter: ArrayString<[u8; 8]>) {
        assert!(self.nodes.len() < self.nodes.capacity());

        let key = self.nodes.insert(list::Node::new(ReqPipe {
            pe,
            filter,
            valid: Cell::new(true),
        }));

        self.list.push_back(&mut self.nodes, key);
    }

    async fn recv(&self) -> zmq::Message {
        let mut scratch = self.recv_scratch.borrow_mut();

        let (mut tasks, slice_scratch) = scratch.get();

        let mut next = self.list.head;

        while let Some(nkey) = next {
            let n = &self.nodes[nkey];
            let p = &n.value;

            if p.valid.get() {
                assert!(tasks.len() < tasks.capacity());

                tasks.push(RecvWrapperFuture {
                    fut: p.pe.receiver.recv(),
                    nkey,
                });
            }

            next = n.next;
        }

        loop {
            match select_slice(&mut tasks, slice_scratch).await {
                (_, (_, Ok(msg))) => return msg,
                (pos, (nkey, Err(mpsc::RecvError))) => {
                    tasks.remove(pos);

                    let p = &self.nodes[nkey].value;
                    p.valid.set(false);

                    self.need_cleanup.set(true);
                }
            }
        }
    }

    async fn send(&self, msg: &arena::Arc<zmq::Message>, ids: &[Id<'_>]) {
        let mut next = self.list.head;

        while let Some(nkey) = next {
            let n = &self.nodes[nkey];
            let p = &n.value;

            let mut do_send = false;

            for id in ids.iter() {
                if id.id.starts_with(p.filter.as_bytes()) {
                    do_send = true;
                    break;
                }
            }

            if p.valid.get() && do_send {
                // blocking send. handle is expected to read as fast as possible
                //   without downstream backpressure
                match p.pe.sender.send(arena::Arc::clone(msg)).await {
                    Ok(_) => {}
                    Err(_) => {
                        p.valid.set(false);

                        self.need_cleanup.set(true);
                    }
                }
            }

            next = n.next;
        }
    }

    fn need_cleanup(&self) -> bool {
        self.need_cleanup.get()
    }

    fn cleanup<F>(&mut self, f: F)
    where
        F: Fn(&ReqPipe),
    {
        let mut next = self.list.head;

        while let Some(nkey) = next {
            let n = &mut self.nodes[nkey];
            let p = &mut n.value;

            next = n.next;

            if !p.valid.get() {
                f(p);

                self.list.remove(&mut self.nodes, nkey);
                self.nodes.remove(nkey);
            }
        }

        self.need_cleanup.set(false);
    }
}

struct StreamHandles {
    nodes: Slab<list::Node<StreamPipe>>,
    list: list::List,
    recv_any_scratch: RefCell<RecvScratch<zmq::Message>>,
    recv_addr_scratch: RefCell<RecvScratch<(ArrayVec<[u8; 64]>, zmq::Message)>>,
    need_cleanup: Cell<bool>,
}

impl StreamHandles {
    fn new(capacity: usize) -> Self {
        Self {
            nodes: Slab::with_capacity(capacity),
            list: list::List::default(),
            recv_any_scratch: RefCell::new(RecvScratch::new(capacity)),
            recv_addr_scratch: RefCell::new(RecvScratch::new(capacity)),
            need_cleanup: Cell::new(false),
        }
    }

    fn len(&self) -> usize {
        self.nodes.len()
    }

    fn add(&mut self, pe: AsyncStreamPipeEnd, filter: ArrayString<[u8; 8]>) {
        assert!(self.nodes.len() < self.nodes.capacity());

        let key = self.nodes.insert(list::Node::new(StreamPipe {
            pe,
            filter,
            valid: Cell::new(true),
        }));

        self.list.push_back(&mut self.nodes, key);
    }

    async fn recv_any(&self) -> zmq::Message {
        let mut scratch = self.recv_any_scratch.borrow_mut();

        let (mut tasks, slice_scratch) = scratch.get();

        let mut next = self.list.head;

        while let Some(nkey) = next {
            let n = &self.nodes[nkey];
            let p = &n.value;

            if p.valid.get() {
                assert!(tasks.len() < tasks.capacity());

                tasks.push(RecvWrapperFuture {
                    fut: p.pe.receiver_any.recv(),
                    nkey,
                });
            }

            next = n.next;
        }

        loop {
            match select_slice(&mut tasks, slice_scratch).await {
                (_, (_, Ok(msg))) => return msg,
                (pos, (nkey, Err(mpsc::RecvError))) => {
                    tasks.remove(pos);

                    let p = &self.nodes[nkey].value;
                    p.valid.set(false);

                    self.need_cleanup.set(true);
                }
            }
        }
    }

    async fn recv_addr(&self) -> (ArrayVec<[u8; 64]>, zmq::Message) {
        let mut scratch = self.recv_addr_scratch.borrow_mut();

        let (mut tasks, slice_scratch) = scratch.get();

        let mut next = self.list.head;

        while let Some(nkey) = next {
            let n = &self.nodes[nkey];
            let p = &n.value;

            if p.valid.get() {
                assert!(tasks.len() < tasks.capacity());

                tasks.push(RecvWrapperFuture {
                    fut: p.pe.receiver_addr.recv(),
                    nkey,
                });
            }

            next = n.next;
        }

        loop {
            match select_slice(&mut tasks, slice_scratch).await {
                (_, (_, Ok(ret))) => return ret,
                (pos, (nkey, Err(mpsc::RecvError))) => {
                    tasks.remove(pos);

                    let p = &self.nodes[nkey].value;
                    p.valid.set(false);

                    self.need_cleanup.set(true);
                }
            }
        }
    }

    async fn send(&self, msg: &arena::Arc<zmq::Message>, ids: &[Id<'_>]) {
        let mut next = self.list.head;

        while let Some(nkey) = next {
            let n = &self.nodes[nkey];
            let p = &n.value;

            let mut do_send = false;

            for id in ids.iter() {
                if id.id.starts_with(p.filter.as_bytes()) {
                    do_send = true;
                    break;
                }
            }

            if p.valid.get() && do_send {
                // blocking send. handle is expected to read as fast as possible
                //   without downstream backpressure
                match p.pe.sender.send(arena::Arc::clone(msg)).await {
                    Ok(_) => {}
                    Err(_) => {
                        p.valid.set(false);

                        self.need_cleanup.set(true);
                    }
                }
            }

            next = n.next;
        }
    }

    fn need_cleanup(&self) -> bool {
        self.need_cleanup.get()
    }

    fn cleanup<F>(&mut self, f: F)
    where
        F: Fn(&StreamPipe),
    {
        let mut next = self.list.head;

        while let Some(nkey) = next {
            let n = &mut self.nodes[nkey];
            let p = &mut n.value;

            next = n.next;

            if !p.valid.get() {
                f(p);

                self.list.remove(&mut self.nodes, nkey);
                self.nodes.remove(nkey);
            }
        }

        self.need_cleanup.set(false);
    }
}

pub struct SocketManager {
    handle_bound: usize,
    thread: Option<thread::JoinHandle<()>>,
    control_pipe: Mutex<(
        channel::Sender<ControlRequest>,
        channel::Receiver<ControlResponse>,
    )>,
}

impl SocketManager {
    // retained_max is the maximum number of received messages that the user
    //   will keep around at any moment. for example, if the user plans to
    //   set up 4 handles on the manager and read 1 message at a time from
    //   each of the handles (i.e. process and drop a message before reading
    //   the next), then the value here should be 4, because there would be
    //   no more than 4 dequeued messages alive at any one time. this number
    //   is needed to help size the internal arena
    pub fn new(
        ctx: Arc<zmq::Context>,
        instance_id: &str,
        retained_max: usize,
        hwm: usize,
        handle_bound: usize,
    ) -> Self {
        let (s1, r1) = channel::channel(1);
        let (s2, r2) = channel::channel(1);

        let instance_id = String::from(instance_id);

        let thread = thread::spawn(move || {
            debug!("manager thread start");

            // 2 control channels, 3 channels per handle, 4 zmq sockets
            let channels = 2 + (HANDLES_MAX * 3);
            let zmqsockets = 4;

            let registrations_max =
                (channels * REGISTRATIONS_PER_CHANNEL) + (zmqsockets * REGISTRATIONS_PER_ZMQSOCKET);

            let reactor = Reactor::new(registrations_max);

            let executor = Executor::new(EXECUTOR_TASKS_MAX);

            executor
                .spawn(Self::run(ctx, s1, r2, instance_id, retained_max, hwm))
                .unwrap();

            executor.run(|timeout| reactor.poll(timeout)).unwrap();

            debug!("manager thread end");
        });

        Self {
            handle_bound,
            thread: Some(thread),
            control_pipe: Mutex::new((s2, r1)),
        }
    }

    pub fn set_client_req_specs(&mut self, specs: &[SpecInfo]) -> Result<(), String> {
        self.control_req(ControlRequest::SetClientReq(specs.to_vec()))
    }

    pub fn set_client_stream_specs(
        &mut self,
        out_specs: &[SpecInfo],
        out_stream_specs: &[SpecInfo],
        in_specs: &[SpecInfo],
    ) -> Result<(), String> {
        self.control_req(ControlRequest::SetClientStream(
            out_specs.to_vec(),
            out_stream_specs.to_vec(),
            in_specs.to_vec(),
        ))
    }

    pub fn client_req_handle(&self, id_prefix: &[u8]) -> ClientReqHandle {
        let (s1, r1) = channel::channel(self.handle_bound);
        let (s2, r2) = channel::channel(self.handle_bound);

        let pe = ReqPipeEnd {
            sender: s1,
            receiver: r2,
        };

        let prefix = ArrayString::from_str(str::from_utf8(id_prefix).unwrap()).unwrap();

        self.control_send(ControlRequest::AddClientReqHandle(pe, prefix));

        ClientReqHandle {
            sender: s2,
            receiver: r1,
        }
    }

    pub fn client_stream_handle(&self, id_prefix: &[u8]) -> ClientStreamHandle {
        let (s1, r1) = channel::channel(self.handle_bound);
        let (s2, r2) = channel::channel(self.handle_bound);
        let (s3, r3) = channel::channel(self.handle_bound);

        let pe = StreamPipeEnd {
            sender: s1,
            receiver_any: r2,
            receiver_addr: r3,
        };

        let prefix = ArrayString::from_str(str::from_utf8(id_prefix).unwrap()).unwrap();

        self.control_send(ControlRequest::AddClientStreamHandle(pe, prefix));

        ClientStreamHandle {
            sender_any: s2,
            sender_addr: s3,
            receiver: r1,
        }
    }

    fn control_send(&self, req: ControlRequest) {
        let pipe = self.control_pipe.lock().unwrap();

        // NOTE: this will block if queue is full
        pipe.0.send(req).unwrap();
    }

    fn control_req(&self, req: ControlRequest) -> Result<(), String> {
        let pipe = self.control_pipe.lock().unwrap();

        // NOTE: this is a blocking exchange
        pipe.0.send(req).unwrap();
        pipe.1.recv().unwrap()
    }

    async fn run(
        ctx: Arc<zmq::Context>,
        control_sender: channel::Sender<ControlResponse>,
        control_receiver: channel::Receiver<ControlRequest>,
        instance_id: String,
        retained_max: usize,
        hwm: usize,
    ) {
        let control_sender = AsyncSender::new(control_sender);
        let control_receiver = AsyncReceiver::new(control_receiver);

        // the messages arena needs to fit the max number of potential incoming messages that
        //   still need to be processed. this is the entire channel queue for every handle, plus
        //   the most number of messages the user might retain, plus 1 extra for the next message
        //   we are preparing to send to the handles
        let arena_size = (HANDLES_MAX * hwm) + retained_max + 1;

        let messages_memory = Arc::new(arena::SyncMemory::new(arena_size));

        let client_req = ClientReqSockets {
            sock: AsyncZmqSocket::new(ZmqSocket::new(&ctx, zmq::DEALER)),
        };

        let client_stream = ClientStreamSockets {
            out: AsyncZmqSocket::new(ZmqSocket::new(&ctx, zmq::PUSH)),
            out_stream: AsyncZmqSocket::new(ZmqSocket::new(&ctx, zmq::ROUTER)),
            in_: AsyncZmqSocket::new(ZmqSocket::new(&ctx, zmq::SUB)),
        };

        client_req
            .sock
            .inner()
            .inner()
            .set_sndhwm(hwm as i32)
            .unwrap();
        client_req
            .sock
            .inner()
            .inner()
            .set_rcvhwm(hwm as i32)
            .unwrap();

        client_stream
            .out
            .inner()
            .inner()
            .set_sndhwm(hwm as i32)
            .unwrap();
        client_stream
            .out_stream
            .inner()
            .inner()
            .set_sndhwm(hwm as i32)
            .unwrap();
        client_stream
            .in_
            .inner()
            .inner()
            .set_rcvhwm(hwm as i32)
            .unwrap();

        client_stream
            .out_stream
            .inner()
            .inner()
            .set_router_mandatory(true)
            .unwrap();

        // a ROUTER socket may still be writable after returning EAGAIN, which
        // could mean that a different peer than the one we tried to write to
        // is writable. there's no way to know when the desired peer will be
        // writable, so we'll keep trying again after a delay
        client_stream
            .out_stream
            .set_retry_timeout(Some(STREAM_OUT_STREAM_DELAY));

        let sub = format!("{} ", instance_id);
        client_stream
            .in_
            .inner()
            .inner()
            .set_subscribe(sub.as_bytes())
            .unwrap();

        let mut req_handles = ReqHandles::new(HANDLES_MAX);
        let mut stream_handles = StreamHandles::new(HANDLES_MAX);

        let mut req_send: Option<ZmqSendToFuture> = None;
        let mut stream_out_send: Option<ZmqSendFuture> = None;
        let mut stream_out_stream_send: Option<ZmqSendToFuture> = None;

        loop {
            let req_handles_recv = if req_send.is_none() {
                Some(req_handles.recv())
            } else {
                None
            };

            let stream_handles_recv_any = if stream_out_send.is_none() {
                Some(stream_handles.recv_any())
            } else {
                None
            };

            let stream_handles_recv_addr = if stream_out_stream_send.is_none() {
                Some(stream_handles.recv_addr())
            } else {
                None
            };

            let result = select_9(
                control_receiver.recv(),
                select_option(req_handles_recv),
                select_option(req_send.as_mut()),
                client_req.sock.recv_routed(),
                select_option(stream_handles_recv_any),
                select_option(stream_out_send.as_mut()),
                select_option(stream_handles_recv_addr),
                select_option(stream_out_stream_send.as_mut()),
                client_stream.in_.recv(),
            )
            .await;

            match result {
                // control_receiver.recv
                Select9::R1(result) => match result {
                    Ok(req) => match req {
                        ControlRequest::Stop => break,
                        ControlRequest::SetClientReq(specs) => {
                            debug!("applying req specs: {:?}", specs);

                            let result = Self::apply_req_specs(&client_req, &specs);

                            control_sender
                                .send(result)
                                .await
                                .expect("failed to send control response");
                        }
                        ControlRequest::SetClientStream(out_specs, out_stream_specs, in_specs) => {
                            debug!(
                                "applying stream specs: {:?} {:?} {:?}",
                                out_specs, out_stream_specs, in_specs
                            );

                            let result = Self::apply_stream_specs(
                                &client_stream,
                                &out_specs,
                                &out_stream_specs,
                                &in_specs,
                            );

                            control_sender
                                .send(result)
                                .await
                                .expect("failed to send control response");
                        }
                        ControlRequest::AddClientReqHandle(pe, filter) => {
                            debug!("adding req handle: filter=[{}]", filter);

                            if req_handles.len() + stream_handles.len() < HANDLES_MAX {
                                req_handles.add(
                                    AsyncReqPipeEnd {
                                        sender: AsyncSender::new(pe.sender),
                                        receiver: AsyncReceiver::new(pe.receiver),
                                    },
                                    filter,
                                );
                            } else {
                                error!("cannot add more than {} handles", HANDLES_MAX);
                            }
                        }
                        ControlRequest::AddClientStreamHandle(pe, filter) => {
                            debug!("adding stream handle: filter=[{}]", filter);

                            if req_handles.len() + stream_handles.len() < HANDLES_MAX {
                                stream_handles.add(
                                    AsyncStreamPipeEnd {
                                        sender: AsyncSender::new(pe.sender),
                                        receiver_any: AsyncReceiver::new(pe.receiver_any),
                                        receiver_addr: AsyncReceiver::new(pe.receiver_addr),
                                    },
                                    filter,
                                );
                            } else {
                                error!("cannot add more than {} handles", HANDLES_MAX);
                            }
                        }
                    },
                    Err(e) => error!("control recv: {}", e),
                },
                // req_handles_recv
                Select9::R2(msg) => {
                    if log_enabled!(log::Level::Trace) {
                        trace!("OUT req {}", packet_to_string(&msg));
                    }

                    let h = MultipartHeader::new();

                    req_send = Some(client_req.sock.send_to(h, msg));
                }
                // req_send
                Select9::R3(result) => {
                    if let Err(e) = result {
                        error!("req zmq send: {}", e);
                    }

                    req_send = None;
                }
                // client_req.sock.recv_routed
                Select9::R4(result) => match result {
                    Ok(msg) => {
                        if log_enabled!(log::Level::Trace) {
                            trace!("IN req {}", packet_to_string(&msg));
                        }

                        Self::handle_req_message(msg, &messages_memory, &mut req_handles).await;
                    }
                    Err(e) => error!("req zmq recv: {}", e),
                },
                // stream_handles_recv_any
                Select9::R5(msg) => {
                    if log_enabled!(log::Level::Trace) {
                        trace!("OUT stream {}", packet_to_string(&msg));
                    }

                    stream_out_send = Some(client_stream.out.send(msg));
                }
                // stream_out_send
                Select9::R6(result) => {
                    if let Err(e) = result {
                        error!("stream zmq send: {}", e);
                    }

                    stream_out_send = None;
                }
                // stream_handles_recv_addr
                Select9::R7((addr, msg)) => {
                    let mut h = MultipartHeader::new();
                    h.push(zmq::Message::from(addr.as_ref()));

                    if log_enabled!(log::Level::Trace) {
                        trace!("OUT stream to {}", packet_to_string(&msg));
                    }

                    stream_out_stream_send = Some(client_stream.out_stream.send_to(h, msg));
                }
                // stream_out_stream_send
                Select9::R8(result) => {
                    if let Err(e) = result {
                        error!("stream zmq send to: {}", e);
                    }

                    stream_out_stream_send = None;
                }
                // client_stream.in_.recv
                Select9::R9(result) => match result {
                    Ok(msg) => {
                        if log_enabled!(log::Level::Trace) {
                            trace!("IN stream {}", packet_to_string(&msg));
                        }

                        Self::handle_stream_message(
                            msg,
                            &messages_memory,
                            &instance_id,
                            &mut stream_handles,
                        )
                        .await;
                    }
                    Err(e) => error!("stream zmq recv: {}", e),
                },
            }

            if req_handles.need_cleanup() {
                req_handles.cleanup(|p| {
                    debug!("req handle disconnected: filter=[{}]", p.filter);
                });
            }

            if stream_handles.need_cleanup() {
                stream_handles.cleanup(|p| {
                    debug!("stream handle disconnected: filter=[{}]", p.filter);
                });
            }
        }
    }

    fn apply_req_specs(client_req: &ClientReqSockets, specs: &[SpecInfo]) -> Result<(), String> {
        if let Err(e) = client_req.sock.inner().apply_specs(&specs) {
            return Err(e.to_string());
        }

        return Ok(());
    }

    fn apply_stream_specs(
        client_stream: &ClientStreamSockets,
        out_specs: &[SpecInfo],
        out_stream_specs: &[SpecInfo],
        in_specs: &[SpecInfo],
    ) -> Result<(), String> {
        if let Err(e) = client_stream.out.inner().apply_specs(&out_specs) {
            return Err(e.to_string());
        }

        if let Err(e) = client_stream
            .out_stream
            .inner()
            .apply_specs(&out_stream_specs)
        {
            return Err(e.to_string());
        }

        if let Err(e) = client_stream.in_.inner().apply_specs(&in_specs) {
            return Err(e.to_string());
        }

        Ok(())
    }

    async fn handle_req_message(
        msg: zmq::Message,
        messages_memory: &Arc<arena::ArcMemory<zmq::Message>>,
        handles: &mut ReqHandles,
    ) {
        let msg = arena::Arc::new(msg, messages_memory).unwrap();

        let mut scratch = ResponseScratch::new();

        let ids = match Response::parse_ids(msg.get(), &mut scratch) {
            Ok(ids) => ids,
            Err(e) => {
                warn!("unable to determine packet id(s): {}", e);
                return;
            }
        };

        handles.send(&msg, ids).await;
    }

    async fn handle_stream_message(
        msg: zmq::Message,
        messages_memory: &Arc<arena::ArcMemory<zmq::Message>>,
        instance_id: &str,
        handles: &mut StreamHandles,
    ) {
        let msg = arena::Arc::new(msg, messages_memory).unwrap();

        let buf = msg.get();

        let mut pos = None;
        for (i, b) in buf.iter().enumerate() {
            if *b == b' ' {
                pos = Some(i);
                break;
            }
        }

        let pos = match pos {
            Some(pos) => pos,
            None => {
                warn!("unable to determine packet address");
                return;
            }
        };

        let addr = &buf[..pos];
        if addr != instance_id.as_bytes() {
            warn!("packet not for us");
            return;
        }

        let buf = &buf[pos + 1..];

        let mut scratch = ResponseScratch::new();

        let ids = match Response::parse_ids(&buf, &mut scratch) {
            Ok(ids) => ids,
            Err(e) => {
                warn!("unable to determine packet id(s): {}", e);
                return;
            }
        };

        handles.send(&msg, ids).await;
    }
}

impl Drop for SocketManager {
    fn drop(&mut self) {
        self.control_send(ControlRequest::Stop);

        let thread = self.thread.take().unwrap();
        thread.join().unwrap();
    }
}

#[derive(Debug)]
pub enum SendError {
    Full(zmq::Message),
    Io(io::Error),
}

pub struct ClientReqHandle {
    sender: channel::Sender<zmq::Message>,
    receiver: channel::Receiver<arena::Arc<zmq::Message>>,
}

impl ClientReqHandle {
    pub fn get_read_registration(&self) -> &event::Registration {
        self.receiver.get_read_registration()
    }

    pub fn get_write_registration(&self) -> &event::Registration {
        self.sender.get_write_registration()
    }

    pub fn recv(&mut self) -> Result<arena::Arc<zmq::Message>, io::Error> {
        match self.receiver.try_recv() {
            Ok(msg) => Ok(msg),
            Err(mpsc::TryRecvError::Empty) => Err(io::Error::from(io::ErrorKind::WouldBlock)),
            Err(mpsc::TryRecvError::Disconnected) => {
                Err(io::Error::from(io::ErrorKind::BrokenPipe))
            }
        }
    }

    pub fn send(&mut self, msg: zmq::Message) -> Result<(), SendError> {
        match self.sender.try_send(msg) {
            Ok(_) => Ok(()),
            Err(mpsc::TrySendError::Full(msg)) => Err(SendError::Full(msg)),
            Err(mpsc::TrySendError::Disconnected(_)) => {
                Err(SendError::Io(io::Error::from(io::ErrorKind::BrokenPipe)))
            }
        }
    }
}

pub struct ClientStreamHandle {
    sender_any: channel::Sender<zmq::Message>,
    sender_addr: channel::Sender<(ArrayVec<[u8; 64]>, zmq::Message)>,
    receiver: channel::Receiver<arena::Arc<zmq::Message>>,
}

impl ClientStreamHandle {
    pub fn get_read_registration(&self) -> &event::Registration {
        self.receiver.get_read_registration()
    }

    pub fn get_write_any_registration(&self) -> &event::Registration {
        self.sender_any.get_write_registration()
    }

    pub fn get_write_addr_registration(&self) -> &event::Registration {
        self.sender_addr.get_write_registration()
    }

    pub fn recv(&mut self) -> Result<arena::Arc<zmq::Message>, io::Error> {
        match self.receiver.try_recv() {
            Ok(msg) => Ok(msg),
            Err(mpsc::TryRecvError::Empty) => Err(io::Error::from(io::ErrorKind::WouldBlock)),
            Err(mpsc::TryRecvError::Disconnected) => {
                Err(io::Error::from(io::ErrorKind::BrokenPipe))
            }
        }
    }

    pub fn send_to_any(&mut self, msg: zmq::Message) -> Result<(), SendError> {
        match self.sender_any.try_send(msg) {
            Ok(_) => Ok(()),
            Err(mpsc::TrySendError::Full(msg)) => Err(SendError::Full(msg)),
            Err(mpsc::TrySendError::Disconnected(_)) => {
                Err(SendError::Io(io::Error::from(io::ErrorKind::BrokenPipe)))
            }
        }
    }

    pub fn send_to_addr(&mut self, addr: &[u8], msg: zmq::Message) -> Result<(), SendError> {
        let mut a = ArrayVec::new();
        if a.try_extend_from_slice(addr).is_err() {
            return Err(SendError::Io(io::Error::from(io::ErrorKind::InvalidInput)));
        }

        match self.sender_addr.try_send((a, msg)) {
            Ok(_) => Ok(()),
            Err(mpsc::TrySendError::Full((_, msg))) => Err(SendError::Full(msg)),
            Err(mpsc::TrySendError::Disconnected(_)) => {
                Err(SendError::Io(io::Error::from(io::ErrorKind::BrokenPipe)))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::event;
    use crate::zhttppacket::ResponsePacket;
    use std::mem;

    fn wait_readable(poller: &mut event::Poller, token: mio::Token) {
        loop {
            poller.poll(None).unwrap();

            for event in poller.iter_events() {
                if event.token() == token && event.is_readable() {
                    return;
                }
            }
        }
    }

    fn wait_writable(poller: &mut event::Poller, token: mio::Token) {
        loop {
            poller.poll(None).unwrap();

            for event in poller.iter_events() {
                if event.token() == token && event.is_writable() {
                    return;
                }
            }
        }
    }

    #[test]
    fn test_send_flow() {
        let zmq_context = Arc::new(zmq::Context::new());

        let mut zsockman = SocketManager::new(Arc::clone(&zmq_context), "test", 1, 1, 1);

        zsockman
            .set_client_stream_specs(
                &vec![SpecInfo {
                    spec: String::from("inproc://flow-test-out"),
                    bind: true,
                    ipc_file_mode: 0,
                }],
                &vec![SpecInfo {
                    spec: String::from("inproc://flow-test-out-stream"),
                    bind: true,
                    ipc_file_mode: 0,
                }],
                &vec![SpecInfo {
                    spec: String::from("inproc://flow-test-in"),
                    bind: true,
                    ipc_file_mode: 0,
                }],
            )
            .unwrap();

        // connect an out-stream receiver. the other sockets we'll leave alone
        let in_stream_sock = zmq_context.socket(zmq::ROUTER).unwrap();
        in_stream_sock
            .set_identity("test-handler".as_bytes())
            .unwrap();
        in_stream_sock.set_rcvhwm(1).unwrap();
        in_stream_sock
            .connect("inproc://flow-test-out-stream")
            .unwrap();

        let mut h = zsockman.client_stream_handle(b"a-");

        let mut poller = event::Poller::new(1024).unwrap();

        poller
            .register_custom(
                h.get_write_addr_registration(),
                mio::Token(1),
                mio::Interest::WRITABLE,
            )
            .unwrap();

        // write four times, which will all succeed eventually. after this
        //   we'll have filled the handle, the manager's temporary variable,
        //   and the HWMs of both the sending and receiving zmq sockets
        for i in 1..=4 {
            loop {
                match h.send_to_addr(
                    "test-handler".as_bytes(),
                    zmq::Message::from(format!("{}", i).into_bytes()),
                ) {
                    Ok(()) => break,
                    Err(SendError::Full(_)) => wait_writable(&mut poller, mio::Token(1)),
                    Err(SendError::Io(e)) => panic!("{:?}", e),
                }
            }
        }

        // once we were able to write a fourth time, this means the manager
        //   has started processing the third message. let's wait a short bit
        //   for the manager to attempt to send the third message to the zmq
        //   socket and fail with EAGAIN
        thread::sleep(Duration::from_millis(10));

        // fifth write will fail. there's no room
        let e = h
            .send_to_addr(
                "test-handler".as_bytes(),
                zmq::Message::from("5".as_bytes()),
            )
            .unwrap_err();

        let msg = match e {
            SendError::Full(msg) => msg,
            _ => panic!("unexpected error"),
        };
        assert_eq!(str::from_utf8(&msg).unwrap(), "5");

        // blocking read from the zmq socket so another message can flow
        let parts = in_stream_sock.recv_multipart(0).unwrap();
        assert_eq!(parts.len(), 3);
        assert!(parts[1].is_empty());
        assert_eq!(parts[2], b"1");

        // fifth write will now succeed, eventually
        loop {
            match h.send_to_addr(
                "test-handler".as_bytes(),
                zmq::Message::from("5".as_bytes()),
            ) {
                Ok(()) => break,
                Err(SendError::Full(_)) => wait_writable(&mut poller, mio::Token(1)),
                Err(SendError::Io(e)) => panic!("{:?}", e),
            }
        }

        // read the rest of the messages
        for i in 2..=5 {
            let parts = in_stream_sock.recv_multipart(0).unwrap();
            assert_eq!(parts.len(), 3);
            assert!(parts[1].is_empty());
            assert_eq!(parts[2], format!("{}", i).as_bytes());
        }
    }

    #[test]
    fn test_req() {
        let zmq_context = Arc::new(zmq::Context::new());

        let mut zsockman = SocketManager::new(Arc::clone(&zmq_context), "test", 1, 100, 100);

        zsockman
            .set_client_req_specs(&vec![SpecInfo {
                spec: String::from("inproc://test-req"),
                bind: true,
                ipc_file_mode: 0,
            }])
            .unwrap();

        let mut h1 = zsockman.client_req_handle(b"a-");
        let mut h2 = zsockman.client_req_handle(b"b-");

        let mut poller = event::Poller::new(1024).unwrap();

        poller
            .register_custom(
                h1.get_read_registration(),
                mio::Token(1),
                mio::Interest::READABLE,
            )
            .unwrap();

        poller
            .register_custom(
                h2.get_read_registration(),
                mio::Token(2),
                mio::Interest::READABLE,
            )
            .unwrap();

        let rep_sock = zmq_context.socket(zmq::REP).unwrap();
        rep_sock.connect("inproc://test-req").unwrap();

        h1.send(zmq::Message::from("hello a".as_bytes())).unwrap();

        let parts = rep_sock.recv_multipart(0).unwrap();
        assert_eq!(parts.len(), 1);
        assert_eq!(parts[0], b"hello a");

        rep_sock
            .send("T26:2:id,3:a-1,4:body,5:world,}".as_bytes(), 0)
            .unwrap();

        let msg;
        loop {
            match h1.recv() {
                Ok(m) => {
                    msg = m;
                    break;
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                    wait_readable(&mut poller, mio::Token(1));
                    continue;
                }
                Err(e) => panic!("recv: {}", e),
            };
        }

        let msg = msg.get();
        let mut scratch = ResponseScratch::new();
        let resp = Response::parse(&msg, &mut scratch).unwrap();

        let rdata = match resp.ptype {
            ResponsePacket::Data(data) => data,
            _ => panic!("expected data packet"),
        };
        assert_eq!(rdata.body, b"world");

        h2.send(zmq::Message::from("hello b".as_bytes())).unwrap();

        let parts = rep_sock.recv_multipart(0).unwrap();
        assert_eq!(parts.len(), 1);
        assert_eq!(parts[0], b"hello b");

        rep_sock
            .send("T26:2:id,3:b-1,4:body,5:world,}".as_bytes(), 0)
            .unwrap();

        let msg;
        loop {
            match h2.recv() {
                Ok(m) => {
                    msg = m;
                    break;
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                    wait_readable(&mut poller, mio::Token(2));
                    continue;
                }
                Err(e) => panic!("recv: {}", e),
            };
        }

        let msg = msg.get();
        let mut scratch = ResponseScratch::new();
        let resp = Response::parse(&msg, &mut scratch).unwrap();

        let rdata = match resp.ptype {
            ResponsePacket::Data(data) => data,
            _ => panic!("expected data packet"),
        };
        assert_eq!(rdata.body, b"world");

        mem::drop(h1);
        mem::drop(h2);
        mem::drop(zsockman);
    }

    #[test]
    fn test_stream() {
        let zmq_context = Arc::new(zmq::Context::new());

        let mut zsockman = SocketManager::new(Arc::clone(&zmq_context), "test", 1, 100, 100);

        zsockman
            .set_client_stream_specs(
                &vec![SpecInfo {
                    spec: String::from("inproc://test-out"),
                    bind: true,
                    ipc_file_mode: 0,
                }],
                &vec![SpecInfo {
                    spec: String::from("inproc://test-out-stream"),
                    bind: true,
                    ipc_file_mode: 0,
                }],
                &vec![SpecInfo {
                    spec: String::from("inproc://test-in"),
                    bind: true,
                    ipc_file_mode: 0,
                }],
            )
            .unwrap();

        let mut h1 = zsockman.client_stream_handle(b"a-");
        let mut h2 = zsockman.client_stream_handle(b"b-");

        let mut poller = event::Poller::new(1024).unwrap();

        poller
            .register_custom(
                h1.get_read_registration(),
                mio::Token(1),
                mio::Interest::READABLE,
            )
            .unwrap();

        poller
            .register_custom(
                h2.get_read_registration(),
                mio::Token(2),
                mio::Interest::READABLE,
            )
            .unwrap();

        let in_sock = zmq_context.socket(zmq::PULL).unwrap();
        in_sock.connect("inproc://test-out").unwrap();

        let in_stream_sock = zmq_context.socket(zmq::ROUTER).unwrap();
        in_stream_sock
            .set_identity("test-handler".as_bytes())
            .unwrap();
        in_stream_sock.connect("inproc://test-out-stream").unwrap();

        let out_sock = zmq_context.socket(zmq::XPUB).unwrap();
        out_sock.connect("inproc://test-in").unwrap();

        // ensure zsockman is subscribed
        let msg = out_sock.recv_msg(0).unwrap();
        assert_eq!(&msg[..], b"\x01test ");

        h1.send_to_any(zmq::Message::from("hello a".as_bytes()))
            .unwrap();

        let parts = in_sock.recv_multipart(0).unwrap();
        assert_eq!(parts.len(), 1);
        assert_eq!(parts[0], b"hello a");

        out_sock
            .send(
                "test T49:4:from,12:test-handler,2:id,3:a-1,4:body,5:world,}".as_bytes(),
                0,
            )
            .unwrap();

        let msg;
        loop {
            match h1.recv() {
                Ok(m) => {
                    msg = m;
                    break;
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                    wait_readable(&mut poller, mio::Token(1));
                    continue;
                }
                Err(e) => panic!("recv: {}", e),
            };
        }

        let msg = msg.get();

        let buf = &msg;

        let mut pos = None;
        for (i, b) in buf.iter().enumerate() {
            if *b == b' ' {
                pos = Some(i);
                break;
            }
        }

        let pos = pos.unwrap();

        let buf = &buf[pos + 1..];

        let mut scratch = ResponseScratch::new();
        let resp = Response::parse(buf, &mut scratch).unwrap();

        let rdata = match resp.ptype {
            ResponsePacket::Data(data) => data,
            _ => panic!("expected data packet"),
        };
        assert_eq!(rdata.body, b"world");

        h2.send_to_any(zmq::Message::from("hello b".as_bytes()))
            .unwrap();

        let parts = in_sock.recv_multipart(0).unwrap();
        assert_eq!(parts.len(), 1);
        assert_eq!(parts[0], b"hello b");

        out_sock
            .send(
                "test T49:4:from,12:test-handler,2:id,3:b-1,4:body,5:world,}".as_bytes(),
                0,
            )
            .unwrap();

        let msg;
        loop {
            match h2.recv() {
                Ok(m) => {
                    msg = m;
                    break;
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                    wait_readable(&mut poller, mio::Token(2));
                    continue;
                }
                Err(e) => panic!("recv: {}", e),
            };
        }

        let msg = msg.get();

        let buf = &msg;

        let mut pos = None;
        for (i, b) in buf.iter().enumerate() {
            if *b == b' ' {
                pos = Some(i);
                break;
            }
        }

        let pos = pos.unwrap();

        let buf = &buf[pos + 1..];

        let mut scratch = ResponseScratch::new();
        let resp = Response::parse(buf, &mut scratch).unwrap();

        let rdata = match resp.ptype {
            ResponsePacket::Data(data) => data,
            _ => panic!("expected data packet"),
        };
        assert_eq!(rdata.body, b"world");

        h1.send_to_addr(
            "test-handler".as_bytes(),
            zmq::Message::from("hello a".as_bytes()),
        )
        .unwrap();
        let parts = in_stream_sock.recv_multipart(0).unwrap();
        assert_eq!(parts.len(), 3);
        assert!(parts[1].is_empty());
        assert_eq!(parts[2], b"hello a");

        h2.send_to_addr(
            "test-handler".as_bytes(),
            zmq::Message::from("hello b".as_bytes()),
        )
        .unwrap();
        let parts = in_stream_sock.recv_multipart(0).unwrap();
        assert_eq!(parts.len(), 3);
        assert!(parts[1].is_empty());
        assert_eq!(parts[2], b"hello b");

        mem::drop(h1);
        mem::drop(h2);
        mem::drop(zsockman);
    }
}
