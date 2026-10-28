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
use std::env;
use std::path::Path;
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

    #[error("port_offset must be a number: {0}")]
    InvalidPortOffset(String),
}

pub struct Settings {
    values: HashMap<String, String>,
    libdir: String,
    rundir: String,
    ipc_prefix: String,
    port_offset: i32,
}

impl Settings {
    pub fn load(path: &Path) -> Result<Self, SettingsError> {
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
                // rust-ini joins a line without '=' onto the next line's key
                if let Some((line, _)) = key.split_once('\n') {
                    return Err(SettingsError::MissingEquals(section, line.to_string()));
                }

                let key = format!("{section}/{}", key.to_lowercase());

                if values.insert(key.clone(), value.to_string()).is_some() {
                    return Err(SettingsError::DuplicateKey(key));
                }
            }
        }

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
                env!("LIB_DIR").to_string()
            }
        });

        let rundir = get("global/rundir")
            .or_else(|| get("runner/rundir"))
            .unwrap_or_default();

        let ipc_prefix = values
            .get("global/ipc_prefix")
            .cloned()
            .unwrap_or_else(|| "pushpin-".to_string());

        let port_offset = match get("global/port_offset") {
            Some(v) => v.parse().map_err(|_| SettingsError::InvalidPortOffset(v))?,
            None => 0,
        };

        Ok(Self {
            values,
            libdir,
            rundir,
            ipc_prefix,
            port_offset,
        })
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.values.get(&key.to_lowercase()).map(String::as_str)
    }

    fn resolve_vars(&self, s: &str) -> String {
        let mut out = s
            .replace("{libdir}", &self.libdir)
            .replace("{rundir}", &self.rundir)
            .replace("{ipc_prefix}", &self.ipc_prefix);

        let mut at = 0;
        while let Some(i) = out[at..].find("tcp://") {
            let host = at + i + 6;

            // Skip past an IPv6 address, since it contains colons
            let host_end = match out[host..].strip_prefix('[') {
                Some(rest) => match rest.find(']') {
                    Some(n) => host + 1 + n,
                    None => break,
                },
                None => host,
            };

            let Some(colon) = out[host_end..].find(':') else {
                break;
            };

            let start = host_end + colon + 1;
            let end = out[start..]
                .find(|c: char| !c.is_ascii_digit())
                .map_or(out.len(), |n| start + n);

            let Ok(port) = out[start..end].parse::<i32>() else {
                break;
            };

            let port = (port + self.port_offset).to_string();
            out.replace_range(start..end, &port);
            at = start + port.len();
        }

        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::test_dir;
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
            settings.resolve_vars(include),
            format!("{}/internal.conf", libdir.display())
        );

        let stats_spec = settings.get("handler/stats_spec").unwrap();
        assert_eq!(settings.resolve_vars(stats_spec), "ipc://run/pushpin-stats");

        settings.port_offset = 1;
        let push_in_spec = settings.get("handler/push_in_spec").unwrap();
        assert_eq!(settings.resolve_vars(push_in_spec), "tcp://127.0.0.1:5561");
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
        assert!(matches!(r, Err(SettingsError::InvalidPortOffset(v)) if v == "one"));
    }

    // Substitution

    fn example_settings() -> Settings {
        Settings::load(Path::new("examples/config/pushpin.conf")).unwrap()
    }

    #[test]
    fn substitute_two_vars() {
        let settings = example_settings();
        assert_eq!(
            settings.resolve_vars("ipc://{rundir}/{ipc_prefix}connmgr-out"),
            "ipc://run/pushpin-connmgr-out"
        );
    }

    #[test]
    fn substitute_var_twice() {
        let settings = example_settings();
        assert_eq!(
            settings.resolve_vars("{rundir}/a,{rundir}/b"),
            "run/a,run/b"
        );
    }

    #[test]
    fn substitute_nothing() {
        let settings = example_settings();
        assert_eq!(settings.resolve_vars("pushpin"), "pushpin");
    }

    #[test]
    fn substitute_mistyped_var() {
        let settings = example_settings();
        let resolved = settings.resolve_vars("ipc://{run_dir}/{ipc_prefix}stats");
        assert!(!resolved.contains("{run_dir}"), "unresolved: {}", resolved);
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
            settings.resolve_vars("tcp://127.0.0.1:5560"),
            "tcp://127.0.0.1:5560"
        );
    }

    #[test]
    fn port_shift_wildcard() {
        let settings = offset_settings(1);
        assert_eq!(settings.resolve_vars("tcp://*:5560"), "tcp://*:5561");
    }

    #[test]
    fn port_shift_hostname() {
        let settings = offset_settings(1);
        assert_eq!(
            settings.resolve_vars("tcp://localhost:5560"),
            "tcp://localhost:5561"
        );
    }

    #[test]
    fn port_shift_with_path() {
        let settings = offset_settings(1);
        assert_eq!(
            settings.resolve_vars("tcp://host:5560/abcdefghijklmnop"),
            "tcp://host:5561/abcdefghijklmnop"
        );
    }

    #[test]
    fn port_shift_ipv6() {
        let settings = offset_settings(1);
        assert_eq!(
            settings.resolve_vars("tcp://[::1]:5560"),
            "tcp://[::1]:5561"
        );
    }

    #[test]
    fn ipc_not_shifted() {
        let settings = offset_settings(1);
        assert_eq!(
            settings.resolve_vars("ipc://run/pushpin-5560"),
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
        assert_eq!(settings.resolve_vars(spec), "tcp://127.0.0.1:5561");
    }
}
