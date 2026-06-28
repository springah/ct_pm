/* main.c -- Chrono Trigger (cocos2d-x 3.14.1) Switch wrapper entry point
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Loads libc++_shared.so + libchrono.so into emulated-Android memory, wires the
 * engine's imports to native shims, then drives the cocos2d-x JNI lifecycle
 * (JNI_OnLoad -> setContext/apk/assets -> nativeInit -> nativeRender loop).
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#ifdef __SWITCH__
#include <switch.h>
#else
#include "switch_compat.h"
#include "os.h"
#include "crash.h"
#endif
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "asset.h"
#include "gfx.h"
#include "opensles.h"
#include "prefs.h"
#include "movie_player.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module cpp_mod;   // libc++_shared.so
so_module game_mod;  // libchrono.so

void ct_resolve_imports(so_module *mod);

// The Linux/PortMaster crash + termination signal handlers live in
// portmaster/crash.c (ct_install_crash_handler / ct_term_requested, declared in
// crash.h). They resolve fault addresses against cpp_mod / game_mod below.

#ifdef __SWITCH__
// provide a replacement heap init so the newlib heap is separate from the .so
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  // The newlib heap backs BOTH the engine's malloc and mesa/nouveau's GPU
  // texture memory (nvMap buffers come from this heap). cocos2d-x + Chrono
  // Trigger's field maps allocate hundreds of MB of textures/render targets, so
  // the newlib heap must get the bulk of memory -- only a small fixed slice is
  // reserved for the two .so load images (libc++_shared ~2MB + libchrono ~16MB).
  size_t so_reserve = (size_t)SO_HEAP_RESERVE_MB * 1024 * 1024;
  if (so_reserve > size / 2)
    so_reserve = size / 2;
  fake_heap_size = size - so_reserve;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}
#endif // __SWITCH__

static void check_syscalls(void) {
#ifdef __SWITCH__
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
#endif
}

static void check_data(void) {
  struct stat st;
  if (stat(SO_NAME, &st) < 0)    fatal_error("Could not find\n%s.\nCheck your data files.", SO_NAME);
  if (stat(SOCPP_NAME, &st) < 0) fatal_error("Could not find\n%s.\nCheck your data files.", SOCPP_NAME);
  if (stat(ASSETS_DIR, &st) < 0) fatal_error("Could not find the\n%s/ folder.\nCheck your data files.", ASSETS_DIR);
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
#ifdef __SWITCH__
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
#else
    screen_width = 1280; screen_height = 720; // handheld panel default
#endif
  } else {
    screen_width = w; screen_height = h;
  }
}

// ---------------------------------------------------------------------------
// EGL / GLES2 context on the default NWindow
// ---------------------------------------------------------------------------

#ifndef __SWITCH__
// Linux/PortMaster: SDL2 owns the window + GLES2 context (see os_linux.c).
// NB: egl_init() returns 1 on success (boolean), os_gfx_init() returns 0 on success.
static int egl_init(void) {
  return os_gfx_init(screen_width, screen_height, &screen_width, &screen_height) == 0;
}
static void egl_deinit(void) { os_gfx_shutdown(); }
#else
static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;

static int egl_init(void) {
  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_display) { debugPrintf("egl: no display\n"); return 0; }
  eglInitialize(s_display, NULL, NULL);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { debugPrintf("egl: bindAPI failed\n"); return 0; }

  // cocos default GLContextAttrs: RGBA8888, depth24, stencil8, no MSAA
  const EGLint cfg_attr[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  EGLConfig config;
  EGLint num = 0;
  if (!eglChooseConfig(s_display, cfg_attr, &config, 1, &num) || num < 1) {
    debugPrintf("egl: no config\n");
    return 0;
  }

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  s_surface = eglCreateWindowSurface(s_display, config, win, NULL);
  if (!s_surface) { debugPrintf("egl: no surface\n"); return 0; }

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, ctx_attr);
  if (!s_context) { debugPrintf("egl: no context\n"); return 0; }

  eglMakeCurrent(s_display, s_surface, s_surface, s_context);
  eglSwapInterval(s_display, 1);
  return 1;
}

static void egl_deinit(void) {
  if (s_display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context) eglDestroyContext(s_display, s_context);
  if (s_surface) eglDestroySurface(s_display, s_surface);
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
}
#endif // __SWITCH__

// ---------------------------------------------------------------------------
// cocos2d-x engine entry points (exported by libchrono.so)
// ---------------------------------------------------------------------------

static int  (*e_JNI_OnLoad)(void *vm, void *reserved);
static void (*e_nativeSetContext)(void *env, void *thiz, void *ctx, void *amgr);
static void (*e_nativeSetApkPath)(void *env, void *thiz, void *path);
static void (*e_setAssetManager)(void *env, void *thiz, void *ctx, void *amgr);
static void (*e_setExternalStorageInfo)(void *env, void *thiz, void *a, void *b, void *c);
static void (*e_nativeInit)(void *env, void *thiz, int w, int h);
static void (*e_nativeRender)(void *env);  // NOTE: env only, no thiz
static void (*e_nativeOnPause)(void);
static void (*e_nativeOnResume)(void);
static void (*e_touchBegin)(void *env, void *thiz, int id, float x, float y);
static void (*e_touchEnd)(void *env, void *thiz, int id, float x, float y);
static void (*e_touchMove)(void *env, void *thiz, void *ids, void *xs, void *ys);
static int  (*e_keyEvent)(void *env, void *thiz, int keycode, int pressed);
// cocos GameControllerAdapter natives: deviceName (jstring) is the FIRST data arg
static void (*e_ctrlConnected)(void *env, void *thiz, void *name, int id);
static void (*e_ctrlButton)(void *env, void *thiz, void *name, int id, int button, int pressed, float value, int analog);
static void (*e_ctrlAxis)(void *env, void *thiz, void *name, int id, int axis, float value, int analog);
static void (*e_bitmapDC)(void *env, void *thiz, int w, int h, void *pixels);
static void (*e_videoCb)(void *env, void *thiz, int index, int event);
static void (*e_insertText)(void *env, void *thiz, void *jstr);
static void (*e_deleteBackward)(void *env, void *thiz);
// cocos ui::EditBox result natives (Cocos2dxEditBoxHelper)
static void (*e_ebDidBegin)(void *env, void *cls, int index);
static void (*e_ebChanged)(void *env, void *cls, int index, void *jstr);
static void (*e_ebDidEnd)(void *env, void *cls, int index, void *jstr);
static void (*e_glInvalidate)(void); // cocos2d::GL::invalidateStateCache (post-movie)

// DeviceInfo::mCurrentLanguage (int): the engine indexes mLocalizationLanguages
// with this directly (ja=0 en=1 de=2 it=3 es=4 fr=5 zh-Hans=6 zh-Hant=7 ko=8).
// We pin it to config.language (see force_language + the patches in main()).
static int *e_lang_var;
static void *g_ctrl_name; // persistent jstring device name for controller events

#define RX(sym) so_try_find_addr_rx(&game_mod, sym)

static void resolve_entry_points(void) {
  e_JNI_OnLoad            = (void *)RX("JNI_OnLoad");
  e_nativeSetContext      = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetContext");
  e_nativeSetApkPath      = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath");
  e_setAssetManager       = (void *)RX("Java_org_cocos2dx_cpp_AppActivity_setAssetManager");
  e_setExternalStorageInfo= (void *)RX("Java_org_cocos2dx_cpp_AppActivity_setExternalStorageInfo");
  e_nativeInit            = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
  e_nativeRender          = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
  e_nativeOnPause         = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
  e_nativeOnResume        = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
  e_touchBegin            = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
  e_touchEnd              = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
  e_touchMove             = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
  e_keyEvent              = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyEvent");
  e_ctrlConnected         = (void *)RX("Java_org_cocos2dx_lib_GameControllerAdapter_nativeControllerConnected");
  e_ctrlButton            = (void *)RX("Java_org_cocos2dx_lib_GameControllerAdapter_nativeControllerButtonEvent");
  e_ctrlAxis              = (void *)RX("Java_org_cocos2dx_lib_GameControllerAdapter_nativeControllerAxisEvent");
  e_bitmapDC              = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC");
  e_videoCb               = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxVideoHelper_nativeExecuteVideoCallback");
  e_insertText            = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInsertText");
  e_deleteBackward        = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeDeleteBackward");
  e_ebDidBegin            = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxEditBoxHelper_editBoxEditingDidBegin");
  e_ebChanged             = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxEditBoxHelper_editBoxEditingChanged");
  e_ebDidEnd              = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxEditBoxHelper_editBoxEditingDidEnd");
  e_glInvalidate          = (void *)RX("_ZN7cocos2d2GL20invalidateStateCacheEv");
  e_lang_var              = (int *)RX("_ZN10DeviceInfo16mCurrentLanguageE");
}

static void *thiz; // fake MainActivity instance handed to the JNI entry points

// ---------------------------------------------------------------------------
// input -- Switch HID -> cocos keyboard + controller events
// Android keycodes (keyboard path) and cocos2d::Controller::Key (controller path)
// ---------------------------------------------------------------------------

// Android KeyEvent keycodes recognised by cocos2d-x 3.14.1's g_keyCodeMap
#define AK_BACK   4
#define AK_DUP    19
#define AK_DDOWN  20
#define AK_DLEFT  21
#define AK_DRIGHT 22
#define AK_DCENTER 23
#define AK_ENTER  66
#define AK_MENU   82
#define AK_NONE   (-1)

// cocos2d::Controller::Key enum values (CCController.h, 3.14.1)
enum {
  CC_JOY_LX = 1000, CC_JOY_LY, CC_JOY_RX, CC_JOY_RY,
  CC_BTN_A, CC_BTN_B, CC_BTN_C, CC_BTN_X, CC_BTN_Y, CC_BTN_Z,
  CC_DPAD_UP, CC_DPAD_DOWN, CC_DPAD_LEFT, CC_DPAD_RIGHT, CC_DPAD_CENTER,
  CC_L_SHOULDER, CC_R_SHOULDER, CC_L_TRIGGER, CC_R_TRIGGER,
  CC_L_THUMB, CC_R_THUMB, CC_BTN_START, CC_BTN_SELECT, CC_BTN_PAUSE
};

typedef struct {
  u64 mask;       // HidNpadButton bit (incl. stick-as-dpad synthesised below)
  int android_kc; // keyboard path keycode, or AK_NONE
  int cc_key;     // controller path cocos key
} KeyMap;

#ifdef __SWITCH__
// Nintendo/SNES layout (mirrors the Linux LinKeyMap): A=confirm, B=cancel/dash,
// X=Menu, Y=char-switch/time-gauge, +=Pause. The engine's X/Y swap defaults to ON,
// so X/Y are CROSSED here (top X -> CC_BTN_Y lands Menu on top to match SNES).
static const KeyMap g_keymap[] = {
  { HidNpadButton_A,     AK_ENTER,   CC_BTN_A },
  { HidNpadButton_B,     AK_BACK,    CC_BTN_B },
  { HidNpadButton_X,     AK_NONE,    CC_BTN_Y },  // top: open Menu (X/Y swap on -> send BUTTON_Y)
  { HidNpadButton_Y,     AK_NONE,    CC_BTN_X },  // left: char-switch / time-gauge
  { HidNpadButton_L,     AK_NONE,    CC_L_SHOULDER },
  { HidNpadButton_R,     AK_NONE,    CC_R_SHOULDER },
  { HidNpadButton_ZL,    AK_NONE,    CC_L_TRIGGER },
  { HidNpadButton_ZR,    AK_NONE,    CC_R_TRIGGER },
  { HidNpadButton_Plus,  AK_NONE,    CC_BTN_START },  // + : pause/system (bit1)
  { HidNpadButton_Minus, AK_NONE,    CC_BTN_SELECT },  // engine ignores SELECT
  { HidNpadButton_StickL, AK_NONE,   CC_L_THUMB },
  { HidNpadButton_StickR, AK_NONE,   CC_R_THUMB },
  { HidNpadButton_Up    | HidNpadButton_StickLUp,    AK_DUP,    CC_DPAD_UP },
  { HidNpadButton_Down  | HidNpadButton_StickLDown,  AK_DDOWN,  CC_DPAD_DOWN },
  { HidNpadButton_Left  | HidNpadButton_StickLLeft,  AK_DLEFT,  CC_DPAD_LEFT },
  { HidNpadButton_Right | HidNpadButton_StickLRight, AK_DRIGHT, CC_DPAD_RIGHT },
};
#define NUM_KEYMAP (sizeof(g_keymap) / sizeof(*g_keymap))

static PadState pad;
static int g_prev[NUM_KEYMAP];

static void send_button(int idx, int pressed) {
  const KeyMap *k = &g_keymap[idx];
  if (k->android_kc != AK_NONE && e_keyEvent)
    e_keyEvent(fake_env, thiz, k->android_kc, pressed);
  if (e_ctrlButton && k->cc_key) // cc_key 0 = no controller mapping
    e_ctrlButton(fake_env, thiz, g_ctrl_name, 0, k->cc_key, pressed, pressed ? 1.0f : 0.0f, 0);
}

static void update_keys(void) {
  padUpdate(&pad);
  const u64 d = padGetButtons(&pad);
  for (unsigned i = 0; i < NUM_KEYMAP; i++) {
    const int now = (d & g_keymap[i].mask) ? 1 : 0;
    if (now != g_prev[i]) {
      send_button((int)i, now);
      g_prev[i] = now;
    }
  }

  // analog sticks -> controller axes (normalised -1..1). Only emit on a real
  // change past a small deadzone: re-sending centred axes every frame floods
  // the engine's controller listeners and can fight d-pad navigation.
  if (e_ctrlAxis) {
    HidAnalogStickState l = padGetStickPos(&pad, 0);
    HidAnalogStickState r = padGetStickPos(&pad, 1);
    const float dz = 0.12f; // ~3900/32768 deadzone
    const float axes[4] = { l.x / 32768.0f, -l.y / 32768.0f, r.x / 32768.0f, -r.y / 32768.0f };
    static const int axis_key[4] = { CC_JOY_LX, CC_JOY_LY, CC_JOY_RX, CC_JOY_RY };
    static float prev_axis[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) {
      float v = (axes[i] > -dz && axes[i] < dz) ? 0.0f : axes[i];
      if (v != prev_axis[i]) {
        e_ctrlAxis(fake_env, thiz, g_ctrl_name, 0, axis_key[i], v, 1);
        prev_axis[i] = v;
      }
    }
  }
}
#endif // __SWITCH__ (HID keymap + pad poll)

// ---------------------------------------------------------------------------
// language -- map config.language to DeviceInfo's LocalizationLanguageType index
// (resources.bin ships all nine under Localize/<code>/)
// ---------------------------------------------------------------------------

static int lang_index(void) {
  const char *l = config.language;
  if (!strcmp(l, "ja")) return 0;
  if (!strcmp(l, "en")) return 1;
  if (!strcmp(l, "de")) return 2;
  if (!strcmp(l, "it")) return 3;
  if (!strcmp(l, "es")) return 4;
  if (!strcmp(l, "fr")) return 5;
  if (!strcmp(l, "zh") || !strcmp(l, "zh-Hans") || !strcmp(l, "zh_CN")) return 6;
  if (!strcmp(l, "zh-Hant") || !strcmp(l, "zh_TW")) return 7;
  if (!strcmp(l, "ko")) return 8;
  return 1; // default English
}
static void force_language(void) {
  if (e_lang_var) *e_lang_var = lang_index();
}

// ---------------------------------------------------------------------------
// touch -- single pointer mapped into the engine's screen space
// ---------------------------------------------------------------------------

#ifdef __SWITCH__
static int touch_down = 0;
static float last_tx = 0, last_ty = 0;

static void update_touch(void) {
  HidTouchScreenState st = {0};
  int have = hidGetTouchScreenStates(&st, 1) && st.count > 0;

  if (have) {
    // libnx reports panel coords in 1280x720; scale to our framebuffer
    const float sx = (float)screen_width / 1280.0f;
    const float sy = (float)screen_height / 720.0f;
    float x = st.touches[0].x * sx;
    float y = st.touches[0].y * sy;
    if (!touch_down) {
      touch_down = 1;
      if (e_touchBegin) e_touchBegin(fake_env, thiz, 0, x, y);
    } else if (e_touchMove) {
      int ids[1] = { 0 };
      float xs[1] = { x }, ys[1] = { y };
      void *jids = jni_new_int_array(ids, 1);
      void *jxs = jni_new_float_array(xs, 1);
      void *jys = jni_new_float_array(ys, 1);
      e_touchMove(fake_env, thiz, jids, jxs, jys);
      jni_delete_ref(jids); jni_delete_ref(jxs); jni_delete_ref(jys);
    }
    last_tx = x; last_ty = y;
  } else if (touch_down) {
    touch_down = 0;
    if (e_touchEnd) e_touchEnd(fake_env, thiz, 0, last_tx, last_ty);
  }
}
#else
// Linux/PortMaster input: SDL_GameController -> the SAME cocos keyboard +
// controller event path the Switch build uses. os_input_poll() (os_linux.c)
// flattens the pad into an os_input_state of OS_BTN_* level bits + analog
// sticks; here we edge-detect those bits and drive e_keyEvent / e_ctrlButton,
// then push the sticks through e_ctrlAxis. This mirrors the Switch g_keymap /
// send_button / update_keys logic so navigation, confirm/cancel and the analog
// axes behave identically across both targets.
//
// Nintendo layout is preserved end-to-end: os_input_poll already swaps the SDL
// positional face buttons so OS_BTN_A is the RIGHT button (confirm -> AK_ENTER /
// CC_BTN_A) and OS_BTN_B is the BOTTOM button (cancel -> AK_BACK / CC_BTN_B).

typedef struct {
  uint32_t bit;       // OS_BTN_* level bit from os_input_state
  int      android_kc; // keyboard path keycode, or AK_NONE
  int      cc_key;     // controller path cocos key, or 0 for none
} LinKeyMap;

// Nintendo/SNES layout, mapped 1:1 by button LABEL to the engine's own cocos
// Controller::Key. The engine (GameController::onKeyDown) decodes each key into
// an internal bit and the game reads only those bits: A(1004)->bit7 Confirm,
// B(1005)->bit3 Cancel/Dash, X(1007)->bit6 Menu, Y(1008)->bit0 Character-switch/
// Time-gauge, START(1021)->bit1 Pause, L/R shoulders(1015/16)->bit5/bit4.
// NOTE the engine's built-in X/Y swap defaults to ON and its in-game remap screen
// is a touch-only UI we can't reach on this (touchscreen-less) device, so we
// compensate by CROSSING X/Y here: the top (X) button sends BUTTON_Y and the left
// (Y) button sends BUTTON_X, which lands Menu on the top button to match SNES.
// (SELECT(1022), the C/Z/PAUSE keys, triggers and thumb-clicks are NOT decoded by
// the engine -- so Select has no native binding; the SNES world-map toggle isn't
// reachable via gamepad. ZL/ZR are kept as triggers in case a later build binds them.)
static const LinKeyMap g_keymap[] = {
  { OS_BTN_A,      AK_ENTER,   CC_BTN_A },       // right button: confirm  (bit7)
  { OS_BTN_B,      AK_BACK,    CC_BTN_B },       // bottom button: cancel/dash (bit3)
  { OS_BTN_X,      AK_NONE,    CC_BTN_Y },       // top button: open Menu  (X/Y swap on -> send BUTTON_Y)
  { OS_BTN_Y,      AK_NONE,    CC_BTN_X },       // left button: char-switch / time-gauge
  { OS_BTN_L,      AK_NONE,    CC_L_SHOULDER },
  { OS_BTN_R,      AK_NONE,    CC_R_SHOULDER },
  { OS_BTN_ZL,     AK_NONE,    CC_L_TRIGGER },
  { OS_BTN_ZR,     AK_NONE,    CC_R_TRIGGER },
  { OS_BTN_START,  AK_NONE,    CC_BTN_START },   // Start: pause/system (bit1)
  { OS_BTN_SELECT, AK_NONE,    CC_BTN_SELECT },  // engine ignores SELECT (no native binding)
  { OS_BTN_UP,     AK_DUP,     CC_DPAD_UP },     // d-pad bits already include left-stick synthesis
  { OS_BTN_DOWN,   AK_DDOWN,   CC_DPAD_DOWN },
  { OS_BTN_LEFT,   AK_DLEFT,   CC_DPAD_LEFT },
  { OS_BTN_RIGHT,  AK_DRIGHT,  CC_DPAD_RIGHT },
};
#define NUM_KEYMAP (sizeof(g_keymap) / sizeof(*g_keymap))

static os_input_state g_in;
static uint32_t g_prev_buttons = 0;

static void send_button(int idx, int pressed) {
  const LinKeyMap *k = &g_keymap[idx];
  if (k->android_kc != AK_NONE && e_keyEvent)
    e_keyEvent(fake_env, thiz, k->android_kc, pressed);
  if (e_ctrlButton && k->cc_key) // cc_key 0 = no controller mapping
    e_ctrlButton(fake_env, thiz, g_ctrl_name, 0, k->cc_key, pressed, pressed ? 1.0f : 0.0f, 0);
}

static void update_keys(void) {
  os_input_poll(&g_in);
  const uint32_t now = g_in.buttons;
  const uint32_t changed = now ^ g_prev_buttons;
  if (changed) {
    for (unsigned i = 0; i < NUM_KEYMAP; i++) {
      if (changed & g_keymap[i].bit)
        send_button((int)i, (now & g_keymap[i].bit) ? 1 : 0);
    }
    g_prev_buttons = now;
  }

  // analog sticks -> controller axes (already normalised -1..1, Y up-positive,
  // deadzone applied in os_input_poll). Only emit on a real change so we don't
  // flood the engine's controller listeners or fight d-pad navigation.
  if (e_ctrlAxis) {
    const float dz = 0.12f;
    const float axes[4] = { g_in.lx, g_in.ly, g_in.rx, g_in.ry };
    static const int axis_key[4] = { CC_JOY_LX, CC_JOY_LY, CC_JOY_RX, CC_JOY_RY };
    static float prev_axis[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) {
      float v = (axes[i] > -dz && axes[i] < dz) ? 0.0f : axes[i];
      if (v != prev_axis[i]) {
        e_ctrlAxis(fake_env, thiz, g_ctrl_name, 0, axis_key[i], v, 1);
        prev_axis[i] = v;
      }
    }
  }
}

static void update_touch(void) { /* TSP optional; not needed for menu/field nav */ }

