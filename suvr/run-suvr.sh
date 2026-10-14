#!/bin/sh
# Launch the SUVR 2D front-end on the Steam Frame. No typing in VR needed.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)

export SUVR_LANG_DIR="$root/src/player/lang"
export SUVR_PLAYER_DIR="$root/src/player/build"
# NB: TP10 Turnip (Main10) is enabled only on the player subprocess, in
# player.rs. Do NOT export VK_ICD_FILENAMES here: the custom ICD breaks the
# 2D front-end's gamescope swapchain and it fails to start.

# Prefer the release build if present, else debug.
bin="$here/target/release/suvr"
[ -x "$bin" ] || bin="$here/target/debug/suvr"

exec "$bin" "$@" 2>&1 | tee /tmp/suvr.log
