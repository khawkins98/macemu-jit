/*
 *  dev_openpic.cpp - OpenPIC (KeyLargo MPIC) model implementation.
 *  Behavioral oracle: QEMU hw/intc/openpic.c + include/hw/ppc/openpic.h
 *  @ de5d8bfd6105d3dd3ae668df9762df244a6d1506 (reimplementation, not a port —
 *  donor study §5.3 verdict).  Scope, divergences, and the §2g rules are
 *  documented in dev_openpic.h.
 *
 *  Oracle cross-reference map (openpic.c line numbers @ the pinned SHA):
 *    openpic_reset            :1254  -> reset_registers()
 *    openpic_gbl_read/write   :623/562 -> glb_read()/glb_write()
 *    openpic_src_read/write   :853/828 -> src_read()/src_write()
 *    openpic_cpu_read/write_internal :1109/965 -> cpu_read()/cpu_write()
 *    openpic_iack             :1056  -> do_iack()
 *    openpic_set_irq          :388   -> set_input()
 *    openpic_update_irq       :325   -> update_irq()
 *    IRQ_local_pipe           :238   -> local_pipe()
 *    IRQ_check/IRQ_get_next   :205/230 -> queue_next()
 *    write_IRQreg_ivpr/idr    :503/445 -> write_ivpr()/write_idr()
 */

#include "dev_openpic.h"

#include <stdio.h>    // snprintf — formatters only (safe threads, never bus paths)
#include <string.h>

#define IVPR_PRIO(x) ((int)(((x) & OPENPIC_IVPR_PRIO_MASK) >> OPENPIC_IVPR_PRIO_SHIFT))

// Sub-bank offsets within the region (oracle: OPENPIC_*_REG_START/SIZE)
#define GLB_START 0x0u
#define GLB_END   0x10F0u
#define TMR_START 0x10F0u
#define TMR_END   0x1310u
#define SRC_START 0x10000u
#define SRC_END   0x12000u
#define CPU_START 0x20000u

// ---------------------------------------------------------------------------
// §2g warning latch (one pending at a time; taking it re-arms — VIA pattern)

static void latch_warn(OpenPICDevice *p, const char *what)
{
	p->warned = 1;
	if (!p->warn_what)
		p->warn_what = what;
}

const char *OpenPICTakePendingWarning(OpenPICDevice *p)
{
	const char *w = p->warn_what;
	p->warn_what = NULL;
	return w;
}

// ---------------------------------------------------------------------------
// Queues.  64 sources fit one uint64_t.  queue_next mirrors IRQ_check
// (openpic.c:205): scan ascending, strictly-greater priority wins, so the
// lowest source number wins priority ties.  Returns -1 / *prio_out = -1 when
// the queue is empty (the oracle's empty-queue priority).

static int queue_next(const OpenPICDevice *p, uint64_t bits, int *prio_out)
{
	int irq = -1, priority = -1;
	for (int i = 0; i < OPENPIC_NUM_SRC; i++) {
		if (!(bits & (1ull << i)))
			continue;
		if (IVPR_PRIO(p->ivpr[i]) > priority) {
			irq = i;
			priority = IVPR_PRIO(p->ivpr[i]);
		}
	}
	if (prio_out)
		*prio_out = priority;
	return irq;
}

// Output line: transitions only (QEMU's redundant qemu_irq_raise/lower calls
// are level-idempotent — the transition is the observable behavior).
static void set_output(OpenPICDevice *p, bool asserted)
{
	if (p->out_asserted == (asserted ? 1 : 0))
		return;
	p->out_asserted = asserted ? 1 : 0;
	if (asserted)
		p->out_raises++;
	else
		p->out_lowers++;
	if (p->out_fn)
		p->out_fn(p->out_opaque, asserted);
}

