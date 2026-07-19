// os_linux.c — SDL2 + POSIX backend of os.h for the PortMaster aarch64 target
// (TrimUI Smart Pro et al). Skeleton: the loader/TLS/GL core is real; input and
// a few env hooks are marked TODO. Reimplemented from the gmloader-next pattern
// (read-only reference, GPL-3) under ct_nx's MIT.
//
// Build inside the PortMaster builder:  ~/Desktop/ct-pm-shell.sh
//   gcc -O2 -march=armv8-a $(sdl2-config --cflags) ... -c portmaster/os_linux.c
#define _GNU_SOURCE
#include "os.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/mman.h>
#include <SDL2/SDL.h>
#include <GLES2/gl2.h>

#ifndef ALIGN_UP
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((size_t)(a) - 1))
#endif

// ===========================================================================
// TLS / stack-guard.
//
// The Android .so reads its stack-protector canary relative to tpidr_el0. On
// glibc we must NOT repoint tpidr_el0 (glibc owns the TCB there). Instead, a
// big host thread_local forces glibc to reserve a large static-TLS block, so
// the guest's tpidr-relative read/write lands in valid, owned memory. This is
// the gmloader-next "don't touch this incantation" trick — it is load-bearing.
// Keep it; do not let the optimizer drop it.
// ===========================================================================
_Thread_local volatile int os_tls_pad[2 << 12] = {0};
__attribute__((used)) int os_tls_touch(void) { return os_tls_pad[0]++; }

void os_tls_init(void) {
    // Touch the pad so the TU/thread actually instantiates its static TLS, then
    // seed the canary the .so expects. The matching __stack_chk_guard data
    // symbol is exported to the guest via imports.c.
    os_tls_touch();
    // value mirrors source/util.c (0x0123456789ABCDEF) — keep in sync.
}

// ===========================================================================
// Code memory: mmap RW, caller copies segments in, then mprotect RX per segment.
// Mirrors the Switch path in source/so_util.c but with POSIX primitives.
// ===========================================================================
int os_code_reserve(os_code_map *m, size_t size, size_t align) {
    size_t total = ALIGN_UP(size, align ? align : 0x1000);
    void *p = mmap(NULL, total, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (p == MAP_FAILED) {
        os_log("os_code_reserve: mmap %zu failed\n", total);
        return -1;
    }
    memset(p, 0, total);
    m->base = p;
    m->size = total;
    m->opaque = NULL;
    return 0;
}

int os_code_protect(void *addr, size_t size, bool exec) {
    // Align to page bounds — mprotect requires it.
    uintptr_t a = (uintptr_t)addr;
    uintptr_t start = a & ~(uintptr_t)0xFFF;
    size_t len = ALIGN_UP((a - start) + size, 0x1000);
    int prot = exec ? (PROT_READ | PROT_EXEC) : (PROT_READ | PROT_WRITE);
    if (mprotect((void *)start, len, prot) != 0) {
        os_log("os_code_protect(%p,%zu,exec=%d) failed\n", addr, size, exec);
        return -1;
    }
    if (exec) __builtin___clear_cache((char *)start, (char *)start + len);
    return 0;
}

void os_code_free(os_code_map *m) {
    if (m && m->base) munmap(m->base, m->size);
    if (m) { m->base = NULL; m->size = 0; m->opaque = NULL; }
}

void os_heap_init(void) { /* glibc handles it; nothing to carve */ }

// ===========================================================================
// Graphics: SDL2 window + GLES2 context.
// ===========================================================================
static SDL_Window   *s_win = NULL;
static SDL_GLContext s_ctx = NULL;
static int s_w = 0, s_h = 0;

int os_gfx_init(int req_w, int req_h, int *out_w, int *out_h) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        os_log("SDL video init: %s\n", SDL_GetError());
        return -1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // PortMaster handhelds run fullscreen at the panel's native size; allow a
    // windowed override (CT_WINDOWED=1) for desktop/headless testing.
    Uint32 flags = SDL_WINDOW_OPENGL;
    if (!getenv("CT_WINDOWED"))
      flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    int w = req_w > 0 ? req_w : 1280;
    int h = req_h > 0 ? req_h : 720;

    // Try a lighter 16-bit depth buffer first -- the 2D renderer's precision
    // needs nowhere near 24-bit, and trimming it saves a little framebuffer
    // bandwidth. Fall back to 24-bit if a driver only exposes a combined
    // 24-depth/8-stencil format, so a missing 16/8 combo never blocks boot.
    const int depth_bits[] = { 16, 24 };
    for (unsigned i = 0; i < 2; i++) {
      SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depth_bits[i]);
      s_win = SDL_CreateWindow("ct", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               w, h, flags);
      if (!s_win) { os_log("SDL_CreateWindow (depth %d): %s\n", depth_bits[i], SDL_GetError()); continue; }
      s_ctx = SDL_GL_CreateContext(s_win);
      if (s_ctx) break;
      os_log("SDL_GL_CreateContext (depth %d): %s\n", depth_bits[i], SDL_GetError());
      SDL_DestroyWindow(s_win); s_win = NULL;
    }
    if (!s_win || !s_ctx) { os_log("no GL context (tried 16- and 24-bit depth)\n"); return -1; }
    SDL_GL_MakeCurrent(s_win, s_ctx);
    SDL_GL_SetSwapInterval(1); // vsync

    SDL_GL_GetDrawableSize(s_win, &w, &h);
    s_w = w; s_h = h;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return 0;
}

