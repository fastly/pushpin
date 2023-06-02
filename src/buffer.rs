/*
 * Copyright (C) 2020-2023 Fanout, Inc.
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

use crate::future::{AsyncWrite, AsyncWriteExt};
use std::cell::RefCell;
use std::cmp;
use std::io;
use std::io::{Read, Write};
use std::mem::{self, MaybeUninit};
use std::rc::Rc;
use std::slice;

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

#[allow(clippy::len_without_is_empty)]
pub trait RefRead {
    fn len(&self) -> usize;
    fn get_ref(&self) -> &[u8];
    fn get_mut(&mut self) -> &mut [u8];
    fn consume(&mut self, amt: usize);

    fn get_ref_vectored<'data, 'bufs>(
        &'data self,
        bufs: &'bufs mut [&'data [u8]],
    ) -> &'bufs mut [&'data [u8]] {
        assert!(!bufs.is_empty());

        bufs[0] = self.get_ref();

        &mut bufs[..1]
    }

    fn get_mut_vectored<'data, 'bufs, const N: usize>(
        &'data mut self,
        bufs: &'bufs mut MaybeUninit<[&'data mut [u8]; N]>,
    ) -> &'bufs mut [&'data mut [u8]] {
        let bufs = unsafe { bufs.assume_init_mut() };

        bufs[0] = self.get_mut();

        &mut bufs[..1]
    }
}

impl RefRead for io::Cursor<&mut [u8]> {
    fn len(&self) -> usize {
        RefRead::get_ref(self).len()
    }

    fn get_ref(&self) -> &[u8] {
        let pos = self.position() as usize;

        &self.get_ref()[pos..]
    }

    fn get_mut(&mut self) -> &mut [u8] {
        let pos = self.position() as usize;

        &mut self.get_mut()[pos..]
    }

    fn consume(&mut self, amt: usize) {
        let pos = self.position();

        self.set_position(pos + (amt as u64));
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
        // on the last buf?
        if start + 1 >= bufs.len() {
            // exceeding the last buf is an error
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

pub async fn write_vectored_offset_async<W: AsyncWrite>(
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
        // on the last buf?
        if start + 1 >= bufs.len() {
            // exceeding the last buf is an error
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

    writer.write_vectored(&arr[..arr_len]).await
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
            // still valid. the only issue with reconstructing the slice is
            // that we currently have a different slice using the same memory
            // at this index. however, this is safe because we also replace
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
            // still valid. the only issue with reconstructing the slice is
            // that we currently have a different slice using the same memory
            // at this index. however, this is safe because we also replace
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
                // valid. we just need to be careful about using them again
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
                // valid. we just need to be careful about using them again
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
                // valid. we just need to be careful about using them again
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

pub struct Buffer {
    buf: Vec<u8>,
    start: usize,
    end: usize,
}

impl Buffer {
    pub fn new(size: usize) -> Buffer {
        let buf = vec![0; size];

        Buffer {
            buf,
            start: 0,
            end: 0,
        }
    }

    pub fn clear(&mut self) {
        self.start = 0;
        self.end = 0;
    }

    pub fn read_avail(&self) -> usize {
        self.end - self.start
    }

    pub fn read_buf(&self) -> &[u8] {
        &self.buf[self.start..self.end]
    }

    pub fn read_commit(&mut self, amount: usize) {
        assert!(self.start + amount <= self.end);

        self.start += amount;
    }

    pub fn write_avail(&self) -> usize {
        self.buf.len() - self.end
    }

    pub fn write_buf(&mut self) -> &mut [u8] {
        let len = self.buf.len();

        &mut self.buf[self.end..len]
    }

    pub fn write_commit(&mut self, amount: usize) {
        assert!(self.end + amount <= self.buf.len());

        self.end += amount;
    }
}

#[cfg(test)]
impl Read for Buffer {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, io::Error> {
        // fully qualified to work around future method warning
        // https://github.com/rust-lang/rust/issues/48919
        let src = Buffer::read_buf(self);
        let size = cmp::min(src.len(), buf.len());

        buf[..size].copy_from_slice(&src[..size]);

        self.read_commit(size);

        Ok(size)
    }
}

impl Write for Buffer {
    fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
        if !buf.is_empty() && self.write_avail() == 0 {
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

// holds a Vec<u8> but only exposes the portion of it considered to be
// readable ("filled"). any remaining bytes may be zeroed or uninitialized
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

pub struct BaseRingBuffer<T> {
    buf: T,
    start: usize,
    end: usize,
    tmp: Rc<TmpBuffer>,
}

impl<T: AsRef<[u8]> + AsMut<[u8]>> BaseRingBuffer<T> {
    pub fn capacity(&self) -> usize {
        self.buf.as_ref().len()
    }

    pub fn clear(&mut self) {
        self.start = 0;
        self.end = 0;
    }

    pub fn write_from<R: Read>(&mut self, r: &mut R) -> Result<usize, io::Error> {
        let size = match r.read(self.write_buf()) {
            Ok(size) => size,
            Err(e) => return Err(e),
        };

        self.write_commit(size);

        Ok(size)
    }

    pub fn read_avail(&self) -> usize {
        self.end - self.start
    }

    pub fn read_buf(&self) -> &[u8] {
        let buf = self.buf.as_ref();
        let end = cmp::min(self.end, buf.len());

        &buf[self.start..end]
    }

    pub fn read_buf_mut(&mut self) -> &mut [u8] {
        let buf = self.buf.as_mut();
        let end = cmp::min(self.end, buf.len());

        &mut buf[self.start..end]
    }

    pub fn read_commit(&mut self, amount: usize) {
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

    pub fn write_avail(&self) -> usize {
        self.buf.as_ref().len() - (self.end - self.start)
    }

    pub fn write_buf(&mut self) -> &mut [u8] {
        let buf = self.buf.as_mut();

        let (start, end) = if self.end < buf.len() {
            (self.end, buf.len())
        } else {
            (self.end - buf.len(), self.start)
        };

        &mut buf[start..end]
    }

    pub fn write_commit(&mut self, amount: usize) {
        assert!((self.end - self.start) + amount <= self.buf.as_ref().len());

        self.end += amount;
    }

    // return true if the readable bytes have not wrapped
    pub fn is_readable_contiguous(&self) -> bool {
        self.end <= self.buf.as_ref().len()
    }

    pub fn align(&mut self) -> usize {
        if self.start == 0 {
            return 0;
        }

        let buf = self.buf.as_mut();
        let size = self.end - self.start;

        if self.end <= buf.len() {
            // if the buffer hasn't wrapped, simply copy down
            buf.copy_within(self.start.., 0);
        } else if size <= self.start {
            // if the buffer has wrapped, but the wrapped part can be copied
            //   without overlapping, then copy the wrapped part followed by
            //   initial part

            let left_size = self.end - buf.len();
            let right_size = buf.len() - self.start;

            buf.copy_within(..left_size, right_size);
            buf.copy_within(self.start..(self.start + right_size), 0);
        } else {
            // if the buffer has wrapped and the wrapped part can't be copied
            //   without overlapping, then use a temporary buffer to
            //   facilitate. smaller part is copied to the temp buffer, then
            //   the larger and small parts (in that order) are copied into
            //   their intended locations. in the worst case, up to 50% of
            //   the buffer may be copied twice

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
impl<T: AsRef<[u8]> + AsMut<[u8]>> Read for BaseRingBuffer<T> {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, io::Error> {
        let mut pos = 0;

        while pos < buf.len() && self.read_avail() > 0 {
            // fully qualified to work around future method warning
            // https://github.com/rust-lang/rust/issues/48919
            let src = Self::read_buf(self);
            let size = cmp::min(src.len(), buf.len() - pos);

            buf[pos..(pos + size)].copy_from_slice(&src[..size]);

            self.read_commit(size);

            pos += size;
        }

        Ok(pos)
    }
}

impl<T: AsRef<[u8]> + AsMut<[u8]>> Write for BaseRingBuffer<T> {
    fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
        if !buf.is_empty() && self.write_avail() == 0 {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        let mut pos = 0;

        while pos < buf.len() && self.write_avail() > 0 {
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

impl<T: AsRef<[u8]> + AsMut<[u8]>> RefRead for BaseRingBuffer<T> {
    fn len(&self) -> usize {
        self.read_avail()
    }

    fn get_ref(&self) -> &[u8] {
        self.read_buf()
    }

    fn get_mut(&mut self) -> &mut [u8] {
        self.read_buf_mut()
    }

    fn consume(&mut self, amt: usize) {
        self.read_commit(amt);
    }

    fn get_ref_vectored<'data, 'bufs>(
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

    fn get_mut_vectored<'data, 'bufs, const N: usize>(
        &'data mut self,
        bufs: &'bufs mut MaybeUninit<[&'data mut [u8]; N]>,
    ) -> &'bufs mut [&'data mut [u8]] {
        let bufs = unsafe { bufs.assume_init_mut() };

        let buf = self.buf.as_mut();
        let buf_len = buf.len();

        if self.end > buf_len && bufs.len() >= 2 {
            let (part1, part2) = buf.split_at_mut(self.start);

            bufs[0] = part2;
            bufs[1] = &mut part1[..(self.end - buf_len)];

            &mut bufs[..2]
        } else {
            bufs[0] = &mut buf[self.start..self.end];

            &mut bufs[..1]
        }
    }
}

impl BaseRingBuffer<Vec<u8>> {
    pub fn new(size: usize, tmp: &Rc<TmpBuffer>) -> Self {
        assert!(size <= tmp.len());

        let buf = vec![0; size];

        BaseRingBuffer {
            buf,
            start: 0,
            end: 0,
            tmp: Rc::clone(tmp),
        }
    }

    // extract inner buffer, aligning it first if necessary, and replace it
    // with an empty buffer. this should be cheap if the inner buffer is
    // already aligned. afterwards, the ringbuffer will have a capacity of
    // zero and will be essentially unusable until set_inner is called with a
    // non-empty buffer
    pub fn take_inner(&mut self) -> FilledBuf {
        self.align();

        let data = mem::take(&mut self.buf);
        let filled = self.end;
        self.end = 0;

        FilledBuf::new(data, filled)
    }

    // replace the inner buffer. this should be cheap if the original inner
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
}

impl<'a> BaseRingBuffer<&'a mut [u8]> {
    pub fn new(buf: &'a mut [u8], tmp: &Rc<TmpBuffer>) -> Self {
        assert!(buf.len() <= tmp.len());

        BaseRingBuffer {
            buf,
            start: 0,
            end: 0,
            tmp: Rc::clone(tmp),
        }
    }
}

pub type RingBuffer = BaseRingBuffer<Vec<u8>>;
pub type SliceRingBuffer<'a> = BaseRingBuffer<&'a mut [u8]>;

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

        // empty
        let mut w = MyWriter::new();
        let r = write_vectored_offset(&mut w, &[], 0);
        assert_eq!(r.unwrap(), 0);

        // offset too large
        let mut w = MyWriter::new();
        let r = write_vectored_offset(&mut w, &[b"apple"], 6);
        assert!(r.is_err());

        // offset too large
        let mut w = MyWriter::new();
        let r = write_vectored_offset(&mut w, &[b"apple", b"banana"], 12);
        assert!(r.is_err());

        // nothing to write
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
        let mut b = Buffer::new(8);

        assert_eq!(b.read_avail(), 0);
        assert_eq!(b.write_avail(), 8);

        let size = b.write(b"hello").unwrap();
        assert_eq!(size, 5);
        assert_eq!(b.read_avail(), 5);
        assert_eq!(b.write_avail(), 3);

        let size = b.write(b"world").unwrap();
        assert_eq!(size, 3);
        assert_eq!(b.read_avail(), 8);
        assert_eq!(b.write_avail(), 0);

        let mut tmp = [0; 16];
        let size = b.read(&mut tmp).unwrap();
        assert_eq!(&tmp[..size], b"hellowor");

        b.clear();
        assert_eq!(b.read_avail(), 0);
        assert_eq!(b.write_avail(), 8);
    }

    #[test]
    fn test_ringbuffer() {
        let mut buf = [0u8; 8];

        let tmp = Rc::new(TmpBuffer::new(8));

        let mut r = RingBuffer::new(8, &tmp);

        assert_eq!(r.read_avail(), 0);
        assert_eq!(r.write_avail(), 8);

        r.write(b"12345").unwrap();

        assert_eq!(r.read_avail(), 5);
        assert_eq!(r.write_avail(), 3);

        r.write(b"678").unwrap();
        let mut bufs_arr = [&b""[..]; VECTORED_MAX];
        let bufs = r.get_ref_vectored(&mut bufs_arr);

        assert_eq!(r.read_avail(), 8);
        assert_eq!(r.write_avail(), 0);
        assert_eq!(r.read_buf(), b"12345678");
        assert_eq!(bufs.len(), 1);
        assert_eq!(bufs[0], b"12345678");

        r.read(&mut buf[..5]).unwrap();

        assert_eq!(r.read_avail(), 3);
        assert_eq!(r.write_avail(), 5);

        assert_eq!(r.write_buf().len(), 5);

        r.write(b"9abcd").unwrap();

        assert_eq!(r.read_avail(), 8);
        assert_eq!(r.write_avail(), 0);

        r.read(&mut buf[5..]).unwrap();

        assert_eq!(r.read_avail(), 5);
        assert_eq!(r.write_avail(), 3);

        r.read(&mut buf[..5]).unwrap();

        assert_eq!(r.read_avail(), 0);
        assert_eq!(r.write_avail(), 8);
        assert_eq!(&buf, b"9abcd678");

        r.write(b"12345").unwrap();
        r.read(&mut buf[..2]).unwrap();

        let mut bufs_arr = [&b""[..]; VECTORED_MAX];
        let bufs = r.get_ref_vectored(&mut bufs_arr);

        assert_eq!(r.read_avail(), 3);
        assert_eq!(r.read_buf(), b"345");
        assert_eq!(bufs.len(), 1);
        assert_eq!(bufs[0], b"345");
        assert_eq!(r.write_avail(), 5);
        assert_eq!(r.write_buf().len(), 3);

        r.align();

        assert_eq!(r.read_avail(), 3);
        assert_eq!(r.read_buf(), b"345");
        assert_eq!(r.write_avail(), 5);
        assert_eq!(r.write_buf().len(), 5);

        r.write(b"6789a").unwrap();
        r.read(&mut buf[..2]).unwrap();
        r.write(b"bc").unwrap();
        let mut bufs_arr = [&b""[..]; VECTORED_MAX];
        let bufs = r.get_ref_vectored(&mut bufs_arr);

        assert_eq!(r.read_avail(), 8);
        assert_eq!(r.read_buf(), b"56789a");
        assert_eq!(bufs.len(), 2);
        assert_eq!(bufs[0], b"56789a");
        assert_eq!(bufs[1], b"bc");
        assert_eq!(r.write_avail(), 0);

        r.align();

        assert_eq!(r.read_avail(), 8);
        assert_eq!(r.read_buf(), b"56789abc");
        assert_eq!(r.write_avail(), 0);

        r.read(&mut buf[..6]).unwrap();
        r.write(b"def123").unwrap();
        let mut bufs_arr = [&b""[..]; VECTORED_MAX];
        let bufs = r.get_ref_vectored(&mut bufs_arr);

        assert_eq!(r.read_avail(), 8);
        assert_eq!(r.read_buf(), b"bc");
        assert_eq!(bufs.len(), 2);
        assert_eq!(bufs[0], b"bc");
        assert_eq!(bufs[1], b"def123");
        assert_eq!(r.write_avail(), 0);

        r.align();
        let mut bufs_arr = [&b""[..]; VECTORED_MAX];
        let bufs = r.get_ref_vectored(&mut bufs_arr);

        assert_eq!(r.read_avail(), 8);
        assert_eq!(r.read_buf(), b"bcdef123");
        assert_eq!(bufs.len(), 1);
        assert_eq!(bufs[0], b"bcdef123");
        assert_eq!(r.write_avail(), 0);

        r.clear();
        r.write(b"12345678").unwrap();
        r.read(&mut buf[..6]).unwrap();
        r.write(b"9abc").unwrap();

        assert_eq!(r.read_avail(), 6);
        assert_eq!(r.read_buf().len(), 2);

        r.align();

        assert_eq!(r.read_avail(), 6);
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
}
