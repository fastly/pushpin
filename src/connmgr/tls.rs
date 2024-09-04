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

use crate::core::event::{self, ReadinessExt};
use crate::core::io::{AsyncRead, AsyncWrite};
use crate::core::net::AsyncTcpStream;
use crate::core::reactor::Registration;
use crate::core::task::get_reactor;
use crate::core::waker::{RefWake, RefWaker, RefWakerData};
use arrayvec::ArrayString;
use log::debug;
use mio::net::TcpStream;
use openssl::error::ErrorStack;
use openssl::pkey::PKey;
use openssl::ssl::{
    self, HandshakeError, MidHandshakeSslStream, NameType, SniError, SslAcceptor, SslConnector,
    SslContext, SslContextBuilder, SslFiletype, SslMethod, SslStream, SslVerifyMode,
};
use openssl::x509::X509;
use std::any::Any;
use std::cell::{Ref, RefCell};
use std::cmp;
use std::collections::HashMap;
use std::fmt;
use std::fs;
use std::future::Future;
use std::io;
use std::io::{Read, Write};
use std::marker::PhantomData;
use std::mem;
use std::os::fd::{FromRawFd, IntoRawFd};
use std::path;
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::ptr;
use std::str::FromStr;
use std::sync::{Arc, Mutex, MutexGuard};
use std::task::{Context, Poll, Waker};
use std::time::{Duration, Instant, SystemTime};

