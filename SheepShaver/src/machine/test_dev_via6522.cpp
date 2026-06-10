/* Drives the 68k STM timeout-helper sequence (SPIKE-S3 §1.5) under a fake clock,
 * plus the M2 additions: N+1 expiry (N7), T2CL-read IFR acknowledge (N6), IFR
 * write-1-clear without timer disarm (N8), T1CL-read ack (S9), and eager IFR
 * latch via a bound EventScheduler with a fake time source. */
#include "dev_via6522.h"
#include "event_sched.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

static uint64_t fake_ticks = 0;                       // VIA ticks (783360 Hz)
static uint64_t fake_clock(void *) { return fake_ticks; }
static uint64_t stepping_clock(void *) { return fake_ticks++; }  // +1 tick per read (TOCTOU probe)
static uint64_t fake_sched_ns() {                     // scheduler sees the same instant in ns
	return (uint64_t)((unsigned __int128)fake_ticks * 1000000000u / VIA_CLOCK_HZ);
}
static bool direct_call(uint32_t, void (*fn)(void *), void *op) { fn(op); return true; }

static VIA6522 via;
#define BASE 0xF3016000u
static uint8_t rd(uint32_t off)            { return (uint8_t)VIARead(&via, BASE + off, 1); }
static void    wr(uint32_t off, uint8_t v) { VIAWrite(&via, BASE + off, 1, v); }

