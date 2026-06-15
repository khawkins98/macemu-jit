/*
 *  dev_openpic.h - OpenPIC (KeyLargo MPIC) interrupt controller model
 *  (Machine Layer M3b Wave 2, stop-rule disposition: PURE MODULE + UNIT TESTS
 *  ONLY).  The EXC_EXTERNAL wiring — bus registration at the Core99 region,
 *  VIA/SCC assertion edges, the M3a delivery-hook extension — is DEFERRED,
 *  gated on the W2.0 NK-handler recon (M3b plan rev 2 C1).  Nothing binds
 *  this module yet; OpenPICBindOutput is the future wiring point.
 *
 *  Behavioral REIMPLEMENTATION (not a code port) against the QEMU oracle, per
 *  the donor study verdict (docs/planning/machine/M3-PIC-CUDA-DONOR-STUDY.md
 *  §5.3) and backport hygiene:
 *    - QEMU hw/intc/openpic.c + include/hw/ppc/openpic.h
 *      @ de5d8bfd6105d3dd3ae668df9762df244a6d1506
 *      (OPENPIC_MODEL_KEYLARGO realize config, openpic_reset, the
 *      glb/src/cpu register dispatch, IRQ_local_pipe/openpic_update_irq
 *      raise semantics, openpic_iack/EOI/CTPR protocol, IVPR write mask,
 *      KEYLARGO_* capacity constants: 64 ext sources, 4 IPI, 0 timers).
 *
 *  Region (donor study §1, resolved Q6): MacIO BAR + 0x40000, span 0x40000
 *  => guest-physical 0xF3040000..0xF307FFFF on Core99.  Sub-banks:
 *    glb 0x0..0x10F0, tmr 0x10F0..0x1310, src 0x10000..0x12000 (256 x 0x20),
 *    cpu 0x20000.. (per-CPU stride 0x1000; CPU0 only here).
 *
 *  Input numbers (donor study §2, resolved Q8 — QEMU-oracle values, macio.h
 *  lines 48-51 @ de5d8bfd…, no independent corroboration): VIA-Cuda 0x19,
 *  ESCC ch B 0x24, ESCC ch A 0x25.  The Q8 tripwire: the FIRST IACK delivered
 *  per source is recorded (source + vector) and emitted via
 *  OpenPICFormatFirstIACKs so a live boot's first deliveries can be checked
 *  against these assignments.
 *
 *  Scope (donor study §5.2 boot-critical subset; YAGNI beyond it):
 *    - Global: BRR1 (reads 0xFFFFFFFF, KeyLargo brr1=-1), FRR (read-only
 *      0x003F0002 = 63 sources-minus-1 <<16 | 0 cpus-minus-1 <<8 | VID 2),
 *      GCR (reset bit performs a full register reset, guest then reads 0 —
 *      QEMU completes reset synchronously; mode field masked to
 *      GCR_MODE_MIXED), VIR (0), PIR (reads 0; writes latched loud — the
 *      CPU-reset side effect is unmodeled), SPVE (8-bit), per-CPU aliases
 *      at glb+0x80..0xB0 (CTPR/WHOAMI/IACK/EOI for the current CPU = CPU0).
 *    - Source bank: per-source IVPR (write mask = MASK|PRIORITY|SENSE|
 *      POLARITY|vector(0xFF); ACTIVITY bit 30 read-only, maintained by the
 *      model; MODE bit 29 is set in the reset value 0xA0000000 but NOT
 *      writable — any guest IVPR write clears it, oracle-exact) + IDR
 *      (masked to bit 0, single CPU).  SENSE bit selects level (1) vs edge
 *      (0) triggering; POLARITY is stored but unused (QEMU stores it and
 *      never acts on it — openpic_set_irq consults only src->level).
 *    - CPU bank (CPU0): CTPR (4-bit task priority, reset 15 => nothing
 *      deliverable until the guest lowers it), WHOAMI (0), IACK (returns the
 *      highest-priority pending-and-unmasked vector, moves it pending ->
 *      in-service, returns SPVE when none qualifies; edge sources clear on
 *      IACK, level sources stay pending until the input is lowered), EOI
 *      (retires the highest-priority in-service source; re-raises the output
 *      if something deliverable remains — note the oracle's EOI re-raise
 *      path does NOT re-check CTPR, only the servicing priority; we match).
 *    - Inputs: OpenPICRaiseInput/OpenPICLowerInput per source (edge: lower is
 *      a no-op; level: tracks the line).  Output: a single INT line to CPU0 —
 *      OpenPICOutputAsserted predicate + OpenPICBindOutput callback seam
 *      (fired on TRANSITIONS only; QEMU's redundant qemu_irq_raise calls are
 *      level-idempotent, transitions are the observable).
 *    - Timers + IPI + multi-CPU: ABSENT-DEVICE-CONSISTENT (M3b plan rev:
 *      loud-log, NOT abort).  Reads return 0xFFFFFFFF (the oracle's own
 *      default-path retval for unhandled offsets), writes are ignored,
 *      first touch latches a loud warning + per-class counters.  Documented
 *      divergence from QEMU: QEMU maps a live tmr bank (TFRR reads 4160000)
 *      and IPI registers even on KeyLargo; real KeyLargo has KEYLARGO_MAX_TMR
 *      = 0 ("Timers don't exist but this makes the code happy", openpic.h:41)
 *      and Mac OS 9 is uniprocessor — absent is the higher-fidelity choice
 *      and the tripwire tells us if the guest disagrees.  Same treatment for
 *      src-bank indices >= 64 (QEMU stores them inertly; we latch loud).
 *    - Unknown registers / inter-bank holes / unaligned (offset & 0xF inside
 *      glb/cpu) / non-32-bit accesses: all-ones reads, ignored writes,
 *      loud-latch (the absent-device idiom).
 *
 *  Endianness note — the W2-3 byte-lane DECISION ([STATIC-oracle], Wave-2
 *  plan rev 2 F16): QEMU maps KeyLargo with the little-endian ops table
 *  (list_le).  This module speaks NATURAL (logical) register values; the
 *  MMIO bus speaks ARCHITECTURAL values (what the BE guest's lwz yields).
 *  A BE lwz of an LE-mapped natural-value register yields bswap32(value),
 *  so the bus trampolines in main_unix VALUE-SWAP 32-bit accesses in both
 *  directions (openpic_bus_read/write).  FALSIFIER (documented per F16; the
 *  guest has never read the PIC, so the oracle decides until live evidence
 *  exists): the first live guest FRR read must observe 0x02003F00
 *  (= bswap32(0x003F0002)); observing 0x003F0002 falsifies the swap —
 *  guest evidence wins, flip to natural pass-through and record the
 *  falsification in EE-CHAIN-RECON.md.  The first FRR read is logged loud
 *  at the trampoline for exactly this purpose.
 *
 *  House rules (MACHINE-LAYER-PLAN §2g): no stdio, no malloc on any
 *  read/write/raise path — warnings are latched (OpenPICTakePendingWarning,
 *  same contract as VIA/Cuda) and formatters snprintf into caller buffers on
 *  safe threads only.  The module is lock-free; the future bus region lock
 *  covers it (same pattern as SCC/VIA).  Telemetry fields live at the end of
 *  the struct; any future fields are APPENDED LAST (struct-layout rule).
 */

