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

use std::cell::RefCell;
use std::fmt;
use std::fs;
use std::io;
use std::os::unix::fs::PermissionsExt;

const MULTIPART_HEADERS_MAX: usize = 1;

fn trim_prefix<'a>(s: &'a str, prefix: &str) -> Result<&'a str, ()> {
    if s.starts_with(prefix) {
        Ok(&s[prefix.len()..])
    } else {
        Err(())
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
pub enum ZmqSocketError {
    Connect(String, zmq::Error),
    Bind(String, zmq::Error),
    SetMode(String, io::Error),
}

impl ToString for ZmqSocketError {
    fn to_string(&self) -> String {
        match self {
            ZmqSocketError::Connect(spec, e) => format!("connect {}: {}", spec, e),
            ZmqSocketError::Bind(spec, e) => format!("bind {}: {}", spec, e),
            ZmqSocketError::SetMode(spec, e) => format!("set mode {}: {}", spec, e),
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
                        let perms = fs::Permissions::from_mode(spec.ipc_file_mode as u32);
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

pub struct MultipartHeader {
    parts: [Option<zmq::Message>; MULTIPART_HEADERS_MAX],
    len: usize,
}

impl MultipartHeader {
    pub fn new() -> Self {
        Self {
            parts: [None; MULTIPART_HEADERS_MAX],
            len: 0,
        }
    }

    pub fn push(&mut self, msg: zmq::Message) {
        self.parts[self.len] = Some(msg);
        self.len += 1;
    }
}

pub struct ZmqSocket {
    pub sock: zmq::Socket,
    pub events: zmq::PollEvents,

    specs: RefCell<Vec<ActiveSpec>>,
}

impl ZmqSocket {
    pub fn new(ctx: &zmq::Context, socket_type: zmq::SocketType) -> Self {
        Self {
            sock: ctx.socket(socket_type).unwrap(),
            events: zmq::PollEvents::empty(),
            specs: RefCell::new(Vec::new()),
        }
    }

    pub fn update_events(&mut self) {
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

    pub fn send(&mut self, msg: zmq::Message) -> Result<(), zmq::Error> {
        if let Err(e) = self.sock.send(msg, zmq::DONTWAIT) {
            self.update_events();
            return Err(e);
        }

        self.update_events();

        Ok(())
    }

    pub fn send_to(
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

    pub fn recv(&mut self) -> Result<zmq::Message, zmq::Error> {
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

    pub fn recv_routed(&mut self) -> Result<zmq::Message, zmq::Error> {
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
                    unsetup_spec(&self.sock, spec);
                }

                return Err(err);
            }
        }

        for spec in to_remove.iter() {
            unsetup_spec(&self.sock, spec);
        }

        // move current specs aside
        let prev_specs = std::mem::replace(&mut *specs, Vec::new());

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
            unsetup_spec(&self.sock, spec);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_send_after_disconnect() {
        let zmq_context = zmq::Context::new();

        let mut s = ZmqSocket::new(&zmq_context, zmq::REQ);
        s.apply_specs(&[SpecInfo {
            spec: String::from("inproc://send-test"),
            bind: true,
            ipc_file_mode: 0,
        }])
        .unwrap();

        assert_eq!(s.events.contains(zmq::POLLOUT), false);

        let r = ZmqSocket::new(&zmq_context, zmq::REP);
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
}
