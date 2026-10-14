//! WebDAV source, ported from C `webdav.c` + `file-browser.c` (webdav case).
//!
//! Locator scheme (owned by this source):
//!   webdav:                       -> list saved servers
//!   webdav:srv:<idx>:<relpath>    -> PROPFIND Depth:1 of a directory
//!   webdav:play:<idx>:<relpath>   -> resolve to an http(s) URL + auth header
//!
//! Servers live in ~/.config/stereonaut/webdav.json (same file as the C side).
//! Media play through the existing http(s) pipeline; Basic credentials ride in
//! STEREONAUT_HTTP_HEADERS (set by player.rs), never on the command line.

use crate::model::{Entry, EntryKind, Listing};
use crate::sources::{Resolved, Source};
use base64::Engine;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;

#[derive(Serialize, Deserialize, Clone, Default)]
pub struct Server {
    #[serde(default)]
    pub name: String,
    pub url: String,
    #[serde(default)]
    pub user: String,
    #[serde(default)]
    pub password: String,
}

pub struct WebdavSource {
    pub servers: Vec<Server>,
}

/// Transient state for the add/edit form (lives in App, not persisted).
#[derive(Default)]
pub struct ServerForm {
    pub open: bool,
    pub edit_idx: Option<usize>, // None = new server
    pub name: String,
    pub url: String,
    pub port: String,
    pub user: String,
    pub password: String,
    pub status: String, // test/save feedback
}

impl ServerForm {
    fn to_server(&self) -> Server {
        Server {
            name: self.name.trim().to_string(),
            url: normalize_url(&self.url, &self.port),
            user: self.user.trim().to_string(),
            password: self.password.clone(),
        }
    }
}

fn config_path() -> PathBuf {
    let base = std::env::var("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| {
            PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| ".".into())).join(".config")
        });
    base.join("stereonaut").join("webdav.json")
}

impl WebdavSource {
    pub fn load() -> Self {
        let servers = std::fs::read_to_string(config_path())
            .ok()
            .and_then(|s| serde_json::from_str::<Vec<Server>>(&s).ok())
            .unwrap_or_default();
        WebdavSource { servers }
    }

    pub fn save(&self) -> std::io::Result<()> {
        let path = config_path();
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        std::fs::write(&path, serde_json::to_string_pretty(&self.servers)?)
    }
}

// ---- URL helpers (ports of wd_origin / wd_base_path / auth) ----------

/// scheme://host[:port] — everything before the first path '/'.
fn origin(url: &str) -> &str {
    let after = url.find("://").map(|i| i + 3).unwrap_or(0);
    match url[after..].find('/') {
        Some(rel) => &url[..after + rel],
        None => url,
    }
}

/// Server-root-relative base path of the configured URL ("/" if none),
/// trailing slash kept.
fn base_path(url: &str) -> String {
    let after = url.find("://").map(|i| i + 3).unwrap_or(0);
    match url[after..].find('/') {
        Some(rel) => {
            let p = &url[after + rel..];
            if p.ends_with('/') {
                p.to_string()
            } else {
                format!("{p}/")
            }
        }
        None => "/".to_string(),
    }
}

/// Build a full URL from a user-typed host/url plus an optional separate port.
/// Adds `http://` when no scheme is present, and inserts `:port` into the host
/// only when the host doesn't already carry one.
fn normalize_url(url: &str, port: &str) -> String {
    let url = url.trim();
    let (scheme, rest) = match url.find("://") {
        Some(i) => (url[..i + 3].to_string(), url[i + 3..].to_string()),
        None => ("http://".to_string(), url.to_string()),
    };
    let port = port.trim();
    if port.is_empty() {
        return format!("{scheme}{rest}");
    }
    let (host, path) = match rest.find('/') {
        Some(i) => (rest[..i].to_string(), rest[i..].to_string()),
        None => (rest, String::new()),
    };
    let host = if host.contains(':') { host } else { format!("{host}:{port}") };
    format!("{scheme}{host}{path}")
}

fn auth_header(s: &Server) -> Option<String> {
    if s.user.is_empty() {
        return None;
    }
    let token = base64::engine::general_purpose::STANDARD
        .encode(format!("{}:{}", s.user, s.password));
    Some(format!("Authorization: Basic {token}"))
}

