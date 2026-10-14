//! The 2D front-end. Side rail of sources + a content pane that lists the
//! current location and launches the VR player on a media click.
//!
//! Reusable blocks live in `ui/`; this file is just wiring (which source is
//! active, what location is shown). Keep integration logic out of here.

use crate::i18n::I18n;
use crate::model::{EntryKind, Listing};
use crate::settings::Settings;
use crate::sources::webdav::ServerForm;
use crate::sources::Sources;
use crate::{player, ui};

pub struct App {
    pub i18n: I18n,
    pub settings: Settings,
    pub sources: Sources,
    pub active: String, // source id
    pub listing: Listing,
    pub wd_form: ServerForm, // WebDAV add/edit form state
    pub sys_notice: String,  // feedback for the Steam-helper restart button
    pub langs: Vec<(String, String)>, // (code, display name) for the picker
}

impl App {
    pub fn new(i18n: I18n, settings: Settings, langs: Vec<(String, String)>, mut sources: Sources) -> Self {
        let active = sources.0.first().map(|s| s.id().to_string()).unwrap_or_default();
        let listing = match sources.get(&active) {
            Some(s) => {
                let root = s.root();
                s.list(&root)
            }
            None => Listing::default(),
        };
        App { i18n, settings, sources, active, listing, wd_form: ServerForm::default(), sys_notice: String::new(), langs }
    }

    fn open(&mut self, source_id: &str, path: &str) {
        if let Some(s) = self.sources.get(source_id) {
            self.active = source_id.to_string();
            // ponytail: blocking list. Local is instant; move to a worker when
            // Jellyfin/WebDAV latency makes the frame hitch.
            self.listing = s.list(path);
            log::debug!(
                "list source={source_id} path={path} entries={} err={:?}",
                self.listing.entries.len(),
                self.listing.error
            );
        }
    }

    fn play(&mut self, path: &str, start_us: i64) {
        let resolved = match self.sources.get(&self.active) {
            Some(s) => s.resolve(path),
            None => Ok(crate::sources::Resolved { locator: path.to_string(), http_header: None }),
        };
        match resolved {
            Ok(r) => {
                player::play(&r.locator, r.http_header.as_deref(), start_us);
            }
            Err(e) => {
                log::error!("resolve failed for {path}: {e}");
                self.listing.error = Some(format!("cannot open: {e}"));
            }
        }
        // Back from VR: nothing to restore, the listing is still here.
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        // Collect clicks here to avoid borrowing self mutably mid-closure.
        let mut switch: Option<String> = None;
        let mut navigate: Option<String> = None;
        let mut launch: Option<(String, i64)> = None;
        let mut restart = false;
        let mut pick_lang: Option<String> = None;

        let rail: Vec<(String, String)> = self
            .sources
            .0
            .iter()
            .map(|s| (s.id().to_string(), self.i18n.t(s.label_key()).to_string()))
            .collect();

        egui::SidePanel::left("rail").resizable(false).show(ctx, |ui_| {
            if let Some(id) = ui::rail(ui_, &rail, &self.active) {
                switch = Some(id);
            }
            // Bottom-left: free the HW decoder by restarting Steam's CEF helper.
            ui_.with_layout(egui::Layout::bottom_up(egui::Align::Min), |ui_| {
                if ui_.button("\u{27F3} Restart Steam helper").clicked() {
                    restart = true;
                }
                if !self.sys_notice.is_empty() {
                    ui_.small(&self.sys_notice);
                }
            });
        });

        // Language picker: a new section under the 2D interface. One selectable
        // per file in the lang dir; the same files the 3D player reads.
        egui::TopBottomPanel::bottom("language").show(ctx, |ui_| {
            ui_.add_space(4.0);
            ui_.horizontal_wrapped(|ui_| {
                ui_.label(format!("{}:", self.i18n.t("options.language")));
                for (code, name) in &self.langs {
                    let selected = *code == self.i18n.lang;
                    if ui_.selectable_label(selected, name).clicked() {
                        pick_lang = Some(code.clone());
                    }
                }
            });
        });

        // WebDAV root shows the server-management page; everything else lists.
        let webdav_root = self.active == "webdav" && self.listing.path == "webdav:";
        egui::CentralPanel::default().show(ctx, |ui_| {
            if webdav_root {
                if let Some(wd) = self.sources.get("webdav").and_then(|s| s.as_webdav_mut()) {
                    if let Some(path) = wd.manage(ui_, &mut self.wd_form) {
                        navigate = Some(path);
                    }
                }
                return;
            }
            match ui::listing_view(ui_, &self.i18n, &self.listing) {
                ui::Clicked::Entry(i) => {
                    if let Some(e) = self.listing.entries.get(i) {
                        match e.kind {
                            EntryKind::Media => launch = Some((e.path.clone(), e.resume_us)),
                            _ => navigate = Some(e.path.clone()),
                        }
                    }
                }
                ui::Clicked::None => {}
            }
        });

        if let Some(id) = switch {
            let root = self.sources.get(&id).map(|s| s.root());
            if let Some(root) = root {
                self.open(&id, &root);
            }
        }
        if let Some(path) = navigate {
            let active = self.active.clone();
            self.open(&active, &path);
        }
        if let Some((path, start)) = launch {
            self.play(&path, start);
        }
        if let Some(code) = pick_lang {
            self.i18n.set_lang(&code);
            self.settings.set_str("language", &code);
            self.settings.save();
        }
        if restart {
            self.sys_notice = if player::restart_webhelper() {
                "Steam helper restarted \u{2014} decoder freed".into()
            } else {
                "restart failed (see log)".into()
            };
        }
    }
}
