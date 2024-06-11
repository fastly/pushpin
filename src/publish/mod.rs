/*
 * Copyright (C) 2021-2023 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:APACHE2$
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
 *
 * $FANOUT_END_LICENSE$
 */

use crate::core::tnetstring;
use std::collections::HashMap;
use std::convert::TryInto;
use std::error::Error;
use std::fs;
use std::io;
use std::io::{BufRead, Read, Write};
use std::net;
use std::str;
use std::sync::Arc;

enum TnValue {
    Null,
    Bool(bool),
    Int(isize),
    Float(f64),
    String(Vec<u8>),
    Array(Vec<TnValue>),
    Map(HashMap<String, TnValue>),
}

impl TnValue {
    fn write_to<'a>(&'a self, w: &mut tnetstring::Writer<'a, '_>) -> Result<(), io::Error> {
        match self {
            Self::Null => w.write_null(),
            Self::Bool(b) => w.write_bool(*b),
            Self::Int(x) => w.write_int(*x),
            Self::Float(x) => w.write_float(*x),
            Self::String(s) => w.write_string(s),
            Self::Array(a) => {
                w.start_array()?;

                for v in a.iter() {
                    v.write_to(w)?;
                }

                w.end_array()
            }
            Self::Map(m) => {
                w.start_map()?;

                for (k, v) in m.iter() {
                    w.write_string(k.as_bytes())?;
                    v.write_to(w)?;
                }

                w.end_map()
            }
        }
    }

    pub fn serialize(&self) -> Result<Vec<u8>, io::Error> {
        let mut out = Vec::new();
        let mut cursor = io::Cursor::new(&mut out[..]);

        let mut w = tnetstring::Writer::new(&mut cursor);
        self.write_to(&mut w)?;

        w.flush()?;

        Ok(out)
    }
}

fn json_to_tnet(v: &serde_json::Value) -> Result<TnValue, io::Error> {
    match v {
        serde_json::Value::Null => Ok(TnValue::Null),
        serde_json::Value::Bool(b) => Ok(TnValue::Bool(*b)),
        serde_json::Value::Number(i) => {
            if i.is_i64() {
                Ok(TnValue::Int(i.as_i64().unwrap() as isize))
            } else if i.is_f64() {
                Ok(TnValue::Float(i.as_f64().unwrap()))
            } else {
                Err(io::Error::from(io::ErrorKind::InvalidData))
            }
        }
        serde_json::Value::String(s) => Ok(TnValue::String(s.clone().into_bytes())),
        serde_json::Value::Array(a) => {
            let mut out = Vec::new();

            for v in a {
                out.push(json_to_tnet(v)?);
            }

            Ok(TnValue::Array(out))
        }
        serde_json::Value::Object(m) => {
            let mut out = HashMap::new();

            for (k, v) in m {
                out.insert(k.clone(), json_to_tnet(v)?);
            }

            Ok(TnValue::Map(out))
        }
    }
}

fn tnet_to_json(v: &TnValue) -> Result<serde_json::Value, io::Error> {
    match v {
        TnValue::Null => Ok(serde_json::Value::Null),
        TnValue::Bool(b) => Ok(serde_json::Value::Bool(*b)),
        TnValue::Int(i) => {
            let num = serde_json::Number::from(*i);

            Ok(serde_json::Value::Number(num))
        }
        TnValue::Float(i) => {
            let num = serde_json::Number::from_f64(*i).unwrap();

            Ok(serde_json::Value::Number(num))
        }
        TnValue::String(s) => {
            let s = match str::from_utf8(s) {
                Ok(s) => s,
                Err(_) => return Err(io::Error::from(io::ErrorKind::InvalidData)),
            };

            Ok(serde_json::Value::String(s.to_string()))
        }
        TnValue::Array(a) => {
            let mut out = Vec::new();

            for v in a {
                out.push(tnet_to_json(v)?);
            }

            Ok(serde_json::Value::Array(out))
        }
        TnValue::Map(m) => {
            let mut out = serde_json::Map::new();

            for (k, v) in m {
                if let TnValue::String(s) = v {
                    if (k == "body" || k == "content") && str::from_utf8(s).is_err() {
                        let k = k.to_owned() + "-bin";
                        let v = base64::encode(s);
                        out.insert(k, serde_json::Value::String(v));
                        continue;
                    }
                }

                out.insert(k.clone(), tnet_to_json(v)?);
            }

            Ok(serde_json::Value::Object(out))
        }
    }
}

