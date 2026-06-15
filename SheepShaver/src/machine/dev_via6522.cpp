/*
 *  dev_via6522.cpp - VIA 6522 timer/IFR surface, M2 scope (SPIKE-S3 §1.5/§4.2).
 *  Timers use an explicit state machine (IDLE -> RUNNING -> FIRED). RUNNING->FIRED
 *  latches the IFR bit exactly once — eagerly via a bound EventScheduler callback,
 *  or lazily on the next register read (idempotent backstop). Cuda SR protocol:
 *  forwarded to a bound CudaDevice (M3b Task 3, VIABindCuda — seam contract in
 *  the header); unbound keeps the M1 loud stub exactly (paravirtual/unit tests).
 *  Conformance notes N6/N7/N8 + T1CL-read ack (rev 2 S9) resolved.
 *
 *  MacIO register layout: reg N at base + N*0x200 (stride 0x200, 16 registers).
 *  Reachable from the Mach handler thread: no malloc, no stdio (MACHINE-LAYER-PLAN
 *  §2g). The one-shot Cuda warning is therefore only LATCHED here (cuda_warn_what);
 *  a safe thread emits it later via VIATakePendingWarning().
 *
 *  §2g malloc-on-fault-path note (M2): timer_arm() calls
 *  v->sched->add_oneshot_timer() which does `new TimerInfo` + std::function capture.
 *  The Mach fault dispatch path can reach VIAWrite (via MMIOBusWrite from
 *  MMIOMachFaultDispatch), so add_oneshot_timer's allocation can run on the
 *  exception-handler thread. This is a KNOWN ACCEPTED M2 RISK — the allocation is
 *  bounded and small, and in practice timer_arm is called from writes to T1C-H/T2C-H
 *  which are config-time operations (not hot fault paths). A malloc-free eager path
 *  (pre-allocated event slot) is a future hardening item (M3). See
 *  MACHINE-LAYER-PLAN §2g rule 1.
 *
 *  Free-run-past-zero read-back is still unmodeled (returns 0 after FIRED);
 *  no S3 consumer reads T2C after expiry (scope fence §4.2).
 */

#include "dev_via6522.h"
#include "dev_cuda.h"
#include "event_sched.h"
#include <stdio.h>    // snprintf only (buffer formatting — no FILE* I/O; §2g note in header)
#include <string.h>

// Register indices (reg N accessed at base + N*0x200).
enum { R_ORB=0, R_ORA=1, R_DDRB=2, R_DDRA=3, R_T1CL=4, R_T1CH=5, R_T1LL=6, R_T1LH=7,
       R_T2CL=8, R_T2CH=9, R_SR=10, R_ACR=11, R_PCR=12, R_IFR=13, R_IER=14, R_ORA_NH=15 };

#define IFR_SR 0x04
#define IFR_T2 0x20
#define IFR_T1 0x40

void VIAReset(VIA6522 *v, uint32_t base, uint64_t (*now)(void *), void *opaque)
{
	memset(v, 0, sizeof(*v));
	v->base = base; v->now_ticks = now; v->clock_opaque = opaque;
}

void VIABindScheduler(VIA6522 *v, EventScheduler *sched,
                      bool (*locked_call)(uint32_t addr, void (*fn)(void *), void *opaque))
{
	v->sched = sched;
	v->locked_call = locked_call;
}

// One-shot warning latch for Cuda-protocol register touches (loud-stub rule).
// NO stdio here: this runs on fault-reachable paths (Mach exception-handler
// thread, under the bus lock). The message is latched; VIATakePendingWarning()
// hands it to a safe thread for emission.
static void cuda_touch(VIA6522 *v, const char *what)
{
	v->cuda_touches++;
	if (!v->cuda_warned) {
		v->cuda_warned = true;
		v->cuda_warn_what = what;   // static string; pending until taken
	}
}

