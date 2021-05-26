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

use crate::arena::recycle_vec;
use crate::channel;
use crate::executor::Executor;
use crate::future::{
    select_from_pair, select_from_slice, AcceptFuture, AsyncReceiver, AsyncSender,
    AsyncTcpListener, WaitWritableFuture,
};
use crate::reactor::Reactor;
use log::{debug, error};
use mio;
use mio::net::{TcpListener, TcpStream};
use std::net::SocketAddr;
use std::sync::mpsc;
use std::thread;

const REACTOR_REGISTRATIONS_MAX: usize = 128;
const EXECUTOR_TASKS_MAX: usize = 1;

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
            let reactor = Reactor::new(REACTOR_REGISTRATIONS_MAX);
            let executor = Executor::new(EXECUTOR_TASKS_MAX);

            executor.spawn(Self::run(r, listeners, senders)).unwrap();

            executor.run(|timeout| reactor.poll(timeout)).unwrap();
        });

        Self {
            thread: Some(thread),
            stop: s,
        }
    }

    async fn run(
        stop: channel::Receiver<()>,
        listeners: Vec<TcpListener>,
        senders: Vec<channel::Sender<(usize, TcpStream, SocketAddr)>>,
    ) {
        let mut stop = AsyncReceiver::new(stop);

        let mut listeners: Vec<AsyncTcpListener> = listeners
            .into_iter()
            .map(|l| AsyncTcpListener::new(l))
            .collect();

        let mut senders: Vec<AsyncSender<(usize, TcpStream, SocketAddr)>> =
            senders.into_iter().map(|s| AsyncSender::new(s)).collect();

        let mut listeners_pos = 0;
        let mut senders_pos = 0;

        let mut sender_tasks_mem: Vec<WaitWritableFuture<(usize, TcpStream, SocketAddr)>> =
            Vec::with_capacity(senders.len());

        let mut listener_tasks_mem: Vec<AcceptFuture> = Vec::with_capacity(listeners.len());

        loop {
            // wait for a sender to become writable

            let mut sender_tasks = recycle_vec(sender_tasks_mem);

            for s in senders.iter_mut() {
                sender_tasks.push(s.wait_writable());
            }

            let result = select_from_pair(stop.recv(), select_from_slice(&mut sender_tasks)).await;

            sender_tasks_mem = recycle_vec(sender_tasks);

            match result {
                (Some(_), None) => break,
                (None, Some(_)) => {}
                _ => unreachable!(),
            }

            // accept a connection

            let mut listener_tasks = recycle_vec(listener_tasks_mem);

            let (b, a) = listeners.split_at_mut(listeners_pos);

            for l in a.iter_mut().chain(b.iter_mut()) {
                listener_tasks.push(l.accept());
            }

            let result;

            loop {
                match select_from_pair(stop.recv(), select_from_slice(&mut listener_tasks)).await {
                    (None, Some((_, Err(e)))) => error!("accept error: {:?}", e),
                    r => {
                        result = r;
                        break;
                    }
                }
            }

            listener_tasks_mem = recycle_vec(listener_tasks);

            let (pos, stream, peer_addr) = match result {
                (Some(_), None) => break,
                (None, Some((pos, Ok((stream, peer_addr))))) => (pos, stream, peer_addr),
                _ => unreachable!(),
            };

            let pos = (listeners_pos + pos) % listeners.len();

            debug!("accepted connection from {}", peer_addr);

            listeners_pos = (pos + 1) % listeners.len();

            // write connection to sender

            let mut pending_sock = Some((pos, stream, peer_addr));

            for _ in 0..senders.len() {
                let sender = &mut senders[senders_pos];

                if !sender.is_writable() {
                    senders_pos = (senders_pos + 1) % senders.len();
                    continue;
                }

                let s = pending_sock.take().unwrap();

                match sender.try_send(s) {
                    Ok(()) => {}
                    Err(mpsc::TrySendError::Full(s)) => pending_sock = Some(s),
                    Err(mpsc::TrySendError::Disconnected(_)) => {
                        // this could happen during shutdown
                        debug!("receiver disconnected");
                    }
                }

                senders_pos = (senders_pos + 1) % senders.len();

                if pending_sock.is_none() {
                    break;
                }
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
    use crate::event;
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
            let l = TcpListener::bind(addr).unwrap();
            addrs.push(l.local_addr().unwrap());
            listeners.push(l);

            let (sender, receiver) = channel::channel(0);
            senders.push(sender);
            receivers.push(receiver);
        }

        let _l = Listener::new(listeners, senders);

        let mut poller = event::Poller::new(1024).unwrap();

        let mut client = std::net::TcpStream::connect(&addrs[0]).unwrap();

        poller
            .register_custom(
                receivers[0].get_read_registration(),
                mio::Token(1),
                mio::Interest::READABLE,
            )
            .unwrap();

        let result = receivers[0].try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Empty);

        loop {
            poller.poll(None).unwrap();

            let mut done = false;
            for event in poller.iter_events() {
                match event.token() {
                    mio::Token(1) => {
                        assert_eq!(event.is_readable(), true);
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
