/*
 * Copyright (C) 2020-2021 Fanout, Inc.
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

use std::cell::RefCell;
use std::cmp;
use std::io;
use std::io::{Read, Write};
use std::rc::Rc;

pub const VECTORED_MAX: usize = 8;

pub trait RefRead {
    fn get_ref(&self) -> &[u8];
    fn get_mut(&mut self) -> &mut [u8];
    fn consume(&mut self, amt: usize);

    fn get_ref_vectored<'data, 'bufs>(
        &'data self,
        bufs: &'bufs mut [&'data [u8]],
    ) -> &'bufs mut [&'data [u8]] {
        assert!(bufs.len() >= 1);

        bufs[0] = self.get_ref();

        &mut bufs[..1]
    }

    fn get_mut_vectored<'data, 'bufs>(
        &'data mut self,
        bufs: &'bufs mut [&'data mut [u8]],
    ) -> &'bufs mut [&'data mut [u8]] {
        assert!(bufs.len() >= 1);

        bufs[0] = self.get_mut();

        &mut bufs[..1]
    }
}

impl RefRead for io::Cursor<&mut [u8]> {
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

pub fn write_vectored_offset(
    writer: &mut dyn Write,
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

    for i in start..bufs.len() {
        let buf = if i == start {
            &bufs[i][offset..]
        } else {
            bufs[i]
        };

        arr[arr_len] = io::IoSlice::new(buf);
        arr_len += 1;
    }

    writer.write_vectored(&arr[..arr_len])
}

pub trait LimitBufs<'a> {
    fn limit(&mut self, size: usize) -> &mut [&'a [u8]];
}

impl<'a> LimitBufs<'a> for [&'a [u8]] {
    fn limit(&mut self, size: usize) -> &mut [&'a [u8]] {
        let mut want = size;

        for i in 0..self.len() {
            let buf_len = self[i].len();

            if buf_len >= want {
                self[i] = &self[i][..want];
                return &mut self[..(i + 1)];
            }

            want -= buf_len;
        }

        self
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
        let src = self.read_buf();
        let size = cmp::min(src.len(), buf.len());

        &buf[..size].copy_from_slice(&src[..size]);

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

        &dest[..size].copy_from_slice(&buf[..size]);

        self.write_commit(size);

        Ok(size)
    }

    fn flush(&mut self) -> Result<(), io::Error> {
        Ok(())
    }
}

pub struct TmpBuffer(RefCell<Vec<u8>>);

impl TmpBuffer {
    pub fn new(size: usize) -> Self {
        Self(RefCell::new(vec![0; size]))
    }

    pub fn len(&self) -> usize {
        self.0.borrow().len()
    }
}

pub struct RingBuffer {
    buf: Vec<u8>,
    start: usize,
    end: usize,
    tmp: Rc<TmpBuffer>,
}

impl RingBuffer {
    pub fn new(size: usize, tmp: &Rc<TmpBuffer>) -> RingBuffer {
        assert!(size <= tmp.len());

        let buf = vec![0; size];

        RingBuffer {
            buf,
            start: 0,
            end: 0,
            tmp: Rc::clone(tmp),
        }
    }

    pub fn capacity(&self) -> usize {
        self.buf.len()
    }

    pub fn clear(&mut self) {
        self.start = 0;
        self.end = 0;
    }

    pub fn write_from(&mut self, r: &mut dyn Read) -> Result<usize, io::Error> {
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
        let end = cmp::min(self.end, self.buf.len());

        &self.buf[self.start..end]
    }

    pub fn read_buf_mut(&mut self) -> &mut [u8] {
        let end = cmp::min(self.end, self.buf.len());

        &mut self.buf[self.start..end]
    }

    pub fn read_commit(&mut self, amount: usize) {
        assert!(self.start + amount <= self.end);

        self.start += amount;

        if self.start == self.end {
            self.start = 0;
            self.end = 0;
        } else if self.start >= self.buf.len() {
            self.start -= self.buf.len();
            self.end -= self.buf.len();
        }
    }

    pub fn write_avail(&self) -> usize {
        self.buf.len() - (self.end - self.start)
    }

    pub fn write_buf(&mut self) -> &mut [u8] {
        let (start, end) = if self.end < self.buf.len() {
            (self.end, self.buf.len())
        } else {
            (self.end - self.buf.len(), self.start)
        };

        &mut self.buf[start..end]
    }

    pub fn write_commit(&mut self, amount: usize) {
        assert!((self.end - self.start) + amount <= self.buf.len());

        self.end += amount;
    }

    pub fn align(&mut self) -> usize {
        if self.start == 0 {
            return 0;
        }

        let size = self.end - self.start;

        if self.end <= self.buf.len() {
            // if the buffer hasn't wrapped, simply copy down
            self.buf.copy_within(self.start.., 0);
        } else if size <= self.start {
            // if the buffer has wrapped, but the wrapped part can be copied
            //   without overlapping, then copy the wrapped part followed by
            //   initial part

            let left_size = self.end - self.buf.len();
            let right_size = self.buf.len() - self.start;

            self.buf.copy_within(..left_size, right_size);
            self.buf
                .copy_within(self.start..(self.start + right_size), 0);
        } else {
            // if the buffer has wrapped and the wrapped part can't be copied
            //   without overlapping, then use a temporary buffer to
            //   facilitate. smaller part is copied to the temp buffer, then
            //   the larger and small parts (in that order) are copied into
            //   their intended locations. in the worst case, up to 50% of
            //   the buffer may be copied twice

            let left_size = self.end - self.buf.len();
            let right_size = self.buf.len() - self.start;

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

            &mut tmp[..lsize].copy_from_slice(&self.buf[lsrc..(lsrc + lsize)]);
            self.buf.copy_within(hsrc..(hsrc + hsize), hdest);
            &mut self.buf[ldest..(ldest + lsize)].copy_from_slice(&mut tmp[..lsize]);
        }

        self.start = 0;
        self.end = size;

        size
    }
}

#[cfg(test)]
impl Read for RingBuffer {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, io::Error> {
        let mut pos = 0;

        while pos < buf.len() && self.read_avail() > 0 {
            let src = self.read_buf();
            let size = cmp::min(src.len(), buf.len() - pos);

            &buf[pos..(pos + size)].copy_from_slice(&src[..size]);

            self.read_commit(size);

            pos += size;
        }

        Ok(pos)
    }
}

impl Write for RingBuffer {
    fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
        if !buf.is_empty() && self.write_avail() == 0 {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        let mut pos = 0;

        while pos < buf.len() && self.write_avail() > 0 {
            let dest = self.write_buf();
            let size = cmp::min(dest.len(), buf.len() - pos);

            &dest[..size].copy_from_slice(&buf[pos..(pos + size)]);

            self.write_commit(size);

            pos += size;
        }

        Ok(pos)
    }

    fn flush(&mut self) -> Result<(), io::Error> {
        Ok(())
    }
}

impl RefRead for RingBuffer {
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
        assert!(bufs.len() >= 1);

        let buf_len = self.buf.len();

        if self.end > buf_len && bufs.len() >= 2 {
            let (part1, part2) = self.buf.split_at(self.start);

            bufs[0] = part2;
            bufs[1] = &part1[..(self.end - buf_len)];

            &mut bufs[..2]
        } else {
            bufs[0] = &self.buf[self.start..self.end];

            &mut bufs[..1]
        }
    }

    fn get_mut_vectored<'data, 'bufs>(
        &'data mut self,
        bufs: &'bufs mut [&'data mut [u8]],
    ) -> &'bufs mut [&'data mut [u8]] {
        assert!(bufs.len() >= 1);

        let buf_len = self.buf.len();

        if self.end > buf_len && bufs.len() >= 2 {
            let (part1, part2) = self.buf.split_at_mut(self.start);

            bufs[0] = part2;
            bufs[1] = &mut part1[..(self.end - buf_len)];

            &mut bufs[..2]
        } else {
            bufs[0] = &mut self.buf[self.start..self.end];

            &mut bufs[..1]
        }
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
}
