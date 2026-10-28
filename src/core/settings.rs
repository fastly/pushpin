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
use std::collections::HashMap;
use std::env;
use std::path::Path;

pub struct Settings {
    values: HashMap<String, String>,
    libdir: String,
    rundir: String,
    ipc_prefix: String,
    port_offset: i32,
}

impl Settings {
    pub fn load(path: &Path) -> Result<Self, ini::Error> {
        let mut values = HashMap::new();

        for (section, props) in Ini::load_from_file(path)?.iter() {
            for (key, value) in props.iter() {
                let key = match section {
                    Some(section) => format!("{section}/{key}"),
                    None => key.to_string(),
                };

                values.insert(key.to_lowercase(), value.to_string());
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

        let port_offset = values
            .get("global/port_offset")
            .and_then(|v| v.parse().ok())
            .unwrap_or(0);

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

            let Some(colon) = out[host..].find(':') else {
                break;
            };

            let start = host + colon + 1;
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
}