// --- M3b Task 3: Cuda attachment seam (full contract in the header) -------------
// Apply seam flags to the IFR DIRECTLY (M6: every seam call already runs under
// the non-recursive bus region lock — locked_call would deadlock). Clear before
// raise: CudaSRRead/Written return CLEAR, CudaSettle's consume-once latch
// returns RAISE; the two never arrive together today, but the order is safe
// if they ever do (a raise must not be lost to a stale clear).
// Timing (plan rev 2 M4, revised by CV-10): lazy-only — dev_cuda arms no
// scheduler one-shots.  Raise delivery is DEFERRED to CudaSettle on the R_IFR
// read surface ONLY (the lazy analogue of QEMU's 20us cuda_delay_set_sr_int);
// mutators and ORB reads never deliver.  No allocation happens on fault paths.
static inline void cuda_apply(VIA6522 *v, uint8_t flags)
{
	if (flags & CUDA_SEAM_CLEAR_SR_INT) v->ifr_latched &= ~IFR_SR;
	if (flags & CUDA_SEAM_RAISE_SR_INT) v->ifr_latched |= IFR_SR;
}

void VIABindCuda(VIA6522 *v, CudaDevice *c)
{
	v->cuda = c;
}

// --- Wave-2 W2-3: summary interrupt output (predicate + contract in header) ---
// Recompute the 6522 IRQ-pin summary from the LATCHED state (no lazy timer
// re-poll here — the mutating entry point that just ran already settled any
// relevant timer; re-polling would recurse into timer_fire). Fires the seam
// on transitions only, under the caller's lock. No malloc/stdio (§2g).
//
// The (ifr_latched & ier & 0x7F) gate is exactly DingusPPC ViaCuda::update_irq
// (devices/common/viacuda.cpp @ b2660e29201730efc2179a43ec6a0a5fb22ad120):
//   active_ints = _via_ifr & _via_ier & 0x7F;  ack_int(irq_id, active_ints ? 1 : 0);
// bit 7 = the composite IRQ to the CPU.  The S4 fix is NOT this gate (already
// correct) but the DELIVERY TIMING — see VIALatchIFRBits + dev_cuda's
// CudaBindTimerDelivery (M14-FINDINGS §3/§4b).
static void via_update_irq(VIA6522 *v)
{
	uint8_t out = ((v->ifr_latched & v->ier & 0x7F) != 0) ? 1 : 0;
	if (out == v->irq_out)
		return;
	v->irq_out = out;
	if (out) v->irq_raises++;
	else     v->irq_lowers++;
	if (v->irq_fn)
		v->irq_fn(v->irq_opaque, out != 0);
}

// S4: out-of-band IFR latch (contract in header). The delivery surface for the
// timer-delayed Cuda SR int — it sets the IFR bit and recomputes the IRQ summary
// WITHOUT a register read, so the int reaches the CPU even though the NewWorld NK
// never polls IFR (M14 wall). DingusPPC assert_sr_int + update_irq, viacuda.cpp
// @ b2660e29201730efc2179a43ec6a0a5fb22ad120 (PROSPECTIVE — needs live-S4 validation).
void VIALatchIFRBits(VIA6522 *v, uint8_t bits)
{
	v->ifr_latched |= (bits & 0x7F);
	via_update_irq(v);
}

void VIABindIRQOutput(VIA6522 *v, void (*fn)(void *opaque, bool asserted), void *opaque)
{
	v->irq_fn = fn;
	v->irq_opaque = opaque;
}

const char *VIATakePendingWarning(VIA6522 *v)
{
	const char *w = v->cuda_warn_what;
	v->cuda_warn_what = 0;
	return w;
}

// --- M6a Wave 2 #4: read-histogram diagnostics (design rationale in the header) ---
static const char *via_reg_names[16] = {
	"ORB", "ORA", "DDRB", "DDRA", "T1CL", "T1CH", "T1LL", "T1LH",
	"T2CL", "T2CH", "SR", "ACR", "PCR", "IFR", "IER", "ORAnh"
};

size_t VIAFormatReadHistogram(const VIA6522 *v, char *buf, size_t buflen)
{
	size_t n = 0;
	for (int i = 0; i < 16 && n < buflen; i++) {
		if (!v->reg_reads[i]) continue;
		n += (size_t)snprintf(buf + n, buflen - n, "%s%s=%llu", n ? " " : "",
		                      via_reg_names[i], (unsigned long long)v->reg_reads[i]);
	}
	return n;
}

