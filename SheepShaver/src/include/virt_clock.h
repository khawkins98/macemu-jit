/*
 *  virt_clock.h - Machine Layer M2 virtual clock (MACHINE-LAYER-PLAN.md §2c).
 *
 *  Guest-visible TB (timebase) and DEC (decrementer) backed by an injected host
 *  monotonic-ns source at a fixed ratio (tb_freq_hz = TimebaseSpeed). DEC expiry
 *  raises an exception CONDITION (latch + telemetry) only — delivery is M3.
 *  DEC is a CPU-internal exception (vector 0x900); it never passes through the
 *  PIC (§2c). Threading: WriteDEC/ReadDEC run on the CPU thread; the scheduler
 *  pump thread may call VirtClockDECExpire() — generation-guarded, atomics only.
 *
 *  M2 scope note (rev 2 finding S7): the condition is modeled only as
 *  count-through-zero after an mtspr DEC write (fires at v+1 ticks; for an
 *  MSB-set v this matches the OEA 0->1 MSB transition). Two OEA condition
 *  sources are deliberately unmodeled until M3 owns delivery: (a) mtspr DEC
 *  itself writing an MSB-set value over an MSB-clear one signals immediately on
 *  real hardware; (b) the cold free-running counter wraps through the MSB
 *  transition every 2^32 ticks. No M2 consumer reads the condition.
 */

#ifndef VIRT_CLOCK_H
#define VIRT_CLOCK_H

#include <stdint.h>
#include <stdio.h>

struct VirtClock {
	uint64_t (*now_ns)(void *opaque);   // injected host monotonic nanoseconds
	void    *opaque;
	uint32_t tb_freq_hz;                // TB/DEC tick rate (TimebaseSpeed)
	uint64_t tb_offset;                 // guest mttbl/mttbu adjustment (tb units, arithmetic mod 2^64)

	// DEC state (CPU-thread-owned)
	uint32_t dec_set_value;             // last mtspr DEC value (0 = cold: free-run from 0)
	uint64_t dec_set_tb;                // TB at that write (0 = cold)

	// Expiry condition (cross-thread: atomics only).
	// (rev 2 finding C1) generation and armed-flag are FUSED into one word so the
	// "is this event current" check and the "consume the arm" step are a single CAS
	// - a stale scheduler event can never steal a fresh generation's arm
	// (spurious pending + permanently lost expiry). dec_arm_word = (gen << 1) | armed.
	uint64_t dec_arm_word;
	uint32_t dec_pending;               // the M2 deliverable: the exception CONDITION latch

	// Eager-expiry hook (wired to the event scheduler by main_unix; may be NULL).
	// Called from VirtClockWriteDEC with the ns-until-expiry and the new generation.
	void (*on_dec_write)(void *cb_opaque, uint64_t ns_until_expiry, uint32_t gen);
	void *cb_opaque;

	// Telemetry (DoD observed-traffic asserts; dumped at exit)
	uint64_t mfspr_dec_reads, mtspr_dec_writes, tb_writes, dec_expiries;

	// W2-4 DEC-cadence capture (CPU-thread-only, appended LAST per struct-offset
	// rule): last-8 written DEC values + value-class buckets. Capture-only,
	// no stdio on the hot path; dumped by VirtClockDumpStats.
	uint32_t dec_write_ring[8];         // last 8 mtspr DEC values (ring)
	uint32_t dec_write_pc_ring[8];      // guest PC of each (set by the mtspr caller
	                                    // via VirtClockNoteDECWritePC before WriteDEC)
	uint32_t dec_write_note_pc;         // staging slot for the next write's PC
	uint32_t dec_write_ring_pos;
	uint64_t dec_w_zero;                // v == 0
	uint64_t dec_w_tiny;                // 0 < v < 0x1000 (<164us @25MHz)
	uint64_t dec_w_small;               // 0x1000 <= v < 0x40000 (<10.5ms)
	uint64_t dec_w_mid;                 // 0x40000 <= v < 0x80000000
	uint64_t dec_w_msb;                 // MSB set (negative write)
};

extern void     VirtClockInit(VirtClock *c, uint32_t tb_freq_hz,
                              uint64_t (*now_ns)(void *), void *opaque);
static inline bool VirtClockReady(const VirtClock *c) { return c->now_ns != 0; }

extern uint64_t VirtClockNowNS(VirtClock *c);   // any thread: raw injected source (devices derive ticks)
// CPU THREAD ONLY (they touch the plain-field DEC/TB state — pump-side callers
// must use VirtClockNowNS / VirtClockDECExpire instead):
extern uint64_t VirtClockTB(VirtClock *c);      // 64-bit TB = ns*freq/1e9 + tb_offset
extern uint32_t VirtClockReadDEC(VirtClock *c); // also performs the lazy expiry check
extern void     VirtClockWriteDEC(VirtClock *c, uint32_t v);
// W2-4 capture aid: note the guest PC of the upcoming WriteDEC (CPU thread only).
static inline void VirtClockNoteDECWritePC(VirtClock *c, uint32_t pc) { c->dec_write_note_pc = pc; }
extern void     VirtClockWriteTBL(VirtClock *c, uint32_t v);
extern void     VirtClockWriteTBU(VirtClock *c, uint32_t v);

// Scheduler-side expiry (pump thread). Latches dec_pending iff gen is current
// and the arm is still outstanding. Safe from any thread.
extern void     VirtClockDECExpire(VirtClock *c, uint32_t gen);

// M3 will consume these; M2 only reports them.
extern bool     VirtClockDECPending(const VirtClock *c);
extern void     VirtClockClearDECPending(VirtClock *c);

extern void     VirtClockDumpStats(const VirtClock *c, FILE *f);

// The emulator's single instance (defined in virt_clock.cpp; init'd by main_unix).
extern VirtClock g_virt_clock;

#endif
