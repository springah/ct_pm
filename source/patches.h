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
 * Every entry that targets existing code records its expected old word, so we
 * verify the .so matches the expected build before writing anything: a mismatch
 * prints a warning to stderr (and so to log.txt) and skips that entry -- it never
 * silently corrupts. The only entries without an old word are P_CAVE slots, whose
 * destinations are confirmed zero-filled padding. The whole pass is additionally
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
#include <stdio.h>
#include "so_util.h"
#include "util.h"
#include <math.h>
#include "config.h"
#ifndef __SWITCH__
#include "rescale.h"
#endif

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
    fprintf(stderr, "ct: patches: symbol not found: %s -- skipping\n", p->sym_name);
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
    // stderr, not debugPrintf: debugPrintf compiles to nothing in release
    // (DEBUG_LOG 0), and a skipped patch is exactly the thing a bug report
    // needs to show. The launcher redirects stderr into log.txt.
    fprintf(stderr,
            "ct: patches: MISMATCH @ %s+0x%x / vaddr 0x%x: "
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

// Cave slot: for a destination that is genuinely zero-filled padding in the .so,
// where there is no meaningful old word to check. Only use this after confirming
// the target range really does read as zeros -- dead *code* is not padding, and
// belongs in P_RAW with its real old word (see the UserScroll cave).
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
// 2.  remove_mobile_ui  (remove_mobile_ui.py v8 + remove_menu_buttons.py v1)
//     Hide the on-screen touch overlays a controller build never uses: field/
//     world-map/title buttons, per-menu StatusBar back/close, the Johnny race
//     minigame buttons, and the VirtualPad colour prompts. Movement default RUN->WALK.
// ---------------------------------------------------------------------------
// nsMenu::StatusBar::init(...), SceneSpecialRace::setMenu and VirtualPad::
// openVPad are exported in .dynsym, so anchor those fixes on the symbol (robust
// to a future rebuild shifting code) rather than a hardcoded raw vaddr.
#define STATUSBAR_INIT4_SYM \
  "_ZN6nsMenu9StatusBar4initERKNSt6__ndk112basic_stringIcNS1_11char_" \
  "traitsIcEENS1_9allocatorIcEEEEbRKNS1_8functionIFvPN7cocos2d3RefEEEESH_"
#define STATUSBAR_INIT5_SYM STATUSBAR_INIT4_SYM "b"
#define RACE_SETMENU_SYM "_ZN16SceneSpecialRace7setMenuEPN7cocos2d4NodeE"
#define VPAD_OPENVPAD_SYM "_ZN10VirtualPad8openVPadEv"

static const PatchEntry g_mobile_ui_patches[] = {

  // Fix 1: FieldMenu::setMenuAvailable(bool)
  //   +24: STRB WZR,[X0,#0x330] -> MOV W1,WZR  (force setVisible(false))
  P_SYM("_ZN9FieldMenu16setMenuAvailableEb", 24,
        0x390CC01F, 0x2A1F03E1,
        "FieldMenu::setMenuAvailable +24 field button always hidden, Start untouched"),

  // Fix 2: world-button-builder helper @ 0x782C7C (no exported symbol)
  //   setPosition(0xC8) -> setVisible(0x170), bool arg = false, two paths
  P_RAW(0x782DC0, 0xF9406508, 0xF940B908,
    "world-button helper +0x144 vtable setPosition->setVisible [path A]"),
  P_RAW(0x782DC8, 0x1E232821, 0x2A1F03E1,
    "world-button helper +0x14C bool arg=false [path A]"),
  P_RAW(0x782DF8, 0x1E261001, 0x2A1F03E1,
    "world-button helper +0x17C bool arg=false [path B]"),
  P_RAW(0x782DFC, 0xF9406508, 0xF940B908,
    "world-button helper +0x180 vtable setPosition->setVisible [path B]"),

  // Fix 3a: setupMenuNodes hide range-check -> NOP (hide every right-side icon)
  P_RAW(0x77FE94, 0x54FFE1A8, 0xD503201F,
    "setupMenuNodes B.HI->NOP: hide all right-side icons unconditionally"),

  // Fix 3b: repurpose hide sequence -> cave call (setVisible+setSkip)
  P_RAW(0x77FE98, 0xF9400348, 0xAA1A03E0,
    "setupMenuNodes LDR X8,[X26]->MOV X0,X26 (State* arg for cave)"),
  P_RAW(0x77FE9C, 0xF9400908, 0x940C98F5,
    "setupMenuNodes LDR X8,[X8,#0x10]->BL 0xAA6270 (setVisible+setSkip cave)"),
  P_RAW(0x77FEA0, 0xAA1A03E0, 0x17FFFF0A,
    "setupMenuNodes MOV X0,X26->B #0x77FAC8 (loop continue)"),

  // Fix 3b cave @ 0xAA6270  (96B zero-padding; distinct from cursor cave 0xAA56A0)
  P_CAVE(0xAA6270, 0xA9BF7BF3, "cave: stp x19,x30,[sp,#-0x10]!"),
  P_CAVE(0xAA6274, 0xAA0003F3, "cave: mov x19,x0"),
  P_CAVE(0xAA6278, 0xF9400008, "cave: ldr x8,[x0]"),
  P_CAVE(0xAA627C, 0xF9400908, "cave: ldr x8,[x8,#0x10] getNode"),
  P_CAVE(0xAA6280, 0xD63F0100, "cave: blr x8"),
  P_CAVE(0xAA6284, 0xF9400008, "cave: ldr x8,[x0]"),
  P_CAVE(0xAA6288, 0xF940B908, "cave: ldr x8,[x8,#0x170] setVisible"),
  P_CAVE(0xAA628C, 0x2A1F03E1, "cave: mov w1,wzr"),
  P_CAVE(0xAA6290, 0xD63F0100, "cave: blr x8"),
  P_CAVE(0xAA6294, 0xAA1303E0, "cave: mov x0,x19"),
  P_CAVE(0xAA6298, 0x52800021, "cave: mov w1,#1"),
  P_CAVE(0xAA629C, 0x940293D9, "cave: bl 0xB4B200 setSkip(true)"),
  P_CAVE(0xAA62A0, 0xA8C17BF3, "cave: ldp x19,x30,[sp],#0x10"),
  P_CAVE(0xAA62A4, 0xD65F03C0, "cave: ret"),

  // Fix 3c: RET-stub UpdateIconVisible so it can never re-show icons
  P_RAW(0x780CC8, 0xA9BD7BFD, 0xD65F03C0,
    "UpdateIconVisible RET-stub: icons stay hidden"),

  // -------------------------------------------------------------------------
  // Fix 4: menu-system status bar back/close buttons (remove_menu_buttons.py)
  //
  //     Targets the menu SYSTEM screens (Equip, Item, Tech, Config,
  //     SaveLoad, Formation, and the top-level Main Menu bar) instead of
  //     the field/world-map/title-screen overlays Fixes 1-3 handle. All of
  //     those screens share one reusable header class, nsMenu::StatusBar,
  //     built via one of two overloads of nsMenu::StatusBar::init(...):
  //
  //       * 4-arg overload  (title, bool, backCallback, closeCallback)
  //           -- used by MenuNodeEquip/Item/Tech/Config/SaveLoad/Formation
  //       * 5-arg overload  (..., isTopBar)
  //           -- same body plus an isTopBar-style branch; patched
  //              defensively even though every verified caller resolves
  //              to the 4-arg one
  //
  //     Disassembly-confirmed structure shared by both overloads:
  //
  //         ldr  x8, [x20, #0x320]      ; backCallback std::function target
  //         cbz  x8, <no_back_button>   ; null -> skip creating back button
  //         ...                         ; non-null -> createBackButton(...),
  //                                     ;   subtracting its width from bar
  //         <no_back_button>:
  //         mov  w8, #0x43f00000        ; float 480.0 -- full design width,
  //         fmov s0, w8                 ;   used as the bar width instead
  //
  //         ldr  x8, [x20, #0x350]      ; closeCallback std::function target
  //         cbz  x8, <no_close_button>  ; null -> skip creating close button
  //         ...                         ; non-null -> createCloseButton(...),
  //                                     ;   same width-subtraction pattern
  //         <no_close_button>:
  //         ...                         ; falls through with whichever
  //                                     ;   width was computed above
  //
  //     That "no button, full-width bar" path already exists and already
  //     runs whenever a caller passes an empty std::function for either
  //     callback -- it is not new logic. This forces the CBZ gate to
  //     always take the skip branch, for every StatusBar, regardless of
  //     what the caller passed, by converting each CBZ into an
  //     unconditional B to the exact same target the CBZ already pointed
  //     at (the branch target is link-time-relative, so translating the
  //     whole .so to a different runtime load_base -- as so_util does --
  //     does not change it).
  //
  //     Net effect: no menu screen ever creates a back or close button
  //     node, and the Main Menu / location / time / money bars resize to
  //     the full region instead of leaving a gap where the button used to
  //     sit. Controller "cancel" input is untouched: these button nodes
  //     are purely visual touch targets wired to their own std::function
  //     callbacks; nothing else in the input pipeline depends on their
  //     existence. Verified safe to always-skip: StatusBar's constructor
  //     zero-initializes both the backButton/closeButton node-pointer
  //     fields and the backCallback/closeCallback std::function slots, and
  //     both StatusBar::setInteractive and the destructor null-check the
  //     button pointers before touching them -- so never allocating the
  //     nodes is a clean, well-defined state, not a dangling-pointer risk.
  // -------------------------------------------------------------------------

  // 4-arg overload: back-button gate @ +0x160 (CBZ X8,+0x40 -> B +0x40)
  P_SYM(STATUSBAR_INIT4_SYM, 0x160, 0xB4000208, 0x14000010,
    "StatusBar::init(4-arg) back button: force existing 'no button' skip "
    "branch (was conditional on backCallback == null)"),

  // 4-arg overload: close-button gate @ +0x258 (CBZ X8,+0xF0 -> B +0xF0)
  P_SYM(STATUSBAR_INIT4_SYM, 0x258, 0xB4000788, 0x1400003C,
    "StatusBar::init(4-arg) close button: force existing 'no button' skip "
    "branch (was conditional on closeCallback == null)"),

  // 5-arg overload: back-button gate @ +0x17C (CBZ X8,+0x40 -> B +0x40)
  P_SYM(STATUSBAR_INIT5_SYM, 0x17C, 0xB4000208, 0x14000010,
    "StatusBar::init(5-arg) back button: force existing 'no button' skip "
    "branch"),

  // 5-arg overload: close-button gate @ +0x2D8 (CBZ X8,+0x154 -> B +0x154)
  P_SYM(STATUSBAR_INIT5_SYM, 0x2D8, 0xB4000AA8, 0x14000055,
    "StatusBar::init(5-arg) close button: force existing 'no button' skip "
    "branch"),

  // -------------------------------------------------------------------------
  // Fix 5: SceneSpecialRace::setMenu -- Johnny's motorcycle race on-screen
  //         touch buttons
  //
  //     SceneSpecialRace::setMenu(cocos2d::Node* menuParent) is exported in
  //     .dynsym (size 2076 bytes) and is the sole place this scene builds
  //     its touch-button overlay -- confirmed by an inlined literal string
  //     "button" built via movz/movk immediates near the top of the
  //     function, and by every button-creation block ending in the exact
  //     same two-call pattern already proven out in Fix 2/3 above:
  //
  //         ldr  x8, [x8, #0xc8]     ; vtable slot: cocos2d::Node::setPosition
  //         ...                      ; s0/s1 loaded with float x/y immediates
  //         blr  x8
  //
  //     A run-time flag at this+0xc20 (set earlier in ::init from a race
  //     sub-mode ID) selects between two layouts:
  //
  //       * sub-mode flag set:   builds 2 buttons (fields this+0x5440,
  //                               this+0x5448)
  //       * sub-mode flag clear: builds 4 buttons -- one anonymous button
  //                               (only ever pushed into the button vector,
  //                               never stored to a named field) plus the
  //                               SAME this+0x5440 / this+0x5448 fields
  //                               (rebuilt with different on-screen
  //                               coordinates for this layout) plus a 4th
  //                               button at this+0x5450
  //
  //     Both layouts funnel into a shared tail that wraps everything built
  //     so far into a button container (this+0x5458) and adds it to
  //     menuParent -- we deliberately do NOT touch that container call
  //     (unconfirmed vtable slot 0x98, not the well-established 0xC8/0x170
  //     Node pair used everywhere else in this file); patching the 6
  //     individual button-creation sites below hides every button in both
  //     layouts without needing to touch that unverified call.
  //
  //     Same technique as Fix 2's world-map buttons: convert the
  //     setPosition(x,y) call into setVisible(false) --
  //
  //         ldr  x8, [x8, #0xc8]   ->   ldr  x8, [x8, #0x170]   (setVisible)
  //         fmov s1, w10           ->   mov  w1, wzr             (bool=false)
  //         blr  x8
  //
  //     The overwritten fmov/fadd instruction in each pair only ever fed
  //     the now-unused s0/s1 position registers -- verified dead once the
  //     call becomes setVisible(bool) instead of setPosition(float,float).
  //     Where the "this" pointer for the virtual call is set up via a
  //     nearby `mov x0,x22` (anonymous button) or an earlier `ldr x0,[x20,
  //     #0x5450]` (4th button), those loads are left untouched.
  // -------------------------------------------------------------------------

  // sub-mode-flag-set layout: button @ this+0x5440
  P_SYM(RACE_SETMENU_SYM, 0x138, 0xF9406508, 0xF940B908,
    "SceneSpecialRace::setMenu +0x138 button(0x5440) [flag-set layout] "
    "setPosition->setVisible vtable slot"),
  P_SYM(RACE_SETMENU_SYM, 0x148, 0x1E270141, 0x2A1F03E1,
    "SceneSpecialRace::setMenu +0x148 button(0x5440) [flag-set layout] "
    "bool arg=false"),

  // sub-mode-flag-set layout: button @ this+0x5448
  P_SYM(RACE_SETMENU_SYM, 0x200, 0xF9406508, 0xF940B908,
    "SceneSpecialRace::setMenu +0x200 button(0x5448) [flag-set layout] "
    "setPosition->setVisible vtable slot"),
  P_SYM(RACE_SETMENU_SYM, 0x210, 0x1E270141, 0x2A1F03E1,
    "SceneSpecialRace::setMenu +0x210 button(0x5448) [flag-set layout] "
    "bool arg=false"),

  // flag-clear layout: anonymous button (button vector only, no named field)
  P_SYM(RACE_SETMENU_SYM, 0x380, 0xF9406508, 0xF940B908,
    "SceneSpecialRace::setMenu +0x380 anonymous button [flag-clear layout] "
    "setPosition->setVisible vtable slot"),
  P_SYM(RACE_SETMENU_SYM, 0x390, 0x1E270120, 0x2A1F03E1,
    "SceneSpecialRace::setMenu +0x390 anonymous button [flag-clear layout] "
    "bool arg=false"),

  // flag-clear layout: button @ this+0x5440 (rebuilt at different coords)
  P_SYM(RACE_SETMENU_SYM, 0x444, 0xF9406508, 0xF940B908,
    "SceneSpecialRace::setMenu +0x444 button(0x5440) [flag-clear layout] "
    "setPosition->setVisible vtable slot"),
  P_SYM(RACE_SETMENU_SYM, 0x454, 0x1E270141, 0x2A1F03E1,
    "SceneSpecialRace::setMenu +0x454 button(0x5440) [flag-clear layout] "
    "bool arg=false"),

  // flag-clear layout: button @ this+0x5448 (rebuilt at different coords)
  P_SYM(RACE_SETMENU_SYM, 0x510, 0xF9406508, 0xF940B908,
    "SceneSpecialRace::setMenu +0x510 button(0x5448) [flag-clear layout] "
    "setPosition->setVisible vtable slot"),
  P_SYM(RACE_SETMENU_SYM, 0x520, 0x1E270141, 0x2A1F03E1,
    "SceneSpecialRace::setMenu +0x520 button(0x5448) [flag-clear layout] "
    "bool arg=false"),

  // flag-clear layout only: 4th button @ this+0x5450
  P_SYM(RACE_SETMENU_SYM, 0x618, 0xF9406508, 0xF940B908,
    "SceneSpecialRace::setMenu +0x618 button(0x5450) [flag-clear layout] "
    "setPosition->setVisible vtable slot"),
  P_SYM(RACE_SETMENU_SYM, 0x628, 0x1E232821, 0x2A1F03E1,
    "SceneSpecialRace::setMenu +0x628 button(0x5450) [flag-clear layout] "
    "bool arg=false"),

  // -------------------------------------------------------------------------
  // Fix 6: VirtualPad::openVPad -- green/yellow/red/blue color-button
  //         prompts (dance scene and other generic input/QTE scenes)
  //
  //     First attempt at this fix (now reverted) tried to NOP the call to
  //     VirtualPad::setupColorButtons out of VirtualPad::init, reasoning
  //     that the call site's return value was discarded and the callee was
  //     purely cosmetic (no nsStateMachine/nsInput::Manager references,
  //     unlike setupCursorColorButtons/setupPasscodeButtons). That was
  //     wrong: the callee doesn't just build cosmetics, it also STORES the
  //     created Menu* into VirtualPad::this+0x360 --
  //
  //         bl   Menu::createWithArray(...)
  //         str  x0, [x20, #0x360]      ; <-- required side effect
  //         ...
  //         ldr  x0, [x20, #0x360]
  //         ldr  x8, [x0]
  //         ldr  x8, [x8, #0x170]       ; setVisible
  //         mov  w1, wzr
  //         blr  x8                     ; starts hidden by default
  //
  //     Skipping the call left this+0x360 uninitialized. VirtualPad::
  //     openVPad() later does `ldr x0,[this,#0x360]; cbz x0,<skip>; ldr
  //     x8,[x0]; ldr x8,[x8,#0x170]; blr x8` with NO null-check on garbage
  //     memory being non-zero -- an unconditional vtable-slot dereference
  //     off whatever was already sitting at that heap offset. That is the
  //     crash the person hit on the button-prompt page. Lesson: a callee
  //     whose only visible effect at its single call site is "return value
  //     discarded" can still be required for a *sibling* member field a
  //     totally different function depends on -- discarded-return does not
  //     mean side-effect-free, and this file's own "don't skip
  //     construction, only flip the final setVisible" philosophy (every
  //     other Fix in this table) exists precisely to avoid this class of
  //     bug. Applying it here properly this time:
  //
  //     VirtualPad::openVPad() (exported, 112 bytes) is the sole place
  //     that flips these nodes visible. It touches three fields in order:
  //
  //       this+0x360 -- the setupColorButtons Menu (dance/QTE buttons)
  //       this+0x368 -- the setupCursorColorButtons/setupPasscodeButtons
  //                     container Node (confirmed via `str x20,[x19,#0x368]`
  //                     in both of those functions' own disassembly)
  //       this+0x370 -- nsMenu::nsInput::Manager* -- NOT a visual node;
  //                     openVPad tail-calls nsInput::Manager::setPause(bool)
  //                     on it (confirmed by resolving the tail-call target
  //                     via .rela.plt) to un-pause input when the pad opens
  //
  //     Both of the first two follow the identical pattern already used
  //     throughout this file -- cbz-guarded, vtable+0x170 (setVisible),
  //     bool arg in w1:
  //
  //         ldr x0, [x19, #0x360]  (or #0x368)
  //         cbz x0, <skip>
  //         ldr x8, [x0]
  //         mov w1, #1              ; true  <-- flip to wzr (false)
  //         ldr x8, [x8, #0x170]
  //         blr x8
  //
  //     Forcing both `mov w1,#1` sites to `mov w1,wzr` means every object
  //     still gets fully constructed exactly as originally (this+0x360/
  //     0x368/0x370 all end up valid, non-garbage pointers -- no more
  //     uninitialized-memory risk), and closeVPad's own setVisible(false)
  //     calls are already correct and untouched. The this+0x370 tail-call
  //     to setPause(bool) is left completely alone, so input for the
  //     cursor/passcode minigames keeps un-pausing/re-pausing exactly as
  //     before -- this patch is visibility-only.
  //
  //     Net effect: covers BOTH the plain dance/QTE color buttons AND the
  //     cursor/passcode variant's buttons (a strict improvement over the
  //     original attempt's scope), with none of the construction-skipping
  //     risk that caused the crash.
  // -------------------------------------------------------------------------

  // openVPad +0x34: this+0x360 (setupColorButtons Menu) setVisible(true)
  // -> setVisible(false)
  P_SYM(VPAD_OPENVPAD_SYM, 0x34, 0x52800021, 0x2A1F03E1,
    "VirtualPad::openVPad +0x34 bool arg=false: color-button Menu "
    "(this+0x360) stays hidden"),

  // openVPad +0x4c: this+0x368 (cursor/passcode container Node)
  // setVisible(true) -> setVisible(false)
  P_SYM(VPAD_OPENVPAD_SYM, 0x4C, 0x52800021, 0x2A1F03E1,
    "VirtualPad::openVPad +0x4C bool arg=false: cursor/passcode button "
    "container (this+0x368) stays hidden"),

  // -------------------------------------------------------------------------
  // Fix 7: cSfcWork CONFIG_WORK constant -- Movement assumed-default
  //         RUN(0) -> WALK(1)
  //
  //     In-game text (FLD_CMES0_217/218) confirms a Settings > Movement
  //     option with two explicit states, Walk and Run, plus <BTN_DASH>
  //     always doing the opposite of whichever is selected. The runtime
  //     read of that field is FieldImpl::atel_isDash (0x58f398): it loads
  //     CONFIG_WORK.Movement from the emulated-WRAM mirror at
  //     [FieldImpl+72 (ChronoCanvas*), +0x14000, +564] and branches
  //     0=Run, 1=Walk, 2=(alt table, also Run-family) -- see the cmp/b.eq
  //     chain against #0x2/#0x1 right after the load. Nothing in that
  //     branch chain is touched by this fix; explicit 1 (Walk) and 2 (Run)
  //     selections resolve exactly as before.
  //
  //     The 0 case is what this fix changes -- but 0 is never *written* by
  //     any Settings-menu code path (MenuNodeConfig::changeRowValue only
  //     ever stores clamped 1/2-style indices back through the row's
  //     setter callback). It is the field's pre-user-input assumed value,
  //     coming from a 16-byte SIMD constant {1,1,0,0} at rodata 0x374b30
  //     that all three of cSfcWork::SetDefaultConfig (0x55c00c),
  //     ::InitNewGameData (0x55bca4), and ::InitNewGamePlusData (0x55c584)
  //     load via `ldr q0,[x8,#2864]` and store into CONFIG_WORK+52..67 --
  //     confirmed identical at all three sites via cross-reference. Word
  //     index 3 of that constant (byte offset 0x374b3c, CONFIG_WORK+64)
  //     lands on the exact same struct field atel_isDash reads (verified
  //     via the FieldImpl/ChronoCanvas offset chain and the GAME_DATA
  //     copy-constructor's memberwise-copy touching the same 564/568
  //     pair). Patching this single rodata word changes the pre-user-input
  //     default at all three call sites simultaneously; no code path,
  //     branch, or the Settings-menu UI logic is modified.
  //
  //     Word index 1 of the same constant (byte offset 0x374b34) is a
  //     verified non-zero neighbor (message-speed default, value 1) --
  //     used below as a same-value sanity check. apply_patch() only ever
  //     warns when cur != new_word AND old_word is non-zero, so an
  //     old==new==1 entry stays silent for as long as the constant blob
  //     hasn't shifted, and flags a MISMATCH (without writing anything) if
  //     a future rebuild moves this rodata layout -- a real verification
  //     gate, unlike old=0 on the actual target word below, which P_RAW's
  //     "don't verify" sentinel can't distinguish from an unchecked cave
  //     slot. This word is genuinely, verifiedly zero in this build; 0 is
  //     the correct old-value to write here, just not one this framework
  //     can double-check on its own.
  // -------------------------------------------------------------------------

  // Sanity check only (no-op while layout is unchanged): rodata 0x374b34,
  // neighboring word in the same CONFIG_WORK-defaults constant, expected
  // to remain 1. If a rebuild ever shifts this blob, this fires a
  // MISMATCH in the debug log for the entry below instead of silently
  // writing to the wrong address.
  P_RAW(0x374b34, 0x00000001, 0x00000001,
    "sanity check: cSfcWork CONFIG_WORK-defaults constant neighbor word "
    "unchanged (verifies rodata layout before the Movement-default patch)"),

  // The actual fix: rodata 0x374b3c, CONFIG_WORK.Movement assumed-default
  // word, RUN(0) -> WALK(1). old=0 is a verified real value here (see
  // derivation above), not an unchecked cave slot.
  P_RAW(0x374b3c, 0x00000000, 0x00000001,
    "cSfcWork CONFIG_WORK.Movement assumed-default RUN(0)->WALK(1); "
    "explicit user Walk/Run choice in Settings still respected by "
    "FieldImpl::atel_isDash"),
};

// ---------------------------------------------------------------------------
// 3.  controller_glyphs  (force <BTN_*> table + bracketed PUA glyphs -> ASCII)
// ---------------------------------------------------------------------------
static const PatchEntry g_glyph_patches[] = {
  P_RAW(0x5fea50, 0x9a891114, 0xaa0803f4,
    "MsgText <BTN_*> tags: force controller glyph table (csel x20,x8,x9,ne -> mov x20,x8)"),

  // ---------------------------------------------------------------------
  // <BTN_*> glyphs: bracketed letters -> plain "[A]"-style ASCII glyphs
  //
  // The builder at 0x5fce6c assembles each glyph string inline in registers
  // (movz/movk) then stores it into every swap-variant slot of TWO parallel
  // tables: a KEY table at 0xbf3708 (the <BTN_*> search strings) and a VALUE
  // table at 0xbf3798 (the glyph each key is replaced with). The dialogue
  // text builder runs std::string::replace(key -> value) for all six rows.
  //
  // Originally each VALUE glyph was the 7-byte UTF-8 string
  //   U+3010 <letter> U+3011   ( e.g. e3 80 90 41 e3 80 91 = bracketed 'A' )
  // built as two overlapping 4-byte halves in a _lo/_hi register pair. We
  // replace each with the plain 3-byte ASCII string "[<letter>]" (e.g.
  // "[A]" = 5b 41 5d), so the button prompt renders as ordinary text in
  // whichever font is already drawing the surrounding dialogue -- no
  // Private-Use codepoints and no separate shared-font lookup required.
  //
  // CRITICAL: the per-glyph SSO size byte must be set WITHOUT shrinking the
  // search keys. The builder originally sourced BOTH the glyph size and the
  // key size from w20, so shrinking w20 also truncated <BTN_R>/<BTN_L> to
  // <BT, leaving 'N_R>'/'N_L>' after the icon. We therefore leave w20 at its
  // original 0x0e (keys stay length 7) and instead route every glyph-VALUE
  // size byte through w16 (set to 0x06 = length 3), which the L/R glyphs
  // already used. Per glyph: set _lo = '5b <letter> 5d 00', zero _hi (NUL
  // pad) -- the ASCII form is the same 3-byte length as the old PUA glyph,
  // so the size-byte plumbing below is unchanged.
  // ---------------------------------------------------------------------
  // <BTN_A> glyph -> "[A]" (5b 41 5d)
  P_RAW(0x5fce84, 0x52901c68, 0x52882b68,
    "<BTN_A> glyph lo: movz w8,#0x415b (ASCII 5b 41 = \"[A\")"),
  P_RAW(0x5fceac, 0x72a83208, 0x72a00ba8,
    "<BTN_A> glyph lo: movk w8,#0x005d,lsl16 (ASCII 5d 00 = \"]\\0\")"),
  P_RAW(0x5fce88, 0x529c682e, 0x5280000e,
    "<BTN_A> glyph hi: movz w14,#0 (NUL pad)"),
  P_RAW(0x5fceb0, 0x72b2300e, 0x72a0000e,
    "<BTN_A> glyph hi: movk w14,#0,lsl16"),
  // <BTN_B> glyph -> "[B]" (5b 42 5d)
  P_RAW(0x5fce8c, 0x52901c75, 0x52884b75,
    "<BTN_B> glyph lo: movz w21,#0x425b (ASCII 5b 42 = \"[B\")"),
  P_RAW(0x5fceb4, 0x72a85215, 0x72a00bb5,
    "<BTN_B> glyph lo: movk w21,#0x005d,lsl16 (ASCII 5d 00 = \"]\\0\")"),
  P_RAW(0x5fce90, 0x529c6856, 0x52800016,
    "<BTN_B> glyph hi: movz w22,#0 (NUL pad)"),
  P_RAW(0x5fceb8, 0x72b23016, 0x72a00016,
    "<BTN_B> glyph hi: movk w22,#0,lsl16"),
  // <BTN_X> glyph -> "[X]" (5b 58 5d)
  P_RAW(0x5fce94, 0x52901c6a, 0x528b0b6a,
    "<BTN_X> glyph lo: movz w10,#0x585b (ASCII 5b 58 = \"[X\")"),
  P_RAW(0x5fcebc, 0x72ab120a, 0x72a00baa,
    "<BTN_X> glyph lo: movk w10,#0x005d,lsl16 (ASCII 5d 00 = \"]\\0\")"),
  P_RAW(0x5fce98, 0x529c6b0f, 0x5280000f,
    "<BTN_X> glyph hi: movz w15,#0 (NUL pad)"),
  P_RAW(0x5fcec0, 0x72b2300f, 0x72a0000f,
    "<BTN_X> glyph hi: movk w15,#0,lsl16"),
  // <BTN_Y> glyph -> "[Y]" (5b 59 5d)
  P_RAW(0x5fce9c, 0x52901c6b, 0x528b2b6b,
    "<BTN_Y> glyph lo: movz w11,#0x595b (ASCII 5b 59 = \"[Y\")"),
  P_RAW(0x5fcec4, 0x72ab320b, 0x72a00bab,
    "<BTN_Y> glyph lo: movk w11,#0x005d,lsl16 (ASCII 5d 00 = \"]\\0\")"),
  P_RAW(0x5fcea0, 0x529c6b31, 0x52800011,
    "<BTN_Y> glyph hi: movz w17,#0 (NUL pad)"),
  P_RAW(0x5fcec8, 0x72b23011, 0x72a00011,
    "<BTN_Y> glyph hi: movk w17,#0,lsl16"),
  // <BTN_R> glyph -> "[R]" (5b 52 5d)   |   <BTN_L> glyph -> "[L]" (5b 4c 5d)
  P_RAW(0x5fce6c, 0xd2901c6c, 0xd28a4b6c,
    "<BTN_R> glyph: movz x12,#0x525b (ASCII 5b 52 = \"[R\")"),
  P_RAW(0x5fce7c, 0xf2aa520c, 0xf2a00bac,
    "<BTN_R> glyph: movk x12,#0x005d,lsl16 (ASCII 5d 00 = \"]\\0\")"),
  P_RAW(0x5fcea4, 0xf2dc684c, 0xf2c0000c,
    "<BTN_R> glyph: movk x12,#0,lsl32 (NUL pad)"),
  P_RAW(0x5fced0, 0xf2f2300c, 0xf2e0000c,
    "<BTN_R> glyph: movk x12,#0,lsl48 (NUL pad)"),
  P_RAW(0x5fce70, 0xd2901c6d, 0xd2898b6d,
    "<BTN_L> glyph: movz x13,#0x4c5b (ASCII 5b 4c = \"[L\")"),
  P_RAW(0x5fce80, 0xf2a9920d, 0xf2a00bad,
    "<BTN_L> glyph: movk x13,#0x005d,lsl16 (ASCII 5d 00 = \"]\\0\")"),
  P_RAW(0x5fcea8, 0xf2dc684d, 0xf2c0000d,
    "<BTN_L> glyph: movk x13,#0,lsl32 (NUL pad)"),
  P_RAW(0x5fced4, 0xf2f2300d, 0xf2e0000d,
    "<BTN_L> glyph: movk x13,#0,lsl48 (NUL pad)"),

  // --- key/glyph size-byte decoupling (the fix for the 'N_R>' leak) ---
  // w20 is left UNTOUCHED at 0x0e so all six <BTN_*> search keys keep their
  // full length. w16 becomes 0x06 and is used as the size byte for every
  // glyph VALUE row (A/B/X/Y newly routed to it; L/R already used it).
  P_RAW(0x5fcecc, 0x52800210, 0x528000d0,
    "glyph size reg: mov w16,#0x10 -> #0x06 (SSO len 3, shared by all 6 glyph values)"),
  // Route the 15 A/B/X/Y glyph-value size stores from w20 -> w16 so they
  // get length 3 while the keys (still w20) keep length 7.
  P_RAW(0x5fcedc, 0x39000134, 0x39000130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x0] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcef4, 0x39006134, 0x39006130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x18] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf04, 0x3900c134, 0x3900c130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x30] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf14, 0x39012134, 0x39012130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x48] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf48, 0x3902a134, 0x3902a130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0xa8] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf58, 0x39030134, 0x39030130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0xc0] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf68, 0x39036134, 0x39036130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0xd8] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf8c, 0x39048134, 0x39048130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x120] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf98, 0x3904e134, 0x3904e130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x138] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcfa8, 0x39054134, 0x39054130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x150] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcfb8, 0x3905a134, 0x3905a130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x168] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcfd8, 0x3906c134, 0x3906c130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x1b0] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcfe4, 0x39072134, 0x39072130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x1c8] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcff4, 0x39078134, 0x39078130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x1e0] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fd000, 0x3907e134, 0x3907e130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x1f8] (use size-3 reg, not key-size reg)"),
};

