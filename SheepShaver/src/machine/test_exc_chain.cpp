/* Standalone unit test for the Wave-2 W2-0 delivery-decision chain:
 * ExcDeliveryDecision + ExcEdgeReRaise (exc_core) composed with the real
 * VirtClock DEC latch. EE-CHAIN-RECON.md §C.1 case table U1-U12 (U12 reworded
 * per Wave-2 rev 2 F6 to mask/decision parity + the pre-positioned-but-
 * unconsumed external_entry) + U13 (rev 2 tension 2: dual-pending priority).
 * Plan: docs/superpowers/plans/2026-06-11-wave2-interrupt-chain.md Task W2-0.
 *
 * Live-tree numbering note: the recon's U-case table predates the FE1F
 * service-surface milestone's EXC_PROGRAM/program_entry (appended third);
 * external_entry is therefore the FOURTH ExcEntryTable field, not the third.
 * The U-case IDs themselves are unchanged.
 *
 * Build: make -C SheepShaver/src/machine test */
#include "exc_core.h"
#include "virt_clock.h"
#include <stdio.h>
#include <assert.h>
#include <stdint.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

/* Deterministic injected time source for the real-VirtClock cases. */
static uint64_t fake_ns = 0;
static uint64_t stub_now_ns(void *) { return fake_ns; }