fn url_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() {
            if let (Some(h), Some(l)) = (hex(b[i + 1]), hex(b[i + 2])) {
                out.push(h * 16 + l);
                i += 3;
                continue;
            }
        }
        out.push(b[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn hex(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

fn is_media(name: &str) -> bool {
    let n = name.to_ascii_lowercase();
    n.ends_with(".mkv") || n.ends_with(".mp4")
}

fn last_segment(path: &str) -> String {
    let decoded = url_decode(path);
    let trimmed = decoded.trim_end_matches('/');
    trimmed.rsplit('/').next().unwrap_or(trimmed).to_string()
}

// ---- PROPFIND response parsing --------------------------------------

struct DavEntry {
    rel: String, // server-root-relative, still %-encoded
    is_dir: bool,
}

/// Scan a WebDAV multistatus body into (rel-path, is_dir) per <response>.
/// Port of the tag scan in webdav.c: one <href> per response; a response is a
/// collection if its block contains <...:collection>. Namespace-insensitive.
fn parse_propfind(body: &str) -> Vec<DavEntry> {
    let lower = body.to_ascii_lowercase();
    // Opening <...:href> tags only (skip closing </...:href>), value -> next '<'.
    let hrefs = find_href_values(body);
    let mut out = Vec::new();
    for i in 0..hrefs.len() {
        let (vs, ve) = hrefs[i];
        let block_end = hrefs.get(i + 1).map(|h| h.0).unwrap_or(body.len());
        let href = &body[vs..ve];
        // strip absolute origin, keep server-root-relative part
        let rel = match href.find("://") {
            Some(p) => {
                let host = &href[p + 3..];
                match host.find('/') {
                    Some(s) => &host[s..],
                    None => "/",
                }
            }
            None => href,
        };
        let is_dir = lower[ve..block_end].contains("collection");
        out.push(DavEntry {
            rel: rel.to_string(),
            is_dir,
        });
    }
    out
}

/// (value_start, value_end) for each opening `<...:href>` tag. Closing tags and
/// other elements are skipped; the value runs to the next '<'.
fn find_href_values(body: &str) -> Vec<(usize, usize)> {
    let bytes = body.as_bytes();
    let mut res = Vec::new();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] != b'<' {
            i += 1;
            continue;
        }
        let after = i + 1;
        if after >= bytes.len() || bytes[after] == b'/' {
            i = after;
            continue;
        }
        let mut end = after;
        while end < bytes.len() && !matches!(bytes[end], b'>' | b' ' | b'/') {
            end += 1;
        }
        let tag = &body[after..end];
        let local = tag.rsplit(':').next().unwrap_or(tag);
        if local.eq_ignore_ascii_case("href") {
            if let Some(gt) = body[end..].find('>') {
                let val_start = end + gt + 1;
                if let Some(lt) = body[val_start..].find('<') {
                    let val_end = val_start + lt;
                    res.push((val_start, val_end));
                    i = val_end;
                    continue;
                }
            }
        }
        i = after;
    }
    res
}

impl WebdavSource {
    fn list_servers(&self) -> Listing {
        let mut listing = Listing {
            path: "webdav:".into(),
            ..Default::default()
        };
        for (i, s) in self.servers.iter().enumerate() {
            let name = if s.name.is_empty() { &s.url } else { &s.name };
            let base = base_path(&s.url);
            listing
                .entries
                .push(Entry::dir(name.clone(), format!("webdav:srv:{i}:{base}")));
        }
        if self.servers.is_empty() {
            listing.error = Some("no WebDAV servers configured (webdav.json)".into());
        }
        listing
    }

    fn list_dir(&self, idx: usize, dir: &str) -> Listing {
        let mut listing = Listing {
            path: format!("webdav:srv:{idx}:{dir}"),
            ..Default::default()
        };
        let server = match self.servers.get(idx) {
            Some(s) => s,
            None => {
                listing.error = Some(format!("no such server ({idx})"));
                return listing;
            }
        };
        let url = format!("{}{}", origin(&server.url), dir);
        let body = match propfind(server, &url) {
            Ok(b) => b,
            Err(e) => {
                listing.error = Some(e);
                return listing;
            }
        };

        // Parent (unless at the server base).
        let base = base_path(&server.url);
        if dir.trim_end_matches('/') != base.trim_end_matches('/') {
            let up = parent_path(dir);
            listing.entries.push(Entry {
                kind: EntryKind::Parent,
                name: "..".into(),
                path: format!("webdav:srv:{idx}:{up}"),
                ..Entry::dir("..", "")
            });
        }

        let self_dir = url_decode(dir);
        let self_dir = self_dir.trim_end_matches('/');
        let mut dirs = Vec::new();
        let mut media = Vec::new();
        for e in parse_propfind(&body) {
            let decoded = url_decode(&e.rel);
            if decoded.trim_end_matches('/') == self_dir {
                continue; // the directory itself
            }
            let name = last_segment(&e.rel);
            if name.is_empty() {
                continue;
            }
            if e.is_dir {
                dirs.push(Entry::dir(name, format!("webdav:srv:{idx}:{}", e.rel)));
            } else if is_media(&name) {
                media.push(Entry::media(
                    name,
                    format!("webdav:play:{idx}:{}", e.rel),
                ));
            }
        }
        dirs.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
        media.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
        listing.entries.extend(dirs);
        listing.entries.extend(media);
        listing
    }
}

fn parent_path(dir: &str) -> String {
    let t = dir.trim_end_matches('/');
    match t.rfind('/') {
        Some(0) | None => "/".to_string(),
        Some(i) => format!("{}/", &t[..i]),
    }
}

/// `webdav:srv:<idx>:<rel>` / `webdav:play:<idx>:<rel>` -> (idx, rel).
fn split_locator(prefix: &str, path: &str) -> Option<(usize, String)> {
    let rest = path.strip_prefix(prefix)?;
    let colon = rest.find(':')?;
    let idx: usize = rest[..colon].parse().ok()?;
    Some((idx, rest[colon + 1..].to_string()))
}

fn propfind(server: &Server, url: &str) -> Result<String, String> {
    let body = "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop>\
        <d:resourcetype/><d:getcontentlength/></d:prop></d:propfind>";
    let mut req = ureq::request("PROPFIND", url)
        .set("Depth", "1")
        .set("Content-Type", "application/xml")
        .set("User-Agent", "Stereonaut");
    if let Some(h) = auth_header(server) {
        if let Some(v) = h.strip_prefix("Authorization: ") {
            req = req.set("Authorization", v);
        }
    }
    match req.send_string(body) {
        Ok(resp) => resp.into_string().map_err(|e| e.to_string()),
        Err(ureq::Error::Status(401, _)) => Err("login rejected by server".into()),
        Err(ureq::Error::Status(code, _)) => Err(format!("WebDAV error (HTTP {code})")),
        Err(e) => Err(e.to_string()),
    }
}

impl Source for WebdavSource {
    fn id(&self) -> &str {
        "webdav"
    }
    fn label_key(&self) -> &str {
        "rail.webdav"
    }
    fn root(&self) -> String {
        "webdav:".into()
    }
    fn list(&mut self, path: &str) -> Listing {
        if path == "webdav:" {
            return self.list_servers();
        }
        if let Some((idx, dir)) = split_locator("webdav:srv:", path) {
            return self.list_dir(idx, &dir);
        }
        Listing {
            path: path.to_string(),
            error: Some(format!("bad webdav path: {path}")),
            ..Default::default()
        }
    }
    fn resolve(&mut self, path: &str) -> std::io::Result<Resolved> {
        let (idx, rel) = split_locator("webdav:play:", path)
            .ok_or_else(|| std::io::Error::new(std::io::ErrorKind::InvalidInput, "bad webdav locator"))?;
        let server = self.servers.get(idx).ok_or_else(|| {
            std::io::Error::new(std::io::ErrorKind::NotFound, "no such server")
        })?;
        Ok(Resolved {
            locator: format!("{}{}", origin(&server.url), rel),
            http_header: auth_header(server),
        })
    }
    fn as_webdav_mut(&mut self) -> Option<&mut WebdavSource> {
        Some(self)
    }
}

// ---- server management (UI + persistence) ---------------------------

impl WebdavSource {
    /// Reachability check: PROPFIND the server base. Ok = credentials work.
    fn test(server: &Server) -> Result<(), String> {
        let url = format!("{}{}", origin(&server.url), base_path(&server.url));
        propfind(server, &url).map(|_| ())
    }

    fn save_server(&mut self, idx: Option<usize>, server: Server) -> Result<(), String> {
        match idx {
            Some(i) if i < self.servers.len() => self.servers[i] = server,
            _ => self.servers.push(server),
        }
        self.save().map_err(|e| e.to_string())
    }

    fn remove(&mut self, idx: usize) -> Result<(), String> {
        if idx < self.servers.len() {
            self.servers.remove(idx);
        }
        self.save().map_err(|e| e.to_string())
    }

    /// Server-list page with add/edit/delete. Returns a path to open when a
    /// server row is clicked. ponytail: English labels; move to i18n keys if
    /// the other pages' translations matter here too.
    pub fn manage(&mut self, ui: &mut egui::Ui, form: &mut ServerForm) -> Option<String> {
        let mut open_path = None;

        if ui.button("\u{FF0B} Add server").clicked() {
            *form = ServerForm { open: true, ..Default::default() };
        }
        ui.separator();

        let mut edit: Option<usize> = None;
        let mut delete: Option<usize> = None;
        for (i, s) in self.servers.iter().enumerate() {
            ui.horizontal(|ui| {
                let name = if s.name.is_empty() { s.url.clone() } else { s.name.clone() };
                if ui.button(format!("\u{1F3AC} {name}")).clicked() {
                    open_path = Some(format!("webdav:srv:{i}:{}", base_path(&s.url)));
                }
                if ui.small_button("\u{270F}").clicked() {
                    edit = Some(i);
                }
                if ui.small_button("\u{1F5D1}").clicked() {
                    delete = Some(i);
                }
            });
        }
        if let Some(i) = edit {
            if let Some(s) = self.servers.get(i) {
                *form = ServerForm {
                    open: true,
                    edit_idx: Some(i),
                    name: s.name.clone(),
                    url: s.url.clone(), // already includes scheme+port
                    user: s.user.clone(),
                    password: s.password.clone(),
                    ..Default::default()
                };
            }
        }
        if let Some(i) = delete {
            let _ = self.remove(i);
            if form.edit_idx == Some(i) {
                form.open = false;
            }
        }

        if form.open {
            ui.separator();
            egui::Grid::new("wd_form").num_columns(2).show(ui, |ui| {
                ui.label("Name");
                ui.text_edit_singleline(&mut form.name);
                ui.end_row();
                ui.label("URL");
                ui.text_edit_singleline(&mut form.url);
                ui.end_row();
                ui.label("Port");
                ui.text_edit_singleline(&mut form.port);
                ui.end_row();
                ui.label("User");
                ui.text_edit_singleline(&mut form.user);
                ui.end_row();
                ui.label("Password");
                ui.add(egui::TextEdit::singleline(&mut form.password).password(true));
                ui.end_row();
            });
            if !form.status.is_empty() {
                ui.label(&form.status);
            }
            ui.horizontal(|ui| {
                if ui.button("Test").clicked() {
                    form.status = match Self::test(&form.to_server()) {
                        Ok(()) => "connection ok".into(),
                        Err(e) => format!("failed: {e}"),
                    };
                }
                if ui.button("Save").clicked() {
                    let server = form.to_server();
                    match Self::test(&server) {
                        Ok(()) => match self.save_server(form.edit_idx, server) {
                            Ok(()) => form.open = false,
                            Err(e) => form.status = format!("save failed: {e}"),
                        },
                        Err(e) => form.status = format!("not saved \u{2014} test failed: {e}"),
                    }
                }
                if ui.button("Cancel").clicked() {
                    form.open = false;
                }
            });
        }
        open_path
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalize_url_adds_scheme_and_port() {
        assert_eq!(normalize_url("h:8443/dav", ""), "http://h:8443/dav");
        assert_eq!(normalize_url("h", "8443"), "http://h:8443");
        assert_eq!(normalize_url("h/dav", "8443"), "http://h:8443/dav");
        assert_eq!(normalize_url("https://h", "443"), "https://h:443");
        // host already carries a port: leave it untouched
        assert_eq!(normalize_url("https://h:9000/dav", "443"), "https://h:9000/dav");
    }

    #[test]
    fn origin_and_base() {
        assert_eq!(origin("https://h:8443/dav/movies"), "https://h:8443");
        assert_eq!(base_path("https://h:8443/dav"), "/dav/");
        assert_eq!(base_path("https://h:8443"), "/");
    }

    #[test]
    fn parses_multistatus() {
        let body = r#"<?xml version="1.0"?>
        <d:multistatus xmlns:d="DAV:">
          <d:response><d:href>/dav/</d:href>
            <d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop></d:propstat>
          </d:response>
          <d:response><d:href>/dav/Sub%20Dir/</d:href>
            <d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop></d:propstat>
          </d:response>
          <d:response><d:href>/dav/clip.mkv</d:href>
            <d:propstat><d:prop><d:resourcetype/></d:prop></d:propstat>
          </d:response>
        </d:multistatus>"#;
        let e = parse_propfind(body);
        assert_eq!(e.len(), 3);
        assert!(e[0].is_dir); // /dav/
        assert!(e[1].is_dir); // Sub Dir
        assert!(!e[2].is_dir); // clip.mkv
        assert_eq!(last_segment(&e[1].rel), "Sub Dir");
        assert_eq!(last_segment(&e[2].rel), "clip.mkv");
    }
}
