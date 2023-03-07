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

use crate::arena;
use crate::tnetstring;
use arrayvec::ArrayVec;
use std::cell::RefCell;
use std::io;
use std::mem;
use std::str;
use thiserror::Error;

pub const IDS_MAX: usize = 128;

const HEADERS_MAX: usize = 64;

const EMPTY_BYTES: &[u8] = b"";

const EMPTY_ID: Id = Id {
    id: EMPTY_BYTES,
    seq: None,
};

pub const EMPTY_HEADER: Header = Header {
    name: "",
    value: EMPTY_BYTES,
};

const EMPTY_HEADERS: [Header; 0] = [EMPTY_HEADER; 0];

#[derive(Debug, Error, PartialEq)]
pub enum ParseError {
    #[error("unrecognized data format")]
    Unrecognized,

    #[error(transparent)]
    TnetParse(#[from] tnetstring::ParseError),

    #[error("{0} must be of type {1}")]
    WrongType(&'static str, tnetstring::FrameType),

    #[error("{0} must be of type map or string")]
    NotMapOrString(&'static str),

    #[error("{0} must be a utf-8 string")]
    NotUtf8(&'static str),

    #[error("{0} must not be negative")]
    NegativeInt(&'static str),

    #[error("too many ids")]
    TooManyIds,

    #[error("too many headers")]
    TooManyHeaders,

    #[error("header item must have size 2")]
    InvalidHeader,

    #[error("no id")]
    NoId,
}

trait ErrorContext<T> {
    fn field(self, field: &'static str) -> Result<T, ParseError>;
}

impl<T> ErrorContext<T> for Result<T, tnetstring::ParseError> {
    fn field(self, field: &'static str) -> Result<T, ParseError> {
        match self {
            Ok(v) => Ok(v),
            Err(tnetstring::ParseError::WrongType(_, expected)) => {
                Err(ParseError::WrongType(field, expected))
            }
            Err(e) => Err(e.into()),
        }
    }
}

impl<T> ErrorContext<T> for Result<T, str::Utf8Error> {
    fn field(self, field: &'static str) -> Result<T, ParseError> {
        match self {
            Ok(v) => Ok(v),
            Err(_) => Err(ParseError::NotUtf8(field)),
        }
    }
}

#[derive(Clone, Copy)]
pub struct Id<'a> {
    pub id: &'a [u8],
    pub seq: Option<u32>,
}

pub struct Header<'a> {
    pub name: &'a str,
    pub value: &'a [u8],
}

#[derive(Debug, PartialEq)]
pub enum ContentType {
    Text,
    Binary,
}

type IdsScratch<'a> = ArrayVec<Id<'a>, IDS_MAX>;
type HeadersScratch<'a> = ArrayVec<Header<'a>, HEADERS_MAX>;

pub struct ParseScratch<'a> {
    ids: IdsScratch<'a>,
    headers: HeadersScratch<'a>,
}

impl ParseScratch<'_> {
    pub fn new() -> Self {
        Self {
            ids: ArrayVec::new(),
            headers: ArrayVec::new(),
        }
    }
}

trait Serialize<'a> {
    fn serialize(&self, w: &mut tnetstring::Writer<'a, '_>) -> Result<(), io::Error>;
}

trait Parse<'buf: 'scratch, 'scratch> {
    type Parsed;

    fn parse(
        root: tnetstring::MapIterator<'buf>,
        scratch: &'scratch mut HeadersScratch<'buf>,
    ) -> Result<Self::Parsed, ParseError>;
}

struct CommonData<'buf, 'ids> {
    from: &'buf [u8],
    ids: &'ids [Id<'buf>],
    multi: bool,
    ptype_str: &'buf str,
}

impl<'buf: 'ids, 'ids> CommonData<'buf, 'ids> {
    fn serialize(&self, w: &mut tnetstring::Writer<'buf, '_>) -> Result<(), io::Error> {
        if !self.from.is_empty() {
            w.write_string(b"from")?;
            w.write_string(self.from)?;
        }

        if self.ids.len() == 1 {
            w.write_string(b"id")?;
            w.write_string(self.ids[0].id)?;

            if let Some(seq) = self.ids[0].seq {
                w.write_string(b"seq")?;
                w.write_int(seq as isize)?;
            }
        } else if self.ids.len() > 1 {
            w.write_string(b"id")?;

            w.start_array()?;
            for id in self.ids.iter() {
                w.start_map()?;

                w.write_string(b"id")?;
                w.write_string(id.id)?;

                if let Some(seq) = id.seq {
                    w.write_string(b"seq")?;
                    w.write_int(seq as isize)?;
                }

                w.end_map()?;
            }
            w.end_array()?;
        }

        if self.multi {
            w.write_string(b"ext")?;
            w.start_map()?;

            w.write_string(b"multi")?;
            w.write_bool(true)?;

            w.end_map()?;
        }

        if !self.ptype_str.is_empty() {
            w.write_string(b"type")?;
            w.write_string(self.ptype_str.as_bytes())?;
        }

        Ok(())
    }

    fn parse(
        root: tnetstring::MapIterator<'buf>,
        scratch: &'ids mut IdsScratch<'buf>,
    ) -> Result<Self, ParseError> {
        let mut from = EMPTY_BYTES;
        let mut multi = false;
        let mut ptype_str = "";

        for e in root.clone() {
            let e = e?;

            match e.key {
                "from" => {
                    from = tnetstring::parse_string(e.data).field("from")?;
                }
                "id" => {
                    match e.ftype {
                        tnetstring::FrameType::Array => {
                            for idm in tnetstring::parse_array(e.data)? {
                                let idm = idm?;

                                if scratch.remaining_capacity() == 0 {
                                    return Err(ParseError::TooManyIds);
                                }

                                let mut id = EMPTY_BYTES;
                                let mut seq = None;

                                for m in tnetstring::parse_map(idm.data)? {
                                    let m = m?;

                                    match m.key {
                                        "id" => {
                                            let s = tnetstring::parse_string(m.data).field("id")?;

                                            id = s;
                                        }
                                        "seq" => {
                                            let x = tnetstring::parse_int(m.data).field("seq")?;

                                            if x < 0 {
                                                return Err(ParseError::NegativeInt("seq"));
                                            }

                                            seq = Some(x as u32);
                                        }
                                        _ => {} // skip unknown fields
                                    }
                                }

                                scratch.push(Id { id, seq });
                            }
                        }
                        tnetstring::FrameType::String => {
                            let s = tnetstring::parse_string(e.data)?;

                            if scratch.is_empty() {
                                scratch.push(EMPTY_ID);
                            }

                            scratch[0].id = s;
                        }
                        _ => {
                            return Err(ParseError::NotMapOrString("id"));
                        }
                    }
                }
                "seq" => {
                    let x = tnetstring::parse_int(e.data).field("seq")?;

                    if x < 0 {
                        return Err(ParseError::NegativeInt("seq"));
                    }

                    if scratch.is_empty() {
                        scratch.push(EMPTY_ID);
                    }

                    scratch[0].seq = Some(x as u32);
                }
                "type" => {
                    let s = tnetstring::parse_string(e.data).field("type")?;

                    let s = str::from_utf8(s).field("type")?;

                    ptype_str = s;
                }
                "ext" => {
                    let ext = tnetstring::parse_map(e.data).field("ext")?;

                    for m in ext {
                        let m = m?;

                        match m.key {
                            "multi" => {
                                let b = tnetstring::parse_bool(m.data).field("multi")?;

                                multi = b;
                            }
                            _ => {} // skip unknown fields
                        }
                    }
                }
                _ => {} // skip unknown fields
            }
        }

        Ok(Self {
            from,
            ids: scratch.as_slice(),
            multi,
            ptype_str,
        })
    }
}

