/*
 *  virt_clock.cpp - Machine Layer M2 virtual clock (see virt_clock.h / MACHINE-LAYER-PLAN §2c).
 *
 *  Threading: dec_set_value/dec_set_tb/tb_offset are CPU-thread-owned (all guest
 *  SPR traffic runs on the CPU thread via the interpreter slow path). The
 *  gen/armed/pending state crosses to the scheduler pump thread -> atomics.
 *  Ratio math uses unsigned __int128: ns up to host-uptime scale (~1e14) times
 *  tb_freq (25e6) overflows uint64.
 */

#include "virt_clock.h"
#include <string.h>

VirtClock g_virt_clock;   // zero-initialized: now_ns==NULL -> VirtClockReady()==false

// SS_M18 S3 T3: host-DEC-arming suppression flag (NK supervisor owns DEC). Pure
// module state — default false (byte-identical); flipped only by the gated glue
// path via VirtClockSuppressHostDEC(). No gate symbol here so the standalone
// virt_clock/exc_chain unit tests link without ppc-cpu.o.
static bool g_vclk_host_dec_suppressed = false;
void VirtClockSuppressHostDEC(bool on) { g_vclk_host_dec_suppressed = on; }

void VirtClockInit(VirtClock *c, uint32_t tb_freq_hz, uint64_t (*now_ns)(void *), void *opaque)
{
	memset(c, 0, sizeof(*c));
	c->tb_freq_hz = tb_freq_hz ? tb_freq_hz : 25000000u;
	c->now_ns = now_ns;
	c->opaque = opaque;
}

uint64_t VirtClockNowNS(VirtClock *c)
{
	return c->now_ns ? c->now_ns(c->opaque) : 0;
}

static inline uint64_t ns_to_tb(const VirtClock *c, uint64_t ns)
{
	return (uint64_t)(((unsigned __int128)ns * c->tb_freq_hz) / 1000000000u);
}

static inline uint64_t tb_to_ns(const VirtClock *c, uint64_t tb)
{
	return (uint64_t)(((unsigned __int128)tb * 1000000000u) / c->tb_freq_hz);
}

uint64_t VirtClockTB(VirtClock *c)
{
	return ns_to_tb(c, VirtClockNowNS(c)) + c->tb_offset;
}

/* Consume the outstanding arm and latch the condition. (rev 2 finding C1)
 * The generation check and the arm-consume are ONE compare-exchange on the
 * fused word - a stale event can never steal a fresh generation's arm. */