// ---------------------------------------------------------------------------
// 4.  fix_diagonal_movement  (UserScroll branch redirects + in-place cave @0x5a15cc)
// ---------------------------------------------------------------------------
static const PatchEntry g_diagonal_patches[] = {
  // Redirect each diagonal block's post-store branch into the shared cave below.
  // Original instruction at each site is the first half of a float divide-by-1.39
  // "diagonal speed normalization" (cbnz into the float path); replacing it with an
  // unconditional branch skips that path entirely for all 4 diagonal blocks.
  P_RAW(0x5a14e4, 0x35000a0c, 0x1400003a,
    "UserScroll diagonal block w8=4 (down-right/up-right family): skip float normalize, use raw-int accumulate"),
  P_RAW(0x5a151c, 0x35000a0c, 0x1400002c,
    "UserScroll diagonal block w8=9: skip float normalize, use raw-int accumulate"),
  P_RAW(0x5a1570, 0x350003ac, 0x14000017,
    "UserScroll diagonal block w8=5: skip float normalize, use raw-int accumulate"),
  P_RAW(0x5a15c8, 0x3500018c, 0x14000001,
    "UserScroll diagonal block w8=8: skip float normalize, use raw-int accumulate"),

  // Cave @ 0x5a15cc (40 bytes made unreachable by the 4 redirects above --
  // nothing else in the function branches here). NOTE: this is dead *code*, not
  // padding, so every slot carries its real old word like any other patch. The
  // old words below are the v2.1.5 instructions that occupied the region (the
  // 0x5a15d0/0x5a15d4 pair is the movz/movk that built the 0x3fb1eb85 (1.39f)
  // diagonal divisor these patches exist to remove).
  P_RAW(0x5a15cc, 0x1e220120, 0x29522d0a, "cave: ldp w10,w11,[x8,#0x90]   ; w10,w11 = raw per-axis step just stored (same magnitude cardinal uses)"),
  P_RAW(0x5a15d0, 0x529d70a9, 0xb940990c, "cave: ldr w12,[x8,#0x98]       ; w12 = current X target"),
  P_RAW(0x5a15d4, 0x72a7f629, 0xb0a018c,  "cave: add w12,w12,w10          ; X target += raw X step (no normalization, no truncation loss)"),
  P_RAW(0x5a15d8, 0x1e270121, 0xb900990c, "cave: str w12,[x8,#0x98]"),
  P_RAW(0x5a15dc, 0x1e211800, 0xb900a10c, "cave: str w12,[x8,#0xa0]       ; commit X (matches cardinal's commit site)"),
  P_RAW(0x5a15e0, 0x14000008, 0xb940a509, "cave: ldr w9,[x8,#0xa4]        ; w9 = current Y target"),
  P_RAW(0x5a15e4, 0x1e341000, 0xb0b0129,  "cave: add w9,w9,w11            ; Y target += raw Y step"),
  P_RAW(0x5a15e8, 0x1e361001, 0xb900a509, "cave: str w9,[x8,#0xa4]"),
  P_RAW(0x5a15ec, 0x7100017f, 0xb900ad09, "cave: str w9,[x8,#0xac]        ; commit Y (matches cardinal's commit site)"),
  P_RAW(0x5a15f0, 0x1e201c20, 0x14000036, "cave: b 0x5a16c8 (UserScroll epilogue, restores x19/x29/x30 and returns)"),
};