pub struct RequestData<'buf, 'headers> {
    pub credits: u32,
    pub more: bool,
    pub stream: bool,
    pub max_size: u32,
    pub timeout: u32,
    pub method: &'buf str,
    pub uri: &'buf str,
    pub headers: &'headers [Header<'buf>],
    pub content_type: Option<ContentType>, // websocket
    pub body: &'buf [u8],
    pub peer_address: &'buf str,
    pub peer_port: u16,
    pub connect_host: &'buf str,
    pub connect_port: u16,
    pub ignore_policies: bool,
    pub trust_connect_host: bool,
    pub ignore_tls_errors: bool,
    pub follow_redirects: bool,
}

impl RequestData<'_, '_> {
    pub fn new() -> Self {
        Self {
            credits: 0,
            more: false,
            stream: false,
            max_size: 0,
            timeout: 0,
            method: "",
            uri: "",
            headers: &EMPTY_HEADERS,
            body: EMPTY_BYTES,
            content_type: None,
            peer_address: "",
            peer_port: 0,
            connect_host: "",
            connect_port: 0,
            ignore_policies: false,
            trust_connect_host: false,
            ignore_tls_errors: false,
            follow_redirects: false,
        }
    }
}

impl<'a> Serialize<'a> for RequestData<'a, 'a> {
    fn serialize(&self, w: &mut tnetstring::Writer<'a, '_>) -> Result<(), io::Error> {
        if !self.method.is_empty() {
            w.write_string(b"method")?;
            w.write_string(self.method.as_bytes())?;
        }

        if !self.uri.is_empty() {
            w.write_string(b"uri")?;
            w.write_string(self.uri.as_bytes())?;
        }

        if !self.headers.is_empty() {
            w.write_string(b"headers")?;
            w.start_array()?;

            for h in self.headers.iter() {
                w.start_array()?;
                w.write_string(h.name.as_bytes())?;
                w.write_string(h.value)?;
                w.end_array()?;
            }

            w.end_array()?;
        }

        if let Some(ctype) = &self.content_type {
            w.write_string(b"content-type")?;

            let s: &[u8] = match ctype {
                ContentType::Text => b"text",
                ContentType::Binary => b"binary",
            };

            w.write_string(s)?;
        }

        if !self.body.is_empty() {
            w.write_string(b"body")?;
            w.write_string(self.body)?;
        }

        if self.credits > 0 {
            w.write_string(b"credits")?;
            w.write_int(self.credits as isize)?;
        }

        if self.more {
            w.write_string(b"more")?;
            w.write_bool(true)?;
        }

        if self.stream {
            w.write_string(b"stream")?;
            w.write_bool(true)?;
        }

        if self.max_size > 0 {
            w.write_string(b"max-size")?;
            w.write_int(self.max_size as isize)?;
        }

        if self.timeout > 0 {
            w.write_string(b"timeout")?;
            w.write_int(self.timeout as isize)?;
        }

        if !self.peer_address.is_empty() {
            w.write_string(b"peer-address")?;
            w.write_string(self.peer_address.as_bytes())?;

            w.write_string(b"peer-port")?;
            w.write_int(self.peer_port as isize)?;
        }

        Ok(())
    }
}

impl<'buf: 'scratch, 'scratch> Parse<'buf, 'scratch> for RequestData<'buf, 'scratch> {
    type Parsed = Self;

    fn parse(
        root: tnetstring::MapIterator<'buf>,
        scratch: &'scratch mut HeadersScratch<'buf>,
    ) -> Result<Self::Parsed, ParseError> {
        let mut credits = 0;
        let mut more = false;
        let mut stream = false;
        let mut max_size = 0;
        let mut timeout = 0;
        let mut method = "";
        let mut uri = "";
        let mut content_type = None;
        let mut body = EMPTY_BYTES;
        let mut peer_address = "";
        let mut peer_port = 0;
        let mut connect_host = "";
        let mut connect_port = 0;
        let mut ignore_policies = false;
        let mut trust_connect_host = false;
        let mut ignore_tls_errors = false;
        let mut follow_redirects = false;

        for e in root {
            let e = e?;

            match e.key {
                "credits" => {
                    let x = tnetstring::parse_int(e.data).field("credits")?;

                    if x < 0 {
                        return Err(ParseError::NegativeInt("credits"));
                    }

                    credits = x as u32;
                }
                "more" => {
                    let b = tnetstring::parse_bool(e.data).field("more")?;

                    more = b;
                }
                "stream" => {
                    let b = tnetstring::parse_bool(e.data).field("stream")?;

                    stream = b;
                }
                "max-size" => {
                    let x = tnetstring::parse_int(e.data).field("max-size")?;

                    if x < 0 {
                        return Err(ParseError::NegativeInt("max-size"));
                    }

                    max_size = x as u32;
                }
                "timeout" => {
                    let x = tnetstring::parse_int(e.data).field("timeout")?;

                    if x < 0 {
                        return Err(ParseError::NegativeInt("timeout"));
                    }

                    timeout = x as u32;
                }
                "method" => {
                    let s = tnetstring::parse_string(e.data).field("method")?;

                    let s = str::from_utf8(s).field("method")?;

                    method = s;
                }
                "uri" => {
                    let s = tnetstring::parse_string(e.data).field("uri")?;

                    let s = str::from_utf8(s).field("uri")?;

                    uri = s;
                }
                "headers" => {
                    let headers = tnetstring::parse_array(e.data).field("headers")?;

                    for ha in headers {
                        let ha = ha?;

                        if scratch.remaining_capacity() == 0 {
                            return Err(ParseError::TooManyHeaders);
                        }

                        let mut hi = tnetstring::parse_array(ha.data).field("header item")?;

                        let name = match hi.next() {
                            Some(Ok(name)) => name,
                            Some(Err(e)) => {
                                return Err(e.into());
                            }
                            None => {
                                return Err(ParseError::InvalidHeader);
                            }
                        };

                        let name = tnetstring::parse_string(name.data).field("header name")?;

                        let name = str::from_utf8(name).field("header name")?;

                        let value = match hi.next() {
                            Some(Ok(name)) => name,
                            Some(Err(e)) => {
                                return Err(e.into());
                            }
                            None => {
                                return Err(ParseError::InvalidHeader);
                            }
                        };

                        let value = tnetstring::parse_string(value.data).field("header value")?;

                        scratch.push(Header { name, value });
                    }
                }
                "content-type" => {
                    let s = tnetstring::parse_string(e.data).field("content-type")?;

                    content_type = Some(match s {
                        b"binary" => ContentType::Binary,
                        _ => ContentType::Text,
                    });
                }
                "body" => {
                    let s = tnetstring::parse_string(e.data).field("body")?;

                    body = s;
                }
                "peer-address" => {
                    let s = tnetstring::parse_string(e.data).field("peer-address")?;

                    let s = str::from_utf8(s).field("peer-address")?;

                    peer_address = s;
                }
                "peer-port" => {
                    let x = tnetstring::parse_int(e.data).field("peer-port")?;

                    if x < 0 {
                        return Err(ParseError::NegativeInt("peer-port"));
                    }

                    peer_port = x as u16;
                }
                "connect-host" => {
                    let s = tnetstring::parse_string(e.data).field("connect-host")?;

                    let s = str::from_utf8(s).field("connect-host")?;

                    connect_host = s;
                }
                "connect-port" => {
                    let x = tnetstring::parse_int(e.data).field("connect-port")?;

                    if x < 0 {
                        return Err(ParseError::NegativeInt("connect-port"));
                    }

                    connect_port = x as u16;
                }
                "ignore-policies" => {
                    let b = tnetstring::parse_bool(e.data).field("ignore-policies")?;

                    ignore_policies = b;
                }
                "trust-connect-host" => {
                    let b = tnetstring::parse_bool(e.data).field("trust-connect-host")?;

                    trust_connect_host = b;
                }
                "ignore-tls-errors" => {
                    let b = tnetstring::parse_bool(e.data).field("ignore-tls-errors")?;

                    ignore_tls_errors = b;
                }
                "follow-redirects" => {
                    let b = tnetstring::parse_bool(e.data).field("follow-redirects")?;

                    follow_redirects = b;
                }
                _ => {} // skip unknown fields
            }
        }

        Ok(Self {
            credits,
            more,
            stream,
            max_size,
            timeout,
            method,
            uri,
            headers: scratch.as_slice(),
            content_type,
            body,
            peer_address,
            peer_port,
            connect_host,
            connect_port,
            ignore_policies,
            trust_connect_host,
            ignore_tls_errors,
            follow_redirects,
        })
    }
}

