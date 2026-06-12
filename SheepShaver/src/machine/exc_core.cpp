/*
 *  exc_core.cpp - Machine Layer M3a OEA exception-transition math
 *                 (docs/superpowers/plans/2026-06-10-machine-layer-m3a.md §Task 1).
 *
 *  Pure module: no globals, no emulator dependencies, stdint only.
 *  All external state is passed in; all results are returned by value.
 *  See exc_core.h for the mask derivation and design notes.
 */

#include "exc_core.h"

ExcTransition ExcEnter(uint32_t cur_pc_restart, uint32_t cur_msr,
                       ExcClass cls, const ExcEntryTable *tbl)
{
	ExcTransition t;

	/* SRR0: DEC/EXT save the restart PC verbatim (not-yet-executed block start).
	 * SC saves pc+4: the instruction after the sc (the sc itself is cur_pc_restart).
	 * PROGRAM (trap type) saves the trap instruction itself verbatim — PEM:
	 * SRR1 bit 15 = 0 means SRR0 points AT the offending instruction. */
	t.srr0 = (cls == EXC_SC) ? cur_pc_restart + 4u : cur_pc_restart;

	/* SRR1: capture only the low 16 bits of the pre-exception MSR.
	 * PROGRAM additionally sets the PEM trap cause bit (FE1F-service-surface
	 * Task A; the masks themselves are LAW and unchanged). */
	t.srr1 = cur_msr & EXC_SRR1_KEEP_MASK;
	if (cls == EXC_PROGRAM)
		t.srr1 |= EXC_SRR1_PROGRAM_TRAP;

	/* New handler MSR: clear the entry-mask bits (POW/EE/PR/FP/FE0/SE/BE/FE1/IR/DR/RI).
	 * ME and IP are preserved. */
	t.msr = cur_msr & ~EXC_MSR_CLEAR_MASK;

	/* Dispatch target: SC uses syscall_entry; PROGRAM uses program_entry;
	 * EXT consumes external_entry WHEN NONZERO and falls back to interrupt_entry
	 * when 0 (Wave-2 W2-3, the deliberate U12 flip — Q-W2 verdict: the NK
	 * discriminates interrupt sources by ENTRY POINT; the published EXT handler
	 * [KDP+0x374]=0x50314880 is a distinct body from the DEC delivery target.
	 * EE-CHAIN-RECON.md §W2S-2. Zero-field tables keep the pre-W2-3 shared-entry
	 * behavior — aggregate initializers stay valid); DEC uses interrupt_entry.
	 * 0 in the resolved entry -> EXC_PC_UNRESOLVED (caller handles via SS_EXC_SC
	 * knob; for PROGRAM/EXT the caller's loud capture-abort handles it). */
	uint32_t entry = (cls == EXC_SC)      ? tbl->syscall_entry
	               : (cls == EXC_PROGRAM) ? tbl->program_entry
	               : (cls == EXC_EXTERNAL && tbl->external_entry != 0)
	                                      ? tbl->external_entry
	                                      : tbl->interrupt_entry;
	t.pc = entry ? entry : EXC_PC_UNRESOLVED;

	return t;
}

/* Wave-2 W2-0: the delivery-gate composition, extracted verbatim from
 * sheepshaver_cpu::deliver_pending_dec_exception (sheepshaver_glue.cpp).
 * THE GATE ORDER IS CONTRACT (pending -> depth -> EE -> native): the exc=
 * telemetry tuple counts per-gate. See the header for the caller obligations
 * (latch discipline, counter mapping, lazy run-mode sampling). */
ExcDecision ExcDeliveryDecision(int pending, int execute_depth,
                                uint32_t msr, uint32_t run_mode_word)
{
	if (!pending)
		return EXC_DECIDE_NONE;
	/* Deliverability rule (MACHINE-LAYER-PLAN §2d): never deliver inside
	 * nested executes — depth 1 means the outermost execute(). */
	if (execute_depth != 1)
		return EXC_DECIDE_DEFER_DEPTH;
	if (!ExcDeliverable(msr))
		return EXC_DECIDE_DEFER_EE;
	/* M6a rung-2 W2 DEC fence: defer while a MixedMode native excursion is in
	 * flight ([XLM_RUN_MODE] != 0) — the KDP register-save shim would save into
	 * the world-flip block the NK switch-back is about to rewrite. */
	if (run_mode_word != 0)
		return EXC_DECIDE_DEFER_NATIVE;
	return EXC_DECIDE_DELIVER;
}

/* Wave-2 W2-0: the EE-edge re-raise predicate (mtmsr / rfi / nested-return).
 * Fires iff a rising EE edge meets an outstanding pending condition. */
int ExcEdgeReRaise(uint32_t old_msr, uint32_t new_msr, int pending)
{
	return !(old_msr & 0x8000u) && (new_msr & 0x8000u) && pending;
}

/* M8 slot-4 consumption Task A: the deferred-EE-edge window predicates
 * (rfi-atomicity emulation — see exc_core.h block comment). Pure; the windows
 * are passed in (single source: rom_patches.cpp's patch-time fill of
 * g_exc_riser_window). All windows are half-open [base, end); an empty window
 * (base == end == 0, riser not armed) can never latch and always fires. */
int ExcDeferredEdgeLatch(uint32_t pc, uint32_t stub_base, uint32_t stub_end)
{
	return pc >= stub_base && pc < stub_end;
}

int ExcDeferredEdgeFire(uint32_t entry_pc, uint32_t stub_base, uint32_t stub_end,
                        uint32_t reload_start, uint32_t reload_end)
{
	if (entry_pc >= stub_base && entry_pc < stub_end)
		return 0;   /* still inside the riser stub */
	if (entry_pc >= reload_start && entry_pc < reload_end)
		return 0;   /* still inside the ctx reload region (bctr not yet taken) */
	return 1;       /* past the bctr — the resume PC is real, fire now */
}

void ExcRfi(uint32_t srr0, uint32_t srr1, uint32_t cur_msr,
            uint32_t *out_pc, uint32_t *out_msr)
{
	/* PC: SRR0 with low 2 bits cleared (OEA requirement; PPC is always word-aligned). */
	*out_pc = srr0 & ~3u;

	/* MSR: EXC_RFI_MSR_MASK bits come from SRR1; remaining bits come from
	 * the handler's current MSR. POW is never restored (see header). */
	*out_msr = (srr1 & EXC_RFI_MSR_MASK) | (cur_msr & ~EXC_RFI_MSR_MASK);
}
