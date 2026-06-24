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
