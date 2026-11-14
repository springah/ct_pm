# TODO — 1.0.0-beta.10 (first public release)

Target: **TrimUI Smart Pro** (Knulli, 1280×720). Everything else is best-effort.
Items are grouped by what blocks the release, what should ship with it, and what is
explicitly deferred. Tick as you go; delete the file (or rename to CHANGELOG notes) at tag time.

## Release gate (must be done before tagging)

- [ ] **Bump `CT_VERSION`** in `source/version.h` to `1.0.0-beta.10` (package.sh reads it;
      the first line of `log.txt` reports it).
- [ ] **Fresh-install test on the TrimUI** from the packaged zip, not a dev tree:
      extract `ct-1.0.0-beta.10.zip` into `ports/`, copy the APK files in, first launch
      writes `config.txt`, title → new game → first field → first battle → save → quit
      with Start+Select → relaunch → load. Check `log.txt` starts with the version line.
- [ ] **Upgrade test from beta.9**: keep an existing `ports/ct/` (config, saves,
      `shadercache/`), drop the new binary in, confirm `config.txt` gains any new keys
      (now logged under `CT_LOG=1`) and saves still load.
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

- [ ] **Linux `sysconf` answers** (`source/libc_shim.c`, `_SC_NPROCESSORS_*` and
      `_SC_PHYS_PAGES`): hard-coded to 3 cores / 3 GB from the Switch. On Linux return
      `sysconf(_SC_NPROCESSORS_ONLN)` and `sysconf(_SC_PHYS_PAGES)`. Check whether the
      engine sizes any thread pool from it before and after (CT_LOG, thread count in
      `/proc/<pid>/status`).
- [ ] **`getDeviceModel`** (`source/jni_fake.c`) returns `"Nintendo Switch"`. Return the
      SDL/`uname` machine string or `"PortMaster"` on Linux; grep the engine's text requests
      under `CT_LOG=1` to make sure nothing branches on it.
- [ ] **Controller name** (`source/main.c`, `"Nintendo Switch Controller"`): pass the
      SDL controller name through on Linux, or a neutral constant.
- [ ] **`CT_MOVELOG` hook** is installed whenever the v2.1.5 fingerprint matches, even
      with the env var unset (`source/main.c`). Gate the hook on the variable so release
      builds don't patch `NanameHantei` for nothing.
- [ ] **Launcher comment hygiene** (`portmaster/pkg/Chrono Trigger.sh`): the zram block
      sizes 768 MB for a 1 GB device. Fine for the TrimUI; add a note or a
      `CT_ZRAM_MB` override before anyone runs it on a 2 GB device.
- [ ] **Decide on the dev harnesses** (`portmaster/harness_load.c`, `harness_stubs.c`,
      `movie_probe.c`, `ctmem_watch.sh`, `ctleak_watch.sh`): move under
      `portmaster/dev/` with a two-line README, or drop them. They are not built by
      `build.sh` or CI today.
- [ ] **CI link check**: `c-compile.yml` only compiles to `.o`. Add a step that links the
      Linux target against stub `libchrono`/`libc++_shared` symbols (or at least links the
      `movie_stub.c` variant) so a missing entry point like the one `movie_stub.c` had is
      caught. Could also run `-Wall -Wextra` and fail on new warnings.
- [ ] **Font fallback face on Knulli**: the launcher copies a system font to
      `fonts/standard.ttf` for CJK. Verify which candidate actually exists on current
      Knulli (DroidSansFallbackFull vs Noto) and that `ja`/`zh` menus render.

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
- The game must be the Android **v2.1.5** build; other versions boot with every patch
  disabled and the stock mobile UI (the log says so).
