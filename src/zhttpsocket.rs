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

use crate::arena;
use crate::channel;
use crate::list;
use crate::tnetstring;
use crate::varlenarray::{VarLenArray64, VarLenStr8};
use crate::zhttppacket::{Id, Response, ResponseScratch};
use log::{debug, error, log_enabled, trace, warn};
use mio::unix::EventedFd;
use slab::Slab;
use std::convert::TryInto;
use std::fmt;
use std::fs;
use std::io;
use std::os::unix::fs::PermissionsExt;
use std::str;
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant};

const MULTIPART_HEADERS_MAX: usize = 1;
const HANDLES_MAX: usize = 1_024;
const TOKENS_PER_HANDLE: usize = 2;
const STREAM_OUT_STREAM_DELAY: Duration = Duration::from_millis(50);
const LOG_METADATA_MAX: usize = 1_000;
const LOG_CONTENT_MAX: usize = 1_000;

fn trim_prefix<'a>(s: &'a str, prefix: &str) -> Result<&'a str, ()> {
    if s.starts_with(prefix) {
        Ok(&s[prefix.len()..])
    } else {
        Err(())
    }
}

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

#[derive(Clone)]
pub struct SpecInfo {
    pub spec: String,
    pub bind: bool,
    pub ipc_file_mode: usize,
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
pub enum SocketError {
    Connect(String, zmq::Error),
    Bind(String, zmq::Error),
    SetMode(String, io::Error),
}

impl ToString for SocketError {
    fn to_string(&self) -> String {
        match self {
            SocketError::Connect(spec, e) => format!("connect {}: {}", spec, e),
            SocketError::Bind(spec, e) => format!("bind {}: {}", spec, e),
            SocketError::SetMode(spec, e) => format!("set mode {}: {}", spec, e),
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

fn setup_spec(sock: &zmq::Socket, spec: &SpecInfo) -> Result<String, SocketError> {
    if spec.bind {
        match sock.bind(&spec.spec) {
            Ok(_) => {
                let endpoint = sock.get_last_endpoint().unwrap().unwrap();

                if let Ok(path) = trim_prefix(&spec.spec, "ipc://") {
                    if spec.ipc_file_mode > 0 {
                        let perms = fs::Permissions::from_mode(spec.ipc_file_mode as u32);
                        if let Err(e) = fs::set_permissions(path, perms) {
                            // if setting perms fails, undo the bind
                            unbind(sock, &endpoint).unwrap();

                            return Err(SocketError::SetMode(spec.spec.clone(), e));
                        }
                    }
                }

                Ok(endpoint)
            }
            Err(e) => Err(SocketError::Bind(spec.spec.clone(), e)),
        }
    } else {
        match sock.connect(&spec.spec) {
            Ok(_) => Ok(spec.spec.clone()),
            Err(e) => Err(SocketError::Connect(spec.spec.clone(), e)),
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

struct MultipartHeader {
    parts: [Option<zmq::Message>; MULTIPART_HEADERS_MAX],
    len: usize,
}

impl MultipartHeader {
    fn new() -> Self {
        Self {
            parts: [None; MULTIPART_HEADERS_MAX],
            len: 0,
        }
    }

    fn push(&mut self, msg: zmq::Message) {
        self.parts[self.len] = Some(msg);
        self.len += 1;
    }
}

struct Socket {
    pub sock: zmq::Socket,
    pub events: zmq::PollEvents,

    specs: Vec<ActiveSpec>,
}

impl Socket {
    fn new(ctx: &zmq::Context, socket_type: zmq::SocketType) -> Self {
        Self {
            sock: ctx.socket(socket_type).unwrap(),
            events: zmq::PollEvents::empty(),
            specs: Vec::new(),
        }
    }

    fn update_events(&mut self) {
        loop {
            match self.sock.get_events() {
                Ok(events) => {
                    self.events = events;
                    break;
                }
                Err(zmq::Error::EINTR) => continue,
                Err(e) => panic!("get events error: {}", e),
            }
        }
    }

    fn send(&mut self, msg: zmq::Message) -> Result<(), zmq::Error> {
        if let Err(e) = self.sock.send(msg, zmq::DONTWAIT) {
            self.update_events();
            return Err(e);
        }

        self.update_events();

        Ok(())
    }

    fn send_to(
        &mut self,
        header: MultipartHeader,
        content: zmq::Message,
    ) -> Result<(), zmq::Error> {
        let mut header = header;
        for i in 0..header.len {
            let msg = header.parts[i].take().unwrap();
            if let Err(e) = self.sock.send(msg, zmq::SNDMORE | zmq::DONTWAIT) {
                self.update_events();
                return Err(e);
            }
        }

        if let Err(e) = self
            .sock
            .send(zmq::Message::new(), zmq::SNDMORE | zmq::DONTWAIT)
        {
            self.update_events();
            return Err(e);
        }

        self.send(content)
    }

    fn recv(&mut self) -> Result<zmq::Message, zmq::Error> {
        // get the first part
        let msg = match self.sock.recv_msg(zmq::DONTWAIT) {
            Ok(msg) => msg,
            Err(e) => {
                self.update_events();
                return Err(e);
            }
        };

        // eat the rest of the parts
        while self.sock.get_rcvmore().unwrap() {
            self.sock.recv_msg(0).unwrap();
        }

        self.update_events();

        Ok(msg)
    }

    fn recv_routed(&mut self) -> Result<zmq::Message, zmq::Error> {
        loop {
            // eat parts until we reach the separator
            match self.sock.recv_msg(zmq::DONTWAIT) {
                Ok(msg) => {
                    if msg.is_empty() {
                        break;
                    }
                }
                Err(e) => {
                    self.update_events();
                    return Err(e);
                }
            }
        }

        // if we get here, we've read the separator. content parts should follow

        if !self.sock.get_rcvmore().unwrap() {
            return Err(zmq::Error::EINVAL);
        }

        // get the first part of the content
        let msg = match self.sock.recv_msg(0) {
            Ok(msg) => msg,
            Err(e) => {
                self.update_events();
                return Err(e);
            }
        };

        // eat the rest of the parts
        while self.sock.get_rcvmore().unwrap() {
            self.sock.recv_msg(0).unwrap();
        }

        self.update_events();

        Ok(msg)
    }

    fn apply_specs(&mut self, new_specs: &[SpecInfo]) -> Result<(), SocketError> {
        let mut to_remove = Vec::new();
        for cur in self.specs.iter() {
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
            for (ci, cur) in self.specs.iter().enumerate() {
                if new.spec == cur.spec.spec && new.bind == cur.spec.bind {
                    found = Some(ci);
                    break;
                }
            }
            match found {
                Some(ci) => {
                    if new.ipc_file_mode != self.specs[ci].spec.ipc_file_mode {
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
            match setup_spec(&self.sock, spec) {
                Ok(endpoint) => {
                    added.push(ActiveSpec {
                        spec: spec.clone(),
                        endpoint,
                    });
                }
                Err(e) => {
                    // undo previous adds
                    for spec in added.iter().rev() {
                        unsetup_spec(&self.sock, spec);
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
                            let perms = fs::Permissions::from_mode(spec.ipc_file_mode as u32);
                            match fs::set_permissions(path, perms) {
                                Ok(_) => {
                                    prev_perms.push((String::from(path), meta.permissions()));
                                }
                                Err(e) => {
                                    err = Some(SocketError::SetMode(spec.spec.clone(), e));
                                }
                            }
                        }
                        Err(e) => {
                            err = Some(SocketError::SetMode(spec.spec.clone(), e));
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
                    unsetup_spec(&self.sock, spec);
                }

                return Err(err);
            }
        }

        for spec in to_remove.iter() {
            unsetup_spec(&self.sock, spec);
        }

        // move current specs aside
        let prev_specs = std::mem::replace(&mut self.specs, Vec::new());

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

            self.specs.push(s.unwrap());
        }

        Ok(())
    }
}

impl Drop for Socket {
    fn drop(&mut self) {
        for spec in self.specs.iter() {
            unsetup_spec(&self.sock, spec);
        }

        self.specs.clear();
    }
}

struct ClientReqSockets {
    sock: Socket,
}

struct ClientStreamSockets {
    out: Socket,
    out_stream: Socket,
    in_: Socket,
}

struct ReqPipeEnd {
    sender: channel::Sender<arena::Rc<zmq::Message>>,
    receiver: channel::Receiver<zmq::Message>,
}

struct StreamPipeEnd {
    sender: channel::Sender<arena::Rc<zmq::Message>>,
    receiver_any: channel::Receiver<zmq::Message>,
    receiver_addr: channel::Receiver<(VarLenArray64<u8>, zmq::Message)>,
}

enum ControlRequest {
    Stop,
    SetClientReq(Vec<SpecInfo>),
    SetClientStream(Vec<SpecInfo>, Vec<SpecInfo>, Vec<SpecInfo>),
    AddClientReqHandle(ReqPipeEnd, VarLenStr8),
    AddClientStreamHandle(StreamPipeEnd, VarLenStr8),
}

type ControlResponse = Result<(), String>;

struct ReqPipe {
    pe: ReqPipeEnd,
    filter: VarLenStr8,
    valid: bool,
    readable: bool,
}

struct StreamPipe {
    pe: StreamPipeEnd,
    filter: VarLenStr8,
    valid: bool,
    readable_any: bool,
    readable_addr: bool,
}

struct ReqHandles {
    nodes: Slab<list::Node<ReqPipe>>,
    list: list::List,
    cur: Option<usize>,
}

impl ReqHandles {
    fn new(capacity: usize) -> Self {
        Self {
            nodes: Slab::with_capacity(capacity),
            list: list::List::default(),
            cur: None,
        }
    }

    fn len(&self) -> usize {
        self.nodes.len()
    }

    fn add<F>(&mut self, pe: ReqPipeEnd, filter: VarLenStr8, f: F)
    where
        F: Fn(&ReqPipe, usize),
    {
        let key = self.nodes.insert(list::Node::new(ReqPipe {
            pe,
            filter,
            valid: true,
            readable: true,
        }));

        self.list.push_back(&mut self.nodes, key);
        self.cur = self.list.head;

        let n = &self.nodes[key];
        let p = &n.value;

        f(p, key);
    }

    fn cur_next(&mut self) {
        if let Some(nkey) = self.cur {
            let n = &self.nodes[nkey];

            if let Some(nkey) = n.next {
                self.cur = Some(nkey);
            } else {
                // wrap around
                self.cur = self.list.head;
            }
        }
    }

    fn any_readable(&self) -> bool {
        let mut next = self.list.head;

        while let Some(nkey) = next {
            let n = &self.nodes[nkey];
            let p = &n.value;

            if p.readable {
                return true;
            }

            next = n.next;
        }

        false
    }

    fn set_readable(&mut self, key: usize) {
        let n = &mut self.nodes[key];
        let p = &mut n.value;

        p.readable = true;
    }

    fn recv(&mut self) -> Option<zmq::Message> {
        if self.cur.is_none() {
            return None;
        }

        let end = self.cur.unwrap();

        loop {
            let nkey = self.cur.unwrap();
            self.cur_next();

            let n = &mut self.nodes[nkey];
            let p = &mut n.value;

            if p.valid && p.readable {
                match p.pe.receiver.try_recv() {
                    Ok(msg) => {
                        return Some(msg);
                    }
                    Err(mpsc::TryRecvError::Empty) => {
                        p.readable = false;
                    }
                    Err(mpsc::TryRecvError::Disconnected) => {
                        p.valid = false;
                    }
                };
            }

            if self.cur.is_none() || self.cur.unwrap() == end {
                break;
            }
        }

        None
    }

    fn send(&mut self, msg: &arena::Rc<zmq::Message>, ids: &[Id]) {
        let mut next = self.list.head;

        while let Some(nkey) = next {
            let n = &mut self.nodes[nkey];
            let p = &mut n.value;

            let mut do_send = false;

            for id in ids.iter() {
                if id.id.starts_with(p.filter.as_bytes()) {
                    do_send = true;
                    break;
                }
            }

            if p.valid && do_send {
                // blocking send. handle is expected to read as fast as possible
                //   without downstream backpressure
                match p.pe.sender.send(arena::Rc::clone(msg)) {
                    Ok(_) => {}
                    Err(_) => {
                        p.valid = false;
                    }
                }
            }

            next = n.next;
        }
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

            if !p.valid {
                f(p);

                self.list.remove(&mut self.nodes, nkey);
                self.nodes.remove(nkey);
                self.cur = self.list.head;
            }
        }
    }
}

struct StreamHandles {
    nodes: Slab<list::Node<StreamPipe>>,
    list: list::List,
    cur: Option<usize>,
}

impl StreamHandles {
    fn new(capacity: usize) -> Self {
        Self {
            nodes: Slab::with_capacity(capacity),
            list: list::List::default(),
            cur: None,
        }
    }

    fn len(&self) -> usize {
        self.nodes.len()
    }

    fn add<F>(&mut self, pe: StreamPipeEnd, filter: VarLenStr8, f: F)
    where
        F: Fn(&StreamPipe, usize),
    {
        let key = self.nodes.insert(list::Node::new(StreamPipe {
            pe,
            filter,
            valid: true,
            readable_any: true,
            readable_addr: true,
        }));

        self.list.push_back(&mut self.nodes, key);
        self.cur = self.list.head;

        let n = &self.nodes[key];
        let p = &n.value;

        f(p, key);
    }

    fn cur_next(&mut self) {
        if let Some(nkey) = self.cur {
            let n = &self.nodes[nkey];

            if let Some(nkey) = n.next {
                self.cur = Some(nkey);
            } else {
                // wrap around
                self.cur = self.list.head;
            }
        }
    }

    fn any_readable_any(&self) -> bool {
        let mut next = self.list.head;

        while let Some(nkey) = next {
            let n = &self.nodes[nkey];
            let p = &n.value;

            if p.readable_any {
                return true;
            }

            next = n.next;
        }

        false
    }

    fn any_readable_addr(&self) -> bool {
        let mut next = self.list.head;

        while let Some(nkey) = next {
            let n = &self.nodes[nkey];
            let p = &n.value;

            if p.readable_addr {
                return true;
            }

            next = n.next;
        }

        false
    }

    fn set_readable_any(&mut self, key: usize) {
        let n = &mut self.nodes[key];
        let p = &mut n.value;

        p.readable_any = true;
    }

    fn set_readable_addr(&mut self, key: usize) {
        let n = &mut self.nodes[key];
        let p = &mut n.value;

        p.readable_addr = true;
    }

    fn recv_any(&mut self) -> Option<zmq::Message> {
        if self.cur.is_none() {
            return None;
        }

        let end = self.cur.unwrap();

        loop {
            let nkey = self.cur.unwrap();
            self.cur_next();

            let n = &mut self.nodes[nkey];
            let p = &mut n.value;

            if p.valid && p.readable_any {
                match p.pe.receiver_any.try_recv() {
                    Ok(msg) => {
                        return Some(msg);
                    }
                    Err(mpsc::TryRecvError::Empty) => {
                        p.readable_any = false;
                    }
                    Err(mpsc::TryRecvError::Disconnected) => {
                        p.valid = false;
                    }
                };
            }

            if self.cur.is_none() || self.cur.unwrap() == end {
                break;
            }
        }

        None
    }

    fn recv_addr(&mut self) -> Option<(VarLenArray64<u8>, zmq::Message)> {
        if self.cur.is_none() {
            return None;
        }

        let end = self.cur.unwrap();

        loop {
            let nkey = self.cur.unwrap();
            self.cur_next();

            let n = &mut self.nodes[nkey];
            let p = &mut n.value;

            if p.valid && p.readable_addr {
                match p.pe.receiver_addr.try_recv() {
                    Ok(ret) => {
                        return Some(ret);
                    }
                    Err(mpsc::TryRecvError::Empty) => {
                        p.readable_addr = false;
                    }
                    Err(mpsc::TryRecvError::Disconnected) => {
                        p.valid = false;
                    }
                };
            }

            if self.cur.is_none() || self.cur.unwrap() == end {
                break;
            }
        }

        None
    }

    fn send(&mut self, msg: &arena::Rc<zmq::Message>, ids: &[Id]) {
        let mut next = self.list.head;

        while let Some(nkey) = next {
            let n = &mut self.nodes[nkey];
            let p = &mut n.value;

            let mut do_send = false;

            for id in ids.iter() {
                if id.id.starts_with(p.filter.as_bytes()) {
                    do_send = true;
                    break;
                }
            }

            if p.valid && do_send {
                // blocking send. handle is expected to read as fast as possible
                //   without downstream backpressure
                match p.pe.sender.send(arena::Rc::clone(msg)) {
                    Ok(_) => {}
                    Err(_) => {
                        p.valid = false;
                    }
                }
            }

            next = n.next;
        }
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

            if !p.valid {
                f(p);

                self.list.remove(&mut self.nodes, nkey);
                self.nodes.remove(nkey);
                self.cur = self.list.head;
            }
        }
    }
}

pub struct SocketManager {
    handle_bound: usize,
    thread: Option<std::thread::JoinHandle<()>>,
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

        let thread = std::thread::spawn(move || {
            Self::run(ctx, s1, r2, instance_id, retained_max, hwm);
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

        let prefix = str::from_utf8(id_prefix).unwrap().try_into().unwrap();

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

        let prefix = str::from_utf8(id_prefix).unwrap().try_into().unwrap();

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

    fn run(
        ctx: Arc<zmq::Context>,
        control_sender: channel::Sender<ControlResponse>,
        control_receiver: channel::Receiver<ControlRequest>,
        instance_id: String,
        retained_max: usize,
        hwm: usize,
    ) {
        debug!("manager thread start");

        // the messages arena needs to fit the max number of potential incoming messages that
        //   still need to be processed. this is the entire channel queue for every handle, plus
        //   the most number of messages the user might retain, plus 1 extra for the next message
        //   we are preparing to send to the handles
        let arena_size = (HANDLES_MAX * hwm) + retained_max + 1;

        let messages_memory = Arc::new(arena::Memory::new(arena_size));

        let mut client_req = ClientReqSockets {
            sock: Socket::new(&ctx, zmq::DEALER),
        };

        let mut client_stream = ClientStreamSockets {
            out: Socket::new(&ctx, zmq::PUSH),
            out_stream: Socket::new(&ctx, zmq::ROUTER),
            in_: Socket::new(&ctx, zmq::SUB),
        };

        client_req.sock.sock.set_sndhwm(hwm as i32).unwrap();
        client_req.sock.sock.set_rcvhwm(hwm as i32).unwrap();

        client_stream.out.sock.set_sndhwm(hwm as i32).unwrap();
        client_stream
            .out_stream
            .sock
            .set_sndhwm(hwm as i32)
            .unwrap();
        client_stream.in_.sock.set_rcvhwm(hwm as i32).unwrap();

        client_stream
            .out_stream
            .sock
            .set_router_mandatory(true)
            .unwrap();

        let sub = format!("{} ", instance_id);
        client_stream
            .in_
            .sock
            .set_subscribe(sub.as_bytes())
            .unwrap();

        const CONTROL: mio::Token = mio::Token(0);
        const CLIENT_REQ: mio::Token = mio::Token(1);
        const CLIENT_STREAM_OUT: mio::Token = mio::Token(2);
        const CLIENT_STREAM_OUT_STREAM: mio::Token = mio::Token(3);
        const CLIENT_STREAM_IN: mio::Token = mio::Token(4);

        const REQ_HANDLE_BASE: usize = 10;
        const STREAM_HANDLE_BASE: usize = REQ_HANDLE_BASE + (HANDLES_MAX * TOKENS_PER_HANDLE);

        let poll = mio::Poll::new().unwrap();

        poll.register(
            control_receiver.get_read_registration(),
            CONTROL,
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            &EventedFd(&client_req.sock.sock.get_fd().unwrap()),
            CLIENT_REQ,
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            &EventedFd(&client_stream.out.sock.get_fd().unwrap()),
            CLIENT_STREAM_OUT,
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            &EventedFd(&client_stream.out_stream.sock.get_fd().unwrap()),
            CLIENT_STREAM_OUT_STREAM,
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            &EventedFd(&client_stream.in_.sock.get_fd().unwrap()),
            CLIENT_STREAM_IN,
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        let mut control_readable = true;

        let mut req_handles = ReqHandles::new(HANDLES_MAX);
        let mut stream_handles = StreamHandles::new(HANDLES_MAX);

        let mut req_pending = None;
        let mut stream_out_pending = None;
        let mut stream_out_stream_pending = None;
        let mut stream_out_stream_delay = None;

        let mut events = mio::Events::with_capacity(1024);

        loop {
            if control_readable {
                match control_receiver.try_recv() {
                    Ok(req) => match req {
                        ControlRequest::Stop => {
                            break;
                        }
                        ControlRequest::SetClientReq(specs) => {
                            debug!("applying req specs: {:?}", specs);

                            if let Err(e) = client_req.sock.apply_specs(&specs) {
                                control_sender.send(Err(e.to_string())).unwrap();
                                continue;
                            }

                            control_sender.send(Ok(())).unwrap();
                        }
                        ControlRequest::SetClientStream(out_specs, out_stream_specs, in_specs) => {
                            debug!(
                                "applying stream specs: {:?} {:?} {:?}",
                                out_specs, out_stream_specs, in_specs
                            );

                            if let Err(e) = client_stream.out.apply_specs(&out_specs) {
                                control_sender.send(Err(e.to_string())).unwrap();
                                continue;
                            }

                            if let Err(e) = client_stream.out_stream.apply_specs(&out_stream_specs)
                            {
                                control_sender.send(Err(e.to_string())).unwrap();
                                continue;
                            }

                            if let Err(e) = client_stream.in_.apply_specs(&in_specs) {
                                control_sender.send(Err(e.to_string())).unwrap();
                                continue;
                            }

                            control_sender.send(Ok(())).unwrap();
                        }
                        ControlRequest::AddClientReqHandle(pe, filter) => {
                            debug!("adding req handle: filter=[{}]", filter);

                            if req_handles.len() + stream_handles.len() < HANDLES_MAX {
                                req_handles.add(pe, filter, |p, key| {
                                    poll.register(
                                        p.pe.receiver.get_read_registration(),
                                        mio::Token(REQ_HANDLE_BASE + (key * TOKENS_PER_HANDLE) + 0),
                                        mio::Ready::readable(),
                                        mio::PollOpt::edge(),
                                    )
                                    .unwrap();
                                });
                            } else {
                                error!("cannot add more than {} handles", HANDLES_MAX);
                            }
                        }
                        ControlRequest::AddClientStreamHandle(pe, filter) => {
                            debug!("adding stream handle: filter=[{}]", filter);

                            if req_handles.len() + stream_handles.len() < HANDLES_MAX {
                                stream_handles.add(pe, filter, |p, key| {
                                    poll.register(
                                        p.pe.receiver_any.get_read_registration(),
                                        mio::Token(
                                            STREAM_HANDLE_BASE + (key * TOKENS_PER_HANDLE) + 0,
                                        ),
                                        mio::Ready::readable(),
                                        mio::PollOpt::edge(),
                                    )
                                    .unwrap();

                                    poll.register(
                                        p.pe.receiver_addr.get_read_registration(),
                                        mio::Token(
                                            STREAM_HANDLE_BASE + (key * TOKENS_PER_HANDLE) + 1,
                                        ),
                                        mio::Ready::readable(),
                                        mio::PollOpt::edge(),
                                    )
                                    .unwrap();
                                });
                            } else {
                                error!("cannot add more than {} handles", HANDLES_MAX);
                            }
                        }
                    },
                    Err(mpsc::TryRecvError::Empty) => {
                        control_readable = false;
                    }
                    Err(e) => {
                        error!("control recv: {}", e);
                    }
                }
            }

            if req_pending.is_none() {
                req_pending = req_handles.recv();

                req_handles.cleanup(|p| {
                    debug!("req handle disconnected: filter=[{}]", p.filter);
                    poll.deregister(p.pe.receiver.get_read_registration())
                        .unwrap();
                });
            }

            if let Some(msg) = &req_pending {
                if client_req.sock.events.contains(zmq::POLLOUT) {
                    let h = MultipartHeader::new();

                    if log_enabled!(log::Level::Trace) {
                        trace!("OUT req {}", packet_to_string(&msg));
                    }

                    // NOTE: when rust-zmq allows resending messages we can
                    //   avoid this copy
                    let tmp = zmq::Message::from(&msg[..]);

                    let mut retry = false;

                    match client_req.sock.send_to(h, tmp) {
                        Ok(_) => {}
                        Err(zmq::Error::EAGAIN) => retry = true,
                        Err(e) => error!("req zmq send: {}", e),
                    }

                    if !retry {
                        req_pending = None;
                    }
                }
            }

            if stream_out_pending.is_none() {
                stream_out_pending = stream_handles.recv_any();

                stream_handles.cleanup(|p| {
                    debug!("stream handle disconnected: filter=[{}]", p.filter);
                    poll.deregister(p.pe.receiver_any.get_read_registration())
                        .unwrap();
                    poll.deregister(p.pe.receiver_addr.get_read_registration())
                        .unwrap();
                });
            }

            if let Some(msg) = &stream_out_pending {
                if client_stream.out.events.contains(zmq::POLLOUT) {
                    if log_enabled!(log::Level::Trace) {
                        trace!("OUT stream {}", packet_to_string(&msg));
                    }

                    // NOTE: when rust-zmq allows resending messages we can
                    //   avoid this copy
                    let tmp = zmq::Message::from(&msg[..]);

                    let mut retry = false;

                    match client_stream.out.send(tmp) {
                        Ok(_) => {}
                        Err(zmq::Error::EAGAIN) => retry = true,
                        Err(e) => error!("stream zmq send: {}", e),
                    }

                    if !retry {
                        stream_out_pending = None;
                    }
                }
            }

            if stream_out_stream_pending.is_none() {
                stream_out_stream_pending = stream_handles.recv_addr();

                stream_handles.cleanup(|p| {
                    debug!("stream handle disconnected: filter=[{}]", p.filter);
                    poll.deregister(p.pe.receiver_any.get_read_registration())
                        .unwrap();
                    poll.deregister(p.pe.receiver_addr.get_read_registration())
                        .unwrap();
                });
            }

            if let Some((addr, msg)) = &stream_out_stream_pending {
                let now = Instant::now();

                let wait = if let Some(t) = stream_out_stream_delay {
                    if now < t {
                        true
                    } else {
                        stream_out_stream_delay = None;

                        false
                    }
                } else {
                    false
                };

                if !wait && client_stream.out_stream.events.contains(zmq::POLLOUT) {
                    let mut h = MultipartHeader::new();
                    h.push(zmq::Message::from(addr.as_ref()));

                    if log_enabled!(log::Level::Trace) {
                        trace!("OUT stream to {}", packet_to_string(&msg));
                    }

                    // NOTE: when rust-zmq allows resending messages we can
                    //   avoid this copy
                    let tmp = zmq::Message::from(&msg[..]);

                    let mut retry = false;

                    match client_stream.out_stream.send_to(h, tmp) {
                        Ok(_) => {}
                        Err(zmq::Error::EAGAIN) => retry = true,
                        Err(e) => error!("stream zmq send: {}", e),
                    }

                    if retry {
                        if client_stream.out_stream.events.contains(zmq::POLLOUT) {
                            // if the socket is still writable after EAGAIN,
                            //   it could mean that a different peer than the
                            //   one we tried to write to is writable. in that
                            //   case, there's no way to know when the desired
                            //   peer will be writable, so we'll just try again
                            //   after a delay
                            stream_out_stream_delay = Some(now + STREAM_OUT_STREAM_DELAY);
                        }
                    } else {
                        stream_out_stream_pending = None;
                    }
                }
            }

            if client_req.sock.events.contains(zmq::POLLIN) {
                match client_req.sock.recv_routed() {
                    Ok(msg) => {
                        if log_enabled!(log::Level::Trace) {
                            trace!("IN req {}", packet_to_string(&msg));
                        }

                        Self::handle_req_message(msg, &messages_memory, &mut req_handles);

                        req_handles.cleanup(|p| {
                            debug!("req handle disconnected: filter=[{}]", p.filter);
                            poll.deregister(p.pe.receiver.get_read_registration())
                                .unwrap();
                        });
                    }
                    Err(zmq::Error::EAGAIN) => {
                        // nothing to do here. socket events should have been updated
                    }
                    Err(e) => {
                        error!("req zmq recv: {}", e);
                    }
                }
            }

            if client_stream.in_.events.contains(zmq::POLLIN) {
                match client_stream.in_.recv() {
                    Ok(msg) => {
                        if log_enabled!(log::Level::Trace) {
                            trace!("IN stream {}", packet_to_string(&msg));
                        }

                        Self::handle_stream_message(
                            msg,
                            &messages_memory,
                            &instance_id,
                            &mut stream_handles,
                        );

                        stream_handles.cleanup(|p| {
                            debug!("stream handle disconnected: filter=[{}]", p.filter);
                            poll.deregister(p.pe.receiver_any.get_read_registration())
                                .unwrap();
                            poll.deregister(p.pe.receiver_addr.get_read_registration())
                                .unwrap();
                        });
                    }
                    Err(zmq::Error::EAGAIN) => {
                        // nothing to do here. socket events should have been updated
                    }
                    Err(e) => {
                        error!("stream zmq recv: {}", e);
                    }
                }
            }

            let now = Instant::now();

            let timeout = if control_readable
                || (req_handles.any_readable() && req_pending.is_none())
                || (req_pending.is_some() && client_req.sock.events.contains(zmq::POLLOUT))
                || (stream_handles.any_readable_any() && stream_out_pending.is_none())
                || (stream_out_pending.is_some() && client_stream.out.events.contains(zmq::POLLOUT))
                || (stream_handles.any_readable_addr() && stream_out_stream_pending.is_none())
                || (stream_out_stream_pending.is_some()
                    && client_stream.out_stream.events.contains(zmq::POLLOUT)
                    && (stream_out_stream_delay.is_none()
                        || now >= stream_out_stream_delay.unwrap()))
                || client_req.sock.events.contains(zmq::POLLIN)
                || client_stream.in_.events.contains(zmq::POLLIN)
            {
                // if there are other things we could do, poll without waiting
                Some(std::time::Duration::from_millis(0))
            } else {
                // otherwise wait for something

                let mut timeout = None;

                if stream_out_stream_pending.is_some() {
                    if let Some(t) = stream_out_stream_delay {
                        if now >= t {
                            timeout = Some(std::time::Duration::from_millis(0));
                        } else {
                            timeout = Some(t - now);
                        }
                    }
                }

                timeout
            };

            poll.poll(&mut events, timeout).unwrap();

            for event in events.iter() {
                match event.token() {
                    CONTROL => {
                        control_readable = true;
                    }
                    CLIENT_REQ => {
                        client_req.sock.update_events();
                    }
                    CLIENT_STREAM_OUT => {
                        client_stream.out.update_events();
                    }
                    CLIENT_STREAM_OUT_STREAM => {
                        client_stream.out_stream.update_events();
                    }
                    CLIENT_STREAM_IN => {
                        client_stream.in_.update_events();
                    }
                    token => {
                        let token = usize::from(token);

                        if token >= REQ_HANDLE_BASE && token < STREAM_HANDLE_BASE {
                            let key = (token - REQ_HANDLE_BASE) / TOKENS_PER_HANDLE;

                            if event.readiness().is_readable() {
                                req_handles.set_readable(key);
                            }
                        } else if token >= STREAM_HANDLE_BASE {
                            let key = (token - STREAM_HANDLE_BASE) / TOKENS_PER_HANDLE;
                            let offset = (token - STREAM_HANDLE_BASE) % TOKENS_PER_HANDLE;

                            if event.readiness().is_readable() {
                                if offset == 0 {
                                    stream_handles.set_readable_any(key);
                                } else if offset == 1 {
                                    stream_handles.set_readable_addr(key);
                                }
                            }
                        }
                    }
                }
            }
        }

        debug!("manager thread end");
    }

    fn handle_req_message(
        msg: zmq::Message,
        messages_memory: &Arc<arena::RcMemory<zmq::Message>>,
        handles: &mut ReqHandles,
    ) {
        let msg = arena::Rc::new(msg, messages_memory).unwrap();

        let mut scratch = ResponseScratch::new();

        let ids = match Response::parse_ids(msg.get(), &mut scratch) {
            Ok(ids) => ids,
            Err(e) => {
                warn!("unable to determine packet id(s): {}", e);
                return;
            }
        };

        handles.send(&msg, ids);
    }

    fn handle_stream_message(
        msg: zmq::Message,
        messages_memory: &Arc<arena::RcMemory<zmq::Message>>,
        instance_id: &str,
        handles: &mut StreamHandles,
    ) {
        let msg = arena::Rc::new(msg, &messages_memory).unwrap();

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

        handles.send(&msg, ids);
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
    receiver: channel::Receiver<arena::Rc<zmq::Message>>,
}

impl ClientReqHandle {
    pub fn get_read_registration(&self) -> &mio::Registration {
        self.receiver.get_read_registration()
    }

    pub fn get_write_registration(&self) -> &mio::Registration {
        self.sender.get_write_registration()
    }

    pub fn recv(&mut self) -> Result<arena::Rc<zmq::Message>, io::Error> {
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
    sender_addr: channel::Sender<(VarLenArray64<u8>, zmq::Message)>,
    receiver: channel::Receiver<arena::Rc<zmq::Message>>,
}

impl ClientStreamHandle {
    pub fn get_read_registration(&self) -> &mio::Registration {
        self.receiver.get_read_registration()
    }

    pub fn get_write_any_registration(&self) -> &mio::Registration {
        self.sender_any.get_write_registration()
    }

    pub fn get_write_addr_registration(&self) -> &mio::Registration {
        self.sender_addr.get_write_registration()
    }

    pub fn recv(&mut self) -> Result<arena::Rc<zmq::Message>, io::Error> {
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
        let addr = match addr.try_into() {
            Ok(addr) => addr,
            Err(_) => return Err(SendError::Io(io::Error::from(io::ErrorKind::InvalidInput))),
        };

        match self.sender_addr.try_send((addr, msg)) {
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
    use crate::zhttppacket::ResponsePacket;
    use std::mem;
    use std::thread;

    fn wait_readable(poll: &mio::Poll, token: mio::Token) {
        loop {
            let mut events = mio::Events::with_capacity(1024);
            poll.poll(&mut events, None).unwrap();

            for event in events {
                if event.token() == token && event.readiness().is_readable() {
                    return;
                }
            }
        }
    }

    fn wait_writable(poll: &mio::Poll, token: mio::Token) {
        loop {
            let mut events = mio::Events::with_capacity(1024);
            poll.poll(&mut events, None).unwrap();

            for event in events {
                if event.token() == token && event.readiness().is_writable() {
                    return;
                }
            }
        }
    }

    #[test]
    fn test_send_after_disconnect() {
        let zmq_context = Arc::new(zmq::Context::new());

        let mut s = Socket::new(&zmq_context, zmq::REQ);
        s.apply_specs(&[SpecInfo {
            spec: String::from("inproc://send-test"),
            bind: true,
            ipc_file_mode: 0,
        }])
        .unwrap();

        assert_eq!(s.events.contains(zmq::POLLOUT), false);

        let mut r = Socket::new(&zmq_context, zmq::REP);
        r.apply_specs(&[SpecInfo {
            spec: String::from("inproc://send-test"),
            bind: false,
            ipc_file_mode: 0,
        }])
        .unwrap();

        s.update_events();

        assert_eq!(s.events.contains(zmq::POLLOUT), true);

        drop(r);

        assert_eq!(s.send((&b"test"[..]).into()), Err(zmq::Error::EAGAIN));

        assert_eq!(s.events.contains(zmq::POLLOUT), false);
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

        let mut h = zsockman.client_stream_handle(b"a-");

        let poll = mio::Poll::new().unwrap();

        poll.register(
            h.get_write_addr_registration(),
            mio::Token(0),
            mio::Ready::writable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        // first write will always succeed
        h.send_to_addr(
            "test-handler".as_bytes(),
            zmq::Message::from("1".as_bytes()),
        )
        .unwrap();

        // second write will always succeed eventually. we may need to wait
        //   for the manager to read the first message from the handle into
        //   a temporary variable
        loop {
            match h.send_to_addr(
                "test-handler".as_bytes(),
                zmq::Message::from("2".as_bytes()),
            ) {
                Ok(()) => break,
                Err(SendError::Full(_)) => wait_writable(&poll, mio::Token(0)),
                Err(SendError::Io(e)) => panic!("{:?}", e),
            }
        }

        // once we were able to write a second time, this means the manager
        //   has started processing the first message. let's wait a short bit
        //   for the manager to attempt to send the first message to the zmq
        //   socket and fail
        thread::sleep(Duration::from_millis(10));

        // third write will fail. there's no room
        let e = h
            .send_to_addr(
                "test-handler".as_bytes(),
                zmq::Message::from("3".as_bytes()),
            )
            .unwrap_err();

        let msg = match e {
            SendError::Full(msg) => msg,
            _ => panic!("unexpected error"),
        };
        assert_eq!(str::from_utf8(&msg).unwrap(), "3");

        // connect to the manager's zmq socket so at least one message can flow
        let in_stream_sock = zmq_context.socket(zmq::ROUTER).unwrap();
        in_stream_sock
            .set_identity("test-handler".as_bytes())
            .unwrap();
        in_stream_sock.set_rcvhwm(1).unwrap();
        in_stream_sock
            .connect("inproc://flow-test-out-stream")
            .unwrap();

        // blocking read for the message
        let parts = in_stream_sock.recv_multipart(0).unwrap();
        assert_eq!(parts.len(), 3);
        assert!(parts[1].is_empty());
        assert_eq!(parts[2], b"1");

        // third write will succeed now
        h.send_to_addr(
            "test-handler".as_bytes(),
            zmq::Message::from("3".as_bytes()),
        )
        .unwrap();
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

        let poll = mio::Poll::new().unwrap();

        poll.register(
            h1.get_read_registration(),
            mio::Token(0),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            h2.get_read_registration(),
            mio::Token(1),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
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
                    wait_readable(&poll, mio::Token(0));
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
                    wait_readable(&poll, mio::Token(1));
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

        let poll = mio::Poll::new().unwrap();

        poll.register(
            h1.get_read_registration(),
            mio::Token(0),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            h2.get_read_registration(),
            mio::Token(1),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        let in_sock = zmq_context.socket(zmq::PULL).unwrap();
        in_sock.connect("inproc://test-out").unwrap();

        let in_stream_sock = zmq_context.socket(zmq::ROUTER).unwrap();
        in_stream_sock
            .set_identity("test-handler".as_bytes())
            .unwrap();
        in_stream_sock.connect("inproc://test-out-stream").unwrap();

        let out_sock = zmq_context.socket(zmq::PUB).unwrap();
        out_sock.connect("inproc://test-in").unwrap();

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
                    wait_readable(&poll, mio::Token(0));
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
                    wait_readable(&poll, mio::Token(1));
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
