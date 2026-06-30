/* movelog.h -- CT_MOVELOG: per-frame diagonal-movement stutter logger
 *
 * Hooks FieldImpl::NanameHantei() via a trampoline written before so_finalize(),
 * then (after each original call) reads the per-frame dx/dy step outputs and
 * sub-pixel accumulators out of the FieldImpl move-state struct and logs them.
 *
 * Enable: CT_MOVELOG=1  (set in the PortMaster launcher or shell before running ct)
 * Output: goes to stderr, which PortMaster captures in log.txt
 *
 * Offset summary (libchrono.so v2.1.5, AArch64):
 *
 *   FieldImpl::NanameHantei() @ libchrono+0x59acfc
 *
 *   FieldImpl move-state pointer: FieldImpl* this  ->  this[2152] = move_state*
 *   (first instruction of NanameHantei: ldr x8,[x0,#2152])
 *
 *   move_state fields written by NanameHantei / GetMain / GetSub:
 *     +304  w: direction_6bit   -- raw 6-bit direction code extracted from ctrl byte
 *     +308  w: ctrl_byte_raw    -- raw input-state byte (ubfx'd to get direction)
 *     +312  d: subpix_xy_lo     -- 64-bit SIMD word; lo32=[+312] X sub-pixel state,
 *                                  hi32=[+316] Y sub-pixel state (both packed)
 *     +316  w: subpix_y         -- Y sub-pixel fractional accumulator (hi lane of above)
 *     +320  w: speed_main       -- main-axis pixel-per-frame speed (0 when idle)
 *     +324  w: speed_sub        -- sub-axis speed (diagonal only)
 *     +336  b: dir_mask         -- direction bitmask byte (bit7 = "diagonal" flag)
 *     +340  b: dir_state2       -- secondary direction state byte
 *     +344  w: dx               -- X step applied this frame (key diagnostic field)
 *     +348  w: dy_flag          -- Y direction flag / fractional carry
 *     +352  w: dy               -- Y step applied this frame (key diagnostic field)
 *     +356  w: dx2              -- secondary X step (diagonal cross-axis component)
 *
 * Cross-checked at libchrono+0x583638:
 *   ldr w8, [x21, #344]   <- reads dx  after NanameHantei returns
 *   ldr w22,[x21, #352]   <- reads dy  after NanameHantei returns
 *   ldr w23,[x21, #356]   <- reads dx2 after NanameHantei returns
 *
 * "is_diagonal" tag: bit7 of move_state[+336] is set when the engine classifies
 * movement as diagonal (NanameHantei takes the GetSub branch which enters with
 * dir_mask bit7 set; cardinal movement goes to GetMain with bit7 clear).
 *
 * UNVERIFIED offsets (safe to read, but interpret with care):
 *   The integer player tile-map coordinates are NOT in the move_state struct --
 *   they live in a parent cSfcWork / FIELD_MAP struct. Those are not logged here
 *   because finding them would require a multi-hop pointer chain with no stable
 *   global anchor. The sub-pixel fields at +312/+316 and the dx/dy steps at
 *   +344/+352 are enough to diagnose dropping/rounding on a per-frame basis.
 */

#ifndef MOVELOG_H
#define MOVELOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call once, BEFORE so_finalize(&game_mod), to install the trampoline.
 * Does nothing if CT_MOVELOG is not set in the environment. */
void movelog_install(struct so_module *game_mod);

#ifdef __cplusplus
}
#endif

#endif /* MOVELOG_H */
