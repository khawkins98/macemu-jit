/* Conformance vectors from SPIKE-S3-STALL-DEVICE-PROBE.md §1.3 (68k STM) and §2.3 (check_work). */
#include "dev_scc8530.h"
#include <assert.h>
#include <stdio.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

static SCC8530 scc;
#define BASE 0xF3012000u
static void ctlw_a(uint8_t v) { SCCWrite(&scc, BASE + 2, 1, v); }
static uint8_t ctlr_a(void)   { return (uint8_t)SCCRead(&scc, BASE + 2, 1); }
static void wr_pair(uint8_t reg, uint8_t val) { ctlw_a(reg); ctlw_a(val); }

int main()
{
	SCCReset(&scc, BASE);

	// --- Vector 1: STM init table (S3 §1.3), prefixed by the ch-B control touch ---
	(void)SCCRead(&scc, BASE + 0, 1);          // tst.b (a3,d3.l): ch B ctrl read must not blow up
	static const uint8_t stm[][2] = { {9,0xC0},{15,0},{4,0x4C},{11,0x50},{14,0},
	                                  {12,0x04},{13,0},{14,1},{10,0},{3,0xC1},{5,0xEA},{1,0} };
	for (unsigned i = 0; i < sizeof(stm)/sizeof(stm[0]); i++) wr_pair(stm[i][0], stm[i][1]);
	(void)SCCRead(&scc, BASE + 6, 1);          // Rx flush read of ch A data
	CHECK(scc.wr[SCC_CH_A][15] == 0x00 && scc.wr[SCC_CH_A][4] == 0x4C);
	CHECK(scc.wr[SCC_CH_A][11] == 0x50 && scc.wr[SCC_CH_A][12] == 0x04);
	CHECK(scc.wr[SCC_CH_A][14] == 0x01 && scc.wr[SCC_CH_A][3] == 0xC1);
	CHECK(scc.wr[SCC_CH_A][5] == 0xEA && scc.wr[SCC_CH_A][1] == 0x00);
	CHECK(scc.wr_writes >= 12);

	// --- Poll loop behavior (S3 §1.4): RR0 bit0=0 (no Rx), bit2=1 (Tx empty) ---
	uint8_t rr0 = ctlr_a();
	CHECK((rr0 & 0x01) == 0 && (rr0 & 0x04) == 0x04);
	CHECK(SCCReadIsIdle(&scc, BASE + 2, rr0));         // idle hint fires on "no char"
	// WR0 <- 1 points at RR1; next ctrl read returns RR1 then resets pointer.
	ctlw_a(1);
	uint8_t rr1 = ctlr_a();
	CHECK((rr1 & 0x01) == 0x01);                       // All Sent = 1
	CHECK((rr1 & 0x70) == 0);                          // error bits 4-6 = 0
	CHECK((ctlr_a() & 0x04) == 0x04);                  // pointer reset: back to RR0
	// WR0 <- 0x30 Error Reset command: pointer stays 0, no state damage.
	ctlw_a(0x30);
	CHECK((ctlr_a() & 0x04) == 0x04);

	// --- Vector 2: check_work init (S3 §2.3), each pair prefixed by a pointer-reset read ---
	SCCReset(&scc, BASE);
	static const uint8_t ck[][2] = { {9,0x80},{4,0x48},{3,0xC0},{5,0x60},{9,0},{10,0},
	                                 {11,0x50},{12,0x0C},{13,0},{14,0x01},{3,0xC1},{5,0xEA} };
	for (unsigned i = 0; i < sizeof(ck)/sizeof(ck[0]); i++) { (void)ctlr_a(); wr_pair(ck[i][0], ck[i][1]); }
	CHECK(scc.wr[SCC_CH_A][4] == 0x48 && scc.wr[SCC_CH_A][12] == 0x0C);
	CHECK(scc.wr[SCC_CH_A][3] == 0xC1 && scc.wr[SCC_CH_A][5] == 0xEA);
	// WR9=0xC0 (hw reset, vector 1) and WR9=0x80 (ch A reset, vector 2) must clear ch A WRs:
	wr_pair(3, 0xC1); wr_pair(9, 0x80);
	CHECK(scc.wr[SCC_CH_A][3] == 0x00);

	// Tx poll + write (S3 §2.2): RR0 bit2 spin terminates immediately; stb +6 counted.
	CHECK((ctlr_a() & 0x04) == 0x04);
	SCCWrite(&scc, BASE + 6, 1, 'K');
	CHECK(scc.tx_bytes == 1);
	// Drain/timeout (S3 §2.4): WR0<-1 then RR1 bit0 All Sent = 1.
	ctlw_a(1); CHECK((ctlr_a() & 0x01) == 0x01);

	// Data reads return 0 with empty Rx; ch B data tolerated.
	CHECK(SCCRead(&scc, BASE + 6, 1) == 0 && SCCRead(&scc, BASE + 4, 1) == 0);
	CHECK(scc.rr0_polls > 0);

	// --- SCCInjectRx: Rx queue injection ---
	SCCReset(&scc, BASE);

	// Inject 2 bytes on ch A; RR0 bit0 must be 1 immediately.
	SCCInjectRx(&scc, SCC_CH_A, 'H');
	SCCInjectRx(&scc, SCC_CH_A, 'i');
	uint8_t rr0_rx = ctlr_a();
	CHECK((rr0_rx & 0x01) == 0x01);           // bit0: Rx character available
	CHECK((rr0_rx & 0x04) == 0x04);           // bit2: Tx buffer empty still set
	CHECK(!SCCReadIsIdle(&scc, BASE + 2, rr0_rx)); // NOT idle when char waiting

	// Pop first byte via data read (+6 = ch A data).
	uint8_t b0 = (uint8_t)SCCRead(&scc, BASE + 6, 1);
	CHECK(b0 == 'H');
	CHECK(scc.rx_consumed == 1);

	// Second byte still queued: RR0 bit0 still 1.
	CHECK((ctlr_a() & 0x01) == 0x01);

	// Pop second byte.
	uint8_t b1 = (uint8_t)SCCRead(&scc, BASE + 6, 1);
	CHECK(b1 == 'i');

	// Queue now empty: RR0 bit0 = 0 again; data read returns 0.
	uint8_t rr0_empty = ctlr_a();
	CHECK((rr0_empty & 0x01) == 0x00);
	CHECK(SCCReadIsIdle(&scc, BASE + 2, rr0_empty));
	CHECK(SCCRead(&scc, BASE + 6, 1) == 0);
	CHECK(scc.rx_injected == 2 && scc.rx_consumed == 2);

	// Queue-full drop: fill to SCC_RX_QUEUE_MAX, then one more is dropped.
	SCCReset(&scc, BASE);
	for (int i = 0; i < SCC_RX_QUEUE_MAX; i++) SCCInjectRx(&scc, SCC_CH_A, (uint8_t)i);
	CHECK(scc.rx_count[SCC_CH_A] == SCC_RX_QUEUE_MAX);
	SCCInjectRx(&scc, SCC_CH_A, 0xFF);       // should drop
	CHECK(scc.rx_dropped == 1);
	CHECK(scc.rx_count[SCC_CH_A] == SCC_RX_QUEUE_MAX);

	// Ch B is independent: injecting on B does not affect ch A's queue.
	SCCReset(&scc, BASE);
	SCCInjectRx(&scc, SCC_CH_B, 'X');
	CHECK((ctlr_a() & 0x01) == 0x00);        // ch A still empty
	CHECK(scc.rx_count[SCC_CH_B] == 1);
	// Ch B data read (+4 = ch B data) pops the byte.
	uint8_t bx = (uint8_t)SCCRead(&scc, BASE + 4, 1);
	CHECK(bx == 'X');
	CHECK(scc.rx_count[SCC_CH_B] == 0);

	// --- Wave-2 W2-3 (rev 2 F4): interrupt-condition state ---------------------
	// Predicate: Rx-available ∧ WR1 Rx-int-enable (0x18) ∧ WR9 MIE (0x08).
	// Seam fires on per-channel TRANSITIONS only.
	{
		static int edges_n = 0;
		static int edge_ch[16];
		static bool edge_lvl[16];
		struct Seam {
			static void fn(void *, int ch, bool asserted) {
				if (edges_n < 16) { edge_ch[edges_n] = ch; edge_lvl[edges_n] = asserted; }
				edges_n++;
			}
		};

		SCCReset(&scc, BASE);
		SCCBindIRQOutput(&scc, Seam::fn, 0);

		// Disabled chip: data alone asserts nothing (WR1=0, WR9 MIE=0).
		SCCInjectRx(&scc, SCC_CH_A, 0x0D);
		CHECK(!SCCIRQCondition(&scc, SCC_CH_A));
		CHECK(edges_n == 0);

		// WR1 Rx-int-on-all (0x10) alone: still gated by MIE.
		wr_pair(1, 0x10);
		CHECK(!SCCIRQCondition(&scc, SCC_CH_A));
		CHECK(edges_n == 0);

		// WR9 MIE: the last gate opens -> assert edge on ch A.
		wr_pair(9, 0x08);
		CHECK(SCCIRQCondition(&scc, SCC_CH_A));
		CHECK(edges_n == 1 && edge_ch[0] == SCC_CH_A && edge_lvl[0]);
		CHECK(scc.irq_raises[SCC_CH_A] == 1);

		// Second byte: level held, NO second edge (transitions only).
		SCCInjectRx(&scc, SCC_CH_A, 0x0A);
		CHECK(edges_n == 1);

		// Drain one byte: still non-empty -> level held, no edge.
		(void)SCCRead(&scc, BASE + 6, 1);
		CHECK(edges_n == 1);
		// Drain the last byte -> deassert edge.
		(void)SCCRead(&scc, BASE + 6, 1);
		CHECK(!SCCIRQCondition(&scc, SCC_CH_A));
		CHECK(edges_n == 2 && edge_ch[1] == SCC_CH_A && !edge_lvl[1]);
		CHECK(scc.irq_lowers[SCC_CH_A] == 1);

		// Re-assert via enqueue, then disable at WR1 -> deassert edge.
		SCCInjectRx(&scc, SCC_CH_A, 0x33);
		CHECK(edges_n == 3 && edge_lvl[2]);
		wr_pair(1, 0x00);
		CHECK(!SCCIRQCondition(&scc, SCC_CH_A));
		CHECK(edges_n == 4 && !edge_lvl[3]);

		// WR1 Rx-int-first-char (0x08) also counts as enabled.
		wr_pair(1, 0x08);
		CHECK(SCCIRQCondition(&scc, SCC_CH_A));
		CHECK(edges_n == 5 && edge_lvl[4]);

		// MIE off (WR9 write w/o bit 3) -> deassert.
		wr_pair(9, 0x00);
		CHECK(!SCCIRQCondition(&scc, SCC_CH_A));
		CHECK(edges_n == 6 && !edge_lvl[5]);

		// WR9 is CHIP-WIDE: enabling MIE through the ch B port gates ch A too.
		SCCWrite(&scc, BASE + 0, 1, 9);          // ch B ctrl: point at WR9
		SCCWrite(&scc, BASE + 0, 1, 0x08);       // MIE on, via ch B
		CHECK(SCCIRQCondition(&scc, SCC_CH_A));
		CHECK(edges_n == 7 && edge_ch[6] == SCC_CH_A && edge_lvl[6]);

		// Channels independent: ch B asserts its own line once enabled.
		SCCWrite(&scc, BASE + 0, 1, 1);          // ch B WR1
		SCCWrite(&scc, BASE + 0, 1, 0x10);       //   Rx-int-on-all
		SCCInjectRx(&scc, SCC_CH_B, 'Y');
		CHECK(SCCIRQCondition(&scc, SCC_CH_B));
		CHECK(edges_n == 8 && edge_ch[7] == SCC_CH_B && edge_lvl[7]);

		// Force-hardware-reset (WR9 cmd 0xC0) clears MIE -> both lines drop.
		wr_pair(9, 0xC0);
		CHECK(!SCCIRQCondition(&scc, SCC_CH_A) && !SCCIRQCondition(&scc, SCC_CH_B));
		CHECK(edges_n == 10);                    // two deassert edges

		// SCCReset clears the binding (Reset-then-Bind order contract).
		SCCReset(&scc, BASE);
		CHECK(scc.irq_fn == 0 && scc.irq_cond[0] == 0 && scc.irq_cond[1] == 0);
	}

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