// IRQ_local_pipe (openpic.c:238), single CPU, INT output only.
static void local_pipe(OpenPICDevice *p, int n, bool active)
{
	int priority = IVPR_PRIO(p->ivpr[n]);
	int raised_prio, servicing_prio;

	if (active)
		p->raised_bits |= 1ull << n;
	else
		p->raised_bits &= ~(1ull << n);

	if (active && priority <= p->ctpr)
		active = false;          // gated by task priority (line stays raised)

	if (active) {
		queue_next(p, p->servicing_bits, &servicing_prio);
		if (servicing_prio >= 0 && priority <= servicing_prio) {
			// hidden behind the in-service interrupt: no output change
		} else {
			set_output(p, true);
		}
	} else {
		queue_next(p, p->raised_bits, &raised_prio);
		queue_next(p, p->servicing_bits, &servicing_prio);
		if (raised_prio > p->ctpr && raised_prio > servicing_prio) {
			// IRQ line stays asserted (something else is deliverable)
		} else {
			set_output(p, false);
		}
	}
}

// openpic_update_irq (openpic.c:325): registers for source n changed.
static void update_irq(OpenPICDevice *p, int n)
{
	bool active = p->pending[n] != 0;
	bool was_active;

	if (p->ivpr[n] & OPENPIC_IVPR_MASK)
		active = false;                       // source disabled

	was_active = (p->ivpr[n] & OPENPIC_IVPR_ACTIVITY) != 0;
	if (!active && !was_active)
		return;

	if (active)
		p->ivpr[n] |= OPENPIC_IVPR_ACTIVITY;
	else
		p->ivpr[n] &= ~OPENPIC_IVPR_ACTIVITY;

	if ((p->idr[n] & 1u) == 0)
		return;                               // no target (activity stays set)

	local_pipe(p, n, active);
}

// openpic_set_irq (openpic.c:388), INT-output sources only.
static void set_input(OpenPICDevice *p, int n, int level)
{
	if (p->level[n]) {
		p->pending[n] = level ? 1 : 0;        // level-sensitive: track the line
		update_irq(p, n);
	} else if (level) {
		p->pending[n] = 1;                    // edge-sensitive: latch on raise
		update_irq(p, n);
	}
	// edge + lower: no-op (oracle)
}

// ---------------------------------------------------------------------------
// Register writes

// write_IRQreg_ivpr (openpic.c:503).  ACTIVITY read-only; MODE not writable
// (absent from the mask — any write clears it, oracle-exact); IRQ_TYPE_NORMAL
// => level tracks the SENSE bit.
static void write_ivpr(OpenPICDevice *p, int n, uint32_t val)
{
	const uint32_t mask = OPENPIC_IVPR_MASK | OPENPIC_IVPR_PRIO_MASK |
	                      OPENPIC_IVPR_SENSE | OPENPIC_IVPR_POLARITY |
	                      OPENPIC_VECTOR_MASK;
	p->ivpr[n] = (p->ivpr[n] & OPENPIC_IVPR_ACTIVITY) | (val & mask);
	p->level[n] = (p->ivpr[n] & OPENPIC_IVPR_SENSE) ? 1 : 0;
	update_irq(p, n);
}

// write_IRQreg_idr (openpic.c:445), no IDR_CRIT flag on KeyLargo:
// idr = val & ((1 << nb_cpus) - 1) = val & 1.  NOTE: no update_irq (oracle).
static void write_idr(OpenPICDevice *p, int n, uint32_t val)
{
	p->idr[n] = val & 1u;
}

// openpic_reset (openpic.c:1254) with the KeyLargo realize config
// (openpic.c:1570).  Resets register state ONLY — base, the output binding,
// telemetry, and the Q8 first-IACK record survive (documented divergence:
// QEMU's device reset has no such distinction; the register-visible state is
// identical, and a GCR soft reset preserving diagnostics is strictly more
// useful here).
static void reset_registers(OpenPICDevice *p)
{
	p->gcr = 0;                               // reset completes synchronously
	p->spve = 0xFFFFFFFFu & OPENPIC_VECTOR_MASK;
	for (int i = 0; i < OPENPIC_NUM_SRC; i++) {
		p->ivpr[i] = OPENPIC_IVPR_RESET;      // masked, MODE set
		p->idr[i] = 0;                        // idr_reset = 0
		p->pending[i] = 0;
		p->level[i] = 0;                      // reset SENSE = 0 => edge
	}
	p->ctpr = 15;
	p->raised_bits = 0;
	p->servicing_bits = 0;
	set_output(p, false);
}

