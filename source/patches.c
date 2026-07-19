/* patches.c -- runtime ARM64 instruction patches for libchrono.so
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

#include <stdint.h>
#include "patches.h"
#include "so_util.h"
#include "util.h"
#include "config.h"

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

  // Cave @ 0x5a15cc (40 bytes of now-dead code after the 4 redirects above;
  // verified empty/unreachable -- nothing else in the function branches here).
  P_CAVE(0x5a15cc, 0x29522d0a, "cave: ldp w10,w11,[x8,#0x90]   ; w10,w11 = raw per-axis step just stored (same magnitude cardinal uses)"),
  P_CAVE(0x5a15d0, 0xb940990c, "cave: ldr w12,[x8,#0x98]       ; w12 = current X target"),
  P_CAVE(0x5a15d4, 0xb0a018c, "cave: add w12,w12,w10          ; X target += raw X step (no normalization, no truncation loss)"),
  P_CAVE(0x5a15d8, 0xb900990c, "cave: str w12,[x8,#0x98]"),
  P_CAVE(0x5a15dc, 0xb900a10c, "cave: str w12,[x8,#0xa0]       ; commit X (matches cardinal's commit site)"),
  P_CAVE(0x5a15e0, 0xb940a509, "cave: ldr w9,[x8,#0xa4]        ; w9 = current Y target"),
  P_CAVE(0x5a15e4, 0xb0b0129, "cave: add w9,w9,w11            ; Y target += raw Y step"),
  P_CAVE(0x5a15e8, 0xb900a509, "cave: str w9,[x8,#0xa4]"),
  P_CAVE(0x5a15ec, 0xb900ad09, "cave: str w9,[x8,#0xac]        ; commit Y (matches cardinal's commit site)"),
  P_CAVE(0x5a15f0, 0x14000036, "cave: b 0x5a16c8 (UserScroll epilogue, restores x19/x29/x30 and returns)"),
};

// ---------------------------------------------------------------------------
// Apply every enabled feature group. Called from main() while game_mod's .text
// is still RW (before so_finalize), and only when the v2.1.5 fingerprint
// matched (g_libchrono_v215). Each group is independent and config-gated;
// additional groups are appended here as they are ported from ct_nx.
// ---------------------------------------------------------------------------
void apply_game_patches(so_module *mod) {
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
}
