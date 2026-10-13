#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
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

# Optional: the Turnip build with TP10 sampling (tools/mesa-tp10), copied to
# build/libvulkan_freedreno.so. Only Stereonaut loads it; it enables native
# HEVC Main10. Without it Main10 is transcoded by Jellyfin as before.
turnip_dir="$install_dir/turnip"
if [ -f "$build_dir/libvulkan_freedreno.so" ]; then
    install -d "$turnip_dir"
    install -m 0755 "$build_dir/libvulkan_freedreno.so" "$turnip_dir/"
    printf '{"file_format_version": "1.0.1", "ICD": {"library_path": "%s", "api_version": "1.4.0"}}\n' \
        "$turnip_dir/libvulkan_freedreno.so" >"$turnip_dir/icd.json"
    chmod 0644 "$turnip_dir/icd.json"
    printf 'installed TP10 Turnip: %s\n' "$turnip_dir"
fi

# A copy of the sources lets the launcher rebuild itself after a SteamOS
# update has replaced a library it was linked against (see below). Skipped
# when this script already runs from that copy.
source_dir="${XDG_DATA_HOME:-$HOME/.local/share}/stereonaut/source"
project_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
if [ "$project_dir" != "$source_dir" ]; then
    rm -rf "$source_dir"
    install -d "$source_dir/src" "$source_dir/tools"
    (cd "$project_dir" && tar -cf - --exclude=src/player/build \
        src/player tools/openxr-playback) | tar -C "$source_dir" -xf -
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

missing=$(missing_libraries stereonaut-launcher stereonaut-session \
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

# Own Turnip with TP10 sampling for HEVC Main10, if installed and still
# loadable; otherwise Main10 is transcoded by Jellyfin.
if [ -f turnip/icd.json ]; then
    turnip_missing=$(missing_libraries turnip/libvulkan_freedreno.so)
    if [ -z "$turnip_missing" ]; then
        export VK_ICD_FILENAMES="$PWD/turnip/icd.json" STEREONAUT_TP10=1
    else
        echo "TP10 Turnip disabled, missing libraries: $turnip_missing"
    fi
fi
exec ./stereonaut-launcher ${1:+"$1"}
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
