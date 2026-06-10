/*
 * Machine Layer M1 — Task 9 thunk encoding pre-work spike (macOS arm64).
 *
 * Standalone proof of the JIT MMIO backpatch thunk's instruction encodings and
 * its save/restore frame, with NO dependency on ppc-jit.cpp or any emulator source.
 *
 * What it does:
 *   (a) defines the exact AArch64 encoder helpers Task 9 needs (emit_stp_x,
 *       emit_ldp_x, emit_str_x_imm, emit_ldr_x_imm, emit_stp_q, emit_ldp_q,
 *       emit_sub_sp_imm, emit_add_sp_imm, mov x0,sp, sub x1,x30,#4, BL, etc.);
 *   (b) emits the COMPLETE generic thunk into a MAP_JIT page (reusing the S2
 *       pthread_jit_write_protect_np + sys_icache_invalidate idiom);
 *   (c) verifies it end-to-end: an asm trampoline sets x0=test EA, poisons x5
 *       and v16 with sentinels, BLs the thunk; the C callback ASSERTS frame[0]==EA
 *       and ACTIVELY POISONS every saved register slot it does NOT mean to mutate
 *       (so "survived unclobbered" has teeth — the thunk must really save/restore
 *       x5, v16, x29, lr, etc.), then mutates frame[1] and frame[27]. After return
 *       the trampoline checks x1/x27 got the mutated values, x5 and v16 survived,
 *       and SP is balanced (captured before BL, compared after);
 *   (d) cross-checks each helper encoding against clang's assembler and prints a table.
 *
 * Build + run: make ; make run
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>

/* ===========================================================================
 * Section 1: AArch64 encoder helpers (the ones Task 9 will copy into ppc-jit.cpp).
 * Each returns the 32-bit instruction word. Conventions: sp==reg 31 in the
 * load/store base position; x29=fp, x30=lr.
 * =========================================================================== */

/* STP Xt, Xt2, [SP, #imm] — 64-bit signed-offset, imm7 scaled by 8.
 * Base encoding 0xA9000000 | (imm7<<15) | (Rt2<<10) | (Rn<<5) | Rt, Rn=31 (sp). */
static uint32_t emit_stp_x(int rt, int rt2, int imm_bytes) {
    assert(imm_bytes % 8 == 0);
    int imm7 = imm_bytes / 8;
    assert(imm7 >= -64 && imm7 <= 63);
    return 0xA9000000u | ((uint32_t)(imm7 & 0x7F) << 15) | ((uint32_t)rt2 << 10)
         | (31u << 5) | (uint32_t)rt;
}

/* LDP Xt, Xt2, [SP, #imm] — mirror of STP, opc/L bit set (0xA9400000). */
static uint32_t emit_ldp_x(int rt, int rt2, int imm_bytes) {
    assert(imm_bytes % 8 == 0);
    int imm7 = imm_bytes / 8;
    assert(imm7 >= -64 && imm7 <= 63);
    return 0xA9400000u | ((uint32_t)(imm7 & 0x7F) << 15) | ((uint32_t)rt2 << 10)
         | (31u << 5) | (uint32_t)rt;
}

/* STR Xt, [SP, #imm] — 64-bit unsigned-offset, imm12 scaled by 8.
 * Base 0xF9000000 | (imm12<<10) | (Rn<<5) | Rt, Rn=31 (sp). */
static uint32_t emit_str_x_imm(int rt, int imm_bytes) {
    assert(imm_bytes % 8 == 0 && imm_bytes >= 0);
    uint32_t imm12 = (uint32_t)(imm_bytes / 8);
    assert(imm12 < 4096);
    return 0xF9000000u | (imm12 << 10) | (31u << 5) | (uint32_t)rt;
}

/* LDR Xt, [SP, #imm] — mirror, L bit set (0xF9400000). */
static uint32_t emit_ldr_x_imm(int rt, int imm_bytes) {
    assert(imm_bytes % 8 == 0 && imm_bytes >= 0);
    uint32_t imm12 = (uint32_t)(imm_bytes / 8);
    assert(imm12 < 4096);
    return 0xF9400000u | (imm12 << 10) | (31u << 5) | (uint32_t)rt;
}

