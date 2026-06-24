// movie_stub.c — Linux placeholder for source/movie_player.c until the FMV path
// is wired to the PortMaster runtime's ffmpeg. movie_play returns 0 ("not found"),
// which makes the engine skip the cutscene and continue — fine for boot-to-title.
#include "movie_player.h"

int  movie_play(const char *name)            { (void)name; return 0; }
void movie_set_gl_invalidate(void (*fn)(void)) { (void)fn; }
