/*
 * Copyright (C) 2023 Fanout, Inc.
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
use crate::buffer::TmpBuffer;
use crate::channel;
use crate::connection::{client_req_connection, client_stream_connection, StreamSharedData};
use crate::event;
use crate::executor::{Executor, Spawner};
use crate::future::{
    event_wait, select_2, select_5, select_option, AsyncLocalReceiver, AsyncLocalSender,
    AsyncReceiver, CancellationSender, CancellationToken, Select2, Select5, Timeout,
};
use crate::list;
use crate::pin;
use crate::reactor::Reactor;
use crate::resolver::Resolver;
use crate::tnetstring;
use crate::zhttppacket;
use crate::zhttpsocket;
use crate::zmq::{MultipartHeader, SpecInfo};
use arrayvec::ArrayVec;
use ipnet::IpNet;
use log::{debug, error, info, warn};
use mio::unix::SourceFd;
use slab::Slab;
use std::cell::Cell;
use std::cell::RefCell;
use std::collections::{HashMap, VecDeque};
use std::convert::TryFrom;
use std::io::{self, Write};
use std::mem;
use std::rc::Rc;
use std::str;
use std::sync::{mpsc, Arc};
use std::thread;
use std::time::Duration;

const REQ_SENDER_BOUND: usize = 1;

// we read and process each request message one at a time, wrapping it in an
// rc, and sending it to connections via channels. on the other side of each
// channel, the message is received and processed immediately. this means the
// max number of messages retained per connection is the channel bound per
// connection
pub const MSG_RETAINED_PER_CONNECTION_MAX: usize = REQ_SENDER_BOUND;

// the max number of messages retained outside of connections is one per
// handle we read from (req and stream), in preparation for sending to any
// connections
pub const MSG_RETAINED_PER_WORKER_MAX: usize = 2;

// run x1
// req_handle_task x1
// stream_handle_task x1
// keep_alives_task x1
const WORKER_NON_CONNECTION_TASKS_MAX: usize = 10;

// this is meant to be an average max of registrations per task, in order
// to determine the total number of registrations sufficient for all tasks,
// however it is not enforced per task
const REGISTRATIONS_PER_TASK_MAX: usize = 32;

const REACTOR_BUDGET: u32 = 100;

const KEEP_ALIVE_TIMEOUT_MS: usize = 45_000;
const KEEP_ALIVE_BATCH_MS: usize = 100;
const KEEP_ALIVE_INTERVAL: Duration = Duration::from_millis(KEEP_ALIVE_BATCH_MS as u64);
const KEEP_ALIVE_BATCHES: usize = KEEP_ALIVE_TIMEOUT_MS / KEEP_ALIVE_BATCH_MS;
const BULK_PACKET_SIZE_MAX: usize = 65_000;
const SHUTDOWN_TIMEOUT: Duration = Duration::from_millis(10_000);

const RESOLVER_THREADS: usize = 10;

fn local_channel<T>(
    bound: usize,
    max_senders: usize,
) -> (channel::LocalSender<T>, channel::LocalReceiver<T>) {
    let (s, r) = channel::local_channel(
        bound,
        max_senders,
        &Reactor::current().unwrap().local_registration_memory(),
    );

    (s, r)
}

fn async_local_channel<T>(
    bound: usize,
    max_senders: usize,
) -> (AsyncLocalSender<T>, AsyncLocalReceiver<T>) {
    let (s, r) = local_channel(bound, max_senders);

    let s = AsyncLocalSender::new(s);
    let r = AsyncLocalReceiver::new(r);

    (s, r)
}

struct BatchKey {
    addr_index: usize,
    nkey: usize,
}

struct BatchGroup<'a, 'b> {
    addr: &'b [u8],
    ids: arena::ReusableVecHandle<'b, zhttppacket::Id<'a>>,
}

impl<'a> BatchGroup<'a, '_> {
    fn addr(&self) -> &[u8] {
        self.addr
    }

    fn ids(&self) -> &[zhttppacket::Id<'a>] {
        &*self.ids
    }
}

struct Batch {
    nodes: Slab<list::Node<usize>>,
    addrs: Vec<(ArrayVec<u8, 64>, list::List)>,
    addr_index: usize,
    group_ids: arena::ReusableVec,
    last_group_ckeys: Vec<usize>,
}

impl Batch {
    fn new(capacity: usize) -> Self {
        Self {
            nodes: Slab::with_capacity(capacity),
            addrs: Vec::with_capacity(capacity),
            addr_index: 0,
            group_ids: arena::ReusableVec::new::<zhttppacket::Id>(capacity),
            last_group_ckeys: Vec::with_capacity(capacity),
        }
    }

    fn len(&self) -> usize {
        self.nodes.len()
    }

    fn capacity(&self) -> usize {
        self.nodes.capacity()
    }

    fn is_empty(&self) -> bool {
        self.nodes.is_empty()
    }

    fn clear(&mut self) {
        self.addrs.clear();
        self.nodes.clear();
        self.addr_index = 0;
    }

    fn add(&mut self, to_addr: &[u8], ckey: usize) -> Result<BatchKey, ()> {
        let mut pos = self.addrs.len();

        for (i, a) in self.addrs.iter().enumerate() {
            if a.0.as_ref() == to_addr {
                pos = i;
            }
        }

        if pos == self.addrs.len() {
            // connection limits to_addr to 64 so this is guaranteed to succeed
            let a = ArrayVec::try_from(to_addr).unwrap();

            self.addrs.push((a, list::List::default()));
        }

        if self.nodes.len() == self.nodes.capacity() {
            return Err(());
        }

        let nkey = self.nodes.insert(list::Node::new(ckey));
        self.addrs[pos].1.push_back(&mut self.nodes, nkey);

        Ok(BatchKey {
            addr_index: pos,
            nkey,
        })
    }

    fn remove(&mut self, key: BatchKey) {
        self.addrs[key.addr_index]
            .1
            .remove(&mut self.nodes, key.nkey);
        self.nodes.remove(key.nkey);
    }

    fn take_group<'a, 'b: 'a, F>(&'a mut self, get_ids: F) -> Option<BatchGroup>
    where
        F: Fn(usize) -> (&'b [u8], u32),
    {
        // find the next addr with items
        while self.addr_index < self.addrs.len() && self.addrs[self.addr_index].1.is_empty() {
            self.addr_index += 1;
        }

        // if all are empty, we're done
        if self.addr_index == self.addrs.len() {
            return None;
        }

        let (addr, keys) = &mut self.addrs[self.addr_index];

        self.last_group_ckeys.clear();

        let mut ids = self.group_ids.get_as_new();

        // get ids/seqs
        while ids.len() < zhttppacket::IDS_MAX {
            let nkey = match keys.pop_front(&mut self.nodes) {
                Some(nkey) => nkey,
                None => break,
            };

            let ckey = self.nodes[nkey].value;
            self.nodes.remove(nkey);

            let (id, seq) = get_ids(ckey);

            self.last_group_ckeys.push(ckey);
            ids.push(zhttppacket::Id { id, seq: Some(seq) });
        }

        Some(BatchGroup { addr, ids })
    }

    fn last_group_ckeys(&self) -> &[usize] {
        &self.last_group_ckeys
    }
}

enum BatchType {
    KeepAlive,
    Cancel,
}

struct ChannelPool<T> {
    items: RefCell<VecDeque<(channel::LocalSender<T>, channel::LocalReceiver<T>)>>,
}

impl<T> ChannelPool<T> {
    fn new(capacity: usize) -> Self {
        Self {
            items: RefCell::new(VecDeque::with_capacity(capacity)),
        }
    }

    fn take(&self) -> Option<(channel::LocalSender<T>, channel::LocalReceiver<T>)> {
        let p = &mut *self.items.borrow_mut();

        p.pop_back()
    }

    fn push(&self, pair: (channel::LocalSender<T>, channel::LocalReceiver<T>)) {
        let p = &mut *self.items.borrow_mut();

        p.push_back(pair);
    }
}

struct ConnectionDone {
    ckey: usize,
}

struct ConnectionItem {
    id: Option<ArrayVec<u8, 64>>,
    stop: Option<CancellationSender>,
    zreceiver_sender: Option<AsyncLocalSender<(arena::Rc<zhttppacket::OwnedRequest>, usize)>>,
    shared: Option<arena::Rc<StreamSharedData>>,
    batch_key: Option<BatchKey>,
}

struct ConnectionItems {
    nodes: Slab<list::Node<ConnectionItem>>,
    nodes_by_id: HashMap<ArrayVec<u8, 64>, usize>,
    batch: Batch,
}

impl ConnectionItems {
    fn new(capacity: usize, batch: Batch) -> Self {
        Self {
            nodes: Slab::with_capacity(capacity),
            nodes_by_id: HashMap::with_capacity(capacity),
            batch,
        }
    }
}

struct ConnectionsInner {
    active: list::List,
    count: usize,
    max: usize,
}

struct Connections {
    items: Rc<RefCell<ConnectionItems>>,
    inner: RefCell<ConnectionsInner>,
}

impl Connections {
    fn new(items: Rc<RefCell<ConnectionItems>>, max: usize) -> Self {
        Self {
            items,
            inner: RefCell::new(ConnectionsInner {
                active: list::List::default(),
                count: 0,
                max,
            }),
        }
    }

    fn count(&self) -> usize {
        self.inner.borrow().count
    }

    fn max(&self) -> usize {
        self.inner.borrow().max
    }

    fn add(
        &self,
        stop: CancellationSender,
        zreceiver_sender: Option<AsyncLocalSender<(arena::Rc<zhttppacket::OwnedRequest>, usize)>>,
        shared: Option<arena::Rc<StreamSharedData>>,
    ) -> Result<usize, ()> {
        let items = &mut *self.items.borrow_mut();
        let c = &mut *self.inner.borrow_mut();

        if items.nodes.len() == items.nodes.capacity() {
            return Err(());
        }

        let nkey = items.nodes.insert(list::Node::new(ConnectionItem {
            id: None,
            stop: Some(stop),
            zreceiver_sender,
            shared,
            batch_key: None,
        }));

        c.active.push_back(&mut items.nodes, nkey);
        c.count += 1;

        Ok(nkey)
    }

    // return zreceiver_sender
    fn remove(
        &self,
        ckey: usize,
    ) -> Option<channel::LocalSender<(arena::Rc<zhttppacket::OwnedRequest>, usize)>> {
        let nkey = ckey;

        let items = &mut *self.items.borrow_mut();
        let c = &mut *self.inner.borrow_mut();
        let ci = &mut items.nodes[nkey].value;

        // clear active keep alive
        if let Some(bkey) = ci.batch_key.take() {
            items.batch.remove(bkey);
        }

        c.active.remove(&mut items.nodes, nkey);
        c.count -= 1;

        let ci = items.nodes.remove(nkey).value;

        if let Some(id) = &ci.id {
            items.nodes_by_id.remove(id);
        }

        match ci.zreceiver_sender {
            Some(sender) => Some(sender.into_inner()),
            None => None,
        }
    }

    fn set_id(&self, ckey: usize, id: Option<&[u8]>) {
        let nkey = ckey;

        let items = &mut *self.items.borrow_mut();
        let ci = &mut items.nodes[nkey].value;

        // unset current id, if any
        if let Some(cur_id) = &ci.id {
            items.nodes_by_id.remove(cur_id);
            ci.id = None;
        }

        if let Some(id) = id {
            // connection limits id to 64 so this is guaranteed to succeed
            let id = ArrayVec::try_from(id).unwrap();

            ci.id = Some(id.clone());
            items.nodes_by_id.insert(id, nkey);
        } else {
            // clear active keep alive
            if let Some(bkey) = ci.batch_key.take() {
                items.batch.remove(bkey);
            }
        }
    }

    fn find_key(&self, id: &[u8]) -> Option<usize> {
        let items = &*self.items.borrow();

        items.nodes_by_id.get(id).copied()
    }

    fn take_zreceiver_sender(
        &self,
        ckey: usize,
    ) -> Option<AsyncLocalSender<(arena::Rc<zhttppacket::OwnedRequest>, usize)>> {
        let nkey = ckey;

        let items = &mut *self.items.borrow_mut();
        let ci = &mut items.nodes[nkey].value;

        ci.zreceiver_sender.take()
    }

    fn set_zreceiver_sender(
        &self,
        ckey: usize,
        sender: AsyncLocalSender<(arena::Rc<zhttppacket::OwnedRequest>, usize)>,
    ) {
        let nkey = ckey;

        let items = &mut *self.items.borrow_mut();
        let ci = &mut items.nodes[nkey].value;

        ci.zreceiver_sender = Some(sender);
    }

    fn stop_all<F>(&self, about_to_stop: F)
    where
        F: Fn(usize),
    {
        let items = &mut *self.items.borrow_mut();
        let cinner = &*self.inner.borrow_mut();

        let mut next = cinner.active.head;
        while let Some(nkey) = next {
            let n = &mut items.nodes[nkey];
            let ci = &mut n.value;

            about_to_stop(nkey);

            ci.stop = None;

            next = n.next;
        }
    }

    fn items_capacity(&self) -> usize {
        self.items.borrow().nodes.capacity()
    }

    fn can_stream(&self, ckey: usize) -> bool {
        let items = &*self.items.borrow();

        match items.nodes.get(ckey) {
            Some(n) => {
                let ci = &n.value;

                // is stream mode with an id
                ci.shared.is_some() && ci.id.is_some()
            }
            None => false,
        }
    }

    fn batch_is_empty(&self) -> bool {
        let items = &*self.items.borrow();

        items.batch.is_empty()
    }

    fn batch_len(&self) -> usize {
        let items = &*self.items.borrow();

        items.batch.len()
    }

    fn batch_capacity(&self) -> usize {
        let items = &*self.items.borrow();

        items.batch.capacity()
    }

    fn batch_clear(&self) {
        let items = &mut *self.items.borrow_mut();

        items.batch.clear();
    }

    fn batch_add(&self, ckey: usize) -> Result<(), ()> {
        let items = &mut *self.items.borrow_mut();
        let ci = &mut items.nodes[ckey].value;
        let cshared = ci.shared.as_ref().unwrap().get();

        // only batch connections with known handler addresses
        let addr_ref = cshared.to_addr();
        let addr = match addr_ref.get() {
            Some(addr) => addr,
            None => return Err(()),
        };

        let bkey = items.batch.add(addr, ckey)?;

        ci.batch_key = Some(bkey);

        Ok(())
    }

    fn next_batch_message(&self, from: &str, btype: BatchType) -> Option<(usize, zmq::Message)> {
        let items = &mut *self.items.borrow_mut();
        let nodes = &mut items.nodes;
        let batch = &mut items.batch;

        while !batch.is_empty() {
            let group = batch
                .take_group(|ckey| {
                    let ci = &nodes[ckey].value;
                    let cshared = ci.shared.as_ref().unwrap().get();

                    // item is guaranteed to have an id. only items with an
                    // id are added to a batch, and if an item's id is
                    // removed then the item is removed from the batch
                    let id = ci.id.as_ref().unwrap();

                    (id, cshared.out_seq())
                })
                .unwrap();

            let count = group.ids().len();

            assert!(count <= zhttppacket::IDS_MAX);

            let zreq = zhttppacket::Request {
                from: from.as_bytes(),
                ids: group.ids(),
                multi: true,
                ptype: match btype {
                    BatchType::KeepAlive => zhttppacket::RequestPacket::KeepAlive,
                    BatchType::Cancel => zhttppacket::RequestPacket::Cancel,
                },
                ptype_str: "",
            };

            let mut data = [0; BULK_PACKET_SIZE_MAX];

            let size = match zreq.serialize(&mut data) {
                Ok(size) => size,
                Err(e) => {
                    error!(
                        "failed to serialize keep-alive packet with {} ids: {}",
                        zreq.ids.len(),
                        e
                    );
                    continue;
                }
            };

            let data = &data[..size];

            let addr = group.addr();

            let msg = {
                let mut v = vec![0; addr.len() + 1 + data.len()];

                v[..addr.len()].copy_from_slice(addr);
                v[addr.len()] = b' ';
                let pos = addr.len() + 1;
                v[pos..(pos + data.len())].copy_from_slice(data);

                // this takes over the vec's memory without copying
                zmq::Message::from(v)
            };

            drop(group);

            for &ckey in batch.last_group_ckeys() {
                let ci = &mut nodes[ckey].value;
                let cshared = ci.shared.as_ref().unwrap().get();

                cshared.inc_out_seq();
                ci.batch_key = None;
            }

            return Some((count, msg));
        }

        None
    }
}

#[derive(Clone)]
struct ConnectionOpts {
    instance_id: Rc<String>,
    buffer_size: usize,
    timeout: Duration,
    rb_tmp: Rc<TmpBuffer>,
    packet_buf: Rc<RefCell<Vec<u8>>>,
    tmp_buf: Rc<RefCell<Vec<u8>>>,
}

struct ConnectionReqOpts {
    body_buffer_size: usize,
    sender: channel::LocalSender<(MultipartHeader, zmq::Message)>,
}

struct ConnectionStreamOpts {
    messages_max: usize,
    allow_compression: bool,
    sender: channel::LocalSender<zmq::Message>,
}

struct Worker {
    thread: Option<thread::JoinHandle<()>>,
    stop: Option<channel::Sender<()>>,
}

impl Worker {
    fn new(
        instance_id: &str,
        id: usize,
        req_maxconn: usize,
        stream_maxconn: usize,
        buffer_size: usize,
        body_buffer_size: usize,
        messages_max: usize,
        req_timeout: Duration,
        stream_timeout: Duration,
        allow_compression: bool,
        deny: &[IpNet],
        resolver: &Arc<Resolver>,
        zsockman: &Arc<zhttpsocket::ServerSocketManager>,
        handle_bound: usize,
    ) -> Self {
        debug!("client worker {}: starting", id);

        let (stop, r_stop) = channel::channel(1);
        let (s_ready, ready) = channel::channel(1);

        let instance_id = String::from(instance_id);
        let deny = deny.to_vec();
        let resolver = Arc::clone(resolver);
        let zsockman = Arc::clone(zsockman);

        let thread = thread::Builder::new()
            .name(format!("client-worker-{}", id))
            .spawn(move || {
                let maxconn = req_maxconn + stream_maxconn;

                // 1 task per connection, plus a handful of supporting tasks
                let tasks_max = maxconn + WORKER_NON_CONNECTION_TASKS_MAX;

                let registrations_max = REGISTRATIONS_PER_TASK_MAX * tasks_max;

                let reactor = Reactor::new(registrations_max);

                let executor = Executor::new(tasks_max);

                {
                    let reactor = reactor.clone();

                    executor.set_pre_poll(move || {
                        reactor.set_budget(Some(REACTOR_BUDGET));
                    });
                }

                executor
                    .spawn(Self::run(
                        r_stop,
                        s_ready,
                        instance_id,
                        id,
                        req_maxconn,
                        stream_maxconn,
                        buffer_size,
                        body_buffer_size,
                        messages_max,
                        req_timeout,
                        stream_timeout,
                        allow_compression,
                        deny,
                        resolver,
                        zsockman,
                        handle_bound,
                    ))
                    .unwrap();

                executor.run(|timeout| reactor.poll(timeout)).unwrap();

                debug!("client worker {}: stopped", id);
            })
            .unwrap();

        ready.recv().unwrap();

        Self {
            thread: Some(thread),
            stop: Some(stop),
        }
    }

    fn stop(&mut self) {
        self.stop = None;
    }

    async fn run(
        stop: channel::Receiver<()>,
        ready: channel::Sender<()>,
        instance_id: String,
        id: usize,
        req_maxconn: usize,
        stream_maxconn: usize,
        buffer_size: usize,
        body_buffer_size: usize,
        messages_max: usize,
        req_timeout: Duration,
        stream_timeout: Duration,
        allow_compression: bool,
        deny: Vec<IpNet>,
        resolver: Arc<Resolver>,
        zsockman: Arc<zhttpsocket::ServerSocketManager>,
        handle_bound: usize,
    ) {
        let executor = Executor::current().unwrap();
        let reactor = Reactor::current().unwrap();
        let stop = AsyncReceiver::new(stop);

        debug!("client-worker {}: allocating buffers", id);

        let rb_tmp = Rc::new(TmpBuffer::new(buffer_size));

        // large enough to fit anything
        let packet_buf = Rc::new(RefCell::new(vec![0; buffer_size + body_buffer_size + 4096]));

        // same size as working buffers
        let tmp_buf = Rc::new(RefCell::new(vec![0; buffer_size]));

        let instance_id = Rc::new(instance_id);

        let ka_batch = (stream_maxconn + (KEEP_ALIVE_BATCHES - 1)) / KEEP_ALIVE_BATCHES;

        let batch = Batch::new(ka_batch);

        let maxconn = req_maxconn + stream_maxconn;

        let conn_items = Rc::new(RefCell::new(ConnectionItems::new(maxconn, batch)));

        let req_conns = Rc::new(Connections::new(conn_items.clone(), req_maxconn));
        let stream_conns = Rc::new(Connections::new(conn_items.clone(), stream_maxconn));

        let (req_handle_stop, r_req_handle_stop) = async_local_channel(1, 1);
        let (stream_handle_stop, r_stream_handle_stop) = async_local_channel(1, 1);
        let (keep_alives_stop, r_keep_alives_stop) = async_local_channel(1, 1);

        let (s_req_handle_done, req_handle_done) = async_local_channel(1, 1);
        let (s_stream_handle_done, stream_handle_done) = async_local_channel(1, 1);
        let (s_keep_alives_done, keep_alives_done) = async_local_channel(1, 1);

        // max_senders is 1 per connection + 1 for the handle task + 1 for the keep alive task
        let (zstream_out_sender, zstream_out_receiver) =
            local_channel(handle_bound, stream_maxconn + 2);

        let zstream_out_receiver = AsyncLocalReceiver::new(zstream_out_receiver);

        let req_handle = zhttpsocket::AsyncServerReqHandle::new(zsockman.server_req_handle());

        let stream_handle =
            zhttpsocket::AsyncServerStreamHandle::new(zsockman.server_stream_handle());

        let deny = Rc::new(deny);

        executor
            .spawn(Self::req_handle_task(
                id,
                r_req_handle_stop,
                s_req_handle_done,
                executor.spawner(),
                Arc::clone(&resolver),
                req_handle,
                req_maxconn,
                req_conns,
                body_buffer_size,
                Rc::clone(&deny),
                handle_bound,
                ConnectionOpts {
                    instance_id: instance_id.clone(),
                    buffer_size,
                    timeout: req_timeout,
                    rb_tmp: rb_tmp.clone(),
                    packet_buf: packet_buf.clone(),
                    tmp_buf: tmp_buf.clone(),
                },
            ))
            .unwrap();

        {
            let zstream_out_sender = zstream_out_sender
                .try_clone(&reactor.local_registration_memory())
                .unwrap();

            executor
                .spawn(Self::stream_handle_task(
                    id,
                    r_stream_handle_stop,
                    s_stream_handle_done,
                    zstream_out_receiver,
                    zstream_out_sender,
                    executor.spawner(),
                    Arc::clone(&resolver),
                    stream_handle,
                    stream_maxconn,
                    stream_conns.clone(),
                    messages_max,
                    allow_compression,
                    Rc::clone(&deny),
                    ConnectionOpts {
                        instance_id: instance_id.clone(),
                        buffer_size,
                        timeout: stream_timeout,
                        rb_tmp: rb_tmp.clone(),
                        packet_buf: packet_buf.clone(),
                        tmp_buf: tmp_buf.clone(),
                    },
                ))
                .unwrap();
        }

        executor
            .spawn(Self::keep_alives_task(
                id,
                r_keep_alives_stop,
                s_keep_alives_done,
                instance_id.clone(),
                zstream_out_sender,
                stream_conns.clone(),
            ))
            .unwrap();

        debug!("client-worker {}: started", id);

        ready.send(()).unwrap();
        drop(ready);

        // wait for stop
        let _ = stop.recv().await;

        // stop keep alives
        drop(keep_alives_stop);
        let _ = keep_alives_done.recv().await;

        // stop remaining tasks
        drop(req_handle_stop);
        drop(stream_handle_stop);
        let _ = req_handle_done.recv().await;
        let stream_handle = stream_handle_done.recv().await.unwrap();

        // send cancels

        stream_conns.batch_clear();

        let now = reactor.now();
        let shutdown_timeout = Timeout::new(now + SHUTDOWN_TIMEOUT);

        let mut next_cancel_index = 0;

        'outer: while next_cancel_index < stream_conns.items_capacity() {
            while stream_conns.batch_len() < stream_conns.batch_capacity()
                && next_cancel_index < stream_conns.items_capacity()
            {
                let key = next_cancel_index;

                next_cancel_index += 1;

                if stream_conns.can_stream(key) {
                    // ignore errors
                    let _ = stream_conns.batch_add(key);
                }
            }

            while let Some((count, msg)) =
                stream_conns.next_batch_message(&instance_id, BatchType::Cancel)
            {
                debug!(
                    "client-worker {}: sending cancels for {} sessions",
                    id, count
                );

                match select_2(pin!(stream_handle.send(msg)), shutdown_timeout.elapsed()).await {
                    Select2::R1(r) => r.unwrap(),
                    Select2::R2(_) => break 'outer,
                }
            }

            stream_conns.batch_clear();
        }
    }

    async fn req_handle_task(
        id: usize,
        stop: AsyncLocalReceiver<()>,
        _done: AsyncLocalSender<()>,
        spawner: Spawner,
        resolver: Arc<Resolver>,
        req_handle: zhttpsocket::AsyncServerReqHandle,
        req_maxconn: usize,
        conns: Rc<Connections>,
        body_buffer_size: usize,
        deny: Rc<Vec<IpNet>>,
        handle_bound: usize,
        opts: ConnectionOpts,
    ) {
        let reactor = Reactor::current().unwrap();

        let msg_retained_max = 1 + (MSG_RETAINED_PER_CONNECTION_MAX * req_maxconn);

        let req_scratch_mem = Rc::new(arena::RcMemory::new(msg_retained_max));
        let req_req_mem = Rc::new(arena::RcMemory::new(msg_retained_max));

        // max_senders is 1 per connection + 1 for this task
        let (zreq_sender, zreq_receiver) = local_channel(handle_bound, req_maxconn + 1);

        let zreq_receiver = AsyncLocalReceiver::new(zreq_receiver);

        // bound is 1 per connection, so all connections can indicate done at once
        // max_senders is 1 per connection + 1 for this task
        let (s_cdone, r_cdone) = channel::local_channel::<ConnectionDone>(
            conns.max(),
            conns.max() + 1,
            &reactor.local_registration_memory(),
        );

        let r_cdone = AsyncLocalReceiver::new(r_cdone);

        debug!("client-worker {}: task started: req_handle", id);

        let mut handle_send = pin!(None);

        loop {
            let receiver_recv = if handle_send.is_none() {
                Some(zreq_receiver.recv())
            } else {
                None
            };

            let req_handle_recv = if conns.count() < conns.max() {
                Some(req_handle.recv())
            } else {
                None
            };

            match select_5(
                stop.recv(),
                select_option(receiver_recv),
                select_option(handle_send.as_mut().as_pin_mut()),
                r_cdone.recv(),
                select_option(pin!(req_handle_recv).as_pin_mut()),
            )
            .await
            {
                // stop.recv
                Select5::R1(_) => break,
                // receiver_recv
                Select5::R2(result) => match result {
                    Ok((header, msg)) => handle_send.set(Some(req_handle.send(header, msg))),
                    Err(e) => panic!("zreq_receiver channel error: {}", e),
                },
                // handle_send
                Select5::R3(result) => {
                    handle_send.set(None);

                    if let Err(e) = result {
                        error!("req send error: {}", e);
                    }
                }
                // r_cdone.recv
                Select5::R4(result) => match result {
                    Ok(done) => {
                        let ret = conns.remove(done.ckey);

                        // req mode doesn't have a sender
                        assert_eq!(ret.is_none(), true);
                    }
                    Err(e) => panic!("r_cdone channel error: {}", e),
                },
                // req_handle_recv
                Select5::R5(result) => match result {
                    Ok((header, msg)) => {
                        let scratch = arena::Rc::new(
                            RefCell::new(zhttppacket::ParseScratch::new()),
                            &req_scratch_mem,
                        )
                        .unwrap();

                        let zreq = match zhttppacket::OwnedRequest::parse(msg, 0, scratch) {
                            Ok(zreq) => zreq,
                            Err(e) => {
                                warn!("client-worker {}: zhttp parse error: {}", id, e);
                                continue;
                            }
                        };

                        let ids = zreq.get().ids;

                        if ids.len() > 1 {
                            warn!(
                                "client-worker {}: request contained more than one id, skipping",
                                id
                            );

                            continue;
                        }

                        let cid: Option<ArrayVec<u8, 64>> = if !ids.is_empty() {
                            match ArrayVec::try_from(ids[0].id) {
                                Ok(v) => Some(v),
                                Err(_) => {
                                    warn!("client-worker {}: request id too long, skipping", id);
                                    continue;
                                }
                            }
                        } else {
                            None
                        };

                        let zreq = arena::Rc::new(zreq, &req_req_mem).unwrap();

                        let (cstop, r_cstop) =
                            CancellationToken::new(&reactor.local_registration_memory());

                        let s_cdone = s_cdone
                            .try_clone(&reactor.local_registration_memory())
                            .unwrap();

                        let zreq_sender = zreq_sender
                            .try_clone(&reactor.local_registration_memory())
                            .unwrap();

                        let ckey = conns.add(cstop, None, None).unwrap();

                        if let Some(cid) = &cid {
                            conns.set_id(ckey, Some(cid.as_slice()));
                        }

                        debug!(
                            "client-worker {}: req conn starting {} {}/{}",
                            id,
                            ckey,
                            conns.count(),
                            conns.max(),
                        );

                        if spawner
                            .spawn(Self::req_connection_task(
                                r_cstop,
                                s_cdone,
                                id,
                                ckey,
                                cid,
                                (header, zreq),
                                Arc::clone(&resolver),
                                Rc::clone(&deny),
                                opts.clone(),
                                ConnectionReqOpts {
                                    body_buffer_size,
                                    sender: zreq_sender,
                                },
                            ))
                            .is_err()
                        {
                            // this should never happen. we only read a message
                            // if we know we can spawn
                            panic!("failed to spawn req_connection_task");
                        }
                    }
                    Err(e) => panic!("client-worker {}: handle read error {}", id, e),
                },
            }
        }

        drop(s_cdone);

        conns.stop_all(|ckey| debug!("client-worker {}: stopping {}", id, ckey));

        while r_cdone.recv().await.is_ok() {}

        debug!("client-worker {}: task stopped: req_handle", id);
    }

    async fn stream_handle_task(
        id: usize,
        stop: AsyncLocalReceiver<()>,
        done: AsyncLocalSender<zhttpsocket::AsyncServerStreamHandle>,
        zstream_out_receiver: AsyncLocalReceiver<zmq::Message>,
        zstream_out_sender: channel::LocalSender<zmq::Message>,
        spawner: Spawner,
        resolver: Arc<Resolver>,
        stream_handle: zhttpsocket::AsyncServerStreamHandle,
        stream_maxconn: usize,
        conns: Rc<Connections>,
        messages_max: usize,
        allow_compression: bool,
        deny: Rc<Vec<IpNet>>,
        opts: ConnectionOpts,
    ) {
        let reactor = Reactor::current().unwrap();

        let stream_shared_mem = Rc::new(arena::RcMemory::new(stream_maxconn));

        let zreceiver_pool = Rc::new(ChannelPool::new(stream_maxconn));
        for _ in 0..stream_maxconn {
            zreceiver_pool.push(local_channel(REQ_SENDER_BOUND, 1));
        }

        let msg_retained_max = 1 + (MSG_RETAINED_PER_CONNECTION_MAX * stream_maxconn);

        let stream_scratch_mem = Rc::new(arena::RcMemory::new(msg_retained_max));
        let stream_req_mem = Rc::new(arena::RcMemory::new(msg_retained_max));

        // bound is 1 per connection, so all connections can indicate done at once
        // max_senders is 1 per connection + 1 for this task
        let (s_cdone, r_cdone) = channel::local_channel::<ConnectionDone>(
            conns.max(),
            conns.max() + 1,
            &reactor.local_registration_memory(),
        );

        let r_cdone = AsyncLocalReceiver::new(r_cdone);

        debug!("client-worker {}: task started: stream_handle", id);

        {
            let mut handle_send = pin!(None);

            'main: loop {
                let receiver_recv = if handle_send.is_none() {
                    Some(zstream_out_receiver.recv())
                } else {
                    None
                };

                let stream_handle_recv = if conns.count() < conns.max() {
                    Some(stream_handle.recv())
                } else {
                    None
                };

                match select_5(
                    stop.recv(),
                    select_option(receiver_recv),
                    select_option(handle_send.as_mut().as_pin_mut()),
                    r_cdone.recv(),
                    select_option(pin!(stream_handle_recv).as_pin_mut()),
                )
                .await
                {
                    // stop.recv
                    Select5::R1(_) => break,
                    // receiver_recv
                    Select5::R2(result) => match result {
                        Ok(msg) => handle_send.set(Some(stream_handle.send(msg))),
                        Err(e) => panic!("zstream_out_receiver channel error: {}", e),
                    },
                    // handle_send
                    Select5::R3(result) => {
                        handle_send.set(None);

                        if let Err(e) = result {
                            error!("stream send error: {}", e);
                        }
                    }
                    // r_cdone.recv
                    Select5::R4(result) => match result {
                        Ok(done) => {
                            let zreceiver_sender = conns.remove(done.ckey).unwrap();

                            let zreceiver = zreceiver_sender
                                .make_receiver(&reactor.local_registration_memory())
                                .unwrap();
                            zreceiver.clear();

                            zreceiver_pool.push((zreceiver_sender, zreceiver));
                        }
                        Err(e) => panic!("r_cdone channel error: {}", e),
                    },
                    // stream_handle_recv
                    Select5::R5(result) => match result {
                        Ok(msg) => {
                            let scratch = arena::Rc::new(
                                RefCell::new(zhttppacket::ParseScratch::new()),
                                &stream_scratch_mem,
                            )
                            .unwrap();

                            let zreq = match zhttppacket::OwnedRequest::parse(msg, 0, scratch) {
                                Ok(zreq) => zreq,
                                Err(e) => {
                                    warn!("client-worker {}: zhttp parse error: {}", id, e);
                                    continue;
                                }
                            };

                            let zreq = arena::Rc::new(zreq, &stream_req_mem).unwrap();

                            let zreq_ref = zreq.get().get();

                            let ids = zreq_ref.ids;

                            if ids.is_empty() {
                                warn!("client-worker {}: packet contained no ids, skipping", id);
                                continue;
                            }

                            if ids.len() == 1 && ids[0].seq == Some(0) {
                                // first packet of a session

                                if !zreq_ref.ptype_str.is_empty() {
                                    warn!("client-worker {}: received non-data message as first message, skipping", id);
                                    continue;
                                }

                                let cid: ArrayVec<u8, 64> = match ArrayVec::try_from(ids[0].id) {
                                    Ok(v) => v,
                                    Err(_) => {
                                        warn!(
                                            "client-worker {}: request id too long, skipping",
                                            id
                                        );
                                        continue;
                                    }
                                };

                                let (cstop, r_cstop) =
                                    CancellationToken::new(&reactor.local_registration_memory());

                                let s_cdone = s_cdone
                                    .try_clone(&reactor.local_registration_memory())
                                    .unwrap();

                                let zstream_out_sender = zstream_out_sender
                                    .try_clone(&reactor.local_registration_memory())
                                    .unwrap();

                                let (zstream_receiver_sender, zstream_receiver) =
                                    zreceiver_pool.take().unwrap();

                                let zstream_receiver_sender =
                                    AsyncLocalSender::new(zstream_receiver_sender);

                                let shared =
                                    arena::Rc::new(StreamSharedData::new(), &stream_shared_mem)
                                        .unwrap();

                                let ckey = conns
                                    .add(
                                        cstop,
                                        Some(zstream_receiver_sender),
                                        Some(arena::Rc::clone(&shared)),
                                    )
                                    .unwrap();

                                debug!(
                                    "client-worker {}: stream conn starting {} {}/{}",
                                    id,
                                    ckey,
                                    conns.count(),
                                    conns.max(),
                                );

                                if spawner
                                    .spawn(Self::stream_connection_task(
                                        r_cstop,
                                        s_cdone,
                                        id,
                                        ckey,
                                        cid,
                                        arena::Rc::clone(&zreq),
                                        Arc::clone(&resolver),
                                        zstream_receiver,
                                        Rc::clone(&deny),
                                        Rc::clone(&conns),
                                        opts.clone(),
                                        ConnectionStreamOpts {
                                            messages_max,
                                            allow_compression,
                                            sender: zstream_out_sender,
                                        },
                                        shared,
                                    ))
                                    .is_err()
                                {
                                    // this should never happen. we only read a message
                                    // if we know we can spawn
                                    panic!("failed to spawn stream_connection_task");
                                }
                            } else {
                                let mut count = 0;

                                for (i, rid) in ids.iter().enumerate() {
                                    if let Some(key) = conns.find_key(&rid.id) {
                                        if let Some(sender) = conns.take_zreceiver_sender(key) {
                                            let mut done = false;

                                            match select_2(
                                                stop.recv(),
                                                sender.send((arena::Rc::clone(&zreq), i)),
                                            )
                                            .await
                                            {
                                                Select2::R1(_) => done = true,
                                                Select2::R2(result) => match result {
                                                    Ok(()) => count += 1,
                                                    Err(mpsc::SendError(_)) => {} // conn task ended
                                                },
                                            }

                                            // always put back the sender
                                            conns.set_zreceiver_sender(key, sender);

                                            if done {
                                                break 'main;
                                            }
                                        }
                                    }
                                }

                                debug!(
                                    "client-worker {}: queued zmq message for {} conns",
                                    id, count
                                );
                            }
                        }
                        Err(e) => panic!("client-worker {}: handle read error {}", id, e),
                    },
                }
            }
        }

        drop(s_cdone);

        conns.stop_all(|ckey| debug!("client-worker {}: stopping {}", id, ckey));

        while r_cdone.recv().await.is_ok() {}

        // give the handle back
        done.send(stream_handle).await.unwrap();

        debug!("client-worker {}: task stopped: stream_handle", id);
    }

    async fn req_connection_task(
        token: CancellationToken,
        done: channel::LocalSender<ConnectionDone>,
        worker_id: usize,
        ckey: usize,
        cid: Option<ArrayVec<u8, 64>>,
        zreq: (MultipartHeader, arena::Rc<zhttppacket::OwnedRequest>),
        resolver: Arc<Resolver>,
        deny: Rc<Vec<IpNet>>,
        opts: ConnectionOpts,
        req_opts: ConnectionReqOpts,
    ) {
        let done = AsyncLocalSender::new(done);

        debug!(
            "client-worker {}: task started: connection-{}",
            worker_id, ckey
        );

        let log_id = if let Some(cid) = &cid {
            // zhttp ids are pretty much always valid strings, but we'll
            // do a lossy conversion just in case
            let cid_str = String::from_utf8_lossy(cid);

            format!("{}-{}-{}", worker_id, ckey, cid_str)
        } else {
            format!("{}-{}", worker_id, ckey)
        };

        client_req_connection(
            token,
            &log_id,
            cid.as_deref(),
            zreq,
            opts.buffer_size,
            req_opts.body_buffer_size,
            &opts.rb_tmp,
            opts.packet_buf,
            opts.timeout,
            &deny,
            &resolver,
            AsyncLocalSender::new(req_opts.sender),
        )
        .await;

        done.send(ConnectionDone { ckey }).await.unwrap();

        debug!(
            "client-worker {}: task stopped: connection-{}",
            worker_id, ckey
        );
    }

    async fn stream_connection_task(
        token: CancellationToken,
        done: channel::LocalSender<ConnectionDone>,
        worker_id: usize,
        ckey: usize,
        cid: ArrayVec<u8, 64>,
        zreq: arena::Rc<zhttppacket::OwnedRequest>,
        resolver: Arc<Resolver>,
        zreceiver: channel::LocalReceiver<(arena::Rc<zhttppacket::OwnedRequest>, usize)>,
        deny: Rc<Vec<IpNet>>,
        conns: Rc<Connections>,
        opts: ConnectionOpts,
        stream_opts: ConnectionStreamOpts,
        shared: arena::Rc<StreamSharedData>,
    ) {
        let done = AsyncLocalSender::new(done);
        let zreceiver = AsyncLocalReceiver::new(zreceiver);

        debug!(
            "client-worker {}: task started: connection-{}",
            worker_id, ckey
        );

        let log_id = {
            // zhttp ids are pretty much always valid strings, but we'll
            // do a lossy conversion just in case
            let cid_str = String::from_utf8_lossy(&cid);

            format!("{}-{}-{}", worker_id, ckey, cid_str)
        };

        client_stream_connection(
            token,
            &log_id,
            &cid,
            zreq,
            opts.buffer_size,
            stream_opts.messages_max,
            &opts.rb_tmp,
            opts.packet_buf,
            opts.tmp_buf,
            opts.timeout,
            stream_opts.allow_compression,
            &deny,
            &opts.instance_id,
            &resolver,
            zreceiver,
            AsyncLocalSender::new(stream_opts.sender),
            shared,
            &|| conns.set_id(ckey, Some(&cid)),
        )
        .await;

        done.send(ConnectionDone { ckey }).await.unwrap();

        debug!(
            "client-worker {}: task stopped: connection-{}",
            worker_id, ckey
        );
    }

    async fn keep_alives_task(
        id: usize,
        stop: AsyncLocalReceiver<()>,
        _done: AsyncLocalSender<()>,
        instance_id: Rc<String>,
        sender: channel::LocalSender<zmq::Message>,
        conns: Rc<Connections>,
    ) {
        debug!("client-worker {}: task started: keep_alives", id);

        let reactor = Reactor::current().unwrap();

        let mut keep_alive_count = 0;
        let mut next_keep_alive_time = reactor.now() + KEEP_ALIVE_INTERVAL;
        let next_keep_alive_timeout = Timeout::new(next_keep_alive_time);
        let mut next_keep_alive_index = 0;

        let sender_registration = reactor
            .register_custom_local(sender.get_write_registration(), mio::Interest::WRITABLE)
            .unwrap();

        sender_registration.set_readiness(Some(mio::Interest::WRITABLE));

        'main: loop {
            while conns.batch_is_empty() {
                // wait for next keep alive time
                match select_2(stop.recv(), next_keep_alive_timeout.elapsed()).await {
                    Select2::R1(_) => break 'main,
                    Select2::R2(_) => {}
                }

                for _ in 0..conns.batch_capacity() {
                    if next_keep_alive_index >= conns.items_capacity() {
                        break;
                    }

                    let key = next_keep_alive_index;

                    next_keep_alive_index += 1;

                    if conns.can_stream(key) {
                        // ignore errors
                        let _ = conns.batch_add(key);
                    }
                }

                keep_alive_count += 1;

                if keep_alive_count >= KEEP_ALIVE_BATCHES {
                    keep_alive_count = 0;
                    next_keep_alive_index = 0;
                }

                // keep steady pace
                next_keep_alive_time += KEEP_ALIVE_INTERVAL;
                next_keep_alive_timeout.set_deadline(next_keep_alive_time);
            }

            match select_2(
                stop.recv(),
                pin!(event_wait(&sender_registration, mio::Interest::WRITABLE)),
            )
            .await
            {
                Select2::R1(_) => break,
                Select2::R2(_) => {}
            }

            if !sender.check_send() {
                // if check_send returns false, we'll be on the waitlist for a notification
                sender_registration.clear_readiness(mio::Interest::WRITABLE);
                continue;
            }

            // if check_send returns true, we are guaranteed to be able to send

            match conns.next_batch_message(&instance_id, BatchType::KeepAlive) {
                Some((count, msg)) => {
                    debug!(
                        "client-worker {}: sending keep alives for {} sessions",
                        id, count
                    );

                    if let Err(e) = sender.try_send(msg) {
                        error!("zhttp write error: {}", e);
                    }
                }
                None => {
                    // this could happen if message construction failed
                    sender.cancel();
                }
            }

            if conns.batch_is_empty() {
                conns.batch_clear();

                let now = reactor.now();

                if now >= next_keep_alive_time + KEEP_ALIVE_INTERVAL {
                    // got really behind somehow. just skip ahead
                    next_keep_alive_time = now + KEEP_ALIVE_INTERVAL;
                    next_keep_alive_timeout.set_deadline(next_keep_alive_time);
                }
            }
        }

        debug!("client-worker {}: task stopped: keep_alives", id);
    }
}

impl Drop for Worker {
    fn drop(&mut self) {
        self.stop();

        let thread = self.thread.take().unwrap();
        thread.join().unwrap();
    }
}

pub struct Client {
    workers: Vec<Worker>,
}

impl Client {
    pub fn new(
        instance_id: &str,
        worker_count: usize,
        req_maxconn: usize,
        stream_maxconn: usize,
        buffer_size: usize,
        body_buffer_size: usize,
        messages_max: usize,
        req_timeout: Duration,
        stream_timeout: Duration,
        allow_compression: bool,
        deny: &[IpNet],
        zsockman: Arc<zhttpsocket::ServerSocketManager>,
        handle_bound: usize,
    ) -> Result<Self, String> {
        // 1 active query per connection
        let queries_max = req_maxconn + stream_maxconn;

        let resolver = Arc::new(Resolver::new(RESOLVER_THREADS, queries_max));

        if !deny.is_empty() {
            info!("blocking outgoing connections to: {:?}", deny);
        }

        let mut workers = Vec::new();

        for i in 0..worker_count {
            let w = Worker::new(
                instance_id,
                i,
                req_maxconn / worker_count,
                stream_maxconn / worker_count,
                buffer_size,
                body_buffer_size,
                messages_max,
                req_timeout,
                stream_timeout,
                allow_compression,
                deny,
                &resolver,
                &zsockman,
                handle_bound,
            );
            workers.push(w);
        }

        Ok(Self { workers })
    }

    pub fn task_sizes() -> Vec<(String, usize)> {
        let req_task_size = {
            let reactor = Reactor::new(10);

            let (_, stop) = CancellationToken::new(&reactor.local_registration_memory());
            let (done, _) = local_channel(1, 1);
            let (sender, _) = local_channel(1, 1);

            let req_scratch_mem = Rc::new(arena::RcMemory::new(1));
            let req_req_mem = Rc::new(arena::RcMemory::new(1));
            let msg_mem = Arc::new(arena::ArcMemory::new(1));

            let scratch = arena::Rc::new(
                RefCell::new(zhttppacket::ParseScratch::new()),
                &req_scratch_mem,
            )
            .unwrap();

            let msg = concat!(
                "T161:4:from,6:client,2:id,1:1,3:seq,1:0#6:method,4:POST,3:uri",
                ",23:http://example.com/path,7:headers,34:30:12:Content-Type,1",
                "0:text/plain,]]4:body,5:hello,4:more,4:true!}",
            );

            let msg = arena::Arc::new(zmq::Message::from(msg.as_bytes()), &msg_mem).unwrap();

            let zreq = zhttppacket::OwnedRequest::parse(msg, 0, scratch).unwrap();
            let zreq = arena::Rc::new(zreq, &req_req_mem).unwrap();

            let resolver = Arc::new(Resolver::new(1, 1));

            let fut = Worker::req_connection_task(
                stop,
                done,
                0,
                0,
                None,
                (MultipartHeader::new(), zreq),
                resolver,
                Rc::new(Vec::new()),
                ConnectionOpts {
                    instance_id: Rc::new("".to_string()),
                    buffer_size: 0,
                    timeout: Duration::from_millis(0),
                    rb_tmp: Rc::new(TmpBuffer::new(1)),
                    packet_buf: Rc::new(RefCell::new(Vec::new())),
                    tmp_buf: Rc::new(RefCell::new(Vec::new())),
                },
                ConnectionReqOpts {
                    body_buffer_size: 0,
                    sender,
                },
            );

            mem::size_of_val(&fut)
        };

        let stream_task_size = {
            let reactor = Reactor::new(10);

            let (_, stop) = CancellationToken::new(&reactor.local_registration_memory());
            let (done, _) = local_channel(1, 1);
            let (_, zreceiver) = local_channel(1, 1);
            let (sender, _) = local_channel(1, 1);

            let batch = Batch::new(1);
            let conn_items = Rc::new(RefCell::new(ConnectionItems::new(1, batch)));
            let conns = Rc::new(Connections::new(conn_items.clone(), 1));

            let req_scratch_mem = Rc::new(arena::RcMemory::new(1));
            let req_req_mem = Rc::new(arena::RcMemory::new(1));
            let msg_mem = Arc::new(arena::ArcMemory::new(1));

            let scratch = arena::Rc::new(
                RefCell::new(zhttppacket::ParseScratch::new()),
                &req_scratch_mem,
            )
            .unwrap();

            let msg = concat!(
                "T161:4:from,6:client,2:id,1:1,3:seq,1:0#6:method,4:POST,3:uri",
                ",23:http://example.com/path,7:headers,34:30:12:Content-Type,1",
                "0:text/plain,]]4:body,5:hello,4:more,4:true!}",
            );

            let msg = arena::Arc::new(zmq::Message::from(msg.as_bytes()), &msg_mem).unwrap();

            let zreq = zhttppacket::OwnedRequest::parse(msg, 0, scratch).unwrap();
            let zreq = arena::Rc::new(zreq, &req_req_mem).unwrap();

            let resolver = Arc::new(Resolver::new(1, 1));

            let stream_shared_mem = Rc::new(arena::RcMemory::new(1));

            let shared = arena::Rc::new(StreamSharedData::new(), &stream_shared_mem).unwrap();

            let fut = Worker::stream_connection_task(
                stop,
                done,
                0,
                0,
                ArrayVec::new(),
                zreq,
                resolver,
                zreceiver,
                Rc::new(Vec::new()),
                conns,
                ConnectionOpts {
                    instance_id: Rc::new("".to_string()),
                    buffer_size: 0,
                    timeout: Duration::from_millis(0),
                    rb_tmp: Rc::new(TmpBuffer::new(1)),
                    packet_buf: Rc::new(RefCell::new(Vec::new())),
                    tmp_buf: Rc::new(RefCell::new(Vec::new())),
                },
                ConnectionStreamOpts {
                    messages_max: 0,
                    allow_compression: false,
                    sender,
                },
                shared,
            );

            mem::size_of_val(&fut)
        };

        let mut v = Vec::new();

        v.push(("client_req_connection_task".to_string(), req_task_size));
        v.push((
            "client_stream_connection_task".to_string(),
            stream_task_size,
        ));

        v
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        for w in self.workers.iter_mut() {
            w.stop();
        }
    }
}

#[derive(Debug, Eq, PartialEq)]
enum StatusMessage {
    Started,
    ReqFinished,
    StreamFinished,
}

enum ControlMessage {
    Stop,
    Req(zmq::Message),
    Stream(zmq::Message),
}

pub struct TestClient {
    _client: Client,
    thread: Option<thread::JoinHandle<()>>,
    status: channel::Receiver<StatusMessage>,
    control: channel::Sender<ControlMessage>,
    next_id: Cell<u64>,
}

impl TestClient {
    pub fn new(workers: usize) -> Self {
        let zmq_context = Arc::new(zmq::Context::new());

        let req_maxconn = 100;
        let stream_maxconn = 100;

        let maxconn = req_maxconn + stream_maxconn;

        let mut zsockman = zhttpsocket::ServerSocketManager::new(
            Arc::clone(&zmq_context),
            "test",
            (MSG_RETAINED_PER_CONNECTION_MAX * maxconn) + (MSG_RETAINED_PER_WORKER_MAX * workers),
            100,
            100,
        );

        zsockman
            .set_server_req_specs(&vec![SpecInfo {
                spec: String::from("inproc://client-test"),
                bind: true,
                ipc_file_mode: 0,
            }])
            .unwrap();

        let zsockman = Arc::new(zsockman);

        let client = Client::new(
            "test",
            workers,
            req_maxconn,
            stream_maxconn,
            1024,
            1024,
            10,
            Duration::from_secs(5),
            Duration::from_secs(5),
            false,
            &[],
            zsockman.clone(),
            100,
        )
        .unwrap();

        zsockman
            .set_server_stream_specs(
                &vec![SpecInfo {
                    spec: String::from("inproc://client-test-out"),
                    bind: true,
                    ipc_file_mode: 0,
                }],
                &vec![SpecInfo {
                    spec: String::from("inproc://client-test-out-stream"),
                    bind: true,
                    ipc_file_mode: 0,
                }],
                &vec![SpecInfo {
                    spec: String::from("inproc://client-test-in"),
                    bind: true,
                    ipc_file_mode: 0,
                }],
            )
            .unwrap();

        let (status_s, status_r) = channel::channel(1);
        let (control_s, control_r) = channel::channel(1000);

        let thread = thread::spawn(move || {
            Self::run(status_s, control_r, zmq_context);
        });

        // wait for handler thread to start
        assert_eq!(status_r.recv().unwrap(), StatusMessage::Started);

        Self {
            _client: client,
            thread: Some(thread),
            status: status_r,
            control: control_s,
            next_id: Cell::new(0),
        }
    }

    pub fn do_req(&self, addr: std::net::SocketAddr) {
        let msg = self.make_req_message(addr).unwrap();

        self.control.send(ControlMessage::Req(msg)).unwrap();
    }

    pub fn do_stream_http(&self, addr: std::net::SocketAddr) {
        let msg = self.make_stream_message(addr, false).unwrap();

        self.control.send(ControlMessage::Stream(msg)).unwrap();
    }

    pub fn do_stream_ws(&self, addr: std::net::SocketAddr) {
        let msg = self.make_stream_message(addr, true).unwrap();

        self.control.send(ControlMessage::Stream(msg)).unwrap();
    }

    pub fn wait_req(&self) {
        assert_eq!(self.status.recv().unwrap(), StatusMessage::ReqFinished);
    }

    pub fn wait_stream(&self) {
        assert_eq!(self.status.recv().unwrap(), StatusMessage::StreamFinished);
    }

    fn make_req_message(&self, addr: std::net::SocketAddr) -> Result<zmq::Message, io::Error> {
        let mut dest = [0; 1024];

        let mut cursor = io::Cursor::new(&mut dest[..]);

        cursor.write(b"T")?;

        let mut w = tnetstring::Writer::new(&mut cursor);

        w.start_map()?;

        let mut tmp = [0u8; 1024];

        let id = {
            let id = self.next_id.get();
            self.next_id.set(id + 1);

            let mut cursor = io::Cursor::new(&mut tmp[..]);
            write!(&mut cursor, "{}", id)?;
            let pos = cursor.position() as usize;

            &tmp[..pos]
        };

        w.write_string(b"id")?;
        w.write_string(id)?;

        w.write_string(b"method")?;
        w.write_string(b"GET")?;

        let mut tmp = [0u8; 1024];

        let uri = {
            let mut cursor = io::Cursor::new(&mut tmp[..]);
            write!(&mut cursor, "http://{}/path", addr)?;
            let pos = cursor.position() as usize;

            &tmp[..pos]
        };

        w.write_string(b"uri")?;
        w.write_string(uri)?;

        w.end_map()?;

        w.flush()?;

        let size = cursor.position() as usize;

        Ok(zmq::Message::from(&dest[..size]))
    }

    fn make_stream_message(
        &self,
        addr: std::net::SocketAddr,
        ws: bool,
    ) -> Result<zmq::Message, io::Error> {
        let mut dest = [0; 1024];

        let mut cursor = io::Cursor::new(&mut dest[..]);

        cursor.write(b"T")?;

        let mut w = tnetstring::Writer::new(&mut cursor);

        w.start_map()?;

        w.write_string(b"from")?;
        w.write_string(b"handler")?;

        let mut tmp = [0u8; 1024];

        let id = {
            let id = self.next_id.get();
            self.next_id.set(id + 1);

            let mut cursor = io::Cursor::new(&mut tmp[..]);
            write!(&mut cursor, "{}", id)?;
            let pos = cursor.position() as usize;

            &tmp[..pos]
        };

        w.write_string(b"id")?;
        w.write_string(id)?;

        w.write_string(b"seq")?;
        w.write_int(0)?;

        let mut tmp = [0u8; 1024];

        let uri = if ws {
            let mut cursor = io::Cursor::new(&mut tmp[..]);
            write!(&mut cursor, "ws://{}/path", addr)?;
            let pos = cursor.position() as usize;

            &tmp[..pos]
        } else {
            w.write_string(b"method")?;
            w.write_string(b"GET")?;

            let mut cursor = io::Cursor::new(&mut tmp[..]);
            write!(&mut cursor, "http://{}/path", addr)?;
            let pos = cursor.position() as usize;

            &tmp[..pos]
        };

        w.write_string(b"uri")?;
        w.write_string(uri)?;

        w.write_string(b"credits")?;
        w.write_int(1024)?;

        w.end_map()?;

        w.flush()?;

        let size = cursor.position() as usize;

        Ok(zmq::Message::from(&dest[..size]))
    }

    fn respond_msg(
        id: &[u8],
        seq: u32,
        ptype: &str,
        content_type: &str,
        body: &[u8],
        code: Option<u16>,
    ) -> Result<zmq::Message, io::Error> {
        let mut dest = [0; 1024];

        let mut cursor = io::Cursor::new(&mut dest[..]);

        cursor.write(b"T")?;

        let mut w = tnetstring::Writer::new(&mut cursor);

        w.start_map()?;

        w.write_string(b"from")?;
        w.write_string(b"handler")?;

        w.write_string(b"id")?;
        w.write_string(id)?;

        w.write_string(b"seq")?;
        w.write_int(seq as isize)?;

        if ptype.is_empty() {
            w.write_string(b"content-type")?;
            w.write_string(content_type.as_bytes())?;
        } else {
            w.write_string(b"type")?;
            w.write_string(ptype.as_bytes())?;
        }

        if let Some(x) = code {
            w.write_string(b"code")?;
            w.write_int(x as isize)?;
        }

        w.write_string(b"body")?;
        w.write_string(body)?;

        w.end_map()?;

        w.flush()?;

        let size = cursor.position() as usize;

        Ok(zmq::Message::from(&dest[..size]))
    }

    fn run(
        status: channel::Sender<StatusMessage>,
        control: channel::Receiver<ControlMessage>,
        zmq_context: Arc<zmq::Context>,
    ) {
        let req_sock = zmq_context.socket(zmq::DEALER).unwrap();
        req_sock.connect("inproc://client-test").unwrap();

        let out_sock = zmq_context.socket(zmq::PUSH).unwrap();
        out_sock.connect("inproc://client-test-out").unwrap();

        let out_stream_sock = zmq_context.socket(zmq::ROUTER).unwrap();
        out_stream_sock
            .connect("inproc://client-test-out-stream")
            .unwrap();

        let in_sock = zmq_context.socket(zmq::SUB).unwrap();
        in_sock.set_subscribe(b"handler ").unwrap();
        in_sock.connect("inproc://client-test-in").unwrap();

        // ensure zsockman is subscribed
        thread::sleep(Duration::from_millis(100));

        status.send(StatusMessage::Started).unwrap();

        let mut poller = event::Poller::new(1).unwrap();

        poller
            .register_custom(
                control.get_read_registration(),
                mio::Token(1),
                mio::Interest::READABLE,
            )
            .unwrap();

        poller
            .register(
                &mut SourceFd(&req_sock.get_fd().unwrap()),
                mio::Token(2),
                mio::Interest::READABLE,
            )
            .unwrap();

        poller
            .register(
                &mut SourceFd(&in_sock.get_fd().unwrap()),
                mio::Token(3),
                mio::Interest::READABLE,
            )
            .unwrap();

        let mut req_events = req_sock.get_events().unwrap();

        let mut in_events = in_sock.get_events().unwrap();

        loop {
            while req_events.contains(zmq::POLLIN) {
                let parts = match req_sock.recv_multipart(zmq::DONTWAIT) {
                    Ok(parts) => parts,
                    Err(zmq::Error::EAGAIN) => {
                        break;
                    }
                    Err(e) => panic!("recv error: {:?}", e),
                };

                req_events = req_sock.get_events().unwrap();

                assert_eq!(parts.len(), 2);

                let msg = &parts[1];
                assert_eq!(msg[0], b'T');

                let mut code: u16 = 0;
                let mut reason = "";
                let mut body = b"".as_slice();

                for f in tnetstring::parse_map(&msg[1..]).unwrap() {
                    let f = f.unwrap();

                    match f.key {
                        "code" => {
                            let x = tnetstring::parse_int(&f.data).unwrap();
                            code = x as u16;
                        }
                        "reason" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            reason = str::from_utf8(s).unwrap();
                        }
                        "body" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            body = s;
                        }
                        _ => {}
                    }
                }

                assert_eq!(code, 200);
                assert_eq!(reason, "OK");
                assert_eq!(str::from_utf8(body).unwrap(), "hello\n");

                status.send(StatusMessage::ReqFinished).unwrap();
            }

            while in_events.contains(zmq::POLLIN) {
                let parts = match in_sock.recv_multipart(zmq::DONTWAIT) {
                    Ok(parts) => parts,
                    Err(zmq::Error::EAGAIN) => {
                        break;
                    }
                    Err(e) => panic!("recv error: {:?}", e),
                };

                in_events = in_sock.get_events().unwrap();

                assert_eq!(parts.len(), 1);

                let buf = &parts[0];

                let mut pos = None;
                for (i, b) in buf.iter().enumerate() {
                    if *b == b' ' {
                        pos = Some(i);
                        break;
                    }
                }

                let pos = pos.unwrap();
                let msg = &buf[(pos + 1)..];

                assert_eq!(msg[0], b'T');

                let mut id = "";
                let mut seq = None;
                let mut ptype = "";
                let mut code = None;
                let mut reason = "";
                let mut content_type = "";
                let mut body = &b""[..];
                let mut more = false;

                for f in tnetstring::parse_map(&msg[1..]).unwrap() {
                    let f = f.unwrap();

                    match f.key {
                        "id" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            id = str::from_utf8(s).unwrap();
                        }
                        "seq" => {
                            let x = tnetstring::parse_int(&f.data).unwrap();
                            seq = Some(x as u32);
                        }
                        "type" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            ptype = str::from_utf8(s).unwrap();
                        }
                        "code" => {
                            let x = tnetstring::parse_int(&f.data).unwrap();
                            code = Some(x as u16);
                        }
                        "reason" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            reason = str::from_utf8(s).unwrap();
                        }
                        "content-type" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            content_type = str::from_utf8(s).unwrap();
                        }
                        "body" => {
                            let s = tnetstring::parse_string(&f.data).unwrap();
                            body = s;
                        }
                        "more" => {
                            let b = tnetstring::parse_bool(&f.data).unwrap();
                            more = b;
                        }
                        _ => {}
                    }
                }

                let seq = seq.unwrap() + 1;

                // as a hack to make the test server stateless, respond to every message
                // using the received sequence number. for messages we don't care about,
                // respond with keep-alive in order to keep the sequencing going
                if ptype.is_empty() || ptype == "ping" || ptype == "pong" || ptype == "close" {
                    if ptype.is_empty() && content_type.is_empty() {
                        // assume http/ws accept, or http body

                        if !reason.is_empty() {
                            // http/ws accept

                            let code = code.unwrap();
                            assert!(code == 200 || code == 101);

                            if code == 200 {
                                assert_eq!(reason, "OK");
                                assert_eq!(body.len(), 0);
                                assert_eq!(more, true);
                            } else {
                                // 101
                                assert_eq!(reason, "Switching Protocols");
                                assert_eq!(body.len(), 0);
                                assert_eq!(more, false);
                            }

                            let msg =
                                Self::respond_msg(id.as_bytes(), seq, "keep-alive", "", b"", None)
                                    .unwrap();
                            out_stream_sock
                                .send_multipart(
                                    [
                                        zmq::Message::from(b"test".as_slice()),
                                        zmq::Message::new(),
                                        msg,
                                    ],
                                    0,
                                )
                                .unwrap();
                        } else {
                            // http body

                            assert_eq!(str::from_utf8(body).unwrap(), "hello\n");
                            assert_eq!(more, false);

                            status.send(StatusMessage::StreamFinished).unwrap();
                        }
                    } else {
                        // assume ws message

                        if ptype == "ping" {
                            ptype = "pong";
                        }

                        // echo
                        let msg =
                            Self::respond_msg(id.as_bytes(), seq, ptype, content_type, body, code)
                                .unwrap();
                        out_stream_sock
                            .send_multipart(
                                [
                                    zmq::Message::from(b"test".as_slice()),
                                    zmq::Message::new(),
                                    msg,
                                ],
                                0,
                            )
                            .unwrap();

                        if ptype == "close" {
                            status.send(StatusMessage::StreamFinished).unwrap();
                        }
                    }
                } else {
                    let msg =
                        Self::respond_msg(id.as_bytes(), seq, "keep-alive", "", b"", None).unwrap();
                    out_stream_sock
                        .send_multipart(
                            [
                                zmq::Message::from(b"test".as_slice()),
                                zmq::Message::new(),
                                msg,
                            ],
                            0,
                        )
                        .unwrap();
                }
            }

            poller.poll(None).unwrap();

            let mut done = false;

            for event in poller.iter_events() {
                match event.token() {
                    mio::Token(1) => match control.try_recv() {
                        Ok(ControlMessage::Stop) => {
                            done = true;
                            break;
                        }
                        Ok(ControlMessage::Req(msg)) => {
                            req_sock
                                .send_multipart([zmq::Message::new(), msg], 0)
                                .unwrap();
                            req_events = req_sock.get_events().unwrap();
                        }
                        Ok(ControlMessage::Stream(msg)) => out_sock.send(msg, 0).unwrap(),
                        Err(_) => {}
                    },
                    mio::Token(2) => req_events = req_sock.get_events().unwrap(),
                    mio::Token(3) => in_events = in_sock.get_events().unwrap(),
                    _ => unreachable!(),
                }
            }

            if done {
                break;
            }
        }
    }
}

impl Drop for TestClient {
    fn drop(&mut self) {
        self.control.try_send(ControlMessage::Stop).unwrap();

        let thread = self.thread.take().unwrap();
        thread.join().unwrap();
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use crate::connection::calculate_ws_accept;
    use crate::websocket;
    use std::io::Read;
    use test_log::test;

    fn recv_frame<R: Read>(
        stream: &mut R,
        buf: &mut Vec<u8>,
    ) -> Result<(bool, u8, Vec<u8>), io::Error> {
        loop {
            let fi = match websocket::read_header(buf) {
                Ok(fi) => fi,
                Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => {
                    let mut chunk = [0; 1024];

                    let size = stream.read(&mut chunk)?;
                    if size == 0 {
                        return Err(io::Error::from(io::ErrorKind::UnexpectedEof));
                    }

                    buf.extend_from_slice(&chunk[..size]);
                    continue;
                }
                Err(e) => return Err(e),
            };

            while buf.len() < fi.payload_offset + fi.payload_size {
                let mut chunk = [0; 1024];

                let size = stream.read(&mut chunk)?;
                if size == 0 {
                    return Err(io::Error::from(io::ErrorKind::UnexpectedEof));
                }

                buf.extend_from_slice(&chunk[..size]);
            }

            let mut content =
                Vec::from(&buf[fi.payload_offset..(fi.payload_offset + fi.payload_size)]);

            if let Some(mask) = fi.mask {
                websocket::apply_mask(&mut content, mask, 0);
            }

            *buf = buf.split_off(fi.payload_offset + fi.payload_size);

            return Ok((fi.fin, fi.opcode, content));
        }
    }

    #[test]
    fn test_batch() {
        let mut batch = Batch::new(3);

        assert_eq!(batch.capacity(), 3);
        assert_eq!(batch.len(), 0);
        assert_eq!(batch.last_group_ckeys(), &[]);

        assert!(batch.add(b"addr-a", 1).is_ok());
        assert!(batch.add(b"addr-a", 2).is_ok());
        assert!(batch.add(b"addr-b", 3).is_ok());
        assert_eq!(batch.len(), 3);

        assert!(batch.add(b"addr-c", 4).is_err());
        assert_eq!(batch.len(), 3);
        assert_eq!(batch.is_empty(), false);

        let ids = ["id-1", "id-2", "id-3"];

        let group = batch
            .take_group(|ckey| (ids[ckey - 1].as_bytes(), 0))
            .unwrap();
        assert_eq!(group.ids().len(), 2);
        assert_eq!(group.ids()[0].id, b"id-1");
        assert_eq!(group.ids()[0].seq, Some(0));
        assert_eq!(group.ids()[1].id, b"id-2");
        assert_eq!(group.ids()[1].seq, Some(0));
        assert_eq!(group.addr(), b"addr-a");
        drop(group);
        assert_eq!(batch.is_empty(), false);
        assert_eq!(batch.last_group_ckeys(), &[1, 2]);

        let group = batch
            .take_group(|ckey| (ids[ckey - 1].as_bytes(), 0))
            .unwrap();
        assert_eq!(group.ids().len(), 1);
        assert_eq!(group.ids()[0].id, b"id-3");
        assert_eq!(group.ids()[0].seq, Some(0));
        assert_eq!(group.addr(), b"addr-b");
        drop(group);
        assert_eq!(batch.is_empty(), true);
        assert_eq!(batch.last_group_ckeys(), &[3]);

        assert!(batch
            .take_group(|ckey| { (ids[ckey - 1].as_bytes(), 0) })
            .is_none());
        assert_eq!(batch.last_group_ckeys(), &[3]);
    }

    #[test]
    fn test_client() {
        let client = TestClient::new(1);

        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();

        // req

        client.do_req(addr);
        let (mut stream, _) = listener.accept().unwrap();

        let mut buf = Vec::new();
        let mut req_end = 0;

        while req_end == 0 {
            let mut chunk = [0; 1024];
            let size = stream.read(&mut chunk).unwrap();
            buf.extend_from_slice(&chunk[..size]);

            for i in 0..(buf.len() - 3) {
                if &buf[i..(i + 4)] == b"\r\n\r\n" {
                    req_end = i + 4;
                    break;
                }
            }
        }

        let expected = format!(
            concat!("GET /path HTTP/1.1\r\n", "Host: {}\r\n", "\r\n"),
            addr
        );

        assert_eq!(str::from_utf8(&buf[..req_end]).unwrap(), expected);

        stream
            .write(
                b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 6\r\n\r\nhello\n",
            )
            .unwrap();
        drop(stream);

        client.wait_req();

        // stream (http)

        client.do_stream_http(addr);
        let (mut stream, _) = listener.accept().unwrap();

        let mut buf = Vec::new();
        let mut req_end = 0;

        while req_end == 0 {
            let mut chunk = [0; 1024];
            let size = stream.read(&mut chunk).unwrap();
            buf.extend_from_slice(&chunk[..size]);

            for i in 0..(buf.len() - 3) {
                if &buf[i..(i + 4)] == b"\r\n\r\n" {
                    req_end = i + 4;
                    break;
                }
            }
        }

        let expected = format!(
            concat!("GET /path HTTP/1.1\r\n", "Host: {}\r\n", "\r\n"),
            addr
        );

        assert_eq!(str::from_utf8(&buf[..req_end]).unwrap(), expected);

        stream
            .write(
                b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 6\r\n\r\nhello\n",
            )
            .unwrap();
        drop(stream);

        client.wait_stream();

        // stream (ws)

        client.do_stream_ws(addr);
        let (mut stream, _) = listener.accept().unwrap();

        let mut buf = Vec::new();
        let mut req_end = 0;

        while req_end == 0 {
            let mut chunk = [0; 1024];
            let size = stream.read(&mut chunk).unwrap();
            buf.extend_from_slice(&chunk[..size]);

            for i in 0..(buf.len() - 3) {
                if &buf[i..(i + 4)] == b"\r\n\r\n" {
                    req_end = i + 4;
                    break;
                }
            }
        }

        let req_buf = &buf[..req_end];

        // use httparse to fish out Sec-WebSocket-Key
        let ws_key = {
            let mut headers = [httparse::EMPTY_HEADER; 32];

            let mut req = httparse::Request::new(&mut headers);

            match req.parse(req_buf) {
                Ok(httparse::Status::Complete(_)) => {}
                _ => panic!("unexpected parse status"),
            }

            let mut ws_key = String::new();

            for h in req.headers {
                if h.name.eq_ignore_ascii_case("Sec-WebSocket-Key") {
                    ws_key = String::from_utf8(h.value.to_vec()).unwrap();
                }
            }

            ws_key
        };

        let expected = format!(
            concat!(
                "GET /path HTTP/1.1\r\n",
                "Host: {}\r\n",
                "Upgrade: websocket\r\n",
                "Connection: Upgrade\r\n",
                "Sec-WebSocket-Version: 13\r\n",
                "Sec-WebSocket-Key: {}\r\n",
                "\r\n"
            ),
            addr, ws_key,
        );

        assert_eq!(str::from_utf8(&buf[..req_end]).unwrap(), expected);

        buf = buf.split_off(req_end);

        let ws_accept = calculate_ws_accept(ws_key.as_bytes()).unwrap();

        let resp_data = format!(
            concat!(
                "HTTP/1.1 101 Switching Protocols\r\n",
                "Upgrade: websocket\r\n",
                "Connection: Upgrade\r\n",
                "Sec-WebSocket-Accept: {}\r\n",
                "\r\n",
            ),
            ws_accept
        );

        stream.write(resp_data.as_bytes()).unwrap();

        // send message

        let mut data = vec![0; 1024];
        let body = &b"hello"[..];
        let size = websocket::write_header(
            true,
            false,
            websocket::OPCODE_TEXT,
            body.len(),
            None,
            &mut data,
        )
        .unwrap();
        data[size..(size + body.len())].copy_from_slice(body);
        stream.write(&data[..(size + body.len())]).unwrap();

        // recv message

        let (fin, opcode, content) = recv_frame(&mut stream, &mut buf).unwrap();
        assert_eq!(fin, true);
        assert_eq!(opcode, websocket::OPCODE_TEXT);
        assert_eq!(str::from_utf8(&content).unwrap(), "hello");
    }

    #[test]
    fn test_ws() {
        let client = TestClient::new(1);

        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();

        client.do_stream_ws(addr);
        let (mut stream, _) = listener.accept().unwrap();

        let mut buf = Vec::new();
        let mut req_end = 0;

        while req_end == 0 {
            let mut chunk = [0; 1024];
            let size = stream.read(&mut chunk).unwrap();
            buf.extend_from_slice(&chunk[..size]);

            for i in 0..(buf.len() - 3) {
                if &buf[i..(i + 4)] == b"\r\n\r\n" {
                    req_end = i + 4;
                    break;
                }
            }
        }

        let req_buf = &buf[..req_end];

        // use httparse to fish out Sec-WebSocket-Key
        let ws_key = {
            let mut headers = [httparse::EMPTY_HEADER; 32];

            let mut req = httparse::Request::new(&mut headers);

            match req.parse(req_buf) {
                Ok(httparse::Status::Complete(_)) => {}
                _ => panic!("unexpected parse status"),
            }

            let mut ws_key = String::new();

            for h in req.headers {
                if h.name.eq_ignore_ascii_case("Sec-WebSocket-Key") {
                    ws_key = String::from_utf8(h.value.to_vec()).unwrap();
                }
            }

            ws_key
        };

        let expected = format!(
            concat!(
                "GET /path HTTP/1.1\r\n",
                "Host: {}\r\n",
                "Upgrade: websocket\r\n",
                "Connection: Upgrade\r\n",
                "Sec-WebSocket-Version: 13\r\n",
                "Sec-WebSocket-Key: {}\r\n",
                "\r\n"
            ),
            addr, ws_key,
        );

        assert_eq!(str::from_utf8(&buf[..req_end]).unwrap(), expected);

        buf = buf.split_off(req_end);

        let ws_accept = calculate_ws_accept(ws_key.as_bytes()).unwrap();

        let resp_data = format!(
            concat!(
                "HTTP/1.1 101 Switching Protocols\r\n",
                "Upgrade: websocket\r\n",
                "Connection: Upgrade\r\n",
                "Sec-WebSocket-Accept: {}\r\n",
                "\r\n",
            ),
            ws_accept
        );

        stream.write(resp_data.as_bytes()).unwrap();

        // send binary

        let mut data = vec![0; 1024];
        let body = &[1, 2, 3][..];
        let size = websocket::write_header(
            true,
            false,
            websocket::OPCODE_BINARY,
            body.len(),
            None,
            &mut data,
        )
        .unwrap();
        data[size..(size + body.len())].copy_from_slice(body);
        stream.write(&data[..(size + body.len())]).unwrap();

        // recv binary

        let (fin, opcode, content) = recv_frame(&mut stream, &mut buf).unwrap();
        assert_eq!(fin, true);
        assert_eq!(opcode, websocket::OPCODE_BINARY);
        assert_eq!(content, &[1, 2, 3][..]);

        buf.clear();

        // send ping

        let mut data = vec![0; 1024];
        let body = &b""[..];
        let size = websocket::write_header(
            true,
            false,
            websocket::OPCODE_PING,
            body.len(),
            None,
            &mut data,
        )
        .unwrap();
        stream.write(&data[..size]).unwrap();

        // recv pong

        let (fin, opcode, content) = recv_frame(&mut stream, &mut buf).unwrap();
        assert_eq!(fin, true);
        assert_eq!(opcode, websocket::OPCODE_PONG);
        assert_eq!(str::from_utf8(&content).unwrap(), "");

        buf.clear();

        // send close

        let mut data = vec![0; 1024];
        let body = &b"\x03\xf0gone"[..];
        let size = websocket::write_header(
            true,
            false,
            websocket::OPCODE_CLOSE,
            body.len(),
            None,
            &mut data,
        )
        .unwrap();
        data[size..(size + body.len())].copy_from_slice(body);
        stream.write(&data[..(size + body.len())]).unwrap();

        // recv close

        let (fin, opcode, content) = recv_frame(&mut stream, &mut buf).unwrap();
        assert_eq!(fin, true);
        assert_eq!(opcode, websocket::OPCODE_CLOSE);
        assert_eq!(&content, &b"\x03\xf0gone"[..]);

        // expect tcp close

        let mut chunk = [0; 1024];
        let size = stream.read(&mut chunk).unwrap();
        assert_eq!(size, 0);

        client.wait_stream();
    }

    #[cfg(target_arch = "x86_64")]
    #[cfg(debug_assertions)]
    #[test]
    fn test_task_sizes() {
        // sizes in debug mode at commit e7af23368a69998b595716d9ca74ce2f812929ad
        const REQ_TASK_SIZE_BASE: usize = 6496;
        const STREAM_TASK_SIZE_BASE: usize = 7880;

        // cause tests to fail if sizes grow too much
        const GROWTH_LIMIT: usize = 1000;
        const REQ_TASK_SIZE_MAX: usize = REQ_TASK_SIZE_BASE + GROWTH_LIMIT;
        const STREAM_TASK_SIZE_MAX: usize = STREAM_TASK_SIZE_BASE + GROWTH_LIMIT;

        let sizes = Client::task_sizes();

        assert_eq!(sizes[0].0, "client_req_connection_task");
        assert!(sizes[0].1 <= REQ_TASK_SIZE_MAX);

        assert_eq!(sizes[1].0, "client_stream_connection_task");
        assert!(sizes[1].1 <= STREAM_TASK_SIZE_MAX);
    }
}
