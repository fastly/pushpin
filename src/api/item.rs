/*
 * Copyright (C) 2026 Fastly, Inc.
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

use crate::core::statusreasons::get_reason;
use crate::core::tnetstring;
use serde::de::{self, MapAccess, SeqAccess, Visitor};
use serde::{Deserialize, Deserializer};
use std::cmp;
use std::collections::HashMap;
use std::fmt;

// support JSON array or object
fn deserialize_headers<'de, D>(deserializer: D) -> Result<Vec<[String; 2]>, D::Error>
where
    D: Deserializer<'de>,
{
    struct Headers;

    impl<'de> Visitor<'de> for Headers {
        type Value = Vec<[String; 2]>;

        fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
            formatter.write_str("an array or object")
        }

        fn visit_seq<S>(self, seq: S) -> Result<Self::Value, S::Error>
        where
            S: SeqAccess<'de>,
        {
            Deserialize::deserialize(de::value::SeqAccessDeserializer::new(seq))
        }

        fn visit_map<M>(self, map: M) -> Result<Self::Value, M::Error>
        where
            M: MapAccess<'de>,
        {
            let map: HashMap<String, String> =
                Deserialize::deserialize(de::value::MapAccessDeserializer::new(map))?;

            Ok(map.into_iter().map(|(k, v)| [k, v]).collect())
        }
    }

    deserializer.deserialize_any(Headers)
}

#[derive(Debug, PartialEq, serde::Deserialize)]
pub struct InHttpResponseFormat {
    pub action: Option<String>,

    pub code: Option<u16>,
    pub reason: Option<String>,

    #[serde(deserialize_with = "deserialize_headers", default)]
    pub headers: Vec<[String; 2]>,

    #[serde(rename(deserialize = "content-filters"), default)]
    pub content_filters: Vec<String>,

    pub body: Option<String>,

    #[serde(rename(deserialize = "body-bin"))]
    pub body_bin: Option<String>,
}

#[derive(Debug, PartialEq, serde::Deserialize)]
pub struct InHttpStreamFormat {
    pub action: Option<String>,

    #[serde(rename(deserialize = "content-filters"), default)]
    pub content_filters: Vec<String>,

    pub content: Option<String>,

    #[serde(rename(deserialize = "content-bin"))]
    pub content_bin: Option<String>,
}

#[derive(Debug, PartialEq, serde::Deserialize)]
pub struct InWsMessageFormat {
    pub action: Option<String>,

    #[serde(rename(deserialize = "type"))]
    pub mtype: Option<String>,

    #[serde(rename(deserialize = "content-filters"), default)]
    pub content_filters: Vec<String>,

    pub content: Option<String>,

    #[serde(rename(deserialize = "content-bin"))]
    pub content_bin: Option<String>,

    pub code: Option<u16>,
    pub reason: Option<String>,
}

#[derive(Debug, PartialEq, serde::Deserialize)]
#[serde(rename_all(serialize = "kebab-case", deserialize = "kebab-case"))]
pub struct InHttpRequestFormat {
    pub action: Option<String>,
    pub method: Option<String>,

    #[serde(deserialize_with = "deserialize_headers", default)]
    pub headers: Vec<[String; 2]>,

    #[serde(default)]
    pub content_filters: Vec<String>,

    pub body: Option<String>,
    pub body_bin: Option<String>,
}

#[derive(Debug, Default, PartialEq, serde::Deserialize)]
pub struct InPublishFormats {
    #[serde(rename(deserialize = "http-response"))]
    pub http_response: Option<InHttpResponseFormat>,

    #[serde(rename(deserialize = "http-stream"))]
    pub http_stream: Option<InHttpStreamFormat>,

    #[serde(rename(deserialize = "ws-message"))]
    pub ws_message: Option<InWsMessageFormat>,

    #[serde(rename(deserialize = "http-request"))]
    pub http_request: Option<InHttpRequestFormat>,

    // DEPRECATED: bayeux
    #[serde(rename(deserialize = "json-object"))]
    pub json_object: Option<serde_json::value::Value>,
}

#[derive(Debug, PartialEq, serde::Deserialize)]
pub struct InPublishItem {
    pub channel: String,
    pub id: Option<String>,

    #[serde(rename(deserialize = "prev-id"))]
    pub prev_id: Option<String>,

    #[serde(default)]
    pub meta: HashMap<String, String>,

    #[serde(default)]
    pub formats: InPublishFormats,

    #[serde(rename(deserialize = "http-response"))]
    pub http_response: Option<InHttpResponseFormat>,

    #[serde(rename(deserialize = "http-stream"))]
    pub http_stream: Option<InHttpStreamFormat>,

    #[serde(rename(deserialize = "ws-message"))]
    pub ws_message: Option<InWsMessageFormat>,

    // DEPRECATED: bayeux
    #[serde(rename(deserialize = "json-object"))]
    pub json_object: Option<serde_json::value::Value>,
}

#[derive(Debug, PartialEq, serde::Deserialize)]
pub struct InPublishItems {
    pub items: Vec<InPublishItem>,
}

#[derive(Debug, Clone, PartialEq, serde::Serialize)]
pub struct OutHttpResponseFormat {
    pub action: String,

    #[serde(skip_serializing_if = "Option::is_none")]
    pub code: Option<u16>,

    #[serde(skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,

    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub headers: Vec<[String; 2]>,

    #[serde(
        rename(serialize = "content-filters"),
        skip_serializing_if = "Vec::is_empty"
    )]
    pub content_filters: Vec<String>,

    #[serde(with = "serde_bytes", skip_serializing_if = "Option::is_none")]
    pub body: Option<Vec<u8>>,
}

#[derive(Debug, Clone, PartialEq, serde::Serialize)]
pub struct OutHttpStreamFormat {
    pub action: String,

    #[serde(
        rename(serialize = "content-filters"),
        skip_serializing_if = "Vec::is_empty"
    )]
    pub content_filters: Vec<String>,

    #[serde(with = "serde_bytes", skip_serializing_if = "Option::is_none")]
    pub content: Option<Vec<u8>>,
}

#[derive(Debug, Clone, PartialEq, serde::Serialize)]
pub struct OutWsMessageFormat {
    pub action: String,

    #[serde(rename(serialize = "type"), skip_serializing_if = "Option::is_none")]
    pub mtype: Option<String>,

    #[serde(
        rename(serialize = "content-filters"),
        skip_serializing_if = "Vec::is_empty"
    )]
    pub content_filters: Vec<String>,

    #[serde(skip_serializing_if = "Option::is_none")]
    pub content: Option<String>,

    #[serde(
        rename(serialize = "content-bin"),
        with = "serde_bytes",
        skip_serializing_if = "Option::is_none"
    )]
    pub content_bin: Option<Vec<u8>>,

    #[serde(skip_serializing_if = "Option::is_none")]
    pub code: Option<u16>,

    #[serde(skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}

#[derive(Debug, Clone, PartialEq, serde::Serialize)]
#[serde(rename_all(serialize = "kebab-case", deserialize = "kebab-case"))]
pub struct OutHttpRequestFormat {
    pub action: String,

    #[serde(skip_serializing_if = "Option::is_none")]
    pub method: Option<String>,

    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub headers: Vec<[String; 2]>,

    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub content_filters: Vec<String>,

    #[serde(with = "serde_bytes", skip_serializing_if = "Option::is_none")]
    pub body: Option<Vec<u8>>,
}

#[derive(Debug, Default, Clone, PartialEq, serde::Serialize)]
pub struct OutPublishFormats {
    #[serde(
        rename(serialize = "http-response"),
        skip_serializing_if = "Option::is_none"
    )]
    pub http_response: Option<OutHttpResponseFormat>,

    #[serde(
        rename(serialize = "http-stream"),
        skip_serializing_if = "Option::is_none"
    )]
    pub http_stream: Option<OutHttpStreamFormat>,

    #[serde(
        rename(serialize = "ws-message"),
        skip_serializing_if = "Option::is_none"
    )]
    pub ws_message: Option<OutWsMessageFormat>,

    #[serde(
        rename(serialize = "http-request"),
        skip_serializing_if = "Option::is_none"
    )]
    pub http_request: Option<OutHttpRequestFormat>,

    #[serde(skip_serializing)]
    pub json_object: Option<serde_json::value::Value>,
}

impl OutPublishFormats {
    fn is_empty(&self) -> bool {
        self.http_response.is_none()
            && self.http_stream.is_none()
            && self.ws_message.is_none()
            && self.http_request.is_none()
            && self.json_object.is_none()
    }
}

#[derive(Debug, Clone, PartialEq, serde::Serialize)]
pub struct OutPublishItem {
    #[serde(skip_serializing)]
    pub channel: String,

    #[serde(skip_serializing_if = "Option::is_none")]
    pub id: Option<String>,

    #[serde(rename(serialize = "prev-id"), skip_serializing_if = "Option::is_none")]
    pub prev_id: Option<String>,

    #[serde(skip_serializing_if = "HashMap::is_empty")]
    pub meta: HashMap<String, String>,

    pub formats: OutPublishFormats,
}

impl OutPublishItem {
    #[allow(clippy::result_unit_err)]
    pub fn serialize(&self) -> Result<Vec<u8>, ()> {
        match tnetstring::to_bytes(self) {
            Ok(v) => Ok(v),
            Err(_) => Err(()),
        }
    }
}

#[derive(Debug, PartialEq)]
pub struct FormatError {
    message: String,
}

impl FormatError {
    fn new<T: AsRef<str>>(message: T) -> Self {
        Self {
            message: message.as_ref().to_string(),
        }
    }
}

impl fmt::Display for FormatError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message)
    }
}

pub fn validate_http_response_format(
    f: &InHttpResponseFormat,
    max_size: usize,
) -> Result<OutHttpResponseFormat, FormatError> {
    let action = match f.action.as_deref() {
        Some("send") | Some("hint") => f.action.as_ref().unwrap().clone(),
        Some(s) => return Err(FormatError::new(format!("{} is not an allowed action", s))),
        None => "send".to_string(),
    };

    let mut out = OutHttpResponseFormat {
        action,
        code: None,
        reason: None,
        headers: Vec::new(),
        content_filters: f.content_filters.clone(),
        body: None,
    };

    if out.action == "send" {
        let code = match f.code {
            Some(code) => {
                if code > 999 {
                    return Err(FormatError::new("code must have a value between 0 and 999"));
                }

                code
            }
            None => 200,
        };

        let reason = match &f.reason {
            Some(reason) => {
                if reason.is_empty() || reason.len() > 127 {
                    return Err(FormatError::new(
                        "UTF-8 encoded reason must have a length between 1 and 127 bytes",
                    ));
                }

                reason.clone()
            }
            None => get_reason(code).to_string(),
        };

        out.code = Some(code);
        out.reason = Some(reason);

        for arr in &f.headers {
            let name = &arr[0];
            let value = &arr[1];

            if name.is_empty() || name.len() > 127 {
                return Err(FormatError::new(
                    "UTF-8 encoded header name must have a length between 1 and 127 bytes",
                ));
            }

            if value.len() > 1023 {
                return Err(FormatError::new(
                    "UTF-8 encoded header value must not exceed 1023 bytes",
                ));
            }
        }

        out.headers = f.headers.clone();

        if let Some(body) = &f.body_bin {
            if body.len() > max_size {
                return Err(FormatError::new(format!(
                    "UTF-8 encoded body-bin must not exceed {} bytes",
                    max_size
                )));
            }

            let body = match base64::decode(body) {
                Ok(v) => v,
                Err(_) => return Err(FormatError::new("body-bin is not valid base64")),
            };

            if body.len() > max_size {
                return Err(FormatError::new(format!(
                    "decoded value of body-bin must not exceed {} bytes",
                    max_size
                )));
            }

            out.body = Some(body);
        } else if let Some(body) = &f.body {
            if body.len() > max_size {
                return Err(FormatError::new(format!(
                    "UTF-8 encoded body must not exceed {} bytes",
                    max_size
                )));
            }

            out.body = Some(body.clone().into_bytes());
        } else {
            return Err(FormatError::new(
                "http-response must contain one of body or body-bin",
            ));
        };
    }

    Ok(out)
}

pub fn validate_http_stream_format(
    f: &InHttpStreamFormat,
    max_size: usize,
) -> Result<OutHttpStreamFormat, FormatError> {
    let action = match f.action.as_deref() {
        Some("send") | Some("hint") | Some("close") => f.action.as_ref().unwrap().clone(),
        Some(s) => return Err(FormatError::new(format!("{} is not an allowed action", s))),
        None => "send".to_string(),
    };

    let mut out = OutHttpStreamFormat {
        action,
        content_filters: f.content_filters.clone(),
        content: None,
    };

    if out.action == "send" {
        if let Some(content) = &f.content_bin {
            if content.len() > max_size {
                return Err(FormatError::new(format!(
                    "UTF-8 encoded content-bin must not exceed {} bytes",
                    max_size
                )));
            }

            let content = match base64::decode(content) {
                Ok(v) => v,
                Err(_) => return Err(FormatError::new("content-bin is not valid base64")),
            };

            if content.len() > max_size {
                return Err(FormatError::new(format!(
                    "decoded value of content-bin must not exceed {} bytes",
                    max_size
                )));
            }

            out.content = Some(content);
        } else if let Some(content) = &f.content {
            if content.len() > max_size {
                return Err(FormatError::new(format!(
                    "UTF-8 encoded content must not exceed {} bytes",
                    max_size
                )));
            }

            out.content = Some(content.clone().into_bytes());
        } else {
            return Err(FormatError::new(
                "http-stream send action must contain one of content or content-bin",
            ));
        }
    }

    Ok(out)
}

pub fn validate_ws_message_format(
    f: &InWsMessageFormat,
    max_size: usize,
) -> Result<OutWsMessageFormat, FormatError> {
    let action = match f.action.as_deref() {
        Some("send") | Some("close") | Some("refresh") => f.action.as_ref().unwrap().clone(),
        Some(s) => return Err(FormatError::new(format!("{} is not an allowed action", s))),
        None => "send".to_string(),
    };

    let mut out = OutWsMessageFormat {
        action,
        mtype: None,
        content_filters: f.content_filters.clone(),
        content: None,
        content_bin: None,
        code: None,
        reason: None,
    };

    if out.action == "send" {
        out.mtype = if let Some(mtype) = &f.mtype {
            match mtype.as_str() {
                "text" | "binary" | "ping" | "pong" => Some(mtype.clone()),
                _ => {
                    return Err(FormatError::new(format!(
                        "'type' contains unknown value: {}",
                        mtype
                    )))
                }
            }
        } else {
            None
        };

        if let Some(content) = &f.content_bin {
            if content.len() > max_size {
                return Err(FormatError::new(format!(
                    "UTF-8 encoded content-bin must not exceed {} bytes",
                    max_size
                )));
            }

            let content = match base64::decode(content) {
                Ok(v) => v,
                Err(_) => return Err(FormatError::new("content-bin is not valid base64")),
            };

            if content.len() > max_size {
                return Err(FormatError::new(format!(
                    "decoded value of content-bin must not exceed {} bytes",
                    max_size
                )));
            }

            out.content_bin = Some(content);
        } else if let Some(content) = &f.content {
            if content.len() > max_size {
                return Err(FormatError::new(format!(
                    "UTF-8 encoded content must not exceed {} bytes",
                    max_size
                )));
            }

            out.content = Some(content.clone());
        } else {
            return Err(FormatError::new(
                "ws-message send action must contain one of content or content-bin",
            ));
        }
    } else if out.action == "close" {
        out.code = f.code;

        if let Some(reason) = &f.reason {
            if reason.len() > 127 {
                return Err(FormatError::new(
                    "UTF-8 encoded reason must not exceed 127 bytes",
                ));
            }

            out.reason = Some(reason.clone());
        }
    }

    Ok(out)
}

pub fn validate_http_request_format(
    f: &InHttpRequestFormat,
    max_size: usize,
) -> Result<OutHttpRequestFormat, FormatError> {
    let action = match f.action.as_deref() {
        Some("send") | None => "send".to_string(),
        Some(s) => return Err(FormatError::new(format!("{} is not an allowed action", s))),
    };

    let mut out = OutHttpRequestFormat {
        action,
        method: None,
        headers: Vec::new(),
        content_filters: f.content_filters.clone(),
        body: None,
    };

    if out.action == "send" {
        let method = match &f.method {
            Some(method) => {
                if method.is_empty() || method.len() > 127 {
                    return Err(FormatError::new(
                        "UTF-8 encoded method must have a length between 1 and 127 bytes",
                    ));
                }

                method.clone()
            }
            None => "POST".to_string(),
        };

        out.method = Some(method);

        for arr in &f.headers {
            let name = &arr[0];
            let value = &arr[1];

            if name.is_empty() || name.len() > 127 {
                return Err(FormatError::new(
                    "UTF-8 encoded header name must have a length between 1 and 127 bytes",
                ));
            }

            if value.len() > 1023 {
                return Err(FormatError::new(
                    "UTF-8 encoded header value must not exceed 1023 bytes",
                ));
            }
        }

        out.headers = f.headers.clone();

        if let Some(body) = &f.body_bin {
            if body.len() > max_size {
                return Err(FormatError::new(format!(
                    "UTF-8 encoded body-bin must not exceed {} bytes",
                    max_size
                )));
            }

            let body = match base64::decode(body) {
                Ok(v) => v,
                Err(_) => return Err(FormatError::new("body-bin is not valid base64")),
            };

            if body.len() > max_size {
                return Err(FormatError::new(format!(
                    "decoded value of body-bin must not exceed {} bytes",
                    max_size
                )));
            }

            out.body = Some(body);
        } else if let Some(body) = &f.body {
            if body.len() > max_size {
                return Err(FormatError::new(format!(
                    "UTF-8 encoded body must not exceed {} bytes",
                    max_size
                )));
            }

            out.body = Some(body.clone().into_bytes());
        } else {
            return Err(FormatError::new(
                "http-request must contain one of body or body-bin",
            ));
        };
    }

    Ok(out)
}

pub fn validate_item(
    item: &InPublishItem,
    max_size: usize,
    allow_json_object: bool,
) -> Result<(OutPublishItem, usize), FormatError> {
    if let Some(id) = &item.id {
        if id.is_empty() || id.len() > 127 {
            return Err(FormatError::new(
                "UTF-8 encoded id must have a length between 1 and 127 bytes",
            ));
        }
    }

    if let Some(prev_id) = &item.prev_id {
        if prev_id.is_empty() || prev_id.len() > 127 {
            return Err(FormatError::new(
                "UTF-8 encoded prev-id must have a length between 1 and 127 bytes",
            ));
        }
    }

    for (k, v) in item.meta.iter() {
        if k.is_empty() || k.len() > 127 {
            return Err(FormatError::new(
                "UTF-8 encoded meta key must have a length between 1 and 127 bytes",
            ));
        }

        if v.len() > 1023 {
            return Err(FormatError::new(format!(
                "UTF-8 encoded value of meta \"{}\" must not exceed 1023 bytes",
                k
            )));
        }
    }

    let mut formats = OutPublishFormats::default();
    let mut size = 0;

    if let Some(f) = item
        .formats
        .http_response
        .as_ref()
        .or(item.http_response.as_ref())
    {
        let f = validate_http_response_format(f, max_size)?;

        let s = match &f.body {
            Some(v) => v.len(),
            None => 0,
        };

        size = cmp::max(size, s);
        formats.http_response = Some(f);
    }

    if let Some(f) = item
        .formats
        .http_stream
        .as_ref()
        .or(item.http_stream.as_ref())
    {
        let f = validate_http_stream_format(f, max_size)?;

        let s = match &f.content {
            Some(v) => v.len(),
            None => 0,
        };

        size = cmp::max(size, s);
        formats.http_stream = Some(f);
    }

    if let Some(f) = item
        .formats
        .ws_message
        .as_ref()
        .or(item.ws_message.as_ref())
    {
        let f = validate_ws_message_format(f, max_size)?;

        let s = match &f.content_bin {
            Some(v) => v.len(),
            None => match &f.content {
                Some(v) => v.len(),
                None => 0,
            },
        };

        size = cmp::max(size, s);
        formats.ws_message = Some(f);
    }

    if let Some(f) = &item.formats.http_request {
        let f = validate_http_request_format(f, max_size)?;

        let s = match &f.body {
            Some(v) => v.len(),
            None => 0,
        };

        size = cmp::max(size, s);
        formats.http_request = Some(f);
    }

    if allow_json_object {
        if let Some(f) = item
            .formats
            .json_object
            .as_ref()
            .or(item.json_object.as_ref())
        {
            size = cmp::max(size, serde_json::to_string(&f).unwrap().len());
            formats.json_object = Some(f.clone());
        }
    }

    if formats.is_empty() {
        if allow_json_object {
            return Err(FormatError::new(
                "must contain at least one transport type (http-response, http-stream, ws-message, http-request, json-object)",
            ));
        } else {
            return Err(FormatError::new(
                "must contain at least one transport type (http-response, http-stream, ws-message, http-request)",
            ));
        }
    }

    Ok((
        OutPublishItem {
            channel: item.channel.clone(),
            id: item.id.clone(),
            prev_id: item.prev_id.clone(),
            formats,
            meta: item.meta.clone(),
        },
        size,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::tnetstring;
    use std::str;

    #[test]
    fn test_decode() {
        let items: InPublishItems =
            serde_json::from_str(r#"{"items":[{"channel":"foo","formats":{}}]}"#).unwrap();

        assert_eq!(
            items,
            InPublishItems {
                items: vec![InPublishItem {
                    channel: "foo".to_string(),
                    id: None,
                    prev_id: None,
                    formats: InPublishFormats::default(),
                    http_response: None,
                    http_stream: None,
                    ws_message: None,
                    json_object: None,
                    meta: HashMap::new(),
                }]
            }
        );
    }

    #[test]
    fn test_decode_http_response() {
        struct Test {
            input: &'static str,
            result: InHttpResponseFormat,
        }

        let tests = [
            Test {
                input: r#"{"code":200,"reason":"OK","headers":[["Content-Type","text/plain"]],"body":"hello"}"#,
                result: InHttpResponseFormat {
                    action: None,
                    code: Some(200),
                    reason: Some("OK".to_string()),
                    headers: vec![["Content-Type".to_string(), "text/plain".to_string()]],
                    content_filters: Vec::new(),
                    body: Some("hello".to_string()),
                    body_bin: None,
                },
            },
            Test {
                input: r#"{"code":200,"reason":"OK","headers":{"Content-Type":"text/plain"},"body":"hello"}"#,
                result: InHttpResponseFormat {
                    action: None,
                    code: Some(200),
                    reason: Some("OK".to_string()),
                    headers: vec![["Content-Type".to_string(), "text/plain".to_string()]],
                    content_filters: Vec::new(),
                    body: Some("hello".to_string()),
                    body_bin: None,
                },
            },
        ];

        for test in tests {
            let hr: InHttpResponseFormat = serde_json::from_str(test.input).unwrap();

            assert_eq!(hr, test.result);
        }
    }

    #[test]
    fn test_validate_item() {
        struct Test {
            input: &'static str,
            result: (OutPublishItem, usize),
        }

        let tests = [
            Test {
                input: r#"{"channel":"foo","formats":{"http-response":{"code":200,"body":"hello"}}}"#,
                result: (
                    OutPublishItem {
                        channel: "foo".to_string(),
                        id: None,
                        prev_id: None,
                        formats: OutPublishFormats {
                            http_response: Some(OutHttpResponseFormat {
                                action: "send".to_string(),
                                code: Some(200),
                                reason: Some("OK".to_string()),
                                headers: Vec::new(),
                                content_filters: Vec::new(),
                                body: Some(b"hello".to_vec()),
                            }),
                            http_stream: None,
                            ws_message: None,
                            http_request: None,
                            json_object: None,
                        },
                        meta: HashMap::new(),
                    },
                    5,
                ),
            },
            Test {
                input: r#"{"channel":"foo","formats":{"http-response":{"action":"hint"}}}"#,
                result: (
                    OutPublishItem {
                        channel: "foo".to_string(),
                        id: None,
                        prev_id: None,
                        formats: OutPublishFormats {
                            http_response: Some(OutHttpResponseFormat {
                                action: "hint".to_string(),
                                code: None,
                                reason: None,
                                headers: Vec::new(),
                                content_filters: Vec::new(),
                                body: None,
                            }),
                            http_stream: None,
                            ws_message: None,
                            http_request: None,
                            json_object: None,
                        },
                        meta: HashMap::new(),
                    },
                    0,
                ),
            },
            Test {
                input: r#"{"channel":"foo","formats":{"http-stream":{"content":"hello"}}}"#,
                result: (
                    OutPublishItem {
                        channel: "foo".to_string(),
                        id: None,
                        prev_id: None,
                        formats: OutPublishFormats {
                            http_response: None,
                            http_stream: Some(OutHttpStreamFormat {
                                action: "send".to_string(),
                                content_filters: Vec::new(),
                                content: Some(b"hello".to_vec()),
                            }),
                            ws_message: None,
                            http_request: None,
                            json_object: None,
                        },
                        meta: HashMap::new(),
                    },
                    5,
                ),
            },
            Test {
                input: r#"{"channel":"foo","formats":{"ws-message":{"content":"hello"}}}"#,
                result: (
                    OutPublishItem {
                        channel: "foo".to_string(),
                        id: None,
                        prev_id: None,
                        formats: OutPublishFormats {
                            http_response: None,
                            http_stream: None,
                            ws_message: Some(OutWsMessageFormat {
                                action: "send".to_string(),
                                mtype: None,
                                content_filters: Vec::new(),
                                content: Some("hello".to_string()),
                                content_bin: None,
                                code: None,
                                reason: None,
                            }),
                            http_request: None,
                            json_object: None,
                        },
                        meta: HashMap::new(),
                    },
                    5,
                ),
            },
            Test {
                input: r#"{"channel":"foo","formats":{"ws-message":{"type":"ping","content":"hello"}}}"#,
                result: (
                    OutPublishItem {
                        channel: "foo".to_string(),
                        id: None,
                        prev_id: None,
                        formats: OutPublishFormats {
                            http_response: None,
                            http_stream: None,
                            ws_message: Some(OutWsMessageFormat {
                                action: "send".to_string(),
                                mtype: Some("ping".to_string()),
                                content_filters: Vec::new(),
                                content: Some("hello".to_string()),
                                content_bin: None,
                                code: None,
                                reason: None,
                            }),
                            http_request: None,
                            json_object: None,
                        },
                        meta: HashMap::new(),
                    },
                    5,
                ),
            },
            Test {
                input: r#"{"channel":"foo","formats":{"ws-message":{"content-bin":"aGVsbG8="}}}"#,
                result: (
                    OutPublishItem {
                        channel: "foo".to_string(),
                        id: None,
                        prev_id: None,
                        formats: OutPublishFormats {
                            http_response: None,
                            http_stream: None,
                            ws_message: Some(OutWsMessageFormat {
                                action: "send".to_string(),
                                mtype: None,
                                content_filters: Vec::new(),
                                content: None,
                                content_bin: Some(b"hello".to_vec()),
                                code: None,
                                reason: None,
                            }),
                            http_request: None,
                            json_object: None,
                        },
                        meta: HashMap::new(),
                    },
                    5,
                ),
            },
            Test {
                input: r#"{"channel":"foo","formats":{"ws-message":{"action":"close"}}}"#,
                result: (
                    OutPublishItem {
                        channel: "foo".to_string(),
                        id: None,
                        prev_id: None,
                        formats: OutPublishFormats {
                            http_response: None,
                            http_stream: None,
                            ws_message: Some(OutWsMessageFormat {
                                action: "close".to_string(),
                                mtype: None,
                                content_filters: Vec::new(),
                                content: None,
                                content_bin: None,
                                code: None,
                                reason: None,
                            }),
                            http_request: None,
                            json_object: None,
                        },
                        meta: HashMap::new(),
                    },
                    0,
                ),
            },
            Test {
                input: r#"{"channel":"foo","formats":{"http-stream":{"content":"some data"},"ws-message":{"content":"larger data"}}}"#,
                result: (
                    OutPublishItem {
                        channel: "foo".to_string(),
                        id: None,
                        prev_id: None,
                        formats: OutPublishFormats {
                            http_response: None,
                            http_stream: Some(OutHttpStreamFormat {
                                action: "send".to_string(),
                                content_filters: Vec::new(),
                                content: Some(b"some data".to_vec()),
                            }),
                            ws_message: Some(OutWsMessageFormat {
                                action: "send".to_string(),
                                mtype: None,
                                content_filters: Vec::new(),
                                content: Some("larger data".to_string()),
                                content_bin: None,
                                code: None,
                                reason: None,
                            }),
                            http_request: None,
                            json_object: None,
                        },
                        meta: HashMap::new(),
                    },
                    11,
                ),
            },
            Test {
                input: r#"{"channel":"foo","formats":{"http-request":{"body":"hello"}}}"#,
                result: (
                    OutPublishItem {
                        channel: "foo".to_string(),
                        id: None,
                        prev_id: None,
                        formats: OutPublishFormats {
                            http_response: None,
                            http_stream: None,
                            ws_message: None,
                            http_request: Some(OutHttpRequestFormat {
                                action: "send".to_string(),
                                method: Some("POST".to_string()),
                                headers: Vec::new(),
                                content_filters: Vec::new(),
                                body: Some(b"hello".to_vec()),
                            }),
                            json_object: None,
                        },
                        meta: HashMap::new(),
                    },
                    5,
                ),
            },
        ];

        for test in tests {
            let item: InPublishItem = serde_json::from_str(test.input).unwrap();

            assert_eq!(validate_item(&item, 1024, false), Ok(test.result));
        }
    }

    #[test]
    fn test_serialize() {
        let item = OutPublishItem {
            channel: "test".to_string(),
            id: None,
            prev_id: None,
            formats: OutPublishFormats {
                http_response: None,
                http_stream: None,
                ws_message: Some(OutWsMessageFormat {
                    action: "send".to_string(),
                    mtype: None,
                    content_filters: Vec::new(),
                    content: Some("hello".to_string()),
                    content_bin: None,
                    code: None,
                    reason: None,
                }),
                http_request: None,
                json_object: None,
            },
            meta: HashMap::new(),
        };

        let payload = item.serialize().unwrap();

        let mut content = String::new();

        for e in tnetstring::parse_map(&payload).unwrap() {
            let e = e.unwrap();

            match e.key {
                "formats" => {
                    for e in tnetstring::parse_map(e.data).unwrap() {
                        let e = e.unwrap();

                        match e.key {
                            "ws-message" => {
                                for e in tnetstring::parse_map(e.data).unwrap() {
                                    let e = e.unwrap();

                                    match e.key {
                                        "content" => {
                                            let s = tnetstring::parse_string(e.data).unwrap();
                                            let s = str::from_utf8(s).unwrap();

                                            content = s.to_string();
                                        }
                                        _ => {}
                                    }
                                }
                            }
                            _ => {}
                        }
                    }
                }
                _ => {}
            }
        }

        assert_eq!(&content, "hello");
    }
}
