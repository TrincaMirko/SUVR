# SUVR refactor — what stays C, what dies, what moves to Rust

Strategy **A**: keep the proven hardware video core in C (Iris V4L2 decode,
Vulkan YCbCr import, OpenXR present, ALSA master clock, seek machine). Rewrite
everything *around* it — the 2D menu, sources, orchestration — in Rust
(`suvr/`). Target is arm64 Steam Frame only; all PC/PCVR/desktop paths go.

## KEEP (C core, becomes the player-only binary `suvr-player`)

- `stereonaut-session.c` — demux + Iris decode + audio, per title.
- `stereonaut-renderer.c` — **player mode only** (see "strip from renderer").
- `media-pipeline.c`, `media-demux.c`, `packet-queue.c`, `audio-output.c`,
  `format-detect.c`, `decoded-video.c` (**Iris path only**).
- `player-ui.c` / `ui-draw.c` — **only** the in-VR playback panel (seek bar,
  play/pause, ±10s, grab). The browser-panel code in these files goes.
- `title-settings.c`, `settings.c` (shared settings.json), `i18n.c` (player
  still needs its own strings), `trickplay.c`.

## DELETE (C)

- `stereonaut-launcher.c` — replaced by `suvr/src/player.rs` (spawn + wait).
- `browser-ui.c` (1901) — replaced by `suvr/src/ui/` + `app.rs`.
- `file-browser.c` (1010) — replaced by `suvr/src/sources/local.rs`.
- `jellyfin.c` (1114) — logic ported into `suvr/src/sources/jellyfin.rs`.
- `webdav.c` (502) — logic ported into `suvr/src/sources/webdav.rs`.
- `thumbnails.c`, `local-thumbnail.c` — replaced by Rust `image` decode.
- `controller-bindings.c` (190) + its config UI — no custom controllers.
- On-screen keyboard (`keyboard_*` in browser-ui.c) — system keyboard instead.
- `player-ui-ipc.h` host protocol — no persistent host; the 2D app spawns the
  player per title and the player exits back to it.
- probes/preview harnesses for the deleted modules
  (`file-browser-probe.c`, `ui-preview.c`, `build-ui-preview.sh`).
- `register-steamvr-app.sh` (PC), PC bits of `make-selfinstall.sh`.

## STRIP from `stereonaut-renderer.c`

- **Browser mode** entirely (`BROWSE`, quad browser panel, aim-ray browser hit
  testing). Keep only player mode.
- **PCVR**: `panels_in_eye_images`, `STEREONAUT_UI_LAYER=eye`, eye-image panel
  pipeline, mipmap/aniso UI path. Frame uses quad layers only.
- **PC interaction profiles**: index, oculus/touch, microsoft/motion,
  htc/vive, htc/vive_cosmos. Keep `valve/frame_controller` +
  `khr/simple_controller` fallback.
- **Custom bindings**: reduce the action set to the fixed minimum —
  aim pose + select (trigger) + grab (squeeze). No modifiers, no combos.
- SteamVR dashboard focus hacks that only matter on PC.

## STRIP from `decoded-video.c`

- `DECODED_VIDEO_HARDWARE` (CUDA/NVDEC, Vulkan Video, VA-API) + host-memory
  memfd download path. Keep the Iris V4L2 CAPTURE DMA-BUF path.
- dav1d AV1-on-CPU path is optional; drop unless you still want experimental
  AV1 on the Frame.

## Rust side (`suvr/`, done so far = skeleton)

- `model.rs` — Entry/Listing (mirrors file_browser_entry).
- `sources/` — `Source` trait + `local` (works), `jellyfin`/`webdav` (skeleton).
- `ui/` — reusable egui widgets (rail, listing_view).
- `app.rs` — wiring: active source + current location + launch.
- `player.rs` — spawn `suvr-player`, block, return.
- `settings.rs`, `i18n.rs` — reuse the existing settings.json and lang files.

## TODO before first run

1. Install a Rust toolchain for aarch64 (none present; host here is x86_64).
2. Build the stripped C core as `suvr-player` (one title in, VR out, exit).
3. Port Jellyfin/WebDAV HTTP from the C files onto `ureq`.
4. Thumbnails: decode with `image`, cache under ~/.cache/stereonaut/thumbs.
