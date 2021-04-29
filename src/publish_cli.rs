/*
 * Copyright (C) 2021 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
 *
 * Pushpin is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

use crate::tnetstring;
use std::collections::HashMap;
use std::error::Error;
use std::fs;
use std::io;
use std::io::Read;

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
    fn write_to<'a>(&'a self, w: &mut tnetstring::Writer<'a>) -> Result<(), io::Error> {
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

        let mut w = tnetstring::Writer::new(&mut out);
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
                    let (http_content, ws_content) = if s.starts_with("@") {
                        let name = &s[1..];

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
                    let mut header = Vec::new();

                    header.push(TnValue::String(name.clone().into()));
                    header.push(TnValue::String(value.clone().into()));

                    headers.push(TnValue::Array(header));
                }

                http_response.insert("headers".into(), TnValue::Array(headers));
            }

            formats.insert("http-response".into(), TnValue::Map(http_response));
        }
        Action::Hint => {
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

    let message = TnValue::Map(item).serialize()?;

    let context = zmq::Context::new();
    let sock = context.socket(zmq::PUSH)?;
    sock.connect(&config.spec)?;
    sock.send(message, 0)?;

    println!("Published");

    Ok(())
}
