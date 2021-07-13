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

use crate::arena;
use crate::tnetstring;
use std::cell::RefCell;
use std::fmt;
use std::io;
use std::mem;
use std::str;

pub const IDS_MAX: usize = 128;

const HEADERS_MAX: usize = 32;

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
        }
    }
}

pub struct ResponseData<'buf, 'headers> {
    pub credits: u32,
    pub more: bool,
    pub code: u32,
    pub reason: &'buf str,
    pub headers: &'headers [Header<'buf>],
    pub content_type: Option<ContentType>, // websocket
    pub body: &'buf [u8],
}

pub struct RejectedInfo<'buf, 'headers> {
    pub code: u32,
    pub reason: &'buf str,
    pub headers: &'headers [Header<'buf>],
    pub body: &'buf [u8],
}

pub struct ErrorData<'buf, 'headers> {
    pub condition: &'buf str,
    pub rejected_info: Option<RejectedInfo<'buf, 'headers>>, // rejected (websocket)
}

pub struct CreditData {
    pub credits: u32,
}

pub struct CloseData<'a> {
    // code, reason
    pub status: Option<(u16, &'a str)>,
}

pub struct PingData<'a> {
    pub credits: u32,
    pub body: &'a [u8],
}

pub struct PongData<'a> {
    pub credits: u32,
    pub body: &'a [u8],
}

pub enum RequestPacket<'buf, 'headers> {
    Data(RequestData<'buf, 'headers>),
    Error(ErrorData<'buf, 'headers>),
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
    Error(ErrorData<'buf, 'headers>),
    Credit(CreditData),
    KeepAlive,
    Cancel,
    HandoffStart,
    HandoffProceed,
    Close(CloseData<'buf>),
    Ping(PingData<'buf>),
    Pong(PongData<'buf>),
}

pub struct Request<'buf, 'ids, 'headers> {
    pub from: &'buf [u8],
    pub ids: &'ids [Id<'buf>],
    pub multi: bool,
    pub ptype: RequestPacket<'buf, 'headers>,
}

impl<'buf, 'ids, 'headers> Request<'buf, 'ids, 'headers> {
    pub fn new_data(
        from: &'buf [u8],
        ids: &'ids [Id<'buf>],
        data: RequestData<'buf, 'headers>,
    ) -> Self {
        Self {
            from: from,
            ids: ids,
            multi: false,
            ptype: RequestPacket::Data(data),
        }
    }

