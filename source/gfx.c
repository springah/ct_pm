/* gfx.c -- system-font text rasterisation (FreeType on the Switch shared font)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <ft2build.h>
#include FT_FREETYPE_H

#include "gfx.h"
#include "util.h"
#include "config.h"

// We load the full set of Switch shared fonts and fall back across them per
// glyph, so Latin, CJK and symbol coverage all work (the engine may ask us to
// draw Japanese with the system font).
#define MAX_FACES 6
static FT_Library g_ft;
static FT_Face g_faces[MAX_FACES];
static int g_face_count = 0;
static int g_ok = 0;
static float g_font_scale = 1.0f;
static int   g_snap = 0;             // resolved font_snap: 0 off, 1 whole steps, 2 half steps
extern int screen_width, screen_height;

// Pixel-font grid: the px/em size at which face 0's outlines sit exactly on
// pixel boundaries (0 = not a pixel font). ChronoType is authored on a 16 px/em
// grid (every outline coordinate is a multiple of 1024/16 = 64 font units), so
// it renders cleanly only at 16/32/48... px; any other size puts strokes on
// half pixels and they come out alternately 1 and 2 px wide -- the lumpy text
// on 4:3 panels, where the engine's 1.333x scale lands sizes like 20 and 13.
static int g_font_grid = 0;

static long gcd_l(long a, long b) { while (b) { long t = a % b; a = b; b = t; } return a; }

static void detect_font_grid(FT_Face face) {
  if (!face || !FT_IS_SCALABLE(face) || face->units_per_EM <= 0) return;
  // Per-glyph: the gcd of the outline coordinates gives that glyph's grid.
  // Vote across glyphs and take the winner (a global gcd is wrecked by one
  // stray glyph or advance -- PixelMplus has a 364-unit advance among 500s;
  // advances are not part of the vote). Grid = upem / g must be a whole
  // 4..64 px/em, and the winner must own nearly every outlined glyph.
  const long upem = face->units_per_EM;
  int votes[65] = {0}; long outlined = 0;
  long n = face->num_glyphs; if (n > 1024) n = 1024;
  for (long gi = 0; gi < n; gi++) {
    if (FT_Load_Glyph(face, (FT_UInt)gi, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP))
      continue;
    if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) continue;
    const FT_Outline *o = &face->glyph->outline;
    if (o->n_points <= 0) continue;
    long g = 0;
    for (int i = 0; i < o->n_points; i++) {
      g = gcd_l(g, labs((long)o->points[i].x));
      g = gcd_l(g, labs((long)o->points[i].y));
    }
    outlined++;
    if (g <= 0 || (upem % g) != 0) continue;
    const long ppem = upem / g;
    if (ppem >= 4 && ppem <= 64) votes[ppem]++;
  }
  int best = 0;
  for (int p = 4; p <= 64; p++) if (votes[p] > votes[best]) best = p;
  if (!best || outlined < 16 || votes[best] * 10 < outlined * 9) return;   // outline font, or no clear grid
  g_font_grid = best;
  debugPrintf("gfx: pixel font detected, native grid %d px/em\n", g_font_grid);
}

// Snapped sizes are exact grid multiples: render them unhinted so the outline
// lands on the pixel grid verbatim (no AA fringe, no hinter nudging).
static FT_Int32 glyph_load_flags(int px) {
  if (g_font_grid > 0 && g_snap && (px % g_font_grid) == 0)
    return FT_LOAD_RENDER | FT_LOAD_NO_HINTING;
  return FT_LOAD_RENDER;
}

// Text drop-shadow (the classic SNES-style offset). Mode:
//   0 = off    : never draw a shadow
//   1 = auto   : honour the engine's createTextBitmapShadowStroke request
//   2 = force  : always draw, using the offset/opacity below (final pixels)
// Default = force, 1px down-right at 70%. The engine requests no shadow of its
// own (auto = flat everywhere), so we force one. A bigger 2px/80% shadow looks
// great on normal text, but the engine draws the *selected* menu row by
// dark-tinting the whole sprite — which turns our offset shadow into a muddy
// halo on the light highlight bar. We can't see that tint (the text reaches us
// as a light fill), so we can't skip it per-row; 1px/70% keeps a clear shadow
// while shrinking that halo to acceptable. Override at runtime via env
// CT_TEXT_SHADOW = "off" | "auto" | "force" | "dx,dy,opacity" (a numeric triple
// implies force; offsets are final on-screen pixels, opacity is 0..1).
static int   g_shadow_mode = 2;     // default: force the SNES-style drop-shadow
static float g_shadow_dx   = 1.0f;  // force-mode offset (final pixels)
static float g_shadow_dy   = 1.0f;
static float g_shadow_op   = 0.7f;  // force-mode opacity (0..1)

void gfx_init(void) {
  if (FT_Init_FreeType(&g_ft)) {
    debugPrintf("gfx: FT_Init_FreeType failed\n");
    return;
  }

#ifndef __SWITCH__
  // Panel profile. Narrow panels (4:3, 1:1) run the engine's 480-wide layout,
  // whose text sizes (16 / 21 px on 640x480) no ChronoType size lands on; the
  // bundled font-4x3.ttf (PixelMplus10: 1-px strokes on a 10 px/em grid) at
  // exactly 2x does -- 20 px cells, 2 px strokes, the same pixel as the 2 px
  // field art. Wide panels keep ChronoType (font.ttf) with half-step snapping
  // and a 1.25 scale (16:9 layout: 24 -> 32 px at 720p).
  const int narrow = screen_height > 0 && (float)screen_width / (float)screen_height < 1.6f;
  int font_scale_env = 0;
  { const char *s = getenv("CT_FONT_SCALE");
    if (s) { float v = (float)atof(s); if (v > 0.1f && v < 8.0f) { g_font_scale = v; font_scale_env = 1; } } }
  if (!font_scale_env)
    g_font_scale = config.font_scale > 0.0f ? config.font_scale : (narrow ? 1.0f : 1.25f);
  { const char *s = getenv("CT_TEXT_SHADOW");
    if (s) {
      if (!strcmp(s, "off"))        g_shadow_mode = 0;
      else if (!strcmp(s, "auto"))  g_shadow_mode = 1;
      else if (!strcmp(s, "force")) g_shadow_mode = 2;
      else {
        float dx, dy, op;
        if (sscanf(s, "%f,%f,%f", &dx, &dy, &op) == 3) {
          g_shadow_dx = dx; g_shadow_dy = dy; g_shadow_op = op;
          g_shadow_mode = 2;   // a numeric triple means "force with these values"
        }
      }
    }
  }
#endif

#ifdef __SWITCH__
  static const PlSharedFontType types[] = {
    PlSharedFontType_Standard,
    PlSharedFontType_NintendoExt,
    PlSharedFontType_ChineseSimplified,
    PlSharedFontType_ExtChineseSimplified,
    PlSharedFontType_ChineseTraditional,
    PlSharedFontType_KO,
  };
  for (unsigned i = 0; i < sizeof(types) / sizeof(*types) && g_face_count < MAX_FACES; i++) {
    PlFontData font;
    if (R_FAILED(plGetSharedFontByType(&font, types[i])))
      continue;
    if (FT_New_Memory_Face(g_ft, font.address, font.size, 0, &g_faces[g_face_count]) == 0)
      g_face_count++;
  }
#else
  // No Switch shared font on Linux; load TTFs bundled with the port (and a few
  // common system paths) for Latin + CJK coverage. Missing fonts degrade
  // gracefully (system-font labels just won't draw; the engine still boots).
  static const char *candidates[] = {
    "font-4x3.ttf", "font.ttf", "fonts/standard.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
  };
  const char *primary = NULL;
  for (unsigned i = 0; i < sizeof(candidates) / sizeof(*candidates) && g_face_count < MAX_FACES; i++) {
    if (i == 0 && !narrow) continue;                       // the 4:3 font only on narrow panels
    if (FT_New_Face(g_ft, candidates[i], 0, &g_faces[g_face_count]) == 0) {
      if (!primary) primary = candidates[i];
      g_face_count++;
    }
  }
#endif

  if (g_face_count > 0) detect_font_grid(g_faces[0]);
#ifndef __SWITCH__
  // font_snap: config 0 = auto (whole steps for the 4:3 font, half steps for
  // ChronoType), 1 / 2 explicit, 3 = off; env CT_FONT_SNAP wins.
  int snap = config.font_snap;
  { const char *s = getenv("CT_FONT_SNAP"); if (s) snap = atoi(s); }
  if (snap == 0) snap = narrow ? 1 : 2;
  g_snap = (snap == 1 || snap == 2) ? snap : 0;
  if (g_snap == 2 && (g_font_grid & 1)) g_snap = 1;   // half of an odd grid is off-grid: whole steps only
  fprintf(stderr, "gfx: %d font face(s) (%s), pixel grid %d px/em, %s panel, font_snap %d, font_scale %g\n",
          g_face_count, primary ? primary : "none", g_font_grid, narrow ? "narrow" : "wide",
          g_snap, (double)g_font_scale);
#endif
  g_ok = g_face_count > 0;
  if (!g_ok)
    debugPrintf("gfx: no shared fonts available\n");
}

// minimal UTF-8 decoder: returns the codepoint and advances *p
static uint32_t utf8_next(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  uint32_t c = *s++;
  if (c >= 0xF0 && s[0] && s[1] && s[2]) {
    c = ((c & 0x07) << 18) | ((s[0] & 0x3F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    s += 3;
  } else if (c >= 0xE0 && s[0] && s[1]) {
    c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F);
    s += 2;
  } else if (c >= 0xC0 && s[0]) {
    c = ((c & 0x1F) << 6) | (s[0] & 0x3F);
    s += 1;
  }
  *p = (const char *)s;
  return c;
}

// pick the first face that has a glyph for cp and load+render it; returns the
// face used (with glyph rendered into its slot) or NULL.
static FT_Face load_glyph(uint32_t cp, int px) {
  for (int i = 0; i < g_face_count; i++) {
    if (FT_Get_Char_Index(g_faces[i], cp) == 0 && cp != ' ')
      continue;
    FT_Set_Pixel_Sizes(g_faces[i], 0, px);
    // the pixel-font treatment (unhinted on-grid render) is for the primary
    // face only; fallback outline faces (CJK) render normally
    if (FT_Load_Char(g_faces[i], cp, i == 0 ? glyph_load_flags(px) : FT_LOAD_RENDER) == 0)
      return g_faces[i];
  }
  // last resort: render with the primary face's notdef
  if (g_face_count > 0) {
    FT_Set_Pixel_Sizes(g_faces[0], 0, px);
    if (FT_Load_Char(g_faces[0], cp, glyph_load_flags(px)) == 0)
      return g_faces[0];
  }
  return NULL;
}

// Glyph cache. Rasterising each glyph via FreeType per call is expensive, and the
// engine rebuilds the text bitmap on EVERY typewriter-reveal frame (re-rendering
// the whole growing string -> O(n^2) renders while moving), which is the cause of
// the "text forming = slowdown" hitch. Cache rendered glyphs by (codepoint, px):
// direct-mapped, evict-on-collision. The working set (Latin + active dialogue
// glyphs at 1-2 sizes) fits easily, so reveals become cache hits, not re-renders.
typedef struct {
  uint32_t cp; int px; int valid;
  int adv, left, top, w, rows;
  unsigned char *buf;   // w*rows 8-bit coverage (pitch == w); NULL if empty/space
} Glyph;
#define GCACHE_SIZE 2048
static Glyph g_gcache[GCACHE_SIZE];

// Half-step pixel-font sizes (font_snap 2): a size that is an odd multiple of
// half the grid (24 px for a 16 px grid = 1.5x) is rendered at double size on
// the exact grid and then decimated -- every art pixel becomes 1 or 2 device
// px (a regular 1-2-1-2 pattern; a 2-px stroke lands on exactly 3). For
// ChronoType, whose strokes are 2 px on its grid, this is pixel-exact.
static int half_step_px(int px) {
  return g_font_grid > 0 && g_snap == 2 &&
         (px % g_font_grid) != 0 && (px % (g_font_grid / 2)) == 0;
}

// Does the primary (pixel) face have this codepoint? Codepoints served by a
// fallback outline face (CJK) must not go through the half-step decimation:
// point-sampling an anti-aliased bitmap drops strokes.
static int primary_has(uint32_t cp) {
  return g_face_count > 0 && (cp == ' ' || FT_Get_Char_Index(g_faces[0], cp) != 0);
}

static const Glyph *get_glyph(uint32_t cp, int px) {
  uint32_t idx = (cp * 2654435761u + (uint32_t)px * 2246822519u) & (GCACHE_SIZE - 1);
  Glyph *e = &g_gcache[idx];
  if (e->valid && e->cp == cp && e->px == px)
    return e;                                   // hit
  if (e->buf) { free(e->buf); e->buf = NULL; }  // evict any prior occupant
  e->valid = 1; e->cp = cp; e->px = px;
  e->adv = e->left = e->top = e->w = e->rows = 0;
  const int half = half_step_px(px) && primary_has(cp);
  FT_Face f = load_glyph(cp, half ? px * 2 : px); // the one FreeType render, on miss
  if (f) {
    FT_GlyphSlot sl = f->glyph;
    e->adv  = (int)(sl->advance.x >> 6);
    e->left = sl->bitmap_left;
    e->top  = sl->bitmap_top;
    e->w    = (int)sl->bitmap.width;
    e->rows = (int)sl->bitmap.rows;
    if (half) {                                   // 2x render -> device metrics (spaces too)
      e->adv  = (e->adv + 1) / 2;
      e->left = e->left / 2;
      e->top  = (e->top + 1) / 2;
    }
    if (e->w > 0 && e->rows > 0) {
      if (!half) {
        e->buf = (unsigned char *)malloc((size_t)e->w * e->rows);
        if (e->buf) {
          for (int ry = 0; ry < e->rows; ry++)
            memcpy(e->buf + (size_t)ry * e->w,
                   sl->bitmap.buffer + (size_t)ry * sl->bitmap.pitch, (size_t)e->w);
        } else { e->w = e->rows = 0; }
      } else {
        const int W2 = e->w, R2 = e->rows, pitch = (int)sl->bitmap.pitch;
        const int W = (W2 + 1) / 2, R = (R2 + 1) / 2;
        e->buf = (unsigned char *)malloc((size_t)W * R);
        if (e->buf) {
          for (int y = 0; y < R; y++)
            for (int x = 0; x < W; x++)
              e->buf[(size_t)y * W + x] = sl->bitmap.buffer[(size_t)(2 * y) * pitch + 2 * x];
          e->w = W; e->rows = R;
        } else { e->w = e->rows = 0; }
      }
    }
  }
  return e;
}

// measure the pixel width of a single line of text at px size
static int measure_line(const char *s, const char *end, int px) {
  int w = 0;
  const char *p = s;
  while (p < end && *p) {
    uint32_t cp = utf8_next(&p);
    if (cp == '\n') break;
    const Glyph *gl = get_glyph(cp, px);
    if (gl) w += gl->adv;
  }
  return w;
}

// Blit one line's glyph run into `out` in colour (r,g,b,a), shifted by (ox,oy).
//   over == 0 : keep-strongest-coverage (the original system-font blend; used
//               for the single-pass, no-shadow case so behaviour is unchanged).
//   over == 1 : premultiplied source-over (used when layering shadow + text).
static void draw_line_glyphs(unsigned char *out, int W, int H,
                             const char *s, const char *end, int rpx,
                             int pen_x0, int baseline,
                             int r, int g, int b, int a,
                             int ox, int oy, int over) {
  int pen_x = pen_x0;
  const char *q = s;
  while (q < end && *q) {
    uint32_t cp = utf8_next(&q);
    if (cp == '\n') break;
    const Glyph *gl = get_glyph(cp, rpx);
    if (!gl) continue;
    if (gl->buf) {
      const int gx = pen_x + gl->left + ox;
      const int gy = baseline - gl->top + oy;
      for (int ry = 0; ry < gl->rows; ry++) {
        const int dy = gy + ry;
        if (dy < 0 || dy >= H) continue;
        const uint8_t *srow = gl->buf + (size_t)ry * gl->w;  // pitch == w
        for (int rx = 0; rx < gl->w; rx++) {
          const int dx = gx + rx;
          if (dx < 0 || dx >= W) continue;
          const int cov = srow[rx];
          if (!cov) continue;
          const int af = cov * a / 255;       // source alpha (0..255)
          if (!af) continue;
          // premultiplied RGBA8888 (byte order R,G,B,A)
          unsigned char *px4 = out + ((size_t)dy * W + dx) * 4;
          if (!over) {
            // simple "over" against transparent: write the strongest coverage
            if (af > px4[3]) {
              px4[0] = (unsigned char)(r * af / 255);
              px4[1] = (unsigned char)(g * af / 255);
              px4[2] = (unsigned char)(b * af / 255);
              px4[3] = (unsigned char)af;
            }
          } else {
            // premultiplied source-over: out = src + dst*(1 - srcA)
            const int ia = 255 - af;
            px4[0] = (unsigned char)(r * af / 255 + px4[0] * ia / 255);
            px4[1] = (unsigned char)(g * af / 255 + px4[1] * ia / 255);
            px4[2] = (unsigned char)(b * af / 255 + px4[2] * ia / 255);
            px4[3] = (unsigned char)(af + px4[3] * ia / 255);
          }
        }
      }
    }
    pen_x += gl->adv;
  }
}

// Glyph metrics at the (possibly snapped) render size, and the line cell that
// holds them: the engine's requested cell, grown when a size snapped UP past
// the request (16 -> 20 px) has taller glyphs than that cell -- else the
// descenders are clipped (PixelMplus10: 19-row cell, 22-row glyphs, the y/g
// tails cut off).
static int snapped_cell(int rpx, int line_h_req, int *asc, int *desc) {
  FT_Set_Pixel_Sizes(g_faces[0], 0, rpx);
  *asc  = (int)(g_faces[0]->size->metrics.ascender >> 6);
  *desc = (int)(-(g_faces[0]->size->metrics.descender >> 6));
  if (*asc <= 0) *asc = (rpx * 4) / 5;
  if (*asc + *desc <= 0) *asc = rpx;
  return (line_h_req > *asc + *desc) ? line_h_req : *asc + *desc;
}

unsigned char *gfx_render_text_rgba(const char *text, int font_size,
                                    int r, int g, int b, int a,
                                    int align_h, int max_w, int max_h, int wrap,
                                    int shadow, double shadow_dx, double shadow_dy,
                                    double shadow_opacity,
                                    int *out_w, int *out_h) {
  if (!g_ok || !text)
    return NULL;
  int px = font_size > 0 ? font_size : 16;
  // Per-font visual-size scale (CT_FONT_SCALE). Pixel fonts like ChronoType render
  // glyphs small within their em, so scale the whole cell up to match the engine's
  // intended on-screen size. Default 1.0 (system-font behaviour).
  px = (int)(px * g_font_scale + 0.5f);
  if (px < 1) px = 1;

  // The Switch shared font is visually larger per pixel than the Android font the
  // engine's sizes target, so render glyphs a touch smaller (rpx) and centre them
  // in the full px line cell: matches Android's size without clipping descenders.
  int rpx = px - (px / 8 + 1);
  if (rpx < 1) rpx = 1;
  // Pixel fonts: snap the glyph size to the largest multiple of the native grid
  // that fits the line cell (see g_font_grid). The line cell (px) is left as
  // requested so the engine's layout metrics are unchanged; only the glyph
  // raster moves. If the engine constrains the label and the text would not
  // fit at that size, step down a multiple (a narrow stat cell gets 1x while a
  // wide button or dialog line keeps 2x).
  const int snap = (g_font_grid > 0 && g_snap);
  // step = the grid (mode 1: 1x, 2x, 3x ...) or half of it (mode 2: also
  // 1.5x, 2.5x ... via the double-render-and-decimate path in get_glyph).
  // Nearest step, ties up: the engine's sizes sit between clean ones (16 / 21 /
  // 29 px on 640x480), and flooring put the 21 px dialogue a whole step below
  // the 16 px labels' ratio (or, scaled x1.5, a step above: 32 px overflowed
  // the dialogue box). The fit loop below still steps down where a boxed
  // label would clip.
  const int step = (g_snap >= 2) ? g_font_grid / 2 : g_font_grid;
  int k = 0;
  if (snap) {
    k = (px + step / 2) / step;
    if (k * step < g_font_grid) k = g_font_grid / step;   // never below 1x
    rpx = k * step;
  }

  // cell/line metrics from the primary face at the full engine size
  FT_Set_Pixel_Sizes(g_faces[0], 0, px);
  int ascender = (int)(g_faces[0]->size->metrics.ascender >> 6);
  int descender = (int)(-(g_faces[0]->size->metrics.descender >> 6));
  int line_h = (int)(g_faces[0]->size->metrics.height >> 6);
  if (line_h <= 0) line_h = px + px / 4;
  if (ascender <= 0) ascender = (px * 4) / 5;

  const int line_h_req = line_h;
  int asc_r = 0, desc_r = 0;
  line_h = snapped_cell(rpx, line_h_req, &asc_r, &desc_r);

  // split into lines on '\n'; optionally greedy-wrap to max_w
  // (we collect line start/end byte ranges)
  #define MAX_LINES 256
  const char *ls[MAX_LINES];
  const char *le[MAX_LINES];
  int nlines = 0;
  int meas_w = 0;
  for (;;) {
    nlines = 0;
    meas_w = 0;
    const char *p = text;
    const char *line_start = text;
    while (*p && nlines < MAX_LINES) {
      const char *cur = p;
      uint32_t cp = utf8_next(&p);
      if (cp == '\n') {
        ls[nlines] = line_start; le[nlines] = cur; nlines++;
        line_start = p;
        continue;
      }
      if (wrap && max_w > 0) {
        int w = measure_line(line_start, p, rpx);
        if (w > max_w && cur != line_start) {
          // break before the current glyph (prefer a previous space if any)
          const char *brk = cur;
          for (const char *q = cur; q > line_start; q--) {
            if (*q == ' ') { brk = q; break; }
          }
          ls[nlines] = line_start; le[nlines] = brk; nlines++;
          line_start = (*brk == ' ') ? brk + 1 : brk;
          p = line_start;
        }
      }
    }
    if (nlines < MAX_LINES) { ls[nlines] = line_start; le[nlines] = p; nlines++; }

    // measured width = widest line
    for (int i = 0; i < nlines; i++) {
      int w = measure_line(ls[i], le[i], rpx);
      if (w > meas_w) meas_w = w;
    }
    // snapped size does not fit the engine's box: try the next multiple down
    if (snap && k * step > g_font_grid &&
        ((max_w > 0 && meas_w > max_w) || (max_h > 0 && nlines * line_h > max_h))) {
      k--;
      rpx = k * step;
      line_h = snapped_cell(rpx, line_h_req, &asc_r, &desc_r);
      continue;
    }
    break;
  }
  int meas_h = nlines * line_h;
  if (meas_h < ascender + descender) meas_h = ascender + descender;

  // snapped-size glyph metrics (asc_r/desc_r), for vertical centring within
  // each line cell
  int content_h = asc_r + desc_r;
  if (content_h <= 0) content_h = rpx;
  int top_pad = (line_h - content_h) / 2;
  if (top_pad < 0) top_pad = 0;

  int W = max_w > 0 ? max_w : meas_w;
  int H = max_h > 0 ? max_h : meas_h;
  if (W <= 0) W = 1;
  if (H <= 0) H = 1;
  if (W > 4096) W = 4096;
  if (H > 4096) H = 4096;

  unsigned char *out = calloc((size_t)W * H * 4, 1);
  if (!out)
    return NULL;

  // Resolve the SNES-style drop-shadow for this draw. In auto mode we honour
  // the engine's request (offsets are in font-size space, so scale them like
  // the glyph metrics); force mode uses the env offsets as final pixels.
  int sh_on = 0, sh_ox = 0, sh_oy = 0, sh_a = 0;
  if (g_shadow_mode == 2) {
    sh_on = 1;
    sh_ox = (int)(g_shadow_dx >= 0 ? g_shadow_dx + 0.5f : g_shadow_dx - 0.5f);
    sh_oy = (int)(g_shadow_dy >= 0 ? g_shadow_dy + 0.5f : g_shadow_dy - 0.5f);
    sh_a  = (int)(g_shadow_op * 255.0f + 0.5f);
  } else if (g_shadow_mode == 1 && shadow && shadow_opacity > 0.0) {
    sh_on = 1;
    double sx = shadow_dx * g_font_scale, sy = shadow_dy * g_font_scale;
    sh_ox = (int)(sx >= 0 ? sx + 0.5 : sx - 0.5);
    sh_oy = (int)(sy >= 0 ? sy + 0.5 : sy - 0.5);
    sh_a  = (int)(shadow_opacity * 255.0 + 0.5);
  }
  if (sh_on) {
    if (sh_a < 1) sh_on = 0;                       // fully transparent → no shadow
    else if (sh_a > 255) sh_a = 255;
    if (sh_ox == 0 && sh_oy == 0) { sh_ox = 1; sh_oy = 1; } // ensure it's visible
  }

  // Pass 0: the drop-shadow (black, offset), composited source-over so it lays
  // cleanly beneath. Pass 1: the text itself. With no shadow, only pass 1 runs
  // and uses the original keep-strongest blend (unchanged behaviour).
  for (int pass = (sh_on ? 0 : 1); pass < 2; pass++) {
    for (int li = 0; li < nlines; li++) {
      int lw = measure_line(ls[li], le[li], rpx);
      int pen_x = 0;
      if (align_h == GFX_ALIGN_CENTER) pen_x = (W - lw) / 2;
      else if (align_h == GFX_ALIGN_RIGHT) pen_x = W - lw;
      if (pen_x < 0) pen_x = 0;
      // baseline of the reduced-size glyphs, centred in this line's px cell
      const int baseline = li * line_h + top_pad + asc_r;
      if (pass == 0)
        draw_line_glyphs(out, W, H, ls[li], le[li], rpx, pen_x, baseline,
                         0, 0, 0, sh_a, sh_ox, sh_oy, /*over=*/1);
      else
        draw_line_glyphs(out, W, H, ls[li], le[li], rpx, pen_x, baseline,
                         r, g, b, a, 0, 0, /*over=*/sh_on);
    }
  }

  if (out_w) *out_w = W;
  if (out_h) *out_h = H;
  return out;
}
