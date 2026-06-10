/*
 *  exc_core.h - Machine Layer M3a OEA exception-transition math
 *               (docs/superpowers/plans/2026-06-10-machine-layer-m3a.md §Task 1).
 *
 *  Pure module: no globals, no emulator dependencies, stdint only.
 *
 *  Shared transform: DEC/EXT/SC use the SAME three MSR masks. Per-class
 *  differences are confined to SRR0 semantics and the dispatch target
 *  (rev 2 #7 — do NOT invent per-class mask differences).
 *
 *  Mask derivation (written against the PEM, rev 2 #3 — these values are LAW):
 *
 *  EXC_MSR_CLEAR_MASK  Bits cleared on entry: POW|EE|PR|FP|FE0|SE|BE|FE1|IR|DR|RI
 *                      ME and IP are preserved.
 *                      MSR bit positions: POW=0x40000, EE=0x8000, PR=0x4000, FP=0x2000,
 *                      ME=0x1000 (KEEP), FE0=0x800, SE=0x400, BE=0x200, FE1=0x100,
 *                      IP=0x40 (KEEP), IR=0x20, DR=0x10, RI=0x2.
 *
 *  EXC_SRR1_KEEP_MASK  SRR1 captures only the low 16 bits of the pre-exception MSR;
 *                      upper bits (including POW) are architecturally not preserved.
 *
 *  EXC_RFI_MSR_MASK    rfi restores these bits from SRR1; remaining bits come from
 *                      the handler's MSR. Compare with EXC_MSR_CLEAR_MASK: note that
 *                      0xFF73 != 0xEF32 — RI (0x2) and some other bits differ; use
 *                      the correct mask for the correct direction.
 *
 *  POW: architecturally lost on rfi. POW is cleared on entry (in EXC_MSR_CLEAR_MASK)
 *  and sits above the 16-bit SRR1 window (EXC_SRR1_KEEP_MASK = 0xFFFF), so it is
 *  never captured and rfi cannot restore it. This is correct OEA behaviour.
 *
 *  0xf072 PR=1 note: the live NK MSR 0xf072 claims PR=1 (user mode) while the
 *  nanokernel runs supervisor — a legacy fiction preserved, not fixed. It propagates
 *  through real SRR1/rfi byte-for-byte and is harmless (PR is unenforced in the
 *  SheepShaver MMU model). Task 1 test 3 documents the byte-equality explicitly.
 *
 *  LE/ILE: degenerate on big-endian Mac (always 0); no special handling needed.
 */

#ifndef EXC_CORE_H
#define EXC_CORE_H

#include <stdint.h>

/* --- PEM masks (LAW — do not change without updating the plan) --- */
#define EXC_MSR_CLEAR_MASK  0x0004EF32u   /* cleared on entry: POW|EE|PR|FP|FE0|SE|BE|FE1|IR|DR|RI */
#define EXC_SRR1_KEEP_MASK  0x0000FFFFu   /* SRR1 = msr & this */
#define EXC_RFI_MSR_MASK    0x0000FF73u   /* rfi: new_msr = (SRR1 & this) | (cur_msr & ~this) */

/* Returned in ExcTransition.pc when the class's table entry is 0 (unresolved). */
#define EXC_PC_UNRESOLVED   0xFFFFFFFFu

/* --- Types --- */

enum ExcClass {
	EXC_DECREMENTER,   /* DEC expiry — vector 0x900 */
	EXC_EXTERNAL,      /* external interrupt — vector 0x500 */
	EXC_SYSCALL        /* sc instruction — vector 0xC00 */
};

/* Entry table: resolved handler PCs. 0 = unresolved (ExcEnter returns EXC_PC_UNRESOLVED). */
struct ExcEntryTable {
	uint32_t interrupt_entry;   /* DEC and EXT share this target */
	uint32_t syscall_entry;     /* SC target; 0 = descoped/unresolved */
};

/* Result of ExcEnter: the complete machine-state transition to apply atomically. */
struct ExcTransition {
	uint32_t srr0;   /* restart address: pc (DEC/EXT) or pc+4 (SC) */
	uint32_t srr1;   /* pre-exception MSR snapshot (low 16 bits only) */
	uint32_t msr;    /* new handler MSR (pre-exception with CLEAR bits zeroed) */
	uint32_t pc;     /* dispatch target from ExcEntryTable (or EXC_PC_UNRESOLVED) */
};

/* --- API --- */

/*
 * ExcEnter — compute the full exception-entry transition.
 *
 * cur_pc_restart: for DEC/EXT, the not-yet-executed restart PC (block-start
 *                 at the poll site); for SC, the address of the sc instruction
 *                 itself (ExcEnter adds the +4 — ownership pinned here).
 * cur_msr:        current machine MSR at the moment of delivery.
 * cls:            exception class (determines SRR0 and target selection).
 * tbl:            resolved entry table (from Task 0 / g_exc_entry_table).
 *
 * Returns a fully-composed ExcTransition. Apply all four fields atomically
 * (SRR0, SRR1 via mtspr, MSR, PC) before any guest code runs in the handler.
 * The caller is responsible for the KDP register-save shim (M3a Task 4) when
 * operating in KDP-SHIM mode.
 */
extern ExcTransition ExcEnter(uint32_t cur_pc_restart, uint32_t cur_msr,
                              ExcClass cls, const ExcEntryTable *tbl);

/*
 * ExcRfi — compute the rfi MSR/PC restore.
 *
 * srr0, srr1: saved by ExcEnter (or written by the handler).
 * cur_msr:    the handler's current MSR at rfi time.
 *
 * out_pc:  guest PC after rfi = srr0 & ~3 (low 2 bits always cleared per OEA).
 * out_msr: restored MSR = (srr1 & EXC_RFI_MSR_MASK) | (cur_msr & ~EXC_RFI_MSR_MASK).
 */
extern void ExcRfi(uint32_t srr0, uint32_t srr1, uint32_t cur_msr,
                   uint32_t *out_pc, uint32_t *out_msr);

/*
 * ExcDeliverable — true if MSR[EE] (bit 15) permits interrupt delivery.
 * Used by the delivery hook in check_spcflags (Task 4).
 */
static inline int ExcDeliverable(uint32_t msr) { return (msr & 0x8000u) != 0; }

#endif /* EXC_CORE_H */
