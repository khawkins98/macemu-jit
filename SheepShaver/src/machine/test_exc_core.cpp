/* Standalone unit test for exc_core.cpp — OEA exception-transition math.
 * Machine Layer M3a (docs/superpowers/plans/2026-06-10-machine-layer-m3a.md §Task 1).
 * Build: make -C SheepShaver/src/machine test */
#include "exc_core.h"
#include <stdio.h>
#include <assert.h>
#include <stdint.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

int main()
{
	/* --- Test 1: ExcDeliverable (EE = MSR bit 15 = 0x8000) --- */
	CHECK( ExcDeliverable(0xf072));  /* 0xf072 has EE=1 -> deliverable */
	CHECK(!ExcDeliverable(0x7072));  /* 0x7072 EE=0 -> not deliverable (anti-vacuity: differs by EE only) */
	CHECK( ExcDeliverable(0x8000));  /* minimum: just EE set */

	/* --- Test 2: Entry MSR transform (ExcEnter output .msr) --- */
	{
		ExcEntryTable tbl = { 0x504268a0u, 0x50312cb0u };
		/* 0xFFFFFFFF -> LITERAL 0xFFFB10CD: catches over- AND under-clearing + ME/IP
		 * preservation. MUST be a literal, not ~EXC_MSR_CLEAR_MASK - the macro-relative
		 * form is tautological (a one-bit mask typo passes by construction; a dropped-SE
		 * mutant survived the suite until this was pinned - quality-review finding). */
		ExcTransition t = ExcEnter(0x00001000u, 0xFFFFFFFFu, EXC_DECREMENTER, &tbl);
		CHECK(t.msr == 0xFFFB10CDu);
		/* 0xf072 -> 0x1040 (hand-derived above: the boot-real transform) */
		ExcTransition t2 = ExcEnter(0x00001000u, 0xf072u, EXC_DECREMENTER, &tbl);
		CHECK(t2.msr == 0x1040u);
		/* FE0|SE|BE|FE1 cluster (0x0F00): all in the clear mask AND in the RFI mask, so
		 * round-trip tests cannot distinguish wrongly-preserved from cleared - pin here. */
		ExcTransition t3 = ExcEnter(0x00001000u, 0x0F00u, EXC_DECREMENTER, &tbl);
		CHECK(t3.msr == 0u);
	}

	/* --- Test 3: SRR1 = cur_msr & EXC_SRR1_KEEP_MASK --- */
	{
		ExcEntryTable tbl = { 0x504268a0u, 0x50312cb0u };
		ExcTransition t1 = ExcEnter(0x00001000u, 0xFFFFFFFFu, EXC_DECREMENTER, &tbl);
		CHECK(t1.srr1 == 0x0000FFFFu);
		ExcTransition t2 = ExcEnter(0x00001000u, 0xf072u, EXC_DECREMENTER, &tbl);
		CHECK(t2.srr1 == 0xf072u);  /* boot-real equivalence: honest composition == legacy fiction */
	}

	/* --- Test 4: SRR0 semantics — DEC/EXT restart verbatim; SC = sc_addr+4 --- */
	{
		ExcEntryTable tbl = { 0x504268a0u, 0x50312cb0u };
		uint32_t restart = 0x50326880u;
		ExcTransition td = ExcEnter(restart, 0xf072u, EXC_DECREMENTER, &tbl);
		CHECK(td.srr0 == restart);
		ExcTransition te = ExcEnter(restart, 0xf072u, EXC_EXTERNAL, &tbl);
		CHECK(te.srr0 == restart);
		uint32_t sc_addr = 0x50312250u;
		ExcTransition ts = ExcEnter(sc_addr, 0xf072u, EXC_SC, &tbl);
		CHECK(ts.srr0 == sc_addr + 4u);
	}

	/* --- Test 5: Dispatch targets --- */
	{
		ExcEntryTable tbl = { 0x504268a0u, 0x50312cb0u };
		ExcTransition td = ExcEnter(0x00001000u, 0xf072u, EXC_DECREMENTER, &tbl);
		CHECK(td.pc == tbl.interrupt_entry);
		ExcTransition te = ExcEnter(0x00001000u, 0xf072u, EXC_EXTERNAL, &tbl);
		CHECK(te.pc == tbl.interrupt_entry);
		ExcTransition ts = ExcEnter(0x00001000u, 0xf072u, EXC_SC, &tbl);
		CHECK(ts.pc == tbl.syscall_entry);
		/* Zero syscall entry -> EXC_PC_UNRESOLVED */
		ExcEntryTable tbl0 = { 0x504268a0u, 0u };
		ExcTransition tu = ExcEnter(0x00001000u, 0xf072u, EXC_SC, &tbl0);
		CHECK(tu.pc == EXC_PC_UNRESOLVED);
	}

	/* --- Test 6: ExcRfi both directions + alignment mask --- */
	{
		uint32_t out_pc, out_msr;
		/* SRR1 saturated, MSR=0 -> only RFI_MASK bits survive from SRR1 */
		ExcRfi(0x00001000u, 0xFFFFFFFFu, 0u, &out_pc, &out_msr);
		CHECK(out_msr == 0x0000FF73u);
		/* SRR1=0, MSR saturated -> non-RFI_MASK bits come from cur_msr */
		ExcRfi(0x00001000u, 0u, 0xFFFFFFFFu, &out_pc, &out_msr);
		CHECK(out_msr == 0xFFFF008Cu);
		/* SRR0 alignment: low 2 bits cleared */
		ExcRfi(0x00001003u, 0u, 0u, &out_pc, &out_msr);
		CHECK(out_pc == (0x00001003u & ~3u));  /* = 0x00001000 */
	}

	/* --- Test 7: Round-trip ExcEnter -> ExcRfi restores pc and msr exactly ---
	 *   Valid for M that have no bits above 0xFFFF and no bits outside 0xFF73
	 *   that aren't preserved via the ~RFI_MSR path from entry_msr.
	 *   The three test values are constructed so POW (bit 18 = 0x40000) is zero.
	 *   Documented loss: POW IS in EXC_MSR_CLEAR_MASK -> cleared on entry and
	 *   not captured in SRR1 -> not restorable by rfi. Asserted below. */
	{
		ExcEntryTable tbl = { 0x504268a0u, 0x50312cb0u };
		uint32_t P = 0x50326880u;  /* aligned restart PC */
		static const uint32_t cases[] = { 0xf072u, 0x9032u, 0xAAAAAAAAu };
		for (int i = 0; i < 3; i++) {
			uint32_t M = cases[i];
			ExcTransition t = ExcEnter(P, M, EXC_DECREMENTER, &tbl);
			uint32_t out_pc, out_msr;
			ExcRfi(t.srr0, t.srr1, t.msr, &out_pc, &out_msr);
			CHECK(out_pc  == P);
			CHECK(out_msr == M);
		}
		/* Documented-loss case: POW = 0x40000 is cleared on entry and above the
		 * SRR1 16-bit window -> rfi cannot restore it */
		uint32_t M_pow = 0x00040000u;  /* only the POW bit */
		ExcTransition tp = ExcEnter(P, M_pow, EXC_DECREMENTER, &tbl);
		uint32_t out_pc2, out_msr2;
		ExcRfi(tp.srr0, tp.srr1, tp.msr, &out_pc2, &out_msr2);
		CHECK(out_msr2 != M_pow);  /* POW lost -- architecturally correct */
	}

	/* --- Test 8: EXC_PROGRAM (FE1F-service-surface Task A, plan rev 3) ---
	 * Trap-type program exception (vector 0x700). Existing tests above are
	 * untouched (extend, never edit). */
	{
		ExcEntryTable tbl = { 0x504268a0u, 0x50312cb0u, 0x50314700u };

		/* 8a: SRR0 = the trap instruction itself, VERBATIM (no +4 — PEM: SRR1
		 * bit 15 = 0, SRR0 points at the offending tw/twi). The live anchor:
		 * the slot-8 twi at 0x5046e8e0. */
		uint32_t twi_addr = 0x5046e8e0u;
		ExcTransition tp = ExcEnter(twi_addr, 0xf072u, EXC_PROGRAM, &tbl);
		CHECK(tp.srr0 == twi_addr);

		/* 8b: SRR1 = (msr & KEEP_MASK) | trap bit — the trap-cause literal is
		 * TEST-PINNED (PEM program-interrupt SRR1 bit 14 = 0x00020000; a literal,
		 * not the macro — same anti-tautology rule as Test 2). */
		CHECK(tp.srr1 == (0xf072u | 0x00020000u));
		ExcTransition tps = ExcEnter(twi_addr, 0xFFFFFFFFu, EXC_PROGRAM, &tbl);
		CHECK(tps.srr1 == 0x0002FFFFu);  /* low-16 keep + trap bit, nothing else */

		/* 8c: entry MSR transform is class-independent (masks are LAW). */
		CHECK(tps.msr == 0xFFFB10CDu);   /* same literal Test 2 pins for DEC */

		/* 8d: dispatch target = program_entry; zero -> EXC_PC_UNRESOLVED. */
		CHECK(tp.pc == tbl.program_entry);
		ExcEntryTable tbl0 = { 0x504268a0u, 0x50312cb0u, 0u };
		ExcTransition tu = ExcEnter(twi_addr, 0xf072u, EXC_PROGRAM, &tbl0);
		CHECK(tu.pc == EXC_PC_UNRESOLVED);

		/* 8e: anti-vacuity — the trap bit is PROGRAM-only; the other classes'
		 * SRR1 stays pure masked-MSR (no cause-bit leakage). */
		CHECK((ExcEnter(twi_addr, 0xf072u, EXC_DECREMENTER, &tbl).srr1 & 0x00020000u) == 0u);
		CHECK((ExcEnter(twi_addr, 0xf072u, EXC_EXTERNAL,    &tbl).srr1 & 0x00020000u) == 0u);
		CHECK((ExcEnter(twi_addr, 0xf072u, EXC_SC,          &tbl).srr1 & 0x00020000u) == 0u);

		/* 8f: two-field aggregate initializers (the pre-Task-A form) leave
		 * program_entry zero-initialized -> UNRESOLVED, not garbage. */
		ExcEntryTable tbl_legacy = { 0x504268a0u, 0x50312cb0u };
		ExcTransition tl = ExcEnter(twi_addr, 0xf072u, EXC_PROGRAM, &tbl_legacy);
		CHECK(tl.pc == EXC_PC_UNRESOLVED);

		/* 8g: rfi round trip — SRR0 verbatim means rfi would RE-EXECUTE the trap
		 * site; the real 0x700 handler advances past it via its own r10+4
		 * protocol, not via SRR0. Pin the raw ExcRfi reading anyway: pc back =
		 * the twi address; the trap bit (above the 16-bit RFI window) does NOT
		 * leak into the restored MSR. */
		uint32_t out_pc, out_msr;
		ExcRfi(tp.srr0, tp.srr1, tp.msr, &out_pc, &out_msr);
		CHECK(out_pc == twi_addr);
		CHECK((out_msr & 0x00020000u) == 0u);
	}

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
