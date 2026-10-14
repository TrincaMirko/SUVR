//! Shared data model for every source. Mirrors the C `file_browser_entry`
//! so behaviour (sorting, resume, codec badges) ports 1:1, without the C.

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum EntryKind {
    Parent,
    Directory,
    Media,
}

#[derive(Clone, Debug)]
pub struct Entry {
    pub kind: EntryKind,
    pub name: String,
    /// Opaque locator the player understands: a local path, a WebDAV URL, or
    /// `jf:item:<id>:<folder>` for Jellyfin.
    pub path: String,
    /// Thumbnail hint: `ITEM_ID|IMAGE` for Jellyfin, a URL for WebDAV, or a
    /// local file path. `None` if there is no picture.
    pub image: Option<String>,
    pub duration_us: i64,
    pub year: u32,
    pub codec: String,
    pub width: u32,
    pub height: u32,
    /// Jellyfin resume position (0 = none) and watched state.
    pub resume_us: i64,
    pub played_percentage: f32,
    pub played: bool,
    pub modified: i64,
    pub size: i64,
}

impl Entry {
    pub fn dir(name: impl Into<String>, path: impl Into<String>) -> Self {
        Self::bare(EntryKind::Directory, name, path)
    }

    pub fn media(name: impl Into<String>, path: impl Into<String>) -> Self {
        Self::bare(EntryKind::Media, name, path)
    }

    fn bare(kind: EntryKind, name: impl Into<String>, path: impl Into<String>) -> Self {
        Entry {
            kind,
            name: name.into(),
            path: path.into(),
            image: None,
            duration_us: 0,
            year: 0,
            codec: String::new(),
            width: 0,
            height: 0,
            resume_us: 0,
            played_percentage: 0.0,
            played: false,
            modified: 0,
            size: 0,
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct Listing {
    pub path: String,
    pub entries: Vec<Entry>,
    pub error: Option<String>,
}
