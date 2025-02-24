/*
 * Copyright (C) 2020-2023 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#![allow(clippy::collapsible_if)]
#![allow(clippy::collapsible_else_if)]

use crate::core::buffer::{write_vectored_offset, FilledBuf, LimitBufs, VECTORED_MAX};
use arrayvec::ArrayVec;
use std::cmp;
use std::convert::TryFrom;
use std::io;
use std::io::{Read, Write};
use std::mem;
use std::str;

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

fn find_one_of(s: &str, values: &[u8]) -> Option<(usize, u8)> {
    for (pos, &c) in s.as_bytes().iter().enumerate() {
        for v in values {
            if c == *v {
                return Some((pos, c));
            }
        }
    }

    None
}

fn find_non_space(s: &str) -> Option<usize> {
    for (pos, c) in s.char_indices() {
        if !c.is_ascii_whitespace() {
            return Some(pos);
        }
    }

    None
}

// return (value, remainder)
fn parse_quoted(s: &str) -> Result<(&str, &str), io::Error> {
    match s.find('"') {
        Some(pos) => Ok((&s[..pos], &s[(pos + 1)..])),
        None => Err(io::Error::from(io::ErrorKind::InvalidData)),
    }
}

// return (value, remainder).
// remainder will start at the first non-space character following the param,
// or will be empty
fn parse_param_value(s: &str) -> Result<(&str, &str), io::Error> {
    let s = match find_non_space(s) {
        Some(pos) => &s[pos..],
        None => return Ok(("", "")),
    };

    if s.as_bytes()[0] == b'"' {
        let (s, remainder) = parse_quoted(&s[1..])?;

        let remainder = match find_non_space(remainder) {
            Some(pos) => &remainder[pos..],
            None => "",
        };

        Ok((s, remainder))
    } else {
        let (s, remainder) = match find_one_of(s, b";,") {
            Some((pos, _)) => (&s[..pos], &s[pos..]),
            None => (s, ""),
        };

        Ok((s.trim(), remainder))
    }
}

pub struct HeaderParamsIterator<'a> {
    s: &'a str,
    done: bool,
}

impl<'a> HeaderParamsIterator<'a> {
    fn new(s: &'a str) -> Self {
        Self { s, done: false }
    }

    fn empty() -> Self {
        Self { s: "", done: true }
    }
}

impl<'a> Iterator for HeaderParamsIterator<'a> {
    type Item = Result<(&'a str, &'a str), io::Error>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.done {
            return None;
        }

        let (k, v, remainder, done) = match find_one_of(self.s, b"=;,") {
            Some((pos, b'=')) => {
                let k = &self.s[..pos];

                let (v, remainder) = match parse_param_value(&self.s[(pos + 1)..]) {
                    Ok(ret) => ret,
                    Err(e) => return Some(Err(e)),
                };

                let (remainder, done) = if !remainder.is_empty() {
                    match remainder.as_bytes()[0] {
                        b';' => (&remainder[1..], false),
                        b',' => (remainder, true),
                        _ => return Some(Err(io::Error::from(io::ErrorKind::InvalidData))),
                    }
                } else {
                    ("", true)
                };

                (k, v, remainder, done)
            }
            Some((pos, b';')) => (&self.s[..pos], "", &self.s[(pos + 1)..], false),
            Some((pos, b',')) => (&self.s[..pos], "", &self.s[pos..], true),
            Some(_) => unreachable!(),
            None => (self.s, "", "", true),
        };

        let k = k.trim();

        if k.is_empty() {
            return Some(Err(io::Error::from(io::ErrorKind::InvalidData)));
        }

        self.s = remainder;
        self.done = done;

        Some(Ok((k, v)))
    }
}

pub struct HeaderValueIterator<'a> {
    s: &'a str,
    done: bool,
}

impl<'a> Iterator for HeaderValueIterator<'a> {
    type Item = Result<(&'a str, HeaderParamsIterator<'a>), io::Error>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.done {
            return None;
        }

        let (first_part, params, remainder, done) = match find_one_of(self.s, b";,") {
            Some((pos, b';')) => {
                // make a temporary params iterator
                let mut params = HeaderParamsIterator::new(&self.s[(pos + 1)..]);

                // drive it to the end
                for p in params.by_ref() {
                    if let Err(e) = p {
                        return Some(Err(e));
                    }
                }

                // when HeaderParamsIterator completes, its remaining value
                // will either start with a comma or be empty
                let (remainder, done) = if params.s.starts_with(',') {
                    (&params.s[1..], false)
                } else if params.s.is_empty() {
                    ("", true)
                } else {
                    unreachable!();
                };

                // prepare a fresh iterator for returning
                let params = HeaderParamsIterator::new(&self.s[(pos + 1)..]);

                (&self.s[..pos], params, remainder, done)
            }
            Some((pos, b',')) => (
                &self.s[..pos],
                HeaderParamsIterator::empty(),
                &self.s[(pos + 1)..],
                false,
            ),
            Some(_) => unreachable!(),
            None => (self.s, HeaderParamsIterator::empty(), "", true),
        };

        let first_part = first_part.trim();

        if first_part.is_empty() {
            return Some(Err(io::Error::from(io::ErrorKind::InvalidData)));
        }

        self.s = remainder;
        self.done = done;

        Some(Ok((first_part, params)))
    }
}

// parse a header value into parts
pub fn parse_header_value(s: &[u8]) -> HeaderValueIterator {
    match str::from_utf8(s) {
        Ok(s) => HeaderValueIterator { s, done: false },
        Err(_) => HeaderValueIterator { s: "", done: false },
    }
}

#[derive(Debug, PartialEq, Clone, Copy)]
struct Chunk {
    header: [u8; CHUNK_HEADER_SIZE_MAX],
    header_len: usize,
    size: usize,
    sent: usize,
}

// writes src to dest as chunks. current chunk state is passed in
fn write_chunk<W: Write>(
    content: &[&[u8]],
    footer: &[u8],
    dest: &mut W,
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

    let mut content = ArrayVec::<&[u8], { VECTORED_MAX - 2 }>::try_from(content).unwrap();
    let content = content.as_mut_slice().limit(data_size);

    let size = {
        let mut out = ArrayVec::<&[u8], VECTORED_MAX>::new();

        out.push(cheader);

        for buf in content.as_slice() {
            out.push(buf);
        }

        out.push(footer);

        write_vectored_offset(dest, out.as_slice(), chunkv.sent)?
    };

    chunkv.sent += size;

    if chunkv.sent < total {
        return Ok(0);
    }

    *chunk = None;

    Ok(data_size)
}

#[cfg(test)]
pub fn write_headers<W: Write>(writer: &mut W, headers: &[Header]) -> Result<(), io::Error> {
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

pub struct ParseScratch<const N: usize> {
    headers: [httparse::Header<'static>; N],
}

#[allow(clippy::new_without_default)]
impl<const N: usize> ParseScratch<N> {
    pub fn new() -> Self {
        Self {
            headers: [httparse::EMPTY_HEADER; N],
        }
    }

    fn clear(&mut self) {
        self.headers.fill(httparse::EMPTY_HEADER);
    }
}

pub enum ParseStatus<'a, T, I, E, const N: usize> {
    Complete(T),
    Incomplete(I, FilledBuf, &'a mut ParseScratch<N>),
    Error(E, FilledBuf, &'a mut ParseScratch<N>),
}

struct OwnedParsedInner<'s, T, const N: usize> {
    parsed: T,
    scratch: &'s mut ParseScratch<N>,
    buf: FilledBuf,
    size: usize,
}

struct OwnedHttparseRequest<'s, const N: usize> {
    inner: Option<OwnedParsedInner<'s, httparse::Request<'s, 'static>, N>>,
}

impl<'s, const N: usize> OwnedHttparseRequest<'s, N> {
    // on success, takes ownership of the buffer/scratch
    // on incomplete/error, returns the buffer/scratch
    fn parse(
        buf: FilledBuf,
        scratch: &'s mut ParseScratch<N>,
    ) -> ParseStatus<'s, Self, (), httparse::Error, N> {
        let buf_ref: &[u8] = buf.filled();
        let headers_mut: &mut [httparse::Header<'static>] = scratch.headers.as_mut();

        // SAFETY: Self will take ownership of buf, and the bytes referred to
        // by buf_ref are on the heap, and buf will not be modified or
        // dropped until Self is dropped, so the bytes referred to by buf_ref
        // will remain valid for the lifetime of Self
        let buf_ref: &'static [u8] = unsafe { mem::transmute(buf_ref) };

        // SAFETY: Self borrows scratch, and the location
        // referred to by headers_mut is on the heap, and the borrow will not
        // be released until Self is dropped, so the location referred to by
        // headers_mut will remain valid for the lifetime of Self
        //
        // further, it is safe for httparse::Request::parse() to write
        // references to buf_ref into headers_mut, because we guarantee buf
        // lives as long as scratch, except if into_buf() is called in
        // which case we clear the content of scratch
        let headers_mut: &'static mut [httparse::Header<'static>] =
            unsafe { mem::transmute(headers_mut) };

        let mut req = httparse::Request::new(headers_mut);

        let size = match req.parse(buf_ref) {
            Ok(httparse::Status::Complete(size)) => size,
            Ok(httparse::Status::Partial) => return ParseStatus::Incomplete((), buf, scratch),
            Err(e) => return ParseStatus::Error(e, buf, scratch),
        };

        ParseStatus::Complete(Self {
            inner: Some(OwnedParsedInner {
                parsed: req,
                scratch,
                buf,
                size,
            }),
        })
    }

    fn get<'a>(&'a self) -> &'a httparse::Request<'a, 'a> {
        let s = self.inner.as_ref().unwrap();

        let req = &s.parsed;

        // SAFETY: here we simply reduce the inner lifetimes to that of the owning
        // object, which is fine
        let req: &'a httparse::Request<'a, 'a> = unsafe { mem::transmute(req) };

        req
    }

    fn remaining_bytes(&self) -> &[u8] {
        let s = self.inner.as_ref().unwrap();

        &s.buf.filled()[s.size..]
    }

    fn into_parts(mut self) -> (FilledBuf, &'s mut ParseScratch<N>) {
        let OwnedParsedInner { buf, scratch, .. } = self.inner.take().unwrap();

        // SAFETY: ensure there are no references to buf in scratch
        scratch.clear();

        (buf, scratch)
    }
}

impl<const N: usize> Drop for OwnedHttparseRequest<'_, N> {
    fn drop(&mut self) {
        // SAFETY: ensure there are no references to buf in scratch
        if let Some(s) = &mut self.inner {
            s.scratch.clear();
        }
    }
}

struct OwnedHttparseResponse<'s, const N: usize> {
    inner: Option<OwnedParsedInner<'s, httparse::Response<'s, 'static>, N>>,
}

impl<'s, const N: usize> OwnedHttparseResponse<'s, N> {
    // on success, takes ownership of the buffer/scratch
    // on incomplete/error, returns the buffer/scratch
    fn parse(
        buf: FilledBuf,
        scratch: &'s mut ParseScratch<N>,
    ) -> ParseStatus<'s, Self, (), httparse::Error, N> {
        let buf_ref: &[u8] = buf.filled();
        let headers_mut: &mut [httparse::Header<'static>] = scratch.headers.as_mut();

        // SAFETY: Self will take ownership of buf, and the bytes referred to
        // by buf_ref are on the heap, and buf will not be modified or
        // dropped until Self is dropped, so the bytes referred to by buf_ref
        // will remain valid for the lifetime of Self
        let buf_ref: &'static [u8] = unsafe { mem::transmute(buf_ref) };

        // SAFETY: Self borrows scratch, and the location
        // referred to by headers_mut is on the heap, and the borrow will not
        // be released until Self is dropped, so the location referred to by
        // headers_mut will remain valid for the lifetime of Self
        //
        // further, it is safe for httparse::Response::parse() to write
        // references to buf_ref into headers_mut, because we guarantee buf
        // lives as long as scratch, except if into_buf() is called in
        // which case we clear the content of scratch
        let headers_mut: &'static mut [httparse::Header<'static>] =
            unsafe { mem::transmute(headers_mut) };

        let mut resp = httparse::Response::new(headers_mut);

        let size = match resp.parse(buf_ref) {
            Ok(httparse::Status::Complete(size)) => size,
            Ok(httparse::Status::Partial) => return ParseStatus::Incomplete((), buf, scratch),
            Err(e) => return ParseStatus::Error(e, buf, scratch),
        };

        ParseStatus::Complete(Self {
            inner: Some(OwnedParsedInner {
                parsed: resp,
                scratch,
                buf,
                size,
            }),
        })
    }

    fn get<'a>(&'a self) -> &'a httparse::Response<'a, 'a> {
        let s = self.inner.as_ref().unwrap();

        let resp = &s.parsed;

        // SAFETY: here we simply reduce the inner lifetimes to that of the owning
        // object, which is fine
        let resp: &'a httparse::Response<'a, 'a> = unsafe { mem::transmute(resp) };

        resp
    }

    fn remaining_bytes(&self) -> &[u8] {
        let s = self.inner.as_ref().unwrap();

        &s.buf.filled()[s.size..]
    }

    fn into_parts(mut self) -> (FilledBuf, &'s mut ParseScratch<N>) {
        let OwnedParsedInner { buf, scratch, .. } = self.inner.take().unwrap();

        // SAFETY: ensure there are no references to buf in scratch
        scratch.clear();

        (buf, scratch)
    }
}

impl<const N: usize> Drop for OwnedHttparseResponse<'_, N> {
    fn drop(&mut self) {
        // SAFETY: ensure there are no references to buf in scratch
        if let Some(s) = &mut self.inner {
            s.scratch.clear();
        }
    }
}

#[derive(Debug, PartialEq)]
pub struct Request<'buf, 'headers> {
    pub method: &'buf str,
    pub uri: &'buf str,
    pub headers: &'headers [httparse::Header<'buf>],
    pub body_size: BodySize,
    pub expect_100: bool,
}

pub struct OwnedRequest<'s, const N: usize> {
    req: OwnedHttparseRequest<'s, N>,
    body_size: BodySize,
    expect_100: bool,
}

impl<const N: usize> OwnedRequest<'_, N> {
    pub fn get(&self) -> Request {
        let req = self.req.get();

        Request {
            method: req.method.unwrap(),
            uri: req.path.unwrap(),
            headers: req.headers,
            body_size: self.body_size,
            expect_100: self.expect_100,
        }
    }

    pub fn remaining_bytes(&self) -> &[u8] {
        self.req.remaining_bytes()
    }

    pub fn into_buf(self) -> FilledBuf {
        self.req.into_parts().0
    }
}

#[derive(Debug, PartialEq)]
pub struct Response<'buf, 'headers> {
    pub code: u16,
    pub reason: &'buf str,
    pub headers: &'headers [httparse::Header<'buf>],
    pub body_size: BodySize,
}

pub struct OwnedResponse<'s, const N: usize> {
    resp: OwnedHttparseResponse<'s, N>,
    body_size: BodySize,
}

impl<const N: usize> OwnedResponse<'_, N> {
    pub fn get(&self) -> Response {
        let resp = self.resp.get();

        Response {
            code: resp.code.unwrap(),
            reason: resp.reason.unwrap(),
            headers: resp.headers,
            body_size: self.body_size,
        }
    }

    pub fn remaining_bytes(&self) -> &[u8] {
        self.resp.remaining_bytes()
    }

    pub fn into_buf(self) -> FilledBuf {
        self.resp.into_parts().0
    }
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

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error(transparent)]
    Parse(#[from] httparse::Error),

    #[error("invalid content length")]
    InvalidContentLength,

    #[error("unsupported transfer encoding")]
    UnsupportedTransferEncoding,

    #[error(transparent)]
    Io(#[from] io::Error),

    #[error("invalid chunk size")]
    InvalidChunkSize,

    #[error("chunk too large")]
    ChunkTooLarge,

    #[error("invalid chunk suffix")]
    InvalidChunkSuffix,
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

#[allow(clippy::new_without_default)]
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

    #[cfg(test)]
    pub fn recv_request(
        &mut self,
        rbuf: &mut io::Cursor<&'buf [u8]>,
        headers: &'headers mut [httparse::Header<'buf>],
    ) -> Option<Result<Request<'buf, 'headers>, Error>> {
        assert_eq!(self.state, ServerState::ReceivingRequest);

        let mut req = httparse::Request::new(headers);

        let buf = &rbuf.get_ref()[(rbuf.position() as usize)..];

        let size = match req.parse(buf) {
            Ok(httparse::Status::Complete(size)) => size,
            Ok(httparse::Status::Partial) => return None,
            Err(e) => return Some(Err(Error::Parse(e))),
        };

        let expect_100 = match self.process_request(&req) {
            Ok(ret) => ret,
            Err(e) => return Some(Err(e)),
        };

        rbuf.set_position(rbuf.position() + (size as u64));

        Some(Ok(Request {
            method: req.method.unwrap(),
            uri: req.path.unwrap(),
            headers: req.headers,
            body_size: self.body_size,
            expect_100,
        }))
    }

    pub fn recv_request_owned<'a, const N: usize>(
        &mut self,
        rbuf: FilledBuf,
        scratch: &'a mut ParseScratch<N>,
    ) -> ParseStatus<'a, OwnedRequest<'a, N>, (), Error, N> {
        assert_eq!(self.state, ServerState::ReceivingRequest);

        let req = match OwnedHttparseRequest::parse(rbuf, scratch) {
            ParseStatus::Complete(req) => req,
            ParseStatus::Incomplete((), rbuf, scratch) => {
                return ParseStatus::Incomplete((), rbuf, scratch)
            }
            ParseStatus::Error(e, rbuf, scratch) => {
                return ParseStatus::Error(Error::Parse(e), rbuf, scratch)
            }
        };

        let expect_100 = match self.process_request(req.get()) {
            Ok(ret) => ret,
            Err(e) => {
                let (buf, scratch) = req.into_parts();
                return ParseStatus::Error(e, buf, scratch);
            }
        };

        ParseStatus::Complete(OwnedRequest {
            req,
            body_size: self.body_size,
            expect_100,
        })
    }

    pub fn skip_recv_request(&mut self) {
        assert_eq!(self.state, ServerState::ReceivingRequest);

        self.state = ServerState::AwaitingResponse;
        self.persistent = false;
    }

    #[allow(clippy::type_complexity)]
    pub fn recv_body(
        &mut self,
        rbuf: &mut io::Cursor<&'buf [u8]>,
        dest: &mut [u8],
        headers: &'headers mut [httparse::Header<'buf>],
    ) -> Result<Option<(usize, Option<&'headers [httparse::Header<'buf>]>)>, Error> {
        assert_eq!(self.state, ServerState::ReceivingBody);

        match self.body_size {
            BodySize::Known(_) => {
                let mut chunk_left = self.chunk_left.unwrap();
                let src_avail = cmp::min(
                    chunk_left,
                    rbuf.get_ref()[(rbuf.position() as usize)..].len(),
                );
                let read_size = cmp::min(src_avail, dest.len());

                // rbuf holds body as-is
                let size = rbuf.read(&mut dest[..read_size])?;

                chunk_left -= size;

                if chunk_left == 0 {
                    self.chunk_left = None;
                    self.state = ServerState::AwaitingResponse;
                } else {
                    self.chunk_left = Some(chunk_left);

                    // nothing to read?
                    if src_avail == 0 {
                        assert_eq!(size, 0);
                        return Ok(None);
                    }
                }

                Ok(Some((size, None)))
            }
            BodySize::Unknown => {
                if self.chunk_left.is_none() {
                    let buf = &rbuf.get_ref()[(rbuf.position() as usize)..];

                    match httparse::parse_chunk_size(buf) {
                        Ok(httparse::Status::Complete((pos, size))) => {
                            let size = match u32::try_from(size) {
                                Ok(size) => size,
                                Err(_) => return Err(Error::ChunkTooLarge),
                            };

                            let size = size as usize;

                            rbuf.set_position(rbuf.position() + (pos as u64));

                            self.chunk_left = Some(size);
                            self.chunk_size = size;
                        }
                        Ok(httparse::Status::Partial) => return Ok(None),
                        Err(_) => return Err(Error::InvalidChunkSize),
                    }
                }

                let mut chunk_left = self.chunk_left.unwrap();

                if chunk_left > 0 {
                    let src_avail = cmp::min(
                        chunk_left,
                        rbuf.get_ref()[(rbuf.position() as usize)..].len(),
                    );
                    let read_size = cmp::min(src_avail, dest.len());

                    let size = rbuf.read(&mut dest[..read_size])?;

                    chunk_left -= size;

                    self.chunk_left = Some(chunk_left);

                    // nothing to read?
                    if src_avail == 0 {
                        assert_eq!(size, 0);
                        return Ok(None);
                    }

                    return Ok(Some((size, None)));
                }

                // done with content bytes. now to read the footer

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
                            Ok(httparse::Status::Partial) => return Ok(None),
                            Err(e) => return Err(Error::Parse(e)),
                        }

                        self.state = ServerState::AwaitingResponse;
                    } else {
                        if buf.len() < 2 {
                            return Ok(None);
                        }

                        if &buf[..2] != b"\r\n" {
                            return Err(Error::InvalidChunkSuffix);
                        }

                        rbuf.set_position(rbuf.position() + 2);
                    }

                    self.chunk_left = None;
                    self.chunk_size = 0;
                }

                Ok(Some((0, trailing_headers)))
            }
            BodySize::NoBody => unreachable!(),
        }
    }

    pub fn send_100_continue<W: Write>(&mut self, writer: &mut W) -> Result<(), Error> {
        writer.write_all(b"HTTP/1.1 100 Continue\r\n\r\n")?;

        Ok(())
    }

    pub fn send_response<W: Write>(
        &mut self,
        writer: &mut W,
        code: u16,
        reason: &str,
        headers: &[Header],
        body_size: BodySize,
    ) -> Result<(), Error> {
        assert!(
            self.state == ServerState::AwaitingResponse || self.state == ServerState::ReceivingBody
        );

        let persistent = if self.state == ServerState::ReceivingBody {
            // when responding early, input stream may be broken
            false
        } else {
            self.persistent
        };

        let mut body_size = body_size;

        // certain responses have no body
        match code {
            100..=199 | 204 | 304 => {
                body_size = BodySize::NoBody;
            }
            _ => {}
        }

        let chunked = body_size == BodySize::Unknown && self.ver_min >= 1;

        if self.ver_min >= 1 {
            writer.write_all(b"HTTP/1.1 ")?;
        } else {
            writer.write_all(b"HTTP/1.0 ")?;
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
            writer.write_all(h.value)?;
            writer.write_all(b"\r\n")?;
        }

        // Connection header

        if persistent && self.ver_min == 0 {
            writer.write_all(b"Connection: keep-alive\r\n")?;
        } else if !persistent && self.ver_min >= 1 {
            writer.write_all(b"Connection: close\r\n")?;
        }

        if chunked {
            writer.write_all(b"Connection: Transfer-Encoding\r\n")?;
        }

        // Content-Length header

        if let BodySize::Known(x) = body_size {
            write!(writer, "Content-Length: {}\r\n", x)?;
        }

        // Transfer-Encoding header

        if chunked {
            writer.write_all(b"Transfer-Encoding: chunked\r\n")?;
        }

        writer.write_all(b"\r\n")?;

        self.state = ServerState::SendingBody;
        self.body_size = body_size;
        self.persistent = persistent;
        self.chunked = chunked;

        Ok(())
    }

    pub fn send_body<W: Write>(
        &mut self,
        writer: &mut W,
        src: &[&[u8]],
        end: bool,
        headers: Option<&[u8]>,
    ) -> Result<usize, Error> {
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

    fn process_request(&mut self, req: &httparse::Request) -> Result<bool, Error> {
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
                    Err(_) => return Err(Error::InvalidContentLength),
                };

                content_len = Some(len);
            } else if h.name.eq_ignore_ascii_case("Transfer-Encoding") {
                if h.value == b"chunked" {
                    chunked = true;
                } else {
                    // unknown transfer encoding
                    return Err(Error::UnsupportedTransferEncoding);
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

        self.state = match self.body_size {
            BodySize::Unknown | BodySize::Known(_) => ServerState::ReceivingBody,
            BodySize::NoBody => ServerState::AwaitingResponse,
        };

        let expect_100 = expect_100 && self.body_size != BodySize::NoBody;

        Ok(expect_100)
    }
}

struct ClientState {
    ver_min: u8,
    body_size: BodySize,
    chunk_left: Option<usize>,
    chunk_size: usize,
    persistent: bool,
    chunked: bool,
    sending_chunk: Option<Chunk>,
}

#[allow(clippy::new_without_default)]
impl ClientState {
    fn new() -> Self {
        Self {
            ver_min: 1,
            body_size: BodySize::NoBody,
            chunk_left: None,
            chunk_size: 0,
            persistent: true,
            chunked: false,
            sending_chunk: None,
        }
    }
}

pub struct ClientRequest {
    state: ClientState,
}

#[allow(clippy::new_without_default)]
impl ClientRequest {
    pub fn new() -> Self {
        Self {
            state: ClientState::new(),
        }
    }

    pub fn send_header<W: Write>(
        mut self,
        writer: &mut W,
        method: &str,
        uri: &str,
        headers: &[Header],
        body_size: BodySize,
        websocket: bool,
    ) -> Result<ClientRequestBody, Error> {
        let body_size = if websocket {
            BodySize::NoBody
        } else {
            body_size
        };

        let chunked = body_size == BodySize::Unknown;

        write!(writer, "{} {} HTTP/1.1\r\n", method, uri)?;

        for h in headers.iter() {
            // we'll override these headers
            if (h.name.eq_ignore_ascii_case("Connection") && !websocket)
                || h.name.eq_ignore_ascii_case("Content-Length")
                || h.name.eq_ignore_ascii_case("Transfer-Encoding")
            {
                continue;
            }

            write!(writer, "{}: ", h.name)?;
            writer.write_all(h.value)?;
            writer.write_all(b"\r\n")?;
        }

        // Connection header

        if chunked {
            writer.write_all(b"Connection: Transfer-Encoding\r\n")?;
        }

        // Content-Length header

        if let BodySize::Known(x) = body_size {
            if x > 0
                || !method.eq_ignore_ascii_case("OPTIONS")
                    && !method.eq_ignore_ascii_case("GET")
                    && !method.eq_ignore_ascii_case("HEAD")
            {
                write!(writer, "Content-Length: {}\r\n", x)?;
            }
        }

        // Transfer-Encoding header

        if chunked {
            writer.write_all(b"Transfer-Encoding: chunked\r\n")?;
        }

        writer.write_all(b"\r\n")?;

        self.state.body_size = body_size;
        self.state.chunked = chunked;

        Ok(ClientRequestBody { state: self.state })
    }
}

pub enum SendStatus<T, P, E> {
    Complete(T, usize),
    Partial(P, usize),
    Error(P, E),
}

pub enum RecvStatus<T, C> {
    NeedBytes(T),
    Read(T, usize, usize),
    Complete(C, usize, usize),
}

pub struct ClientRequestBody {
    state: ClientState,
}

impl ClientRequestBody {
    pub fn into_early_response(mut self) -> ClientResponse {
        self.state.persistent = false;

        ClientResponse { state: self.state }
    }

    pub fn send<W: Write>(
        mut self,
        writer: &mut W,
        src: &[&[u8]],
        end: bool,
        headers: Option<&[u8]>,
    ) -> SendStatus<ClientResponse, Self, Error> {
        let state = &mut self.state;

        let mut src_len = 0;
        for buf in src.iter() {
            src_len += buf.len();
        }

        if let BodySize::NoBody = state.body_size {
            // ignore the data

            if end {
                return SendStatus::Complete(ClientResponse { state: self.state }, 0);
            }

            return SendStatus::Partial(self, src_len);
        }

        if !state.chunked {
            let size = match write_vectored_offset(writer, src, 0) {
                Ok(ret) => ret,
                Err(e) => return SendStatus::Error(self, e.into()),
            };

            assert!(size <= src_len);

            if end && size == src_len {
                return SendStatus::Complete(ClientResponse { state: self.state }, size);
            }

            return SendStatus::Partial(self, size);
        }

        // chunked

        let mut content_written = 0;

        if src_len > 0 {
            content_written = match write_chunk(
                src,
                CHUNK_FOOTER,
                writer,
                &mut state.sending_chunk,
                CHUNK_SIZE_MAX,
            ) {
                Ok(ret) => ret,
                Err(e) => return SendStatus::Error(self, e.into()),
            };

            assert!(content_written <= src_len);
        }

        // if all content is written then we can send the closing chunk
        if end && content_written == src_len {
            let footer = if let Some(headers) = headers {
                headers
            } else {
                CHUNK_FOOTER
            };

            match write_chunk(
                &[b""],
                footer,
                writer,
                &mut state.sending_chunk,
                CHUNK_SIZE_MAX,
            ) {
                Ok(ret) => ret,
                Err(e) => return SendStatus::Error(self, e.into()),
            };

            if state.sending_chunk.is_none() {
                return SendStatus::Complete(ClientResponse { state: self.state }, content_written);
            }
        }

        SendStatus::Partial(self, content_written)
    }
}

pub struct ClientResponse {
    state: ClientState,
}

impl ClientResponse {
    pub fn recv_header<const N: usize>(
        mut self,
        rbuf: FilledBuf,
        scratch: &mut ParseScratch<N>,
    ) -> ParseStatus<'_, (OwnedResponse<'_, N>, ClientResponseBody), Self, Error, N> {
        let resp = match OwnedHttparseResponse::parse(rbuf, scratch) {
            ParseStatus::Complete(resp) => resp,
            ParseStatus::Incomplete((), rbuf, scratch) => {
                return ParseStatus::Incomplete(self, rbuf, scratch)
            }
            ParseStatus::Error(e, rbuf, scratch) => {
                return ParseStatus::Error(Error::Parse(e), rbuf, scratch)
            }
        };

        if let Err(e) = self.process_response(resp.get()) {
            let (buf, scratch) = resp.into_parts();
            return ParseStatus::Error(e, buf, scratch);
        }

        ParseStatus::Complete((
            OwnedResponse {
                resp,
                body_size: self.state.body_size,
            },
            ClientResponseBody { state: self.state },
        ))
    }

    fn process_response(&mut self, resp: &httparse::Response) -> Result<(), Error> {
        let state = &mut self.state;

        let version = resp.version.unwrap();
        let code = resp.code.unwrap();

        let mut content_len = None;
        let mut chunked = false;
        let mut keep_alive = false;
        let mut close = false;

        for i in 0..resp.headers.len() {
            let h = resp.headers[i];

            if h.name.eq_ignore_ascii_case("Content-Length") {
                let len = parse_as_int(h.value);
                let len = match len {
                    Ok(len) => len,
                    Err(_) => return Err(Error::InvalidContentLength),
                };

                content_len = Some(len);
            } else if h.name.eq_ignore_ascii_case("Transfer-Encoding") {
                if h.value == b"chunked" {
                    chunked = true;
                } else {
                    // unknown transfer encoding
                    return Err(Error::UnsupportedTransferEncoding);
                }
            } else if h.name.eq_ignore_ascii_case("Connection") {
                if !keep_alive && header_contains_param(h.value, b"keep-alive", true) {
                    keep_alive = true;
                }

                if !close && header_contains_param(h.value, b"close", false) {
                    close = true;
                }
            }
        }

        state.ver_min = version;
        state.chunked = false;

        if chunked {
            state.body_size = BodySize::Unknown;
            state.chunked = true;
        } else if let Some(len) = content_len {
            state.body_size = BodySize::Known(len);
            state.chunk_left = Some(len);
        } else {
            state.body_size = match code {
                100..=199 | 204 | 304 => BodySize::NoBody,
                _ => BodySize::Unknown,
            };
        }

        let close_end = state.body_size == BodySize::Unknown && !chunked;

        if version >= 1 {
            state.persistent = !close && !close_end;
        } else {
            state.persistent = keep_alive && !close && !close_end;
        }

        Ok(())
    }
}

pub struct ClientResponseBody {
    state: ClientState,
}

impl ClientResponseBody {
    #[cfg(test)]
    pub fn size(&self) -> BodySize {
        self.state.body_size
    }

    pub fn recv<'buf, const N: usize>(
        self,
        src: &'buf [u8],
        dest: &mut [u8],
        end: bool,
        scratch: &mut mem::MaybeUninit<[httparse::Header<'buf>; N]>,
    ) -> Result<RecvStatus<ClientResponseBody, ClientFinished>, Error> {
        match self.state.body_size {
            BodySize::Known(_) => self.process_known_size(src, dest, end),
            BodySize::Unknown => {
                if self.state.chunked {
                    self.process_unknown_size_chunked(src, dest, end, scratch)
                } else {
                    self.process_unknown_size(src, dest, end)
                }
            }
            BodySize::NoBody => Ok(RecvStatus::Complete(
                ClientFinished {
                    _headers_range: None,
                    persistent: self.state.persistent,
                },
                0,
                0,
            )),
        }
    }

    fn process_known_size(
        mut self,
        src: &[u8],
        dest: &mut [u8],
        end: bool,
    ) -> Result<RecvStatus<ClientResponseBody, ClientFinished>, Error> {
        let state = &mut self.state;

        let mut chunk_left = state.chunk_left.unwrap();
        let max_read = cmp::min(chunk_left, src.len());
        let src = &src[..max_read];

        // src holds body as-is
        let mut rbuf = io::Cursor::new(src);
        let size = rbuf.read(dest)?;

        chunk_left -= size;

        if chunk_left == 0 {
            state.chunk_left = None;

            return Ok(RecvStatus::Complete(
                ClientFinished {
                    _headers_range: None,
                    persistent: state.persistent,
                },
                size,
                size,
            ));
        }

        // we are expecting more bytes

        state.chunk_left = Some(chunk_left);

        // nothing to read?
        if src.is_empty() {
            assert_eq!(size, 0);

            // if the input has ended, return error
            if end {
                return Err(Error::Io(io::Error::from(io::ErrorKind::UnexpectedEof)));
            }

            return Ok(RecvStatus::NeedBytes(self));
        }

        // there was something to read. however, whether anything actually
        // got read depends on the length of dest

        Ok(RecvStatus::Read(self, size, size))
    }

    fn process_unknown_size(
        self,
        src: &[u8],
        dest: &mut [u8],
        end: bool,
    ) -> Result<RecvStatus<ClientResponseBody, ClientFinished>, Error> {
        // src holds body as-is
        let mut rbuf = io::Cursor::new(src);
        let size = rbuf.read(dest)?;

        // we're done when we've consumed the entire input
        if size == src.len() && end {
            return Ok(RecvStatus::Complete(
                ClientFinished {
                    _headers_range: None,
                    persistent: self.state.persistent,
                },
                size,
                size,
            ));
        }

        // nothing to read?
        if src.is_empty() {
            assert_eq!(size, 0);

            return Ok(RecvStatus::NeedBytes(self));
        }

        // there was something to read. however, whether anything actually
        // got read depends on the length of dest

        Ok(RecvStatus::Read(self, size, size))
    }

    fn process_unknown_size_chunked<'buf, const N: usize>(
        mut self,
        src: &'buf [u8],
        dest: &mut [u8],
        end: bool,
        scratch: &mut mem::MaybeUninit<[httparse::Header<'buf>; N]>,
    ) -> Result<RecvStatus<ClientResponseBody, ClientFinished>, Error> {
        let state = &mut self.state;

        let mut pos = if state.chunk_left.is_none() {
            match httparse::parse_chunk_size(src) {
                Ok(httparse::Status::Complete((pos, size))) => {
                    let size = match u32::try_from(size) {
                        Ok(size) => size,
                        Err(_) => return Err(Error::ChunkTooLarge),
                    };

                    let size = size as usize;

                    state.chunk_left = Some(size);
                    state.chunk_size = size;

                    pos
                }
                Ok(httparse::Status::Partial) => {
                    if end {
                        return Err(Error::Io(io::Error::from(io::ErrorKind::UnexpectedEof)));
                    }

                    return Ok(RecvStatus::NeedBytes(self));
                }
                Err(_) => {
                    return Err(Error::InvalidChunkSize);
                }
            }
        } else {
            0
        };

        let mut chunk_left = state.chunk_left.unwrap();

        if chunk_left > 0 {
            let max_read = cmp::min(chunk_left, src.len() - pos);
            let src = &src[pos..(pos + max_read)];

            let mut rbuf = io::Cursor::new(src);
            let size = rbuf.read(dest)?;

            pos += size;
            chunk_left -= size;

            state.chunk_left = Some(chunk_left);

            // nothing to read?
            if src.is_empty() {
                assert_eq!(size, 0);

                if end {
                    return Err(Error::Io(io::Error::from(io::ErrorKind::UnexpectedEof)));
                }

                // if pos advanced we need to return it
                if pos > 0 {
                    return Ok(RecvStatus::Read(self, pos, 0));
                }

                return Ok(RecvStatus::NeedBytes(self));
            }

            // there was something to read. however, whether anything actually
            // got read depends on the length of dest

            return Ok(RecvStatus::Read(self, pos, size));
        }

        // done with content bytes. now to read the footer

        // final chunk?
        if state.chunk_size == 0 {
            let src = &src[pos..];

            // trailing headers
            let scratch = unsafe { scratch.assume_init_mut() };
            match httparse::parse_headers(src, scratch) {
                Ok(httparse::Status::Complete((x, _))) => {
                    let headers_start = pos;
                    let headers_end = pos + x;

                    return Ok(RecvStatus::Complete(
                        ClientFinished {
                            _headers_range: Some((headers_start, headers_end)),
                            persistent: state.persistent,
                        },
                        headers_end,
                        0,
                    ));
                }
                Ok(httparse::Status::Partial) => {
                    if end {
                        return Err(Error::Io(io::Error::from(io::ErrorKind::UnexpectedEof)));
                    }

                    // if pos advanced we need to return it
                    if pos > 0 {
                        return Ok(RecvStatus::Read(self, pos, 0));
                    }

                    return Ok(RecvStatus::NeedBytes(self));
                }
                Err(e) => return Err(Error::Parse(e)),
            }
        }

        // for chunks of non-zero size, pos for header/content will have
        // already been returned by previous calls
        assert_eq!(pos, 0);

        if src.len() < 2 {
            if end {
                return Err(Error::Io(io::Error::from(io::ErrorKind::UnexpectedEof)));
            }

            return Ok(RecvStatus::NeedBytes(self));
        }

        if &src[..2] != b"\r\n" {
            return Err(Error::InvalidChunkSuffix);
        }

        state.chunk_left = None;
        state.chunk_size = 0;

        Ok(RecvStatus::Read(self, 2, 0))
    }
}

pub struct ClientFinished {
    _headers_range: Option<(usize, usize)>,
    pub persistent: bool,
}

#[cfg(test)]
mod tests {
    use super::*;

    const HEADERS_MAX: usize = 32;

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
        pub code: u16,
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

        let rbuf = FilledBuf::new(src.to_vec(), src.len());

        let mut result = TestRequest::new();

        assert_eq!(p.state(), ServerState::ReceivingRequest);

        let mut scratch = ParseScratch::<HEADERS_MAX>::new();

        let req = match p.recv_request_owned(rbuf, &mut scratch) {
            ParseStatus::Complete(req) => req,
            _ => panic!("recv_request_owned did not return complete"),
        };

        let mut rbuf = io::Cursor::new(req.remaining_bytes());

        let req = req.get();

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
            let mut headers = [httparse::EMPTY_HEADER; HEADERS_MAX];

            let (size, trailing_headers) = p
                .recv_body(&mut rbuf, &mut buf[..read_size], &mut headers)
                .unwrap()
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
                    Err(Error::Io(e)) if e.kind() == io::ErrorKind::WriteZero => 0,
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
    fn test_recv_request_header() {
        struct Test<'buf, 'headers> {
            name: &'static str,
            data: &'buf str,
            result: Option<Result<Request<'buf, 'headers>, Error>>,
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
                result: Some(Err(Error::Parse(httparse::Error::Token))),
                state: ServerState::ReceivingRequest,
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                rbuf_position: 0,
            },
            Test {
                name: "invalid-content-length",
                data: "GET / HTTP/1.0\r\nContent-Length: a\r\n\r\n",
                result: Some(Err(Error::InvalidContentLength)),
                state: ServerState::ReceivingRequest,
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                rbuf_position: 0,
            },
            Test {
                name: "unsupported-transfer-encoding",
                data: "GET / HTTP/1.0\r\nTransfer-Encoding: bogus\r\n\r\n",
                result: Some(Err(Error::UnsupportedTransferEncoding)),
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

        for test in tests.iter() {
            let mut p = ServerProtocol::new();

            let src = test.data.as_bytes();
            let rbuf = FilledBuf::new(src.to_vec(), src.len());
            let mut scratch = ParseScratch::<HEADERS_MAX>::new();

            let mut rbuf_position = 0;

            let r = p.recv_request_owned(rbuf, &mut scratch);

            match r {
                ParseStatus::Complete(req) => {
                    let expected = match &test.result {
                        Some(Ok(req)) => req,
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(req.get(), *expected, "test={}", test.name);

                    rbuf_position = (src.len() - req.remaining_bytes().len()) as u64
                }
                ParseStatus::Incomplete(_, _, _) => {
                    assert!(test.result.is_none(), "test={}", test.name);
                }
                ParseStatus::Error(e, _, _) => {
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
            assert_eq!(rbuf_position, test.rbuf_position, "test={}", test.name);
        }
    }

    #[test]
    fn test_recv_request_body() {
        struct Test<'buf, 'headers> {
            name: &'static str,
            data: &'buf str,
            body_size: BodySize,
            chunk_left: Option<usize>,
            chunk_size: usize,
            result: Result<Option<(usize, Option<&'headers [httparse::Header<'buf>]>)>, Error>,
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
                result: Ok(Some((3, None))),
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
                result: Ok(Some((5, None))),
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
                result: Ok(None),
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
                result: Err(Error::InvalidChunkSize),
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
                result: Err(Error::ChunkTooLarge),
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
                result: Ok(None),
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
                result: Ok(Some((3, None))),
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
                result: Ok(Some((5, None))),
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
                result: Ok(Some((5, None))),
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
                result: Ok(None),
                state: ServerState::ReceivingBody,
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "chunked-footer-parse-error",
                data: "XX",
                body_size: BodySize::Unknown,
                chunk_left: Some(0),
                chunk_size: 5,
                result: Err(Error::InvalidChunkSuffix),
                state: ServerState::ReceivingBody,
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "chunked-complete-full",
                data: "5\r\nhello\r\n",
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                result: Ok(Some((5, None))),
                state: ServerState::ReceivingBody,
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 8,
                dest_data: "hello",
            },
            Test {
                name: "chunked-complete-mid",
                data: "lo\r\n",
                body_size: BodySize::Unknown,
                chunk_left: Some(2),
                chunk_size: 5,
                result: Ok(Some((2, None))),
                state: ServerState::ReceivingBody,
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 2,
                dest_data: "lo",
            },
            Test {
                name: "chunked-complete-end",
                data: "\r\n",
                body_size: BodySize::Unknown,
                chunk_left: Some(0),
                chunk_size: 5,
                result: Ok(Some((0, None))),
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
                result: Ok(Some((0, Some(&[])))),
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
                result: Ok(None),
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
                result: Err(Error::Parse(httparse::Error::Token)),
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
                result: Ok(Some((
                    0,
                    Some(&[httparse::Header {
                        name: "Foo",
                        value: b"Bar",
                    }]),
                ))),
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
                Ok(Some((size, headers))) => {
                    let (expected_size, expected_headers) = match &test.result {
                        Ok(Some((size, headers))) => (size, headers),
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(size, *expected_size, "test={}", test.name);
                    assert_eq!(headers, *expected_headers, "test={}", test.name);

                    dest_size = size;
                }
                Ok(None) => match &test.result {
                    Ok(None) => {}
                    _ => panic!("result mismatch: test={}", test.name),
                },
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
    fn test_send_response_header() {
        struct Test<'buf, 'headers> {
            name: &'static str,
            write_space: usize,
            code: u16,
            reason: &'static str,
            headers: &'headers [Header<'buf>],
            body_size: BodySize,
            ver_min: u8,
            persistent: bool,
            result: Result<(), Error>,
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
    fn test_send_response_body() {
        struct Test {
            name: &'static str,
            write_space: usize,
            src: &'static str,
            end: bool,
            headers: Option<&'static [u8]>,
            body_size: BodySize,
            chunked: bool,
            sending_chunk: Option<Chunk>,
            result: Result<usize, Error>,
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
    fn test_server_req() {
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
    fn test_server_resp() {
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
    fn test_server_persistent() {
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

    #[test]
    fn test_send_request_header() {
        struct Test<'buf, 'headers> {
            name: &'static str,
            write_space: usize,
            method: &'static str,
            uri: &'static str,
            headers: &'headers [Header<'buf>],
            body_size: BodySize,
            websocket: bool,
            result: Result<(), Error>,
            body_size_after: BodySize,
            chunked: bool,
            written: &'static str,
        }

        let tests = [
            Test {
                name: "cant-write",
                write_space: 2,
                method: "GET",
                uri: "/foo",
                headers: &[],
                body_size: BodySize::Known(0),
                websocket: false,
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
                body_size_after: BodySize::Known(0),
                chunked: false,
                written: "",
            },
            Test {
                name: "cant-write-request-line",
                write_space: 12,
                method: "GET",
                uri: "/foo",
                headers: &[],
                body_size: BodySize::Known(0),
                websocket: false,
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
                body_size_after: BodySize::Known(0),
                chunked: false,
                written: "GET /foo",
            },
            Test {
                name: "cant-write-header-name",
                write_space: 22,
                method: "GET",
                uri: "/foo",
                headers: &[Header {
                    name: "Foo",
                    value: b"Bar",
                }],
                body_size: BodySize::Known(0),
                websocket: false,
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
                body_size_after: BodySize::Known(0),
                chunked: false,
                written: "GET /foo HTTP/1.1\r\nFoo",
            },
            Test {
                name: "cant-write-header-value",
                write_space: 26,
                method: "GET",
                uri: "/foo",
                headers: &[Header {
                    name: "Foo",
                    value: b"Bar",
                }],
                body_size: BodySize::Known(0),
                websocket: false,
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
                body_size_after: BodySize::Known(0),
                chunked: false,
                written: "GET /foo HTTP/1.1\r\nFoo: ",
            },
            Test {
                name: "cant-write-header-eol",
                write_space: 28,
                method: "GET",
                uri: "/foo",
                headers: &[Header {
                    name: "Foo",
                    value: b"Bar",
                }],
                body_size: BodySize::Known(0),
                websocket: false,
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
                body_size_after: BodySize::Known(0),
                chunked: false,
                written: "GET /foo HTTP/1.1\r\nFoo: Bar",
            },
            Test {
                name: "cant-write-transfer-encoding",
                write_space: 27,
                method: "POST",
                uri: "/foo",
                headers: &[],
                body_size: BodySize::Unknown,
                websocket: false,
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
                body_size_after: BodySize::Unknown,
                chunked: false,
                written: "POST /foo HTTP/1.1\r\n",
            },
            Test {
                name: "cant-write-content-length",
                write_space: 27,
                method: "POST",
                uri: "/foo",
                headers: &[],
                body_size: BodySize::Known(0),
                websocket: false,
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
                body_size_after: BodySize::Known(0),
                chunked: false,
                written: "POST /foo HTTP/1.1\r\n",
            },
            Test {
                name: "cant-write-eol",
                write_space: 20,
                method: "POST",
                uri: "/foo",
                headers: &[],
                body_size: BodySize::Unknown,
                websocket: false,
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
                body_size_after: BodySize::Unknown,
                chunked: false,
                written: "POST /foo HTTP/1.1\r\n",
            },
            Test {
                name: "exclude-headers",
                write_space: 1024,
                method: "POST",
                uri: "/foo",
                headers: &[
                    Header {
                        name: "Connection",
                        value: b"X",
                    },
                    Header {
                        name: "Foo",
                        value: b"Bar",
                    },
                    Header {
                        name: "Content-Length",
                        value: b"X",
                    },
                    Header {
                        name: "Transfer-Encoding",
                        value: b"X",
                    },
                ],
                body_size: BodySize::Known(0),
                websocket: false,
                result: Ok(()),
                body_size_after: BodySize::Known(0),
                chunked: false,
                written: "POST /foo HTTP/1.1\r\nFoo: Bar\r\nContent-Length: 0\r\n\r\n",
            },
            Test {
                name: "no-body",
                write_space: 1024,
                method: "GET",
                uri: "/foo",
                headers: &[],
                body_size: BodySize::NoBody,
                websocket: false,
                result: Ok(()),
                body_size_after: BodySize::NoBody,
                chunked: false,
                written: "GET /foo HTTP/1.1\r\n\r\n",
            },
            Test {
                name: "len",
                write_space: 1024,
                method: "POST",
                uri: "/foo",
                headers: &[],
                body_size: BodySize::Known(42),
                websocket: false,
                result: Ok(()),
                body_size_after: BodySize::Known(42),
                chunked: false,
                written: "POST /foo HTTP/1.1\r\nContent-Length: 42\r\n\r\n",
            },
            Test {
                name: "no-len",
                write_space: 1024,
                method: "POST",
                uri: "/foo",
                headers: &[],
                body_size: BodySize::Unknown,
                websocket: false,
                result: Ok(()),
                body_size_after: BodySize::Unknown,
                chunked: true,
                written: "POST /foo HTTP/1.1\r\nConnection: Transfer-Encoding\r\nTransfer-Encoding: chunked\r\n\r\n",
            },
            Test {
                name: "force-no-body",
                write_space: 1024,
                method: "GET",
                uri: "/foo",
                headers: &[],
                body_size: BodySize::Known(42),
                websocket: true,
                result: Ok(()),
                body_size_after: BodySize::NoBody,
                chunked: false,
                written: "GET /foo HTTP/1.1\r\n\r\n",
            },
        ];

        for test in tests.iter() {
            let req = ClientRequest::new();

            let mut w = MyBuffer::new(test.write_space, false);

            let r = req.send_header(
                &mut w,
                test.method,
                test.uri,
                test.headers,
                test.body_size,
                test.websocket,
            );

            match r {
                Ok(req_body) => {
                    match &test.result {
                        Ok(_) => {}
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(
                        req_body.state.body_size, test.body_size_after,
                        "test={}",
                        test.name
                    );
                    assert_eq!(req_body.state.chunked, test.chunked, "test={}", test.name);
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

            assert_eq!(
                str::from_utf8(&w.data).unwrap(),
                test.written,
                "test={}",
                test.name
            );
        }
    }

    #[test]
    fn test_send_request_body() {
        struct Test {
            name: &'static str,
            write_space: usize,
            src: &'static str,
            end: bool,
            headers: Option<&'static [u8]>,
            body_size: BodySize,
            chunked: bool,
            sending_chunk: Option<Chunk>,
            result: Result<(bool, usize), Error>,
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
                result: Ok((false, 5)),
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
                result: Ok((true, 0)),
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
                result: Ok((false, 3)),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Ok((false, 5)),
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
                result: Ok((true, 0)),
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
                result: Ok((false, 0)),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Ok((false, 5)),
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
                result: Ok((false, 0)),
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
                result: Err(Error::Io(io::Error::from(io::ErrorKind::WriteZero))),
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
                result: Ok((true, 0)),
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
                result: Ok((true, 0)),
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
                result: Ok((true, 5)),
                sending_chunk_after: None,
                written: "5\r\nhello\r\n0\r\n\r\n",
            },
        ];

        for test in tests.iter() {
            let req_body = ClientRequestBody {
                state: ClientState {
                    ver_min: 0,
                    body_size: test.body_size,
                    chunk_left: None,
                    chunk_size: 0,
                    persistent: false,
                    chunked: test.chunked,
                    sending_chunk: test.sending_chunk,
                },
            };

            let mut w = MyBuffer::new(test.write_space, true);

            let (state, r) =
                match req_body.send(&mut w, &[test.src.as_bytes()], test.end, test.headers) {
                    SendStatus::Complete(resp, size) => (resp.state, Ok((true, size))),
                    SendStatus::Partial(req_body, size) => (req_body.state, Ok((false, size))),
                    SendStatus::Error(req_body, e) => (req_body.state, Err(e)),
                };

            match r {
                Ok((done, size)) => {
                    let (expected_done, expected_size) = match &test.result {
                        Ok(v) => v,
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(done, *expected_done, "test={}", test.name);
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

            assert_eq!(
                state.sending_chunk, test.sending_chunk_after,
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
    fn test_recv_response_header() {
        struct Test<'buf, 'headers> {
            name: &'static str,
            data: &'buf str,
            result: Option<Result<Response<'buf, 'headers>, Error>>,
            ver_min: u8,
            chunk_left: Option<usize>,
            persistent: bool,
            chunked: bool,
            rbuf_position: u64,
        }

        let tests = [
            Test {
                name: "partial",
                data: "H",
                result: None,
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                chunked: false,
                rbuf_position: 0,
            },
            Test {
                name: "parse-error",
                data: "H\n",
                result: Some(Err(Error::Parse(httparse::Error::Token))),
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                chunked: false,
                rbuf_position: 0,
            },
            Test {
                name: "invalid-content-length",
                data: "HTTP/1.0 200 OK\r\nContent-Length: a\r\n\r\n",
                result: Some(Err(Error::InvalidContentLength)),
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                chunked: false,
                rbuf_position: 0,
            },
            Test {
                name: "unsupported-transfer-encoding",
                data: "HTTP/1.0 200 OK\r\nTransfer-Encoding: bogus\r\n\r\n",
                result: Some(Err(Error::UnsupportedTransferEncoding)),
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                chunked: false,
                rbuf_position: 0,
            },
            Test {
                name: "no-body",
                data: "HTTP/1.0 204 No Content\r\nFoo: Bar\r\n\r\n",
                result: Some(Ok(Response {
                    code: 204,
                    reason: "No Content",
                    headers: &[httparse::Header {
                        name: "Foo",
                        value: b"Bar",
                    }],
                    body_size: BodySize::NoBody,
                })),
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                chunked: false,
                rbuf_position: 37,
            },
            Test {
                name: "body-size-known",
                data: "HTTP/1.0 200 OK\r\nContent-Length: 42\r\n\r\n",
                result: Some(Ok(Response {
                    code: 200,
                    reason: "OK",
                    headers: &[httparse::Header {
                        name: "Content-Length",
                        value: b"42",
                    }],
                    body_size: BodySize::Known(42),
                })),
                ver_min: 0,
                chunk_left: Some(42),
                persistent: false,
                chunked: false,
                rbuf_position: 39,
            },
            Test {
                name: "body-size-unknown",
                data: "HTTP/1.0 200 OK\r\n\r\n",
                result: Some(Ok(Response {
                    code: 200,
                    reason: "OK",
                    headers: &[],
                    body_size: BodySize::Unknown,
                })),
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                chunked: false,
                rbuf_position: 19,
            },
            Test {
                name: "body-size-unknown-chunked",
                data: "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
                result: Some(Ok(Response {
                    code: 200,
                    reason: "OK",
                    headers: &[httparse::Header {
                        name: "Transfer-Encoding",
                        value: b"chunked",
                    }],
                    body_size: BodySize::Unknown,
                })),
                ver_min: 0,
                chunk_left: None,
                persistent: false,
                chunked: true,
                rbuf_position: 47,
            },
            Test {
                name: "1.0-persistent",
                data: "HTTP/1.0 200 OK\r\nContent-Length: 5\r\nConnection: keep-alive\r\n\r\n",
                result: Some(Ok(Response {
                    code: 200,
                    reason: "OK",
                    headers: &[
                        httparse::Header {
                            name: "Content-Length",
                            value: b"5",
                        },
                        httparse::Header {
                            name: "Connection",
                            value: b"keep-alive",
                        },
                    ],
                    body_size: BodySize::Known(5),
                })),
                ver_min: 0,
                chunk_left: Some(5),
                persistent: true,
                chunked: false,
                rbuf_position: 62,
            },
            Test {
                name: "1.1-persistent",
                data: "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n",
                result: Some(Ok(Response {
                    code: 200,
                    reason: "OK",
                    headers: &[httparse::Header {
                        name: "Content-Length",
                        value: b"5",
                    }],
                    body_size: BodySize::Known(5),
                })),
                ver_min: 1,
                chunk_left: Some(5),
                persistent: true,
                chunked: false,
                rbuf_position: 38,
            },
            Test {
                name: "1.1-non-persistent",
                data: "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n",
                result: Some(Ok(Response {
                    code: 200,
                    reason: "OK",
                    headers: &[httparse::Header {
                        name: "Connection",
                        value: b"close",
                    }],
                    body_size: BodySize::Unknown,
                })),
                ver_min: 1,
                chunk_left: None,
                persistent: false,
                chunked: false,
                rbuf_position: 38,
            },
        ];

        for test in tests.iter() {
            let resp = ClientResponse {
                state: ClientState::new(),
            };

            let src = test.data.as_bytes();
            let rbuf = FilledBuf::new(src.to_vec(), src.len());
            let mut scratch = ParseScratch::<HEADERS_MAX>::new();

            let mut rbuf_position = 0;

            let r = resp.recv_header(rbuf, &mut scratch);

            match r {
                ParseStatus::Complete((resp, resp_body)) => {
                    let expected = match &test.result {
                        Some(Ok(req)) => req,
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(resp.get(), *expected, "test={}", test.name);

                    assert_eq!(resp_body.state.ver_min, test.ver_min, "test={}", test.name);
                    assert_eq!(
                        resp_body.state.chunk_left, test.chunk_left,
                        "test={}",
                        test.name
                    );
                    assert_eq!(
                        resp_body.state.persistent, test.persistent,
                        "test={}",
                        test.name
                    );
                    assert_eq!(resp_body.state.chunked, test.chunked, "test={}", test.name);

                    rbuf_position = (src.len() - resp.remaining_bytes().len()) as u64
                }
                ParseStatus::Incomplete(_, _, _) => {
                    assert!(test.result.is_none(), "test={}", test.name);
                }
                ParseStatus::Error(e, _, _) => {
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

            assert_eq!(rbuf_position, test.rbuf_position, "test={}", test.name);
        }
    }

    #[test]
    fn test_recv_response_body() {
        enum Status {
            NeedBytes,
            Read,
            Complete,
        }

        struct Test<'buf, 'headers> {
            name: &'static str,
            data: &'buf str,
            end: bool,
            body_size: BodySize,
            chunk_left: Option<usize>,
            chunk_size: usize,
            chunked: bool,
            result: Result<(Status, usize, Option<&'headers [httparse::Header<'buf>]>), Error>,
            chunk_left_after: Option<usize>,
            chunk_size_after: usize,
            rbuf_position: u64,
            dest_data: &'static str,
        }

        let tests = [
            Test {
                name: "known-partial",
                data: "hel",
                end: false,
                body_size: BodySize::Known(5),
                chunk_left: Some(5),
                chunk_size: 0,
                chunked: false,
                result: Ok((Status::Read, 3, None)),
                chunk_left_after: Some(2),
                chunk_size_after: 0,
                rbuf_position: 3,
                dest_data: "hel",
            },
            Test {
                name: "known-partial-no-data",
                data: "",
                end: false,
                body_size: BodySize::Known(5),
                chunk_left: Some(5),
                chunk_size: 0,
                chunked: false,
                result: Ok((Status::NeedBytes, 0, None)),
                chunk_left_after: Some(5),
                chunk_size_after: 0,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "known-partial-end",
                data: "hel",
                end: true,
                body_size: BodySize::Known(5),
                chunk_left: Some(5),
                chunk_size: 0,
                chunked: false,
                result: Ok((Status::Read, 3, None)),
                chunk_left_after: Some(2),
                chunk_size_after: 0,
                rbuf_position: 3,
                dest_data: "hel",
            },
            Test {
                name: "known-partial-end-no-data",
                data: "",
                end: true,
                body_size: BodySize::Known(5),
                chunk_left: Some(5),
                chunk_size: 0,
                chunked: false,
                result: Err(io::Error::from(io::ErrorKind::UnexpectedEof).into()),
                chunk_left_after: Some(5),
                chunk_size_after: 0,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "known-complete",
                data: "hello",
                end: false,
                body_size: BodySize::Known(5),
                chunk_left: Some(5),
                chunk_size: 0,
                chunked: false,
                result: Ok((Status::Complete, 5, None)),
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 5,
                dest_data: "hello",
            },
            Test {
                name: "unknown-partial",
                data: "hel",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: false,
                result: Ok((Status::Read, 3, None)),
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 3,
                dest_data: "hel",
            },
            Test {
                name: "unknown-partial-no-data",
                data: "",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: false,
                result: Ok((Status::NeedBytes, 0, None)),
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "unknown-complete",
                data: "hello",
                end: true,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: false,
                result: Ok((Status::Complete, 5, None)),
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 5,
                dest_data: "hello",
            },
            Test {
                name: "chunked-header-partial",
                data: "5",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Ok((Status::NeedBytes, 0, None)),
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "chunked-header-partial-end",
                data: "5",
                end: true,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Err(io::Error::from(io::ErrorKind::UnexpectedEof).into()),
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "chunked-header-parse-error",
                data: "z",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Err(Error::InvalidChunkSize),
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "chunked-too-large",
                data: "ffffffffff\r\n",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Err(Error::ChunkTooLarge),
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "chunked-header-ok",
                data: "5\r\n",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Ok((Status::Read, 0, None)),
                chunk_left_after: Some(5),
                chunk_size_after: 5,
                rbuf_position: 3,
                dest_data: "",
            },
            Test {
                name: "chunked-content-partial",
                data: "5\r\nhel",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Ok((Status::Read, 3, None)),
                chunk_left_after: Some(2),
                chunk_size_after: 5,
                rbuf_position: 6,
                dest_data: "hel",
            },
            Test {
                name: "chunked-content-partial-no-data",
                data: "5\r\n",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Ok((Status::Read, 0, None)),
                chunk_left_after: Some(5),
                chunk_size_after: 5,
                rbuf_position: 3,
                dest_data: "",
            },
            Test {
                name: "chunked-content-partial-end",
                data: "5\r\nhel",
                end: true,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Ok((Status::Read, 3, None)),
                chunk_left_after: Some(2),
                chunk_size_after: 5,
                rbuf_position: 6,
                dest_data: "hel",
            },
            Test {
                name: "chunked-content-partial-end-no-data",
                data: "5\r\n",
                end: true,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Err(io::Error::from(io::ErrorKind::UnexpectedEof).into()),
                chunk_left_after: Some(5),
                chunk_size_after: 5,
                rbuf_position: 3,
                dest_data: "",
            },
            Test {
                name: "chunked-content-mid-no-data",
                data: "",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: Some(5),
                chunk_size: 5,
                chunked: true,
                result: Ok((Status::NeedBytes, 0, None)),
                chunk_left_after: Some(5),
                chunk_size_after: 5,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "chunked-footer-partial-full-none",
                data: "5\r\nhello",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Ok((Status::Read, 5, None)),
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 8,
                dest_data: "hello",
            },
            Test {
                name: "chunked-footer-partial-full-none-end",
                data: "5\r\nhello",
                end: true,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Ok((Status::Read, 5, None)),
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 8,
                dest_data: "hello",
            },
            Test {
                name: "chunked-footer-partial-full-r",
                data: "5\r\nhello\r",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Ok((Status::Read, 5, None)),
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 8,
                dest_data: "hello",
            },
            Test {
                name: "chunked-footer-partial-mid-r",
                data: "\r",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: Some(0),
                chunk_size: 5,
                chunked: true,
                result: Ok((Status::NeedBytes, 0, None)),
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "chunked-footer-partial-mid-r-last",
                data: "\r",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: Some(0),
                chunk_size: 0,
                chunked: true,
                result: Ok((Status::NeedBytes, 0, None)),
                chunk_left_after: Some(0),
                chunk_size_after: 0,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "chunked-footer-parse-error",
                data: "XX",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: Some(0),
                chunk_size: 5,
                chunked: true,
                result: Err(Error::InvalidChunkSuffix),
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 0,
                dest_data: "",
            },
            Test {
                name: "chunked-complete-full",
                data: "5\r\nhello\r\n",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Ok((Status::Read, 5, None)),
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 8,
                dest_data: "hello",
            },
            Test {
                name: "chunked-complete-mid",
                data: "lo\r\n",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: Some(2),
                chunk_size: 5,
                chunked: true,
                result: Ok((Status::Read, 2, None)),
                chunk_left_after: Some(0),
                chunk_size_after: 5,
                rbuf_position: 2,
                dest_data: "lo",
            },
            Test {
                name: "chunked-complete-end",
                data: "\r\n",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: Some(0),
                chunk_size: 5,
                chunked: true,
                result: Ok((Status::Read, 0, None)),
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 2,
                dest_data: "",
            },
            Test {
                name: "chunked-empty",
                data: "0\r\n\r\n",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Ok((Status::Complete, 0, Some(&[]))),
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 5,
                dest_data: "",
            },
            Test {
                name: "trailing-headers-partial",
                data: "0\r\nhelloXX",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Ok((Status::Read, 0, None)),
                chunk_left_after: Some(0),
                chunk_size_after: 0,
                rbuf_position: 3,
                dest_data: "",
            },
            Test {
                name: "trailing-headers-partial-end",
                data: "0\r\nhelloXX",
                end: true,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Err(io::Error::from(io::ErrorKind::UnexpectedEof).into()),
                chunk_left_after: Some(0),
                chunk_size_after: 0,
                rbuf_position: 3,
                dest_data: "",
            },
            Test {
                name: "trailing-headers-parse-error",
                data: "0\r\nhelloXX\n",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Err(Error::Parse(httparse::Error::Token)),
                chunk_left_after: Some(0),
                chunk_size_after: 0,
                rbuf_position: 3,
                dest_data: "",
            },
            Test {
                name: "trailing-headers-complete",
                data: "0\r\nFoo: Bar\r\n\r\n",
                end: false,
                body_size: BodySize::Unknown,
                chunk_left: None,
                chunk_size: 0,
                chunked: true,
                result: Ok((
                    Status::Complete,
                    0,
                    Some(&[httparse::Header {
                        name: "Foo",
                        value: b"Bar",
                    }]),
                )),
                chunk_left_after: None,
                chunk_size_after: 0,
                rbuf_position: 15,
                dest_data: "",
            },
        ];

        for test in tests.iter() {
            let resp_body = ClientResponseBody {
                state: ClientState {
                    ver_min: 0,
                    body_size: test.body_size,
                    chunk_left: test.chunk_left,
                    chunk_size: test.chunk_size,
                    persistent: false,
                    chunked: test.chunked,
                    sending_chunk: None,
                },
            };

            let mut dest = [0; 1024];
            let mut scratch = mem::MaybeUninit::<[httparse::Header; HEADERS_MAX]>::uninit();

            let r = resp_body.recv(test.data.as_bytes(), &mut dest, test.end, &mut scratch);

            let (r, headers) = match r {
                Ok(RecvStatus::Complete(finished, read, written)) => {
                    if let Some((start, end)) = finished._headers_range {
                        let headers_data = &test.data.as_bytes()[start..end];
                        let scratch = unsafe { scratch.assume_init_mut() };

                        match httparse::parse_headers(headers_data, scratch) {
                            Ok(httparse::Status::Complete((pos, headers))) => {
                                assert_eq!(pos, end - start);

                                (
                                    Ok(RecvStatus::Complete(finished, read, written)),
                                    Some(headers),
                                )
                            }
                            Ok(httparse::Status::Partial) => panic!("unexpected partial parse"),
                            Err(e) => (Err(e.into()), None),
                        }
                    } else {
                        (Ok(RecvStatus::Complete(finished, read, written)), None)
                    }
                }
                r => (r, None),
            };

            match r {
                Ok(RecvStatus::NeedBytes(resp_body)) => {
                    match &test.result {
                        Ok((Status::NeedBytes, _, _)) => {}
                        _ => panic!("result mismatch: test={}", test.name),
                    }

                    let state = resp_body.state;

                    assert_eq!(
                        state.chunk_left, test.chunk_left_after,
                        "test={}",
                        test.name
                    );
                    assert_eq!(
                        state.chunk_size, test.chunk_size_after,
                        "test={}",
                        test.name
                    );
                }
                Ok(RecvStatus::Complete(_, read, written)) => {
                    let (expected_size, expected_headers) = match &test.result {
                        Ok((Status::Complete, size, headers)) => (size, headers),
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(written, *expected_size, "test={}", test.name);
                    assert_eq!(read as u64, test.rbuf_position, "test={}", test.name);
                    assert_eq!(headers, *expected_headers, "test={}", test.name);
                }
                Ok(RecvStatus::Read(resp_body, read, written)) => {
                    let expected_size = match &test.result {
                        Ok((Status::Read, size, _)) => size,
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    let state = resp_body.state;

                    assert_eq!(written, *expected_size, "test={}", test.name);
                    assert_eq!(
                        state.chunk_left, test.chunk_left_after,
                        "test={}",
                        test.name
                    );
                    assert_eq!(
                        state.chunk_size, test.chunk_size_after,
                        "test={}",
                        test.name
                    );
                    assert_eq!(read as u64, test.rbuf_position, "test={}", test.name);

                    assert_eq!(
                        str::from_utf8(&dest[..written]).unwrap(),
                        test.dest_data,
                        "test={}",
                        test.name
                    );
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
        }
    }

    #[test]
    fn test_client_flow() {
        let req = ClientRequest::new();

        let mut out = MyBuffer::new(1024, true);

        let req_body = req
            .send_header(
                &mut out,
                "GET",
                "/foo",
                &[Header {
                    name: "Host",
                    value: b"example.com",
                }],
                BodySize::NoBody,
                false,
            )
            .unwrap();

        let expected = "GET /foo HTTP/1.1\r\nHost: example.com\r\n\r\n";

        assert_eq!(str::from_utf8(&out.data).unwrap(), expected);

        out.data.clear();

        let resp = match req_body.send(&mut out, &[], true, None) {
            SendStatus::Complete(resp, read) => {
                assert_eq!(read, 0);

                resp
            }
            _ => panic!("unexpected status"),
        };

        assert_eq!(out.data.is_empty(), true);

        let mut buf = Vec::new();

        let data = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: text/plain\r\n",
            "Content-Length: 6\r\n",
            "\r\n",
            "hello\n",
        );

        let size = buf.write(data.as_bytes()).unwrap();
        buf.resize(1024, 0);

        let buf = FilledBuf::new(buf, size);
        let mut scratch = ParseScratch::<HEADERS_MAX>::new();

        let (resp, resp_body) = match resp.recv_header(buf, &mut scratch) {
            ParseStatus::Complete(ret) => ret,
            _ => panic!("unexpected status"),
        };

        {
            let resp = resp.get();

            assert_eq!(resp.code, 200);
            assert_eq!(resp.headers.len(), 2);
            assert_eq!(resp.headers[0].name, "Content-Type");
            assert_eq!(resp.headers[0].value, b"text/plain");
            assert_eq!(resp.headers[1].name, "Content-Length");
            assert_eq!(resp.headers[1].value, b"6");

            assert_eq!(resp_body.size(), BodySize::Known(6));
        }

        let mut out = [0; 1024];
        let mut scratch = mem::MaybeUninit::<[httparse::Header; HEADERS_MAX]>::uninit();

        let finished = match resp_body
            .recv(resp.remaining_bytes(), &mut out, false, &mut scratch)
            .unwrap()
        {
            RecvStatus::Complete(finished, read, written) => {
                assert_eq!(read, 6);
                assert_eq!(written, 6);

                finished
            }
            _ => panic!("unexpected status"),
        };

        assert_eq!(finished._headers_range, None);
        assert_eq!(finished.persistent, true);
    }

    fn collect_values<'a>(
        s: &'a [u8],
    ) -> Result<Vec<(&'a str, Vec<(&'a str, &'a str)>)>, io::Error> {
        let mut out = Vec::new();

        for part in parse_header_value(s) {
            let (name, params_iter) = part?;

            let mut params = Vec::new();
            for p in params_iter {
                let (k, v) = p?;
                params.push((k, v));
            }

            out.push((name, params));
        }

        Ok(out)
    }

    #[test]
    fn test_parse_header_value() {
        struct Test {
            name: &'static str,
            value: &'static str,
            result: Result<Vec<(&'static str, Vec<(&'static str, &'static str)>)>, io::Error>,
        }

        let tests = [
            Test {
                name: "empty",
                value: "",
                result: Err(io::Error::from(io::ErrorKind::InvalidData)),
            },
            Test {
                name: "value",
                value: "apple",
                result: Ok(vec![("apple", vec![])]),
            },
            Test {
                name: "incomplete-value",
                value: "apple,",
                result: Err(io::Error::from(io::ErrorKind::InvalidData)),
            },
            Test {
                name: "incomplete-param",
                value: "apple;",
                result: Err(io::Error::from(io::ErrorKind::InvalidData)),
            },
            Test {
                name: "incomplete-second-param",
                value: "apple; type=gala;",
                result: Err(io::Error::from(io::ErrorKind::InvalidData)),
            },
            Test {
                name: "value-with-param",
                value: "apple; type=gala",
                result: Ok(vec![("apple", vec![("type", "gala")])]),
            },
            Test {
                name: "value-with-params",
                value: "apple; type=\"granny smith\"; color=green",
                result: Ok(vec![(
                    "apple",
                    vec![("type", "granny smith"), ("color", "green")],
                )]),
            },
            Test {
                name: "values",
                value: "apple, banana, cherry",
                result: Ok(vec![
                    ("apple", vec![]),
                    ("banana", vec![]),
                    ("cherry", vec![]),
                ]),
            },
            Test {
                name: "values-and-params",
                value: "apple, banana; color=yellow; ripe=true, cherry",
                result: Ok(vec![
                    ("apple", vec![]),
                    ("banana", vec![("color", "yellow"), ("ripe", "true")]),
                    ("cherry", vec![]),
                ]),
            },
            Test {
                name: "spacing",
                value: "apple ,banana ;color= yellow ; ripe=  \"true\" , cherry",
                result: Ok(vec![
                    ("apple", vec![]),
                    ("banana", vec![("color", "yellow"), ("ripe", "true")]),
                    ("cherry", vec![]),
                ]),
            },
        ];

        for test in tests {
            match collect_values(test.value.as_bytes()) {
                Ok(values) => {
                    let expected = match test.result {
                        Ok(v) => v,
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(values, expected, "test={}", test.name);
                }
                Err(e) => {
                    let expected = match test.result {
                        Err(e) => e,
                        _ => panic!("result mismatch: test={}", test.name),
                    };

                    assert_eq!(e.kind(), expected.kind(), "test={}", test.name);
                }
            }
        }
    }
}
