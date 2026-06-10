/*
 *  dev_via6522.h - VIA 6522 timer/IFR surface (M1 scope: SPIKE-S3 §1.5 / §4.2).
 *  MacIO layout: 16 registers at 0x200 stride from base (reg N at base + N*0x200).
 *  Time source injected for testability; ticks are VIA clock units (783360 Hz on Macs).
 *
 *  M2: timers are an explicit state machine driven by the virtual clock; expiry
 *  latches IFR once (eagerly via a bound EventScheduler, or lazily on the next
 *  register read). Conformance notes N6/N7/N8 resolved here (+T1CL-read ack, rev 2 S9).
 */

#ifndef DEV_VIA6522_H
#define DEV_VIA6522_H

#include "mmio_bus.h"

#define VIA_CLOCK_HZ 783360u

enum VIATimerState { VIA_TIMER_IDLE = 0, VIA_TIMER_RUNNING = 1, VIA_TIMER_FIRED = 2 };

class EventScheduler;   // event_sched.h not required by pure users

struct VIA6522 {
	uint32_t base;
	uint64_t (*now_ticks)(void *clock_opaque);   // monotonic VIA ticks (783360 Hz)
	void *clock_opaque;
	uint8_t  ora, orb, ddra, ddrb, acr, pcr, sr, ier, ifr_latched;
	uint8_t  t1l_l, t1l_h, t2l_l;
	uint64_t t1_load_time, t2_load_time;         // now_ticks at counter load
	uint16_t t1_count, t2_count;                 // programmed counts
	uint8_t  t1_state, t2_state;                 // VIATimerState
	uint32_t t1_gen, t2_gen;                     // arm generation (stale events no-op)
	// Scheduler binding (optional; NULL = lazy-only, e.g. unit tests without sched)
	EventScheduler *sched;
	bool (*locked_call)(uint32_t addr, void (*fn)(void *), void *opaque); // prod: MMIOBusWithRegion
	uint64_t cuda_touches;
	bool     cuda_warned;
	const char *cuda_warn_what;
};

extern void VIAReset(VIA6522 *v, uint32_t base,
                     uint64_t (*now_ticks)(void *), void *clock_opaque);
extern uint64_t VIARead(void *opaque, uint32_t addr, unsigned size);
extern void VIAWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value);

// Bind the scheduler AFTER VIAReset (and after the bus region exists, in prod).
// locked_call runs the expiry transition under the device's lock; tests may pass
// a direct-call shim. ticks->ns conversion is internal (VIA_CLOCK_HZ).
extern void VIABindScheduler(VIA6522 *v, EventScheduler *sched,
                             bool (*locked_call)(uint32_t addr, void (*fn)(void *), void *opaque));

// Return-and-clear the pending Cuda loud-stub warning text (static string), or NULL.
// Device handlers can run on the Mach exception-handler thread, where stdio is
// forbidden (MACHINE-LAYER-PLAN §2g) — the warning is only latched there and must
// be emitted by a safe thread (e.g. the emul thread / stats dump at exit).
extern const char *VIATakePendingWarning(VIA6522 *v);

#endif
