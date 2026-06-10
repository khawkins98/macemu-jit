/*
 *  mmio_machfault.cpp - JIT-path MMIO dispatch (MACHINE-LAYER-PLAN section 2b path 1).
 *  Runs on the Mach exception-handler thread with the CPU thread suspended (section 2g):
 *  no malloc, no foreign locks, stdio only on terminal-abort paths.
 *  Port of spikes/s2-mach-fault-decode/main.cpp (decode at :120, inject at :128-130).
 */

#include "mmio_bus.h"
#include "a64_mmio_decode.h"
#include <mach/mach.h>
#include <mach/thread_status.h>
#include <stdio.h>
#include <stdlib.h>

// The real backpatcher lives in ppc-jit.cpp (Task 9), compiled only when that build
// defines SS_MMIO_BACKPATCH (rev 2 finding C4). Those are STRONG definitions that, when
// present, override the weak no-op defaults below.
//
// On Mach-O a plain `weak` (or `weak_import`) *undefined* symbol still fails to link -
// the static linker can only mark a symbol weak if some linked image defines it. So
// rather than rely on a NULL address for an absent symbol, we provide weak default
// definitions here: any build without the strong overrides (the emulator build before
// Task 9, the rom-harness build, and this standalone unit test) links these inert stubs
// and the backpatch path stays cold-only. The pointer guards in MMIOMachFaultDispatch
// remain valid (the symbols are always non-NULL; the stubs simply return false).
extern "C" __attribute__((weak)) bool ppc_jit_pc_in_cache(const void *host_pc)
{ (void)host_pc; return false; }
extern "C" __attribute__((weak)) bool ppc_jit_backpatch_mmio(uint32_t *site, const A64MemAccess *acc)
{ (void)site; (void)acc; return false; }

#define MMIO_BACKPATCH_THRESHOLD 8

// Per-site fault counters: open-addressed table, touched ONLY by the (single)
// Mach handler thread -> no locking. Preallocated (section 2g: no malloc).
#define SITE_TABLE_SIZE 1024   // power of two
static struct { uintptr_t pc; uint32_t count; } site_table[SITE_TABLE_SIZE];

static uint32_t site_count_bump(uintptr_t pc)
{
	uint32_t h = (uint32_t)(pc >> 2) & (SITE_TABLE_SIZE - 1);
	for (unsigned probe = 0; probe < 8; probe++, h = (h + 1) & (SITE_TABLE_SIZE - 1)) {
		if (site_table[h].pc == pc) return ++site_table[h].count;
		if (site_table[h].pc == 0) { site_table[h].pc = pc; site_table[h].count = 1; return 1; }
	}
	return 1;   // table pressure: behave as cold (keep injecting)
}

bool MMIOMachFaultDispatch(uint32_t guest_addr, void *thread_state64)
{
#ifdef _STRUCT_ARM_THREAD_STATE64
	_STRUCT_ARM_THREAD_STATE64 *ts = (_STRUCT_ARM_THREAD_STATE64 *)thread_state64;
	if (!ts) return false;
	uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(*ts);   // PAC-safe (S2 section 3)
	if (!pc) return false;
	uint32_t insn = *(const uint32_t *)pc;                       // same-task read (S2 section 4.3)

	A64MemAccess acc;
	if (!A64DecodeMMIOAccess(insn, &acc)) {
		// In bus range but not a JIT guest-access form: a host C++ accessor or our
		// own tooling touched device space through a raw pointer. Contract violation
		// (MACHINE-LAYER-PLAN section 2b host-accessor path): abort loudly, never skip.
		fprintf(stderr, "[MMIO] FATAL: undecodable access to device addr 0x%08x at host pc %p "
		        "(insn 0x%08x) - host code must use MMIOBusRead/Write\n",
		        guest_addr, (void *)pc, insn);
		abort();
	}

	MMIOBusCountJITFault(guest_addr);
	unsigned bytes = 1u << acc.size_log2;

	// Hot site? Hand it to the backpatcher (Task 9): the CPU thread is suspended AT
	// this pc, so rewriting the site and resuming WITHOUT inject re-executes it as a
	// bus call. Until Task 9 lands the weak symbol is absent and this is skipped.
	if (ppc_jit_pc_in_cache && ppc_jit_backpatch_mmio
	    && ppc_jit_pc_in_cache((const void *)pc)
	    && site_count_bump(pc) >= MMIO_BACKPATCH_THRESHOLD
	    && ppc_jit_backpatch_mmio((uint32_t *)pc, &acc)) {
		MMIOBusCountBackpatch(guest_addr);
		return true;   // state untouched; resume re-executes the patched BL
	}

	// Cold path: emulate the access, skip the LDR/STR only (the REV still runs - S2 section 2 contract).
	if (acc.is_load) {
		uint64_t arch = MMIOBusRead(guest_addr, bytes);
		uint64_t raw = A64SwapForWidth(arch, acc.size_log2);   // inject RAW BE; REV swaps it back
		if (acc.rt != 31)                                       // rt==31 is WZR/XZR (S2 section 4.3)
			ts->__x[acc.rt] = raw;                              // zero-extended 64-bit write
	} else {
		// Store: the JIT's REV ran BEFORE the faulting STR, so x[rt] already holds raw BE.
		uint64_t raw = (acc.rt == 31) ? 0 : ts->__x[acc.rt];
		uint64_t arch = A64SwapForWidth(raw, acc.size_log2);
		MMIOBusWrite(guest_addr, bytes, arch);
	}
	arm_thread_state64_set_pc_fptr(*ts, (void *)(pc + 4));     // PAC-safe PC advance
	return true;
#else
	(void)guest_addr; (void)thread_state64;
	return false;
#endif
}
