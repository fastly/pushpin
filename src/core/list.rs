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

use crate::core::memorypool;
use slab::Slab;
use std::cell::{Cell, Ref, RefCell, RefMut};
use std::fmt;
use std::marker::PhantomData;
use std::rc::Rc;

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

    #[track_caller]
    fn take_link(
        &mut self,
        index: <Self::Index as Index>::Ref<'_>,
        rel: Relation,
    ) -> Option<Self::Index>;

    fn with_borrow<F, R>(&self, index: <Self::Index as Index>::Ref<'_>, f: F) -> R
    where
        F: for<'a> FnOnce(&'a Self::Value) -> R;

    fn with_borrow_mut<F, R>(&mut self, index: <Self::Index as Index>::Ref<'_>, f: F) -> R
    where
        F: for<'a> FnOnce(&'a mut Self::Value) -> R;
}

/// A linked list with arbitrary node storage. Instances are generic over a
/// provided `Backend` which supplies node management behavior. The API is
/// designed to allow for storing nodes in a single `Slab` (using `usize` for
/// node references) or for storing nodes in individual ref-counted
/// instances.
///
/// For storing nodes in a single `Slab`, `Backend` is implemented on `Slab`,
/// so a `Slab` can be used as a backend directly, providing both the node
/// management behavior as well as the storage. Notably, this backend allows
/// the list to be `Send`.
///
/// For storing nodes in individual ref-counted instances, there's
/// `RcBackend` which is a zero-sized type providing the node management
/// behavior based on `Rc`. It doesn't need to provide storage since the
/// nodes themselves provide their own storage.
#[derive(Clone)]
pub struct List<B: Backend> {
    head: Option<B::Index>,
    tail: Option<B::Index>,
}

impl<B: Backend> List<B> {
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

    #[track_caller]
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

    pub fn iter<'a>(&self, backend: &'a B) -> ListIterator<'a, B> {
        ListIterator {
            backend,
            next: self.head.clone(),
        }
    }
}

impl<B: Backend> Default for List<B> {
    fn default() -> Self {
        Self {
            head: None,
            tail: None,
        }
    }
}

pub struct ListIterator<'a, B: Backend> {
    backend: &'a B,
    next: Option<B::Index>,
}

impl<B: Backend> Iterator for ListIterator<'_, B> {
    type Item = B::Index;

    fn next(&mut self) -> Option<Self::Item> {
        let next = self.next.take()?;

        self.next = self.backend.clone_link(next.to_ref(), Relation::Next);

        Some(next)
    }
}

/// Convenience wrapper around `List` and a backend, obviating the need to
/// pass the backend to every method call.
pub struct BoundList<B: Backend> {
    inner: List<B>,
    backend: B,
}

impl<B: Backend> BoundList<B> {
    pub fn is_empty(&self) -> bool {
        self.inner.is_empty()
    }

    pub fn head(&self) -> Option<<B::Index as Index>::Ref<'_>> {
        self.inner.head()
    }

    pub fn tail(&self) -> Option<<B::Index as Index>::Ref<'_>> {
        self.inner.tail()
    }

    pub fn insert(&mut self, index: B::Index, after: Option<<B::Index as Index>::Ref<'_>>) {
        self.inner.insert(&mut self.backend, index, after)
    }

    #[track_caller]
    pub fn remove(&mut self, index: <B::Index as Index>::Ref<'_>) {
        self.inner.remove(&mut self.backend, index)
    }

    pub fn pop_front(&mut self) -> Option<B::Index> {
        self.inner.pop_front(&mut self.backend)
    }

    pub fn push_back(&mut self, index: B::Index) {
        self.inner.push_back(&mut self.backend, index)
    }

    pub fn concat(&mut self, other: &mut Self) {
        self.inner.concat(&mut self.backend, &mut other.inner)
    }

    pub fn iter(&self) -> ListIterator<'_, B> {
        self.inner.iter(&self.backend)
    }
}

impl<B: Backend + Default> Default for BoundList<B> {
    fn default() -> Self {
        Self {
            inner: Default::default(),
            backend: B::default(),
        }
    }
}

