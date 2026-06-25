/* movie_player.h -- FMV cutscene playback for the Chrono Trigger Switch port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __MOVIE_PLAYER_H__
#define __MOVIE_PLAYER_H__

// Play the named cutscene (e.g. "008.dat", an XOR-obfuscated MP4 in assets/).
// Blocking; returns when the clip ends or the player skips with A/B/+. Returns 0
// if the file was not found (caller should then just continue).
int movie_play(const char *name);

// Register cocos2d::GL::invalidateStateCache (must be resolved at boot, before
// so_finalize locks the symbol tables) so the engine re-applies its GL state
// cache after a movie.
void movie_set_gl_invalidate(void (*fn)(void));

// Register a callback run when the movie hands control back to the engine, so the
// host can re-baseline its input edge-detector and drop a button still held to
// skip the clip (otherwise the main loop reports it to the game as a fresh press).
void movie_set_input_drain(void (*fn)(void));

#endif
