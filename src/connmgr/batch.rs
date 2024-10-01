/*
 * Copyright (C) 2020-2023 Fanout, Inc.
 * Copyright (C) 2023-2024 Fastly, Inc.
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

use crate::connmgr::zhttppacket;
use crate::connmgr::zhttpsocket::FROM_MAX;
use crate::core::arena;
use crate::core::list;
use arrayvec::ArrayVec;
use slab::Slab;
use std::convert::TryFrom;

pub struct BatchKey {
    addr_index: usize,
    nkey: usize,
}

pub struct BatchGroup<'a, 'b> {
    addr: &'b [u8],
    use_router: bool,
    ids: arena::ReusableVecHandle<'b, zhttppacket::Id<'a>>,
}

impl<'a> BatchGroup<'a, '_> {
    pub fn addr(&self) -> &[u8] {
        self.addr
    }

    #[allow(dead_code)]
    pub fn use_router(&self) -> bool {
        self.use_router
    }

    pub fn ids(&self) -> &[zhttppacket::Id<'a>] {
        &self.ids
    }
}

struct AddrItem {
    addr: ArrayVec<u8, FROM_MAX>,
    use_router: bool,
    keys: list::List,
}

pub struct Batch {
    nodes: Slab<list::Node<usize>>,
    addrs: Vec<AddrItem>,
    addr_index: usize,
    group_ids: arena::ReusableVec,
    last_group_ckeys: Vec<usize>,
}

impl Batch {
    pub fn new(capacity: usize) -> Self {
        Self {
            nodes: Slab::with_capacity(capacity),
            addrs: Vec::with_capacity(capacity),
            addr_index: 0,
            group_ids: arena::ReusableVec::new::<zhttppacket::Id>(capacity),
            last_group_ckeys: Vec::with_capacity(capacity),
        }
    }

    pub fn len(&self) -> usize {
        self.nodes.len()
    }

    pub fn capacity(&self) -> usize {
        self.nodes.capacity()
    }

    pub fn is_empty(&self) -> bool {
        self.nodes.is_empty()
    }

    pub fn clear(&mut self) {
        self.addrs.clear();
        self.nodes.clear();
        self.addr_index = 0;
    }

    pub fn add(&mut self, to_addr: &[u8], use_router: bool, ckey: usize) -> Result<BatchKey, ()> {
        if self.nodes.len() == self.nodes.capacity() {
            return Err(());
        }

        // if all existing nodes have been removed via remove() or take_group(),
        // such that is_empty() returns true, start clean
        if self.nodes.is_empty() {
            self.addrs.clear();
            self.addr_index = 0;
        }

        let mut pos = self.addrs.len();

        for (n, ai) in self.addrs.iter().enumerate() {
            if ai.addr.as_slice() == to_addr && ai.use_router == use_router {
                pos = n;
            }
        }

        if pos == self.addrs.len() {
            if self.addrs.len() == self.addrs.capacity() {
                return Err(());
            }

            // connection limits to_addr to FROM_MAX so this is guaranteed to succeed
            let addr = ArrayVec::try_from(to_addr).unwrap();

            self.addrs.push(AddrItem {
                addr,
                use_router,
                keys: list::List::default(),
            });
        } else {
            // adding not allowed if take_group() has already moved past the index
            if pos < self.addr_index {
                return Err(());
            }
        }

        let nkey = self.nodes.insert(list::Node::new(ckey));
        self.addrs[pos].keys.push_back(&mut self.nodes, nkey);

        Ok(BatchKey {
            addr_index: pos,
            nkey,
        })
    }

    pub fn remove(&mut self, key: BatchKey) {
        self.addrs[key.addr_index]
            .keys
            .remove(&mut self.nodes, key.nkey);
        self.nodes.remove(key.nkey);
    }

    pub fn take_group<'a, 'b: 'a, F>(&'a mut self, get_id: F) -> Option<BatchGroup>
    where
        F: Fn(usize) -> Option<(&'b [u8], u32)>,
    {
        let addrs = &mut self.addrs;
        let mut ids = self.group_ids.get_as_new();

        while ids.is_empty() {
            // find the next addr with items
            while self.addr_index < addrs.len() && addrs[self.addr_index].keys.is_empty() {
                self.addr_index += 1;
            }

            // if all are empty, we're done
            if self.addr_index == addrs.len() {
                assert!(self.nodes.is_empty());
                return None;
            }

            let keys = &mut addrs[self.addr_index].keys;

            self.last_group_ckeys.clear();
            ids.clear();

            // get ids/seqs
            while ids.len() < zhttppacket::IDS_MAX {
                let nkey = match keys.pop_front(&mut self.nodes) {
                    Some(nkey) => nkey,
                    None => break,
                };

                let ckey = self.nodes[nkey].value;
                self.nodes.remove(nkey);

                if let Some((id, seq)) = get_id(ckey) {
                    self.last_group_ckeys.push(ckey);
                    ids.push(zhttppacket::Id { id, seq: Some(seq) });
                }
            }
        }

        let ai = &addrs[self.addr_index];

        Some(BatchGroup {
            addr: &ai.addr,
            use_router: ai.use_router,
            ids,
        })
    }

    pub fn last_group_ckeys(&self) -> &[usize] {
        &self.last_group_ckeys
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn add_take() {
        let ids = ["id-1", "id-2", "id-3", "id-4"];
        let mut batch = Batch::new(4);

        assert_eq!(batch.capacity(), 4);
        assert_eq!(batch.len(), 0);
        assert!(batch.last_group_ckeys().is_empty());

        assert!(batch.add(b"addr-a", false, 1).is_ok());
        assert!(batch.add(b"addr-a", false, 2).is_ok());
        assert!(batch.add(b"addr-b", false, 3).is_ok());
        assert!(batch.add(b"addr-b", true, 4).is_ok());
        assert_eq!(batch.len(), 4);

        assert!(batch.add(b"addr-c", false, 5).is_err());
        assert_eq!(batch.len(), 4);
        assert_eq!(batch.is_empty(), false);

        let group = batch
            .take_group(|ckey| Some((ids[ckey - 1].as_bytes(), 0)))
            .unwrap();
        assert_eq!(group.ids().len(), 2);
        assert_eq!(group.ids()[0].id, b"id-1");
        assert_eq!(group.ids()[0].seq, Some(0));
        assert_eq!(group.ids()[1].id, b"id-2");
        assert_eq!(group.ids()[1].seq, Some(0));
        assert_eq!(group.addr(), b"addr-a");
        assert!(!group.use_router());
        drop(group);
        assert_eq!(batch.is_empty(), false);
        assert_eq!(batch.last_group_ckeys(), &[1, 2]);

        let group = batch
            .take_group(|ckey| Some((ids[ckey - 1].as_bytes(), 0)))
            .unwrap();
        assert_eq!(group.ids().len(), 1);
        assert_eq!(group.ids()[0].id, b"id-3");
        assert_eq!(group.ids()[0].seq, Some(0));
        assert_eq!(group.addr(), b"addr-b");
        assert!(!group.use_router());
        drop(group);
        assert_eq!(batch.is_empty(), false);
        assert_eq!(batch.last_group_ckeys(), &[3]);

        let group = batch
            .take_group(|ckey| Some((ids[ckey - 1].as_bytes(), 0)))
            .unwrap();
        assert_eq!(group.ids().len(), 1);
        assert_eq!(group.ids()[0].id, b"id-4");
        assert_eq!(group.ids()[0].seq, Some(0));
        assert_eq!(group.addr(), b"addr-b");
        assert!(group.use_router());
        drop(group);
        assert_eq!(batch.is_empty(), true);
        assert_eq!(batch.last_group_ckeys(), &[4]);

        assert!(batch
            .take_group(|ckey| Some((ids[ckey - 1].as_bytes(), 0)))
            .is_none());
        assert_eq!(batch.last_group_ckeys(), &[4]);
    }

    #[test]
    fn add_remove_take() {
        let ids = ["id-1", "id-2", "id-3"];
        let mut batch = Batch::new(3);

        let bkey = batch.add(b"addr-a", false, 1).unwrap();
        assert!(batch.add(b"addr-b", false, 2).is_ok());
        assert_eq!(batch.len(), 2);
        batch.remove(bkey);
        assert_eq!(batch.len(), 1);

        let group = batch
            .take_group(|ckey| Some((ids[ckey - 1].as_bytes(), 0)))
            .unwrap();
        assert_eq!(group.ids().len(), 1);
        assert_eq!(group.ids()[0].id, b"id-2");
        assert_eq!(group.ids()[0].seq, Some(0));
        assert_eq!(group.addr(), b"addr-b");
        drop(group);
        assert_eq!(batch.is_empty(), true);

        assert!(batch.add(b"addr-a", false, 3).is_ok());
        assert_eq!(batch.len(), 1);
        assert!(!batch.is_empty());

        let group = batch
            .take_group(|ckey| Some((ids[ckey - 1].as_bytes(), 0)))
            .unwrap();
        assert_eq!(group.ids().len(), 1);
        assert_eq!(group.ids()[0].id, b"id-3");
        assert_eq!(group.ids()[0].seq, Some(0));
        assert_eq!(group.addr(), b"addr-a");
        drop(group);
        assert_eq!(batch.is_empty(), true);
    }

    #[test]
    fn add_take_omit() {
        let ids = ["id-1", "id-2", "id-3"];
        let mut batch = Batch::new(3);

        assert!(batch.add(b"addr-a", false, 1).is_ok());
        assert!(batch.add(b"addr-b", false, 2).is_ok());
        assert!(batch.add(b"addr-b", false, 3).is_ok());

        let group = batch
            .take_group(|ckey| {
                if ckey < 3 {
                    None
                } else {
                    Some((ids[ckey - 1].as_bytes(), 0))
                }
            })
            .unwrap();
        assert_eq!(group.ids().len(), 1);
        assert_eq!(group.ids()[0].id, b"id-3");
        assert_eq!(group.ids()[0].seq, Some(0));
        assert_eq!(group.addr(), b"addr-b");
        drop(group);
        assert_eq!(batch.is_empty(), true);
    }
}
