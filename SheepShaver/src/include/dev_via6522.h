/*
 *  dev_via6522.h - VIA 6522 timer/IFR surface (M1 scope: SPIKE-S3 §1.5 / §4.2).
 *  MacIO layout: 16 registers at 0x200 stride from base (reg N at base + N*0x200).
 *  Time source injected for testability; ticks are VIA clock units (783360 Hz on Macs).
 */

#ifndef DEV_VIA6522_H
#define DEV_VIA6522_H

#include "mmio_bus.h"

#define VIA_CLOCK_HZ 783360u

struct VIA6522 {
	uint32_t base;
	uint64_t (*now_ticks)(void *clock_opaque);   // monotonic VIA ticks
	void *clock_opaque;
	uint8_t  ora, orb, ddra, ddrb, acr, pcr, sr, ier, ifr_latched;
	uint8_t  t1l_l, t1l_h, t2l_l;
	uint64_t t1_load_time, t2_load_time;         // now_ticks at counter load
	uint16_t t1_count, t2_count;                 // programmed counts
	bool     t1_running, t2_running;
	uint64_t cuda_touches;                       // loud-stub telemetry (SR + handshake bits)
	bool     cuda_warned;                        // one-shot warning latch
	const char *cuda_warn_what;                  // pending warning text (NULL = none); see VIATakePendingWarning
};

extern void VIAReset(VIA6522 *v, uint32_t base,
                     uint64_t (*now_ticks)(void *), void *clock_opaque);
extern uint64_t VIARead(void *opaque, uint32_t addr, unsigned size);
extern void VIAWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value);
// Return-and-clear the pending Cuda loud-stub warning text (static string), or NULL.
// Device handlers can run on the Mach exception-handler thread, where stdio is
// forbidden (MACHINE-LAYER-PLAN §2g) — the warning is only latched there and must
// be emitted by a safe thread (e.g. the emul thread / stats dump at exit).
extern const char *VIATakePendingWarning(VIA6522 *v);

#endif
