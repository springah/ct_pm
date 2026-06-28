# ct_nx → PortMaster — port scaffolding

WIP for the Linux/aarch64 (PortMaster) port. Kept out of `source/` so it does NOT
get swept into the Switch Makefile's `source/*.c` wildcard. See `~/Desktop/ct-portmaster.md`
for the full scope and `~/Desktop/ct-pm-shell.sh` to enter the builder.

## Files
- `os.h`       — platform abstraction (the only OS surface the shared code may call).
- `os_linux.c` — SDL2 + POSIX backend (loader/TLS/GL core real; input/env = TODO).
- `os_switch.c` — NOT YET WRITTEN: move the existing libnx calls out of `source/`
  into this backend so both platforms share one tree.

## The TLS finding (the one real unknown — SOLVED by reference)
The `.so` reads its stack canary at `tpidr_el0 + 0x28`. The Switch repoints `tpidr_el0`
(`source/util.c` `armSetTlsRw`); glibc forbids that. Fix in `os_linux.c`: a large host
`_Thread_local os_tls_pad[2<<12]` forces glibc to reserve enough static TLS that the
guest's tpidr-relative access stays valid (the gmloader-next "incantation"). Plus the
`__stack_chk_guard` data symbol already exported in `source/imports.c`.

## Refactor map — source/ call sites → os_* (do this when wiring it up)
| source file | current (libnx) | replace with |
|---|---|---|
| `so_util.c:60` | `svcMapProcessCodeMemory` + `virtmemFindCodeMemory`/reservation | `os_code_reserve()` |
| `so_util.c:95,462` | `svcSetProcessMemoryPermission(Perm_Rx/Rw)` | `os_code_protect(addr,size,exec)` |
| `so_util.c:465` | `svcUnmapProcessCodeMemory` + remove reservation | `os_code_free()` |
| `main.c:40` | `__libnx_initheap` + `svcSetHeapSize` | `os_heap_init()` (no-op on Linux) |
| `util.c:60` | `armSetTlsRw` (custom TLS block) | `os_tls_init()` (TLS-pad trick) |
| `main.c:107-128` | `eglGetDisplay(EGL_DEFAULT_DISPLAY)` + `nwindow*` + EGL | `os_gfx_init()` |
| `movie_player.c:172`, present path | `eglSwapBuffers` | `os_gfx_swap()` |
| `imports.c:702` | game's `eglGetProcAddress` import | route to `os_gl_get_proc()` |
| `main.c:279,291,502` | `PadState`/`padUpdate`/`padGetButtons` | `os_input_init/poll()` |
| `main.c:96,513` | `appletGetOperationMode`/`appletGetFocusState` | `os_is_focused()` + fixed res |
| paths `/switch/ct/`, `fsdev` | hard-coded | `os_data_dir()` (`$GAMEDIR`) |
| `setGetSystemLanguage` | libnx set:sys | `os_system_language()` |
| `mutexLock`, `svcSleepThread` | libnx | POSIX `pthread_mutex`, `os_sleep_ms()` |

## Loader specifics carried over from gmloader-next (reference)
- mmap one RW region for the whole PT_LOAD span + a PATCH_SZ trampoline arena
  (`MAP_PRIVATE|ANONYMOUS|POPULATE`), memcpy segments to relocated vaddr, then
  `mprotect` exec segments to `R+X` (+ `__builtin___clear_cache`). Our `so_util.c`
  is already structured this way (RW first, then perms) — only the primitives swap.
- aarch64 relocations: ct_nx already handles them for the Switch build; reuse as-is.

## MILESTONE #1 — "LOADS CLEAN" ✅ (2026-06-24)
`source/so_util.c` is now dual-target via `#ifdef __SWITCH__` (Switch path byte-
identical — `ct_nx.nro` rebuilt + verified; `#else` uses `os_code_*`). Harness
`harness_load.c` + `os_linux.c` built & run in the aarch64 builder against the REAL
libc++_shared.so + libchrono.so: both **mapped, relocated (incl. RELR), cross-resolved,
and mprotect'd RX** → "LOADS CLEAN". The mmap RW→mprotect RX dance works on glibc.
Rerun: `gcc -O2 -march=armv8-a -Iportmaster -Isource $(sdl2-config --cflags)
portmaster/harness_load.c source/so_util.c portmaster/os_linux.c $(sdl2-config --libs)
-o /tmp/h && /tmp/h portmaster/_so`  (put the two .so in `portmaster/_so/`, gitignored).

