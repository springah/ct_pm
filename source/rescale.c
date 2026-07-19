/* rescale.c -- reduced internal render resolution (Linux/PortMaster only)
 *
 * The PortMaster handhelds are GPU-bound running the engine at the panel's
 * native size (TrimUI Smart Pro: PowerVR GE8300 at a fixed 702 MHz driving
 * 1280x720 -- heavy field/battle scenes sit at 34-44 fps). The GPU clock is
 * not adjustable on these BSPs, so the one real lever is pixels drawn:
 *
 *   - main() tells nativeInit a reduced internal size, so every engine-side
 *     viewport/scissor/RenderTexture is natively sized to it;
 *   - the import table (imports.c) redirects the engine's binds of the
 *     default framebuffer into an offscreen FBO of that size;
 *   - ct_rescale_present() draws the FBO texture to the real backbuffer as a
 *     linear-filtered fullscreen quad just before the swap.
 *
 * The engine never knows. Our own native overlays (movie_player.c FMVs, the
 * osk.c keyboard) keep drawing at full panel resolution by bracketing
 * themselves with ct_rescale_suspend()/resume().
 *
 * Configured by config.txt `render_scale` (default 0.75 -> 960x540 on a 720p
 * panel; 1 disables) or the CT_RENDER_SCALE env override.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __SWITCH__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "config.h"
#include "rescale.h"
#include "os.h"

static int g_active = 0;
static int g_suspended = 0;
static int g_win_w = 1280, g_win_h = 720;
static int g_int_w = 0, g_int_h = 0;

static GLuint g_fbo, g_color, g_depth, g_stencil;
static GLuint g_prog, g_vsh, g_fsh;
static GLint g_aPos, g_aTex, g_uTex;

static void (*g_gl_invalidate)(void); // cocos2d::GL::invalidateStateCache

void ct_rescale_set_gl_invalidate(void (*fn)(void)) { g_gl_invalidate = fn; }

int ct_rescale_active(void) { return g_active; }

void ct_rescale_engine_size(int *w, int *h) {
  if (w) *w = g_active ? g_int_w : g_win_w;
  if (h) *h = g_active ? g_int_h : g_win_h;
}

unsigned ct_rescale_redirect_fb(unsigned fb) {
  return (g_active && fb == 0) ? g_fbo : fb;
}

static const char *VSH =
  "attribute vec2 aPos; attribute vec2 aTex; varying vec2 vTex;"
  "void main(){ vTex=aTex; gl_Position=vec4(aPos,0.0,1.0); }";
static const char *FSH =
  "precision mediump float; varying vec2 vTex; uniform sampler2D uTex;"
  "void main(){ gl_FragColor=texture2D(uTex,vTex); }";

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  return ok ? s : (glDeleteShader(s), 0u);
}

static void destroy_all(void) {
  if (g_prog) glDeleteProgram(g_prog);
  if (g_vsh) glDeleteShader(g_vsh);
  if (g_fsh) glDeleteShader(g_fsh);
  if (g_fbo) glDeleteFramebuffers(1, &g_fbo);
  if (g_color) glDeleteTextures(1, &g_color);
  if (g_depth) glDeleteRenderbuffers(1, &g_depth);
  if (g_stencil) glDeleteRenderbuffers(1, &g_stencil);
  g_prog = g_vsh = g_fsh = g_fbo = g_color = g_depth = g_stencil = 0;
}

static float pick_scale(void) {
  float s = config.render_scale;
  const char *env = getenv("CT_RENDER_SCALE");
  if (env && *env) s = (float)atof(env);
  if (s <= 0.0f) s = 1.0f; // unset/garbage -> native
  if (s < 0.3f) s = 0.3f;
  return s;
}

void ct_rescale_setup(int win_w, int win_h) {
  g_win_w = win_w; g_win_h = win_h;

  const float s = pick_scale();
  if (s >= 0.995f)
    return; // native resolution: interpose fully disabled, zero overhead

  int iw = ((int)((float)win_w * s + 0.5f)) & ~1;
  int ih = ((int)((float)win_h * s + 0.5f)) & ~1;
  if (iw < 320) iw = 320;
  if (ih < 240) ih = 240;
  if (iw >= win_w || ih >= win_h)
    return; // panel already small enough that scaling gains nothing

  while (glGetError() != GL_NO_ERROR) {} // don't misattribute stale errors

  glGenTextures(1, &g_color);
  glBindTexture(GL_TEXTURE_2D, g_color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iw, ih, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenFramebuffers(1, &g_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color, 0);

  // Depth + stencil to match the engine's own surface (depth24/stencil8; cocos
  // ClippingNode needs working stencil). Prefer the packed OES format, fall
  // back to separate D16+S8 renderbuffers where packed isn't supported.
  glGenRenderbuffers(1, &g_depth);
  glBindRenderbuffer(GL_RENDERBUFFER, g_depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, iw, ih);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depth);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_depth);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, iw, ih);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depth);
    glGenRenderbuffers(1, &g_stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, g_stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, iw, ih);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_stencil);
  }

  int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (ok) {
    g_vsh = compile(GL_VERTEX_SHADER, VSH);
    g_fsh = compile(GL_FRAGMENT_SHADER, FSH);
    g_prog = glCreateProgram();
    glAttachShader(g_prog, g_vsh);
    glAttachShader(g_prog, g_fsh);
    glLinkProgram(g_prog);
    GLint linked = 0; glGetProgramiv(g_prog, GL_LINK_STATUS, &linked);
    ok = g_vsh && g_fsh && linked;
    if (ok) {
      g_aPos = glGetAttribLocation(g_prog, "aPos");
      g_aTex = glGetAttribLocation(g_prog, "aTex");
      g_uTex = glGetUniformLocation(g_prog, "uTex");
    }
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

  if (!ok) {
    destroy_all();
    fprintf(stderr, "ct: rescale FBO unavailable on this driver; "
                    "rendering at native %dx%d\n", win_w, win_h);
    return;
  }

  g_int_w = iw; g_int_h = ih;
  g_active = 1;
  os_log("rescale: engine renders %dx%d, presented at %dx%d (scale %.3g)\n",
         iw, ih, win_w, win_h, s);
}

void ct_rescale_begin_frame(void) {
  if (!g_active) return;
  g_suspended = 0;
  glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
  // The present blit leaves the window-sized viewport behind; restore the
  // internal one so the engine's default is right even before it sets its own.
  glViewport(0, 0, g_int_w, g_int_h);
}

void ct_rescale_present(void) {
  if (!g_active || g_suspended) return;

  // Snapshot the enable-caps we clobber; invalidateStateCache doesn't restore
  // caps (same contract as the movie/OSK overlays).
  GLboolean sv_scissor = glIsEnabled(GL_SCISSOR_TEST);
  GLboolean sv_depth   = glIsEnabled(GL_DEPTH_TEST);
  GLboolean sv_stencil = glIsEnabled(GL_STENCIL_TEST);
  GLboolean sv_blend   = glIsEnabled(GL_BLEND);
  GLboolean sv_cull    = glIsEnabled(GL_CULL_FACE);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, g_win_w, g_win_h);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);

  glUseProgram(g_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_color);
  glUniform1i(g_uTex, 0);
  glBindBuffer(GL_ARRAY_BUFFER, 0); // client-side attrib pointers below

  static const GLfloat quad[] = {
    -1.0f,  1.0f, 0.0f, 1.0f,
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
  };
  glEnableVertexAttribArray(g_aPos);
  glEnableVertexAttribArray(g_aTex);
  glVertexAttribPointer(g_aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
  glVertexAttribPointer(g_aTex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(g_aPos);
  glDisableVertexAttribArray(g_aTex);
  glUseProgram(0);

  if (sv_scissor) glEnable(GL_SCISSOR_TEST);
  if (sv_depth)   glEnable(GL_DEPTH_TEST);
  if (sv_stencil) glEnable(GL_STENCIL_TEST);
  if (sv_blend)   glEnable(GL_BLEND);
  if (sv_cull)    glEnable(GL_CULL_FACE);
  if (g_gl_invalidate) g_gl_invalidate(); // we changed GL state behind cocos's back
}

void ct_rescale_suspend(void) {
  if (!g_active || g_suspended) return;
  g_suspended = 1;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, g_win_w, g_win_h); // overlays size themselves off GL_VIEWPORT
}

void ct_rescale_resume(void) {
  if (!g_active || !g_suspended) return;
  g_suspended = 0;
  glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
  glViewport(0, 0, g_int_w, g_int_h);
}

#endif // !__SWITCH__
