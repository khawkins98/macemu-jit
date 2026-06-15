/* PLANTED-TABLE micro-test for the SS_M18 S3 T2 host->NK EXT-injection shim
 * (G(T2) observable C1). The live install needs S2b (unbuilt), so this stands in
 * for a live boot: plant a SYNTHETIC ExcEntryTable / KDP slot at a known guest
 * address, point the shim's resolver at it, and assert an asserted device IRQ
 *   (a) resolves the EXT slot from the planted table (reads KDP+0x374),
 *   (b) vectors via ExcEnter(planted tbl) to the planted vector, and
 *   (c) g_exc_entry_table is NOT consulted on that path.
 * Plus the sentinel guards (0x0 / 0xDEADBEEF / 0xFFFFFFFF => STOP) and a
 * not-hardcoded check (a DIFFERENT planted vector must be followed).
 *
 * Build: make -C SheepShaver/src/machine test_exc_inject */
#include "exc_inject.h"
#include "exc_core.h"
#include <stdio.h>
#include <assert.h>
#include <stdint.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

/* POISON g_exc_entry_table. The shim MUST NOT consult this global on the
 * injection path; defining it here (exc_inject.cpp / exc_core.cpp never
 * reference it — declaration-only) proves link-independence AND lets us assert
 * the planted result never surfaces any poison field. */
ExcEntryTable g_exc_entry_table = { 0xBADBAD00u, 0xBADBAD01u, 0xBADBAD02u, 0xBADBAD03u };

#define PLANTED_KDP   0x68ffe000u   /* synthetic KDP base */
#define PLANTED_VEC   0x50314880u   /* value planted at KDP+0x374 (the NK EXT body) */

/* Fake guest memory: a single readable cell at KDP+0x374 whose value is *ctx.
 * Any read of a different address returns a poison word (would fail an assert if
 * the resolver consulted the wrong slot). */
static uint32_t g_last_read_addr = 0;
static int      g_read_count = 0;

static uint32_t planted_read32(uint32_t addr, void *ctx)
{
	g_last_read_addr = addr;
	g_read_count++;
	if (addr == PLANTED_KDP + NK_KDP_EXT_VECTOR_OFFSET)
		return *(uint32_t *)ctx;
	return 0xCAFEF00Du;   /* wrong slot — must never be returned */
}

/* T4 reader: serves whichever of the three NK vector slots (EXT/SC/PROGRAM) is
 * read, returning the planted value *ctx. Records the slot addr so the test can
 * assert each injector consults ITS slot. A read of any other address returns the
 * poison word (would fail the slot-addr asserts). */
static uint32_t planted_read32_slot(uint32_t addr, void *ctx)
{
	g_last_read_addr = addr;
	g_read_count++;
	if (addr == PLANTED_KDP + NK_KDP_EXT_VECTOR_OFFSET ||
	    addr == PLANTED_KDP + NK_KDP_SC_VECTOR_OFFSET ||
	    addr == PLANTED_KDP + NK_KDP_PROGRAM_VECTOR_OFFSET)
		return *(uint32_t *)ctx;
	return 0xCAFEF00Du;   /* wrong slot — must never be returned */
}

