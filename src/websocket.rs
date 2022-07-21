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

use crate::buffer::{write_vectored_offset, RefRead, VECTORED_MAX};
use std::cell::{Cell, RefCell};
use std::cmp;
use std::io;
use std::io::Write;

pub const WS_GUID: &str = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// 1 byte flags + 9 bytes payload size + 4 bytes mask
pub const HEADER_SIZE_MAX: usize = 14;

const PSIZE_3BYTE: usize = 126;
const PSIZE_9BYTE: usize = 65536;

pub const OPCODE_CONTINUATION: u8 = 0;
pub const OPCODE_TEXT: u8 = 1;
pub const OPCODE_BINARY: u8 = 2;
pub const OPCODE_CLOSE: u8 = 8;
pub const OPCODE_PING: u8 = 9;
pub const OPCODE_PONG: u8 = 10;

pub const CONTROL_FRAME_PAYLOAD_MAX: usize = 125;

#[derive(Clone, Copy)]
pub struct FrameInfo {
    pub fin: bool,
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
        opcode: buf[0] & 0x0f,
        mask,
        payload_offset: hsize,
        payload_size: psize,
    })
}

// return payload offset
pub fn write_header(
    fin: bool,
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

#[cfg(test)]
pub struct Frame<'a> {
    pub opcode: u8,
    pub data: &'a [u8],
    pub fin: bool,
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
}

struct ReceivingMessage {
    opcode: u8,
    frame_payload_read: usize,
}

struct Sending {
    frame: Option<SendingFrame>,
    message: Option<SendingMessage>,
}

struct Receiving {
    frame: Option<FrameInfo>,
    message: Option<ReceivingMessage>,
}

pub struct Protocol {
    state: Cell<State>,
    sending: RefCell<Sending>,
    receiving: RefCell<Receiving>,
}