struct ParsedUrl {
    scheme: String,
    host: String,
    path: String,
    connect_host: String,
    connect_port: u16,
}

fn parse_url(url: &str) -> Result<ParsedUrl, io::Error> {
    let pos = match url.find(':') {
        Some(pos) => pos,
        None => return Err(io::Error::from(io::ErrorKind::InvalidData)),
    };

    let scheme = &url[..pos];

    let s = &url[(pos + 1)..];

    if !s.starts_with("//") {
        return Err(io::Error::from(io::ErrorKind::InvalidData));
    }

    let s = &s[2..];

    let pos = match s.find('/') {
        Some(pos) => pos,
        None => s.len(),
    };

    let host = &s[..pos];
    let path = &s[pos..];

    let (connect_host, connect_port) = match host.find(':') {
        Some(pos) => {
            let port = &host[(pos + 1)..];

            let port = match port.parse() {
                Ok(x) => x,
                Err(_) => return Err(io::Error::from(io::ErrorKind::InvalidData)),
            };

            (&host[..pos], port)
        }
        None => {
            let port = if scheme == "https" { 443 } else { 80 };

            (host, port)
        }
    };

    Ok(ParsedUrl {
        scheme: scheme.into(),
        host: host.into(),
        path: path.into(),
        connect_host: connect_host.into(),
        connect_port,
    })
}

struct TlsStream {
    stream: rustls::StreamOwned<rustls::ClientConnection, net::TcpStream>,
}

impl TlsStream {
    fn new(stream: net::TcpStream, host: &str) -> Result<Self, Box<dyn Error>> {
        let mut root_store = rustls::RootCertStore::empty();

        for cert in rustls_native_certs::load_native_certs()? {
            root_store.add(&rustls::Certificate(cert.0)).unwrap();
        }

        let config = rustls::ClientConfig::builder()
            .with_safe_defaults()
            .with_root_certificates(root_store)
            .with_no_client_auth();

        let server_name = host.try_into()?;

        let client = rustls::ClientConnection::new(Arc::new(config), server_name)?;

        Ok(Self {
            stream: rustls::StreamOwned::new(client, stream),
        })
    }
}

impl Read for TlsStream {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, io::Error> {
        match self.stream.read(buf) {
            Ok(ret) => Ok(ret),
            Err(e) if e.kind() == io::ErrorKind::ConnectionAborted => Ok(0),
            Err(e) => Err(e),
        }
    }
}

impl Write for TlsStream {
    fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
        self.stream.write(buf)
    }

    fn flush(&mut self) -> Result<(), io::Error> {
        self.stream.flush()
    }
}

enum Stream {
    Plain(net::TcpStream),
    Tls(Box<TlsStream>),
}

impl Read for Stream {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, io::Error> {
        match self {
            Self::Plain(stream) => stream.read(buf),
            Self::Tls(stream) => stream.read(buf),
        }
    }
}

impl Write for Stream {
    fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
        match self {
            Self::Plain(stream) => stream.write(buf),
            Self::Tls(stream) => stream.write(buf),
        }
    }

    fn flush(&mut self) -> Result<(), io::Error> {
        match self {
            Self::Plain(stream) => stream.flush(),
            Self::Tls(stream) => stream.flush(),
        }
    }
}