void OpenPICReset(OpenPICDevice *p, uint32_t base)
{
	memset(p, 0, sizeof(*p));
	p->base = base;
	reset_registers(p);
}

void OpenPICBindOutput(OpenPICDevice *p, OpenPICOutputFn fn, void *opaque)
{
	p->out_fn = fn;
	p->out_opaque = opaque;
}

bool OpenPICOutputAsserted(const OpenPICDevice *p)
{
	return p->out_asserted != 0;
}

// ---------------------------------------------------------------------------
// Input lines

void OpenPICRaiseInput(OpenPICDevice *p, unsigned n)
{
	if (n >= OPENPIC_NUM_SRC) {
		p->bad_inputs++;
		latch_warn(p, "openpic: Raise/LowerInput source out of range (>= 64)");
		return;
	}
	p->raises[n]++;
	set_input(p, (int)n, 1);
}

void OpenPICLowerInput(OpenPICDevice *p, unsigned n)
{
	if (n >= OPENPIC_NUM_SRC) {
		p->bad_inputs++;
		latch_warn(p, "openpic: Raise/LowerInput source out of range (>= 64)");
		return;
	}
	p->lowers++;
	set_input(p, (int)n, 0);
}

// ---------------------------------------------------------------------------
// IACK / EOI / CTPR (openpic_iack :1056, openpic_cpu_write_internal :965)

static uint32_t do_iack(OpenPICDevice *p)
{
	uint32_t retval;
	int irq, prio;

	p->iacks_total++;
	set_output(p, false);                     // oracle lowers unconditionally

	irq = queue_next(p, p->raised_bits, &prio);
	if (irq < 0) {
		p->iacks_spurious++;
		return p->spve;                       // nothing pending
	}

	if (!(p->ivpr[irq] & OPENPIC_IVPR_ACTIVITY) || !(IVPR_PRIO(p->ivpr[irq]) > p->ctpr)) {
		// "bad raised IRQ": raised but not deliverable (e.g. gated by CTPR).
		// Oracle error_report path — we latch instead (§2g) and return SPVE.
		p->iacks_bad++;
		latch_warn(p, "openpic: IACK found a raised-but-undeliverable source");
		update_irq(p, irq);
		retval = p->spve;
	} else {
		p->servicing_bits |= 1ull << irq;     // enter servicing
		retval = p->ivpr[irq] & OPENPIC_VECTOR_MASK;
		p->iacks[irq]++;
		if (!(p->first_iack_seen & (1ull << irq))) {   // Q8 tripwire record
			p->first_iack_seen |= 1ull << irq;
			p->first_iack_vec[irq] = (uint8_t)retval;
		}
	}

	if (!p->level[irq]) {
		// edge-sensitive: consumed by IACK (runs on the bad path too, oracle)
		p->ivpr[irq] &= ~OPENPIC_IVPR_ACTIVITY;
		p->pending[irq] = 0;
		p->raised_bits &= ~(1ull << irq);
	}
	// (IPI/timer multicast block: absent on this model)

	return retval;
}

static void do_eoi(OpenPICDevice *p)
{
	int s_irq, s_prio, n_irq, n_prio;

	s_irq = queue_next(p, p->servicing_bits, &s_prio);
	if (s_irq < 0) {
		p->eois_empty++;                      // EOI with nothing in service
		return;
	}

	p->servicing_bits &= ~(1ull << s_irq);
	p->eois[s_irq]++;

	queue_next(p, p->servicing_bits, &s_prio);          // next in service
	n_irq = queue_next(p, p->raised_bits, &n_prio);     // queued interrupts
	if (n_irq >= 0 && n_prio > s_prio) {
		// NOTE: the oracle's EOI re-raise does NOT re-check CTPR — matched.
		set_output(p, true);
	}
}