// debug frame grab: read the back buffer and write a vertically-flipped PPM.
void os_gfx_capture(const char *path) {
    if (!s_ctx || s_w <= 0 || s_h <= 0) return;
    size_t n = (size_t)s_w * s_h * 4;
    unsigned char *px = (unsigned char *)malloc(n);
    if (!px) return;
    glReadPixels(0, 0, s_w, s_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", s_w, s_h);
        for (int y = s_h - 1; y >= 0; y--) {            // flip: GL origin is bottom-left
            const unsigned char *row = px + (size_t)y * s_w * 4;
            for (int x = 0; x < s_w; x++)
                fwrite(row + x * 4, 1, 3, f);            // RGB, drop alpha
        }
        fclose(f);
        os_log("os_gfx_capture: wrote %s (%dx%d)\n", path, s_w, s_h);
    }
    free(px);
}

void  os_gfx_swap(void)  { if (s_win) SDL_GL_SwapWindow(s_win); }
void *os_gl_get_proc(const char *name) { return SDL_GL_GetProcAddress(name); }
void  os_gfx_shutdown(void) {
    if (s_ctx) SDL_GL_DeleteContext(s_ctx);
    if (s_win) SDL_DestroyWindow(s_win);
    s_ctx = NULL; s_win = NULL;
}

// ===========================================================================
// Input — SDL_GameController.
//
// We open the first available game controller (PortMaster maps the handheld's
// built-in buttons to a standard SDL_GameController via gamecontrollerdb.txt /
// the SDL_GAMECONTROLLERCONFIG env it sets up). Hotplug is handled so a pad
// plugged in after launch still works. Every frame we poll the *level* state of
// each button/axis (the engine-injection layer in main.c does its own edge
// detection), and translate SDL's positional button names into OS_BTN_*.
//
// Face-button mapping note: SDL uses Xbox positional naming, so
// SDL_CONTROLLER_BUTTON_A is the BOTTOM face button and _B is the RIGHT one.
// The game uses a Nintendo layout (right button confirms, bottom cancels). To
// keep "A = confirm / B = cancel" matching the Switch build's *physical* layout,
// we map the RIGHT button (SDL _B) -> OS_BTN_A (confirm) and the BOTTOM button
// (SDL _A) -> OS_BTN_B (cancel). X/Y are likewise swapped to the Nintendo
// positions (SDL _Y top -> OS_BTN_X, SDL _X left -> OS_BTN_Y) so the on-screen
// glyphs line up with a Nintendo-style face cluster.
// ===========================================================================
static SDL_GameController *s_pad = NULL;
static SDL_JoystickID      s_pad_id = -1;

