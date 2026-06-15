/*
 *  test_dev_openpic.cpp - Unit tests for the OpenPIC (KeyLargo MPIC) model
 *  (M3b Wave 2, stop-rule disposition: model + tests only, wiring deferred).
 *  Conformance oracle: QEMU hw/intc/openpic.c + include/hw/ppc/openpic.h
 *  @ de5d8bfd6105d3dd3ae668df9762df244a6d1506 (see dev_openpic.h header notes).
 *  Vectors pin VALUES (mutation-resistant), not echoes: reset constants,
 *  the IVPR write mask, IACK/EOI lifecycle, edge/level sense, TPR gating,
 *  priority ordering, the Q8 input numbers, the absent-device latches.
 */
#include "dev_openpic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

#define BASE 0xF3040000u

// Register offsets (pinned literals — oracle openpic.c dispatch tables)
#define R_BRR1   0x0000u
#define R_FRR    0x1000u
#define R_GCR    0x1020u
#define R_VIR    0x1080u
#define R_PIR    0x1090u
#define R_SPVE   0x10E0u
#define R_CTPR   0x20080u
#define R_WHOAMI 0x20090u
#define R_IACK   0x200A0u
#define R_EOI    0x200B0u
#define SRC_IVPR(n) (0x10000u + (uint32_t)(n) * 0x20u)
#define SRC_IDR(n)  (0x10000u + (uint32_t)(n) * 0x20u + 0x10u)

static OpenPICDevice pic;

static uint32_t rd(uint32_t off)            { return (uint32_t)OpenPICRead(&pic, BASE + off, 4); }
static void wr(uint32_t off, uint32_t val)  { OpenPICWrite(&pic, BASE + off, 4, val); }

// Output-transition recorder (the future-wiring seam under test)
static int out_events[64];
static int out_n;
static void on_out(void *opaque, bool asserted)
{
	(void)opaque;
	if (out_n < 64)
		out_events[out_n] = asserted ? 1 : 0;
	out_n++;
}

static void fresh(void)
{
	OpenPICReset(&pic, BASE);
	OpenPICBindOutput(&pic, on_out, NULL);
	out_n = 0;
}

// Program source n: priority, vector, sense (1 = level), IDR -> CPU0.
static void src_setup(int n, int prio, int vec, int sense)
{
	wr(SRC_IVPR(n), (sense ? OPENPIC_IVPR_SENSE : 0) |
	                ((uint32_t)prio << OPENPIC_IVPR_PRIO_SHIFT) | (uint32_t)vec);
	wr(SRC_IDR(n), 1);
}