size_t VIAFormatOrbTrace(const VIA6522 *v, char *buf, size_t buflen)
{
	if (!v->orb_write_count) return 0;
	size_t n = (size_t)snprintf(buf, buflen, "ddrb=%02X writes=%llu trace=",
	                            v->ddrb, (unsigned long long)v->orb_write_count);
	for (uint32_t i = 0; i < v->orb_wtrace_n && n < buflen; i++)
		n += (size_t)snprintf(buf + n, buflen - n, "%s%02X", i ? "," : "",
		                      v->orb_wtrace[i]);
	return n < buflen ? n : buflen - 1;
}

static VIA6522 *g_diag_via;   // heartbeat telemetry instance (prod bring-up registers it)

void VIARegisterDiagInstance(VIA6522 *v)
{
	g_diag_via = v;
}

size_t VIAFormatTopReads(char *buf, size_t buflen)
{
	VIA6522 *v = g_diag_via;
	if (!v) return 0;
	// Unlocked aligned 64-bit loads (single-copy-atomic on AArch64; header note).
	int top1 = -1, top2 = -1;
	for (int i = 0; i < 16; i++) {
		uint64_t c = v->reg_reads[i];
		if (!c) continue;
		if (top1 < 0 || c > v->reg_reads[top1]) { top2 = top1; top1 = i; }
		else if (top2 < 0 || c > v->reg_reads[top2]) { top2 = i; }
	}
	if (top1 < 0) return 0;
	if (top2 < 0)
		return (size_t)snprintf(buf, buflen, "(%s=%llu)", via_reg_names[top1],
		                        (unsigned long long)v->reg_reads[top1]);
	return (size_t)snprintf(buf, buflen, "(%s=%llu,%s=%llu)",
	                        via_reg_names[top1], (unsigned long long)v->reg_reads[top1],
	                        via_reg_names[top2], (unsigned long long)v->reg_reads[top2]);
}

static inline uint64_t via_ticks_to_ns(uint64_t ticks)
{
	return (uint64_t)((unsigned __int128)ticks * 1000000000u / VIA_CLOCK_HZ);
}

// RUNNING -> FIRED transition: latch the IFR bit exactly once. Idempotent under
// the device lock; both the lazy poll (reads) and the scheduler event call it.
static void timer_fire(VIA6522 *v, bool t1)
{
	uint8_t *state = t1 ? &v->t1_state : &v->t2_state;
	if (*state != VIA_TIMER_RUNNING) return;
	*state = VIA_TIMER_FIRED;
	v->ifr_latched |= (t1 ? IFR_T1 : IFR_T2);
}

// Lazy poll: fire if the deadline passed. N7: hardware expiry is at N+1 ticks.
static void timer_poll(VIA6522 *v, bool t1)
{
	uint8_t state = t1 ? v->t1_state : v->t2_state;
	if (state != VIA_TIMER_RUNNING) return;
	uint64_t dt = v->now_ticks(v->clock_opaque) - (t1 ? v->t1_load_time : v->t2_load_time);
	if (dt > (t1 ? (uint64_t)v->t1_count : (uint64_t)v->t2_count))
		timer_fire(v, t1);
}

// Live count for RUNNING; 0 otherwise (fence: no S3 consumer reads after expiry).
// Samples now_ticks() exactly ONCE: deciding (fire?) and computing (count) from
// separate clock reads is a TOCTOU — time advancing between them makes
// cnt - dt underflow to 0xFFFF near expiry (guest sees the counter jump UP).
static uint16_t timer_count_now(VIA6522 *v, bool t1)
{
	uint8_t state = t1 ? v->t1_state : v->t2_state;
	if (state != VIA_TIMER_RUNNING) return 0;
	uint64_t dt = v->now_ticks(v->clock_opaque) - (t1 ? v->t1_load_time : v->t2_load_time);
	uint16_t cnt = t1 ? v->t1_count : v->t2_count;
	if (dt > cnt) { timer_fire(v, t1); return 0; }
	if (dt == cnt) return 0;            // counter at 0, fires next tick (N7)
	return (uint16_t)(cnt - dt);
}

// Compute live IFR value: poll lazy timers, merge latched bits, apply bit7 master.
static uint8_t ifr_now(VIA6522 *v)
{
	timer_poll(v, false);
	timer_poll(v, true);
	uint8_t ifr = v->ifr_latched & 0x7F;
	if (ifr & v->ier & 0x7F) ifr |= 0x80;
	return ifr;
}

