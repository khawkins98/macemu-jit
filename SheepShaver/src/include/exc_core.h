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
 *                      upper bits (including POW) are not preserved through SRR1 for
 *                      these classes (reserved-zero on this silicon; POW loss by design).
 *
 *  EXC_RFI_MSR_MASK    rfi restores these bits from SRR1; remaining bits come from
 *                      the handler's MSR. Compare with EXC_MSR_CLEAR_MASK low half:
 *                      XOR = 0x1041 = ME|IP|LE — rfi RESTORES ME/IP/LE from SRR1
 *                      while entry PRESERVES ME/IP; use the correct mask for the
 *                      correct direction.
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

/* FE1F-service-surface Task A (plan rev 3, 2026-06-11): program-exception SRR1
 * cause bit for a TRAP-type program interrupt (vector 0x700). PEM program-
 * interrupt SRR1 semantics (32-bit, IBM bit numbering, bit 0 = MSB):
 *   bit 11 (0x00100000)  FP enabled exception
 *   bit 12 (0x00080000)  illegal instruction
 *   bit 13 (0x00040000)  privileged instruction
 *   bit 14 (0x00020000)  TRAP (tw/twi with condition satisfied)  <- this one
 *   bit 15 (0x00010000)  SRR0 points at the NEXT instruction (0 for trap:
 *                        SRR0 = the address of the tw/twi itself)
 * Only the trap bit is modeled (the only program-exception cause we deliver);
 * bits 11-13/15 stay 0 by construction. SRR1 low 16 bits keep the MSR snapshot
 * per EXC_SRR1_KEEP_MASK exactly as for the other classes (masks are LAW). */
#define EXC_SRR1_PROGRAM_TRAP  0x00020000u

/* --- Types --- */

enum ExcClass {
	EXC_DECREMENTER,   /* DEC expiry — vector 0x900 */
	EXC_EXTERNAL,      /* external interrupt — vector 0x500 */
	EXC_SC,            /* sc instruction — vector 0xC00 (EXC_SYSCALL avoided: conflicts with
	                    * macOS SDK <mach/exception_types.h> macro of the same name) */
	EXC_PROGRAM        /* program interrupt — vector 0x700, trap type (tw/twi taken).
	                    * SRR0 = the trap instruction itself (verbatim, NOT +4);
	                    * SRR1 gains EXC_SRR1_PROGRAM_TRAP atop the masked MSR.
	                    * FE1F-service-surface Task A (appended last). */
};

/* Entry table: resolved handler PCs. 0 = unresolved (ExcEnter returns EXC_PC_UNRESOLVED).
 * Fields are appended LAST only (standing rule — existing aggregate initializers
 * {intr, sc} keep compiling with later fields zero-initialized). */
