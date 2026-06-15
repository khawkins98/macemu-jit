/* SS_M18 S3 T3 (Operation NewSheep) — gated-ON STATIC assertion for the
 * synthetic-supervisor scheduler-yield half of the retirement (the part that is
 * feasibly unit-testable without a live NK boot, per G(T3)).
 *
 * Asserts the two pure-module yield mechanisms the gated glue path flips under
 * NkSupervisorEnabled():
 *   1. VirtClockSuppressHostDEC(true)  => VirtClockWriteDEC stops arming the host
 *      on_dec_write hook, while STILL updating the virtual clock (the NK reads DEC
 *      via mfspr). Default OFF is byte-identical (the hook still fires).
 *   2. EventSchedulerYield(true)       => process_timers() runs no callbacks and
 *      returns the idle slice (0). Default OFF is byte-identical (callbacks fire).
 *
 * NOT covered here (no standalone harness; verified structurally + by the
 * byte-identity gate): the SheepExcDeliverPending() call-site bypass in
 * ppc-cpu.cpp check_spcflags and the g_exc_entry_table DEC/EXT nulling in
 * sheepshaver_glue.cpp init_emul_ppc — both live in TUs that need the whole CPU.
 * The gated-ON "SS-scheduler-ticks=0 while NK live" observable is DEFERRED to
 * T5/GREEN-PASS (no live NK install until S2b). Build: make -C .../machine test */
#include "virt_clock.h"
#include "event_sched.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

static uint64_t fake_ns = 0;
static uint64_t fake_now(void *) { return fake_ns; }

static int arm_calls = 0;
static void fake_arm(void *, uint64_t, uint32_t) { arm_calls++; }

static void test_virt_clock_suppress()
{
	VirtClock c;
	VirtClockInit(&c, 25000000u, fake_now, 0);
	c.on_dec_write = fake_arm; c.cb_opaque = 0;

	// --- default OFF: byte-identical legacy behavior — the hook arms ---
	arm_calls = 0;
	VirtClockWriteDEC(&c, 1000);
	CHECK(arm_calls == 1);                       // host hook armed (legacy)
	CHECK(c.dec_set_value == 1000);              // clock state updated

	// --- gated ON: host arming retired, clock state STILL updates ---
	VirtClockSuppressHostDEC(true);
	arm_calls = 0;
	fake_ns = 100000;
	VirtClockWriteDEC(&c, 500);
	CHECK(arm_calls == 0);                        // RETIRED: no host arm
	CHECK(c.dec_set_value == 500);                // NK still reads a live DEC
	CHECK(c.mtspr_dec_writes == 2);               // the write still counted/applied
	// the lazy expiry path still works (the NK's own DEC source)
	fake_ns += 501 * 40;                          // elapsed > 500
	(void)VirtClockReadDEC(&c);
	CHECK(VirtClockDECPending(&c));               // advisory pending still latches

	// --- restore default (so the flag does not leak across tests) ---
	VirtClockSuppressHostDEC(false);
	arm_calls = 0;
	VirtClockWriteDEC(&c, 250);
	CHECK(arm_calls == 1);                         // back to legacy: hook arms again
}

static int cb_fired = 0;
static void yield_test_cb() { cb_fired++; }

static void test_event_sched_yield()
{
	EventScheduler es;
	es.set_time_now_cb([]() -> uint64_t { return fake_ns; });
	es.set_notify_changes_cb([]() {});

	// --- default OFF: byte-identical — an expired timer fires ---
	fake_ns = 0;
	cb_fired = 0;
	es.add_oneshot_timer(100, yield_test_cb);
	fake_ns = 1000;
	uint64_t slice = es.process_timers();
	CHECK(cb_fired == 1);                          // legacy: callback ran
	CHECK(slice == 0);                             // queue now empty

	// --- gated ON: pump quiescent — no callback, idle slice ---
	EventSchedulerYield(true);
	cb_fired = 0;
	es.add_oneshot_timer(100, yield_test_cb);      // added at now=1000 -> due at 1100
	fake_ns = 2000;                                // now PAST due (1100)
	slice = es.process_timers();
	CHECK(cb_fired == 0);                          // RETIRED: pump fired nothing
	CHECK(slice == 0);                             // idle slice (pump caps to 10ms)

	// --- restore default: the still-queued due timer fires once the pump resumes ---
	EventSchedulerYield(false);
	cb_fired = 0;
	(void)es.process_timers();                     // fake_ns=2000 > 1100 -> fires
	CHECK(cb_fired == 1);                          // back to legacy: callback ran
}

int main()
{
	test_virt_clock_suppress();
	test_event_sched_yield();
	printf("test_nk_supervisor_yield: %d checks passed\n", n_pass);
	return 0;
}