/* CTPR write (oracle: openpic_cpu_write_internal :965, the 0x80 case).
 *
 * RECOMPUTE DIVERGENCE NOTE (the b86449c9 review minor, owed to W2-3 — the
 * equivalence analysis, written down): QEMU's CTPR path consults the CACHED
 * queue priorities it maintains incrementally (dst->raised.priority /
 * dst->servicing.priority, kept current by IRQ_check after every queue
 * mutation):
 *     if (dst->raised.priority <= dst->ctpr)        -> lower output
 *     else if (dst->raised.priority > dst->servicing.priority) -> raise
 * This model keeps no cached priorities; it RECOMPUTES both via queue_next()
 * (a full scan of raised_bits/servicing_bits). Equivalent by analysis:
 * queue_next computes exactly the value IRQ_check caches — the highest IVPR
 * priority over the same membership bits (and -1 when empty, matching the
 * oracle's reset/empty priority) — and every mutation of raised_bits/
 * servicing_bits in this file goes through paths that leave the bits
 * authoritative. So cached-vs-recomputed cannot diverge unless the bit sets
 * themselves diverge, which would be a bug in BOTH representations. The
 * comparison chain below is then literal: raised <= ctpr lowers, else
 * raised > servicing raises, else no change — oracle-exact. */
static void write_ctpr(OpenPICDevice *p, uint32_t val)
{
	int raised_prio, servicing_prio;

	p->ctpr = (int32_t)(val & 0xFu);
	queue_next(p, p->raised_bits, &raised_prio);
	queue_next(p, p->servicing_bits, &servicing_prio);
	if (raised_prio <= p->ctpr)
		set_output(p, false);
	else if (raised_prio > servicing_prio)
		set_output(p, true);
}

// ---------------------------------------------------------------------------
// CPU bank (CPU0 only; reg = offset & 0xFF0 within the per-CPU window)

static uint32_t cpu_read(OpenPICDevice *p, uint32_t reg)
{
	switch (reg) {
	case 0x80:  return (uint32_t)p->ctpr;     // CTPR
	case 0x90:  return 0;                     // WHOAMI (CPU0)
	case 0xA0:  return do_iack(p);            // IACK
	case 0xB0:  return 0;                     // EOI reads as 0
	case 0x40: case 0x50: case 0x60: case 0x70:   // IPIDR: absent device
		p->ipi_touches++;
		latch_warn(p, "openpic: IPI register touched (absent device)");
		return 0xFFFFFFFFu;
	default:
		p->unknown_accesses++;
		latch_warn(p, "openpic: unknown CPU-bank register");
		return 0xFFFFFFFFu;
	}
}

static void cpu_write(OpenPICDevice *p, uint32_t reg, uint32_t val)
{
	switch (reg) {
	case 0x80:  write_ctpr(p, val); break;    // CTPR
	case 0x90:  break;                        // WHOAMI read-only
	case 0xA0:  break;                        // IACK read-only
	case 0xB0:  do_eoi(p); break;             // EOI
	case 0x40: case 0x50: case 0x60: case 0x70:   // IPIDR: absent device
		p->ipi_touches++;
		latch_warn(p, "openpic: IPI register touched (absent device)");
		break;
	default:
		p->unknown_accesses++;
		latch_warn(p, "openpic: unknown CPU-bank register");
		break;
	}
}

// ---------------------------------------------------------------------------
// Global bank

