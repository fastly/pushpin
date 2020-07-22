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

use mio;
use std::mem;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::Arc;

pub struct Sender<T> {
    sender: Option<mpsc::SyncSender<T>>,
    read_set_readiness: mio::SetReadiness,
    write_registration: mio::Registration,
    cts: Option<Arc<AtomicBool>>,
}

impl<T> Sender<T> {
    // NOTE: only makes sense for rendezvous channels
    pub fn can_send(&self) -> bool {
        match &self.cts {
            Some(cts) => cts.load(Ordering::Relaxed),
            None => true,
        }
    }

    pub fn get_write_registration(&self) -> &mio::Registration {
        &self.write_registration
    }

    pub fn try_send(&self, t: T) -> Result<(), mpsc::TrySendError<T>> {
        if let Some(cts) = &self.cts {
            let ret = cts.compare_and_swap(true, false, Ordering::Relaxed);

            if !ret {
                return Err(mpsc::TrySendError::Full(t));
            }

            // cts will only be true if a read was performed while the queue
            // was empty, and this function is the only place where the queue
            // is written to. this means the try_send call below will only
            // fail if the receiver disconnected
        }

        match self.sender.as_ref().unwrap().try_send(t) {
            Ok(_) => {
                self.read_set_readiness
                    .set_readiness(mio::Ready::readable())
                    .unwrap();

                Ok(())
            }
            Err(e) => Err(e),
        }
    }

    pub fn send(&self, t: T) -> Result<(), mpsc::SendError<T>> {
        if self.cts.is_some() {
            panic!("blocking send with rendezvous channel not supported")
        }

        match self.sender.as_ref().unwrap().send(t) {
            Ok(_) => {
                self.read_set_readiness
                    .set_readiness(mio::Ready::readable())
                    .unwrap();

                Ok(())
            }
            Err(e) => Err(e),
        }
    }
}

impl<T> Drop for Sender<T> {
    fn drop(&mut self) {
        mem::drop(self.sender.take().unwrap());

        self.read_set_readiness
            .set_readiness(mio::Ready::readable())
            .unwrap();
    }
}

pub struct Receiver<T> {
    receiver: mpsc::Receiver<T>,
    read_registration: mio::Registration,
    write_set_readiness: mio::SetReadiness,
    cts: Option<Arc<AtomicBool>>,
}

impl<T> Receiver<T> {
    pub fn get_read_registration(&self) -> &mio::Registration {
        &self.read_registration
    }

    pub fn try_recv(&self) -> Result<T, mpsc::TryRecvError> {
        match self.receiver.try_recv() {
            Ok(t) => {
                if self.cts.is_none() {
                    self.write_set_readiness
                        .set_readiness(mio::Ready::writable())
                        .unwrap();
                }

                Ok(t)
            }
            Err(mpsc::TryRecvError::Empty) if self.cts.is_some() => {
                let cts = self.cts.as_ref().unwrap();

                let ret = cts.compare_and_swap(false, true, Ordering::Relaxed);

                if !ret {
                    self.write_set_readiness
                        .set_readiness(mio::Ready::writable())
                        .unwrap();
                }

                Err(mpsc::TryRecvError::Empty)
            }
            Err(e) => Err(e),
        }
    }

    pub fn recv(&self) -> Result<T, mpsc::RecvError> {
        let t = self.receiver.recv()?;

        if self.cts.is_none() {
            self.write_set_readiness
                .set_readiness(mio::Ready::writable())
                .unwrap();
        }

        Ok(t)
    }
}