// Re-baseline the controller edge-detector to the buttons currently held. Called
// when a blocking modal (FMV skip / on-screen keyboard) hands control back, so a
// button still held to dismiss it isn't re-sent to the engine as a fresh press
// when the main loop resumes (e.g. skipping the intro must not also advance the
// title screen into the New Game menu).
void input_consume_held(void) {
  os_input_poll(&g_in);
  g_prev_buttons = g_in.buttons;
}
#endif

// ---------------------------------------------------------------------------

int main(void) {
#ifndef __SWITCH__
  ct_install_crash_handler();
  // PortMaster launches from $GAMEDIR; make data paths (so/assets/config) relative.
  if (chdir(os_data_dir()) != 0) { /* check_data() will report if files are missing */ }
#endif
  cpu_boost(1);

  if (read_config(CONFIG_NAME) != 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  check_data();
  set_screen_size(config.screen_width, config.screen_height);

#ifdef __SWITCH__
  plInitialize(PlServiceType_User);
#endif
  gfx_init();

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO) < 0)
    debugPrintf("SDL_Init(audio) failed: %s\n", SDL_GetError());

  if (!egl_init())
    fatal_error("Failed to create an OpenGL ES 2.0 context.");

  // --- load both modules: libc++_shared first so libchrono's std imports bind ---
