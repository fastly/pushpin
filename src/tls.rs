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
use std::fmt;
use std::fs;
use std::io;
use std::io::{Read, Write};
use std::mem;
use std::path;
use std::path::{Path, PathBuf};
use std::str::FromStr;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::SystemTime;

const DOMAIN_LEN_MAX: usize = 253;

enum IdentityError {
    InvalidName,
    CertMetadata(PathBuf, io::Error),
    KeyMetadata(PathBuf, io::Error),
    SslContext(ErrorStack),
    CertContent(PathBuf, ErrorStack),
    KeyContent(PathBuf, ErrorStack),
    CertCheck(ErrorStack),
}

impl fmt::Display for IdentityError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidName => write!(f, "invalid name"),
            Self::CertMetadata(fname, e) => {
                write!(f, "failed to read cert file metadata {:?}: {}", fname, e)
            }
            Self::KeyMetadata(fname, e) => {
                write!(f, "failed to read key file metadata {:?}: {}", fname, e)
            }
            Self::SslContext(e) => write!(f, "failed to create SSL context: {}", e),
            Self::CertContent(fname, e) => {
                write!(f, "failed to read cert content {:?}: {}", fname, e)
            }
            Self::KeyContent(fname, e) => {
                write!(f, "failed to read key content {:?}: {}", fname, e)
            }
            Self::CertCheck(e) => write!(f, "failed to check private key: {}", e),
        }
    }
}

struct Identity {
    ssl_context: SslContext,
    cert_fname: PathBuf,
    key_fname: PathBuf,
    modified: Option<SystemTime>,
}

impl Identity {
    fn from_name(dir: &Path, name: &str) -> Result<Self, IdentityError> {
        // forbid long names
        if name.len() > DOMAIN_LEN_MAX {
            return Err(IdentityError::InvalidName);
        }

        // forbid control chars and '/', for safe filesystem usage
        for c in name.chars() {
            if (c as u32) < 0x20 || path::is_separator(c) {
                return Err(IdentityError::InvalidName);
            }
        }

        let cert_fname = dir.join(Path::new(&format!("{}.crt", name)));

        let cert_metadata = match fs::metadata(&cert_fname) {
            Ok(md) => md,
            Err(e) => return Err(IdentityError::CertMetadata(cert_fname, e)),
        };

        let key_fname = dir.join(Path::new(&format!("{}.key", name)));

        let key_metadata = match fs::metadata(&key_fname) {
            Ok(md) => md,
            Err(e) => return Err(IdentityError::KeyMetadata(key_fname, e)),
        };

        let cert_modified = cert_metadata.modified();
        let key_modified = key_metadata.modified();

        let modified = if cert_modified.is_ok() && key_modified.is_ok() {
            Some(cmp::max(cert_modified.unwrap(), key_modified.unwrap()))
        } else {
            None
        };

        let mut ctx = match SslContextBuilder::new(SslMethod::tls()) {
            Ok(ctx) => ctx,
            Err(e) => return Err(IdentityError::SslContext(e)),
        };

        if let Err(e) = ctx.set_certificate_chain_file(&cert_fname) {
            return Err(IdentityError::CertContent(cert_fname, e));
        }

        if let Err(e) = ctx.set_private_key_file(&key_fname, SslFiletype::PEM) {
            return Err(IdentityError::KeyContent(key_fname, e));
        }

        if let Err(e) = ctx.check_private_key() {
            return Err(IdentityError::CertCheck(e));
        }

        Ok(Self {
            ssl_context: ctx.build(),
            cert_fname,
            key_fname,
            modified,
        })
    }
}

fn modified_after(fnames: &[&Path], t: SystemTime) -> Result<bool, io::Error> {
    for fname in fnames {
        match fs::metadata(fname)?.modified() {
            Ok(modified) if modified > t => return Ok(true),
            _ => {}
        }
    }

    Ok(false)
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

    fn get_by_domain<'a>(&'a self, domain: &str) -> Option<IdentityRef<'a>> {
        let name = domain.to_lowercase();

        // try to find a file named after the exact host, then try with a
        //   wildcard pattern at the same subdomain level. the filename
        //   format uses underscores instead of asterisks. so, a domain of
        //   www.example.com will attempt to be matched against a file named
        //   www.example.com.crt and _.example.com.crt. wildcards at other
        //   levels are not supported

        if let Some(identity) = self.get_by_name(&name) {
            return Some(identity);
        }

        let pos = match name.find('.') {
            Some(pos) => pos,
            None => return None,
        };

        let name = format!("_{}", &name[pos..]);

        if let Some(identity) = self.get_by_name(&name) {
            return Some(identity);
        }

        None
    }

    fn get_by_name<'a>(&'a self, name: &str) -> Option<IdentityRef<'a>> {
        self.ensure_updated(name);

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

    fn ensure_updated(&self, name: &str) {
        let mut data = self.data.lock().unwrap();

        let mut update = false;

        if let Some(value) = data.get(name) {
            if let Some(modified) = value.modified {
                update = match modified_after(&[&value.cert_fname, &value.key_fname], modified) {
                    Ok(b) => b,
                    Err(_) => true,
                };
            }
        } else {
            update = true;
        }

        if update {
            let identity = match Identity::from_name(&self.dir, name) {
                Ok(identity) => identity,
                Err(e) => {
                    debug!("failed to load cert {}: {}", name, e);
                    return;
                }
            };

            data.insert(String::from(name), identity);

            debug!("loaded cert: {}", name);
        }
    }
}

