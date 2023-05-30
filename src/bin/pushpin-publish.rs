/*
 * Copyright (C) 2021-2023 Fanout, Inc.
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

use clap::{Arg, ArgAction, Command};
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

    let matches = Command::new(PROGRAM_NAME)
        .version(env!("APP_VERSION"))
        .about("Publish messages to Pushpin")
        .arg(
            Arg::new("channel")
                .required(true)
                .num_args(1)
                .value_name("channel")
                .help("Channel to send to"),
        )
        .arg(
            Arg::new("content")
                .num_args(1)
                .value_name("content")
                .help("Content to use for HTTP body and WebSocket message"),
        )
        .arg(
            Arg::new("id")
                .long("id")
                .num_args(1)
                .value_name("id")
                .help("Payload ID"),
        )
        .arg(
            Arg::new("prev-id")
                .long("prev-id")
                .num_args(1)
                .value_name("id")
                .help("Previous payload ID"),
        )
        .arg(
            Arg::new("sender")
                .long("sender")
                .num_args(1)
                .value_name("sender")
                .help("Sender meta value"),
        )
        .arg(
            Arg::new("code")
                .long("code")
                .num_args(1)
                .value_name("code")
                .help("HTTP response code to use")
                .default_value("200"),
        )
        .arg(
            Arg::new("header")
                .short('H')
                .long("header")
                .num_args(1)
                .value_name("\"K: V\"")
                .action(ArgAction::Append)
                .help("Add HTTP response header"),
        )
        .arg(
            Arg::new("meta")
                .short('M')
                .long("meta")
                .num_args(1)
                .value_name("\"K=V\"")
                .action(ArgAction::Append)
                .help("Add meta variable"),
        )
        .arg(
            Arg::new("hint")
                .long("hint")
                .action(ArgAction::SetTrue)
                .help("Send hint instead of content"),
        )
        .arg(
            Arg::new("close")
                .long("close")
                .action(ArgAction::SetTrue)
                .help("Close streaming and WebSocket connections"),
        )
        .arg(
            Arg::new("patch")
                .long("patch")
                .action(ArgAction::SetTrue)
                .help("Content is JSON patch"),
        )
        .arg(
            Arg::new("no-seq")
                .long("no-seq")
                .action(ArgAction::SetTrue)
                .help("Bypass sequencing buffer"),
        )
        .arg(
            Arg::new("no-eol")
                .long("no-eol")
                .action(ArgAction::SetTrue)
                .help("Don't add newline to HTTP payloads"),
        )
        .arg(
            Arg::new("spec")
                .long("spec")
                .num_args(1)
                .value_name("spec")
                .help("GRIP URL or ZeroMQ PUSH spec")
                .default_value(default_spec),
        )
        .arg(
            Arg::new("user")
                .short('u')
                .long("user")
                .num_args(1)
                .value_name("user:pass")
                .help("Authenticate using basic auth"),
        )
        .get_matches();

    let channel = matches.get_one::<String>("channel").unwrap().clone();

    let content = matches.get_one::<String>("content").cloned();

    let id = matches.get_one::<String>("id").cloned().unwrap_or_default();
    let prev_id = matches
        .get_one::<String>("prev-id")
        .cloned()
        .unwrap_or_default();
    let sender = matches
        .get_one::<String>("sender")
        .cloned()
        .unwrap_or_default();

    let code = matches.get_one::<String>("code").unwrap();

    let code: u16 = match code.parse() {
        Ok(x) => x,
        Err(e) => {
            eprintln!("Error: failed to parse code: {}", e);
            process::exit(1);
        }
    };

    let headers = matches
        .get_many::<String>("header")
        .unwrap_or_default()
        .map(|v| v.to_owned())
        .collect();

    let meta = matches
        .get_many::<String>("meta")
        .unwrap_or_default()
        .map(|v| v.to_owned())
        .collect();

    let hint = *matches.get_one("hint").unwrap();
    let close = *matches.get_one("close").unwrap();
    let patch = *matches.get_one("patch").unwrap();
    let no_seq = *matches.get_one("no-seq").unwrap();
    let no_eol = *matches.get_one("no-eol").unwrap();

    let spec = matches.get_one::<String>("spec").unwrap().clone();

    let user = matches.get_one::<String>("user").cloned();

    let args = Args {
        channel,
        content,
        id,
        prev_id,
        sender,
        code,
        headers,
        meta,
        hint,
        close,
        patch,
        no_seq,
        no_eol,
        spec,
        user,
    };

    if let Err(e) = process_args_and_run(args) {
        eprintln!("Error: {}", e);
        process::exit(1);
    }
}
