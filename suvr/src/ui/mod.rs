//! Reusable 2D widgets. Pure egui, no source/player knowledge. New pages reuse
//! these instead of drawing raw. Keep each one small and self-contained.

use crate::i18n::I18n;
use crate::model::{EntryKind, Listing};

pub enum Clicked {
    None,
    Entry(usize),
}

/// Left side rail: one selectable button per source. Returns the id clicked.
pub fn rail(ui: &mut egui::Ui, items: &[(String, String)], active: &str) -> Option<String> {
    let mut clicked = None;
    ui.add_space(8.0);
    for (id, label) in items {
        let selected = id == active;
        if ui.selectable_label(selected, label).clicked() {
            clicked = Some(id.clone());
        }
    }
    clicked
}

/// Scrollable list of a location. Folders and media as rows; `..` navigates up.
pub fn listing_view(ui: &mut egui::Ui, i18n: &I18n, listing: &Listing) -> Clicked {
    if let Some(err) = &listing.error {
        ui.colored_label(egui::Color32::LIGHT_RED, err);
    }
    if listing.entries.is_empty() && listing.error.is_none() {
        ui.label(i18n.t("status.empty"));
        return Clicked::None;
    }

    let mut result = Clicked::None;
    egui::ScrollArea::vertical().show(ui, |ui| {
        for (i, e) in listing.entries.iter().enumerate() {
            let icon = match e.kind {
                EntryKind::Parent => "⬆",
                EntryKind::Directory => "📁",
                EntryKind::Media => "🎬",
            };
            let mut text = format!("{icon}  {}", e.name);
            if e.kind == EntryKind::Media && !e.codec.is_empty() {
                text.push_str(&format!("   [{}]", e.codec));
            }
            if ui.selectable_label(false, text).clicked() {
                result = Clicked::Entry(i);
            }
        }
    });
    result
}
