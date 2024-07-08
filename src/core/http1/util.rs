/*
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

use crate::core::buffer::{Buffer, VecRingBuffer};
use crate::core::io::{AsyncRead, AsyncReadExt};
use std::cmp;
use std::future::Future;
use std::io;
use std::pin::Pin;
use std::task::{Context, Poll};

// some reasonable number
pub const HEADERS_MAX: usize = 64;

// return the capacity increase
pub fn resize_write_buffer_if_full<F>(
    buf: &mut VecRingBuffer,
    block_size: usize,
    blocks_max: usize,
    mut reserve: F,
) -> usize
where
    F: FnMut() -> bool,
{
    assert!(blocks_max >= 2);

    // all but one block can be used for writing
    let allowed = blocks_max - 1;

    if buf.remaining_capacity() == 0
        && buf.capacity() < block_size.checked_mul(allowed).unwrap()
        && reserve()
    {
        buf.resize(buf.capacity() + block_size);

        block_size
    } else {
        0
    }
}

pub async fn recv_nonzero<R: AsyncRead>(
    r: &mut R,
    buf: &mut VecRingBuffer,
) -> Result<(), io::Error> {
    if buf.remaining_capacity() == 0 {
        return Err(io::Error::from(io::ErrorKind::WriteZero));
    }

    let size = match r.read(buf.write_buf()).await {
        Ok(size) => size,
        Err(e) => return Err(e),
    };

    buf.write_commit(size);

    if size == 0 {
        return Err(io::Error::from(io::ErrorKind::UnexpectedEof));
    }

    Ok(())
}

pub struct LimitedRingBuffer<'a> {
    pub inner: &'a mut VecRingBuffer,
    pub limit: usize,
}

impl AsRef<[u8]> for LimitedRingBuffer<'_> {
    fn as_ref(&self) -> &[u8] {
        let buf = Buffer::read_buf(self.inner);
        let limit = cmp::min(buf.len(), self.limit);

        &buf[..limit]
    }
}

pub struct AsyncOperation<O, C>
where
    C: FnMut(),
{
    op_fn: O,
    cancel_fn: C,
}

impl<O, C, R> AsyncOperation<O, C>
where
    O: FnMut(&mut Context) -> Option<R>,
    C: FnMut(),
{
    pub fn new(op_fn: O, cancel_fn: C) -> Self {
        Self { op_fn, cancel_fn }
    }
}

impl<O, C, R> Future for AsyncOperation<O, C>
where
    O: FnMut(&mut Context) -> Option<R> + Unpin,
    C: FnMut() + Unpin,
{
    type Output = R;

    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let s = Pin::into_inner(self);

        match (s.op_fn)(cx) {
            Some(ret) => Poll::Ready(ret),
            None => Poll::Pending,
        }
    }
}

impl<O, C> Drop for AsyncOperation<O, C>
where
    C: FnMut(),
{
    fn drop(&mut self) {
        (self.cancel_fn)();
    }
}

pub enum SendStatus<T, P, E> {
    Complete(T),
    EarlyResponse(T),
    Partial(P, usize),
    Error(P, E),
}

pub enum RecvStatus<T, C> {
    Read(T, usize),
    Complete(C, usize),
}
