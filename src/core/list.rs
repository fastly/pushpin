/*
 * Copyright (C) 2020-2023 Fanout, Inc.
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

use crate::core::arena;
use slab::Slab;
use std::cell::{Cell, Ref, RefCell, RefMut};
use std::fmt;
use std::marker::PhantomData;
use std::ops::IndexMut;
use std::rc::Rc;

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
            value,
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
        self.head.is_none()
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
            // Nothing to do
            return;
        }

        // Other is non-empty so this is guaranteed to succeed
        let hkey = other.head.unwrap();

        let next = nodes[hkey].next;

        // Since we're inserting after the tail, this will set next=None
        self.insert(nodes, self.tail, hkey);

        // Restore the node's next key
        nodes[hkey].next = next;

        self.tail = other.tail;

        other.head = None;
        other.tail = None;
    }

    pub fn iter<'a, T, S>(&self, nodes: &'a S) -> ListIterator<'a, S>
    where
        S: IndexMut<usize, Output = Node<T>>,
    {
        ListIterator {
            nodes,
            next: self.head,
        }
    }
}

pub struct ListIterator<'a, S> {
    nodes: &'a S,
    next: Option<usize>,
}

impl<'a, T, S> Iterator for ListIterator<'a, S>
where
    T: 'a,
    S: IndexMut<usize, Output = Node<T>>,
{
    type Item = (usize, &'a T);

    fn next(&mut self) -> Option<Self::Item> {
        if let Some(nkey) = self.next.take() {
            let n = &self.nodes[nkey];
            self.next = n.next;

            Some((nkey, &n.value))
        } else {
            None
        }
    }
}

pub enum Relation {
    Prev,
    Next,
}

pub trait Index {
    type Ref<'a>: Clone + Copy + PartialEq + fmt::Debug
    where
        Self: 'a;

    fn to_ref(&self) -> Self::Ref<'_>;
    fn from_ref(r: Self::Ref<'_>) -> Self;
}

pub trait Backend {
    type Value;
    type Index: Index + Clone + PartialEq + fmt::Debug;
    type Borrow<'a>
    where
        Self::Value: 'a,
        Self: 'a;
    type BorrowMut<'a>
    where
        Self::Value: 'a,
        Self: 'a;

    fn index_eq(
        this: <Self::Index as Index>::Ref<'_>,
        other: <Self::Index as Index>::Ref<'_>,
    ) -> bool;

    fn clone_link(
        &self,
        index: <Self::Index as Index>::Ref<'_>,
        rel: Relation,
    ) -> Option<Self::Index>;

    fn set_link(
        &mut self,
        index: <Self::Index as Index>::Ref<'_>,
        rel: Relation,
        value: Option<Self::Index>,
    );

    fn take_link(
        &mut self,
        index: <Self::Index as Index>::Ref<'_>,
        rel: Relation,
    ) -> Option<Self::Index>;

    fn borrow<'a, 'b: 'a>(&'b self, index: <Self::Index as Index>::Ref<'a>) -> Self::Borrow<'a>;

    fn borrow_mut<'a, 'b: 'a>(
        &'b mut self,
        index: <Self::Index as Index>::Ref<'a>,
    ) -> Self::BorrowMut<'a>;
}

pub struct GenericList<B: Backend> {
    head: Option<B::Index>,
    tail: Option<B::Index>,
}

impl<B: Backend> GenericList<B> {
    pub fn is_empty(&self) -> bool {
        self.head.is_none()
    }

    pub fn head(&self) -> Option<<B::Index as Index>::Ref<'_>> {
        self.head.as_ref().map(|i| i.to_ref())
    }

    pub fn tail(&self) -> Option<<B::Index as Index>::Ref<'_>> {
        self.tail.as_ref().map(|i| i.to_ref())
    }

    pub fn insert(
        &mut self,
        backend: &mut B,
        index: B::Index,
        after: Option<<B::Index as Index>::Ref<'_>>,
    ) {
        if let Some(after) = after {
            let next = backend.take_link(after, Relation::Next);
            backend.set_link(index.to_ref(), Relation::Next, next.clone());

            backend.set_link(after, Relation::Next, Some(index.clone()));

            if let Some(next) = next {
                let prev = backend.take_link(next.to_ref(), Relation::Prev);
                backend.set_link(index.to_ref(), Relation::Prev, prev);

                backend.set_link(next.to_ref(), Relation::Prev, Some(index));
            } else {
                backend.set_link(index.to_ref(), Relation::Prev, Some(Index::from_ref(after)));

                self.tail = Some(index);
            }
        } else {
            if let Some(next) = self.head.take() {
                backend.set_link(next.to_ref(), Relation::Prev, Some(index.clone()));

                backend.set_link(index.to_ref(), Relation::Next, Some(next));
            } else {
                backend.set_link(index.to_ref(), Relation::Next, None);

                self.tail = Some(index.clone());
            }

            self.head = Some(index);
        }
    }

    pub fn remove(&mut self, backend: &mut B, index: <B::Index as Index>::Ref<'_>) {
        let prev = backend.take_link(index, Relation::Prev);
        let next = backend.take_link(index, Relation::Next);

        if let Some(prev) = &prev {
            backend.set_link(prev.to_ref(), Relation::Next, next.clone());
        }

        if let Some(next) = &next {
            backend.set_link(next.to_ref(), Relation::Prev, prev.clone());
        }

        if let Some(head) = &self.head {
            if B::index_eq(head.to_ref(), index) {
                self.head = next;
            }
        }

        if let Some(tail) = &self.tail {
            if B::index_eq(tail.to_ref(), index) {
                self.tail = prev;
            }
        }
    }

    pub fn pop_front(&mut self, backend: &mut B) -> Option<B::Index> {
        let index = self.head.take()?;

        let next = backend.take_link(index.to_ref(), Relation::Next);

        if let Some(next) = &next {
            backend.set_link(next.to_ref(), Relation::Prev, None);
        } else {
            self.tail = None;
        }

        backend.set_link(index.to_ref(), Relation::Prev, None);

        self.head = next;

        Some(index)
    }

    pub fn push_back(&mut self, backend: &mut B, index: B::Index) {
        if let Some(after) = self.tail.take() {
            backend.set_link(after.to_ref(), Relation::Next, Some(index.clone()));

            backend.set_link(index.to_ref(), Relation::Prev, Some(after));
            backend.set_link(index.to_ref(), Relation::Next, None);

            self.tail = Some(index);
        } else {
            self.insert(backend, index, None);
        }
    }

    pub fn concat(&mut self, backend: &mut B, other: &mut Self) {
        let Some(head) = other.head.take() else {
            // Nothing to do
            return;
        };

        // Move the next link aside so push_back doesn't delete it
        let next = backend.take_link(head.to_ref(), Relation::Next);

        self.push_back(backend, head.clone());

        // Restore the node's next link
        backend.set_link(head.to_ref(), Relation::Next, next);

        self.tail = other.tail.take();
    }

    pub fn iter<'a>(&self, backend: &'a B) -> GenericListIterator<'a, B> {
        GenericListIterator {
            backend,
            next: self.head.clone(),
        }
    }
}

impl<B: Backend> Default for GenericList<B> {
    fn default() -> Self {
        Self {
            head: None,
            tail: None,
        }
    }
}

pub struct GenericListIterator<'a, B: Backend> {
    backend: &'a B,
    next: Option<B::Index>,
}

impl<B: Backend> Iterator for GenericListIterator<'_, B> {
    type Item = B::Index;

    fn next(&mut self) -> Option<Self::Item> {
        let next = self.next.take()?;

        self.next = self.backend.clone_link(next.to_ref(), Relation::Next);

        Some(next)
    }
}

pub struct SlabNode<T> {
    pub prev: Option<usize>,
    pub next: Option<usize>,
    pub value: T,
}

impl<T> SlabNode<T> {
    pub fn new(value: T) -> Self {
        Self {
            prev: None,
            next: None,
            value,
        }
    }
}

impl Index for usize {
    type Ref<'a> = usize;

    fn to_ref(&self) -> Self::Ref<'_> {
        *self
    }

    fn from_ref(r: Self::Ref<'_>) -> Self {
        r
    }
}

impl<T> Backend for Slab<SlabNode<T>> {
    type Value = T;
    type Index = usize;
    type Borrow<'a>
        = &'a Self::Value
    where
        Self::Value: 'a;
    type BorrowMut<'a>
        = &'a mut Self::Value
    where
        Self::Value: 'a;

    fn index_eq(
        this: <Self::Index as Index>::Ref<'_>,
        other: <Self::Index as Index>::Ref<'_>,
    ) -> bool {
        this == other
    }

    fn clone_link(
        &self,
        index: <Self::Index as Index>::Ref<'_>,
        rel: Relation,
    ) -> Option<Self::Index> {
        match rel {
            Relation::Prev => self[index].prev,
            Relation::Next => self[index].next,
        }
    }

    fn set_link(
        &mut self,
        index: <Self::Index as Index>::Ref<'_>,
        rel: Relation,
        value: Option<Self::Index>,
    ) {
        match rel {
            Relation::Prev => self[index].prev = value,
            Relation::Next => self[index].next = value,
        }
    }

    fn take_link(
        &mut self,
        index: <Self::Index as Index>::Ref<'_>,
        rel: Relation,
    ) -> Option<Self::Index> {
        match rel {
            Relation::Prev => self[index].prev.take(),
            Relation::Next => self[index].next.take(),
        }
    }

    fn borrow<'a, 'b: 'a>(&'b self, index: <Self::Index as Index>::Ref<'a>) -> Self::Borrow<'a> {
        &self[index].value
    }

    fn borrow_mut<'a, 'b: 'a>(
        &'b mut self,
        index: <Self::Index as Index>::Ref<'a>,
    ) -> Self::BorrowMut<'a> {
        &mut self[index].value
    }
}

pub struct NodeData<T> {
    pub prev: Cell<Option<RcNode<T>>>,
    pub next: Cell<Option<RcNode<T>>>,
    pub value: RefCell<T>,
}

pub type NodeMemory<T> = arena::RcMemory<NodeData<T>>;

pub enum RcNode<T> {
    Arena(arena::Rc<NodeData<T>>),
    Std(Rc<NodeData<T>>),
}

impl<T> RcNode<T> {
    pub fn new(value: T, memory: Option<&Rc<NodeMemory<T>>>) -> Self {
        let data = NodeData {
            prev: Cell::new(None),
            next: Cell::new(None),
            value: RefCell::new(value),
        };

        if let Some(memory) = memory {
            match arena::Rc::new(data, memory) {
                Ok(r) => Self::Arena(r),
                Err(arena::InsertError(data)) => Self::Std(Rc::new(data)),
            }
        } else {
            Self::Std(Rc::new(data))
        }
    }

    pub fn prev(&self) -> &Cell<Option<RcNode<T>>> {
        match self {
            Self::Arena(r) => &r.get().prev,
            Self::Std(r) => &r.prev,
        }
    }

    pub fn next(&self) -> &Cell<Option<RcNode<T>>> {
        match self {
            Self::Arena(r) => &r.get().next,
            Self::Std(r) => &r.next,
        }
    }

    pub fn value(&self) -> Ref<'_, T> {
        match self {
            Self::Arena(r) => r.get().value.borrow(),
            Self::Std(r) => r.value.borrow(),
        }
    }

    pub fn value_mut(&self) -> RefMut<'_, T> {
        match self {
            Self::Arena(r) => r.get().value.borrow_mut(),
            Self::Std(r) => r.value.borrow_mut(),
        }
    }

    pub fn ptr_eq(this: &Self, other: &Self) -> bool {
        match (this, other) {
            (Self::Arena(this), Self::Arena(other)) => arena::Rc::ptr_eq(this, other),
            (Self::Std(this), Self::Std(other)) => Rc::ptr_eq(this, other),
            _ => false,
        }
    }
}

impl<T> Clone for RcNode<T> {
    fn clone(&self) -> Self {
        match self {
            Self::Arena(r) => Self::Arena(arena::Rc::clone(r)),
            Self::Std(r) => Self::Std(Rc::clone(r)),
        }
    }
}

impl<T> PartialEq for RcNode<T> {
    fn eq(&self, other: &Self) -> bool {
        Self::ptr_eq(self, other)
    }
}

impl<T> fmt::Debug for RcNode<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "RcNode")
    }
}

impl<T> Index for RcNode<T> {
    type Ref<'a>
        = &'a RcNode<T>
    where
        T: 'a;

    fn to_ref(&self) -> Self::Ref<'_> {
        self
    }

    fn from_ref(r: Self::Ref<'_>) -> Self {
        r.clone()
    }
}

fn cell_get_cloned<T: Clone + Default>(c: &Cell<T>) -> T {
    let cur = c.take();
    let out = cur.clone();
    c.set(cur);

    out
}

#[derive(Default)]
pub struct RcBackend<T>(PhantomData<T>);

impl<T> Backend for RcBackend<T> {
    type Value = T;
    type Index = RcNode<Self::Value>;
    type Borrow<'a>
        = Ref<'a, Self::Value>
    where
        Self::Value: 'a;
    type BorrowMut<'a>
        = RefMut<'a, Self::Value>
    where
        Self::Value: 'a;

    fn index_eq(
        this: <Self::Index as Index>::Ref<'_>,
        other: <Self::Index as Index>::Ref<'_>,
    ) -> bool {
        this == other
    }

    fn clone_link(
        &self,
        index: <Self::Index as Index>::Ref<'_>,
        rel: Relation,
    ) -> Option<Self::Index> {
        match rel {
            Relation::Prev => cell_get_cloned(index.prev()),
            Relation::Next => cell_get_cloned(index.next()),
        }
    }

    fn set_link(
        &mut self,
        index: <Self::Index as Index>::Ref<'_>,
        rel: Relation,
        value: Option<Self::Index>,
    ) {
        match rel {
            Relation::Prev => index.prev().set(value),
            Relation::Next => index.next().set(value),
        }
    }

    fn take_link(
        &mut self,
        index: <Self::Index as Index>::Ref<'_>,
        rel: Relation,
    ) -> Option<Self::Index> {
        match rel {
            Relation::Prev => index.prev().take(),
            Relation::Next => index.next().take(),
        }
    }

    fn borrow<'a, 'b: 'a>(&'b self, index: <Self::Index as Index>::Ref<'a>) -> Self::Borrow<'a> {
        index.value()
    }

    fn borrow_mut<'a, 'b: 'a>(
        &'b mut self,
        index: <Self::Index as Index>::Ref<'a>,
    ) -> Self::BorrowMut<'a> {
        index.value_mut()
    }
}

pub type SlabList<T> = GenericList<Slab<SlabNode<T>>>;

/*#[derive(Default)]
pub struct RcList<T> {
    inner: GenericList<RcBackend<T>>,
    backend: RcBackend<T>,
}

impl<T> RcList<T> {
    pub fn is_empty(&self) -> bool {
        self.inner.is_empty()
    }

    pub fn head(&self) -> Option<<RcBackend<T>::Index as Index>::Ref<'_>> {
        self.inner.head()
    }

    pub fn tail(&self) -> Option<<RcBackend<T>::Index as Index>::Ref<'_>> {
        self.inner.tail()
    }

    pub fn insert(
        &mut self,
        index: RcBackend<T>::Index,
        after: Option<<RcBackend<T>::Index as Index>::Ref<'_>>,
    ) {
        self.inner.insert(&mut self.backend, index, after)
    }

    pub fn remove(&mut self, index: <RcBackend<T>::Index as Index>::Ref<'_>) {
        self.inner.remove(&mut self.backend, index)
    }

    pub fn pop_front(&mut self) -> Option<RcBackend<T>::Index> {
        self.inner.pop_front(&mut self.backend)
    }

    pub fn push_back(&mut self, index: RcBackend<T>::Index) {
        self.inner.push_back(&mut self.backend, index)
    }

    pub fn concat(&mut self, other: &mut Self) {
        self.inner.concat(&mut self.backend, &mut other.inner)
    }

    pub fn iter(&self) -> GenericListIterator<'_, RcBackend<T>> {
        self.inner.iter(&self.backend)
    }
}*/

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_list_push_pop() {
        let mut nodes = Slab::new();
        let n1 = nodes.insert(Node::new("n1"));
        let n2 = nodes.insert(Node::new("n2"));
        let n3 = nodes.insert(Node::new("n3"));

        // Prevent unused warning on data field
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
    fn test_list_remove() {
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

        // Already removed
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

    #[test]
    fn test_list_iter() {
        let mut nodes = Slab::new();
        let n1 = nodes.insert(Node::new("n1"));
        let n2 = nodes.insert(Node::new("n2"));
        let n3 = nodes.insert(Node::new("n3"));

        let mut l = List::default();
        l.push_back(&mut nodes, n1);
        l.push_back(&mut nodes, n2);
        l.push_back(&mut nodes, n3);

        let mut it = l.iter(&nodes);
        assert_eq!(it.next(), Some((n1, &"n1")));
        assert_eq!(it.next(), Some((n2, &"n2")));
        assert_eq!(it.next(), Some((n3, &"n3")));
        assert_eq!(it.next(), None);
    }

    fn generic_push_pop<B, A>(mut b: B, mut l: GenericList<B>, add: A)
    where
        B: Backend<Value = &'static str>,
        A: Fn(&mut B, &'static str) -> B::Index,
    {
        let n1 = add(&mut b, "n1");
        let n2 = add(&mut b, "n2");
        let n3 = add(&mut b, "n3");

        assert_eq!(b.clone_link(n1.to_ref(), Relation::Prev), None);
        assert_eq!(b.clone_link(n1.to_ref(), Relation::Next), None);
        assert_eq!(b.clone_link(n2.to_ref(), Relation::Prev), None);
        assert_eq!(b.clone_link(n2.to_ref(), Relation::Next), None);
        assert_eq!(b.clone_link(n3.to_ref(), Relation::Prev), None);
        assert_eq!(b.clone_link(n3.to_ref(), Relation::Next), None);

        assert_eq!(l.is_empty(), true);
        assert_eq!(l.head(), None);
        assert_eq!(l.tail(), None);
        assert_eq!(l.pop_front(&mut b), None);

        l.push_back(&mut b, n1.clone());
        assert_eq!(l.is_empty(), false);
        assert_eq!(l.head(), Some(n1.to_ref()));
        assert_eq!(l.tail(), Some(n1.to_ref()));
        assert_eq!(b.clone_link(n1.to_ref(), Relation::Prev), None);
        assert_eq!(b.clone_link(n1.to_ref(), Relation::Next), None);

        l.push_back(&mut b, n2.clone());
        assert_eq!(l.is_empty(), false);
        assert_eq!(l.head(), Some(n1.to_ref()));
        assert_eq!(l.tail(), Some(n2.to_ref()));
        assert_eq!(b.clone_link(n1.to_ref(), Relation::Prev), None);
        assert_eq!(b.clone_link(n1.to_ref(), Relation::Next), Some(n2.clone()));
        assert_eq!(b.clone_link(n2.to_ref(), Relation::Prev), Some(n1.clone()));
        assert_eq!(b.clone_link(n2.to_ref(), Relation::Next), None);

        l.push_back(&mut b, n3.clone());
        assert_eq!(l.is_empty(), false);
        assert_eq!(l.head(), Some(n1.to_ref()));
        assert_eq!(l.tail(), Some(n3.to_ref()));
        assert_eq!(b.clone_link(n1.to_ref(), Relation::Prev), None);
        assert_eq!(b.clone_link(n1.to_ref(), Relation::Next), Some(n2.clone()));
        assert_eq!(b.clone_link(n2.to_ref(), Relation::Prev), Some(n1.clone()));
        assert_eq!(b.clone_link(n2.to_ref(), Relation::Next), Some(n3.clone()));
        assert_eq!(b.clone_link(n3.to_ref(), Relation::Prev), Some(n2.clone()));
        assert_eq!(b.clone_link(n3.to_ref(), Relation::Next), None);

        let i = l.pop_front(&mut b);
        assert_eq!(i, Some(n1));
        assert_eq!(l.is_empty(), false);
        assert_eq!(l.head(), Some(n2.to_ref()));
        assert_eq!(l.tail(), Some(n3.to_ref()));
        assert_eq!(b.clone_link(n2.to_ref(), Relation::Prev), None);
        assert_eq!(b.clone_link(n2.to_ref(), Relation::Next), Some(n3.clone()));
        assert_eq!(b.clone_link(n3.to_ref(), Relation::Prev), Some(n2.clone()));
        assert_eq!(b.clone_link(n3.to_ref(), Relation::Next), None);

        let i = l.pop_front(&mut b);
        assert_eq!(i, Some(n2));
        assert_eq!(l.is_empty(), false);
        assert_eq!(l.head(), Some(n3.to_ref()));
        assert_eq!(l.tail(), Some(n3.to_ref()));
        assert_eq!(b.clone_link(n3.to_ref(), Relation::Prev), None);
        assert_eq!(b.clone_link(n3.to_ref(), Relation::Next), None);

        let i = l.pop_front(&mut b);
        assert_eq!(i, Some(n3));
        assert_eq!(l.is_empty(), true);
        assert_eq!(l.head(), None);
        assert_eq!(l.tail(), None);

        assert_eq!(l.pop_front(&mut b), None);
    }

    fn generic_remove<B, A>(mut b: B, mut l: GenericList<B>, add: A)
    where
        B: Backend<Value = &'static str>,
        A: Fn(&mut B, &'static str) -> B::Index,
    {
        let n1 = add(&mut b, "n1");

        assert_eq!(b.clone_link(n1.to_ref(), Relation::Prev), None);
        assert_eq!(b.clone_link(n1.to_ref(), Relation::Next), None);

        assert_eq!(l.is_empty(), true);
        assert_eq!(l.head(), None);
        assert_eq!(l.tail(), None);

        l.push_back(&mut b, n1.clone());
        assert_eq!(l.is_empty(), false);
        assert_eq!(l.head(), Some(n1.to_ref()));
        assert_eq!(l.tail(), Some(n1.to_ref()));
        assert_eq!(b.clone_link(n1.to_ref(), Relation::Prev), None);
        assert_eq!(b.clone_link(n1.to_ref(), Relation::Next), None);

        l.remove(&mut b, n1.to_ref());
        assert_eq!(l.is_empty(), true);
        assert_eq!(l.head(), None);
        assert_eq!(l.tail(), None);
        assert_eq!(b.clone_link(n1.to_ref(), Relation::Prev), None);
        assert_eq!(b.clone_link(n1.to_ref(), Relation::Next), None);

        // Already removed
        l.remove(&mut b, n1.to_ref());
        assert_eq!(l.is_empty(), true);
        assert_eq!(l.head(), None);
        assert_eq!(l.tail(), None);
        assert_eq!(b.clone_link(n1.to_ref(), Relation::Prev), None);
        assert_eq!(b.clone_link(n1.to_ref(), Relation::Next), None);
    }

    fn generic_concat<B, A>(mut be: B, mut a: GenericList<B>, mut b: GenericList<B>, add: A)
    where
        B: Backend<Value = &'static str>,
        A: Fn(&mut B, &'static str) -> B::Index,
    {
        let n1 = add(&mut be, "n1");
        let n2 = add(&mut be, "n2");

        a.concat(&mut be, &mut b);
        assert_eq!(a.is_empty(), true);
        assert_eq!(a.head(), None);
        assert_eq!(a.tail(), None);
        assert_eq!(b.is_empty(), true);
        assert_eq!(b.head(), None);
        assert_eq!(b.tail(), None);

        a.push_back(&mut be, n1.clone());
        b.push_back(&mut be, n2.clone());

        a.concat(&mut be, &mut b);
        assert_eq!(a.is_empty(), false);
        assert_eq!(a.head(), Some(n1.to_ref()));
        assert_eq!(a.tail(), Some(n2.to_ref()));
        assert_eq!(b.is_empty(), true);
        assert_eq!(b.head(), None);
        assert_eq!(b.tail(), None);
        assert_eq!(be.clone_link(n1.to_ref(), Relation::Prev), None);
        assert_eq!(be.clone_link(n1.to_ref(), Relation::Next), Some(n2.clone()));
        assert_eq!(be.clone_link(n2.to_ref(), Relation::Prev), Some(n1));
        assert_eq!(be.clone_link(n2.to_ref(), Relation::Next), None);
    }

    fn generic_iter<B, A>(mut b: B, mut l: GenericList<B>, add: A)
    where
        B: Backend<Value = &'static str>,
        A: Fn(&mut B, &'static str) -> B::Index,
    {
        let n1 = add(&mut b, "n1");
        let n2 = add(&mut b, "n2");
        let n3 = add(&mut b, "n3");

        l.push_back(&mut b, n1.clone());
        l.push_back(&mut b, n2.clone());
        l.push_back(&mut b, n3.clone());

        let mut it = l.iter(&b);
        assert_eq!(it.next(), Some(n1));
        assert_eq!(it.next(), Some(n2));
        assert_eq!(it.next(), Some(n3));
        assert_eq!(it.next(), None);
    }

    #[test]
    fn test_slab_push_pop() {
        let nodes = Slab::new();
        let l = GenericList::default();
        generic_push_pop(nodes, l, |nodes, v| nodes.insert(SlabNode::new(v)));
    }

    #[test]
    fn test_slab_remove() {
        let nodes = Slab::new();
        let l = GenericList::default();
        generic_remove(nodes, l, |nodes, v| nodes.insert(SlabNode::new(v)));
    }

    #[test]
    fn test_slab_concat() {
        let nodes = Slab::new();
        let a = GenericList::default();
        let b = GenericList::default();
        generic_concat(nodes, a, b, |nodes, v| nodes.insert(SlabNode::new(v)));
    }

    #[test]
    fn test_slab_iter() {
        let nodes = Slab::new();
        let l = GenericList::default();
        generic_iter(nodes, l, |nodes, v| nodes.insert(SlabNode::new(v)));
    }

    #[test]
    fn test_rc_push_pop() {
        let b = RcBackend::default();
        let l = GenericList::default();
        generic_push_pop(b, l, move |_, v| RcNode::new(v, None));
    }

    #[test]
    fn test_rc_remove() {
        let b = RcBackend::default();
        let l = GenericList::default();
        generic_remove(b, l, move |_, v| RcNode::new(v, None));
    }

    #[test]
    fn test_rc_concat() {
        let be = RcBackend::default();
        let a = GenericList::default();
        let b = GenericList::default();
        generic_concat(be, a, b, move |_, v| RcNode::new(v, None));
    }

    #[test]
    fn test_rc_iter() {
        let b = RcBackend::default();
        let l = GenericList::default();
        generic_iter(b, l, move |_, v| RcNode::new(v, None));
    }
}