static uint32_t glb_read(OpenPICDevice *p, uint32_t off)
{
	if (off & 0xFu) {
		p->unknown_accesses++;
		latch_warn(p, "openpic: unaligned global-bank access");
		return 0xFFFFFFFFu;
	}
	switch (off) {
	case 0x00:    return 0xFFFFFFFFu;         // BRR1 (KeyLargo brr1 = -1)
	case 0x80: case 0x90: case 0xA0: case 0xB0:
	case 0x40: case 0x50: case 0x60: case 0x70:
		return cpu_read(p, off);              // per-CPU alias (current = CPU0)
	case 0x1000:  return OPENPIC_FRR_VALUE;   // FRR
	case 0x1020:  return p->gcr;              // GCR
	case 0x1080:  return 0;                   // VIR (VIR_GENERIC)
	case 0x1090:  return 0;                   // PIR reads as 0
	case 0x10A0: case 0x10B0: case 0x10C0: case 0x10D0:   // IPI_IVPR: absent
		p->ipi_touches++;
		latch_warn(p, "openpic: IPI register touched (absent device)");
		return 0xFFFFFFFFu;
	case 0x10E0:  return p->spve;             // SPVE
	default:
		p->unknown_accesses++;
		latch_warn(p, "openpic: unknown global-bank register");
		return 0xFFFFFFFFu;
	}
}

static void glb_write(OpenPICDevice *p, uint32_t off, uint32_t val)
{
	if (off & 0xFu) {
		p->unknown_accesses++;
		latch_warn(p, "openpic: unaligned global-bank access");
		return;
	}
	switch (off) {
	case 0x00:    break;                      // BRR1 read-only
	case 0x80: case 0x90: case 0xA0: case 0xB0:
	case 0x40: case 0x50: case 0x60: case 0x70:
		cpu_write(p, off, val);               // per-CPU alias (current = CPU0)
		break;
	case 0x1000:  break;                      // FRR read-only
	case 0x1020:                              // GCR
		if (val & OPENPIC_GCR_RESET) {
			reset_registers(p);
			p->gcr_resets++;
		} else {
			p->gcr = (p->gcr & ~OPENPIC_GCR_MODE_MIXED) |
			         ((uint32_t)val & OPENPIC_GCR_MODE_MIXED);
		}
		break;
	case 0x1080:  break;                      // VIR read-only
	case 0x1090:                              // PIR: CPU-reset output unmodeled
		p->pir_writes++;
		latch_warn(p, "openpic: PIR write (CPU soft-reset unmodeled)");
		break;
	case 0x10A0: case 0x10B0: case 0x10C0: case 0x10D0:   // IPI_IVPR: absent
		p->ipi_touches++;
		latch_warn(p, "openpic: IPI register touched (absent device)");
		break;
	case 0x10E0:                              // SPVE
		p->spve = (uint32_t)val & OPENPIC_VECTOR_MASK;
		break;
	default:
		p->unknown_accesses++;
		latch_warn(p, "openpic: unknown global-bank register");
		break;
	}
}

// ---------------------------------------------------------------------------
// Source bank: idx = (off - SRC_START) >> 5; reg = off & 0x1F
// (0x00 IVPR, 0x10 IDR; 0x18 ILR is FSL-only — unknown here)

static uint32_t src_read(OpenPICDevice *p, uint32_t off)
{
	uint32_t idx = (off - SRC_START) >> 5;
	uint32_t reg = off & 0x1Fu;

	if (idx >= OPENPIC_NUM_SRC) {
		// QEMU stores these inertly (src[] spans 264); absent here (header note)
		if (idx < OPENPIC_NUM_SRC + 4) {      // the 4 KeyLargo IPI sources
			p->ipi_touches++;
			latch_warn(p, "openpic: IPI register touched (absent device)");
		} else {
			p->unknown_accesses++;
			latch_warn(p, "openpic: source index out of range (>= 64)");
		}
		return 0xFFFFFFFFu;
	}
	switch (reg) {
	case 0x00:  return p->ivpr[idx];
	case 0x10:  return p->idr[idx];
	default:
		p->unknown_accesses++;
		latch_warn(p, "openpic: unknown source-bank register");
		return 0xFFFFFFFFu;
	}
}