enum Stream<'a> {
    Ssl(SslStream<&'a mut TcpStream>),
    MidHandshakeSsl(MidHandshakeSslStream<&'a mut TcpStream>),
    NoSsl,
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

                    match cache.get_by_domain(name) {
                        Some(ctx) => ctx,
                        None => match &default_cert {
                            Some(default_cert) => match cache.get_by_name(default_cert) {
                                Some(ctx) => ctx,
                                None => return Err(SniError::ALERT_FATAL),
                            },
                            None => return Err(SniError::ALERT_FATAL),
                        },
                    }
                }
                None => match &default_cert {
                    Some(default_cert) => match cache.get_by_name(default_cert) {
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
        let mut tcp_stream_boxed = Box::new(stream);

        let tcp_stream: &mut TcpStream = &mut tcp_stream_boxed;

        // safety: TlsStream will take ownership of tcp_stream, and the value
        // referred to by tcp_stream is on the heap, and tcp_stream will not
        // be dropped until TlsStream is dropped, so the value referred to
        // by tcp_stream will remain valid for the lifetime of TlsStream.
        // further, tcp_stream is a mutable reference, and will only ever
        // be exclusively mutably accessed, either when wrapped by SslStream
        // or MidHandshakeSslStream, or when known to be not wrapped
        let tcp_stream: &'static mut TcpStream = unsafe { mem::transmute(tcp_stream) };

        let stream = match self.acceptor.accept(tcp_stream) {
            Ok(stream) => Stream::Ssl(stream),
            Err(HandshakeError::SetupFailure(e)) => return Err(e),
            Err(HandshakeError::Failure(stream)) => Stream::MidHandshakeSsl(stream),
            Err(HandshakeError::WouldBlock(stream)) => Stream::MidHandshakeSsl(stream),
        };

        Ok(TlsStream {
            stream,
            tcp_stream: tcp_stream_boxed,
            id: ArrayString::new(),
        })
    }
}

pub struct TlsStream {
    stream: Stream<'static>,
    tcp_stream: Box<TcpStream>,
    id: ArrayString<[u8; 32]>,
}

impl TlsStream {
    pub fn get_tcp(&mut self) -> &mut TcpStream {
        match &mut self.stream {
            Stream::Ssl(stream) => stream.get_mut(),
            Stream::MidHandshakeSsl(stream) => stream.get_mut(),
            Stream::NoSsl => &mut self.tcp_stream,
        }
    }

    pub fn set_id(&mut self, id: &str) {
        self.id = ArrayString::from_str(id).unwrap();
    }

    pub fn shutdown(&mut self) -> Result<(), io::Error> {
        match &mut self.stream {
            Stream::Ssl(stream) => match stream.shutdown() {
                Ok(_) => {
                    debug!("conn {}: tls shutdown sent", self.id);

                    Ok(())
                }
                Err(e) => Err(match e.into_io_error() {
                    Ok(e) => e,
                    Err(_) => io::Error::from(io::ErrorKind::Other),
                }),
            },
            _ => Err(io::Error::from(io::ErrorKind::Other)),
        }
    }

    fn ensure_handshake(&mut self) -> Result<(), io::Error> {
        match &self.stream {
            Stream::Ssl(_) => Ok(()),
            Stream::MidHandshakeSsl(_) => match mem::replace(&mut self.stream, Stream::NoSsl) {
                Stream::MidHandshakeSsl(stream) => match stream.handshake() {
                    Ok(stream) => {
                        debug!("conn {}: tls handshake success", self.id);
                        self.stream = Stream::Ssl(stream);

                        Ok(())
                    }
                    Err(HandshakeError::SetupFailure(_)) => {
                        Err(io::Error::from(io::ErrorKind::Other))
                    }
                    Err(HandshakeError::Failure(_)) => Err(io::Error::from(io::ErrorKind::Other)),
                    Err(HandshakeError::WouldBlock(stream)) => {
                        self.stream = Stream::MidHandshakeSsl(stream);

                        Err(io::Error::from(io::ErrorKind::WouldBlock))
                    }
                },
                _ => unreachable!(),
            },
            Stream::NoSsl => Err(io::Error::from(io::ErrorKind::Other)),
        }
    }
}

impl Read for TlsStream {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, io::Error> {
        self.ensure_handshake()?;

        match &mut self.stream {
            Stream::Ssl(stream) => SslStream::read(stream, buf),
            _ => unreachable!(),
        }
    }
}

impl Write for TlsStream {
    fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
        self.ensure_handshake()?;

        match &mut self.stream {
            Stream::Ssl(stream) => SslStream::write(stream, buf),
            _ => unreachable!(),
        }
    }

    fn flush(&mut self) -> Result<(), io::Error> {
        Ok(())
    }
}