impl<B: Backend + Clone> Clone for BoundList<B> {
    fn clone(&self) -> Self {
        Self {
            inner: self.inner.clone(),
            backend: self.backend.clone(),
        }
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

    fn with_borrow<F, R>(&self, index: <Self::Index as Index>::Ref<'_>, f: F) -> R
    where
        F: for<'a> FnOnce(&'a Self::Value) -> R,
    {
        f(&self[index].value)
    }

    fn with_borrow_mut<F, R>(&mut self, index: <Self::Index as Index>::Ref<'_>, f: F) -> R
    where
        F: for<'a> FnOnce(&'a mut Self::Value) -> R,
    {
        f(&mut self[index].value)
    }
}

pub type SlabList<T> = List<Slab<SlabNode<T>>>;

pub struct NodeData<T> {
    pub prev: Cell<Option<RcNode<T>>>,
    pub next: Cell<Option<RcNode<T>>>,
    pub value: RefCell<T>,
}

pub type NodeMemory<T> = memorypool::RcMemory<NodeData<T>>;

pub struct RcNode<T>(memorypool::Rc<NodeData<T>>);

impl<T> RcNode<T> {
    pub fn new(value: T, memory: Option<&Rc<NodeMemory<T>>>) -> Self {
        let data = NodeData {
            prev: Cell::new(None),
            next: Cell::new(None),
            value: RefCell::new(value),
        };

        let inner = match memory {
            Some(m) if m.len() < m.capacity() => memorypool::Rc::try_new_in(data, m).unwrap(),
            _ => memorypool::Rc::new(data),
        };

        Self(inner)
    }

    pub fn prev(&self) -> &Cell<Option<RcNode<T>>> {
        &self.0.prev
    }

    pub fn next(&self) -> &Cell<Option<RcNode<T>>> {
        &self.0.next
    }

    pub fn value(&self) -> Ref<'_, T> {
        self.0.value.borrow()
    }

    pub fn value_mut(&self) -> RefMut<'_, T> {
        self.0.value.borrow_mut()
    }

    pub fn ptr_eq(this: &Self, other: &Self) -> bool {
        memorypool::Rc::ptr_eq(&this.0, &other.0)
    }
}

impl<T> Clone for RcNode<T> {
    fn clone(&self) -> Self {
        Self(memorypool::Rc::clone(&self.0))
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

pub struct RcBackend<T>(PhantomData<T>);

impl<T> Default for RcBackend<T> {
    fn default() -> Self {
        Self(PhantomData)
    }
}

impl<T> Copy for RcBackend<T> {}

impl<T> Clone for RcBackend<T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> Backend for RcBackend<T> {
    type Value = T;
    type Index = RcNode<Self::Value>;

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

    fn with_borrow<F, R>(&self, index: <Self::Index as Index>::Ref<'_>, f: F) -> R
    where
        F: for<'a> FnOnce(&'a Self::Value) -> R,
    {
        f(&index.value())
    }

    fn with_borrow_mut<F, R>(&mut self, index: <Self::Index as Index>::Ref<'_>, f: F) -> R
    where
        F: for<'a> FnOnce(&'a mut Self::Value) -> R,
    {
        f(&mut index.value_mut())
    }
}

pub type RcList<T> = BoundList<RcBackend<T>>;

#[cfg(test)]
mod tests {
    use super::*;

    fn generic_push_pop<B, A>(mut b: B, mut l: List<B>, add: A)
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

    fn generic_remove<B, A>(mut b: B, mut l: List<B>, add: A)
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

    fn generic_concat<B, A>(mut be: B, mut a: List<B>, mut b: List<B>, add: A)
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

    fn generic_iter<B, A>(mut b: B, mut l: List<B>, add: A)
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
        let l = List::default();
        generic_push_pop(nodes, l, |nodes, v| nodes.insert(SlabNode::new(v)));
    }

    #[test]
    fn test_slab_remove() {
        let nodes = Slab::new();
        let l = List::default();
        generic_remove(nodes, l, |nodes, v| nodes.insert(SlabNode::new(v)));
    }

    #[test]
    fn test_slab_concat() {
        let nodes = Slab::new();
        let a = List::default();
        let b = List::default();
        generic_concat(nodes, a, b, |nodes, v| nodes.insert(SlabNode::new(v)));
    }

    #[test]
    fn test_slab_iter() {
        let nodes = Slab::new();
        let l = List::default();
        generic_iter(nodes, l, |nodes, v| nodes.insert(SlabNode::new(v)));
    }

    #[test]
    fn test_rc_push_pop() {
        let b = RcBackend::default();
        let l = List::default();
        generic_push_pop(b, l, move |_, v| RcNode::new(v, None));
    }

    #[test]
    fn test_rc_remove() {
        let b = RcBackend::default();
        let l = List::default();
        generic_remove(b, l, move |_, v| RcNode::new(v, None));
    }

    #[test]
    fn test_rc_concat() {
        let be = RcBackend::default();
        let a = List::default();
        let b = List::default();
        generic_concat(be, a, b, move |_, v| RcNode::new(v, None));
    }

    #[test]
    fn test_rc_iter() {
        let b = RcBackend::default();
        let l = List::default();
        generic_iter(b, l, move |_, v| RcNode::new(v, None));
    }
}
