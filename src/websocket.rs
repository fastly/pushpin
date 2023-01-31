/*
 * Copyright (C) 2020-2022 Fanout, Inc.
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

use crate::buffer::{
    trim_for_display, write_vectored_offset, BaseRingBuffer, LimitBufsMut, RefRead, VECTORED_MAX,
};
use crate::http1::HeaderParamsIterator;
use arrayvec::ArrayVec;
use log::{log_enabled, trace};
use miniz_oxide::deflate;
use miniz_oxide::inflate::stream::{inflate, InflateState};
use miniz_oxide::{DataFormat, MZError, MZFlush, MZStatus};
use std::ascii;
use std::cell::{Cell, RefCell};
use std::cmp;
use std::fmt;
use std::io;
use std::io::Write;
use std::mem::{self, MaybeUninit};

pub const WS_GUID: &str = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// 1 byte flags + 9 bytes payload size + 4 bytes mask
pub const HEADER_SIZE_MAX: usize = 14;

const LOG_CONTENT_MAX: usize = 1_000;

const PSIZE_3BYTE: usize = 126;
const PSIZE_9BYTE: usize = 65536;

pub const OPCODE_CONTINUATION: u8 = 0;
pub const OPCODE_TEXT: u8 = 1;
pub const OPCODE_BINARY: u8 = 2;
pub const OPCODE_CLOSE: u8 = 8;
pub const OPCODE_PING: u8 = 9;
pub const OPCODE_PONG: u8 = 10;

pub const CONTROL_FRAME_PAYLOAD_MAX: usize = 125;

const DEFAULT_MAX_WINDOW_BITS: u8 = 15;
const DEFLATE_SUFFIX: [u8; 4] = [0x00, 0x00, 0xff, 0xff];
const ENC_NEXT_BUF_SIZE: usize = DEFLATE_SUFFIX.len();

struct Bufs<'a> {
    data: &'a [&'a [u8]],
}

impl<'a> Bufs<'a> {
    fn new(data: &'a [&'a [u8]]) -> Self {
        Self { data }
    }
}

impl fmt::Display for Bufs<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        use fmt::Write;

        let data: Vec<u8> = self
            .data
            .iter()
            .map(|v| *v)
            .flatten()
            .map(|v| *v)
            .take(LOG_CONTENT_MAX + 1)
            .collect();

        let mut s = String::new();

        write!(&mut s, "\"")?;

        for b in data {
            write!(&mut s, "{}", ascii::escape_default(b))?;
        }

        write!(&mut s, "\"")?;

        write!(f, "{}", trim_for_display(&s, LOG_CONTENT_MAX))
    }
}

#[derive(Clone, Copy)]
pub struct FrameInfo {
    pub fin: bool,
    pub rsv1: bool,
    pub opcode: u8,
    pub mask: Option<[u8; 4]>,
    pub payload_offset: usize,
    pub payload_size: usize,
}

fn header_size(payload_size: usize, masked: bool) -> usize {
    let size = if payload_size < PSIZE_3BYTE {
        1 + 1
    } else if payload_size < PSIZE_9BYTE {
        1 + 3
    } else {
        1 + 9
    };

    if masked {
        size + 4
    } else {
        size
    }
}

pub fn read_header(buf: &[u8]) -> Result<FrameInfo, io::Error> {
    if buf.len() < 2 {
        return Err(io::Error::from(io::ErrorKind::UnexpectedEof));
    }

    let b1 = buf[1] & 0x7f;

    let (mut hsize, psize) = if b1 < (PSIZE_3BYTE as u8) {
        (2, b1 as usize)
    } else if b1 == (PSIZE_3BYTE as u8) {
        if buf.len() < 2 + 2 {
            return Err(io::Error::from(io::ErrorKind::UnexpectedEof));
        }

        let mut arr = [0; 2];
        arr.copy_from_slice(&buf[2..4]);
        (4, u16::from_be_bytes(arr) as usize)
    } else {
        if buf.len() < 2 + 8 {
            return Err(io::Error::from(io::ErrorKind::UnexpectedEof));
        }

        let mut arr = [0; 8];
        arr.copy_from_slice(&buf[2..10]);
        (10, u64::from_be_bytes(arr) as usize)
    };

    let mask = if buf[1] & 0x80 != 0 {
        if buf.len() < hsize + 4 {
            return Err(io::Error::from(io::ErrorKind::UnexpectedEof));
        }

        let mut mask = [0; 4];
        mask.copy_from_slice(&buf[hsize..hsize + 4]);
        hsize += 4;

        Some(mask)
    } else {
        None
    };

    Ok(FrameInfo {
        fin: buf[0] & 0x80 != 0,
        rsv1: buf[0] & 0x40 != 0,
        opcode: buf[0] & 0x0f,
        mask,
        payload_offset: hsize,
        payload_size: psize,
    })
}

// return payload offset
pub fn write_header(
    fin: bool,
    rsv1: bool,
    opcode: u8,
    payload_size: usize,
    mask: Option<[u8; 4]>,
    buf: &mut [u8],
) -> Result<usize, io::Error> {
    let hsize = header_size(payload_size, mask.is_some());
    if buf.len() < hsize {
        return Err(io::Error::from(io::ErrorKind::WriteZero));
    }

    let mut b0 = 0;
    if fin {
        b0 |= 0x80;
    }
    if rsv1 {
        b0 |= 0x40;
    }
    b0 |= opcode & 0x0f;

    buf[0] = b0;

    let hsize = if payload_size < PSIZE_3BYTE {
        buf[1] = payload_size as u8;
        2
    } else if payload_size < PSIZE_9BYTE {
        buf[1] = PSIZE_3BYTE as u8;
        let arr = (payload_size as u16).to_be_bytes();
        buf[2..4].copy_from_slice(&arr);
        4
    } else {
        buf[1] = 0x7f;
        let arr = (payload_size as u64).to_be_bytes();
        buf[2..10].copy_from_slice(&arr);
        10
    };

    if let Some(mask) = mask {
        buf[1] |= 0x80;
        buf[hsize..hsize + 4].copy_from_slice(&mask);
        Ok(hsize + 4)
    } else {
        Ok(hsize)
    }
}

fn apply_mask(buf: &mut [u8], mask: [u8; 4], offset: usize) {
    for (i, c) in buf.iter_mut().enumerate() {
        *c ^= mask[(offset + i) % 4];
    }
}

fn parse_empty(s: &str, dest: &mut bool) -> Result<(), io::Error> {
    // must not be set yet and value must be empty
    if *dest || !s.is_empty() {
        return Err(io::Error::from(io::ErrorKind::InvalidData));
    }

    *dest = true;

    Ok(())
}

// set default to allow the param with no value
fn parse_bits(s: &str, dest: &mut Option<u8>, default: Option<u8>) -> Result<(), io::Error> {
    // must not be set yet
    if dest.is_some() {
        return Err(io::Error::from(io::ErrorKind::InvalidData));
    }

    if s.is_empty() {
        if let Some(x) = default {
            *dest = Some(x);
            return Ok(());
        }
    }

    // must be a valid u8
    let x = match s.parse() {
        Ok(x) => x,
        Err(_) => return Err(io::Error::from(io::ErrorKind::InvalidData)),
    };

    // number must be between 8 and 15, inclusive
    if x >= 8 && x <= 15 {
        *dest = Some(x);
        return Ok(());
    }

    Err(io::Error::from(io::ErrorKind::InvalidData))
}

pub struct PerMessageDeflateConfig {
    pub client_no_context_takeover: bool,
    pub server_no_context_takeover: bool,
    pub client_max_window_bits: u8,
    pub server_max_window_bits: u8,
}

impl PerMessageDeflateConfig {
    pub fn from_params(params: HeaderParamsIterator) -> Result<Self, io::Error> {
        let mut client_no_context_takeover = false;
        let mut server_no_context_takeover = false;
        let mut client_max_window_bits = None;
        let mut server_max_window_bits = None;

        for param in params {
            let (k, v) = param?;

            match k {
                "client_no_context_takeover" => parse_empty(v, &mut client_no_context_takeover)?,
                "server_no_context_takeover" => parse_empty(v, &mut server_no_context_takeover)?,
                "client_max_window_bits" => parse_bits(v, &mut client_max_window_bits, Some(15))?,
                "server_max_window_bits" => parse_bits(v, &mut server_max_window_bits, None)?,
                _ => return Err(io::Error::from(io::ErrorKind::InvalidData)), // undefined param
            }
        }

        Ok(Self {
            client_no_context_takeover,
            server_no_context_takeover,
            client_max_window_bits: client_max_window_bits.unwrap_or(DEFAULT_MAX_WINDOW_BITS),
            server_max_window_bits: server_max_window_bits.unwrap_or(DEFAULT_MAX_WINDOW_BITS),
        })
    }

    pub fn create_response(&self) -> Result<Self, ()> {
        // we don't support non-default server_max_window_bits
        if self.server_max_window_bits != DEFAULT_MAX_WINDOW_BITS {
            return Err(());
        }

        Ok(Self {
            // ack. makes no difference to us
            client_no_context_takeover: self.client_no_context_takeover,
            // ack. we'll agree to whatever the client wants
            server_no_context_takeover: self.server_no_context_takeover,
            // ignore. we always support the maximum window size
            client_max_window_bits: DEFAULT_MAX_WINDOW_BITS,
            // ignore. we require the client to support the maximum window size
            server_max_window_bits: DEFAULT_MAX_WINDOW_BITS,
        })
    }

    pub fn serialize<W: Write>(&self, w: &mut W) -> Result<(), io::Error> {
        if self.client_no_context_takeover {
            write!(w, "; client_no_context_takeover")?;
        }

        if self.server_no_context_takeover {
            write!(w, "; server_no_context_takeover")?;
        }

        if self.client_max_window_bits != DEFAULT_MAX_WINDOW_BITS {
            write!(
                w,
                "; client_max_window_bits={}",
                self.client_max_window_bits
            )?;
        }

        if self.server_max_window_bits != DEFAULT_MAX_WINDOW_BITS {
            write!(
                w,
                "; server_max_window_bits={}",
                self.server_max_window_bits
            )?;
        }

        Ok(())
    }
}

impl Default for PerMessageDeflateConfig {
    fn default() -> Self {
        Self {
            client_no_context_takeover: false,
            server_no_context_takeover: false,
            client_max_window_bits: DEFAULT_MAX_WINDOW_BITS,
            server_max_window_bits: DEFAULT_MAX_WINDOW_BITS,
        }
    }
}

trait ArrayVecExt<T> {
    fn resize(&mut self, new_len: usize, value: T);
    fn shift_left(&mut self, amount: usize);
}

impl<T: Copy, const N: usize> ArrayVecExt<T> for ArrayVec<T, N> {
    fn resize(&mut self, new_len: usize, value: T) {
        assert!(new_len <= self.capacity());

        if new_len > self.len() {
            let old_len = self.len();
            unsafe {
                self.set_len(new_len);
            }
            self[old_len..].fill(value);
        } else if new_len < self.len() {
            self.truncate(new_len);
        }
    }

    fn shift_left(&mut self, amount: usize) {
        assert!(amount <= self.len());

        self.copy_within(amount.., 0);

        unsafe {
            self.set_len(self.len() - amount);
        }
    }
}

pub struct DeflateEncoder {
    enc: Box<deflate::core::CompressorOxide>,
    next_buf: ArrayVec<u8, ENC_NEXT_BUF_SIZE>,
    end: bool,
}

impl DeflateEncoder {
    pub fn new() -> Self {
        let mut enc = Box::new(deflate::core::CompressorOxide::default());

        enc.set_format_and_level(
            DataFormat::Raw,
            deflate::CompressionLevel::DefaultLevel as u8,
        );

        Self {
            enc,
            next_buf: ArrayVec::new(),
            end: false,
        }
    }

    pub fn reset(&mut self) {
        self.enc.reset();
    }

    pub fn encode(
        &mut self,
        src: &[u8],
        end: bool,
        dest: &mut [u8],
    ) -> Result<(usize, usize, bool), io::Error> {
        let (read, mut written, mut end_ack) = self.encode_step(src, end, dest)?;

        if !src.is_empty() && read == src.len() && end && !end_ack {
            let (r, w, ea) = self.encode_step(&[], end, &mut dest[written..])?;

            assert_eq!(r, 0);
            written += w;
            end_ack = ea;
        }

        Ok((read, written, end_ack))
    }

    pub fn encode_to_ringbuffer<T: AsRef<[u8]> + AsMut<[u8]>>(
        &mut self,
        src: &[u8],
        end: bool,
        dest: &mut BaseRingBuffer<T>,
    ) -> Result<(usize, bool), io::Error> {
        let wbuf = dest.write_buf();

        let (mut read, written, mut end_ack) = self.encode(src, end, wbuf)?;

        let write_maxed = written == wbuf.len();
        dest.write_commit(written);

        if !end_ack && write_maxed && dest.write_avail() > 0 {
            let (r, written, ea) = self.encode(&src[read..], end, dest.write_buf())?;

            dest.write_commit(written);

            read += r;
            end_ack = ea;
        }

        Ok((read, end_ack))
    }

    fn encode_step(
        &mut self,
        src: &[u8],
        end: bool,
        dest: &mut [u8],
    ) -> Result<(usize, usize, bool), io::Error> {
        // once end=true has been processed, the caller must stop providing
        // data in src and must continue to set end until end is returned
        if self.end && (!src.is_empty() || !end) {
            return Err(io::Error::from(io::ErrorKind::Other));
        }

        let next_buf = &mut self.next_buf;

        // we want to flush exactly once per message. to ensure this, we
        // flush only when there is no more input (to avoid a situation of
        // input not being accepted at the time of flush) and if we have not
        // flushed yet for the current message
        let flush = if src.is_empty() && end && !self.end {
            self.end = true;

            MZFlush::Sync
        } else {
            MZFlush::None
        };

        let (consumed, written, maybe_more) =
            if dest.len() > next_buf.len() && dest.len() >= next_buf.remaining_capacity() {
                // if there's enough room in dest to hold all of next_buf plus at
                // least one more byte, and there's at least as much room in dest
                // as in next_buf, then encode directly into dest

                // move next_buf into dest
                let offset = next_buf.len();
                dest[..offset].copy_from_slice(next_buf.as_ref());
                next_buf.clear();

                // encode into the remaining space
                let (result, maybe_more) = {
                    let dest = &mut dest[offset..];
                    assert!(!dest.is_empty());

                    let result = deflate::stream::deflate(&mut self.enc, src, dest, flush);

                    match result.status {
                        Ok(MZStatus::Ok) => {}
                        Err(MZError::Buf) => {}
                        _ => return Err(io::Error::from(io::ErrorKind::Other)),
                    }

                    assert!(result.bytes_consumed <= src.len());
                    assert!(result.bytes_written <= dest.len());

                    (result, result.bytes_written == dest.len())
                };

                let dest = &mut dest[..(offset + result.bytes_written)];

                // keep back the ending bytes in next_buf
                assert!(next_buf.is_empty());
                let keep = cmp::min(ENC_NEXT_BUF_SIZE, dest.len());
                next_buf.write(&dest[(dest.len() - keep)..]).unwrap();

                let written = dest.len() - keep;

                (result.bytes_consumed, written, maybe_more)
            } else {
                // if next_buf can't fit into dest with room to spare, or if
                // there's more room in next_buf than in dest, then encode into a
                // temporary buffer and move the bytes into place afterwards.
                // note that the temporary buffer will be small

                // dest.len() is either less than or equal to next_buf.len()
                // or less than next_buf.remaining_capacity(). in either case
                // this will not exceed next_buf's capacity
                assert!(dest.len() <= ENC_NEXT_BUF_SIZE);

                // stating the obvious
                assert!(next_buf.remaining_capacity() <= ENC_NEXT_BUF_SIZE);

                let tmp_size = dest.len() + next_buf.remaining_capacity();

                // based on above asserts
                assert!(tmp_size <= ENC_NEXT_BUF_SIZE * 2);

                let mut tmp: ArrayVec<u8, { ENC_NEXT_BUF_SIZE * 2 }> = ArrayVec::new();
                tmp.resize(tmp_size, 0);

                // encode into tmp
                let (result, maybe_more) = {
                    let result = deflate::stream::deflate(&mut self.enc, src, tmp.as_mut(), flush);

                    match result.status {
                        Ok(MZStatus::Ok) => {}
                        Err(MZError::Buf) => {}
                        _ => return Err(io::Error::from(io::ErrorKind::Other)),
                    }

                    assert!(result.bytes_consumed <= src.len());
                    assert!(result.bytes_written <= tmp.len());

                    (result, result.bytes_written == tmp.len())
                };

                tmp.truncate(result.bytes_written);

                let mut written = 0;

                // if the encoded bytes don't fit in next_buf, then we can
                // move some bytes to dest
                if tmp.len() > next_buf.remaining_capacity() {
                    let to_write = tmp.len() - next_buf.remaining_capacity();

                    // move the starting bytes of next_buf to the front of dest
                    let size = cmp::min(to_write, next_buf.len());
                    dest[..size].copy_from_slice(&next_buf[..size]);
                    next_buf.shift_left(size);

                    written += size;

                    // if dest still has room, move from tmp
                    if written < to_write {
                        assert!(next_buf.is_empty());

                        let size = to_write - written;
                        assert!(size <= tmp.len());

                        dest[written..(written + size)].copy_from_slice(&tmp[..size]);
                        tmp.shift_left(size);

                        written += size;
                    }
                }

                // append tmp to next_buf
                next_buf.write(tmp.as_ref()).unwrap();

                (result.bytes_consumed, written, maybe_more)
            };

        let mut end_ack = false;

        if self.end
            && consumed == src.len()
            && next_buf.len() == DEFLATE_SUFFIX.len()
            && !maybe_more
        {
            if next_buf.as_ref() != DEFLATE_SUFFIX {
                return Err(io::Error::from(io::ErrorKind::Other));
            }

            self.next_buf.clear();
            self.end = false;
            end_ack = true;
        }

        Ok((consumed, written, end_ack))
    }
}

pub struct DeflateDecoder {
    dec: Box<InflateState>,
    suffix_pos: Option<usize>,
}

impl DeflateDecoder {
    pub fn new() -> Self {
        Self {
            dec: InflateState::new_boxed(DataFormat::Raw),
            suffix_pos: None,
        }
    }
}

pub trait Decoder {
    fn decode(
        &mut self,
        src: &[u8],
        end: bool,
        dest: &mut [u8],
    ) -> Result<(usize, usize, bool), io::Error>;
}

impl Decoder for DeflateDecoder {
    fn decode(
        &mut self,
        src: &[u8],
        end: bool,
        dest: &mut [u8],
    ) -> Result<(usize, usize, bool), io::Error> {
        let (consumed, mut written) = if self.suffix_pos.is_none() {
            let result = inflate(&mut self.dec, &src, dest, MZFlush::None);

            match result.status {
                Ok(MZStatus::Ok) => {}
                Err(MZError::Buf) => {}
                _ => return Err(io::Error::from(io::ErrorKind::Other)),
            }

            assert!(result.bytes_consumed <= src.len());
            assert!(result.bytes_written <= dest.len());

            if result.bytes_consumed == src.len() && end {
                self.suffix_pos = Some(0);
            }

            if result.bytes_written == dest.len() {
                return Ok((result.bytes_consumed, result.bytes_written, false));
            }

            (result.bytes_consumed, result.bytes_written)
        } else {
            (0, 0)
        };

        let mut end_ack = false;

        if let Some(pos) = &mut self.suffix_pos {
            // if the input is fully consumed when end is set, then the
            // caller must continue to set end until end is returned
            if !end {
                return Err(io::Error::from(io::ErrorKind::Other));
            }

            let dest = &mut dest[written..];

            let suffix = DEFLATE_SUFFIX;
            let suffix_left = &suffix[*pos..];

            let result = inflate(&mut self.dec, suffix_left, dest, MZFlush::None);

            match result.status {
                Ok(MZStatus::Ok) => {}
                Err(MZError::Buf) => {}
                _ => return Err(io::Error::from(io::ErrorKind::Other)),
            }

            assert!(result.bytes_consumed <= suffix_left.len());
            assert!(result.bytes_written <= dest.len());

            *pos += result.bytes_consumed;

            // we are done when the entire input is consumed and there is
            // space left in the output buffer. if there is no space left in
            // the output buffer then there might be more to write
            if *pos == suffix.len() && result.bytes_written < dest.len() {
                self.suffix_pos = None;
                end_ack = true;
            }

            written += result.bytes_written;
        }

        Ok((consumed, written, end_ack))
    }
}

pub fn deflate_codec_state_size() -> usize {
    let encoder_size = mem::size_of::<deflate::core::CompressorOxide>();
    let decoder_size = mem::size_of::<InflateState>();

    encoder_size + decoder_size
}

// call preprocess_fn on any bytes about to be decoded. this can be used
// to apply mask processing as needed
fn decode_from_refread<T, D, F>(
    src: &mut T,
    limit: usize,
    end: bool,
    dec: &mut D,
    dest: &mut [u8],
    mut preprocess_fn: F,
) -> Result<(usize, bool), io::Error>
where
    T: RefRead + ?Sized,
    D: Decoder,
    F: FnMut(&mut [u8], usize),
{
    let buf = src.get_mut();
    let limit = cmp::min(limit, buf.len());
    let buf = &mut buf[..limit];

    preprocess_fn(buf, 0);

    let (read, mut written, mut end_ack) = dec.decode(buf, end, dest)?;

    let read_maxed = read == buf.len();
    src.consume(read);

    let buf = src.get_mut();
    let buf = &mut buf[..(limit - read)];

    if !end_ack && read_maxed && !buf.is_empty() {
        // this will not overlap with previously preprocessed bytes
        preprocess_fn(buf, read);

        let (read, w, ea) = dec.decode(buf, end, &mut dest[written..])?;

        src.consume(read);

        written += w;
        end_ack = ea;
    }

    Ok((written, end_ack))
}

fn unmask_and_decode<T, D>(
    src: &mut T,
    limit: usize,
    end: bool,
    mask: Option<[u8; 4]>,
    mask_offset: usize,
    dec: &mut D,
    dest: &mut [u8],
) -> Result<(usize, usize, bool), io::Error>
where
    T: RefRead + ?Sized,
    D: Decoder,
{
    // if a mask needs to be applied, it needs to be applied to the
    // received bytes before they are passed to the decoder. however,
    // we don't know in advance how many bytes the decoder will
    // accept. in order to preserve the integrity of the input
    // buffer, and to avoid copying, we apply the mask directly to
    // the input buffer and then revert it on any bytes that weren't
    // accepted. in the best case, the decoder will accept all the
    // bytes with nothing to revert. in the worst case, the decoder
    // will accept nothing and all the bytes will be reverted

    let mut masked = 0;
    let orig_len = src.len();

    let (written, output_end) = decode_from_refread(src, limit, end, dec, dest, |buf, offset| {
        if let Some(mask) = mask {
            apply_mask(buf, mask, mask_offset + offset);
            masked += buf.len();
        }
    })?;

    let read = orig_len - src.len();

    if let Some(mask) = mask {
        // undo the mask on any unread bytes

        assert!(masked >= read);
        masked -= read;

        let mut bufs_arr = MaybeUninit::uninit();
        let bufs = src.get_mut_vectored(&mut bufs_arr).limit(masked);

        let mut count = 0;
        for buf in bufs {
            apply_mask(buf, mask, mask_offset + read + count);
            count += buf.len();
        }
    }

    Ok((read, written, output_end))
}

#[cfg(test)]
pub struct Frame<'a> {
    pub opcode: u8,
    pub data: &'a [u8],
    pub fin: bool,
}

#[derive(PartialEq)]
pub enum CompressionMode {
    Compressed,
    Uncompressed,
}

#[derive(Debug, PartialEq, Clone, Copy)]
pub enum State {
    // call: send_frame, recv_frame
    // next: Connected, PeerClosed, Closing
    Connected,

    // call: send_frame
    // next: PeerClosed, Finished
    PeerClosed,

    // call: recv_frame
    // next: Closing, Finished
    Closing,

    // session has completed
    Finished,
}

#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    InvalidControlFrame,
    UnexpectedOpcode,
    CompressionError,
}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Self {
        Self::Io(e)
    }
}

struct SendingFrame {
    header: [u8; HEADER_SIZE_MAX],
    header_len: usize,
    sent: usize,
}

struct SendingMessage {
    opcode: u8,
    mask: Option<[u8; 4]>,
    frame_sent: bool,
    enc_output_end: bool,
}

struct ReceivingMessage {
    opcode: u8,
    frame_payload_read: usize,
    compression_mode: CompressionMode,
}

struct Sending {
    frame: RefCell<Option<SendingFrame>>,
    message: RefCell<Option<SendingMessage>>,
}

struct Receiving {
    frame: Option<FrameInfo>,
    message: Option<ReceivingMessage>,
}

struct DeflateState<T> {
    enc: DeflateEncoder,
    dec: DeflateDecoder,
    allow_takeover: bool,
    enc_buf: BaseRingBuffer<T>,
}

pub struct Protocol<T> {
    state: Cell<State>,
    sending: Sending,
    receiving: RefCell<Receiving>,
    deflate_state: Option<RefCell<DeflateState<T>>>,
}

impl<'buf, T: AsRef<[u8]> + AsMut<[u8]>> Protocol<T> {
    pub fn new(deflate_config: Option<(PerMessageDeflateConfig, BaseRingBuffer<T>)>) -> Self {
        let deflate_state = match deflate_config {
            Some((config, enc_buf)) => Some(RefCell::new(DeflateState {
                enc: DeflateEncoder::new(),
                dec: DeflateDecoder::new(),
                allow_takeover: !config.server_no_context_takeover,
                enc_buf,
            })),
            None => None,
        };

        Self {
            state: Cell::new(State::Connected),
            sending: Sending {
                frame: RefCell::new(None),
                message: RefCell::new(None),
            },
            receiving: RefCell::new(Receiving {
                frame: None,
                message: None,
            }),
            deflate_state,
        }
    }

    pub fn state(&self) -> State {
        self.state.get()
    }

    pub fn send_frame<W: Write>(
        &self,
        writer: &mut W,
        opcode: u8,
        src: &[&[u8]],
        fin: bool,
        rsv1: bool,
        mask: Option<[u8; 4]>,
    ) -> Result<usize, Error> {
        assert!(self.state.get() == State::Connected || self.state.get() == State::PeerClosed);

        let sending_frame = &mut *self.sending.frame.borrow_mut();

        let mut src_len = 0;
        for buf in src.iter() {
            src_len += buf.len();
        }

        if sending_frame.is_none() {
            let mut h = [0; HEADER_SIZE_MAX];

            let size = write_header(fin, rsv1, opcode, src_len, mask, &mut h[..])?;

            *sending_frame = Some(SendingFrame {
                header: h,
                header_len: size,
                sent: 0,
            });
        }

        let frame = sending_frame.as_mut().unwrap();

        let header = &frame.header[..frame.header_len];

        let total = header.len() + src_len;

        let mut out_arr = [&b""[..]; VECTORED_MAX];
        let mut out_arr_len = 0;

        out_arr[0] = header;
        out_arr_len += 1;

        for buf in src.iter() {
            out_arr[out_arr_len] = buf;
            out_arr_len += 1;
        }

        let out = &out_arr[..out_arr_len];
        let size = write_vectored_offset(writer, out, frame.sent)?;

        if log_enabled!(log::Level::Trace) {
            trace!("OUT sock {}", Bufs::new(out));
        }

        frame.sent += size;

        if frame.sent < total {
            return Ok(0);
        }

        *sending_frame = None;

        if opcode == OPCODE_CLOSE {
            if self.state.get() == State::PeerClosed {
                self.state.set(State::Finished);
            } else {
                self.state.set(State::Closing);
            }
        }

        Ok(src_len)
    }

    // on success, it's up to the caller to advance the buffer by frame.data.len()
    #[cfg(test)]
    pub fn recv_frame<R: RefRead>(
        &mut self,
        rbuf: &'buf mut R,
    ) -> Option<Result<Frame<'buf>, Error>> {
        assert!(self.state.get() == State::Connected || self.state.get() == State::Closing);

        let receiving = &mut *self.receiving.borrow_mut();

        if receiving.frame.is_none() {
            let fi = match read_header(rbuf.get_ref()) {
                Ok(fi) => fi,
                Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => return None,
                Err(e) => return Some(Err(e.into())),
            };

            rbuf.consume(fi.payload_offset);

            receiving.frame = Some(fi);
        }

        let fi = receiving.frame.unwrap();

        if rbuf.get_ref().len() < fi.payload_size {
            return None;
        }

        if fi.opcode == OPCODE_CLOSE {
            if self.state.get() == State::Closing {
                self.state.set(State::Finished);
            } else {
                self.state.set(State::PeerClosed);
            }
        }

        let buf = rbuf.get_mut();

        if let Some(mask) = fi.mask {
            apply_mask(buf, mask, 0);
        }

        receiving.frame = None;

        Some(Ok(Frame {
            opcode: fi.opcode,
            data: &buf[..fi.payload_size],
            fin: fi.fin,
        }))
    }

    pub fn is_sending_message(&self) -> bool {
        self.sending.message.borrow().is_some()
    }

    pub fn send_message_start(&self, opcode: u8, mask: Option<[u8; 4]>) {
        assert!(self.state.get() == State::Connected || self.state.get() == State::PeerClosed);

        let sending_message = &mut *self.sending.message.borrow_mut();

        assert_eq!(sending_message.is_some(), false);

        *sending_message = Some(SendingMessage {
            opcode,
            mask,
            frame_sent: false,
            enc_output_end: false,
        });
    }

    pub fn send_message_content<W: Write>(
        &self,
        writer: &mut W,
        src: &[&[u8]],
        end: bool,
    ) -> Result<(usize, bool), Error> {
        assert!(self.state.get() == State::Connected || self.state.get() == State::PeerClosed);

        let mut sending_message = self.sending.message.borrow_mut();

        let msg = sending_message.as_mut().unwrap();

        let mut src_len = 0;
        for buf in src.iter() {
            src_len += buf.len();
        }

        let is_control = msg.opcode & 0x08 != 0;

        // control frames (ping, pong, close) must have a small payload length
        //   and must not be fragmented
        if is_control && (src_len > CONTROL_FRAME_PAYLOAD_MAX || !end) {
            return Err(Error::InvalidControlFrame);
        }

        let opcode = if msg.frame_sent {
            OPCODE_CONTINUATION
        } else {
            msg.opcode
        };

        let (read, sent_all) = match &self.deflate_state {
            Some(state) if !is_control => {
                let state = &mut *state.borrow_mut();

                let mut read = 0;

                if !msg.enc_output_end {
                    if src_len > 0 {
                        for (i, buf) in src.iter().enumerate() {
                            // only set end on the last buf
                            let end = end && (i == src.len() - 1);

                            let dest = &mut state.enc_buf;

                            let (r, oe) = state.enc.encode_to_ringbuffer(buf, end, dest)?;

                            read += r;
                            msg.enc_output_end = oe;

                            if r < buf.len() || oe {
                                break;
                            }
                        }
                    } else {
                        let dest = &mut state.enc_buf;

                        let (_, oe) = state.enc.encode_to_ringbuffer(&[], end, dest)?;

                        msg.enc_output_end = oe;
                    }
                }

                // we should never get EOS if there are bytes left to send
                assert!(!msg.enc_output_end || read == src_len);

                let mut sent_all = false;

                if state.enc_buf.read_avail() > 0 || msg.enc_output_end {
                    // send_frame adds 1 element to vector
                    let mut bufs_arr = [&b""[..]; VECTORED_MAX - 1];
                    let bufs = state.enc_buf.get_ref_vectored(&mut bufs_arr);

                    let size = self.send_frame(
                        writer,
                        opcode,
                        bufs,
                        msg.enc_output_end,
                        opcode != OPCODE_CONTINUATION, // set rsv1 on first frame
                        msg.mask,
                    )?;

                    state.enc_buf.read_commit(size);

                    msg.frame_sent = true;

                    sent_all = msg.enc_output_end && state.enc_buf.read_avail() == 0;
                }

                (read, sent_all)
            }
            _ => {
                let read = self.send_frame(writer, opcode, src, end, false, msg.mask)?;

                msg.frame_sent = true;

                (read, end && read == src_len)
            }
        };

        if sent_all && self.sending.frame.borrow().is_none() {
            *sending_message = None;

            if let Some(state) = &self.deflate_state {
                let mut state = state.borrow_mut();

                if !state.allow_takeover {
                    state.enc.reset();
                }
            }
        }

        let done = sending_message.is_none();

        Ok((read, done))
    }

    pub fn recv_message_content<R: RefRead>(
        &self,
        rbuf: &mut R,
        dest: &mut [u8],
    ) -> Option<Result<(u8, usize, bool), Error>> {
        assert!(self.state.get() == State::Connected || self.state.get() == State::Closing);

        let receiving = &mut *self.receiving.borrow_mut();

        if receiving.frame.is_none() {
            let fi = match read_header(rbuf.get_ref()) {
                Ok(fi) => fi,
                Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => return None,
                Err(e) => return Some(Err(e.into())),
            };

            rbuf.consume(fi.payload_offset);

            receiving.frame = Some(fi);

            if let Some(msg) = &mut receiving.message {
                if fi.opcode != OPCODE_CONTINUATION {
                    return Some(Err(Error::UnexpectedOpcode));
                }

                // only the first frame should set this bit
                if fi.rsv1 {
                    return Some(Err(Error::CompressionError));
                }

                msg.frame_payload_read = 0;
            } else {
                if fi.opcode == OPCODE_CONTINUATION {
                    return Some(Err(Error::UnexpectedOpcode));
                }

                if fi.opcode & 0x08 != 0
                    && (fi.payload_size > CONTROL_FRAME_PAYLOAD_MAX || !fi.fin || fi.rsv1)
                {
                    return Some(Err(Error::InvalidControlFrame));
                }

                let compression_mode = if fi.rsv1 {
                    CompressionMode::Compressed
                } else {
                    CompressionMode::Uncompressed
                };

                receiving.message = Some(ReceivingMessage {
                    opcode: fi.opcode,
                    frame_payload_read: 0,
                    compression_mode,
                });
            }
        }

        let fi = receiving.frame.as_ref().unwrap();
        let msg = receiving.message.as_mut().unwrap();

        let (written, frame_end) = if msg.compression_mode == CompressionMode::Compressed {
            let state = match &self.deflate_state {
                Some(state) => state,
                None => return Some(Err(Error::CompressionError)),
            };

            let mut state = state.borrow_mut();

            let left = fi.payload_size - msg.frame_payload_read;
            let limit = cmp::min(left, rbuf.len());

            let end = ((msg.frame_payload_read + limit) == fi.payload_size) && fi.fin;

            let (read, written, output_end) = match unmask_and_decode(
                rbuf,
                limit,
                end,
                fi.mask,
                msg.frame_payload_read,
                &mut state.dec,
                dest,
            ) {
                Ok(ret) => ret,
                Err(e) => return Some(Err(e.into())),
            };

            msg.frame_payload_read += read;

            let frame_end = if fi.fin {
                // no output progress means we need more input
                if written == 0 && !output_end {
                    return None;
                }

                // finish final frame only when we hit EOS
                (msg.frame_payload_read == fi.payload_size) && output_end
            } else {
                msg.frame_payload_read == fi.payload_size
            };

            (written, frame_end)
        } else {
            let buf = rbuf.get_ref();

            // control frames must be available in their entirety
            if fi.opcode & 0x08 != 0 && buf.len() < fi.payload_size {
                return None;
            }

            let left = fi.payload_size - msg.frame_payload_read;

            if left > 0 && buf.len() == 0 {
                return None;
            }

            let buf = &buf[..cmp::min(left, buf.len())];

            let size = cmp::min(buf.len(), dest.len());

            let dest = &mut dest[..size];

            dest.copy_from_slice(&buf[..size]);

            rbuf.consume(size);

            if let Some(mask) = fi.mask {
                apply_mask(dest, mask, msg.frame_payload_read);
            }

            msg.frame_payload_read += size;

            assert!(msg.frame_payload_read <= fi.payload_size);

            (size, msg.frame_payload_read == fi.payload_size)
        };

        let opcode = msg.opcode;
        let fin = fi.fin;

        if frame_end {
            receiving.frame = None;

            if fin {
                receiving.message = None;

                if opcode == OPCODE_CLOSE {
                    if self.state.get() == State::Closing {
                        self.state.set(State::Finished);
                    } else {
                        self.state.set(State::PeerClosed);
                    }
                }
            }
        }

        Some(Ok((opcode, written, receiving.message.is_none())))
    }
}

pub mod testutil {
    use super::*;
    use crate::buffer::{RingBuffer, TmpBuffer};
    use std::rc::Rc;

    pub struct BenchSendMessageArgs {
        protocol: Protocol<Vec<u8>>,
        dest: Vec<u8>,
    }

    pub struct BenchSendMessage {
        use_deflate: bool,
        content: Vec<u8>,
    }

    impl BenchSendMessage {
        pub fn new(use_deflate: bool) -> Self {
            let mut content = Vec::with_capacity(1024);
            for i in 0..1024 {
                content.push((i % 256) as u8);
            }

            Self {
                use_deflate,
                content,
            }
        }

        pub fn init(&self) -> BenchSendMessageArgs {
            let deflate_config = if self.use_deflate {
                let tmp = Rc::new(TmpBuffer::new(256));

                Some((
                    PerMessageDeflateConfig::default(),
                    RingBuffer::new(256, &tmp),
                ))
            } else {
                None
            };

            BenchSendMessageArgs {
                protocol: Protocol::new(deflate_config),
                dest: Vec::with_capacity(16_384),
            }
        }

        pub fn run(&self, args: &mut BenchSendMessageArgs) {
            let p = &mut args.protocol;
            let src = &self.content;
            let dest = &mut args.dest;

            p.send_message_start(OPCODE_TEXT, None);

            let mut src_pos = 0;

            loop {
                let (size, done) = p
                    .send_message_content(dest, &[&src[src_pos..]], true)
                    .unwrap();

                src_pos += size;

                assert!(dest.len() < dest.capacity() || done);

                if done {
                    break;
                }
            }
        }
    }

    pub struct BenchRecvMessageArgs {
        protocol: Protocol<Vec<u8>>,
        rbuf: RingBuffer,
        dest: Vec<u8>,
    }

    pub struct BenchRecvMessage {
        use_deflate: bool,
        tmp: Rc<TmpBuffer>,
        msg: Vec<u8>,
    }

    impl BenchRecvMessage {
        pub fn new(use_deflate: bool) -> Self {
            let mut content = Vec::with_capacity(1024);
            for i in 0..1024 {
                content.push((i % 256) as u8);
            }

            let tmp = Rc::new(TmpBuffer::new(16_384));

            let deflate_config = if use_deflate {
                Some((
                    PerMessageDeflateConfig::default(),
                    RingBuffer::new(16_384, &tmp),
                ))
            } else {
                None
            };

            let p = Protocol::new(deflate_config);

            let mut msg = Vec::new();
            p.send_message_start(OPCODE_TEXT, None);
            let (size, done) = p.send_message_content(&mut msg, &[&content], true).unwrap();
            assert_eq!(size, content.len());
            assert_eq!(done, true);

            Self {
                use_deflate,
                tmp,
                msg,
            }
        }

        pub fn init(&self) -> BenchRecvMessageArgs {
            let deflate_config = if self.use_deflate {
                Some((
                    PerMessageDeflateConfig::default(),
                    RingBuffer::new(256, &self.tmp),
                ))
            } else {
                None
            };

            let mut rbuf = RingBuffer::new(16_384, &self.tmp);

            let size = rbuf.write(&self.msg).unwrap();
            assert_eq!(size, self.msg.len());

            let mut dest = Vec::with_capacity(16_384);
            dest.resize(dest.capacity(), 0);

            BenchRecvMessageArgs {
                protocol: Protocol::new(deflate_config),
                rbuf,
                dest,
            }
        }

        pub fn run(&self, args: &mut BenchRecvMessageArgs) {
            let p = &mut args.protocol;
            let rbuf = &mut args.rbuf;
            let dest = &mut args.dest;

            let mut dest_pos = 0;

            loop {
                let (_, size, end) = p
                    .recv_message_content(rbuf, &mut dest[dest_pos..])
                    .unwrap()
                    .unwrap();

                dest_pos += size;

                assert!(dest_pos < dest.len() || end);

                if end {
                    break;
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::testutil::*;
    use super::*;
    use crate::buffer::{RingBuffer, TmpBuffer};
    use std::rc::Rc;

    struct MyWriter {
        data: Vec<u8>,
    }

    impl MyWriter {
        fn new() -> Self {
            Self { data: Vec::new() }
        }
    }

    impl Write for MyWriter {
        fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
            self.data.extend_from_slice(buf.as_ref());

            Ok(buf.len())
        }

        fn write_vectored(&mut self, bufs: &[io::IoSlice]) -> Result<usize, io::Error> {
            let mut total = 0;

            for buf in bufs {
                total += buf.len();
                self.data.extend_from_slice(buf.as_ref());
            }

            Ok(total)
        }

        fn flush(&mut self) -> Result<(), io::Error> {
            Ok(())
        }
    }

    #[test]
    fn test_header_size() {
        assert_eq!(header_size(0, false), 2);
        assert_eq!(header_size(125, false), 2);
        assert_eq!(header_size(125, true), 6);

        assert_eq!(header_size(126, false), 4);
        assert_eq!(header_size(65535, false), 4);
        assert_eq!(header_size(65535, true), 8);

        assert_eq!(header_size(65536, false), 10);
        assert_eq!(header_size(65536, true), HEADER_SIZE_MAX);
    }

    #[test]
    fn test_read_write_header() {
        let mut buf = [
            0x81, 0x85, 0x01, 0x02, 0x03, 0x04, 0x69, 0x67, 0x6f, 0x68, 0x6e,
        ];

        let r = read_header(&buf);
        assert!(r.is_ok());

        let fi = r.unwrap();
        assert_eq!(fi.fin, true);
        assert_eq!(fi.opcode, OPCODE_TEXT);
        assert_eq!(fi.mask, Some([0x01, 0x02, 0x03, 0x04]));
        assert_eq!(fi.payload_offset, 6);
        assert_eq!(fi.payload_size, 5);

        let end = fi.payload_offset + fi.payload_size;
        let payload = &mut buf[fi.payload_offset..end];
        apply_mask(payload, (&fi.mask).unwrap(), 0);
        assert_eq!(payload, b"hello");

        let payload = b"hello";
        let mut buf2 = Vec::new();
        buf2.resize(header_size(payload.len(), true) + payload.len(), 0);
        let r = write_header(
            true,
            false,
            OPCODE_TEXT,
            payload.len(),
            Some([0x01, 0x02, 0x03, 0x04]),
            &mut buf2,
        );
        assert!(r.is_ok());

        let offset = r.unwrap();
        assert_eq!(offset, 6);
        buf2[offset..offset + payload.len()].copy_from_slice(payload);
        assert_eq!(buf2, buf);
    }

    #[test]
    fn test_apply_mask() {
        let mut buf = [b'a', b'b', b'c', b'd', b'e'];
        apply_mask(&mut buf, [0x01, 0x02, 0x03, 0x04], 0);
        assert_eq!(buf, [0x60, 0x60, 0x60, 0x60, 0x64]);
    }

    #[test]
    fn test_deflate_bulk() {
        {
            let mut enc = DeflateEncoder::new();
            let mut dec = DeflateDecoder::new();
            let data = b"Hello";

            let mut compressed = [0; 1024];
            let (read, written, end) = enc.encode(data, true, &mut compressed).unwrap();
            assert_eq!(read, 5);
            assert_eq!(end, true);
            let compressed = &compressed[..written];
            let expected = [0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00];
            assert_eq!(compressed, &expected);

            let mut uncompressed = [0; 1024];
            let (read, written, end) = dec.decode(&compressed, true, &mut uncompressed).unwrap();
            assert_eq!(read, compressed.len());
            assert_eq!(end, true);
            let uncompressed = &uncompressed[..written];
            assert_eq!(uncompressed, data);
        }
    }

    #[test]
    fn test_deflate_by_byte() {
        {
            let mut enc = DeflateEncoder::new();
            let mut dec = DeflateDecoder::new();
            let data = b"Hello";

            let mut compressed = [0; 1024];
            assert_eq!(
                enc.encode(&[], false, &mut compressed).unwrap(),
                (0, 0, false)
            );

            let mut read_pos = 0;
            let mut write_pos = 0;
            loop {
                let (read, written, end) = enc
                    .encode(
                        &data[read_pos..],
                        true,
                        &mut compressed[write_pos..(write_pos + 1)],
                    )
                    .unwrap();
                // there must always be progress
                assert!(read > 0 || written > 0 || end);
                read_pos += read;
                write_pos += written;
                if end {
                    break;
                }
            }

            let compressed = &compressed[..write_pos];
            let expected = [0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00];
            assert_eq!(compressed, &expected);

            let mut uncompressed = [0; 1024];
            assert_eq!(
                dec.decode(&[], false, &mut uncompressed).unwrap(),
                (0, 0, false)
            );

            let mut read_pos = 0;
            let mut write_pos = 0;
            loop {
                let (read, written, end) = dec
                    .decode(
                        &compressed[read_pos..],
                        true,
                        &mut uncompressed[write_pos..(write_pos + 1)],
                    )
                    .unwrap();
                // there must always be progress
                assert!(read > 0 || written > 0 || end);
                read_pos += read;
                write_pos += written;
                if end {
                    break;
                }
            }
            assert_eq!(read_pos, compressed.len());
            let uncompressed = &uncompressed[..write_pos];
            assert_eq!(uncompressed, data);
        }
    }

    #[test]
    fn test_send_frame() {
        let p = Protocol::<[u8; 0]>::new(None);

        assert_eq!(p.state(), State::Connected);

        let mut writer = MyWriter::new();

        let size = p
            .send_frame(&mut writer, OPCODE_TEXT, &[b"hello"], true, false, None)
            .unwrap();

        assert_eq!(size, 5);
        assert_eq!(writer.data, b"\x81\x05hello");
        assert_eq!(p.state(), State::Connected);
    }

    #[test]
    fn test_send_message() {
        let p = Protocol::<[u8; 0]>::new(None);

        assert_eq!(p.state(), State::Connected);

        let mut writer = MyWriter::new();

        p.send_message_start(OPCODE_TEXT, None);

        let (size, done) = p
            .send_message_content(&mut writer, &[b"hel", b"lo"], true)
            .unwrap();
        assert_eq!(size, 5);
        assert_eq!(done, true);
        assert_eq!(writer.data, b"\x81\x05hello");
        assert_eq!(p.state(), State::Connected);

        writer.data.clear();
        p.send_message_start(OPCODE_TEXT, None);

        let (size, done) = p
            .send_message_content(&mut writer, &[b"hello"], false)
            .unwrap();
        assert_eq!(size, 5);
        assert_eq!(done, false);
        assert_eq!(writer.data, b"\x01\x05hello");
        assert_eq!(p.state(), State::Connected);

        writer.data.clear();

        let (size, done) = p.send_message_content(&mut writer, &[b""], true).unwrap();
        assert_eq!(size, 0);
        assert_eq!(done, true);
        assert_eq!(writer.data, b"\x80\x00");
        assert_eq!(p.state(), State::Connected);

        writer.data.clear();
        p.send_message_start(OPCODE_PING, None);

        let (size, done) = p
            .send_message_content(&mut writer, &[b"hello"], true)
            .unwrap();
        assert_eq!(size, 5);
        assert_eq!(done, true);
        assert_eq!(writer.data, b"\x89\x05hello");
        assert_eq!(p.state(), State::Connected);

        writer.data.clear();
        p.send_message_start(OPCODE_PING, None);

        let r = p.send_message_content(&mut writer, &[b"hello"], false);
        assert!(r.is_err());
    }

    #[test]
    fn test_recv_frame() {
        let mut data = b"\x81\x05hello".to_vec();

        let mut rbuf = io::Cursor::new(&mut data[..]);

        let mut p = Protocol::<[u8; 0]>::new(None);

        assert_eq!(p.state(), State::Connected);

        let frame = p.recv_frame(&mut rbuf).unwrap().unwrap();

        assert_eq!(frame.opcode, OPCODE_TEXT);
        assert_eq!(frame.data, b"hello");
        assert_eq!(frame.fin, true);

        let size = frame.data.len();
        rbuf.consume(size);

        assert_eq!(p.state(), State::Connected);
    }

    #[test]
    fn test_recv_message() {
        let mut data = b"\x81\x05hello".to_vec();

        let mut rbuf = io::Cursor::new(&mut data[..]);

        let p = Protocol::<[u8; 0]>::new(None);

        assert_eq!(p.state(), State::Connected);

        let mut dest = [0; 1024];

        let (opcode, size, end) = p
            .recv_message_content(&mut rbuf, &mut dest)
            .unwrap()
            .unwrap();
        let data = &dest[..size];

        assert_eq!(opcode, OPCODE_TEXT);
        assert_eq!(data, b"hello");
        assert_eq!(end, true);

        assert_eq!(p.state(), State::Connected);

        let mut data = b"".to_vec();

        let mut rbuf = io::Cursor::new(&mut data[..]);

        let r = p.recv_message_content(&mut rbuf, &mut dest);
        assert!(r.is_none());

        let mut data = b"\x01\x03hel\x80\x02lo".to_vec();

        let mut rbuf = io::Cursor::new(&mut data[..]);

        let (opcode, size, end) = p
            .recv_message_content(&mut rbuf, &mut dest)
            .unwrap()
            .unwrap();
        let data = &dest[..size];

        assert_eq!(opcode, OPCODE_TEXT);
        assert_eq!(data, b"hel");
        assert_eq!(end, false);

        let (opcode, size, end) = p
            .recv_message_content(&mut rbuf, &mut dest)
            .unwrap()
            .unwrap();
        let data = &dest[..size];

        assert_eq!(opcode, OPCODE_TEXT);
        assert_eq!(data, b"lo");
        assert_eq!(end, true);

        assert_eq!(p.state(), State::Connected);

        let mut data = b"\x81\x05hel".to_vec();

        let mut rbuf = io::Cursor::new(&mut data[..]);

        let (opcode, size, end) = p
            .recv_message_content(&mut rbuf, &mut dest)
            .unwrap()
            .unwrap();
        let data = &dest[..size];

        assert_eq!(opcode, OPCODE_TEXT);
        assert_eq!(data, b"hel");
        assert_eq!(end, false);

        assert!(p.recv_message_content(&mut rbuf, &mut dest).is_none());

        let mut data = b"lo".to_vec();

        let mut rbuf = io::Cursor::new(&mut data[..]);

        let (opcode, size, end) = p
            .recv_message_content(&mut rbuf, &mut dest)
            .unwrap()
            .unwrap();
        let data = &dest[..size];

        assert_eq!(opcode, OPCODE_TEXT);
        assert_eq!(data, b"lo");
        assert_eq!(end, true);

        assert_eq!(p.state(), State::Connected);

        let mut data = b"\x01\x03hel\x01\x02lo".to_vec();

        let mut rbuf = io::Cursor::new(&mut data[..]);

        let (opcode, size, end) = p
            .recv_message_content(&mut rbuf, &mut dest)
            .unwrap()
            .unwrap();
        let data = &dest[..size];

        assert_eq!(opcode, OPCODE_TEXT);
        assert_eq!(data, b"hel");
        assert_eq!(end, false);

        let r = p.recv_message_content(&mut rbuf, &mut dest).unwrap();
        assert!(r.is_err());
    }

    #[test]
    fn test_send_recv_compressed() {
        let tmp = Rc::new(TmpBuffer::new(1024));

        let p = Protocol::new(Some((
            PerMessageDeflateConfig::default(),
            RingBuffer::new(1024, &tmp),
        )));

        let mut writer = MyWriter::new();

        p.send_message_start(OPCODE_TEXT, None);

        let (size, done) = p
            .send_message_content(&mut writer, &[b"Hel", b"lo"], true)
            .unwrap();
        assert_eq!(size, 5);
        assert_eq!(done, true);
        assert_eq!(
            writer.data,
            &[0xc1, 0x07, 0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00]
        );

        let mut rbuf = io::Cursor::new(writer.data.as_mut());

        let p = Protocol::new(Some((
            PerMessageDeflateConfig::default(),
            RingBuffer::new(1024, &tmp),
        )));

        let mut dest = [0; 1024];

        let (opcode, size, end) = p
            .recv_message_content(&mut rbuf, &mut dest)
            .unwrap()
            .unwrap();
        let data = &dest[..size];

        assert_eq!(opcode, OPCODE_TEXT);
        assert_eq!(data, b"Hello");
        assert_eq!(end, true);
    }

    #[test]
    fn test_send_recv_compressed_fragmented() {
        let tmp = Rc::new(TmpBuffer::new(1024));

        let p = Protocol::new(Some((
            PerMessageDeflateConfig::default(),
            RingBuffer::new(1024, &tmp),
        )));

        let mut writer = MyWriter::new();

        p.send_message_start(OPCODE_TEXT, None);

        let (size, done) = p
            .send_message_content(&mut writer, &[b"hello"], false)
            .unwrap();
        assert_eq!(size, 5);
        assert_eq!(done, false);

        // flush the encoded data
        {
            let state = &mut *p.deflate_state.as_ref().unwrap().borrow_mut();

            let (_, output_end) = state
                .enc
                .encode_to_ringbuffer(&[], true, &mut state.enc_buf)
                .unwrap();
            assert_eq!(output_end, true);

            state.enc_buf.write(&DEFLATE_SUFFIX).unwrap();
        }

        // send flushed data as first frame
        let (size, done) = p.send_message_content(&mut writer, &[], false).unwrap();
        assert_eq!(size, 0);
        assert_eq!(done, false);
        assert_eq!(writer.data.is_empty(), false);

        // send second frame
        let (size, done) = p
            .send_message_content(&mut writer, &[b" world"], true)
            .unwrap();
        assert_eq!(size, 6);
        assert_eq!(done, true);

        let mut rbuf = io::Cursor::new(writer.data.as_mut());

        let p = Protocol::new(Some((
            PerMessageDeflateConfig::default(),
            RingBuffer::new(1024, &tmp),
        )));

        let mut result: Vec<u8> = Vec::new();

        loop {
            let mut dest = [0; 1024];

            let (opcode, size, end) = p
                .recv_message_content(&mut rbuf, &mut dest)
                .unwrap()
                .unwrap();
            assert_eq!(opcode, OPCODE_TEXT);
            result.extend(&dest[..size]);
            if end {
                break;
            }
        }

        assert_eq!(result, b"hello world");
    }

    struct LimitedDeflateDecoder {
        dec: DeflateDecoder,
        limit: usize,
    }

    impl LimitedDeflateDecoder {
        fn new(limit: usize) -> Self {
            Self {
                dec: DeflateDecoder::new(),
                limit,
            }
        }

        fn increase_limit(&mut self, amt: usize) {
            self.limit += amt
        }
    }

    impl Decoder for LimitedDeflateDecoder {
        fn decode(
            &mut self,
            src: &[u8],
            end: bool,
            dest: &mut [u8],
        ) -> Result<(usize, usize, bool), io::Error> {
            let limit = cmp::min(self.limit, src.len());
            let end = end && limit >= src.len();

            let (read, written, output_end) = self.dec.decode(&src[..limit], end, dest)?;

            self.limit -= read;

            Ok((read, written, output_end))
        }
    }

    #[test]
    fn test_unmask_and_decode() {
        // "Hello" compressed and masked with [0x01, 0x02, 0x03, 0x04]
        let mut msg = [
            0xf2 ^ 0x01,
            0x48 ^ 0x02,
            0xcd ^ 0x03,
            0xc9 ^ 0x04,
            0xc9 ^ 0x01,
            0x07 ^ 0x02,
            0x00 ^ 0x03,
        ];

        let mask = [0x01, 0x02, 0x03, 0x04];
        let mut rbuf = io::Cursor::new(&mut msg[..]);
        let mut dec = LimitedDeflateDecoder::new(5);
        let mut dest = [0; 1024];

        let mut written = 0;

        let (read, w, output_end) =
            unmask_and_decode(&mut rbuf, 1024, true, Some(mask), 0, &mut dec, &mut dest).unwrap();

        written += w;

        assert_eq!(read, 5);
        assert_eq!(output_end, false);

        dec.increase_limit(1024);

        let (read, w, output_end) = unmask_and_decode(
            &mut rbuf,
            1024,
            true,
            Some(mask),
            read,
            &mut dec,
            &mut dest[written..],
        )
        .unwrap();

        written += w;

        assert_eq!(read, 2);
        assert_eq!(output_end, true);
        assert_eq!(&dest[..written], b"Hello");
    }

    #[test]
    fn bench_send_message() {
        let t = BenchSendMessage::new(false);
        t.run(&mut t.init());
    }

    #[test]
    fn bench_recv_message() {
        let t = BenchRecvMessage::new(false);
        t.run(&mut t.init());
    }
}
