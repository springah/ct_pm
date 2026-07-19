/* patches.h -- runtime ARM64 instruction patches for libchrono.so
 *
 * Public interface for the config-gated runtime patch groups. The patch tables
 * and apply machinery live in patches.c; see the file banner there for the full
 * description (per-entry old-word verification, the v2.1.5 fingerprint gate, and
 * the cursor_fix / remove_mobile_ui / controller_glyphs / fix_diagonal_movement
 * groups).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __PATCHES_H__
#define __PATCHES_H__

#include "so_util.h"  // so_module

// Apply every config-enabled patch group to `mod` (libchrono). MUST be called
// while mod->load_base is still writable (before so_finalize) and only after the
// v2.1.5 fingerprint check has passed -- each entry additionally verifies its
// expected old word, so a mismatched build is skipped per-site, never corrupted.
void apply_game_patches(so_module *mod);

#endif // __PATCHES_H__
