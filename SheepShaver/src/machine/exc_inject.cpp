/*
 *  exc_inject.cpp - SS_M18 S3 (Operation NewSheep) T2: the host->NK EXT-injection
 *                   shim (LOAD-BEARING). See exc_inject.h for the architecture.
 *
 *  Pure module: no globals, no emulator dependencies. CONSUMES ExcEnter only —
 *  the PEM mask math in exc_core.cpp is LAW and is never referenced here beyond
 *  the public ExcEnter() call. g_exc_entry_table is DELIBERATELY NOT referenced:
 *  the injection builds its own NK-resolved table so a future T3 retirement of
 *  g_exc_entry_table cannot sever the async device-IRQ->NK-EXT seam.
 */

#include "exc_inject.h"

uint32_t ExcResolveNkExtVector(uint32_t kdp_base, ExcGuestRead32 read32, void *ctx)
{
	if (!read32)
		return 0;
	uint32_t v = read32(kdp_base + NK_KDP_EXT_VECTOR_OFFSET, ctx);
	/* Sentinel guard (Stop-rule #6/#9): an uninstalled / poisoned slot means the
	 * NK's KDP table has not been written yet (pre-S2b: no loader to run the
	 * install). Report UNRESOLVED — the caller STOPs rather than vector into
	 * junk. */
	if (v == 0u || v == 0xDEADBEEFu || v == 0xFFFFFFFFu)
		return 0;
	return v;
}

ExcTransition ExcInjectExternal(uint32_t restart_pc, uint32_t cur_msr,
                                uint32_t kdp_base, ExcGuestRead32 read32, void *ctx,
                                uint32_t *out_vector)
{
	uint32_t vec = ExcResolveNkExtVector(kdp_base, read32, ctx);
	if (out_vector)
		*out_vector = vec;
	if (vec == 0u) {
		/* Unresolved: do not vector. pc = EXC_PC_UNRESOLVED signals STOP. */
		ExcTransition t;
		t.srr0 = 0u;
		t.srr1 = 0u;
		t.msr  = 0u;
		t.pc   = EXC_PC_UNRESOLVED;
		return t;
	}
	/* LOCAL table — NOT g_exc_entry_table. external_entry carries the live
	 * NK-resolved vector; the remaining fields stay 0. Because external_entry is
	 * nonzero, ExcEnter(EXC_EXTERNAL) dispatches HERE (never the interrupt_entry
	 * fallback), so this path is provably independent of g_exc_entry_table. */
	ExcEntryTable nk_tbl;
	nk_tbl.interrupt_entry = 0u;
	nk_tbl.syscall_entry   = 0u;
	nk_tbl.program_entry   = 0u;
	nk_tbl.external_entry  = vec;
	return ExcEnter(restart_pc, cur_msr, EXC_EXTERNAL, &nk_tbl);
}

/* SS_M18 S3-impl T4: generic live-KDP vector resolver (the EXT resolver one slot
 * over) with the same Stop-rule #6 sentinel guard. */
uint32_t ExcResolveNkVector(uint32_t kdp_base, uint32_t slot_offset,
                            ExcGuestRead32 read32, void *ctx)
{
	if (!read32)
		return 0;
	uint32_t v = read32(kdp_base + slot_offset, ctx);
	if (v == 0u || v == 0xDEADBEEFu || v == 0xFFFFFFFFu)
		return 0;
	return v;
}

static ExcTransition exc_unresolved(void)
{
	ExcTransition t;
	t.srr0 = 0u; t.srr1 = 0u; t.msr = 0u; t.pc = EXC_PC_UNRESOLVED;
	return t;
}

ExcTransition ExcInjectSyscall(uint32_t restart_pc, uint32_t cur_msr,
                               uint32_t kdp_base, ExcGuestRead32 read32, void *ctx,
                               uint32_t *out_vector)
{
	uint32_t vec = ExcResolveNkVector(kdp_base, NK_KDP_SC_VECTOR_OFFSET, read32, ctx);
	if (out_vector)
		*out_vector = vec;
	if (vec == 0u)
		return exc_unresolved();
	/* LOCAL table — syscall_entry only; ExcEnter(EXC_SC) dispatches HERE, provably
	 * independent of g_exc_entry_table. */
	ExcEntryTable nk_tbl;
	nk_tbl.interrupt_entry = 0u;
	nk_tbl.syscall_entry   = vec;
	nk_tbl.program_entry   = 0u;
	nk_tbl.external_entry  = 0u;
	return ExcEnter(restart_pc, cur_msr, EXC_SC, &nk_tbl);
}

ExcTransition ExcInjectProgram(uint32_t restart_pc, uint32_t cur_msr,
                               uint32_t kdp_base, ExcGuestRead32 read32, void *ctx,
                               uint32_t *out_vector)
{
	uint32_t vec = ExcResolveNkVector(kdp_base, NK_KDP_PROGRAM_VECTOR_OFFSET, read32, ctx);
	if (out_vector)
		*out_vector = vec;
	if (vec == 0u)
		return exc_unresolved();
	ExcEntryTable nk_tbl;
	nk_tbl.interrupt_entry = 0u;
	nk_tbl.syscall_entry   = 0u;
	nk_tbl.program_entry   = vec;
	nk_tbl.external_entry  = 0u;
	return ExcEnter(restart_pc, cur_msr, EXC_PROGRAM, &nk_tbl);
}
