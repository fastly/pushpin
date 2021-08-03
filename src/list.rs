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

use std::ops::IndexMut;

pub struct Node<T> {
    pub prev: Option<usize>,
    pub next: Option<usize>,
    pub value: T,
}

impl<T> Node<T> {
    pub fn new(value: T) -> Self {
        Self {
            prev: None,
            next: None,
            value: value,
        }
    }
}

#[derive(Default, Clone, Copy)]
pub struct List {
    pub head: Option<usize>,
    pub tail: Option<usize>,
}

impl List {
    pub fn is_empty(&self) -> bool {
        !self.head.is_some()
    }

    pub fn insert<T, S>(&mut self, nodes: &mut S, after: Option<usize>, key: usize)
    where
        S: IndexMut<usize, Output = Node<T>>,
    {
        let next = if let Some(pkey) = after {
            let pn = &mut nodes[pkey];

            let next = pn.next;
            pn.next = Some(key);

            let n = &mut nodes[key];
            n.prev = Some(pkey);

            next
        } else {
            let next = self.head;
            self.head = Some(key);

            let n = &mut nodes[key];
            n.prev = None;

            next
        };

        let n = &mut nodes[key];
        n.next = next;

        if let Some(nkey) = next {
            let nn = &mut nodes[nkey];

            nn.prev = Some(key);
        } else {
            self.tail = Some(key);
        }
    }

    pub fn remove<T, S>(&mut self, nodes: &mut S, key: usize)
    where
        S: IndexMut<usize, Output = Node<T>>,
    {
        let n = &mut nodes[key];

        let prev = n.prev.take();
        let next = n.next.take();

        if let Some(pkey) = prev {
            let pn = &mut nodes[pkey];
            pn.next = next;
        }

        if let Some(nkey) = next {
            let nn = &mut nodes[nkey];
            nn.prev = prev;
        }

        if let Some(hkey) = self.head {
            if hkey == key {
                self.head = next;
            }
        }

        if let Some(tkey) = self.tail {
            if tkey == key {
                self.tail = prev;
            }
        }
    }

    pub fn pop_front<T, S>(&mut self, nodes: &mut S) -> Option<usize>
    where
        S: IndexMut<usize, Output = Node<T>>,
    {
        match self.head {
            Some(key) => {
                self.remove(nodes, key);

                Some(key)
            }
            None => None,
        }
    }

    pub fn push_back<T, S>(&mut self, nodes: &mut S, key: usize)
    where
        S: IndexMut<usize, Output = Node<T>>,
    {
        self.insert(nodes, self.tail, key);
    }