#ifndef DEV_OPENPIC_H
#define DEV_OPENPIC_H

#include <stddef.h>
#include <stdint.h>

// Region (resolved Q6): Core99 guest-physical base + span.
#define OPENPIC_CORE99_BASE 0xF3040000u
#define OPENPIC_REGION_SPAN 0x40000u

// Capacity (QEMU KEYLARGO_* constants @ de5d8bfd…, openpic.h:34-42)
#define OPENPIC_NUM_SRC     64          // external sources (KEYLARGO_MAX_EXT)
#define OPENPIC_VECTOR_MASK 0xFFu       // KeyLargo vector_mask

// FRR: ((nb_irqs-1)<<16) | ((nb_cpus-1)<<8) | VID_REVISION_1_2
#define OPENPIC_FRR_VALUE   0x003F0002u

// PIC input numbers (resolved Q8 — QEMU macio.h NEWWORLD_* values)
#define OPENPIC_IRQ_VIA_CUDA 0x19
#define OPENPIC_IRQ_ESCC_B   0x24
#define OPENPIC_IRQ_ESCC_A   0x25
// M7 Task B-2 (interrupt-injection sign-off shape (i)): the HOST interrupt
// source's reserved input.  The real platform has NO PIC input for the
// decrementer/timer — the DEC is a CPU-internal exception (PowerPC
// architecture), and KeyLargo's MPIC has zero timer sources
// (KEYLARGO_MAX_TMR = 0, "Timers don't exist but this makes the code happy",
// QEMU openpic.h:41 @ de5d8bfd…) — so this is a documented RESERVED choice:
// input 0x3F, the top of the 64-source KeyLargo external bank, unassigned in
// the Q8 device map above and in QEMU's NewWorld macio realize assignments at
// the pinned SHA.  Vector = input number (identity — the same convention the
// [DIAG-FORCED] bring-up uses); 0x3F is also the LAST in-range vector for the
// NK fallback's IACK leg (vector < 0x40, [STATIC] rom901.bin 0x50326070).
// Constant only — no model behavior change rides this define.
#define OPENPIC_IRQ_HOST     0x3F