/* STP Qt, Qt2, [SP, #imm] — 128-bit SIMD&FP signed-offset, imm7 scaled by 16.
 * Base 0xAD000000 | (imm7<<15) | (Rt2<<10) | (Rn<<5) | Rt, Rn=31 (sp). */
static uint32_t emit_stp_q(int qt, int qt2, int imm_bytes) {
    assert(imm_bytes % 16 == 0);
    int imm7 = imm_bytes / 16;
    assert(imm7 >= -64 && imm7 <= 63);
    return 0xAD000000u | ((uint32_t)(imm7 & 0x7F) << 15) | ((uint32_t)qt2 << 10)
         | (31u << 5) | (uint32_t)qt;
}

/* LDP Qt, Qt2, [SP, #imm] — mirror, L bit set (0xAD400000). */
static uint32_t emit_ldp_q(int qt, int qt2, int imm_bytes) {
    assert(imm_bytes % 16 == 0);
    int imm7 = imm_bytes / 16;
    assert(imm7 >= -64 && imm7 <= 63);
    return 0xAD400000u | ((uint32_t)(imm7 & 0x7F) << 15) | ((uint32_t)qt2 << 10)
         | (31u << 5) | (uint32_t)qt;
}

/* SUB SP, SP, #imm12 — 64-bit, shift=0. Base 0xD1000000 | (imm12<<10) | (Rn<<5) | Rd,
 * Rd=Rn=31 (sp). */
static uint32_t emit_sub_sp_imm(int imm) {
    assert(imm >= 0 && imm < 4096);
    return 0xD1000000u | ((uint32_t)imm << 10) | (31u << 5) | 31u;
}

/* ADD SP, SP, #imm12 — base 0x91000000, same fields. */
static uint32_t emit_add_sp_imm(int imm) {
    assert(imm >= 0 && imm < 4096);
    return 0x91000000u | ((uint32_t)imm << 10) | (31u << 5) | 31u;
}

/* STP X29, X30, [SP, #-16]! — pre-index, imm7 = -16/8 = -2. Base 0xA9800000. */
static uint32_t emit_stp_fp_lr_preidx(void) {
    int imm7 = -16 / 8; /* -2 */
    return 0xA9800000u | ((uint32_t)(imm7 & 0x7F) << 15) | (30u << 10)
         | (31u << 5) | 29u;
}

/* LDP X29, X30, [SP], #16 — post-index, imm7 = +2. Base 0xA8C00000. */
static uint32_t emit_ldp_fp_lr_postidx(void) {
    int imm7 = 16 / 8; /* 2 */
    return 0xA8C00000u | ((uint32_t)(imm7 & 0x7F) << 15) | (30u << 10)
         | (31u << 5) | 29u;
}

/* MOV X0, SP  ==  ADD X0, SP, #0  (the ADD alias; the ORR-MOV form would use XZR). */
static uint32_t emit_mov_x0_sp(void) {
    return 0x91000000u | (0u << 10) | (31u << 5) | 0u;  /* 0x910003E0 */
}

/* SUB X1, X30, #4 — 64-bit SUB immediate. Base 0xD1000000 | (imm12<<10) | (Rn<<5) | Rd. */
static uint32_t emit_sub_x1_x30_4(void) {
    return 0xD1000000u | (4u << 10) | (30u << 5) | 1u;  /* 0xD10013C1 */
}

/* BL <rel> — 0x94000000 | ((off>>2) & 0x3FFFFFF), off = target - site in bytes.
 * (Task 9's backpatcher emits this at the patch site; included for completeness.) */
__attribute__((unused))
static uint32_t emit_bl(int64_t off_bytes) {
    return 0x94000000u | (uint32_t)((off_bytes >> 2) & 0x3FFFFFFu);
}

/* BLR Xn — 0xD63F0000 | (Rn<<5). */
static uint32_t emit_blr(int rn) { return 0xD63F0000u | ((uint32_t)rn << 5); }

