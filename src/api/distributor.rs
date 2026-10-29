/*
 * Copyright (C) 2026 Fastly, Inc.
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

use crate::core::zmq::{SpecInfo, ZmqSocket};
use std::error::Error;
use std::sync::Mutex;

#[derive(Clone)]
pub enum MetadataValue {
    String(String),
    Bytes(Vec<u8>),
}

pub trait MessageDistributor: Send + Sync {
    fn name(&self) -> &str;

    fn distribute(
        &self,
        metadata: &[(&str, MetadataValue)],
        channel: &str,
        item: Vec<u8>,
    ) -> Result<(), Box<dyn Error>>;
}

pub struct ZmqDistributor {
    item_out_sock: Mutex<ZmqSocket>,
}

impl ZmqDistributor {
    pub fn new(
        zmq_context: &zmq::Context,
        item_out_specs: &[SpecInfo],
    ) -> Result<Self, Box<dyn Error>> {
        let item_out_sock = ZmqSocket::new(zmq_context, zmq::PUB);

        if let Err(e) = item_out_sock.apply_specs(item_out_specs) {
            return Err(format!("failed to set item out specs: {e}").into());
        }

        Ok(Self {
            item_out_sock: Mutex::new(item_out_sock),
        })
    }
}

impl MessageDistributor for ZmqDistributor {
    fn name(&self) -> &str {
        "zmq"
    }

    fn distribute(
        &self,
        _metadata: &[(&str, MetadataValue)],
        channel: &str,
        item: Vec<u8>,
    ) -> Result<(), Box<dyn Error>> {
        let parts = [zmq::Message::from(channel), zmq::Message::from(item)];

        let item_out_sock = self.item_out_sock.lock().unwrap();

        // Sending to a PUB socket is non-blocking
        item_out_sock.send_multipart(parts, 0)?;

        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zmq_distribute() {
        let zmq_context = zmq::Context::new();

        let item_in_sock = zmq_context.socket(zmq::SUB).unwrap();
        item_in_sock.set_subscribe(&[]).unwrap();
        item_in_sock.bind("inproc://test-distribute-item").unwrap();

        let distributor = ZmqDistributor::new(
            &zmq_context,
            &[SpecInfo {
                spec: "inproc://test-distribute-item".to_string(),
                bind: false,
                ipc_file_mode: 0,
            }],
        )
        .unwrap();

        // Activate the subscription without receiving
        zmq::poll(&mut [item_in_sock.as_poll_item(zmq::POLLIN)], 1).unwrap();

        // Ensure we are subscribed
        std::thread::sleep(std::time::Duration::from_millis(100));

        distributor.distribute(&[], "test", "hello".into()).unwrap();

        let expected = "hello";

        let msgs = item_in_sock.recv_multipart(0).unwrap();
        assert_eq!(msgs.len(), 2);
        assert_eq!(str::from_utf8(&*msgs[0]).unwrap(), "test");
        assert_eq!(str::from_utf8(&*msgs[1]).unwrap(), expected);
    }
}