fn publish_http(
    base_url: &str,
    basic_auth: Option<&str>,
    item: &serde_json::Value,
) -> Result<(), Box<dyn Error>> {
    let parsed_url = match parse_url(base_url) {
        Ok(p) => p,
        Err(_) => return Err("invalid URL".into()),
    };

    let path = parsed_url.path + "/publish/";

    let items = vec![item.clone()];

    let mut data = serde_json::Map::new();
    data.insert("items".into(), serde_json::Value::Array(items));

    let data = serde_json::Value::Object(data);

    let body = data.to_string().into_bytes();

    let mut req = format!(
        "POST {} HTTP/1.0\r\n\
        Host: {}\r\n\
        Content-Type: application/json\r\n\
        Content-Length: {}\r\n",
        path,
        parsed_url.host,
        body.len()
    );

    if let Some(s) = basic_auth {
        if parsed_url.scheme != "https" {
            return Err("Authentication requires https".into());
        }

        req.push_str(&format!("Authorization: Basic {}\r\n", base64::encode(s)));
    }

    req += "\r\n";

    let stream =
        net::TcpStream::connect((parsed_url.connect_host.as_str(), parsed_url.connect_port))?;

    let mut stream = if parsed_url.scheme == "https" {
        Stream::Tls(Box::new(TlsStream::new(stream, &parsed_url.host)?))
    } else {
        Stream::Plain(stream)
    };

    stream.write_all(req.as_bytes())?;

    stream.write_all(&body)?;

    let mut reader = io::BufReader::new(&mut stream);

    let mut first = true;
    let mut err = true;

    loop {
        let mut line = String::new();

        let size = reader.read_line(&mut line)?;

        if size == 0 {
            return Err(io::Error::from(io::ErrorKind::UnexpectedEof).into());
        }

        let line = line.trim();

        if first {
            first = false;

            let pos = match line.find(' ') {
                Some(pos) => pos,
                None => return Err(io::Error::from(io::ErrorKind::InvalidData).into()),
            };

            let rest = &line[(pos + 1)..];

            let pos = match rest.find(' ') {
                Some(pos) => pos,
                None => return Err(io::Error::from(io::ErrorKind::InvalidData).into()),
            };

            let code = &rest[..pos];

            let code: u16 = match code.parse() {
                Ok(x) => x,
                Err(_) => return Err(io::Error::from(io::ErrorKind::InvalidData).into()),
            };

            if code == 200 {
                err = false;
            }
        }

        if line.is_empty() {
            break;
        }
    }

    let mut resp = String::new();

    reader.read_to_string(&mut resp)?;

    if err {
        return Err(resp.trim().into());
    }

    Ok(())
}

fn publish_zmq(spec: &str, item: &TnValue) -> Result<(), Box<dyn Error>> {
    let message = item.serialize()?;

    let context = zmq::Context::new();
    let sock = context.socket(zmq::PUSH)?;
    sock.connect(spec)?;
    sock.send(message, 0)?;

    Ok(())
}

pub enum Content {
    Value(String),
    Patch(Vec<serde_json::Value>),
}

pub struct Message {
    pub code: u16,
    pub content: Content,
}

pub enum Action {
    Send(Message),
    Hint,
    Close,
}

pub struct Config {
    pub spec: String,
    pub basic_auth: Option<String>,
    pub channel: String,
    pub id: String,
    pub prev_id: String,
    pub sender: String,
    pub action: Action,
    pub headers: Vec<(String, String)>,
    pub meta: Vec<(String, String)>,
    pub no_seq: bool,
    pub eol: bool,
}