// ---------------------------------------------------------------------------
// 5.  Framing cluster: ui_scale_fix / game_area_width_fix / field_zoom_fix /
//     map_zoom_fix.  Ported from ct_nx (patches.h sections 7-9 + map_zoom) and
//     generalised from the Switch's fixed 640x360 canvas to any panel.
//
//     WHY. libchrono has no single design resolution: a static initialiser at
//     0x641c00.. builds an aspect-bucketed table -- 568x320 (16:9), 480x360
//     (4:3), 568x340 (16:10), 1680x720 (ultrawide) -- and cocos2d scales the
//     scene by panel/design per axis. No PortMaster panel is an integer
//     multiple of any entry (1280/568 = 2.2535, 640/480 = 1.333). On top of
//     that the field map node is drawn at a hard-coded (1.875, 1.66667)
//     art->design scale, so field art pixels land at 2.5 x 2.22 panel px on a
//     640x480 panel and 4.23 x 3.76 at 720p: non-integer AND non-square --
//     the uneven-pixel / motion-shimmer look. UI sprites and text get the
//     same fractional scale.
//
//     THE CURE is three coupled patches (never ship them piecemeal):
//       ui_scale_fix   -- stamp EVERY table entry with (panel / design_scale)
//                         so the scene scale is exactly design_scale on both
//                         axes, whatever the runtime picker chooses.
//                         design_scale auto = floor(panel_w / 640), min 1:
//                         640x480 -> 1 (design 640x480), 1280x720 -> 2
//                         (640x360, the ct_nx value), 1920x1080 -> 3.
//       game_area_width_fix -- ctr::gameArea's width is a hard-coded 568.0
//                         (its height is adaptive); make the width adaptive
//                         too. A proven no-op at the stock 568 design.
//       field_zoom_fix -- the fieldmap node's setScale(1.875, 1.66667) becomes
//                         setScale(z, z); the view-size / camera-limit
//                         densities follow 1/z so the drawn view still covers
//                         exactly the design canvas; the node is re-anchored to
//                         the canvas centre. Panel px per art px = z *
//                         design_scale. Auto z picks the smallest integer
//                         panel-px size p whose visible rows (panel_h / p) fit
//                         the engine's fixed 432x224 field RenderTexture
//                         (<= 220 rows): 640x480 -> 3 px (213x160 art visible),
//                         720p -> 4 px (320x180, ct_nx's shipped framing).
//       map_zoom_fix   -- the same for the WorldMap node (four setScale sites)
//                         plus its anchor.
//
//     Every site below is ct_nx's (same libchrono v2.1.5, same ISA; load_base
//     == load_virtbase here, so raw vaddrs apply directly) and every old word
//     was re-verified against this .so before porting. ct_nx's 16:9-only
//     constants (640 / 360 / 320 / 180) are replaced by the resolved design
//     size; the derivations live in ct_nx's patches.h and are not repeated.
// ---------------------------------------------------------------------------