    pub fn new_error(from: &'buf [u8], ids: &'ids [Id<'buf>], condition: &'buf str) -> Self {
        Self {
            from: from,
            ids: ids,
            multi: false,
            ptype: RequestPacket::Error(ErrorData {
                condition,
                rejected_info: None,
            }),
        }
    }

    pub fn new_credit(from: &'buf [u8], ids: &'ids [Id<'buf>], credits: u32) -> Self {
        Self {
            from: from,
            ids: ids,
            multi: false,
            ptype: RequestPacket::Credit(CreditData { credits }),
        }
    }

    pub fn new_keep_alive(from: &'buf [u8], ids: &'ids [Id<'buf>]) -> Self {
        Self {
            from: from,
            ids: ids,
            multi: false,
            ptype: RequestPacket::KeepAlive,
        }
    }

    pub fn new_cancel(from: &'buf [u8], ids: &'ids [Id<'buf>]) -> Self {
        Self {
            from: from,
            ids: ids,
            multi: false,
            ptype: RequestPacket::Cancel,
        }
    }

    pub fn new_handoff_start(from: &'buf [u8], ids: &'ids [Id<'buf>]) -> Self {
        Self {
            from: from,
            ids: ids,
            multi: false,
            ptype: RequestPacket::HandoffStart,
        }
    }

    pub fn new_handoff_proceed(from: &'buf [u8], ids: &'ids [Id<'buf>]) -> Self {
        Self {
            from: from,
            ids: ids,
            multi: false,
            ptype: RequestPacket::HandoffProceed,
        }
    }

    pub fn new_close(
        from: &'buf [u8],
        ids: &'ids [Id<'buf>],
        status: Option<(u16, &'buf str)>,
    ) -> Self {
        Self {
            from: from,
            ids: ids,
            multi: false,
            ptype: RequestPacket::Close(CloseData { status }),
        }
    }

    pub fn new_ping(from: &'buf [u8], ids: &'ids [Id<'buf>], body: &'buf [u8]) -> Self {
        Self {
            from: from,
            ids: ids,
            multi: false,
            ptype: RequestPacket::Ping(PingData { credits: 0, body }),
        }
    }

    pub fn new_pong(from: &'buf [u8], ids: &'ids [Id<'buf>], body: &'buf [u8]) -> Self {
        Self {
            from: from,
            ids: ids,
            multi: false,
            ptype: RequestPacket::Pong(PongData { credits: 0, body }),
        }
    }

    pub fn serialize(&self, dest: &mut [u8]) -> Result<usize, io::Error> {
        if dest.len() < 1 {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        dest[0] = b'T';

        let mut cursor = io::Cursor::new(&mut dest[1..]);
        let mut w = tnetstring::Writer::new(&mut cursor);

        w.start_map()?;

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

        match &self.ptype {
            RequestPacket::Data(data) => {
                if !data.method.is_empty() {
                    w.write_string(b"method")?;
                    w.write_string(data.method.as_bytes())?;
                }

                if !data.uri.is_empty() {
                    w.write_string(b"uri")?;
                    w.write_string(data.uri.as_bytes())?;
                }

                if !data.headers.is_empty() {
                    w.write_string(b"headers")?;
                    w.start_array()?;

                    for h in data.headers.iter() {
                        w.start_array()?;
                        w.write_string(h.name.as_bytes())?;
                        w.write_string(h.value)?;
                        w.end_array()?;
                    }

                    w.end_array()?;
                }

                if let Some(ctype) = &data.content_type {
                    w.write_string(b"content-type")?;

                    let s: &[u8] = match ctype {
                        ContentType::Text => b"text",
                        ContentType::Binary => b"binary",
                    };

                    w.write_string(s)?;
                }

                if !data.body.is_empty() {
                    w.write_string(b"body")?;
                    w.write_string(&data.body)?;
                }

                if data.credits > 0 {
                    w.write_string(b"credits")?;
                    w.write_int(data.credits as isize)?;
                }

                if data.more {
                    w.write_string(b"more")?;
                    w.write_bool(true)?;
                }

                if data.stream {
                    w.write_string(b"stream")?;
                    w.write_bool(true)?;
                }

                if data.max_size > 0 {
                    w.write_string(b"max_size")?;
                    w.write_int(data.max_size as isize)?;
                }

                if data.timeout > 0 {
                    w.write_string(b"timeout")?;
                    w.write_int(data.timeout as isize)?;
                }

                if !data.peer_address.is_empty() {
                    w.write_string(b"peer-address")?;
                    w.write_string(data.peer_address.as_bytes())?;

                    w.write_string(b"peer-port")?;
                    w.write_int(data.peer_port as isize)?;
                }
            }
            RequestPacket::Error(data) => {
                w.write_string(b"type")?;
                w.write_string(b"error")?;

                w.write_string(b"condition")?;
                w.write_string(data.condition.as_bytes())?;
            }
            RequestPacket::Credit(data) => {
                w.write_string(b"type")?;
                w.write_string(b"credit")?;

                w.write_string(b"credits")?;
                w.write_int(data.credits as isize)?;
            }
            RequestPacket::KeepAlive => {
                w.write_string(b"type")?;
                w.write_string(b"keep-alive")?;
            }
            RequestPacket::Cancel => {
                w.write_string(b"type")?;
                w.write_string(b"cancel")?;
            }
            RequestPacket::HandoffStart => {
                w.write_string(b"type")?;
                w.write_string(b"handoff-start")?;
            }
            RequestPacket::HandoffProceed => {
                w.write_string(b"type")?;
                w.write_string(b"handoff-proceed")?;
            }
            RequestPacket::Close(data) => {
                w.write_string(b"type")?;
                w.write_string(b"close")?;

                if let Some(status) = data.status {
                    w.write_string(b"code")?;
                    w.write_int(status.0 as isize)?;

                    if !status.1.is_empty() {
                        w.write_string(b"body")?;
                        w.write_string(status.1.as_bytes())?;
                    }
                }
            }
            RequestPacket::Ping(data) => {
                w.write_string(b"type")?;
                w.write_string(b"ping")?;

                if !data.body.is_empty() {
                    w.write_string(b"body")?;
                    w.write_string(&data.body)?;
                }
            }
            RequestPacket::Pong(data) => {
                w.write_string(b"type")?;
                w.write_string(b"pong")?;

                if !data.body.is_empty() {
                    w.write_string(b"body")?;
                    w.write_string(&data.body)?;
                }
            }
        }

        w.end_map()?;

        w.flush()?;

        Ok((cursor.position() as usize) + 1)
    }
}

#[derive(Debug, PartialEq)]
pub enum ParseError {
    Unrecognized,
    TnetParse(tnetstring::ParseError),
    WrongType(&'static str, tnetstring::FrameType),
    NotMapOrString(&'static str),
    NotUtf8(&'static str),
    NegativeInt(&'static str),
    TooManyIds,
    TooManyHeaders,
    InvalidHeader,
    NoId,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Unrecognized => write!(f, "unrecognized data format"),
            Self::TnetParse(e) => e.fmt(f),
            Self::WrongType(field, expected) => write!(f, "{} must be of type {}", field, expected),
            Self::NotMapOrString(field) => write!(f, "{} must be of type map or string", field),
            Self::NotUtf8(field) => write!(f, "{} must be a utf-8 string", field),
            Self::NegativeInt(field) => write!(f, "{} must not be negative", field),
            Self::TooManyIds => write!(f, "too many ids"),
            Self::TooManyHeaders => write!(f, "too many headers"),
            Self::InvalidHeader => write!(f, "header item must have size 2"),
            Self::NoId => write!(f, "no id"),
        }
    }
}

impl From<tnetstring::ParseError> for ParseError {
    fn from(e: tnetstring::ParseError) -> Self {
        Self::TnetParse(e)
    }
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

pub struct ResponseScratch<'a> {
    pub ids: [Id<'a>; IDS_MAX],
    pub headers: [Header<'a>; HEADERS_MAX],
}

impl ResponseScratch<'_> {
    pub fn new() -> Self {
        Self {
            ids: [EMPTY_ID; IDS_MAX],
            headers: [EMPTY_HEADER; HEADERS_MAX],
        }
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
    pub fn parse_ids(
        src: &'buf [u8],
        scratch: &'scratch mut ResponseScratch<'buf>,
    ) -> Result<&'scratch [Id<'buf>], ParseError> {
        if src.len() < 1 || src[0] != b'T' {
            return Err(ParseError::Unrecognized);
        }

        let root = tnetstring::parse_map(&src[1..]).field("root")?;

        let mut ids_pos = 0;

        for e in root.clone() {
            let e = e?;

            match e.key {
                "id" => {
                    match e.ftype {
                        tnetstring::FrameType::Array => {
                            for idm in tnetstring::parse_array(e.data)? {
                                let idm = idm?;

                                if ids_pos >= scratch.ids.len() {
                                    return Err(ParseError::TooManyIds);
                                }

                                for m in tnetstring::parse_map(idm.data)? {
                                    let m = m?;

                                    match m.key {
                                        "id" => {
                                            let s = tnetstring::parse_string(m.data).field("id")?;

                                            scratch.ids[ids_pos].id = s;
                                        }
                                        _ => {} // skip other fields
                                    }
                                }

                                ids_pos += 1;
                            }
                        }
                        tnetstring::FrameType::String => {
                            let s = tnetstring::parse_string(e.data)?;

                            scratch.ids[0].id = s;

                            ids_pos = 1;
                        }
                        _ => {
                            return Err(ParseError::NotMapOrString("id"));
                        }
                    }

                    return Ok(&scratch.ids[..ids_pos]);
                }
                _ => {} // skip other fields
            }
        }

        Ok(&scratch.ids[..ids_pos])
    }

    pub fn parse(
        src: &'buf [u8],
        scratch: &'scratch mut ResponseScratch<'buf>,
    ) -> Result<Response<'buf, 'scratch, 'scratch>, ParseError> {
        if src.len() < 1 || src[0] != b'T' {
            return Err(ParseError::Unrecognized);
        }

        let root = tnetstring::parse_map(&src[1..]).field("root")?;

        // first, read the common fields

        let mut from = EMPTY_BYTES;
        let mut ids_pos = 0;
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

                                if ids_pos >= scratch.ids.len() {
                                    return Err(ParseError::TooManyIds);
                                }

                                for m in tnetstring::parse_map(idm.data)? {
                                    let m = m?;

                                    match m.key {
                                        "id" => {
                                            let s = tnetstring::parse_string(m.data).field("id")?;

                                            scratch.ids[ids_pos].id = s;
                                        }
                                        "seq" => {
                                            let x = tnetstring::parse_int(m.data).field("seq")?;

                                            if x < 0 {
                                                return Err(ParseError::NegativeInt("seq"));
                                            }

                                            scratch.ids[ids_pos].seq = Some(x as u32);
                                        }
                                        _ => {} // skip unknown fields
                                    }
                                }

                                ids_pos += 1;
                            }
                        }
                        tnetstring::FrameType::String => {
                            let s = tnetstring::parse_string(e.data)?;

                            scratch.ids[0].id = s;

                            ids_pos = 1;
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

                    scratch.ids[0].seq = Some(x as u32);
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

        let ptype = match ptype_str {
            // data
            "" => {
                let mut credits = 0;
                let mut more = false;
                let mut code = 0;
                let mut reason = "";
                let mut headers_pos = 0;
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

                            code = x as u32;
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

                                if headers_pos >= scratch.headers.len() {
                                    return Err(ParseError::TooManyHeaders);
                                }

                                let mut hi =
                                    tnetstring::parse_array(ha.data).field("header item")?;

                                let name = match hi.next() {
                                    Some(Ok(name)) => name,
                                    Some(Err(e)) => {
                                        return Err(e.into());
                                    }
                                    None => {
                                        return Err(ParseError::InvalidHeader);
                                    }
                                };

                                let name =
                                    tnetstring::parse_string(name.data).field("header name")?;

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

                                let value =
                                    tnetstring::parse_string(value.data).field("header value")?;

                                scratch.headers[headers_pos] = Header { name, value };

                                headers_pos += 1;
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

                ResponsePacket::Data(ResponseData {
                    credits,
                    more,
                    code,
                    reason,
                    headers: &scratch.headers[..headers_pos],
                    content_type,
                    body,
                })
            }
            "error" => {
                let mut condition = "";
                let mut code = 0;
                let mut reason = "";
                let mut headers_pos = 0;
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

                            code = x as u32;
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

                                if headers_pos >= scratch.headers.len() {
                                    return Err(ParseError::TooManyHeaders);
                                }

                                let mut hi =
                                    tnetstring::parse_array(ha.data).field("header item")?;

                                let name = match hi.next() {
                                    Some(Ok(name)) => name,
                                    Some(Err(e)) => {
                                        return Err(e.into());
                                    }
                                    None => {
                                        return Err(ParseError::InvalidHeader);
                                    }
                                };

                                let name =
                                    tnetstring::parse_string(name.data).field("header name")?;

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

                                let value =
                                    tnetstring::parse_string(value.data).field("header value")?;

                                scratch.headers[headers_pos] = Header { name, value };

                                headers_pos += 1;
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
                        headers: &scratch.headers[..headers_pos],
                        body,
                    })
                } else {
                    None
                };

                ResponsePacket::Error(ErrorData {
                    condition,
                    rejected_info,
                })
            }
            "credit" => {
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

                ResponsePacket::Credit(CreditData { credits })
            }
            "keep-alive" => ResponsePacket::KeepAlive,
            "cancel" => ResponsePacket::Cancel,
            "handoff-start" => ResponsePacket::HandoffStart,
            "handoff-proceed" => ResponsePacket::HandoffProceed,
            "close" => {
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
                    ResponsePacket::Close(CloseData {
                        status: Some((code, reason)),
                    })
                } else {
                    ResponsePacket::Close(CloseData { status: None })
                }
            }
            "ping" | "pong" => {
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

                match ptype_str {
                    "ping" => ResponsePacket::Ping(PingData { credits, body }),
                    "pong" => ResponsePacket::Pong(PongData { credits, body }),
                    _ => unreachable!(),
                }
            }
            _ => ResponsePacket::Unknown,
        };

        Ok(Response {
            from,
            ids: &scratch.ids[..ids_pos],
            multi,
            ptype,
            ptype_str,
        })
    }
}

pub struct OwnedResponse {
    _src: arena::Arc<zmq::Message>,
    _scratch: arena::Rc<RefCell<ResponseScratch<'static>>>,
    resp: Response<'static, 'static, 'static>,
}

impl OwnedResponse {
    pub fn parse(
        src: arena::Arc<zmq::Message>,
        offset: usize,
        scratch: arena::Rc<RefCell<ResponseScratch<'static>>>,
    ) -> Result<Self, ParseError> {
        let src_ref: &[u8] = &src.get()[offset..];

        // safety: Self will take ownership of src, and the bytes referred to
        // by src_ref are on the heap, and src will not be modified or
        // dropped until Self is dropped, so the bytes referred to by src_ref
        // will remain valid for the lifetime of Self
        let src_ref: &'static [u8] = unsafe { mem::transmute(src_ref) };

        // safety: Self will take ownership of scratch, and the location
        // referred to by scratch_mut is in an arena, and scratch will not
        // be dropped until Self is dropped, so the location referred to by
        // scratch_mut will remain valid for the lifetime of Self
        //
        // further, it is safe for Response::parse() to write references to
        // src_ref into scratch_mut, because src_ref and scratch_mut have
        // the same lifetime
        let scratch_mut: &'static mut ResponseScratch<'static> =
            unsafe { scratch.get().as_ptr().as_mut().unwrap() };

        let resp = Response::parse(src_ref, scratch_mut)?;

        Ok(Self {
            _src: src,
            _scratch: scratch,
            resp,
        })
    }

    pub fn get<'a>(&'a self) -> &'a Response<'a, 'a, 'a> {
        let resp = &self.resp;

        // safety: here we simply reduce the inner lifetimes to that of the owning
        // object, which is fine
        let resp: &'a Response<'a, 'a, 'a> = unsafe { mem::transmute(resp) };

        resp
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::rc::Rc;
    use std::sync::Arc;

    #[test]
    fn test_serialize() {
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
                    }),
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
                    ptype: RequestPacket::Error(ErrorData {
                        condition: "bad-request",
                        rejected_info: None,
                    }),
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
    fn test_parse() {
        let data = concat!(
            "T208:4:more,4:true!7:headers,34:30:12:Content-Type,10:text/pl",
            "ain,]]12:content-type,6:binary,4:from,6:server,2:id,1:1,6:rea",
            "son,2:OK,7:credits,3:100#9:user-data,12:3:foo,3:bar,}3:seq,1:",
            "0#4:code,3:200#4:body,5:hello,}"
        )
        .as_bytes();

        let mut scratch = ResponseScratch::new();
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
    fn test_owned_parse() {
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
        let scratch =
            arena::Rc::new(RefCell::new(ResponseScratch::new()), &scratch_memory).unwrap();

        let resp = OwnedResponse::parse(msg, 5, scratch).unwrap();

        let resp = resp.get();

        assert_eq!(resp.from, b"server");
        assert_eq!(resp.ids.len(), 1);
        assert_eq!(resp.ids[0].id, b"1");
        assert_eq!(resp.ids[0].seq, Some(0));
    }
}