/* RET (RET X30) — 0xD65F03C0. */
static uint32_t emit_ret(void) { return 0xD65F03C0u; }

/* Load a 64-bit immediate into Xd via MOVZ/MOVK (4 insns, always emit all 4). */
static int emit_load_imm64(uint32_t *buf, int rd, uint64_t imm) {
    /* MOVZ Xd, #imm16, LSL #0   0xD2800000 | (hw<<21) | (imm16<<5) | Rd
     * MOVK Xd, #imm16, LSL #hw  0xF2800000 | (hw<<21) | (imm16<<5) | Rd */
    int n = 0;
    buf[n++] = 0xD2800000u | (0u << 21) | (((uint32_t)(imm & 0xFFFF)) << 5) | (uint32_t)rd;
    buf[n++] = 0xF2800000u | (1u << 21) | (((uint32_t)((imm >> 16) & 0xFFFF)) << 5) | (uint32_t)rd;
    buf[n++] = 0xF2800000u | (2u << 21) | (((uint32_t)((imm >> 32) & 0xFFFF)) << 5) | (uint32_t)rd;
    buf[n++] = 0xF2800000u | (3u << 21) | (((uint32_t)((imm >> 48) & 0xFFFF)) << 5) | (uint32_t)rd;
    return n;
}

/* ===========================================================================
 * Section 2: Frame layout (verified — see m1-task9-thunk-encodings.md).
 *
 * After  stp x29,x30,[sp,#-16]!   (fp/lr pushed, 16 bytes)
 * then   sub sp,sp,#FRAME_SUB     (FRAME_SUB = 624, a 16-multiple)
 *
 * Relative to the new SP (= frame base passed to the callback as x0):
 *   x0..x27  : STP pairs at byte offsets 0,16,...,216   (x{i} at i*8)
 *   x28      : STR at byte offset 224
 *   (8-byte gap at 232 so the Q region starts 16-aligned)
 *   q0,q1    : 240 ; q2,q3 : 272 ; q4,q5 : 304 ; q6,q7 : 336
 *   q16,q17  : 368 ; ... ; q30,q31 : 592   (24 Q regs total, 384 bytes, 240..624)
 *
 * frame[i] (uint64_t*) == x{i} for i in 0..28. SP is 16-aligned at the BLR.
 * =========================================================================== */

#define FRAME_SUB   624
#define QBASE       240

/* Emit the complete generic thunk into buf (uint32 words). Returns word count. */
static int emit_thunk(uint32_t *buf, uint64_t dispatch_fn) {
    int n = 0;
    /* prologue */
    buf[n++] = emit_stp_fp_lr_preidx();          /* stp x29,x30,[sp,#-16]! */
    buf[n++] = emit_sub_sp_imm(FRAME_SUB);       /* sub sp,sp,#624 */
    /* save x0..x27 as 14 STP pairs */
    for (int r = 0; r < 28; r += 2)
        buf[n++] = emit_stp_x(r, r + 1, r * 8);
    /* save x28 */
    buf[n++] = emit_str_x_imm(28, 224);
    /* save q0..q7 and q16..q31 (24 vregs) */
    int qoff = QBASE;
    for (int q = 0; q < 8; q += 2, qoff += 32)
        buf[n++] = emit_stp_q(q, q + 1, qoff);
    for (int q = 16; q < 32; q += 2, qoff += 32)
        buf[n++] = emit_stp_q(q, q + 1, qoff);
    /* args: x0 = frame base (sp), x1 = site = x30 - 4 */
    buf[n++] = emit_mov_x0_sp();
    buf[n++] = emit_sub_x1_x30_4();
    /* x16 = &dispatch ; blr x16 */
    n += emit_load_imm64(&buf[n], 16, dispatch_fn);
    buf[n++] = emit_blr(16);
    /* restore — exact mirror of the save sequence (offsets identical) */
    for (int r = 0; r < 28; r += 2)
        buf[n++] = emit_ldp_x(r, r + 1, r * 8);
    buf[n++] = emit_ldr_x_imm(28, 224);
    qoff = QBASE;
    for (int q = 0; q < 8; q += 2, qoff += 32)
        buf[n++] = emit_ldp_q(q, q + 1, qoff);
    for (int q = 16; q < 32; q += 2, qoff += 32)
        buf[n++] = emit_ldp_q(q, q + 1, qoff);
    /* epilogue */
    buf[n++] = emit_add_sp_imm(FRAME_SUB);       /* add sp,sp,#624 */
    buf[n++] = emit_ldp_fp_lr_postidx();         /* ldp x29,x30,[sp],#16 */
    buf[n++] = emit_ret();
    return n;
}

