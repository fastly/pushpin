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
}

pub struct Settings {
    values: HashMap<String, String>,
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

        Ok(Self { values })
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.values.get(&key.to_lowercase()).map(String::as_str)
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
}
