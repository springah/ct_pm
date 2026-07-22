/* config.h -- Chrono Trigger Switch wrapper configuration
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// Memory reserved for the two .so load images. The rest of the heap backs both
// engine malloc and mesa GPU textures (field maps need the bulk) -- see
// __libnx_initheap in main.c.
#define SO_HEAP_RESERVE_MB 96

// The engine (libchrono.so) and its C++ runtime (libc++_shared.so). The Java-side
// libRMS.so/libencrypt.so are not needed -- the wrapper drives the cocos2d-x JNI
// entry points directly.
#define SO_NAME    "libchrono.so"
#define SOCPP_NAME "libc++_shared.so"

// Loose game assets (the APK's assets/ folder), served via the fake AAssetManager.
#define ASSETS_DIR "assets"

#define CONFIG_NAME "config.txt"
#define LOG_NAME    "debug.log"

#define DEBUG_LOG 0

extern int screen_width;
extern int screen_height;

// Default UI language. "default" auto-detects the system language (from $LANG,
// see os_system_language) and collapses it onto one of the codes resources.bin
// ships; anything unsupported falls back to English. A concrete code
// (en/ja/de/it/es/fr/zh/zh-Hant/ko) pins that language instead.
#define LANG_DEFAULT "default"

typedef struct {
  int screen_width;
  int screen_height;
  char language[8];
  // Internal render scale (Linux/PortMaster; see rescale.c). The engine
  // renders at panel*scale and is upscaled at present; 1 = native/off.
  float render_scale;
  // mesa/GLES tuning (Linux/PortMaster). Both default on; set 0 in config.txt
  // if a driver misbehaves.
  //   gl_threaded -- run mesa's GL submission on a worker core (mesa_glthread),
  //                  offloading draw-submission from the single-threaded engine.
  //   gl_no_error -- skip mesa's per-call GL validation (MESA_NO_ERROR); cocos's
  //                  calls are already well-formed, so this is pure CPU savings.
  int gl_threaded;
  int gl_no_error;
  // On-disk GL program binary cache (shadercache.c): skips the driver
  // compile+link the engine repeats per scene. Experimental, default off;
  // needs a driver exposing GL_OES_get_program_binary (self-disables if not).
  int shader_cache;
  // Runtime libchrono patches (patches.h; each old-word verified, and only
  // applied when the v2.1.5 fingerprint matches). On by default (verified
  // on-device against v2.1.5); set 0 in config.txt to disable any one.
  //   cursor_fix            -- white-on-dark menu selection instead of the mobile
  //                            build's cream colour-invert highlight.
  //   remove_mobile_ui      -- hide the on-screen touch overlays (field/world/title
  //                            buttons, per-menu back/close, race + colour prompts);
  //                            movement default RUN->WALK.
  //   controller_glyphs     -- render <BTN_*> button prompts + bracketed PUA glyphs.
  //   fix_diagonal_movement -- smooth the field diagonal-movement stutter.
  int cursor_fix;
  int remove_mobile_ui;
  int controller_glyphs;
  int fix_diagonal_movement;
  // Input: remap the four "extra" physical buttons to an engine action. Each of
  // key_zl/key_zr/key_start/key_select takes one of:
  //   a b x y l r zl zr start select menu none
  // (only the emitted action changes; edge/press stays on the real button).
  // Defaults keep the stock binding. right_stick_mirror: 1 = the right stick
  // also drives movement when the left stick is centred.
  char key_zl[16];
  char key_zr[16];
  char key_start[16];
  char key_select[16];
  int right_stick_mirror;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