static const int OS_TRIGGER_THRESH = 8000; // ZL/ZR digital threshold (~0.24)
static const int OS_STICK_DEADZONE = 3900; // ~0.12 of 32768

// The TrimUI MENU/FN button is a separate evdev "sunxi-keyboard" device, not the
// gamepad SDL opens, so it arrives as SDL_KEYDOWN rather than a controller button.
// Track it so "menu + Start" can quit. Covers the likely keysyms a menu/fn/home
// key emits across these handhelds.
static int g_menu_key_down = 0;
static int os_is_menu_key(SDL_Keycode k) {
    return k == SDLK_MENU || k == SDLK_ESCAPE || k == SDLK_HOME ||
           k == SDLK_AC_HOME || k == SDLK_APPLICATION || k == SDLK_POWER ||
           k == SDLK_F1;
}

// Generic fallback mapping for a plain joystick whose GUID the framework's
// SDL_GAMECONTROLLERCONFIG doesn't cover. Covers the common handheld layout
// (face a:b0 b:b1 x:b2 y:b3, shoulders b4/b5, select/start b6/b7, hat dpad,
// sticks axes0-3, triggers axes4/5). Applied per-GUID so a correct framework
// mapping always wins (we only add when SDL_IsGameController was already false).
static void os_input_force_map(int i) {
    SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
    char gs[40] = {0};
    SDL_JoystickGetGUIDString(g, gs, sizeof(gs));
    const char *name = SDL_JoystickNameForIndex(i);
    char map[512];
    snprintf(map, sizeof(map),
        "%s,%s,a:b0,b:b1,x:b2,y:b3,back:b6,start:b7,guide:b8,"
        "leftshoulder:b4,rightshoulder:b5,leftstick:b9,rightstick:b10,"
        "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
        "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,",
        gs, name ? name : "ct pad");
    SDL_GameControllerAddMapping(map);
    os_log("input: added fallback mapping for GUID %s (%s)\n", gs, name ? name : "?");
}

static void os_input_open_first(void) {
    if (s_pad) return;
    int n = SDL_NumJoysticks();
    os_log("input: SDL sees %d joystick(s)\n", n);
    for (int i = 0; i < n; i++) {
        SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
        char gs[40] = {0}; SDL_JoystickGetGUIDString(g, gs, sizeof(gs));
        os_log("input:  [%d] '%s' GUID=%s isGC=%d\n", i,
               SDL_JoystickNameForIndex(i) ? SDL_JoystickNameForIndex(i) : "?",
               gs, SDL_IsGameController(i));
        if (!SDL_IsGameController(i))
            os_input_force_map(i);          // teach SDL this pad, then retry
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController *gc = SDL_GameControllerOpen(i);
        if (!gc) { os_log("input:  open failed: %s\n", SDL_GetError()); continue; }
        s_pad = gc;
        s_pad_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
        os_log("input: opened controller '%s'\n", SDL_GameControllerName(gc));
        break;
    }
    if (!s_pad) os_log("input: no usable controller opened\n");
}

void os_input_init(void) {
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK);
    os_input_open_first();
}

