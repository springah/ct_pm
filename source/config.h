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
  // renders at panel*scale and is upscaled at present; 1 (default) = native.
  // Set 0.75 on GPU-bound devices to trade sharpness for fps.
  float render_scale;
  // Upscale filter at present: "linear" (default, soft) or "nearest" (sharp;
  // with an integer scale pair like 0.5 on a 640x480 panel every internal
  // pixel maps to an exact block -- true integer scaling).
  char render_filter[8];
  // force_nearest -- rewrite every GL texture filter to NEAREST at the wrapper
  // (imports.c), killing bilinear sampling of the game's own art. render_filter
  // only covers our final upscale blit; this covers how the engine samples its
  // textures, which is where the softness actually comes from. A binary patch on
  // libchrono's texture-creation sites is NOT sufficient: GL's default MAG filter
  // is GL_LINEAR, so any texture created outside those paths still samples LINEAR,
  // and runtime setAntiAliasTexParameters calls can re-enable it later. Default
  // ON (verified on-device): pixel art gets crisp. Set 0 for the softer,
  // bilinear look -- deliberately-softened stretched backgrounds go blocky at 1.
  int force_nearest;
  // mesa/GLES tuning (Linux/PortMaster).
  //   gl_threaded -- run mesa's GL submission on a worker core (mesa_glthread).
  //                  Default OFF: a no-op on blob drivers (PowerVR) and measured
  //                  added latency on panfrost/Mali; set 1 only if a mesa driver
  //                  demonstrably benefits.
  //   gl_no_error -- skip mesa's per-call GL validation (MESA_NO_ERROR); cocos's
  //                  calls are already well-formed, so this is pure CPU savings.
  //                  Default on.
  int gl_threaded;
  int gl_no_error;
  // On-disk GL program binary cache (shadercache.c): skips the driver
  // compile+link the engine repeats per scene. Default on (verified on
  // PowerVR GE8300 + Mali-G31); self-disables when the driver lacks
  // GL_OES_get_program_binary.
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
  // Framing / scale cluster (patches.h section 5; ported from ct_nx and
  // generalised to any panel). Boot-time libchrono patches: relaunch to change.
  //   ui_scale_fix   -- stamp EVERY entry of the engine's design-resolution table
  //                     with panel/design_scale, so the scene scale is exactly
  //                     design_scale on both axes (stock: 568x320 / 480x360 ...,
  //                     never an integer multiple of any panel).
  //   design_scale   -- panel px per design unit. 0 = auto: floor(panel_w/640),
  //                     min 1 -> 1 on 640x480, 2 on 1280x720, 3 on 1080p.
  //   game_area_width_fix -- ctr::gameArea width adaptive (hard-coded 568.0 in
  //                     stock); no-op at the stock design, required with ui_scale_fix.
  //   field_zoom_fix -- square, integer field art pixels: the fieldmap node's
  //                     setScale(1.875, 1.66667) becomes (z, z) and the view /
  //                     camera-limit sizes follow 1/z so the drawn view still
  //                     fills the canvas. Panel px per art px = z * design_scale.
  //   field_zoom     -- z. 0 = auto: the smallest integer panel-px size whose
  //                     visible rows fit the engine's fixed 432x224 field plane
  //                     (<= 220 rows): 3 px on 640x480, 4 px at 720p (ct_nx).
  //   map_zoom_fix / map_zoom -- world-map counterpart; map_zoom 0 = field_zoom.
  int   ui_scale_fix;
  float design_scale;
  int   game_area_width_fix;
  int   field_zoom_fix;
  float field_zoom;
  int   map_zoom_fix;
  float map_zoom;
  // font_snap -- pixel fonts (ChronoType: a 16 px/em grid) only render cleanly
  // at whole multiples of their native grid, but the engine's layout on a
  // 4:3 panel wants 1.5x. Modes (gfx.c):
  //   0 = off (FreeType at the fractional size: lumpy strokes)
  //   1 = whole multiples only (1x / 2x / 3x), largest that fits the cell
  //   2 = half multiples too (1.5x = render 3x, keep every other pixel: a
  //       regular 1-2-1-2 px pattern)
  //   3 = half multiples, box-filtered (1.5x with soft, even edges)
  // Auto-detected from the font's outlines; a non-pixel font is untouched.
  int   font_snap;
  // text_scale_fix -- draw system-font labels 1:1 (patches.h). Stock cocos2d
  // renders them at points x 2 and draws the sprite at design_scale / 2, which
  // is 2/3 on a 4:3 panel: every glyph nearest-squeezed. With the fix a 12-pt
  // label is a 16 px bitmap drawn 16 px tall on 640x480. No-op at 720p.
  int   text_scale_fix;
  // font_scale -- visual size of the UI font relative to the engine's request.
  // 0 = auto (1.5: 12-pt labels -> 24 px on 640x480 = 1.5x ChronoType, 3 px
  // strokes like the 3 px/art field; 1x was "tiny" in dialogue). Env
  // CT_FONT_SCALE overrides.
  float font_scale;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
