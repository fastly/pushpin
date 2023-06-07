/*
 * Copyright (C) 2020-2023 Fanout, Inc.
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
use openssl::error::ErrorStack;
use openssl::pkey::PKey;
use openssl::ssl::{
    self, HandshakeError, MidHandshakeSslStream, NameType, SniError, SslAcceptor, SslConnector,
    SslContext, SslContextBuilder, SslFiletype, SslMethod, SslStream, SslVerifyMode,
};
use openssl::x509::X509;
use std::any::Any;
use std::cmp;
use std::collections::HashMap;
use std::fmt;
use std::fs;
use std::io;
use std::io::{Read, Write};
use std::marker::PhantomData;
use std::mem;
use std::path;
use std::path::{Path, PathBuf};
use std::ptr;
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

        #[allow(clippy::unnecessary_unwrap)]
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
                update = modified_after(&[&value.cert_fname, &value.key_fname], modified)
                    .unwrap_or(true);
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

trait ReadWrite: Read + Write + Any + Send {
    fn as_any(&mut self) -> &mut dyn Any;
    fn into_any(self: Box<Self>) -> Box<dyn Any>;
}

impl<T: Read + Write + Any + Send> ReadWrite for T {
    fn as_any(&mut self) -> &mut dyn Any {
        self
    }

    fn into_any(self: Box<Self>) -> Box<dyn Any> {
        self
    }
}

enum Stream<T> {
    Ssl(SslStream<T>),
    MidHandshakeSsl(MidHandshakeSslStream<T>),
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

    pub fn new_self_signed() -> Self {
        let mut acceptor = SslAcceptor::mozilla_intermediate(SslMethod::tls()).unwrap();

        let cert_pem = concat!(
            "-----BEGIN CERTIFICATE-----\n",
            "MIIBnzCCAQgCCQCxQ/hcU1Ac0TANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAls\n",
            "b2NhbGhvc3QwHhcNMjMwNjA3MDUxNjMxWhcNMjMwNjA4MDUxNjMxWjAUMRIwEAYD\n",
            "VQQDDAlsb2NhbGhvc3QwgZ8wDQYJKoZIhvcNAQEBBQADgY0AMIGJAoGBAOgNk9yF\n",
            "6A4Ey/cxWlz9U0aecVEyBVS72X/a+ySVySEgvzs54sA7oWOzgiMsNsQjJiJE5yv3\n",
            "Kr0nfuPBEADxGufJOdy/S57y3unxtpE9nUJY2WVuBjXIN9MzH3fIMQWXX9yBr/Ij\n",
            "cfvnDDOAu6VtBFCrdZNjIz0NJZlyUuI+YQJHAgMBAAEwDQYJKoZIhvcNAQELBQAD\n",
            "gYEAPo38vYyMUi3uzK2F/jXhQW5R/6eIxL/KvNuY+gVpogVea66jx+y2VgEaUC3C\n",
            "wqH1SHPt5EN511NED0lEfH5CCpjGxjcl5jbTMb4/F0jCaIk+H8uzyOl6aHP5OTT3\n",
            "WCt/kBSEiCkG5FSE9dHThd2plDCwlnNOOx13UzCga6eh0gc=\n",
            "-----END CERTIFICATE-----"
        );

        let key_pem = concat!(
            "-----BEGIN PRIVATE KEY-----\n",
            "MIICdgIBADANBgkqhkiG9w0BAQEFAASCAmAwggJcAgEAAoGBAOgNk9yF6A4Ey/cx\n",
            "Wlz9U0aecVEyBVS72X/a+ySVySEgvzs54sA7oWOzgiMsNsQjJiJE5yv3Kr0nfuPB\n",
            "EADxGufJOdy/S57y3unxtpE9nUJY2WVuBjXIN9MzH3fIMQWXX9yBr/IjcfvnDDOA\n",
            "u6VtBFCrdZNjIz0NJZlyUuI+YQJHAgMBAAECgYEAjeBHR+vjDjcmkXLuQa5srN+Q\n",
            "fskrczwK5d338M1XlFaWNNrWZRvQN8n3xhNxRIgM96TTBhFvYwjzzsIqS7kd7fD+\n",
            "dm0B5X2q4Fw0fT8cuD7WPrYRjR5akvJmCFZhfBq6ndEQ8o1U7MU2i5EXEne4IeuH\n",
            "mdxC3D5rBaYquKJfuCECQQD5dY7ZZQWVxVRFdfufDOLelgRp6XL425hqbgcXT9tW\n",
            "jCRAERG9cztxh5YH330KyVwiZlNwybYjgdawjJ2Zz4k3AkEA7iMvMIqJfwktdjIz\n",
            "MX3UcYa9qSAOdmbvhZ8rXizj+RzOYiX+PVODg4IEJtcwoHfH1Xfq/kk+oeIkb9O7\n",
            "IdqXcQJAVQ66eHGzp8+y3kROWXsBWDf6pUpOQ4BMxe1iSZaXCTmbmqS3Ucuatyku\n",
            "BN01O5pQ6gHN7aU5j33UADrR+gIDnQJACJfycwD81z3Aizxiho2w5evj2j+S5gju\n",
            "6daFnR9nlqzIcdhHJXVnEI7XkYNAePn5lyV9sHF6NiNQB00PurgFsQJAcVp3zlrL\n",
            "hT4mZo+nwtQIAV8UaI8e4/jGmHuPdkCqQRyGVLqf7IWiKvsYptlnn0wcluQiCZVk\n",
            "wEt0AuQ0pfQPig==\n",
            "-----END PRIVATE KEY-----"
        );

        let cert = X509::from_pem(cert_pem.as_bytes()).unwrap();
        let key = PKey::private_key_from_pem(key_pem.as_bytes()).unwrap();

        acceptor.set_certificate(&cert).unwrap();
        acceptor.set_private_key(&key).unwrap();

        Self {
            acceptor: acceptor.build(),
        }
    }

    pub fn accept(
        &self,
        stream: mio::net::TcpStream,
    ) -> Result<TlsStream<mio::net::TcpStream>, ssl::Error> {
        let result = TlsStream::new(false, stream, |stream| {
            let stream = match self.acceptor.accept(stream) {
                Ok(stream) => Stream::Ssl(stream),
                Err(HandshakeError::SetupFailure(e)) => return Err(e.into()),
                Err(HandshakeError::Failure(stream)) => return Err(stream.into_error()),
                Err(HandshakeError::WouldBlock(stream)) => Stream::MidHandshakeSsl(stream),
            };

            Ok(stream)
        });

        match result {
            Ok(stream) => Ok(stream),
            Err((_, e)) => Err(e),
        }
    }
}

pub enum VerifyMode {
    Full,
    None,
}

#[derive(Debug)]
pub enum TlsStreamError {
    Io(io::Error),
    Ssl(ErrorStack),
    Unusable,
}

impl TlsStreamError {
    fn into_io_error(self) -> io::Error {
        match self {
            TlsStreamError::Io(e) => e,
            _ => io::Error::from(io::ErrorKind::Other),
        }
    }
}

fn replace_at<T, F>(value_at: &mut T, replace_fn: F)
where
    F: FnOnce(T) -> T,
{
    // SAFETY: we use ptr::read to get the current value and then put a new
    // value in its place with ptr::write before returning
    unsafe {
        let p = value_at as *mut T;
        ptr::write(p, replace_fn(ptr::read(p)));
    }
}

pub struct TlsStream<T> {
    stream: Stream<&'static mut Box<dyn ReadWrite>>,
    plain_stream: Box<Box<dyn ReadWrite>>,
    id: ArrayString<64>,
    client: bool,
    _marker: PhantomData<T>,
}

impl<T> TlsStream<T>
where
    T: Read + Write + Any + Send,
{
    pub fn connect(
        domain: &str,
        stream: T,
        verify_mode: VerifyMode,
    ) -> Result<Self, (T, ssl::Error)> {
        Self::new(true, stream, |stream| {
            let mut connector = SslConnector::builder(SslMethod::tls())?;

            if let VerifyMode::None = verify_mode {
                connector.set_verify(SslVerifyMode::NONE);
            }

            let connector = connector.build();

            let stream = match connector.connect(domain, stream) {
                Ok(stream) => Stream::Ssl(stream),
                Err(HandshakeError::SetupFailure(e)) => return Err(e.into()),
                Err(HandshakeError::Failure(stream)) => return Err(stream.into_error()),
                Err(HandshakeError::WouldBlock(stream)) => Stream::MidHandshakeSsl(stream),
            };

            Ok(stream)
        })
    }

    pub fn get_inner<'a>(&'a mut self) -> &'a mut T {
        let plain_stream: &'a mut Box<dyn ReadWrite> = match &mut self.stream {
            Stream::Ssl(stream) => stream.get_mut(),
            Stream::MidHandshakeSsl(stream) => stream.get_mut(),
            Stream::NoSsl => Box::as_mut(&mut self.plain_stream),
        };

        let plain_stream: &mut dyn ReadWrite = Box::as_mut(plain_stream);

        plain_stream.as_any().downcast_mut().unwrap()
    }

    #[allow(clippy::result_unit_err)]
    pub fn set_id(&mut self, id: &str) -> Result<(), ()> {
        self.id = match ArrayString::from_str(id) {
            Ok(s) => s,
            Err(_) => return Err(()),
        };

        Ok(())
    }

    pub fn ensure_handshake(&mut self) -> Result<(), TlsStreamError> {
        match &self.stream {
            Stream::Ssl(_) => Ok(()),
            Stream::MidHandshakeSsl(_) => match mem::replace(&mut self.stream, Stream::NoSsl) {
                Stream::MidHandshakeSsl(stream) => match stream.handshake() {
                    Ok(stream) => {
                        debug!("{} {}: tls handshake success", self.log_prefix(), self.id);
                        self.stream = Stream::Ssl(stream);

                        Ok(())
                    }
                    Err(HandshakeError::SetupFailure(e)) => Err(TlsStreamError::Ssl(e)),
                    Err(HandshakeError::Failure(stream)) => {
                        let e = stream.into_error();

                        match e.into_io_error() {
                            Ok(e) => Err(TlsStreamError::Io(e)),
                            Err(e) => match e.ssl_error() {
                                Some(e) => Err(TlsStreamError::Ssl(e.clone())),
                                None => {
                                    Err(TlsStreamError::Io(io::Error::from(io::ErrorKind::Other)))
                                }
                            },
                        }
                    }
                    Err(HandshakeError::WouldBlock(stream)) => {
                        self.stream = Stream::MidHandshakeSsl(stream);

                        Err(TlsStreamError::Io(io::Error::from(
                            io::ErrorKind::WouldBlock,
                        )))
                    }
                },
                _ => unreachable!(),
            },
            Stream::NoSsl => Err(TlsStreamError::Unusable),
        }
    }

    pub fn shutdown(&mut self) -> Result<(), io::Error> {
        match &mut self.stream {
            Stream::Ssl(stream) => match stream.shutdown() {
                Ok(_) => {
                    debug!("{} {}: tls shutdown sent", self.log_prefix(), self.id);

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

    pub fn change_inner<F, U>(mut self, change_fn: F) -> TlsStream<U>
    where
        F: FnOnce(T) -> U,
        U: Read + Write + Any + Send,
    {
        let plain_stream: &mut Box<dyn ReadWrite> = Box::as_mut(&mut self.plain_stream);

        replace_at(plain_stream, |plain_stream: Box<dyn ReadWrite>| {
            let plain_stream: Box<T> = plain_stream.into_any().downcast().unwrap();
            let plain_stream: U = change_fn(*plain_stream);

            Box::new(plain_stream)
        });

        // SAFETY: nothing is changing except the phantom data type
        unsafe { mem::transmute(self) }
    }

    fn new<F>(client: bool, stream: T, init_fn: F) -> Result<Self, (T, ssl::Error)>
    where
        F: FnOnce(
            &'static mut Box<dyn ReadWrite>,
        ) -> Result<Stream<&'static mut Box<dyn ReadWrite>>, ssl::Error>,
    {
        // box the stream, casting to ReadWrite
        let inner_box: Box<dyn ReadWrite> = Box::new(stream);

        // box it again. this way we have a pointer-to-a-pointer on the heap,
        // allowing us to change where the outer pointer points to later on
        // without changing its location
        let mut outer_box: Box<Box<dyn ReadWrite>> = Box::new(inner_box);

        // get the outer pointer
        let outer: &mut Box<dyn ReadWrite> = Box::as_mut(&mut outer_box);

        // safety: TlsStream will take ownership of outer_box, and the value
        // referred to by outer_box is on the heap, and outer_box will not
        // be dropped until TlsStream is dropped, so the value referred to
        // by outer_box will remain valid for the lifetime of TlsStream.
        // further, outer is a mutable reference, and will only ever be
        // exclusively mutably accessed, either when wrapped by SslStream
        // or MidHandshakeSslStream, or when known to be not wrapped
        let outer: &'static mut Box<dyn ReadWrite> = unsafe { mem::transmute(outer) };

        let stream = match init_fn(outer) {
            Ok(stream) => stream,
            Err(e) => {
                let inner_box: Box<dyn ReadWrite> = *outer_box;
                let stream: T = *inner_box.into_any().downcast().unwrap();

                return Err((stream, e));
            }
        };

        Ok(Self {
            stream,
            plain_stream: outer_box,
            id: ArrayString::new(),
            client,
            _marker: PhantomData,
        })
    }

    fn log_prefix(&self) -> &'static str {
        if self.client {
            "client-conn"
        } else {
            "conn"
        }
    }
}

impl<T> Read for TlsStream<T>
where
    T: Read + Write + Any + Send,
{
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, io::Error> {
        if let Err(e) = self.ensure_handshake() {
            return Err(e.into_io_error());
        }

        match &mut self.stream {
            Stream::Ssl(stream) => SslStream::read(stream, buf),
            _ => unreachable!(),
        }
    }
}

impl<T> Write for TlsStream<T>
where
    T: Read + Write + Any + Send,
{
    fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
        if let Err(e) = self.ensure_handshake() {
            return Err(e.into_io_error());
        }

        match &mut self.stream {
            Stream::Ssl(stream) => SslStream::write(stream, buf),
            _ => unreachable!(),
        }
    }

    fn flush(&mut self) -> Result<(), io::Error> {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Debug)]
    struct ReadWriteA {
        a: i32,
    }

    impl Read for ReadWriteA {
        fn read(&mut self, _buf: &mut [u8]) -> Result<usize, io::Error> {
            Err(io::Error::from(io::ErrorKind::WouldBlock))
        }
    }

    impl Write for ReadWriteA {
        fn write(&mut self, _buf: &[u8]) -> Result<usize, io::Error> {
            Err(io::Error::from(io::ErrorKind::WouldBlock))
        }

        fn flush(&mut self) -> Result<(), io::Error> {
            Ok(())
        }
    }

    #[derive(Debug)]
    struct ReadWriteB {
        b: i32,
    }

    impl Read for ReadWriteB {
        fn read(&mut self, _buf: &mut [u8]) -> Result<usize, io::Error> {
            Err(io::Error::from(io::ErrorKind::WouldBlock))
        }
    }

    impl Write for ReadWriteB {
        fn write(&mut self, _buf: &[u8]) -> Result<usize, io::Error> {
            Err(io::Error::from(io::ErrorKind::WouldBlock))
        }

        fn flush(&mut self) -> Result<(), io::Error> {
            Ok(())
        }
    }

    #[derive(Debug)]
    struct ReadWriteC {
        c: i32,
    }

    impl Read for ReadWriteC {
        fn read(&mut self, _buf: &mut [u8]) -> Result<usize, io::Error> {
            Err(io::Error::from(io::ErrorKind::Other))
        }
    }

    impl Write for ReadWriteC {
        fn write(&mut self, _buf: &[u8]) -> Result<usize, io::Error> {
            Err(io::Error::from(io::ErrorKind::Other))
        }

        fn flush(&mut self) -> Result<(), io::Error> {
            Err(io::Error::from(io::ErrorKind::Other))
        }
    }

    #[test]
    fn test_get_change_inner() {
        let a = ReadWriteA { a: 1 };
        let mut stream = TlsStream::connect("localhost", a, VerifyMode::Full).unwrap();
        assert_eq!(stream.get_inner().a, 1);
        let mut stream = stream.change_inner(|_| ReadWriteB { b: 2 });
        assert_eq!(stream.get_inner().b, 2);
    }

    #[test]
    fn test_connect_error() {
        let c = ReadWriteC { c: 1 };
        let (stream, e) = match TlsStream::connect("localhost", c, VerifyMode::Full) {
            Ok(_) => panic!("unexpected success"),
            Err(ret) => ret,
        };
        assert_eq!(stream.c, 1);
        assert_eq!(e.into_io_error().unwrap().kind(), io::ErrorKind::Other);
    }
}
