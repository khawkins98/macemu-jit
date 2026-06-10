/* Standalone unit test for the DingusPPC TimerManager port. Fake time source;
 * asserts ordering, one-shot/cyclic semantics, drift correction, cancel, and the
 * no-lock-during-callback contract (cancel/add from inside a callback). */
#include "event_sched.h"
#include <assert.h>
#include <stdio.h>
#include <vector>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

static uint64_t fake_ns = 0;

int main()
{
	EventScheduler es;
	es.set_time_now_cb([]() { return fake_ns; });
	int notifies = 0;
	es.set_notify_changes_cb([&]() { notifies++; });

	// Empty queue: slice 0.
	CHECK(es.process_timers() == 0);

	// --- Ordering + one-shot fires exactly once ---
	std::vector<int> fired;
	es.add_oneshot_timer(300, [&]() { fired.push_back(3); });
	es.add_oneshot_timer(100, [&]() { fired.push_back(1); });
	es.add_oneshot_timer(200, [&]() { fired.push_back(2); });
	CHECK(notifies == 3);
	uint64_t slice = es.process_timers();
	CHECK(fired.empty() && slice == 100);     // nothing expired; next expiry in 100ns
	fake_ns = 250;
	slice = es.process_timers();
	CHECK(fired.size() == 2 && fired[0] == 1 && fired[1] == 2);
	CHECK(slice == 50);                       // 300 - 250
	fake_ns = 300;
	CHECK(es.process_timers() == 0);          // last one fired; queue empty
	CHECK(fired.size() == 3 && fired[2] == 3);
	fake_ns = 400;
	es.process_timers();
	CHECK(fired.size() == 3);                 // one-shots never re-fire

	// --- Cyclic timer with drift correction (donor: timeout_ns_new <= now -> now+interval) ---
	fake_ns = 0;
	int cyc = 0;
	uint32_t cid = es.add_cyclic_timer(100, [&]() { cyc++; });
	fake_ns = 100; es.process_timers(); CHECK(cyc == 1);
	fake_ns = 200; es.process_timers(); CHECK(cyc == 2);
	fake_ns = 550; es.process_timers();       // missed beats: fires once, re-arms at now+interval
	CHECK(cyc == 3);
	uint64_t s2 = es.process_timers();
	CHECK(s2 == 100);                          // next at 650, not at 400
	es.cancel_timer(cid);
	fake_ns = 1000; es.process_timers(); CHECK(cyc == 3);

	// --- cancel by id: middle FIRST (rev 2 finding S8: cancelling the head first
	// would promote b to head and never exercise remove_by_id's erase/make_heap path) ---
	fired.clear();
	uint32_t a = es.add_oneshot_timer(100, [&]() { fired.push_back(10); });
	uint32_t b = es.add_oneshot_timer(200, [&]() { fired.push_back(20); });
	uint32_t cd = es.add_oneshot_timer(300, [&]() { fired.push_back(30); });
	(void)cd;
	es.cancel_timer(b);                        // middle while a is head (erase + make_heap)
	es.cancel_timer(a);                        // head (top-pop branch)
	fake_ns += 1000; es.process_timers();
	CHECK(fired.size() == 1 && fired[0] == 30);

	// --- add/cancel from INSIDE a callback (recursive_mutex + no-lock-during-cb) ---
	fired.clear();
	es.add_oneshot_timer(10, [&]() {
		es.add_oneshot_timer(5, [&]() { fired.push_back(99); });
	});
	fake_ns += 10; es.process_timers();        // outer fires, schedules inner at +5
	fake_ns += 5;  es.process_timers();
	CHECK(fired.size() == 1 && fired[0] == 99);

	// --- immediate timer fires on the next process_timers ---
	int imm = 0;
	es.add_immediate_timer([&]() { imm++; });
	es.process_timers();
	CHECK(imm == 1);

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