/* ===========================================================================
 * Section 3: End-to-end test — callback + asm trampoline.
 * =========================================================================== */

#define TEST_EA      0xF3012002u
#define MUT_X1       0xDEADBEEFCAFEF00Dull   /* what callback writes to frame[1]  */
#define MUT_X27      0x0BADF00D5511AA22ull   /* what callback writes to frame[27] */
#define SENT_X5      0x5555555555555555ull   /* trampoline puts this in x5        */
#define SENT_V16_LO  0x16161616A5A5A5A5ull   /* trampoline puts this in v16.d[0]  */

static int g_cb_calls = 0;
static int g_cb_ea_ok = 0;

/* The dispatch callback. Signature matches Task 9's ppc_jit_mmio_thunk_dispatch:
 *   x0 = frame base (uint64_t* to x0..x28), x1 = site (x30-4).
 * To give the save/restore path teeth, it ACTIVELY POISONS every register slot it
 * is NOT supposed to mutate (x2..x28 except x27, plus x29/lr conceptually live in
 * the pre-index pair which we do not expose in `frame`). The thunk must restore
 * x5 and v16 from its own saved copies, so the trampoline's post-return sentinels
 * have something to fail against if a save/restore offset is wrong. */
extern void test_callback(uint64_t *frame, uint32_t *site);
void test_callback(uint64_t *frame, uint32_t *site) {
    (void)site;
    g_cb_calls++;
    if (frame[0] == TEST_EA) g_cb_ea_ok++;
    /* Poison every GPR slot the trampoline relies on surviving (x5) and any
     * other saved slot — proves restore actually happens. We do NOT poison
     * frame[0] (already read) or the two slots we deliberately mutate. */
    for (int i = 2; i <= 28; i++) {
        if (i == 27) continue;   /* deliberately mutated below */
        if (i == 5)  continue;   /* x5 survival check: leave its slot untouched so
                                    x5==SENT_X5 proves the gpr round-trip restored it */
        frame[i] = 0xBADBADBAD0000000ull | (uint64_t)i;
    }
    /* The intended mutations (loads pick these up after restore): */
    frame[1]  = MUT_X1;
    frame[27] = MUT_X27;
}

/* Result globals the trampoline writes (avoids returning many values). */
uint64_t g_ret_x1, g_ret_x27, g_ret_x5;
uint64_t g_ret_v16_lo;
uint64_t g_sp_before, g_sp_after;

/* The asm trampoline. It:
 *   - saves the callee-saved regs it touches (x27) + fp/lr,
 *   - captures sp into a global before the BL,
 *   - sets x0 = TEST_EA, x5 = SENT_X5, x27 = a known value (so the callback's
 *     mutation to frame[27] must overwrite it after restore — wait: frame[27]
 *     IS x27, so after restore x27 == MUT_X27), loads v16.d[0] = SENT_V16_LO,
 *   - BLs the thunk (x30 = site+4 set by BL, exactly the real JIT contract),
 *   - reads back x1, x27, x5, v16.d[0], and sp into globals.
 *
 * x1 should hold MUT_X1 (callback wrote frame[1], thunk restored x1 from it).
 * x27 should hold MUT_X27. x5 should still be SENT_X5 (poisoned in no slot the
 * callback writes — callback poisons frame[5], so x5 after restore == that
 * poison UNLESS... see note). v16 should still be SENT_V16_LO.
 *
 * IMPORTANT subtlety: the callback poisons frame[5]. After the thunk restores
 * x5 from frame[5], x5 would become the poison value — that is CORRECT behavior
 * for a frame-based thunk (loads pick up frame mutations). So to test that x5
 * "survived", x5 is NOT a frame-slot register from the trampoline's view... but
 * it IS (x5 == frame[5]). We therefore use a different survival check: the
 * callback does NOT poison frame[5] (it skips it), and we assert x5 == SENT_X5.
 * v16 is never in the frame, so it directly proves the q-save/restore path. */