int main()
{
	/* (a) resolve reads the EXT slot from the planted table at KDP+0x374. */
	uint32_t slot = PLANTED_VEC;
	g_read_count = 0;
	uint32_t vec = ExcResolveNkExtVector(PLANTED_KDP, planted_read32, &slot);
	CHECK(vec == PLANTED_VEC);
	CHECK(g_last_read_addr == PLANTED_KDP + 0x374u);   /* the EXT slot, not the +0x360 base */
	CHECK(g_read_count == 1);                          /* exactly one read — cacheable */

	/* (b) ExcInjectExternal vectors via ExcEnter(planted tbl) to the planted
	 * vector, with the PEM transition applied byte-for-byte (masks = LAW). */
	uint32_t out_vec = 0;
	const uint32_t restart = 0x90001234u;
	const uint32_t msr_in  = 0x0000f072u;   /* the live NK MSR shape (EE=1) */
	ExcTransition t = ExcInjectExternal(restart, msr_in,
	                                    PLANTED_KDP, planted_read32, &slot, &out_vec);
	CHECK(out_vec == PLANTED_VEC);
	CHECK(t.pc   == PLANTED_VEC);                       /* dispatched to the planted vector */
	CHECK(t.srr0 == restart);                          /* EXT saves restart PC verbatim */
	CHECK(t.srr1 == (msr_in & EXC_SRR1_KEEP_MASK));    /* PEM mask, untouched */
	CHECK(t.msr  == (msr_in & ~EXC_MSR_CLEAR_MASK));   /* PEM mask, untouched */

	/* (c) g_exc_entry_table is NOT consulted — no poison field surfaces. */
	CHECK(t.pc != g_exc_entry_table.external_entry);
	CHECK(t.pc != g_exc_entry_table.interrupt_entry);
	CHECK(t.pc != g_exc_entry_table.syscall_entry);
	CHECK(t.pc != g_exc_entry_table.program_entry);

	/* Not-hardcoded: plant a DIFFERENT vector; the shim must follow the live
	 * table, not a baked-in 0x50314880. */
	uint32_t alt = 0x50319999u;
	ExcTransition t2 = ExcInjectExternal(0x90002000u, msr_in,
	                                     PLANTED_KDP, planted_read32, &alt, &out_vec);
	CHECK(out_vec == alt);
	CHECK(t2.pc == alt);

	/* Sentinel guards => UNRESOLVED (the EXPECTED gated-ON state pre-S2b). */
	uint32_t z    = 0x00000000u;
	uint32_t dead = 0xDEADBEEFu;
	uint32_t allf = 0xFFFFFFFFu;
	CHECK(ExcResolveNkExtVector(PLANTED_KDP, planted_read32, &z)    == 0u);
	CHECK(ExcResolveNkExtVector(PLANTED_KDP, planted_read32, &dead) == 0u);
	CHECK(ExcResolveNkExtVector(PLANTED_KDP, planted_read32, &allf) == 0u);

	/* Unresolved inject => pc = EXC_PC_UNRESOLVED, out_vector = 0 (caller STOPs). */
	ExcTransition tu = ExcInjectExternal(0x90003000u, msr_in,
	                                     PLANTED_KDP, planted_read32, &z, &out_vec);
	CHECK(out_vec == 0u);
	CHECK(tu.pc == EXC_PC_UNRESOLVED);

	/* A null reader is also UNRESOLVED (defensive). */
	CHECK(ExcResolveNkExtVector(PLANTED_KDP, NULL, &slot) == 0u);

	/* ============================================================
	 * SS_M18 S3-impl T4 — sc / program injectors (same planted-table
	 * contract, the SYNCHRONOUS seams). Plant per-slot values and assert each
	 * injector (a) reads ITS slot (SC=KDP+0x390 / PROGRAM=KDP+0x37c), (b) vectors
	 * via ExcEnter to the planted vector with the right exc class, (c) never
	 * consults g_exc_entry_table, and (d) honors the sentinel STOP. */
	{
		uint32_t sc_vec   = 0x50314ac0u;   /* NK-published sc handler */
		uint32_t prog_vec = 0x50314700u;   /* NK-published program handler */

		/* sc: reads KDP+0x390, EXC_SC (SRR0 = pc, NOT pc+4 — the kpx_cpu sc path
		 * sets PC absolutely; ExcEnter's EXC_SC composes srr0 from cur_pc_restart). */
		uint32_t scout = 0;
		g_read_count = 0;
		ExcTransition ts = ExcInjectSyscall(0x90004000u, msr_in,
		                                    PLANTED_KDP, planted_read32_slot, &sc_vec, &scout);
		CHECK(g_last_read_addr == PLANTED_KDP + NK_KDP_SC_VECTOR_OFFSET);  /* 0x390 */
		CHECK(NK_KDP_SC_VECTOR_OFFSET == 0x390u);
		CHECK(scout == sc_vec);
		CHECK(ts.pc == sc_vec);
		CHECK(ts.pc != g_exc_entry_table.syscall_entry);   /* poison not surfaced */
		CHECK(ts.pc != g_exc_entry_table.program_entry);

		/* not-hardcoded: a different planted sc vector is followed. */
		uint32_t sc_alt = 0x5031abcdu;
		ExcTransition ts2 = ExcInjectSyscall(0x90004100u, msr_in,
		                                     PLANTED_KDP, planted_read32_slot, &sc_alt, &scout);
		CHECK(scout == sc_alt);
		CHECK(ts2.pc == sc_alt);

		/* program: reads KDP+0x37c, EXC_PROGRAM. */
		uint32_t pout = 0;
		ExcTransition tp = ExcInjectProgram(0x90005000u, msr_in,
		                                    PLANTED_KDP, planted_read32_slot, &prog_vec, &pout);
		CHECK(g_last_read_addr == PLANTED_KDP + NK_KDP_PROGRAM_VECTOR_OFFSET);  /* 0x37c */
		CHECK(NK_KDP_PROGRAM_VECTOR_OFFSET == 0x37cu);
		CHECK(pout == prog_vec);
		CHECK(tp.pc == prog_vec);
		CHECK(tp.pc != g_exc_entry_table.program_entry);   /* poison not surfaced */

		/* sentinel STOP for both synchronous injectors (the pre-S2b gated-ON state). */
		uint32_t dead2 = 0xDEADBEEFu;
		ExcTransition tsu = ExcInjectSyscall(0x90006000u, msr_in,
		                                     PLANTED_KDP, planted_read32_slot, &dead2, &scout);
		CHECK(scout == 0u);
		CHECK(tsu.pc == EXC_PC_UNRESOLVED);
		ExcTransition tpu = ExcInjectProgram(0x90006100u, msr_in,
		                                     PLANTED_KDP, planted_read32_slot, &dead2, &pout);
		CHECK(pout == 0u);
		CHECK(tpu.pc == EXC_PC_UNRESOLVED);
	}

	printf("test_exc_inject: %d checks passed\n", n_pass);
	return 0;
}
