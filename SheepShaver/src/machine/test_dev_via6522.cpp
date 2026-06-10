/* Drives the exact 68k STM timeout-helper sequence (SPIKE-S3 §1.5) under a fake clock. */
#include "dev_via6522.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

static uint64_t fake_now = 0;
static uint64_t fake_clock(void *) { return fake_now; }

static VIA6522 via;
#define BASE 0xF3016000u
static uint8_t rd(uint32_t off)            { return (uint8_t)VIARead(&via, BASE + off, 1); }
static void    wr(uint32_t off, uint8_t v) { VIAWrite(&via, BASE + off, 1, v); }

int main()
{
	VIAReset(&via, BASE, fake_clock, 0);

	// --- STM timeout helper sequence (S3 §1.5, offsets are reg# * 0x200) ---
	wr(0x0600, rd(0x0600) | 0x08);   // DDRA: PA3 output
	wr(0x1e00, rd(0x1e00) & ~0x08);  // ORA no-handshake: PA3 low
	wr(0x1600, 0x00);                // ACR = 0: one-shot T2, SR off
	wr(0x1c00, 0x20);                // IER <- 0x20: bit7 clear => DISABLE T2 interrupt
	CHECK((rd(0x1c00) & 0x20) == 0); // IER read: bit not enabled (read returns 0x80|IER)
	wr(0x1000, 0xFF);                // T2C-L latch
	wr(0x1200, 0xFF);                // T2C-H: load 0xFFFF, clear IFR.5, start countdown
	CHECK((rd(0x1a00) & 0x20) == 0); // armed: T2 not yet expired
	fake_now += 0x10000;             // > 0xFFFF VIA ticks elapse
	CHECK((rd(0x1a00) & 0x20) == 0x20);  // IFR bit 5 set: T2 timeout fired
	// Write-1-to-clear:
	wr(0x1a00, 0x20);
	CHECK((rd(0x1a00) & 0x20) == 0);

	// IFR bit 7 master: set only when (IFR & IER & 0x7F) != 0.
	wr(0x1c00, 0xA0);                // IER: bit7 set => ENABLE T2
	wr(0x1000, 0x10); wr(0x1200, 0x00);  // T2 = 0x0010
	fake_now += 0x20;
	uint8_t ifr = rd(0x1a00);
	CHECK((ifr & 0x20) && (ifr & 0x80));

	// T2C reads return the live decrementing count (low/high bytes).
	wr(0x1000, 0x00); wr(0x1200, 0x01);  // T2 = 0x0100
	fake_now += 0x40;
	CHECK(rd(0x1000) == 0xC0 && rd(0x1200) == 0x00);  // 0x0100-0x40 = 0x00C0

	// Stored-state registers round-trip.
	CHECK(VIATakePendingWarning(&via) == 0);                // no Cuda touch yet -> no pending warning
	wr(0x0000, 0x18); CHECK((rd(0x0000) & 0x18) == 0x18);   // ORB bits 3/4 (Cuda handshake)
	CHECK(via.cuda_touches > 0);                            // handshake bits counted as Cuda touches
	// Loud stub is latched (no stdio on the fault path) and retrievable exactly once:
	const char *warn = VIATakePendingWarning(&via);
	CHECK(warn != 0 && strstr(warn, "ORB") != 0);
	CHECK(VIATakePendingWarning(&via) == 0);                // take-and-clear: second call empty
	wr(0x1400, 0x55); CHECK(rd(0x1400) == 0x55);            // SR stored
	CHECK(via.cuda_touches >= 3);                           // SR write + read counted
	CHECK(VIATakePendingWarning(&via) == 0);                // one-shot: later touches don't re-latch

	// T1 latches round-trip (regs 6/7 store without starting the counter).
	wr(0x0c00, 0x34); CHECK(rd(0x0c00) == 0x34);            // T1L-L
	wr(0x0e00, 0x12); CHECK(rd(0x0e00) == 0x12);            // T1L-H
	CHECK(!via.t1_running);                                 // latch writes never arm T1

	// T1 minimal: load via T1C-H, IFR bit 6 after expiry.
	wr(0x0800, 0x10); wr(0x0a00, 0x00);
	fake_now += 0x20;
	CHECK((rd(0x1a00) & 0x40) == 0x40);

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