pub struct ResponseData<'buf, 'headers> {
    pub credits: u32,
    pub more: bool,
    pub code: u16,
    pub reason: &'buf str,
    pub headers: &'headers [Header<'buf>],
    pub content_type: Option<ContentType>, // websocket
    pub body: &'buf [u8],
}

impl<'a> Serialize<'a> for ResponseData<'a, 'a> {
    fn serialize(&self, w: &mut tnetstring::Writer<'a, '_>) -> Result<(), io::Error> {
        if self.code > 0 {
            w.write_string(b"code")?;
            w.write_int(self.code as isize)?;
        }

        if !self.reason.is_empty() {
            w.write_string(b"reason")?;
            w.write_string(self.reason.as_bytes())?;
        }

        if !self.headers.is_empty() {
            w.write_string(b"headers")?;
            w.start_array()?;

            for h in self.headers.iter() {
                w.start_array()?;
                w.write_string(h.name.as_bytes())?;
                w.write_string(h.value)?;
                w.end_array()?;
            }

            w.end_array()?;
        }

        if let Some(ctype) = &self.content_type {
            w.write_string(b"content-type")?;

            let s: &[u8] = match ctype {
                ContentType::Text => b"text",
                ContentType::Binary => b"binary",
            };

            w.write_string(s)?;
        }

        if !self.body.is_empty() {
            w.write_string(b"body")?;
            w.write_string(self.body)?;
        }

        if self.credits > 0 {
            w.write_string(b"credits")?;
            w.write_int(self.credits as isize)?;
        }

        if self.more {
            w.write_string(b"more")?;
            w.write_bool(true)?;
        }

        Ok(())
    }
}

impl<'buf: 'scratch, 'scratch> Parse<'buf, 'scratch> for ResponseData<'buf, 'scratch> {
    type Parsed = Self;

    fn parse(
        root: tnetstring::MapIterator<'buf>,
        scratch: &'scratch mut HeadersScratch<'buf>,
    ) -> Result<Self::Parsed, ParseError> {
        let mut credits = 0;
        let mut more = false;
        let mut code = 0;
        let mut reason = "";
        let mut content_type = None;
        let mut body = EMPTY_BYTES;

        for e in root {
            let e = e?;

            match e.key {
                "credits" => {
                    let x = tnetstring::parse_int(e.data).field("credits")?;

                    if x < 0 {
                        return Err(ParseError::NegativeInt("credits"));
                    }

                    credits = x as u32;
                }
                "more" => {
                    let b = tnetstring::parse_bool(e.data).field("more")?;

                    more = b;
                }
                "code" => {
                    let x = tnetstring::parse_int(e.data).field("code")?;

                    if x < 0 {
                        return Err(ParseError::NegativeInt("code"));
                    }

                    code = x as u16;
                }
                "reason" => {
                    let s = tnetstring::parse_string(e.data).field("reason")?;

                    let s = str::from_utf8(s).field("reason")?;

                    reason = s;
                }
                "headers" => {
                    let headers = tnetstring::parse_array(e.data).field("headers")?;

                    for ha in headers {
                        let ha = ha?;

                        if scratch.remaining_capacity() == 0 {
                            return Err(ParseError::TooManyHeaders);
                        }

                        let mut hi = tnetstring::parse_array(ha.data).field("header item")?;

                        let name = match hi.next() {
                            Some(Ok(name)) => name,
                            Some(Err(e)) => {
                                return Err(e.into());
                            }
                            None => {
                                return Err(ParseError::InvalidHeader);
                            }
                        };

                        let name = tnetstring::parse_string(name.data).field("header name")?;

                        let name = str::from_utf8(name).field("header name")?;

                        let value = match hi.next() {
                            Some(Ok(name)) => name,
                            Some(Err(e)) => {
                                return Err(e.into());
                            }
                            None => {
                                return Err(ParseError::InvalidHeader);
                            }
                        };

                        let value = tnetstring::parse_string(value.data).field("header value")?;

                        scratch.push(Header { name, value });
                    }
                }
                "content-type" => {
                    let s = tnetstring::parse_string(e.data).field("content-type")?;

                    content_type = Some(match s {
                        b"binary" => ContentType::Binary,
                        _ => ContentType::Text,
                    });
                }
                "body" => {
                    let s = tnetstring::parse_string(e.data).field("body")?;

                    body = s;
                }
                _ => {} // skip unknown fields
            }
        }

        Ok(Self {
            credits,
            more,
            code,
            reason,
            headers: scratch.as_slice(),
            content_type,
            body,
        })
    }
}