int main()
{
	/* The boot-real entry table shape (4 fields since W2-0; external_entry
	 * pre-positioned per rev 2 F6, default candidate 0x50314880, UNCONSUMED). */
	ExcEntryTable tbl = { 0x50412b1cu, 0x50314ac0u, 0x50314700u, 0x50314880u };

	/* --- U1: pending=0, everything else permissive -> NONE, no state --- */
	CHECK(ExcDeliveryDecision(0, 1, 0xf072u, 0) == EXC_DECIDE_NONE);
	/* pending gates FIRST: even with every later gate closed, no-pending is NONE */
	CHECK(ExcDeliveryDecision(0, 2, 0x7072u, 1) == EXC_DECIDE_NONE);

	/* --- U2: pending=1, depth=2, EE=1, mode=0 -> DEFER_DEPTH (depth outranks EE) --- */
	CHECK(ExcDeliveryDecision(1, 2, 0xf072u, 0) == EXC_DECIDE_DEFER_DEPTH);
	/* depth outranks EE — the telemetry contract (EE also closed, still DEPTH) */
	CHECK(ExcDeliveryDecision(1, 2, 0x7072u, 0) == EXC_DECIDE_DEFER_DEPTH);
	/* the live gate is depth != 1, not depth > 1: depth 0 defers too */
	CHECK(ExcDeliveryDecision(1, 0, 0xf072u, 0) == EXC_DECIDE_DEFER_DEPTH);

	/* --- U3: pending=1, depth=1, EE=0, mode=0 -> DEFER_EE --- */
	CHECK(ExcDeliveryDecision(1, 1, 0x7072u, 0) == EXC_DECIDE_DEFER_EE);

	/* --- U4: pending=1, depth=1, EE=1, mode!=0 -> DEFER_NATIVE (the W2 fence) --- */
	CHECK(ExcDeliveryDecision(1, 1, 0xf072u, 1) == EXC_DECIDE_DEFER_NATIVE);
	/* EE outranks native: EE closed + mode set still counts as DEFER_EE */
	CHECK(ExcDeliveryDecision(1, 1, 0x7072u, 1) == EXC_DECIDE_DEFER_EE);
	/* any nonzero mode word fences (the NK parks the MMCB pointer there) */
	CHECK(ExcDeliveryDecision(1, 1, 0xf072u, 0x68fff400u) == EXC_DECIDE_DEFER_NATIVE);

	/* --- U5: pending=1, depth=1, EE=1, mode=0 -> DELIVER --- */
	CHECK(ExcDeliveryDecision(1, 1, 0xf072u, 0) == EXC_DECIDE_DELIVER);
	/* minimum-EE form (just bit 15) delivers too — EE is the only MSR gate */
	CHECK(ExcDeliveryDecision(1, 1, 0x8000u, 0) == EXC_DECIDE_DELIVER);

	/* --- U6: deferral retains the latch; delivery consumes EXACTLY once
	 *        (real VirtClock; the decision function itself touches NO state —
	 *        the caller-obligation contract in exc_core.h) --- */
	{
		VirtClock c;
		VirtClockInit(&c, 25000000u, stub_now_ns, 0);
		c.dec_pending = 1;   /* the latch word (virt_clock.h:42) */

		/* deferral paths: decision computed, latch untouched */
		CHECK(ExcDeliveryDecision(VirtClockDECPending(&c), 2, 0xf072u, 0)
		      == EXC_DECIDE_DEFER_DEPTH);
		CHECK(VirtClockDECPending(&c));
		CHECK(ExcDeliveryDecision(VirtClockDECPending(&c), 1, 0x7072u, 0)
		      == EXC_DECIDE_DEFER_EE);
		CHECK(VirtClockDECPending(&c));
		CHECK(ExcDeliveryDecision(VirtClockDECPending(&c), 1, 0xf072u, 1)
		      == EXC_DECIDE_DEFER_NATIVE);
		CHECK(VirtClockDECPending(&c));

		/* delivery: caller clears exactly once; latch then reads clear and the
		 * next decision is NONE (no double consumption) */
		CHECK(ExcDeliveryDecision(VirtClockDECPending(&c), 1, 0xf072u, 0)
		      == EXC_DECIDE_DELIVER);
		VirtClockClearDECPending(&c);
		CHECK(!VirtClockDECPending(&c));
		CHECK(ExcDeliveryDecision(VirtClockDECPending(&c), 1, 0xf072u, 0)
		      == EXC_DECIDE_NONE);
	}

	/* --- U7: edge-predicate truth table — fires iff (old EE=0 ∧ new EE=1 ∧ pending) --- */
	CHECK( ExcEdgeReRaise(0x7072u, 0xf072u, 1));  /* 0->1, pending: FIRE */
	CHECK(!ExcEdgeReRaise(0x7072u, 0xf072u, 0));  /* 0->1, no pending */
	CHECK(!ExcEdgeReRaise(0xf072u, 0xf072u, 1));  /* 1->1 (no edge) */
	CHECK(!ExcEdgeReRaise(0xf072u, 0x7072u, 1));  /* 1->0 (falling) */
	CHECK(!ExcEdgeReRaise(0x7072u, 0x7072u, 1));  /* 0->0 */
	CHECK(!ExcEdgeReRaise(0x1040u, 0x5052u, 1));  /* non-EE bit churn, EE stable 0 */

	/* --- U8: the mtmsr re-raise contracts (the night-run pair) --- */
	CHECK( ExcEdgeReRaise(0x7072u, 0xf072u, 1));
	CHECK( ExcEdgeReRaise(0x7072u, 0xd032u, 1));
	CHECK(!ExcEdgeReRaise(0xf072u, 0x7072u, 1));

	/* --- U9: rfi composition — ExcRfi from srr1=0xf072 at handler msr 0x1040
	 *        raises EE; feed old/new into the edge predicate with pending -> fire
	 *        (the §B.3 instant-delivery scenario as math) --- */
	{
		uint32_t out_pc, out_msr;
		ExcRfi(0x50326880u, 0xf072u, 0x1040u, &out_pc, &out_msr);
		CHECK(out_msr == 0xf072u);             /* full restore through the RFI mask */
		CHECK((out_msr & 0x8000u) != 0);       /* EE rose */
		CHECK(ExcEdgeReRaise(0x1040u, out_msr, 1));
		CHECK(out_pc == 0x50326880u);
	}

	/* --- U10: the full deliver -> rfi -> deliver storm round trip
	 *         (never double-consumes the latch) --- */
	{
		VirtClock c;
		VirtClockInit(&c, 25000000u, stub_now_ns, 0);

		/* expiry latches while EE=0: DEFER_EE */
		c.dec_pending = 1;
		uint32_t msr = 0x7072u;
		CHECK(ExcDeliveryDecision(VirtClockDECPending(&c), 1, msr, 0)
		      == EXC_DECIDE_DEFER_EE);

		/* guest mtmsr raises EE: edge fires, poll delivers */
		uint32_t new_msr = 0xf072u;
		CHECK(ExcEdgeReRaise(msr, new_msr, VirtClockDECPending(&c)));
		msr = new_msr;
		CHECK(ExcDeliveryDecision(VirtClockDECPending(&c), 1, msr, 0)
		      == EXC_DECIDE_DELIVER);
		ExcTransition t = ExcEnter(0x50326880u, msr, EXC_DECREMENTER, &tbl);
		VirtClockClearDECPending(&c);          /* consume EXACTLY once */
		CHECK(!VirtClockDECPending(&c));
		CHECK(t.msr == 0x1040u);               /* handler runs EE=0 */
		msr = t.msr;

		/* second expiry while in the handler: DEFER_EE again (no recursion) */
		c.dec_pending = 1;
		CHECK(ExcDeliveryDecision(VirtClockDECPending(&c), 1, msr, 0)
		      == EXC_DECIDE_DEFER_EE);

		/* handler rfi restores EE=1: edge fires, second delivery, consume once */
		uint32_t out_pc, out_msr;
		ExcRfi(t.srr0, t.srr1, msr, &out_pc, &out_msr);
		CHECK(ExcEdgeReRaise(msr, out_msr, VirtClockDECPending(&c)));
		CHECK(ExcDeliveryDecision(VirtClockDECPending(&c), 1, out_msr, 0)
		      == EXC_DECIDE_DELIVER);
		VirtClockClearDECPending(&c);
		CHECK(!VirtClockDECPending(&c));
		/* cadence terminates: nothing pending -> NONE (no double consumption) */
		CHECK(ExcDeliveryDecision(VirtClockDECPending(&c), 1, out_msr, 0)
		      == EXC_DECIDE_NONE);
	}

	/* --- U11: the [0x2810]-fence release — mode 1 -> DEFER_NATIVE; the NK's
	 *         switch-back clear (stw r8,0x2810(0) at 0x50312b04, modeled as
	 *         input) -> DELIVER on the next poll, latch retained across --- */
	{
		VirtClock c;
		VirtClockInit(&c, 25000000u, stub_now_ns, 0);
		c.dec_pending = 1;
		CHECK(ExcDeliveryDecision(VirtClockDECPending(&c), 1, 0xf072u, 1)
		      == EXC_DECIDE_DEFER_NATIVE);
		CHECK(VirtClockDECPending(&c));        /* fence deferral retains the latch */
		CHECK(ExcDeliveryDecision(VirtClockDECPending(&c), 1, 0xf072u, 0)
		      == EXC_DECIDE_DELIVER);
	}

	/* --- U12: EXC_EXTERNAL parity (reworded per Wave-2 rev 2 F6: MASK parity,
	 *         not shared-entry-shape) — the decision logic is source-agnostic,
	 *         ExcEnter(EXC_EXTERNAL) applies the same LAW masks, and (SINCE
	 *         W2-3 — the sanctioned, deliberate U12 flip) external_entry IS
	 *         consumed when nonzero, with the interrupt_entry fallback when 0.
	 *         Both arms pinned below. --- */
	{
		/* decision parity: an EXT-pending input walks the identical gates */
		CHECK(ExcDeliveryDecision(1, 2, 0xf072u, 0) == EXC_DECIDE_DEFER_DEPTH);
		CHECK(ExcDeliveryDecision(1, 1, 0x7072u, 0) == EXC_DECIDE_DEFER_EE);
		CHECK(ExcDeliveryDecision(1, 1, 0xf072u, 1) == EXC_DECIDE_DEFER_NATIVE);
		CHECK(ExcDeliveryDecision(1, 1, 0xf072u, 0) == EXC_DECIDE_DELIVER);

		/* mask parity: EXT transition == DEC transition except (nothing) */
		ExcTransition te = ExcEnter(0x50326880u, 0xf072u, EXC_EXTERNAL, &tbl);
		ExcTransition td = ExcEnter(0x50326880u, 0xf072u, EXC_DECREMENTER, &tbl);
		CHECK(te.srr0 == td.srr0);
		CHECK(te.srr1 == td.srr1);
		CHECK(te.msr  == td.msr);

		/* W2-3 consumption pin (the deliberate U12 flip — pre-W2-3 this pinned
		 * the UNCONSUMED state; the flip is the sanctioned drift, recorded in
		 * the Wave-2 plan W2-3 body): EXT now dispatches to external_entry
		 * (0x50314880, the NK-published [KDP+0x374] per Q-W2) when nonzero. */
		CHECK(te.pc == tbl.external_entry);
		CHECK(te.pc != tbl.interrupt_entry);   /* anti-vacuous: targets differ */

		/* fallback arm pinned: external_entry=0 -> interrupt_entry (zero-field
		 * tables / pre-W2-3 aggregate initializers keep the shared-entry shape) */
		ExcEntryTable tbl_noext = { 0x50412b1cu, 0x50314ac0u, 0x50314700u, 0u };
		CHECK(ExcEnter(0x50326880u, 0xf072u, EXC_EXTERNAL, &tbl_noext).pc
		      == tbl_noext.interrupt_entry);

		/* SRR1.EE=1 mandatory at EXT delivery (the NK EXT body's punch-through
		 * guard PANICS on SRR1 bit 0x8000 clear — EE-CHAIN-RECON §W2S-2): the
		 * delivery gate admits only EE=1 MSRs, and ExcEnter's SRR1 keeps the
		 * low 16 bits — so any deliverable MSR yields SRR1.EE=1. Pin it. */
		CHECK((te.srr1 & 0x8000u) != 0u);          /* boot-real 0xf072 case */
		CHECK((ExcEnter(0x50326880u, 0x8000u, EXC_EXTERNAL, &tbl).srr1 & 0x8000u) != 0u);
	}

	/* --- U13 (rev 2 tension 2): dual-pending — DEC and EXT both pending.
	 *    Priority DEC-before-EXT (M3b rev 2 m11/C1: OEA ranks External ABOVE
	 *    Decrementer, but our DEC latch is one-shot-clear-on-delivery while PIC
	 *    pending is LEVEL-HELD and safely waits one poll). The hook polls DEC
	 *    first; delivering DEC consumes ONLY the DEC latch — EXT pending
	 *    survives untouched and delivers on the next poll.
	 *
	 *    STARVATION PREDICATE (documented for W2-3, not asserted here): EXT
	 *    pending across >N consecutive DEC deliveries with NO intervening EXT
	 *    delivery must trip a loud tripwire (symmetric to the EXT re-delivery
	 *    runaway guard); the W2-3 hook owns the counter. --- */
	{
		VirtClock c;
		VirtClockInit(&c, 25000000u, stub_now_ns, 0);
		c.dec_pending = 1;          /* DEC: one-shot latch */
		int ext_pending = 1;        /* EXT: level-held output (the W2-3 PIC flag) */

		/* poll order DEC first: DEC delivers */
		CHECK(ExcDeliveryDecision(VirtClockDECPending(&c), 1, 0xf072u, 0)
		      == EXC_DECIDE_DELIVER);
		ExcTransition td = ExcEnter(0x50326880u, 0xf072u, EXC_DECREMENTER, &tbl);
		VirtClockClearDECPending(&c);   /* one-shot: DEC consumed */
		CHECK(td.pc == tbl.interrupt_entry);

		/* EXT survived the DEC delivery (level-held, caller never cleared it) */
		CHECK(ext_pending == 1);
		CHECK(ExcDeliveryDecision(ext_pending, 1, 0xf072u, 0)
		      == EXC_DECIDE_DELIVER);
		ExcTransition te = ExcEnter(0x50326880u, 0xf072u, EXC_EXTERNAL, &tbl);
		CHECK(te.pc == tbl.external_entry);   /* W2-3: EXT entry-point discrimination */
		/* and the DEC latch was not double-consumed by the EXT poll */
		CHECK(!VirtClockDECPending(&c));
	}

	/* --- U14 (M8 slot-4 consumption Task A): the deferred-EE-edge window
	 *    predicates (rfi-atomicity emulation). Boot-real window values
	 *    (9.0.1 primary copy, Task-0 recon Q-C1 [STATIC]+[PROBE✓]):
	 *    LIVE stub = [0x50318000, 0x5031801c) — 7 words TOTAL (3 nest + 3
	 *    riser + final b; boot-real: "riser windows ... stub 50318000-5031801c");
	 *    reload [0x503244e4, 0x50324528) — exclusive end one word PAST the
	 *    bctr at 0x324524 (the replaced rfi). The live windows are filled at
	 *    patch time by rom_patches.cpp (single source); these literals pin
	 *    the PREDICATE SEMANTICS, not the addresses — se below is a LOOSE
	 *    8-word test literal (0x...20), deliberately wider than the live end
	 *    (Task-C P2 relabel; the predicates are pure in [sb,se)). --- */
	{
		const uint32_t sb = 0x50318000u, se = 0x50318020u; /* se = loose test bound (live end 0x...1c) */
		const uint32_t rs = 0x503244e4u, re = 0x50324528u;

		/* latch: fires only inside the stub window */
		CHECK( ExcDeferredEdgeLatch(0x50318014u, sb, se));  /* the riser's mtmsr itself */
		CHECK( ExcDeferredEdgeLatch(0x50318018u, sb, se));  /* the post-mtmsr boundary (b4's torn restart) */
		CHECK( ExcDeferredEdgeLatch(0x50318000u, sb, se));  /* base inclusive */
		CHECK( ExcDeferredEdgeLatch(0x5031801cu, sb, se));  /* last word INSIDE the loose test bound
		                                                     * (one past the LIVE 7-word stub end) */
		CHECK(!ExcDeferredEdgeLatch(0x50318020u, sb, se));  /* end exclusive (of the loose bound) */
		CHECK(!ExcDeferredEdgeLatch(0x50317ffcu, sb, se));  /* below the window */
		CHECK(!ExcDeferredEdgeLatch(0x503244e8u, sb, se));  /* reload region is NOT a latch site */
		CHECK(!ExcDeferredEdgeLatch(0x50318014u, 0u, 0u));  /* riser not armed: empty window never latches */

		/* fire: held inside stub OR reload, fires everywhere else */
		CHECK(!ExcDeferredEdgeFire(0x50318018u, sb, se, rs, re)); /* still in the stub */
		CHECK(!ExcDeferredEdgeFire(0x503244e4u, sb, se, rs, re)); /* reload entry */
		CHECK(!ExcDeferredEdgeFire(0x503244e8u, sb, se, rs, re)); /* THE shape-A livelock block */
		CHECK(!ExcDeferredEdgeFire(0x503244f8u, sb, se, rs, re)); /* b2's run-variant block */
		CHECK(!ExcDeferredEdgeFire(0x50324524u, sb, se, rs, re)); /* the bctr word itself */
		CHECK( ExcDeferredEdgeFire(0x50324528u, sb, se, rs, re)); /* first word past the bctr */
		CHECK( ExcDeferredEdgeFire(0x5046e1a0u, sb, se, rs, re)); /* the DR resume PC (b4 ctx+0xfc) */
		CHECK( ExcDeferredEdgeFire(0x50313200u, sb, se, rs, re)); /* the published DEC entry */
		CHECK( ExcDeferredEdgeFire(0x503244e8u, 0u, 0u, 0u, 0u)); /* empty windows: degenerate fire
		                                                           * (moot — empty window never latches) */
	}

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
