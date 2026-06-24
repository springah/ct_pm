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
> **TrimUI Smart Pro** (Allwinner A133 / Mali-G31 / Knulli). See `portmaster/NOTES.md`.

## You supply the game

No game assets or original code are included. From your own legally-owned APK (v2.1.5):
* `lib/arm64-v8a/libchrono.so` — the engine
* `lib/arm64-v8a/libc++_shared.so` — its C++ runtime
* the entire `assets/` folder (`resources.bin`, `001.dat`…`008.dat`, `007-en.dat`,
  `Shaders/`, `build_date.txt`)

Place them in the port's game directory (`.../ports/ct/`) alongside the `ct` binary.

## Build (PortMaster / aarch64 Linux)

Built in the PortMaster aarch64 builder image (glibc 2.31 for broad device compatibility):

```sh
IMG=ghcr.io/monkeyx-net/portmaster-build-templates/portmaster-builder:aarch64-latest
docker run --rm --platform=linux/arm64 -v "$PWD":/workspace -w /workspace "$IMG" bash -lc '
  CF="-O2 -march=armv8-a -Iportmaster -Isource $(sdl2-config --cflags) $(pkg-config --cflags freetype2)"
  SRC="portmaster/os_linux.c portmaster/movie_stub.c portmaster/compat_libc.c \
       source/asset.c source/config.c source/error.c source/gfx.c source/imports.c \
       source/jni_fake.c source/libc_shim.c source/main.c source/opensles.c \
       source/prefs.c source/so_util.c source/util.c"
  gcc $CF $SRC $(sdl2-config --libs) $(pkg-config --libs freetype2) \
      -lGLESv2 -lEGL -lpthread -lm -ldl -o ct'
```

`source/movie_player.c` (FMV) is replaced by `portmaster/movie_stub.c` on Linux until the
runtime ffmpeg path is wired. See `portmaster/NOTES.md` for the full architecture,
milestone history, and the `os_*` abstraction map.

## Build (Nintendo Switch)

Unchanged from `ct_nx` — devkitA64 + libnx (`make`). The `#ifdef __SWITCH__` paths are
byte-identical to the original; the `.nro` still builds.

## Configuration

`config.txt` (created on first run): `screen_width`/`screen_height` (`-1` = panel default),
`language` (`en fr de it es ja ko zh`). Launcher env: `CT_FONT_SCALE` tunes the UI font size.

## Credits

* **fgsfds** — [max_nx](https://github.com/fgsfdsfgs/max_nx), the loader this is based on
* **TheOfficialFloW** — the original Vita ports that pioneered the technique
* **NaGaa95** — `ct_nx`, the Switch port this derives from
* **JohnnyonFlame** — gmloader-next, reference for the Linux ELF-loader + glibc TLS handling
* **Caveras** — the *ChronoType* SNES font recreation (CC BY-NC-SA; see `portmaster/pkg/ct/font-license.txt`)

## Legal

No affiliation with Square Enix. "Chrono Trigger" is a trademark of its owner. No game
assets or original program code are included; users must supply their own legally-owned
copy. Source under the MIT License (see `LICENSE`); bundled font under its own license.
