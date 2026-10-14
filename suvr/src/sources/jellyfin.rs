//! Jellyfin source. Port of C `jellyfin.c`: Quick Connect login, library
//! listings, PlaybackInfo -> Direct Play / HLS URL. Paths are
//! `jf:item:<id>:<folder>`; `resolve` turns the selected item into a stream URL.
//!
//! ponytail: left as a typed skeleton. Port the HTTP calls from jellyfin.c
//! onto `ureq` when wiring the Jellyfin page; the UI already routes to it.

use crate::model::Listing;
use crate::sources::{Resolved, Source};

pub struct JellyfinSource {
    pub base_url: String,
    pub token: Option<String>,
}

impl Source for JellyfinSource {
    fn id(&self) -> &str {
        "jellyfin"
    }
    fn label_key(&self) -> &str {
        "rail.jellyfin"
    }
    fn root(&self) -> String {
        "jf:root".into()
    }
    fn list(&mut self, path: &str) -> Listing {
        Listing {
            path: path.to_string(),
            error: Some("jellyfin: not wired yet".into()),
            ..Default::default()
        }
    }
    fn resolve(&mut self, _path: &str) -> std::io::Result<Resolved> {
        unimplemented!("port PlaybackInfo from jellyfin.c")
    }
}