// Scheduler expiry events. The event captures {via, t1, gen}; it must run under
// the device's lock (locked_call -> MMIOBusWithRegion in prod, direct in tests)
// and no-op if the timer was re-armed/cleared since (generation guard).
struct VIATimerEvent { VIA6522 *v; bool t1; uint32_t gen; };
static void via_expiry_locked(void *opaque)
{
	VIATimerEvent *ev = (VIATimerEvent *)opaque;
	VIA6522 *v = ev->v;
	uint32_t cur = ev->t1 ? v->t1_gen : v->t2_gen;
	if (cur == ev->gen) {
		timer_fire(v, ev->t1);
		// W2-3: the eager expiry latches IFR outside any VIARead/VIAWrite —
		// recompute the summary output here too (runs under the region lock
		// via locked_call, same contract).
		via_update_irq(v);
	}
}

static void timer_arm(VIA6522 *v, bool t1, uint16_t count)
{
	if (t1) { v->t1_count = count; v->t1_load_time = v->now_ticks(v->clock_opaque);
	          v->t1_state = VIA_TIMER_RUNNING; v->t1_gen++; }
	else    { v->t2_count = count; v->t2_load_time = v->now_ticks(v->clock_opaque);
	          v->t2_state = VIA_TIMER_RUNNING; v->t2_gen++; }
	if (v->sched && v->locked_call) {
		VIA6522 *vv = v; bool is_t1 = t1;
		uint32_t gen = t1 ? v->t1_gen : v->t2_gen;
		// N7: deadline is count+1 ticks from load.
		uint64_t ns = via_ticks_to_ns((uint64_t)count + 1);
		v->sched->add_oneshot_timer(ns, [vv, is_t1, gen]() {
			VIATimerEvent ev = { vv, is_t1, gen };
			vv->locked_call(vv->base, via_expiry_locked, &ev);
		});
	}
}

static uint64_t via_read_inner(VIA6522 *v, uint32_t addr, unsigned size)
{
	(void)size;   // consumers are byte-wide; wider reads return the low byte
	unsigned reg = ((addr - v->base) >> 9) & 0xF;
	v->reg_reads[reg]++;   // M6a Wave 2 #4: read histogram (under the bus lock)
	switch (reg) {
	case R_ORB:
		if (v->cuda)
			// C1: bit 3 recomputed.  NO CudaSettle here (CV-10): TREQ derivation
			// is synchronous, but raise delivery is IFR-read-only — settling on
			// ORB reads can deliver the post-edge raise into IFR.2 before the
			// guest's SR read, which then clears it (the ROM 0x9584 sync park).
			return CudaDeriveORB(v->cuda, v->orb);
		return v->orb;
	case R_ORA:
	case R_ORA_NH:    return v->ora;
	case R_DDRB:      return v->ddrb;
	case R_DDRA:      return v->ddra;
	case R_T1CL:      { uint16_t r = timer_count_now(v, true);
	                    v->ifr_latched &= ~IFR_T1;          // (rev 2 S9) T1CL read acks T1, like T2's N6
	                    return r & 0xFF; }
	case R_T1CH:      { uint16_t r = timer_count_now(v, true); return r >> 8; }
	case R_T1LL:      return v->t1l_l;
	case R_T1LH:      return v->t1l_h;
	case R_T2CL:      { uint16_t r = timer_count_now(v, false);
	                    v->ifr_latched &= ~IFR_T2;          // N6: T2CL read acknowledges T2
	                    return r & 0xFF; }
	case R_T2CH:      { uint16_t r = timer_count_now(v, false); return r >> 8; }
	case R_SR:
		if (v->cuda) {
			uint8_t val;
			cuda_apply(v, CudaSRRead(v->cuda, &val));  // M4: SR access clears IFR.2
			return val;                                // Cuda's SR replaces stored sr
		}
		cuda_touch(v, "SR read");  return v->sr;
	case R_ACR:       return v->acr;
	case R_PCR:       return v->pcr;
	case R_IFR:
		if (v->cuda)
			cuda_apply(v, CudaSettle(v->cuda));   // CV-10: the ONE delivery surface
		return ifr_now(v);
	case R_IER:       return 0x80 | v->ier;   // bit7 always set on reads (6522 spec)
	}
	return 0;
}