int main()
{
	char buf[1024];

	// =====================================================================
	// A. Reset state (oracle: openpic_reset + KeyLargo realize config)
	// =====================================================================
	OpenPICReset(&pic, BASE);
	CHECK(rd(R_FRR) == 0x003F0002u);          // 63<<16 | 0<<8 | VID 2
	CHECK(rd(R_GCR) == 0);                    // reset completed synchronously
	CHECK(rd(R_SPVE) == 0xFFu);               // spve = -1 & vector_mask
	CHECK(rd(R_VIR) == 0);                    // VIR_GENERIC
	CHECK(rd(R_PIR) == 0);
	CHECK(rd(R_BRR1) == 0xFFFFFFFFu);         // KeyLargo brr1 = -1
	CHECK(rd(R_CTPR) == 15);                  // ctpr reset = 15 (all gated)
	CHECK(rd(R_WHOAMI) == 0);
	CHECK(rd(R_EOI) == 0);                    // EOI reads as 0
	CHECK(rd(SRC_IVPR(0)) == 0xA0000000u);    // MASK | MODE
	CHECK(rd(SRC_IVPR(0x19)) == 0xA0000000u);
	CHECK(rd(SRC_IVPR(63)) == 0xA0000000u);
	CHECK(rd(SRC_IDR(0x19)) == 0);            // idr_reset = 0
	CHECK(!OpenPICOutputAsserted(&pic));
	CHECK(OpenPICTakePendingWarning(&pic) == NULL);
	// IACK on a freshly reset PIC -> spurious vector
	CHECK(rd(R_IACK) == 0xFFu);
	CHECK(pic.iacks_total == 1 && pic.iacks_spurious == 1 && pic.iacks_bad == 0);
	CHECK(!OpenPICOutputAsserted(&pic));

	// =====================================================================
	// B. Register write masks (oracle: write_IRQreg_ivpr/idr)
	// =====================================================================
	fresh();
	wr(SRC_IVPR(5), 0xFFFFFFFFu);
	CHECK(rd(SRC_IVPR(5)) == 0x80CF00FFu);    // MASK|POL|SENSE|PRIO|vec; no MODE/ACT
	wr(SRC_IDR(5), 0xFFFFFFFFu);
	CHECK(rd(SRC_IDR(5)) == 1);               // masked to bit 0 (single CPU)
	wr(SRC_IVPR(5), OPENPIC_IVPR_MODE);       // MODE not writable
	CHECK(rd(SRC_IVPR(5)) == 0);              // ...and any write clears it
	wr(SRC_IVPR(5), OPENPIC_IVPR_ACTIVITY);   // ACTIVITY read-only
	CHECK(rd(SRC_IVPR(5)) == 0);
	// Read-only globals: writes ignored
	wr(R_FRR, 0x12345678u);
	CHECK(rd(R_FRR) == 0x003F0002u);
	wr(R_BRR1, 0);
	CHECK(rd(R_BRR1) == 0xFFFFFFFFu);
	wr(R_VIR, 0x55u);
	CHECK(rd(R_VIR) == 0);
	wr(R_WHOAMI, 7);
	CHECK(rd(R_WHOAMI) == 0);
	wr(R_IACK, 0x42u);                        // IACK is read-only
	CHECK(pic.iacks_total == 0);              // a WRITE must not IACK

	// =====================================================================
	// C. Basic edge-source lifecycle: raise -> output -> IACK -> EOI
	// =====================================================================
	fresh();
	wr(R_CTPR, 0);
	CHECK(rd(R_CTPR) == 0);
	src_setup(0x19, /*prio*/8, /*vec*/0x42, /*edge*/0);
	CHECK(!OpenPICOutputAsserted(&pic));      // programmed but not raised
	OpenPICRaiseInput(&pic, 0x19);
	CHECK(OpenPICOutputAsserted(&pic));
	CHECK(pic.raises[0x19] == 1);
	CHECK((rd(SRC_IVPR(0x19)) & OPENPIC_IVPR_ACTIVITY) != 0);   // activity visible
	CHECK(rd(R_IACK) == 0x42u);               // the programmed vector, not 0x19
	CHECK(!OpenPICOutputAsserted(&pic));      // IACK lowers the line
	CHECK(pic.iacks[0x19] == 1);
	CHECK((rd(SRC_IVPR(0x19)) & OPENPIC_IVPR_ACTIVITY) == 0);   // edge: consumed
	CHECK(rd(R_IACK) == 0xFFu);               // nothing left -> spurious
	CHECK(pic.iacks_spurious == 1);
	wr(R_EOI, 0);
	CHECK(pic.eois[0x19] == 1);
	wr(R_EOI, 0);                             // EOI with nothing in service
	CHECK(pic.eois_empty == 1);
	CHECK(pic.eois[0x19] == 1);
	// The edge PENDING latch was consumed by IACK: reprogramming the IVPR
	// (which runs update_irq) must not re-deliver a phantom interrupt
	wr(SRC_IVPR(0x19), (8u << 16) | 0x42u);
	CHECK(!OpenPICOutputAsserted(&pic));
	CHECK(rd(R_IACK) == 0xFFu);

	// =====================================================================
	// D. IVPR mask semantics: masked raise -> nothing; unmask -> delivers
	// =====================================================================
	fresh();
	wr(R_CTPR, 0);
	wr(SRC_IVPR(0x20), OPENPIC_IVPR_MASK | (7u << 16) | 0x66u);   // masked
	wr(SRC_IDR(0x20), 1);
	OpenPICRaiseInput(&pic, 0x20);
	CHECK(!OpenPICOutputAsserted(&pic));
	CHECK((rd(SRC_IVPR(0x20)) & OPENPIC_IVPR_ACTIVITY) == 0);
	CHECK(rd(R_IACK) == 0xFFu);               // no IACK for a masked source
	// Unmasking delivers the latched edge (oracle: pending set even when masked)
	wr(SRC_IVPR(0x20), (7u << 16) | 0x66u);
	CHECK(OpenPICOutputAsserted(&pic));
	CHECK(rd(R_IACK) == 0x66u);
	wr(R_EOI, 0);
	// Re-masking a pending level source withdraws it (update on IVPR write)
	src_setup(0x21, 6, 0x21, /*level*/1);
	OpenPICRaiseInput(&pic, 0x21);
	CHECK(OpenPICOutputAsserted(&pic));
	wr(SRC_IVPR(0x21), OPENPIC_IVPR_MASK | OPENPIC_IVPR_SENSE | (6u << 16) | 0x21u);
	CHECK(!OpenPICOutputAsserted(&pic));
	CHECK(rd(R_IACK) == 0xFFu);
	OpenPICLowerInput(&pic, 0x21);

	// =====================================================================
	// E. Priority ordering: two pending -> higher-priority vector first
	// =====================================================================
	fresh();
	wr(R_CTPR, 0);
	src_setup(0x24, /*prio*/3, /*vec*/0x24, 0);
	src_setup(0x25, /*prio*/9, /*vec*/0x99, 0);
	OpenPICRaiseInput(&pic, 0x24);
	OpenPICRaiseInput(&pic, 0x25);
	CHECK(OpenPICOutputAsserted(&pic));
	CHECK(rd(R_IACK) == 0x99u);               // higher priority wins
	CHECK(!OpenPICOutputAsserted(&pic));      // lower-prio 0x24 hidden in service
	wr(R_EOI, 0);                             // retire 0x99
	CHECK(pic.eois[0x25] == 1);
	CHECK(OpenPICOutputAsserted(&pic));       // EOI re-raises for pending 0x24
	CHECK(rd(R_IACK) == 0x24u);
	wr(R_EOI, 0);
	CHECK(pic.eois[0x24] == 1);
	CHECK(!OpenPICOutputAsserted(&pic));
	// Priority tie -> lowest source number wins (oracle's strict > scan)
	src_setup(0x30, 5, 0x31, 0);
	src_setup(0x31, 5, 0x32, 0);
	OpenPICRaiseInput(&pic, 0x31);
	OpenPICRaiseInput(&pic, 0x30);
	CHECK(rd(R_IACK) == 0x31u);               // source 0x30's vector
	CHECK(rd(R_IACK) == 0x32u);
	wr(R_EOI, 0);
	wr(R_EOI, 0);

	// =====================================================================
	// F. Nested preemption: higher priority preempts lower in-service
	// =====================================================================
	fresh();
	wr(R_CTPR, 0);
	src_setup(0x05, /*prio*/2, /*vec*/0x11, 0);
	src_setup(0x06, /*prio*/9, /*vec*/0x22, 0);
	OpenPICRaiseInput(&pic, 0x05);
	CHECK(rd(R_IACK) == 0x11u);               // low-prio now in service
	CHECK(!OpenPICOutputAsserted(&pic));
	OpenPICRaiseInput(&pic, 0x06);
	CHECK(OpenPICOutputAsserted(&pic));       // preempts the in-service IRQ
	CHECK(rd(R_IACK) == 0x22u);
	wr(R_EOI, 0);                             // retires the HIGHEST in service
	CHECK(pic.eois[0x06] == 1 && pic.eois[0x05] == 0);
	CHECK(!OpenPICOutputAsserted(&pic));      // nothing else raised
	wr(R_EOI, 0);                             // now the lower one
	CHECK(pic.eois[0x05] == 1);
	CHECK(pic.eois_empty == 0);
	// An equal-priority raise must NOT preempt (hidden behind servicing)
	OpenPICRaiseInput(&pic, 0x05);
	CHECK(rd(R_IACK) == 0x11u);
	src_setup(0x07, 2, 0x33, 0);
	OpenPICRaiseInput(&pic, 0x07);
	CHECK(!OpenPICOutputAsserted(&pic));      // prio 2 <= servicing prio 2
	wr(R_EOI, 0);
	CHECK(OpenPICOutputAsserted(&pic));       // surfaces after retirement
	CHECK(rd(R_IACK) == 0x33u);
	wr(R_EOI, 0);

	// Oracle quirk (matched + documented): the EOI re-raise path does NOT
	// re-check CTPR — only the remaining servicing priority.
	fresh();
	wr(R_CTPR, 0);
	src_setup(0x08, /*prio*/5, /*vec*/0x50, 0);
	src_setup(0x09, /*prio*/7, /*vec*/0x70, 0);
	OpenPICRaiseInput(&pic, 0x08);
	CHECK(rd(R_IACK) == 0x50u);               // prio-5 in service
	OpenPICRaiseInput(&pic, 0x09);
	CHECK(OpenPICOutputAsserted(&pic));       // prio 7 preempts
	wr(R_CTPR, 10);                           // gate everything mid-service
	CHECK(!OpenPICOutputAsserted(&pic));
	wr(R_EOI, 0);                             // retire the prio-5 IRQ
	CHECK(OpenPICOutputAsserted(&pic));       // re-raised DESPITE 7 <= ctpr 10
	CHECK(rd(R_IACK) == 0xFFu);               // ...but IACK still gates -> bad path
	CHECK(pic.iacks_bad == 1);
	OpenPICTakePendingWarning(&pic);

	// =====================================================================
	// G. TPR (CTPR) gating
	// =====================================================================
	fresh();                                   // ctpr = 15: everything gated
	src_setup(0x10, /*prio*/5, /*vec*/0x55, 0);
	OpenPICRaiseInput(&pic, 0x10);
	CHECK(!OpenPICOutputAsserted(&pic));      // 5 <= 15: gated
	CHECK((rd(SRC_IVPR(0x10)) & OPENPIC_IVPR_ACTIVITY) != 0);   // still raised
	// IACK while gated: the oracle's "bad raised IRQ" path -> SPVE
	CHECK(rd(R_IACK) == 0xFFu);
	CHECK(pic.iacks_bad == 1 && pic.iacks[0x10] == 0);
	CHECK(OpenPICTakePendingWarning(&pic) != NULL);
	// ...and the edge source was consumed by that IACK (oracle quirk: the
	// edge-clear block runs on the bad path too)
	CHECK((rd(SRC_IVPR(0x10)) & OPENPIC_IVPR_ACTIVITY) == 0);
	CHECK(rd(R_IACK) == 0xFFu);
	CHECK(pic.iacks_spurious == 1);
	// Raise again; lowering CTPR below the priority delivers
	OpenPICRaiseInput(&pic, 0x10);
	CHECK(!OpenPICOutputAsserted(&pic));
	wr(R_CTPR, 4);
	CHECK(OpenPICOutputAsserted(&pic));       // 5 > 4
	wr(R_CTPR, 10);
	CHECK(!OpenPICOutputAsserted(&pic));      // raised back under the gate
	wr(R_CTPR, 5);
	CHECK(!OpenPICOutputAsserted(&pic));      // boundary: 5 <= 5 still gated
	wr(R_CTPR, 0);
	CHECK(OpenPICOutputAsserted(&pic));
	CHECK(rd(R_IACK) == 0x55u);
	wr(R_EOI, 0);
	// Raise-time boundary: a raise arriving while ctpr == priority is gated
	// in local_pipe itself (prio <= ctpr), not just by the CTPR write path
	wr(R_CTPR, 5);
	OpenPICRaiseInput(&pic, 0x10);
	CHECK(!OpenPICOutputAsserted(&pic));      // 5 <= 5 at raise time
	wr(R_CTPR, 4);
	CHECK(OpenPICOutputAsserted(&pic));
	CHECK(rd(R_IACK) == 0x55u);
	wr(R_EOI, 0);

	// =====================================================================
	// H. Level-sensitive source: stays pending until lowered
	// =====================================================================
	fresh();
	wr(R_CTPR, 0);
	src_setup(0x19, /*prio*/8, /*vec*/0x42, /*level*/1);
	OpenPICRaiseInput(&pic, 0x19);
	CHECK(OpenPICOutputAsserted(&pic));
	CHECK(rd(R_IACK) == 0x42u);
	CHECK(!OpenPICOutputAsserted(&pic));
	CHECK((rd(SRC_IVPR(0x19)) & OPENPIC_IVPR_ACTIVITY) != 0);   // NOT consumed
	wr(R_EOI, 0);
	CHECK(pic.eois[0x19] == 1);
	CHECK(OpenPICOutputAsserted(&pic));       // still pending -> re-asserts
	CHECK(rd(R_IACK) == 0x42u);               // delivered again
	CHECK(pic.iacks[0x19] == 2);
	OpenPICLowerInput(&pic, 0x19);            // device deasserts its line
	CHECK((rd(SRC_IVPR(0x19)) & OPENPIC_IVPR_ACTIVITY) == 0);
	wr(R_EOI, 0);
	CHECK(!OpenPICOutputAsserted(&pic));      // no re-raise after the lower
	CHECK(rd(R_IACK) == 0xFFu);

	// =====================================================================
	// I. Edge source: Lower is a no-op; redundant Raise doesn't re-fire output
	// =====================================================================
	fresh();
	wr(R_CTPR, 0);
	src_setup(0x0A, 4, 0x77, 0);
	OpenPICRaiseInput(&pic, 0x0A);
	CHECK(OpenPICOutputAsserted(&pic));
	OpenPICLowerInput(&pic, 0x0A);
	CHECK(OpenPICOutputAsserted(&pic));       // edge: lower ignored
	CHECK(pic.lowers == 1);
	out_n = 0;
	OpenPICRaiseInput(&pic, 0x0A);            // second edge while asserted
	CHECK(out_n == 0);                        // no output transition
	CHECK(rd(R_IACK) == 0x77u);
	wr(R_EOI, 0);

	// =====================================================================
	// J. Spurious vector register (SPVE)
	// =====================================================================
	fresh();
	wr(R_SPVE, 0x77u);
	CHECK(rd(R_SPVE) == 0x77u);
	CHECK(rd(R_IACK) == 0x77u);               // empty IACK returns programmed SPVE
	wr(R_SPVE, 0x1FFu);
	CHECK(rd(R_SPVE) == 0xFFu);               // masked to vector_mask

	// =====================================================================
	// K. GCR: mode field write + reset bit
	// =====================================================================
	fresh();
	wr(R_GCR, OPENPIC_GCR_MODE_MIXED);
	CHECK(rd(R_GCR) == 0x20000000u);
	wr(R_GCR, 0x40000000u);                   // outside the mode mask
	CHECK(rd(R_GCR) == 0);
	// Dirty the state, then GCR reset
	wr(R_CTPR, 0);
	wr(R_SPVE, 0x33u);
	src_setup(0x19, 8, 0x42, 0);
	OpenPICRaiseInput(&pic, 0x19);
	CHECK(rd(R_IACK) == 0x42u);
	wr(R_GCR, OPENPIC_GCR_RESET);
	CHECK(pic.gcr_resets == 1);
	CHECK(rd(R_GCR) == 0);                    // reset completes synchronously
	CHECK(rd(R_FRR) == 0x003F0002u);
	CHECK(rd(R_SPVE) == 0xFFu);
	CHECK(rd(R_CTPR) == 15);
	CHECK(rd(SRC_IVPR(0x19)) == 0xA0000000u);
	CHECK(rd(SRC_IDR(0x19)) == 0);
	CHECK(!OpenPICOutputAsserted(&pic));
	CHECK(rd(R_IACK) == 0xFFu);               // queues emptied
	CHECK(pic.iacks[0x19] == 1);              // telemetry SURVIVES a GCR reset
	CHECK(pic.first_iack_seen != 0);          // Q8 record survives too

	// =====================================================================
	// L. Per-CPU aliases in the global bank (glb+0x80..0xB0 -> CPU0)
	// =====================================================================
	fresh();
	CHECK(rd(0x80) == 15);                    // CTPR alias
	wr(0x80, 3);
	CHECK(rd(R_CTPR) == 3);                   // same register
	CHECK(rd(0x90) == 0);                     // WHOAMI alias
	CHECK(rd(0xA0) == 0xFFu);                 // IACK alias (empty -> SPVE)
	CHECK(pic.iacks_spurious == 1);
	wr(0xB0, 0);                              // EOI alias
	CHECK(pic.eois_empty == 1);

	// =====================================================================
	// M. Absent devices + unknown registers: all-ones reads, loud latch
	// =====================================================================
	fresh();
	CHECK(OpenPICTakePendingWarning(&pic) == NULL);
	// Timer bank (KEYLARGO_MAX_TMR = 0 -> absent; QEMU divergence documented)
	CHECK(rd(0x10F0) == 0xFFFFFFFFu);         // TFRR offset
	CHECK(pic.timer_touches == 1);
	CHECK(pic.warned == 1);
	{
		const char *w = OpenPICTakePendingWarning(&pic);
		CHECK(w != NULL && strstr(w, "timer") != NULL);
	}
	CHECK(OpenPICTakePendingWarning(&pic) == NULL);   // consume-once
	wr(0x1100, 5);                            // timer-bank write ignored
	CHECK(pic.timer_touches == 2);
	// IPI registers (uniprocessor -> absent): glb IPIDR alias, IPI_IVPR,
	// CPU-bank IPIDR, and the 4 IPI source-bank slots (idx 64..67)
	CHECK(rd(0x40) == 0xFFFFFFFFu);
	wr(0x10A0, 1);
	CHECK(rd(0x20040) == 0xFFFFFFFFu);
	CHECK(rd(SRC_IVPR(65)) == 0xFFFFFFFFu);
	CHECK(pic.ipi_touches == 4);
	// PIR write: CPU soft-reset side effect unmodeled, latched loud
	wr(R_PIR, 1);
	CHECK(pic.pir_writes == 1);
	CHECK(OpenPICTakePendingWarning(&pic) != NULL);
	// Unknown/holes/unaligned/bad-size accesses
	uint64_t u0 = pic.unknown_accesses;
	CHECK(rd(0x2000) == 0xFFFFFFFFu);         // hole between tmr and src
	CHECK(rd(0x10008) == 0xFFFFFFFFu);        // reserved source-bank offset
	CHECK(rd(0x10018) == 0xFFFFFFFFu);        // ILR: FSL-only register
	CHECK(rd(SRC_IVPR(70)) == 0xFFFFFFFFu);   // source index past IPI range
	CHECK(rd(0x1004) == 0xFFFFFFFFu);         // unaligned global access
	CHECK(rd(0x20084) == 0xFFFFFFFFu);        // unaligned CPU-bank access
	CHECK((uint32_t)OpenPICRead(&pic, BASE + R_FRR, 1) == 0xFFFFFFFFu);  // size != 4
	CHECK(rd(0x40000) == 0xFFFFFFFFu);        // out of span
	CHECK(rd(0x21080) == 0xFFFFFFFFu);        // CPU1 window: invalid (single CPU)
	CHECK(rd(0x1010) == 0xFFFFFFFFu);         // unknown global register
	CHECK(rd(0x200C0) == 0xFFFFFFFFu);        // unknown CPU-bank register
	CHECK(pic.unknown_accesses == u0 + 11);
	wr(0x2000, 1);                            // hole write ignored + latched
	CHECK(pic.unknown_accesses == u0 + 12);
	// None of the above disturbed live state
	CHECK(rd(R_FRR) == 0x003F0002u);
	CHECK(!OpenPICOutputAsserted(&pic));

	// =====================================================================
	// N. Q8 input numbers + first-IACK tripwire
	// =====================================================================
	CHECK(OPENPIC_IRQ_VIA_CUDA == 0x19);
	CHECK(OPENPIC_IRQ_ESCC_B == 0x24);
	CHECK(OPENPIC_IRQ_ESCC_A == 0x25);
	CHECK(SRC_IVPR(OPENPIC_IRQ_VIA_CUDA) == 0x10320u);   // pinned guest offsets
	CHECK(SRC_IVPR(OPENPIC_IRQ_ESCC_B) == 0x10480u);
	CHECK(SRC_IVPR(OPENPIC_IRQ_ESCC_A) == 0x104A0u);
	fresh();
	wr(R_CTPR, 0);
	src_setup(OPENPIC_IRQ_VIA_CUDA, 5, 0x1A, 0);
	src_setup(OPENPIC_IRQ_ESCC_B, 6, 0x2B, 0);
	src_setup(OPENPIC_IRQ_ESCC_A, 7, 0x3C, 0);
	OpenPICRaiseInput(&pic, OPENPIC_IRQ_VIA_CUDA);
	OpenPICRaiseInput(&pic, OPENPIC_IRQ_ESCC_B);
	OpenPICRaiseInput(&pic, OPENPIC_IRQ_ESCC_A);
	CHECK(rd(R_IACK) == 0x3Cu);               // ESCC A: priority 7
	CHECK(rd(R_IACK) == 0x2Bu);               // ESCC B: priority 6
	CHECK(rd(R_IACK) == 0x1Au);               // VIA:    priority 5
	wr(R_EOI, 0); wr(R_EOI, 0); wr(R_EOI, 0);
	CHECK(pic.eois[0x19] == 1 && pic.eois[0x24] == 1 && pic.eois[0x25] == 1);
	CHECK(OpenPICFormatFirstIACKs(&pic, buf, sizeof(buf)) > 0);
	CHECK(strstr(buf, "src=0x19 vec=0x1a") != NULL);
	CHECK(strstr(buf, "src=0x24 vec=0x2b") != NULL);
	CHECK(strstr(buf, "src=0x25 vec=0x3c") != NULL);
	// The record keeps the FIRST vector even if the guest reprograms it
	wr(SRC_IVPR(0x19), (5u << 16) | 0x88u);
	OpenPICRaiseInput(&pic, 0x19);
	CHECK(rd(R_IACK) == 0x88u);
	wr(R_EOI, 0);
	CHECK(pic.first_iack_vec[0x19] == 0x1A);
	CHECK(pic.iacks[0x19] == 2);

	// =====================================================================
	// O. Out-of-range inputs: counted, latched, no abort
	// =====================================================================
	fresh();
	OpenPICRaiseInput(&pic, 64);              // first IPI input number
	OpenPICRaiseInput(&pic, 200);
	OpenPICLowerInput(&pic, 64);
	CHECK(pic.bad_inputs == 3);
	CHECK(OpenPICTakePendingWarning(&pic) != NULL);
	CHECK(!OpenPICOutputAsserted(&pic));

	// =====================================================================
	// P. Output callback seam (the future EXC_EXTERNAL wiring point)
	// =====================================================================
	fresh();
	wr(R_CTPR, 0);
	src_setup(0x19, 8, 0x42, 0);
	out_n = 0;
	OpenPICRaiseInput(&pic, 0x19);
	CHECK(out_n == 1 && out_events[0] == 1);  // asserted edge
	CHECK(rd(R_IACK) == 0x42u);
	CHECK(out_n == 2 && out_events[1] == 0);  // deasserted edge on IACK
	wr(R_EOI, 0);
	CHECK(out_n == 2);                        // no spurious transition on EOI
	CHECK(pic.out_raises == 1 && pic.out_lowers == 1);

	// =====================================================================
	// Q. IDR semantics: no target -> no delivery; IDR write alone no update
	// =====================================================================
	fresh();
	wr(R_CTPR, 0);
	wr(SRC_IVPR(0x30), (5u << 16) | 0x33u);   // unmasked, but IDR still 0
	OpenPICRaiseInput(&pic, 0x30);
	CHECK(!OpenPICOutputAsserted(&pic));      // no target
	CHECK((rd(SRC_IVPR(0x30)) & OPENPIC_IVPR_ACTIVITY) != 0);   // oracle quirk
	CHECK(rd(R_IACK) == 0xFFu);               // never entered the raised queue
	wr(SRC_IDR(0x30), 1);
	CHECK(!OpenPICOutputAsserted(&pic));      // IDR write does NOT update (oracle)
	OpenPICRaiseInput(&pic, 0x30);            // next edge delivers
	CHECK(OpenPICOutputAsserted(&pic));
	CHECK(rd(R_IACK) == 0x33u);
	wr(R_EOI, 0);

	// =====================================================================
	// R. Stats formatting
	// =====================================================================
	fresh();
	wr(R_CTPR, 0);
	src_setup(0x19, 8, 0x42, 0);
	OpenPICRaiseInput(&pic, 0x19);
	CHECK(rd(R_IACK) == 0x42u);
	wr(R_EOI, 0);
	rd(0x10F0);                               // one timer touch
	CHECK(OpenPICFormatStats(&pic, buf, sizeof(buf)) > 0);
	CHECK(strstr(buf, "raises=1") != NULL);
	CHECK(strstr(buf, "iacks=1/1") != NULL);
	CHECK(strstr(buf, "spurious=0") != NULL);
	CHECK(strstr(buf, "eois=1") != NULL);
	CHECK(strstr(buf, "out=0") != NULL);
	CHECK(strstr(buf, "tmr=1") != NULL);
	CHECK(strstr(buf, "bad_inputs=0") != NULL);
	CHECK(OpenPICFormatFirstIACKs(&pic, buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "src=0x19 vec=0x42") == 0);
	// No-IACK-yet instance formats nothing
	{
		OpenPICDevice p2;
		OpenPICReset(&p2, BASE);
		buf[0] = 'x';
		CHECK(OpenPICFormatFirstIACKs(&p2, buf, sizeof(buf)) == 0);
	}

	printf("test_dev_openpic: RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
