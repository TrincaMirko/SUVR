//! Launches the 3D VR player (the kept C core, player-only) for one title and
//! blocks until it exits, then control returns to the 2D app. This is the
//! whole "2D -> small 3D app -> back to 2D" contract.

use std::process::Command;

/// Directory holding the kept C player binaries. The launcher execs
/// `./stereonaut-session` / `./stereonaut-renderer` with relative paths, so we
/// must run it from there. Overridable with SUVR_PLAYER_DIR.
fn player_dir() -> String {
    std::env::var("SUVR_PLAYER_DIR")
        .unwrap_or_else(|_| "/home/steamos/Applications/SUVR/src/player/build".into())
}

pub struct PlayResult {
    pub ok: bool,
    pub code: Option<i32>,
}

/// Restart Steam's CEF helper (`steamwebhelper`) to release the V4L2 decoder
/// buffers it holds idle — those orphaned contexts make the player's 8K
/// `REQBUFS OUTPUT` fail with ENOMEM. Steam respawns the helper automatically.
pub fn restart_webhelper() -> bool {
    match Command::new("pkill").arg("-x").arg("steamwebhelper").status() {
        Ok(s) => {
            log::info!("pkill steamwebhelper status={:?}", s.code());
            // pkill: 0 = killed something, 1 = nothing matched. Both are "ok".
            matches!(s.code(), Some(0) | Some(1))
        }
        Err(e) => {
            log::error!("pkill steamwebhelper failed: {e}");
            false
        }
    }
}

/// Open `locator` (local path or resolved http(s) URL) in VR. `http_header` is
/// an optional `Authorization: ...` line forwarded to libavformat via
/// STEREONAUT_HTTP_HEADERS. `start_us` is a resume position (0 = from start).
/// Blocks until the player exits.
pub fn play(locator: &str, http_header: Option<&str>, start_us: i64) -> PlayResult {
    let dir = player_dir();
    let mut cmd = Command::new(format!("{dir}/stereonaut-launcher"));
    cmd.current_dir(&dir);
    cmd.arg("--play").arg(locator);
    // HEVC Main10 needs the patched TP10 Turnip. Enable it here, on the player
    // only: the custom ICD must not reach the 2D front-end (it breaks its
    // gamescope swapchain). Reuse the Turnip in this tree, else the installed
    // one; without it Main10 is rejected and 8-bit still plays.
    let home = std::env::var("HOME").unwrap_or_default();
    for icd in [
        format!("{dir}/turnip/icd.json"),
        format!("{home}/.local/lib/stereonaut/turnip/icd.json"),
    ] {
        if std::path::Path::new(&icd).exists() {
            cmd.env("VK_ICD_FILENAMES", &icd).env("STEREONAUT_TP10", "1");
            break;
        }
    }
    if let Some(h) = http_header {
        // Credentials via env, never argv (stays out of process listings).
        cmd.env("STEREONAUT_HTTP_HEADERS", h);
    }
    if start_us > 0 {
        // Passed through to the session as a SESSION_ARG.
        cmd.arg("--start-us").arg(start_us.to_string());
    }
    // The C launcher redirects its own stdout/stderr to <dir>/log/stereonaut.1.log.
    log::info!("launching player --play {locator}; player log: {dir}/log/stereonaut.1.log");
    match cmd.status() {
        Ok(status) => {
            log::info!("player exited ok={} code={:?}", status.success(), status.code());
            PlayResult { ok: status.success(), code: status.code() }
        }
        Err(e) => {
            log::error!("failed to launch player: {e}");
            PlayResult { ok: false, code: None }
        }
    }
}
