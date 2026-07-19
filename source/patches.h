/* patches.h -- runtime ARM64 instruction patches for libchrono.so
 *
 * Ported from the ct_nx (Switch) fork to the PortMaster/aarch64-Linux target.
 * PortMaster runs the same libchrono.so (Chrono Trigger Android v2.1.5) on the
 * same ARM64 ISA, so the machine-code edits below are byte-identical to the
 * Switch build's; only the apply mechanism differs (ct_pm's so_util loader).
 *
 * Applied at boot to load_base (the writable RW mapping; on Linux load_base ==
 * load_virtbase) before so_finalize() mprotects it to RX. Equivalent in effect
 * to the offline Python patcher scripts, but driven by config.txt so individual
 * fixes can be toggled without re-patching the .so on disk.
 *
 * Every entry records the expected old word, so we verify the .so matches the
 * expected build before writing anything: a mismatch prints a warning and skips
 * that entry -- it never silently corrupts. The whole pass is additionally
 * gated on the v2.1.5 fingerprint check in main.c (g_libchrono_v215).
 *
 * Copyright (C) 2026 ppkantorski <https://github.com/ppkantorski>
 *   (original ct_nx patches; NaGaa95 loader base)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __PATCHES_H__
#define __PATCHES_H__

#include <stdint.h>
#include "so_util.h"
#include "util.h"

// ---------------------------------------------------------------------------
// Patch descriptor
//
// sym_name: exported symbol whose load_base address anchors the patch.
//           NULL means use raw_vaddr directly (for functions with no exported
//           symbol -- the world-button helper, cave sites, etc.).
// func_off:  byte offset from the symbol's load_base address to the word.
//            Ignored when sym_name is NULL.
// raw_vaddr: link-time virtual address used when sym_name is NULL.
// old_word:  expected current instruction (LE uint32). 0 = "don't check"
//            (used for cave slots that are verified-zero in the .so).
// new_word:  replacement instruction.
// desc:      human-readable label for debug output.
// ---------------------------------------------------------------------------
typedef struct {
  const char   *sym_name;
  uint32_t      func_off;
  uint32_t      raw_vaddr;
  uint32_t      old_word;
  uint32_t      new_word;
  const char   *desc;
} PatchEntry;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Compute the writable load_base address for a patch entry.
// Returns 0 if the symbol is required but missing (caller skips).
static uintptr_t patch_addr(so_module *mod, const PatchEntry *p) {
  if (p->sym_name) {
    // Symbol-relative: load_base + symbol_value + func_off
    for (int i = 0; i < mod->num_syms; i++) {
      const char *name = mod->dynstrtab + mod->syms[i].st_name;
      if (__builtin_strcmp(name, p->sym_name) == 0)
        return (uintptr_t)mod->load_base + mod->syms[i].st_value + p->func_off;
    }
    debugPrintf("patches: symbol not found: %s\n", p->sym_name);
    return 0;
  } else {
    // Raw vaddr: load_base + raw_vaddr (load_base == load_virtbase on Linux, and
    // the .so links at base 0, so raw_vaddr is a direct file/image offset).
    return (uintptr_t)mod->load_base + p->raw_vaddr;
  }
}

// Apply one patch. Prints a diagnostic on mismatch but never aborts.
static void apply_patch(so_module *mod, const PatchEntry *p) {
  uintptr_t addr = patch_addr(mod, p);
  if (!addr) return;

  uint32_t cur;
  __builtin_memcpy(&cur, (void *)addr, 4);

  if (cur == p->new_word) {
    // Already at the desired value -- idempotent, no write needed.
    return;
  }
  if (p->old_word && cur != p->old_word) {
    debugPrintf("patches: MISMATCH @ %s+0x%x / vaddr 0x%x: "
                "expected %08x got %08x -- skipping (%s)\n",
                p->sym_name ? p->sym_name : "(raw)",
                p->sym_name ? p->func_off : p->raw_vaddr,
                p->raw_vaddr, p->old_word, cur, p->desc);
    return;
  }

  __builtin_memcpy((void *)addr, &p->new_word, 4);
  debugPrintf("patches: %08x -> %08x  %s\n", cur, p->new_word, p->desc);
}

// Apply an array of patches.
static void apply_patches(so_module *mod, const PatchEntry *table, int count) {
  for (int i = 0; i < count; i++)
    apply_patch(mod, &table[i]);
}

#define PATCH_COUNT(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

// ---------------------------------------------------------------------------
// Macro helpers for the patch tables
// ---------------------------------------------------------------------------

// Symbol-relative entry: sym at func_off, expected old -> new
#define P_SYM(sym, off, old, new, desc) \
  { (sym), (uint32_t)(off), 0, (uint32_t)(old), (uint32_t)(new), (desc) }

// Raw-vaddr entry: no symbol, raw virtual address, old -> new
// old=0 means "don't verify" (used for cave zero-slots).
#define P_RAW(vaddr, old, new, desc) \
  { NULL, 0, (uint32_t)(vaddr), (uint32_t)(old), (uint32_t)(new), (desc) }

// Cave slot: zero-initialised padding, no old-word check needed.
#define P_CAVE(vaddr, new, desc) \
  { NULL, 0, (uint32_t)(vaddr), 0, (uint32_t)(new), (desc) }

// ---------------------------------------------------------------------------
// 1.  cursor_fix  (remove_cursor_invert.py v9)
//
//     Selected-item text: BLACK -> WHITE on all menus
//     Selection highlight: cream (0xfe,0xff,0xdd,0xcc) -> dark semi-transparent (0,0,0,0xa0)
//     Dialogue choice NON-selected bg: normal opacity -> dimmed (cave at 0xaa56a0)
// ---------------------------------------------------------------------------
static const PatchEntry g_cursor_patches[] = {

  // 1. BattleTechMenu::setListButtonFontColors(int)  selected text BLACK->WHITE
  P_RAW(0x6e40cc, 0xf946594a, 0xf946914a,
    "BattleTechMenu::setListButtonFontColors selected text BLACK->WHITE"),

  // 2. BattleItemMenu::onSelectChanged(int)  crash fix + WHITE
  P_RAW(0x6e52dc, 0x79002be9, 0x79002bea,
    "BattleItemMenu::onSelectChanged r,g BLACK->WHITE (strh w9->w10)"),
  P_RAW(0x6e52e0, 0x39005be8, 0x39005bea,
    "BattleItemMenu::onSelectChanged b BLACK->WHITE (strb w8->w10)"),

  // 3. MsgWindow::createMenu  dialogue choice windows
  P_RAW(0x5fd998, 0xaa1703e0, 0xaa1503e0,
    "MsgWindow::createMenu ItemSpriteA selectedImg bg WHITE->BLACK"),
  P_RAW(0x5fd9e8, 0xaa1703e0, 0xaa1503e0,
    "MsgWindow::createMenu ItemSpriteB normalImg bg WHITE->BLACK"),
  P_RAW(0x5fda18, 0xaa1503e1, 0xaa1703e1,
    "MsgWindow::createMenu selected-choice text tint BLACK->WHITE"),

  // 4. FieldMenu::init()  selected-state sprite colour BLACK->WHITE
  P_RAW(0x755538, 0xf9465908, 0xf9469108,
    "FieldMenu::init selected-state sprite colour BLACK->WHITE"),

  // 5. WorldMenu button helper (MENU/MAP/WARP)  selected-state BLACK->WHITE
  P_RAW(0x782ca4, 0xf9465929, 0xf9469129,
    "WorldMenu button helper selected-state sprite colour BLACK->WHITE"),

  // 6. BattleMenu::drawCommandWindow(int,bool,...)  selected text BLACK->WHITE
  P_RAW(0x5b56f0, 0xf946594a, 0xf946914a,
    "BattleMenu::drawCommandWindow(int,bool) selected text BLACK->WHITE"),

  // 7. BattleMenu::drawCommandWindow()  selected cmd text BLACK->WHITE
  P_RAW(0x5b2210, 0xf9465842, 0xf9469042,
    "BattleMenu::drawCommandWindow() selected cmd text BLACK->WHITE"),

  // 8. BattleListMenu highlight-colour: cream -> dark semi-transparent
  P_RAW(0x6e6994, 0x52801fc1, 0x52800001,
    "BattleListMenu highlight r 0xfe->0x00"),
  P_RAW(0x6e6998, 0x52801fe2, 0x52800002,
    "BattleListMenu highlight g 0xff->0x00"),
  P_RAW(0x6e699c, 0x52801ba3, 0x52800003,
    "BattleListMenu highlight b 0xdd->0x00"),
  P_RAW(0x6e69a0, 0x52801984, 0x52801404,
    "BattleListMenu highlight a 0xcc->0xa0"),

  // 9. BattleMenu::init() cmd-window SELECTED-FILL colour (2 sites)
  P_RAW(0x5ad8b0, 0x52801fc1, 0x52800001,
    "cmd-window fill #0x5ad8b0 r 0xfe->0x00"),
  P_RAW(0x5ad8b4, 0x52801fe2, 0x52800002,
    "cmd-window fill #0x5ad8b0 g 0xff->0x00"),
  P_RAW(0x5ad8b8, 0x52801ba3, 0x52800003,
    "cmd-window fill #0x5ad8b0 b 0xdd->0x00"),
  P_RAW(0x5ad8bc, 0x52801984, 0x52801404,
    "cmd-window fill #0x5ad8b0 a 0xcc->0xa0"),
  P_RAW(0x5ada68, 0x52801fc1, 0x52800001,
    "cmd-window fill #0x5ada68 r 0xfe->0x00"),
  P_RAW(0x5ada6c, 0x52801fe2, 0x52800002,
    "cmd-window fill #0x5ada68 g 0xff->0x00"),
  P_RAW(0x5ada70, 0x52801ba3, 0x52800003,
    "cmd-window fill #0x5ada68 b 0xdd->0x00"),
  P_RAW(0x5ada74, 0x52801984, 0x52801404,
    "cmd-window fill #0x5ada68 a 0xcc->0xa0"),

  // 10. MsgWindow::createMenu  dialogue NON-selected bg: dim via cave
  //     Route normal-bg makeColorNode call through cave at 0xaa56a0
  //     that enables opacity cascade then setOpacity(160).
  P_RAW(0x5fd950, 0x940000b5, 0x94129f54,
    "dialogue normal bg makeColorNode -> cascade+dim cave"),

  // Cave @ 0xaa56a0 (zero-padding region; BL offset from 0x5fd950 verified above)
  P_CAVE(0xaa56a0, 0xa9bf7bf3, "cave: stp x19,x30,[sp,#-0x10]!"),
  P_CAVE(0xaa56a4, 0x97ed6160, "cave: bl #0x5fdc24 (makeColorNode)"),
  P_CAVE(0xaa56a8, 0xaa0003f3, "cave: mov x19,x0"),
  P_CAVE(0xaa56ac, 0x52800021, "cave: mov w1,#1"),
  P_CAVE(0xaa56b0, 0x97f2768f, "cave: bl #0x7430ec (setCascadeOpacityEnabledRecursive)"),
  P_CAVE(0xaa56b4, 0xaa1303e0, "cave: mov x0,x19"),
  P_CAVE(0xaa56b8, 0x52801401, "cave: mov w1,#0xa0 (opacity 160)"),
  P_CAVE(0xaa56bc, 0xf9400268, "cave: ldr x8,[x19]"),
  P_CAVE(0xaa56c0, 0xf9424908, "cave: ldr x8,[x8,#0x490] (setOpacity vtable slot)"),
  P_CAVE(0xaa56c4, 0xd63f0100, "cave: blr x8"),
  P_CAVE(0xaa56c8, 0xaa1303e0, "cave: mov x0,x19"),
  P_CAVE(0xaa56cc, 0xa8c17bf3, "cave: ldp x19,x30,[sp],#0x10"),
  P_CAVE(0xaa56d0, 0xd65f03c0, "cave: ret"),
};

// ---------------------------------------------------------------------------
// Apply every enabled feature group. Called from main() while game_mod's .text
// is still RW (before so_finalize), and only when the v2.1.5 fingerprint
// matched (g_libchrono_v215). Each group is independent and config-gated;
// additional groups are appended here as they are ported from ct_nx.
// ---------------------------------------------------------------------------
static inline void apply_game_patches(so_module *mod) {
  extern Config config;

  if (config.cursor_fix) {
    debugPrintf("patches: applying cursor_fix\n");
    apply_patches(mod, g_cursor_patches, PATCH_COUNT(g_cursor_patches));
  }
}

#endif // __PATCHES_H__