// --- ARM64 encoders (each verified against assembler output in ct_nx) -------
static uint32_t movz_topf(int rd, float v) {          // movz w<rd>, #top16(v), lsl #16
  union { float f; uint32_t u; } x; x.f = v;
  return 0x52A00000u | ((x.u >> 16) << 5) | (uint32_t)rd;
}
static uint32_t movz_w(int rd, uint16_t imm16) {      // movz w<rd>, #imm16
  return 0x52800000u | ((uint32_t)imm16 << 5) | (uint32_t)rd;
}
static uint32_t movk_w_hi(int rd, uint16_t imm16) {   // movk w<rd>, #imm16, lsl #16
  return 0x72A00000u | ((uint32_t)imm16 << 5) | (uint32_t)rd;
}
static uint32_t fmov_s_from_w(int sd, int wn) {       // fmov s<sd>, w<wn>
  return 0x1E270000u | ((uint32_t)wn << 5) | (uint32_t)sd;
}
static uint32_t encode_b(uintptr_t from, uintptr_t to) { // b <to>  (+-128MB)
  int64_t off = (int64_t)to - (int64_t)from;
  return 0x14000000u | (uint32_t)((off / 4) & 0x3FFFFFF);
}
static uint32_t encode_bl(uintptr_t from, uintptr_t to) { // bl <to> (+-128MB)
  int64_t off = (int64_t)to - (int64_t)from;
  return 0x94000000u | (uint32_t)((off / 4) & 0x3FFFFFF);
}
static uint32_t f32_bits(float v) { union { float f; uint32_t u; } x; x.f = v; return x.u; }
// Nearest float representable in a movz top-16 immediate (7 mantissa bits).
static float rn16(float f) {
  union { float f; uint32_t u; } x; x.f = f;
  x.u = (x.u + 0x8000u) & 0xFFFF0000u;
  return x.f;
}
#define NOP_WORD 0xd503201fu

