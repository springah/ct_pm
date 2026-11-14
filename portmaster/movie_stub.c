// movie_stub.c — Linux placeholder for source/movie_player.c, for quick
// boot-to-title builds without FFmpeg. movie_play returns 0 ("not found"), which
// makes the engine skip the cutscene and continue. Must mirror every entry point
// main.c calls on movie_player.h, or the swap does not link.
#include "movie_player.h"

int  movie_play(const char *name)              { (void)name; return 0; }
void movie_set_gl_invalidate(void (*fn)(void)) { (void)fn; }
void movie_set_input_drain(void (*fn)(void))   { (void)fn; }