static void src_write(OpenPICDevice *p, uint32_t off, uint32_t val)
{
	uint32_t idx = (off - SRC_START) >> 5;
	uint32_t reg = off & 0x1Fu;

	if (idx >= OPENPIC_NUM_SRC) {
		if (idx < OPENPIC_NUM_SRC + 4) {
			p->ipi_touches++;
			latch_warn(p, "openpic: IPI register touched (absent device)");
		} else {
			p->unknown_accesses++;
			latch_warn(p, "openpic: source index out of range (>= 64)");
		}
		return;
	}
	switch (reg) {
	case 0x00:  write_ivpr(p, (int)idx, val); break;
	case 0x10:  write_idr(p, (int)idx, val); break;
	default:
		p->unknown_accesses++;
		latch_warn(p, "openpic: unknown source-bank register");
		break;
	}
}

// ---------------------------------------------------------------------------
// MMIODevice-shaped entry points

uint64_t OpenPICRead(void *opaque, uint32_t addr, unsigned size)
{
	OpenPICDevice *p = (OpenPICDevice *)opaque;
	uint32_t off = addr - p->base;

	p->reads++;
	if (size != 4 || off >= OPENPIC_REGION_SPAN) {
		p->unknown_accesses++;
		latch_warn(p, "openpic: non-32-bit or out-of-span access");
		return 0xFFFFFFFFu;
	}
	if (off < GLB_END)
		return glb_read(p, off);
	if (off < TMR_END) {                      // timer bank: absent device
		p->timer_touches++;
		latch_warn(p, "openpic: timer-bank register touched (absent device)");
		return 0xFFFFFFFFu;
	}
	if (off >= SRC_START && off < SRC_END)
		return src_read(p, off);
	if (off >= CPU_START) {
		uint32_t idx = (off & 0x1F000u) >> 12;    // per-CPU window
		if (idx != 0) {                       // CPU1+ invalid (single CPU)
			p->unknown_accesses++;
			latch_warn(p, "openpic: non-CPU0 per-CPU bank access");
			return 0xFFFFFFFFu;
		}
		if (off & 0xFu) {
			p->unknown_accesses++;
			latch_warn(p, "openpic: unaligned CPU-bank access");
			return 0xFFFFFFFFu;
		}
		return cpu_read(p, off & 0xFF0u);
	}
	p->unknown_accesses++;                    // inter-bank hole
	latch_warn(p, "openpic: access into an unmapped hole");
	return 0xFFFFFFFFu;
}

void OpenPICWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value)
{
	OpenPICDevice *p = (OpenPICDevice *)opaque;
	uint32_t off = addr - p->base;
	uint32_t val = (uint32_t)value;

	p->writes++;
	if (size != 4 || off >= OPENPIC_REGION_SPAN) {
		p->unknown_accesses++;
		latch_warn(p, "openpic: non-32-bit or out-of-span access");
		return;
	}
	if (off < GLB_END) {
		glb_write(p, off, val);
		return;
	}
	if (off < TMR_END) {                      // timer bank: absent device
		p->timer_touches++;
		latch_warn(p, "openpic: timer-bank register touched (absent device)");
		return;
	}
	if (off >= SRC_START && off < SRC_END) {
		src_write(p, off, val);
		return;
	}
	if (off >= CPU_START) {
		uint32_t idx = (off & 0x1F000u) >> 12;
		if (idx != 0) {
			p->unknown_accesses++;
			latch_warn(p, "openpic: non-CPU0 per-CPU bank access");
			return;
		}
		if (off & 0xFu) {
			p->unknown_accesses++;
			latch_warn(p, "openpic: unaligned CPU-bank access");
			return;
		}
		cpu_write(p, off & 0xFF0u, val);
		return;
	}
	p->unknown_accesses++;                    // inter-bank hole
	latch_warn(p, "openpic: access into an unmapped hole");
}