// ui_scale_fix: stamp every design-resolution table entry with (w, h). The
// (568x340) entry's width register reuses the 16:9 entry's (s8), so only its
// height instruction exists.
static void apply_ui_scale_fix(so_module *mod, float w, float h) {
  static const struct { uint32_t va; int rd; float oldv; int is_h; } sites[] = {
    { 0x641c34, 8,  568.0f, 0 },  // 16:9 entry, width
    { 0x641c38, 9,  320.0f, 1 },  // 16:9 entry, height
    { 0x641c4c, 8,  480.0f, 0 },  // 4:3 entry, width
    { 0x641c50, 9,  360.0f, 1 },  // 4:3 entry, height
    { 0x641c68, 8,  340.0f, 1 },  // 16:10 entry, height (width = 16:9 width via s8)
    { 0x641c80, 8, 1680.0f, 0 },  // ultrawide entry, width
    { 0x641c84, 9,  720.0f, 1 },  // ultrawide entry, height
  };
  PatchEntry e[7];
  for (int i = 0; i < 7; i++) {
    e[i].sym_name  = NULL;
    e[i].func_off  = 0;
    e[i].raw_vaddr = sites[i].va;
    e[i].old_word  = movz_topf(sites[i].rd, sites[i].oldv);
    e[i].new_word  = movz_topf(sites[i].rd, rn16(sites[i].is_h ? h : w));
    e[i].desc      = "design resolution table entry (unified value)";
  }
  apply_patches(mod, e, 7);
}

// game_area_width_fix: gameArea = (origin.x, y, visW - 2*origin.x, h) instead
// of the hard-coded 568-wide rect (AppDelegate::applicationDidFinishLaunching).
static const PatchEntry g_gamearea_patches[] = {
  P_RAW(0x641a94, 0x52a881c8, 0xbd401be6,
        "gameArea: width literal 568.0 -> ldr s6,[sp,#0x18] (visible width)"),
  P_RAW(0x641a98, 0x1e270106, 0xd503201f,
        "gameArea: fmov s6,w8 -> nop (s6 now loaded directly)"),
  P_RAW(0x641ab0, 0x1e202860, 0x1e204060,
        "gameArea: x = origin.x (fadd s0,s3,s0 -> fmov s0,s3)"),
};

// field_zoom_fix, fixed part: the touch-drag / hit-test converters (mobile-only
// input code, no display effect) kept dimensionally consistent with the zoom.
static const PatchEntry g_field_zoom_fix_patches[] = {
  P_RAW(0x360b08, 0x3ff00000, 0x40000000, "onTouchMoved/Ended: rodata view scale X: 1.875f -> 2.0f"),
  P_RAW(0x360b0c, 0x3fd55555, 0x40000000, "onTouchMoved/Ended: rodata view scale Y: 1.66667f -> 2.0f"),
};

// field_zoom_fix, view part: FieldMap's view-size and camera-limit constants
// follow the zoom so the fieldmap node (view_art_px * zoom design units) fills
// the design canvas, and the character keeps stock's vertical registration
// (52% down the canvas: 112 art rows above the tile row at stock's 216-row
// display window) at every zoom. Sites, per the ct_nx disassembly:
//   0x56d874  FieldMap::init            viewH_art = visibleH * C / 320
//   0x570cfc  FieldMap::setScrollLimit  X-limit design->art conversion, C/480 = 1/zoom
//   0x570d0c  FieldMap::setScrollLimit  Y-limit overhang K = (visibleH-320) * C / 320,
//                                       wanted K = viewH - 192
// viewH is capped at 220: the plane is a fixed 432x224 RenderTexture.
// Visible field rows (art px) for a zoom on a canvas: the character-pinned
// branch or the screen-fill branch, whichever is taller, capped at 220 -- the
// engine's field plane is 432x224 and the last rows are never drawn.
static float field_view_rows(float zoom, float design_h) {
  if (zoom < 0.05f) zoom = 0.05f;
  const float char_y = design_h * (186.66667f / 360.0f);   // stock registration, scaled to the canvas
  float viewH = 112.0f + (design_h - char_y) / zoom;       // character-pinned branch
  float fill  = design_h / zoom;                            // screen-fill branch
  if (fill > viewH) viewH = fill;
  if (viewH > 220.0f) viewH = 220.0f;
  return viewH;
}

static void apply_field_view_zoom(so_module *mod, float zoom, float design_h) {
  if (zoom < 0.05f) zoom = 0.05f;
  const float viewH = field_view_rows(zoom, design_h);

  const float density_v = rn16(viewH * (320.0f / design_h));
  const float conv_h    = rn16(480.0f / zoom);
  // K site: only meaningful when the canvas is taller than the 320 the formula
  // subtracts; at 360 this is ct_nx's 8*(viewH-192).
  const int   have_k    = design_h > 321.0f;
  const float kscale_v  = have_k ? rn16((viewH - 192.0f) * 320.0f / (design_h - 320.0f)) : 192.0f;

  const PatchEntry e[3] = {
    { NULL, 0, 0x56d874, movz_topf(8, 192.0f), movz_topf(8, density_v),
      "FieldMap::init: view-height density 192 -> viewH*320/designH" },
    { NULL, 0, 0x570cfc, movz_topf(8, 256.0f), movz_topf(8, conv_h),
      "setScrollLimit: X-limit design->art conversion 256 -> 480/zoom" },
    { NULL, 0, 0x570d0c, movz_topf(9, 192.0f), movz_topf(9, kscale_v),
      "setScrollLimit: Y-limit overhang scale 192 -> (viewH-192)*320/(designH-320)" },
  };
  apply_patches(mod, e, have_k ? 3 : 2);
}

// field_zoom_fix, blit part: FieldMap::makeField's fieldmap-node setScale(x, y)
// call. The original 4 slots (0x5761fc-0x576210) become one "load float into
// w9, fmov s0 and s1 from it" sequence, so any zoom works and X == Y always.
static void apply_field_zoom(so_module *mod, float zoom) {
  union { float f; uint32_t u; } x; x.f = zoom;
  const uint16_t lo16 = (uint16_t)(x.u & 0xFFFF), hi16 = (uint16_t)(x.u >> 16);
  const PatchEntry e[4] = {
    { NULL, 0, 0x5761fc, 0x528aaaa9, movz_w(9, lo16),      "makeField: fieldmap setScale: movz w9,#lo16(zoom)" },
    { NULL, 0, 0x576200, 0x1e2fd000, movk_w_hi(9, hi16),   "makeField: fieldmap setScale: movk w9,#hi16(zoom),lsl#16" },
    { NULL, 0, 0x576208, 0x72a7faa9, fmov_s_from_w(0, 9),  "makeField: fieldmap setScale X: fmov s0,w9" },
    { NULL, 0, 0x576210, 0x1e270121, fmov_s_from_w(1, 9),  "makeField: fieldmap setScale Y: fmov s1,w9" },
  };
  apply_patches(mod, e, 4);
}

