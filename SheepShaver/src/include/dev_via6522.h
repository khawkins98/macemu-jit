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
struct CudaDevice;      // dev_cuda.h not required by pure users (M3b seam)

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
	// M6a Wave 2 #4 diagnostic: per-register read histogram (reg index 0..15).
	// Bumped in VIARead (runs under the bus region lock; plain increments are
	// race-free there). Identifies WHICH register a guest poll loop hammers.
	uint64_t reg_reads[16];
	// M3b C3 polarity forensics: ORB write-value transition trace (first 32 distinct
	// consecutive values) + total write count. The 68k handshake engines are invisible
	// to PPC-PC probes, so the written bit-4/5 idle/handshake pattern is the polarity
	// evidence (Cuda TREQ=3/TACK=4/TIP=5 active-LOW vs the Egret-polarity engine).
	// Same §2g rules as reg_reads: capture-only here, emission on safe threads.
	uint8_t  orb_wtrace[32];
	uint32_t orb_wtrace_n;
	uint64_t orb_write_count;
	// M3b Task 3: bound Cuda model (NULL = unbound -> M1 loud-stub behavior,
	// the paravirtual/unit-test default). See VIABindCuda below for the seam
	// contract and the rev 2 M4 timing decision.
	CudaDevice *cuda;
	// --- Wave-2 W2-3: summary interrupt output (fields APPENDED LAST) ---
	// Level = ((ifr_latched & ier & 0x7F) != 0) — the 6522 IRQ-pin summary
	// (the same composition R_IFR's bit 7 reports). Recomputed at the end of
	// every VIARead/VIAWrite and at the scheduler expiry transition; the seam
	// fires on TRANSITIONS only, under the caller's lock (prod: the VIA bus
	// region lock). Cross-region lock order: device -> pic (the callback may
	// take the PIC region lock; never the reverse).
	// LAZY-DELIVERY CAVEAT (the header's own Wave-2 warning, now load-bearing):
	// Cuda SR-int delivery is settle-on-IFR-read — for a guest that stops
	// polling, that edge fires only on the next VIA access. Timer expiries DO
	// fire eagerly via the bound scheduler. Recorded; W2-4's IER-push decision
	// owns the remainder.
	void   (*irq_fn)(void *opaque, bool asserted);
	void    *irq_opaque;
	uint8_t  irq_out;       // current summary level (0/1)
	uint64_t irq_raises, irq_lowers;   // telemetry: transitions
	// (append any future fields HERE, last)
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

// --- M3b Task 3: Cuda attachment seam -------------------------------------------
// Bind the Cuda protocol model behind the SR/ORB surface (replaces the M1 loud
// stub). When bound:
//   - R_ORB writes forward to CudaORBWritten (written byte + current ACR);
//   - R_SR read/write forward to CudaSRRead/CudaSRWritten — the Cuda's SR byte
//     replaces the VIA's stored sr on those paths;
//   - R_ORB reads run CudaDeriveORB alone — NO settle (CV-10): bit 3 = TREQ
//     recomputed from Cuda state on EVERY read (C1, the ROM does RMW on ORB;
//     bits 4/5 pass through from the stored byte). TREQ is mutated
//     synchronously, so ORB polling never depends on pending delivery;
//   - R_IFR reads run CudaSettle — the SINGLE deferred SR-int delivery
//     surface (the deterministic lazy analogue of QEMU's 20µs
//     cuda_delay_set_sr_int: the int must land AFTER the host's SR read of
//     the same edge, or the ROM's post-sync 15000-budget IFR wait starves —
//     CV-10, commit d3e60d88).
// Returned CUDA_SEAM_* flags are applied to ifr_latched bit 2 DIRECTLY (M6:
// the bus region lock is non-recursive — every seam call already runs under
// it, so locked_call would deadlock; the Cuda module itself is lock-free).
// Timing decision (plan rev 2 M4, documented at the seam): lazy-only. dev_cuda
// arms NO scheduler one-shots — the VIA header's "config-time, not hot fault
// paths" deferral rationale does not hold for per-SR-byte timing (first-touch
// reachable from the Mach handler thread), so settle-on-IFR-read is the ONLY
// delivery mechanism and no allocation can occur on fault paths. The
// poll-driven boot protocol (S3 §1.5) works lazy-only by design. Wave-2
// caveat: this assumes a POLLING guest — if/when IER-driven CPU interrupt
// delivery lands, an IFR-read-only delivery point can never fire for a
// sleeping guest; the surface must then gain a non-read trigger (timer or
// IER-gated push).
// Unbound (NULL/never called): M1 behavior exactly — cuda_touch loud stub,
// stored sr/orb echo. The ORB write-value trace (orb_wtrace) runs in BOTH modes.
extern void VIABindCuda(VIA6522 *v, CudaDevice *c);

// Wave-2 W2-3: bind the summary-interrupt output seam AFTER VIAReset (reset
// clears the binding). fn fires on summary-level transitions (see the struct
// comment for the predicate, recompute points, lock order, and the lazy-
// delivery caveat). NULL fn unbinds.
extern void VIABindIRQOutput(VIA6522 *v,
                             void (*fn)(void *opaque, bool asserted), void *opaque);

// Return-and-clear the pending Cuda loud-stub warning text (static string), or NULL.
// Device handlers can run on the Mach exception-handler thread, where stdio is
// forbidden (MACHINE-LAYER-PLAN §2g) — the warning is only latched there and must
// be emitted by a safe thread (e.g. the emul thread / stats dump at exit).
extern const char *VIATakePendingWarning(VIA6522 *v);

// --- M6a Wave 2 #4: read-histogram diagnostics ---------------------------------
// Design (§2g-safe): VIARead only bumps plain counters under the bus lock — no
// stdio, no malloc (it can run on the Mach exception-handler thread). Emission
// happens elsewhere, on safe threads:
//   - full histogram: mmio_dump_stats_atexit (main_unix) prints "[VIA] reads: ..."
//     via VIAFormatReadHistogram;
//   - alarm-killed boots skip atexit, so the heartbeat also carries the top-2
//     registers: prod registers its instance once (VIARegisterDiagInstance) and
//     the heartbeat thread calls VIAFormatTopReads. The heartbeat reads the
//     counters WITHOUT the lock — aligned 64-bit loads are single-copy-atomic on
//     AArch64, so the worst case is a slightly stale count (benign for telemetry).
// Both formatters snprintf into a caller buffer (no FILE* — not stdio-locked).

// Format all nonzero entries as "ORB=N ORA=N ... IFR=N IER=N". Returns chars
// written (0 if no reads yet). Safe from any thread (see note above).
extern size_t VIAFormatReadHistogram(const VIA6522 *v, char *buf, size_t buflen);

// Register the prod VIA instance for heartbeat telemetry (call once at bus
// bring-up). Unit tests may also use it; last registration wins.
extern void VIARegisterDiagInstance(VIA6522 *v);

// Format the top-2 most-read registers of the registered instance as
// "(IFR=N,T2CL=M)" (one entry if only one is nonzero). Returns chars written;
// 0 when no instance is registered or no reads happened.
extern size_t VIAFormatTopReads(char *buf, size_t buflen);

// Format the ORB write-value trace as "ddrb=XX writes=N trace=AA,BB,..." (hex
// values). Returns chars written (0 if ORB was never written). Safe-thread rule
// as above (stats dump / heartbeat emit it, never the capture path).
extern size_t VIAFormatOrbTrace(const VIA6522 *v, char *buf, size_t buflen);

#endif
