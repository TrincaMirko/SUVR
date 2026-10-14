//! Global preferences in $XDG_CONFIG_HOME/stereonaut/settings.json (same file
//! the C side used). Untyped JSON map so new keys never need a schema change.

use serde_json::{Map, Value};
use std::path::PathBuf;

pub struct Settings {
    path: PathBuf,
    map: Map<String, Value>,
}

fn config_path() -> PathBuf {
    let base = std::env::var("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| {
            PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| ".".into())).join(".config")
        });
    base.join("stereonaut").join("settings.json")
}

impl Settings {
    pub fn load() -> Self {
        let path = config_path();
        let map = std::fs::read_to_string(&path)
            .ok()
            .and_then(|s| serde_json::from_str::<Value>(&s).ok())
            .and_then(|v| v.as_object().cloned())
            .unwrap_or_default();
        Settings { path, map }
    }

    pub fn get_str<'a>(&'a self, key: &str, fallback: &'a str) -> &'a str {
        self.map.get(key).and_then(|v| v.as_str()).unwrap_or(fallback)
    }

    pub fn set_str(&mut self, key: &str, value: &str) {
        self.map.insert(key.into(), Value::String(value.into()));
    }

    pub fn save(&self) {
        if let Some(dir) = self.path.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        if let Ok(s) = serde_json::to_string_pretty(&self.map) {
            let _ = std::fs::write(&self.path, s);
        }
    }
}
