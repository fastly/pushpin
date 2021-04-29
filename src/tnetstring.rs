/*
 * Copyright (C) 2020 Fanout, Inc.
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

use std::ascii;
use std::fmt;
use std::io;
use std::io::Write;
use std::str;

const F64_SIZE_MAX: usize = 64;
const OPS_MAX: usize = 1_000;

const TRUE_BYTES: &[u8] = b"true";
const FALSE_BYTES: &[u8] = b"false";

fn usize_len(value: usize) -> usize {
    let mut x = value;
    let mut len = 1;

    while x >= 10 {
        x /= 10;
        len += 1;
    }

    len
}

fn isize_len(value: isize) -> usize {
    let mut x = value;
    let mut len: usize = 1;

    if x < 0 {
        x = x.abs();
        len += 1;
    }

    while x >= 10 {
        x /= 10;
        len += 1;
    }

    len
}

fn f64_len(value: f64) -> usize {
    let mut buf = [0; F64_SIZE_MAX];
    let mut cursor = io::Cursor::new(&mut buf[..]);
    write!(&mut cursor, "{}", value).unwrap();

    cursor.position() as usize
}

fn bool_bytes(value: bool) -> &'static [u8] {
    if value {
        TRUE_BYTES
    } else {
        FALSE_BYTES
    }
}

fn bool_len(value: bool) -> usize {
    bool_bytes(value).len()
}

fn write_exact(w: &mut dyn io::Write, data: &[u8]) -> Result<(), io::Error> {
    let size = w.write(data)?;
    if size < data.len() {
        return Err(io::Error::from(io::ErrorKind::WriteZero));
    }

    Ok(())
}

#[derive(Copy, Clone, Debug, PartialEq)]
pub enum FrameType {
    Null,
    Bool,
    Int,
    Float,
    String,
    Array,
    Map,
}

impl fmt::Display for FrameType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let s: &str = (*self).into();
        write!(f, "{}", s)
    }
}

impl From<FrameType> for &str {
    fn from(ftype: FrameType) -> &'static str {
        match ftype {
            FrameType::Null => "null",
            FrameType::Bool => "bool",
            FrameType::Int => "int",
            FrameType::Float => "float",
            FrameType::String => "string",
            FrameType::Array => "array",
            FrameType::Map => "map",
        }
    }
}

#[derive(Debug, PartialEq)]
pub enum ParseError {
    UnexpectedEof,
    InvalidData,
    WrongType(FrameType, FrameType), // got, expected
    InvalidKey,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnexpectedEof => write!(f, "unexpected eof"),
            Self::InvalidData => write!(f, "invalid data"),
            Self::WrongType(got, expected) => {
                write!(f, "wrong type {}, expected {}", got, expected)
            }
            Self::InvalidKey => write!(f, "map key must be a utf-8 string"),
        }
    }
}

#[derive(Copy, Clone)]
enum Op<'a> {
    Invalid,
    #[allow(dead_code)]
    Null,
    Bool(bool),
    Int(isize),
    #[allow(dead_code)]
    Float(f64),
    String(&'a [u8]),
    StartArray,
    EndArray,
    StartMap,
    EndMap,
}

impl Op<'_> {
    fn serialize(&self, w: &mut dyn io::Write, len: usize) -> Result<(), io::Error> {
        match self {
            Op::Invalid => unreachable!(),
            Op::Null => {
                write!(w, "{}:~", len)?;
            }
            Op::Bool(b) => {
                write!(w, "{}:", len)?;
                write_exact(w, bool_bytes(*b))?;
                write_exact(w, b"!")?;
            }
            Op::Int(x) => {
                write!(w, "{}:{}#", len, *x)?;
            }
            Op::Float(x) => {
                write!(w, "{}:{}^", len, *x)?;
            }
            Op::String(s) => {
                write!(w, "{}:", len)?;
                write_exact(w, s)?;
                write_exact(w, b",")?;
            }
            Op::StartArray | Op::StartMap => {
                write!(w, "{}:", len)?;
            }
            Op::EndArray => {
                write_exact(w, b"]")?;
            }
            Op::EndMap => {
                write_exact(w, b"}")?;
            }
        }

        Ok(())
    }
}

// calculate the length of the first op and any dependants
// return the number of ops processed
fn calc_len(ops: &[Op], lens: &mut [usize]) -> usize {
    assert!(ops.len() > 0);
    assert_eq!(ops.len(), lens.len());

    let (len, count) = match ops[0] {
        Op::Invalid => unreachable!(),
        Op::Null => (0, 1),
        Op::Bool(b) => (bool_len(b), 1),
        Op::Int(x) => (isize_len(x), 1),
        Op::Float(x) => (f64_len(x), 1),
        Op::String(s) => (s.len(), 1),
        Op::StartArray => {
            let mut total = 0;
            let mut end = None;

            let mut i = 1;
            while i < ops.len() {
                match ops[i] {
                    Op::EndArray => {
                        end = Some(i + 1);
                        break;
                    }
                    Op::EndMap => panic!("unexpected EndMap"),
                    _ => {
                        let count = calc_len(&ops[i..], &mut lens[i..]);

                        let len = lens[i];
                        total += usize_len(len) + len + 2;

                        i += count;
                    }
                }
            }

            if end.is_none() {
                panic!("expected EndArray");
            }

            (total, end.unwrap())
        }
        Op::EndArray => panic!("EndArray without StartArray"),
        Op::StartMap => {
            let mut total = 0;
            let mut end = None;

            let mut i = 1;
            while i < ops.len() {
                match ops[i] {
                    Op::EndMap => {
                        end = Some(i + 1);
                        break;
                    }
                    Op::EndArray => panic!("unexpected EndArray"),
                    _ => {
                        let count = calc_len(&ops[i..], &mut lens[i..]);

                        let len = lens[i];
                        total += usize_len(len) + len + 2;

                        i += count;
                    }
                }
            }

            if end.is_none() {
                panic!("expected EndMap");
            }

            (total, end.unwrap())
        }
        Op::EndMap => panic!("EndMap without StartMap"),
    };

    lens[0] = len;

    count
}

pub struct Writer<'a> {
    ops: [Op<'a>; OPS_MAX],
    len: usize,
    dest: &'a mut dyn io::Write,
}

impl<'a> Writer<'a> {
    pub fn new(dest: &'a mut dyn io::Write) -> Self {
        Self {
            ops: [Op::Invalid; OPS_MAX],
            len: 0,
            dest,
        }
    }

    fn append(&mut self, op: Op<'a>) -> Result<(), io::Error> {
        if self.len >= self.ops.len() {
            return Err(io::Error::from(io::ErrorKind::WriteZero));
        }

        self.ops[self.len] = op;
        self.len += 1;

        Ok(())
    }

    pub fn write_null(&mut self) -> Result<(), io::Error> {
        self.append(Op::Null)
    }

    pub fn write_bool(&mut self, b: bool) -> Result<(), io::Error> {
        self.append(Op::Bool(b))
    }

    pub fn write_int(&mut self, x: isize) -> Result<(), io::Error> {
        self.append(Op::Int(x))
    }

    pub fn write_float(&mut self, x: f64) -> Result<(), io::Error> {
        self.append(Op::Float(x))
    }

    pub fn write_string(&mut self, s: &'a [u8]) -> Result<(), io::Error> {
        self.append(Op::String(s))
    }

    pub fn start_array(&mut self) -> Result<(), io::Error> {
        self.append(Op::StartArray)
    }

    pub fn end_array(&mut self) -> Result<(), io::Error> {
        self.append(Op::EndArray)
    }

    pub fn start_map(&mut self) -> Result<(), io::Error> {
        self.append(Op::StartMap)
    }

    pub fn end_map(&mut self) -> Result<(), io::Error> {
        self.append(Op::EndMap)
    }

    pub fn flush(&mut self) -> Result<(), io::Error> {
        let mut lens = [0; OPS_MAX];
        let mut i = 0;

        while i < self.len {
            let count = calc_len(&self.ops[i..self.len], &mut lens[i..self.len]);

            assert!(i + count <= self.len);

            for _ in 0..count {
                self.ops[i].serialize(self.dest, lens[i])?;
                self.ops[i] = Op::Invalid;
                i += 1;
            }
        }

        self.len = 0;

        Ok(())
    }
}

pub struct Frame<'a> {
    pub ftype: FrameType,
    pub data: &'a [u8],
}

pub fn parse_frame(src: &[u8]) -> Result<(Frame, usize), ParseError> {
    let mut size_end: Option<usize> = None;

    // find ':'
    for (i, &c) in src.iter().enumerate() {
        if c == b':' {
            size_end = Some(i);
            break;
        } else if !(c as char).is_digit(10) {
            return Err(ParseError::InvalidData);
        }
    }

    if size_end.is_none() {
        return Err(ParseError::UnexpectedEof);
    }

    let size_end = size_end.unwrap();

    let size = match str::from_utf8(&src[..size_end]) {
        Ok(size) => size,
        Err(_) => {
            return Err(ParseError::InvalidData);
        }
    };

    let size: usize = match size.parse() {
        Ok(size) => size,
        Err(_) => {
            return Err(ParseError::InvalidData);
        }
    };

    if size_end + size + 2 > src.len() {
        return Err(ParseError::UnexpectedEof);
    }

    let type_byte = src[size_end + 1 + size];

    let frame_type = match type_byte {
        b'~' => FrameType::Null,
        b'!' => FrameType::Bool,
        b'#' => FrameType::Int,
        b'^' => FrameType::Float,
        b',' => FrameType::String,
        b']' => FrameType::Array,
        b'}' => FrameType::Map,
        _ => {
            return Err(ParseError::InvalidData);
        }
    };

    let end = size_end + size + 2;

    Ok((
        Frame {
            ftype: frame_type,
            data: &src[(size_end + 1)..(size_end + 1 + size)],
        },
        end,
    ))
}

#[cfg(test)]
pub fn parse_null(src: &[u8]) -> Result<(), ParseError> {
    let (frame, _) = parse_frame(src)?;

    match frame.ftype {
        FrameType::Null => Ok(()),
        _ => Err(ParseError::WrongType(frame.ftype, FrameType::Null)),
    }
}

pub fn parse_bool(src: &[u8]) -> Result<bool, ParseError> {
    let (frame, _) = parse_frame(src)?;

    match frame.ftype {
        FrameType::Bool => {}
        _ => {
            return Err(ParseError::WrongType(frame.ftype, FrameType::Bool));
        }
    }

    match frame.data {
        TRUE_BYTES => Ok(true),
        FALSE_BYTES => Ok(false),
        _ => Err(ParseError::InvalidData),
    }
}

pub fn parse_int(src: &[u8]) -> Result<isize, ParseError> {
    let (frame, _) = parse_frame(src)?;

    match frame.ftype {
        FrameType::Int => {}
        _ => {
            return Err(ParseError::WrongType(frame.ftype, FrameType::Int));
        }
    }

    let x = match str::from_utf8(frame.data) {
        Ok(x) => x,
        Err(_) => {
            return Err(ParseError::InvalidData);
        }
    };

    match x.parse() {
        Ok(x) => Ok(x),
        Err(_) => Err(ParseError::InvalidData),
    }
}

#[cfg(test)]
pub fn parse_float(src: &[u8]) -> Result<f64, ParseError> {
    let (frame, _) = parse_frame(src)?;

    match frame.ftype {
        FrameType::Float => {}
        _ => {
            return Err(ParseError::WrongType(frame.ftype, FrameType::Float));
        }
    }

    let x = match str::from_utf8(frame.data) {
        Ok(x) => x,
        Err(_) => {
            return Err(ParseError::InvalidData);
        }
    };

    match x.parse() {
        Ok(x) => Ok(x),
        Err(_) => Err(ParseError::InvalidData),
    }
}

pub fn parse_string(src: &[u8]) -> Result<&[u8], ParseError> {
    let (frame, _) = parse_frame(src)?;

    match frame.ftype {
        FrameType::String => Ok(frame.data),
        _ => Err(ParseError::WrongType(frame.ftype, FrameType::String)),
    }
}

#[derive(Debug)]
pub struct SequenceItem<'a> {
    pub ftype: FrameType,
    pub data: &'a [u8],
}

#[derive(Debug)]
pub struct SequenceIterator<'a> {
    src: &'a [u8],
    pos: usize,
}

impl<'a> SequenceIterator<'a> {
    pub fn new(src: &'a [u8]) -> Self {
        Self { src: src, pos: 0 }
    }
}

impl<'a> Iterator for SequenceIterator<'a> {
    type Item = Result<SequenceItem<'a>, ParseError>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.pos >= self.src.len() {
            return None;
        }

        match parse_frame(&self.src[self.pos..]) {
            Ok((frame, end)) => {
                let next_pos = self.pos + end;

                let idata = &self.src[self.pos..next_pos];

                self.pos = next_pos;

                Some(Ok(SequenceItem {
                    ftype: frame.ftype,
                    data: idata,
                }))
            }
            Err(e) => {
                // make this the last iteration
                self.pos = self.src.len();

                Some(Err(e))
            }
        }
    }
}

pub fn parse_array(src: &[u8]) -> Result<SequenceIterator, ParseError> {
    let (frame, _) = parse_frame(src)?;

    match frame.ftype {
        FrameType::Array => Ok(SequenceIterator::new(frame.data)),
        _ => Err(ParseError::WrongType(frame.ftype, FrameType::Array)),
    }
}

#[derive(Debug)]
pub struct MapItem<'a> {
    pub key: &'a str,
    pub ftype: FrameType,
    pub data: &'a [u8],
}

#[derive(Copy, Clone, Debug)]
pub struct MapIterator<'a> {
    src: &'a [u8],
    pos: usize,
}

impl<'a> MapIterator<'a> {
    pub fn new(src: &'a [u8]) -> Self {
        Self { src, pos: 0 }
    }
}

impl<'a> Iterator for MapIterator<'a> {
    type Item = Result<MapItem<'a>, ParseError>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.pos >= self.src.len() {
            return None;
        }

        let (kframe, kend) = match parse_frame(&self.src[self.pos..]) {
            Ok(frame) => frame,
            Err(e) => {
                // make this the last iteration
                self.pos = self.src.len();

                return Some(Err(e));
            }
        };

        let kdata = match kframe.ftype {
            FrameType::String => kframe.data,
            _ => {
                // make this the last iteration
                self.pos = self.src.len();

                return Some(Err(ParseError::InvalidKey));
            }
        };

        let kstr = match str::from_utf8(kdata) {
            Ok(s) => s,
            Err(_) => {
                // make this the last iteration
                self.pos = self.src.len();

                return Some(Err(ParseError::InvalidKey));
            }
        };

        let vpos = self.pos + kend;

        let (vframe, vend) = match parse_frame(&self.src[vpos..]) {
            Ok(frame) => frame,
            Err(e) => {
                // make this the last iteration
                self.pos = self.src.len();

                return Some(Err(e));
            }
        };

        let next_pos = vpos + vend;

        let vdata = &self.src[vpos..next_pos];

        self.pos = next_pos;

        Some(Ok(MapItem {
            key: kstr,
            ftype: vframe.ftype,
            data: vdata,
        }))
    }
}

pub fn parse_map(src: &[u8]) -> Result<MapIterator, ParseError> {
    let (frame, _) = parse_frame(src)?;

    match frame.ftype {
        FrameType::Map => Ok(MapIterator::new(frame.data)),
        _ => Err(ParseError::WrongType(frame.ftype, FrameType::Map)),
    }
}

impl fmt::Display for Frame<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.ftype {
            FrameType::Map => {
                let it = MapIterator::new(self.data);

                write!(f, "{{ ")?;

                for (i, mi) in it.enumerate() {
                    let mi = match mi {
                        Ok(mi) => mi,
                        Err(_) => return Ok(()),
                    };

                    // can't fail
                    let (frame, _) = parse_frame(mi.data).unwrap();

                    if i > 0 {
                        write!(f, ", ")?;
                    }

                    write!(f, "\"{}\": {}", mi.key, frame)?;
                }

                write!(f, " }}")
            }
            FrameType::Array => {
                let it = SequenceIterator::new(self.data);

                write!(f, "[ ")?;

                for (i, si) in it.enumerate() {
                    let si = match si {
                        Ok(si) => si,
                        Err(_) => return Ok(()),
                    };

                    // can't fail
                    let (frame, _) = parse_frame(si.data).unwrap();

                    if i > 0 {
                        write!(f, ", ")?;
                    }

                    write!(f, "{}", frame)?;
                }

                write!(f, " ]")
            }
            FrameType::Null => write!(f, "null"),
            FrameType::Bool => match self.data {
                TRUE_BYTES => write!(f, "true"),
                FALSE_BYTES => write!(f, "false"),
                _ => write!(f, "<invalid bool>"),
            },
            FrameType::Int => {
                let x = match str::from_utf8(self.data) {
                    Ok(x) => x,
                    Err(_) => return write!(f, "<invalid int>"),
                };

                let x: isize = match x.parse() {
                    Ok(x) => x,
                    Err(_) => return write!(f, "<invalid int>"),
                };

                write!(f, "{}", x)
            }
            FrameType::Float => {
                let x = match str::from_utf8(self.data) {
                    Ok(x) => x,
                    Err(_) => return write!(f, "<invalid float>"),
                };

                let x: f64 = match x.parse() {
                    Ok(x) => x,
                    Err(_) => return write!(f, "<invalid float>"),
                };

                write!(f, "{}", x)
            }
            FrameType::String => {
                write!(f, "\"")?;

                for b in self.data {
                    write!(f, "{}", ascii::escape_default(*b))?;
                }

                write!(f, "\"")
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_length() {
        assert_eq!(usize_len(0), 1);
        assert_eq!(usize_len(1), 1);
        assert_eq!(usize_len(9), 1);
        assert_eq!(usize_len(10), 2);
        assert_eq!(usize_len(99), 2);
        assert_eq!(usize_len(100), 3);

        assert_eq!(isize_len(0), 1);
        assert_eq!(isize_len(1), 1);
        assert_eq!(isize_len(9), 1);
        assert_eq!(isize_len(10), 2);
        assert_eq!(isize_len(99), 2);
        assert_eq!(isize_len(100), 3);
        assert_eq!(isize_len(-1), 2);
        assert_eq!(isize_len(-9), 2);
        assert_eq!(isize_len(-10), 3);
    }

    #[test]
    fn test_null() {
        let mut buf = [0; 256];

        let mut cursor = io::Cursor::new(&mut buf[..]);
        let mut w = Writer::new(&mut cursor);
        w.write_null().unwrap();
        w.flush().unwrap();
        let end = cursor.position() as usize;

        assert_eq!(&buf[..end], b"0:~");

        let e = parse_null(b"bogus").unwrap_err();
        assert_eq!(e, ParseError::InvalidData);

        let e = parse_null(b"5:hello,").unwrap_err();
        assert_eq!(e, ParseError::WrongType(FrameType::String, FrameType::Null));

        let r = parse_null(b"0:~").unwrap();
        assert_eq!(r, ());
    }

    #[test]
    fn test_bool() {
        let mut buf = [0; 256];

        let mut cursor = io::Cursor::new(&mut buf[..]);
        let mut w = Writer::new(&mut cursor);
        w.write_bool(true).unwrap();
        w.flush().unwrap();
        let end = cursor.position() as usize;

        assert_eq!(&buf[..end], b"4:true!");

        let mut cursor = io::Cursor::new(&mut buf[..]);
        let mut w = Writer::new(&mut cursor);
        w.write_bool(false).unwrap();
        w.flush().unwrap();
        let end = cursor.position() as usize;

        assert_eq!(&buf[..end], b"5:false!");

        let e = parse_bool(b"bogus").unwrap_err();
        assert_eq!(e, ParseError::InvalidData);

        let e = parse_bool(b"5:hello,").unwrap_err();
        assert_eq!(e, ParseError::WrongType(FrameType::String, FrameType::Bool));

        let e = parse_bool(b"5:bogus!").unwrap_err();
        assert_eq!(e, ParseError::InvalidData);

        let b = parse_bool(b"4:true!").unwrap();
        assert_eq!(b, true);

        let b = parse_bool(b"5:false!").unwrap();
        assert_eq!(b, false);
    }

    #[test]
    fn test_int() {
        let mut buf = [0; 256];

        let mut cursor = io::Cursor::new(&mut buf[..]);
        let mut w = Writer::new(&mut cursor);
        w.write_int(0).unwrap();
        w.flush().unwrap();
        let end = cursor.position() as usize;

        assert_eq!(&buf[..end], b"1:0#");

        let mut cursor = io::Cursor::new(&mut buf[..]);
        let mut w = Writer::new(&mut cursor);
        w.write_int(42).unwrap();
        w.flush().unwrap();
        let end = cursor.position() as usize;

        assert_eq!(&buf[..end], b"2:42#");

        let mut cursor = io::Cursor::new(&mut buf[..]);
        let mut w = Writer::new(&mut cursor);
        w.write_int(-42).unwrap();
        w.flush().unwrap();
        let end = cursor.position() as usize;

        assert_eq!(&buf[..end], b"3:-42#");

        let e = parse_int(b"bogus").unwrap_err();
        assert_eq!(e, ParseError::InvalidData);

        let e = parse_int(b"5:hello,").unwrap_err();
        assert_eq!(e, ParseError::WrongType(FrameType::String, FrameType::Int));

        let e = parse_int(b"5:bogus#").unwrap_err();
        assert_eq!(e, ParseError::InvalidData);

        let x = parse_int(b"1:0#").unwrap();
        assert_eq!(x, 0);

        let x = parse_int(b"2:42#").unwrap();
        assert_eq!(x, 42);

        let x = parse_int(b"3:-42#").unwrap();
        assert_eq!(x, -42);
    }

    #[test]
    fn test_float() {
        let mut buf = [0; 256];

        let mut cursor = io::Cursor::new(&mut buf[..]);
        let mut w = Writer::new(&mut cursor);
        w.write_float(0.0).unwrap();
        w.flush().unwrap();
        let end = cursor.position() as usize;

        assert_eq!(&buf[..end], b"1:0^");

        let mut cursor = io::Cursor::new(&mut buf[..]);
        let mut w = Writer::new(&mut cursor);
        w.write_float(-0.5).unwrap();
        w.flush().unwrap();
        let end = cursor.position() as usize;

        assert_eq!(&buf[..end], b"4:-0.5^");

        let e = parse_float(b"bogus").unwrap_err();
        assert_eq!(e, ParseError::InvalidData);

        let e = parse_float(b"5:hello,").unwrap_err();
        assert_eq!(
            e,
            ParseError::WrongType(FrameType::String, FrameType::Float)
        );

        let e = parse_float(b"5:bogus^").unwrap_err();
        assert_eq!(e, ParseError::InvalidData);

        let x = parse_float(b"1:0^").unwrap();
        assert_eq!(x, 0.0);

        let x = parse_float(b"4:-0.5^").unwrap();
        assert_eq!(x, -0.5);
    }

    #[test]
    fn test_string() {
        let mut buf = [0; 256];

        let mut cursor = io::Cursor::new(&mut buf[..]);
        let mut w = Writer::new(&mut cursor);
        w.write_string(b"").unwrap();
        w.flush().unwrap();
        let end = cursor.position() as usize;

        assert_eq!(&buf[..end], b"0:,");

        let mut cursor = io::Cursor::new(&mut buf[..]);
        let mut w = Writer::new(&mut cursor);
        w.write_string(b"hello").unwrap();
        w.flush().unwrap();
        let end = cursor.position() as usize;

        assert_eq!(&buf[..end], b"5:hello,");

        let e = parse_string(b"bogus").unwrap_err();
        assert_eq!(e, ParseError::InvalidData);

        let e = parse_string(b"1:0#").unwrap_err();
        assert_eq!(e, ParseError::WrongType(FrameType::Int, FrameType::String));

        let s = parse_string(b"5:hello,").unwrap();
        assert_eq!(s, b"hello");
    }

    #[test]
    fn test_array() {
        let mut buf = [0; 256];
        let mut cursor = io::Cursor::new(&mut buf[..]);

        let mut w = Writer::new(&mut cursor);
        w.start_array().unwrap();
        w.write_string(b"foo").unwrap();
        w.write_string(b"bar").unwrap();
        w.end_array().unwrap();
        w.flush().unwrap();
        let end = cursor.position() as usize;

        assert_eq!(&buf[..end], b"12:3:foo,3:bar,]");

        let e = parse_array(b"bogus").unwrap_err();
        assert_eq!(e, ParseError::InvalidData);

        let e = parse_array(b"5:hello,").unwrap_err();
        assert_eq!(
            e,
            ParseError::WrongType(FrameType::String, FrameType::Array)
        );

        let mut it = parse_array(b"5:inner]").unwrap();
        let e = it.next().unwrap().unwrap_err();
        assert_eq!(e, ParseError::InvalidData);

        let mut it = parse_array(b"8:5:hello,]").unwrap();
        let si = it.next().unwrap().unwrap();
        assert_eq!(si.ftype, FrameType::String);
        assert_eq!(si.data, b"5:hello,");

        assert!(it.next().is_none());
    }

    #[test]
    fn test_map() {
        let mut buf = [0; 256];
        let mut cursor = io::Cursor::new(&mut buf[..]);

        let mut w = Writer::new(&mut cursor);
        w.start_map().unwrap();
        w.write_string(b"foo").unwrap();
        w.write_string(b"bar").unwrap();
        w.end_map().unwrap();
        w.flush().unwrap();
        let end = cursor.position() as usize;

        assert_eq!(&buf[..end], b"12:3:foo,3:bar,}");

        let e = parse_map(b"bogus").unwrap_err();
        assert_eq!(e, ParseError::InvalidData);

        let e = parse_map(b"5:hello,").unwrap_err();
        assert_eq!(e, ParseError::WrongType(FrameType::String, FrameType::Map));

        let mut it = parse_map(b"5:inner}").unwrap();
        let e = it.next().unwrap().unwrap_err();
        assert_eq!(e, ParseError::InvalidData);

        let mut it = parse_map(b"4:1:0#}").unwrap();
        let e = it.next().unwrap().unwrap_err();
        assert_eq!(e, ParseError::InvalidKey);

        let mut it = parse_map(b"4:1:\x80,}").unwrap();
        let e = it.next().unwrap().unwrap_err();
        assert_eq!(e, ParseError::InvalidKey);

        let mut it = parse_map(b"7:4:name,}").unwrap();
        let e = it.next().unwrap().unwrap_err();
        assert_eq!(e, ParseError::UnexpectedEof);

        let mut it = parse_map(b"8:4:name,X}").unwrap();
        let e = it.next().unwrap().unwrap_err();
        assert_eq!(e, ParseError::InvalidData);

        let mut it = parse_map(b"15:4:name,5:alice,}").unwrap();
        let mi = it.next().unwrap().unwrap();
        assert_eq!(mi.key, "name");
        assert_eq!(mi.ftype, FrameType::String);
        assert_eq!(mi.data, b"5:alice,");

        assert!(it.next().is_none());
    }

    #[test]
    fn test_sequence() {
        let mut buf = [0; 256];

        let mut cursor = io::Cursor::new(&mut buf[..]);
        let mut w = Writer::new(&mut cursor);
        w.write_string(b"apple").unwrap();
        w.write_string(b"banana").unwrap();
        w.flush().unwrap();
        let end = cursor.position() as usize;

        assert_eq!(&buf[..end], b"5:apple,6:banana,");

        let mut buf = [0; 10];

        let mut cursor = io::Cursor::new(&mut buf[..]);
        let mut w = Writer::new(&mut cursor);
        w.write_string(b"apple").unwrap();
        w.write_string(b"banana").unwrap();

        // won't fit
        let e = w.flush().unwrap_err();

        assert_eq!(e.kind(), io::ErrorKind::WriteZero);

        let mut it = SequenceIterator::new(b"5:apple,6:banana,");

        let s = parse_string(it.next().unwrap().unwrap().data).unwrap();
        assert_eq!(s, b"apple");

        let s = parse_string(it.next().unwrap().unwrap().data).unwrap();
        assert_eq!(s, b"banana");

        assert!(it.next().is_none());
    }

    #[test]
    fn test_overflow() {
        let mut buf = [0; 256];
        let mut cursor = io::Cursor::new(&mut buf[..]);
        let mut w = Writer::new(&mut cursor);
        for _ in 0..OPS_MAX {
            w.write_string(b"foo").unwrap();
        }

        // won't fit
        let e = w.write_string(b"foo").unwrap_err();
        assert_eq!(e.kind(), io::ErrorKind::WriteZero);
    }
}