// IVPR bits (guest-visible; QEMU openpic.h IVPR_* @ de5d8bfd…)
#define OPENPIC_IVPR_MASK     0x80000000u   // 1 = source masked
#define OPENPIC_IVPR_ACTIVITY 0x40000000u   // read-only, model-maintained
#define OPENPIC_IVPR_MODE     0x20000000u   // set at reset, NOT guest-writable
#define OPENPIC_IVPR_POLARITY 0x00800000u   // stored, unused (oracle-exact)
#define OPENPIC_IVPR_SENSE    0x00400000u   // 1 = level-triggered, 0 = edge
#define OPENPIC_IVPR_PRIO_SHIFT 16
#define OPENPIC_IVPR_PRIO_MASK  0x000F0000u
#define OPENPIC_IVPR_RESET    (OPENPIC_IVPR_MASK | OPENPIC_IVPR_MODE)  // 0xA0000000

// GCR bits
#define OPENPIC_GCR_RESET      0x80000000u
#define OPENPIC_GCR_MODE_MIXED 0x20000000u  // KeyLargo mpic_mode_mask

// Output callback (the FUTURE wiring point — fires on output transitions,
// under whatever lock the caller of the mutating entry point held).
typedef void (*OpenPICOutputFn)(void *opaque, bool asserted);

struct OpenPICDevice {
	uint32_t base;                       // guest-physical region base
	// --- global registers ---
	uint32_t gcr;                        // mode bits only (reset completes sync)
	uint32_t spve;                       // spurious vector (8-bit)
	// --- per-source registers (64 external sources) ---
	uint32_t ivpr[OPENPIC_NUM_SRC];      // incl. ACTIVITY bit (read-only)
	uint32_t idr[OPENPIC_NUM_SRC];       // masked to bit 0 (CPU0)
	uint8_t  pending[OPENPIC_NUM_SRC];   // input line / edge latch
	uint8_t  level[OPENPIC_NUM_SRC];     // cached !!(ivpr & SENSE)
	// --- CPU0 destination state ---
	int32_t  ctpr;                       // current task priority (reset 15)
	uint64_t raised_bits;                // pending-for-delivery queue
	uint64_t servicing_bits;             // in-service queue
	uint8_t  out_asserted;               // the single INT output line
	// --- injected output seam (bound AFTER reset; reset clears it) ---
	OpenPICOutputFn out_fn;
	void *out_opaque;
	// --- telemetry (plain counters; §2g capture-only, emission elsewhere) ---
	uint64_t reads, writes;
	uint64_t raises[OPENPIC_NUM_SRC];    // OpenPICRaiseInput calls per source
	uint64_t lowers;                     // OpenPICLowerInput calls (total)
	uint64_t iacks[OPENPIC_NUM_SRC];     // successful IACK deliveries per source
	uint64_t eois[OPENPIC_NUM_SRC];      // EOI retirements per source
	uint64_t iacks_total;                // all IACK reads (incl. spurious/bad)
	uint64_t iacks_spurious;             // IACK with nothing raised -> SPVE
	uint64_t iacks_bad;                  // raised-but-undeliverable -> SPVE
	uint64_t eois_empty;                 // EOI with nothing in service
	uint64_t out_raises, out_lowers;     // output TRANSITIONS
	uint64_t gcr_resets;                 // GCR reset-bit soft resets
	uint64_t bad_inputs;                 // Raise/LowerInput with n >= 64
	uint64_t timer_touches;              // tmr-bank accesses (absent device)
	uint64_t ipi_touches;                // IPI register accesses (absent device)
	uint64_t pir_writes;                 // PIR writes (CPU-reset unmodeled)
	uint64_t unknown_accesses;           // holes / reserved / unaligned / bad size
	// Q8 tripwire: first IACK per source (vector recorded once)
	uint64_t first_iack_seen;            // bitmask of sources ever IACKed
	uint8_t  first_iack_vec[OPENPIC_NUM_SRC];
	// warning latch (one pending at a time; taking it re-arms — VIA pattern)
	uint8_t  warned;                     // any warning ever latched (telemetry)
	const char *warn_what;               // static string, pending until taken
	// (append any future fields HERE, last)
};