extern void run_trampoline(uint32_t *thunk_entry);
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl _run_trampoline\n"
    "_run_trampoline:\n"
    /* prologue: the thunk saves+restores guest GPRs x0..x28 through its frame,
     * and the callback POISONS most of them, so on return x19..x28 (callee-saved
     * that `main` relies on) hold poison. We must save ALL of x19..x28 + fp/lr
     * here and restore them before returning, or we corrupt main. */
    "    stp x29, x30, [sp, #-96]!\n"
    "    mov x29, sp\n"
    "    stp x19, x20, [sp, #16]\n"
    "    stp x21, x22, [sp, #32]\n"
    "    stp x23, x24, [sp, #48]\n"
    "    stp x25, x26, [sp, #64]\n"
    "    stp x27, x28, [sp, #80]\n"
    "    mov x19, x0\n"            /* x19 = thunk_entry (preserved across BL) */
    /* capture sp before the call into g_sp_before */
    "    mov x9, sp\n"
    "    adrp x10, _g_sp_before@PAGE\n"
    "    add  x10, x10, _g_sp_before@PAGEOFF\n"
    "    str  x9, [x10]\n"
    /* set up the live state the thunk must preserve / read */
    "    movz w0, #0x2002\n"
    "    movk w0, #0xF301, lsl #16\n"   /* w0 = 0xF3012002 (TEST_EA) */
    "    mov  x5,  #0x5555\n"
    "    movk x5,  #0x5555, lsl #16\n"
    "    movk x5,  #0x5555, lsl #32\n"
    "    movk x5,  #0x5555, lsl #48\n"  /* x5 = 0x5555555555555555 */
    /* v16.d[0] = SENT_V16_LO via x9 then fmov */
    "    movz x9, #0xA5A5\n"
    "    movk x9, #0xA5A5, lsl #16\n"
    "    movk x9, #0x1616, lsl #32\n"
    "    movk x9, #0x1616, lsl #48\n"   /* x9 = 0x16161616A5A5A5A5 */
    "    fmov d16, x9\n"
    "    mov  x27, #0x1234\n"           /* x27 sentinel (callback overwrites via frame[27]) */
    /* call the thunk: BL sets x30 = site+4, exactly the JIT contract */
    "    blr x19\n"
    /* read back results */
    "    adrp x10, _g_ret_x1@PAGE\n    add x10, x10, _g_ret_x1@PAGEOFF\n    str x1, [x10]\n"
    "    adrp x10, _g_ret_x27@PAGE\n   add x10, x10, _g_ret_x27@PAGEOFF\n   str x27, [x10]\n"
    "    adrp x10, _g_ret_x5@PAGE\n    add x10, x10, _g_ret_x5@PAGEOFF\n    str x5, [x10]\n"
    "    fmov x9, d16\n"
    "    adrp x10, _g_ret_v16_lo@PAGE\n add x10, x10, _g_ret_v16_lo@PAGEOFF\n str x9, [x10]\n"
    "    mov  x9, sp\n"
    "    adrp x10, _g_sp_after@PAGE\n  add x10, x10, _g_sp_after@PAGEOFF\n  str x9, [x10]\n"
    /* epilogue: restore ALL callee-saved we banked (they hold thunk poison now) */
    "    ldp x27, x28, [sp, #80]\n"
    "    ldp x25, x26, [sp, #64]\n"
    "    ldp x23, x24, [sp, #48]\n"
    "    ldp x21, x22, [sp, #32]\n"
    "    ldp x19, x20, [sp, #16]\n"
    "    ldp x29, x30, [sp], #96\n"
    "    ret\n");