    pub fn concat<T, S>(&mut self, nodes: &mut S, other: &mut Self)
    where
        S: IndexMut<usize, Output = Node<T>>,
    {
        if other.is_empty() {
            // nothing to do
            return;
        }

        // other is non-empty so this is guaranteed to succeed
        let hkey = other.head.unwrap();

        let next = nodes[hkey].next;

        // since we're inserting after the tail, this will set next=None
        self.insert(nodes, self.tail, hkey);

        // restore the node's next key
        nodes[hkey].next = next;

        self.tail = other.tail;

        other.head = None;
        other.tail = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use slab::Slab;

    #[test]
    fn test_list_push_pop() {
        let mut nodes = Slab::new();
        let n1 = nodes.insert(Node::new("n1"));
        let n2 = nodes.insert(Node::new("n2"));
        let n3 = nodes.insert(Node::new("n3"));

        // prevent unused warning on data field
        assert_eq!(nodes[n1].value, "n1");

        assert_eq!(nodes[n1].prev, None);
        assert_eq!(nodes[n1].next, None);
        assert_eq!(nodes[n2].prev, None);
        assert_eq!(nodes[n2].next, None);
        assert_eq!(nodes[n3].prev, None);
        assert_eq!(nodes[n3].next, None);

        let mut l = List::default();
        assert_eq!(l.is_empty(), true);
        assert_eq!(l.head, None);
        assert_eq!(l.tail, None);
        assert_eq!(l.pop_front(&mut nodes), None);

        l.push_back(&mut nodes, n1);
        assert_eq!(l.is_empty(), false);
        assert_eq!(l.head, Some(n1));
        assert_eq!(l.tail, Some(n1));
        assert_eq!(nodes[n1].prev, None);
        assert_eq!(nodes[n1].next, None);

        l.push_back(&mut nodes, n2);
        assert_eq!(l.is_empty(), false);
        assert_eq!(l.head, Some(n1));
        assert_eq!(l.tail, Some(n2));
        assert_eq!(nodes[n1].prev, None);
        assert_eq!(nodes[n1].next, Some(n2));
        assert_eq!(nodes[n2].prev, Some(n1));
        assert_eq!(nodes[n2].next, None);

        l.push_back(&mut nodes, n3);
        assert_eq!(l.is_empty(), false);
        assert_eq!(l.head, Some(n1));
        assert_eq!(l.tail, Some(n3));
        assert_eq!(nodes[n1].prev, None);
        assert_eq!(nodes[n1].next, Some(n2));
        assert_eq!(nodes[n2].prev, Some(n1));
        assert_eq!(nodes[n2].next, Some(n3));
        assert_eq!(nodes[n3].prev, Some(n2));
        assert_eq!(nodes[n3].next, None);

        let key = l.pop_front(&mut nodes);
        assert_eq!(key, Some(n1));
        assert_eq!(l.is_empty(), false);
        assert_eq!(l.head, Some(n2));
        assert_eq!(l.tail, Some(n3));
        assert_eq!(nodes[n2].prev, None);
        assert_eq!(nodes[n2].next, Some(n3));
        assert_eq!(nodes[n3].prev, Some(n2));
        assert_eq!(nodes[n3].next, None);

        let key = l.pop_front(&mut nodes);
        assert_eq!(key, Some(n2));
        assert_eq!(l.is_empty(), false);
        assert_eq!(l.head, Some(n3));
        assert_eq!(l.tail, Some(n3));
        assert_eq!(nodes[n3].prev, None);
        assert_eq!(nodes[n3].next, None);

        let key = l.pop_front(&mut nodes);
        assert_eq!(key, Some(n3));
        assert_eq!(l.is_empty(), true);
        assert_eq!(l.head, None);
        assert_eq!(l.tail, None);

        assert_eq!(l.pop_front(&mut nodes), None);
    }

    #[test]
    fn test_remove() {
        let mut nodes = Slab::new();
        let n1 = nodes.insert(Node::new("n1"));

        assert_eq!(nodes[n1].prev, None);
        assert_eq!(nodes[n1].next, None);

        let mut l = List::default();
        assert_eq!(l.is_empty(), true);
        assert_eq!(l.head, None);
        assert_eq!(l.tail, None);

        l.push_back(&mut nodes, n1);
        assert_eq!(l.is_empty(), false);
        assert_eq!(l.head, Some(n1));
        assert_eq!(l.tail, Some(n1));
        assert_eq!(nodes[n1].prev, None);
        assert_eq!(nodes[n1].next, None);

        l.remove(&mut nodes, n1);
        assert_eq!(l.is_empty(), true);
        assert_eq!(l.head, None);
        assert_eq!(l.tail, None);
        assert_eq!(nodes[n1].prev, None);
        assert_eq!(nodes[n1].next, None);

        // already removed
        l.remove(&mut nodes, n1);
        assert_eq!(l.is_empty(), true);
        assert_eq!(l.head, None);
        assert_eq!(l.tail, None);
        assert_eq!(nodes[n1].prev, None);
        assert_eq!(nodes[n1].next, None);
    }

    #[test]
    fn test_list_concat() {
        let mut nodes = Slab::new();
        let n1 = nodes.insert(Node::new("n1"));
        let n2 = nodes.insert(Node::new("n2"));

        let mut a = List::default();
        let mut b = List::default();

        a.concat(&mut nodes, &mut b);
        assert_eq!(a.is_empty(), true);
        assert_eq!(a.head, None);
        assert_eq!(a.tail, None);
        assert_eq!(b.is_empty(), true);
        assert_eq!(b.head, None);
        assert_eq!(b.tail, None);

        a.push_back(&mut nodes, n1);
        b.push_back(&mut nodes, n2);

        a.concat(&mut nodes, &mut b);
        assert_eq!(a.is_empty(), false);
        assert_eq!(a.head, Some(n1));
        assert_eq!(a.tail, Some(n2));
        assert_eq!(b.is_empty(), true);
        assert_eq!(b.head, None);
        assert_eq!(b.tail, None);
        assert_eq!(nodes[n1].prev, None);
        assert_eq!(nodes[n1].next, Some(n2));
        assert_eq!(nodes[n2].prev, Some(n1));
        assert_eq!(nodes[n2].next, None);
    }
}
