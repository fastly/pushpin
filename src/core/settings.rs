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

use ini::Ini;
use std::collections::{HashMap, HashSet};
use std::convert::TryFrom;
use std::env;
use std::net::IpAddr;
use std::path::{Path, PathBuf};
use std::str::FromStr;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum SettingsError {
    #[error(transparent)]
    Ini(#[from] ini::Error),

    #[error("line without '=' in [{0}]: {1}")]
    MissingEquals(String, String),

    #[error("repeated section [{0}]")]
    RepeatedSection(String),

    #[error("duplicate key {0}")]
    DuplicateKey(String),

    #[error("key outside of a section: {0}")]
    KeyOutsideSection(String),

    #[error("{key} must be {expected}: {value}")]
    InvalidValue {
        key: String,
        value: String,
        expected: &'static str,
    },

    #[error("invalid tcp address: {0}")]
    InvalidTcpAddress(String),

    #[error("invalid libdir: {0}")]
    InvalidLibdir(String),
}

pub struct Settings {
    values: HashMap<String, String>,
    libdir: PathBuf,
    rundir: PathBuf,
    ipc_prefix: String,
    port_offset: i32,
    config_dir: PathBuf,
}

fn invalid_value(key: &str, value: &str, expected: &'static str) -> SettingsError {
    SettingsError::InvalidValue {
        key: key.to_string(),
        value: value.to_string(),
        expected,
    }
}

fn read_file(path: &Path) -> Result<HashMap<String, String>, SettingsError> {
    read_ini(&Ini::load_from_file(path)?)
}

fn read_ini(ini: &Ini) -> Result<HashMap<String, String>, SettingsError> {
    let mut values = HashMap::new();
    let mut sections = HashSet::new();

    for (section, props) in ini.iter() {
        let Some(section) = section else {
            if let Some((key, _)) = props.iter().next() {
                return Err(SettingsError::KeyOutsideSection(key.to_string()));
            }

            continue;
        };

        let section = section.to_string();

        if !sections.insert(section.clone()) {
            return Err(SettingsError::RepeatedSection(section));
        }

        for (key, value) in props.iter() {
            // rust-ini reads a key until it finds '=' or ':', not until the end of the line, so a
            // line missing its '=' gets merged with the next line into one key containing a
            // newline (e.g. "foo\nbar=3" gives key "foo\nbar" with value "3")
            if let Some((line, _)) = key.split_once('\n') {
                return Err(SettingsError::MissingEquals(section, line.to_string()));
            }

            let key = format!("{section}/{key}");

            if values.insert(key.clone(), value.to_string()).is_some() {
                return Err(SettingsError::DuplicateKey(key));
            }
        }
    }

    Ok(values)
}

impl Settings {
    pub fn load(path: &Path) -> Result<Self, SettingsError> {
        let config_dir = path.parent().unwrap_or(Path::new("")).to_path_buf();

        Self::from_values(read_file(path)?, config_dir)
    }

    fn from_values(
        values: HashMap<String, String>,
        config_dir: PathBuf,
    ) -> Result<Self, SettingsError> {
        let get = |key: &str| values.get(key).filter(|v| !v.is_empty()).cloned();

        let libdir = get("global/libdir").map(PathBuf::from).unwrap_or_else(|| {
            if Path::new("src/bin/pushpin.rs").exists() {
                // Running in tree
                env::current_dir().unwrap_or_default().join("src")
            } else {
                // Use compiled value
                PathBuf::from(env!("LIB_DIR"))
            }
        });

        let rundir = get("global/rundir")
            .or_else(|| get("runner/rundir")) // Fallback to runner section (deprecated)
            .map(PathBuf::from)
            .unwrap_or_default();

        let ipc_prefix = values
            .get("global/ipc_prefix")
            .cloned()
            .unwrap_or_else(|| "pushpin-".to_string());

        let port_offset = match get("global/port_offset") {
            Some(v) => v
                .parse()
                .map_err(|_| invalid_value("global/port_offset", &v, "a number"))?,
            None => 0,
        };

        let mut settings = Self {
            values,
            libdir,
            rundir,
            ipc_prefix,
            port_offset,
            config_dir,
        };

        let include = match settings.values.get("global/include").map(String::as_str) {
            // If include is exactly "internal.conf", rewrite relative to libdir.
            // TODO: remove this hack at next major version
            Some("internal.conf") => "{libdir}/internal.conf".to_string(),
            Some(include) => include.to_string(),
            None => String::new(),
        };

        let include = settings.resolve_vars(&include)?;

        if !include.is_empty() {
            // If include is a relative path, then use it relative to the config file location
            let include = settings.config_dir.join(include);

            for (key, value) in read_file(&include)? {
                settings.values.entry(key).or_insert(value);
            }
        }

        Ok(settings)
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.values.get(key).map(String::as_str)
    }

    pub fn config_dir(&self) -> &Path {
        &self.config_dir
    }

    pub fn set_ipc_prefix(&mut self, ipc_prefix: &str) {
        self.ipc_prefix = ipc_prefix.to_string();
    }

    pub fn set_port_offset(&mut self, port_offset: i32) {
        self.port_offset = port_offset;
    }

    pub fn string(&self, key: &str) -> Result<Option<String>, SettingsError> {
        match self.get(key) {
            None => Ok(None),
            Some(v) if v.contains(',') => Err(invalid_value(key, v, "a single value")),
            Some(v) => self.resolve_vars(v).map(Some),
        }
    }

    pub fn list(&self, key: &str) -> Result<Vec<String>, SettingsError> {
        match self.get(key) {
            None => Ok(Vec::new()),
            Some(value) => value
                .split(',')
                .map(str::trim)
                .filter(|v| !v.is_empty())
                .map(|v| self.resolve_vars(v))
                .collect(),
        }
    }

    pub fn number<T: FromStr + Default>(&self, key: &str) -> Result<Option<T>, SettingsError> {
        match self.get(key) {
            None | Some("-1") => Ok(None),
            Some("") => Ok(Some(T::default())),
            Some(v) => v
                .parse()
                .map(Some)
                .map_err(|_| invalid_value(key, v, "a number")),
        }
    }

    pub fn bool(&self, key: &str) -> Result<Option<bool>, SettingsError> {
        match self.get(key) {
            None => Ok(None),
            Some("" | "0") => Ok(Some(false)),
            Some("1") => Ok(Some(true)),
            Some(v) if v.eq_ignore_ascii_case("false") => Ok(Some(false)),
            Some(v) if v.eq_ignore_ascii_case("true") => Ok(Some(true)),
            Some(v) => Err(invalid_value(key, v, "true or false")),
        }
    }

    pub fn ip_addr(&self, key: &str) -> Result<Option<IpAddr>, SettingsError> {
        match self.get(key) {
            None | Some("") => Ok(None),
            Some(v) => v
                .parse()
                .map(Some)
                .map_err(|_| invalid_value(key, v, "an IP address")),
        }
    }

    pub fn octal(&self, key: &str) -> Result<Option<u32>, SettingsError> {
        match self.get(key) {
            None | Some("-1") => Ok(None),
            Some(v) => u32::from_str_radix(v, 8)
                .map(Some)
                .map_err(|_| invalid_value(key, v, "an octal number")),
        }
    }

    pub fn port(&self, key: &str) -> Result<Option<u16>, SettingsError> {
        match self.get(key) {
            None | Some("-1") => Ok(None),
            Some("") => Ok(Some(0)),
            Some(v) => match v.parse::<u16>() {
                Ok(0) => Ok(Some(0)),
                Ok(port) => u16::try_from(i32::from(port) + self.port_offset)
                    .map(Some)
                    .map_err(|_| invalid_value(key, v, "a port")),
                Err(_) => Err(invalid_value(key, v, "a port")),
            },
        }
    }

    fn resolve_vars(&self, s: &str) -> Result<String, SettingsError> {
        let mut out = s
            .replace(
                "{libdir}",
                self.libdir.to_str().ok_or_else(|| {
                    SettingsError::InvalidLibdir(self.libdir.display().to_string())
                })?,
            )
            .replace(
                "{rundir}",
                self.rundir.to_str().expect("rundir must be utf-8"),
            )
            .replace("{ipc_prefix}", &self.ipc_prefix);

        // Adjust tcp ports
        let mut at = 0;
        while let Some(i) = out[at..].find("tcp://") {
            let host = at + i + 6;

            // Skip past an IPv6 address, since it contains colons
            let host_end = match out[host..].strip_prefix('[') {
                Some(rest) => {
                    let Some(n) = rest.find(']') else {
                        return Err(SettingsError::InvalidTcpAddress(s.to_string()));
                    };

                    host + 1 + n
                }
                None => host,
            };

            let Some(colon) = out[host_end..].find(':') else {
                return Err(SettingsError::InvalidTcpAddress(s.to_string()));
            };

            let start = host_end + colon + 1;
            let end = out[start..]
                .find(|c: char| !c.is_ascii_digit())
                .map_or(out.len(), |n| start + n);

            let Ok(port) = out[start..end].parse::<i32>() else {
                return Err(SettingsError::InvalidTcpAddress(s.to_string()));
            };

            let port = (port + self.port_offset).to_string();
            out.replace_range(start..end, &port);
            at = start + port.len();
        }

        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    mod helpers {
        use super::*;
        use crate::core::test_dir;
        use std::fs;
        use std::path::PathBuf;

        pub(super) fn parse(text: &str) -> Result<Settings, SettingsError> {
            let ini = Ini::load_from_str(text).map_err(ini::Error::Parse)?;
            Settings::from_values(read_ini(&ini)?, PathBuf::new())
        }

        pub(super) fn read_value<T>(
            key: &str,
            value: Option<&str>,
            read: impl Fn(&Settings, &str) -> Result<T, SettingsError>,
        ) -> Result<T, String> {
            let (section, name) = key.split_once('/').unwrap();
            let conf = match value {
                Some(v) => format!("[{}]\n{}={}\n", section, name, v),
                None => String::new(),
            };

            let mut settings = parse(&conf).unwrap();
            settings.set_port_offset(1);

            read(&settings, key).map_err(|e| e.to_string())
        }

        pub(super) fn write_conf(test: &str, file: &str, contents: &str) -> PathBuf {
            let dir = test_dir().join("settings").join(test);
            fs::create_dir_all(&dir).unwrap();

            let path = dir.join(file);
            fs::write(&path, contents).unwrap();

            path
        }
    }

    #[test]
    fn example_config_loads() {
        let settings = Settings::load(Path::new("examples/config/pushpin.conf")).unwrap();

        assert_eq!(settings.get("global/rundir"), Some("run"));
        assert_eq!(
            settings.get("runner/services"),
            Some("connmgr,proxy,handler")
        );
        assert_eq!(settings.get("proxy/cdn_loop"), Some(""));
        assert_eq!(settings.get("proxy/nonexistent"), None);

        // From internal.conf, through the include
        assert_eq!(
            settings.get("proxy/connmgr_in_specs"),
            Some("ipc://{rundir}/{ipc_prefix}connmgr-out")
        );
    }

    mod syntax {
        use super::helpers::*;

        #[test]
        fn values_read_as_written() {
            let cases = [
                (
                    "[Proxy]\nSig_Iss=pushpin\n",
                    "Proxy/Sig_Iss",
                    Some("pushpin"),
                ),
                ("[Proxy]\nSig_Iss=pushpin\n", "proxy/sig_iss", None),
                (
                    "[proxy]\n  sig_iss  =  pushpin  \n",
                    "proxy/sig_iss",
                    Some("pushpin"),
                ),
                (
                    "[proxy]\nupstream_key=base64:YWJjZA==\n",
                    "proxy/upstream_key",
                    Some("base64:YWJjZA=="),
                ),
            ];

            for (conf, key, expected) in cases {
                assert_eq!(parse(conf).unwrap().get(key), expected, "{}", conf);
            }
        }

        #[test]
        fn malformed_config_fails() {
            let cases = [
                (
                    "[proxy]\nworkers=2\nworkers=4\n",
                    "duplicate key proxy/workers",
                ),
                (
                    "[proxy]\nworkers=2\n[handler]\n[proxy]\ndebug=true\n",
                    "repeated section [proxy]",
                ),
                (
                    "[proxy]\nworkers 4\nsig_iss=pushpin\n",
                    "line without '=' in [proxy]: workers 4",
                ),
                (
                    "workers=4\n[proxy]\nsig_iss=pushpin\n",
                    "key outside of a section: workers",
                ),
                (
                    "[global]\nport_offset=one\n",
                    "global/port_offset must be a number: one",
                ),
            ];

            for (conf, expected) in cases {
                let err = parse(conf).err().map(|e| e.to_string());
                assert_eq!(err.as_deref(), Some(expected), "{}", conf);
            }
        }
    }

    mod includes {
        use super::helpers::*;
        use super::*;
        use std::io;

        #[test]
        fn internal_conf_found_in_libdir() {
            let settings = parse("[global]\ninclude=internal.conf\n").unwrap();
            assert_eq!(
                settings.get("proxy/connmgr_in_specs"),
                Some("ipc://{rundir}/{ipc_prefix}connmgr-out")
            );
        }

        #[test]
        fn relative_to_config() {
            write_conf("include_relative", "extra.conf", "[proxy]\nsig_iss=extra\n");
            let main = write_conf(
                "include_relative",
                "pushpin.conf",
                "[global]\ninclude=extra.conf\n",
            );

            let settings = Settings::load(&main).unwrap();
            assert_eq!(settings.get("proxy/sig_iss"), Some("extra"));
        }

        #[test]
        fn main_overrides_include() {
            write_conf(
                "include_override",
                "extra.conf",
                "[proxy]\nsig_iss=extra\nworkers=2\n",
            );
            let main = write_conf(
                "include_override",
                "pushpin.conf",
                "[global]\ninclude=extra.conf\n[proxy]\nsig_iss=main\n",
            );

            let settings = Settings::load(&main).unwrap();
            assert_eq!(settings.get("proxy/sig_iss"), Some("main"));
            assert_eq!(settings.get("proxy/workers"), Some("2"));
        }

        #[test]
        fn not_nested() {
            write_conf("include_nested", "second.conf", "[proxy]\nsig_iss=second\n");
            write_conf(
                "include_nested",
                "first.conf",
                "[global]\ninclude=second.conf\n[proxy]\nworkers=2\n",
            );
            let main = write_conf(
                "include_nested",
                "pushpin.conf",
                "[global]\ninclude=first.conf\n",
            );

            let settings = Settings::load(&main).unwrap();
            assert_eq!(settings.get("proxy/workers"), Some("2"));
            assert_eq!(settings.get("proxy/sig_iss"), None);
        }

        #[test]
        fn empty_ignored() {
            let settings = parse("[global]\ninclude=\n[proxy]\nworkers=2\n").unwrap();
            assert_eq!(settings.get("proxy/workers"), Some("2"));
        }

        #[test]
        fn missing_file_fails() {
            let r = parse("[global]\ninclude=nope.conf\n");
            assert!(matches!(
                r,
                Err(SettingsError::Ini(ini::Error::Io(e))) if e.kind() == io::ErrorKind::NotFound
            ));
        }
    }

    mod variables {
        use super::helpers::*;
        use super::*;

        #[test]
        fn libdir_falls_back_to_source() {
            let in_tree = env::current_dir().unwrap_or_default().join("src");
            let libdir = |conf: &str| parse(conf).unwrap().libdir;

            assert_eq!(
                libdir("[global]\nlibdir=/usr/lib/pushpin\n"),
                Path::new("/usr/lib/pushpin")
            );
            assert_eq!(libdir(""), in_tree);
            assert_eq!(libdir("[global]\nlibdir=\n"), in_tree);
        }

        #[test]
        fn rundir_prefers_global() {
            let rundir = |conf: &str| parse(conf).unwrap().rundir;

            assert_eq!(
                rundir("[runner]\nrundir=/var/run/pushpin\n"),
                Path::new("/var/run/pushpin")
            );
            assert_eq!(
                rundir("[global]\nrundir=/var/run/global\n[runner]\nrundir=/var/run/runner\n"),
                Path::new("/var/run/global")
            );
        }

        #[test]
        fn defaults_when_unset() {
            let unset = parse("").unwrap();
            assert_eq!(unset.ipc_prefix, "pushpin-");
            assert_eq!(unset.port_offset, 0);

            let empty = parse("[global]\nipc_prefix=\nport_offset=\n").unwrap();
            assert_eq!(empty.ipc_prefix, "");
            assert_eq!(empty.port_offset, 0);
        }
    }

    mod ports {
        use super::helpers::*;

        #[test]
        fn tcp_shifted_by_offset() {
            let mut settings = parse("").unwrap();
            settings.set_port_offset(1);

            let cases = [
                (
                    "tcp://host:5560/abcdefghijklmnop",
                    Ok("tcp://host:5561/abcdefghijklmnop"),
                ),
                ("tcp://[::1]:5560", Ok("tcp://[::1]:5561")),
                ("ipc://run/pushpin-5560", Ok("ipc://run/pushpin-5560")),
                (
                    "tcp://127.0.0.1",
                    Err("invalid tcp address: tcp://127.0.0.1"),
                ),
                (
                    "tcp://127.0.0.1:",
                    Err("invalid tcp address: tcp://127.0.0.1:"),
                ),
                (
                    "tcp://[::1:5560",
                    Err("invalid tcp address: tcp://[::1:5560"),
                ),
            ];

            for (addr, expected) in cases {
                let got = settings.resolve_vars(addr).map_err(|e| e.to_string());
                assert_eq!(got.as_deref().map_err(String::as_str), expected, "{}", addr);
            }
        }

        #[test]
        fn offset_from_config() {
            let settings =
                parse("[global]\nport_offset=1\n[handler]\npush_in_spec=tcp://127.0.0.1:5560\n")
                    .unwrap();
            assert_eq!(
                settings.string("handler/push_in_spec").unwrap(),
                Some("tcp://127.0.0.1:5561".to_string())
            );
        }
    }

    mod getters {
        use super::helpers::*;
        use super::*;

        #[test]
        fn string_is_single_value() {
            let read = |v| read_value("proxy/sig_iss", v, Settings::string);

            assert_eq!(read(None), Ok(None));
            assert_eq!(read(Some("")), Ok(Some(String::new())));
            assert_eq!(read(Some("pushpin")), Ok(Some("pushpin".to_string())));
            assert_eq!(
                read(Some("a,b")),
                Err("proxy/sig_iss must be a single value: a,b".to_string())
            );
        }

        #[test]
        fn list_split_and_trimmed() {
            let read = |v| read_value("runner/services", v, Settings::list).unwrap();

            assert!(read(None).is_empty());
            assert!(read(Some("")).is_empty());
            assert_eq!(
                read(Some("connmgr, proxy ,,handler")),
                vec!["connmgr", "proxy", "handler"]
            );
        }

        #[test]
        fn list_items_substituted() {
            let settings = parse(
                "[global]\nrundir=run\n[proxy]\nm2a_in_specs=ipc://{rundir}/{ipc_prefix}m2zhttp-out,ipc://{rundir}/{ipc_prefix}m2zws-out\n",
            )
            .unwrap();
            assert_eq!(
                settings.list("proxy/m2a_in_specs").unwrap(),
                vec![
                    "ipc://run/pushpin-m2zhttp-out",
                    "ipc://run/pushpin-m2zws-out"
                ]
            );
        }

        #[test]
        fn number_minus_one_is_unset() {
            let read = |v| read_value("proxy/workers", v, Settings::number::<u32>);

            assert_eq!(read(None), Ok(None));
            assert_eq!(read(Some("-1")), Ok(None));
            assert_eq!(read(Some("")), Ok(Some(0)));
            assert_eq!(read(Some("4")), Ok(Some(4)));
            assert_eq!(
                read(Some("4x")),
                Err("proxy/workers must be a number: 4x".to_string())
            );
            assert_eq!(
                read(Some("1,2")),
                Err("proxy/workers must be a number: 1,2".to_string())
            );
        }

        #[test]
        fn bool_accepted_values() {
            let read = |v| read_value("proxy/debug", v, Settings::bool);

            assert_eq!(read(None), Ok(None));
            assert_eq!(read(Some("true")), Ok(Some(true)));
            assert_eq!(read(Some("TRUE")), Ok(Some(true)));
            assert_eq!(read(Some("1")), Ok(Some(true)));
            assert_eq!(read(Some("false")), Ok(Some(false)));
            assert_eq!(read(Some("0")), Ok(Some(false)));
            assert_eq!(read(Some("")), Ok(Some(false)));
            assert_eq!(
                read(Some("yes")),
                Err("proxy/debug must be true or false: yes".to_string())
            );
            assert_eq!(
                read(Some("2")),
                Err("proxy/debug must be true or false: 2".to_string())
            );
        }

        #[test]
        fn octal_rejects_invalid() {
            let read = |v| read_value("handler/ipc_file_mode", v, Settings::octal);

            assert_eq!(read(None), Ok(None));
            assert_eq!(read(Some("-1")), Ok(None));
            assert_eq!(read(Some("777")), Ok(Some(0o777)));
            assert_eq!(
                read(Some("")),
                Err("handler/ipc_file_mode must be an octal number: ".to_string())
            );
            assert_eq!(
                read(Some("abc")),
                Err("handler/ipc_file_mode must be an octal number: abc".to_string())
            );
            assert_eq!(
                read(Some("789")),
                Err("handler/ipc_file_mode must be an octal number: 789".to_string())
            );
        }

        #[test]
        fn port_shifted_except_zero() {
            // read_value applies a port offset of 1
            let read = |v| read_value("handler/push_in_http_port", v, Settings::port);

            assert_eq!(read(None), Ok(None));
            assert_eq!(read(Some("-1")), Ok(None));
            assert_eq!(read(Some("")), Ok(Some(0)));
            assert_eq!(read(Some("0")), Ok(Some(0)));
            assert_eq!(read(Some("5561")), Ok(Some(5562)));
            assert_eq!(
                read(Some("70000")),
                Err("handler/push_in_http_port must be a port: 70000".to_string())
            );
            assert_eq!(
                read(Some("65535")),
                Err("handler/push_in_http_port must be a port: 65535".to_string())
            );
        }

        #[test]
        fn overrides_apply() {
            let mut settings = parse(
                "[global]\nrundir=run\n[handler]\nstats_spec=ipc://{rundir}/{ipc_prefix}stats\npush_in_spec=tcp://127.0.0.1:5560\npush_in_http_port=5561\n",
            )
            .unwrap();
            settings.set_ipc_prefix("p1-");
            settings.set_port_offset(10);
            assert_eq!(
                settings.string("handler/stats_spec").unwrap(),
                Some("ipc://run/p1-stats".to_string())
            );
            assert_eq!(
                settings.string("handler/push_in_spec").unwrap(),
                Some("tcp://127.0.0.1:5570".to_string())
            );
            assert_eq!(
                settings.port("handler/push_in_http_port").unwrap(),
                Some(5571)
            );
        }
    }
}
