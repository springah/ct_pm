#!/bin/bash
# build.sh — compile the Linux/PortMaster `ct` binary inside the PortMaster
# aarch64 builder image (glibc 2.31). Run from the repo root, e.g.:
#
#   docker run --rm --platform=linux/arm64 \
#     -v "$PWD":/workspace -v "$HOME/Desktop/ct_switch_build/ffmpeg-aarch64":/ff \
#     -w /workspace IMG bash portmaster/build.sh
#
# FFPREFIX must point at a decode-only FFmpeg 7.x build (see
# ../ct_switch_build/ffmpeg-src/build_ffmpeg.sh). The real FMV player
# (source/movie_player.c) is compiled in; portmaster/movie_stub.c is only used
# for quick boot-to-title builds without ffmpeg.
set -e

FFPREFIX="${FFPREFIX:-/ff}"
OUT="${OUT:-ct}"

if [ ! -f "$FFPREFIX/include/libavformat/avformat.h" ]; then
  echo "!! FFmpeg headers not found at $FFPREFIX/include — build ffmpeg first." >&2
  exit 1
fi

CF="-O3 -g -mcpu=cortex-a53 -Iportmaster -Isource \
    $(sdl2-config --cflags) $(pkg-config --cflags freetype2) -I$FFPREFIX/include"

SRC="portmaster/os_linux.c portmaster/compat_libc.c portmaster/osk.c portmaster/crash.c \
     source/asset.c source/config.c source/error.c source/gfx.c source/imports.c \
     source/jni_fake.c source/libc_shim.c source/main.c source/movelog.c \
     source/opensles.c source/patches.c source/prefs.c source/rescale.c \
     source/so_util.c source/util.c source/movie_player.c"

LIBS="$(sdl2-config --libs) $(pkg-config --libs freetype2) \
      -L$FFPREFIX/lib -lavformat -lavcodec -lswscale -lswresample -lavutil \
      -lGLESv2 -lEGL -lpthread -lm -ldl"

echo ">> compiling $OUT (with FMV: source/movie_player.c, ffmpeg @ $FFPREFIX)"
# shellcheck disable=SC2086
gcc $CF $SRC $LIBS -o "$OUT"
echo ">> done: $(ls -la "$OUT" | awk '{print $5" bytes"}')"
echo ">> ffmpeg NEEDED entries:"
readelf -d "$OUT" | grep NEEDED | grep -E "libav|libsw" || echo "   (none — FMV not linked?)"
