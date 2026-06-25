#!/usr/bin/env bash
# package.sh -- assemble a PortMaster-installable ct.zip from portmaster/pkg/
# plus the freshly-built `ct` binary. Whitelist staging: only known files are
# copied in, so the user's commercial game data (libchrono.so / libc++_shared.so
# / assets/) and dev cruft (ct.bak, log.txt, saves) can NEVER leak into the zip.
#
# The zip extracts into /roms/ports/, yielding the runtime layout:
#   Chrono Trigger.sh
#   ct/{port.json, ct, font.ttf, screenshot.png, libs.aarch64/, licenses/}
# The user drops their own libchrono.so/libc++_shared.so/assets/ into ct/ on
# first run (the launcher gates on them).
#
# NOTE: this builds the self-host / on-device install zip. An OFFICIAL PortMaster
# submission is produced differently -- by `tools/build_release.py` inside a
# PortMaster-New fork, from the repo-layout tree (metadata files at the port top
# level + README.md + gameinfo.xml). See portmaster/NOTES.md.
#
# Usage:  portmaster/package.sh [path-to-ct-binary]
#   default binary: ./ct (repo root, where portmaster/build.sh writes it)
#   override out dir/name with OUT=/path/ct.zip
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
PKG="$HERE/pkg"
CT_BIN="${1:-$REPO/ct}"
OUT="${OUT:-$REPO/ct.zip}"

die() { echo "!! $*" >&2; exit 1; }

[ -f "$CT_BIN" ]                              || die "ct binary not found at $CT_BIN (run portmaster/build.sh first, or pass the path)"
[ -f "$PKG/port.json" ]                       || die "missing $PKG/port.json"
[ -f "$PKG/Chrono Trigger.sh" ]               || die "missing launch script"
[ -f "$PKG/ct/font.ttf" ]                     || die "missing pkg/ct/font.ttf"
[ -d "$PKG/ct/libs.aarch64" ]                 || die "missing pkg/ct/libs.aarch64 (run ffmpeg-build.sh + copy the .so in)"
ls "$PKG/ct/libs.aarch64"/lib*.so* >/dev/null 2>&1 || die "no FFmpeg .so in pkg/ct/libs.aarch64"
[ -d "$PKG/ct/licenses" ]                     || die "missing pkg/ct/licenses"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# --- whitelist-copy into the staging tree -------------------------------------
cp "$PKG/Chrono Trigger.sh"          "$STAGE/Chrono Trigger.sh"
mkdir -p "$STAGE/ct/libs.aarch64" "$STAGE/ct/licenses"
cp "$PKG/port.json"                  "$STAGE/ct/port.json"
cp "$CT_BIN"                         "$STAGE/ct/ct"
cp "$PKG/ct/font.ttf"                "$STAGE/ct/font.ttf"
cp "$PKG/ct/libs.aarch64/"lib*.so*   "$STAGE/ct/libs.aarch64/"
cp "$PKG/ct/licenses/"*              "$STAGE/ct/licenses/"
[ -f "$PKG/ct/screenshot.png" ] && cp "$PKG/ct/screenshot.png" "$STAGE/ct/screenshot.png"
chmod +x "$STAGE/ct/ct" "$STAGE/Chrono Trigger.sh"

# --- zip ----------------------------------------------------------------------
rm -f "$OUT"
( cd "$STAGE" && zip -r -X "$OUT" "Chrono Trigger.sh" "ct" >/dev/null )

echo ">> built $OUT ($(du -h "$OUT" | cut -f1))"
echo ">> contents:"
unzip -Z1 "$OUT" | grep -v '/$' | sed 's/^/   /'
echo ">> reminder: the user supplies libchrono.so + libc++_shared.so + assets/ (never shipped)."
