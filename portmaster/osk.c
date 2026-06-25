// osk.c — controller-driven on-screen keyboard backing the Linux swkbd* API.
//
// The Android/cocos text-entry paths (jni_fake.c open_ime + jni_ime_service) call
// the libnx software-keyboard surface (swkbdCreate/.../swkbdShow). On the Switch
// that opens the system IME applet; PortMaster handhelds have no system keyboard,
// so we render our own keyboard over the live GL surface and drive it with the
// gamepad. swkbdShow() is blocking (like the Switch applet and like movie_play),
// runs on the main/GL thread from jni_ime_service(), and returns the entered text.
//
// Rendering reuses gfx_render_text_rgba() (FreeType on the bundled ChronoType
// font) so the keyboard matches the game's look. GL state is saved/restored by
// calling cocos2d::GL::invalidateStateCache afterwards (osk_set_gl_invalidate),
// the same trick movie_player.c uses.
#define _GNU_SOURCE
#include "switch_compat.h"
#include "os.h"
#include "gfx.h"

#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// swkbd config shims (state is just initial text + max length)
// ---------------------------------------------------------------------------
Result swkbdCreate(SwkbdConfig *c, int n) { (void)n; if (c) { c->initial[0] = 0; c->maxlen = 64; } return 0; }
void   swkbdConfigMakePresetDefault(SwkbdConfig *c) { (void)c; }
void   swkbdConfigSetInitialText(SwkbdConfig *c, const char *s) { if (c && s) snprintf(c->initial, sizeof(c->initial), "%s", s); }
void   swkbdConfigSetStringLenMax(SwkbdConfig *c, u32 n) { if (c) c->maxlen = n ? (int)n : 64; }
void   swkbdClose(SwkbdConfig *c) { (void)c; }

static void (*g_gl_invalidate)(void); // cocos2d::GL::invalidateStateCache
void osk_set_gl_invalidate(void (*fn)(void)) { g_gl_invalidate = fn; }

static void (*g_input_drain)(void); // host: re-baseline the input edge-detector
void osk_set_input_drain(void (*fn)(void)) { g_input_drain = fn; }

// ---------------------------------------------------------------------------
// GL: one tinted-textured-quad program; a 1x1 white texture stands in for solids.
// ---------------------------------------------------------------------------
static const char *VSH =
  "attribute vec2 aPos; attribute vec2 aTex; varying vec2 vTex;"
  "void main(){ vTex=aTex; gl_Position=vec4(aPos,0.0,1.0); }";
static const char *FSH =
  "precision mediump float; varying vec2 vTex; uniform sampler2D uTex; uniform vec4 uColor;"
  "void main(){ gl_FragColor = uColor * texture2D(uTex,vTex); }";

static GLuint g_prog, g_white;
static GLint  g_aPos, g_aTex, g_uTex, g_uColor;
static int    g_vw = 1280, g_vh = 720;

static GLuint compile(GLenum t, const char *s) {
  GLuint sh = glCreateShader(t); glShaderSource(sh, 1, &s, NULL); glCompileShader(sh); return sh;
}
static int gl_setup(void) {
  GLuint vs = compile(GL_VERTEX_SHADER, VSH), fs = compile(GL_FRAGMENT_SHADER, FSH);
  g_prog = glCreateProgram();
  glAttachShader(g_prog, vs); glAttachShader(g_prog, fs); glLinkProgram(g_prog);
  GLint ok = 0; glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
  glDeleteShader(vs); glDeleteShader(fs);
  if (!ok) { os_log("osk: program link failed\n"); return 0; }
  g_aPos = glGetAttribLocation(g_prog, "aPos");
  g_aTex = glGetAttribLocation(g_prog, "aTex");
  g_uTex = glGetUniformLocation(g_prog, "uTex");
  g_uColor = glGetUniformLocation(g_prog, "uColor");
  unsigned char wpix[4] = { 255, 255, 255, 255 };
  glGenTextures(1, &g_white); glBindTexture(GL_TEXTURE_2D, g_white);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, wpix);
  return 1;
}
// pixel-space quad (top-left origin). tex==0 -> solid uColor; colour is premultiplied.
static void quad(float px, float py, float pw, float ph, GLuint tex,
                 float r, float g, float b, float a) {
  float L = px / g_vw * 2 - 1, R = (px + pw) / g_vw * 2 - 1;
  float T = 1 - py / g_vh * 2,  B = 1 - (py + ph) / g_vh * 2;
  const GLfloat v[] = { L,T,0,0,  L,B,0,1,  R,T,1,0,  R,B,1,1 };
  glUseProgram(g_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex ? tex : g_white);
  glUniform1i(g_uTex, 0);
  glUniform4f(g_uColor, r * a, g * a, b * a, a); // premultiplied
  glEnableVertexAttribArray(g_aPos); glEnableVertexAttribArray(g_aTex);
  glVertexAttribPointer(g_aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), v);
  glVertexAttribPointer(g_aTex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), v + 2);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