void os_input_poll(os_input_state *st) {
    memset(st, 0, sizeof(*st));

    // Drain the event queue: keep the window responsive, catch quit, and react
    // to controller hotplug. Button/axis state itself is sampled by level below.
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            st->quit = true;
            break;
        case SDL_KEYDOWN:
            if (os_is_menu_key(e.key.keysym.sym)) g_menu_key_down = 1;
            break;
        case SDL_KEYUP:
            if (os_is_menu_key(e.key.keysym.sym)) g_menu_key_down = 0;
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (!s_pad && SDL_IsGameController(e.cdevice.which)) {
                SDL_GameController *gc = SDL_GameControllerOpen(e.cdevice.which);
                if (gc) {
                    s_pad = gc;
                    s_pad_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
                    os_log("input: controller '%s' connected\n", SDL_GameControllerName(gc));
                }
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (s_pad && e.cdevice.which == s_pad_id) {
                SDL_GameControllerClose(s_pad);
                s_pad = NULL;
                s_pad_id = -1;
                os_log("input: controller disconnected\n");
                os_input_open_first(); // fall back to any other pad still attached
            }
            break;
        default:
            break;
        }
    }

    if (!s_pad)
        return;

    SDL_GameControllerUpdate();
    SDL_GameController *c = s_pad;

    uint32_t b = 0;
    // Nintendo-layout face buttons (see header note): swap SDL positions.
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B)) b |= OS_BTN_A; // right -> confirm
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A)) b |= OS_BTN_B; // bottom -> cancel
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y)) b |= OS_BTN_X; // top -> X
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X)) b |= OS_BTN_Y; // left -> Y

    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  b |= OS_BTN_L;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= OS_BTN_R;
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > OS_TRIGGER_THRESH) b |= OS_BTN_ZL;
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > OS_TRIGGER_THRESH) b |= OS_BTN_ZR;

    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) b |= OS_BTN_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))  b |= OS_BTN_SELECT;

    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    b |= OS_BTN_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  b |= OS_BTN_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  b |= OS_BTN_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= OS_BTN_RIGHT;

    // Left stick also drives the d-pad bits (matches the Switch StickL synthesis)
    // so menus can be navigated with the stick. main.c reads OS_BTN_* for nav and
    // the raw lx/ly/rx/ry for the engine's analog controller axes.
    int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    int ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    int rx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    int ry = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);
    if (lx < -OS_STICK_DEADZONE) b |= OS_BTN_LEFT;
    if (lx >  OS_STICK_DEADZONE) b |= OS_BTN_RIGHT;
    if (ly < -OS_STICK_DEADZONE) b |= OS_BTN_UP;
    if (ly >  OS_STICK_DEADZONE) b |= OS_BTN_DOWN;

    // -- CT_STICKLOG: left-stick Y diagnostic -------------------------------
    // The TrimUI pad (GUID 0300...02000000) is isGC=1 via the PortMaster
    // framework's gamecontrollerdb, so our fallback mapping is NOT applied and
    // the stick Y comes out wrong (down dead / reads as up). Set CT_STICKLOG=1
    // to dump the *mapped* LEFTX/LEFTY (post-gamecontrollerdb), the synthesised
    // d-pad bits, and EVERY raw joystick axis a0..aN. Wiggle the stick, capture
    // log.txt, and read off which physical axis the stick Y is really on and its
    // sign -> then override the GUID mapping. Throttled to fire only on movement.
    if (getenv("CT_STICKLOG")) {
        SDL_Joystick *j = SDL_GameControllerGetJoystick(c);
        int na = j ? SDL_JoystickNumAxes(j) : 0;
        if (na > 8) na = 8;
        int raw[8] = {0};
        for (int k = 0; k < na; k++) raw[k] = SDL_JoystickGetAxis(j, k);
        static int prev_raw[8], prev_lx, prev_ly, have_prev;
        int moved = !have_prev || abs(lx - prev_lx) > 1500 || abs(ly - prev_ly) > 1500;
        for (int k = 0; k < na && !moved; k++)
            if (abs(raw[k] - prev_raw[k]) > 1500) moved = 1;
        if (moved) {
            char rbuf[160]; int p = 0;
            for (int k = 0; k < na; k++)
                p += snprintf(rbuf + p, (size_t)(sizeof(rbuf) - p), " a%d=%6d", k, raw[k]);
            os_log("stick: mapLX=%6d mapLY=%6d dpad[%c%c%c%c]%s\n",
                   lx, ly,
                   (b & OS_BTN_UP)    ? 'U' : '.',
                   (b & OS_BTN_DOWN)  ? 'D' : '.',
                   (b & OS_BTN_LEFT)  ? 'L' : '.',
                   (b & OS_BTN_RIGHT) ? 'R' : '.',
                   rbuf);
            for (int k = 0; k < na; k++) prev_raw[k] = raw[k];
            prev_lx = lx; prev_ly = ly; have_prev = 1;
        }
    }

    // Quit hotkey (reliable, native — no gptokeyb grab): Start+Select (universal),
    // Start+Guide (if the menu button maps to the gamepad guide), or the hardware
    // MENU key (separate evdev) + Start. Any of these requests a clean quit.
    {
        int start  = (b & OS_BTN_START) != 0;
        int select = (b & OS_BTN_SELECT) != 0;
        int guide  = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_GUIDE);
        if (start && (select || guide || g_menu_key_down))
            st->quit = true;
    }

    st->buttons = b;
    // Normalise to -1..1. The engine's CC_JOY_LY convention is "up = negative":
    // the Switch path sends -l.y where Nintendo reports +y=up, i.e. UP -> -1.0.
    // SDL already reports up as a NEGATIVE Y, so we pass it through UN-negated to
    // match (UP -> -1.0). Do NOT negate Y here — doing so double-flips it, leaving
    // the analog axis fighting the (correct) stick-as-dpad bits so vertical cancels
    // out (left/right fine, up/down dead). X needs no flip: SDL & Nintendo agree.
    st->lx = lx / 32768.0f;
    st->ly = ly / 32768.0f;
    st->rx = rx / 32768.0f;
    st->ry = ry / 32768.0f;
}

