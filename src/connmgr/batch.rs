/*
 * Copyright (C) 2020-2023 Fanout, Inc.
 * Copyright (C) 2023-2026 Fastly, Inc.
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
use crate::core::list::{SlabList, SlabNode};
use crate::core::memorypool;
use arrayvec::ArrayVec;
use slab::Slab;
use std::convert::TryFrom;

pub struct BatchKey {
    addr_index: usize,
    nkey: usize,
}

pub struct BatchGroup<'a> {
    addr: &'a [u8],
    use_router: bool,
    removed: &'a [(usize, bool)],
}

impl BatchGroup<'_> {
    pub fn addr(&self) -> &[u8] {
        self.addr
    }

    pub fn use_router(&self) -> bool {
        self.use_router
    }

    /// Returns slice of (ckey, included).
    pub fn removed(&self) -> &[(usize, bool)] {
        self.removed
    }
}

pub struct BatchGroupWithIds<'a, 'b> {
    inner: BatchGroup<'a>,
    ids: memorypool::ReusableVecHandle<'b, zhttppacket::Id<'b>>,
}

impl<'a, 'b> BatchGroupWithIds<'a, 'b> {
    pub fn addr(&self) -> &[u8] {
        self.inner.addr()
    }

    pub fn use_router(&self) -> bool {
        self.inner.use_router()
    }

    /// Returns slice of (ckey, included).
    #[cfg(test)]
    pub fn removed(&self) -> &[(usize, bool)] {
        &self.inner.removed()
    }

    pub fn ids(&self) -> &[zhttppacket::Id<'b>] {
        &self.ids
    }

    pub fn discard_ids(self) -> BatchGroup<'a> {
        self.inner
    }
}

struct AddrItem {
    addr: ArrayVec<u8, FROM_MAX>,
    use_router: bool,
    keys: SlabList<usize>,
}

pub struct Batch {
    nodes: Slab<SlabNode<usize>>,
    addrs: Vec<AddrItem>,
    addr_index: usize,
    group_ids: memorypool::ReusableVec,
    group_removed: Vec<(usize, bool)>,
}

impl Batch {
    pub fn new(capacity: usize) -> Self {
        Self {
            nodes: Slab::with_capacity(capacity),
            addrs: Vec::with_capacity(capacity),
            addr_index: 0,
            group_ids: memorypool::ReusableVec::new::<zhttppacket::Id>(capacity),
            group_removed: Vec::with_capacity(capacity),
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
                keys: SlabList::default(),
            });
        } else {
            // adding not allowed if take_group() has already moved past the index
            if pos < self.addr_index {
                return Err(());
            }
        }

        let nkey = self.nodes.insert(SlabNode::new(ckey));
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

    /// Returns a set of IDs for connections in the batch that have the same
    /// peer. The caller can then easily send a single packet addressed to
    /// all of them. The caller should repeatedly call `take_group` until all
    /// the connections are drained from the batch. Returns None when there
    /// are no connections in the batch.
    ///
    /// This method works by removing connections from the batch one at a
    /// time, and calling the `include` function for each one with its ckey.
    /// If `include` returns Some((ID, seq)), then it is included in the
    /// returned set of IDs. If it returns None, then the connection is
    /// excluded from the set.
    ///
    /// If the batch has connections and `include` returns None for all of
    /// them, then this method will return an empty set of IDs.
    pub fn take_group<'a: 'b, 'b, F>(&'a mut self, include: F) -> Option<BatchGroupWithIds<'a, 'b>>
    where
        F: Fn(usize) -> Option<(&'b [u8], u32)>,
    {
        let addrs = &mut self.addrs;
        let mut ids = self.group_ids.get_as_new();

        self.group_removed.clear();

        while ids.is_empty() {
            // Find the next addr with items
            while self.addr_index < addrs.len() && addrs[self.addr_index].keys.is_empty() {
                self.addr_index += 1;
            }

            // If all are empty, we're done
            if self.addr_index == addrs.len() {
                assert!(self.nodes.is_empty());
                break;
            }

            let keys = &mut addrs[self.addr_index].keys;

            // Get ids/seqs
            while ids.len() < zhttppacket::IDS_MAX {
                let nkey = match keys.pop_front(&mut self.nodes) {
                    Some(nkey) => nkey,
                    None => break,
                };

                let ckey = self.nodes[nkey].value;

                let included = if let Some((id, seq)) = include(ckey) {
                    ids.push(zhttppacket::Id { id, seq: Some(seq) });

                    true
                } else {
                    false
                };

                self.nodes.remove(nkey);
                self.group_removed.push((ckey, included));
            }
        }

        if self.group_removed.is_empty() {
            assert!(ids.is_empty());
            return None;
        }

        let (addr, use_router): (&[u8], bool) = if !ids.is_empty() {
            let ai = &addrs[self.addr_index];

            (&ai.addr, ai.use_router)
        } else {
            (b"", false)
        };

        Some(BatchGroupWithIds {
            inner: BatchGroup {
                addr,
                use_router,
                removed: &self.group_removed,
            },
            ids,
        })
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
        assert_eq!(group.removed(), &[(1, true), (2, true)]);
        drop(group);
        assert_eq!(batch.is_empty(), false);

        let group = batch
            .take_group(|ckey| Some((ids[ckey - 1].as_bytes(), 0)))
            .unwrap();
        assert_eq!(group.ids().len(), 1);
        assert_eq!(group.ids()[0].id, b"id-3");
        assert_eq!(group.ids()[0].seq, Some(0));
        assert_eq!(group.addr(), b"addr-b");
        assert!(!group.use_router());
        assert_eq!(group.removed(), &[(3, true)]);
        drop(group);
        assert_eq!(batch.is_empty(), false);

        let group = batch
            .take_group(|ckey| Some((ids[ckey - 1].as_bytes(), 0)))
            .unwrap();
        assert_eq!(group.ids().len(), 1);
        assert_eq!(group.ids()[0].id, b"id-4");
        assert_eq!(group.ids()[0].seq, Some(0));
        assert_eq!(group.addr(), b"addr-b");
        assert!(group.use_router());
        assert_eq!(group.removed(), &[(4, true)]);
        drop(group);
        assert_eq!(batch.is_empty(), true);

        assert!(batch
            .take_group(|ckey| Some((ids[ckey - 1].as_bytes(), 0)))
            .is_none());
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
        assert_eq!(group.removed(), &[(2, true)]);
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
        assert_eq!(group.removed(), &[(3, true)]);
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
        assert_eq!(group.removed(), &[(1, false), (2, false), (3, true)]);
        drop(group);
        assert_eq!(batch.is_empty(), true);
    }
}
