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

use crate::buffer::{write_vectored_offset, LimitBufs, VECTORED_MAX};
use std::cmp;
use std::convert::TryFrom;
use std::io;
use std::io::{Read, Write};
use std::str;

pub const HEADERS_MAX: usize = 32;

const CHUNK_SIZE_MAX: usize = 0xffff;
const CHUNK_HEADER_SIZE_MAX: usize = 6; // ffff\r\n
const CHUNK_FOOTER: &[u8] = b"\r\n";

fn parse_as_int(src: &[u8]) -> Result<usize, io::Error> {
    let int_str = str::from_utf8(src);
    let int_str = match int_str {
        Ok(int_str) => int_str,
        Err(_) => {
            return Err(io::Error::from(io::ErrorKind::InvalidData));
        }
    };

    let x = int_str.parse();
    let x = match x {
        Ok(x) => x,
        Err(_) => {
            return Err(io::Error::from(io::ErrorKind::InvalidData));
        }
    };

    Ok(x)
}

fn header_contains_param(value: &[u8], param: &[u8], ignore_case: bool) -> bool {
    let param_str = str::from_utf8(param);
    let param_str = match param_str {
        Ok(param_str) => param_str,
        Err(_) => {
            return false;
        }
    };

    for part in value.split(|b| *b == b',') {
        let part_str = str::from_utf8(part);
        let part_str = match part_str {
            Ok(part_str) => part_str,
            Err(_) => {
                continue;
            }
        };

        let part_str = part_str.trim();

        if ignore_case {
            if part_str.eq_ignore_ascii_case(param_str) {
                return true;
            }
        } else {
            if part_str == param_str {
                return true;
            }
        }
    }

    false
}

#[derive(Debug, PartialEq, Clone, Copy)]
struct Chunk {
    header: [u8; CHUNK_HEADER_SIZE_MAX],
    header_len: usize,
    size: usize,
    sent: usize,
}

// writes src to dest as chunks. current chunk state is passed in
fn write_chunk(
    content: &[&[u8]],
    footer: &[u8],
    dest: &mut dyn Write,
    chunk: &mut Option<Chunk>,
    max_size: usize,
) -> Result<usize, io::Error> {
    assert!(max_size <= CHUNK_SIZE_MAX);

    let mut content_len = 0;
    for buf in content.iter() {
        content_len += buf.len();
    }

    if chunk.is_none() {
        let size = cmp::min(content_len, max_size);

        let mut h = [0; CHUNK_HEADER_SIZE_MAX];

        let h_len = {
            let mut c = io::Cursor::new(&mut h[..]);
            write!(&mut c, "{:x}\r\n", size).unwrap();

            c.position() as usize
        };

        *chunk = Some(Chunk {
            header: h,
            header_len: h_len,
            size,
            sent: 0,
        });
    }

    let chunkv = chunk.as_mut().unwrap();

    let cheader = &chunkv.header[..chunkv.header_len];
    let data_size = chunkv.size;

    let total = cheader.len() + data_size + footer.len();

    let mut content_arr = [&b""[..]; VECTORED_MAX - 2];
    for (i, buf) in content.iter().enumerate() {
        content_arr[i] = buf;
    }

    let trim_content = (&mut content_arr[..content.len()]).limit(data_size);

    let mut out_arr = [&b""[..]; VECTORED_MAX];
    let mut out_arr_len = 0;

    out_arr[0] = cheader;
    out_arr_len += 1;

    for buf in trim_content.iter() {
        out_arr[out_arr_len] = buf;
        out_arr_len += 1;
    }

    out_arr[out_arr_len] = footer;
    out_arr_len += 1;

    let size = write_vectored_offset(dest, &out_arr[..out_arr_len], chunkv.sent)?;

    chunkv.sent += size;

    if chunkv.sent < total {
        return Ok(0);
    }

    *chunk = None;

    Ok(data_size)
}

#[cfg(test)]
pub fn write_headers(writer: &mut dyn io::Write, headers: &[Header]) -> Result<(), io::Error> {
    for h in headers.iter() {
        write!(writer, "{}: ", h.name)?;
        writer.write(h.value)?;
        writer.write(b"\r\n")?;
    }

    writer.write(b"\r\n")?;

    Ok(())
}

#[derive(Clone, Copy)]
pub struct Header<'a> {
    pub name: &'a str,
    pub value: &'a [u8],
}

pub const EMPTY_HEADER: Header<'static> = Header {
    name: "",
    value: b"",
};

#[derive(Debug, PartialEq, Clone, Copy)]
pub enum BodySize {
    NoBody,
    Known(usize),
    Unknown,
}

#[derive(Debug, PartialEq)]
pub struct Request<'buf, 'headers> {
    pub method: &'buf str,
    pub uri: &'buf str,
    pub headers: &'headers [httparse::Header<'buf>],
    pub body_size: BodySize,
    pub expect_100: bool,
}

#[derive(Debug, PartialEq, Clone, Copy)]
pub enum ServerState {
    // call: recv_request
    // next: ReceivingRequest, ReceivingBody, AwaitingResponse
    ReceivingRequest,

    // call: recv_body
    // next: ReceivingBody, AwaitingResponse
    ReceivingBody,

    // call: send_response
    // next: SendingBody
    AwaitingResponse,

    // call: send_body
    // next: SendingBody, Finished
    SendingBody,

    // request/response has completed
    Finished,
}

#[derive(Debug)]
pub enum ServerError {
    ParseError(httparse::Error),
    InvalidContentLength,
    UnsupportedTransferEncoding,
    Io(io::Error),
    InvalidChunkSize,
    ChunkTooLarge,
    InvalidChunkSuffix,
}

impl From<io::Error> for ServerError {
    fn from(e: io::Error) -> Self {
        Self::Io(e)
    }
}

pub struct ServerProtocol {
    state: ServerState,
    ver_min: u8,
    body_size: BodySize,
    chunk_left: Option<usize>,
    chunk_size: usize,
    persistent: bool,
    chunked: bool,
    sending_chunk: Option<Chunk>,
}

impl<'buf, 'headers> ServerProtocol {
    pub fn new() -> Self {
        Self {
            state: ServerState::ReceivingRequest,
            ver_min: 0,
            body_size: BodySize::NoBody,
            chunk_left: None,
            chunk_size: 0,
            persistent: false,
            chunked: false,
            sending_chunk: None,
        }
    }

    pub fn state(&self) -> ServerState {
        self.state
    }

    pub fn is_persistent(&self) -> bool {
        self.persistent
    }