#ifdef __SWITCH__
  if (so_load(&cpp_mod, SOCPP_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", SOCPP_NAME);

  void *chrono_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + cpp_mod.load_size, 0x100000);
  size_t used = (uintptr_t)chrono_base - (uintptr_t)heap_so_base;
  if (so_load(&game_mod, SO_NAME, chrono_base, heap_so_limit - used) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);
#else
  // Linux: each module gets its own mmap (load_base == load_virtbase).
  if (so_load(&cpp_mod, SOCPP_NAME, NULL, (size_t)-1) < 0)
    fatal_error("Could not load\n%s.", SOCPP_NAME);
  if (so_load(&game_mod, SO_NAME, NULL, (size_t)-1) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);
#endif

  // relocate + resolve (libchrono's std::/__cxa_ symbols resolve into libc++_shared)
  ct_resolve_imports(&cpp_mod);
  ct_resolve_imports(&game_mod);

  // resolve every engine entry point now, while the symbol tables (in load_base)
  // are still readable -- so_finalize maps the code and locks load_base out.
  resolve_entry_points();
  if (!e_nativeInit || !e_nativeRender || !e_JNI_OnLoad)
    fatal_error("Could not resolve cocos2d-x engine entry points.");

  // Force the UI/text language to config.language regardless of what the engine
  // detects. resources.bin contains all languages under Localize/<code>/; the
  // engine selects via DeviceInfo::mCurrentLanguage, read DIRECTLY by both
  // getCurrentLanguage() and getLocalizeResourcePath(). We can't reliably win
  // the timing race against the engine's own write, so we patch those two reads
  // to a constant (the config index). Done now, while load_base is writable.
  {
    const int idx = lang_index() & 0xffff;
    // getCurrentLanguage: ldr w0,[x8] @ +0x8  ->  movz w0, #idx
    uintptr_t gcl = so_find_addr(&game_mod, "_ZN10DeviceInfo18getCurrentLanguageEv");
    *(uint32_t *)(gcl + 0x8) = 0x52800000u | ((uint32_t)idx << 5);
    // getLocalizeResourcePath: ldrsw x9,[x9] @ +0x20  ->  movz x9, #idx
    uintptr_t glrp = so_find_addr(&game_mod, "_ZN10DeviceInfo23getLocalizeResourcePathEv");
    *(uint32_t *)(glrp + 0x20) = 0xd2800009u | ((uint32_t)idx << 5);
  }

  so_finalize(&cpp_mod);
  so_finalize(&game_mod);
  so_flush_caches(&cpp_mod);
  so_flush_caches(&game_mod);

  tls_setup_guard();

  // C++ static constructors: runtime first, then the game.
  so_execute_init_array(&cpp_mod);
  so_execute_init_array(&game_mod);
  so_free_temp(&cpp_mod);
  so_free_temp(&game_mod);

  // --- JNI + cocos bootstrap ---
  jni_init();
  thiz = jni_make_object("AppActivity");

  // writable / save directory = the game directory (CWD)
  static char wdir[512];
  if (getcwd(wdir, sizeof(wdir)) && wdir[0]) {
    size_t n = strlen(wdir);
    if (n > 1 && wdir[n - 1] == '/') wdir[n - 1] = 0;
  } else {
    strcpy(wdir, ".");
  }
  jni_set_writable_path(wdir);
  {
    char prefs_path[600];
    snprintf(prefs_path, sizeof(prefs_path), "%s/Cocos2dxPrefsFile.txt", wdir);
    prefs_init(prefs_path);
  }

  jni_set_bitmap_cb((BitmapDCFn)e_bitmapDC);
  jni_set_video_cb((VideoCbFn)e_videoCb);
  jni_set_ime_cb((ImeInsertFn)e_insertText, (ImeDeleteFn)e_deleteBackward);
  jni_set_editbox_cb((EbBeginFn)e_ebDidBegin, (EbTextFn)e_ebChanged, (EbTextFn)e_ebDidEnd);
  movie_set_gl_invalidate(e_glInvalidate);
#ifndef __SWITCH__
  movie_set_input_drain(input_consume_held); // FMV skip must not bleed into the engine
  osk_set_gl_invalidate(e_glInvalidate); // on-screen keyboard restores cocos GL state too
  osk_set_input_drain(input_consume_held); // ...nor the OSK confirm/cancel button
#endif

  // a persistent device-name jstring for the controller event callbacks
  g_ctrl_name = jni_make_string("Nintendo Switch Controller");

  // force the UI language before the engine builds the title scene
  force_language();

  // JniHelper::setJavaVM + cocos_android_app_init (creates the AppDelegate)
  if (e_JNI_OnLoad)
    e_JNI_OnLoad(fake_vm, NULL);

  // hand the engine its context/assets/writable roots
  void *ctx  = jni_make_object("Context");
  void *amgr = jni_make_object("AssetManager");
  if (e_nativeSetContext) e_nativeSetContext(fake_env, thiz, ctx, amgr);
  if (e_nativeSetApkPath) { void *p = jni_make_string("game.apk"); e_nativeSetApkPath(fake_env, thiz, p); }
  if (e_setAssetManager) e_setAssetManager(fake_env, thiz, ctx, amgr);
  if (e_setExternalStorageInfo) {
    void *a = jni_make_string(wdir);
    void *b = jni_make_string(wdir);
    void *c = jni_make_string("com.square_enix.android_googleplay.chrono");
    e_setExternalStorageInfo(fake_env, thiz, a, b, c);
  }

  // create the GLView + run applicationDidFinishLaunching (the engine's entry)
  e_nativeInit(fake_env, thiz, screen_width, screen_height);

  // input
#ifdef __SWITCH__
  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();
  for (unsigned i = 0; i < NUM_KEYMAP; i++) g_prev[i] = 0;
#else
  os_input_init();
#endif
  // register controller 0 so Controller::getKeyStatus polling has a target
  if (e_ctrlConnected)
    e_ctrlConnected(fake_env, thiz, g_ctrl_name, 0);

#ifndef __SWITCH__
  // CT_OSK_TEST: pop the on-screen keyboard at boot so it can be exercised without
  // navigating to an in-game name field. Type + OK, then check the log for the result.
  if (getenv("CT_OSK_TEST")) {
    e_nativeRender(fake_env); os_gfx_swap(); // establish the viewport + a backdrop
    SwkbdConfig kb; swkbdCreate(&kb, 0);
    swkbdConfigSetInitialText(&kb, "CRONO");
    swkbdConfigSetStringLenMax(&kb, 8);
    char nm[64] = {0};
    Result rc = swkbdShow(&kb, nm, sizeof(nm));
    swkbdClose(&kb);
    os_log("CT_OSK_TEST: rc=%u name='%s'\n", (unsigned)rc, nm);
  }
#endif

  int paused = 0;
  int boot_frames = 0;
#ifndef __SWITCH__
  const int cap_enabled = getenv("CT_CAPTURE") != NULL; // hoist out of the frame loop
#endif
#ifdef __SWITCH__
  while (appletMainLoop() && !jni_quit_requested) {
#else
  while (!jni_quit_requested && !g_in.quit && !ct_term_requested()) {
#endif
    // pause/resume on focus changes
#ifdef __SWITCH__
    const int focused = appletGetFocusState() == AppletFocusState_InFocus;
#else
    const int focused = os_is_focused();
#endif
    if (!focused && !paused) { if (e_nativeOnPause) e_nativeOnPause(); paused = 1; }
    else if (focused && paused) { if (e_nativeOnResume) e_nativeOnResume(); paused = 0; }

    if (paused) {
#ifdef __SWITCH__
      svcSleepThread(16000000ull); // ~16ms; don't spin while backgrounded
#else
      update_keys(); // still pump events so we can unpause / quit
      os_sleep_ms(16);
#endif
      continue;
    }

    update_keys();
    update_touch();
    force_language(); // keep mCurrentLanguage pinned for any direct readers
                      // (getCurrentLanguage/getLocalizeResourcePath are patched)

    e_nativeRender(fake_env);
#ifdef __SWITCH__
    eglSwapBuffers(s_display, s_surface);
#else
    // debug: dump frames off the GPU back buffer when CT_CAPTURE is set --
    // early, then periodically, so we can watch it progress splash -> title.
    if (cap_enabled) {
      if (boot_frames == 60 || (boot_frames > 0 && boot_frames % 300 == 0)) {
        char p[256];
        snprintf(p, sizeof(p), "%s/frame_%05d.ppm", os_data_dir(), boot_frames);
        os_gfx_capture(p);
        os_gfx_capture("latest.ppm"); // always-overwritten newest frame
      }
    }
    os_gfx_swap();
#endif

    jni_ime_service(); // show swkbd for a pending EditBox, outside nativeRender

    // FMV transition fix: the movie scenes have no video-completion path — they only
    // leave on a skip input (cocos KeyCode 6 -> SceneManager::NextScene). Our blocking
    // movie_play() has no async COMPLETED, so when a clip ends we synthesize that skip
    // here, on a clean frame boundary outside the engine's startVideo call.
    if (jni_consume_video_finished()) {
      // NextScene's destination is chosen by SceneManager's current-scene-id (an int
      // reached via a pointer at libchrono+0xbc7210), not by the skip itself. The boot
      // attract plays as a blocking native movie WITHOUT the engine entering
      // DemoMovieScene, so that id is left on the boot/new-game scene -> the skip walks
      // into the New Game flow (controls guide -> battle-mode pick). Force the id to
      // DemoMovieScene (28) so NextScene takes the demo branch (-> replaceScene(
      // create(3)) = TitleScene). Leave in-game cutscenes (PlayMovieScene, id 29)
      // alone — they already have the correct destination. (Offset is libchrono v2.1.5.)
      void **sid_pp = (void **)((uintptr_t)game_mod.load_virtbase + 0xbc7210);
      if (*sid_pp) {
        int *sid = (int *)*sid_pp;
        if (*sid != 29) *sid = 28; // 29 = PlayMovieScene cutscene; don't redirect it
      }
      if (e_keyEvent) {
        e_keyEvent(fake_env, thiz, AK_BACK, 1); // KeyCode 6 == Back/Escape
        e_keyEvent(fake_env, thiz, AK_BACK, 0);
      }
    }

#ifdef __SWITCH__
    if (boot_frames < 10 && ++boot_frames == 10)
      cpu_boost(0);
#else
    if (boot_frames < 100000) boot_frames++; // free-running frame counter for capture
    if (boot_frames == 10) cpu_boost(0);
#endif
  }

  prefs_flush();
  if (e_nativeOnPause) e_nativeOnPause();  // engine autosave/flush (data safety)
#ifdef __SWITCH__
  opensles_shutdown();
  egl_deinit();
  plExit();
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
#else
  // Fast quit: saves are already flushed above. Skip the slow SDL-audio / EGL /
  // Mali teardown (which makes returning to the frontend feel sluggish) and let
  // the kernel reclaim everything immediately.
  fflush(NULL);
  _exit(0);
#endif
  return 0;
}
