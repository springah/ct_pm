# TODO — 1.0.0-beta.10 (first public release)

Target: **TrimUI Smart Pro** (Knulli, 1280×720). Everything else is best-effort.
Items are grouped by what blocks the release, what should ship with it, and what is
explicitly deferred. Tick as you go; delete the file (or rename to CHANGELOG notes) at tag time.

## Release gate (must be done before tagging)

- [x] **Bump `CT_VERSION`** in `source/version.h` to `1.0.0-beta.10` (package.sh reads it;
      the first line of `log.txt` reports it).
- [ ] **Fresh-install test on the TrimUI** from the packaged zip, not a dev tree:
      extract `ct-1.0.0-beta.10.zip` into `ports/`, copy the APK files in, first launch
      writes `config.txt`, title → new game → first field → first battle → save → quit
      with Start+Select → relaunch → load. Check `log.txt` starts with the version line.
- [ ] **Upgrade test from beta.9**: keep an existing `ports/ct/` (config, saves,
      `shadercache/`), drop the new binary in, confirm `config.txt` gains any new keys
      (now logged under `CT_LOG=1`) and saves still load.
      *2026-11-14, first beta.10 build on the TrimUI: `config.txt` went from 9 to 29 keys
      with the old values kept, and a save was written. Repeat on the final binary.*
- [ ] **Missing-files path**: launch with `libchrono.so` absent and confirm the framework
      dialog shows the message, not a silent return to the menu.
- [ ] **Crash path**: confirm a non-zero exit shows the "exited unexpectedly" dialog and
      that `log.txt` has the crash handler's register dump (`portmaster/crash.c`).
- [ ] **Full-length cutscene**: let the intro FMV (`001.dat`, ~158 s) play to the end once
      and skip it once; confirm the title shows and audio returns to the engine.
- [ ] **Name entry**: the on-screen keyboard (`portmaster/osk.c`) on a real new game;
      confirm the entered name lands in the save.
- [ ] **Battery / thermals sanity**: 30 min of play with the default governor settings in
      the launcher (`schedutil`, 1.0 GHz floor). Note the drain for the release notes.
- [ ] **Shader cache cold vs warm**: delete `shadercache/`, time a few scene transitions,
      then repeat warm. The README promises "noticeably smoother"; confirm it is still true
      on the current Knulli build.
- [ ] **Write release notes** (GitHub release body): supported device, what the user
      supplies, the quit combo, known issues (below), and that 4:3 is unsupported.
- [ ] **Tag + Release**: `git tag v1.0.0-beta.10`, attach `ct-1.0.0-beta.10.zip`. Build
      in the pinned builder image (glibc 2.31, ffmpeg 7.1) via `portmaster/build.sh`.

## Should ship in beta.10 (small, low risk)

- [x] **Linux `sysconf` answers** (`source/libc_shim.c`): returns the device's own core
      count and RAM on Linux. Only `libc++_shared.so` imports `sysconf`
      (`std::thread::hardware_concurrency`); `libchrono.so` never calls it. Compare the
      thread count in `/proc/<pid>/status` on the TrimUI against beta.9 during the playtest.
- [x] **`getDeviceModel`** (`source/jni_fake.c`): returns `"PortMaster"` on Linux. The
      engine never asks for it (the name is not in `libchrono.so`).
- [x] **Controller name** (`source/main.c`): `"Gamepad"` on Linux. No controller-name
      strings exist in `libchrono.so`, so nothing branches on it.
- [x] **`CT_MOVELOG` hook**: already gated. `movelog_install` returns before touching
      `NanameHantei` unless the variable is set.
- [x] **Launcher zram size**: `CT_ZRAM_MB` overrides the 768 MB default (`0` = no zram).
- [x] **Dev harnesses**: moved to `portmaster/dev/` with a README.
- [x] **CI link check**: `c-compile.yml` now links both variants (the real FMV player and
      `movie_stub.c`). `-Wall` is not enabled: `source/main.c` has three unused Switch-side
      symbols that would need guarding first.
- [x] **Upstream keypad fix** (ct_nx `5883b63`): `remove_mobile_ui` no longer hides the
      passcode / cursor-colour container (`VirtualPad::openVPad +0x4C`). Not reproduced on
      the TrimUI; check the passcode puzzle if a save reaches it.
