// harness_stubs.c — load-test-only placeholders for the app-layer functions that
// imports.c's table points at (prefs/gfx/movie/config/util). They let the table
// LINK so the resolver can run; they are NEVER called by harness_load.c (it does
// not execute guest code). Real implementations come from prefs.c/gfx.c/
// movie_player.c/config.c/util.c when those are ported in milestone #3.
#include <stddef.h>
#include "config.h"
#include "prefs.h"
#include "gfx.h"
#include "movie_player.h"
#include "util.h"

// config.c
Config config;
int screen_width = 1280;
int screen_height = 720;

// util.c
int  ret0(void)            { return 0; }
int  retm1(void)           { return -1; }
void cpu_boost(int on)     { (void)on; }
void tls_setup_guard(void) { /* milestone #3: -> os_tls_init() */ }

// prefs.c
void        prefs_init(const char *p)                 { (void)p; }
void        prefs_flush(void)                         {}
const char *prefs_get_string(const char *k, const char *d) { (void)k; return d; }
int         prefs_get_bool(const char *k, int d)      { (void)k; return d; }
int         prefs_get_int(const char *k, int d)       { (void)k; return d; }
float       prefs_get_float(const char *k, float d)   { (void)k; return d; }
void        prefs_set_string(const char *k, const char *v) { (void)k; (void)v; }
void        prefs_set_bool(const char *k, int v)      { (void)k; (void)v; }
void        prefs_set_int(const char *k, int v)       { (void)k; (void)v; }
void        prefs_set_float(const char *k, float v)   { (void)k; (void)v; }
void        prefs_delete(const char *k)               { (void)k; }

// gfx.c
unsigned char *gfx_render_text_rgba(const char *text, int font_size,
                                    int r, int g, int b, int a,
                                    int align_h, int max_w, int max_h, int wrap,
                                    int shadow, double shadow_dx, double shadow_dy,
                                    double shadow_opacity,
                                    int *out_w, int *out_h) {
  (void)text;(void)font_size;(void)r;(void)g;(void)b;(void)a;(void)align_h;
  (void)max_w;(void)max_h;(void)wrap;
  (void)shadow;(void)shadow_dx;(void)shadow_dy;(void)shadow_opacity;
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  return NULL;
}

// movie_player.c
int movie_play(const char *name) { (void)name; return 0; }