**Import surface = 281 unique unresolved in libchrono** (the shim work, by category):
- 135 rest — mostly plain libc/math/stdio → **resolve straight to glibc** (little/no shim).
- 99 GLES/EGL (`gl*`/`egl*`) → GL thunk table / `SDL_GL_GetProcAddress`.
- 24 `pthread_*` → glibc, via the existing bionic-ABI `*_fake` opaque wrappers.
- 13 `__*_chk` fortify → glibc provides these.
- 10 Android/JNI/AAsset/`__system_property` → `jni_fake` + `libc_shim`.
So the bulk (135 + 13) is nearly free on glibc; real work is the GL table + JNI/Android.

## MILESTONE #2 — "RESOLVES CLEAN" ✅ (2026-06-24)
Full Linux import table links + resolves **0 unresolved** for BOTH modules. The shim
trio `imports.c` / `libc_shim.c` / `jni_fake.c` now build on Linux via a tiny
`portmaster/switch_compat.h` (only the libnx symbols actually used: Mutex/mutexInit/
Lock/Unlock, RwLock+rwlock*, Semaphore+semaphore*, svcSleepThread, svcGetThreadId,
armGetSystemTick[Freq], swkbd* stubs, __errno, Result/R_*). `asset.c` + `opensles.c`
were already clean (POSIX + SDL2). Two genuine portability fixes: `<sys/stat.h>` added to
imports.c (mkdir), `sem_getvalue` guarded in libc_shim.c (.count is libnx-only).
All edits guarded by `#ifdef __SWITCH__`; **`ct_nx.nro` rebuilt + verified** unchanged.

`harness_stubs.c` = load-test-only placeholders for the app-layer fns the table points at
(prefs/gfx/movie/config/util) so it links without milestone-3 files. Build+run:
```
gcc -O2 -march=armv8-a -Iportmaster -Isource $(sdl2-config --cflags) \
  portmaster/harness_load.c portmaster/harness_stubs.c source/so_util.c \
  portmaster/os_linux.c source/imports.c source/libc_shim.c source/jni_fake.c \
  source/asset.c source/opensles.c $(sdl2-config --libs) -lGLESv2 -lEGL -lpthread -lm \
  -o /tmp/h2 && /tmp/h2 portmaster/_so
```

## MILESTONE #3 — "BOOTS + RUNS ENGINE" ✅ (2026-06-24)
A **real aarch64 Linux `ct` ELF** builds from the full tree and **runs the actual game
engine** headlessly (no crash). main.c/util.c/gfx.c/movie_player.c/error.c are now
dual-target (`#ifdef __SWITCH__`): heap→noop, EGL/nwindow→`os_gfx_*`, HID input→
`os_input_*` (stub for now), applet loop→`!quit`+`os_is_focused`, paths→chdir `os_data_dir`,
tls_setup_guard→`os_tls_init`, system font→bundled TTF fallback. New Linux units:
`movie_stub.c` (FMV deferred — needs runtime ffmpeg), `compat_libc.c` (strlcpy).
Switch `.nro` rebuilt + verified after every change.