pub struct RequestErrorData<'a> {
    pub condition: &'a str,
}

impl<'a> Serialize<'a> for RequestErrorData<'a> {
    fn serialize(&self, w: &mut tnetstring::Writer<'a, '_>) -> Result<(), io::Error> {
        w.write_string(b"condition")?;
        w.write_string(self.condition.as_bytes())?;

        Ok(())
    }
}

impl<'buf: 'scratch, 'scratch> Parse<'buf, 'scratch> for RequestErrorData<'buf> {
    type Parsed = Self;

    fn parse(
        root: tnetstring::MapIterator<'buf>,
        _scratch: &'scratch mut HeadersScratch<'buf>,
    ) -> Result<Self::Parsed, ParseError> {
        let mut condition = "";

        for e in root {
            let e = e?;

            match e.key {
                "condition" => {
                    let s = tnetstring::parse_string(e.data).field("condition")?;

                    let s = str::from_utf8(s).field("condition")?;

                    condition = s;
                }
                _ => {} // skip unknown fields
            }
        }

        Ok(Self { condition })
    }
}

pub struct RejectedInfo<'buf, 'headers> {
    pub code: u16,
    pub reason: &'buf str,
    pub headers: &'headers [Header<'buf>],
    pub body: &'buf [u8],
}

pub struct ResponseErrorData<'buf, 'headers> {
    pub condition: &'buf str,
    pub rejected_info: Option<RejectedInfo<'buf, 'headers>>, // rejected (websocket)
}

impl<'a> Serialize<'a> for ResponseErrorData<'a, 'a> {
    fn serialize(&self, w: &mut tnetstring::Writer<'a, '_>) -> Result<(), io::Error> {
        w.write_string(b"condition")?;
        w.write_string(self.condition.as_bytes())?;

        if let Some(ri) = &self.rejected_info {
            w.write_string(b"code")?;
            w.write_int(ri.code as isize)?;

            w.write_string(b"reason")?;
            w.write_string(ri.reason.as_bytes())?;

            if !ri.headers.is_empty() {
                w.write_string(b"headers")?;
                w.start_array()?;

                for h in ri.headers.iter() {
                    w.start_array()?;
                    w.write_string(h.name.as_bytes())?;
                    w.write_string(h.value)?;
                    w.end_array()?;
                }

                w.end_array()?;
            }

            w.write_string(b"body")?;
            w.write_string(ri.body)?;
        }

        Ok(())
    }
}

impl<'buf: 'scratch, 'scratch> Parse<'buf, 'scratch> for ResponseErrorData<'buf, 'scratch> {
    type Parsed = Self;

    fn parse(
        root: tnetstring::MapIterator<'buf>,
        scratch: &'scratch mut HeadersScratch<'buf>,
    ) -> Result<Self::Parsed, ParseError> {
        let mut condition = "";
        let mut code = 0;
        let mut reason = "";
        let mut body = EMPTY_BYTES;

        for e in root {
            let e = e?;

            match e.key {
                "condition" => {
                    let s = tnetstring::parse_string(e.data).field("condition")?;

                    let s = str::from_utf8(s).field("condition")?;

                    condition = s;
                }
                "code" => {
                    let x = tnetstring::parse_int(e.data).field("code")?;

                    if x < 0 {
                        return Err(ParseError::NegativeInt("code"));
                    }

                    code = x as u16;
                }
                "reason" => {
                    let s = tnetstring::parse_string(e.data).field("reason")?;

                    let s = str::from_utf8(s).field("reason")?;

                    reason = s;
                }
                "headers" => {
                    let headers = tnetstring::parse_array(e.data).field("headers")?;

                    for ha in headers {
                        let ha = ha?;

                        if scratch.remaining_capacity() == 0 {
                            return Err(ParseError::TooManyHeaders);
                        }

                        let mut hi = tnetstring::parse_array(ha.data).field("header item")?;

                        let name = match hi.next() {
                            Some(Ok(name)) => name,
                            Some(Err(e)) => {
                                return Err(e.into());
                            }
                            None => {
                                return Err(ParseError::InvalidHeader);
                            }
                        };

                        let name = tnetstring::parse_string(name.data).field("header name")?;

                        let name = str::from_utf8(name).field("header name")?;

                        let value = match hi.next() {
                            Some(Ok(name)) => name,
                            Some(Err(e)) => {
                                return Err(e.into());
                            }
                            None => {
                                return Err(ParseError::InvalidHeader);
                            }
                        };

                        let value = tnetstring::parse_string(value.data).field("header value")?;

                        scratch.push(Header { name, value });
                    }
                }
                "body" => {
                    let s = tnetstring::parse_string(e.data).field("body")?;

                    body = s;
                }
                _ => {} // skip unknown fields
            }
        }

        let rejected_info = if condition == "rejected" {
            Some(RejectedInfo {
                code,
                reason,
                headers: scratch.as_slice(),
                body,
            })
        } else {
            None
        };

        Ok(Self {
            condition,
            rejected_info,
        })
    }
}

pub struct CreditData {
    pub credits: u32,
}

impl Serialize<'_> for CreditData {
    fn serialize(&self, w: &mut tnetstring::Writer) -> Result<(), io::Error> {
        w.write_string(b"credits")?;
        w.write_int(self.credits as isize)?;

        Ok(())
    }
}

impl<'buf: 'scratch, 'scratch> Parse<'buf, 'scratch> for CreditData {
    type Parsed = Self;

    fn parse(
        root: tnetstring::MapIterator,
        _scratch: &mut HeadersScratch,
    ) -> Result<Self::Parsed, ParseError> {
        let mut credits = 0;

        for e in root {
            let e = e?;

            match e.key {
                "credits" => {
                    let x = tnetstring::parse_int(e.data).field("credits")?;

                    if x < 0 {
                        return Err(ParseError::NegativeInt("credits"));
                    }

                    credits = x as u32;
                }
                _ => {} // skip unknown fields
            }
        }

        Ok(Self { credits })
    }
}

pub struct CloseData<'a> {
    // code, reason
    pub status: Option<(u16, &'a str)>,
}

impl<'a> Serialize<'a> for CloseData<'a> {
    fn serialize(&self, w: &mut tnetstring::Writer<'a, '_>) -> Result<(), io::Error> {
        if let Some(status) = self.status {
            w.write_string(b"code")?;
            w.write_int(status.0 as isize)?;

            if !status.1.is_empty() {
                w.write_string(b"body")?;
                w.write_string(status.1.as_bytes())?;
            }
        }

        Ok(())
    }
}

impl<'buf: 'scratch, 'scratch> Parse<'buf, 'scratch> for CloseData<'buf> {
    type Parsed = Self;