// Power-on reset: zeroes everything (including the output binding and
// telemetry), sets base, applies the KeyLargo reset register state.
extern void OpenPICReset(OpenPICDevice *p, uint32_t base);

// Bind the output seam AFTER OpenPICReset.  fn fires on every output
// transition (asserted edge + deasserted edge).  NOT wired anywhere yet.
extern void OpenPICBindOutput(OpenPICDevice *p, OpenPICOutputFn fn, void *opaque);

// The MMIODevice-shaped trampolines (opaque = OpenPICDevice*; addr is the
// absolute guest address; only size == 4 is a valid register access).
extern uint64_t OpenPICRead(void *opaque, uint32_t addr, unsigned size);
extern void OpenPICWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value);

// Device input lines (n = PIC input number, e.g. OPENPIC_IRQ_VIA_CUDA).
// Edge sources (IVPR sense = 0): Raise latches pending, Lower is a no-op.
// Level sources (sense = 1): the calls track the line level.
// n >= OPENPIC_NUM_SRC: counted + latched, otherwise ignored (no abort).
extern void OpenPICRaiseInput(OpenPICDevice *p, unsigned n);
extern void OpenPICLowerInput(OpenPICDevice *p, unsigned n);

// The single-line "interrupt output asserted?" predicate.
extern bool OpenPICOutputAsserted(const OpenPICDevice *p);

// Return-and-clear the pending one-shot warning (static string), or NULL.
// Same emission contract as VIATakePendingWarning (safe threads only).
extern const char *OpenPICTakePendingWarning(OpenPICDevice *p);

// Format telemetry as "reads=N writes=N iacks=N ..." into the caller's buffer
// (snprintf; no FILE*).  Returns chars written.  Safe threads only.
extern size_t OpenPICFormatStats(const OpenPICDevice *p, char *buf, size_t buflen);

// Format the Q8 tripwire record as "src=0x19 vec=0x?? ..." (one entry per
// source that has been IACKed, in source order).  Returns chars written
// (0 if no IACK was ever delivered).  Safe threads only.
extern size_t OpenPICFormatFirstIACKs(const OpenPICDevice *p, char *buf, size_t buflen);

// --- Wave-2 W2-3: registered-diag-instance formatters (the VIA/Cuda
// RegisterDiagInstance idiom) — crash-path/heartbeat telemetry without the
// consumer holding the device pointer.  All return 0 when no instance is
// registered (paravirtual / gated-off boots stay byte-identical).  Unlocked
// counter loads (aligned 64-bit, single-copy-atomic on AArch64 — worst case
// a slightly stale count; benign for telemetry).  Safe threads only.
extern void OpenPICRegisterDiagInstance(OpenPICDevice *p);
extern size_t OpenPICFormatStatsRegistered(char *buf, size_t buflen);
extern size_t OpenPICFormatFirstIACKsRegistered(char *buf, size_t buflen);
// Compact heartbeat form: " pic=out:O/r:RAISES/i:IACKS" (leading space).
extern size_t OpenPICFormatBriefRegistered(char *buf, size_t buflen);

#endif