// field_zoom_fix, anchor part: makeField's setPosition gets (designW/2 -
// 128*zoom, 0) instead of the zoom-blind (ctr::x_offset, 0). Identity at the
// stock zoom on a 640-wide canvas (80, 0). Y stays 0 -- every vertical node
// shift ct_nx tried exposed unrendered plane rows; Y is handled by viewH above.
// Only 2 of the 6 slots before the blr are free (x21 = &ctr::x_offset is read
// again ~700 bytes later by an overlay node), so the X/Y loads go in a 20-byte
// cave at 0x376394 (verified all-zero, R+X segment) reached by a branch.
static void apply_field_node_anchor(so_module *mod, float zoom, float design_w, float design_h) {
  if (zoom < 0.05f) zoom = 0.05f;
  const uint32_t CAVE_CODE  = 0x376394;
  const uint32_t BRANCH_OUT = 0x576220;  // was: movi d1,#0 (dead: old Y-arg = 0.0)
  const uint32_t RESUME     = 0x576224;  // mov x0,x26 -- unchanged, resumes here
  const uint32_t NOP_SITE   = 0x576230;  // was: ldr s0,[x21] (dead: old X-arg)
  const uintptr_t base = (uintptr_t)mod->load_base;
  const uintptr_t code_addr = base + CAVE_CODE, branch_addr = base + BRANCH_OUT, resume_addr = base + RESUME;

  uint8_t cur[20];
  __builtin_memcpy(cur, (const void *)code_addr, sizeof(cur));
  for (unsigned i = 0; i < sizeof(cur); i++)
    if (cur[i]) {
      fprintf(stderr, "ct: patches: field node-anchor cave @0x%x is not empty -- skipping\n", CAVE_CODE);
      return;
    }

  const float node_x = design_w * 0.5f - 128.0f * zoom;
  // Y: 0 while the plane fills the canvas. When the canvas shows more rows
  // than the 224-row plane holds (2 px on 640x480: 240 wanted) the empty rows
  // would all sit at the top; lift the node by half the deficit so the picture
  // is letterboxed evenly -- the SNES's 224-line picture on a 240-line screen
  // (measured: the plane is drawn to all 224 rows, not just the 220 the view
  // limit budgets). Design units: the node is unscaled, its children carry zoom.
  const float deficit = design_h - 224.0f * zoom;
  const float node_y = deficit > 0.0f ? rn16(deficit * 0.5f) : 0.0f;
  uint32_t words[5] = {
    movz_topf(9, rn16(node_x)), fmov_s_from_w(0, 9),   // s0 = node_x
    movz_topf(10, node_y),      fmov_s_from_w(1, 10),  // s1 = node_y
    0,
  };
  words[4] = encode_b(code_addr + 16, resume_addr);
  __builtin_memcpy((void *)code_addr, words, sizeof(words));

  const PatchEntry e[2] = {
    { NULL, 0, BRANCH_OUT, 0x2f00e401, encode_b(branch_addr, code_addr),
      "makeField: setPosition X/Y args -> node-anchor cave (was: movi d1,#0)" },
    { NULL, 0, NOP_SITE, 0xbd4002a0, NOP_WORD,
      "makeField: old ctr::x_offset X-arg read -> nop (x21 itself untouched)" },
  };
  apply_patches(mod, e, 2);
}

// map_zoom_fix: WorldMap's four setScale(1.875, 1.66667) sites -> (zoom, zoom).
// initWeatherMap's X is an fcsel between 1.875 and 2.34375 (= 1.25x); that
// ratio is kept (zoom vs zoom*1.25).
static void apply_map_zoom(so_module *mod, float zoom) {
  if (zoom < 0.05f) zoom = 0.05f;
  const uint32_t movz9      = movz_topf(9, rn16(zoom));
  const uint32_t fmov_s0_w9 = fmov_s_from_w(0, 9);
  static const uint32_t SIMPLE_BASES[3] = {
    0x607c98,   // WorldMap::Init2, first fieldmap-alike node
    0x607db4,   // WorldMap::Init2, second fieldmap-alike node
    0x609c28,   // WorldMap::exitMiniMap
  };
  for (int i = 0; i < 3; i++) {
    const uint32_t b = SIMPLE_BASES[i];
    const PatchEntry e[3] = {
      { NULL, 0, b - 8, 0x528aaaa9, movz9,      "WorldMap setScale: movz w9,#0x5555 -> movz w9,#top16(zoom)" },
      { NULL, 0, b,     0x1e2fd000, fmov_s0_w9, "WorldMap setScale: fmov s0,#1.875 -> fmov s0,w9" },
      { NULL, 0, b + 4, 0x72a7faa9, NOP_WORD,   "WorldMap setScale: movk w9,#0x3fd5,lsl#16 -> nop" },
    };
    apply_patches(mod, e, 3);
  }
  const PatchEntry e[4] = {
    { NULL, 0, 0x608404, 0x52a802c8, movz_topf(8, rn16(zoom * 1.25f)), "initWeatherMap: mov w8,#2.34375 -> #top16(zoom*1.25)" },
    { NULL, 0, 0x60840c, 0x1e2fd000, NOP_WORD,   "initWeatherMap: fmov s0,#1.875 -> nop (dead write)" },
    { NULL, 0, 0x608420, 0x528aaaa9, movz9,      "initWeatherMap: movz w9,#0x5555 -> movz w9,#top16(zoom)" },
    { NULL, 0, 0x608424, 0x72a7faa9, fmov_s0_w9, "initWeatherMap: movk -> fmov s0,w9 (fcsel operand)" },
  };
  apply_patches(mod, e, 4);
}

// map_zoom_fix, anchor part: WorldMap::Init2's map-node setPosition gets the
// canvas-centred (designW/2 - 128*zoom, designH/2 - 96*zoom + 12) instead of
// the zoom-blind (ctr::x_offset, visibleH - 320). The +12 is ct_nx's on-device
// vertical trim (design units). The 10 stock slots between setScale and the
// blr are reused in place: no cave needed.
static void apply_map_node_anchor(so_module *mod, float zoom, float design_w, float design_h) {
  if (zoom < 0.05f) zoom = 0.05f;
  // X: centre the 256*zoom-wide view on the canvas (ct_nx formula; identity
  // (designW-480)/2 = ctr::x_offset at stock 1.875). Y: ct_nx formula, which
  // also drops the year panel from the mobile button-row reserve to the
  // bottom edge, where remove_mobile_ui leaves the space free.
  const float node_x = design_w * 0.5f - 128.0f * zoom;
  const float node_y = design_h * 0.5f - 96.0f * zoom + 12.0f;
  // The HUD is not drawn on the scene: WorldImpl::drawWorld renders the
  // "windows" (year plate etc.) into two 544x256 RenderTextures that are
  // children of this node (getWnd(i) = tag 500+i), so they ride along with
  // any node shift -- on 640x480 the -48 shift pushed "1000 A.D." off the
  // left edge. Both wnd layers take their X from one constant (Init2
  // 0x607a2c: mov w9,#80.0 -> s8, used by tags 500 and 501; the map planes
  // at 0x607950 have their own copy and are untouched). Move them right by
  // the node shift in node-local (art) units: (stock_x - node_x) / zoom =
  // 128 - 240/zoom (0 at stock, 8 at 2.0, 21.3 at 2.25).
  const float stock_x = (design_w - 480.0f) * 0.5f;
  const float wnd_x = 80.0f + (stock_x - node_x) / zoom;
  const PatchEntry w[1] = {
    { NULL, 0, 0x607a2c, 0x52a85409, movz_topf(9, rn16(wnd_x)),
      "WorldMap::Init2 wnd layers (tags 500/501) setPosition X: 80.0 -> 80 + (stock_x - node_x)/zoom" },
  };
  apply_patches(mod, w, 1);
  const PatchEntry e[7] = {
    { NULL, 0, 0x607cbc, 0x52b87408, movz_topf(9, rn16(node_x)),  "WorldMap::Init2 setPosition: mov w8,#-320.0 -> movz w9,#node_x" },
    { NULL, 0, 0x607cc0, 0xbd409fe0, fmov_s_from_w(0, 9),        "WorldMap::Init2 setPosition: ldr s0,[sp,#156] -> fmov s0,w9" },
    { NULL, 0, 0x607cc4, 0xf0002de9, movz_topf(10, rn16(node_y)), "WorldMap::Init2 setPosition: adrp x9 -> movz w10,#node_y" },
    { NULL, 0, 0x607cc8, 0x1e270101, fmov_s_from_w(1, 10),       "WorldMap::Init2 setPosition: fmov s1,w8 -> fmov s1,w10" },
    { NULL, 0, 0x607ccc, 0xf9465529, NOP_WORD,                   "WorldMap::Init2 setPosition: ldr x9,[x9,#3240] -> nop" },
    { NULL, 0, 0x607cdc, 0x1e212801, NOP_WORD,                   "WorldMap::Init2 setPosition: fadd s1,s0,s1 -> nop" },
    { NULL, 0, 0x607ce0, 0xbd400120, NOP_WORD,                   "WorldMap::Init2 setPosition: ldr s0,[x9] -> nop" },
  };
  apply_patches(mod, e, 7);
}