pub fn channel<T>(bound: usize) -> (Sender<T>, Receiver<T>) {
    let (read_reg, read_sr) = mio::Registration::new2();
    let (write_reg, write_sr) = mio::Registration::new2();

    // rendezvous channel
    if bound == 0 {
        let (s, r) = mpsc::sync_channel::<T>(1);

        let cts = Arc::new(AtomicBool::new(false));

        let sender = Sender {
            sender: Some(s),
            read_set_readiness: read_sr,
            write_registration: write_reg,
            cts: Some(Arc::clone(&cts)),
        };

        let receiver = Receiver {
            receiver: r,
            read_registration: read_reg,
            write_set_readiness: write_sr,
            cts: Some(Arc::clone(&cts)),
        };

        (sender, receiver)
    } else {
        let (s, r) = mpsc::sync_channel::<T>(bound);

        let sender = Sender {
            sender: Some(s),
            read_set_readiness: read_sr,
            write_registration: write_reg,
            cts: None,
        };

        let receiver = Receiver {
            receiver: r,
            read_registration: read_reg,
            write_set_readiness: write_sr,
            cts: None,
        };

        // channel is immediately writable
        receiver
            .write_set_readiness
            .set_readiness(mio::Ready::writable())
            .unwrap();

        (sender, receiver)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time;

    #[test]
    fn test_send_recv_0() {
        let (sender, receiver) = channel(0);

        assert_eq!(sender.can_send(), false);

        let result = sender.try_send(42);
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TrySendError::Full(42));

        let result = receiver.try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Empty);

        assert_eq!(sender.can_send(), true);

        let result = sender.try_send(42);
        assert_eq!(result.is_ok(), true);
        assert_eq!(sender.can_send(), false);

        let result = receiver.try_recv();
        assert_eq!(result.is_ok(), true);

        let v = result.unwrap();
        assert_eq!(v, 42);

        let result = receiver.try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Empty);

        mem::drop(sender);

        let result = receiver.try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Disconnected);
    }

    #[test]
    fn test_send_recv_1() {
        let (sender, receiver) = channel(1);

        let result = receiver.try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Empty);

        let result = sender.try_send(42);
        assert_eq!(result.is_ok(), true);

        let result = sender.try_send(42);
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TrySendError::Full(42));

        let result = receiver.try_recv();
        assert_eq!(result.is_ok(), true);

        let v = result.unwrap();
        assert_eq!(v, 42);

        let result = receiver.try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Empty);

        mem::drop(sender);

        let result = receiver.try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Disconnected);
    }

    #[test]
    fn test_notify_0() {
        let (sender, receiver) = channel(0);

        let poll = mio::Poll::new().unwrap();

        poll.register(
            sender.get_write_registration(),
            mio::Token(0),
            mio::Ready::writable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            receiver.get_read_registration(),
            mio::Token(1),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        assert_eq!(sender.can_send(), false);

        let mut events = mio::Events::with_capacity(1024);
        poll.poll(&mut events, Some(time::Duration::from_millis(0)))
            .unwrap();
        assert_eq!(events.iter().next(), None);

        let result = receiver.try_recv();
        assert_eq!(result.is_err(), true);
        assert_eq!(result.unwrap_err(), mpsc::TryRecvError::Empty);

        loop {
            let mut events = mio::Events::with_capacity(1024);
            poll.poll(&mut events, None).unwrap();

            let mut done = false;
            for event in events {
                match event.token() {
                    mio::Token(0) => {
                        assert_eq!(event.readiness().is_writable(), true);
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

        assert_eq!(sender.can_send(), true);

        sender.try_send(42).unwrap();

        loop {
            let mut events = mio::Events::with_capacity(1024);
            poll.poll(&mut events, None).unwrap();

            let mut done = false;
            for event in events {
                match event.token() {
                    mio::Token(1) => {
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

        let v = receiver.try_recv().unwrap();

        assert_eq!(v, 42);

        mem::drop(sender);

        loop {
            let mut events = mio::Events::with_capacity(1024);
            poll.poll(&mut events, None).unwrap();

            let mut done = false;
            for event in events {
                match event.token() {
                    mio::Token(1) => {
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

        let e = receiver.try_recv().unwrap_err();
        assert_eq!(e, mpsc::TryRecvError::Disconnected);
    }

    #[test]
    fn test_notify_1() {
        let (sender, receiver) = channel(1);

        let poll = mio::Poll::new().unwrap();

        poll.register(
            sender.get_write_registration(),
            mio::Token(0),
            mio::Ready::writable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        poll.register(
            receiver.get_read_registration(),
            mio::Token(1),
            mio::Ready::readable(),
            mio::PollOpt::edge(),
        )
        .unwrap();

        let mut events = mio::Events::with_capacity(1024);
        poll.poll(&mut events, Some(time::Duration::from_millis(0)))
            .unwrap();
        let event = events.iter().next();
        assert!(event.is_some());
        let event = event.unwrap();
        assert_eq!(event.token(), mio::Token(0));
        assert!(event.readiness().is_writable());

        sender.try_send(42).unwrap();

        loop {
            let mut events = mio::Events::with_capacity(1024);
            poll.poll(&mut events, None).unwrap();

            let mut done = false;
            for event in events {
                match event.token() {
                    mio::Token(1) => {
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

        let v = receiver.try_recv().unwrap();

        assert_eq!(v, 42);

        mem::drop(sender);

        loop {
            let mut events = mio::Events::with_capacity(1024);
            poll.poll(&mut events, None).unwrap();

            let mut done = false;
            for event in events {
                match event.token() {
                    mio::Token(1) => {
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

        let e = receiver.try_recv().unwrap_err();
        assert_eq!(e, mpsc::TryRecvError::Disconnected);
    }
}