// ===========================================================================
// Environment.
// ===========================================================================
const char *os_data_dir(void) {
    const char *g = getenv("GAMEDIR");   // set by PortMaster's launch script
    return (g && *g) ? g : ".";
}

const char *os_system_language(void) {
    // Resolve the system UI language onto one of the codes resources.bin ships
    // (ja/en/de/it/es/fr/zh/zh-Hant/ko); regional variants collapse onto their
    // base, and anything unsupported (nl/pt/ru/...) falls back to English --
    // this backs config.language == "default". config.txt still overrides it.
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LC_MESSAGES");
    if (!l || !*l) l = getenv("LANG");
    if (!l || strlen(l) < 2) return "en";

    // Split "ll_RR.ENC" / "ll-RR" into a 2-letter language + uppercase region.
    char lang[3] = { 0 }, region[8] = { 0 };
    lang[0] = (char)tolower((unsigned char)l[0]);
    lang[1] = (char)tolower((unsigned char)l[1]);
    const char *p = l + 2;
    if (*p == '_' || *p == '-') {
        p++;
        unsigned r = 0;
        while (*p && *p != '.' && *p != '@' && r < sizeof(region) - 1)
            region[r++] = (char)toupper((unsigned char)*p++);
    }

    if (!strcmp(lang, "ja")) return "ja";
    if (!strcmp(lang, "de")) return "de";
    if (!strcmp(lang, "it")) return "it";
    if (!strcmp(lang, "es")) return "es";
    if (!strcmp(lang, "fr")) return "fr";
    if (!strcmp(lang, "ko")) return "ko";
    if (!strcmp(lang, "zh"))
        return (!strcmp(region, "TW") || !strcmp(region, "HK") || !strcmp(region, "MO"))
                 ? "zh-Hant" : "zh";
    return "en"; // en + everything resources.bin lacks
}

bool os_is_focused(void) {
    if (!s_win) return true;
    return (SDL_GetWindowFlags(s_win) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

void os_sleep_ms(unsigned ms) { usleep((useconds_t)ms * 1000u); }

void os_log(const char *fmt, ...) {
    // Quiet by default for release builds; export CT_LOG=1 to restore the
    // startup/input/error logging (the crash signal handler in main.c always
    // logs regardless — that path doesn't go through here).
    static int en = -1;
    if (en < 0) en = getenv("CT_LOG") ? 1 : 0;
    if (!en) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
