#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
build_dir="$script_dir/build"
# Steam Frame ships its loader outside the normal linker search path.  A PC
# SteamVR installation normally exposes the loader through pkg-config (or the
# standard -lopenxr_loader name).  OPENXR_LOADER remains an escape hatch for
# either platform, for example when testing a runtime from a separate SDK.
if [ -n "${OPENXR_LOADER:-}" ]; then
  openxr_loader="$OPENXR_LOADER"
elif [ -r /opt/steamvr/bin/linuxarm64/libopenxr_loader.so ]; then
  openxr_loader=/opt/steamvr/bin/linuxarm64/libopenxr_loader.so
elif pkg-config --exists openxr; then
  openxr_loader=$(pkg-config --libs openxr)
else
  openxr_loader=-lopenxr_loader
fi

mkdir -p "$build_dir"

cc -std=c11 -O2 -Wall -Wextra -Werror \
  $(pkg-config --cflags libavformat libavcodec libavutil libswresample alsa \
                       libcurl json-c libjpeg) \
  -I"$project_dir/tools/openxr-playback" \
  -o "$build_dir/stereonaut-session" \
  "$script_dir/stereonaut-session.c" \
  "$script_dir/media-pipeline.c" \
  "$script_dir/packet-queue.c" \
  "$script_dir/audio-output.c" \
  "$script_dir/media-demux.c" \
  "$script_dir/jellyfin.c" \
  "$script_dir/webdav.c" \
  "$script_dir/playback-report.c" \
  "$script_dir/title-settings.c" \
  "$script_dir/format-detect.c" \
  "$script_dir/decoded-video.c" \
  "$script_dir/settings.c" \
  $(pkg-config --libs libavformat libavcodec libavutil libswresample alsa \
                libcurl json-c libjpeg) \
  -lvulkan -lm -pthread

# XR host: video renderer, media browser and Jellyfin client in one process.
cc -std=c11 -O2 -Wall -Wextra -Werror \
  $(pkg-config --cflags libcurl json-c freetype2 libjpeg libavformat \
                       libavcodec libavutil) \
  -I/usr/include -I"$script_dir" -I"$project_dir/tools/openxr-playback" \
  -o "$build_dir/stereonaut-renderer" \
  "$script_dir/stereonaut-renderer.c" "$script_dir/browser-ui.c" \
  "$script_dir/player-ui.c" "$script_dir/ui-draw.c" "$script_dir/thumbnails.c" \
  "$script_dir/file-browser.c" "$script_dir/jellyfin.c" \
  "$script_dir/webdav.c" \
  "$script_dir/trickplay.c" "$script_dir/settings.c" \
  "$script_dir/title-settings.c" "$script_dir/local-thumbnail.c" \
  "$script_dir/controller-bindings.c" "$script_dir/i18n.c" \
  $(pkg-config --libs libcurl json-c freetype2 libjpeg libavformat \
                libavcodec libavutil) \
  -lvulkan -lm -pthread $openxr_loader

cc -std=c11 -O2 -Wall -Wextra -Werror \
  -I"$script_dir" -o "$build_dir/stereonaut-launcher" \
  "$script_dir/stereonaut-launcher.c"

glslc -fshader-stage=vert \
  "$project_dir/tools/openxr-playback/video-projection.vert" \
  -o "$build_dir/video-projection.vert.spv"
glslc -fshader-stage=frag \
  "$project_dir/tools/openxr-playback/nv12-projection.frag" \
  -o "$build_dir/nv12-projection.frag.spv"
glslc -fshader-stage=frag \
  "$project_dir/tools/openxr-playback/tp10-projection.frag" \
  -o "$build_dir/tp10-projection.frag.spv"
glslc -fshader-stage=vert \
  "$project_dir/tools/openxr-playback/ui-panel.vert" \
  -o "$build_dir/ui-panel.vert.spv"
glslc -fshader-stage=frag \
  "$project_dir/tools/openxr-playback/ui-panel.frag" \
  -o "$build_dir/ui-panel.frag.spv"
glslc -fshader-stage=comp \
  "$project_dir/tools/openxr-playback/stereo-disparity.comp" \
  -o "$build_dir/stereo-disparity.comp.spv"
glslc -fshader-stage=comp -DTP10 \
  "$project_dir/tools/openxr-playback/stereo-disparity.comp" \
  -o "$build_dir/stereo-disparity-tp10.comp.spv"