// ---------------------------------------------------------------------------
// Formatters (safe threads only — never bus paths)

size_t OpenPICFormatStats(const OpenPICDevice *p, char *buf, size_t buflen)
{
	uint64_t raises_total = 0, iacks_src = 0, eois_total = 0;
	for (int i = 0; i < OPENPIC_NUM_SRC; i++) {
		raises_total += p->raises[i];
		iacks_src += p->iacks[i];
		eois_total += p->eois[i];
	}
	int n = snprintf(buf, buflen,
	    "reads=%llu writes=%llu raises=%llu lowers=%llu "
	    "iacks=%llu/%llu spurious=%llu bad=%llu eois=%llu eoi_empty=%llu "
	    "out=%u out_raises=%llu out_lowers=%llu gcr_resets=%llu "
	    "bad_inputs=%llu tmr=%llu ipi=%llu pir_w=%llu unknown=%llu",
	    (unsigned long long)p->reads, (unsigned long long)p->writes,
	    (unsigned long long)raises_total, (unsigned long long)p->lowers,
	    (unsigned long long)iacks_src, (unsigned long long)p->iacks_total,
	    (unsigned long long)p->iacks_spurious, (unsigned long long)p->iacks_bad,
	    (unsigned long long)eois_total, (unsigned long long)p->eois_empty,
	    (unsigned)p->out_asserted,
	    (unsigned long long)p->out_raises, (unsigned long long)p->out_lowers,
	    (unsigned long long)p->gcr_resets, (unsigned long long)p->bad_inputs,
	    (unsigned long long)p->timer_touches, (unsigned long long)p->ipi_touches,
	    (unsigned long long)p->pir_writes, (unsigned long long)p->unknown_accesses);
	return (n < 0) ? 0 : (size_t)n;
}

// --- Wave-2 W2-3: registered-diag-instance formatters (header contract) ----

static OpenPICDevice *g_diag_pic;   // prod bring-up registers it (gated on SS_NW_PIC)

void OpenPICRegisterDiagInstance(OpenPICDevice *p)
{
	g_diag_pic = p;
}

size_t OpenPICFormatStatsRegistered(char *buf, size_t buflen)
{
	return g_diag_pic ? OpenPICFormatStats(g_diag_pic, buf, buflen) : 0;
}

size_t OpenPICFormatFirstIACKsRegistered(char *buf, size_t buflen)
{
	return g_diag_pic ? OpenPICFormatFirstIACKs(g_diag_pic, buf, buflen) : 0;
}

size_t OpenPICFormatBriefRegistered(char *buf, size_t buflen)
{
	const OpenPICDevice *p = g_diag_pic;
	if (!p)
		return 0;
	uint64_t raises_total = 0, iacks_src = 0;
	for (int i = 0; i < OPENPIC_NUM_SRC; i++) {
		raises_total += p->raises[i];
		iacks_src += p->iacks[i];
	}
	int n = snprintf(buf, buflen, " pic=out:%u/r:%llu/i:%llu",
	                 (unsigned)p->out_asserted,
	                 (unsigned long long)raises_total,
	                 (unsigned long long)iacks_src);
	return (n < 0) ? 0 : (size_t)n;
}

size_t OpenPICFormatFirstIACKs(const OpenPICDevice *p, char *buf, size_t buflen)
{
	size_t pos = 0;
	for (int i = 0; i < OPENPIC_NUM_SRC && pos + 1 < buflen; i++) {
		if (!(p->first_iack_seen & (1ull << i)))
			continue;
		int n = snprintf(buf + pos, buflen - pos, "%ssrc=0x%02x vec=0x%02x",
		                 pos ? " " : "", i, p->first_iack_vec[i]);
		if (n < 0)
			break;
		pos += (size_t)n;
		if (pos >= buflen)
			pos = buflen - 1;
	}
	return pos;
}