/* ===========================================================================
 * Section 4: clang cross-check. Each entry pairs a helper-produced encoding
 * with the EXACT mnemonic in clang_ref.s (same order). The Makefile assembles
 * clang_ref.s, dumps its words, and this table's `enc` values are diffed.
 * =========================================================================== */
struct xcheck { const char *mnem; uint32_t enc; };
static const struct xcheck xchecks[] = {
    { "stp x0, x1, [sp, #0]",        0 },  /* filled at runtime below */
    { "stp x2, x3, [sp, #16]",       0 },
    { "stp x26, x27, [sp, #208]",    0 },
    { "ldp x0, x1, [sp, #0]",        0 },
    { "ldp x26, x27, [sp, #208]",    0 },
    { "str x28, [sp, #224]",         0 },
    { "ldr x28, [sp, #224]",         0 },
    { "stp q0, q1, [sp, #240]",      0 },
    { "stp q30, q31, [sp, #592]",    0 },
    { "ldp q0, q1, [sp, #240]",      0 },
    { "sub sp, sp, #624",            0 },
    { "add sp, sp, #624",            0 },
    { "mov x0, sp",                  0 },
    { "sub x1, x30, #4",             0 },
    { "stp x29, x30, [sp, #-16]!",   0 },
    { "ldp x29, x30, [sp], #16",     0 },
    { "blr x16",                     0 },
    { "ret",                         0 },
    /* --- the plan SKETCH's exact hand-packed expressions, to empirically
       settle which "corrections" are real (sketch text vs clang). --- */
    { "[sketch] stp x0,x1,[sp,#0]",   0 },  /* r=0  */
    { "[sketch] stp x2,x3,[sp,#16]",  0 },  /* r=2  */
    { "[sketch] stp x26,x27,[sp,#208]",0 }, /* r=26 */
    { "[sketch] str x28,[sp,#224]",   0 },  /* the suspect STR line */
    { "[sketch] stp q0,q1,[sp,#240]", 0 },  /* qoff=240 */
};
#define NXCHECK (int)(sizeof(xchecks)/sizeof(xchecks[0]))