typedef struct { GLuint tex; int w, h; } Tex;
static Tex text_tex(const char *s, int fs) {
  int w = 0, h = 0;
  unsigned char *px = gfx_render_text_rgba(s, fs, 255, 255, 255, 255,
                                           GFX_ALIGN_LEFT, 0, 0, 0, &w, &h);
  Tex t = { 0, 0, 0 };
  if (!px || w <= 0 || h <= 0) { free(px); return t; }
  glGenTextures(1, &t.tex); glBindTexture(GL_TEXTURE_2D, t.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  // the engine may leave GL_UNPACK_ALIGNMENT at 4/8; our RGBA rows are exactly
  // w*4 bytes, so force byte alignment or odd-width glyphs get row-padded (shear).
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
  free(px); t.w = w; t.h = h; return t;
}
static void tex_free(Tex *t) { if (t->tex) glDeleteTextures(1, &t->tex); t->tex = 0; }
// draw a text texture centred in a pixel rect (scaled down to fit, never up past 1x)
static void draw_text_centred(Tex *t, float cx, float cy, float maxw, float maxh) {
  if (!t->tex) return;
  float s = 1.0f;
  if (t->w > maxw) s = maxw / t->w;
  if (t->h * s > maxh) s = maxh / t->h;
  float w = t->w * s, h = t->h * s;
  quad(cx - w / 2, cy - h / 2, w, h, t->tex, 1, 1, 1, 1);
}

// ---------------------------------------------------------------------------
// Keyboard model
// ---------------------------------------------------------------------------
enum { K_CHAR = 0, K_SHIFT, K_SPACE, K_DEL, K_OK };
typedef struct {
  char  label[12];
  int   ch;       // codepoint for char keys (-1 otherwise)
  int   special;  // K_*
  int   row;
  float x, y, w, h;
  Tex   tex;
} Key;

static const char *ROW_LC[4] = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
static const char *ROW_UC[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };

#define MAXKEYS 64
static Key  g_keys[MAXKEYS];
static int  g_nkeys;
static int  g_sel;
static int  g_shift;

static void build_keys(void) {
  for (int i = 0; i < g_nkeys; i++) tex_free(&g_keys[i].tex);
  g_nkeys = 0;

  const float pad = g_vw * 0.03f;
  const float areaW = g_vw - pad * 2;
  const float top = g_vh * 0.36f;          // keyboard grid starts here
  const float bottom = g_vh * 0.985f;      // ...and must fit by here
  const int   cols = 10;
  const int   nrows = 5;                    // 4 char rows + the function row
  const float gap = areaW * 0.012f;
  const float rgap = (bottom - top) * 0.05f;
  const float kh = ((bottom - top) - rgap * (nrows - 1)) / nrows;
  float kw = (areaW - gap * (cols - 1)) / cols;
  if (kw > kh * 1.4f) kw = kh * 1.4f;       // keep keys from getting too wide
  const char **rows = g_shift ? ROW_UC : ROW_LC;
  int fs = (int)(kh * 0.58f);

  for (int r = 0; r < 4; r++) {
    int n = (int)strlen(rows[r]);
    float rowW = n * kw + (n - 1) * gap;
    float x0 = (g_vw - rowW) / 2;
    float y = top + r * (kh + rgap);
    for (int c = 0; c < n && g_nkeys < MAXKEYS; c++) {
      Key *k = &g_keys[g_nkeys++];
      k->label[0] = rows[r][c]; k->label[1] = 0;
      k->ch = (unsigned char)rows[r][c]; k->special = K_CHAR; k->row = r;
      k->x = x0 + c * (kw + gap); k->y = y; k->w = kw; k->h = kh;
      k->tex = text_tex(k->label, fs);
    }
  }
  // function row: SHIFT  SPACE(wide)  DEL  OK
  {
    float y = top + 4 * (kh + rgap);
    struct { const char *lbl; int sp; float wmul; } F[4] = {
      { g_shift ? "shift*" : "shift", K_SHIFT, 1.6f },
      { "space", K_SPACE, 3.2f },
      { "del",   K_DEL,   1.6f },
      { "OK",    K_OK,    1.6f },
    };
    float blockW = cols * kw + (cols - 1) * gap; // match the char-key block width
    float total = 0; for (int i = 0; i < 4; i++) total += F[i].wmul;
    float unit = (blockW - gap * 3) / total;
    float x = (g_vw - blockW) / 2;
    for (int i = 0; i < 4 && g_nkeys < MAXKEYS; i++) {
      Key *k = &g_keys[g_nkeys++];
      snprintf(k->label, sizeof(k->label), "%s", F[i].lbl);
      k->ch = -1; k->special = F[i].sp; k->row = 4;
      k->w = unit * F[i].wmul; k->h = kh; k->x = x; k->y = y;
      k->tex = text_tex(F[i].lbl, fs);
      x += k->w + gap;
    }
  }
}

static float kcx(const Key *k) { return k->x + k->w / 2; }
static float kcy(const Key *k) { return k->y + k->h / 2; }

// move selection within/between rows; up/down pick nearest by horizontal centre.
static void nav(int dx, int dy) {
  Key *cur = &g_keys[g_sel];
  int best = -1; float bestd = 1e9f;
  if (dx) {
    for (int i = 0; i < g_nkeys; i++) {
      if (g_keys[i].row != cur->row) continue;
      float d = kcx(&g_keys[i]) - kcx(cur);
      if (dx > 0 && d > 1 && d < bestd) { bestd = d; best = i; }
      if (dx < 0 && d < -1 && -d < bestd) { bestd = -d; best = i; }
    }
  } else if (dy) {
    int want = cur->row + dy;
    for (int i = 0; i < g_nkeys; i++) {
      if (g_keys[i].row != want) continue;
      float d = kcx(&g_keys[i]) - kcx(cur); if (d < 0) d = -d;
      if (d < bestd) { bestd = d; best = i; }
    }
  }
  if (best >= 0) g_sel = best;
}

// ---------------------------------------------------------------------------
// swkbdShow — the blocking keyboard
// ---------------------------------------------------------------------------
Result swkbdShow(SwkbdConfig *c, char *out, size_t len) {
  if (!out || len == 0) return 1;
  if (!g_prog && !gl_setup()) return 1;

  int maxlen = c && c->maxlen > 0 ? c->maxlen : 64;
  if (maxlen > (int)len - 1) maxlen = (int)len - 1;
  char buf[1024] = { 0 };
  if (c && c->initial[0]) snprintf(buf, sizeof(buf), "%.*s", maxlen, c->initial);

  GLint vp[4] = { 0, 0, 0, 0 };
  glGetIntegerv(GL_VIEWPORT, vp);
  g_vw = vp[2] > 0 ? vp[2] : 1280;
  g_vh = vp[3] > 0 ? vp[3] : 720;

  // Snapshot the engine's GL enable-caps; the OSK toggles depth/cull/blend and
  // leaves attrib arrays enabled. invalidateStateCache won't restore the caps,
  // so save them here and put them back exactly before returning to the engine.
  GLboolean sv_depth = glIsEnabled(GL_DEPTH_TEST);
  GLboolean sv_cull  = glIsEnabled(GL_CULL_FACE);
  GLboolean sv_blend = glIsEnabled(GL_BLEND);

  g_shift = 0; g_sel = 0;
  build_keys();

  Tex t_title = text_tex("Enter name", (int)(g_vh * 0.055f));
  Tex t_buf = { 0, 0, 0 };
  char buf_shown[1024] = { 1 }; // force first render

  uint32_t prev = 0xFFFFFFFF; // no edges on the first frame
  int first = 1;
  int result = 1;       // default: cancel
  int done = 0;
  int frame = 0;
  const char *cap = getenv("CT_OSK_CAPTURE");

  while (!done) {
    os_input_state st;
    os_input_poll(&st);
    uint32_t b = st.buttons;
    if (first) { prev = b; first = 0; }
    uint32_t e = b & ~prev; // freshly pressed
    prev = b;

    if (st.quit) { result = 1; break; }
    if (e & OS_BTN_LEFT)  nav(-1, 0);
    if (e & OS_BTN_RIGHT) nav(+1, 0);
    if (e & OS_BTN_UP)    nav(0, -1);
    if (e & OS_BTN_DOWN)  nav(0, +1);
    if (e & OS_BTN_Y) { g_shift = !g_shift; build_keys(); }     // quick shift
    if (e & OS_BTN_B) { int n = (int)strlen(buf); if (n > 0) buf[n - 1] = 0; } // quick del
    if (e & OS_BTN_START) { result = 0; done = 1; }             // quick confirm
    if (e & OS_BTN_SELECT) { result = 1; done = 1; }            // cancel
    if (e & OS_BTN_A) {
      Key *k = &g_keys[g_sel];
      switch (k->special) {
        case K_CHAR: { int n = (int)strlen(buf); if (n < maxlen) { buf[n] = (char)k->ch; buf[n + 1] = 0; } } break;
        case K_SHIFT: g_shift = !g_shift; build_keys(); break;
        case K_SPACE: { int n = (int)strlen(buf); if (n < maxlen) { buf[n] = ' '; buf[n + 1] = 0; } } break;
        case K_DEL:   { int n = (int)strlen(buf); if (n > 0) buf[n - 1] = 0; } break;
        case K_OK:    result = 0; done = 1; break;
      }
    }

    // ---- render ----
    glViewport(0, 0, g_vw, g_vh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied
    glClearColor(0.04f, 0.05f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // panel
    quad(g_vw * 0.03f, g_vh * 0.06f, g_vw * 0.94f, g_vh * 0.88f, 0, 0.10f, 0.12f, 0.22f, 0.95f);
    // title
    draw_text_centred(&t_title, g_vw * 0.5f, g_vh * 0.135f, g_vw * 0.9f, g_vh * 0.08f);
    // text-entry box + current text
    quad(g_vw * 0.10f, g_vh * 0.20f, g_vw * 0.80f, g_vh * 0.10f, 0, 0.03f, 0.04f, 0.08f, 1.0f);
    if (strcmp(buf, buf_shown) != 0) {
      tex_free(&t_buf);
      t_buf = text_tex(buf[0] ? buf : " ", (int)(g_vh * 0.06f));
      snprintf(buf_shown, sizeof(buf_shown), "%s", buf);
    }
    draw_text_centred(&t_buf, g_vw * 0.5f, g_vh * 0.25f, g_vw * 0.76f, g_vh * 0.085f);

    // keys
    for (int i = 0; i < g_nkeys; i++) {
      Key *k = &g_keys[i];
      int seld = (i == g_sel);
      if (seld) quad(k->x - 2, k->y - 2, k->w + 4, k->h + 4, 0, 0.95f, 0.80f, 0.20f, 1.0f); // highlight
      quad(k->x, k->y, k->w, k->h, 0, seld ? 0.20f : 0.16f, seld ? 0.18f : 0.18f, seld ? 0.10f : 0.30f, 1.0f);
      draw_text_centred(&k->tex, kcx(k), kcy(k), k->w * 0.86f, k->h * 0.72f);
    }

    if (cap && frame == 20) os_gfx_capture("osk.ppm"); // debug: grab a rendered frame
    frame++;
    os_gfx_swap();
    os_sleep_ms(16);
  }

  if (result == 0) snprintf(out, len, "%s", buf);

  // cleanup
  tex_free(&t_title); tex_free(&t_buf);
  for (int i = 0; i < g_nkeys; i++) tex_free(&g_keys[i].tex);
  g_nkeys = 0;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4); // back to GL default for the engine
  glDisableVertexAttribArray(g_aPos);
  glDisableVertexAttribArray(g_aTex);
  if (sv_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
  if (sv_cull)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
  if (sv_blend) glEnable(GL_BLEND);      else glDisable(GL_BLEND);
  glUseProgram(0);
  if (g_gl_invalidate) g_gl_invalidate(); // we changed GL state behind cocos's back
  if (g_input_drain) g_input_drain(); // drop the button still held to confirm/cancel
  return result == 0 ? 0 : 1;
}