static void dec_fire(VirtClock *c, uint32_t gen)
{
	uint64_t expected = ((uint64_t)gen << 1) | 1;          // this gen, still armed
	if (!__atomic_compare_exchange_n(&c->dec_arm_word, &expected,
	                                 (uint64_t)gen << 1,    // same gen, disarmed
	                                 false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
		return;                                   // stale gen, or already fired
	__atomic_store_n(&c->dec_pending, 1, __ATOMIC_RELEASE);
	__atomic_fetch_add(&c->dec_expiries, 1, __ATOMIC_RELAXED);  // (rev 2 finding S5)
}

uint32_t VirtClockReadDEC(VirtClock *c)
{
	c->mfspr_dec_reads++;                          // CPU-thread-only counter
	uint64_t tb = VirtClockTB(c);
	uint64_t elapsed = tb - c->dec_set_tb;
	/* Lazy expiry check (works without a scheduler): the counter crossed below
	 * zero once elapsed exceeds the written value (expiry at v+1 ticks). */
	uint64_t w = __atomic_load_n(&c->dec_arm_word, __ATOMIC_ACQUIRE);
	if ((w & 1) && elapsed > c->dec_set_value)
		dec_fire(c, (uint32_t)(w >> 1));
	return c->dec_set_value - (uint32_t)elapsed;  // uint32 wrap = free-run past zero
}

void VirtClockWriteDEC(VirtClock *c, uint32_t v)
{
	c->mtspr_dec_writes++;                         // CPU-thread-only counter
	// W2-4 DEC-cadence capture (CPU-thread-only; no stdio here)
	c->dec_write_pc_ring[c->dec_write_ring_pos & 7] = c->dec_write_note_pc;
	c->dec_write_ring[c->dec_write_ring_pos++ & 7] = v;
	if      (v == 0)            c->dec_w_zero++;
	else if (v & 0x80000000u)   c->dec_w_msb++;
	else if (v < 0x1000u)       c->dec_w_tiny++;
	else if (v < 0x40000u)      c->dec_w_small++;
	else                        c->dec_w_mid++;
	c->dec_set_tb = VirtClockTB(c);
	c->dec_set_value = v;
	uint32_t gen = (uint32_t)(__atomic_load_n(&c->dec_arm_word, __ATOMIC_RELAXED) >> 1) + 1;
	__atomic_store_n(&c->dec_arm_word, ((uint64_t)gen << 1) | 1, __ATOMIC_RELEASE);
	// SS_M18 S3 T3: under the NK supervisor the host on_dec_write arming is
	// RETIRED — the clock state above is still updated (the NK reads DEC via
	// mfspr / VirtClockReadDEC's lazy expiry), but the synthetic host scheduler is
	// not armed. Default OFF => byte-identical.
	if (c->on_dec_write && !g_vclk_host_dec_suppressed)
		c->on_dec_write(c->cb_opaque, tb_to_ns(c, (uint64_t)v + 1), gen);
}

void VirtClockWriteTBL(VirtClock *c, uint32_t v)
{
	c->tb_writes++;
	uint64_t cur = VirtClockTB(c);
	uint64_t want = (cur & 0xFFFFFFFF00000000ull) | v;
	c->tb_offset += want - cur;            // arithmetic mod 2^64 (well-defined unsigned wrap)
}

void VirtClockWriteTBU(VirtClock *c, uint32_t v)
{
	c->tb_writes++;
	uint64_t cur = VirtClockTB(c);
	uint64_t want = ((uint64_t)v << 32) | (uint32_t)cur;
	c->tb_offset += want - cur;
}

void VirtClockDECExpire(VirtClock *c, uint32_t gen) { dec_fire(c, gen); }

bool VirtClockDECPending(const VirtClock *c)
{
	return __atomic_load_n((uint32_t *)&c->dec_pending, __ATOMIC_ACQUIRE) != 0;
}

void VirtClockClearDECPending(VirtClock *c)
{
	__atomic_store_n(&c->dec_pending, 0, __ATOMIC_RELEASE);
}

void VirtClockDumpStats(const VirtClock *c, FILE *f)
{
	// dec_expiries/dec_pending cross threads: relaxed atomic reads (the rest are
	// CPU-thread counters; this is an at-exit/diagnostic dump).
	fprintf(f, "[VCLK] tb_freq=%uHz mfspr_dec=%llu mtspr_dec=%llu tb_writes=%llu "
	        "dec_expiries=%llu pending=%u\n",
	        c->tb_freq_hz,
	        (unsigned long long)c->mfspr_dec_reads,
	        (unsigned long long)c->mtspr_dec_writes,
	        (unsigned long long)c->tb_writes,
	        (unsigned long long)__atomic_load_n((uint64_t *)&c->dec_expiries, __ATOMIC_RELAXED),
	        (unsigned)__atomic_load_n((uint32_t *)&c->dec_pending, __ATOMIC_RELAXED));
	if (c->mtspr_dec_writes) {
		fprintf(f, "[VCLK] dec writes: zero=%llu tiny=%llu small=%llu mid=%llu msb=%llu last8=",
		        (unsigned long long)c->dec_w_zero, (unsigned long long)c->dec_w_tiny,
		        (unsigned long long)c->dec_w_small, (unsigned long long)c->dec_w_mid,
		        (unsigned long long)c->dec_w_msb);
		for (int i = 0; i < 8; i++)
			fprintf(f, "%08x@%08x%s", c->dec_write_ring[(c->dec_write_ring_pos + i) & 7],
			        c->dec_write_pc_ring[(c->dec_write_ring_pos + i) & 7],
			        i == 7 ? "\n" : ",");
	}
}