const DOMAIN_LEN_MAX: usize = 253;
const CONFIG_CACHE_TTL: Duration = Duration::from_secs(60);

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
            "MIICpDCCAYwCCQDkzIPOmEje1DANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAls\n",
            "b2NhbGhvc3QwHhcNMjMwNjA4MjIxMjE3WhcNMjMwNjA5MjIxMjE3WjAUMRIwEAYD\n",
            "VQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC7\n",
            "Lj9eFGJ0hsbtn1ebNaakK/f3tktLbYhT7eZ547T1OYfPs9stk7ZMaNPXv/CPbz4x\n",
            "5NZC89rghUScZYFGAfQE5Rxrso8vUzUSAzRebSm5LG3BYsHyKf7lZkD3cK1kBPtl\n",
            "lRMQ0/Jg6WkUglYWV8/2Cm8SoJpdllBbbl0bOu1S8QMswb4IrZ1UE130tbP5SnSb\n",
            "bke2ahVrnJ2lC63sD64rBedYWm5FSHlJ2ciRPe1tr+owqSVrHrjZjrTHovyMVsff\n",
            "BFJ1iVfnzkxR/tyGFlHHngkRdwtO81Orc9yAIe8v1U3y6F+Tk2LIwW4PYh/xqj4W\n",
            "ijPttBqrybO5T+jDV/PNAgMBAAEwDQYJKoZIhvcNAQELBQADggEBADQmWrdkwdtR\n",
            "Fu+9GBjXsmjPNvN72Da4UtLf8Y+LgA/XYKGCFaGxpFm+61DOpbjpUR3B8MRQzn45\n",
            "x4/ZcNmRrYj7yiBlj/Y/bQKfBLaTG2JCJ2ffdBgZMPG3U9wLQKsUbOsdznkSYG18\n",
            "CGTM3btznIlW7pkDsw3CRkKoYWNRd0STzifa2ASCEgRAFemYIj/YysVw6nWTtIHY\n",
            "5Ez+TDwOpUkuk2haE6UvaxR0+q3r+10907HqZejyLmSY+FQk1ylAfJtJcJvpbrB+\n",
            "kQa8kPmOm+hnLGDXFI0qfBHfuiKDX7yi39aFgWI/Mbz5wKHr0IIoJmncayYacnGX\n",
            "coUhiF2hpf0=\n",
            "-----END CERTIFICATE-----\n"
        );

        let key_pem = concat!(
            "-----BEGIN PRIVATE KEY-----\n",
            "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC7Lj9eFGJ0hsbt\n",
            "n1ebNaakK/f3tktLbYhT7eZ547T1OYfPs9stk7ZMaNPXv/CPbz4x5NZC89rghUSc\n",
            "ZYFGAfQE5Rxrso8vUzUSAzRebSm5LG3BYsHyKf7lZkD3cK1kBPtllRMQ0/Jg6WkU\n",
            "glYWV8/2Cm8SoJpdllBbbl0bOu1S8QMswb4IrZ1UE130tbP5SnSbbke2ahVrnJ2l\n",
            "C63sD64rBedYWm5FSHlJ2ciRPe1tr+owqSVrHrjZjrTHovyMVsffBFJ1iVfnzkxR\n",
            "/tyGFlHHngkRdwtO81Orc9yAIe8v1U3y6F+Tk2LIwW4PYh/xqj4WijPttBqrybO5\n",
            "T+jDV/PNAgMBAAECggEAB1lIeZwZRXPpKXkhCmHv2fAz+xC4Igz51jm327854orQ\n",
            "rzHjgAWVmahf8M+DVU5Lxc+zLcu/IyN4Tx+ZFLOM7ghEtmG7R2Nf6QYhLzff9Hov\n",
            "EPGcpbJKZJ1AHbbZx9x+Nj3FEtsPYAip7Hk1ggkOjB1awQN3LAdzvjM2CpSkrqXg\n",
            "c4GQ4hK3tkyIZxPiC6pr6246+UjakzFGXT5zzQajbkFHrM8s4Wn42tbdd6N14jgv\n",
            "5mdR6bAzusG8P3IRlO4zQ/NQTCXI6kz4SdTlZERaxt35pThXRkcifMPcGRTageax\n",
            "l1ZxBIRjTSp60tPR6fcH8std8hEcRExcOeCmOld4gQKBgQDwWz5vQCUyvza6l/O3\n",
            "G6huXmQcpFea5PpWtII55bp3DTen6SrB3cGGtKZZqfN7IXFODUIUIvQEf4bI8r0y\n",
            "Vu6Sypnq+CIbRN5aul7X+do5gEpFEZW+BdbBN+mCBaf16xaxS9GWZj1wCWSjyE4s\n",
            "PE7jEbLgVPwd+8FmK3XemaF7bQKBgQDHXQC7XjZ0OxfeAOVLz1vShBBlbDtJEonY\n",
            "cuSveZqEiLEaUFuU3XFuExbyfCRjNNsz6JROXvCO2KQ6HbI/tkZCmJYoQ8mhhAF+\n",
            "5QN9hGZgMPcvPEZW4AEih5qVrwO3IQGF3YJnYLvyyroEjQ7nSwCf/HPCF5Gl/K41\n",
            "QPRlM5e94QKBgFyhPYGQfgV9rbDhqLpTvWizle934o8+WcAalumLQH5rKJzcfm7y\n",
            "cIfijQ2XMs+sRsdm0qWCBvrIzwAYlJOW7yDBVeo5MKPDudHLa4verZxldboCmev+\n",
            "whH641IJrf5XWIqBhsdopZrM8+0u3/mqUFiwVHiiJ/vCL3mZnDZqjNJNAoGAFge2\n",
            "7v2IMuvcxVGABRKS6P5i+XIuUvLTfLGlh6Z+ZqrcNzYuCJM315wQaxdAxh2vI1tO\n",
            "GCLxnjdeXnWtntC7jtxhq21iOJDnwWf5LMOWtIZ0qimU9ECon3IwqN3AIVpqWqqR\n",
            "oG7WFgxE5f/YZ8Kn/QXenNIR7C+x6HyXBR/gYsECgYEAg6PSkpYdOxaTZzaxIxS3\n",
            "HUUy7H1+wzV/ZCKIMZEfH23kUiHMZXjp3xI1FTlGcbMFpOkmjwi+MFHEMcvmwzmc\n",
            "owdohdh7ngo60nkgMwz5TyWBWDdT+Otogi7F37qAt/fjd4xmNjsyTY4b2OwuP1/S\n",
            "X7Rmwy1AQ2WKrwOSy4d3xDs=\n",
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

impl From<ssl::Error> for TlsStreamError {
    fn from(e: ssl::Error) -> Self {
        match e.into_io_error() {
            Ok(e) => Self::Io(e),
            Err(e) => match e.ssl_error() {
                Some(e) => Self::Ssl(e.clone()),
                None => Self::Io(io::Error::from(io::ErrorKind::Other)),
            },
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

fn apply_wants(e: &ssl::Error, interests: &mut Option<mio::Interest>) {
    match e.code() {
        ssl::ErrorCode::WANT_READ => *interests = Some(mio::Interest::READABLE),
        ssl::ErrorCode::WANT_WRITE => *interests = Some(mio::Interest::WRITABLE),
        _ => {}
    }
}

struct Connector {
    inner: Arc<SslConnector>,
    created: Instant,
}

struct Connectors {
    verify_full: Option<Connector>,
    verify_none: Option<Connector>,
}

// represents a cache of reusable data among sessions. internally, this data
// consists of SslConnectors for the purpose of caching root certs read from
// disk. the type is given a vague name in order to avoid committing to what
// exactly is cached.
pub struct TlsConfigCache {
    connectors: Mutex<Connectors>,
}

impl Default for TlsConfigCache {
    fn default() -> Self {
        Self::new()
    }
}

impl TlsConfigCache {
    pub fn new() -> Self {
        Self {
            connectors: Mutex::new(Connectors {
                verify_full: None,
                verify_none: None,
            }),
        }
    }

    fn get_connector(&self, verify_mode: VerifyMode) -> Result<Arc<SslConnector>, ErrorStack> {
        let mut connectors = self
            .connectors
            .lock()
            .expect("failed to obtain lock on tls config cache");

        let slot = match verify_mode {
            VerifyMode::Full => &mut connectors.verify_full,
            VerifyMode::None => &mut connectors.verify_none,
        };

        let connector = match slot {
            Some(c) if c.created.elapsed() < CONFIG_CACHE_TTL => &c.inner,
            _ => {
                let mut builder = SslConnector::builder(SslMethod::tls())?;

                match verify_mode {
                    VerifyMode::Full => builder.set_verify(SslVerifyMode::PEER),
                    VerifyMode::None => builder.set_verify(SslVerifyMode::NONE),
                }

                let c = slot.insert(Connector {
                    inner: Arc::new(builder.build()),
                    created: Instant::now(),
                });

                &c.inner
            }
        };

        Ok(Arc::clone(connector))
    }
}

pub struct TlsStream<T> {
    stream: Stream<&'static mut Box<dyn ReadWrite>>,
    plain_stream: Box<Box<dyn ReadWrite>>,
    id: ArrayString<64>,
    client: bool,
    interests_for_handshake: Option<mio::Interest>,
    interests_for_shutdown: Option<mio::Interest>,
    interests_for_read: Option<mio::Interest>,
    interests_for_write: Option<mio::Interest>,
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
        config_cache: &TlsConfigCache,
    ) -> Result<Self, (T, ssl::Error)> {
        Self::new(true, stream, |stream| {
            let connector = config_cache.get_connector(verify_mode)?;

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

    pub fn interests_for_handshake(&self) -> Option<mio::Interest> {
        self.interests_for_handshake
    }

    pub fn interests_for_shutdown(&self) -> Option<mio::Interest> {
        self.interests_for_shutdown
    }

    pub fn interests_for_read(&self) -> Option<mio::Interest> {
        self.interests_for_read
    }

    pub fn interests_for_write(&self) -> Option<mio::Interest> {
        self.interests_for_write
    }

    pub fn ensure_handshake(&mut self) -> Result<(), TlsStreamError> {
        self.interests_for_handshake = None;

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
                    Err(HandshakeError::Failure(stream)) => Err(stream.into_error().into()),
                    Err(HandshakeError::WouldBlock(stream)) => {
                        apply_wants(stream.error(), &mut self.interests_for_handshake);

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
        self.interests_for_shutdown = None;

        let stream = match &mut self.stream {
            Stream::Ssl(stream) => stream,
            _ => return Err(io::Error::from(io::ErrorKind::Other)),
        };

        if let Err(e) = stream.shutdown() {
            apply_wants(&e, &mut self.interests_for_shutdown);

            match e.into_io_error() {
                Ok(e) => return Err(e),
                Err(_) => return Err(io::Error::from(io::ErrorKind::Other)),
            }
        }

        debug!("{} {}: tls shutdown sent", self.log_prefix(), self.id);

        Ok(())
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
            id: ArrayString::from("<unknown>").unwrap(),
            client,
            interests_for_handshake: None,
            interests_for_shutdown: None,
            interests_for_read: None,
            interests_for_write: None,
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

    fn ssl_read(&mut self, buf: &mut [u8]) -> Result<usize, TlsStreamError> {
        self.interests_for_read = None;

        if let Err(e) = self.ensure_handshake() {
            match &e {
                TlsStreamError::Io(e) if e.kind() == io::ErrorKind::WouldBlock => {
                    self.interests_for_read = self.interests_for_handshake;
                }
                _ => {}
            }

            return Err(e);
        }

        let stream = match &mut self.stream {
            Stream::Ssl(stream) => stream,
            _ => unreachable!(),
        };

        match stream.ssl_read(buf) {
            Ok(size) => Ok(size),
            Err(e) if e.code() == ssl::ErrorCode::ZERO_RETURN => Ok(0),
            Err(e) => {
                apply_wants(&e, &mut self.interests_for_read);

                Err(e.into())
            }
        }
    }

    fn ssl_write(&mut self, buf: &[u8]) -> Result<usize, TlsStreamError> {
        self.interests_for_write = None;

        if let Err(e) = self.ensure_handshake() {
            match &e {
                TlsStreamError::Io(e) if e.kind() == io::ErrorKind::WouldBlock => {
                    self.interests_for_write = self.interests_for_handshake;
                }
                _ => {}
            }

            return Err(e);
        }

        let stream = match &mut self.stream {
            Stream::Ssl(stream) => stream,
            _ => unreachable!(),
        };

        match stream.ssl_write(buf) {
            Ok(size) => Ok(size),
            Err(e) => {
                apply_wants(&e, &mut self.interests_for_write);

                Err(e.into())
            }
        }
    }
}

impl<T> Read for TlsStream<T>
where
    T: Read + Write + Any + Send,
{
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, io::Error> {
        match self.ssl_read(buf) {
            Ok(size) => Ok(size),
            Err(e) => Err(e.into_io_error()),
        }
    }
}

impl<T> Write for TlsStream<T>
where
    T: Read + Write + Any + Send,
{
    fn write(&mut self, buf: &[u8]) -> Result<usize, io::Error> {
        match self.ssl_write(buf) {
            Ok(size) => Ok(size),
            Err(e) => Err(e.into_io_error()),
        }
    }

    fn flush(&mut self) -> Result<(), io::Error> {
        Ok(())
    }
}

struct TlsOpInner {
    readiness: event::Readiness,
    waker: Option<(Waker, mio::Interest)>,
}

struct TlsOp {
    inner: RefCell<TlsOpInner>,
}

impl TlsOp {
    fn new() -> Self {
        Self {
            inner: RefCell::new(TlsOpInner {
                readiness: None,
                waker: None,
            }),
        }
    }

    fn readiness(&self) -> event::Readiness {
        self.inner.borrow().readiness
    }

    pub fn set_readiness(&self, readiness: event::Readiness) {
        self.inner.borrow_mut().readiness = readiness;
    }

    pub fn clear_readiness(&self, readiness: mio::Interest) {
        let inner = &mut *self.inner.borrow_mut();

        if let Some(cur) = inner.readiness.take() {
            inner.readiness = cur.remove(readiness);
        }
    }

    fn set_waker(&self, waker: &Waker, interest: mio::Interest) {
        let inner = &mut *self.inner.borrow_mut();

        let waker = if let Some((current_waker, _)) = inner.waker.take() {
            if current_waker.will_wake(waker) {
                // keep the current waker
                current_waker
            } else {
                // switch to the new waker
                waker.clone()
            }
        } else {
            // we didn't have a waker yet, so we'll use this one
            waker.clone()
        };

        inner.waker = Some((waker, interest));
    }

    fn clear_waker(&self) {
        let inner = &mut *self.inner.borrow_mut();

        inner.waker = None;
    }

    fn apply_readiness(&self, readiness: mio::Interest) {
        let inner = &mut *self.inner.borrow_mut();

        let (became_readable, became_writable) = {
            let prev_readiness = inner.readiness;

            inner.readiness.merge(readiness);

            (
                !prev_readiness.contains_any(mio::Interest::READABLE)
                    && inner.readiness.contains_any(mio::Interest::READABLE),
                !prev_readiness.contains_any(mio::Interest::WRITABLE)
                    && inner.readiness.contains_any(mio::Interest::WRITABLE),
            )
        };

        if became_readable || became_writable {
            if let Some((_, interest)) = &inner.waker {
                if (became_readable && interest.is_readable())
                    || (became_writable && interest.is_writable())
                {
                    let (waker, _) = inner.waker.take().unwrap();
                    waker.wake();
                }
            }
        }
    }
}

pub struct TlsWaker {
    registration: RefCell<Option<Registration>>,
    handshake: TlsOp,
    shutdown: TlsOp,
    read: TlsOp,
    write: TlsOp,
}

#[allow(clippy::new_without_default)]
impl TlsWaker {
    pub fn new() -> Self {
        Self {
            registration: RefCell::new(None),
            handshake: TlsOp::new(),
            shutdown: TlsOp::new(),
            read: TlsOp::new(),
            write: TlsOp::new(),
        }
    }

    fn registration(&self) -> Ref<'_, Registration> {
        Ref::map(self.registration.borrow(), |b| b.as_ref().unwrap())
    }

    fn set_registration(&self, registration: Registration) {
        let readiness = registration.readiness();

        registration.clear_readiness(mio::Interest::READABLE | mio::Interest::WRITABLE);

        for op in [&self.handshake, &self.shutdown, &self.read, &self.write] {
            op.set_readiness(readiness);
        }

        *self.registration.borrow_mut() = Some(registration);
    }

    fn take_registration(&self) -> Registration {
        self.registration.borrow_mut().take().unwrap()
    }
}

impl RefWake for TlsWaker {
    fn wake(&self) {
        if let Some(readiness) = self.registration().readiness() {
            self.registration()
                .clear_readiness(mio::Interest::READABLE | mio::Interest::WRITABLE);

            for op in [&self.handshake, &self.shutdown, &self.read, &self.write] {
                op.apply_readiness(readiness);
            }
        }
    }
}

pub struct AsyncTlsStream<'a> {
    waker: RefWaker<'a, TlsWaker>,
    stream: Option<TlsStream<TcpStream>>,
}

impl<'a: 'b, 'b> AsyncTlsStream<'a> {
    pub fn new(mut s: TlsStream<TcpStream>, waker_data: &'a RefWakerData<TlsWaker>) -> Self {
        let registration = get_reactor()
            .register_io(
                s.get_inner(),
                mio::Interest::READABLE | mio::Interest::WRITABLE,
            )
            .unwrap();

        // assume I/O operations are ready to be attempted
        registration.set_readiness(Some(mio::Interest::READABLE | mio::Interest::WRITABLE));

        Self::new_with_registration(s, waker_data, registration)
    }

    pub fn connect(
        domain: &str,
        stream: AsyncTcpStream,
        verify_mode: VerifyMode,
        waker_data: &'a RefWakerData<TlsWaker>,
        config_cache: &TlsConfigCache,
    ) -> Result<Self, ssl::Error> {
        let (registration, stream) = stream.into_evented().into_parts();

        let stream = match TlsStream::connect(domain, stream, verify_mode, config_cache) {
            Ok(stream) => stream,
            Err((mut stream, e)) => {
                registration.deregister_io(&mut stream).unwrap();

                return Err(e);
            }
        };

        Ok(Self::new_with_registration(
            stream,
            waker_data,
            registration,
        ))
    }

    pub fn ensure_handshake(&'b mut self) -> EnsureHandshakeFuture<'a, 'b> {
        EnsureHandshakeFuture { s: self }
    }

    pub fn inner(&mut self) -> &mut TlsStream<TcpStream> {
        self.stream.as_mut().unwrap()
    }

    pub fn into_inner(mut self) -> TlsStream<TcpStream> {
        let mut stream = self.stream.take().unwrap();

        self.waker
            .registration()
            .deregister_io(stream.get_inner())
            .unwrap();

        stream
    }

    pub fn into_std(mut self) -> TlsStream<std::net::TcpStream> {
        let mut stream = self.stream.take().unwrap();

        self.waker
            .registration()
            .deregister_io(stream.get_inner())
            .unwrap();

        stream.change_inner(|stream| unsafe {
            std::net::TcpStream::from_raw_fd(stream.into_raw_fd())
        })
    }

    // assumes stream is in non-blocking mode
    pub fn from_std(
        stream: TlsStream<std::net::TcpStream>,
        waker_data: &'a RefWakerData<TlsWaker>,
    ) -> Self {
        let stream = stream.change_inner(TcpStream::from_std);

        Self::new(stream, waker_data)
    }

    fn new_with_registration(
        s: TlsStream<TcpStream>,
        waker_data: &'a RefWakerData<TlsWaker>,
        registration: Registration,
    ) -> Self {
        let waker = RefWaker::new(waker_data);
        waker.set_registration(registration);

        waker.registration().set_waker_persistent(true);
        waker.registration().set_waker(
            waker.as_std(&mut mem::MaybeUninit::uninit()),
            mio::Interest::READABLE | mio::Interest::WRITABLE,
        );

        Self {
            waker,
            stream: Some(s),
        }
    }
}

impl<'a> Drop for AsyncTlsStream<'a> {
    fn drop(&mut self) {
        let registration = self.waker.take_registration();

        if let Some(stream) = &mut self.stream {
            registration.deregister_io(stream.get_inner()).unwrap();
        }
    }
}

impl AsyncRead for AsyncTlsStream<'_> {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &mut [u8],
    ) -> Poll<Result<usize, io::Error>> {
        let f = &mut *self;

        let registration = f.waker.registration();
        let op = &f.waker.read;
        let stream = f.stream.as_mut().unwrap();

        let interests = stream.interests_for_read();

        if let Some(interests) = interests {
            if !op.readiness().contains_any(interests) {
                op.set_waker(cx.waker(), interests);

                return Poll::Pending;
            }
        }

        if !registration.pull_from_budget_with_waker(cx.waker()) {
            return Poll::Pending;
        }

        match stream.read(buf) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                let interests = stream.interests_for_read().unwrap();
                op.clear_readiness(interests);
                op.set_waker(cx.waker(), interests);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn cancel(&mut self) {
        let op = &self.waker.read;

        op.clear_waker();
    }
}

impl AsyncWrite for AsyncTlsStream<'_> {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut Context,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        let f = &mut *self;

        let registration = f.waker.registration();
        let op = &f.waker.write;
        let stream = f.stream.as_mut().unwrap();

        let interests = stream.interests_for_write();

        if let Some(interests) = interests {
            if !op.readiness().contains_any(interests) {
                op.set_waker(cx.waker(), interests);

                return Poll::Pending;
            }
        }

        if !registration.pull_from_budget_with_waker(cx.waker()) {
            return Poll::Pending;
        }

        match stream.write(buf) {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                let interests = stream.interests_for_write().unwrap();
                op.clear_readiness(interests);
                op.set_waker(cx.waker(), interests);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn poll_close(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Result<(), io::Error>> {
        let f = &mut *self;

        let registration = f.waker.registration();
        let op = &f.waker.shutdown;
        let stream = f.stream.as_mut().unwrap();

        let interests = stream.interests_for_shutdown();

        if let Some(interests) = interests {
            if !op.readiness().contains_any(interests) {
                op.set_waker(cx.waker(), interests);

                return Poll::Pending;
            }
        }

        if !registration.pull_from_budget_with_waker(cx.waker()) {
            return Poll::Pending;
        }

        match stream.shutdown() {
            Ok(size) => Poll::Ready(Ok(size)),
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                let interests = stream.interests_for_shutdown().unwrap();
                op.clear_readiness(interests);
                op.set_waker(cx.waker(), interests);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }

    fn is_writable(&self) -> bool {
        let op = &self.waker.write;
        let stream = self.stream.as_ref().unwrap();

        if let Some(interests) = stream.interests_for_write() {
            op.readiness().contains_any(interests)
        } else {
            true
        }
    }

    fn cancel(&mut self) {
        let write_op = &self.waker.write;
        let shutdown_op = &self.waker.shutdown;

        write_op.clear_waker();
        shutdown_op.clear_waker();
    }
}

pub struct EnsureHandshakeFuture<'a, 'b> {
    s: &'b mut AsyncTlsStream<'a>,
}

impl Future for EnsureHandshakeFuture<'_, '_> {
    type Output = Result<(), TlsStreamError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let f = &mut *self;

        let registration = f.s.waker.registration();
        let op = &f.s.waker.handshake;
        let stream = f.s.stream.as_mut().unwrap();

        let interests = stream.interests_for_handshake();

        if let Some(interests) = interests {
            if !op.readiness().contains_any(interests) {
                op.set_waker(cx.waker(), interests);

                return Poll::Pending;
            }
        }

        if !registration.pull_from_budget_with_waker(cx.waker()) {
            return Poll::Pending;
        }

        match stream.ensure_handshake() {
            Ok(()) => Poll::Ready(Ok(())),
            Err(TlsStreamError::Io(e)) if e.kind() == io::ErrorKind::WouldBlock => {
                let interests = stream.interests_for_handshake().unwrap();
                op.clear_readiness(interests);
                op.set_waker(cx.waker(), interests);

                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }
}

impl Drop for EnsureHandshakeFuture<'_, '_> {
    fn drop(&mut self) {
        let op = &self.s.waker.handshake;

        op.clear_waker();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::executor::Executor;
    use crate::core::io::{AsyncReadExt, AsyncWriteExt};
    use crate::core::net::AsyncTcpListener;
    use crate::core::reactor::Reactor;
    use std::str;

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
        let mut stream =
            TlsStream::connect("localhost", a, VerifyMode::Full, &TlsConfigCache::new()).unwrap();
        assert_eq!(stream.get_inner().a, 1);
        let mut stream = stream.change_inner(|_| ReadWriteB { b: 2 });
        assert_eq!(stream.get_inner().b, 2);
    }

    #[test]
    fn test_connect_error() {
        let c = ReadWriteC { c: 1 };
        let (stream, e) =
            match TlsStream::connect("localhost", c, VerifyMode::Full, &TlsConfigCache::new()) {
                Ok(_) => panic!("unexpected success"),
                Err(ret) => ret,
            };
        assert_eq!(stream.c, 1);
        assert_eq!(e.into_io_error().unwrap().kind(), io::ErrorKind::Other);
    }

    #[test]
    fn test_async_tlsstream() {
        let reactor = Reactor::new(3); // 3 registrations
        let executor = Executor::new(2); // 2 tasks

        let spawner = executor.spawner();

        executor
            .spawn(async move {
                let addr = "127.0.0.1:0".parse().unwrap();
                let listener = AsyncTcpListener::bind(addr).expect("failed to bind");
                let acceptor = TlsAcceptor::new_self_signed();
                let addr = listener.local_addr().unwrap();

                spawner
                    .spawn(async move {
                        let stream = AsyncTcpStream::connect(&[addr]).await.unwrap();
                        let tls_waker_data = RefWakerData::new(TlsWaker::new());
                        let mut stream = AsyncTlsStream::connect(
                            "localhost",
                            stream,
                            VerifyMode::None,
                            &tls_waker_data,
                            &TlsConfigCache::new(),
                        )
                        .unwrap();

                        stream.ensure_handshake().await.unwrap();

                        let size = stream.write("hello".as_bytes()).await.unwrap();
                        assert_eq!(size, 5);

                        stream.close().await.unwrap();
                    })
                    .unwrap();

                let (stream, _) = listener.accept().await.unwrap();
                let stream = acceptor.accept(stream).unwrap();

                let tls_waker_data = RefWakerData::new(TlsWaker::new());
                let mut stream = AsyncTlsStream::new(stream, &tls_waker_data);

                let mut resp = [0u8; 1024];
                let mut resp = io::Cursor::new(&mut resp[..]);

                loop {
                    let mut buf = [0; 1024];

                    let size = stream.read(&mut buf).await.unwrap();
                    if size == 0 {
                        break;
                    }

                    resp.write(&buf[..size]).unwrap();
                }

                let size = resp.position() as usize;
                let resp = str::from_utf8(&resp.get_ref()[..size]).unwrap();

                assert_eq!(resp, "hello");

                stream.close().await.unwrap();
            })
            .unwrap();

        executor.run(|timeout| reactor.poll(timeout)).unwrap();
    }
}
