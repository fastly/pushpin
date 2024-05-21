/*
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

mod error;
mod protocol;
mod util;

pub mod client;
pub mod server;

pub use error::*;
pub use protocol::{
    parse_header_value, BodySize, Header, HeaderParamsIterator, ParseScratch, Request, Response,
    EMPTY_HEADER,
};
pub use util::{RecvStatus, SendStatus};
