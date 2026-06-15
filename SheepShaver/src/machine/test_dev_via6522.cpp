/* Drives the 68k STM timeout-helper sequence (SPIKE-S3 §1.5) under a fake clock,
 * plus the M2 additions: N+1 expiry (N7), T2CL-read IFR acknowledge (N6), IFR
 * write-1-clear without timer disarm (N8), T1CL-read ack (S9), and eager IFR
 * latch via a bound EventScheduler with a fake time source. */
#include "dev_via6522.h"
#include "dev_cuda.h"
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

	// --- M3b Task 3: Cuda attachment seam (bind a REAL CudaDevice) -------------
	// All checks above ran UNBOUND (M1 loud-stub behavior verified intact).
	// Now script CV-0 + a full GET_TIME through the VIA register surface.
	static CudaDevice cu;
	VIAReset(&via, BASE, fake_clock, 0);
	CudaReset(&cu, 0, 0);                  // no time source: GET_TIME returns 0
	VIABindCuda(&via, &cu);
	wr(0x0400, 0x30);                      // DDRB = 0x30 (bits 4/5 out, bit 3 in)
	wr(0x0000, 0x38);                      // ORB idle (TACK+TIP negated)
	CHECK((rd(0x0000) & 0x08) == 0x08);    // CV-1: derived bit 3 high (idle)
	CHECK(via.cuda_touches == 0);          // loud stub replaced when bound
	// CV-0 sync/attention: ORB 0x28 (TACK asserted-low, TIP negated)
	wr(0x0000, 0x28);
	CHECK((rd(0x0000) & 0x08) == 0);       // TREQ asserted: the 18338 poll terminates
	CHECK((rd(0x1a00) & 0x04) == 0x04);    // IFR bit 2 raised (seen via R_IFR read)
	(void)rd(0x1400);                      // SR read clears IFR.2 (M4)
	CHECK((rd(0x1a00) & 0x04) == 0);
	wr(0x0000, 0x38);                      // host negates TACK: sync done
	CHECK((rd(0x0000) & 0x08) == 0x08);    // back to idle
	CHECK((rd(0x1a00) & 0x04) == 0x04);    // raise from the sync-end edge
	wr(0x1a00, 0x04);                      // IFR write-1-clear works on bit 2 too
	CHECK((rd(0x1a00) & 0x04) == 0);
	CHECK(cu.syncs == 1);
	// Full GET_TIME via the register surface (proves ORB writes forward the
	// live ACR and the Cuda SR byte replaces the VIA's stored sr both ways).
	wr(0x1600, 0x10);                      // ACR bit 4: shift out (host->Cuda)
	wr(0x1400, 0x01);                      // SR = PSEUDO packet type
	wr(0x0000, 0x18);                      // assert TIP: byte 0 captured
	CHECK((rd(0x1a00) & 0x04) == 0x04);    // capture raises IFR.2
	wr(0x1400, 0x03);                      // SR = GET_TIME
	wr(0x0000, 0x08);                      // TACK toggle: byte 1 captured
	wr(0x0000, 0x38);                      // negate TIP: commit (C2, synchronous)
	CHECK((rd(0x0000) & 0x08) == 0);       // response queued: TREQ low at commit
	wr(0x1600, 0x00);                      // ACR: shift in (Cuda->host)
	wr(0x0000, 0x18);                      // assert TIP: byte 0 -> SR
	{
		static const uint8_t expect[7] = { 0x01, 0x00, 0x03, 0, 0, 0, 0 };
		uint8_t tack = 0x10;
		for (int i = 0; i < 7; i++) {
			CHECK(rd(0x1400) == expect[i]);
			if (i < 6) {
				CHECK((rd(0x0000) & 0x08) == 0);   // more bytes: TREQ stays low
				tack ^= 0x10;
				wr(0x0000, (uint8_t)(0x08 | tack));
			}
		}
	}
	CHECK((rd(0x0000) & 0x08) == 0x08);    // TREQ negated after the last byte
	wr(0x0000, 0x38);                      // end transaction
	CHECK(cu.cmd_get_time == 1);
	CHECK(via.cuda_touches == 0);          // never touched the loud stub while bound
	CHECK(VIATakePendingWarning(&via) == 0);
	// orb_wtrace keeps working in bound mode (CV-0 + GET_TIME wrote ORB above).
	CHECK(via.orb_write_count > 0);
	{
		char trace[256];
		CHECK(VIAFormatOrbTrace(&via, trace, sizeof(trace)) > 0);
		CHECK(strstr(trace, "ddrb=30") != 0);
	}
	// Unbound again (fresh reset zeroes the binding): loud-stub behavior returns.
	VIAReset(&via, BASE, fake_clock, 0);
	CHECK(via.cuda == 0);
	wr(0x1400, 0x55); CHECK(rd(0x1400) == 0x55);   // stored sr echo
	CHECK(via.cuda_touches == 2);                  // SR write + SR read touched

	// --- Wave-2 W2-3: summary interrupt output (ifr & ier & 0x7F seam) --------
	{
		static int edges_n = 0;
		static bool edge_lvl[16];
		struct Seam {
			static void fn(void *, bool asserted) {
				if (edges_n < 16) edge_lvl[edges_n] = asserted;
				edges_n++;
			}
		};

		fake_ticks = 0;
		VIAReset(&via, BASE, fake_clock, 0);
		VIABindIRQOutput(&via, Seam::fn, 0);

		// IFR flag with IER disabled: NO output (the summary gate).
		wr(0x1c00, 0x00);                  // IER: nothing enabled (bit7=0 noop form)
		wr(0x1000, 0x10); wr(0x1200, 0x00);   // T2 = 0x0010, start
		fake_ticks += 0x20;
		CHECK((rd(0x1a00) & 0x20) == 0x20);   // T2 flag latched (lazy poll)
		CHECK(via.irq_out == 0 && edges_n == 0);

		// Enable T2 in IER -> assert edge at the IER write itself.
		wr(0x1c00, 0xA0);
		CHECK(via.irq_out == 1);
		CHECK(edges_n == 1 && edge_lvl[0]);
		CHECK(via.irq_raises == 1);

		// W1C of the T2 flag -> deassert edge.
		wr(0x1a00, 0x20);
		CHECK(via.irq_out == 0);
		CHECK(edges_n == 2 && !edge_lvl[1]);
		CHECK(via.irq_lowers == 1);

		// Lazy expiry observed through a read recomputes the output too:
		wr(0x1000, 0x08); wr(0x1200, 0x00);   // T2 = 8, IER.T2 still on
		fake_ticks += 0x10;
		(void)rd(0x1a00);                     // poll latches + raises
		CHECK(via.irq_out == 1 && edges_n == 3 && edge_lvl[2]);
		// T2CL read acks (N6) -> deassert through the read path.
		(void)rd(0x1000);
		CHECK(via.irq_out == 0 && edges_n == 4 && !edge_lvl[3]);

		// Eager scheduler expiry fires the seam OUTSIDE any read/write:
		{
			EventScheduler sched;
			sched.set_time_now_cb(fake_sched_ns);
			sched.set_notify_changes_cb([]() {});
			VIABindScheduler(&via, &sched, direct_call);
			wr(0x1000, 0x04); wr(0x1200, 0x00);   // T2 = 4
			fake_ticks += 0x10;                   // pass the deadline
			sched.process_timers();               // eager latch -> assert edge
			CHECK(via.irq_out == 1);
			CHECK(edges_n == 5 && edge_lvl[4]);
			sched.cancel_all_timers();            // drain before the lambda's &via dies
			VIABindScheduler(&via, 0, 0);
		}

		// VIAReset clears the binding + level (Reset-then-Bind contract).
		VIAReset(&via, BASE, fake_clock, 0);
		CHECK(via.irq_fn == 0 && via.irq_out == 0);
	}

	// --- S4: VIALatchIFRBits — out-of-band IFR latch (timer-delayed Cuda SR int).
	// Sets IFR bits + recomputes the summary WITHOUT a VIARead/VIAWrite register
	// access, so the int reaches the CPU even though the NewWorld NK never polls
	// IFR.  The IFR&IER summary gate still decides whether the CPU IRQ asserts.
	{
		static int edges_n = 0;
		static bool edge_lvl[8];
		struct Seam {
			static void fn(void *, bool asserted) {
				if (edges_n < 8) edge_lvl[edges_n] = asserted;
				edges_n++;
			}
		};
		VIAReset(&via, BASE, fake_clock, 0);
		VIABindIRQOutput(&via, Seam::fn, 0);

		// IER.SR (bit 2) disabled: latching IFR.SR sets the flag but the summary
		// gate keeps the CPU IRQ LOW (the M14 ordering the lazy path could honor).
		VIALatchIFRBits(&via, 0x04);
		CHECK((via.ifr_latched & 0x04) == 0x04);   // flag set out-of-band
		CHECK(via.irq_out == 0 && edges_n == 0);   // masked: no CPU IRQ

		// Enable IER.SR -> the already-latched flag now asserts the summary edge.
		wr(0x1c00, 0x84);                          // IER: set bit 2 (bit7=1 = set form)
		CHECK(via.irq_out == 1 && edges_n == 1 && edge_lvl[0]);

		// W1C the SR flag through the register path -> deassert.
		wr(0x1a00, 0x04);
		CHECK(via.irq_out == 0 && edges_n == 2 && !edge_lvl[1]);

		// With IER.SR enabled, a fresh out-of-band latch asserts immediately.
		VIALatchIFRBits(&via, 0x04);
		CHECK(via.irq_out == 1 && edges_n == 3 && edge_lvl[2]);

		// bits are masked to 0x7F (the composite bit 7 is never latched directly).
		wr(0x1a00, 0x04);                          // clear SR flag again
		(void)edge_lvl;
		VIALatchIFRBits(&via, 0x80);
		CHECK((via.ifr_latched & 0x80) == 0);      // bit 7 masked out

		VIAReset(&via, BASE, fake_clock, 0);
	}

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
