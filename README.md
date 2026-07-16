<div align=center>

<img src="extras/banner.png" alt="Banner" width="40%">

</div>
<h1 align=center>Chrono Trigger — PortMaster (aarch64 Linux) port</h1>

A loader/port of the Android version of *Chrono Trigger*
(`com.square_enix.android_googleplay.chrono`, v2.1.5) for **PortMaster** handhelds
(aarch64 Linux: muOS / Knulli / ArkOS / ROCKNIX / AmberELEC …). It maps the original
Android `arm64-v8a` `.so` files, shims their imports to native libraries, and runs the
game in a minimal emulated-Android environment via **SDL2 + GLES2**.

This is a sibling of [`ct_nx`](https://github.com/springah/ct_nx) (the Nintendo Switch
port). The codebase is **dual-target**: the OS layer is behind `#ifdef __SWITCH__`, so the
same `source/` builds for both Switch (libnx) and Linux (SDL2/POSIX, in `portmaster/`).

> Status: **playable** — boots, renders on the GPU, plays and saves. Verified on a
> **TrimUI Smart Pro** (Allwinner A133 / Mali-G31), **Anbernic RG40XX-V** and
> **RG35XX-SP** (Allwinner H700), all on Knulli. See `portmaster/NOTES.md`.

## You supply the game

No game assets or original code are included. From your own legally-owned APK (v2.1.5):
* `lib/arm64-v8a/libchrono.so` — the engine
* `lib/arm64-v8a/libc++_shared.so` — its C++ runtime
* the entire `assets/` folder (`resources.bin`, `001.dat`…`008.dat`, `007-en.dat`,
  `Shaders/`, `build_date.txt`)

Place them in the port's game directory (`.../ports/ct/`) alongside the `ct` binary.

## Build (PortMaster / aarch64 Linux)

Built in the PortMaster aarch64 builder image (glibc 2.31 for broad device compatibility).
First build the bundled decode-only FFmpeg once, then the game:

```sh
IMG=ghcr.io/monkeyx-net/portmaster-build-templates/portmaster-builder:aarch64-latest

# 1) minimal FFmpeg 7.1 (mov + h264/hevc/aac decode + swscale/swresample) -> $FF/{include,lib}
#    see portmaster/ffmpeg-build.sh (LGPL; ~3.8 MB of .so, sonames .61/.59/.8/.5)

# 2) the game, with the FMV player linked
docker run --rm --platform=linux/arm64 \
  -v "$PWD":/workspace -v "$FF":/ff -w /workspace "$IMG" bash portmaster/build.sh
```

`portmaster/build.sh` compiles `source/movie_player.c` (the real FMV player) and links the
bundled FFmpeg; the 5 `.so.*` are copied into `portmaster/pkg/ct/libs/` and found at runtime
via the launcher's `LD_LIBRARY_PATH`. For a quick boot-to-title build without ffmpeg, swap
in `portmaster/movie_stub.c`. See `portmaster/NOTES.md` for the full architecture, the FMV
section, milestone history, and the `os_*` abstraction map.

## Configuration

`config.txt` (created on first run):
* `screen_width` / `screen_height` — `-1` = panel default.
* `language` — in-game text/UI localization; the game ships all nine under
  `Localize/<code>/`. One of `en fr de it es ja ko zh` (`zh-Hant` / `zh_TW` = Traditional
  Chinese). Each code loads its own localized data; an unrecognized value falls back to
  English.
* `render_scale` — internal render resolution as a fraction of the panel: the engine
  renders into a `panel × scale` FBO that is upscaled at present. Defaults to **0.75**
  (≈960×540 on a 720p panel) to keep GPU-bound handhelds at full speed; set `1` for
  native/off. See `source/rescale.c`.

Launcher env overrides: `CT_FONT_SCALE` (UI font size, default `1.5`), `CT_RENDER_SCALE`
(overrides `render_scale`), and `CT_TEXT_SHADOW` (`off` / `auto` / `dx,dy,opacity` — the
SNES-style 1px drop-shadow is baked in otherwise). The launcher also adds a temporary
zram swap and eases the CPU governor for the session (`CT_GOV` / `CT_MIN_KHZ`), restoring
both on exit.

## Pixel-art mods (optional)

The Android textures can be re-skinned with community pixel-art packs — e.g.
[Pixel Demaster](https://www.nexusmods.com/chronotrigger/mods/8) (by Shiryu) — to swap the
smoothed mobile art for the SNES-style look (and optionally SNES button prompts, UI colour
schemes, and icon sets). `tools/pixeldemaster/` repacks **your own** `resources.bin` with
**your own** downloaded mod; no assets are shipped — it only transcodes files you supply.
The `.ctp` patch format it reads comes from River Nyxx's [CT_Explore](https://rivernyxx.com).
See [`tools/pixeldemaster/README.md`](tools/pixeldemaster/README.md).

## Credits

* **fgsfds** — [max_nx](https://github.com/fgsfdsfgs/max_nx), the loader this is based on
* **TheOfficialFloW** — the original Vita ports that pioneered the technique
* **NaGaa95** — `ct_nx`, the Switch port this derives from
* **JohnnyonFlame** — gmloader-next, reference for the Linux ELF-loader + glibc TLS handling
* **Caveras** — the *ChronoType* SNES font recreation (CC BY-NC-SA; see `portmaster/pkg/ct/licenses/font-license.txt`)
* **FFmpeg** — the [FFmpeg project](https://ffmpeg.org) (LGPL v2.1); a minimal decode-only
  build is bundled for cutscene playback (`portmaster/ffmpeg-build.sh`, license in
  `portmaster/pkg/ct/licenses/`)

## Legal

No affiliation with Square Enix. "Chrono Trigger" is a trademark of its owner. No game
assets or original program code are included; users must supply their own legally-owned
copy. Source under the MIT License (see `LICENSE`); bundled font under its own license.
