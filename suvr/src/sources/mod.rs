//! A media source is anything that can list folders and hand the player a
//! locator. Add an integration = implement this trait and register it; the UI
//! stays untouched. This is the seam the whole refactor exists for.

use crate::model::Listing;

pub mod local;
pub mod jellyfin;
pub mod webdav;

/// What the player binary needs to open a title: a locator (local path or
/// http(s) URL) and, for authenticated sources, the Basic-auth header the C
/// session forwards to libavformat via STEREONAUT_HTTP_HEADERS.
pub struct Resolved {
    pub locator: String,
    pub http_header: Option<String>,
}

pub trait Source: Send {
    /// Stable id used in settings and the side rail (e.g. "local").
    fn id(&self) -> &str;
    /// Translation key for the rail label (e.g. "rail.local").
    fn label_key(&self) -> &str;
    /// Location shown first when the user opens this source.
    fn root(&self) -> String;
    /// List a location. Blocking; the UI runs it off the paint thread.
    fn list(&mut self, path: &str) -> Listing;
    /// Turn an entry path into something the player binary can open. Most
    /// sources pass the path through; Jellyfin/WebDAV resolve a stream URL.
    fn resolve(&mut self, path: &str) -> std::io::Result<Resolved> {
        Ok(Resolved { locator: path.to_string(), http_header: None })
    }
    /// Downcast hook so the UI can reach the WebDAV server-management page.
    /// Only WebDAV overrides this.
    fn as_webdav_mut(&mut self) -> Option<&mut webdav::WebdavSource> {
        None
    }
}

/// Registry of enabled sources, in rail order.
pub struct Sources(pub Vec<Box<dyn Source>>);

impl Sources {
    pub fn new() -> Self {
        // Local is always on; WebDAV appears in the rail and lists saved servers
        // (empty until the user adds one). Jellyfin is wired later.
        Sources(vec![
            Box::new(local::LocalSource::new()),
            Box::new(webdav::WebdavSource::load()),
        ])
    }

    pub fn get(&mut self, id: &str) -> Option<&mut Box<dyn Source>> {
        self.0.iter_mut().find(|s| s.id() == id)
    }
}
