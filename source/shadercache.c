/* shadercache.c -- on-disk GL program binary cache (Linux/PortMaster only)
 *
 * The engine (cocos2d-x GLProgram) recreates its GL programs per scene from
 * the same handful of shader sources -- a CT_IOLOG capture measured
 * example_Simple.vsh and ShaderDrawPalettedTexture.fsh each read 263x in one
 * session, i.e. a driver compile+link per scene change. On the PowerVR and
 * Mali drivers that compile is main-thread stall at scene entry, and it is
 * the residual hitch left after the asset reopen pooling.
 *
 * The interpose defers glCompileShader (compile status is answered
 * optimistically; the sources are fixed shipped assets) and satisfies
 * glLinkProgram from a GL_OES_get_program_binary blob cached under
 * shadercache/, keyed by a hash of the attached sources, the pre-link
 * glBindAttribLocation set, and the driver's renderer/version strings. A
 * miss compiles and links for real, then stores the fresh binary. Any
 * restore failure falls back to the real path and rewrites the entry, so a
 * stale cache self-heals; a driver reporting zero binary formats disables
 * the whole scheme at first link (the deferred compiles are then simply
 * issued late).
 *
 * Gated by config.txt `shader_cache` / env CT_SHADER_CACHE; CT_SHADER_LOG=1
 * traces hits/misses/stores to log.txt.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>

#include "config.h"
#include "shadercache.h"

#ifndef GL_PROGRAM_BINARY_LENGTH_OES
#define GL_PROGRAM_BINARY_LENGTH_OES 0x8741
#endif
#ifndef GL_NUM_PROGRAM_BINARY_FORMATS_OES
#define GL_NUM_PROGRAM_BINARY_FORMATS_OES 0x87FE
#endif

typedef void (*PFN_GetProgramBinary)(GLuint program, GLsizei bufSize,
                                     GLsizei *length, GLenum *format, void *binary);
typedef void (*PFN_ProgramBinary)(GLuint program, GLenum format,
                                  const void *binary, GLint length);

#define SC_DIR          "shadercache"
#define SC_MAGIC        0x43545343u // "CTSC"
#define SC_VERSION      1u
#define SC_MAX_SHADERS  1024
#define SC_MAX_PROGRAMS 512
#define SC_MAX_ATTRIBS  16
#define SC_MAX_BLOB     (8 * 1024 * 1024)

typedef struct {
  GLuint id; // 0 = free slot
  char *src;
  unsigned len;
  int deferred; // engine asked for a compile we have not issued yet
  int compiled; // the real glCompileShader has been issued
  unsigned serial; // distinguishes reuses of the same GL id (see ProgRec)
} ShaderRec;

typedef struct {
  char name[32];
  GLuint index;
} AttribBind;

typedef struct {
  GLuint id; // 0 = free slot
  GLuint vs, fs;
  unsigned vs_serial, fs_serial; // must match the ShaderRec's, or the GL id
                                 // was recycled and the record is a stranger
  AttribBind attribs[SC_MAX_ATTRIBS];
  int nattribs;
  unsigned long long last_key; // survives the engine deleting the shaders
} ProgRec;

static ShaderRec g_shaders[SC_MAX_SHADERS];
static ProgRec g_programs[SC_MAX_PROGRAMS];

static int g_state = 0; // 0 = before first link, 1 = enabled, -1 = disabled
static PFN_GetProgramBinary p_GetProgramBinary;
static PFN_ProgramBinary p_ProgramBinary;
static unsigned long long g_driver_tag;
static int g_log = 0;
static unsigned g_hits, g_misses, g_stores;

// Deferral has to be decided before init can run (init needs a current GL
// context, so it waits for the first glLinkProgram); config alone gates it.
// Worst case a deferred compile is issued at link time once init disables.
static int sc_wanted(void) {
#ifdef __SWITCH__
  return 0; // native res + hardware driver; the Switch build stays untouched
#else
  static int wanted = -1;
  if (wanted < 0) {
    const char *e = getenv("CT_SHADER_CACHE");
    wanted = e ? atoi(e) != 0 : config.shader_cache != 0;
  }
  return wanted;
#endif
}

static unsigned long long fnv1a(unsigned long long h, const void *data, size_t n) {
  const unsigned char *p = (const unsigned char *)data;
  while (n--) {
    h ^= *p++;
    h *= 0x100000001b3ull;
  }
  return h;
}
#define FNV_SEED 0xcbf29ce484222325ull

static ShaderRec *sh_find(GLuint id) {
  if (!id) return NULL;
  for (int i = 0; i < SC_MAX_SHADERS; i++)
    if (g_shaders[i].id == id) return &g_shaders[i];
  return NULL;
}

static ShaderRec *sh_alloc(GLuint id) {
  static unsigned serial;
  ShaderRec *s = sh_find(id);
  if (s) return s;
  for (int i = 0; i < SC_MAX_SHADERS; i++) {
    if (g_shaders[i].id == 0) {
      g_shaders[i].id = id;
      g_shaders[i].serial = ++serial;
      return &g_shaders[i];
    }
  }
  return NULL; // table full: the shader just goes uncached
}

static ProgRec *prog_find(GLuint id) {
  if (!id) return NULL;
  for (int i = 0; i < SC_MAX_PROGRAMS; i++)
    if (g_programs[i].id == id) return &g_programs[i];
  return NULL;
}

static ProgRec *prog_alloc(GLuint id) {
  ProgRec *p = prog_find(id);
  if (p) return p;
  for (int i = 0; i < SC_MAX_PROGRAMS; i++) {
    if (g_programs[i].id == 0) {
      memset(&g_programs[i], 0, sizeof(ProgRec));
      g_programs[i].id = id;
      return &g_programs[i];
    }
  }
  return NULL;
}

static void sc_init(void) {
  g_log = getenv("CT_SHADER_LOG") != NULL;
  if (!sc_wanted()) {
    g_state = -1;
    return;
  }
  GLint nformats = 0;
  glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS_OES, &nformats);
  while (glGetError() != GL_NO_ERROR) {} // an ES2-only driver raises INVALID_ENUM
  p_GetProgramBinary = (PFN_GetProgramBinary)eglGetProcAddress("glGetProgramBinaryOES");
  if (!p_GetProgramBinary)
    p_GetProgramBinary = (PFN_GetProgramBinary)eglGetProcAddress("glGetProgramBinary");
  p_ProgramBinary = (PFN_ProgramBinary)eglGetProcAddress("glProgramBinaryOES");
  if (!p_ProgramBinary)
    p_ProgramBinary = (PFN_ProgramBinary)eglGetProcAddress("glProgramBinary");
  if (nformats <= 0 || !p_GetProgramBinary || !p_ProgramBinary) {
    fprintf(stderr, "shadercache: disabled (driver reports %d binary formats)\n",
            (int)nformats);
    g_state = -1;
    return;
  }
  const char *renderer = (const char *)glGetString(GL_RENDERER);
  const char *version = (const char *)glGetString(GL_VERSION);
  if (!renderer) renderer = "?";
  if (!version) version = "?";
  // A driver update invalidates every blob wholesale via the filename tag; a
  // stale entry that slips through fails glProgramBinary and is rewritten.
  g_driver_tag = fnv1a(FNV_SEED, renderer, strlen(renderer));
  g_driver_tag = fnv1a(g_driver_tag, version, strlen(version));
  mkdir(SC_DIR, 0755);
  g_state = 1;
  fprintf(stderr, "shadercache: enabled (%d formats, %s)\n", (int)nformats, renderer);
}

// A ShaderRec only speaks for a ProgRec's attachment if the serial recorded at
// glAttachShader still matches -- otherwise the engine deleted that shader and
// the driver recycled its id onto a different one (a stranger with different
// sources, which would silently mis-key the program).
static ShaderRec *attached_shader(GLuint id, unsigned serial) {
  ShaderRec *s = sh_find(id);
  return (s && s->serial == serial) ? s : NULL;
}

static void compile_attached(ProgRec *pr) {
  ShaderRec *recs[2] = { attached_shader(pr->vs, pr->vs_serial),
                         attached_shader(pr->fs, pr->fs_serial) };
  for (int i = 0; i < 2; i++) {
    ShaderRec *s = recs[i];
    if (s && s->deferred && !s->compiled) {
      glCompileShader(s->id);
      s->compiled = 1;
      s->deferred = 0;
    }
  }
}

static unsigned long long prog_key(ProgRec *pr) {
  unsigned long long h = FNV_SEED;
  GLuint ids[2] = { pr->vs, pr->fs };
  unsigned serials[2] = { pr->vs_serial, pr->fs_serial };
  int have = 0;
  for (int i = 0; i < 2; i++) {
    if (!ids[i]) continue;
    ShaderRec *s = attached_shader(ids[i], serials[i]);
    if (!s || !s->src) return 0; // unrecorded or recycled id: cannot key this
    h = fnv1a(h, "|", 1);
    h = fnv1a(h, s->src, s->len);
    have = 1;
  }
  if (!have) return 0;
  for (int i = 0; i < pr->nattribs; i++) {
    h = fnv1a(h, pr->attribs[i].name, strlen(pr->attribs[i].name));
    h = fnv1a(h, &pr->attribs[i].index, sizeof(GLuint));
  }
  return h ? h : 1;
}

static void sc_path(char *buf, size_t n, unsigned long long key) {
  snprintf(buf, n, SC_DIR "/%016llx-%016llx.bin", g_driver_tag, key);
}

static int try_restore(GLuint program, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  unsigned hdr[4] = { 0 }; // magic, version, format, length
  void *blob = NULL;
  if (fread(hdr, sizeof(hdr), 1, f) != 1 || hdr[0] != SC_MAGIC ||
      hdr[1] != SC_VERSION || hdr[3] == 0 || hdr[3] > SC_MAX_BLOB)
    goto bad;
  blob = malloc(hdr[3]);
  if (!blob || fread(blob, hdr[3], 1, f) != 1)
    goto bad;
  fclose(f);
  p_ProgramBinary(program, (GLenum)hdr[2], blob, (GLint)hdr[3]);
  free(blob);
  while (glGetError() != GL_NO_ERROR) {} // a rejected blob raises an error
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    unlink(path); // stale (driver update, foreign file); the miss rewrites it
    return 0;
  }
  return 1;
bad:
  fclose(f);
  free(blob);
  unlink(path);
  return 0;
}

static void store(GLuint program, const char *path) {
  GLint blen = 0;
  glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH_OES, &blen);
  while (glGetError() != GL_NO_ERROR) {}
  if (blen <= 0 || blen > SC_MAX_BLOB) return;
  void *blob = malloc((size_t)blen);
  if (!blob) return;
  GLsizei got = 0;
  GLenum fmt = 0;
  p_GetProgramBinary(program, blen, &got, &fmt, blob);
  if (glGetError() != GL_NO_ERROR || got <= 0) {
    free(blob);
    return;
  }
  char tmp[288];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (f) {
    unsigned hdr[4] = { SC_MAGIC, SC_VERSION, (unsigned)fmt, (unsigned)got };
    if (fwrite(hdr, sizeof(hdr), 1, f) == 1 && fwrite(blob, (size_t)got, 1, f) == 1) {
      fclose(f);
      rename(tmp, path);
      g_stores++;
      if (g_log)
        fprintf(stderr, "shadercache: stored %s (%d bytes)\n", path, (int)got);
    } else {
      fclose(f);
      unlink(tmp);
    }
  }
  free(blob);
}

// --- the interpose wrappers -----------------------------------------------

void ct_sc_glShaderSource(GLuint shader, GLsizei count,
                          const GLchar *const *string, const GLint *length) {
  glShaderSource(shader, count, string, length);
  if (!sc_wanted() || g_state < 0) return;
  ShaderRec *s = sh_alloc(shader);
  if (!s) return;
  size_t total = 0;
  for (GLsizei i = 0; i < count; i++) {
    if (!string[i]) continue;
    total += (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
  }
  char *src = (char *)malloc(total + 1);
  if (!src) return;
  size_t off = 0;
  for (GLsizei i = 0; i < count; i++) {
    if (!string[i]) continue;
    size_t n = (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
    memcpy(src + off, string[i], n);
    off += n;
  }
  src[off] = 0;
  free(s->src);
  s->src = src;
  s->len = (unsigned)off;
  s->deferred = 0;
  s->compiled = 0;
}

void ct_sc_glCompileShader(GLuint shader) {
  if (sc_wanted() && g_state >= 0) {
    ShaderRec *s = sh_find(shader);
    if (s && s->src) {
      s->deferred = 1; // issued for real only on a cache miss at link
      return;
    }
  }
  glCompileShader(shader);
}

void ct_sc_glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
  ShaderRec *s = sh_find(shader);
  if (s && s->deferred && !s->compiled) {
    // The compile has not run; answer as a success would. The sources are
    // fixed shipped assets, so a real failure here would be a first.
    if (pname == GL_COMPILE_STATUS) { *params = GL_TRUE; return; }
    if (pname == GL_INFO_LOG_LENGTH) { *params = 0; return; }
  }
  glGetShaderiv(shader, pname, params);
}

void ct_sc_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length,
                              GLchar *infoLog) {
  ShaderRec *s = sh_find(shader);
  if (s && s->deferred && !s->compiled) {
    if (bufSize > 0 && infoLog) infoLog[0] = 0;
    if (length) *length = 0;
    return;
  }
  glGetShaderInfoLog(shader, bufSize, length, infoLog);
}

void ct_sc_glAttachShader(GLuint program, GLuint shader) {
  glAttachShader(program, shader);
  if (!sc_wanted() || g_state < 0) return;
  ShaderRec *s = sh_find(shader);
  ProgRec *pr = prog_alloc(program);
  if (!pr) {
    // Untracked program: no record will drive the late compile at link, so a
    // deferred compile would be orphaned (link on a never-compiled shader).
    // Issue it now instead.
    if (s && s->deferred && !s->compiled) {
      glCompileShader(s->id);
      s->compiled = 1;
      s->deferred = 0;
    }
    return;
  }
  GLint type = 0;
  glGetShaderiv(shader, GL_SHADER_TYPE, &type);
  if (type == GL_VERTEX_SHADER) {
    pr->vs = shader;
    pr->vs_serial = s ? s->serial : 0;
  } else if (type == GL_FRAGMENT_SHADER) {
    pr->fs = shader;
    pr->fs_serial = s ? s->serial : 0;
  }
}

void ct_sc_glDetachShader(GLuint program, GLuint shader) {
  glDetachShader(program, shader);
  ProgRec *pr = prog_find(program);
  if (pr) {
    if (pr->vs == shader) pr->vs = 0;
    if (pr->fs == shader) pr->fs = 0;
  }
}

void ct_sc_glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) {
  glBindAttribLocation(program, index, name);
  if (!sc_wanted() || g_state < 0 || !name) return;
  ProgRec *pr = prog_alloc(program); // cocos binds before attach is also fine
  if (!pr) return;
  for (int i = 0; i < pr->nattribs; i++) {
    if (!strcmp(pr->attribs[i].name, name)) {
      pr->attribs[i].index = index;
      return;
    }
  }
  if (pr->nattribs < SC_MAX_ATTRIBS) {
    strlcpy(pr->attribs[pr->nattribs].name, name, sizeof(pr->attribs[0].name));
    pr->attribs[pr->nattribs].index = index;
    pr->nattribs++;
  }
}

void ct_sc_glLinkProgram(GLuint program) {
  if (g_state == 0) sc_init();
  ProgRec *pr = prog_find(program);
  if (!pr) { // nothing recorded (cache off from the start, or table overflow)
    glLinkProgram(program);
    return;
  }
  if (g_state < 0) {
    compile_attached(pr);
    glLinkProgram(program);
    return;
  }
  unsigned long long key = prog_key(pr);
  if (!key) key = pr->last_key; // re-link after the engine deleted the shaders
  if (!key) {
    compile_attached(pr);
    glLinkProgram(program);
    return;
  }
  pr->last_key = key;
  char path[256];
  sc_path(path, sizeof(path), key);
  if (try_restore(program, path)) {
    g_hits++;
    if (g_log)
      fprintf(stderr, "shadercache: hit %016llx (%u hits / %u misses)\n",
              key, g_hits, g_misses);
    return;
  }
  g_misses++;
  compile_attached(pr);
  glLinkProgram(program);
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (ok) store(program, path);
}

void ct_sc_glDeleteShader(GLuint shader) {
  glDeleteShader(shader);
  ShaderRec *s = sh_find(shader);
  if (s) {
    free(s->src);
    memset(s, 0, sizeof(ShaderRec));
  }
}

void ct_sc_glDeleteProgram(GLuint program) {
  glDeleteProgram(program);
  ProgRec *pr = prog_find(program);
  if (pr)
    memset(pr, 0, sizeof(ProgRec));
}