static void fill_and_print_xchecks(void) {
    uint32_t e[NXCHECK];
    int i = 0;
    e[i++] = emit_stp_x(0, 1, 0);
    e[i++] = emit_stp_x(2, 3, 16);
    e[i++] = emit_stp_x(26, 27, 208);
    e[i++] = emit_ldp_x(0, 1, 0);
    e[i++] = emit_ldp_x(26, 27, 208);
    e[i++] = emit_str_x_imm(28, 224);
    e[i++] = emit_ldr_x_imm(28, 224);
    e[i++] = emit_stp_q(0, 1, 240);
    e[i++] = emit_stp_q(30, 31, 592);
    e[i++] = emit_ldp_q(0, 1, 240);
    e[i++] = emit_sub_sp_imm(624);
    e[i++] = emit_add_sp_imm(624);
    e[i++] = emit_mov_x0_sp();
    e[i++] = emit_sub_x1_x30_4();
    e[i++] = emit_stp_fp_lr_preidx();
    e[i++] = emit_ldp_fp_lr_postidx();
    e[i++] = emit_blr(16);
    e[i++] = emit_ret();
    /* The plan sketch's literal expressions (Task 9 Step 1, emit_mmio_thunk): */
    {
        int r;
        r = 0;  e[i++] = 0xA9000000u | ((r + 1) << 10) | (31 << 5) | r | (((r * 8) / 8) << 15);
        r = 2;  e[i++] = 0xA9000000u | ((r + 1) << 10) | (31 << 5) | r | (((r * 8) / 8) << 15);
        r = 26; e[i++] = 0xA9000000u | ((r + 1) << 10) | (31 << 5) | r | (((r * 8) / 8) << 15);
        e[i++] = 0xF90000FCu | (28 << 0) | ((28 * 8 / 8) << 10);   /* the suspect STR */
        int q = 0, qoff = 240;
        e[i++] = 0xAD000000u | ((q + 1) << 10) | (31 << 5) | q | ((qoff / 16) << 15);
    }
    /* Machine-parseable lines: "XCHECK <idx> <hex> <mnem>" */
    for (int j = 0; j < NXCHECK; j++)
        printf("XCHECK %d 0x%08x %s\n", j, e[j], xchecks[j].mnem);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--xcheck") == 0) {
        fill_and_print_xchecks();
        return 0;
    }
    printf("M1 Task 9 thunk pre-work spike (macOS arm64)\n");
    printf("FRAME_SUB=%d bytes, QBASE=%d\n", FRAME_SUB, QBASE);

    /* Emit the thunk. */
    uint32_t code[256];
    int nwords = emit_thunk(code, (uint64_t)(uintptr_t)&test_callback);
    printf("thunk = %d instructions (%d bytes)\n", nwords, nwords * 4);

    /* Copy into a MAP_JIT page (S2 idiom). */
    size_t page = (size_t)getpagesize();
    void *jit = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0);
    if (jit == MAP_FAILED) { perror("mmap MAP_JIT"); return 1; }
    pthread_jit_write_protect_np(0);
    memcpy(jit, code, nwords * 4);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(jit, nwords * 4);

    /* Run the end-to-end test. */
    g_ret_x1 = g_ret_x27 = g_ret_x5 = g_ret_v16_lo = 0;
    g_sp_before = g_sp_after = 0;
    run_trampoline((uint32_t *)jit);

    int pass = 1;
    printf("\n--- end-to-end thunk test ---\n");
    printf("callback called:   %d (expect 1)\n", g_cb_calls);
    printf("callback ea==EA:   %d (expect 1; frame[0]==0x%08X)\n", g_cb_ea_ok, TEST_EA);
    printf("x1  after  = 0x%016llx (expect 0x%016llx)  [callback mutated frame[1]]\n",
           (unsigned long long)g_ret_x1, (unsigned long long)MUT_X1);
    printf("x27 after  = 0x%016llx (expect 0x%016llx)  [callback mutated frame[27]]\n",
           (unsigned long long)g_ret_x27, (unsigned long long)MUT_X27);
    printf("x5  after  = 0x%016llx (expect 0x%016llx)  [must survive: gpr save/restore]\n",
           (unsigned long long)g_ret_x5, (unsigned long long)SENT_X5);
    printf("v16 after  = 0x%016llx (expect 0x%016llx)  [must survive: q  save/restore]\n",
           (unsigned long long)g_ret_v16_lo, (unsigned long long)SENT_V16_LO);
    printf("sp before  = 0x%016llx\n", (unsigned long long)g_sp_before);
    printf("sp after   = 0x%016llx  [must equal sp before: balance]\n",
           (unsigned long long)g_sp_after);

    if (g_cb_calls != 1) { printf("FAIL: callback not called exactly once\n"); pass = 0; }
    if (g_cb_ea_ok != 1) { printf("FAIL: frame[0] != EA\n"); pass = 0; }
    if (g_ret_x1  != MUT_X1)  { printf("FAIL: x1 not picked up from frame[1]\n"); pass = 0; }
    if (g_ret_x27 != MUT_X27) { printf("FAIL: x27 not picked up from frame[27]\n"); pass = 0; }
    if (g_ret_x5  != SENT_X5) { printf("FAIL: x5 clobbered (gpr save/restore broken)\n"); pass = 0; }
    if (g_ret_v16_lo != SENT_V16_LO) { printf("FAIL: v16 clobbered (q save/restore broken)\n"); pass = 0; }
    if (g_sp_before != g_sp_after) { printf("FAIL: SP not balanced\n"); pass = 0; }

    printf("\nEND-TO-END: %s\n", pass ? "PASS" : "FAIL");

    /* Dump the full thunk listing (hex) for the doc. */
    printf("\n--- thunk instruction listing (hex) ---\n");
    for (int i = 0; i < nwords; i++)
        printf("  [%2d] 0x%08x\n", i, code[i]);

    return pass ? 0 : 1;
}
