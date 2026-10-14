#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
build_dir="$script_dir/build"
install_dir="$HOME/.local/lib/stereonaut"
bin_dir="$HOME/.local/bin"
applications_dir="$HOME/.local/share/applications"

for file in stereonaut-launcher stereonaut-session \
            stereonaut-renderer video-projection.vert.spv \
            nv12-projection.frag.spv tp10-projection.frag.spv \
            stereo-disparity.comp.spv stereo-disparity-tp10.comp.spv \
            ui-panel.vert.spv ui-panel.frag.spv; do
    if [ ! -f "$build_dir/$file" ]; then
        echo "missing build artifact: $build_dir/$file" >&2
        exit 1
    fi
done

install -d "$install_dir" "$bin_dir" "$applications_dir"
install -m 0755 \
    "$build_dir/stereonaut-launcher" \
    "$build_dir/stereonaut-session" \
    "$build_dir/stereonaut-renderer" \
    "$install_dir/"

# 2D front-end (Rust suvr) \u2014 the REAL entry point: it browses and spawns the
# player-only 3D core per title. Built with the device's rustup toolchain
# (~/.cargo/bin is not on the default non-login PATH).
export PATH="$HOME/.cargo/bin:$PATH"
command -v cargo >/dev/null 2>&1 || {
    echo "FATAL: cargo not found (install rustup); cannot build the suvr front-end" >&2
    exit 1
}
( cd "$project_dir/suvr" && cargo build --release )
install -m 0755 "$project_dir/suvr/target/release/suvr" "$install_dir/suvr"
install -m 0644 \
    "$build_dir/video-projection.vert.spv" \
    "$build_dir/nv12-projection.frag.spv" \
    "$build_dir/tp10-projection.frag.spv" \
    "$build_dir/stereo-disparity.comp.spv" \
    "$build_dir/stereo-disparity-tp10.comp.spv" \
    "$build_dir/ui-panel.vert.spv" \
    "$build_dir/ui-panel.frag.spv" \
    "$install_dir/"

# Language catalogues, loaded at runtime relative to the launcher's cwd.
install -d "$install_dir/lang"
install -m 0644 "$script_dir/lang/"* "$install_dir/lang/"

# TP10 Turnip (Mesa freedreno with Main10 sampling) — MANDATORY. Native HEVC
# Main10 depends on it. Preferred source: build/libvulkan_freedreno.so (freshly
# cross-built via tools/mesa-tp10); fallback: the committed prebuilt aarch64
# binary (prebuilt/aarch64/). Fails hard if neither exists.
turnip_dir="$install_dir/turnip"
turnip_so="$build_dir/libvulkan_freedreno.so"
[ -f "$turnip_so" ] || turnip_so="$project_dir/prebuilt/aarch64/libvulkan_freedreno.so"
if [ ! -f "$turnip_so" ]; then
    echo "FATAL: TP10 Turnip not found (build/ or prebuilt/aarch64/libvulkan_freedreno.so)" >&2
    exit 1
fi
install -d "$turnip_dir"
install -m 0755 "$turnip_so" "$turnip_dir/libvulkan_freedreno.so"
printf '{"file_format_version": "1.0.1", "ICD": {"library_path": "%s", "api_version": "1.4.0"}}\n' \
    "$turnip_dir/libvulkan_freedreno.so" >"$turnip_dir/icd.json"
chmod 0644 "$turnip_dir/icd.json"
if ldd "$turnip_dir/libvulkan_freedreno.so" 2>/dev/null | grep -q "not found"; then
    echo "WARNING: TP10 Turnip has missing libraries (SteamOS update?); Main10 will" >&2
    echo "         fall back to Jellyfin transcode until it is rebuilt from mesa." >&2
fi
printf 'installed TP10 Turnip: %s\n' "$turnip_dir"

# A copy of the sources lets the launcher rebuild itself after a SteamOS
# update has replaced a library it was linked against (see below). Skipped
# when this script already runs from that copy.
source_dir="${XDG_DATA_HOME:-$HOME/.local/share}/stereonaut/source"
# A git checkout at source_dir (what install.sh sets up) is kept intact so the
# launcher can `git pull` it. Only fall back to a flat tar copy when installing
# from some other tree that is not itself the source checkout.
if [ "$project_dir" != "$source_dir" ] && [ ! -d "$source_dir/.git" ]; then
    rm -rf "$source_dir"
    install -d "$source_dir/src" "$source_dir/tools"
    (cd "$project_dir" && tar -cf - --exclude=src/player/build \
        src/player tools/openxr-playback prebuilt) | tar -C "$source_dir" -xf -
    printf 'installed sources for rebuilds: %s\n' "$source_dir"
fi

# Written to a temporary file and moved into place: this script may be run by
# the launcher itself, which must not see its file change while it runs.
cat >"$bin_dir/stereonaut.new" <<'EOF'
#!/bin/sh
set -eu
# Without an argument the launcher opens the start page (local files and
# Jellyfin). The log of the last run helps to diagnose stutter and failures.
state_dir="${XDG_STATE_HOME:-$HOME/.local/state}/stereonaut"
mkdir -p "$state_dir"
log="$state_dir/launcher.log"
exec >"$log" 2>&1
source_dir="${XDG_DATA_HOME:-$HOME/.local/share}/stereonaut/source"
cd "$HOME/.local/lib/stereonaut"