Headless run (SDL offscreen + mesa swrast) reached, with DEBUG_LOG on:
`sysprop get(ro.arch)` → `JniHelper::setJavaVM` (JNI_OnLoad) → `cocos_android_app_init`
→ `sead: release version 17.11.1.A` (the game's SEAD engine!) → libpng texture decode →
game-data load. **Exit 124 (timeout, still running) — NOT a crash.** So loader/TLS/
relocations/shims/JNI/asset are all runtime-stable on glibc aarch64.

Run recipe (headless, software GL — SLOW, not visual):
```
cd <gamedir>; GAMEDIR=$PWD SDL_VIDEODRIVER=offscreen EGL_PLATFORM=surfaceless \
  SDL_AUDIODRIVER=dummy CT_WINDOWED=1 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe ./ct
```
Full build cmd: all source/*.c EXCEPT movie_player.c, + portmaster/{os_linux,movie_stub,
compat_libc}.c, link `$(sdl2-config --libs) -lGLESv2 -lEGL $(pkg-config --libs freetype2)
-lpthread -lm -ldl`. ⚠️ gotcha found+fixed: Linux `egl_init()` must return `==0` (os_gfx_init
is 0-on-success; egl_init is 1-on-success) — got it backwards first and aborted despite OK GL.

## Build order
0. `~/Desktop/ct-pm-shell.sh`
1. [done] loader (#1).  2. [done] imports resolve (#2).  3. [done] boots + runs engine (#3).
4. **On-device bring-up (needs the TrimUI — real GPU + full data):**
   - Confirm title-screen renders (software GL is too slow to verify visually).
   - `common.bin` / archive data-path: engine fopen's `<gamedir>/common.bin` (probe) and
     falls back to resources.bin archive reader — verify asset.c archive path serves it.
   - Controller input: implement `os_input_poll` mapping (SDL_GameController → OS_BTN_*),
     then revive the e_ctrl*/e_key* injection on Linux (currently stubbed `update_keys`).
   - Bundle a TTF font (gfx.c fallback list); FMV via runtime ffmpeg (replace movie_stub).
5. PortMaster packaging: `ct.sh`, `port.json`, gptokeyb fallback, `$GAMEDIR` layout.

## Compile check
`os_linux.c` syntax-checked in the builder (SDL2 2.32, glibc 2.31). It is a skeleton —
not yet linked against the rest; input/env TODOs marked inline.

## FMV / cutscenes — WIRED (2026-06-24)
The intro + story cutscenes (`assets/001.dat`..`008.dat`, `007-en.dat`) are
XOR-obfuscated MP4 (H.264 720x486 + AAC 48kHz). `source/movie_player.c` (the proven
Switch path) is now compiled on Linux too — `portmaster/movie_stub.c` is only for
quick boot-to-title builds without ffmpeg.

Platform deltas vs the Switch build (all in `movie_player.c`, behind `#ifdef __SWITCH__`):
- present: `os_gfx_swap()` instead of `eglSwapBuffers` (already there).
- skip input: a Linux `skip_pressed()` polls `os_input_poll()` (A/B/Start/Select),
  edge-detected so a button held from the launching menu can't insta-skip, and it
  honours the quit combo. Polling each spin also pumps SDL events so the OS doesn't
  flag the blocking movie as hung.
- audio: `opensles_movie_*` is pure SDL (cross-platform) — unchanged.

### Bundled FFmpeg (decode-only, LGPL)
The device (Knulli) ships FFmpeg 7.x, but for portability across PortMaster devices
we **bundle** a minimal decode-only FFmpeg 7.1.1 in `pkg/ct/libs.aarch64/` and load it via
the launcher's `LD_LIBRARY_PATH=$GAMEDIR/libs.${DEVICE_ARCH}` (PortMaster sets
DEVICE_ARCH=aarch64; the per-arch `libs.<arch>` folder is the PortMaster convention).
Built with `portmaster/ffmpeg-build.sh`
(mov demux + h264/hevc/aac decode + swscale + swresample; ~3.8 MB, sonames .61/.59/.8/.5).
The only external NEEDED are libc/libm/libpthread/ld-linux (present everywhere).
Note: enabling **hevc** is required even though cutscenes are h264 — h264's SEI parser
(`h2645_sei.o`) references `ff_aom_uninit_film_grain_params`, only defined when the
HEVC decoder/SEI is compiled (an FFmpeg minimal-build dep gap).

LGPL: unmodified, dynamically linked. `pkg/ct/licenses/FFmpeg-COPYING.LGPLv2.1.txt`
+ the committed build script (`ffmpeg-build.sh`) satisfy the relink/source obligation.

### Build (with FMV)
`portmaster/build.sh` compiles `source/movie_player.c` (NOT movie_stub) and links
`-lavformat -lavcodec -lswscale -lswresample -lavutil` from `$FFPREFIX`. Run it in the
builder with the ffmpeg prefix mounted at `/ff`. Then copy the 5 `.so.*` from the
ffmpeg build into `pkg/ct/libs.aarch64/` named by soname (these are gitignored build artifacts).

