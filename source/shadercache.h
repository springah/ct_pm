/* shadercache.h -- on-disk GL program binary cache (Linux/PortMaster only)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __SHADERCACHE_H__
#define __SHADERCACHE_H__

#include <GLES2/gl2.h>

// Interpose wrappers for the engine's shader/program entry points (wired into
// all three GL resolution paths in imports.c, like the render-scale redirect).
// Passthrough when shader_cache is off, on the Switch, or when the driver
// reports no program binary formats.
void ct_sc_glShaderSource(GLuint shader, GLsizei count,
                          const GLchar *const *string, const GLint *length);
void ct_sc_glCompileShader(GLuint shader);
void ct_sc_glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
void ct_sc_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length,
                              GLchar *infoLog);
void ct_sc_glAttachShader(GLuint program, GLuint shader);
void ct_sc_glDetachShader(GLuint program, GLuint shader);
void ct_sc_glBindAttribLocation(GLuint program, GLuint index, const GLchar *name);
void ct_sc_glLinkProgram(GLuint program);
void ct_sc_glDeleteShader(GLuint shader);
void ct_sc_glDeleteProgram(GLuint program);

#endif // __SHADERCACHE_H__
