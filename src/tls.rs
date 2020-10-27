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

use arrayvec::ArrayString;
use log::debug;
use mio::net::TcpStream;
use openssl::error::ErrorStack;
use openssl::ssl::{
    HandshakeError, MidHandshakeSslStream, NameType, SniError, SslAcceptor, SslContext,
    SslContextBuilder, SslFiletype, SslMethod, SslStream,
};
use std::cmp;
use std::collections::HashMap;
use std::fs;
use std::io;
use std::io::{Read, Write};
use std::mem;
use std::path::{Path, PathBuf};
use std::str::FromStr;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::SystemTime;

struct Identity {
    ssl_context: SslContext,
    cert_fname: PathBuf,
    key_fname: PathBuf,
    modified: Option<SystemTime>,
}

struct IdentityRef<'a> {
    _data: MutexGuard<'a, HashMap<String, Identity>>,
    name: &'a str,
    value: &'a Identity,
}

pub struct IdentityCache {
    dir: PathBuf,
    data: Mutex<HashMap<String, Identity>>,
}

impl IdentityCache {
    pub fn new(certs_dir: &Path) -> Self {
        Self {
            dir: certs_dir.to_path_buf(),
            data: Mutex::new(HashMap::new()),
        }
    }

    fn get<'a>(&'a self, name: &str) -> Option<IdentityRef<'a>> {
        let mut data = self.data.lock().unwrap();

        let mut update = false;

        if let Some(value) = data.get(name) {
            if let Some(modified) = value.modified {
                update = match fs::metadata(&value.cert_fname) {
                    Ok(md) => match md.modified() {
                        Ok(t) => t > modified,
                        Err(_) => false,
                    },
                    Err(_) => true,
                };

                if !update {
                    update = match fs::metadata(&value.key_fname) {
                        Ok(md) => match md.modified() {
                            Ok(t) => t > modified,
                            Err(_) => false,
                        },
                        Err(_) => true,
                    };
                }
            }
        } else {
            update = true;
        }

        if update {
            let cert_fname = self.dir.join(Path::new(&format!("{}.crt", name)));

            let cert_metadata = match fs::metadata(&cert_fname) {
                Ok(md) => md,
                Err(e) => {
                    debug!("failed to read cert file metadata {:?}: {}", cert_fname, e);
                    return None;
                }
            };

            let key_fname = self.dir.join(Path::new(&format!("{}.key", name)));

            let key_metadata = match fs::metadata(&key_fname) {
                Ok(md) => md,
                Err(e) => {
                    debug!("failed to read key file metadata {:?}: {}", key_fname, e);
                    return None;
                }
            };

            let cert_modified = cert_metadata.modified();
            let key_modified = key_metadata.modified();

            let modified = if cert_modified.is_ok() && key_modified.is_ok() {
                let cert_modified = cert_modified.unwrap();
                let key_modified = key_modified.unwrap();

                Some(cmp::max(cert_modified, key_modified))
            } else {
                None
            };

            let mut ctx = match SslContextBuilder::new(SslMethod::tls()) {
                Ok(ctx) => ctx,
                Err(e) => {
                    debug!("failed to create SSL context: {}", e);
                    return None;
                }
            };

            if let Err(e) = ctx.set_certificate_chain_file(&cert_fname) {
                debug!("failed to read cert content {:?}: {}", cert_fname, e);
                return None;
            }

            if let Err(e) = ctx.set_private_key_file(&key_fname, SslFiletype::PEM) {
                debug!("failed to read key content {:?}: {}", key_fname, e);
                return None;
            }

            if let Err(e) = ctx.check_private_key() {
                debug!("failed to check private key for keypair {}: {}", name, e);
                return None;
            }

            let ctx = ctx.build();

            debug!("loaded cert: {}", name);

            data.insert(
                String::from(name),
                Identity {
                    ssl_context: ctx,
                    cert_fname,
                    key_fname,
                    modified,
                },
            );
        }

        mem::drop(data);

        let data = self.data.lock().unwrap();

        if let Some((name, value)) = data.get_key_value(name) {
            // extending the lifetimes is safe because we keep the owning MutexGuard
            let name = unsafe { mem::transmute::<&String, &'a String>(name) };
            let value = unsafe { mem::transmute::<&Identity, &'a Identity>(value) };

            Some(IdentityRef {
                _data: data,
                name: name.as_str(),
                value,
            })
        } else {
            None
        }
    }
}