struct ExcEntryTable {
	uint32_t interrupt_entry;   /* DEC and EXT share this target */
	uint32_t syscall_entry;     /* SC target; 0 = descoped/unresolved */
	uint32_t program_entry;     /* PROGRAM (0x700) target; 0 = unresolved
	                             * (FE1F-service-surface Task A, appended last) */
	uint32_t external_entry;    /* EXT (0x500) target — PRE-POSITIONED, NOT YET
	                             * CONSUMED (Wave-2 rev 2 F6, appended last).
	                             * ExcEnter(EXC_EXTERNAL) still dispatches to
	                             * interrupt_entry until W2-3 resolves the entry-
	                             * point discrimination via Q-W2 (default candidate:
	                             * the NK-published [KDP+0x374]=0x50314880, primary
	                             * copy). Default 0. test_exc_chain U12 pins the
	                             * unconsumed-ness — flipping consumption is a
	                             * deliberate W2-3 test change, not silent drift. */
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

/* --- Wave-2 W2-0: the delivery-decision chain, extracted pure
 *     (docs/superpowers/plans/2026-06-11-wave2-interrupt-chain.md, recon §C.1) --- */

/*
 * ExcDecision — the outcome of one delivery-gate evaluation.
 *
 * THE GATE ORDER IS CONTRACT: pending -> depth -> EE -> native.
 * The live `exc=` telemetry tuple counts per-gate (deferred_depth /
 * deferred_ee / deferred_native / delivered_*); transposing two gates
 * changes which counter a given machine state increments, i.e. changes
 * the tuple class on a boot. Do not reorder (test_exc_chain U2/U4 pin
 * depth-outranks-EE and EE-outranks-native).
 */
enum ExcDecision {
	EXC_DECIDE_NONE,           /* no pending condition — nothing to do, no counter */
	EXC_DECIDE_DEFER_DEPTH,    /* nested execute() in flight (depth != 1)          */
	EXC_DECIDE_DEFER_EE,       /* MSR[EE] = 0                                      */
	EXC_DECIDE_DEFER_NATIVE,   /* [XLM_RUN_MODE] != 0 — MixedMode native excursion */
	EXC_DECIDE_DELIVER         /* all gates open — deliver now                     */
};

/*
 * ExcDeliveryDecision — pure composition of the four delivery gates.
 *
 * pending:       the exception-condition latch (caller samples it; e.g.
 *                VirtClockDECPending for DEC, the PIC output flag for EXT).
 * execute_depth: the emulator's nested-execute() depth (deliverable only at
 *                the outermost depth, == 1).
 * msr:           current machine MSR (EE gate via ExcDeliverable).
 * run_mode_word: the sampled guest [XLM_RUN_MODE] word (0x2810).
 *
 * CALLER OBLIGATIONS (the real contract — this function is pure and touches
 * NO state; everything stateful stays with the caller):
 *  - The caller samples guest memory ([XLM_RUN_MODE]) and passes the word in.
 *    Sanctioned lazy-sampling idiom where the guest read is costly or unsafe
 *    (e.g. unmapped lowmem in the SS_TEST harness): call once with
 *    run_mode_word=0, and only on EXC_DECIDE_DELIVER re-sample and re-call —
 *    the first three gates never consult the word, so the two-phase form is
 *    decision-identical to a single sampled call.
 *  - On any DEFER_* result the caller leaves the source latch SET (the EE-edge
 *    re-raises and the block-boundary poll pick it up later) and increments
 *    exactly the counter matching the decision.
 *  - On EXC_DECIDE_DELIVER the caller clears a one-shot latch (DEC) EXACTLY
 *    ONCE, before applying the ExcEnter transition. Level-held sources (the
 *    W2-3 PIC output) are NOT cleared by the caller — they stay asserted
 *    until the guest's IACK/EOI/mask drops the line.
 *  - EXC_DECIDE_NONE increments nothing and consumes nothing.
 */
extern ExcDecision ExcDeliveryDecision(int pending, int execute_depth,
                                       uint32_t msr, uint32_t run_mode_word);

/*
 * ExcEdgeReRaise — the EE-edge re-raise predicate shared by the three
 * re-raise families: execute_mtmsr, execute_rfi, and the glue
 * nested-execute-return rechecks.
 *
 * Fires iff (old EE=0  AND  new EE=1  AND  pending) — a rising EE edge with
 * a deliverable condition outstanding. The caller's only obligation on a
 * nonzero return is to kick the poll (trigger_interrupt()); the delivery hook
 * re-gates on the REAL machine state, so a spurious kick is safe and a missed
 * kick is not (no 60 Hz safety net on the newworld diagnostic boot — the
 * edge re-raises are load-bearing, M3A-ENTRY-TABLE.md).
 *
 * Nested-return canonical form: across a nested execute() the MSR may have
 * changed arbitrarily, so those sites pass the synthetic full edge
 * (old_msr=0, new_msr=0x8000) — i.e. re-raise whenever pending, behavior-
 * identical to the pre-extraction unconditional-if-pending recheck.
 */
extern int ExcEdgeReRaise(uint32_t old_msr, uint32_t new_msr, int pending);

/*
 * The emulator's resolved entry table (defined in sheepshaver_glue.cpp, filled
 * at newworld init from M3A-ENTRY-TABLE.md / SS_EXC_ENTRY). Declaration lives
 * here so consumers (ppc-execute.cpp, the delivery hook) share one typed decl
 * (quality-review M3: no function-scope externs). Declaration-only: the pure
 * module itself never references it — standalone tests stay link-clean.
 */
extern ExcEntryTable g_exc_entry_table;

#endif /* EXC_CORE_H */