    fn parse(
        root: tnetstring::MapIterator<'buf>,
        _scratch: &'scratch mut HeadersScratch<'buf>,
    ) -> Result<Self::Parsed, ParseError> {
        let mut code = None;
        let mut reason = "";

        for e in root {
            let e = e?;

            match e.key {
                "code" => {
                    let x = tnetstring::parse_int(e.data).field("code")?;

                    if x < 0 {
                        return Err(ParseError::NegativeInt("code"));
                    }

                    code = Some(x as u16);
                }
                "body" => {
                    let s = tnetstring::parse_string(e.data).field("body")?;

                    let s = str::from_utf8(s).field("condition")?;

                    reason = s;
                }
                _ => {} // skip unknown fields
            }
        }

        if let Some(code) = code {
            Ok(Self {
                status: Some((code, reason)),
            })
        } else {
            Ok(Self { status: None })
        }
    }
}

fn parse_ping_or_pong(root: tnetstring::MapIterator) -> Result<(u32, &[u8]), ParseError> {
    let mut credits = 0;
    let mut body = EMPTY_BYTES;

    for e in root {
        let e = e?;

        match e.key {
            "credits" => {
                let x = tnetstring::parse_int(e.data).field("credits")?;

                if x < 0 {
                    return Err(ParseError::NegativeInt("credits"));
                }

                credits = x as u32;
            }
            "body" => {
                let s = tnetstring::parse_string(e.data).field("body")?;

                body = s;
            }
            _ => {} // skip unknown fields
        }
    }

    Ok((credits, body))
}

pub struct PingData<'a> {
    pub credits: u32,
    pub body: &'a [u8],
}

impl<'a> Serialize<'a> for PingData<'a> {
    fn serialize(&self, w: &mut tnetstring::Writer<'a, '_>) -> Result<(), io::Error> {
        if !self.body.is_empty() {
            w.write_string(b"body")?;
            w.write_string(self.body)?;
        }

        Ok(())
    }
}

impl<'buf: 'scratch, 'scratch> Parse<'buf, 'scratch> for PingData<'buf> {
    type Parsed = Self;

    fn parse(
        root: tnetstring::MapIterator<'buf>,
        _scratch: &'scratch mut HeadersScratch<'buf>,
    ) -> Result<Self::Parsed, ParseError> {
        let (credits, body) = parse_ping_or_pong(root)?;
        Ok(Self { credits, body })
    }
}

pub struct PongData<'a> {
    pub credits: u32,
    pub body: &'a [u8],
}

impl<'a> Serialize<'a> for PongData<'a> {
    fn serialize(&self, w: &mut tnetstring::Writer<'a, '_>) -> Result<(), io::Error> {
        if !self.body.is_empty() {
            w.write_string(b"body")?;
            w.write_string(self.body)?;
        }

        Ok(())
    }
}

impl<'buf: 'scratch, 'scratch> Parse<'buf, 'scratch> for PongData<'buf> {
    type Parsed = Self;

    fn parse(
        root: tnetstring::MapIterator<'buf>,
        _scratch: &'scratch mut HeadersScratch<'buf>,
    ) -> Result<Self::Parsed, ParseError> {
        let (credits, body) = parse_ping_or_pong(root)?;
        Ok(Self { credits, body })
    }
}

