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
use std::path::Path;
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
}

pub struct Settings {
    values: HashMap<String, String>,
    libdir: String,
    rundir: String,
    ipc_prefix: String,
    port_offset: i32,
}

fn invalid_value(key: &str, value: &str, expected: &'static str) -> SettingsError {
    SettingsError::InvalidValue {
        key: key.to_string(),
        value: value.to_string(),
        expected,
    }
}

fn read_file(path: &Path) -> Result<HashMap<String, String>, SettingsError> {
    let mut values = HashMap::new();
    let mut sections = HashSet::new();

    for (section, props) in Ini::load_from_file(path)?.iter() {
        let Some(section) = section else {
            if let Some((key, _)) = props.iter().next() {
                return Err(SettingsError::KeyOutsideSection(key.to_string()));
            }

            continue;
        };

        let section = section.to_lowercase();

        if !sections.insert(section.clone()) {
            return Err(SettingsError::RepeatedSection(section));
        }

        for (key, value) in props.iter() {
            if let Some((line, _)) = key.split_once('\n') {
                return Err(SettingsError::MissingEquals(section, line.to_string()));
            }

            let key = format!("{section}/{}", key.to_lowercase());

            if values.insert(key.clone(), value.to_string()).is_some() {
                return Err(SettingsError::DuplicateKey(key));
            }
        }
    }

    Ok(values)
}

impl Settings {
    pub fn load(path: &Path) -> Result<Self, SettingsError> {
        let values = read_file(path)?;

        let get = |key: &str| values.get(key).filter(|v| !v.is_empty()).cloned();

        let libdir = get("global/libdir").unwrap_or_else(|| {
            if Path::new("src/bin/pushpin.rs").exists() {
                // Running in tree
                env::current_dir()
                    .unwrap_or_default()
                    .join("src")
                    .to_string_lossy()
                    .into_owned()
            } else {
                // Use compiled value
                env!("LIB_DIR").to_string()
            }
        });

        let rundir = get("global/rundir")
            .or_else(|| get("runner/rundir")) // Fallback to runner section (deprecated)
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
            let include = path.parent().unwrap_or(Path::new("")).join(include);

            for (key, value) in read_file(&include)? {
                settings.values.entry(key).or_insert(value);
            }
        }

        Ok(settings)
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.values.get(&key.to_lowercase()).map(String::as_str)
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