uint64_t VIARead(void *opaque, uint32_t addr, unsigned size)
{
	VIA6522 *v = (VIA6522 *)opaque;
	uint64_t r = via_read_inner(v, addr, size);
	// W2-3: reads mutate IFR (timer-count acks, Cuda settle/SR clear, lazy
	// timer polls) — recompute the summary output after every access.
	via_update_irq(v);
	return r;
}

void VIAWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value)
{
	VIA6522 *v = (VIA6522 *)opaque;
	(void)size;
	uint8_t b = (uint8_t)value;
	switch (((addr - v->base) >> 9) & 0xF) {
	case R_ORB:
		// Cuda handshake lines live in ORB bits 3/4/5 (M3b C3: TREQ=3 input,
		// TACK=4, TIP=5, active-LOW — donor study §3.2 prose had 3/4; corrected).
		// Loud stub only while UNBOUND (M1 behavior); bound forwards to the model.
		if (!v->cuda && ((v->orb ^ b) & 0x38)) cuda_touch(v, "ORB handshake bits 3/4/5");
		// C3 polarity forensics: trace value TRANSITIONS (capture-only, §2g;
		// runs in both bound and unbound modes).
		v->orb_write_count++;
		if (v->orb_wtrace_n < sizeof(v->orb_wtrace) &&
		    (v->orb_wtrace_n == 0 || v->orb_wtrace[v->orb_wtrace_n - 1] != b))
			v->orb_wtrace[v->orb_wtrace_n++] = b;
		v->orb = b;
		if (v->cuda)
			cuda_apply(v, CudaORBWritten(v->cuda, b, v->acr));
		break;
	case R_ORA:
	case R_ORA_NH:    v->ora = b; break;
	case R_DDRB:      v->ddrb = b; break;
	case R_DDRA:      v->ddra = b; break;
	// T1C-L write / T1 latch-L: just stores the low byte latch.
	case R_T1CL:
	case R_T1LL:      v->t1l_l = b; break;
	// T1 latch-H: stores latch only (does NOT start/reload the counter).
	case R_T1LH:      v->t1l_h = b; break;
	// T1C-H write: load counter from latch+b, start, clear IFR.T1.
	case R_T1CH:
		v->t1l_h = b;
		timer_arm(v, true, (uint16_t)((b << 8) | v->t1l_l));
		v->ifr_latched &= ~IFR_T1;
		break;
	// T2C-L write: latch the low byte (counter not yet loaded).
	case R_T2CL:      v->t2l_l = b; break;
	// T2C-H write: load counter from {b, t2l_l}, start, clear IFR.T2.
	case R_T2CH:
		timer_arm(v, false, (uint16_t)((b << 8) | v->t2l_l));
		v->ifr_latched &= ~IFR_T2;
		break;
	case R_SR:
		if (v->cuda) { cuda_apply(v, CudaSRWritten(v->cuda, b)); break; }
		cuda_touch(v, "SR write"); v->sr = b; break;
	case R_ACR:       v->acr = b; break;
	case R_PCR:       v->pcr = b; break;
	case R_IFR:
		// N8: write-1-to-clear clears flags ONLY; a RUNNING timer keeps running
		// (its eventual RUNNING->FIRED still latches once), a FIRED one stays
		// consumed. But first settle any deadline that already passed (the flag
		// must exist before it can be cleared; without this, a blind clear is
		// "resurrected" by the next poll — on hardware the flag was set at the
		// deadline, so that clear is permanent).
		if (b & IFR_T1) timer_poll(v, true);
		if (b & IFR_T2) timer_poll(v, false);
		v->ifr_latched &= ~(b & 0x7F);
		break;
	case R_IER:
		// Bit 7 = 1: set (enable) the masked bits. Bit 7 = 0: clear (disable).
		if (b & 0x80) v->ier |= (b & 0x7F);
		else          v->ier &= ~(b & 0x7F);
		break;
	}
	// W2-3: writes mutate IFR/IER (W1C, timer loads, IER enables, Cuda seam)
	// — recompute the summary output after every access.
	via_update_irq(v);
}
