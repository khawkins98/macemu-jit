/*
 *  dev_via6522.cpp - VIA 6522 timer/IFR surface, M1 scope (SPIKE-S3 §1.5/§4.2).
 *  Timers are lazy: latch load-time, compute count/expiry on read. M2's event
 *  scheduler replaces this with real callbacks. Cuda SR protocol = loud stub.
 *
 *  MacIO register layout: reg N at base + N*0x200 (stride 0x200, 16 registers).
 *  Reachable from the Mach handler thread: no malloc, no stdio (MACHINE-LAYER-PLAN
 *  §2g). The one-shot Cuda warning is therefore only LATCHED here (cuda_warn_what);
 *  a safe thread emits it later via VIATakePendingWarning().
 */

#include "dev_via6522.h"
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

// Lazy timer: compute the remaining count (or 0 if expired/not running).
// Sets *expired if the timer has been armed and the deadline has passed.
// NOTE: timer_remaining() returns 0 once expired — the 6522 actually
// free-runs past zero, but no S3 consumer reads T2C after expiry (scope
// fence §4.2). Adjust if future tests require free-run behavior.
// Other out-of-fence edges (also unexercised by S3 consumers): count==0
// is instantly expired; expiry fires at cnt ticks, not the hardware's
// cnt+1; a T2C read after an IFR write-1-clear stopped the timer returns
// the stale (frozen) count rather than a free-running one.
static uint16_t timer_remaining(VIA6522 *v, bool t1, bool *expired)
{
	uint64_t now = v->now_ticks(v->clock_opaque);
	uint64_t load = t1 ? v->t1_load_time : v->t2_load_time;
	uint16_t cnt  = t1 ? v->t1_count : v->t2_count;
	bool running  = t1 ? v->t1_running : v->t2_running;
	uint64_t dt = now - load;
	// dt >= cnt: the counter has reached/passed zero (lazy expiry, M1 fence).
	*expired = running && dt >= cnt;
	return (uint16_t)(*expired ? 0 : cnt - (uint16_t)dt);
}

// Compute live IFR value: merge latched bits with lazy timer expiry, apply bit7 master.
static uint8_t ifr_now(VIA6522 *v)
{
	uint8_t ifr = v->ifr_latched;
	bool exp;
	timer_remaining(v, false, &exp); if (exp) ifr |= IFR_T2;
	timer_remaining(v, true,  &exp); if (exp) ifr |= IFR_T1;
	ifr &= 0x7F;
	// Bit 7: asserted when any enabled interrupt is pending.
	if (ifr & v->ier & 0x7F) ifr |= 0x80;
	return ifr;
}

uint64_t VIARead(void *opaque, uint32_t addr, unsigned size)
{
	VIA6522 *v = (VIA6522 *)opaque;
	(void)size;   // consumers are byte-wide; wider reads return the low byte
	bool exp;
	switch (((addr - v->base) >> 9) & 0xF) {
	case R_ORB:       return v->orb;
	case R_ORA:
	case R_ORA_NH:    return v->ora;
	case R_DDRB:      return v->ddrb;
	case R_DDRA:      return v->ddra;
	case R_T1CL:      { uint16_t r = timer_remaining(v, true,  &exp); return  r & 0xFF; }
	case R_T1CH:      { uint16_t r = timer_remaining(v, true,  &exp); return  r >> 8; }
	case R_T1LL:      return v->t1l_l;
	case R_T1LH:      return v->t1l_h;
	case R_T2CL:      { uint16_t r = timer_remaining(v, false, &exp); return  r & 0xFF; }
	case R_T2CH:      { uint16_t r = timer_remaining(v, false, &exp); return  r >> 8; }
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
		v->t1_count = (uint16_t)((b << 8) | v->t1l_l);
		v->t1_load_time = v->now_ticks(v->clock_opaque);
		v->t1_running = true;
		v->ifr_latched &= ~IFR_T1;
		break;
	// T2C-L write: latch the low byte (counter not yet loaded).
	case R_T2CL:      v->t2l_l = b; break;
	// T2C-H write: load counter from {b, t2l_l}, start, clear IFR.T2.
	case R_T2CH:
		v->t2_count = (uint16_t)((b << 8) | v->t2l_l);
		v->t2_load_time = v->now_ticks(v->clock_opaque);
		v->t2_running = true;
		v->ifr_latched &= ~IFR_T2;
		break;
	case R_SR:        cuda_touch(v, "SR write"); v->sr = b; break;
	case R_ACR:       v->acr = b; break;
	case R_PCR:       v->pcr = b; break;
	case R_IFR: {
		// Write-1-to-clear. For lazy timer bits, clear by stopping the timer
		// so ifr_now() no longer recomputes expiry for that bit.
		bool exp;
		if (b & IFR_T2) { timer_remaining(v, false, &exp); if (exp) v->t2_running = false; }
		if (b & IFR_T1) { timer_remaining(v, true,  &exp); if (exp) v->t1_running = false; }
		v->ifr_latched &= ~(b & 0x7F);
		break;
	}
	case R_IER:
		// Bit 7 = 1: set (enable) the masked bits. Bit 7 = 0: clear (disable).
		if (b & 0x80) v->ier |= (b & 0x7F);
		else          v->ier &= ~(b & 0x7F);
		break;
	}
}