impl<'buf> Protocol {
    pub fn new() -> Self {
        Self {
            state: Cell::new(State::Connected),
            sending: RefCell::new(Sending {
                frame: None,
                message: None,
            }),
            receiving: RefCell::new(Receiving {
                frame: None,
                message: None,
            }),
        }
    }

    pub fn state(&self) -> State {
        self.state.get()
    }

    pub fn send_frame(
        &self,
        writer: &mut dyn Write,
        opcode: u8,
        src: &[&[u8]],
        fin: bool,
        mask: Option<[u8; 4]>,
    ) -> Result<usize, Error> {
        assert!(self.state.get() == State::Connected || self.state.get() == State::PeerClosed);

        let sending = &mut *self.sending.borrow_mut();

        let mut src_len = 0;
        for buf in src.iter() {
            src_len += buf.len();
        }

        if sending.frame.is_none() {
            let mut h = [0; HEADER_SIZE_MAX];

            let size = write_header(fin, opcode, src_len, mask, &mut h[..])?;

            sending.frame = Some(SendingFrame {
                header: h,
                header_len: size,
                sent: 0,
            });
        }

        let sending_frame = sending.frame.as_mut().unwrap();

        let header = &sending_frame.header[..sending_frame.header_len];

        let total = header.len() + src_len;

        let mut out_arr = [&b""[..]; VECTORED_MAX];
        let mut out_arr_len = 0;

        out_arr[0] = header;
        out_arr_len += 1;

        for buf in src.iter() {
            out_arr[out_arr_len] = buf;
            out_arr_len += 1;
        }

        let size = write_vectored_offset(writer, &out_arr[..out_arr_len], sending_frame.sent)?;

        sending_frame.sent += size;

        if sending_frame.sent < total {
            return Ok(0);
        }

        sending.frame = None;

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
    pub fn recv_frame(
        &mut self,
        rbuf: &'buf mut dyn RefRead,
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
        self.sending.borrow().message.is_some()
    }

    pub fn send_message_start(&self, opcode: u8, mask: Option<[u8; 4]>) {
        assert!(self.state.get() == State::Connected || self.state.get() == State::PeerClosed);

        let sending = &mut *self.sending.borrow_mut();

        assert_eq!(sending.message.is_some(), false);

        sending.message = Some(SendingMessage {
            opcode,
            mask,
            frame_sent: false,
        });
    }

    pub fn send_message_content(
        &self,
        writer: &mut dyn Write,
        src: &[&[u8]],
        end: bool,
    ) -> Result<(usize, bool), Error> {
        assert!(self.state.get() == State::Connected || self.state.get() == State::PeerClosed);

        let sending = self.sending.borrow();

        let msg = sending.message.as_ref().unwrap();

        let mut src_len = 0;
        for buf in src.iter() {
            src_len += buf.len();
        }

        // control frames (ping, pong, close) must have a small payload length
        //   and must not be fragmented
        if msg.opcode & 0x08 != 0 && (src_len > CONTROL_FRAME_PAYLOAD_MAX || !end) {
            return Err(Error::InvalidControlFrame);
        }

        let opcode = if msg.frame_sent {
            OPCODE_CONTINUATION
        } else {
            msg.opcode
        };

        let fin = if let Some(f) = &sending.frame {
            f.header[0] & 0x80 != 0
        } else {
            end
        };

        let mask = msg.mask;

        drop(sending);

        let size = self.send_frame(writer, opcode, src, fin, mask)?;

        let mut sending = self.sending.borrow_mut();

        if sending.frame.is_none() && fin {
            sending.message = None;
        } else {
            let msg = sending.message.as_mut().unwrap();
            msg.frame_sent = true;
        }

        let done = sending.message.is_none();

        Ok((size, done))
    }

    pub fn recv_message_content(
        &self,
        rbuf: &mut dyn RefRead,
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

                msg.frame_payload_read = 0;
            } else {
                if fi.opcode == OPCODE_CONTINUATION {
                    return Some(Err(Error::UnexpectedOpcode));
                }

                if fi.opcode & 0x08 != 0 && (fi.payload_size > CONTROL_FRAME_PAYLOAD_MAX || !fi.fin)
                {
                    return Some(Err(Error::InvalidControlFrame));
                }

                receiving.message = Some(ReceivingMessage {
                    opcode: fi.opcode,
                    frame_payload_read: 0,
                });
            }
        }

        let fi = receiving.frame.as_ref().unwrap();
        let msg = receiving.message.as_mut().unwrap();

        let buf = rbuf.get_ref();

        // control frames must be available in their entirety
        if fi.opcode & 0x08 != 0 && buf.len() < fi.payload_size {
            return None;
        }

        let left = fi.payload_size - msg.frame_payload_read;

        if left > 0 && buf.len() == 0 {
            return None;
        }

        let size = cmp::min(cmp::min(left, buf.len()), dest.len());

        let dest = &mut dest[..size];

        dest.copy_from_slice(&buf[..size]);

        rbuf.consume(size);

        if let Some(mask) = fi.mask {
            apply_mask(dest, mask, msg.frame_payload_read);
        }

        let opcode = msg.opcode;
        let fin = fi.fin;

        msg.frame_payload_read += size;

        if msg.frame_payload_read >= fi.payload_size {
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

        Some(Ok((opcode, size, receiving.message.is_none())))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
    fn test_send_frame() {
        let p = Protocol::new();

        assert_eq!(p.state(), State::Connected);

        let mut writer = MyWriter::new();

        let size = p
            .send_frame(&mut writer, OPCODE_TEXT, &[b"hello"], true, None)
            .unwrap();

        assert_eq!(size, 5);
        assert_eq!(writer.data, b"\x81\x05hello");
        assert_eq!(p.state(), State::Connected);
    }

    #[test]
    fn test_send_message() {
        let p = Protocol::new();

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

        let mut p = Protocol::new();

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

        let p = Protocol::new();

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
}
