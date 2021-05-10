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

use clap::{App, Arg};
use pushpin::publish_cli::{run, Action, Config, Content, Message};
use std::env;
use std::error::Error;
use std::process;

const PROGRAM_NAME: &str = "pushpin-publish";
const DEFAULT_SPEC: &str = "http://localhost:5561";

struct Args {
    channel: String,
    content: Option<String>,
    id: String,
    prev_id: String,
    sender: String,
    code: u16,
    headers: Vec<String>,
    meta: Vec<String>,
    hint: bool,
    close: bool,
    patch: bool,
    no_seq: bool,
    no_eol: bool,
    spec: String,
    user: Option<String>,
}

fn process_args_and_run(args: Args) -> Result<(), Box<dyn Error>> {
    let action = if args.hint {
        Action::Hint
    } else if args.close {
        Action::Close
    } else {
        if args.code > 999 {
            return Err("code must be an integer between 0 and 999".into());
        }

        let content = match args.content {
            Some(s) => s,
            None => return Err("must specify content".into()),
        };

        let content = if args.patch {
            let v: serde_json::Value = serde_json::from_str(&content)?;

            let arr = match v {
                serde_json::Value::Array(arr) => arr,
                _ => return Err("patch content must be a JSON array".into()),
            };

            Content::Patch(arr)
        } else {
            Content::Value(content)
        };

        Action::Send(Message {
            code: args.code,
            content,
        })
    };

    let mut headers = Vec::new();

    for v in args.headers {
        let pos = match v.find(':') {
            Some(pos) => pos,
            None => return Err("header must be in the form \"name: value\"".into()),
        };

        let name = &v[..pos];
        let val = &v[(pos + 1)..].trim();

        headers.push((name.to_string(), val.to_string()));
    }

    let mut meta = Vec::new();

    for v in args.meta {
        let pos = match v.find('=') {
            Some(pos) => pos,
            None => return Err("meta must be in the form \"name=value\"".into()),
        };

        let name = &v[..pos];
        let val = &v[(pos + 1)..].trim();

        meta.push((name.to_string(), val.to_string()));
    }

    let config = Config {
        spec: args.spec,
        basic_auth: args.user,
        channel: args.channel,
        id: args.id,
        prev_id: args.prev_id,
        sender: args.sender,
        action,
        headers,
        meta,
        no_seq: args.no_seq,
        eol: !args.no_eol,
    };

    run(&config)
}

fn main() {
    let default_spec = match env::var("GRIP_URL") {
        Ok(s) => s,
        Err(_) => DEFAULT_SPEC.to_string(),
    };

    let matches = App::new(PROGRAM_NAME)
        .version(env!("APP_VERSION"))
        .about("Publish messages to Pushpin")
        .arg(
            Arg::with_name("channel")
                .required(true)
                .takes_value(true)
                .value_name("channel")
                .help("Channel to send to"),
        )
        .arg(
            Arg::with_name("content")
                .takes_value(true)
                .value_name("content")
                .help("Content to use for HTTP body and WebSocket message"),
        )
        .arg(
            Arg::with_name("id")
                .long("id")
                .takes_value(true)
                .value_name("id")
                .help("Payload ID"),
        )
        .arg(
            Arg::with_name("prev-id")
                .long("prev-id")
                .takes_value(true)
                .value_name("id")
                .help("Previous payload ID"),
        )
        .arg(
            Arg::with_name("sender")
                .long("sender")
                .takes_value(true)
                .value_name("sender")
                .help("Sender meta value"),
        )
        .arg(
            Arg::with_name("code")
                .long("code")
                .takes_value(true)
                .value_name("code")
                .help("HTTP response code to use")
                .default_value("200"),
        )
        .arg(
            Arg::with_name("header")
                .short("H")
                .long("header")
                .takes_value(true)
                .value_name("\"K: V\"")
                .multiple(true)
                .help("Add HTTP response header"),
        )
        .arg(
            Arg::with_name("meta")
                .short("M")
                .long("meta")
                .takes_value(true)
                .value_name("\"K=V\"")
                .multiple(true)
                .help("Add meta variable"),
        )
        .arg(
            Arg::with_name("hint")
                .long("hint")
                .help("Send hint instead of content"),
        )
        .arg(
            Arg::with_name("close")
                .long("close")
                .help("Close streaming and WebSocket connections"),
        )
        .arg(
            Arg::with_name("patch")
                .long("patch")
                .help("Content is JSON patch"),
        )
        .arg(
            Arg::with_name("no-seq")
                .long("no-seq")
                .help("Bypass sequencing buffer"),
        )
        .arg(
            Arg::with_name("no-eol")
                .long("no-eol")
                .help("Don't add newline to HTTP payloads"),
        )
        .arg(
            Arg::with_name("spec")
                .long("spec")
                .takes_value(true)
                .value_name("spec")
                .help("GRIP URL or ZeroMQ PUSH spec")
                .default_value(&default_spec),
        )
        .arg(
            Arg::with_name("user")
                .short("u")
                .long("user")
                .takes_value(true)
                .value_name("user:pass")
                .help("Authenticate using basic auth"),
        )
        .get_matches();

    let channel = matches.value_of("channel").unwrap();

    let content = matches
        .value_of("content")
        .map_or(None, |s| Some(String::from(s)));

    let id = matches.value_of("id").unwrap_or("");
    let prev_id = matches.value_of("prev-id").unwrap_or("");
    let sender = matches.value_of("sender").unwrap_or("");

    let code = matches.value_of("code").unwrap();

    let code: u16 = match code.parse() {
        Ok(x) => x,
        Err(e) => {
            eprintln!("Error: failed to parse code: {}", e);
            process::exit(1);
        }
    };

    let headers = if matches.is_present("header") {
        matches
            .values_of("header")
            .unwrap()
            .map(String::from)
            .collect()
    } else {
        Vec::new()
    };

    let meta = if matches.is_present("meta") {
        matches
            .values_of("meta")
            .unwrap()
            .map(String::from)
            .collect()
    } else {
        Vec::new()
    };

    let hint = matches.is_present("hint");
    let close = matches.is_present("close");
    let patch = matches.is_present("patch");
    let no_seq = matches.is_present("no-seq");
    let no_eol = matches.is_present("no-eol");

    let spec = matches.value_of("spec").unwrap();

    let user = matches
        .value_of("user")
        .map_or(None, |s| Some(String::from(s)));

    let args = Args {
        channel: channel.to_string(),
        content,
        id: id.to_string(),
        prev_id: prev_id.to_string(),
        sender: sender.to_string(),
        code,
        headers,
        meta,
        hint,
        close,
        patch,
        no_seq,
        no_eol,
        spec: spec.to_string(),
        user,
    };

    if let Err(e) = process_args_and_run(args) {
        eprintln!("Error: {}", e);
        process::exit(1);
    }
}
