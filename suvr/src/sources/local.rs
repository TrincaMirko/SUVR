//! Local filesystem source. Folders first, then `.mkv`/`.mp4`, name-sorted.
//! Ports the behaviour of C `file-browser.c` for the local case.

use crate::model::{Entry, Listing};
use crate::sources::Source;
use std::fs;

pub struct LocalSource {
    home: String,
}

impl LocalSource {
    pub fn new() -> Self {
        LocalSource {
            home: std::env::var("HOME").unwrap_or_else(|_| "/".into()),
        }
    }
}

fn is_media(name: &str) -> bool {
    let n = name.to_ascii_lowercase();
    n.ends_with(".mkv") || n.ends_with(".mp4")
}

impl Source for LocalSource {
    fn id(&self) -> &str {
        "local"
    }
    fn label_key(&self) -> &str {
        "rail.local"
    }
    fn root(&self) -> String {
        self.home.clone()
    }

    fn list(&mut self, path: &str) -> Listing {
        let mut listing = Listing {
            path: path.to_string(),
            ..Default::default()
        };
        let read = match fs::read_dir(path) {
            Ok(r) => r,
            Err(e) => {
                listing.error = Some(e.to_string());
                return listing;
            }
        };

        let mut dirs = Vec::new();
        let mut media = Vec::new();
        for ent in read.flatten() {
            let name = ent.file_name().to_string_lossy().into_owned();
            if name.starts_with('.') {
                continue;
            }
            let full = ent.path().to_string_lossy().into_owned();
            let meta = ent.metadata().ok();
            let is_dir = meta.as_ref().map(|m| m.is_dir()).unwrap_or(false);
            if is_dir {
                dirs.push(Entry::dir(name, full));
            } else if is_media(&name) {
                let mut e = Entry::media(name, full);
                if let Some(m) = &meta {
                    e.size = m.len() as i64;
                    e.modified = m
                        .modified()
                        .ok()
                        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                        .map(|d| d.as_secs() as i64)
                        .unwrap_or(0);
                }
                media.push(e);
            }
        }
        dirs.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
        media.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));

        // Parent entry unless we are at the filesystem root.
        if let Some(parent) = std::path::Path::new(path).parent() {
            listing.entries.push(Entry {
                kind: crate::model::EntryKind::Parent,
                name: "..".into(),
                path: parent.to_string_lossy().into_owned(),
                ..Entry::dir("..", "")
            });
        }
        listing.entries.extend(dirs);
        listing.entries.extend(media);
        listing
    }
}
