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
	 * SC saves pc+4: the instruction after the sc (the sc itself is cur_pc_restart). */
	t.srr0 = (cls == EXC_SYSCALL) ? cur_pc_restart + 4u : cur_pc_restart;

	/* SRR1: capture only the low 16 bits of the pre-exception MSR. */
	t.srr1 = cur_msr & EXC_SRR1_KEEP_MASK;

	/* New handler MSR: clear the entry-mask bits (POW/EE/PR/FP/FE0/SE/BE/FE1/IR/DR/RI).
	 * ME and IP are preserved. */
	t.msr = cur_msr & ~EXC_MSR_CLEAR_MASK;

	/* Dispatch target: DEC and EXT share interrupt_entry; SC uses syscall_entry.
	 * 0 in the table entry -> EXC_PC_UNRESOLVED (caller handles via SS_EXC_SC knob). */
	uint32_t entry = (cls == EXC_SYSCALL) ? tbl->syscall_entry : tbl->interrupt_entry;
	t.pc = entry ? entry : EXC_PC_UNRESOLVED;

	return t;
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
