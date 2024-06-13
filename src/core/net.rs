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

use log::error;
use mio::net::{TcpListener, TcpStream, UnixListener, UnixStream};
use socket2::Socket;
use std::fmt;
use std::os::unix::io::{FromRawFd, IntoRawFd};
use std::ptr;

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
    Unix(mio::net::SocketAddr),
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
