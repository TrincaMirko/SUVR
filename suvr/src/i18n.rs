//! Translations loaded from the existing `src/player/lang/` files (key=value,
//! `#` comments). English is the default; missing keys fall back to the key.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

pub struct I18n {
    pub lang: String,
    dir: PathBuf,
    map: HashMap<String, String>,
}

fn parse(text: &str) -> HashMap<String, String> {
    let mut map = HashMap::new();
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if let Some((k, v)) = line.split_once('=') {
            map.insert(k.trim().to_string(), v.trim().to_string());
        }
    }
    map
}

impl I18n {
    /// `dir` is the lang directory (e.g. the installed `lang/`). `lang` is
    /// "eng" | "deu" | "ita"; unknown falls back to "eng".
    pub fn load(dir: &Path, lang: &str) -> Self {
        let file = dir.join(lang);
        let text = std::fs::read_to_string(&file)
            .or_else(|_| std::fs::read_to_string(dir.join("eng")))
            .unwrap_or_default();
        I18n {
            lang: lang.to_string(),
            dir: dir.to_path_buf(),
            map: parse(&text),
        }
    }

    pub fn t<'a>(&'a self, key: &'a str) -> &'a str {
        self.map.get(key).map(String::as_str).unwrap_or(key)
    }

    /// Switch the active language in place (reloads from the same dir).
    pub fn set_lang(&mut self, lang: &str) {
        *self = I18n::load(&self.dir.clone(), lang);
    }

    /// Available languages as (code, display name) from each file's
    /// `language.name`, sorted by name. The file name is the code (e.g. "fra").
    pub fn list(dir: &Path) -> Vec<(String, String)> {
        let mut out = Vec::new();
        let Ok(entries) = std::fs::read_dir(dir) else { return out };
        for e in entries.flatten() {
            if !e.file_type().map(|t| t.is_file()).unwrap_or(false) {
                continue;
            }
            let code = e.file_name().to_string_lossy().into_owned();
            let text = std::fs::read_to_string(e.path()).unwrap_or_default();
            let name = parse(&text).get("language.name").cloned().unwrap_or_else(|| code.clone());
            out.push((code, name));
        }
        out.sort_by(|a, b| a.1.cmp(&b.1));
        out
    }
}

#[cfg(test)]
mod tests {
    use super::parse;

    #[test]
    fn parses_pairs_and_skips_comments() {
        let m = parse("# c\n\nrail.local = File locali \nx=y=z\n");
        assert_eq!(m.get("rail.local").unwrap(), "File locali");
        assert_eq!(m.get("x").unwrap(), "y=z"); // only first '=' splits
        assert_eq!(m.len(), 2);
    }
}
