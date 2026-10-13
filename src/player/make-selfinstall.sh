#!/bin/sh
# Packs the built player and its sources into one self-extracting installer
# (makeself style: a shell stub followed by a gzip tarball). Running the
# resulting file extracts to a temporary directory and runs the normal
# install-user-launcher.sh, so one file installs the binaries, shaders,
# languages and the source copy used for self-rebuilds.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
out="${1:-$project_dir/SUVR-install.run}"

# Build first so the binaries and shaders are present to bundle.
"$script_dir/build-stereonaut-session.sh"

payload=$(mktemp)
trap 'rm -f "$payload"' EXIT
# Ship the sources too (minus the heavy build/ tree) so the launcher can
# rebuild itself after a SteamOS library bump, exactly like a normal install.
# The build artifacts the installer copies live in src/player/build and are
# included explicitly.
tar -C "$project_dir" -czf "$payload" \
    --exclude='src/player/build/*.o' \
    src/player tools/openxr-playback

cat >"$out" <<'STUB'
#!/bin/sh
# Self-extracting SUVR installer. Unpacks to a temp dir and installs into
# ~/.local (binaries, shaders, languages, desktop entry, source copy).
set -eu
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
start=$(awk '/^__SUVR_ARCHIVE__$/{print NR + 1; exit}' "$0")
tail -n +"$start" "$0" | tar -C "$dir" -xzf -
exec "$dir/src/player/install-user-launcher.sh"
__SUVR_ARCHIVE__
STUB
cat "$payload" >>"$out"
chmod 0755 "$out"

printf 'built self-installer: %s (%s)\n' "$out" "$(du -h "$out" | cut -f1)"
printf 'install with: sh %s\n' "$out"