### Packaging (PortMaster zip)
`portmaster/package.sh [ct-binary]` assembles a PortMaster-installable `ct.zip` from
`pkg/` + the built `ct` (whitelist staging, so the user's commercial files and dev cruft
can't leak). Zip extracts to `/roms/ports/`: `Chrono Trigger.sh` + `ct/{port.json, ct,
font.ttf, screenshot.png, libs.aarch64/, licenses/}`. port.json is `version 2`, `name
"ct.zip"`, `rtr false`, `runtime null`, `arch ["aarch64"]` (the Half-Life template — the
canonical native, user-supplied-commercial-files port). This is the self-host / on-device
zip; an OFFICIAL PortMaster submission is built by `tools/build_release.py` in a
PortMaster-New fork from the repo-layout tree (metadata + README.md + gameinfo.xml at the
port top level) and needs a 4:3 gameplay screenshot + multi-CFW testing — see the packaging
gap-analysis. Launcher uses the framework helpers `get_controls`, `pm_platform_helper`,
`pm_finish` (all provided by control.txt/funcs.txt/mod_<cfw>.txt).

### Verification (2026-06-24)
`portmaster/movie_probe.c` — a headless decode smoke test (no display, no engine):
de-obfuscates a real `.dat`, demuxes + decodes video & audio frames through the bundled
ffmpeg. On the TrimUI it decoded the intro (`001.dat`, 158.4s, h264 720x486 + aac 48k):
90 video + 143 audio frames, `PROBE_OK`. Confirms de-obfuscation, mov demux, H.264 +
AAC decode, swscale->RGBA and the new swr channel-layout API all work on-device. The GL
blit (same quad as Switch) + on-screen playback still want a real visual test.

## On-screen keyboard — DONE (2026-06-25)
PortMaster handhelds have no system IME, so name entry (cocos TextField + ui::EditBox,
both routed through the libnx `swkbd*` surface) used to fall back to default names.
`portmaster/osk.c` now implements `swkbd*` on Linux as a real controller-driven keyboard
drawn over the GL surface: full QWERTY + digits + shift/space/del/OK, d-pad/stick nav,
A=type B=backspace Y=shift Start=OK Select=cancel. Labels are rendered with
`gfx_render_text_rgba` (the bundled ChronoType font) so it matches the game. GL state is
handed back to cocos via `osk_set_gl_invalidate(e_glInvalidate)` (wired in main.c beside the
movie one), exactly like movie_player. `swkbd*` moved from inline stubs in switch_compat.h
to real functions in osk.c. ⚠️ **GL_UNPACK_ALIGNMENT**: the engine leaves it non-1, so the
RGBA glyph uploads must `glPixelStorei(GL_UNPACK_ALIGNMENT,1)` or odd-width glyphs shear
diagonally (even-width ones look fine — the giveaway). Verified on the TrimUI:
`CT_OSK_TEST=1` pops the keyboard at boot; an interactive pass returned `rc=0 name='Claude'`.

## Screenshot (port.json image)
`pkg/ct/screenshot.png` is referenced by port.json. It's generated from the user's own game,
not committed (the repo ships no game art — same stance as the .so/assets): boot with
`CT_CAPTURE=1` (writes frame_NNNNN.ppm + latest.ppm every 300 frames via os_gfx_capture),
then capture an **in-game (field/battle)** frame — PortMaster requires a **4:3,
≥640×480 gameplay** screenshot (NOT the 16:9 title). e.g.
`Image.open('latest.ppm').resize((640,480)).save('screenshot.png')` from a gameplay frame.
Ships in the zip (assembled by `portmaster/package.sh`). ⚠️ the current screenshot is the
old 16:9 640x360 title shot and must be re-captured before any official submission.

## Backlog / next time (codebase-review findings, 2026-06-28)
Health items surfaced in a code review — not blockers, "do when convenient":

- **CI (started).** `.github/workflows/c-compile.yml` compiles the Linux target
  to .o on an arm64 runner (movie_stub config — no proprietary .so, no ffmpeg),
  catching header/`#ifdef __SWITCH__` `#else`-branch regressions. `lint.yml`
  runs shellcheck (`-S error`) + `py_compile` on the pixeldemaster tools. Gaps
  to close later: (a) the Switch branches aren't covered here — they're built in
  ct_nx; if a Makefile returns, add a `devkitpro/devkita64` job. (b) the
  ffmpeg-linked `movie_player.c` is only validated in the builder image (distro
  ffmpeg version skew) — not in CI. (c) shellcheck is `-S error` to stay green;
  tighten to `warning` once the scripts are clean.
- **Automated tests.** Only manual smoke probes today (`movie_probe.c`, the
  load/resolve harnesses). They need the real `.so` + assets, so they can't run
  on a hosted runner — candidate for a self-hosted device runner (the TrimUI).
- **Finish the OS-abstraction symmetry.** The Linux side goes through `os_*`
  (`os.h`/`os_linux.c`); the Switch side is still inline libnx under
  `#ifdef __SWITCH__`. Writing `os_switch.c` (already flagged "NOT YET WRITTEN"
  at the top of this file) would let both targets share one shape.
- **Split the monolithic units.** `jni_fake.c` (~1055), `main.c` / `imports.c`
  (~847 each), `opensles.c` (~749). `main.c` especially mixes entry point, crash
  handler, heap init and the lifecycle loop — those could be separate files.
  (Shim switchboards like `jni_fake.c` are inherently large — lower priority.)
- **Screenshot** re-capture (4:3 ≥640×480 gameplay) — see the section above and
  `multiverse/`. Still the one hard gate on an official PortMaster submission.
