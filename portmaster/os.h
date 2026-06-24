// os.h — platform abstraction for the ct_nx loader.
//
// Goal: the loader/engine-glue code in source/ (so_util, main, util, gfx,
// opensles, movie_player) should call ONLY these os_* entry points for anything
// OS-specific. Two backends implement them:
//   * os_switch.c  — wraps libnx   (refactor of the current source/*.c calls)
//   * os_linux.c   — wraps SDL2 + POSIX (the PortMaster aarch64 target)
//
// Everything else (ELF parse/relocate, bionic shims, JNI fake, ffmpeg) is
// already OS-agnostic and stays shared.
//
// Reference for the Linux backend: JohnnyonFlame/gmloader-next (GPL-3 — read,
// don't copy; reimplement here under ct_nx's MIT).
#ifndef CT_OS_H
#define CT_OS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Code memory — how the Android .so's PT_LOAD segments get mapped executable.
//
// Switch: virtmemFindCodeMemory + svcMapProcessCodeMemory, then
//         svcSetProcessMemoryPermission(Perm_Rx/Perm_Rw).  (W->X is forbidden,
//         so RX is set straight from the freshly-mapped state — see so_util.c.)
// Linux:  mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE),
//         then mprotect(PROT_READ|PROT_EXEC) per executable segment.
//
// os_code_reserve() returns a writable mapping of `size` bytes (aligned to
// `align`) into which the caller memcpy's segments; later it flips each segment
// to its final perms with os_code_protect(); os_code_free() tears it down.
// ---------------------------------------------------------------------------
typedef struct os_code_map {
    void  *base;     // start of the writable mapping
    size_t size;     // full mapped size
    void  *opaque;   // backend bookkeeping (Switch: virtmem reservation handle)
} os_code_map;

int  os_code_reserve(os_code_map *m, size_t size, size_t align);
int  os_code_protect(void *addr, size_t size, bool exec); // exec? R+X : R+W
void os_code_free(os_code_map *m);

// ---------------------------------------------------------------------------
// Heap — the engine wants the bulk of RAM (textures/render targets).
// Switch: __libnx_initheap + svcSetHeapSize carve a big newlib heap.
// Linux:  no-op; glibc malloc + the kernel handle it. (RAM budget is the real
//         constraint on the 1 GB TrimUI — render < 720p / cap texture budget.)
// ---------------------------------------------------------------------------
void os_heap_init(void);

// ---------------------------------------------------------------------------
// TLS / stack-guard — the .so reads its stack canary from tpidr_el0 + 0x28.
// Switch: armSetTlsRw() repoints tpidr_el0 at a custom block (util.c).
// Linux:  CANNOT hijack tpidr_el0 (glibc owns the TCB). Instead a large host
//         thread_local forces glibc to reserve enough static TLS so the guest's
//         tpidr-relative access stays in valid memory; os_tls_init() seeds the
//         canary value. __stack_chk_guard is also exported as a data symbol via
//         imports.c. Call once per thread that will run guest code.
// ---------------------------------------------------------------------------
void os_tls_init(void);

// ---------------------------------------------------------------------------
// Graphics — a current GLES2 context + buffer swap. cocos2d-x only ever asks
// the loader for eglGetProcAddress, so os_gl_get_proc() is what the game's
// imported eglGetProcAddress is wired to.
// Switch: nwindowGetDefault + EGL on EGL_DEFAULT_DISPLAY + eglSwapBuffers.
// Linux:  SDL_CreateWindow(SDL_WINDOW_OPENGL) + SDL_GL_CreateContext +
//         SDL_GL_SwapWindow + SDL_GL_GetProcAddress.
// req_w/req_h may be -1 to mean "native"; out_w/out_h get the actual size.
// ---------------------------------------------------------------------------
int   os_gfx_init(int req_w, int req_h, int *out_w, int *out_h);
void  os_gfx_swap(void);
void *os_gl_get_proc(const char *name);
void  os_gfx_shutdown(void);
// debug: glReadPixels the current back buffer to a binary PPM (P6). Used to grab
// a real frame off the device GPU for inspection. No-op if no context.
void  os_gfx_capture(const char *path);

// ---------------------------------------------------------------------------
// Input — polled once per frame into a flat state the engine-injection layer
// reads. Button bits are the loader's own enum (below); the backend maps its
// native source onto them. Switch: PadState/padUpdate. Linux: SDL_GameController.
// ---------------------------------------------------------------------------
enum {
    OS_BTN_A = 1<<0,  OS_BTN_B = 1<<1,  OS_BTN_X = 1<<2,  OS_BTN_Y = 1<<3,
    OS_BTN_L = 1<<4,  OS_BTN_R = 1<<5,  OS_BTN_ZL = 1<<6, OS_BTN_ZR = 1<<7,
    OS_BTN_UP = 1<<8, OS_BTN_DOWN = 1<<9, OS_BTN_LEFT = 1<<10, OS_BTN_RIGHT = 1<<11,
    OS_BTN_START = 1<<12, OS_BTN_SELECT = 1<<13,
};
typedef struct os_input_state {
    uint32_t buttons;          // OS_BTN_* bitfield
    float    lx, ly, rx, ry;   // analog sticks, -1..1
    bool     touch;            // touchscreen pressed (TSP has one; optional)
    int      touch_x, touch_y; // touch position in render pixels
    bool     quit;             // window close / quit requested
} os_input_state;

void os_input_init(void);
void os_input_poll(os_input_state *st);

// ---------------------------------------------------------------------------
// Filesystem / environment.
// os_data_dir(): where libchrono.so + assets/ + config live.
//   Switch: "/switch/ct/".  Linux: $GAMEDIR (PortMaster) else cwd.
// os_system_language(): 2-letter code; config can override (en/ja/...).
// os_is_focused(): Switch applet focus; Linux SDL window focus (skip rendering
//   when false to save battery).
// ---------------------------------------------------------------------------
const char *os_data_dir(void);
const char *os_system_language(void);
bool        os_is_focused(void);

// ---------------------------------------------------------------------------
// Misc. Threads/mutexes are NOT abstracted: use POSIX (pthread/usleep) directly
// in shared code — both newlib (devkitA64) and glibc provide them. The Switch
// `mutexLock`/`svcSleepThread` call sites become pthread_mutex/usleep.
// ---------------------------------------------------------------------------
void os_sleep_ms(unsigned ms);
void os_log(const char *fmt, ...);   // debugPrintf on Switch, stderr on Linux

#ifdef __cplusplus
}
#endif
#endif // CT_OS_H