int main()
{
	VIAReset(&via, BASE, fake_clock, 0);

	// --- STM timeout helper sequence (S3 §1.5), lazy mode (no scheduler bound) ---
	wr(0x0600, rd(0x0600) | 0x08);   // DDRA: PA3 output
	wr(0x1e00, rd(0x1e00) & ~0x08);  // ORA no-handshake: PA3 low
	wr(0x1600, 0x00);                // ACR = 0: one-shot T2, SR off
	wr(0x1c00, 0x20);                // IER <- 0x20: bit7 clear => DISABLE T2 interrupt
	CHECK((rd(0x1c00) & 0x20) == 0);
	wr(0x1000, 0xFF);                // T2C-L latch
	wr(0x1200, 0xFF);                // T2C-H: load 0xFFFF, clear IFR.5, start
	CHECK((rd(0x1a00) & 0x20) == 0); // armed, not expired
	// N7: expiry at N+1 ticks — at dt == N it has NOT fired yet:
	fake_ticks += 0xFFFF;
	CHECK((rd(0x1a00) & 0x20) == 0);
	fake_ticks += 1;                 // dt == N+1
	CHECK((rd(0x1a00) & 0x20) == 0x20);
	CHECK(via.t2_state == VIA_TIMER_FIRED);
	// N8: IFR write-1-clear clears the flag; FIRED is consumed, no re-assert:
	wr(0x1a00, 0x20);
	CHECK((rd(0x1a00) & 0x20) == 0);
	fake_ticks += 0x100;
	CHECK((rd(0x1a00) & 0x20) == 0);

	// IFR bit 7 master: set only when (IFR & IER & 0x7F) != 0.
	wr(0x1c00, 0xA0);                // IER: enable T2
	wr(0x1000, 0x10); wr(0x1200, 0x00);  // T2 = 0x0010
	fake_ticks += 0x20;
	uint8_t ifr = rd(0x1a00);
	CHECK((ifr & 0x20) && (ifr & 0x80));
	// N6: T2C-L read acknowledges (clears) IFR.T2:
	(void)rd(0x1000);
	CHECK((rd(0x1a00) & 0x20) == 0);

	// T2C reads return the live decrementing count while RUNNING.
	wr(0x1000, 0x00); wr(0x1200, 0x01);  // T2 = 0x0100
	fake_ticks += 0x40;
	CHECK(rd(0x1200) == 0x00);            // high byte first: no acknowledge side effect
	CHECK(rd(0x1000) == 0xC0);            // 0x0100 - 0x40 (this read also acks — flag was clear)
	wr(0x1a00, 0x20);                     // clean up (timer still RUNNING; N8: stays running)
	fake_ticks += 0x200;                  // let it fire
	CHECK((rd(0x1a00) & 0x20) == 0x20);   // RUNNING -> FIRED still happens after an IFR clear
	wr(0x1a00, 0x20);

	// Stored-state registers round-trip + Cuda loud stub telemetry.
	wr(0x0000, 0x18); CHECK((rd(0x0000) & 0x18) == 0x18);
	CHECK(via.cuda_touches > 0);
	wr(0x1400, 0x55); CHECK(rd(0x1400) == 0x55);
	CHECK(via.cuda_touches >= 3);
	CHECK(VIATakePendingWarning(&via) != 0 && VIATakePendingWarning(&via) == 0);

	// T1 minimal: load via T1C-H, IFR bit 6 after expiry (N+1 rule applies).
	wr(0x0800, 0x10); wr(0x0a00, 0x00);
	fake_ticks += 0x11;
	CHECK((rd(0x1a00) & 0x40) == 0x40);
	(void)rd(0x0800);                     // (rev 2 S9) T1CL read acknowledges T1
	CHECK((rd(0x1a00) & 0x40) == 0);

	// --- Eager path: bound scheduler latches IFR with NO intervening register read ---
	EventScheduler es;
	es.set_time_now_cb(fake_sched_ns);
	es.set_notify_changes_cb([]() {});
	VIAReset(&via, BASE, fake_clock, 0);
	VIABindScheduler(&via, &es, direct_call);
	wr(0x1000, 0x40); wr(0x1200, 0x00);   // T2 = 0x0040: arms a one-shot at (0x41 ticks) in ns
	fake_ticks += 0x41;
	es.process_timers();                   // pump: expiry event fires under locked_call
	CHECK(via.t2_state == VIA_TIMER_FIRED);
	CHECK((via.ifr_latched & 0x20) == 0x20);   // latched WITHOUT any VIARead
	// Re-arm before expiry: stale event must no-op (generation guard).
	wr(0x1000, 0x10); wr(0x1200, 0x00);   // gen G: T2 = 0x10
	wr(0x1000, 0xF0); wr(0x1200, 0x00);   // gen G+1: T2 = 0xF0 (re-write while running)
	fake_ticks += 0x20;                    // past gen-G deadline, before gen-G+1's
	es.process_timers();                   // gen-G event fires -> must be ignored
	CHECK(via.t2_state == VIA_TIMER_RUNNING);
	CHECK((rd(0x1a00) & 0x20) == 0);

	// --- Fix-1 boundary: T2CL read at exactly dt == N returns 0x00, not 0xFFxx ---
	// (lazy/unbound config so the pump can't interfere; drain the eager section's
	// queued one-shot first - its lambda holds &via, whose binding the reset nulls)
	es.cancel_all_timers();
	VIAReset(&via, BASE, fake_clock, 0);
	wr(0x1000, 0x30); wr(0x1200, 0x00);   // T2 = 0x0030
	fake_ticks += 0x30;                    // dt == N exactly: counter at 0, not fired (N7)
	CHECK(rd(0x1000) == 0x00);
	CHECK(via.t2_state == VIA_TIMER_RUNNING);

	// --- Fix-1 TOCTOU discriminator: clock that ADVANCES BETWEEN READS (rev 3 Q1).
	// The frozen-clock boundary check above passes even against the pre-fix
	// two-read code; this one fails pre-fix (returns 0xFF) and passes fixed. ---
	VIAReset(&via, BASE, stepping_clock, 0);
	wr(0x1000, 0x30); wr(0x1200, 0x00);   // arm consumes one read for load_time
	fake_ticks += 0x2F;                    // the count-read itself lands at dt == 0x30
	CHECK(rd(0x1000) == 0x00);             // pre-fix: 0xFF (second read pushed dt past cnt)
	CHECK(via.t2_state == VIA_TIMER_RUNNING);
	VIAReset(&via, BASE, fake_clock, 0);   // restore the frozen clock for what follows

	// --- Fix-2: blind IFR clear across an un-polled deadline stays clear ---
	wr(0x1000, 0x10); wr(0x1200, 0x00);   // T2 = 0x0010
	fake_ticks += 0x20;                    // deadline passed, NO intervening read
	wr(0x1a00, 0x20);                      // blind write-1-clear: settles then clears
	CHECK((rd(0x1a00) & 0x20) == 0);       // flag must NOT be resurrected by the poll

	// --- M6a Wave 2 #4: per-register read histogram ---
	VIAReset(&via, BASE, fake_clock, 0);   // reset clears reg_reads
	CHECK(via.reg_reads[13] == 0);
	(void)rd(0x1a00); (void)rd(0x1a00); (void)rd(0x1a00);   // IFR (reg 13) x3
	(void)rd(0x1000);                                       // T2CL (reg 8) x1
	CHECK(via.reg_reads[13] == 3);
	CHECK(via.reg_reads[8] == 1);
	char hist[256];
	CHECK(VIAFormatReadHistogram(&via, hist, sizeof(hist)) > 0);
	CHECK(strcmp(hist, "T2CL=1 IFR=3") == 0);   // nonzero entries, register order
	char top[64];
	CHECK(VIAFormatTopReads(top, sizeof(top)) == 0);  // not registered yet
	VIARegisterDiagInstance(&via);
	CHECK(VIAFormatTopReads(top, sizeof(top)) > 0);
	CHECK(strcmp(top, "(IFR=3,T2CL=1)") == 0);        // top-2, descending

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
