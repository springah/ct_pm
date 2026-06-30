/* movelog.c -- CT_MOVELOG: per-frame diagonal-movement stutter logger
 *
 * See movelog.h for full offset documentation and methodology.
 *
 * How the trampoline works
 * ------------------------
 * hook_arm64(addr, dst) overwrites the first 16 bytes of the target function
 * with a position-independent LDR X17, #8 / BR X17 / <64-bit dst> sequence.
 * That's exactly 3 words (12 bytes), but the loader aligns hook slots to 16.
 * The original 4 instructions (16 bytes) that were there are saved into a
 * small heap trampoline buffer so the real function can still run: we call the
 * trampoline, let it fall through to NanameHantei+16, done.  After it returns
 * we read the move-state fields from FieldImpl::this (which was x0 on entry).
 *
 * On AArch64, the register calling convention says x0 is the 'this' pointer for
 * a C++ member function.  Our wrapper receives x0 via the plain C ABI and sees
 * the same FieldImpl* -- no C++ headers needed.
 *
 * Compilation note
 * ----------------
 * This file uses GNU C nested function syntax (__attribute__((constructor))) --
 * it is plain C99 + POSIX.  No C++ required.  Link with the same flags as the
 * rest of source/.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "so_util.h"   /* so_module, hook_arm64, so_find_addr */
#include "util.h"      /* os_log / debugPrintf */

/* -------------------------------------------------------------------------
 * Offsets verified against libchrono.so v2.1.5 disassembly (see header).
 * ------------------------------------------------------------------------- */
#define NANAME_HANTEI_OFFSET   0x59acfc  /* FieldImpl::NanameHantei() vaddr */

/* From FieldImpl 'this': this[2152] = move_state* */
#define MS_PTR_OFFSET          2152

/* Fields inside move_state (all int32 unless noted) */
#define MS_DIR6                304   /* 6-bit direction code */
#define MS_SUBPIX_X            312   /* X sub-pixel accumulator (lo32 of 64-bit pair) */
#define MS_SUBPIX_Y            316   /* Y sub-pixel accumulator (hi32 of 64-bit pair) */
#define MS_SPEED_MAIN          320   /* main-axis step speed */
#define MS_DX                  344   /* X pixel step applied this frame  *** KEY *** */
#define MS_DY_FLAG             348   /* Y direction flag / carry */
#define MS_DY                  352   /* Y pixel step applied this frame  *** KEY *** */
#define MS_DX2                 356   /* secondary X step (diagonal cross-axis) */
#define MS_DIR_MASK            336   /* direction bitmask byte; bit7 = diagonal */

/* Size of the stolen prologue we must preserve for the trampoline. */
#define STOLEN_BYTES           16    /* 4 AArch64 instructions */

/* -------------------------------------------------------------------------
 * Trampoline state
 * ------------------------------------------------------------------------- */

/* Pointer to the original function body (installed as a callable stub). */
static void (*s_orig_naname)(void *this_ptr);

/* -------------------------------------------------------------------------
 * The wrapper: called in place of NanameHantei().
 * x0 = FieldImpl* this  (AArch64 C calling convention == C++ member 'this')
 * ------------------------------------------------------------------------- */
static void movelog_wrapper(void *this_ptr) {
    /* Call the real NanameHantei via the trampoline. */
    s_orig_naname(this_ptr);

    /* Read move_state* from this[2152]. */
    void *ms_ptr = *(void **)((char *)this_ptr + MS_PTR_OFFSET);
    if (!ms_ptr)
        return;

    const char *ms = (const char *)ms_ptr;

    int32_t dx         = *(const int32_t *)(ms + MS_DX);
    int32_t dy         = *(const int32_t *)(ms + MS_DY);
    int32_t dx2        = *(const int32_t *)(ms + MS_DX2);
    int32_t dy_flag    = *(const int32_t *)(ms + MS_DY_FLAG);
    int32_t spix_x     = *(const int32_t *)(ms + MS_SUBPIX_X);
    int32_t spix_y     = *(const int32_t *)(ms + MS_SUBPIX_Y);
    int32_t speed_main = *(const int32_t *)(ms + MS_SPEED_MAIN);
    uint8_t dir_mask   = *(const uint8_t *)(ms + MS_DIR_MASK);

    /* Throttle: only log when there is actual movement (dx or dy nonzero). */
    if (dx == 0 && dy == 0 && speed_main == 0)
        return;

    /* Diagonal flag: bit 7 of dir_mask is set by GetSub (diagonal) paths. */
    int is_diag = (dir_mask & 0x80) ? 1 : 0;

    fprintf(stderr,
        "MOVELOG: %s dx=%4d dy=%4d dx2=%4d dy_flag=%3d spix_x=%08x spix_y=%08x speed=%d dir=%02x\n",
        is_diag ? "DIAG" : "CARD",
        dx, dy, dx2, dy_flag,
        (unsigned)spix_x, (unsigned)spix_y,
        speed_main,
        (unsigned)dir_mask);
}

/* -------------------------------------------------------------------------
 * Install the trampoline.
 * Must be called BEFORE so_finalize() -- while the .text segment is still RW.
 * ------------------------------------------------------------------------- */
void movelog_install(so_module *game_mod) {
    if (!getenv("CT_MOVELOG"))
        return;

    uintptr_t fn_addr = (uintptr_t)game_mod->load_virtbase + NANAME_HANTEI_OFFSET;

    /* Allocate an executable trampoline: stolen bytes + LDR X17,#8 + BR X17 + <addr64> */
    const size_t tramp_size = STOLEN_BYTES + 16; /* stolen + trampoline branch */
    uint8_t *tramp = (uint8_t *)mmap(NULL, tramp_size,
                                     PROT_READ | PROT_WRITE | PROT_EXEC,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) {
        fprintf(stderr, "CT_MOVELOG: mmap trampoline failed\n");
        return;
    }

    /* Copy the first STOLEN_BYTES (4 instructions) from NanameHantei.
     * The hook_arm64 call below will overwrite these 16 bytes in the original. */
    memcpy(tramp, (void *)fn_addr, STOLEN_BYTES);

    /* Append a branch back to fn_addr+STOLEN_BYTES so the trampoline continues
     * into the rest of the real function. */
    uint32_t *branch_slot = (uint32_t *)(tramp + STOLEN_BYTES);
    branch_slot[0] = 0x58000051u; /* LDR X17, #8 */
    branch_slot[1] = 0xd61f0220u; /* BR  X17 */
    *(uint64_t *)&branch_slot[2] = (uint64_t)(fn_addr + STOLEN_BYTES);

    /* Flush I-cache for the new trampoline. */
    __builtin___clear_cache((char *)tramp, (char *)tramp + tramp_size);

    /* Store the trampoline callable.  The ABI is: x0 = this_ptr (same as original). */
    s_orig_naname = (void (*)(void *))tramp;

    /* Overwrite the original function with a branch to our wrapper.
     * hook_arm64 writes:  LDR X17, #8 / BR X17 / <64-bit dst>  (16 bytes total). */
    hook_arm64(fn_addr, (uintptr_t)movelog_wrapper);

    /* The caller (main.c) will call so_flush_caches() + so_finalize() immediately
     * after, which flushes our patch into the I-cache as part of its normal pass. */

    fprintf(stderr, "CT_MOVELOG: installed at libchrono+0x%lx -> tramp %p\n",
            (unsigned long)NANAME_HANTEI_OFFSET, tramp);
}