// The world-map OVERVIEW (WorldMap::enterMiniMap) is the "worldmap" node
// itself rescaled from zoom to (0.3125, 0.2778) -- the whole 1536x1024 map at
// the 4:3 design width (x0.5556 on 16:9) -- so it keeps the node position we
// set in Init2: on 640x480 it sat 64 px right of centre, cut off at the panel
// edge. Its X is correct at the node's STOCK position, so enterMiniMap sets
// the node X to stock_x and exitMiniMap puts node_x back. Both functions
// fetch the node with getChildByName("worldmap") and, the name being a short
// string, skip straight to the `ldr x8,[x0]` before their setScale -- that
// word is the site in each (the branch-not-taken path lands there too).
// Enter: x21 is dead (rewritten right after). Exit: x21 is live, x22 is
// free, and w9 (loaded one word earlier for the setScale that follows) is
// re-loaded after the call by copying that word. Y is left alone: the stock
// layout keeps the map above the year label.
static void apply_map_minimap_anchor(so_module *mod, float stock_x, float node_x) {
  const uintptr_t base = (uintptr_t)mod->load_base;
  const uint32_t CAVE = 0xd0218;                 // inside the 808-byte zero block, past the text caves
  const uint32_t SETPOSX = 0x889058;             // cocos2d::Node::setPositionX(float)
  const uint32_t ENTER = 0x60a2b4, EXIT = 0x609c24;   // both: ldr x8,[x0] with x0 = the node
  const uint32_t sb = f32_bits(stock_x), nb = f32_bits(node_x);
  uint32_t w[16]; int n = 0, off_exit;
  // enter: node X -> stock_x
  w[n++] = 0xaa0003f5u;                          // mov x21, x0
  w[n++] = movz_w(16, (uint16_t)(sb & 0xffff));
  w[n++] = movk_w_hi(16, (uint16_t)(sb >> 16));
  w[n++] = fmov_s_from_w(0, 16);
  w[n++] = encode_bl(base + CAVE + n * 4, base + SETPOSX);
  w[n++] = 0xaa1503e0u;                          // mov x0, x21
  w[n++] = 0xf9400008u;                          // ldr x8, [x0]
  w[n++] = encode_b(base + CAVE + n * 4, base + ENTER + 4);
  // exit: node X -> node_x
  off_exit = n * 4;
  w[n++] = 0xaa0003f6u;                          // mov x22, x0
  w[n++] = movz_w(16, (uint16_t)(nb & 0xffff));
  w[n++] = movk_w_hi(16, (uint16_t)(nb >> 16));
  w[n++] = fmov_s_from_w(0, 16);
  w[n++] = encode_bl(base + CAVE + n * 4, base + SETPOSX);
  w[n++] = 0xaa1603e0u;                          // mov x0, x22
  // w9 was loaded one word before the site and is consumed by the setScale
  // after it; the call clobbered it. Re-issue whatever instruction sits there
  // NOW -- stock `mov w9,#0x5555`, or apply_map_zoom's `movz w9,#top16(zoom)`
  // (assuming the stock word here once scaled the node to ~0: map gone, only
  // the separately scaled clouds left).
  __builtin_memcpy(&w[n], (const void *)(base + EXIT - 4), 4); n++;
  w[n++] = 0xf9400008u;                          // ldr x8, [x0]
  w[n++] = encode_b(base + CAVE + n * 4, base + EXIT + 4);
  const uint8_t *cur = (const uint8_t *)(base + CAVE);
  for (int i = 0; i < n * 4; i++)
    if (cur[i]) {
      fprintf(stderr, "ct: patches: minimap anchor cave @0x%x is not empty -- skipping\n", CAVE);
      return;
    }
  __builtin_memcpy((void *)(base + CAVE), w, (size_t)n * 4);
  const PatchEntry e[2] = {
    { NULL, 0, ENTER, 0xf9400008u, encode_b(base + ENTER, base + CAVE),
      "WorldMap::enterMiniMap: node setPositionX(stock_x) via cave" },
    { NULL, 0, EXIT, 0xf9400008u, encode_b(base + EXIT, base + CAVE + off_exit),
      "WorldMap::exitMiniMap: node setPositionX(node_x) via cave" },
  };
  apply_patches(mod, e, 2);
}

// map_zoom_fix has no camera part. Patching WorldMap::setScroll's 128.0
// (0x6098cc, the half-view constant that registers the player horizontally)
// to 240/zoom was tried and REMOVED: on hardware it moved the map plane but not
// the objects (WorldObjectManager places them from its own state), so the
// player stood 21 art px off his tile. Horizontal centring is the node's job
// (apply_map_node_anchor) and the 2-px map rule keeps the 256-column SNES
// window inside the panel.

// ---------------------------------------------------------------------------
// text_scale_fix -- draw system-font labels 1:1.
//
// The engine runs cocos2d with Director::setContentScaleFactor(2.0)
// (AppDelegate, 0x641af8: its art is @2x the design points). For text that
// means Texture2D::initWithString asks the platform for the font at
// points x 2 (a 12-pt label -> a 24 px bitmap), and the resulting sprite is
// sized in points (px / 2) and then drawn at the design->panel scale. Net:
// every text bitmap is drawn at design_scale / 2 -- 1:1 only where the
// design scale is 2 (720p), and 2/3 on a 640x480 panel (design 480x360,
// scale 1.333): a 24 px bitmap squeezed to 16 px by nearest sampling, which
// is why no glyph size ever looked clean on 4:3 (measured with a frame drawn
// around each bitmap: 232x36 -> 153x24, 102x20 -> 68x13).
//
// Fix: render the bitmap at PANEL resolution and undo the point-space shrink:
//   1. Texture2D::initWithString 0x93a324  `ldr s1,[x0,#392]` (the CSF that
//      multiplies fontSize / dimensions / stroke) -> design_scale via cave:
//      a 12-pt label becomes a 16 px request on 640x480 (= 1x pixel font).
//   2. Label::createSpriteForSystemFont 0x86af40, right after
//      Sprite::createWithTexture: setScale(CSF/design_scale) on the sprite
//      (a 16 px texture is 8 pt at CSF 2; x1.5 -> 12 pt -> 16 px on screen).
//   3. same function 0x86afa4..b4 `setContentSize(sprite->getContentSize())`
//      -> the size x CSF/design_scale, so the label's box (used for
//      anchoring/centring) matches what is drawn.
//   4. Label::createShadowSpriteForSystemFont 0x86ab3c: same setScale on
//      the engine's shadow sprite (unused by this game's labels, kept
//      consistent).
// A scale of exactly 1 (design_scale == CSF, i.e. 720p) is a no-op, so the
// 16:9 look is unchanged. w16 is the scratch register (IP0, dead at all
// four sites -- neither function touches x16/x17); x21 is dead between
// createWithTexture and its reload at 0x86af5c. Caves live in the 808-byte
// zero block at 0xd0118 (ct_nx's debug cave, otherwise unused here).
// ---------------------------------------------------------------------------
static void apply_text_scale_fix(so_module *mod, float design_scale) {
  const uintptr_t base = (uintptr_t)mod->load_base;
  const uint32_t CSF_SITE = 0x641af8;            // AppDelegate: fmov s0,#2.0 -> setContentScaleFactor
  uint32_t csf_word; __builtin_memcpy(&csf_word, (const void *)(base + CSF_SITE), 4);
  if (csf_word != 0x1e201000u) {
    fprintf(stderr, "ct: patches: text_scale_fix: content scale factor site 0x%x is %08x, not fmov s0,#2.0 -- skipping\n",
            CSF_SITE, csf_word);
    return;
  }
  const float csf = 2.0f;
  if (design_scale < 0.05f) return;
  const float k = csf / design_scale;            // sprite scale (1.5 on 640x480, 1.0 at 720p)
  if (fabsf(k - 1.0f) < 1e-3f) {
    debugPrintf("patches: text_scale_fix: design scale == content scale factor, nothing to do\n");
    return;
  }
  const uint32_t CAVE = 0xd0118;
  const uint32_t SETSCALE = 0x888ebc;            // cocos2d::Node::setScale(float)
  uint32_t w[40]; int n = 0;
  uint32_t off1, off2, off3, off4;
  const uint32_t fb = f32_bits(design_scale), kb = f32_bits(k);
  // cave 1: s1 = design_scale; back to 0x93a328
  off1 = n * 4;
  w[n++] = movz_w(16, (uint16_t)(fb & 0xffff));
  w[n++] = movk_w_hi(16, (uint16_t)(fb >> 16));
  w[n++] = fmov_s_from_w(1, 16);
  w[n++] = encode_b(base + CAVE + n * 4, base + 0x93a328);
  // cave 2: x0 = sprite -> setScale(k); restore x0; replaced `ldr x8,[x0]`; back to 0x86af44
  off2 = n * 4;
  w[n++] = 0xaa0003f5u;                          // mov x21, x0
  w[n++] = movz_w(16, (uint16_t)(kb & 0xffff));
  w[n++] = movk_w_hi(16, (uint16_t)(kb >> 16));
  w[n++] = fmov_s_from_w(0, 16);
  w[n++] = encode_bl(base + CAVE + n * 4, base + SETSCALE);
  w[n++] = 0xaa1503e0u;                          // mov x0, x21
  w[n++] = 0xf9400008u;                          // ldr x8, [x0]
  w[n++] = encode_b(base + CAVE + n * 4, base + 0x86af44);
  // cave 3: x0 = &sprite content size -> label->setContentSize(size * k); back to 0x86afb8
  off3 = n * 4;
  w[n++] = 0x2d400400u;                          // ldp s0, s1, [x0]
  w[n++] = movz_w(16, (uint16_t)(kb & 0xffff));
  w[n++] = movk_w_hi(16, (uint16_t)(kb >> 16));
  w[n++] = fmov_s_from_w(2, 16);
  w[n++] = 0x1e220800u;                          // fmul s0, s0, s2
  w[n++] = 0x1e220821u;                          // fmul s1, s1, s2
  w[n++] = 0xd10043ffu;                          // sub sp, sp, #16
  w[n++] = 0x2d0007e0u;                          // stp s0, s1, [sp]
  w[n++] = 0x910003e1u;                          // mov x1, sp
  w[n++] = 0xaa1303e0u;                          // mov x0, x19   (the Label)
  w[n++] = 0xf9400268u;                          // ldr x8, [x19]
  w[n++] = 0xf940b108u;                          // ldr x8, [x8, #352]  (setContentSize slot)
  w[n++] = 0xd63f0100u;                          // blr x8
  w[n++] = 0x910043ffu;                          // add sp, sp, #16
  w[n++] = encode_b(base + CAVE + n * 4, base + 0x86afb8);
  // cave 4: shadow sprite: replaced `str x0,[x19,#968]`, setScale(k); back to 0x86ab40
  off4 = n * 4;
  w[n++] = 0xf901e660u;                          // str x0, [x19, #968]
  w[n++] = movz_w(16, (uint16_t)(kb & 0xffff));
  w[n++] = movk_w_hi(16, (uint16_t)(kb >> 16));
  w[n++] = fmov_s_from_w(0, 16);
  w[n++] = encode_bl(base + CAVE + n * 4, base + SETSCALE);
  w[n++] = encode_b(base + CAVE + n * 4, base + 0x86ab40);
  // the cave must be untouched zero bytes
  const uint8_t *cur = (const uint8_t *)(base + CAVE);
  for (int i = 0; i < n * 4; i++)
    if (cur[i]) {
      fprintf(stderr, "ct: patches: text_scale_fix: cave @0x%x is not empty -- skipping\n", CAVE);
      return;
    }
  __builtin_memcpy((void *)(base + CAVE), w, (size_t)n * 4);
  const PatchEntry e[4] = {
    { NULL, 0, 0x93a324, 0xbd418801u, encode_b(base + 0x93a324, base + CAVE + off1),
      "Texture2D::initWithString: font px = pt x design_scale (was x content scale factor 2)" },
    { NULL, 0, 0x86af40, 0xf9400008u, encode_b(base + 0x86af40, base + CAVE + off2),
      "Label::createSpriteForSystemFont: text sprite setScale(CSF/design_scale)" },
    { NULL, 0, 0x86afa4, 0xf9400268u, encode_b(base + 0x86afa4, base + CAVE + off3),
      "Label::createSpriteForSystemFont: label content size x CSF/design_scale" },
    { NULL, 0, 0x86ab3c, 0xf901e660u, encode_b(base + 0x86ab3c, base + CAVE + off4),
      "Label::createShadowSpriteForSystemFont: shadow sprite setScale(CSF/design_scale)" },
  };
  apply_patches(mod, e, 4);
  fprintf(stderr, "ct: text_scale_fix: labels at panel resolution (font px = pt x %g, sprite x %g)\n",
          (double)design_scale, (double)k);
}

