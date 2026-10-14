//! SUVR 2D front-end entry point. Opens a flat window inside the headset; the
//! VR compositor feeds it ordinary mouse/keyboard events. No OpenXR, no
//! controllers here — those live only in the 3D player (see src/player.rs).

mod app;
mod i18n;
mod logging;
mod model;
mod player;
mod settings;
mod sources;
mod ui;

use std::path::PathBuf;

fn lang_dir() -> PathBuf {
    // Installed layout keeps lang next to the binary; dev falls back to the repo.
    if let Ok(p) = std::env::var("SUVR_LANG_DIR") {
        return PathBuf::from(p);
    }
    let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .map(|p| p.join("src/player/lang"))
        .unwrap_or_else(|| PathBuf::from("lang"));
    repo
}

fn main() -> eframe::Result<()> {
    logging::init();
    let settings = settings::Settings::load();
    let lang = settings.get_str("language", "eng").to_string();
    let dir = lang_dir();
    let i18n = i18n::I18n::load(&dir, &lang);
    let langs = i18n::I18n::list(&dir);
    let sources = sources::Sources::new();

    let title = i18n.t("app.title").to_string();
    let options = eframe::NativeOptions::default();
    eframe::run_native(
        if title == "app.title" { "SUVR" } else { &title },
        options,
        Box::new(move |_cc| Ok(Box::new(app::App::new(i18n, settings, langs, sources)))),
    )
}