enum Stream {
    Ssl(SslStream<TcpStream>),
    MidHandshakeSsl(MidHandshakeSslStream<TcpStream>),
}

pub struct TlsAcceptor {
    acceptor: SslAcceptor,
}

impl TlsAcceptor {
    pub fn new(cache: &Arc<IdentityCache>, default_cert: Option<&str>) -> Self {
        let mut acceptor = SslAcceptor::mozilla_intermediate(SslMethod::tls()).unwrap();

        let cache = Arc::clone(cache);
        let default_cert: Option<String> = default_cert.map(|s| s.to_owned());

        acceptor.set_servername_callback(move |ssl, _| {
            let identity = match ssl.servername(NameType::HOST_NAME) {
                Some(name) => {
                    debug!("tls server name: {}", name);

                    match cache.get(name) {
                        Some(ctx) => ctx,
                        None => match &default_cert {
                            Some(default_cert) => match cache.get(default_cert) {
                                Some(ctx) => ctx,
                                None => return Err(SniError::ALERT_FATAL),
                            },
                            None => return Err(SniError::ALERT_FATAL),
                        },
                    }
                }
                None => match &default_cert {
                    Some(default_cert) => match cache.get(default_cert) {
                        Some(ctx) => ctx,
                        None => return Err(SniError::ALERT_FATAL),
                    },
                    None => return Err(SniError::ALERT_FATAL),
                },
            };

            debug!("using cert: {}", identity.name);

            if ssl.set_ssl_context(&identity.value.ssl_context).is_err() {
                return Err(SniError::ALERT_FATAL);
            }

            Ok(())
        });

        Self {
            acceptor: acceptor.build(),
        }
    }

    pub fn accept(&self, stream: TcpStream) -> Result<TlsStream, ErrorStack> {
        let stream = match self.acceptor.accept(stream) {
            Ok(stream) => Stream::Ssl(stream),
            Err(HandshakeError::SetupFailure(e)) => return Err(e),
            Err(HandshakeError::Failure(stream)) => Stream::MidHandshakeSsl(stream),
            Err(HandshakeError::WouldBlock(stream)) => Stream::MidHandshakeSsl(stream),
        };

        Ok(TlsStream {
            stream: Some(stream),
            id: ArrayString::new(),
        })
    }
}

pub struct TlsStream {
    stream: Option<Stream>,
    id: ArrayString<[u8; 32]>,
}

impl TlsStream {
    pub fn get_tcp(&self) -> Option<&TcpStream> {
        match &self.stream {
            Some(Stream::Ssl(stream)) => Some(stream.get_ref()),
            Some(Stream::MidHandshakeSsl(stream)) => Some(stream.get_ref()),
            None => None,
        }
    }

    pub fn set_id(&mut self, id: &str) {
        self.id = ArrayString::from_str(id).unwrap();
    }

    fn ensure_handshake(&mut self) -> Result<(), io::Error> {
        match &self.stream {
            Some(Stream::Ssl(_)) => Ok(()),
            Some(Stream::MidHandshakeSsl(_)) => match self.stream.take().unwrap() {
                Stream::MidHandshakeSsl(stream) => match stream.handshake() {
                    Ok(stream) => {
                        debug!("conn {}: tls handshake success", self.id);
                        self.stream = Some(Stream::Ssl(stream));

                        Ok(())
                    }
                    Err(HandshakeError::SetupFailure(_)) => {
                        Err(io::Error::from(io::ErrorKind::Other))
                    }
                    Err(HandshakeError::Failure(_)) => Err(io::Error::from(io::ErrorKind::Other)),
                    Err(HandshakeError::WouldBlock(stream)) => {
                        self.stream = Some(Stream::MidHandshakeSsl(stream));

                        Err(io::Error::from(io::ErrorKind::WouldBlock))
                    }
                },
                _ => unreachable!(),
            },
            None => Err(io::Error::from(io::ErrorKind::Other)),
        }
    }
}

impl Read for TlsStream {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, io::Error> {
        self.ensure_handshake()?;

        match &mut self.stream {
            Some(Stream::Ssl(stream)) => SslStream::read(stream, buf),
            _ => unreachable!(),
        }
    }
}

impl Write for TlsStream {
    fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
        self.ensure_handshake()?;

        match &mut self.stream {
            Some(Stream::Ssl(stream)) => SslStream::write(stream, buf),
            _ => unreachable!(),
        }
    }

    fn flush(&mut self) -> Result<(), io::Error> {
        Ok(())
    }
}
