#!/usr/bin/env bash
# package.sh -- assemble a PortMaster-installable ct_pm.zip from portmaster/pkg/
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
# NOTE: this builds the self-host / on-device install zip. An OFFICIAL submission
# is produced differently -- by `tools/build_release.py` inside a PortMaster-MV-New
# fork, from a `ports/ct/` repo-layout tree. See portmaster/multiverse/README.md,
# which is the single source of truth for submission.
#
# Usage:  portmaster/package.sh [path-to-ct-binary]
#   default binary: ./ct (repo root, where portmaster/build.sh writes it)
#   override out dir/name with OUT=/path/whatever.zip
#
# The distributed archive carries its version (ct-<version>.zip) so a downloaded
# file can be identified without opening it. That is deliberately NOT the same
# string as port.json's "name": that field is the port's identity to
# HarbourMaster and must stay "ct.zip" to match the port directory, which is also
# what the Multiverse build tooling enforces. Download filename and port identity
# are different things; do not "fix" one to match the other.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
PKG="$HERE/pkg"
CT_BIN="${1:-$REPO/ct}"
# Single source of truth for the version: the same header the binary reports.
CT_VER="$(sed -n 's/^#define CT_VERSION "\(.*\)"/\1/p' "$REPO/source/version.h")"
[ -n "$CT_VER" ] || { echo "!! could not read CT_VERSION from source/version.h" >&2; exit 1; }
OUT="${OUT:-$REPO/ct-$CT_VER.zip}"

die() { echo "!! $*" >&2; exit 1; }

[ -f "$CT_BIN" ]                              || die "ct binary not found at $CT_BIN (run portmaster/build.sh first, or pass the path)"
[ -f "$PKG/port.json" ]                       || die "missing $PKG/port.json"
[ -f "$PKG/Chrono Trigger.sh" ]               || die "missing launch script"
[ -f "$PKG/ct/font.ttf" ]                     || die "missing pkg/ct/font.ttf"
[ -d "$PKG/ct/libs.aarch64" ]                 || die "missing pkg/ct/libs.aarch64 (run ffmpeg-build.sh + copy the .so in)"
ls "$PKG/ct/libs.aarch64"/lib*.so* >/dev/null 2>&1 || die "no FFmpeg .so in pkg/ct/libs.aarch64"
[ -d "$PKG/ct/licenses" ]                     || die "missing pkg/ct/licenses"
# Declared by port.json and gameinfo.xml, so shipping without them leaves the port
# pointing at files that aren't there. Fail here rather than quietly omitting them.
[ -f "$PKG/ct/screenshot.png" ]               || die "missing pkg/ct/screenshot.png (capture one with CT_CAPTURE=1)"
[ -f "$PKG/gameinfo.xml" ]                    || die "missing pkg/gameinfo.xml"

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
cp "$PKG/ct/screenshot.png"          "$STAGE/ct/screenshot.png"
cp "$PKG/gameinfo.xml"               "$STAGE/gameinfo.xml"
chmod +x "$STAGE/ct/ct" "$STAGE/Chrono Trigger.sh"

# --- zip ----------------------------------------------------------------------
rm -f "$OUT"
( cd "$STAGE" && zip -r -X "$OUT" "Chrono Trigger.sh" "gameinfo.xml" "ct" >/dev/null )

echo ">> built $OUT ($(du -h "$OUT" | cut -f1))"
echo ">> contents:"
unzip -Z1 "$OUT" | grep -v '/$' | sed 's/^/   /'
echo ">> reminder: the user supplies libchrono.so + libc++_shared.so + assets/ (never shipped)."