# Libraries the given files need but the system no longer has (a SteamOS
# update can replace a library by a new major version).
missing_libraries() {
    ldd "$@" 2>/dev/null | sed -n 's/^[[:space:]]*\([^[:space:]]*\).*not found.*/\1/p' |
        sort -u | tr '\n' ' '
}

# Dialogs appear on the SteamVR desktop (visible in the headset) or the
# desktop session, dark like the player (under gamescope libadwaita gets no
# usable colour scheme and falls back to light); without zenity the answer
# is "no".
has_dialogs() {
    command -v zenity >/dev/null 2>&1
}

dialog() {
    ADW_DEBUG_COLOR_SCHEME=prefer-dark GTK_THEME=Adwaita:dark zenity "$@"
}

ask() {
    has_dialogs &&
        dialog --question --title=Stereonaut --text="$1" \
            --ok-label="Neu bauen" --cancel-label="Abbrechen" --timeout=120
}

report_error() {
    echo "$1"
    if has_dialogs; then
        dialog --error --title=Stereonaut --timeout=120 \
            --text="$1

Details: $log" || true
    fi
}

# Builds and installs from the source copy; progress as a pulsing bar.
rebuild() {
    status="$state_dir/rebuild.status"
    echo 1 >"$status"
    {
        if "$source_dir/src/player/build-stereonaut-session.sh" >&2 &&
            "$source_dir/src/player/install-user-launcher.sh" >&2; then
            echo 0 >"$status"
        fi
        echo 100
    } | if has_dialogs; then
        dialog --progress --pulsate --no-cancel --auto-close \
            --title=Stereonaut --text="Stereonaut wird neu gebaut …" || true
    else
        cat >/dev/null
    fi
    [ "$(cat "$status")" = 0 ]
}

# Auto-update from GitHub at launch (best-effort; offline or no git = skip).
# A new upstream commit triggers a rebuild+reinstall before the player starts.
if [ -d "$source_dir/.git" ] && command -v git >/dev/null 2>&1; then
    before=$(git -C "$source_dir" rev-parse HEAD 2>/dev/null || echo none)
    git -C "$source_dir" pull --ff-only >/dev/null 2>&1 || true
    after=$(git -C "$source_dir" rev-parse HEAD 2>/dev/null || echo none)
    if [ "$before" != "$after" ]; then
        echo "update: $before -> $after; rebuilding"
        rebuild || report_error "Automatisches Update fehlgeschlagen."
    fi
fi

missing=$(missing_libraries suvr stereonaut-launcher stereonaut-session \
                            stereonaut-renderer)
if [ -n "$missing" ]; then
    echo "missing libraries: $missing"
    if [ ! -x "$source_dir/src/player/build-stereonaut-session.sh" ]; then
        report_error "Nach einem SteamOS-Update fehlen Bibliotheken ($missing). Es liegt kein Quelltext zum Neubauen vor; bitte Stereonaut neu installieren."
        exit 1
    fi
    if ! ask "Nach einem SteamOS-Update fehlen Bibliotheken:
$missing

Stereonaut jetzt neu bauen? Das dauert etwa 10 Sekunden."; then
        echo "rebuild declined or not answered"
        exit 1
    fi
    if ! rebuild; then
        report_error "Der Neubau von Stereonaut ist fehlgeschlagen."
        exit 1
    fi
    missing=$(missing_libraries stereonaut-launcher stereonaut-session \
                                stereonaut-renderer)
    if [ -n "$missing" ]; then
        report_error "Auch nach dem Neubau fehlen Bibliotheken: $missing"
        exit 1
    fi
    echo "rebuild finished"
fi

# TP10 Turnip is applied to the PLAYER subprocess only, by the front-end
# (player.rs). Exporting the custom ICD here would break the 2D front-end's
# gamescope swapchain, so we only sanity-check that it is present.
if [ -f turnip/icd.json ]; then
    turnip_missing=$(missing_libraries turnip/libvulkan_freedreno.so)
    [ -z "$turnip_missing" ] ||
        echo "WARNING: TP10 Turnip missing libraries: $turnip_missing; Main10 will transcode"
else
    echo "WARNING: TP10 Turnip not installed; Main10 will transcode"
fi

# Launch the 2D front-end (browser). SUVR_PLAYER_DIR points at this install so
# suvr finds stereonaut-launcher/session/renderer and turnip/icd.json here.
export SUVR_PLAYER_DIR="$PWD" SUVR_LANG_DIR="$PWD/lang"
exec ./suvr
EOF
chmod 0755 "$bin_dir/stereonaut.new"
mv "$bin_dir/stereonaut.new" "$bin_dir/stereonaut"

cat >"$applications_dir/stereonaut.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Stereonaut
Comment=Native OpenXR video player
Exec=$bin_dir/stereonaut
Terminal=false
Categories=AudioVideo;Player;
StartupNotify=false
EOF
chmod 0644 "$applications_dir/stereonaut.desktop"

printf 'installed launcher: %s\n' "$bin_dir/stereonaut"
printf 'installed desktop entry: %s\n' \
       "$applications_dir/stereonaut.desktop"
