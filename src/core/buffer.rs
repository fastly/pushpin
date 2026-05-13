/*
 * Copyright (C) 2020-2023 Fanout, Inc.
 * Copyright (C) 2023 Fastly, Inc.
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

use crate::core::counter::{Counter, CounterDec};
use std::cell::RefCell;
use std::cmp;
use std::io;
use std::io::Write;
use std::mem::{self, MaybeUninit};
use std::rc::Rc;
use std::slice;

#[cfg(test)]
use std::io::Read;

pub const VECTORED_MAX: usize = 8;

pub fn trim_for_display(s: &str, max: usize) -> String {
    // NOTE: O(n)
    let char_len = s.chars().count();

    if char_len > max && max >= 7 {
        let dist = max / 2;
        let mut left_end = 0;
        let mut right_start = 0;

        // NOTE: O(n)
        for (i, (pos, _)) in s.char_indices().enumerate() {
            // dist guaranteed to be < char_len
            if i == dist {
                left_end = pos;
            }

            // (char_len - dist + 3) guaranteed to be < char_len
            if i == char_len - dist + 3 {
                right_start = pos;
            }
        }

        let left = &s[..left_end];
        let right = &s[right_start..];

        format!("{}...{}", left, right)
    } else {
        s.to_owned()
    }
}

fn init_array<'a, T, const N: usize>(arr: &'a mut MaybeUninit<[T; N]>, src: &mut [T]) -> &'a mut [T]
where
    T: Default,
{
    // SAFETY: T and MaybeUninit<T> have the same layout
    let arr: &mut [MaybeUninit<T>; N] = unsafe { mem::transmute(arr) };

    let len = cmp::min(arr.len(), src.len());

    for (d, s) in arr.iter_mut().zip(src) {
        d.write(mem::take(s));
    }

    // SAFETY: the slice will contain only initialized elements
    unsafe { slice::from_raw_parts_mut(arr[0].as_mut_ptr(), len) }
}

pub trait Buffer {
    fn len(&self) -> usize;
    fn remaining_capacity(&self) -> usize;

    fn read_buf(&self) -> &[u8];
    fn read_buf_mut(&mut self) -> &mut [u8];
    fn read_commit(&mut self, amount: usize);

    fn write_buf(&mut self) -> &mut [u8];
    fn write_commit(&mut self, amount: usize);

    fn is_empty(&self) -> bool {
        self.len() == 0
    }

    fn read_bufs<'data, 'bufs>(
        &'data self,
        bufs: &'bufs mut [&'data [u8]],
    ) -> &'bufs mut [&'data [u8]] {
        if !bufs.is_empty() {
            bufs[0] = self.read_buf();

            &mut bufs[..1]
        } else {
            &mut []
        }
    }

    fn read_bufs_mut<'data, 'bufs, const N: usize>(
        &'data mut self,
        bufs: &'bufs mut MaybeUninit<[&'data mut [u8]; N]>,
    ) -> &'bufs mut [&'data mut [u8]] {
        init_array(bufs, &mut [self.read_buf_mut()])
    }
}

// For reading only
impl Buffer for io::Cursor<&mut [u8]> {
    fn len(&self) -> usize {
        Buffer::read_buf(self).len()
    }

    fn remaining_capacity(&self) -> usize {
        0
    }

    fn read_buf(&self) -> &[u8] {
        let pos = self.position() as usize;

        &self.get_ref()[pos..]
    }

    fn read_buf_mut(&mut self) -> &mut [u8] {
        let pos = self.position() as usize;

        &mut self.get_mut()[pos..]
    }

    fn read_commit(&mut self, amount: usize) {
        let pos = self.position();

        self.set_position(pos + (amount as u64));
    }

    fn write_buf(&mut self) -> &mut [u8] {
        &mut []
    }

    fn write_commit(&mut self, amount: usize) {
        assert_eq!(amount, 0);
    }
}

pub fn write_vectored_offset<W: Write>(
    writer: &mut W,
    bufs: &[&[u8]],
    offset: usize,
) -> Result<usize, io::Error> {
    if bufs.is_empty() {
        return Ok(0);
    }

    let mut offset = offset;
    let mut start = 0;

    while offset >= bufs[start].len() {
        // On the last buf?
        if start + 1 >= bufs.len() {
            // Exceeding the last buf is an error
            if offset > bufs[start].len() {
                return Err(io::Error::from(io::ErrorKind::InvalidInput));
            }

            return Ok(0);
        }

        offset -= bufs[start].len();
        start += 1;
    }

    let mut arr = [io::IoSlice::new(&b""[..]); VECTORED_MAX];
    let mut arr_len = 0;

    for (index, &buf) in bufs.iter().enumerate().skip(start) {
        let buf = if index == start { &buf[offset..] } else { buf };

        arr[arr_len] = io::IoSlice::new(buf);
        arr_len += 1;
    }

    writer.write_vectored(&arr[..arr_len])
}

struct LimitBufsRestore<T> {
    index: usize,
    ptr: T,
    len: usize,
}

pub struct LimitBufsGuard<'a, 'b> {
    bufs: &'b mut [&'a [u8]],
    start: usize,
    end: usize,
    restore: Option<LimitBufsRestore<*const u8>>,
}

impl<'a: 'b, 'b> LimitBufsGuard<'a, 'b> {
    pub fn as_slice(&self) -> &[&'a [u8]] {
        &self.bufs[self.start..self.end]
    }
}

impl<'a: 'b, 'b> Drop for LimitBufsGuard<'a, 'b> {
    fn drop(&mut self) {
        if let Some(restore) = self.restore.take() {
            // SAFETY: ptr and len were collected earlier from the original
            // memory referred to by the slice at this index and they are
            // still valid. The only issue with reconstructing the slice is
            // that we currently have a different slice using the same memory
            // at this index. However, this is safe because we also replace
            // the slice at this index and the two slices don't coexist
            unsafe {
                self.bufs[restore.index] = slice::from_raw_parts(restore.ptr, restore.len);
            }
        }
    }
}

pub struct LimitBufsMutGuard<'a, 'b> {
    bufs: &'b mut [&'a mut [u8]],
    start: usize,
    end: usize,
    restore: Option<LimitBufsRestore<*mut u8>>,
}

impl<'a: 'b, 'b> LimitBufsMutGuard<'a, 'b> {
    pub fn as_slice(&mut self) -> &mut [&'a mut [u8]] {
        &mut self.bufs[self.start..self.end]
    }
}

impl<'a: 'b, 'b> Drop for LimitBufsMutGuard<'a, 'b> {
    fn drop(&mut self) {
        if let Some(restore) = self.restore.take() {
            // SAFETY: ptr and len were collected earlier from the original
            // memory referred to by the slice at this index and they are
            // still valid. The only issue with reconstructing the slice is
            // that we currently have a different slice using the same memory
            // at this index. However, this is safe because we also replace
            // the slice at this index and the two slices don't coexist
            unsafe {
                self.bufs[restore.index] = slice::from_raw_parts_mut(restore.ptr, restore.len);
            }
        }
    }
}

pub trait LimitBufs<'a, 'b> {
    fn limit(&'b mut self, size: usize) -> LimitBufsGuard<'a, 'b>;
}

impl<'a: 'b, 'b> LimitBufs<'a, 'b> for [&'a [u8]] {
    fn limit(&'b mut self, size: usize) -> LimitBufsGuard<'a, 'b> {
        let mut end = self.len();
        let mut restore = None;
        let mut want = size;

        for (index, item) in self.iter_mut().enumerate() {
            let buf: &[u8] = item;
            let buf_len = buf.len();

            if buf_len >= want {
                let len = buf.len();
                let ptr = buf.as_ptr();

                restore = Some(LimitBufsRestore { index, ptr, len });

                // SAFETY: ptr and len were obtained above and are still
                // valid. We just need to be careful about using them again
                // later on from the restore field
                unsafe {
                    *item = &slice::from_raw_parts(ptr, len)[..want];
                }

                end = index + 1;
                break;
            }

            want -= buf_len;
        }

        LimitBufsGuard {
            bufs: self,
            start: 0,
            end,
            restore,
        }
    }
}

pub trait LimitBufsMut<'a: 'b, 'b> {
    fn skip(&'b mut self, size: usize) -> LimitBufsMutGuard<'a, 'b>;
    fn limit(&'b mut self, size: usize) -> LimitBufsMutGuard<'a, 'b>;
}

impl<'a: 'b, 'b> LimitBufsMut<'a, 'b> for [&'a mut [u8]] {
    fn skip(&'b mut self, size: usize) -> LimitBufsMutGuard<'a, 'b> {
        let mut start = 0;
        let end = self.len();
        let mut restore = None;
        let mut skip = size;

        for (index, item) in self.iter_mut().enumerate() {
            let buf: &mut [u8] = item;
            let buf_len = buf.len();

            if buf_len >= skip {
                let len = buf.len();
                let ptr = buf.as_mut_ptr();

                restore = Some(LimitBufsRestore { index, ptr, len });

                // SAFETY: ptr and len were obtained above and are still
                // valid. We just need to be careful about using them again
                // later on from the restore field
                unsafe {
                    *item = &mut slice::from_raw_parts_mut(ptr, len)[skip..];
                }

                start = index;
                break;
            }

            skip -= buf_len;
        }

        LimitBufsMutGuard {
            bufs: self,
            start,
            end,
            restore,
        }
    }

    fn limit(&'b mut self, size: usize) -> LimitBufsMutGuard<'a, 'b> {
        let mut end = self.len();
        let mut restore = None;
        let mut want = size;

        for (index, item) in self.iter_mut().enumerate() {
            let buf: &mut [u8] = item;
            let buf_len = buf.len();

            if buf_len >= want {
                let len = buf.len();
                let ptr = buf.as_mut_ptr();

                restore = Some(LimitBufsRestore { index, ptr, len });

                // SAFETY: ptr and len were obtained above and are still
                // valid. We just need to be careful about using them again
                // later on from the restore field
                unsafe {
                    *item = &mut slice::from_raw_parts_mut(ptr, len)[..want];
                }

                end = index + 1;
                break;
            }

            want -= buf_len;
        }

        LimitBufsMutGuard {
            bufs: self,
            start: 0,
            end,
            restore,
        }
    }
}

pub struct ContiguousBuffer {
    buf: Vec<u8>,
    start: usize,
    end: usize,
}

#[allow(clippy::len_without_is_empty)]
impl ContiguousBuffer {
    pub fn new(size: usize) -> Self {
        let buf = vec![0; size];

        Self {
            buf,
            start: 0,
            end: 0,
        }
    }

    pub fn clear(&mut self) {
        self.start = 0;
        self.end = 0;
    }
}

impl Buffer for ContiguousBuffer {
    fn len(&self) -> usize {
        self.end - self.start
    }

    fn remaining_capacity(&self) -> usize {
        self.buf.len() - self.end
    }

    fn read_buf(&self) -> &[u8] {
        &self.buf[self.start..self.end]
    }

    fn read_buf_mut(&mut self) -> &mut [u8] {
        &mut self.buf[self.start..self.end]
    }

    fn read_commit(&mut self, amount: usize) {
        assert!(self.start + amount <= self.end);

        self.start += amount;
    }

    fn write_buf(&mut self) -> &mut [u8] {
        let len = self.buf.len();

        &mut self.buf[self.end..len]
    }

    fn write_commit(&mut self, amount: usize) {
        assert!(self.end + amount <= self.buf.len());

        self.end += amount;
    }
}

#[cfg(test)]
impl Read for ContiguousBuffer {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, io::Error> {
        // Fully qualified to work around future method warning
        // https://github.com/rust-lang/rust/issues/48919
        let src = Buffer::read_buf(self);
        let size = cmp::min(src.len(), buf.len());

        buf[..size].copy_from_slice(&src[..size]);

        self.read_commit(size);

        Ok(size)
    }
}

impl Write for ContiguousBuffer {
    fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
        if !buf.is_empty() && self.remaining_capacity() == 0 {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        let dest = self.write_buf();
        let size = cmp::min(dest.len(), buf.len());

        dest[..size].copy_from_slice(&buf[..size]);

        self.write_commit(size);

        Ok(size)
    }

    fn flush(&mut self) -> Result<(), io::Error> {
        Ok(())
    }
}

pub struct TmpBuffer(RefCell<Vec<u8>>);

#[allow(clippy::len_without_is_empty)]
impl TmpBuffer {
    pub fn new(size: usize) -> Self {
        Self(RefCell::new(vec![0; size]))
    }

    pub fn len(&self) -> usize {
        self.0.borrow().len()
    }
}

// Holds a Vec<u8> but only exposes the portion of it considered to be
// readable ("filled"). Any remaining bytes may be zeroed or uninitialized
// and are not considered to be readable
pub struct FilledBuf {
    data: Vec<u8>,
    filled: usize,
}

impl FilledBuf {
    // panics if filled is larger than data.len()
    pub fn new(data: Vec<u8>, filled: usize) -> Self {
        assert!(filled <= data.len());

        Self { data, filled }
    }

    pub fn filled(&self) -> &[u8] {
        &self.data[..self.filled]
    }

    pub fn filled_len(&self) -> usize {
        self.filled
    }

    pub fn into_inner(self) -> Vec<u8> {
        self.data
    }
}

pub struct RingBuffer<T> {
    buf: T,
    start: usize,
    end: usize,
    tmp: Rc<TmpBuffer>,
}

#[allow(clippy::len_without_is_empty)]
impl<T: AsRef<[u8]> + AsMut<[u8]>> RingBuffer<T> {
    pub fn capacity(&self) -> usize {
        self.buf.as_ref().len()
    }

    pub fn clear(&mut self) {
        self.start = 0;
        self.end = 0;
    }

    // Return true if the readable bytes have not wrapped
    pub fn is_readable_contiguous(&self) -> bool {
        self.end <= self.buf.as_ref().len()
    }

    pub fn align(&mut self) -> usize {
        assert!(self.buf.as_ref().len() <= self.tmp.len());

        if self.start == 0 {
            return 0;
        }

        let buf = self.buf.as_mut();
        let size = self.end - self.start;

        if self.end <= buf.len() {
            // If the buffer hasn't wrapped, simply copy down
            buf.copy_within(self.start.., 0);
        } else if size <= self.start {
            // If the buffer has wrapped, but the wrapped part can be copied
            // without overlapping, then copy the wrapped part followed by
            // initial part

            let left_size = self.end - buf.len();
            let right_size = buf.len() - self.start;

            buf.copy_within(..left_size, right_size);
            buf.copy_within(self.start..(self.start + right_size), 0);
        } else {
            // If the buffer has wrapped and the wrapped part can't be copied
            // without overlapping, then use a temporary buffer to
            // facilitate. Smaller part is copied to the temp buffer, then
            // the larger and small parts (in that order) are copied into
            // their intended locations. In the worst case, up to 50% of
            // the buffer may be copied twice

            let left_size = self.end - buf.len();
            let right_size = buf.len() - self.start;

            let (lsize, lsrc, ldest, hsize, hsrc, hdest);

            if left_size < right_size {
                lsize = left_size;
                hsize = right_size;
                lsrc = 0;
                ldest = hsize;
                hsrc = self.start;
                hdest = 0;
            } else {
                lsize = right_size;
                hsize = left_size;
                lsrc = self.start;
                ldest = 0;
                hsrc = 0;
                hdest = lsize;
            }

            let mut tmp = self.tmp.0.borrow_mut();

            tmp[..lsize].copy_from_slice(&buf[lsrc..(lsrc + lsize)]);
            buf.copy_within(hsrc..(hsrc + hsize), hdest);
            buf[ldest..(ldest + lsize)].copy_from_slice(&tmp[..lsize]);
        }

        self.start = 0;
        self.end = size;

        size
    }

    pub fn get_tmp(&self) -> &Rc<TmpBuffer> {
        &self.tmp
    }
}

#[cfg(test)]
impl<T: AsRef<[u8]> + AsMut<[u8]>> Read for RingBuffer<T> {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, io::Error> {
        let mut pos = 0;

        while pos < buf.len() && self.len() > 0 {
            // Fully qualified to work around future method warning
            // https://github.com/rust-lang/rust/issues/48919
            let src = Buffer::read_buf(self);
            let size = cmp::min(src.len(), buf.len() - pos);

            buf[pos..(pos + size)].copy_from_slice(&src[..size]);

            self.read_commit(size);

            pos += size;
        }

        Ok(pos)
    }
}

impl<T: AsRef<[u8]> + AsMut<[u8]>> Write for RingBuffer<T> {
    fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
        if !buf.is_empty() && self.remaining_capacity() == 0 {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        let mut pos = 0;

        while pos < buf.len() && self.remaining_capacity() > 0 {
            let dest = self.write_buf();
            let size = cmp::min(dest.len(), buf.len() - pos);

            dest[..size].copy_from_slice(&buf[pos..(pos + size)]);

            self.write_commit(size);

            pos += size;
        }

        Ok(pos)
    }

    fn flush(&mut self) -> Result<(), io::Error> {
        Ok(())
    }
}

impl<T: AsRef<[u8]> + AsMut<[u8]>> Buffer for RingBuffer<T> {
    fn len(&self) -> usize {
        self.end - self.start
    }

    fn remaining_capacity(&self) -> usize {
        self.buf.as_ref().len() - (self.end - self.start)
    }

    fn read_buf(&self) -> &[u8] {
        let buf = self.buf.as_ref();
        let end = cmp::min(self.end, buf.len());

        &buf[self.start..end]
    }

    fn read_buf_mut(&mut self) -> &mut [u8] {
        let buf = self.buf.as_mut();
        let end = cmp::min(self.end, buf.len());

        &mut buf[self.start..end]
    }

    fn read_commit(&mut self, amount: usize) {
        assert!(self.start + amount <= self.end);

        let buf = self.buf.as_ref();

        self.start += amount;

        if self.start == self.end {
            self.start = 0;
            self.end = 0;
        } else if self.start >= buf.len() {
            self.start -= buf.len();
            self.end -= buf.len();
        }
    }

    fn write_buf(&mut self) -> &mut [u8] {
        let buf = self.buf.as_mut();

        let (start, end) = if self.end < buf.len() {
            (self.end, buf.len())
        } else {
            (self.end - buf.len(), self.start)
        };

        &mut buf[start..end]
    }

    fn write_commit(&mut self, amount: usize) {
        assert!((self.end - self.start) + amount <= self.buf.as_ref().len());

        self.end += amount;
    }

    fn read_bufs<'data, 'bufs>(
        &'data self,
        bufs: &'bufs mut [&'data [u8]],
    ) -> &'bufs mut [&'data [u8]] {
        assert!(!bufs.is_empty());

        let buf = self.buf.as_ref();
        let buf_len = buf.len();

        if self.end > buf_len && bufs.len() >= 2 {
            let (part1, part2) = buf.split_at(self.start);

            bufs[0] = part2;
            bufs[1] = &part1[..(self.end - buf_len)];

            &mut bufs[..2]
        } else {
            bufs[0] = &buf[self.start..self.end];

            &mut bufs[..1]
        }
    }

    fn read_bufs_mut<'data, 'bufs, const N: usize>(
        &'data mut self,
        bufs: &'bufs mut MaybeUninit<[&'data mut [u8]; N]>,
    ) -> &'bufs mut [&'data mut [u8]] {
        let buf = self.buf.as_mut();
        let buf_len = buf.len();

        if self.end > buf_len {
            let (part1, part2) = buf.split_at_mut(self.start);

            init_array(bufs, &mut [part2, &mut part1[..(self.end - buf_len)]])
        } else {
            init_array(bufs, &mut [&mut buf[self.start..self.end]])
        }
    }
}

impl RingBuffer<Vec<u8>> {
    pub fn new(size: usize, tmp: &Rc<TmpBuffer>) -> Self {
        assert!(size <= tmp.len());

        let buf = vec![0; size];

        Self {
            buf,
            start: 0,
            end: 0,
            tmp: Rc::clone(tmp),
        }
    }

    // Extract inner buffer, aligning it first if necessary, and replace it
    // with an empty buffer. This should be cheap if the inner buffer is
    // already aligned. Afterwards, the ringbuffer will have a capacity of
    // zero and will be essentially unusable until set_inner is called with a
    // non-empty buffer
    pub fn take_inner(&mut self) -> FilledBuf {
        self.align();

        let data = mem::take(&mut self.buf);
        let filled = self.end;
        self.end = 0;

        FilledBuf::new(data, filled)
    }

    // Replace the inner buffer. This should be cheap if the original inner
    // buffer is empty, which is the case if take_inner was called earlier.
    // panics if the new buffer is larger than the tmp buffer
    pub fn set_inner(&mut self, buf: FilledBuf) {
        let filled = buf.filled_len();
        let data = buf.into_inner();

        assert!(data.len() <= self.tmp.len());

        self.buf = data;
        self.start = 0;
        self.end = filled;
    }

    pub fn swap_inner(&mut self, other: &mut Self) {
        let buf = self.take_inner();
        self.set_inner(other.take_inner());
        other.set_inner(buf);
    }

    pub fn resize(&mut self, size: usize) {
        if size == self.buf.len() {
            return;
        }

        self.align();

        self.buf.resize(size, 0);
        self.buf.shrink_to_fit();

        self.end = cmp::min(self.end, size);
    }
}

impl<'a> RingBuffer<&'a mut [u8]> {
    pub fn new(buf: &'a mut [u8], tmp: &Rc<TmpBuffer>) -> Self {
        assert!(buf.len() <= tmp.len());

        Self {
            buf,
            start: 0,
            end: 0,
            tmp: Rc::clone(tmp),
        }
    }
}

pub type VecRingBuffer = RingBuffer<Vec<u8>>;
pub type SliceRingBuffer<'a> = RingBuffer<&'a mut [u8]>;

/// Manages buffer expansion budget for a connection.
///
/// BufferBudget provides unified memory accounting for buffer expansion across a connection.
/// It works with any VecRingBuffer, regardless of whether it's used for read or write operations.
///
/// # Features
///
/// - **Unified memory budget**: Total memory usage is capped by both local and shared limits
/// - **Block-based expansion**: All buffers grow by fixed block size increments
/// - **Automatic cleanup**: Memory blocks are automatically returned to the shared counter when dropped
/// - **Production-safe**: Constructor never panics, even when expansion blocks are exhausted
///
/// # Example
///
/// ```
/// use pushpin::core::buffer::{BufferBudget, VecRingBuffer, TmpBuffer};
/// use pushpin::core::counter::Counter;
/// use std::rc::Rc;
/// use std::io::Write;
///
/// let counter = Counter::new(100); // Shared counter with 100 excess blocks for expansion
/// let mut budget = BufferBudget::new(&counter)
///     .with_blocks_max(3)    // Local limit of 3 expansion blocks
///     .with_block_size(64);  // 64-byte block size
/// let tmp = Rc::new(TmpBuffer::new(1024));
/// let mut read_buf = VecRingBuffer::new(64, &tmp);
/// let mut write_buf = VecRingBuffer::new(64, &tmp);
///
/// // Fill buffers to capacity and expand them
/// let data = vec![b'x'; 64];
/// read_buf.write_all(&data).unwrap();
/// write_buf.write_all(&data).unwrap();
///
/// // Expand buffers as needed (works with any number of buffers)
/// budget.expand_buffer_if_needed(&mut read_buf);
/// budget.expand_buffer_if_needed(&mut write_buf);
///
/// assert_eq!(budget.blocks_used(), 2); // 2 (expansions)
///
/// // Or use defaults for simpler cases:
/// let mut simple_budget = BufferBudget::new(&counter); // No local limit, 1-byte expansion
/// simple_budget.expand_buffer_if_needed(&mut read_buf); // Expands by 1 byte if needed
/// // Blocks are automatically returned to shared counter when budget is dropped
/// ```
pub struct BufferBudget<'a> {
    blocks_avail: CounterDec<'a>,
    blocks_max: Option<usize>,
    block_size: usize,
    blocks_used: usize,
}

impl<'a> BufferBudget<'a> {
    pub fn new(blocks_avail: &'a Counter) -> Self {
        // Note: blocks_used starts at 0 and tracks only expansion blocks used, not base buffer
        // allocations. This ensures the constructor never panics due to exhausted expansion blocks,
        // allowing connections to start successfully even when the shared pool is empty. The shared
        // counter should only contain excess blocks available for expansion.
        Self {
            blocks_avail: CounterDec::new(blocks_avail),
            blocks_max: None, // No local limit by default
            block_size: 1,    // Default to 1-byte expansion
            blocks_used: 0,   // Start at 0 (tracks expansion blocks only)
        }
    }

    pub fn with_blocks_max(mut self, blocks_max: usize) -> Self {
        self.blocks_max = Some(blocks_max);
        self
    }

    pub fn with_block_size(mut self, block_size: usize) -> Self {
        self.block_size = block_size;
        self
    }

    pub fn blocks_used(&self) -> usize {
        self.blocks_used
    }

    pub fn blocks_max(&self) -> Option<usize> {
        self.blocks_max
    }

    /// Expand buffer if it is full and budget allows
    pub fn expand_buffer_if_needed(&mut self, buf: &mut VecRingBuffer) -> usize {
        let local_limit_ok = self.blocks_max.map_or(true, |max| self.blocks_used < max);

        if buf.remaining_capacity() == 0 && local_limit_ok && self.blocks_avail.dec(1).is_ok() {
            self.blocks_used += 1;
            buf.resize(buf.capacity() + self.block_size);
            return self.block_size;
        }

        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};

    #[test]
    fn test_write_vectored_offset() {
        struct MyWriter {
            bufs: Vec<String>,
        }

        impl MyWriter {
            fn new() -> Self {
                Self { bufs: Vec::new() }
            }
        }

        impl Write for MyWriter {
            fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
                self.bufs.push(String::from_utf8(buf.to_vec()).unwrap());

                Ok(buf.len())
            }

            fn write_vectored(&mut self, bufs: &[io::IoSlice]) -> Result<usize, io::Error> {
                let mut total = 0;

                for buf in bufs {
                    total += buf.len();
                    self.bufs.push(String::from_utf8(buf.to_vec()).unwrap());
                }

                Ok(total)
            }

            fn flush(&mut self) -> Result<(), io::Error> {
                Ok(())
            }
        }

        // Empty
        let mut w = MyWriter::new();
        let r = write_vectored_offset(&mut w, &[], 0);
        assert_eq!(r.unwrap(), 0);

        // Offset too large
        let mut w = MyWriter::new();
        let r = write_vectored_offset(&mut w, &[b"apple"], 6);
        assert!(r.is_err());

        // Offset too large
        let mut w = MyWriter::new();
        let r = write_vectored_offset(&mut w, &[b"apple", b"banana"], 12);
        assert!(r.is_err());

        // Nothing to write
        let mut w = MyWriter::new();
        let r = write_vectored_offset(&mut w, &[b"apple"], 5);
        assert_eq!(r.unwrap(), 0);

        let mut w = MyWriter::new();
        let r = write_vectored_offset(&mut w, &[b"apple"], 0);
        assert_eq!(r.unwrap(), 5);
        assert_eq!(w.bufs.len(), 1);
        assert_eq!(w.bufs[0], "apple");

        let mut w = MyWriter::new();
        let r = write_vectored_offset(&mut w, &[b"apple"], 3);
        assert_eq!(r.unwrap(), 2);
        assert_eq!(w.bufs.len(), 1);
        assert_eq!(w.bufs[0], "le");

        let mut w = MyWriter::new();
        let r = write_vectored_offset(&mut w, &[b"apple", b"banana"], 3);
        assert_eq!(r.unwrap(), 8);
        assert_eq!(w.bufs.len(), 2);
        assert_eq!(w.bufs[0], "le");
        assert_eq!(w.bufs[1], "banana");

        let mut w = MyWriter::new();
        let r = write_vectored_offset(&mut w, &[b"apple", b"banana"], 5);
        assert_eq!(r.unwrap(), 6);
        assert_eq!(w.bufs.len(), 1);
        assert_eq!(w.bufs[0], "banana");

        let mut w = MyWriter::new();
        let r = write_vectored_offset(&mut w, &[b"apple", b"banana"], 6);
        assert_eq!(r.unwrap(), 5);
        assert_eq!(w.bufs.len(), 1);
        assert_eq!(w.bufs[0], "anana");
    }

    #[test]
    fn test_buffer() {
        let mut b = ContiguousBuffer::new(8);

        assert_eq!(b.len(), 0);
        assert_eq!(b.remaining_capacity(), 8);

        let size = b.write(b"hello").unwrap();
        assert_eq!(size, 5);
        assert_eq!(b.len(), 5);
        assert_eq!(b.remaining_capacity(), 3);

        let size = b.write(b"world").unwrap();
        assert_eq!(size, 3);
        assert_eq!(b.len(), 8);
        assert_eq!(b.remaining_capacity(), 0);

        let mut tmp = [0; 16];
        let size = b.read(&mut tmp).unwrap();
        assert_eq!(&tmp[..size], b"hellowor");

        b.clear();
        assert_eq!(b.len(), 0);
        assert_eq!(b.remaining_capacity(), 8);
    }

    #[test]
    fn test_ringbuffer() {
        let mut buf = [0u8; 8];

        let tmp = Rc::new(TmpBuffer::new(8));

        let mut r = VecRingBuffer::new(8, &tmp);

        assert_eq!(r.len(), 0);
        assert_eq!(r.remaining_capacity(), 8);

        r.write(b"12345").unwrap();

        assert_eq!(r.len(), 5);
        assert_eq!(r.remaining_capacity(), 3);

        r.write(b"678").unwrap();
        let mut bufs_arr = [&b""[..]; VECTORED_MAX];
        let bufs = r.read_bufs(&mut bufs_arr);

        assert_eq!(r.len(), 8);
        assert_eq!(r.remaining_capacity(), 0);
        assert_eq!(r.read_buf(), b"12345678");
        assert_eq!(bufs.len(), 1);
        assert_eq!(bufs[0], b"12345678");

        r.read(&mut buf[..5]).unwrap();

        assert_eq!(r.len(), 3);
        assert_eq!(r.remaining_capacity(), 5);

        assert_eq!(r.write_buf().len(), 5);

        r.write(b"9abcd").unwrap();

        assert_eq!(r.len(), 8);
        assert_eq!(r.remaining_capacity(), 0);

        r.read(&mut buf[5..]).unwrap();

        assert_eq!(r.len(), 5);
        assert_eq!(r.remaining_capacity(), 3);

        r.read(&mut buf[..5]).unwrap();

        assert_eq!(r.len(), 0);
        assert_eq!(r.remaining_capacity(), 8);
        assert_eq!(&buf, b"9abcd678");

        r.write(b"12345").unwrap();
        r.read(&mut buf[..2]).unwrap();

        let mut bufs_arr = [&b""[..]; VECTORED_MAX];
        let bufs = r.read_bufs(&mut bufs_arr);

        assert_eq!(r.len(), 3);
        assert_eq!(r.read_buf(), b"345");
        assert_eq!(bufs.len(), 1);
        assert_eq!(bufs[0], b"345");
        assert_eq!(r.remaining_capacity(), 5);
        assert_eq!(r.write_buf().len(), 3);

        r.align();

        assert_eq!(r.len(), 3);
        assert_eq!(r.read_buf(), b"345");
        assert_eq!(r.remaining_capacity(), 5);
        assert_eq!(r.write_buf().len(), 5);

        r.write(b"6789a").unwrap();
        r.read(&mut buf[..2]).unwrap();
        r.write(b"bc").unwrap();
        let mut bufs_arr = [&b""[..]; VECTORED_MAX];
        let bufs = r.read_bufs(&mut bufs_arr);

        assert_eq!(r.len(), 8);
        assert_eq!(r.read_buf(), b"56789a");
        assert_eq!(bufs.len(), 2);
        assert_eq!(bufs[0], b"56789a");
        assert_eq!(bufs[1], b"bc");
        assert_eq!(r.remaining_capacity(), 0);

        r.align();

        assert_eq!(r.len(), 8);
        assert_eq!(r.read_buf(), b"56789abc");
        assert_eq!(r.remaining_capacity(), 0);

        r.read(&mut buf[..6]).unwrap();
        r.write(b"def123").unwrap();
        let mut bufs_arr = [&b""[..]; VECTORED_MAX];
        let bufs = r.read_bufs(&mut bufs_arr);

        assert_eq!(r.len(), 8);
        assert_eq!(r.read_buf(), b"bc");
        assert_eq!(bufs.len(), 2);
        assert_eq!(bufs[0], b"bc");
        assert_eq!(bufs[1], b"def123");
        assert_eq!(r.remaining_capacity(), 0);

        r.align();
        let mut bufs_arr = [&b""[..]; VECTORED_MAX];
        let bufs = r.read_bufs(&mut bufs_arr);

        assert_eq!(r.len(), 8);
        assert_eq!(r.read_buf(), b"bcdef123");
        assert_eq!(bufs.len(), 1);
        assert_eq!(bufs[0], b"bcdef123");
        assert_eq!(r.remaining_capacity(), 0);

        r.clear();
        r.write(b"12345678").unwrap();
        r.read(&mut buf[..6]).unwrap();
        r.write(b"9abc").unwrap();

        assert_eq!(r.len(), 6);
        assert_eq!(r.read_buf().len(), 2);

        r.align();

        assert_eq!(r.len(), 6);
        assert_eq!(r.read_buf().len(), 6);
    }

    #[test]
    fn test_slice_ringbuffer() {
        let mut buf = [0; 8];
        let mut backing_buf = [0; 8];

        let tmp = Rc::new(TmpBuffer::new(8));

        let mut r = SliceRingBuffer::new(&mut backing_buf, &tmp);
        r.write(b"12345678").unwrap();
        let size = r.read(&mut buf[..4]).unwrap();
        assert_eq!(&buf[..size], b"1234");
        r.write(b"90ab").unwrap();
        let size = r.read(&mut buf).unwrap();
        assert_eq!(&buf[..size], b"567890ab");
    }

    #[test]
    fn test_limitbufs() {
        let mut buf1 = [b'1', b'2', b'3', b'4'];
        let mut buf2 = [b'5', b'6', b'7', b'8'];
        let mut buf3 = [b'9', b'0', b'a', b'b'];

        let mut bufs = [buf1.as_slice(), buf2.as_slice(), buf3.as_slice()];
        {
            let limited = bufs.limit(7);
            let limited = limited.as_slice();
            assert_eq!(limited.len(), 2);
            assert_eq!(&limited[0], b"1234");
            assert_eq!(&limited[1], b"567");
        }
        assert_eq!(bufs.len(), 3);
        assert_eq!(&bufs[0], b"1234");
        assert_eq!(&bufs[1], b"5678");
        assert_eq!(&bufs[2], b"90ab");

        let mut bufs = [
            buf1.as_mut_slice(),
            buf2.as_mut_slice(),
            buf3.as_mut_slice(),
        ];
        {
            let mut limited = bufs.limit(7);
            let limited = limited.as_slice();
            assert_eq!(limited.len(), 2);
            assert_eq!(&limited[0], b"1234");
            assert_eq!(&limited[1], b"567");
        }
        {
            let mut limited = bufs.skip(7);
            let limited = limited.as_slice();
            assert_eq!(limited.len(), 2);
            assert_eq!(&limited[0], b"8");
            assert_eq!(&limited[1], b"90ab");
        }
        assert_eq!(bufs.len(), 3);
        assert_eq!(&bufs[0], b"1234");
        assert_eq!(&bufs[1], b"5678");
        assert_eq!(&bufs[2], b"90ab");
    }

    #[test]
    fn test_resize() {
        let tmp = Rc::new(TmpBuffer::new(16));
        let mut r = VecRingBuffer::new(8, &tmp);

        assert_eq!(r.capacity(), 8);

        let size = r.write(b"12345678").unwrap();
        assert_eq!(size, 8);

        let mut buf = [0; 4];
        let size = r.read(&mut buf).unwrap();
        assert_eq!(size, 4);
        assert_eq!(&buf[..size], b"1234");

        let size = r.write(b"90ab").unwrap();
        assert_eq!(size, 4);

        assert!(r.write(b"cdef").is_err());

        r.resize(12);
        assert_eq!(r.capacity(), 12);

        let size = r.write(b"cdef").unwrap();
        assert_eq!(size, 4);

        let mut buf = [0; 12];
        let size = r.read(&mut buf).unwrap();
        assert_eq!(size, 12);
        assert_eq!(&buf[..size], b"567890abcdef");

        let size = r.write(b"1234567890").unwrap();
        assert_eq!(size, 10);

        r.resize(8);
        assert_eq!(r.capacity(), 8);

        let mut buf = [0; 12];
        let size = r.read(&mut buf).unwrap();
        assert_eq!(size, 8);
        assert_eq!(&buf[..size], b"12345678");
    }

    #[test]
    fn test_buffer_budget_accounting() {
        let counter = Counter::new(100); // Start with plenty of blocks available
        let mut budget = BufferBudget::new(&counter)
            .with_blocks_max(3)
            .with_block_size(64);
        let tmp = Rc::new(TmpBuffer::new(1024));

        assert_eq!(budget.blocks_used(), 0);
        assert_eq!(budget.blocks_max(), Some(3));

        // Test read buffer expansion with block-based strategy
        let mut read_buf = VecRingBuffer::new(64, &tmp);
        let data = vec![b'x'; 64];
        read_buf.write_all(&data).unwrap();

        budget.expand_buffer_if_needed(&mut read_buf); // 1 block (64 → 128)

        // Fill remaining capacity to trigger second expansion
        let remaining_data = vec![b'y'; 64];
        read_buf.write_all(&remaining_data).unwrap();
        budget.expand_buffer_if_needed(&mut read_buf); // 1 more block (128 → 192)

        // Test write buffer expansion (1 expansion)
        let mut write_buf = VecRingBuffer::new(64, &tmp);
        write_buf.write_all(&data).unwrap();
        budget.expand_buffer_if_needed(&mut write_buf); // 1 block

        assert_eq!(budget.blocks_used(), 3); // 1 + 1 + 1 = 3 expansion blocks
    }

    #[test]
    fn test_buffer_budget_expansion() {
        let counter = Counter::new(100);
        let mut budget = BufferBudget::new(&counter)
            .with_blocks_max(1)
            .with_block_size(64);
        let tmp = Rc::new(TmpBuffer::new(1024));
        let mut buf = VecRingBuffer::new(64, &tmp);

        // Fill buffer to capacity
        let data = vec![b'x'; 64];
        buf.write_all(&data).unwrap();
        assert_eq!(buf.remaining_capacity(), 0);

        // Should expand when no capacity and budget available
        let expanded = budget.expand_buffer_if_needed(&mut buf);
        assert_eq!(expanded, 64);
        assert_eq!(buf.capacity(), 128);
        assert_eq!(budget.blocks_used(), 1); // 1 (expansion)
    }

    #[test]
    fn test_buffer_budget_exhaustion() {
        let counter = Counter::new(100);
        let mut budget = BufferBudget::new(&counter)
            .with_blocks_max(3)
            .with_block_size(64); // Limit of 3 expansion blocks
        let tmp = Rc::new(TmpBuffer::new(1024));

        // Use up all blocks via expansion
        let mut read_buf = VecRingBuffer::new(64, &tmp);
        let mut write_buf = VecRingBuffer::new(32, &tmp); // Start smaller so write buffer can expand
        let read_data = vec![b'x'; 64];
        let write_data = vec![b'y'; 32];

        read_buf.write_all(&read_data).unwrap();
        write_buf.write_all(&write_data).unwrap();

        // Use up budget with expansions
        assert!(budget.expand_buffer_if_needed(&mut read_buf) > 0); // 1 block (64 → 128)
        assert!(budget.expand_buffer_if_needed(&mut write_buf) > 0); // 1 block (32 → 96)
        assert_eq!(budget.blocks_used(), 2); // 2 (expansions)

        // Try one more read expansion - this uses 1 block, should succeed
        read_buf.write_all(&read_data).unwrap(); // Fill expanded buffer to capacity
        assert!(budget.expand_buffer_if_needed(&mut read_buf) > 0); // Should succeed

        // Now budget is exhausted (used all 3 expansion blocks) - write expansion should fail
        let remaining = write_buf.remaining_capacity();
        let fill_data = vec![b'z'; remaining];
        write_buf.write_all(&fill_data).unwrap(); // Fill write buffer completely
        assert_eq!(budget.expand_buffer_if_needed(&mut write_buf), 0); // Should fail - budget exhausted
        assert_eq!(budget.blocks_used(), 3); // 3 (expansions)

        // Now budget is exhausted - no more expansions should succeed
        // Both buffers should fail to expand due to budget exhaustion
        assert_eq!(budget.expand_buffer_if_needed(&mut read_buf), 0);
        assert_eq!(budget.expand_buffer_if_needed(&mut write_buf), 0);
    }

    #[test]
    fn test_buffer_budget_mixed_types() {
        let counter = Counter::new(100);
        let mut budget = BufferBudget::new(&counter)
            .with_blocks_max(4)
            .with_block_size(64); // 4 (expansions)
        let tmp = Rc::new(TmpBuffer::new(1024));

        // Can expand across buffer types up to total limit
        let mut read_buf = VecRingBuffer::new(64, &tmp);
        let mut write_buf = VecRingBuffer::new(64, &tmp);

        // First read expansion: 64 → 128 bytes (uses 1 block)
        let data64 = vec![b'x'; 64];
        read_buf.write_all(&data64).unwrap();
        assert!(budget.expand_buffer_if_needed(&mut read_buf) > 0);

        // Second read expansion: 128 → 192 bytes (uses 1 more block)
        // Fill the remaining 64 bytes to make the 128-byte buffer full
        let remaining_data = vec![b'y'; 64];
        read_buf.write_all(&remaining_data).unwrap();
        assert!(budget.expand_buffer_if_needed(&mut read_buf) > 0);

        // Now we've used 1 + 1 = 2 blocks for read expansions

        // Expand write buffer (2 blocks)
        for _ in 0..2 {
            write_buf.write_all(&data64).unwrap();
            assert!(budget.expand_buffer_if_needed(&mut write_buf) > 0);
        }

        assert_eq!(budget.blocks_used(), 4); // 2 (read) + 2 (write)

        // Should be at limit - no more expansion possible
        // Fill buffers to capacity to test expansion failure
        let remaining_read = read_buf.remaining_capacity();
        if remaining_read > 0 {
            let fill_data = vec![b'z'; remaining_read];
            read_buf.write_all(&fill_data).unwrap();
        }
        write_buf.write_all(&data64).unwrap();
        assert_eq!(budget.expand_buffer_if_needed(&mut read_buf), 0);
        assert_eq!(budget.expand_buffer_if_needed(&mut write_buf), 0);
    }

    #[test]
    fn test_buffer_budget_shared_counter_integration() {
        // Test that BufferBudget properly uses the shared Counter
        let counter = Counter::new(3); // 3 excess blocks: budget1 uses 2, budget2 uses 1
        let mut budget1 = BufferBudget::new(&counter)
            .with_blocks_max(10) // Local limit higher than global
            .with_block_size(64);
        let mut budget2 = BufferBudget::new(&counter)
            .with_blocks_max(10)
            .with_block_size(64);
        let tmp = Rc::new(TmpBuffer::new(1024));

        // budget1 uses 1 block via read expansion (64 → 128)
        let mut read_buf1 = VecRingBuffer::new(64, &tmp);
        let data = vec![b'x'; 64];
        read_buf1.write_all(&data).unwrap();
        assert!(budget1.expand_buffer_if_needed(&mut read_buf1) > 0); // Uses 1 block

        // Fill to capacity and expand again (128 → 192) - this uses 1 more block
        read_buf1.write_all(&data).unwrap(); // Now 128 bytes used, 0 remaining
        assert!(budget1.expand_buffer_if_needed(&mut read_buf1) > 0); // Uses 1 more block

        // At this point budget1 has used 2 expansion blocks, leaving 1 block available

        // budget2 uses the remaining 1 expansion block
        let mut write_buf2 = VecRingBuffer::new(64, &tmp);
        write_buf2.write_all(&data).unwrap();
        assert!(budget2.expand_buffer_if_needed(&mut write_buf2) > 0); // Uses the last expansion block

        // Now all 3 excess blocks are used - no more expansion should be possible

        // Confirm no more expansion is possible for either budget
        // Try to fill buffers to capacity for expansion attempt, but write_buf2 is already full
        let remaining = read_buf1.remaining_capacity();
        if remaining > 0 {
            let fill_data = vec![b'y'; remaining];
            read_buf1.write_all(&fill_data).unwrap();
        }
        // write_buf2 is already full, so don't try to write more to it
        assert_eq!(budget1.expand_buffer_if_needed(&mut read_buf1), 0); // Should fail due to global limit
        assert_eq!(budget2.expand_buffer_if_needed(&mut write_buf2), 0); // Should fail due to global limit
    }

    #[test]
    fn test_buffer_budget_defaults() {
        let counter = Counter::new(10); // Small counter for testing
        let tmp = Rc::new(TmpBuffer::new(1024));

        // Test that BufferBudget works with just a counter (no builder methods)
        let mut budget = BufferBudget::new(&counter);
        assert_eq!(budget.blocks_used(), 0); // Starts with 0 expansion blocks
        assert_eq!(budget.blocks_max(), None); // No local limit by default

        // Test expansion with default 1-byte block size
        let mut buf = VecRingBuffer::new(64, &tmp);
        buf.write_all(&vec![b'x'; 64]).unwrap(); // Fill buffer
        let expanded = budget.expand_buffer_if_needed(&mut buf);
        assert_eq!(expanded, 1); // Should expand by 1 byte (default block_size)
        assert_eq!(buf.capacity(), 65); // 64 + 1
        assert_eq!(budget.blocks_used(), 1); // 1 expansion

        // Since blocks_max is None, it can keep expanding (limited only by shared counter)
        buf.write_all(&[b'y']).unwrap(); // Fill the new byte
        let expanded2 = budget.expand_buffer_if_needed(&mut buf);
        assert_eq!(expanded2, 1); // Should expand by another 1 byte
        assert_eq!(buf.capacity(), 66); // 65 + 1
        assert_eq!(budget.blocks_used(), 2); // 2 expansions
    }
}