- [ ] **Font fallback face on Knulli**: `DroidSansFallbackFull.ttf` exists on current
      Knulli and is what the launcher copies to `fonts/standard.ttf`. A headless run with
      `language ja` loads it and serves the Japanese text requests. Still to do: look at
      `ja` / `zh` menus on the device.
- [ ] **Title says "Touch to Start"**: the engine's own string, same in beta.9. The
      optional controller-wording patch fixes it (README, *Controller wording*; verified
      headless: the title reads "Press to Start"). Decide whether the release notes
      should recommend it.

## Decisions to make

- [x] **4:3 code paths in the release build.** Decided: stripped. The narrow-panel rules in
      `ct_framing_resolve`, the `narrow` font profile in `source/gfx.c`, `font-4x3.ttf` and
      its licence are gone (commit history has them: `git log -S narrow -- source/gfx.c`).
      4:3 panels now run the 16:9 auto rules, unsupported.
- [x] **Screenshot aspect.** Decided: keep the 1280×720 gameplay shot; it matches the
      supported device.
- [ ] **Multiverse submission timing**: beta.10 self-hosted first, submit after one round
      of outside feedback, or submit beta.10 directly? `portmaster/multiverse/README.md`
      has the checklist either way.

## Deferred past beta.10 (noted, not scheduled)

- **4:3 framing rework from scratch.** The removed narrow-panel path (fractional
      `design_scale` 1.333, 2 px field, PixelMplus10 at 2×, letterbox bars; see the commit
      that stripped it) did not hold up well on the RG40XX-H / RG35XX-SP. Treat it as a
      reference only: the next attempt should start from what the engine's 4:3 table entry
      actually wants and decide field zoom, UI scale and font as one design, not three
      independent auto rules. Not part of this release.
- **Palette texture cache** (ct_nx `011bd89`, `source/palette_cache.c`): ported on the
      `palette-cache` branch as `cache_palettes` (default 0). The engine uploads a new
      palette texture per sprite per frame; the cache shares them. Still to do before it
      merges: play field, world map and an effect-heavy battle on the TrimUI with
      `cache_palettes 1`, look for wrong colours, and compare frame pacing with it off.
- **Runtime `.ctp` mod loader** (ct_nx's `source/modpack.c`): would let users drop packs
      into a `mods/` folder instead of repacking `resources.bin` offline. Not ported.
- **Other CFWs** (muOS, ArkOS, ROCKNIX, AmberELEC): untested. The untested surface is
      `$DEVICE_ARCH` from older `control.txt`, the host SDL2/freetype/GLES libraries, and
      the zram/cpufreq block in the launcher. Needs testers.
- **Other 16:9 devices** (RG556, Odin, etc.): should match the TrimUI framing; 1080p
      panels take `design_scale 3` by the auto rule but nobody has looked at one.
- **`__SWITCH__` branches** (55 regions in 15 files): kept so `source/` stays mergeable
      with ct_nx, which is maintained separately. Do not remove without talking to that
      project; do not add new ones.
- **Glyph size shrink in `gfx.c`** (`rpx = px - (px/8 + 1)`): inherited from the Switch
      shared-font tuning and kept because the pixel-font snapping decides the final size
      anyway. Revisit if a non-pixel `font.ttf` is ever supported.
- **Automated on-device tests**: the smoke probes need the real `.so` and assets, so
      they can only run on a self-hosted runner (the TrimUI). Worth it once there is a
      second device.
- **Split `jni_fake.c` / `imports.c` / `main.c`** (~1000 lines each): the next easy carve
      from `main.c` is the JNI entry-point resolve/bind table.

## Known issues to list in the release notes

- No in-game exit; quit is Start+Select (or MENU+Start). Powering off mid-game can lose
  the save.
- First session compiles shaders; expect stutter on scene changes until `shadercache/`
  is warm.
- 4:3 panels: not supported; they run the 16:9 rules and look small.
- Some text still refers to touch ("Touch to Start", a few tutorial lines); the optional
  controller-wording patch in the README rewrites it.
- The game must be the Android **v2.1.5** build; other versions boot with every patch
  disabled and the stock mobile UI (the log says so).
