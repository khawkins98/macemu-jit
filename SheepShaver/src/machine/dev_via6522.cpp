/*
 *  dev_via6522.cpp - VIA 6522 timer/IFR surface, M2 scope (SPIKE-S3 §1.5/§4.2).
 *  Timers use an explicit state machine (IDLE -> RUNNING -> FIRED). RUNNING->FIRED
 *  latches the IFR bit exactly once — eagerly via a bound EventScheduler callback,
 *  or lazily on the next register read (idempotent backstop). Cuda SR protocol =
 *  loud stub. Conformance notes N6/N7/N8 + T1CL-read ack (rev 2 S9) resolved.
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
#include "event_sched.h"
#include <stdio.h>    // snprintf only (buffer formatting — no FILE* I/O; §2g note in header)
#include <string.h>

// Register indices (reg N accessed at base + N*0x200).
enum { R_ORB=0, R_ORA=1, R_DDRB=2, R_DDRA=3, R_T1CL=4, R_T1CH=5, R_T1LL=6, R_T1LH=7,
       R_T2CL=8, R_T2CH=9, R_SR=10, R_ACR=11, R_PCR=12, R_IFR=13, R_IER=14, R_ORA_NH=15 };

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
	if (cur == ev->gen)
		timer_fire(v, ev->t1);
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

uint64_t VIARead(void *opaque, uint32_t addr, unsigned size)
{
	VIA6522 *v = (VIA6522 *)opaque;
	(void)size;   // consumers are byte-wide; wider reads return the low byte
	unsigned reg = ((addr - v->base) >> 9) & 0xF;
	v->reg_reads[reg]++;   // M6a Wave 2 #4: read histogram (under the bus lock)
	switch (reg) {
	case R_ORB:       return v->orb;
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
	case R_SR:        cuda_touch(v, "SR read");  return v->sr;
	case R_ACR:       return v->acr;
	case R_PCR:       return v->pcr;
	case R_IFR:       return ifr_now(v);
	case R_IER:       return 0x80 | v->ier;   // bit7 always set on reads (6522 spec)
	}
	return 0;
}

void VIAWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value)
{
	VIA6522 *v = (VIA6522 *)opaque;
	(void)size;
	uint8_t b = (uint8_t)value;
	switch (((addr - v->base) >> 9) & 0xF) {
	case R_ORB:
		// Bits 3/4 of ORB are the Cuda handshake lines (TREQ/TIP/byteack).
		if ((v->orb ^ b) & 0x18) cuda_touch(v, "ORB handshake bits 3/4");
		v->orb = b;
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
	case R_SR:        cuda_touch(v, "SR write"); v->sr = b; break;
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
}