pub fn run(config: &Config) -> Result<(), Box<dyn Error>> {
    let mut formats = HashMap::new();

    match &config.action {
        Action::Send(msg) => {
            let mut http_response = HashMap::new();

            http_response.insert("code".into(), TnValue::Int(msg.code as isize));

            match &msg.content {
                Content::Value(s) => {
                    let (http_content, ws_content) = if let Some(name) = s.strip_prefix('@') {
                        let mut f = match fs::File::open(name) {
                            Ok(f) => f,
                            Err(e) => return Err(format!("can't read file {}: {}", name, e).into()),
                        };

                        let mut http_content = Vec::new();

                        f.read_to_end(&mut http_content)?;

                        let ws_content = http_content.clone();

                        (http_content, ws_content)
                    } else {
                        let mut http_content = s.clone();

                        let ws_content = http_content.clone();

                        if config.eol {
                            http_content += "\n";
                        }

                        (http_content.into_bytes(), ws_content.into_bytes())
                    };

                    http_response.insert("body".into(), TnValue::String(http_content.clone()));

                    let mut http_stream = HashMap::new();
                    http_stream.insert("content".into(), TnValue::String(http_content));
                    formats.insert("http-stream".into(), TnValue::Map(http_stream));

                    let mut ws_message = HashMap::new();
                    ws_message.insert("content".into(), TnValue::String(ws_content));
                    formats.insert("ws-message".into(), TnValue::Map(ws_message));
                }
                Content::Patch(arr) => {
                    let mut patch = Vec::new();

                    for op in arr {
                        patch.push(json_to_tnet(op)?);
                    }

                    http_response.insert("body-patch".into(), TnValue::Array(patch));
                }
            }

            if !config.headers.is_empty() {
                let mut headers = Vec::new();

                for (name, value) in config.headers.iter() {
                    let header = vec![
                        TnValue::String(name.clone().into()),
                        TnValue::String(value.clone().into()),
                    ];

                    headers.push(TnValue::Array(header));
                }

                http_response.insert("headers".into(), TnValue::Array(headers));
            }

            formats.insert("http-response".into(), TnValue::Map(http_response));
        }
        Action::Hint => {
            let mut http_response = HashMap::new();
            http_response.insert("action".into(), TnValue::String("hint".into()));
            formats.insert("http-response".into(), TnValue::Map(http_response));

            let mut http_stream = HashMap::new();
            http_stream.insert("action".into(), TnValue::String("hint".into()));
            formats.insert("http-stream".into(), TnValue::Map(http_stream));

            let mut ws_message = HashMap::new();
            ws_message.insert("action".into(), TnValue::String("hint".into()));
            formats.insert("ws-message".into(), TnValue::Map(ws_message));
        }
        Action::Close => {
            let mut http_stream = HashMap::new();
            http_stream.insert("action".into(), TnValue::String("close".into()));
            formats.insert("http-stream".into(), TnValue::Map(http_stream));

            let mut ws_message = HashMap::new();
            ws_message.insert("action".into(), TnValue::String("close".into()));
            formats.insert("ws-message".into(), TnValue::Map(ws_message));
        }
    };

    let mut meta = HashMap::new();

    if !config.sender.is_empty() {
        meta.insert(
            "sender".into(),
            TnValue::String(config.sender.clone().into()),
        );
    }

    for (name, value) in config.meta.iter() {
        meta.insert(name.clone(), TnValue::String(value.clone().into()));
    }

    let mut item = HashMap::new();

    item.insert(
        "channel".into(),
        TnValue::String(config.channel.clone().into()),
    );

    if !config.id.is_empty() {
        item.insert("id".into(), TnValue::String(config.id.clone().into()));
    }

    if !config.prev_id.is_empty() {
        item.insert(
            "prev-id".into(),
            TnValue::String(config.prev_id.clone().into()),
        );
    }

    item.insert("formats".into(), TnValue::Map(formats));

    if !meta.is_empty() {
        item.insert("meta".into(), TnValue::Map(meta));
    }

    if config.no_seq {
        item.insert("no-seq".into(), TnValue::Bool(true));
    }

    let item = TnValue::Map(item);

    if config.spec.starts_with("https:") || config.spec.starts_with("http:") {
        let item = tnet_to_json(&item)?;

        let basic_auth = config.basic_auth.as_deref();

        publish_http(&config.spec, basic_auth, &item)?;
    } else {
        publish_zmq(&config.spec, &item)?;
    }

    println!("Published");

    Ok(())
}