pub enum RequestPacket<'buf, 'headers> {
    Unknown,
    Data(RequestData<'buf, 'headers>),
    Error(RequestErrorData<'buf>),
    Credit(CreditData),
    KeepAlive,
    Cancel,
    HandoffStart,
    HandoffProceed,
    Close(CloseData<'buf>),
    Ping(PingData<'buf>),
    Pong(PongData<'buf>),
}

pub enum ResponsePacket<'buf, 'headers> {
    Unknown,
    Data(ResponseData<'buf, 'headers>),
    Error(ResponseErrorData<'buf, 'headers>),
    Credit(CreditData),
    KeepAlive,
    Cancel,
    HandoffStart,
    HandoffProceed,
    Close(CloseData<'buf>),
    Ping(PingData<'buf>),
    Pong(PongData<'buf>),
}

pub fn parse_ids<'buf, 'scratch>(
    src: &'buf [u8],
    scratch: &'scratch mut ParseScratch<'buf>,
) -> Result<&'scratch [Id<'buf>], ParseError> {
    if src.is_empty() || src[0] != b'T' {
        return Err(ParseError::Unrecognized);
    }

    let root = tnetstring::parse_map(&src[1..]).field("root")?;

    for e in root.clone() {
        let e = e?;

        match e.key {
            "id" => {
                match e.ftype {
                    tnetstring::FrameType::Array => {
                        for idm in tnetstring::parse_array(e.data)? {
                            let idm = idm?;

                            if scratch.ids.remaining_capacity() == 0 {
                                return Err(ParseError::TooManyIds);
                            }

                            let mut id = EMPTY_BYTES;

                            for m in tnetstring::parse_map(idm.data)? {
                                let m = m?;

                                match m.key {
                                    "id" => {
                                        let s = tnetstring::parse_string(m.data).field("id")?;

                                        id = s;
                                    }
                                    _ => {} // skip other fields
                                }
                            }

                            scratch.ids.push(Id { id, seq: None });
                        }
                    }
                    tnetstring::FrameType::String => {
                        let s = tnetstring::parse_string(e.data)?;

                        scratch.ids.push(Id { id: s, seq: None });
                    }
                    _ => {
                        return Err(ParseError::NotMapOrString("id"));
                    }
                }

                return Ok(scratch.ids.as_slice());
            }
            _ => {} // skip other fields
        }
    }

    Ok(scratch.ids.as_slice())
}

pub trait PacketParse<'buf: 'scratch, 'scratch> {
    type Parsed;

    fn parse(
        src: &'buf [u8],
        scratch: &'scratch mut ParseScratch<'buf>,
    ) -> Result<Self::Parsed, ParseError>;
}

pub struct Request<'buf, 'ids, 'headers> {
    pub from: &'buf [u8],
    pub ids: &'ids [Id<'buf>],
    pub multi: bool,
    pub ptype: RequestPacket<'buf, 'headers>,
    pub ptype_str: &'buf str,
}

impl<'buf, 'ids, 'headers, 'scratch> Request<'buf, 'ids, 'headers> {
    pub fn new_data(
        from: &'buf [u8],
        ids: &'ids [Id<'buf>],
        data: RequestData<'buf, 'headers>,
    ) -> Self {
        Self::new(from, ids, RequestPacket::Data(data))
    }

    pub fn new_error(from: &'buf [u8], ids: &'ids [Id<'buf>], condition: &'buf str) -> Self {
        Self::new(
            from,
            ids,
            RequestPacket::Error(RequestErrorData { condition }),
        )
    }

    pub fn new_credit(from: &'buf [u8], ids: &'ids [Id<'buf>], credits: u32) -> Self {
        Self::new(from, ids, RequestPacket::Credit(CreditData { credits }))
    }

    pub fn new_keep_alive(from: &'buf [u8], ids: &'ids [Id<'buf>]) -> Self {
        Self::new(from, ids, RequestPacket::KeepAlive)
    }

    pub fn new_cancel(from: &'buf [u8], ids: &'ids [Id<'buf>]) -> Self {
        Self::new(from, ids, RequestPacket::Cancel)
    }

    pub fn new_handoff_start(from: &'buf [u8], ids: &'ids [Id<'buf>]) -> Self {
        Self::new(from, ids, RequestPacket::HandoffStart)
    }

    pub fn new_handoff_proceed(from: &'buf [u8], ids: &'ids [Id<'buf>]) -> Self {
        Self::new(from, ids, RequestPacket::HandoffProceed)
    }

    pub fn new_close(
        from: &'buf [u8],
        ids: &'ids [Id<'buf>],
        status: Option<(u16, &'buf str)>,
    ) -> Self {
        Self::new(from, ids, RequestPacket::Close(CloseData { status }))
    }

    pub fn new_ping(from: &'buf [u8], ids: &'ids [Id<'buf>], body: &'buf [u8]) -> Self {
        Self::new(
            from,
            ids,
            RequestPacket::Ping(PingData { credits: 0, body }),
        )
    }

    pub fn new_pong(from: &'buf [u8], ids: &'ids [Id<'buf>], body: &'buf [u8]) -> Self {
        Self::new(
            from,
            ids,
            RequestPacket::Pong(PongData { credits: 0, body }),
        )
    }

    pub fn serialize(&self, dest: &mut [u8]) -> Result<usize, io::Error> {
        if dest.is_empty() {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        dest[0] = b'T';

        let mut cursor = io::Cursor::new(&mut dest[1..]);
        let mut w = tnetstring::Writer::new(&mut cursor);

        w.start_map()?;

        let common = CommonData {
            from: self.from,
            ids: self.ids,
            multi: self.multi,
            ptype_str: match &self.ptype {
                RequestPacket::Data(_) => "",
                RequestPacket::Error(_) => "error",
                RequestPacket::Credit(_) => "credit",
                RequestPacket::KeepAlive => "keep-alive",
                RequestPacket::Cancel => "cancel",
                RequestPacket::HandoffStart => "handoff-start",
                RequestPacket::HandoffProceed => "handoff-proceed",
                RequestPacket::Close(_) => "close",
                RequestPacket::Ping(_) => "ping",
                RequestPacket::Pong(_) => "pong",
                RequestPacket::Unknown => panic!("invalid packet type"),
            },
        };

        common.serialize(&mut w)?;

        match &self.ptype {
            RequestPacket::Data(data) => data.serialize(&mut w)?,
            RequestPacket::Error(data) => data.serialize(&mut w)?,
            RequestPacket::Credit(data) => data.serialize(&mut w)?,
            RequestPacket::Close(data) => data.serialize(&mut w)?,
            RequestPacket::Ping(data) => data.serialize(&mut w)?,
            RequestPacket::Pong(data) => data.serialize(&mut w)?,
            _ => {}
        }

        w.end_map()?;

        w.flush()?;

        Ok((cursor.position() as usize) + 1)
    }

    fn new(from: &'buf [u8], ids: &'ids [Id<'buf>], ptype: RequestPacket<'buf, 'headers>) -> Self {
        Self {
            from: from,
            ids: ids,
            multi: false,
            ptype,
            ptype_str: "",
        }
    }
}

impl<'buf: 'scratch, 'scratch> PacketParse<'buf, 'scratch> for Request<'buf, 'scratch, 'scratch> {
    type Parsed = Self;

    fn parse(
        src: &'buf [u8],
        scratch: &'scratch mut ParseScratch<'buf>,
    ) -> Result<Self::Parsed, ParseError> {
        if src.is_empty() || src[0] != b'T' {
            return Err(ParseError::Unrecognized);
        }

        let root = tnetstring::parse_map(&src[1..]).field("root")?;

        let CommonData {
            from,
            ids,
            multi,
            ptype_str,
        } = CommonData::parse(root.clone(), &mut scratch.ids)?;

        let ptype = match ptype_str {
            // data
            "" => RequestPacket::Data(RequestData::parse(root, &mut scratch.headers)?),
            "error" => RequestPacket::Error(RequestErrorData::parse(root, &mut scratch.headers)?),
            "credit" => RequestPacket::Credit(CreditData::parse(root, &mut scratch.headers)?),
            "keep-alive" => RequestPacket::KeepAlive,
            "cancel" => RequestPacket::Cancel,
            "handoff-start" => RequestPacket::HandoffStart,
            "handoff-proceed" => RequestPacket::HandoffProceed,
            "close" => RequestPacket::Close(CloseData::parse(root, &mut scratch.headers)?),
            "ping" => RequestPacket::Ping(PingData::parse(root, &mut scratch.headers)?),
            "pong" => RequestPacket::Pong(PongData::parse(root, &mut scratch.headers)?),
            _ => RequestPacket::Unknown,
        };

        Ok(Self {
            from,
            ids,
            multi,
            ptype,
            ptype_str,
        })
    }
}

pub struct Response<'buf, 'ids, 'headers> {
    pub from: &'buf [u8],
    pub ids: &'ids [Id<'buf>],
    pub multi: bool,
    pub ptype: ResponsePacket<'buf, 'headers>,
    pub ptype_str: &'buf str,
}

impl<'buf, 'scratch> Response<'_, '_, '_> {
    pub fn serialize(&self, dest: &mut [u8]) -> Result<usize, io::Error> {
        if dest.is_empty() {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        dest[0] = b'T';

        let mut cursor = io::Cursor::new(&mut dest[1..]);
        let mut w = tnetstring::Writer::new(&mut cursor);

        w.start_map()?;

        let common = CommonData {
            from: self.from,
            ids: self.ids,
            multi: self.multi,
            ptype_str: match &self.ptype {
                ResponsePacket::Data(_) => "",
                ResponsePacket::Error(_) => "error",
                ResponsePacket::Credit(_) => "credit",
                ResponsePacket::KeepAlive => "keep-alive",
                ResponsePacket::Cancel => "cancel",
                ResponsePacket::HandoffStart => "handoff-start",
                ResponsePacket::HandoffProceed => "handoff-proceed",
                ResponsePacket::Close(_) => "close",
                ResponsePacket::Ping(_) => "ping",
                ResponsePacket::Pong(_) => "pong",
                ResponsePacket::Unknown => panic!("invalid packet type"),
            },
        };

        common.serialize(&mut w)?;

        match &self.ptype {
            ResponsePacket::Data(data) => data.serialize(&mut w)?,
            ResponsePacket::Error(data) => data.serialize(&mut w)?,
            ResponsePacket::Credit(data) => data.serialize(&mut w)?,
            ResponsePacket::Close(data) => data.serialize(&mut w)?,
            ResponsePacket::Ping(data) => data.serialize(&mut w)?,
            ResponsePacket::Pong(data) => data.serialize(&mut w)?,
            _ => {}
        }

        w.end_map()?;

        w.flush()?;

        Ok((cursor.position() as usize) + 1)
    }
}

impl<'buf: 'scratch, 'scratch> PacketParse<'buf, 'scratch> for Response<'buf, 'scratch, 'scratch> {
    type Parsed = Self;

    fn parse(
        src: &'buf [u8],
        scratch: &'scratch mut ParseScratch<'buf>,
    ) -> Result<Self::Parsed, ParseError> {
        if src.is_empty() || src[0] != b'T' {
            return Err(ParseError::Unrecognized);
        }

        let root = tnetstring::parse_map(&src[1..]).field("root")?;

        let CommonData {
            from,
            ids,
            multi,
            ptype_str,
        } = CommonData::parse(root.clone(), &mut scratch.ids)?;

        let ptype = match ptype_str {
            // data
            "" => ResponsePacket::Data(ResponseData::parse(root, &mut scratch.headers)?),
            "error" => ResponsePacket::Error(ResponseErrorData::parse(root, &mut scratch.headers)?),
            "credit" => ResponsePacket::Credit(CreditData::parse(root, &mut scratch.headers)?),
            "keep-alive" => ResponsePacket::KeepAlive,
            "cancel" => ResponsePacket::Cancel,
            "handoff-start" => ResponsePacket::HandoffStart,
            "handoff-proceed" => ResponsePacket::HandoffProceed,
            "close" => ResponsePacket::Close(CloseData::parse(root, &mut scratch.headers)?),
            "ping" => ResponsePacket::Ping(PingData::parse(root, &mut scratch.headers)?),
            "pong" => ResponsePacket::Pong(PongData::parse(root, &mut scratch.headers)?),
            _ => ResponsePacket::Unknown,
        };

        Ok(Self {
            from,
            ids,
            multi,
            ptype,
            ptype_str,
        })
    }
}

pub struct OwnedPacket<T> {
    inner: T,
    _scratch: arena::Rc<RefCell<ParseScratch<'static>>>,
    _src: arena::Arc<zmq::Message>,
}

impl<T> OwnedPacket<T>
where
    T: PacketParse<'static, 'static, Parsed = T>,
{
    pub fn parse(
        src: arena::Arc<zmq::Message>,
        offset: usize,
        scratch: arena::Rc<RefCell<ParseScratch<'static>>>,
    ) -> Result<Self, ParseError> {
        let src_ref: &[u8] = &src.get()[offset..];

        // SAFETY: Self will take ownership of src, and the bytes referred to
        // by src_ref are on the heap, and src will not be modified or
        // dropped until Self is dropped, so the bytes referred to by src_ref
        // will remain valid for the lifetime of Self
        let src_ref: &'static [u8] = unsafe { mem::transmute(src_ref) };

        // SAFETY: Self will take ownership of scratch, and the location
        // referred to by scratch_mut is in an arena, and scratch will not
        // be dropped until Self is dropped, so the location referred to by
        // scratch_mut will remain valid for the lifetime of Self
        //
        // further, it is safe for T::parse() to write references to src_ref
        // into scratch_mut, because src_ref and scratch_mut have the same
        // lifetime
        let scratch_mut: &'static mut ParseScratch<'static> =
            unsafe { scratch.get().as_ptr().as_mut().unwrap() };

        let inner = T::parse(src_ref, scratch_mut)?;

        Ok(Self {
            inner,
            _scratch: scratch,
            _src: src,
        })
    }
}

pub type OwnedRequest = OwnedPacket<Request<'static, 'static, 'static>>;

impl OwnedRequest {
    pub fn get<'a>(&'a self) -> &'a Request<'a, 'a, 'a> {
        let req: &Request = &self.inner;

        // SAFETY: here we reduce the inner lifetimes from 'static to that of
        // the owning object, which is fine
        unsafe { mem::transmute(req) }
    }
}

pub type OwnedResponse = OwnedPacket<Response<'static, 'static, 'static>>;

impl OwnedResponse {
    pub fn get<'a>(&'a self) -> &'a Response<'a, 'a, 'a> {
        let resp: &Response = &self.inner;

        // SAFETY: here we reduce the inner lifetimes from 'static to that of
        // the owning object, which is fine
        unsafe { mem::transmute(resp) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::rc::Rc;
    use std::sync::Arc;

    #[test]
    fn test_req_serialize() {
        struct Test {
            name: &'static str,
            req: Request<'static, 'static, 'static>,
            expected: &'static str,
        }

        // data, error, credit, keepalive, cancel, handoffstart/proceed, close, ping, pong
        let tests = [
            Test {
                name: "data",
                req: Request {
                    from: b"client",
                    ids: &[Id {
                        id: b"1",
                        seq: Some(0),
                    }],
                    multi: false,
                    ptype: RequestPacket::Data(RequestData {
                        credits: 0,
                        more: true,
                        stream: false,
                        max_size: 0,
                        timeout: 0,
                        method: "POST",
                        uri: "http://example.com/path",
                        headers: &[Header {
                            name: "Content-Type",
                            value: b"text/plain",
                        }],
                        content_type: None,
                        body: b"hello",
                        peer_address: "",
                        peer_port: 0,
                        connect_host: "",
                        connect_port: 0,
                        ignore_policies: false,
                        trust_connect_host: false,
                        ignore_tls_errors: false,
                        follow_redirects: false,
                    }),
                    ptype_str: "",
                },
                expected: concat!(
                    "T161:4:from,6:client,2:id,1:1,3:seq,1:0#6:method,4:POST,3:uri",
                    ",23:http://example.com/path,7:headers,34:30:12:Content-Type,1",
                    "0:text/plain,]]4:body,5:hello,4:more,4:true!}",
                ),
            },
            Test {
                name: "error",
                req: Request {
                    from: b"client",
                    ids: &[Id {
                        id: b"1",
                        seq: Some(0),
                    }],
                    multi: false,
                    ptype: RequestPacket::Error(RequestErrorData {
                        condition: "bad-request",
                    }),
                    ptype_str: "",
                },
                expected: concat!(
                    "T77:4:from,6:client,2:id,1:1,3:seq,1:0#4:type,5:error,9:condi",
                    "tion,11:bad-request,}",
                ),
            },
        ];

        for test in tests.iter() {
            let mut data = [0; 1024];

            let size = test.req.serialize(&mut data).unwrap();

            assert_eq!(
                str::from_utf8(&data[..size]).unwrap(),
                test.expected,
                "test={}",
                test.name
            );
        }
    }

    #[test]
    fn test_resp_serialize() {
        struct Test {
            name: &'static str,
            resp: Response<'static, 'static, 'static>,
            expected: &'static str,
        }

        // data, error, credit, keepalive, cancel, handoffstart/proceed, close, ping, pong
        let tests = [
            Test {
                name: "data",
                resp: Response {
                    from: b"server",
                    ids: &[Id {
                        id: b"1",
                        seq: Some(0),
                    }],
                    multi: false,
                    ptype: ResponsePacket::Data(ResponseData {
                        credits: 0,
                        more: true,
                        code: 200,
                        reason: "OK",
                        headers: &[Header {
                            name: "Content-Type",
                            value: b"text/plain",
                        }],
                        content_type: None,
                        body: b"hello",
                    }),
                    ptype_str: "",
                },
                expected: concat!(
                    "T139:4:from,6:server,2:id,1:1,3:seq,1:0#4:code,3:200#6:reason",
                    ",2:OK,7:headers,34:30:12:Content-Type,10:text/plain,]]4:body,",
                    "5:hello,4:more,4:true!}",
                ),
            },
            Test {
                name: "error",
                resp: Response {
                    from: b"server",
                    ids: &[Id {
                        id: b"1",
                        seq: Some(0),
                    }],
                    multi: false,
                    ptype: ResponsePacket::Error(ResponseErrorData {
                        condition: "bad-request",
                        rejected_info: None,
                    }),
                    ptype_str: "",
                },
                expected: concat!(
                    "T77:4:from,6:server,2:id,1:1,3:seq,1:0#4:type,5:error,9:condi",
                    "tion,11:bad-request,}",
                ),
            },
        ];

        for test in tests.iter() {
            let mut data = [0; 1024];

            let size = test.resp.serialize(&mut data).unwrap();

            assert_eq!(
                str::from_utf8(&data[..size]).unwrap(),
                test.expected,
                "test={}",
                test.name
            );
        }
    }

    #[test]
    fn test_req_parse() {
        let data = concat!(
            "T198:4:more,4:true!7:headers,34:30:12:Content-Type,10:text/pl",
            "ain,]]12:content-type,6:binary,4:from,6:client,2:id,1:1,6:met",
            "hod,4:POST,3:uri,19:https://example.com,7:credits,3:100#3:seq",
            ",1:0#4:body,5:hello,}"
        )
        .as_bytes();

        let mut scratch = ParseScratch::new();
        let req = Request::parse(&data, &mut scratch).unwrap();

        assert_eq!(req.from, b"client");
        assert_eq!(req.ids.len(), 1);
        assert_eq!(req.ids[0].id, b"1");
        assert_eq!(req.ids[0].seq, Some(0));

        let rdata = match req.ptype {
            RequestPacket::Data(data) => data,
            _ => panic!("expected data packet"),
        };

        assert_eq!(rdata.credits, 100);
        assert_eq!(rdata.more, true);
        assert_eq!(rdata.method, "POST");
        assert_eq!(rdata.uri, "https://example.com");
        assert_eq!(rdata.headers.len(), 1);
        assert_eq!(rdata.headers[0].name, "Content-Type");
        assert_eq!(rdata.headers[0].value, b"text/plain");
        assert_eq!(rdata.body, b"hello");

        let ctype = rdata.content_type.unwrap();
        assert_eq!(ctype, ContentType::Binary);
    }

    #[test]
    fn test_resp_parse() {
        let data = concat!(
            "T208:4:more,4:true!7:headers,34:30:12:Content-Type,10:text/pl",
            "ain,]]12:content-type,6:binary,4:from,6:server,2:id,1:1,6:rea",
            "son,2:OK,7:credits,3:100#9:user-data,12:3:foo,3:bar,}3:seq,1:",
            "0#4:code,3:200#4:body,5:hello,}"
        )
        .as_bytes();

        let mut scratch = ParseScratch::new();
        let resp = Response::parse(&data, &mut scratch).unwrap();

        assert_eq!(resp.from, b"server");
        assert_eq!(resp.ids.len(), 1);
        assert_eq!(resp.ids[0].id, b"1");
        assert_eq!(resp.ids[0].seq, Some(0));

        let rdata = match resp.ptype {
            ResponsePacket::Data(data) => data,
            _ => panic!("expected data packet"),
        };

        assert_eq!(rdata.credits, 100);
        assert_eq!(rdata.more, true);
        assert_eq!(rdata.code, 200);
        assert_eq!(rdata.reason, "OK");
        assert_eq!(rdata.headers.len(), 1);
        assert_eq!(rdata.headers[0].name, "Content-Type");
        assert_eq!(rdata.headers[0].value, b"text/plain");
        assert_eq!(rdata.body, b"hello");

        let ctype = rdata.content_type.unwrap();
        assert_eq!(ctype, ContentType::Binary);
    }

    #[test]
    fn test_owned_req_parse() {
        let data = concat!(
            "T198:4:more,4:true!7:headers,34:30:12:Content-Type,10:text/pl",
            "ain,]]12:content-type,6:binary,4:from,6:client,2:id,1:1,6:met",
            "hod,4:POST,3:uri,19:https://example.com,7:credits,3:100#3:seq",
            ",1:0#4:body,5:hello,}"
        )
        .as_bytes();

        let msg_memory = Arc::new(arena::ArcMemory::new(1));
        let scratch_memory = Rc::new(arena::RcMemory::new(1));

        let msg = arena::Arc::new(zmq::Message::from(data), &msg_memory).unwrap();
        let scratch = arena::Rc::new(RefCell::new(ParseScratch::new()), &scratch_memory).unwrap();

        let req = OwnedRequest::parse(msg, 0, scratch).unwrap();

        let req = req.get();

        assert_eq!(req.from, b"client");
        assert_eq!(req.ids.len(), 1);
        assert_eq!(req.ids[0].id, b"1");
        assert_eq!(req.ids[0].seq, Some(0));
    }

    #[test]
    fn test_owned_resp_parse() {
        let data = concat!(
            "addr T208:4:more,4:true!7:headers,34:30:12:Content-Type,10:te",
            "xt/plain,]]12:content-type,6:binary,4:from,6:server,2:id,1:1,",
            "6:reason,2:OK,7:credits,3:100#9:user-data,12:3:foo,3:bar,}3:s",
            "eq,1:0#4:code,3:200#4:body,5:hello,}"
        )
        .as_bytes();

        let msg_memory = Arc::new(arena::ArcMemory::new(1));
        let scratch_memory = Rc::new(arena::RcMemory::new(1));

        let msg = arena::Arc::new(zmq::Message::from(data), &msg_memory).unwrap();
        let scratch = arena::Rc::new(RefCell::new(ParseScratch::new()), &scratch_memory).unwrap();

        let resp = OwnedResponse::parse(msg, 5, scratch).unwrap();

        let resp = resp.get();

        assert_eq!(resp.from, b"server");
        assert_eq!(resp.ids.len(), 1);
        assert_eq!(resp.ids[0].id, b"1");
        assert_eq!(resp.ids[0].seq, Some(0));
    }
}
