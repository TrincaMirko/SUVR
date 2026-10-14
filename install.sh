#!/bin/sh
# One-shot installer for Stereonaut / SUVR on the Steam Frame (SteamOS, aarch64).
#
# Clones (or updates) the repo into the self-rebuild source directory, builds
# the player natively on-device, installs it together with the MANDATORY TP10
# Turnip (native HEVC Main10), and wires the launcher to auto-update from GitHub
# at every start.
#
# Bootstrap straight on the device:
#   curl -fsSL https://raw.githubusercontent.com/TrincaMirko/SUVR/main/install.sh | sh
# or, from a checkout:
#   sh install.sh
set -eu

REPO="${SUVR_REPO:-https://github.com/TrincaMirko/SUVR.git}"
BRANCH="${SUVR_BRANCH:-main}"
src="${XDG_DATA_HOME:-$HOME/.local/share}/stereonaut/source"

for t in git meson ninja cc glslc pkg-config; do
    command -v "$t" >/dev/null 2>&1 || { echo "missing build tool: $t" >&2; exit 1; }
done

if [ -d "$src/.git" ]; then
    echo "updating existing checkout: $src"
    git -C "$src" fetch --depth 1 origin "$BRANCH"
    git -C "$src" checkout -B "$BRANCH" "origin/$BRANCH"
else
    echo "cloning $REPO -> $src"
    mkdir -p "$(dirname "$src")"
    git clone --depth 1 --branch "$BRANCH" "$REPO" "$src"
fi

cd "$src"
echo "=== build ==="
sh src/player/build-stereonaut-session.sh
echo "=== install (incl. mandatory TP10 Turnip) ==="
sh src/player/install-user-launcher.sh

echo
echo "done. Launch with:  ~/.local/bin/stereonaut"
echo "or the 'Stereonaut' desktop entry. It auto-updates from GitHub at start."