    pub fn recv_request(
        &mut self,
        rbuf: &mut io::Cursor<&'buf [u8]>,
        headers: &'headers mut [httparse::Header<'buf>],
    ) -> Option<Result<Request<'buf, 'headers>, ServerError>> {
        assert_eq!(self.state, ServerState::ReceivingRequest);

        let mut req = httparse::Request::new(headers);

        let buf = &rbuf.get_ref()[(rbuf.position() as usize)..];

        let size = match req.parse(buf) {
            Ok(httparse::Status::Complete(size)) => size,
            Ok(httparse::Status::Partial) => return None,
            Err(e) => return Some(Err(ServerError::ParseError(e))),
        };

        let version = req.version.unwrap();

        let mut content_len = None;
        let mut chunked = false;
        let mut keep_alive = false;
        let mut close = false;
        let mut expect_100 = false;

        for i in 0..req.headers.len() {
            let h = req.headers[i];

            if h.name.eq_ignore_ascii_case("Content-Length") {
                let len = parse_as_int(h.value);
                let len = match len {
                    Ok(len) => len,
                    Err(_) => {
                        return Some(Err(ServerError::InvalidContentLength));
                    }
                };

                content_len = Some(len);
            } else if h.name.eq_ignore_ascii_case("Transfer-Encoding") {
                if h.value == b"chunked" {
                    chunked = true;
                } else {
                    // unknown transfer encoding
                    return Some(Err(ServerError::UnsupportedTransferEncoding));
                }
            } else if h.name.eq_ignore_ascii_case("Connection") {
                if !keep_alive && header_contains_param(h.value, b"keep-alive", true) {
                    keep_alive = true;
                }

                if !close && header_contains_param(h.value, b"close", false) {
                    close = true;
                }
            } else if h.name.eq_ignore_ascii_case("Expect") {
                if header_contains_param(h.value, b"100-continue", false) && version >= 1 {
                    expect_100 = true;
                }
            }
        }

        self.ver_min = version;

        if chunked {
            self.body_size = BodySize::Unknown;
        } else if let Some(len) = content_len {
            self.body_size = BodySize::Known(len);
            self.chunk_left = Some(len);
        } else {
            self.body_size = BodySize::NoBody;
        }

        if version >= 1 {
            self.persistent = !close;
        } else {
            self.persistent = keep_alive && !close;
        }

        rbuf.set_position(rbuf.position() + (size as u64));

        self.state = match self.body_size {
            BodySize::Unknown | BodySize::Known(_) => ServerState::ReceivingBody,
            BodySize::NoBody => ServerState::AwaitingResponse,
        };

        Some(Ok(Request {
            method: req.method.unwrap(),
            uri: req.path.unwrap(),
            headers: req.headers,
            body_size: self.body_size,
            expect_100: expect_100 && self.body_size != BodySize::NoBody,
        }))
    }

    pub fn recv_body(
        &mut self,
        rbuf: &mut io::Cursor<&'buf [u8]>,
        dest: &mut [u8],
        headers: &'headers mut [httparse::Header<'buf>],
    ) -> Result<(usize, Option<&'headers [httparse::Header<'buf>]>), ServerError> {
        assert_eq!(self.state, ServerState::ReceivingBody);

        match self.body_size {
            BodySize::Known(_) => {
                let mut chunk_left = self.chunk_left.unwrap();
                let read_size = cmp::min(chunk_left, dest.len());

                // rbuf holds body as-is
                let size = rbuf.read(&mut dest[..read_size])?;

                chunk_left -= size;

                if chunk_left == 0 {
                    self.chunk_left = None;
                    self.state = ServerState::AwaitingResponse;
                } else {
                    self.chunk_left = Some(chunk_left);
                }

                Ok((size, None))
            }
            BodySize::Unknown => {
                if self.chunk_left.is_none() {
                    let buf = &rbuf.get_ref()[(rbuf.position() as usize)..];

                    match httparse::parse_chunk_size(buf) {
                        Ok(httparse::Status::Complete((pos, size))) => {
                            let size = match u32::try_from(size) {
                                Ok(size) => size,
                                Err(_) => return Err(ServerError::ChunkTooLarge),
                            };

                            let size = size as usize;

                            rbuf.set_position(rbuf.position() + (pos as u64));

                            self.chunk_left = Some(size);
                            self.chunk_size = size;
                        }
                        Ok(httparse::Status::Partial) => {
                            return Ok((0, None));
                        }
                        Err(_) => {
                            return Err(ServerError::InvalidChunkSize);
                        }
                    }
                }

                let mut chunk_left = self.chunk_left.unwrap();

                let size;

                if chunk_left > 0 {
                    let read_size = cmp::min(chunk_left, dest.len());

                    size = rbuf.read(&mut dest[..read_size])?;

                    chunk_left -= size;

                    self.chunk_left = Some(chunk_left);
                } else {
                    size = 0;
                }

                let mut trailing_headers = None;

                if chunk_left == 0 {
                    let buf = &rbuf.get_ref()[(rbuf.position() as usize)..];

                    if self.chunk_size == 0 {
                        // trailing headers
                        match httparse::parse_headers(buf, headers) {
                            Ok(httparse::Status::Complete((pos, headers))) => {
                                rbuf.set_position(rbuf.position() + (pos as u64));

                                trailing_headers = Some(headers);
                            }
                            Ok(httparse::Status::Partial) => {
                                return Ok((size, None));
                            }
                            Err(e) => {
                                return Err(ServerError::ParseError(e));
                            }
                        }

                        self.state = ServerState::AwaitingResponse;
                    } else {
                        if buf.len() < 2 {
                            return Ok((size, None));
                        }

                        if &buf[..2] != b"\r\n" {
                            return Err(ServerError::InvalidChunkSuffix);
                        }

                        rbuf.set_position(rbuf.position() + 2);
                    }

                    self.chunk_left = None;
                    self.chunk_size = 0;
                }

                Ok((size, trailing_headers))
            }
            BodySize::NoBody => unreachable!(),
        }
    }

    pub fn send_100_continue(&mut self, writer: &mut dyn Write) -> Result<(), ServerError> {
        writer.write(b"HTTP/1.1 100 Continue\r\n\r\n")?;

        Ok(())
    }

    pub fn send_response(
        &mut self,
        writer: &mut dyn Write,
        code: u32,
        reason: &str,
        headers: &[Header],
        body_size: BodySize,
    ) -> Result<(), ServerError> {
        assert!(
            self.state == ServerState::AwaitingResponse || self.state == ServerState::ReceivingBody
        );

        if self.state == ServerState::ReceivingBody {
            // when responding early, input stream may be broken
            self.persistent = false;
        }

        let mut body_size = body_size;

        // certain responses have no body
        match code {
            100..=199 | 204 | 304 => {
                body_size = BodySize::NoBody;
            }
            _ => {}
        }

        let chunked = if body_size == BodySize::Unknown && self.ver_min >= 1 {
            true
        } else {
            false
        };

        if self.ver_min >= 1 {
            writer.write(b"HTTP/1.1 ")?;
        } else {
            writer.write(b"HTTP/1.0 ")?;
        }

        write!(writer, "{} {}\r\n", code, reason)?;

        for h in headers.iter() {
            // we'll override these headers
            if (h.name.eq_ignore_ascii_case("Connection") && code != 101)
                || h.name.eq_ignore_ascii_case("Content-Length")
                || h.name.eq_ignore_ascii_case("Transfer-Encoding")
            {
                continue;
            }

            write!(writer, "{}: ", h.name)?;
            writer.write(h.value)?;
            writer.write(b"\r\n")?;
        }

        // Connection header

        if self.persistent && self.ver_min == 0 {
            writer.write(b"Connection: keep-alive\r\n")?;
        } else if !self.persistent && self.ver_min >= 1 {
            writer.write(b"Connection: close\r\n")?;
        }

        if chunked {
            writer.write(b"Connection: Transfer-Encoding\r\n")?;
        }

        // Content-Length header

        if let BodySize::Known(x) = body_size {
            write!(writer, "Content-Length: {}\r\n", x)?;
        }

        // Transfer-Encoding header

        if chunked {
            writer.write(b"Transfer-Encoding: chunked\r\n")?;
        }

        writer.write(b"\r\n")?;

        self.state = ServerState::SendingBody;
        self.body_size = body_size;
        self.chunked = chunked;

        Ok(())
    }

    pub fn send_body(
        &mut self,
        writer: &mut dyn Write,
        src: &[&[u8]],
        end: bool,
        headers: Option<&[u8]>,
    ) -> Result<usize, ServerError> {
        assert_eq!(self.state, ServerState::SendingBody);

        let mut src_len = 0;
        for buf in src.iter() {
            src_len += buf.len();
        }

        if let BodySize::NoBody = self.body_size {
            // ignore the data

            if end {
                self.state = ServerState::Finished;
            }

            return Ok(src_len);
        }

        if !self.chunked {
            let size = write_vectored_offset(writer, src, 0)?;

            if end && size >= src_len {
                self.state = ServerState::Finished;
            }

            return Ok(size);
        }

        // chunked

        let mut content_written = 0;

        if src_len > 0 {
            content_written = write_chunk(
                src,
                CHUNK_FOOTER,
                writer,
                &mut self.sending_chunk,
                CHUNK_SIZE_MAX,
            )?;
        }

        // if all content is written then we can send the closing chunk
        if end && content_written >= src_len {
            let footer = if let Some(headers) = headers {
                headers
            } else {
                CHUNK_FOOTER
            };

            write_chunk(
                &[b""],
                footer,
                writer,
                &mut self.sending_chunk,
                CHUNK_SIZE_MAX,
            )?;

            if self.sending_chunk.is_none() {
                self.state = ServerState::Finished;
            }
        }

        Ok(content_written)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem;

    struct MyBuffer {
        data: Vec<u8>,
        max: usize,
        allow_partial: bool,
    }

    impl MyBuffer {
        fn new(cap: usize, allow_partial: bool) -> Self {
            Self {
                data: Vec::new(),
                max: cap,
                allow_partial,
            }
        }
    }

    impl Write for MyBuffer {
        fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
            let size = cmp::min(buf.len(), self.max - self.data.len());

            if (size == 0 && !buf.is_empty()) || (size < buf.len() && !self.allow_partial) {
                return Err(io::Error::from(io::ErrorKind::WriteZero));
            }

            self.data.extend_from_slice(&buf[..size]);

            Ok(size)
        }

        fn write_vectored(&mut self, bufs: &[io::IoSlice]) -> Result<usize, io::Error> {
            let mut total = 0;

            for buf in bufs {
                let size = match self.write(buf.as_ref()) {
                    Ok(size) => size,
                    Err(e) => {
                        if e.kind() == io::ErrorKind::WriteZero && total > 0 {
                            return Ok(total);
                        }
                        return Err(e);
                    }
                };

                total += size;

                if size < buf.len() {
                    break;
                }
            }

            Ok(total)
        }

        fn flush(&mut self) -> Result<(), io::Error> {
            Ok(())
        }
    }

    struct TestRequest {
        pub method: String,
        pub uri: String,
        pub headers: Vec<(String, Vec<u8>)>,
        pub body: Vec<u8>,
        pub trailing_headers: Vec<(String, Vec<u8>)>,
        pub persistent: bool,
    }

    impl TestRequest {
        fn new() -> Self {
            Self {
                method: String::new(),
                uri: String::new(),
                headers: Vec::new(),
                body: Vec::new(),
                trailing_headers: Vec::new(),
                persistent: false,
            }
        }
    }

    struct TestResponse {
        pub code: u32,
        pub reason: String,
        pub headers: Vec<(String, Vec<u8>)>,
        pub body: Vec<u8>,
        pub chunked: bool,
        pub trailing_headers: Vec<(String, Vec<u8>)>,
    }

    impl TestResponse {
        fn new() -> Self {
            Self {
                code: 0,
                reason: String::new(),
                headers: Vec::new(),
                body: Vec::new(),
                chunked: false,
                trailing_headers: Vec::new(),
            }
        }
    }

    fn read_req(p: &mut ServerProtocol, src: &[u8], read_size: usize) -> TestRequest {
        const READ_SIZE_MAX: usize = 1024;
        const LOOPS_MAX: u32 = 20;

        assert!(read_size <= READ_SIZE_MAX);

        assert_eq!(p.state(), ServerState::ReceivingRequest);

        let mut rbuf = io::Cursor::new(src);

        let mut result = TestRequest::new();

        assert_eq!(p.state(), ServerState::ReceivingRequest);

        let mut headers = [httparse::EMPTY_HEADER; HEADERS_MAX];

        let req = p.recv_request(&mut rbuf, &mut headers).unwrap().unwrap();

        result.method = String::from(req.method);
        result.uri = String::from(req.uri);

        for h in req.headers {
            let name = String::from(h.name);
            let value = Vec::from(h.value);
            result.headers.push((name, value));
        }

        for _ in 0..LOOPS_MAX {
            if p.state() != ServerState::ReceivingBody {
                break;
            }

            let mut buf = [0; READ_SIZE_MAX];

            let (size, trailing_headers) = p
                .recv_body(&mut rbuf, &mut buf[..read_size], &mut headers)
                .unwrap();

            result.body.extend_from_slice(&buf[..size]);

            if let Some(trailing_headers) = trailing_headers {
                for h in trailing_headers {
                    let name = String::from(h.name);
                    let value = Vec::from(h.value);
                    result.trailing_headers.push((name, value));
                }
            }
        }

        result.persistent = p.is_persistent();

        assert_eq!(p.state(), ServerState::AwaitingResponse);

        return result;
    }

    fn write_resp(p: &mut ServerProtocol, resp: TestResponse, write_size: usize) -> Vec<u8> {
        const WRITE_SIZE_MAX: usize = 1024;
        const LOOPS_MAX: u32 = 20;

        assert!(write_size <= WRITE_SIZE_MAX);

        assert_eq!(p.state(), ServerState::AwaitingResponse);

        let mut header_out = [0; 1024];

        let mut wbuf = io::Cursor::new(&mut header_out[..]);

        let mut headers = Vec::new();
        for h in resp.headers.iter() {
            headers.push(Header {
                name: &h.0,
                value: &h.1,
            });
        }

        let body_size = if resp.chunked {
            BodySize::Unknown
        } else {
            BodySize::Known(resp.body.len())
        };

        p.send_response(&mut wbuf, resp.code, &resp.reason, &headers, body_size)
            .unwrap();

        let size = wbuf.position() as usize;

        let header_out = &header_out[..size];

        let mut body_out = MyBuffer::new(0, true);

        let mut sent = 0;

        for _ in 0..LOOPS_MAX {
            if p.state() != ServerState::SendingBody {
                break;
            }

            body_out.max += write_size;

            let trailing_headers = if !resp.trailing_headers.is_empty() {
                let mut buf = Vec::new();

                for (name, value) in resp.trailing_headers.iter() {
                    write!(buf, "{}: ", name).unwrap();
                    buf.write(value).unwrap();
                    write!(buf, "\r\n").unwrap();
                }

                write!(buf, "\r\n").unwrap();

                Some(buf)
            } else {
                None
            };

            let trailing_headers: Option<&[u8]> = if let Some(trailing_headers) = &trailing_headers
            {
                Some(trailing_headers)
            } else {
                None
            };

            let size =
                match p.send_body(&mut body_out, &[&resp.body[sent..]], true, trailing_headers) {
                    Ok(size) => size,
                    Err(ServerError::Io(e)) if e.kind() == io::ErrorKind::WriteZero => 0,
                    Err(_) => panic!("send_body failed"),
                };

            sent += size;
        }

        assert_eq!(p.state(), ServerState::Finished);

        let mut out = Vec::new();

        out.extend_from_slice(header_out);
        out.append(&mut body_out.data);

        out
    }

    #[test]
    fn test_parse_as_int() {
        // invalid utf8
        assert!(parse_as_int(b"\xa0\xa1").is_err());

        // not an integer
        assert!(parse_as_int(b"bogus").is_err());

        // not a non-negative integer
        assert!(parse_as_int(b"-123").is_err());

        // success
        assert_eq!(parse_as_int(b"0").unwrap(), 0);
        assert_eq!(parse_as_int(b"123").unwrap(), 123);
    }

    #[test]
    fn test_header_contains_param() {
        // param invalid utf8
        assert_eq!(header_contains_param(b"", b"\xa0\xa1", false), false);

        // skip invalid utf8 part
        assert_eq!(header_contains_param(b"\xa0\xa1,a", b"a", false), true);

        // not found
        assert_eq!(header_contains_param(b"", b"a", false), false);
        assert_eq!(header_contains_param(b"a", b"b", false), false);
        assert_eq!(header_contains_param(b"a,b", b"c", false), false);

        // success
        assert_eq!(header_contains_param(b"a", b"a", false), true);
        assert_eq!(header_contains_param(b"a,b", b"a", false), true);
        assert_eq!(header_contains_param(b"a,b", b"b", false), true);
        assert_eq!(header_contains_param(b" a ,b", b"a", false), true);
        assert_eq!(header_contains_param(b"a, b  ", b"b", false), true);
        assert_eq!(header_contains_param(b"A", b"a", true), true);
    }

    #[test]
    fn test_write_chunk() {
        struct Test {
            name: &'static str,
            write_space: usize,
            data: &'static [&'static [u8]],
            footer: &'static str,
            chunk: Option<Chunk>,
            max_size: usize,
            result: Result<usize, io::Error>,
            chunk_after: Option<Chunk>,
            written: &'static str,
        }

        let tests = [
            Test {
                name: "new-partial",
                write_space: 2,
                data: &[b"hello"],
                footer: "\r\n",
                chunk: None,
                max_size: CHUNK_SIZE_MAX,
                result: Ok(0),
                chunk_after: Some(Chunk {
                    header: [b'5', b'\r', b'\n', 0, 0, 0],
                    header_len: 3,
                    size: 5,
                    sent: 2,
                }),
                written: "5\r",
            },
            Test {
                name: "resume-partial",
                write_space: 2,
                data: &[b"hello"],
                footer: "\r\n",
                chunk: Some(Chunk {
                    header: [b'5', b'\r', b'\n', 0, 0, 0],
                    header_len: 3,
                    size: 5,
                    sent: 2,
                }),
                max_size: CHUNK_SIZE_MAX,
                result: Ok(0),
                chunk_after: Some(Chunk {
                    header: [b'5', b'\r', b'\n', 0, 0, 0],
                    header_len: 3,
                    size: 5,
                    sent: 4,
                }),
                written: "\nh",
            },
            Test {
                name: "error",
                write_space: 0,
                data: &[b"hello"],
                footer: "\r\n",
                chunk: None,
                max_size: CHUNK_SIZE_MAX,
                result: Err(io::Error::from(io::ErrorKind::WriteZero)),
                chunk_after: Some(Chunk {
                    header: [b'5', b'\r', b'\n', 0, 0, 0],
                    header_len: 3,
                    size: 5,
                    sent: 0,
                }),
                written: "",
            },
            Test {
                name: "complete",
                write_space: 1024,
                data: &[b"hello"],
                footer: "\r\n",
                chunk: Some(Chunk {
                    header: [b'5', b'\r', b'\n', 0, 0, 0],
                    header_len: 3,
                    size: 5,
                    sent: 4,
                }),
                max_size: CHUNK_SIZE_MAX,
                result: Ok(5),
                chunk_after: None,
                written: "ello\r\n",
            },
            Test {
                name: "partial-content",
                write_space: 1024,
                data: &[b"hel", b"lo world"],
                footer: "\r\n",
                chunk: Some(Chunk {
                    header: [b'7', b'\r', b'\n', 0, 0, 0],
                    header_len: 3,
                    size: 7,
                    sent: 0,
                }),
                max_size: 7,
                result: Ok(7),
                chunk_after: None,
                written: "7\r\nhello w\r\n",
            },
        ];

        for test in tests.iter() {
            let mut w = MyBuffer::new(test.write_space, true);
            let mut chunk = test.chunk.clone();

            let r = write_chunk(
                test.data,
                test.footer.as_bytes(),
                &mut w,
                &mut chunk,
                test.max_size,
            );

            match r {
                Ok(size) => {
                    let expected = match &test.result {
                        Ok(size) => size,
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(size, *expected, "test={}", test.name);
                }
                Err(_) => {
                    assert!(test.result.is_err(), "test={}", test.name);
                }
            }

            assert_eq!(chunk, test.chunk_after, "test={}", test.name);

            assert_eq!(
                str::from_utf8(&w.data).unwrap(),
                test.written,
                "test={}",
                test.name
            );
        }
    }

    #[test]
    fn test_write_headers() {
        struct Test<'buf, 'headers> {
            name: &'static str,
            write_space: usize,
            headers: &'headers [Header<'buf>],
            err: bool,
            written: &'static str,
        }

        let tests = [
            Test {
                name: "cant-write-header-name",
                write_space: 2,
                headers: &[
                    Header {
                        name: "A",
                        value: b"a",
                    },
                    Header {
                        name: "B",
                        value: b"b",
                    },
                ],
                err: true,
                written: "A",
            },
            Test {
                name: "cant-write-header-value",
                write_space: 3,
                headers: &[
                    Header {
                        name: "A",
                        value: b"a",
                    },
                    Header {
                        name: "B",
                        value: b"b",
                    },
                ],
                err: true,
                written: "A: ",
            },
            Test {
                name: "cant-write-header-eol",
                write_space: 4,
                headers: &[
                    Header {
                        name: "A",
                        value: b"a",
                    },
                    Header {
                        name: "B",
                        value: b"b",
                    },
                ],
                err: true,
                written: "A: a",
            },
            Test {
                name: "cant-write-eol",
                write_space: 13,
                headers: &[
                    Header {
                        name: "A",
                        value: b"a",
                    },
                    Header {
                        name: "B",
                        value: b"b",
                    },
                ],
                err: true,
                written: "A: a\r\nB: b\r\n",
            },
            Test {
                name: "success",
                write_space: 1024,
                headers: &[
                    Header {
                        name: "A",
                        value: b"a",
                    },
                    Header {
                        name: "B",
                        value: b"b",
                    },
                ],
                err: false,
                written: "A: a\r\nB: b\r\n\r\n",
            },
        ];

        for test in tests.iter() {
            let mut w = MyBuffer::new(test.write_space, false);

            let r = write_headers(&mut w, test.headers);

            assert_eq!(r.is_err(), test.err, "test={}", test.name);
            assert_eq!(
                str::from_utf8(&w.data).unwrap(),
                test.written,
                "test={}",
                test.name
            );
        }
    }

    #[test]
    fn test_recv_request() {
        struct Test<'buf, 'headers> {
            name: &'static str,
            data: &'buf str,
            result: Option<Result<Request<'buf, 'headers>, ServerError>>,
            state: ServerState,
            ver_min: u8,
            chunk_left: Option<usize>,
            persistent: bool,
            rbuf_position: u64,
        }

        let tests = [
            Test {
                name: "partial",
                data: "G",
                result: None,
                state: ServerState::ReceivingRequest,
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                rbuf_position: 0,
            },
            Test {
                name: "parse-error",
                data: "G\n",
                result: Some(Err(ServerError::ParseError(httparse::Error::Token))),
                state: ServerState::ReceivingRequest,
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                rbuf_position: 0,
            },
            Test {
                name: "invalid-content-length",
                data: "GET / HTTP/1.0\r\nContent-Length: a\r\n\r\n",
                result: Some(Err(ServerError::InvalidContentLength)),
                state: ServerState::ReceivingRequest,
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                rbuf_position: 0,
            },
            Test {
                name: "unsupported-transfer-encoding",
                data: "GET / HTTP/1.0\r\nTransfer-Encoding: bogus\r\n\r\n",
                result: Some(Err(ServerError::UnsupportedTransferEncoding)),
                state: ServerState::ReceivingRequest,
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                rbuf_position: 0,
            },
            Test {
                name: "no-body",
                data: "GET / HTTP/1.0\r\nFoo: Bar\r\n\r\n",
                result: Some(Ok(Request {
                    method: "GET",
                    uri: "/",
                    headers: &[httparse::Header {
                        name: "Foo",
                        value: b"Bar",
                    }],
                    body_size: BodySize::NoBody,
                    expect_100: false,
                })),
                state: ServerState::AwaitingResponse,
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                rbuf_position: 28,
            },
            Test {
                name: "body-size-known",
                data: "GET / HTTP/1.0\r\nContent-Length: 42\r\n\r\n",
                result: Some(Ok(Request {
                    method: "GET",
                    uri: "/",
                    headers: &[httparse::Header {
                        name: "Content-Length",
                        value: b"42",
                    }],
                    body_size: BodySize::Known(42),
                    expect_100: false,
                })),
                state: ServerState::ReceivingBody,
                ver_min: 0,
                chunk_left: Some(42),
                persistent: false,
                rbuf_position: 38,
            },
            Test {
                name: "body-size-unknown",
                data: "GET / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n",
                result: Some(Ok(Request {
                    method: "GET",
                    uri: "/",
                    headers: &[httparse::Header {
                        name: "Transfer-Encoding",
                        value: b"chunked",
                    }],
                    body_size: BodySize::Unknown,
                    expect_100: false,
                })),
                state: ServerState::ReceivingBody,
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                rbuf_position: 46,
            },
            Test {
                name: "1.0-persistent",
                data: "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
                result: Some(Ok(Request {
                    method: "GET",
                    uri: "/",
                    headers: &[httparse::Header {
                        name: "Connection",
                        value: b"keep-alive",
                    }],
                    body_size: BodySize::NoBody,
                    expect_100: false,
                })),
                state: ServerState::AwaitingResponse,
                ver_min: 0,
                chunk_left: None,
                persistent: true,
                rbuf_position: 42,
            },
            Test {
                name: "1.1-persistent",
                data: "GET / HTTP/1.1\r\n\r\n",
                result: Some(Ok(Request {
                    method: "GET",
                    uri: "/",
                    headers: &[],
                    body_size: BodySize::NoBody,
                    expect_100: false,
                })),
                state: ServerState::AwaitingResponse,
                ver_min: 1,
                chunk_left: None,
                persistent: true,
                rbuf_position: 18,
            },
            Test {
                name: "1.1-non-persistent",
                data: "GET / HTTP/1.1\r\nConnection: close\r\n\r\n",
                result: Some(Ok(Request {
                    method: "GET",
                    uri: "/",
                    headers: &[httparse::Header {
                        name: "Connection",
                        value: b"close",
                    }],
                    body_size: BodySize::NoBody,
                    expect_100: false,
                })),
                state: ServerState::AwaitingResponse,
                ver_min: 1,
                chunk_left: None,
                persistent: false,
                rbuf_position: 37,
            },
            Test {
                name: "expect-100",
                data: "POST / HTTP/1.1\r\nContent-Length: 10\r\nExpect: 100-continue\r\n\r\n",
                result: Some(Ok(Request {
                    method: "POST",
                    uri: "/",
                    headers: &[
                        httparse::Header {
                            name: "Content-Length",
                            value: b"10",
                        },
                        httparse::Header {
                            name: "Expect",
                            value: b"100-continue",
                        },
                    ],
                    body_size: BodySize::Known(10),
                    expect_100: true,
                })),
                state: ServerState::ReceivingBody,
                ver_min: 1,
                chunk_left: Some(10),
                persistent: true,
                rbuf_position: 61,
            },
        ];

        for test in tests.iter() {
            let mut p = ServerProtocol::new();
            let mut c = io::Cursor::new(test.data.as_bytes());
            let mut headers = [httparse::EMPTY_HEADER; HEADERS_MAX];

            let r = p.recv_request(&mut c, &mut headers);

            match r {
                None => {
                    assert!(test.result.is_none(), "test={}", test.name);
                }
                Some(Ok(req)) => {
                    let expected = match &test.result {
                        Some(Ok(req)) => req,
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(req, *expected, "test={}", test.name);
                }
                Some(Err(e)) => {
                    let expected = match &test.result {
                        Some(Err(e)) => e,
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(
                        mem::discriminant(&e),
                        mem::discriminant(expected),
                        "test={}",
                        test.name
                    );
                }
            }

            assert_eq!(p.state(), test.state, "test={}", test.name);
            assert_eq!(p.ver_min, test.ver_min, "test={}", test.name);
            assert_eq!(p.chunk_left, test.chunk_left, "test={}", test.name);
            assert_eq!(p.is_persistent(), test.persistent, "test={}", test.name);
            assert_eq!(c.position(), test.rbuf_position, "test={}", test.name);
        }
    }

    #[test]
    fn test_recv_body() {
        struct Test<'buf, 'headers> {
            name: &'static str,
            data: &'buf str,
            body_size: BodySize,
            chunk_left: Option<usize>,
            chunk_size: usize,
            result: Result<(usize, Option<&'headers [httparse::Header<'buf>]>), ServerError>,
            state: ServerState,
            chunk_left_after: Option<usize>,
            chunk_size_after: usize,
            rbuf_position: u64,
            dest_data: &'static str,
        }

        let tests = [
            Test {
                name: "partial",
                data: "hel",
                body_size: BodySize::Known(5),
                chunk_left: Some(5),
                chunk_size: 0,
                result: Ok((3, None)),
                state: ServerState::ReceivingBody,
                chunk_left_after: Some(2),
                chunk_size_after: 0,
                rbuf_position: 3,
                dest_data: "hel",
            },
            Test {
                name: "complete",
                data: "hello",
                body_size: BodySize::Known(5),
                chunk_left: Some(5),
                chunk_size: 0,
                result: Ok((5, None)),
                state: ServerState::AwaitingResponse,
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 5,
                dest_data: "hello",
            },
            Test {
                name: "chunked-header-partial",
                data: "5",
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                result: Ok((0, None)),
                state: ServerState::ReceivingBody,
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "chunked-header-parse-error",
                data: "z",
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                result: Err(ServerError::InvalidChunkSize),
                state: ServerState::ReceivingBody,
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "chunked-too-large",
                data: "ffffffffff\r\n",
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                result: Err(ServerError::ChunkTooLarge),
                state: ServerState::ReceivingBody,
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "chunked-header-ok",
                data: "5\r\n",
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                result: Ok((0, None)),
                state: ServerState::ReceivingBody,
                chunk_left_after: Some(5),
                chunk_size_after: 5,
                rbuf_position: 3,
                dest_data: "",
            },
            Test {
                name: "chunked-content-partial",
                data: "5\r\nhel",
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                result: Ok((3, None)),
                state: ServerState::ReceivingBody,
                chunk_left_after: Some(2),
                chunk_size_after: 5,
                rbuf_position: 6,
                dest_data: "hel",
            },
            Test {
                name: "chunked-footer-partial-full-none",
                data: "5\r\nhello",
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                result: Ok((5, None)),
                state: ServerState::ReceivingBody,
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 8,
                dest_data: "hello",
            },
            Test {
                name: "chunked-footer-partial-full-r",
                data: "5\r\nhello\r",
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                result: Ok((5, None)),
                state: ServerState::ReceivingBody,
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 8,
                dest_data: "hello",
            },
            Test {
                name: "chunked-footer-partial-mid-r",
                data: "\r",
                body_size: BodySize::Unknown,
                chunk_left: Some(0),
                chunk_size: 5,
                result: Ok((0, None)),
                state: ServerState::ReceivingBody,
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "chunked-footer-parse-error",
                data: "5\r\nhelloXX",
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                result: Err(ServerError::InvalidChunkSuffix),
                state: ServerState::ReceivingBody,
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 8,
                dest_data: "",
            },
            Test {
                name: "chunked-complete-full",
                data: "5\r\nhello\r\n",
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                result: Ok((5, None)),
                state: ServerState::ReceivingBody,
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 10,
                dest_data: "hello",
            },
            Test {
                name: "chunked-complete-mid",
                data: "lo\r\n",
                body_size: BodySize::Unknown,
                chunk_left: Some(2),
                chunk_size: 5,
                result: Ok((2, None)),
                state: ServerState::ReceivingBody,
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 4,
                dest_data: "lo",
            },
            Test {
                name: "chunked-complete-end",
                data: "\r\n",
                body_size: BodySize::Unknown,
                chunk_left: Some(0),
                chunk_size: 5,
                result: Ok((0, None)),
                state: ServerState::ReceivingBody,
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 2,
                dest_data: "",
            },
            Test {
                name: "chunked-empty",
                data: "0\r\n\r\n",
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                result: Ok((0, Some(&[]))),
                state: ServerState::AwaitingResponse,
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 5,
                dest_data: "",
            },
            Test {
                name: "trailing-headers-partial",
                data: "0\r\nhelloXX",
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                result: Ok((0, None)),
                state: ServerState::ReceivingBody,
                chunk_left_after: Some(0),
                chunk_size_after: 0,
                rbuf_position: 3,
                dest_data: "",
            },
            Test {
                name: "trailing-headers-parse-error",
                data: "0\r\nhelloXX\n",
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                result: Err(ServerError::ParseError(httparse::Error::Token)),
                state: ServerState::ReceivingBody,
                chunk_left_after: Some(0),
                chunk_size_after: 0,
                rbuf_position: 3,
                dest_data: "",
            },
            Test {
                name: "trailing-headers-complete",
                data: "0\r\nFoo: Bar\r\n\r\n",
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                result: Ok((
                    0,
                    Some(&[httparse::Header {
                        name: "Foo",
                        value: b"Bar",
                    }]),
                )),
                state: ServerState::AwaitingResponse,
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 15,
                dest_data: "",
            },
        ];

        for test in tests.iter() {
            let mut p = ServerProtocol {
                state: ServerState::ReceivingBody,
                ver_min: 0,
                body_size: test.body_size,
                chunk_left: test.chunk_left,
                chunk_size: test.chunk_size,
                persistent: false,
                chunked: test.body_size == BodySize::Unknown,
                sending_chunk: None,
            };

            let mut c = io::Cursor::new(test.data.as_bytes());
            let mut dest = [0; 1024];
            let mut dest_size = 0;
            let mut headers = [httparse::EMPTY_HEADER; HEADERS_MAX];

            let r = p.recv_body(&mut c, &mut dest, &mut headers);

            match r {
                Ok((size, headers)) => {
                    let (expected_size, expected_headers) = match &test.result {
                        Ok((size, headers)) => (size, headers),
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(size, *expected_size, "test={}", test.name);
                    assert_eq!(headers, *expected_headers, "test={}", test.name);

                    dest_size = size;
                }
                Err(e) => {
                    let expected = match &test.result {
                        Err(e) => e,
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(
                        mem::discriminant(&e),
                        mem::discriminant(expected),
                        "test={}",
                        test.name
                    );
                }
            }

            assert_eq!(p.state(), test.state, "test={}", test.name);
            assert_eq!(p.chunk_left, test.chunk_left_after, "test={}", test.name);
            assert_eq!(p.chunk_size, test.chunk_size_after, "test={}", test.name);
            assert_eq!(c.position(), test.rbuf_position, "test={}", test.name);
            assert_eq!(
                str::from_utf8(&dest[..dest_size]).unwrap(),
                test.dest_data,
                "test={}",
                test.name
            );
        }
    }

    #[test]
    fn test_send_response() {
        struct Test<'buf, 'headers> {
            name: &'static str,
            write_space: usize,
            code: u32,
            reason: &'static str,
            headers: &'headers [Header<'buf>],
            body_size: BodySize,
            ver_min: u8,
            persistent: bool,
            result: Result<(), ServerError>,
            state: ServerState,
            body_size_after: BodySize,
            chunked: bool,
            written: &'static str,
        }

        let tests = [
			Test {
				name: "cant-write-1.1",
				write_space: 5,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::Known(0),
				ver_min: 1,
				persistent: false,
				result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
				state: ServerState::AwaitingResponse,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "",
			},
			Test {
				name: "cant-write-1.0",
				write_space: 5,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::Known(0),
				ver_min: 0,
				persistent: false,
				result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
				state: ServerState::AwaitingResponse,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "",
			},
			Test {
				name: "cant-write-status-line",
				write_space: 12,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::Known(0),
				ver_min: 0,
				persistent: false,
				result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
				state: ServerState::AwaitingResponse,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.0 200",
			},
			Test {
				name: "cant-write-header-name",
				write_space: 20,
				code: 200,
				reason: "OK",
				headers: &[
					Header { name: "Foo", value: b"Bar" },
				],
				body_size: BodySize::Known(0),
				ver_min: 0,
				persistent: false,
				result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
				state: ServerState::AwaitingResponse,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.0 200 OK\r\nFoo",
			},
			Test {
				name: "cant-write-header-value",
				write_space: 24,
				code: 200,
				reason: "OK",
				headers: &[
					Header { name: "Foo", value: b"Bar" },
				],
				body_size: BodySize::Known(0),
				ver_min: 0,
				persistent: false,
				result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
				state: ServerState::AwaitingResponse,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.0 200 OK\r\nFoo: ",
			},
			Test {
				name: "cant-write-header-eol",
				write_space: 26,
				code: 200,
				reason: "OK",
				headers: &[
					Header { name: "Foo", value: b"Bar" },
				],
				body_size: BodySize::Known(0),
				ver_min: 0,
				persistent: false,
				result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
				state: ServerState::AwaitingResponse,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.0 200 OK\r\nFoo: Bar",
			},
			Test {
				name: "cant-write-keep-alive",
				write_space: 26,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::Known(0),
				ver_min: 0,
				persistent: true,
				result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
				state: ServerState::AwaitingResponse,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.0 200 OK\r\n",
			},
			Test {
				name: "cant-write-close",
				write_space: 26,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::Known(0),
				ver_min: 1,
				persistent: false,
				result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
				state: ServerState::AwaitingResponse,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.1 200 OK\r\n",
			},
			Test {
				name: "cant-write-transfer-encoding",
				write_space: 26,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::Unknown,
				ver_min: 1,
				persistent: false,
				result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
				state: ServerState::AwaitingResponse,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.1 200 OK\r\n",
			},
			Test {
				name: "cant-write-content-length",
				write_space: 26,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::Known(0),
				ver_min: 0,
				persistent: false,
				result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
				state: ServerState::AwaitingResponse,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.0 200 OK\r\n",
			},
			Test {
				name: "cant-write-te-chunked",
				write_space: 50,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::Unknown,
				ver_min: 1,
				persistent: true,
				result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
				state: ServerState::AwaitingResponse,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.1 200 OK\r\nConnection: Transfer-Encoding\r\n",
			},
			Test {
				name: "cant-write-eol",
				write_space: 18,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::Unknown,
				ver_min: 0,
				persistent: false,
				result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
				state: ServerState::AwaitingResponse,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.0 200 OK\r\n",
			},
			Test {
				name: "exclude-headers",
				write_space: 1024,
				code: 200,
				reason: "OK",
				headers: &[
					Header { name: "Connection", value: b"X" },
					Header { name: "Foo", value: b"Bar" },
					Header { name: "Content-Length", value: b"X" },
					Header { name: "Transfer-Encoding", value: b"X" },
				],
				body_size: BodySize::Unknown,
				ver_min: 0,
				persistent: false,
				result: Ok(()),
				state: ServerState::SendingBody,
				body_size_after: BodySize::Unknown,
				chunked: false,
				written: "HTTP/1.0 200 OK\r\nFoo: Bar\r\n\r\n",
			},
			Test {
				name: "exclude-headers-101",
				write_space: 1024,
				code: 101,
				reason: "Switching Protocols",
				headers: &[
					Header { name: "Connection", value: b"X" },
					Header { name: "Foo", value: b"Bar" },
					Header { name: "Content-Length", value: b"X" },
					Header { name: "Transfer-Encoding", value: b"X" },
				],
				body_size: BodySize::NoBody,
				ver_min: 0,
				persistent: false,
				result: Ok(()),
				state: ServerState::SendingBody,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.0 101 Switching Protocols\r\nConnection: X\r\nFoo: Bar\r\n\r\n",
			},
			Test {
				name: "1.0-no-body",
				write_space: 1024,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::NoBody,
				ver_min: 0,
				persistent: false,
				result: Ok(()),
				state: ServerState::SendingBody,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.0 200 OK\r\n\r\n",
			},
			Test {
				name: "1.0-len",
				write_space: 1024,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::Known(42),
				ver_min: 0,
				persistent: false,
				result: Ok(()),
				state: ServerState::SendingBody,
				body_size_after: BodySize::Known(42),
				chunked: false,
				written: "HTTP/1.0 200 OK\r\nContent-Length: 42\r\n\r\n",
			},
			Test {
				name: "1.0-no-len",
				write_space: 1024,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::Unknown,
				ver_min: 0,
				persistent: false,
				result: Ok(()),
				state: ServerState::SendingBody,
				body_size_after: BodySize::Unknown,
				chunked: false,
				written: "HTTP/1.0 200 OK\r\n\r\n",
			},
			Test {
				name: "1.1-no-body",
				write_space: 1024,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::NoBody,
				ver_min: 1,
				persistent: true,
				result: Ok(()),
				state: ServerState::SendingBody,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.1 200 OK\r\n\r\n",
			},
			Test {
				name: "1.1-len",
				write_space: 1024,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::Known(42),
				ver_min: 1,
				persistent: true,
				result: Ok(()),
				state: ServerState::SendingBody,
				body_size_after: BodySize::Known(42),
				chunked: false,
				written: "HTTP/1.1 200 OK\r\nContent-Length: 42\r\n\r\n",
			},
			Test {
				name: "1.1-no-len",
				write_space: 1024,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::Unknown,
				ver_min: 1,
				persistent: true,
				result: Ok(()),
				state: ServerState::SendingBody,
				body_size_after: BodySize::Unknown,
				chunked: true,
				written: "HTTP/1.1 200 OK\r\nConnection: Transfer-Encoding\r\nTransfer-Encoding: chunked\r\n\r\n",
			},
			Test {
				name: "1.0-persistent",
				write_space: 1024,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::NoBody,
				ver_min: 0,
				persistent: true,
				result: Ok(()),
				state: ServerState::SendingBody,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\n",
			},
			Test {
				name: "1.0-non-persistent",
				write_space: 1024,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::NoBody,
				ver_min: 0,
				persistent: false,
				result: Ok(()),
				state: ServerState::SendingBody,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.0 200 OK\r\n\r\n",
			},
			Test {
				name: "1.1-persistent",
				write_space: 1024,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::NoBody,
				ver_min: 1,
				persistent: true,
				result: Ok(()),
				state: ServerState::SendingBody,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.1 200 OK\r\n\r\n",
			},
			Test {
				name: "1.1-non-persistent",
				write_space: 1024,
				code: 200,
				reason: "OK",
				headers: &[],
				body_size: BodySize::NoBody,
				ver_min: 1,
				persistent: false,
				result: Ok(()),
				state: ServerState::SendingBody,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n",
			},
			Test {
				name: "force-no-body",
				write_space: 1024,
				code: 101,
				reason: "Switching Protocols",
				headers: &[],
				body_size: BodySize::Known(42),
				ver_min: 0,
				persistent: false,
				result: Ok(()),
				state: ServerState::SendingBody,
				body_size_after: BodySize::NoBody,
				chunked: false,
				written: "HTTP/1.0 101 Switching Protocols\r\n\r\n",
			},
		];

        for test in tests.iter() {
            let mut p = ServerProtocol {
                state: ServerState::AwaitingResponse,
                ver_min: test.ver_min,
                body_size: BodySize::NoBody,
                chunk_left: None,
                chunk_size: 0,
                persistent: test.persistent,
                chunked: false,
                sending_chunk: None,
            };

            let mut w = MyBuffer::new(test.write_space, false);

            let r = p.send_response(&mut w, test.code, test.reason, test.headers, test.body_size);

            match r {
                Ok(_) => {
                    match &test.result {
                        Ok(_) => {}
                        _ => panic!("result mismatch: test={}", test.name),
                    };
                }
                Err(e) => {
                    let expected = match &test.result {
                        Err(e) => e,
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(
                        mem::discriminant(&e),
                        mem::discriminant(expected),
                        "test={}",
                        test.name
                    );
                }
            }

            assert_eq!(p.state(), test.state, "test={}", test.name);
            assert_eq!(p.body_size, test.body_size_after, "test={}", test.name);
            assert_eq!(p.chunked, test.chunked, "test={}", test.name);

            assert_eq!(
                str::from_utf8(&w.data).unwrap(),
                test.written,
                "test={}",
                test.name
            );
        }
    }

    #[test]
    fn test_send_body() {
        struct Test {
            name: &'static str,
            write_space: usize,
            src: &'static str,
            end: bool,
            headers: Option<&'static [u8]>,
            body_size: BodySize,
            chunked: bool,
            sending_chunk: Option<Chunk>,
            result: Result<usize, ServerError>,
            state: ServerState,
            sending_chunk_after: Option<Chunk>,
            written: &'static str,
        }

        let tests = [
            Test {
                name: "no-body",
                write_space: 1024,
                src: "hello",
                end: false,
                headers: None,
                body_size: BodySize::NoBody,
                chunked: false,
                sending_chunk: None,
                result: Ok(5),
                state: ServerState::SendingBody,
                sending_chunk_after: None,
                written: "",
            },
            Test {
                name: "no-body-end",
                write_space: 1024,
                src: "",
                end: true,
                headers: None,
                body_size: BodySize::NoBody,
                chunked: false,
                sending_chunk: None,
                result: Ok(0),
                state: ServerState::Finished,
                sending_chunk_after: None,
                written: "",
            },
            Test {
                name: "non-chunked-partial",
                write_space: 3,
                src: "hello",
                end: false,
                headers: None,
                body_size: BodySize::Unknown,
                chunked: false,
                sending_chunk: None,
                result: Ok(3),
                state: ServerState::SendingBody,
                sending_chunk_after: None,
                written: "hel",
            },
            Test {
                name: "non-chunked-error",
                write_space: 0,
                src: "hello",
                end: false,
                headers: None,
                body_size: BodySize::Unknown,
                chunked: false,
                sending_chunk: None,
                result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
                state: ServerState::SendingBody,
                sending_chunk_after: None,
                written: "",
            },
            Test {
                name: "non-chunked",
                write_space: 1024,
                src: "hello",
                end: false,
                headers: None,
                body_size: BodySize::Unknown,
                chunked: false,
                sending_chunk: None,
                result: Ok(5),
                state: ServerState::SendingBody,
                sending_chunk_after: None,
                written: "hello",
            },
            Test {
                name: "non-chunked-end",
                write_space: 1024,
                src: "",
                end: true,
                headers: None,
                body_size: BodySize::Unknown,
                chunked: false,
                sending_chunk: None,
                result: Ok(0),
                state: ServerState::Finished,
                sending_chunk_after: None,
                written: "",
            },
            Test {
                name: "chunked-partial",
                write_space: 2,
                src: "hello",
                end: false,
                headers: None,
                body_size: BodySize::Unknown,
                chunked: true,
                sending_chunk: None,
                result: Ok(0),
                state: ServerState::SendingBody,
                sending_chunk_after: Some(Chunk {
                    header: [b'5', b'\r', b'\n', 0, 0, 0],
                    header_len: 3,
                    size: 5,
                    sent: 2,
                }),
                written: "5\r",
            },
            Test {
                name: "chunked-error",
                write_space: 0,
                src: "hello",
                end: false,
                headers: None,
                body_size: BodySize::Unknown,
                chunked: true,
                sending_chunk: None,
                result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
                state: ServerState::SendingBody,
                sending_chunk_after: Some(Chunk {
                    header: [b'5', b'\r', b'\n', 0, 0, 0],
                    header_len: 3,
                    size: 5,
                    sent: 0,
                }),
                written: "",
            },
            Test {
                name: "chunked-complete",
                write_space: 1024,
                src: "hello",
                end: false,
                headers: None,
                body_size: BodySize::Unknown,
                chunked: true,
                sending_chunk: None,
                result: Ok(5),
                state: ServerState::SendingBody,
                sending_chunk_after: None,
                written: "5\r\nhello\r\n",
            },
            Test {
                name: "end-partial",
                write_space: 2,
                src: "",
                end: true,
                headers: None,
                body_size: BodySize::Unknown,
                chunked: true,
                sending_chunk: None,
                result: Ok(0),
                state: ServerState::SendingBody,
                sending_chunk_after: Some(Chunk {
                    header: [b'0', b'\r', b'\n', 0, 0, 0],
                    header_len: 3,
                    size: 0,
                    sent: 2,
                }),
                written: "0\r",
            },
            Test {
                name: "end-error",
                write_space: 0,
                src: "",
                end: true,
                headers: None,
                body_size: BodySize::Unknown,
                chunked: true,
                sending_chunk: None,
                result: Err(ServerError::Io(io::Error::from(io::ErrorKind::WriteZero))),
                state: ServerState::SendingBody,
                sending_chunk_after: Some(Chunk {
                    header: [b'0', b'\r', b'\n', 0, 0, 0],
                    header_len: 3,
                    size: 0,
                    sent: 0,
                }),
                written: "",
            },
            Test {
                name: "end-complete",
                write_space: 1024,
                src: "",
                end: true,
                headers: None,
                body_size: BodySize::Unknown,
                chunked: true,
                sending_chunk: None,
                result: Ok(0),
                state: ServerState::Finished,
                sending_chunk_after: None,
                written: "0\r\n\r\n",
            },
            Test {
                name: "end-headers",
                write_space: 1024,
                src: "",
                end: true,
                headers: Some(b"Foo: Bar\r\n\r\n"),
                body_size: BodySize::Unknown,
                chunked: true,
                sending_chunk: None,
                result: Ok(0),
                state: ServerState::Finished,
                sending_chunk_after: None,
                written: "0\r\nFoo: Bar\r\n\r\n",
            },
            Test {
                name: "content-and-end",
                write_space: 1024,
                src: "hello",
                end: true,
                headers: None,
                body_size: BodySize::Unknown,
                chunked: true,
                sending_chunk: None,
                result: Ok(5),
                state: ServerState::Finished,
                sending_chunk_after: None,
                written: "5\r\nhello\r\n0\r\n\r\n",
            },
        ];

        for test in tests.iter() {
            let mut p = ServerProtocol {
                state: ServerState::SendingBody,
                ver_min: 0,
                body_size: test.body_size,
                chunk_left: None,
                chunk_size: 0,
                persistent: false,
                chunked: test.chunked,
                sending_chunk: test.sending_chunk,
            };

            let mut w = MyBuffer::new(test.write_space, true);

            let r = p.send_body(&mut w, &[test.src.as_bytes()], test.end, test.headers);

            match r {
                Ok(size) => {
                    let expected_size = match &test.result {
                        Ok(size) => size,
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(size, *expected_size, "test={}", test.name);
                }
                Err(e) => {
                    let expected = match &test.result {
                        Err(e) => e,
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(
                        mem::discriminant(&e),
                        mem::discriminant(expected),
                        "test={}",
                        test.name
                    );
                }
            }

            assert_eq!(p.state(), test.state, "test={}", test.name);
            assert_eq!(
                p.sending_chunk, test.sending_chunk_after,
                "test={}",
                test.name
            );

            assert_eq!(
                str::from_utf8(&w.data).unwrap(),
                test.written,
                "test={}",
                test.name
            );
        }
    }

    #[test]
    fn test_req() {
        let data = "GET /foo HTTP/1.1\r\nHost: example.com\r\n\r\n".as_bytes();

        let mut p = ServerProtocol::new();
        let req = read_req(&mut p, data, 2);

        assert_eq!(req.method, "GET");
        assert_eq!(req.uri, "/foo");
        assert_eq!(req.headers.len(), 1);
        assert_eq!(req.headers[0].0, "Host");
        assert_eq!(req.headers[0].1, b"example.com");
        assert_eq!(req.body.len(), 0);
        assert_eq!(req.trailing_headers.len(), 0);
        assert_eq!(req.persistent, true);

        let data = concat!(
            "POST /foo HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n"
        )
        .as_bytes();

        let mut p = ServerProtocol::new();
        let req = read_req(&mut p, data, 2);

        assert_eq!(req.method, "POST");
        assert_eq!(req.uri, "/foo");
        assert_eq!(req.headers.len(), 2);
        assert_eq!(req.headers[0].0, "Host");
        assert_eq!(req.headers[0].1, b"example.com");
        assert_eq!(req.body, b"hello\n");
        assert_eq!(req.trailing_headers.len(), 0);
        assert_eq!(req.persistent, true);

        let data = concat!(
            "POST /foo HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Transfer-Encoding: chunked\r\n",
            "\r\n",
            "6\r\nhello\n\r\n",
            "0\r\n\r\n"
        )
        .as_bytes();

        let mut p = ServerProtocol::new();
        let req = read_req(&mut p, data, 2);

        assert_eq!(req.method, "POST");
        assert_eq!(req.uri, "/foo");
        assert_eq!(req.headers.len(), 2);
        assert_eq!(req.headers[0].0, "Host");
        assert_eq!(req.headers[0].1, b"example.com");
        assert_eq!(req.body, b"hello\n");
        assert_eq!(req.trailing_headers.len(), 0);
        assert_eq!(req.persistent, true);

        let data = concat!(
            "POST /foo HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Transfer-Encoding: chunked\r\n",
            "\r\n",
            "6\r\nhello\n\r\n",
            "0\r\n",
            "Foo: bar\r\n",
            "\r\n"
        )
        .as_bytes();

        let mut p = ServerProtocol::new();
        let req = read_req(&mut p, data, 2);

        assert_eq!(req.method, "POST");
        assert_eq!(req.uri, "/foo");
        assert_eq!(req.headers.len(), 2);
        assert_eq!(req.headers[0].0, "Host");
        assert_eq!(req.headers[0].1, b"example.com");
        assert_eq!(req.body, b"hello\n");
        assert_eq!(req.trailing_headers.len(), 1);
        assert_eq!(req.trailing_headers[0].0, "Foo");
        assert_eq!(req.trailing_headers[0].1, b"bar");
        assert_eq!(req.persistent, true);
    }

    #[test]
    fn test_resp() {
        let data = "GET /foo HTTP/1.1\r\nHost: example.com\r\n\r\n";

        let mut p = ServerProtocol::new();
        read_req(&mut p, data.as_bytes(), 2);

        let mut resp = TestResponse::new();
        resp.code = 200;
        resp.reason = String::from("OK");
        resp.headers = vec![(String::from("Content-Type"), b"text/plain".to_vec())];
        resp.body = b"hello\n".to_vec();

        let out = write_resp(&mut p, resp, 2);

        let data = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n",
        );

        assert_eq!(str::from_utf8(&out).unwrap(), data);

        let data = "GET /foo HTTP/1.1\r\nHost: example.com\r\n\r\n";

        let mut p = ServerProtocol::new();
        read_req(&mut p, data.as_bytes(), 2);

        let mut resp = TestResponse::new();
        resp.code = 200;
        resp.reason = String::from("OK");
        resp.headers = vec![(String::from("Content-Type"), b"text/plain".to_vec())];
        resp.body = b"hello\n".to_vec();
        resp.chunked = true;

        let out = write_resp(&mut p, resp, 2);

        let data = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Connection: Transfer-Encoding\r\n",
            "Transfer-Encoding: chunked\r\n",
            "\r\n",
            "6\r\nhello\n\r\n0\r\n\r\n",
        );

        assert_eq!(str::from_utf8(&out).unwrap(), data);

        let data = "GET /foo HTTP/1.1\r\nHost: example.com\r\n\r\n";

        let mut p = ServerProtocol::new();
        read_req(&mut p, data.as_bytes(), 2);

        let mut resp = TestResponse::new();
        resp.code = 200;
        resp.reason = String::from("OK");
        resp.headers = vec![(String::from("Content-Type"), b"text/plain".to_vec())];
        resp.body = b"hello\n".to_vec();
        resp.chunked = true;
        resp.trailing_headers = vec![(String::from("Foo"), b"bar".to_vec())];

        let out = write_resp(&mut p, resp, 2);

        let data = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Connection: Transfer-Encoding\r\n",
            "Transfer-Encoding: chunked\r\n",
            "\r\n",
            "6\r\nhello\n\r\n",
            "0\r\n",
            "Foo: bar\r\n",
            "\r\n"
        );

        assert_eq!(str::from_utf8(&out).unwrap(), data);
    }

    #[test]
    fn test_persistent() {
        // http 1.0 without keep alive
        let data = concat!("GET /foo HTTP/1.0\r\n", "Host: example.com\r\n", "\r\n").as_bytes();

        let mut p = ServerProtocol::new();
        let req = read_req(&mut p, data, 2);

        assert_eq!(req.persistent, false);

        // http 1.0 with keep alive
        let data = concat!(
            "GET /foo HTTP/1.0\r\n",
            "Host: example.com\r\n",
            "Connection: keep-alive\r\n",
            "\r\n"
        )
        .as_bytes();

        let mut p = ServerProtocol::new();
        let req = read_req(&mut p, data, 2);

        assert_eq!(req.persistent, true);

        // http 1.1 without keep alive
        let data = concat!(
            "GET /foo HTTP/1.1\r\n",
            "Host: example.com\r\n",
            "Connection: close\r\n",
            "\r\n"
        )
        .as_bytes();

        let mut p = ServerProtocol::new();
        let req = read_req(&mut p, data, 2);

        assert_eq!(req.persistent, false);

        // http 1.1 with keep alive
        let data = concat!("GET /foo HTTP/1.1\r\n", "Host: example.com\r\n", "\r\n").as_bytes();

        let mut p = ServerProtocol::new();
        let req = read_req(&mut p, data, 2);

        assert_eq!(req.persistent, true);
    }
}
