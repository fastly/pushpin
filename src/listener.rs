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

use crate::channel;
use log::{debug, error};
use mio;
use mio::net::{TcpListener, TcpStream};
use std::io;
use std::net::SocketAddr;
use std::sync::mpsc;
use std::thread;

pub struct Listener {
    thread: Option<thread::JoinHandle<()>>,
    stop: channel::Sender<()>,
}

impl Listener {
    pub fn new(
        listeners: Vec<TcpListener>,
        senders: Vec<channel::Sender<(usize, TcpStream, SocketAddr)>>,
    ) -> Listener {
        let (s, r) = channel::channel(1);

        let thread = thread::spawn(move || {
            Self::run(r, listeners, senders);
        });

        Self {
            thread: Some(thread),
            stop: s,
        }
    }

    fn run(
        stop: channel::Receiver<()>,
        listeners: Vec<TcpListener>,
        senders: Vec<channel::Sender<(usize, TcpStream, SocketAddr)>>,
    ) {
        let poll = mio::Poll::new().unwrap();

        poll.register(
            stop.get_read_registration(),
            mio::Token(0),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        let listener_base = 1;
        let mut listeners_readable = Vec::new();
        let mut listeners_pos = 0;

        for (i, l) in listeners.iter().enumerate() {
            poll.register(
                l,
                mio::Token(listener_base + i),
                mio::Ready::readable(),
                mio::PollOpt::edge(),
            )
            .unwrap();

            listeners_readable.push(true);
        }

        let sender_base = listener_base + listeners.len();
        let mut senders_writable = Vec::new();
        let mut senders_pos = 0;

        for (i, s) in senders.iter().enumerate() {
            poll.register(
                s.get_write_registration(),
                mio::Token(sender_base + i),
                mio::Ready::writable(),
                mio::PollOpt::edge(),
            )
            .unwrap();

            senders_writable.push(s.can_send());
        }

        let mut pending_sock = None;

        let mut events = mio::Events::with_capacity(1024);

        loop {
            let mut can_send = false;

            for &b in senders_writable.iter() {
                if b {
                    can_send = true;
                    break;
                }
            }

            let mut can_accept = false;

            for &b in listeners_readable.iter() {
                if b {
                    can_accept = true;
                    break;
                }
            }

            if can_accept && pending_sock.is_none() && can_send {
                for _ in 0..listeners_readable.len() {
                    if !listeners_readable[listeners_pos] {
                        listeners_pos = (listeners_pos + 1) % listeners_readable.len();
                        continue;
                    }

                    match listeners[listeners_pos].accept() {
                        Ok((stream, peer_addr)) => {
                            debug!("accepted connection from {}", peer_addr);
                            pending_sock = Some((listeners_pos, stream, peer_addr));
                        }
                        Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                            listeners_readable[listeners_pos] = false;
                        }
                        Err(e) => {
                            error!("accept error: {:?}", e);
                        }
                    }

                    listeners_pos = (listeners_pos + 1) % listeners_readable.len();

                    if pending_sock.is_some() {
                        break;
                    }
                }
            }

            if pending_sock.is_some() && can_send {
                for _ in 0..senders_writable.len() {
                    if !senders_writable[senders_pos] {
                        senders_pos = (senders_pos + 1) % senders_writable.len();
                        continue;
                    }

                    let sender = &senders[senders_pos];

                    let sock = pending_sock.take().unwrap();

                    match sender.try_send(sock) {
                        Ok(()) => {
                            senders_writable[senders_pos] = sender.can_send();
                        }
                        Err(mpsc::TrySendError::Full(sock)) => {
                            senders_writable[senders_pos] = sender.can_send();
                            pending_sock = Some(sock);
                        }
                        Err(mpsc::TrySendError::Disconnected(_)) => {
                            // this could happen during shutdown
                            senders_writable[senders_pos] = false;
                            debug!("receiver disconnected");
                        }
                    }

                    senders_pos = (senders_pos + 1) % senders_writable.len();

                    if pending_sock.is_none() {
                        break;
                    }
                }

                if pending_sock.is_none() {
                    // if we successfully sent, loop again from the top instead of polling
                    continue;
                }
            }

            poll.poll(&mut events, None).unwrap();

            let mut done = false;

            for event in events.iter() {
                let t = usize::from(event.token());

                if t == 0 {
                    if stop.try_recv().is_ok() {
                        done = true;
                        break;
                    }
                } else if t < sender_base {
                    let i = t - listener_base;
                    listeners_readable[i] = true
                } else {
                    let i = t - sender_base;
                    senders_writable[i] = senders[i].can_send();
                }
            }

            if done {
                break;
            }
        }
    }
}

impl Drop for Listener {
    fn drop(&mut self) {
        // this should never fail. receiver won't disconnect unless
        //   we tell it to
        self.stop.send(()).unwrap();

        let thread = self.thread.take().unwrap();
        thread.join().unwrap();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::mem;
    use std::sync::mpsc;

    #[test]
    fn test_accept() {
        let mut addrs = Vec::new();
        let mut listeners = Vec::new();
        let mut senders = Vec::new();
        let mut receivers = Vec::new();

        for _ in 0..2 {
            let addr = "127.0.0.1:0".parse().unwrap();
            let l = TcpListener::bind(&addr).unwrap();
            addrs.push(l.local_addr().unwrap());
            listeners.push(l);

            let (sender, receiver) = channel::channel(0);
            senders.push(sender);
            receivers.push(receiver);
        }

        let _l = Listener::new(listeners, senders);

        let poll = mio::Poll::new().unwrap();

        let mut client = std::net::TcpStream::connect(&addrs[0]).unwrap();

        poll.register(
            receivers[0].get_read_registration(),
            mio::Token(0),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        let result = receivers[0].try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Empty);

        loop {
            let mut events = mio::Events::with_capacity(1024);
            poll.poll(&mut events, None).unwrap();

            let mut done = false;
            for event in events {
                match event.token() {
                    mio::Token(0) => {
                        assert_eq!(event.readiness().is_readable(), true);
                        done = true;
                        break;
                    }
                    _ => unreachable!(),
                }
            }

            if done {
                break;
            }
        }

        let (lnum, mut peer_client, _) = receivers[0].try_recv().unwrap();

        assert_eq!(lnum, 0);

        peer_client.write(b"hello").unwrap();
        mem::drop(peer_client);

        let mut buf = Vec::new();
        client.read_to_end(&mut buf).unwrap();
        assert_eq!(&buf, b"hello");
    }
}