// Resolve the per-panel framing from config + the engine's frame size (the
// internal FBO when render_scale < 1, else the panel).
typedef struct {
  int   frame_w, frame_h;
  float design_scale, design_w, design_h;
  float field_zoom, map_zoom;
} CtFraming;

static CtFraming ct_framing_resolve(void) {
  extern Config config;
  CtFraming f;
  int fw = screen_width, fh = screen_height;
#ifndef __SWITCH__
  ct_rescale_engine_size(&fw, &fh);
#endif
  if (fw <= 0) fw = 1280;
  if (fh <= 0) fh = 720;
  f.frame_w = fw; f.frame_h = fh;
  // Auto design scale. Wide panels: an integer multiple of the 640-wide 16:9
  // canvas (1280x720 -> 2, 640x360 exact). Narrow panels (4:3, 1:1): the
  // engine's own 480-wide 4:3 layout -- its menus are laid out for a 480x360
  // box, so a 1:1 640x480 canvas leaves them centred in dead space (tried on
  // the RG40XX-H; rejected). 640x480 -> 1.333 (design 480x360, stock UI size),
  // 720x720 -> 1.5, 1024x768 -> 2.133. Field/map art stays integer regardless
  // (field_zoom auto compensates), only UI sprites carry the fractional scale.
  float s = config.design_scale;
  if (s <= 0.0f) {
    const float aspect = (float)fw / (float)fh;
    if (aspect >= 1.6f) s = floorf((float)fw / 640.0f);
    else                s = (float)fw / 480.0f;
    if (s < 1.0f) s = 1.0f;
  }
  f.design_scale = s;
  f.design_w = (float)fw / s;
  f.design_h = (float)fh / s;
  float z = config.field_zoom;
  if (z <= 0.0f) {
    float p;
    if ((float)fw / (float)fh >= 1.6f) {
      // Wide panels: the smallest whole px/art whose rows fit the 432x224
      // plane (720p -> 4 px, 320x180 art: the ct_nx look).
      p = ceilf((float)fh / 220.0f);
    } else {
      // Narrow panels: the SNES's 256 columns on screen (640 -> 2 px, 1024 ->
      // 4 px), bumped only if that would ask for more than a 240-row picture
      // (720x720 -> 3 px). 3 px on 640x480 showed 213x160 -- too little room
      // (user, RG40XX-H); 2 px shows 320x220 with 20 px letterbox bars.
      p = floorf((float)fw / 256.0f);
      if (p < 1.0f) p = 1.0f;
      while ((float)fh / p > 240.0f) p += 1.0f;
    }
    if (p < 1.0f) p = 1.0f;
    z = p / s;
  }
  f.field_zoom = z;
  // Auto map zoom: same px/art as the field, but capped so the SNES 256-column
  // world-map window fits the panel width. The map's screen-fixed sprites (the
  // "1000 A.D." year plate at the bottom-left) sit inside those 256 columns, so
  // 3 px on a 640-wide panel (768 px) clips the plate off the left edge
  // (tried on the RG40XX-H; rejected). 640x480 -> 2 px (map_zoom 1.5, plate
  // visible, more map than the SNES window since the planes are 544 wide),
  // 1280x720 -> 4 px, 640x360 -> 2 px (unchanged).
  float mz = config.map_zoom;
  if (mz <= 0.0f) {
    float pm = floorf((float)fw / 256.0f);
    const float pf = z * s;
    if (pm < 1.0f) pm = 1.0f;
    if (pm > pf) pm = pf;
    mz = pm / s;
  }
  f.map_zoom = mz;
  return f;
}

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

  if (config.remove_mobile_ui) {
    debugPrintf("patches: applying remove_mobile_ui\n");
    apply_patches(mod, g_mobile_ui_patches, PATCH_COUNT(g_mobile_ui_patches));
  }

  if (config.controller_glyphs) {
    debugPrintf("patches: applying controller_glyphs\n");
    apply_patches(mod, g_glyph_patches, PATCH_COUNT(g_glyph_patches));
  }

  if (config.fix_diagonal_movement) {
    debugPrintf("patches: applying diagonal_movement fix\n");
    apply_patches(mod, g_diagonal_patches, PATCH_COUNT(g_diagonal_patches));
  }

  // Framing cluster (section 5). The field/map zoom patches assume the
  // ui_scale_fix canvas -- without it the engine keeps its 568/480-wide designs
  // and the zoomed view no longer matches the canvas -- so they are gated on it.
  if (config.ui_scale_fix || config.field_zoom_fix || config.map_zoom_fix || config.game_area_width_fix) {
    const CtFraming fr = ct_framing_resolve();
    fprintf(stderr, "ct: framing: frame %dx%d design %gx%g (scale %g) field_zoom %g "
                    "(%g px/art, %gx%g art visible) map_zoom %g%s\n",
            fr.frame_w, fr.frame_h, (double)fr.design_w, (double)fr.design_h,
            (double)fr.design_scale, (double)fr.field_zoom,
            (double)(fr.field_zoom * fr.design_scale),
            (double)(fr.design_w / fr.field_zoom), (double)(fr.design_h / fr.field_zoom),
            (double)fr.map_zoom, config.ui_scale_fix ? "" : " [ui_scale_fix off: zoom patches skipped]");
    // UI font size: config font_scale, else auto by panel (see config.h).
    if (config.ui_scale_fix) {
      if (config.text_scale_fix) apply_text_scale_fix(mod, fr.design_scale);
      if (config.field_zoom_fix) {
        debugPrintf("patches: applying field_zoom_fix (zoom=%g)\n", (double)fr.field_zoom);
        apply_patches(mod, g_field_zoom_fix_patches, PATCH_COUNT(g_field_zoom_fix_patches));
        apply_field_view_zoom(mod, fr.field_zoom, fr.design_h);
        apply_field_zoom(mod, fr.field_zoom);
        apply_field_node_anchor(mod, fr.field_zoom, fr.design_w, fr.design_h);
      }
      if (config.map_zoom_fix) {
        debugPrintf("patches: applying map_zoom_fix (zoom=%g)\n", (double)fr.map_zoom);
        apply_map_zoom(mod, fr.map_zoom);
        apply_map_node_anchor(mod, fr.map_zoom, fr.design_w, fr.design_h);
        if (config.map_minimap_fix)
          apply_map_minimap_anchor(mod, (fr.design_w - 480.0f) * 0.5f,
                                        fr.design_w * 0.5f - 128.0f * fr.map_zoom);
        // No camera patch: shifting WorldMap::setScroll moved the map but not
        // the objects on hardware (see the note above ct_framing_resolve).
      }
    }
    if (config.game_area_width_fix) {
      debugPrintf("patches: applying game_area_width_fix\n");
      apply_patches(mod, g_gamearea_patches, PATCH_COUNT(g_gamearea_patches));
    }
    if (config.ui_scale_fix) {
      debugPrintf("patches: design resolution %gx%g (all aspect-table entries)\n", (double)fr.design_w, (double)fr.design_h);
      apply_ui_scale_fix(mod, fr.design_w, fr.design_h);
    }
  }
}

#endif // __PATCHES_H__
