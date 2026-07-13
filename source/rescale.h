/* rescale.h -- reduced internal render resolution (Linux/PortMaster only)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __RESCALE_H__
#define __RESCALE_H__

#ifdef __SWITCH__
// The Switch renders at native resolution (vsync-locked 60); the interpose
// compiles away so shared call sites need no #ifdefs.
static inline void ct_rescale_setup(int w, int h) { (void)w; (void)h; }
static inline int  ct_rescale_active(void) { return 0; }
static inline void ct_rescale_engine_size(int *w, int *h) { (void)w; (void)h; }
static inline unsigned ct_rescale_redirect_fb(unsigned fb) { return fb; }
static inline void ct_rescale_begin_frame(void) {}
static inline void ct_rescale_present(void) {}
static inline void ct_rescale_suspend(void) {}
static inline void ct_rescale_resume(void) {}
static inline void ct_rescale_set_gl_invalidate(void (*fn)(void)) { (void)fn; }
#else

// Decide the internal size and create the FBO. Requires a current GL context
// (call after egl_init). Falls back to native resolution (inactive) on any
// failure or when render_scale/CT_RENDER_SCALE resolves to ~1.0.
void ct_rescale_setup(int win_w, int win_h);

int  ct_rescale_active(void);

// The size the engine must be told at nativeInit: internal when active,
// window otherwise. Leaves *w/*h untouched only if setup was never called.
void ct_rescale_engine_size(int *w, int *h);

// imports.c: redirect the engine's binds of the default framebuffer.
unsigned ct_rescale_redirect_fb(unsigned fb);

// Main loop: bind the FBO before e_nativeRender, upscale to the backbuffer
// before the swap.
void ct_rescale_begin_frame(void);
void ct_rescale_present(void);

// Native full-resolution overlays (FMV, on-screen keyboard): draw to the real
// backbuffer between suspend and resume.
void ct_rescale_suspend(void);
void ct_rescale_resume(void);

// cocos2d::GL::invalidateStateCache -- the present blit changes GL state
// behind cocos's back every frame (same trick as movie_player.c).
void ct_rescale_set_gl_invalidate(void (*fn)(void));

#endif // __SWITCH__
#endif // __RESCALE_H__
