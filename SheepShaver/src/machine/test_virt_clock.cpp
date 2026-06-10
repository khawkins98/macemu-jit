/* Standalone unit test for virt_clock.cpp. Simulates the check_work DEC deadline
 * math (SPIKE-S3 §2.4) as the consumer contract. Build: make -C SheepShaver/src/machine test */
#include "virt_clock.h"
#include <assert.h>
#include <string.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

static uint64_t fake_ns = 0;
static uint64_t fake_now(void *) { return fake_ns; }

static uint64_t armed_ns; static uint32_t armed_gen; static int arm_calls = 0;
static void fake_arm(void *, uint64_t ns, uint32_t gen) { armed_ns = ns; armed_gen = gen; arm_calls++; }

int main()
{
	VirtClock c;
	memset(&c, 0xAA, sizeof(c));            // poison: Init must fully reset
	VirtClockInit(&c, 25000000u, fake_now, 0);   // 25 MHz TB (TimebaseSpeed default)
	CHECK(VirtClockReady(&c));

	// --- TB ratio math: 1 second of ns = 25e6 ticks; large values don't overflow ---
	fake_ns = 1000000000ull;                              CHECK(VirtClockTB(&c) == 25000000ull);
	fake_ns = 1000000000000000ull;  /* ~11.6 days */      CHECK(VirtClockTB(&c) == 25000000000000ull);
	fake_ns = 0;

	// --- Cold-state DEC == M1 synthetic down-counter: 0 - TB ---
	fake_ns = 40;                            // 40 ns @ 25 MHz = 1 tick
	CHECK(VirtClockReadDEC(&c) == 0xFFFFFFFFu);           // 0 - 1
	fake_ns = 4000;                          // 100 ticks
	CHECK(VirtClockReadDEC(&c) == (uint32_t)(0u - 100u));
	CHECK(c.mfspr_dec_reads == 2);

	// --- mtspr DEC: countdown from the written value ---
	VirtClockWriteDEC(&c, 1000);
	CHECK(c.mtspr_dec_writes == 1);
	CHECK(arm_calls == 0);                   // no hook wired yet -> no arm
	CHECK(VirtClockReadDEC(&c) == 1000);
	fake_ns += 400 * 40;                     // +400 ticks
	CHECK(VirtClockReadDEC(&c) == 600);
	// Expiry: condition latches when the counter crosses below zero (elapsed > value)
	CHECK(!VirtClockDECPending(&c));
	fake_ns += 601 * 40;                     // elapsed = 1001 > 1000
	uint32_t d = VirtClockReadDEC(&c);
	CHECK(d == 0xFFFFFFFFu);                 // wrapped: 1000 - 1001
	CHECK(VirtClockDECPending(&c));
	CHECK(c.dec_expiries == 1);
	// One-shot per write: further reads do not re-latch / re-count
	fake_ns += 40;
	(void)VirtClockReadDEC(&c);
	CHECK(c.dec_expiries == 1);
	VirtClockClearDECPending(&c);
	CHECK(!VirtClockDECPending(&c));

	// --- Eager hook + generation guard ---
	c.on_dec_write = fake_arm; c.cb_opaque = 0;
	VirtClockWriteDEC(&c, 250);              // 250 ticks @25MHz = 10000 ns
	CHECK(arm_calls == 1 && armed_ns == (uint64_t)(250 + 1) * 40);   // expiry at v+1 ticks
	uint32_t gen1 = armed_gen;
	VirtClockWriteDEC(&c, 500);              // re-write: new generation
	CHECK(arm_calls == 2 && armed_gen == gen1 + 1);
	VirtClockDECExpire(&c, gen1);            // stale event: must no-op
	CHECK(!VirtClockDECPending(&c) && c.dec_expiries == 1);
	VirtClockDECExpire(&c, armed_gen);       // current event: latches
	CHECK(VirtClockDECPending(&c) && c.dec_expiries == 2);
	VirtClockDECExpire(&c, armed_gen);       // double-fire: no-op (arm consumed)
	CHECK(c.dec_expiries == 2);
	VirtClockClearDECPending(&c);

	// --- mttbl/mttbu: TB continues from the written value ---
	fake_ns = 1000000000ull;                 // raw TB = 25e6
	VirtClockWriteTBU(&c, 0);
	VirtClockWriteTBL(&c, 0x1000);           // guest sets TB = 0x1000
	CHECK(VirtClockTB(&c) == 0x1000);
	fake_ns += 40;                           // +1 tick
	CHECK(VirtClockTB(&c) == 0x1001);
	VirtClockWriteTBU(&c, 2);                // TBU write preserves current TBL
	CHECK((VirtClockTB(&c) >> 32) == 2 && (uint32_t)VirtClockTB(&c) == 0x1001);
	CHECK(c.tb_writes == 3);

	// --- The S3 §2.4 check_work consumer simulation (deadline = DEC - timeout) ---
	VirtClockWriteDEC(&c, 0);                // worst case: DEC just wrapped/cold-like
	uint32_t timeout = 0x8000;               // [KDP-0x438] >> 8 scale, representative
	uint32_t deadline = VirtClockReadDEC(&c) - timeout;
	int iters = 0;
	for (;;) {
		uint32_t now = VirtClockReadDEC(&c);
		if ((int32_t)(now - deadline) <= 0) break;   // subf. + ble
		fake_ns += 40 * 1024;                // 1024 ticks per "loop pass"
		if (++iters > 100000) break;
	}
	CHECK(iters > 0 && iters <= (int)(timeout / 1024) + 2);   // terminated by deadline, not the guard

	VirtClockDumpStats(&c, stdout);
	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