    pub fn adjust_port(&self, key: &str) -> Result<Option<u16>, SettingsError> {
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
            .replace("{libdir}", &self.libdir)
            .replace("{rundir}", &self.rundir)
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
    use crate::core::test_dir;
    use std::fmt::Debug;
    use std::fs;
    use std::path::PathBuf;

    #[test]
    fn load_example_config() {
        let settings = Settings::load(Path::new("examples/config/pushpin.conf")).unwrap();

        assert_eq!(settings.get("global/rundir"), Some("run"));
        assert_eq!(
            settings.get("runner/services"),
            Some("connmgr,proxy,handler")
        );
        assert_eq!(settings.get("proxy/cdn_loop"), Some(""));
        assert_eq!(settings.get("proxy/nonexistent"), None);
    }

    #[test]
    fn resolve_vars() {
        let mut settings = Settings::load(Path::new("examples/config/pushpin.conf")).unwrap();

        let include = settings.get("global/include").unwrap();
        let libdir = env::current_dir().unwrap().join("src");
        assert_eq!(
            settings.resolve_vars(include).unwrap(),
            format!("{}/internal.conf", libdir.display())
        );

        let stats_spec = settings.get("handler/stats_spec").unwrap();
        assert_eq!(
            settings.resolve_vars(stats_spec).unwrap(),
            "ipc://run/pushpin-stats"
        );

        settings.port_offset = 1;
        let push_in_spec = settings.get("handler/push_in_spec").unwrap();
        assert_eq!(
            settings.resolve_vars(push_in_spec).unwrap(),
            "tcp://127.0.0.1:5561"
        );
    }

    fn write_conf(test: &str, file: &str, contents: &str) -> PathBuf {
        let dir = test_dir().join("settings").join(test);
        fs::create_dir_all(&dir).unwrap();

        let path = dir.join(file);
        fs::write(&path, contents).unwrap();

        path
    }

    fn load_conf(test: &str, contents: &str) -> Result<Settings, SettingsError> {
        Settings::load(&write_conf(test, "pushpin.conf", contents))
    }

    fn in_tree_libdir() -> String {
        env::current_dir()
            .unwrap()
            .join("src")
            .to_string_lossy()
            .into_owned()
    }

    // Parsing

    #[test]
    fn uppercase_section_and_key() {
        let settings = load_conf("uppercase", "[Proxy]\nSig_Iss=pushpin\n").unwrap();
        assert_eq!(settings.get("proxy/sig_iss"), Some("pushpin"));
    }

    #[test]
    fn duplicate_key_errors() {
        let r = load_conf("dup_key", "[proxy]\nworkers=2\nworkers=4\n");
        assert!(matches!(r, Err(SettingsError::DuplicateKey(k)) if k == "proxy/workers"));
    }

    #[test]
    fn repeated_section_errors() {
        let r = load_conf(
            "dup_section",
            "[proxy]\nworkers=2\n[handler]\n[proxy]\ndebug=true\n",
        );
        assert!(matches!(r, Err(SettingsError::RepeatedSection(s)) if s == "proxy"));
    }

    #[test]
    fn comment_lines_ignored() {
        let settings = load_conf(
            "comments",
            "[runner]\n# a comment\n#http_port=7999\n; another comment\nlogdir=log\n",
        )
        .unwrap();
        assert_eq!(settings.get("runner/http_port"), None);
        assert_eq!(settings.get("runner/#http_port"), None);
        assert_eq!(settings.get("runner/logdir"), Some("log"));
    }

    #[test]
    fn empty_value() {
        let settings = load_conf("empty_value", "[proxy]\nupstream_key=\n").unwrap();
        assert_eq!(settings.get("proxy/upstream_key"), Some(""));
    }

    #[test]
    fn whitespace_trimmed() {
        let settings = load_conf("whitespace", "[proxy]\n  sig_iss  =  pushpin  \n").unwrap();
        assert_eq!(settings.get("proxy/sig_iss"), Some("pushpin"));
    }

    #[test]
    fn value_containing_equals() {
        let settings = load_conf("equals", "[proxy]\nupstream_key=base64:YWJjZA==\n").unwrap();
        assert_eq!(settings.get("proxy/upstream_key"), Some("base64:YWJjZA=="));
    }

    #[test]
    fn value_containing_colons() {
        let settings = load_conf(
            "colons",
            "[handler]\nmessage_keys=prod-a:file:/etc/pushpin/private/message-key-a\n",
        )
        .unwrap();
        assert_eq!(
            settings.get("handler/message_keys"),
            Some("prod-a:file:/etc/pushpin/private/message-key-a")
        );
    }

    #[test]
    fn quoted_value() {
        let settings = load_conf("quoted", "[proxy]\nsig_key=\"changeme\"\n").unwrap();
        assert_eq!(settings.get("proxy/sig_key"), Some("changeme"));
    }

    #[test]
    fn line_without_equals_errors() {
        let r = load_conf("no_equals", "[proxy]\nworkers 4\nsig_iss=pushpin\n");
        assert!(matches!(
            r,
            Err(SettingsError::MissingEquals(s, line)) if s == "proxy" && line == "workers 4"
        ));
    }

    #[test]
    fn unclosed_section_errors() {
        let r = load_conf("unclosed_section", "[proxy\nworkers=4\n");
        assert!(r.is_err());
    }

    #[test]
    fn key_outside_section_errors() {
        let r = load_conf("no_section", "workers=4\n[proxy]\nsig_iss=pushpin\n");
        assert!(matches!(r, Err(SettingsError::KeyOutsideSection(k)) if k == "workers"));
    }

    #[test]
    fn empty_file() {
        let settings = load_conf("empty_file", "").unwrap();
        assert_eq!(settings.get("proxy/workers"), None);
    }

    #[test]
    fn missing_file_errors() {
        let path = test_dir().join("settings").join("does-not-exist.conf");
        assert!(Settings::load(&path).is_err());
    }

    #[test]
    fn crlf_line_endings() {
        let settings = load_conf("crlf", "[proxy]\r\nworkers=4\r\nsig_iss=pushpin\r\n").unwrap();
        assert_eq!(settings.get("proxy/workers"), Some("4"));
        assert_eq!(settings.get("proxy/sig_iss"), Some("pushpin"));
    }

    // Include

    #[test]
    fn example_config_loads_internal_conf() {
        let settings = Settings::load(Path::new("examples/config/pushpin.conf")).unwrap();
        assert_eq!(
            settings.get("proxy/connmgr_in_specs"),
            Some("ipc://{rundir}/{ipc_prefix}connmgr-out")
        );
    }

    #[test]
    fn include_internal_conf_shorthand() {
        let settings = load_conf("include_shorthand", "[global]\ninclude=internal.conf\n").unwrap();
        assert_eq!(
            settings.get("proxy/connmgr_in_specs"),
            Some("ipc://{rundir}/{ipc_prefix}connmgr-out")
        );
    }

    #[test]
    fn include_relative_to_config_dir() {
        write_conf("include_relative", "extra.conf", "[proxy]\nsig_iss=extra\n");
        let settings = load_conf("include_relative", "[global]\ninclude=extra.conf\n").unwrap();
        assert_eq!(settings.get("proxy/sig_iss"), Some("extra"));
    }

    #[test]
    fn include_absolute_path() {
        let extra = write_conf("include_absolute", "extra.conf", "[proxy]\nsig_iss=extra\n");
        let main = format!("[global]\ninclude={}\n", extra.display());
        let settings = load_conf("include_absolute", &main).unwrap();
        assert_eq!(settings.get("proxy/sig_iss"), Some("extra"));
    }

    #[test]
    fn main_wins_over_include() {
        write_conf(
            "include_override",
            "extra.conf",
            "[proxy]\nsig_iss=extra\nworkers=2\n",
        );
        let settings = load_conf(
            "include_override",
            "[global]\ninclude=extra.conf\n[proxy]\nsig_iss=main\n",
        )
        .unwrap();
        assert_eq!(settings.get("proxy/sig_iss"), Some("main"));
        assert_eq!(settings.get("proxy/workers"), Some("2"));
    }

    #[test]
    fn missing_include_errors() {
        let r = load_conf("include_missing", "[global]\ninclude=nope.conf\n");
        assert!(r.is_err());
    }

    #[test]
    fn empty_include() {
        let settings =
            load_conf("include_empty", "[global]\ninclude=\n[proxy]\nworkers=2\n").unwrap();
        assert_eq!(settings.get("proxy/workers"), Some("2"));
    }

    #[test]
    fn nested_include_not_followed() {
        write_conf("include_nested", "second.conf", "[proxy]\nsig_iss=second\n");
        write_conf(
            "include_nested",
            "first.conf",
            "[global]\ninclude=second.conf\n[proxy]\nworkers=2\n",
        );
        let settings = load_conf("include_nested", "[global]\ninclude=first.conf\n").unwrap();
        assert_eq!(settings.get("proxy/workers"), Some("2"));
        assert_eq!(settings.get("proxy/sig_iss"), None);
    }

    // Variables

    #[test]
    fn libdir_from_config() {
        let settings = load_conf("libdir_config", "[global]\nlibdir=/usr/lib/pushpin\n").unwrap();
        assert_eq!(settings.libdir, "/usr/lib/pushpin");
    }

    #[test]
    fn libdir_unset_in_tree() {
        let settings = load_conf("libdir_unset", "[global]\nrundir=run\n").unwrap();
        assert_eq!(settings.libdir, in_tree_libdir());
    }

    #[test]
    fn libdir_empty_in_tree() {
        let settings = load_conf("libdir_empty", "[global]\nlibdir=\n").unwrap();
        assert_eq!(settings.libdir, in_tree_libdir());
    }

    #[test]
    fn rundir_from_global() {
        let settings = load_conf("rundir_global", "[global]\nrundir=/var/run/pushpin\n").unwrap();
        assert_eq!(settings.rundir, "/var/run/pushpin");
    }

    #[test]
    fn rundir_from_runner() {
        let settings = load_conf("rundir_runner", "[runner]\nrundir=/var/run/pushpin\n").unwrap();
        assert_eq!(settings.rundir, "/var/run/pushpin");
    }

    #[test]
    fn rundir_global_wins_over_runner() {
        let settings = load_conf(
            "rundir_both",
            "[global]\nrundir=/var/run/global\n[runner]\nrundir=/var/run/runner\n",
        )
        .unwrap();
        assert_eq!(settings.rundir, "/var/run/global");
    }

    #[test]
    fn rundir_unset() {
        let settings = load_conf("rundir_unset", "[global]\nipc_prefix=pushpin-\n").unwrap();
        assert_eq!(settings.rundir, "");
    }

    #[test]
    fn ipc_prefix_default() {
        let settings = load_conf("ipc_prefix_default", "[global]\nrundir=run\n").unwrap();
        assert_eq!(settings.ipc_prefix, "pushpin-");
    }

    #[test]
    fn ipc_prefix_set() {
        let settings = load_conf("ipc_prefix_set", "[global]\nipc_prefix=pushpin2-\n").unwrap();
        assert_eq!(settings.ipc_prefix, "pushpin2-");
    }

    #[test]
    fn ipc_prefix_empty() {
        let settings = load_conf("ipc_prefix_empty", "[global]\nipc_prefix=\n").unwrap();
        assert_eq!(settings.ipc_prefix, "");
    }

    #[test]
    fn port_offset_default() {
        let settings = load_conf("port_offset_default", "[global]\nrundir=run\n").unwrap();
        assert_eq!(settings.port_offset, 0);
    }

    #[test]
    fn port_offset_set() {
        let settings = load_conf("port_offset_set", "[global]\nport_offset=10\n").unwrap();
        assert_eq!(settings.port_offset, 10);
    }

    #[test]
    fn port_offset_empty() {
        let settings = load_conf("port_offset_empty", "[global]\nport_offset=\n").unwrap();
        assert_eq!(settings.port_offset, 0);
    }

    #[test]
    fn port_offset_not_a_number_errors() {
        let r = load_conf("port_offset_nan", "[global]\nport_offset=one\n");
        assert!(matches!(
            r,
            Err(SettingsError::InvalidValue { key, value, .. })
                if key == "global/port_offset" && value == "one"
        ));
    }

    // Substitution

    fn example_settings() -> Settings {
        Settings::load(Path::new("examples/config/pushpin.conf")).unwrap()
    }

    #[test]
    fn substitute_two_vars() {
        let settings = example_settings();
        assert_eq!(
            settings
                .resolve_vars("ipc://{rundir}/{ipc_prefix}connmgr-out")
                .unwrap(),
            "ipc://run/pushpin-connmgr-out"
        );
    }

    #[test]
    fn substitute_var_twice() {
        let settings = example_settings();
        assert_eq!(
            settings.resolve_vars("{rundir}/a,{rundir}/b").unwrap(),
            "run/a,run/b"
        );
    }

    #[test]
    fn substitute_nothing() {
        let settings = example_settings();
        assert_eq!(settings.resolve_vars("pushpin").unwrap(), "pushpin");
    }

    // Port shift

    fn offset_settings(port_offset: i32) -> Settings {
        let mut settings = example_settings();
        settings.port_offset = port_offset;
        settings
    }

    #[test]
    fn port_offset_zero_unchanged() {
        let settings = offset_settings(0);
        assert_eq!(
            settings.resolve_vars("tcp://127.0.0.1:5560").unwrap(),
            "tcp://127.0.0.1:5560"
        );
    }

    #[test]
    fn port_shift_wildcard() {
        let settings = offset_settings(1);
        assert_eq!(
            settings.resolve_vars("tcp://*:5560").unwrap(),
            "tcp://*:5561"
        );
    }

    #[test]
    fn port_shift_hostname() {
        let settings = offset_settings(1);
        assert_eq!(
            settings.resolve_vars("tcp://localhost:5560").unwrap(),
            "tcp://localhost:5561"
        );
    }

    #[test]
    fn port_shift_with_path() {
        let settings = offset_settings(1);
        assert_eq!(
            settings
                .resolve_vars("tcp://host:5560/abcdefghijklmnop")
                .unwrap(),
            "tcp://host:5561/abcdefghijklmnop"
        );
    }

    #[test]
    fn port_shift_ipv6() {
        let settings = offset_settings(1);
        assert_eq!(
            settings.resolve_vars("tcp://[::1]:5560").unwrap(),
            "tcp://[::1]:5561"
        );
    }

    #[test]
    fn tcp_missing_port_errors() {
        let settings = offset_settings(1);
        let r = settings.resolve_vars("tcp://127.0.0.1");
        assert!(matches!(r, Err(SettingsError::InvalidTcpAddress(v)) if v == "tcp://127.0.0.1"));
    }

    #[test]
    fn tcp_empty_port_errors() {
        let settings = offset_settings(1);
        let r = settings.resolve_vars("tcp://127.0.0.1:");
        assert!(matches!(r, Err(SettingsError::InvalidTcpAddress(_))));
    }

    #[test]
    fn tcp_unclosed_ipv6_errors() {
        let settings = offset_settings(1);
        let r = settings.resolve_vars("tcp://[::1:5560");
        assert!(matches!(r, Err(SettingsError::InvalidTcpAddress(_))));
    }

    #[test]
    fn ipc_not_shifted() {
        let settings = offset_settings(1);
        assert_eq!(
            settings.resolve_vars("ipc://run/pushpin-5560").unwrap(),
            "ipc://run/pushpin-5560"
        );
    }

    #[test]
    fn port_offset_from_config_file() {
        let settings = load_conf(
            "port_offset_file",
            "[global]\nport_offset=1\n[handler]\npush_in_spec=tcp://127.0.0.1:5560\n",
        )
        .unwrap();
        let spec = settings.get("handler/push_in_spec").unwrap();
        assert_eq!(settings.resolve_vars(spec).unwrap(), "tcp://127.0.0.1:5561");
    }

    // Typed values

    fn is_invalid(r: Result<impl Debug, SettingsError>, k: &str, v: &str) -> bool {
        matches!(r, Err(SettingsError::InvalidValue { key, value, .. }) if key == k && value == v)
    }

    #[test]
    fn string_substitutes_vars() {
        let settings = load_conf(
            "string_vars",
            "[global]\nrundir=run\n[handler]\nstats_spec=ipc://{rundir}/{ipc_prefix}stats\n",
        )
        .unwrap();
        assert_eq!(
            settings.string("handler/stats_spec").unwrap(),
            Some("ipc://run/pushpin-stats".to_string())
        );
    }

    #[test]
    fn string_missing_or_empty() {
        let settings = load_conf("string_empty", "[proxy]\nupstream_key=\n").unwrap();
        assert_eq!(
            settings.string("proxy/upstream_key").unwrap(),
            Some(String::new())
        );
        assert_eq!(settings.string("proxy/sig_key").unwrap(), None);
    }

    #[test]
    fn string_given_list_errors() {
        let settings = load_conf("string_list", "[proxy]\nsig_iss=a,b\n").unwrap();
        assert!(is_invalid(
            settings.string("proxy/sig_iss"),
            "proxy/sig_iss",
            "a,b"
        ));
    }

    #[test]
    fn list_splits_and_trims() {
        let settings = load_conf(
            "list_split",
            "[runner]\nservices=connmgr, proxy ,,handler\n",
        )
        .unwrap();
        assert_eq!(
            settings.list("runner/services").unwrap(),
            vec!["connmgr", "proxy", "handler"]
        );
    }

    #[test]
    fn list_substitutes_each_item() {
        let settings = example_settings();
        assert_eq!(
            settings.list("proxy/m2a_in_specs").unwrap(),
            vec![
                "ipc://run/pushpin-m2zhttp-out",
                "ipc://run/pushpin-m2zws-out"
            ]
        );
    }

    #[test]
    fn list_missing_or_empty() {
        let settings = load_conf("list_empty", "[proxy]\nx_forwarded_for=\n").unwrap();
        assert!(settings.list("proxy/x_forwarded_for").unwrap().is_empty());
        assert!(settings
            .list("proxy/x_forwarded_for_trusted")
            .unwrap()
            .is_empty());
    }

    #[test]
    fn number_parses() {
        let settings = load_conf("number", "[proxy]\nworkers=4\n").unwrap();
        assert_eq!(settings.number::<u32>("proxy/workers").unwrap(), Some(4));
        assert_eq!(
            settings.number::<u32>("proxy/max_open_requests").unwrap(),
            None
        );
    }

    #[test]
    fn number_invalid_errors() {
        let settings =
            load_conf("number_bad", "[proxy]\nworkers=4x\nmax_open_requests=1,2\n").unwrap();
        assert!(is_invalid(
            settings.number::<u32>("proxy/workers"),
            "proxy/workers",
            "4x"
        ));
        assert!(is_invalid(
            settings.number::<u32>("proxy/max_open_requests"),
            "proxy/max_open_requests",
            "1,2"
        ));
    }

    #[test]
    fn number_negative_for_unsigned_errors() {
        let settings = load_conf("number_negative", "[handler]\nmessage_wait=-5\n").unwrap();
        assert!(is_invalid(
            settings.number::<u32>("handler/message_wait"),
            "handler/message_wait",
            "-5"
        ));
    }

    #[test]
    fn bool_parses() {
        let settings = load_conf(
            "bool",
            "[proxy]\ndebug=true\nlog_from=false\nauto_cross_origin=TRUE\n",
        )
        .unwrap();
        assert_eq!(settings.bool("proxy/debug").unwrap(), Some(true));
        assert_eq!(settings.bool("proxy/log_from").unwrap(), Some(false));
        assert_eq!(
            settings.bool("proxy/auto_cross_origin").unwrap(),
            Some(true)
        );
        assert_eq!(settings.bool("proxy/log_user_agent").unwrap(), None);
    }

    #[test]
    fn bool_invalid_errors() {
        let settings = load_conf("bool_bad", "[proxy]\ndebug=yes\nlog_from=2\n").unwrap();
        assert!(is_invalid(
            settings.bool("proxy/debug"),
            "proxy/debug",
            "yes"
        ));
        assert!(is_invalid(
            settings.bool("proxy/log_from"),
            "proxy/log_from",
            "2"
        ));
    }

    #[test]
    fn bool_one_zero_and_empty() {
        let settings = load_conf(
            "bool_one_zero",
            "[proxy]\ndebug=1\nlog_from=0\nauto_cross_origin=\n",
        )
        .unwrap();
        assert_eq!(settings.bool("proxy/debug").unwrap(), Some(true));
        assert_eq!(settings.bool("proxy/log_from").unwrap(), Some(false));
        assert_eq!(
            settings.bool("proxy/auto_cross_origin").unwrap(),
            Some(false)
        );
    }

    #[test]
    fn minus_one_is_unset() {
        let settings = load_conf(
            "minus_one",
            "[handler]\nmessage_rate=-1\nipc_file_mode=-1\npush_in_http_port=-1\n",
        )
        .unwrap();
        assert_eq!(
            settings.number::<u32>("handler/message_rate").unwrap(),
            None
        );
        assert_eq!(settings.octal("handler/ipc_file_mode").unwrap(), None);
        assert_eq!(
            settings.adjust_port("handler/push_in_http_port").unwrap(),
            None
        );
    }

    #[test]
    fn empty_number_and_port_are_zero() {
        let settings = load_conf(
            "empty_zero",
            "[global]\nport_offset=1\n[handler]\nmessage_wait=\npush_in_http_port=\n",
        )
        .unwrap();
        assert_eq!(
            settings.number::<u32>("handler/message_wait").unwrap(),
            Some(0)
        );
        assert_eq!(
            settings.adjust_port("handler/push_in_http_port").unwrap(),
            Some(0)
        );
    }

    #[test]
    fn octal_parses() {
        let settings = load_conf("octal", "[handler]\nipc_file_mode=777\n").unwrap();
        assert_eq!(
            settings.octal("handler/ipc_file_mode").unwrap(),
            Some(0o777)
        );
    }

    #[test]
    fn octal_empty_errors() {
        let settings = load_conf("octal_empty", "[handler]\nipc_file_mode=\n").unwrap();
        assert!(is_invalid(
            settings.octal("handler/ipc_file_mode"),
            "handler/ipc_file_mode",
            ""
        ));
    }

    #[test]
    fn octal_invalid_errors() {
        let settings = load_conf(
            "octal_bad",
            "[handler]\nipc_file_mode=abc\n[proxy]\nipc_file_mode=789\n",
        )
        .unwrap();
        assert!(is_invalid(
            settings.octal("handler/ipc_file_mode"),
            "handler/ipc_file_mode",
            "abc"
        ));
        assert!(is_invalid(
            settings.octal("proxy/ipc_file_mode"),
            "proxy/ipc_file_mode",
            "789"
        ));
    }

    #[test]
    fn port_shifted_by_offset() {
        let settings = load_conf(
            "port_shift",
            "[global]\nport_offset=1\n[handler]\npush_in_http_port=5561\n",
        )
        .unwrap();
        assert_eq!(
            settings.adjust_port("handler/push_in_http_port").unwrap(),
            Some(5562)
        );
    }

    #[test]
    fn port_zero_not_shifted() {
        let settings = load_conf(
            "port_zero",
            "[global]\nport_offset=1\n[handler]\npush_in_http_port=0\n",
        )
        .unwrap();
        assert_eq!(
            settings.adjust_port("handler/push_in_http_port").unwrap(),
            Some(0)
        );
    }

    #[test]
    fn port_invalid_errors() {
        let settings = load_conf(
            "port_bad",
            "[global]\nport_offset=1\n[handler]\npush_in_http_port=70000\n[runner]\nhttp_port=65535\n",
        )
        .unwrap();
        assert!(is_invalid(
            settings.adjust_port("handler/push_in_http_port"),
            "handler/push_in_http_port",
            "70000"
        ));
        assert!(is_invalid(
            settings.adjust_port("runner/http_port"),
            "runner/http_port",
            "65535"
        ));
    }

    #[test]
    fn overrides_apply() {
        let mut settings = load_conf(
            "overrides",
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
            settings.adjust_port("handler/push_in_http_port").unwrap(),
            Some(5571)
        );
    }
}
