/*
 *  sheepshaver_glue.cpp - Glue Kheperix CPU to SheepShaver CPU engine interface
 *
 *  SheepShaver (C) 1997-2008 Christian Bauer and Marc Hellwig
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "xlowmem.h"
#include "emul_op.h"
#include "rom_patches.h"
#include "macos_util.h"
#include "machine_profile.h"
#include "mmio_bus.h"
#include "dev_cuda.h"
#include "dev_openpic.h"   // W2-3: crash-path [PIC] stats (registered-instance formatters)
#include "virt_clock.h"
#include "exc_core.h"
#include "block-alloc.hpp"
#include "sigsegv.h"
#include "vm_alloc.h"
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
#include "cpu/jit/aarch64/ppc-jit.h"
#endif
#include "cpu/ppc/ppc-cpu.hpp"
#include "cpu/ppc/ppc-operations.hpp"
#include "cpu/ppc/ppc-instructions.hpp"
#include "thunks.h"

// Used for NativeOp trampolines
#include "video.h"
#include "name_registry.h"
#include "serial.h"
#include "ether.h"
#include "timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/mman.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

/* MAP_FIXED_NOREPLACE is a Linux-only mmap flag (Linux >= 4.17). macOS and
   other BSDs lack it. Define it as a no-op (0) so the fixed-address hint is
   simply ignored; without MAP_FIXED the address hint becomes advisory-only
   and the kernel may place the mapping elsewhere.  The caller's retry-and-
   validate logic (low-4GB check) handles any mismatch, so behaviour is
   unchanged. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif

#ifdef USE_SDL_VIDEO
#include "my_sdl.h"
#endif

#if ENABLE_MON
#include "mon.h"
#include "mon_disass.h"
#endif

#define DEBUG 0
#include "debug.h"

extern "C" {
#include "dis-asm.h"
}

/* M3a Task 3: interrupt entry table — newworld profile only.
 * Default interrupt_entry = 0x50412b1c (probe-verified in M3A-ENTRY-TABLE.md).
 * syscall_entry default (NK-syscall-surface Task A; newworld DEFAULT since
 * Task C) = 0x50314ac0 — opt-out with SS_NW_SC_SURFACE=0 (polarity mirrors
 * SS_NW_MM_SWITCH; applied at table finalization in init_emul_ppc; the static
 * initializer below stays {intr, 0} so the opted-out abort-with-capture
 * baseline is byte-identical).
 * Override via SS_EXC_ENTRY=0xINT[,0xSC] for no-rebuild iteration.
 * Paravirtual never reads this table.
 *
 * CROSS-COPY ASYMMETRY (deliberate, recorded per plan rev 2 P-m5 +
 * M3A-ENTRY-TABLE.md "Syscall entry resolution" Q-S1): interrupt_entry points
 * at the STAGED copy (0x50412b1c = static 0x312b1c + 0x100000; Task-7-proven
 * delivery target), while syscall_entry points at the PRIMARY copy
 * (0x50314ac0 = static file 0x314ac0 + ROMBase, NO +0x100000) — because the
 * live syscall entry follows what the NK itself publishes: [KDP+0x390] =
 * 0x50314ac0 [PROBE✓], and the NK relocation base [KDP+0x64c] = 0x50310000.
 * Both copies are byte-identical at their probed anchors; reconciling the two
 * interrupt targets is explicitly out of this milestone's scope. */
#define NW_INTERRUPT_ENTRY_DEFAULT 0x50412b1cu  /* M3A-ENTRY-TABLE.md probe-verified */
/* Wave-2 W2-4 step 0 (EE-CHAIN-RECON.md D-3/D-4; coordinator sign-off item 2
 * GRANTED): the NK-PUBLISHED DEC (0x900) handler, [KDP+0x384] = 0x50313200
 * [PROBE✓ 2026-06-11 w2-4-step0 boot 1: [0x68ffe384]=0x50313200, siblings
 * [KDP+0x390]=0x50314ac0 / [KDP+0x374]=0x50314880 and [KDP+0x64c]=0x50310000
 * re-confirmed in the same probe]. PRIMARY copy per the publication precedent
 * (the sc/program/EXT rule: the live value follows what the NK publishes; the
 * old default's staged-copy asymmetry is retired on this path). Gated by
 * SS_NW_DEC_PUBLISHED — NEWWORLD DEFAULT since the M7 Task C cluster flip
 * (opt-out =0): gate ON re-points interrupt_entry here AND switches the DEC shim
 * to the 2-SPR shape (see the delivery hook) — 0x50313200 opens with the
 * SHARED save prologue 0x313d40 like sc/program/EXT (W2S-2 verdict: same
 * prologue, same EE-punch-through guard, same bounce exit; self-contained,
 * r9-free — retires the W2S-R1 r9 hazard and the cr6/cr7 flag-composition
 * hazards instead of probing them). SS_EXC_ENTRY precedence unchanged. */
#define NW_INTERRUPT_PUBLISHED_DEFAULT 0x50313200u  /* primary copy, NK-published [KDP+0x384] [PROBE✓] */
#define NW_SYSCALL_ENTRY_DEFAULT   0x50314ac0u  /* primary copy, NK-published [KDP+0x390] [PROBE✓] */
/* FE1F-service-surface Task A (plan rev 3): the program-interrupt (0x700) entry —
 * the NK's published 0x700 handler. PRIMARY copy like the syscall entry (the same
 * cross-copy note applies: the live value follows what the NK publishes,
 * [KDP+0x37c] = 0x50314700 [PROBE✓], = static file 0x314700 + ROMBase, NO
 * +0x100000; the interrupt entry's staged-copy asymmetry is recorded above).
 * NEWWORLD DEFAULT since Task C (acceptance battery green pre/post-flip);
 * opt-out with SS_NW_FE1F_SURFACE=0 (explicit-"0"-only, the SS_NW_SC_SURFACE
 * polarity precedent). */
#define NW_PROGRAM_ENTRY_DEFAULT   0x50314700u  /* primary copy, NK-published [KDP+0x37c] [PROBE✓] */
/* Wave-2 W2-3 (rev 2 F6 + Q-W2): the EXT (0x500) entry — the NK-published
 * external-interrupt handler, [KDP+0x374] = 0x50314880 [PROBE✓ syscall
 * milestone], primary copy (the same publication-precedent rule as syscall/
 * program). CONSUMED by ExcEnter(EXC_EXTERNAL) since W2-3 (the sanctioned
 * U12 flip); inert until an EXT source exists — the only live caller is the
 * delivery hook's EXT branch, reachable only when the SS_NW_PIC-gated PIC
 * output is wired AND asserted. Its shim is the sc/program 2-SPR shim
 * (EE-CHAIN-RECON.md §W2S-2 verdict), NOT the DEC KDP save shim. */
#define NW_EXTERNAL_ENTRY_DEFAULT  0x50314880u  /* primary copy, NK-published [KDP+0x374] [PROBE✓] */
ExcEntryTable g_exc_entry_table = { NW_INTERRUPT_ENTRY_DEFAULT, 0u, 0u,
                                    NW_EXTERNAL_ENTRY_DEFAULT };

/* Wave-2 W2-1 (plan rev 2 F1): the SS_EXC_ENTRY=0xINT[,0xSC[,0xEXT]] override
 * parse, SHARED between the boot path (init_emul_ppc table finalization below)
 * and the SS_TEST harness path (main_unix's harness gate returns before the
 * boot parse ever runs — the F1 fix is calling this from the harness knob
 * block in ss_run_one_vector). The no-comma form PRESERVES syscall_entry (the
 * P-M1 trap fix, carried verbatim — only an explicit ",0xSC" field overrides
 * it). W2-3: optional THIRD field overrides external_entry the same way (an
 * absent field preserves the default; an explicit ",...,0" clears it, which
 * restores the pre-W2-3 shared-entry fallback — used by the harness EXT
 * vectors to discriminate which entry a delivery consumed).
 * Returns true iff the env var was present and applied (the caller decides
 * what to log in the no-override case). */
static bool exc_entry_table_apply_env_override(void)
{
	const char *exc_env = getenv("SS_EXC_ENTRY");
	if (!(exc_env && exc_env[0]))
		return false;
	char *endp = NULL;
	uint32_t ie = (uint32_t)strtoul(exc_env, &endp, 0);
	if (endp && *endp == ',') {
		char *endp2 = NULL;
		g_exc_entry_table.syscall_entry =
			(uint32_t)strtoul(endp + 1, &endp2, 0);
		if (endp2 && *endp2 == ',')
			g_exc_entry_table.external_entry =
				(uint32_t)strtoul(endp2 + 1, NULL, 0);
	}
	g_exc_entry_table.interrupt_entry = ie;
	fprintf(stderr, "[EXC] entry table override (SS_EXC_ENTRY): "
	        "interrupt=0x%08x syscall=0x%08x external=0x%08x\n",
	        g_exc_entry_table.interrupt_entry,
	        g_exc_entry_table.syscall_entry,
	        g_exc_entry_table.external_entry);
	return true;
}

/* W2-4 step 0: the SS_NW_DEC_PUBLISHED gate, resolved once. DEFAULT ON since
 * the M7 Task C cluster flip (with SS_NW_EE_RISER + SS_NW_HOST_IRQ; battery
 * green pre/post-flip); opt-out with SS_NW_DEC_PUBLISHED=0 (explicit-"0"-only,
 * the SS_NW_SC_SURFACE polarity — restores the legacy KDP-shim route and the
 * 0x50412b1c entry default). Shared between the boot finalization (entry-table
 * default) and the delivery hook (shim shape) — the hook is also reachable on
 * the SS_TEST harness path, which never runs init_emul_ppc, so the gate must
 * not live only in the boot parse; run-exc.sh PINS the gate per lane
 * (BASE_ENV=0, H8=1) so lane contracts do not float with this default, and
 * ss_test_exc_knobs_apply warns on gate-on-without-override (pre-flip item 1).
 * CONTRACT: the gate selects the SHIM SHAPE as well as the entry default;
 * SS_EXC_ENTRY overrides the ENTRY VALUE only (precedence unchanged). An
 * override pointing back at the save-and-switch body 0x50412b1c therefore
 * needs the gate OFF to get its KDP shim. */
static bool exc_dec_published_enabled(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *e = getenv("SS_NW_DEC_PUBLISHED");
		cached = (e && strcmp(e, "0") == 0) ? 0 : 1;
	}
	return cached != 0;
}

/* M3a Task 4 telemetry: DEC delivery counters (newworld only — paravirtual never
 * runs the hook). CPU-thread-only writers (check_spcflags context, plan §2g), so
 * plain uint64_t is fine; readers (heartbeat, crash dump) run on the same thread
 * or post-mortem. Exposed via SheepExcStats(). */
static uint64_t exc_stat_delivered_dec  = 0;
static uint64_t exc_stat_deferred_ee    = 0;
static uint64_t exc_stat_deferred_depth = 0;
static uint64_t exc_stat_deferred_native = 0;	// M6a W2: deferred during native excursion ([XLM_RUN_MODE]!=0)
static uint64_t exc_stat_delivered_sc   = 0;	// NK-syscall-surface Task A (plan rev 2 P-M4): delivered sc count
static uint64_t exc_stat_delivered_program = 0;	// FE1F-service-surface Task A: delivered 0x700 (trap) count
static uint64_t exc_stat_delivered_ext  = 0;	// Wave-2 W2-3: delivered EXC_EXTERNAL count (7th field, appended LAST)

/* M7 item 2 (DEFER_NATIVE wake-up edge, INTERRUPT-INJECTION-RECON.md Q4 +
 * re-pin addendum): per-episode re-arm budget. Reset on any non-native
 * decision; spent budget = polling falls back to kick-driven. CPU-thread-only
 * like the stats above. The cap is sized >> the observed transient-window
 * length (~10^2-10^3 records) so window-exit delivery is unaffected. */
#define EXC_NATIVE_REARM_CAP 65536u
static uint32_t exc_native_rearm_used = 0;

/* --- Wave-2 W2-3: the EXC_EXTERNAL pending source (the OpenPIC output) -----
 *
 * F5 atomicity contract: the PIC's bound-output callback (main_unix) runs
 * under the PIC bus-region lock on WHATEVER thread mutated the PIC (the CPU
 * thread via guest MMIO faults, or the scheduler-pump thread via the
 * SS_SCC_RX_INJECT one-shot -> SCC lock -> PIC lock). The flag is therefore a
 * single-copy-atomic 32-bit word (never a plain bool), written with release
 * and read lock-free with acquire at the CPU-thread poll. The CPU kick on the
 * ASSERT edge (TriggerInterrupt — the DEC-expiry idiom) lives with the
 * callback in main_unix; a spurious kick is safe (the hook re-gates on real
 * machine state), a missed kick is not (no 60 Hz safety net on newworld).
 *
 * LEVEL-HELD semantics (rev 2 C1, BINDING): the hook never clears this —
 * only the PIC's deassert edge does (guest IACK/EOI/mask retiring the line,
 * or the source dropping). Mirrors OpenPICOutputAsserted exactly: main_unix
 * forwards BOTH edges.
 *
 * exc_ext_configured: set once at PIC bring-up (SS_NW_PIC on) or by the
 * SS_TEST_EXT_PENDING harness knob. Gates the exc= tuple's 7th field so
 * gated-off boots stay BYTE-IDENTICAL to the pre-W2-3 baseline class. */
static volatile uint32 exc_ext_pending_flag = 0;
static volatile uint32 exc_ext_configured = 0;
/* Tripwire counters (CPU-thread writers in the hook; the SetPending resets
 * run on the asserting thread — benign telemetry races, counts only). */
static uint64_t exc_ext_delivs_this_assert = 0;   // EXT deliveries since the last edge
static uint64_t exc_dec_delivs_while_ext  = 0;    // DEC deliveries with EXT pending, since last EXT delivery/edge
#define EXC_EXT_RUNAWAY_N    16   /* re-delivery runaway guard (rev 2 C1) */
#define EXC_EXT_STARVATION_N 64   /* U13 starvation tripwire (rev 2 tension 2) */

extern "C" int SheepExcExtPending(void)
{
	return __atomic_load_n(&exc_ext_pending_flag, __ATOMIC_ACQUIRE) != 0;
}

extern "C" int SheepExcExtConfigured(void)
{
	return __atomic_load_n(&exc_ext_configured, __ATOMIC_ACQUIRE) != 0;
}

extern "C" void SheepExcExtConfigure(void)
{
	__atomic_store_n(&exc_ext_configured, 1u, __ATOMIC_RELEASE);
}

extern "C" void SheepExcExtSetPending(int asserted)
{
	__atomic_store_n(&exc_ext_pending_flag, asserted ? 1u : 0u, __ATOMIC_RELEASE);
	/* Edge bookkeeping for the tripwires (telemetry-only, racy-benign). */
	exc_ext_delivs_this_assert = 0;
	exc_dec_delivs_while_ext = 0;
	/* First few edges to stderr for live triage (the DEC-delivery idiom).
	 * stdio caveat: the PIC callback path runs under bus locks but never on
	 * the Mach exception-handler thread for the SCC-inject lever (pump
	 * thread) and is bounded (<=6 lines) on the fault path — the macio-stub
	 * first-touch precedent. */
	static uint64_t edge_count = 0;
	if (++edge_count <= 6)
		fprintf(stderr, "[EXC] EXT pending %s (edge #%llu)\n",
		        asserted ? "ASSERTED" : "deasserted",
		        (unsigned long long)edge_count);
}

/* --- M7 Task A (interrupt-injection plan rev 2 A1 + Task 0 Q-I4(c)): the HOST
 * interrupt source — a DEDICATED deliver-once-per-assert-edge latch.
 *
 * THIRD semantics, deliberately beside the two existing ones:
 *   - DEC latch:        one-shot, cleared by the hook on delivery.
 *   - PIC EXT level:    LEVEL-HELD (rev 2 C1, BINDING) — the hook never clears
 *                       exc_ext_pending_flag; only the PIC deassert edge does.
 *                       C1 is UNTOUCHED by this latch: real PIC sources keep
 *                       level-held semantics on their own word.
 *   - HOST irq latch:   deliver-ONCE-per-assert-edge (this word). Armed by a
 *                       0->1 assert edge (SetInterruptFlag's newworld arm),
 *                       CONSUMED at EXT delivery (atomic exchange-0 on the CPU
 *                       thread), re-armed only by the next assert edge.
 *
 * Why not a level: a bare level-held host source is a CLOSED LIVELOCK at this
 * frontier — restart PC = block start (no progress across a delivery), all six
 * EE re-raise compose sites re-deliver on every rfi/mtmsr EE rise, and the
 * retirement site (OP_IRQ's ClearInterruptFlag, emul_op.cpp:841) is
 * HasMacStarted()-gated and unreachable pre-warm-start. Proven in vivo by
 * Task 0: 68,657,433 deliveries/40 s, runaway tripwire, guest frozen
 * (INTERRUPT-INJECTION-RECON.md "A1 livelock proven in vivo").
 *
 * A5 source composition: OR at the POLL site (deliver_pending_dec_exception +
 * the six EE re-raise compose sites) — this latch NEVER writes
 * exc_ext_pending_flag and the PIC path never writes this word, so the two
 * sources cannot clobber each other's edges.
 *
 * F5 atomicity: single-copy-atomic uint32; release store on the asserting
 * thread (host timer / tick threads via SetInterruptFlag), acquire load at the
 * CPU-thread poll, exchange for the edge/consume transitions. The
 * TriggerInterrupt kick on the assert edge lives with the caller (main_unix) —
 * a spurious kick is safe, a missed kick is not (no 60 Hz net on newworld).
 *
 * Counters are racy-benign telemetry (edges on asserting threads, consumed on
 * the CPU thread); exactly-once-per-edge acceptance compares edges vs consumed.
 * exc_host_irq_enabled gates ALL new output (the exc-tuple 7th-field idiom) so
 * gated-off boots stay byte-identical to the E4 baseline class. */
static volatile uint32 exc_host_irq_latch = 0;
static volatile uint32 exc_host_irq_enabled = 0;
static uint64_t exc_host_irq_edges = 0;      /* 0->1 assert edges */
static uint64_t exc_host_irq_consumed = 0;   /* latch consumptions at EXT delivery */
static uint64_t exc_host_irq_deasserts = 0;  /* level retired with latch still armed */

extern "C" int SheepExcHostIrqPending(void)
{
	return __atomic_load_n(&exc_host_irq_latch, __ATOMIC_ACQUIRE) != 0;
}

extern "C" int SheepExcHostIrqEnabled(void)
{
	return __atomic_load_n(&exc_host_irq_enabled, __ATOMIC_ACQUIRE) != 0;
}

extern "C" void SheepExcHostIrqConfigure(void)
{
	__atomic_store_n(&exc_host_irq_enabled, 1u, __ATOMIC_RELEASE);
	SheepExcExtConfigure();		/* exc= tuple gains the 7th field */
}

/* Assert edge. Returns 1 iff this call armed the latch (a true 0->1 edge —
 * the caller kicks the CPU thread on exactly those); an already-armed latch
 * is NOT a new edge (the pending delivery will service it). */
extern "C" int SheepExcHostIrqAssert(void)
{
	if (__atomic_exchange_n(&exc_host_irq_latch, 1u, __ATOMIC_ACQ_REL) != 0)
		return 0;
	exc_host_irq_edges++;
	/* Tripwire edge bookkeeping — same idiom as SheepExcExtSetPending
	 * (telemetry-only, racy-benign).
	 * M7 Task C pre-flip item 4 (Task-A review P3) — ACCEPTED-WITH-NOTE:
	 * these episode counters are SHARED with the PIC level source, so a host
	 * assert edge resets the PIC's runaway/starvation episode arithmetic and
	 * could mask a PIC re-delivery runaway. Accepted for the host-only
	 * default state (SS_NW_PIC's default stays HELD; in the co-armed TEST
	 * cluster the PIC's only live source IS this host line, so there is no
	 * independent PIC episode to mask). Before any DEFAULT state co-arms
	 * SS_NW_PIC with independent device sources (SCC 0x25 / VIA 0x19), split
	 * the episode counters per source — recorded in the M7 plan Task C
	 * dispositions. */
	exc_ext_delivs_this_assert = 0;
	exc_dec_delivs_while_ext = 0;
	static uint64_t hedge_count = 0;
	if (++hedge_count <= 6)
		fprintf(stderr, "[EXC] EXT pending ASSERTED (host-irq latch, edge #%llu)\n",
		        (unsigned long long)hedge_count);
	return 1;
}

/* Deassert edge: the level predicate (InterruptFlags != 0, Q-I4(a)) went
 * false with the latch still armed — retire it un-delivered. Pre-warm-start
 * this never fires (no route clears InterruptFlags before [0xcfc]=='WLSC'). */
extern "C" void SheepExcHostIrqDeassert(void)
{
	if (__atomic_exchange_n(&exc_host_irq_latch, 0u, __ATOMIC_ACQ_REL) != 0)
		exc_host_irq_deasserts++;
}

/* CPU-thread consume at EXT delivery (deliver-once-per-edge). */
static void exc_host_irq_consume(void)
{
	if (__atomic_exchange_n(&exc_host_irq_latch, 0u, __ATOMIC_ACQ_REL) != 0)
		exc_host_irq_consumed++;
}

/* Telemetry formatter for the term-dump/crash paths. Returns 0 (no output)
 * unless the SS_NW_HOST_IRQ bring-up (or the harness knob) enabled the
 * source — gated-off boots stay byte-identical. */
extern "C" int SheepExcHostIrqFormatStats(char *buf, int len)
{
	if (!SheepExcHostIrqEnabled())
		return 0;
	snprintf(buf, len, "edges=%llu consumed=%llu deasserts=%llu pending=%u",
	         (unsigned long long)exc_host_irq_edges,
	         (unsigned long long)exc_host_irq_consumed,
	         (unsigned long long)exc_host_irq_deasserts,
	         (unsigned)__atomic_load_n(&exc_host_irq_latch, __ATOMIC_ACQUIRE));
	return 1;
}

/* FE1F-service-surface Task C (the authorized fix-budget item, Task B record):
 * the SC delivered-print caps at 5 (live-triage idiom), which left selectors
 * #6+ unenumerated once the FE1F surface pushed delivered_sc past 5 (13 on the
 * Task-A/B boots, only #1-5 printed). Counters-for-counts (plan rev 2 P-m6):
 * per-selector delivery counts, first 16 DISTINCT selectors in arrival order
 * (NK gateway bounds-checks selectors vs 0x86; the boot path uses a handful).
 * Dumped once at exit (atexit, armed on first delivery — newworld-only by
 * construction: the shim is only called on the resolved newworld sc path) and
 * explicitly on the crash path (atexit does not fire there). The cap-5 triage
 * prints stay unchanged. */
static struct { uint32 sel; uint64_t n; } exc_sc_sel_counts[16];
static unsigned exc_sc_sel_distinct = 0;
static uint64_t exc_sc_sel_overflow = 0;	// deliveries beyond 16 distinct selectors
static bool exc_sc_sel_dumped = false;

static void exc_dump_sc_selectors(void)
{
	if (exc_sc_sel_dumped || exc_sc_sel_distinct == 0)
		return;
	exc_sc_sel_dumped = true;
	fprintf(stderr, "[EXC] sc selectors (arrival order, distinct=%u):", exc_sc_sel_distinct);
	for (unsigned i = 0; i < exc_sc_sel_distinct; i++)
		fprintf(stderr, " 0x%02x x%llu", exc_sc_sel_counts[i].sel,
		        (unsigned long long)exc_sc_sel_counts[i].n);
	if (exc_sc_sel_overflow)
		fprintf(stderr, " (+%llu deliveries beyond 16 distinct)",
		        (unsigned long long)exc_sc_sel_overflow);
	fprintf(stderr, "\n");
}

// Emulation time statistics
#ifndef EMUL_TIME_STATS
#define EMUL_TIME_STATS 0
#endif

#if EMUL_TIME_STATS
static clock_t emul_start_time;
static uint32 interrupt_count = 0, ppc_interrupt_count = 0;
static clock_t interrupt_time = 0;
static uint32 exec68k_count = 0;
static clock_t exec68k_time = 0;
static uint32 native_exec_count = 0;
static clock_t native_exec_time = 0;
static uint32 macos_exec_count = 0;
static clock_t macos_exec_time = 0;
#endif

static void enter_mon(void)
{
	// Start up mon in real-mode
#if ENABLE_MON
	const char *arg[4] = {"mon", "-m", "-r", NULL};
	mon(3, arg);
#endif
}

// From main_*.cpp
extern uintptr SignalStackBase();

// From rsrc_patches.cpp
extern "C" void check_load_invoc(uint32 type, int16 id, uint32 h);
extern "C" void named_check_load_invoc(uint32 type, uint32 name, uint32 h);

#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
// From ppc-cpu.cpp: dump the SS_JIT_TRACE_RING execution history (crash diagnosis)
extern "C" void ppc_jit_dump_trace_ring(void);
// From ppc-cpu.cpp: once-guarded crash flush of the SS_DR_R24_RING 68k-PC ring.
// Called EARLY in the SIGSEGV handler (before the re-fault-prone register/disasm
// dumps) so the ring survives a re-fault; no-op unless SS_DR_R24_RING=1.
extern "C" void ppc_jit_r24ring_crash_flush(void);
// From ppc-cpu.cpp: record an EMUL_OP entry ('E') / return ('R') into the trace ring.
// a_regs points at gpr[16] (8 consecutive uint32s = 68k A0-A7).
extern "C" void ppc_jit_ring_record_emulop(char type, uint32 pc68k, uint32 op,
                                           uint32 d0, uint32 d1, uint32 sp, const uint32 *a_regs);
#endif

// PowerPC EmulOp to exit from emulation looop
const uint32 POWERPC_EXEC_RETURN = POWERPC_EMUL_OP | 1;

// Enable Execute68k() safety checks?
#define SAFE_EXEC_68K 1

// Save FP state in Execute68k()?
#define SAVE_FP_EXEC_68K 1

// Interrupts in EMUL_OP mode?
#define INTERRUPTS_IN_EMUL_OP_MODE 1

// Interrupts in native mode?
#define INTERRUPTS_IN_NATIVE_MODE 1

// Pointer to Kernel Data
static KernelData * kernel_data;

// SIGSEGV handler
sigsegv_return_t sigsegv_handler(sigsegv_address_t, sigsegv_address_t);

#if PPC_ENABLE_JIT && PPC_REENTRANT_JIT
// Special trampolines for EmulOp and NativeOp
static uint8 *emul_op_trampoline;
static uint8 *native_op_trampoline;
#endif


/**
 *		PowerPC emulator glue with special 'sheep' opcodes
 **/

enum {
	PPC_I(SHEEP) = PPC_I(MAX),
	PPC_I(SHEEP_MAX)
};

class sheepshaver_cpu
	: public powerpc_cpu
{
	void init_decoder();
	void execute_sheep(uint32 opcode);

public:

	// Constructor
	sheepshaver_cpu();

	// C2.0 RPC: dump registers as JSON for the Inspector
	void dump_regs_json(char *buf, int bufsz) {
		int pos = 0;
		pos += snprintf(buf + pos, bufsz - pos, "{\"pc\":\"0x%08x\"", (unsigned)pc());
		pos += snprintf(buf + pos, bufsz - pos, ",\"lr\":\"0x%08x\"", (unsigned)lr());
		pos += snprintf(buf + pos, bufsz - pos, ",\"ctr\":\"0x%08x\"", (unsigned)ctr());
		pos += snprintf(buf + pos, bufsz - pos, ",\"cr\":\"0x%08x\"", (unsigned)cr().get());
		pos += snprintf(buf + pos, bufsz - pos, ",\"xer\":\"0x%08x\"", (unsigned)xer().get());
		pos += snprintf(buf + pos, bufsz - pos, ",\"gpr\":[");
		for (int i = 0; i < 32; i++) {
			if (i > 0) pos += snprintf(buf + pos, bufsz - pos, ",");
			pos += snprintf(buf + pos, bufsz - pos, "\"0x%08x\"", (unsigned)gpr(i));
		}
		pos += snprintf(buf + pos, bufsz - pos, "]}");
	}

	// Direct access to register state for JIT
	// powerpc_cpu::_regs is at a known offset from 'this'; regs are 16-byte aligned within
	// Batch opcode-test helper: clear all special CPU flags between vectors so a
	// prior vector's SPCFLAG_*_EXEC_RETURN (set by the exec-return sentinel /
	// illegal handler / invalidate_cache) does not make the next execute() bail at
	// entry. spcflags() is protected on the base; expose a public reset here.
	void reset_spcflags_for_test() { spcflags().init(); }

	// Batch opcode-test helper: reset the trailing supervisor block (sprg/sdr1/bat/
	// srr0/srr1/sr/msr) between vectors. Without this, batch and legacy REGDUMPs
	// diverge for any vector reading state a previous vector wrote (the legacy path
	// gets a fresh process = fresh init_registers; batch reuses the CPU object).
	// Wave 0: extended to cover the new sr[16]/msr fields.
	void reset_supervisor_for_test() {
		for (int i = 0; i < 4; i++) sprg_reg(i) = 0;
		sdr1_reg() = 0;
		for (int i = 0; i < 16; i++) bat_reg(i) = 0;
		srr0_reg() = 0;
		srr1_reg() = 0;
		for (int i = 0; i < 16; i++) sr_reg(i) = 0;
		msr_reg() = 0xf072;
	}

	// Wave-2 W2-1 opcode-test helper (SS_TEST_MSR): set the initial MSR for a
	// deliverability vector. msr_reg() is protected on the base; expose a public
	// setter here (the reset_*_for_test idiom). Default-off knob — when
	// SS_TEST_MSR is unset this is never called and the legacy table sees the
	// reset_supervisor_for_test value (0xf072) exactly as before.
	void set_msr_for_test(uint32 v) { msr_reg() = v; }

	// Batch opcode-test helper: zero the floating-point and AltiVec state that a
	// freshly-constructed CPU starts with (FPR=0/FPSCR=0 from init_registers, VR=0/
	// vrsave=0 from the zero-initialized allocation) but that the caller's per-vector
	// GPR/CR/XER/LR/CTR setup does NOT touch. The REGDUMP includes FPR and VR (and
	// FPSCR drives FP rounding), so without this, FP/vector results bleed across
	// vectors and batch output diverges from the legacy one-process-per-vector path.
	// (init_registers()/regs() are private on the base; use the public accessors.)
	void reset_fp_vec_for_test() {
		for (int i = 0; i < 32; i++) { fpr_dw(i) = 0; vr(i).j[0] = 0; vr(i).j[1] = 0; }
		fpscr() = 0;
		vrsave() = 0;
	}

	void *regs_for_jit() {
		// Same calculation as regs_ptr() but accessible from public scope
		char *base = (char *)this;
		// _regs.regs is the first field after the vtable pointer
		// On aarch64 with vtable, sizeof(void*) = 8, then padding to 16-byte alignment
		// Use set_register/get_register to find the actual address by side effect:
		// Actually, simplest: GPR(0) is at offset 0 in powerpc_registers.
		// set GPR0 to a known sentinel, scan memory to find it.
		uint32 sentinel = 0xDEADF00D;
		uint32 saved = get_register(powerpc_registers::GPR(0)).i;
		set_register(powerpc_registers::GPR(0), any_register(sentinel));
		// Scan for sentinel within first 1KB from 'this'
		void *result = NULL;
		for (int off = 0; off < 1024; off += 4) {
			if (*(uint32 *)(base + off) == sentinel) {
				result = base + off;
				break;
			}
		}
		set_register(powerpc_registers::GPR(0), any_register(saved));
		return result;
	}

	// CR & XER accessors
	uint32 get_cr() const		{ return cr().get(); }
	void set_cr(uint32 v)		{ cr().set(v); }
	uint32 get_xer() const		{ return xer().get(); }
	void set_xer(uint32 v)		{ xer().set(v); }

	// Execute NATIVE_OP routine
	void execute_native_op(uint32 native_op);
	static void call_execute_native_op(powerpc_cpu * cpu, uint32 native_op);

	// Execute EMUL_OP routine
	void execute_emul_op(uint32 emul_op);
	static void call_execute_emul_op(powerpc_cpu * cpu, uint32 emul_op);

	// Execute 68k routine
	void execute_68k(uint32 entry, M68kRegisters *r);

	// Execute ppc routine
	void execute_ppc(uint32 entry);

	// Execute MacOS/PPC code
	uint32 execute_macos_code(uint32 tvect, int nargs, uint32 const *args);

#if PPC_ENABLE_JIT
	// Compile one instruction
	virtual int compile1(codegen_context_t & cg_context);
#endif
	// Resource manager thunk
	void get_resource(uint32 old_get_resource);
	static void call_get_resource(powerpc_cpu * cpu, uint32 old_get_resource);

	// Handle MacOS interrupt
	void interrupt(uint32 entry);

	// M3a Task 4: deliver a pending DEC exception in place (newworld profile).
	// Returns true iff delivered (live regs mutated); false if not pending or deferred.
	bool deliver_pending_dec_exception();

	// Make sure the SIGSEGV handler can access CPU registers
	friend sigsegv_return_t sigsegv_handler(sigsegv_info_t *sip);
};

sheepshaver_cpu::sheepshaver_cpu()
{
	init_decoder();

#if PPC_ENABLE_JIT
	if (PrefsFindBool("jit"))
		enable_jit();
#endif
}

void sheepshaver_cpu::init_decoder()
{
	static const instr_info_t sheep_ii_table[] = {
		{ "sheep",
		  (execute_pmf)&sheepshaver_cpu::execute_sheep,
		  PPC_I(SHEEP),
		  D_form, 6, 0, CFLOW_JUMP | CFLOW_TRAP
		}
	};

	const int ii_count = sizeof(sheep_ii_table)/sizeof(sheep_ii_table[0]);
	D(bug("SheepShaver extra decode table has %d entries\n", ii_count));

	for (int i = 0; i < ii_count; i++) {
		const instr_info_t * ii = &sheep_ii_table[i];
		init_decoder_entry(ii);
	}
}

/*		NativeOp instruction format:
		+------------+-------------------------+--+-----------+------------+
		|      6     |                         |FN|    OP     |      2     |
		+------------+-------------------------+--+-----------+------------+
		 0         5 |6                      18 19 20      25 26        31
*/

typedef bit_field< 19, 19 > FN_field;
typedef bit_field< 20, 25 > NATIVE_OP_field;
typedef bit_field< 26, 31 > EMUL_OP_field;

void sheepshaver_cpu::call_execute_emul_op(powerpc_cpu * cpu, uint32 emul_op) {
	static_cast<sheepshaver_cpu *>(cpu)->execute_emul_op(emul_op);
}

// Execute EMUL_OP routine
void sheepshaver_cpu::execute_emul_op(uint32 emul_op)
{
	M68kRegisters r68;
	WriteMacInt32(XLM_68K_R25, gpr(25));
	WriteMacInt32(XLM_RUN_MODE, MODE_EMUL_OP);
	for (int i = 0; i < 8; i++)
		r68.d[i] = gpr(8 + i);
	for (int i = 0; i < 7; i++)
		r68.a[i] = gpr(16 + i);
	r68.a[7] = gpr(1);
	uint32 saved_cr = get_cr() & 0xff9fffff; // mask_operand::compute(11, 8)
	uint32 saved_xer = get_xer();
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	/* Trace-ring EMUL_OP entry record ('E'): from_pc = 68k PC, to_pc/op = the
	 * EMUL_OP number, r27/r29 fields carry 68k D0/D1, a[] = A0-A7.  Runs in
	 * BOTH interpreter and JIT mode — this is the comparison point for
	 * working-vs-broken boot analysis. */
	ppc_jit_ring_record_emulop('E', gpr(24), emul_op, gpr(8), gpr(9), gpr(1), &gpr(16));
	/* SS_EMULOP_COUNTS=1: per-op execution counters dumped to stderr every 5s.
	 * Settles "does OP_IRQ (op 40) ever fire" decisively — the bug-#2 premise. */
	{
		static int counts_enabled = -1;
		static uint32 op_counts[64];
		static time_t last_dump = 0;
		if (counts_enabled < 0) {
			const char *e = getenv("SS_EMULOP_COUNTS");
			counts_enabled = (e && *e == '1') ? 1 : 0;
		}
		if (counts_enabled) {
			if (emul_op < 64) op_counts[emul_op]++;
			time_t now = time(NULL);
			if (now - last_dump >= 5) {
				last_dump = now;
				fprintf(stderr, "EMULOP-COUNTS:");
				for (int i = 0; i < 64; i++)
					if (op_counts[i]) fprintf(stderr, " %d=%u", i, op_counts[i]);
				fprintf(stderr, "\n");
				fflush(stderr);
			}
		}
	}
#endif
	EmulOp(&r68, gpr(24), emul_op);
	set_cr(saved_cr);
	set_xer(saved_xer);
	for (int i = 0; i < 8; i++)
		gpr(8 + i) = r68.d[i];
	for (int i = 0; i < 7; i++)
		gpr(16 + i) = r68.a[i];
	gpr(1) = r68.a[7];
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	/* EMUL_OP return record ('R'): registers now hold the results (D0 = result code). */
	ppc_jit_ring_record_emulop('R', gpr(24), emul_op, gpr(8), gpr(9), gpr(1), &gpr(16));

	/* SS_EMULOP_TRACE=1: log SCSI-related EMUL_OP results to /tmp/emulop_trace.log
	 * for differential comparison between interpreter and JIT modes. */
	{
		static int trace_enabled = -1;
		static FILE *trace_fp = NULL;
		static int trace_count = 0;
		if (trace_enabled < 0) {
			const char *e = getenv("SS_EMULOP_TRACE");
			trace_enabled = (e && *e == '1') ? 1 : 0;
			if (trace_enabled) {
				trace_fp = fopen("/tmp/emulop_trace.log", "w");
				if (!trace_fp) trace_enabled = 0;
			}
		}
		if (trace_enabled && trace_count < 500 &&
		    (emul_op == 41 || emul_op == 42)) { /* OP_SCSI_DISPATCH=41, OP_SCSI_ATOMIC=42 */
			fprintf(trace_fp, "EMULOP op=%d D0=%08X D1=%08X A0=%08X PC=%08X\n",
				emul_op, gpr(8), gpr(9), gpr(16), gpr(24));
			fflush(trace_fp);
			trace_count++;
		}
	}
#endif
	WriteMacInt32(XLM_RUN_MODE, MODE_68K);

	/* If the guest requested power-off or restart, break out of the CPU loop. */
	if (power_off_requested || restart_requested)
		spcflags().set(SPCFLAG_CPU_EXEC_RETURN);
}

// Execute SheepShaver instruction
void sheepshaver_cpu::execute_sheep(uint32 opcode)
{
//	D(bug("Extended opcode %08x at %08x (68k pc %08x)\n", opcode, pc(), gpr(24)));
	assert((((opcode >> 26) & 0x3f) == 6) && OP_MAX <= 64 + 3);

	switch (opcode & 0x3f) {
	case 0:		// EMUL_RETURN
		QuitEmulator();
		break;

	case 1:		// EXEC_RETURN
		spcflags().set(SPCFLAG_CPU_EXEC_RETURN);
		break;

	case 2:		// EXEC_NATIVE
		execute_native_op(NATIVE_OP_field::extract(opcode));
		if (FN_field::test(opcode))
			pc() = lr();
		else
			pc() += 4;
		break;

	default:	// EMUL_OP
		execute_emul_op(EMUL_OP_field::extract(opcode) - 3);
		pc() += 4;
		break;
	}
}

// Compile one instruction
#if PPC_ENABLE_JIT
int sheepshaver_cpu::compile1(codegen_context_t & cg_context)
{
	const instr_info_t *ii = cg_context.instr_info;
	if (ii->mnemo != PPC_I(SHEEP))
		return COMPILE_FAILURE;

	int status = COMPILE_FAILURE;
	powerpc_dyngen & dg = cg_context.codegen;
	uint32 opcode = cg_context.opcode;

	switch (opcode & 0x3f) {
	case 0:		// EMUL_RETURN
		dg.gen_invoke(QuitEmulator);
		status = COMPILE_CODE_OK;
		break;

	case 1:		// EXEC_RETURN
		dg.gen_spcflags_set(SPCFLAG_CPU_EXEC_RETURN);
		// Don't check for pending interrupts, we do know we have to
		// get out of this block ASAP
		dg.gen_exec_return();
		status = COMPILE_EPILOGUE_OK;
		break;

	case 2: {	// EXEC_NATIVE
		uint32 selector = NATIVE_OP_field::extract(opcode);
		switch (selector) {
#if !PPC_REENTRANT_JIT
		// Filter out functions that may invoke Execute68k() or
		// CallMacOS(), this would break reentrancy as they could
		// invalidate the translation cache and even overwrite
		// continuation code when we are done with them.
		case NATIVE_PATCH_NAME_REGISTRY:
			dg.gen_invoke(DoPatchNameRegistry);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_VIDEO_INSTALL_ACCEL:
			dg.gen_invoke(VideoInstallAccel);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_VIDEO_VBL:
			dg.gen_invoke(VideoVBL);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_GET_RESOURCE:
		case NATIVE_GET_1_RESOURCE:
		case NATIVE_GET_IND_RESOURCE:
		case NATIVE_GET_1_IND_RESOURCE:
		case NATIVE_R_GET_RESOURCE: {
			static const uint32 get_resource_ptr[] = {
				XLM_GET_RESOURCE,
				XLM_GET_1_RESOURCE,
				XLM_GET_IND_RESOURCE,
				XLM_GET_1_IND_RESOURCE,
				XLM_R_GET_RESOURCE
			};
			uint32 old_get_resource = ReadMacInt32(get_resource_ptr[selector - NATIVE_GET_RESOURCE]);
			typedef void (*func_t)(dyngen_cpu_base, uint32);
			func_t func = &sheepshaver_cpu::call_get_resource;
			dg.gen_invoke_CPU_im(func, old_get_resource);
			status = COMPILE_CODE_OK;
			break;
		}
#endif
		case NATIVE_CHECK_LOAD_INVOC:
			dg.gen_load_T0_GPR(3);
			dg.gen_load_T1_GPR(4);
			dg.gen_se_16_32_T1();
			dg.gen_load_T2_GPR(5);
			dg.gen_invoke_T0_T1_T2((void (*)(uint32, uint32, uint32))check_load_invoc);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NAMED_CHECK_LOAD_INVOC:
			dg.gen_load_T0_GPR(3);
			dg.gen_load_T1_GPR(4);
			dg.gen_load_T2_GPR(5);
			dg.gen_invoke_T0_T1_T2((void (*)(uint32, uint32, uint32))named_check_load_invoc);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_SYNC_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_sync_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_BITBLT_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_bitblt_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_FILLRECT_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_fillrect_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_UNKNOWN_HOOK:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0_ret_T0((uint32 (*)(uint32))NQD_unknown_hook);
			dg.gen_store_T0_GPR(3);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_BITBLT:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0((void (*)(uint32))NQD_bitblt);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_INVRECT:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0((void (*)(uint32))NQD_invrect);
			status = COMPILE_CODE_OK;
			break;
		case NATIVE_NQD_FILLRECT:
			dg.gen_load_T0_GPR(3);
			dg.gen_invoke_T0((void (*)(uint32))NQD_fillrect);
			status = COMPILE_CODE_OK;
			break;
		}
		// Could we fully translate this NativeOp?
		if (status == COMPILE_CODE_OK) {
			if (!FN_field::test(opcode))
				cg_context.done_compile = false;
			else {
				dg.gen_load_T0_LR_aligned();
				dg.gen_set_PC_T0();
				cg_context.done_compile = true;
			}
			break;
		}
#if PPC_REENTRANT_JIT
		// Try to execute NativeOp trampoline
		if (!FN_field::test(opcode))
			dg.gen_set_PC_im(cg_context.pc + 4);
		else {
			dg.gen_load_T0_LR_aligned();
			dg.gen_set_PC_T0();
		}
		dg.gen_mov_32_T0_im(selector);
		dg.gen_jmp(native_op_trampoline);
		cg_context.done_compile = true;
		status = COMPILE_EPILOGUE_OK;
		break;
#else
		// Invoke NativeOp handler
		if (!FN_field::test(opcode)) {
			typedef void (*func_t)(dyngen_cpu_base, uint32);
			func_t func = &sheepshaver_cpu::call_execute_native_op;
			dg.gen_invoke_CPU_im(func, selector);
			cg_context.done_compile = false;
			status = COMPILE_CODE_OK;
		}
		// Otherwise, let it generate a call to execute_sheep() which
		// will cause necessary updates to the program counter
		break;
#endif
	}

	default: {	// EMUL_OP
		uint32 emul_op = EMUL_OP_field::extract(opcode) - 3;
#if PPC_REENTRANT_JIT
		// Try to execute EmulOp trampoline
		dg.gen_set_PC_im(cg_context.pc + 4);
		dg.gen_mov_32_T0_im(emul_op);
		dg.gen_jmp(emul_op_trampoline);
		cg_context.done_compile = true;
		status = COMPILE_EPILOGUE_OK;
		break;
#else
		// Invoke EmulOp handler
		typedef void (*func_t)(dyngen_cpu_base, uint32);
		func_t func = &sheepshaver_cpu::call_execute_emul_op;
		dg.gen_invoke_CPU_im(func, emul_op);
		cg_context.done_compile = false;
		status = COMPILE_CODE_OK;
		break;
#endif
	}
	}
	return status;
}
#endif

// Handle MacOS interrupt
void sheepshaver_cpu::interrupt(uint32 entry)
{
#if EMUL_TIME_STATS
	ppc_interrupt_count++;
	const clock_t interrupt_start = clock();
#endif

	// Save program counters and branch registers
	uint32 saved_pc = pc();
	uint32 saved_lr = lr();
	uint32 saved_ctr= ctr();
	uint32 saved_sp = gpr(1);

	// Initialize stack pointer to SheepShaver alternate stack base
	gpr(1) = SignalStackBase() - 64;

	// Build trampoline to return from interrupt
	SheepVar32 trampoline = POWERPC_EXEC_RETURN;

	// Prepare registers for nanokernel interrupt routine
	WriteMacInt32(KERNEL_DATA_BASE + 0x004, gpr(1));
	WriteMacInt32(KERNEL_DATA_BASE + 0x018, gpr(6));

	gpr(6) = ReadMacInt32(KERNEL_DATA_BASE + 0x65c);
	assert(gpr(6) != 0);
	WriteMacInt32(gpr(6) + 0x13c, gpr(7));
	WriteMacInt32(gpr(6) + 0x144, gpr(8));
	WriteMacInt32(gpr(6) + 0x14c, gpr(9));
	WriteMacInt32(gpr(6) + 0x154, gpr(10));
	WriteMacInt32(gpr(6) + 0x15c, gpr(11));
	WriteMacInt32(gpr(6) + 0x164, gpr(12));
	WriteMacInt32(gpr(6) + 0x16c, gpr(13));

	gpr(1)  = KernelDataAddr;
	gpr(7)  = ReadMacInt32(KERNEL_DATA_BASE + 0x660);
	gpr(8)  = 0;
	gpr(10) = trampoline.addr();
	gpr(12) = trampoline.addr();
	gpr(13) = get_cr();

	// rlwimi. r7,r7,8,0,0
	uint32 result = op_ppc_rlwimi::apply(gpr(7), 8, 0x80000000, gpr(7));
	record_cr0(result);
	gpr(7) = result;

	gpr(11) = 0xf072; // MSR (SRR1)
	cr().set((gpr(11) & 0x0fff0000) | (get_cr() & ~0x0fff0000));

	// Enter nanokernel
	if (ROMType == ROMTYPE_NEWWORLD) {
		static int nw_int_count = 0;
		if (nw_int_count < 5)
			// (retagged from [NW-INT] in M3a - that mechanism is deleted; this is the
			// legacy nested-execute interrupt() entry, paravirtual-reachable via MODE_NATIVE)
			fprintf(stderr, "[NK-ENTER] enter handler @%08x, r1=%08x r6=%08x r10(ret)=%08x\n",
			        entry, (uint32)gpr(1), (uint32)gpr(6), (uint32)gpr(10));
		nw_int_count++;
	}
	execute(entry);
	if (ROMType == ROMTYPE_NEWWORLD) {
		static int nw_ret_count = 0;
		if (nw_ret_count < 5)
			fprintf(stderr, "[NK-ENTER] returned from handler, resuming pc=%08x\n", saved_pc);
		nw_ret_count++;
	}

	// Restore program counters and branch registers
	pc() = saved_pc;
	lr() = saved_lr;
	ctr()= saved_ctr;
	gpr(1) = saved_sp;

	// M3a Task 4.4: depth-deferred DEC re-raised on return toward depth 1
	// (the delivery hook defers while execute_depth > 1; this nested execute()
	// just returned and host state is restored, so re-raise if the latch is
	// still set — the next outermost-depth poll can then deliver).
	// W2-0: routed through the extracted predicate via the synthetic full edge
	// (MSR may have changed arbitrarily across the nested execute; the delivery
	// hook re-gates on the real MSR) — behavior-identical to the unconditional
	// if-pending recheck. See ExcEdgeReRaise's header comment.
	if (MachineProfileIsNewWorld() &&
	    ExcEdgeReRaise(0u, 0x8000u,   /* W2-3: + the level-held EXT source */
	                   (VirtClockDECPending(&g_virt_clock) || SheepExcExtPending()
	                    || SheepExcHostIrqPending()) ? 1 : 0))   /* M7: + host latch */
		trigger_interrupt();

#if EMUL_TIME_STATS
	interrupt_time += (clock() - interrupt_start);
#endif
}

/*
 *  M3a Task 4: real DEC exception delivery at the block-boundary poll
 *  (docs/superpowers/plans/2026-06-10-machine-layer-m3a.md, KDP-SHIM mode per
 *  docs/planning/machine/M3A-ENTRY-TABLE.md).
 *
 *  Called from check_spcflags' HANDLE arm (ppc-cpu.cpp), newworld profile only,
 *  on the CPU thread (§2g — no locking beyond the VirtClock atomics).
 *
 *  Contract notes:
 *   - Source discrimination (rev 2 M3): this hook consumes ONLY the VirtClock
 *     DEC latch. InterruptFlags/VIA pending stays with HandleInterrupt (fenced
 *     for newworld in Task 2) until M3b's PIC.
 *   - restart PC = block-start: the JIT chain-entry poll stores the block-start
 *     PC into pc() before any body code runs, so pc() here is always a
 *     not-yet-executed instruction => a valid SRR0. DR-emulator blocks have no
 *     entry poll (rev 2 F8) — delivery inside chained DR loops waits for the
 *     between-block dispatcher check (acceptable; irrelevant to the diagnostic
 *     boot).
 *   - XLM_IRQ_NEST is deliberately IGNORED in favor of the real MSR[EE] gate:
 *     the staged NK parks it at 0xFFFFFFFF (M3A-ENTRY-TABLE.md finding 3).
 *     Revisit if Task 7 shows the NK manipulating it as its interrupt mask.
 *   - Deferral (EE off or execute_depth > 1) leaves the latch SET and returns
 *     false (legacy fall-through unchanged). The re-raise comes from the Task 3
 *     EE-edge hooks (mtmsr/rfi) and the Task 4.4 nested-execute-return rechecks.
 *     The LAZY latch path (VirtClockReadDEC -> dec_fire on the CPU thread,
 *     virt_clock.cpp — pure module, deliberately not modified) needs no kick of
 *     its own: its latch is picked up by those same EE-edge re-raises and by the
 *     next scheduler-expiry kick (main_unix.cpp vclk_dec_arm).
 */
bool sheepshaver_cpu::deliver_pending_dec_exception()
{
	// Wave-2 W2-0: the gate COMPOSITION lives in exc_core (ExcDeliveryDecision,
	// pure, law-tested by test_exc_chain). Gate order pending -> depth -> EE ->
	// native is CONTRACT (the exc= tuple counts per-gate). This caller keeps the
	// stateful obligations (header doc-comment): sample guest memory, increment
	// exactly the counter matching the decision, leave the latch SET on any
	// deferral, clear it EXACTLY ONCE on DELIVER.
	//
	// Two-phase run-mode sampling (the sanctioned lazy idiom): [XLM_RUN_MODE]
	// (0x2810) is guest lowmem — unmapped on the SS_TEST harness path — and the
	// pre-extraction code only read it after the pending/depth/EE gates passed.
	// Pass 0 first; only a provisional DELIVER pays the guest read and re-asks.
	// Decision-identical to a single sampled call (the first three gates never
	// consult the word).
	//
	// The native fence itself (M6a rung-2 W2, plan rev 3.1 item 3): defer while
	// a MixedMode NATIVE EXCURSION is in flight — [XLM_RUN_MODE] is maintained
	// 1-forward/0-backward by the NK on exactly the FE01/FE02 switch pair
	// (MODE_NATIVE=1 during PPC-native windows, MODE_68K=0 while the 68k world
	// runs). Under the W2 [KDP+0x65c] world-flip discipline the word holds the
	// MMCB during a native window; the KDP register-save shim below saves
	// r7-r13 through r6=[KDP+0x65c] — a DEC delivered mid-excursion would save
	// into the MMCB whose slots the NK switch-back save is about to rewrite.
	// Latch stays set; EE-edge re-raises + the block-boundary poll pick it up
	// once [0x2810] returns to 0. Newworld-gated by construction.
	// Wave-2 W2-3: SECOND pending source — the OpenPIC output (level-held,
	// rev 2 C1). The gates are evaluated ONCE on the combined pending (per-poll
	// deferral counts keep their pre-W2-3 tuple semantics; with the PIC gated
	// off ext_pending is constant 0 and this is byte-identical to the DEC-only
	// hook). Source selection happens after DELIVER.
	// M7 Task A (Q-I4(c)/A5): the host once-per-edge latch OR-composes with the
	// PIC level AT THE POLL — neither source writes the other's word.
	const int dec_pending = VirtClockDECPending(&g_virt_clock) ? 1 : 0;
	const int ext_pending = SheepExcExtPending() | SheepExcHostIrqPending();
	const int pending = dec_pending | ext_pending;
	// M7 Task A (Task 0 Q-I6 verdict (ii)): the DEFER_NATIVE fence is NARROWED
	// route-aware — the run_mode defer is SKIPPED when the delivery would route
	// to a PUBLISHED handler, KEPT for the legacy KDP-shim DEC route.
	//   - The fence's original rationale (exc_core.cpp:70-73) is the legacy KDP
	//     register-save shim: it writes guest memory (the ECB save slots below)
	//     that a mid-native-excursion NK switch-back is about to rewrite. The
	//     published routes use the 2-SPR shim ONLY (SPRG1:=r1, SPRG2:=LR — two
	//     SPR writes, no guest memory), and the NK bodies do their own complete
	//     save/restore (shared prologue 0x313d40 r0,r7-r13; the EXT fallback's
	//     bl 0x3238d4/0x323944 pair is offset-matched to its r20-r31 clobber
	//     set — re-verified [STATIC], Task 0 Q-I6 row). restart PC = JIT block
	//     start is a clean boundary either way.
	//   - Kept fence + the parked frontier ([0x2810]=1 indefinitely) would make
	//     every published-route delivery undeliverable by design (kicks re-poll
	//     but never deliver) — Task A's acceptance would be unfalsifiable.
	// Route resolution mirrors the source selection below: DEC-before-EXT, so
	// dec_pending decides the route. Gated-off boots are decision-identical:
	// ext_pending==0 without a configured EXT source, and the DEC route is
	// published only under SS_NW_DEC_PUBLISHED (newworld default ON since the
	// M7 Task C cluster flip; opt-out =0 restores the legacy KDP route).
	const bool route_published = dec_pending
	                ? exc_dec_published_enabled()
	                : (g_exc_entry_table.external_entry != 0);
	ExcDecision decision = ExcDeliveryDecision(pending, current_execute_depth(),
	                                           msr_reg(), 0);
	if (decision == EXC_DECIDE_DELIVER && !route_published)
		decision = ExcDeliveryDecision(pending, current_execute_depth(),
		                               msr_reg(), ReadMacInt32(XLM_RUN_MODE));
	switch (decision) {
	case EXC_DECIDE_NONE:
		exc_native_rearm_used = 0;	// M7 item 2: non-native decision resets the re-arm budget
		return false;
	case EXC_DECIDE_DEFER_DEPTH:
		exc_stat_deferred_depth++;
		exc_native_rearm_used = 0;
		return false;
	case EXC_DECIDE_DEFER_EE:
		exc_stat_deferred_ee++;
		exc_native_rearm_used = 0;
		return false;
	case EXC_DECIDE_DEFER_NATIVE:
		exc_stat_deferred_native++;
		/* M7 item 2 / W2-4 item 1b — the post-DEFER_NATIVE wake-up edge
		 * (INTERRUPT-INJECTION-RECON.md Q4, option (c)): a native-window
		 * deferral consumed the HANDLE spcflag, and between windows EE in the
		 * 68k world is 0, so no EE-edge ever re-raises — the latched DEC slept
		 * through every subsequent native->68k window (97 deferrals,
		 * delivered_dec=0, D-7 acceptance boot). Re-arm the poll: check_spcflags
		 * re-asks at every subsequent block boundary until the window exits.
		 * For a TRANSIENT window (the recon's 151 balanced set/clear pairs,
		 * ~10^2-10^3 records) the first run_mode==0 boundary delivers.
		 *
		 * RE-PIN (2026-06-12, recon addendum "wake-up edge re-pin"): the recon's
		 * "self-terminating, bounded by window length" claim is FALSIFIED at the
		 * post-P-M5-fix frontier — the riser-on boot now PARKS inside a native
		 * window that never exits (deferred_native == executed blocks == 5.3e9
		 * over 50s, ~107M re-polls/s, frontier regressed). Unbounded re-arm is
		 * therefore capped per pending episode: budget EXC_NATIVE_REARM_CAP
		 * (>> window length, so transient-window delivery is unaffected); the
		 * budget resets whenever the decision leaves the native regime (DELIVER /
		 * NONE / DEFER_EE / DEFER_DEPTH). Cap exhaustion leaves the latch SET and
		 * stops re-arming (one stderr line) — the pre-fix kick-driven behavior:
		 * any later TriggerInterrupt/EE-edge re-raise polls again (single-shot,
		 * the budget stays spent until the regime changes).
		 * Tuple-semantics note: deferred_native now counts every re-poll inside
		 * a window (the honest re-poll cost meter), not one count per window.
		 * Newworld-gated by construction (sole caller is check_spcflags'
		 * MachineProfileIsNewWorld() branch). The consumption-side guard in
		 * check_spcflags keeps the re-armed flag from re-entering the legacy
		 * HandleInterrupt fall-through per block boundary. */
		if (exc_native_rearm_used < EXC_NATIVE_REARM_CAP) {
			exc_native_rearm_used++;
			spcflags().set(SPCFLAG_CPU_HANDLE_INTERRUPT);
		}
		else if (exc_native_rearm_used == EXC_NATIVE_REARM_CAP) {
			exc_native_rearm_used++;	/* log once per episode */
			fprintf(stderr, "[EXC] DEFER_NATIVE re-arm budget exhausted (%u) — "
			        "parked native window; latch stays set, polling returns to "
			        "kick-driven\n", EXC_NATIVE_REARM_CAP);
		}
		return false;
	case EXC_DECIDE_DELIVER:
		exc_native_rearm_used = 0;	// M7 item 2: delivery resets the re-arm budget
		break;
	}

	// --- Source selection: DEC before EXT (M3b rev 2 m11/C1, justified
	// locally as required): OEA ranks External ABOVE Decrementer, but our DEC
	// latch is ONE-SHOT-clear-on-delivery while PIC pending is LEVEL-HELD —
	// an EXT line safely waits one poll (it stays asserted until the guest
	// retires it), whereas inverting the order would let a stuck-asserted EXT
	// line starve the one-shot DEC forever. The U13 starvation tripwire below
	// bounds the residual risk in the chosen order. ---
	if (!dec_pending) {
		// EXT-only delivery (the DEC path below is the pre-W2-3 body, unchanged).
		const uint32 restart_pc_ext = pc();
		ExcTransition te = ExcEnter(restart_pc_ext, msr_reg(), EXC_EXTERNAL,
		                            &g_exc_entry_table);
		if (te.pc == EXC_PC_UNRESOLVED) {
			fprintf(stderr, "[EXC] FATAL: EXT delivery with unresolved entry "
			        "(restart=%08x msr=%08x) - check SS_EXC_ENTRY\n",
			        restart_pc_ext, msr_reg());
			abort();
		}
		// LEVEL-HELD (rev 2 C1, BINDING): do NOT clear the PIC pending — the
		// guest's handler retires it (IACK/EOI/mask -> output deassert edge).
		// At today's frontier that handler is the [KDP+0x5b0] fallback
		// (0x50325f00, registered-handler table NOT installed — W2L-1);
		// observe what it does, record honestly.
		//
		// The 2-SPR shim (Q-W2 verdict, EE-CHAIN-RECON §W2S-2; the sc/program
		// precedent): the EXT body 0x50314880 opens with the SHARED save
		// prologue 0x313d40, which consumes exactly SPRG1 := caller r1 and
		// SPRG2 := caller LR. Everything else it reads is NK-maintained staged
		// state. The DEC KDP save shim does NOT transfer (same verdict as sc
		// Q-S2) — deliberately absent here. Unconditional like the sc/program
		// shims (no SS_EXC_BARE gate: two register writes, no guest memory).
		sprg_reg(1) = gpr(1);
		sprg_reg(2) = lr();
		// SRR1.EE=1 mandatory (the NK EXT body's punch-through guard PANICS on
		// EE=0): structurally guaranteed — the EE gate above admits only EE=1
		// MSRs and ExcEnter keeps the low 16 bits (test_exc_chain pins it).
		srr0_reg() = te.srr0;
		srr1_reg() = te.srr1;
		msr_reg()  = te.msr;
		pc()       = te.pc;
		exc_stat_delivered_ext++;
		// M7 Task A: CONSUME the host once-per-edge latch — this delivery
		// services its assert edge; re-armed only by the next edge. The PIC
		// level (C1, level-held) is deliberately NOT cleared here. If both
		// sources were pending, this one delivery services the latch's edge
		// and the still-held PIC level re-delivers on its own.
		exc_host_irq_consume();
		// Re-delivery runaway guard (rev 2 C1): EXT is level-held, so each
		// handler round-trip that fails to retire the line re-delivers on the
		// next EE rise. N deliveries with no intervening deassert edge = the
		// guest is not retiring the source — loud once per assert episode.
		if (++exc_ext_delivs_this_assert == EXC_EXT_RUNAWAY_N)
			fprintf(stderr, "[EXC] TRIPWIRE: EXT re-delivery runaway - %u "
			        "deliveries with no PIC retirement (IACK/EOI/mask); the "
			        "guest handler is not servicing the line\n",
			        (unsigned)EXC_EXT_RUNAWAY_N);
		exc_dec_delivs_while_ext = 0;   // EXT got through: starvation reset
		if (exc_stat_delivered_ext <= 5)
			fprintf(stderr, "[EXC] EXT delivered #%llu: restart=%08x srr1=%08x "
			        "msr=%08x -> entry=%08x\n",
			        (unsigned long long)exc_stat_delivered_ext, te.srr0, te.srr1,
			        te.msr, te.pc);
		return true;
	}

	// DEC delivery (the pre-W2-3 body, unchanged below) + the U13 starvation
	// tripwire (rev 2 tension 2): EXT pending across >N consecutive DEC
	// deliveries with no EXT delivery means the DEC-before-EXT order is
	// starving the level-held source — loud, symmetric to the runaway guard.
	if (ext_pending && ++exc_dec_delivs_while_ext == EXC_EXT_STARVATION_N)
		fprintf(stderr, "[EXC] TRIPWIRE: EXT starvation - %u consecutive DEC "
		        "deliveries with EXT pending and no EXT delivery (U13; "
		        "DEC-before-EXT order under sustained dual-pending)\n",
		        (unsigned)EXC_EXT_STARVATION_N);
	VirtClockClearDECPending(&g_virt_clock);

	const uint32 restart_pc = pc();

	// Compute the architectural transition from the PRE-exception MSR (the shim
	// below never touches msr, so ordering vs the KDP writes is free).
	ExcTransition t = ExcEnter(restart_pc, msr_reg(), EXC_DECREMENTER, &g_exc_entry_table);
	if (t.pc == EXC_PC_UNRESOLVED) {
		// (quality-review I1) Misuse hardening: SS_EXC_ENTRY=0 / unparsable would
		// otherwise wild-jump AFTER the latch clear + KDP mutation. Symmetric with
		// the sc path's guard.
		fprintf(stderr, "[EXC] FATAL: DEC delivery with unresolved interrupt entry "
		        "(restart=%08x msr=%08x) - check SS_EXC_ENTRY\n", restart_pc, msr_reg());
		abort();
	}

	// SS_EXC_BARE=1: the bounded direct-entry experiment — skip the KDP ABI shim
	// entirely, apply only the architectural transition. Resolved once.
	static const bool exc_bare = []() {
		const char *e = getenv("SS_EXC_BARE");
		return e && e[0] && e[0] != '0';
	}();

	/* W2-4 step 0 (SS_NW_DEC_PUBLISHED — newworld DEFAULT since the M7 Task C
	 * cluster flip; opt-out =0): the published-handler
	 * route — interrupt_entry defaults to 0x50313200 (the NK-published DEC
	 * handler) and the shim is the sc/program/EXT 2-SPR shim, NOT the KDP
	 * register-save shim below. Rationale (EE-CHAIN-RECON.md W2S-2 Q-W2
	 * verdict #2/#3): 0x50313200 opens with the SHARED save prologue 0x313d40,
	 * which consumes exactly SPRG1 := caller r1 and SPRG2 := caller LR;
	 * everything else it reads is NK-maintained ([KDP-0x10] flags, [KDP-0x14]
	 * save target — NOT the [KDP+0x65c]/[KDP+0x660] emulator-interface pair,
	 * and NOT r9/cr6/cr7, the W2S-R1 hazards this re-point retires). The
	 * DEC-shim's ECB/[KDP+0x65c] register-save logic does NOT transfer (same
	 * verdict as sc Q-S2). Unconditional like the sc/program/EXT shims —
	 * SS_EXC_BARE is moot here (two SPR writes, no guest memory) and is
	 * deliberately ignored, matching the EXT-branch precedent above.
	 * The KDP-shim block below STAYS as the gate-off fallback: it is the
	 * proven delivery shape for the save-and-switch body 0x50412b1c (the
	 * pre-step-0 default, still reachable via gate-off and/or SS_EXC_ENTRY
	 * pointed back at it). */
	const bool dec_published = exc_dec_published_enabled();
	if (dec_published) {
		sprg_reg(1) = gpr(1);
		sprg_reg(2) = lr();
		// SRR1.EE=1 mandatory (the same punch-through guard as the EXT body —
		// shared prologue): structurally guaranteed by the EE delivery gate +
		// ExcEnter's low-16 keep mask, exactly as on the EXT branch.
	} else if (!exc_bare) {
		/* --- KDP register-save shim (KDP-SHIM mode, M3A-ENTRY-TABLE.md) ---
		 * Transcribed EXACTLY from sheepshaver_cpu::interrupt() above (same
		 * offsets, same order, same rlwimi/record_cr0/CR-splice), with THREE
		 * honest upgrades:
		 *   1. gpr(10)/gpr(12) = the REAL restart PC (was: trampoline address)
		 *   2. gpr(11)         = the REAL composed SRR1 (was: 0xf072 literal;
		 *                        byte-equal on the boot-real 0xf072 case —
		 *                        exc_core Task 1 test 3)
		 *   3. SRR0/SRR1/MSR/PC = the real ExcEnter transition (below)
		 * and FOUR omissions (we are not nesting — the guest handler runs on the
		 * live CPU and returns via real rfi, Task 3):
		 *   1. NO stack swap to SignalStackBase (so KDP+0x004 saves the LIVE
		 *      guest r1, not a host signal-stack pointer — strictly more honest)
		 *   2. NO trampoline allocation
		 *   3. NO nested execute()
		 *   4. NO saved/restored host PC/LR/CTR/SP around the handler. */
		WriteMacInt32(KERNEL_DATA_BASE + 0x004, gpr(1));
		WriteMacInt32(KERNEL_DATA_BASE + 0x018, gpr(6));

		gpr(6) = ReadMacInt32(KERNEL_DATA_BASE + 0x65c);
		if (gpr(6) == 0) {
			// The NK handler prologue stores through r6 in its first instructions
			// (rev 2 #1) — a zero ECB means silent guest-memory corruption. Abort
			// loudly instead. (Probe-verified live value: 0x68fff000.)
			fprintf(stderr, "[EXC] FATAL: [KDP+0x65c] (ECB) is 0 at DEC delivery "
			        "(restart_pc=%08x) — KDP shim has no save area\n", restart_pc);
			abort();
		}
		WriteMacInt32(gpr(6) + 0x13c, gpr(7));
		WriteMacInt32(gpr(6) + 0x144, gpr(8));
		WriteMacInt32(gpr(6) + 0x14c, gpr(9));
		WriteMacInt32(gpr(6) + 0x154, gpr(10));
		WriteMacInt32(gpr(6) + 0x15c, gpr(11));
		WriteMacInt32(gpr(6) + 0x164, gpr(12));
		WriteMacInt32(gpr(6) + 0x16c, gpr(13));

		gpr(1)  = KernelDataAddr;
		gpr(7)  = ReadMacInt32(KERNEL_DATA_BASE + 0x660);
		gpr(8)  = 0;
		gpr(10) = t.srr0;		// honest upgrade: real restart PC (was: trampoline)
		gpr(12) = t.srr0;		// honest upgrade: real restart PC (was: trampoline)
		gpr(13) = get_cr();		// captured BEFORE the rlwimi's record_cr0 — interrupt()'s exact order

		// rlwimi. r7,r7,8,0,0  (the Rc form mutates CR0 via record_cr0, AFTER r13
		// captured the pre-rlwimi CR — replicating interrupt() exactly)
		uint32 result = op_ppc_rlwimi::apply(gpr(7), 8, 0x80000000, gpr(7));
		record_cr0(result);
		gpr(7) = result;

		gpr(11) = t.srr1;		// honest upgrade: real composed SRR1 (was: 0xf072 literal)
		// CR splice: interrupt() injects SRR1 bits into CR fields 1-3
		// (mask 0x0fff0000) AFTER setting gpr(11). Replicated with the real SRR1
		// for ABI fidelity. Since SRR1 <= 0xFFFF always (EXC_SRR1_KEEP_MASK),
		// (srr1 & 0x0fff0000) == 0 — byte-identical to the legacy 0xf072 case.
		cr().set((gpr(11) & 0x0fff0000) | (get_cr() & ~0x0fff0000));
	}

	// Apply the architectural transition to the LIVE registers. The dispatcher
	// re-derives the next block from pc() after check_spcflags returns true
	// (rev 2 code-verified) — in-place delivery, no nested execute.
	srr0_reg() = t.srr0;
	srr1_reg() = t.srr1;
	msr_reg()  = t.msr;
	pc()       = t.pc;
	exc_stat_delivered_dec++;

	// First few deliveries to stderr for live-boot triage (Task 7); the running
	// totals ride the [HB] heartbeat (rev 2 M5: SIGALRM skips atexit dumps).
	if (exc_stat_delivered_dec <= 5)
		fprintf(stderr, "[EXC] DEC delivered #%llu: restart=%08x srr1=%08x msr=%08x -> entry=%08x%s\n",
		        (unsigned long long)exc_stat_delivered_dec, t.srr0, t.srr1, t.msr, t.pc,
		        dec_published ? " (2-SPR)" : exc_bare ? " (BARE)" : "");
	return true;
}

// Execute 68k routine
void sheepshaver_cpu::execute_68k(uint32 entry, M68kRegisters *r)
{
#if EMUL_TIME_STATS
	exec68k_count++;
	const clock_t exec68k_start = clock();
#endif

#if SAFE_EXEC_68K
	if (ReadMacInt32(XLM_RUN_MODE) != MODE_EMUL_OP)
		printf("FATAL: Execute68k() not called from EMUL_OP mode\n");
#endif

	// Save program counters and branch registers
	uint32 saved_pc = pc();
	uint32 saved_lr = lr();
	uint32 saved_ctr= ctr();
	uint32 saved_cr = get_cr();

	// Create MacOS stack frame
	// FIXME: make sure MacOS doesn't expect PPC registers to live on top
	uint32 sp = gpr(1);
	gpr(1) -= 56;
	WriteMacInt32(gpr(1), sp);

	// Save PowerPC registers
	uint32 saved_GPRs[19];
	memcpy(&saved_GPRs[0], &gpr(13), sizeof(uint32)*(32-13));
#if SAVE_FP_EXEC_68K
	double saved_FPRs[18];
	memcpy(&saved_FPRs[0], &fpr(14), sizeof(double)*(32-14));
#endif

	// Setup registers for 68k emulator
	cr().set(CR_SO_field<2>::mask());			// Supervisor mode
	for (int i = 0; i < 8; i++)					// d[0]..d[7]
	  gpr(8 + i) = r->d[i];
	for (int i = 0; i < 7; i++)					// a[0]..a[6]
	  gpr(16 + i) = r->a[i];
	gpr(23) = 0;
	gpr(24) = entry;
	gpr(25) = ReadMacInt32(XLM_68K_R25);		// MSB of SR
	gpr(26) = 0;
	gpr(28) = 0;								// VBR
	gpr(29) = ReadMacInt32(KERNEL_DATA_BASE + 0x1074);		// Pointer to opcode table
	gpr(30) = ReadMacInt32(KERNEL_DATA_BASE + 0x1078);		// Address of emulator
	gpr(31) = KernelDataAddr + 0x1000;

	// Push return address (points to EXEC_RETURN opcode) on stack
	gpr(1) -= 4;
	WriteMacInt32(gpr(1), XLM_EXEC_RETURN_OPCODE);
	
	// Rentering 68k emulator
	WriteMacInt32(XLM_RUN_MODE, MODE_68K);

	// Set r0 to 0 for 68k emulator
	gpr(0) = 0;

	// Execute 68k opcode
	uint32 opcode = ReadMacInt16(gpr(24));
	gpr(27) = (int32)(int16)ReadMacInt16(gpr(24) += 2);
	gpr(29) += opcode * 8;
	execute(gpr(29));

	// Save r25 (contains current 68k interrupt level)
	WriteMacInt32(XLM_68K_R25, gpr(25));

	// Reentering EMUL_OP mode
	WriteMacInt32(XLM_RUN_MODE, MODE_EMUL_OP);

	// Save 68k registers
	for (int i = 0; i < 8; i++)					// d[0]..d[7]
	  r->d[i] = gpr(8 + i);
	for (int i = 0; i < 7; i++)					// a[0]..a[6]
	  r->a[i] = gpr(16 + i);

	// Restore PowerPC registers
	memcpy(&gpr(13), &saved_GPRs[0], sizeof(uint32)*(32-13));
#if SAVE_FP_EXEC_68K
	memcpy(&fpr(14), &saved_FPRs[0], sizeof(double)*(32-14));
#endif

	// Cleanup stack
	gpr(1) += 56;

	// Restore program counters and branch registers
	pc() = saved_pc;
	lr() = saved_lr;
	ctr()= saved_ctr;
	set_cr(saved_cr);

	// M3a Task 4.4: depth-deferred DEC re-raised on return toward depth 1.
	// W2-0: synthetic-full-edge form of ExcEdgeReRaise (= if-pending re-raise;
	// behavior-identical — see the predicate's header comment).
	if (MachineProfileIsNewWorld() &&
	    ExcEdgeReRaise(0u, 0x8000u,   /* W2-3: + the level-held EXT source */
	                   (VirtClockDECPending(&g_virt_clock) || SheepExcExtPending()
	                    || SheepExcHostIrqPending()) ? 1 : 0))   /* M7: + host latch */
		trigger_interrupt();

#if EMUL_TIME_STATS
	exec68k_time += (clock() - exec68k_start);
#endif
}

// Call MacOS PPC code
uint32 sheepshaver_cpu::execute_macos_code(uint32 tvect, int nargs, uint32 const *args)
{
#if EMUL_TIME_STATS
	macos_exec_count++;
	const clock_t macos_exec_start = clock();
#endif

	// Save program counters and branch registers
	uint32 saved_pc = pc();
	uint32 saved_lr = lr();
	uint32 saved_ctr= ctr();

	// Build trampoline with EXEC_RETURN
	SheepVar32 trampoline = POWERPC_EXEC_RETURN;
	lr() = trampoline.addr();

	gpr(1) -= 64;								// Create stack frame
	uint32 proc = ReadMacInt32(tvect);			// Get routine address
	uint32 toc = ReadMacInt32(tvect + 4);		// Get TOC pointer

	// Save PowerPC registers
	uint32 regs[8];
	regs[0] = gpr(2);
	for (int i = 0; i < nargs; i++)
		regs[i + 1] = gpr(i + 3);

	// Prepare and call MacOS routine
	gpr(2) = toc;
	for (int i = 0; i < nargs; i++)
		gpr(i + 3) = args[i];
	execute(proc);
	uint32 retval = gpr(3);

	// Restore PowerPC registers
	for (int i = 0; i <= nargs; i++)
		gpr(i + 2) = regs[i];

	// Cleanup stack
	gpr(1) += 64;

	// Restore program counters and branch registers
	pc() = saved_pc;
	lr() = saved_lr;
	ctr()= saved_ctr;

	// M3a Task 4.4: depth-deferred DEC re-raised on return toward depth 1.
	// W2-0: synthetic-full-edge form of ExcEdgeReRaise (= if-pending re-raise;
	// behavior-identical — see the predicate's header comment).
	if (MachineProfileIsNewWorld() &&
	    ExcEdgeReRaise(0u, 0x8000u,   /* W2-3: + the level-held EXT source */
	                   (VirtClockDECPending(&g_virt_clock) || SheepExcExtPending()
	                    || SheepExcHostIrqPending()) ? 1 : 0))   /* M7: + host latch */
		trigger_interrupt();

#if EMUL_TIME_STATS
	macos_exec_time += (clock() - macos_exec_start);
#endif

	return retval;
}

// Execute ppc routine
inline void sheepshaver_cpu::execute_ppc(uint32 entry)
{
	// Save branch registers
	uint32 saved_lr = lr();

	SheepVar32 trampoline = POWERPC_EXEC_RETURN;
	WriteMacInt32(trampoline.addr(), POWERPC_EXEC_RETURN);
	lr() = trampoline.addr();

	execute(entry);

	// Restore branch registers
	lr() = saved_lr;

	// M3a Task 4.4: depth-deferred DEC re-raised on return toward depth 1.
	// W2-0: synthetic-full-edge form of ExcEdgeReRaise (= if-pending re-raise;
	// behavior-identical — see the predicate's header comment).
	if (MachineProfileIsNewWorld() &&
	    ExcEdgeReRaise(0u, 0x8000u,   /* W2-3: + the level-held EXT source */
	                   (VirtClockDECPending(&g_virt_clock) || SheepExcExtPending()
	                    || SheepExcHostIrqPending()) ? 1 : 0))   /* M7: + host latch */
		trigger_interrupt();
}

void sheepshaver_cpu::call_get_resource(powerpc_cpu * cpu, uint32 old_get_resource) {
	static_cast<sheepshaver_cpu *>(cpu)->get_resource(old_get_resource);
}

// Resource Manager thunk
inline void sheepshaver_cpu::get_resource(uint32 old_get_resource)
{
	uint32 type = gpr(3);
	int16 id = gpr(4);

	// Create stack frame
	gpr(1) -= 56;

	// Call old routine
	execute_ppc(old_get_resource);

	// Call CheckLoad()
	uint32 handle = gpr(3);
	check_load_invoc(type, id, handle);
	gpr(3) = handle;

	// Cleanup stack
	gpr(1) += 56;
}


/**
 *		SheepShaver CPU engine interface
 **/

// PowerPC CPU emulator
static sheepshaver_cpu *ppc_cpu = NULL;

// M3a Task 4: free-function seam for check_spcflags (declared in ppc-cpu.hpp
// next to HandleInterrupt — same idiom). Newworld-gated at the call site.
bool SheepExcDeliverPending(void)
{
	return ppc_cpu && ppc_cpu->deliver_pending_dec_exception();
}

/* NK-syscall-surface Task A: the sc-side vector-stub shim (Q-S2 pinned ABI,
 * M3A-ENTRY-TABLE.md "Syscall entry resolution (vector 0xC00)").
 *
 * Transcribed from the real 0xC00 vector-stub TEMPLATE at ROM file 0x300c08
 * ([STATIC], never installed at [0xC00] — page probed junk/uninstalled):
 *   mtspr SPRG1,r1 / mflr r1 / mtspr SPRG2,r1 / mfspr r1,SPRG3 /
 *   lwz r1,0x30(r1) / mtlr r1 / blrl
 * Its postconditions reduce to EXACTLY TWO SPR writes the handler consumes:
 *   SPRG1 := caller r1   (consumed at 0x313d48: [KDP+4] := SPRG1 — the
 *                         caller-r1 save the restore path returns through)
 *   SPRG2 := caller LR   (consumed at 0x313d84: r12 := SPRG2 → ctx → exit
 *                         mtlr r12 — the caller's return LR)
 * Everything else the handler consumes (SPRG0=KDP, SPRG3=KDP+0x360,
 * [KDP-0x14] ctx, [KDP-0x10]/[KDP-4]) is NK-maintained staged state,
 * live-verified — no KDP writes, no register mutation, no guest-side seeds.
 *
 * DELIBERATE ASYMMETRY vs the DEC-path KDP shim above (plan rev 2 P-m5;
 * Task-0 finding): the SYSCALL save path saves through the [KDP-0x14] current
 * ctx (the MMCB mid-excursion), NOT [KDP+0x65c] — the DEC-shim's [KDP+0x65c]
 * offset logic does NOT transfer and is intentionally absent here. Only what
 * Q-S2's register table demands is transcribed.
 *
 * Host-side glue helper called from execute_syscall's newworld arm (the DEC
 * precedent; MACHINE-LAYER-PLAN §2d profile-gated slow-path site discipline —
 * keeps powerpc_cpu honest). Called only when the entry resolved (the
 * delivered-sc counter rides here, plan rev 2 P-M4: counters for counts,
 * probes for ABI). selector_r0 is telemetry-only — the shim's architectural
 * effect is exactly the two SPR writes. */
extern "C" void SheepExcSyscallShim(uint32 caller_r1, uint32 caller_lr, uint32 selector_r0)
{
	ppc_cpu->sprg_reg(1) = caller_r1;	// SHIM WRITE #1: SPRG1 := caller r1
	ppc_cpu->sprg_reg(2) = caller_lr;	// SHIM WRITE #2: SPRG2 := caller LR
	exc_stat_delivered_sc++;
	/* Task C: per-selector count (find-or-append; see exc_sc_sel_counts). */
	{
		unsigned i = 0;
		while (i < exc_sc_sel_distinct && exc_sc_sel_counts[i].sel != selector_r0)
			i++;
		if (i < exc_sc_sel_distinct)
			exc_sc_sel_counts[i].n++;
		else if (i < 16) {
			exc_sc_sel_counts[i].sel = selector_r0;
			exc_sc_sel_counts[i].n = 1;
			exc_sc_sel_distinct = i + 1;
		} else
			exc_sc_sel_overflow++;
		if (exc_stat_delivered_sc == 1)
			atexit(exc_dump_sc_selectors);
	}
	// First few deliveries to stderr for live triage (the DEC-delivery idiom);
	// the running total rides the [HB] heartbeat as the 5th exc= field.
	if (exc_stat_delivered_sc <= 5)
		fprintf(stderr, "[EXC] SC delivered #%llu: r0=%08x r1=%08x lr=%08x -> entry=%08x\n",
		        (unsigned long long)exc_stat_delivered_sc, selector_r0,
		        caller_r1, caller_lr, g_exc_entry_table.syscall_entry);
}

/* FE1F-service-surface Task A (plan rev 3): the 0x700-side vector-stub shim —
 * the sc-shim's sibling, one vector over. The NK's 0x700 handler (0x50314700)
 * opens with the SAME save helper the sc handler family uses (bl 0x50313d40),
 * which consumes exactly the two SPRs the real lowmem vector stub writes:
 *   SPRG1 := caller r1   (consumed at 0x313d4c: [KDP+4] := SPRG1)
 *   SPRG2 := caller LR   (consumed at 0x313d84: r12 := SPRG2; the handler's
 *                         fast rfi exit restores LR from SPRG2 / r1 from SPRG1
 *                         at 0x314ad8..0x314ae8)
 * — verified by static disassembly of the raw==patched dump (md5 7b1378be…)
 * this session. Transcribe ONLY what the handler consumes (the sc-shim rule);
 * everything else it reads (SPRG0=KDP, [KDP-0x14] ctx, [KDP+0x648] table base,
 * exit-pointer array [KDP+0x5f0+4·slot]) is NK-maintained staged state.
 *
 * Called from execute_illegal's newworld trap arm (ppc-execute.cpp) when the
 * program entry is RESOLVED, before the transition is applied. trap_word/srr0
 * are telemetry: the slot id rides the placeholder encoding (twi 31,r31,N =
 * 0x0fff000N — the NK decodes it the same way, xoris r8,r8,0x0fff). Slot 15 is
 * the DR allocator-exhaustion path: rung-2 Task T's parked-stop diagnostics
 * moved HERE as telemetry (plan rev 3 item 1 — the slot is a real trap
 * placeholder, restored; exhaustion now reaches the NK's own slot-15 exit). */
extern "C" void SheepExcProgramShim(uint32 caller_r1, uint32 caller_lr,
                                    uint32 trap_word, uint32 srr0)
{
	ppc_cpu->sprg_reg(1) = caller_r1;	// SHIM WRITE #1: SPRG1 := caller r1
	ppc_cpu->sprg_reg(2) = caller_lr;	// SHIM WRITE #2: SPRG2 := caller LR
	exc_stat_delivered_program++;
	const bool is_slot = (trap_word & 0xffff0000u) == 0x0fff0000u;
	const uint32 slot = trap_word & 0xffffu;
	if (exc_stat_delivered_program <= 5) {
		char slotbuf[16];
		if (is_slot) snprintf(slotbuf, sizeof slotbuf, "%u", slot);
		else         snprintf(slotbuf, sizeof slotbuf, "n/a");
		fprintf(stderr, "[EXC] PROGRAM delivered #%llu: srr0=%08x word=%08x slot=%s "
		        "r1=%08x lr=%08x -> entry=%08x\n",
		        (unsigned long long)exc_stat_delivered_program, srr0, trap_word,
		        slotbuf, caller_r1, caller_lr, g_exc_entry_table.program_entry);
	}
	if (is_slot && slot == 15)
		fprintf(stderr, "[EXC] PROGRAM slot-15 (DR allocator EXHAUSTION) #%llu: "
		        "srr0=%08x — pool-sizing tripwire (was Task T's parked stop; "
		        "now delivered to the NK slot-15 exit)\n",
		        (unsigned long long)exc_stat_delivered_program, srr0);
}

// M3a Task 4 telemetry export (heartbeat + crash-path dump).
// M6a W2: + out[3] = deferred_native (the native-excursion DEC fence).
// NK-syscall-surface Task A: + out[4] = delivered_sc (plan rev 2 P-M4).
// FE1F-service-surface Task A: + out[5] = delivered_program (6th exc= field).
// Wave-2 W2-3: + out[6] = delivered_ext (7th field, APPENDED LAST). Emitters
// print it only when SheepExcExtConfigured() — gated-off boots keep the
// 6-field tuple byte-identical to the pre-W2-3 baseline class.
extern "C" void SheepExcStats(uint64_t out[7])
{
	out[0] = exc_stat_delivered_dec;
	out[1] = exc_stat_deferred_ee;
	out[2] = exc_stat_deferred_depth;
	out[3] = exc_stat_deferred_native;
	out[4] = exc_stat_delivered_sc;
	out[5] = exc_stat_delivered_program;
	out[6] = exc_stat_delivered_ext;
}

// C2.0 RPC: dump PPC registers as JSON for the SiliconSheep Inspector
extern "C" void ss_dump_registers_json(char *buf, int bufsz) {
	if (!ppc_cpu) {
		snprintf(buf, bufsz, "{\"error\":\"CPU not initialized\"}");
		return;
	}
	ppc_cpu->dump_regs_json(buf, bufsz);
}

void FlushCodeCache(uintptr start, uintptr end)
{
	D(bug("FlushCodeCache(%08x, %08x)\n", start, end));
	ppc_cpu->invalidate_cache_range(start, end);
}

// Dump PPC registers
static void dump_registers(void)
{
	ppc_cpu->dump_registers();
}

// Dump log
static void dump_log(void)
{
	ppc_cpu->dump_log();
}

static int read_mem(bfd_vma memaddr, bfd_byte *myaddr, int length, struct disassemble_info *info)
{
	Mac2Host_memcpy(myaddr, memaddr, length);
	return 0;
}

static void dump_disassembly(const uint32 pc, const int prefix_count, const int suffix_count)
{
	struct disassemble_info info;
	INIT_DISASSEMBLE_INFO(info, stderr, fprintf);
	info.read_memory_func = read_mem;

	const int count = prefix_count + suffix_count + 1;
	const uint32 base_addr = pc - prefix_count * 4;
	for (int i = 0; i < count; i++) {
		const bfd_vma addr = base_addr + i * 4;
		fprintf(stderr, "%s0x%8llx:  ", addr == pc ? " >" : "  ", addr);
		print_insn_ppc(addr, &info);
		fprintf(stderr, "\n");
	}
}

sigsegv_return_t sigsegv_handler(sigsegv_info_t *sip)
{
#if ENABLE_VOSF
	// Handle screen fault
	extern bool Screen_fault_handler(sigsegv_info_t *sip);
	if (Screen_fault_handler(sip))
		return SIGSEGV_RETURN_SUCCESS;
#endif

	const uintptr addr = (uintptr)sigsegv_get_fault_address(sip);

	// Machine Layer M1: MMIO bus dispatch (MACHINE-LAYER-PLAN.md section 2b, JIT
	// path). Must run BEFORE any legacy skip (and before the ROM-write check
	// below) so no device-space access is silently eaten. Inactive on the
	// paravirtual default (predicted-untaken branch).
	if (mmio_bus_active) {
		uint32 gaddr = (uint32)((uintptr)addr - VMBaseDiff);   // host -> guest
		if (MMIOBusInRange(gaddr)) {
			void *ts = sigsegv_get_thread_state(sip);
			if (ts && MMIOMachFaultDispatch(gaddr, ts))
				return SIGSEGV_RETURN_STATE_MODIFIED;
			fprintf(stderr, "[MMIO] FATAL: in-range fault not serviced (gaddr=0x%08x)\n", gaddr);
			return SIGSEGV_RETURN_FAILURE;
		}
	}

#if HAVE_SIGSEGV_SKIP_INSTRUCTION
	// Ignore writes to ROM
	if ((addr - (uintptr)ROMBaseHost) < ROM_SIZE)
		return SIGSEGV_RETURN_SKIP_INSTRUCTION;

	// Get program counter of target CPU
	sheepshaver_cpu * const cpu = ppc_cpu;
	const uint32 pc = cpu->pc();
	
	// Fault in Mac ROM or RAM?
	bool mac_fault = (pc >= ROMBase && pc < (ROMBase + ROM_AREA_SIZE)) || (pc >= RAMBase && pc < (RAMBase + RAMSize)) || (pc >= DR_CACHE_BASE && pc < (DR_CACHE_BASE + DR_CACHE_SIZE));
	if (mac_fault) {

		// Legacy PC-keyed skip hacks (installer probes, serial-driver device
		// probes). PARAVIRTUAL ONLY: on the newworld fidelity profile and the
		// SS_MMIO_BUS=1 named third config these would silently eat MMIO
		// accesses the bus must see (MACHINE-LAYER-PLAN.md section 2b).
		if (!MachineUsesMMIOBus()) {

			// "VM settings" during MacOS 8 installation
			if (pc == ROMBase + 0x488160 && cpu->gpr(20) == 0xf8000000)
				return SIGSEGV_RETURN_SKIP_INSTRUCTION;

			// MacOS 8.5 installation
			else if (pc == ROMBase + 0x488140 && cpu->gpr(16) == 0xf8000000)
				return SIGSEGV_RETURN_SKIP_INSTRUCTION;

			// MacOS 8 serial drivers on startup
			else if (pc == ROMBase + 0x48e080 && (cpu->gpr(8) == 0xf3012002 || cpu->gpr(8) == 0xf3012000))
				return SIGSEGV_RETURN_SKIP_INSTRUCTION;

			// MacOS 8.1 serial drivers on startup
			else if (pc == ROMBase + 0x48c5e0 && (cpu->gpr(20) == 0xf3012002 || cpu->gpr(20) == 0xf3012000))
				return SIGSEGV_RETURN_SKIP_INSTRUCTION;
			else if (pc == ROMBase + 0x4a10a0 && (cpu->gpr(20) == 0xf3012002 || cpu->gpr(20) == 0xf3012000))
				return SIGSEGV_RETURN_SKIP_INSTRUCTION;

			// MacOS 8.6 serial drivers on startup (with DR Cache and OldWorld ROM)
			else if ((pc - DR_CACHE_BASE) < DR_CACHE_SIZE && (cpu->gpr(16) == 0xf3012002 || cpu->gpr(16) == 0xf3012000))
				return SIGSEGV_RETURN_SKIP_INSTRUCTION;
			else if ((pc - DR_CACHE_BASE) < DR_CACHE_SIZE && (cpu->gpr(20) == 0xf3012002 || cpu->gpr(20) == 0xf3012000))
				return SIGSEGV_RETURN_SKIP_INSTRUCTION;
		}

		// Ignore writes to the zero page (SheepMem; both profiles)
		if ((uint32)(addr - SheepMem::ZeroPage()) < (uint32)SheepMem::PageSize())
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// Ignore all other faults, if requested. PARAVIRTUAL ONLY: on the
		// newworld profile and the SS_MMIO_BUS=1 named third config an
		// unexpected fault must abort loudly, not be silently skipped
		// (MACHINE-LAYER-PLAN.md section 2b / section 6).
		if (!MachineUsesMMIOBus() && PrefsFindBool("ignoresegv"))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
	}
#else
#error "FIXME: You don't have the capability to skip instruction within signal handlers"
#endif

	fprintf(stderr, "SIGSEGV\n");
	fprintf(stderr, "  pc %p\n", sigsegv_get_fault_instruction_address(sip));
	fprintf(stderr, "  ea %p\n", sigsegv_get_fault_address(sip));
	// Machine Layer M1 acceptance instrumentation (Task 12): the atexit MMIO
	// stats dump is skipped when the diagnostic boot dies on a signal, so emit
	// the bus telemetry here, before the heavier register/disasm/trace dumps
	// that can themselves re-fault. The fault PC is a normal guest PC (not a
	// bus dispatch), so no device lock is held — reading the counters is safe.
	if (MachineUsesMMIOBus()) {
		MMIOBusDumpStats(stderr);
		// Machine Layer M3b (Task 3): Cuda protocol counters on the crash path
		// too — same reasoning, counters-read-only (CudaFormatStatsRegistered
		// snprintfs the registered instance's plain counters; returns 0 until
		// bus bring-up registers it, so paravirtual/early crashes stay silent).
		char cuda_stats[512];
		if (CudaFormatStatsRegistered(cuda_stats, sizeof(cuda_stats)))
			fprintf(stderr, "[CUDA] %s\n", cuda_stats);
		CudaDumpPacketTrace(stderr);   // no-op unless SS_CUDA_TRACE=1
		// Wave-2 W2-3: OpenPIC counters + the Q8 first-IACK record on the
		// crash path too (registered-instance formatters return 0 unless the
		// SS_NW_PIC bring-up registered the PIC — gated-off boots unchanged).
		char pic_stats[512];
		if (OpenPICFormatStatsRegistered(pic_stats, sizeof(pic_stats)))
			fprintf(stderr, "[PIC] %s\n", pic_stats);
		if (OpenPICFormatFirstIACKsRegistered(pic_stats, sizeof(pic_stats)))
			fprintf(stderr, "[PIC] first-iacks: %s\n", pic_stats);
	}
	// Machine Layer M2 acceptance instrumentation (Task 8): same reasoning for
	// the virtual-clock telemetry — the [VCLK] atexit dump never runs on the
	// signal-death path, and the seam DoD asserts mtspr_dec/mfspr_dec counts.
	if (VirtClockReady(&g_virt_clock))
		VirtClockDumpStats(&g_virt_clock, stderr);
	// Machine Layer M3a (Task 4): same reasoning for the DEC exception-delivery
	// telemetry — the heartbeat is periodic and the atexit path never runs on
	// signal death, so emit the counters here too. Newworld-only (the hook never
	// runs on paravirtual; keeps paravirtual crash output byte-identical).
	if (MachineProfileIsNewWorld()) {
		uint64_t exc[7];
		SheepExcStats(exc);
		/* W2-3: the 7th field (delivered_ext) prints only when the EXT source
		 * is configured — gated-off boots keep the 6-field line byte-identical. */
		char extbuf[40];
		extbuf[0] = 0;
		if (SheepExcExtConfigured())
			snprintf(extbuf, sizeof extbuf, " delivered_ext=%llu",
			         (unsigned long long)exc[6]);
		fprintf(stderr, "[EXC] delivered_dec=%llu deferred_ee=%llu deferred_depth=%llu "
		        "deferred_native=%llu delivered_sc=%llu delivered_program=%llu%s\n",
		        (unsigned long long)exc[0], (unsigned long long)exc[1],
		        (unsigned long long)exc[2], (unsigned long long)exc[3],
		        (unsigned long long)exc[4], (unsigned long long)exc[5], extbuf);
		/* Task C: the atexit selector dump does not fire on the signal-death
		 * path (same reasoning as the counters above) — dump explicitly. */
		exc_dump_sc_selectors();
		/* M7 Task A: host-irq latch counters on the crash path too (atexit
		 * never runs here). Silent unless SS_NW_HOST_IRQ armed the source. */
		char hirq_stats[160];
		if (SheepExcHostIrqFormatStats(hirq_stats, sizeof(hirq_stats)))
			fprintf(stderr, "[EXC] host-irq: %s\n", hirq_stats);
	}
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	/* SS_DR_R24_RING crash flush — EARLY, right after the counters-only
	 * telemetry and BEFORE dump_registers()/dump_disassembly(), which can
	 * themselves re-fault and kill the process (instr-hardening item 3).
	 * Once-guarded in ppc-cpu.cpp; no-op unless the ring is enabled. */
	ppc_jit_r24ring_crash_flush();
#endif
	dump_registers();
	dump_log();
	dump_disassembly(pc, 8, 8);
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	/* Dump the in-memory JIT execution trace ring (SS_JIT_TRACE_RING=1) —
	 * the block-execution history leading up to this crash. */
	ppc_jit_dump_trace_ring();
#endif

	enter_mon();
	QuitEmulator();

	return SIGSEGV_RETURN_FAILURE;
}

/*
 *  Initialize CPU emulation
 */

/*
 *  SS_TEST_HEX / SS_TEST_DUMP — opcode equivalence test mode
 *
 *  When SS_TEST_HEX is set, SheepShaver injects the given PPC instruction
 *  sequence into RAM, executes it in the normal interpreter, and (if
 *  SS_TEST_DUMP=1) emits a REGDUMP line to stderr with all register state.
 *  This enables the jit-test harness to compare interpreter vs JIT output.
 */

static bool ss_test_dump_enabled()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("SS_TEST_DUMP") && *getenv("SS_TEST_DUMP") &&
		          strcmp(getenv("SS_TEST_DUMP"), "0") != 0) ? 1 : 0;
	return cached != 0;
}

static bool ss_parse_hex_words(const char *hex, uint32 *out, size_t max, size_t *count)
{
	size_t n = 0;
	const char *p = hex;
	while (*p) {
		while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';'))
			p++;
		if (!*p) break;
		if (n >= max) return false;
		char *end = NULL;
		unsigned long v = strtoul(p, &end, 16);
		if (end == p || v > 0xFFFFFFFFUL) return false;
		out[n++] = (uint32)v;
		p = end;
	}
	*count = n;
	return n > 0;
}

/* SS_SEED_MEM guest-memory poke knob (defined in ppc-cpu.cpp). */
extern void ss_seed_mem_apply_immediate(void);
extern void ss_seed_mem_check_pc(uint32_t pc);

/* Batch session state (see ss_run_opcode_test). In batch mode the test RAM, the
 * CPU object, and the JIT code cache are created ONCE and reused across all
 * vectors — reset per vector rather than re-allocated. This is mandatory, not just
 * an optimization: per-vector teardown/setup (new sheepshaver_cpu -> vm_acquire of
 * a fresh decode cache, plus repeated JIT init/exit) accumulates global state
 * (vm_acquire's monotonic next_address bump allocator never reclaims released
 * regions) that crashes the process after a few hundred iterations. Reusing the
 * objects and resetting REGISTER/MEMORY/cache state per vector gives true
 * per-vector isolation (proven by diffing legacy-vs-batch REGDUMPs) without the
 * accumulation. Single-vector mode (s_in_batch == false) keeps allocating and
 * freeing everything per call exactly as before — byte-identical output. */
static bool               s_in_batch          = false;
static sheepshaver_cpu   *s_session_cpu       = NULL;
static uint8             *s_session_ram        = NULL;
static bool               s_session_jit_inited = false;
static const size_t       SS_TEST_RAM_SIZE     = 16 * 1024 * 1024;

/* ---- Wave-2 W2-1: deliverability knobs (harness path ONLY; plan
 * docs/superpowers/plans/2026-06-11-wave2-interrupt-chain.md Task W2-1 +
 * rev 2 F1/F2/F11) ----
 *
 * All knobs default OFF; with none set this is one cached-int test per vector
 * and the legacy 353-vector table is byte-unaffected.
 *
 *  SS_TEST_DEC_PENDING=1  arm the DEC latch (dec_pending=1) before EACH vector
 *                         (re-armed per vector, so batch mode stays isolated).
 *                         The harness clock itself is already live (rev 2 F11:
 *                         VirtClockInitHost runs at main_unix's SS_TEST gate
 *                         before ss_run_opcode_test) — only the latch arm is
 *                         missing. Also maps guest lowmem (the F2 fix, below).
 *  SS_TEST_EXT_PENDING=1  assert the level-held EXT source (the W2-3 PIC-
 *                         output flag) — the EXT-branch analogue of
 *                         SS_TEST_DEC_PENDING (re-asserted per vector; the
 *                         hook never clears a level-held source). Also maps
 *                         the F2 lowmem page and configures the EXCSTAT
 *                         7th field (delivered_ext).
 *  SS_TEST_HOST_IRQ=1     (M7 Task A) arm the HOST once-per-assert-edge latch
 *                         before EACH vector (one assert edge per vector) —
 *                         the latch analogue of SS_TEST_EXT_PENDING. The hook
 *                         CONSUMES this source at delivery (unlike the
 *                         level-held PIC seam), so the EXCSTAT host-irq
 *                         counters pin the once-per-edge contract
 *                         (edges=1 consumed=1 pending=0 after one delivery).
 *                         Also maps the F2 lowmem page + the EXCSTAT fields.
 *  SS_TEST_MSR=0xHEX      initial MSR for the vector. Default (unset) is the
 *                         reset_supervisor_for_test value 0xf072 — byte-
 *                         compatible with all legacy vectors; the EE-edge
 *                         vectors start at 0x7072 (EE=0).
 *  SS_TEST_EXC_STUB=1     plant the capture stub at guest 0x1000C000
 *                         (re-planted per vector — the per-vector RAM memset
 *                         would otherwise erase it in batch mode):
 *                           mfmsr r20; mfspr r21,srr0; mfspr r22,srr1; blr
 *                         The REGDUMP has no MSR/SRR0/SRR1 — the stub captures
 *                         them into GPRs the REGDUMP does carry.
 *                  =2     (W2-4 step 0) the EXTENDED stub: two extra rows
 *                         mfspr r23,sprg1; mfspr r24,sprg2 before the blr —
 *                         makes the 2-SPR shim writes REGDUMP-pinnable (the
 *                         H8 published-DEC conformance vector). "1" plants
 *                         the original stub byte-identically.
 *  SS_TEST_EXC_STATS=1    print one EXCSTAT line (the exc= 6-tuple) after the
 *                         vector — the H4 deferral-telemetry observable.
 *                         Counters are cumulative per process (the exc lane
 *                         runs one process per vector, so absolute == delta).
 *  SS_EXC_ENTRY=...       applied HERE via the shared boot-parse helper
 *                         (rev 2 F1: the boot parse lives in init_emul_ppc,
 *                         which the harness gate exits main() before reaching
 *                         — without this the env var silently never parsed).
 *  SS_MACHINE=newworld    additionally resolves the machine profile here:
 *                         every link in the delivery chain (check_spcflags
 *                         hook, mtmsr/rfi EE edges, execute_syscall) is gated
 *                         on MachineProfileIsNewWorld(), whose resolver
 *                         MachineProfileInit() is boot-path-only. Prefs are
 *                         not initialized on the harness path; PrefsFindString
 *                         returns NULL and the SS_MACHINE env wins (precedence
 *                         rule 1). Unset => no call, no [MACHINE] stderr line,
 *                         legacy behavior untouched.
 *
 * F2 — the lowmem fix. DECISION: MAP a real zero page (not zero-substitute in
 * the hook). The delivery hook's two-phase run-mode sampling reads guest
 * [0x2810] (XLM_RUN_MODE) on a provisional DELIVER — exactly these vectors'
 * case, so the read is LIVE here — and the harness maps only test RAM at
 * 0x10000000. We mmap zero-filled guest [0x0, 0x4000) (host NATMEM_OFFSET+0;
 * fixed length 0x4000 so XLM_RUN_MODE 0x2810 / XLM_IRQ_NEST 0x2818 / Ticks
 * 0x16a are covered regardless of host page size). Rationale for mapping over
 * substitution: the hook's code path stays IDENTICAL to the boot path (zero =
 * MODE_68K = deliverable); a harness-only zero-substitute branch inside the
 * hook would un-test the very guest read W2-2/W2-3 depend on. The page also
 * keeps the legacy HandleInterrupt fall-through (deferral case) read-safe
 * (XLM_IRQ_NEST=0, run_mode=MODE_68K). REAL_ADDRESSING cannot map guest page
 * 0 (it is the host NULL page) — the knob refuses loudly there. */
static void ss_test_exc_knobs_apply(sheepshaver_cpu *cpu, uint8 *test_ram)
{
	static int parsed = 0;
	static int knob_dec_pending = 0;
	static int knob_ext_pending = 0;
	static int knob_host_irq = 0;	/* M7 Task A: the once-per-edge latch knob */
	static int knob_msr_set = 0;
	static uint32 knob_msr = 0;
	static int knob_stub = 0;
	if (!parsed) {
		parsed = 1;
		const char *e;
		e = getenv("SS_TEST_DEC_PENDING");
		knob_dec_pending = (e && e[0] && e[0] != '0') ? 1 : 0;
		/* W2-3: SS_TEST_EXT_PENDING=1 — assert the level-held EXT source (the
		 * PIC-output flag) directly at the seam the delivery hook samples.
		 * This is the harness-level EXT delivery exerciser: no PIC/SCC model
		 * runs here; the knob tests the hook's EXT branch + entry-point
		 * discrimination + the 2-SPR shim, exactly as SS_TEST_DEC_PENDING
		 * tests the DEC branch. Needs the same F2 lowmem page. */
		e = getenv("SS_TEST_EXT_PENDING");
		knob_ext_pending = (e && e[0] && e[0] != '0') ? 1 : 0;
		/* M7 Task A: SS_TEST_HOST_IRQ=1 — the host-latch analogue (one assert
		 * edge per vector; the hook consumes it at delivery). */
		e = getenv("SS_TEST_HOST_IRQ");
		knob_host_irq = (e && e[0] && e[0] != '0') ? 1 : 0;
		e = getenv("SS_TEST_MSR");
		if (e && e[0]) {
			knob_msr = (uint32)strtoul(e, NULL, 0);
			knob_msr_set = 1;
		}
		e = getenv("SS_TEST_EXC_STUB");
		/* W2-4 step 0: "2" plants the EXTENDED stub (adds mfspr r23,sprg1 /
		 * mfspr r24,sprg2) so the 2-SPR shim rows land in REGDUMP-visible
		 * GPRs — the H8 conformance observable. "1" stays byte-identical. */
		knob_stub = (e && e[0] && e[0] != '0') ? (e[0] == '2' ? 2 : 1) : 0;
		/* F1: harness-side entry-table setup via the shared parse. */
		exc_entry_table_apply_env_override();
		/* M7 Task C pre-flip item 1 (W2-4 step-0 review P2), the gate-site
		 * half of the guard: SS_NW_DEC_PUBLISHED selects the 2-SPR shim
		 * shape, and on the harness path NO ROM is mapped — gate-on without
		 * an SS_EXC_ENTRY override aims deliveries at the unmapped
		 * entry-table default. run-exc.sh pins the gate per lane and aborts
		 * on mis-wiring; warn loudly here for ad-hoc SS_TEST_HEX runs. */
		if (exc_dec_published_enabled() && !getenv("SS_EXC_ENTRY"))
			fprintf(stderr, "[EXC-TEST] WARNING: SS_NW_DEC_PUBLISHED is ON with no "
			        "SS_EXC_ENTRY override - DEC deliveries aim the 2-SPR shim at "
			        "entry 0x%08x, which is NOT mapped on the harness path "
			        "(expect an unmapped-entry crash)\n",
			        g_exc_entry_table.interrupt_entry);
		/* Profile resolution (see block comment). */
		e = getenv("SS_MACHINE");
		if (e && e[0])
			MachineProfileInit();
		if (knob_dec_pending || knob_ext_pending || knob_host_irq) {
#if REAL_ADDRESSING
			fprintf(stderr, "SS_TEST_DEC_PENDING/SS_TEST_EXT_PENDING: unsupported "
			        "under REAL_ADDRESSING (guest lowmem page 0 is the host NULL "
			        "page) — knobs ignored\n");
			knob_dec_pending = 0;
			knob_ext_pending = 0;
			knob_host_irq = 0;
#else
			uint8 *lm_host = Mac2HostAddr(0);
			void *lm = mmap((void *)lm_host, 0x4000, PROT_READ | PROT_WRITE,
			                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
			if (lm == MAP_FAILED || lm != (void *)lm_host) {
				fprintf(stderr, "SS_TEST_DEC_PENDING: cannot map guest lowmem page "
				        "at %p: %s\n", (void *)lm_host, strerror(errno));
				exit(1);
			}
			fprintf(stderr, "[EXC-TEST] guest lowmem [0x0,0x4000) mapped zero (F2); "
			        "%s armed per vector%s%s%s\n",
			        knob_dec_pending && knob_ext_pending ? "DEC latch + EXT level"
			        : knob_ext_pending ? "EXT level"
			        : knob_dec_pending ? "DEC latch" : "host-irq latch",
			        knob_host_irq && (knob_dec_pending || knob_ext_pending)
			            ? " + host-irq latch" : "",
			        knob_msr_set ? "; SS_TEST_MSR set" : "",
			        knob_stub ? "; capture stub at 0x1000C000" : "");
#endif
		}
	}
	/* Per-vector application (batch mode re-arms/replants every vector). */
	if (knob_dec_pending) {
		/* The latch word is cross-thread atomic on the boot path; the harness
		 * is single-threaded, but use the module's idiom anyway. */
		__atomic_store_n(&g_virt_clock.dec_pending, 1u, __ATOMIC_RELEASE);
	}
	if (knob_ext_pending) {
		/* Level-held: assert (and re-assert per vector); the hook never clears
		 * it — process teardown is the deassert. Configure first so the
		 * EXCSTAT line carries the 7th field. */
		SheepExcExtConfigure();
		SheepExcExtSetPending(1);
	}
	if (knob_host_irq) {
		/* M7 Task A: one assert edge per vector. The hook CONSUMES this latch
		 * at delivery (once-per-edge) — the EXCSTAT host-irq counters are the
		 * observable (edges=1 consumed=1 pending=0 after one delivery). */
		SheepExcHostIrqConfigure();
		SheepExcHostIrqAssert();
	}
	if (knob_msr_set)
		cpu->set_msr_for_test(knob_msr);
	if (knob_stub) {
		/* SS_TEST_EXC_STUB=1: the W2-1 capture stub, byte-identical to the
		 * original. =2 (W2-4 step 0): the extended stub — the two extra mfspr
		 * rows capture SPRG1/SPRG2 into r23/r24, making the 2-SPR shim writes
		 * (SPRG1:=caller r1, SPRG2:=caller LR) REGDUMP-pinnable (H8). */
		static const uint32 stub[] = {
			0x7E8000A6,	/* mfmsr r20      */
			0x7EBA02A6,	/* mfspr r21,srr0 */
			0x7EDB02A6,	/* mfspr r22,srr1 */
			0x7EF142A6,	/* mfspr r23,sprg1  (stub=2 only) */
			0x7F1242A6,	/* mfspr r24,sprg2  (stub=2 only) */
			0x4E800020,	/* blr            */
		};
		uint8 *p = test_ram + 0xC000;
		size_t n = sizeof(stub) / sizeof(stub[0]);
		size_t i_blr = n - 1;
		for (size_t i = 0; i < n; i++) {
			uint32 w = stub[i];
			if (knob_stub == 1) {
				if (i == 3) w = stub[i_blr];	/* short stub: blr right after srr1 */
				else if (i > 3) w = 0;			/* clear the tail (batch replant) */
			}
			p[4*i + 0] = (w >> 24) & 0xFF;
			p[4*i + 1] = (w >> 16) & 0xFF;
			p[4*i + 2] = (w >> 8)  & 0xFF;
			p[4*i + 3] =  w        & 0xFF;
		}
	}
}

/* W2-1 H4 observable: the exc= 6-tuple as one greppable line. Cached gate —
 * zero output (and zero cost beyond one int test) when the knob is unset. */
static bool ss_test_exc_stats_enabled()
{
	static int cached = -1;
	if (cached < 0) {
		const char *e = getenv("SS_TEST_EXC_STATS");
		cached = (e && e[0] && e[0] != '0') ? 1 : 0;
	}
	return cached != 0;
}

/* Tear down whatever the batch session allocated. Called once after the batch
 * loop finishes. No-op outside batch mode. */
static void ss_test_session_end(void)
{
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	if (s_session_jit_inited) { ppc_jit_aarch64_exit(); s_session_jit_inited = false; }
#endif
	if (s_session_cpu) { delete s_session_cpu; s_session_cpu = NULL; ppc_cpu = NULL; }
	if (s_session_ram) { munmap(s_session_ram, SS_TEST_RAM_SIZE); s_session_ram = NULL; }
}

/* Run a single opcode test vector (hex = space/comma/semicolon-separated 32-bit
 * PPC words). In single-vector mode this is fully self-contained: maps fresh test
 * RAM at a fixed guest address, creates a fresh CPU, executes (interp or JIT per
 * SS_TEST_JIT), emits the REGDUMP, then tears everything down. In batch mode
 * (s_in_batch) it reuses the persistent session resources and resets per-vector
 * state instead — see ss_test_session_end and the batch note in jit-test/run.sh. */
static bool ss_run_one_vector(const char *hex)
{
	if (!(hex && *hex))
		return false;

	uint32 words[1024];
	size_t n_words = 0;
	if (!ss_parse_hex_words(hex, words, sizeof(words)/sizeof(words[0]), &n_words)) {
		fprintf(stderr, "SS_TEST_HEX parse failed\n");
		return true;
	}

	const size_t test_ram_size = SS_TEST_RAM_SIZE;

	/* Guest (Mac) base address of the test RAM. The interpreter translates
	   guest -> host via vm_do_get_real_address(); see how the two addressing
	   models differ below. We use 0x10000000, the same low base the main
	   boot path uses for RAMBase (RAM_BASE in main_unix.cpp). */
	const uint32 test_base = 0x10000000UL;
	uint8 *test_ram = NULL;

	/* Batch: reuse the session RAM mapping (zeroed below) instead of remapping. */
	if (s_in_batch && s_session_ram) {
		test_ram = s_session_ram;
#if REAL_ADDRESSING
		RAMBase = (uint32)(uintptr_t)test_ram;
#else
		RAMBase = test_base;
#endif
		RAMBaseHost = test_ram;
		RAMSize = test_ram_size;
		goto have_ram;
	}

#if REAL_ADDRESSING
	/* REAL_ADDRESSING: guest address == host address (as uint32), so the
	   backing store MUST live in the low 32-bit host address space. This is
	   the original Linux/x86 path and is left exactly as it was. */
	test_ram = (uint8 *)mmap(
		(void *)(uintptr_t)test_base, test_ram_size,
		PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
		-1, 0);
	if (test_ram == MAP_FAILED) {
		/* Retry without FIXED hint */
		test_ram = (uint8 *)mmap(NULL, test_ram_size,
			PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS,
			-1, 0);
	}
	if (test_ram == MAP_FAILED || (uintptr_t)test_ram > 0xFFFFFFFFUL) {
		fprintf(stderr, "SS_TEST: cannot allocate RAM in low 4GB\n");
		if (test_ram != MAP_FAILED) munmap(test_ram, test_ram_size);
		return true;
	}
	/* For REAL_ADDRESSING the host pointer is the guest address itself, so
	   RAMBase must reflect where the buffer actually landed. */
	RAMBase = (uint32)(uintptr_t)test_ram;
#else
	/* DIRECT_ADDRESSING (macOS arm64): host = NATMEM_OFFSET + guest (see
	   vm_do_get_real_address in vm.hpp). The guest address stays a small
	   32-bit value (test_base); the backing store lives at the corresponding
	   high host address NATMEM_OFFSET + test_base, exactly like the working
	   main boot path (main_unix.cpp: vm_mac_acquire_fixed(RAM_BASE, ...)).
	   No PROT_EXEC: the interpreter never executes from guest RAM directly,
	   and the JIT uses its own MAP_JIT code cache.
	   (Braced so test_host's scope ends before the have_ram label — a goto may
	   not jump over a variable initialization that is still in scope.) */
	{
		uint8 *test_host = Mac2HostAddr(test_base);
		test_ram = (uint8 *)mmap(
			(void *)test_host, test_ram_size,
			PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
			-1, 0);
		if (test_ram == MAP_FAILED || test_ram != test_host) {
			fprintf(stderr, "SS_TEST: cannot map test RAM at %p (guest 0x%08x): %s\n",
				(void *)test_host, test_base, strerror(errno));
			if (test_ram != MAP_FAILED) munmap(test_ram, test_ram_size);
			return true;
		}
	}
	/* RAMBase is the guest address; RAMBaseHost is the host pointer. */
	RAMBase = test_base;
#endif

	RAMBaseHost = test_ram;
	RAMSize = test_ram_size;
	if (s_in_batch)
		s_session_ram = test_ram;   /* first batch vector: remember for reuse */

have_ram:
	/* Always start each vector from zeroed RAM (fresh mapping is already zero;
	   a reused session mapping must be re-cleared so prior vectors don't bleed). */
	memset(test_ram, 0, test_ram_size);

	const uint32 code_offset = 0x4000;
	const uint32 stack_offset = test_ram_size - 0x4000;

	/* Write PPC instructions directly to host buffer (big-endian) */
	for (size_t i = 0; i < n_words; i++) {
		uint8 *p = test_ram + code_offset + i * 4;
		p[0] = (words[i] >> 24) & 0xFF;
		p[1] = (words[i] >> 16) & 0xFF;
		p[2] = (words[i] >> 8)  & 0xFF;
		p[3] =  words[i]        & 0xFF;
	}
	/* Append blr (0x4E800020) */
	{
		uint8 *p = test_ram + code_offset + n_words * 4;
		p[0] = 0x4E; p[1] = 0x80; p[2] = 0x00; p[3] = 0x20;
	}
	/* Place the POWERPC_EXEC_RETURN sentinel at offset 0x8000 so that when the
	   appended blr returns to LR the interpreter's execute loop terminates
	   cleanly. (Big-endian.) */
	{
		const uint32 ret_offset = 0x8000;
		uint8 *p = test_ram + ret_offset;
		p[0] = (POWERPC_EXEC_RETURN >> 24) & 0xFF;
		p[1] = (POWERPC_EXEC_RETURN >> 16) & 0xFF;
		p[2] = (POWERPC_EXEC_RETURN >> 8)  & 0xFF;
		p[3] =  POWERPC_EXEC_RETURN        & 0xFF;
	}

	/* Guest (Mac) addresses handed to the CPU. In REAL these equal the host
	   pointers (RAMBase == host base); in DIRECT they are test_base-relative
	   guest addresses that vm_do_get_real_address() maps back to test_ram. */
	uint32 test_addr = RAMBase + code_offset;
	uint32 stack_addr = RAMBase + stack_offset;
	uint32 blr_addr = RAMBase + code_offset + (n_words + 1) * 4;

	/* Default test execution goes through the interpreter (cpu->execute below).
	   On aarch64 powerpc_cpu::execute() otherwise re-enters the aarch64 JIT by
	   default (gated only by SS_USE_JIT), and that JIT still emits REAL-style
	   memory accesses — it treats the 32-bit guest address as a host pointer,
	   which faults under DIRECT_ADDRESSING (Task 2c will port the codegen).
	   So unless the caller explicitly opted into JIT mode via SS_TEST_JIT, pin
	   the test CPU to the interpreter by forcing SS_USE_JIT=0 before the first
	   execute() reads it (the gate caches the value in a static). The explicit
	   SS_TEST_JIT path below is unaffected; it drives the JIT directly. */
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	{
		const char *want_jit = getenv("SS_TEST_JIT");
		if (!(want_jit && *want_jit && strcmp(want_jit, "0") != 0) && !getenv("SS_USE_JIT"))
			setenv("SS_USE_JIT", "0", 1);
	}
#endif

	/* Create CPU (batch: reuse the session CPU, resetting its caches + spcflags so
	   the same guest PC recompiles fresh and no execute-return flag bleeds over). */
	sheepshaver_cpu *cpu;
	if (s_in_batch && s_session_cpu) {
		cpu = s_session_cpu;
		cpu->invalidate_cache();          /* clear interp block + decode cache */
		cpu->reset_fp_vec_for_test();       /* FPR/VR/FPSCR/vrsave back to fresh-CPU state */
		cpu->reset_spcflags_for_test();     /* AFTER invalidate_cache (which sets a flag) */
		cpu->reset_supervisor_for_test();   /* Wave 0: sprg/sdr1/bat/srr0/srr1/sr/msr */
	} else {
		cpu = new sheepshaver_cpu();
		if (s_in_batch)
			s_session_cpu = cpu;
	}
	ppc_cpu = cpu;

	for (int i = 0; i < 32; i++)
		cpu->set_register(powerpc_registers::GPR(i), any_register((uint32)0));
	cpu->set_register(powerpc_registers::GPR(1), any_register(stack_addr));
	cpu->set_register(powerpc_registers::LR, any_register(RAMBase + 0x8000));
	cpu->set_register(powerpc_registers::CTR, any_register((uint32)0));
	cpu->set_register(powerpc_registers::CR, any_register((uint32)0));
	cpu->set_register(powerpc_registers::XER, any_register((uint32)0));

	/* Optional: seed registers */
	const char *init = getenv("SS_TEST_INIT");
	if (init && *init) {
		uint32 init_vals[33];
		size_t init_count = 0;
		if (ss_parse_hex_words(init, init_vals, 33, &init_count) && init_count >= 32) {
			for (int i = 0; i < 32; i++)
				cpu->set_register(powerpc_registers::GPR(i), any_register(init_vals[i]));
			if (init_count >= 33)
				cpu->set_register(powerpc_registers::CR, any_register(init_vals[32]));
		}
	}

	/* Wave-2 W2-1: deliverability knobs (default-off; see ss_test_exc_knobs_apply).
	 * AFTER all register setup (so SS_TEST_MSR overrides the reset value) and
	 * AFTER the RAM memset above (so the capture stub survives into this vector). */
	ss_test_exc_knobs_apply(cpu, test_ram);

	/* Execute */
	/* Execute — either via JIT or interpreter */
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	{
		const char *use_jit = getenv("SS_TEST_JIT");
		if (use_jit && *use_jit && strcmp(use_jit, "0") != 0) {
			/* Batch: init the JIT once for the session, then flush (reset the code
			   cache to base) per vector so the same guest PC recompiles fresh.
			   Single: init/exit around this one vector as before. */
			bool jit_ready;
			if (s_in_batch) {
				if (!s_session_jit_inited)
					s_session_jit_inited = ppc_jit_aarch64_init(1024);
				else
					ppc_jit_aarch64_flush();
				jit_ready = s_session_jit_inited;
			} else {
				jit_ready = ppc_jit_aarch64_init(1024);
			}
			if (jit_ready) {
				ppc_jit_block jblk;
				if (ppc_jit_aarch64_compile(test_addr, test_ram, test_ram_size, &jblk)) {
					fprintf(stderr, "SS_TEST_JIT: compiled %d PPC insns -> %zu bytes native (complete=%d)\n",
						jblk.n_insns, jblk.code_size, jblk.complete);
					/* Dispatch loop: a single compiled block ends at a block
					 * terminator (taken branch / blr / illegal) and writes the
					 * next guest PC to regs->pc. To match the interpreter, keep
					 * compiling and running the block at the current PC until we
					 * reach the return sentinel (LR target RAMBase+0x8000) or hit
					 * an instruction the JIT cannot compile (then hand off to the
					 * interpreter for the remainder). A bounded iteration guard
					 * prevents runaway loops in pathological test vectors. */
					const uint32 sentinel = RAMBase + 0x8000;
					/* Register this cpu for the inline-interpreter-call bridge:
					 * the JIT is driven below without going through execute(),
					 * so the bridge's s_active_cpu would otherwise be NULL. */
					cpu->jit_set_active();
					cpu->set_register(powerpc_registers::PC, any_register(test_addr));
					int guard = 0;
					bool jit_ok = true;
					for (;;) {
						uint32 cur = (uint32)cpu->get_register(powerpc_registers::PC).i;
						if (cur == sentinel) break;
						if (++guard > 100000) { jit_ok = false; break; }
						if (!ppc_jit_aarch64_compile(cur, test_ram, test_ram_size, &jblk)
						    || !jblk.complete) {
							/* Can't JIT this point — let the interpreter finish. */
							jit_ok = false;
							break;
						}
						/* SS_SEED_MEM (PC-triggered): mirror the execute()-loop JIT
						 * block-entry hook here, since the harness drives the JIT
						 * directly (bypassing execute()). Fires seeds at this block PC. */
						ss_seed_mem_check_pc(cur);
						ppc_jit_entry_fn fn = (ppc_jit_entry_fn)(void *)jblk.code;
						fn(cpu->regs_for_jit());
					}
					fprintf(stderr, "SS_TEST_JIT: native execution complete\n");
					if (!s_in_batch) ppc_jit_aarch64_exit();
					if (jit_ok)
						goto regdump;
					/* Fall through to interpreter to complete from current PC. */
					cpu->execute((uint32)cpu->get_register(powerpc_registers::PC).i);
					goto regdump;
				}
				if (!s_in_batch) ppc_jit_aarch64_exit();
			}
			fprintf(stderr, "SS_TEST_JIT: fallback to interpreter\n");
		}
	}
#endif
	cpu->execute(test_addr);

regdump:

	/* Dump registers */
	if (ss_test_dump_enabled()) {
		fprintf(stderr, "REGDUMP:");
		for (int i = 0; i < 32; i++)
			fprintf(stderr, " GPR%d=%08x", i, (unsigned)cpu->get_register(powerpc_registers::GPR(i)).i);
		fprintf(stderr, " CR=%08x", (unsigned)cpu->get_register(powerpc_registers::CR).i);
		fprintf(stderr, " LR=%08x", (unsigned)cpu->get_register(powerpc_registers::LR).i);
		fprintf(stderr, " CTR=%08x", (unsigned)cpu->get_register(powerpc_registers::CTR).i);
		fprintf(stderr, " XER=%08x", (unsigned)cpu->get_register(powerpc_registers::XER).i);
		/* FPR/VR raw bits: dumped so the harness (which raw-diffs the REGDUMP line, interp vs
		 * JIT) actually compares floating-point and AltiVec results. Without these, any result
		 * that lands only in an FPR/VR was invisible — the root cause of the vsel/ev_mixed/
		 * vacuous-FP bugs slipping through. Bits are dumped raw (consistent both modes), so the
		 * differential is valid regardless of the interpreter's ev_mixed VR byte order. */
		for (int i = 0; i < 32; i++) {
			double fd = cpu->fpr(i);
			uint64 fbits; memcpy(&fbits, &fd, sizeof(fbits));
			fprintf(stderr, " FPR%d=%016llx", i, (unsigned long long)fbits);
		}
		for (int i = 0; i < 32; i++) {
			const powerpc_vr &v = cpu->vr(i);
			fprintf(stderr, " VR%d=%08x%08x%08x%08x", i,
			        (unsigned)v.w[0], (unsigned)v.w[1], (unsigned)v.w[2], (unsigned)v.w[3]);
		}
		fprintf(stderr, "\n");
	}

	/* Wave-2 W2-1 (H4 observable): the exc= tuple, one line, knob-gated —
	 * SS_TEST_EXC_STATS unset => no output, legacy REGDUMP stream unchanged.
	 * W2-3: the 7th field (delivered_ext) appends only when the EXT source is
	 * configured (the SS_TEST_EXT_PENDING knob) — pre-W2-3 lane greps intact. */
	if (ss_test_exc_stats_enabled()) {
		uint64_t exc[7];
		SheepExcStats(exc);
		char extbuf[40];
		extbuf[0] = 0;
		if (SheepExcExtConfigured())
			snprintf(extbuf, sizeof extbuf, " delivered_ext=%llu",
			         (unsigned long long)exc[6]);
		/* M7 Task A: host-irq latch counters append only under the
		 * SS_TEST_HOST_IRQ knob (the 7th-field byte-identical idiom). */
		char hirqbuf[176];
		hirqbuf[0] = 0;
		if (SheepExcHostIrqEnabled()) {
			char inner[160];
			SheepExcHostIrqFormatStats(inner, sizeof inner);
			snprintf(hirqbuf, sizeof hirqbuf, " host_irq[%s]", inner);
		}
		fprintf(stderr, "EXCSTAT: delivered_dec=%llu deferred_ee=%llu "
		        "deferred_depth=%llu deferred_native=%llu delivered_sc=%llu "
		        "delivered_program=%llu%s%s\n",
		        (unsigned long long)exc[0], (unsigned long long)exc[1],
		        (unsigned long long)exc[2], (unsigned long long)exc[3],
		        (unsigned long long)exc[4], (unsigned long long)exc[5], extbuf,
		        hirqbuf);
	}

	/* Batch keeps the CPU + RAM alive for the next vector (torn down once by
	   ss_test_session_end). Single mode frees everything here, as before. */
	if (!s_in_batch) {
		delete cpu;
		ppc_cpu = NULL;
		munmap(test_ram, test_ram_size);
	}
	return true;
}

/*
 *  Opcode test entry point. Two modes:
 *
 *   - Single (SS_TEST_HEX=...): runs one vector. Output is byte-identical to the
 *     historical path (just the REGDUMP line).
 *
 *   - Batch (SS_TEST_HEX_FILE=/path): runs ALL vectors in ONE process. The file
 *     is one vector per line, "name<TAB>hexwords". Each vector is preceded by a
 *     "=== VECTOR name ===" frame marker on stderr so the harness can split the
 *     combined output, then runs through ss_run_one_vector() against the persistent
 *     session resources (one CPU, one RAM mapping, one JIT cache), with per-vector
 *     RAM zero + register reset + cache invalidate/flush giving full per-vector
 *     register/memory isolation. This collapses the harness from ~700 process
 *     launches to 2 — see ss_run_one_vector / ss_test_session_end.
 */
bool ss_run_opcode_test(void)
{
	const char *file = getenv("SS_TEST_HEX_FILE");
	if (file && *file) {
		FILE *f = fopen(file, "r");
		if (!f) {
			fprintf(stderr, "SS_TEST_HEX_FILE: cannot open %s: %s\n", file, strerror(errno));
			return true;
		}
		s_in_batch = true;
		char line[8192];
		while (fgets(line, sizeof(line), f)) {
			/* Strip trailing newline / CR */
			size_t len = strlen(line);
			while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
				line[--len] = '\0';
			if (len == 0) continue;
			/* Split "name<TAB>hex". Tolerate spaces around the tab. */
			char *tab = strchr(line, '\t');
			if (!tab) continue;        /* malformed line: skip */
			*tab = '\0';
			char *name = line;
			char *hex = tab + 1;
			while (*name == ' ') name++;
			while (*hex == ' ' || *hex == '\t') hex++;
			fprintf(stderr, "=== VECTOR %s ===\n", name);
			fflush(stderr);
			ss_run_one_vector(hex);
			fflush(stderr);
		}
		fclose(f);
		ss_test_session_end();
		s_in_batch = false;
		return true;
	}

	const char *hex = getenv("SS_TEST_HEX");
	if (!(hex && *hex))
		return false;
	return ss_run_one_vector(hex);
}
void init_emul_ppc(void)
{
	// Export jitcachesize pref as env var for ppc-cpu.cpp (which can't include prefs.h).
	// The pref value is in bytes (after K/M/G suffix parsing); convert to KB for the
	// env var since ppc_jit_aarch64_init() takes KB.
	if (!getenv("SS_JIT_CACHE_KB")) {
		int32 bytes = PrefsFindInt32("jitcachesize");
		if (bytes > 0) {
			int32 kb = bytes / 1024;
			if (kb < 1024) kb = 1024;       /* minimum 1 MB */
			if (kb > 1048576) kb = 1048576;  /* maximum 1 GB */
			char buf[32];
			snprintf(buf, sizeof(buf), "%d", kb);
			setenv("SS_JIT_CACHE_KB", buf, 0);
		}
	}

	// Get pointer to KernelData in host address space
	kernel_data = (KernelData *)Mac2HostAddr(KERNEL_DATA_BASE);

	// Initialize main CPU emulator
	ppc_cpu = new sheepshaver_cpu();
	ppc_cpu->set_register(powerpc_registers::GPR(3), any_register((uint32)ROMBase + 0x30d000));
	ppc_cpu->set_register(powerpc_registers::GPR(4), any_register(KernelDataAddr + 0x1000));
	// SS_NW_TRAMPOLINE (New World parcels probe, default off): the parcels nanokernel expects the
	// Trampoline bootloader to have set SPRG0 = per-CPU/KDP base (it does `mfsprg r1,0` BEFORE ever
	// setting it). SheepShaver runs no Trampoline, so SPRG0=0 -> garbage KDP -> the first spinlock
	// (ROM 0x312700, boot block 12) reads a garbage lock word and deadlocks. Seed SPRG0 with the
	// KernelData base as a first probe; watch the next read via SS_LOG_FIRST_BLOCKS. 1.1 sets its own
	// SPRG0 in Init, so this initial value is harmless there. See NEW-WORLD-ROM-SUPPORT-PLAN.md.
	if (MachineProfileIsNewWorld()) {
		/* P1: SPRG0 = per-CPU block base.
		 * P2: the nanokernel indexes KDP at NEGATIVE offsets (lock word at [KDP-0xb50]).
		 * P3 (new): the nanokernel's pool/heap allocator initializes a free-list at KDP-0x7000
		 * (0x68FF7000). KERNEL_AREA_SIZE is only 0x2000 — the shmem mapping covers
		 * [KDP-0x2000, KDP+0x2000) after SHMLBA alignment. Everything below KDP-0x2000 is
		 * UNMAPPED, so the pool init's stores silently fault (ignoresegv skips them), the pool
		 * data structure is never written, the allocator reads garbage, and the zeroing loop
		 * stalls. Fix: explicitly map and zero 0x8000 bytes below the kernel-data shmem base.
		 * The shmem base is at (KernelDataAddr & ~(SHMLBA-1)); we back the region just below it.
		 * Default off; gated so the 1.1 path is untouched. See SPRG0-KDP-DESIGN.md. */
		const uint32 kdp = KernelDataAddr;
		const uint32 sub_kdp_size = 0x8000;
		const uint32 shmem_base = kdp & ~0x3FFF;  // SHMLBA=0x4000 on arm64
		const uint32 sub_kdp_base = shmem_base - sub_kdp_size;
		if (vm_acquire_fixed(Mac2HostAddr(sub_kdp_base), sub_kdp_size) < 0) {
			fprintf(stderr, "[NW-TRAMP] WARNING: failed to map sub-KDP region [%08x..%08x): %s\n",
			        sub_kdp_base, shmem_base, strerror(errno));
		} else {
			memset(Mac2HostAddr(sub_kdp_base), 0, sub_kdp_size);
			fprintf(stderr, "[NW-TRAMP] mapped+zeroed sub-KDP pool region [%08x..%08x) (%u KB)\n",
			        sub_kdp_base, shmem_base, sub_kdp_size / 1024);
		}

		/* P4+P5: Kernel memory layout matching Init.s expectations.
		 *
		 * Init.s (the NanoKernel's boot code) computes the memory layout from r13/r14/r15
		 * (which rom_patches.cpp seeds via lis instructions at 0x310008). It expects:
		 *   HTAB_base = r13 + r15 - r14   (HTAB at TOP of kernel memory)
		 *   KDP       = (HTAB_base & ~0xFFFF) - 0x2000
		 *   KernelMemoryBase = r13         (bottom of allocation)
		 *   KernelMemoryEnd  = r13 + r15   (top = end of HTAB)
		 *
		 * The free-list priming loop (Reset.s, at ~0x503121B4 in decompressed ROM) builds
		 * page descriptors growing UPWARD via `stwu r31, 4(r29)`, starting at
		 * KernelMemoryBase - 4. For 256MB RAM / 4KB pages = 65536 entries × 4 bytes = 256KB.
		 * (NB: the ROM patch `desc_create` NOPs this stwu for OldWorld; for NW, the
		 * g_rom_904_lenient path skips that NOP so the real instruction executes.)
		 *
		 * Memory layout (ascending addresses):
		 *   68FB0000  [kmem_base]      — KernelMemoryBase (r13)
		 *             |                  page descriptors grow UPWARD (256KB)
		 *   68FF0000  |                  (end of descriptor region)
		 *   68FF4000  [sub_kdp_base]   — sub-KDP pool + IRP (32KB, zeroed)
		 *   68FFC000  [shmem_base]     — KDP shmem alignment boundary
		 *   68FFE000  [kdp]            — Kernel Data Page (8KB)
		 *   69000000  [htab_base]      — HTAB (64KB, at KDP+0x2000)
		 *   69010000  [kmem_end]       — KernelMemoryEnd (r13+r15)
		 *
		 * Init.s will recompute KDP from HTAB placement. For it to land on our
		 * KernelDataAddr (0x68FFE000), HTAB must be at KDP+0x2000 = 0x69000000. */
		const uint32 htab_size = 0x10000;  // 64KB minimum HTAB
		const uint32 htab_base = kdp + 0x2000;  // Init.s: KDP = (HTAB & ~0xFFFF) - 0x2000
		if (vm_acquire_fixed(Mac2HostAddr(htab_base), htab_size) < 0) {
			fprintf(stderr, "[NW-TRAMP] WARNING: failed to map HTAB region [%08x..%08x): %s\n",
			        htab_base, htab_base + htab_size, strerror(errno));
		} else {
			memset(Mac2HostAddr(htab_base), 0, htab_size);
			ppc_cpu->gpr(0) = 0;
			ppc_cpu->sdr1_reg() = htab_base;  // HTABMASK=0 → 64KB
			fprintf(stderr, "[NW-TRAMP] HTAB mapped [%08x..%08x) (%u KB), SDR1=%08x\n",
			        htab_base, htab_base + htab_size, htab_size / 1024, htab_base);
		}

		/* Page descriptor region: the ROM's free-list builder (stwu r31,4(r29))
		 * writes page descriptors UPWARD starting at KernelMemoryBase.
		 * For 256MB RAM / 4KB pages = 65536 entries × 4 bytes = 256KB.
		 * KernelMemoryBase must be low enough that the descriptors don't overwrite
		 * the sub-KDP pool, IRP, or KDP. Place it so descriptors end before the
		 * sub-KDP pool (0x68FF4000), leaving a safety gap. */
		const uint32 ram_size_bytes = RAMSize;
		const uint32 page_count = ram_size_bytes / 4096;
		const uint32 pgdesc_size = (page_count * 4 + 0xFFF) & ~0xFFF;
		const uint32 kmem_end  = htab_base + htab_size;
		// KernelMemoryBase: must be 64KB-aligned (lis-compatible) and leave room for
		// pgdesc_size bytes of descriptors ABOVE it before hitting sub_kdp_base.
		const uint32 kmem_base = (sub_kdp_base - pgdesc_size) & ~0xFFFF;
		if (vm_acquire_fixed(Mac2HostAddr(kmem_base), sub_kdp_base - kmem_base) < 0) {
			fprintf(stderr, "[NW-TRAMP] WARNING: failed to map page-descriptor region [%08x..%08x): %s\n",
			        kmem_base, sub_kdp_base, strerror(errno));
		} else {
			memset(Mac2HostAddr(kmem_base), 0, sub_kdp_base - kmem_base);
			fprintf(stderr, "[NW-TRAMP] page-descriptor region [%08x..%08x) (%u KB) for %u pages\n",
			        kmem_base, sub_kdp_base, (sub_kdp_base - kmem_base) / 1024, page_count);
		}
		fprintf(stderr, "[NW-TRAMP] KernelMemoryBase=%08x, KernelMemoryEnd=%08x (total=%08x)\n",
		        kmem_base, kmem_end, kmem_end - kmem_base);

		/* P6: NKSystemInfo — GPR(5) points to firmware info block so Init.s
		 * copies it into KDP.SysInfo (KDP+0xC00 region).
		 * Place at sub_kdp_base (already mapped+zeroed, safe from descriptor writes). */
		const uint32 sysinfo_addr = sub_kdp_base;
		memset(Mac2HostAddr(sysinfo_addr), 0, 0x120);
		WriteMacInt32(sysinfo_addr + 0x000, ram_size_bytes);  // PhysicalMemorySize
		WriteMacInt32(sysinfo_addr + 0x030, RAMBase);         // Bank0Start
		WriteMacInt32(sysinfo_addr + 0x034, ram_size_bytes);  // Bank0Size
		ppc_cpu->gpr(5) = sysinfo_addr;
		fprintf(stderr, "[NW-TRAMP] NKSystemInfo@%08x: PhysMem=%08x Bank0=[%08x..+%08x)\n",
		        sysinfo_addr, ram_size_bytes, (uint32)RAMBase, ram_size_bytes);

		/* P7: IRP (Info Record Page) — the skipped cold-init (0x5031008C) normally
		 * sets [KDP-0x20] = KDP - 0xA000. The free-list bank scan (Reset.s) reads
		 * bank entries from IRP+0xDF0..IRP+0xEBC (26 eight-byte {start,size} pairs)
		 * via: r30 = [KDP-0x20]; r19 = r30 + 0xDE8; then iterates lwzu/lwz at r19.
		 * Without this pointer, r30=0 and the scan reads guest low memory (all zeros),
		 * producing r22=0xFFFFFFFC (no pages found) and an ~infinite mapping loop. */
		const uint32 irp_base = kdp - 0xA000;  // = 0x68FF4000 (inside sub-KDP pool)
		WriteMacInt32(irp_base + 0xDF0, RAMBase);         // Bank0Start
		WriteMacInt32(irp_base + 0xDF4, ram_size_bytes);  // Bank0Size
		// Banks 1-25 already zero (sub-KDP pool was zeroed above)
		fprintf(stderr, "[NW-TRAMP] IRP=%08x, bank0@%08x=[%08x..+%08x)\n",
		        irp_base, irp_base + 0xDF0, (uint32)RAMBase, ram_size_bytes);

		/* M6a Wave 2 item #1 (M6A-WAVE2-SHIM-RECON.md §2 Option A, §3 row 1):
		 * seed the NK hardware-info record ('Hnfo') behind [KDP+0xfd0] so the ROM's
		 * 68k machine detect (0x5000afb4: move.l ([$68ffefd0],$70),d0 / cmpi.l
		 * #'Hnfo' / move.w ([$68ffefd0],$76),d0 — byte-verified in the raw parcels
		 * image) takes the data-driven table-match path (0xAB86, record list @0xE15C,
		 * id 0x3035 = [0xE1DC]) and the raw machine-probe dispatcher at ROM+0xAD7C —
		 * which jumps into AddrMap DATA and F-line-faults (memo §1.2/§1.3) — is
		 * never reached.
		 *
		 * Placement: record at IRP+0xf00 = 0x68ff4f00, in the mapped+zeroed sub-KDP
		 * pool (clear of the NKSystemInfo block at +0x000..0x120, the IRP bank table
		 * at +0xDF0..0xEBC, and the NK pool free-list at KDP-0x7000). This address
		 * ALSO satisfies the PPC-side sibling check [[KDP-0x20]+0xf70] == 'Hnfo'
		 * (memo §2.4 item 4 / §6.5 same-record hypothesis: [KDP+0xfd0] =
		 * [KDP-0x20]+0xf00 puts the +0x70 tag at IRP+0xf70) for free.
		 *
		 * Record layout (memo §2 Option A + §6.4):
		 *   +0x08  pointer to a writable scratch record — the copy-out at ROM+0xAC20
		 *          (movea.l ([$68ffefd0],$8),a0; byte-verified) writes scratch
		 *          fields +0x10..+0x16
		 *   +0x70  'Hnfo' tag (0x486e666f)
		 *   +0x76  machine id WORD = 0x3035 (the id the universal_info patch's
		 *          synthesized record carries; present in the parcels record table
		 *          at 0xE1DC)
		 *   rest   zero (pool pre-zeroed above)
		 * Scratch record at 0x68ff5000 — the machine-detect copy-out writes
		 * scratch bytes +0x10..+0x17 there, so it is NOT free space.  Rung 2
		 * Task T: the MM save-record pool, briefly staged at this same address
		 * (the rev 2 C1 collision), is relocated to 0x68ff5800; the scratch
		 * page reserve is 0x68ff5000..0x68ff57ff.  Authoritative allocation:
		 * the "Sub-KDP occupancy map" in M6A-ONGOING-ENTRY-DESIGN.md
		 * ("Rung 2 contracts") — consult/extend it before placing ANYTHING in
		 * [0x68ff4000..0x68ffc000).
		 *
		 * NOTE: the [KDP+0xfd0] POINTER lives in the KDP page, which NK cold-init
		 * partially wipes (cf. the KDP+0x6b4 cap clobber, comment below) — the
		 * table[0] trampoline (rom_patches.cpp, PatchROM_NW_trampoline) re-asserts
		 * it GUEST-SIDE after NK init; the record body here is the durable part. */
		const uint32 hnfo_rec     = irp_base + 0xf00;   // 0x68ff4f00
		const uint32 hnfo_scratch = irp_base + 0x1000;  // 0x68ff5000
		WriteMacInt32(hnfo_rec + 0x08, hnfo_scratch);   // writable scratch (0xAC20 copy-out)
		WriteMacInt32(hnfo_rec + 0x70, 0x486e666f);     // 'Hnfo' tag
		WriteMacInt16(hnfo_rec + 0x76, 0x3035);         // machine id (memo: [0xE1DC])
		WriteMacInt32(kdp + 0xfd0, hnfo_rec);           // [KDP+0xfd0] → record
		fprintf(stderr, "[NW-TRAMP] W2 'Hnfo' record @%08x ([KDP+0xfd0]), id=0x3035, "
		        "scratch=%08x (machine-detect data path, memo §2 Option A)\n",
		        hnfo_rec, hnfo_scratch);

		ppc_cpu->sprg_reg(0) = kdp;
		// M3a (Boot-A root cause): the cold MSR fiction 0xf072 claims EE=1 from the
		// first instruction, so the first DEC expiry delivered into NK COLD-INIT
		// (all registers zero, LR=0 -> the handler's r7-flag blr exit jumped to 0).
		// Architecturally reset MSR has EE=0; the OS enables interrupts when ready.
		// Seed the newworld boot MSR as the fiction MINUS EE (0x7072): delivery
		// defers (deferred_ee telemetry) until the NK genuinely raises EE, at which
		// point the Task-3 EE-edge re-raise delivers at the correct moment.
		// Paravirtual keeps 0xf072 (cold value in init_registers, untouched).
		ppc_cpu->msr_reg() = 0x7072;
		fprintf(stderr, "[EXC] boot MSR seeded 0x7072 (EE=0 until the NK enables interrupts)\n");
		memset(Mac2HostAddr(kdp - 0x1000), 0, 0x1000);
		WriteMacInt32(kdp - 4, kdp);
		WriteMacInt32(kdp - 0x20, irp_base);  // [KDP-0x20] = IRP base
		fprintf(stderr, "[NW-TRAMP] SPRG0=%08x, [SPRG0-4]=KDP=%08x, [KDP-0x20]=IRP=%08x\n",
		        (uint32)kdp, (uint32)kdp, irp_base);
		/* NB: the page-descriptor build-loop cap (KDP+0x6b4) cannot be seeded here —
		 * the nanokernel cold-init zeroes the KDP region after init_emul_ppc, so any
		 * value written now is clobbered before the loop reads it. The cap is forced
		 * to the real page count via a ROM instruction patch instead; see the
		 * "page-descriptor loop cap" patch in rom_patches.cpp (patch_nanokernel). */

		/* P8: Dispatch fields for the 68k emulator handoff.
		 * The parcels nanokernel's dispatch routine (0x503126b4) does:
		 *   lwz r8, 0x5a0(r1)  → mtspr SPRG0  (context ptr)
		 *   lwz r8, 0x5a4(r1)  → addi +0x26e8 → mtspr SRR0  (68k code base → entry)
		 *   lwz r4, 0x648(r1)  (opcode table — set below)
		 *   lwz r9, -0x964(r1) → mtspr SRR1   (UserModeMSR — set below)
		 *   rfi  → enters SheepShaver's DR Emulator at code_base + 0x26e8
		 *
		 * The cold-init path (0x50310040, never taken) sets +0x5a0/+0x5a4 and posts
		 * the first work item to [KDP-0x900]. We seed them here instead.
		 * code_base: patch_68k_emul() writes emulator start at ROM+0x36f900.
		 * 0x5036f900 - 0x26e8 = 0x5036d218. */
		const uint32 emul_code_base = (uint32)ROMBase + 0x36d218;
		WriteMacInt32(kdp + 0x5a0, kdp);             // context ptr = KDP (same as SPRG0)
		WriteMacInt32(kdp + 0x5a4, emul_code_base);  // 68k code base (primary ROM)
		// [KDP-0x900] = SCC (serial controller) base address.
		// The nanokernel's check_work (0x50326880) reads SCC RR0 via this
		// pointer; if null, returns -1 immediately (no serial hardware).
		// Non-null: check_work does BAT3-setup, reads RR0 byte 2, checks
		// bit 0 ("Rx char available"). If set, reads data byte 6.
		//
		// The register access pattern (alternating reg#/data writes at
		// offsets 2 and 6) matches a Zilog SCC (8530), not a VIA 6522.
		//
		// M1: point check_work at the bus's SCC region (0xF3012000). The SCC 8530
		// model (machine/dev_scc8530.cpp) answers RR0 honestly — bit0 ("Rx char
		// available") is always 0 because no Rx source is connected in M1, so the
		// old M0 phantom-character hazards no longer apply: the nanokernel's idle/
		// yield primitive (0x503272e0) never falls through to the Thud debug console
		// (0x50327540), and check_work's timeout loop (0x50326548) sees a stable
		// "no character" state. The byte writes the nanokernel makes during SCC init
		// now Mach-fault into the model instead of corrupting fake memory
		// (SPIKE-S3 §2: lbz +2 = RR0, lbz +6 = data, full WR init at 0x50326980).
		// SS_NW_NO_SCC=1 restores the M0 behavior (base 0 → check_work returns -1).
		if (!MachineEnvFlag("SS_NW_NO_SCC")) {
			WriteMacInt32(kdp - 0x900, 0xF3012000);
			fprintf(stderr, "[NW-TRAMP] [KDP-0x900]=0xF3012000 (SCC via MMIO bus)\n");
		} else {
			WriteMacInt32(kdp - 0x900, 0);
			fprintf(stderr, "[NW-TRAMP] [KDP-0x900]=0 (no SCC — check_work returns -1)\n");
		}
		fprintf(stderr, "[NW-TRAMP] dispatch: +0x5a0(ctx)=%08x, +0x5a4(code_base)=%08x, "
		        "entry=%08x\n",
		        kdp, emul_code_base, emul_code_base + 0x26e8);

		// ECB pointer and DR Emulator KDP fields (read by DR Emulator at handoff).
		// These must be set for Path A (nanokernel trampoline) as well as Path B.
		const uint32 ecb = kdp + 0x1000;
		WriteMacInt32(kdp + 0x65c, ecb);          // ECB ptr → EmulatorData at KDP+0x1000
		WriteMacInt32(kdp + 0x660, 0);
		// M6a Wave 1 rev-2 finding 6 NOTE: [KDP+0x5a4] (=0x5036d218, 68k code
		// base, seeded above) is a DORMANT cross-world constant — PRIMARY-world
		// value in a mirror-world boot, on a route probe-verified never to
		// execute today (the jump68k dispatch tail / patched entry).
		// Reconcile when its route goes live (M6A-DR-HANDOFF-ANALYSIS.md §3).
		//
		// Rung-2 Task X RESOLUTION (verify-and-leave, plan rev 3.1 item 4) for
		// the [KDP+0x5f0/+0x5f4] pair below: these glue seeds are DEAD ON
		// ARRIVAL.  NK cold-init rebuilds both words to the staged NK pair
		// 0x50313bf8/0x503143a0 BEFORE the first table[0] dispatch — probe
		// evidence (/tmp/taskx_boot1.log, 2026-06-11): at table[0] COLD entry
		// visit=1 (the earliest post-NK observable) [KDP+0x5f0]=0x50313bf8
		// [KDP+0x5f4]=0x503143a0 already, and identically at the first WARM
		// (native-completion) entry.  The live warm switch-back path traverses
		// the NK-rebuilt values correctly (W2 round trip) — do NOT retarget to
		// the mirror (the once-planned 0x50466080 retarget is STRUCK: it would
		// destroy both switch directions).  CHOICE: comment-only, seeds kept —
		// removal can't be fully proven safe (a pre-NK-rebuild reader inside NK
		// cold-init can't be excluded without instrumenting NK init, and the
		// known consumers — the slot-stub exits — first run post-rebuild), and
		// the stale primary-world values are guaranteed overwritten pre-dispatch.
		WriteMacInt32(kdp + 0x5f0, (uint32)ROMBase + 0x366080);  // EMUL_RETURN handler (dead seed, see above)
		WriteMacInt32(kdp + 0x5f4, (uint32)ROMBase + 0x366080);  // (dead seed — NK rebuilds pre-dispatch)
		// M6a Wave 1 (memo §3 row "[KDP+0x648]"; plan rev 2 finding 7): this field
		// is architecturally the entry-VECTOR table, not the opcode dispatch table —
		// the skipped NK writer (0x503107fc) computes LA_EmulatorCode + [ConfigInfo
		// +0x84] = ROMBase+0x460000+0xe8c0.  The old seed ROMBase+0x380000 was wrong
		// on both identity (opcode table) and world (primary, in a mirror boot).
		// Mirror entry-vector table = ROMBase+0x46e8c0 (table[0] = the [NW-TRAMP]
		// redirect target, probe-verified entered).
		WriteMacInt32(kdp + 0x648, (uint32)ROMBase + 0x46e8c0);  // entry-vector table (mirror)
		WriteMacInt32(kdp - 0x964, 0x0000d032);                  // UserModeMSR

		// 68k exception vectors: SP=0 (diagnostic), reset PC, and rte stubs for 2..63.
		const uint32 reset_68k = (uint32)ROMBase + 0x2a;
		WriteMacInt32(0, 0);
		WriteMacInt32(4, reset_68k);
		const uint32 rte_addr = (uint32)ROMBase + 0x3196;
		for (int vec = 2; vec < 64; vec++)
			WriteMacInt32(vec * 4, rte_addr);
		/* M6a Wave 2 item #2 (M6A-WAVE2-SHIM-RECON.md §2 "exception-vector
		 * quick-win", §3 row 2): vectors 0x10 (illegal) / 0x28 (A-line) / 0x2C
		 * (F-line) get DEDICATED diagnosable-stop stubs — one `bra.s *` self-loop
		 * each, planted by PatchROM_NW_trampoline in the mirror zero run at
		 * 0x50429c00/10/20 — so a vectored exception parks at a unique probe-able
		 * PC instead of the shared rte stub (or, pre-fix, the PC=0 slide of the
		 * Wave-1 crash, memo §1.3).
		 * NOTE (re-verified in Wave 2): these glue-time low-memory writes are
		 * WIPED before the 68k world starts — the Wave-1 crash dump shows
		 * [0x2C]=0 despite the rte loop above. The writes that actually survive
		 * are the guest-side stores in the table[0] trampoline (rom_patches.cpp),
		 * which re-assert these three vectors after NK init; seeded here too for
		 * symmetry and for any pre-NK consumer. */
		WriteMacInt32(0x10, (uint32)ROMBase + 0x429c00);  // illegal-instruction stop
		WriteMacInt32(0x28, (uint32)ROMBase + 0x429c10);  // A-line stop
		WriteMacInt32(0x2C, (uint32)ROMBase + 0x429c20);  // F-line stop
		fprintf(stderr, "[NW-TRAMP] W2 68k vector stop stubs: [0x10]=%08x "
		        "[0x28]=%08x [0x2c]=%08x (bra.s * self-loops)\n",
		        (uint32)ROMBase + 0x429c00, (uint32)ROMBase + 0x429c10,
		        (uint32)ROMBase + 0x429c20);
		fprintf(stderr, "[NW-TRAMP] ECB=%08x +0x648(entry-vectors)=%08x +0x5f0(emul_ret)=%08x "
		        "guest[4](68k-reset)=%08x\n",
		        ecb, (uint32)ROMBase + 0x46e8c0,
		        (uint32)ROMBase + 0x366080, reset_68k);

		// ECB pre-population: the nanokernel scheduler context-switches to the
		// DR Emulator by restoring GPRs from ECB and bctr'ing to ECB+0xfc.
		// Without cold-start init, the ECB is garbage → crash. Pre-populate
		// the fields the context-switch reads:
		//   ECB+0xfc        = dispatch target (ongoing entry 0x5036f900)
		//   ECB+0x104+N*8   = saved GPR N (zero most, set r24/r29)
		//   ECB+0x7fc+i*4   = 68k opcode handler table (0x97 entries)
		// The nanokernel's context-switch restore uses r6 = KDP+0x1100
		// (not KDP+0x1000). ECB "base" for the restore is at +0x100 from ecb.
		const uint32 ctx = ecb + 0x100;
		memset(Mac2HostAddr(ecb), 0, 0x2000);
		// M6a Wave 1 (memo §3 row "ECB ctx+0xfc/+0x1ec", §4 mirror option; plan rev 2
		// finding 4): mirror-world values — the old ROMBase+0x36f900/+0x380000 were
		// PRIMARY-world constants in a MIRROR-world boot (region 0x50360000 never
		// compiles; the live kernel/emulator run from the 0x504xxxxx mirror).
		//   ctx+0xfc  := ROMBase+0x46f900  (mirror ongoing entry)
		//   ctx+0x1ec := ROMBase+0x480000  (saved r29 = mirror LA_DispatchTable)
		// Rev-2 NOTE: these are COLD-BOOT seeds only — the NK's interrupt save
		// dynamically overwrites them (stw r10,0xfc(r6) etc., the scheduler's
		// [r6+0xfc]/+0x13c..0x16c save/restore convention, memo §2.4) once the
		// first real round-trip happens; the rung-2 ongoing-entry contract owns
		// their steady-state values.
		WriteMacInt32(ctx + 0xfc, (uint32)ROMBase + 0x46f900);  // dispatch → ongoing entry (mirror)
		WriteMacInt32(ctx + 0x1c4, reset_68k);   // saved r24 = 68k reset PC
		WriteMacInt32(ctx + 0x1ec, (uint32)ROMBase + 0x480000);  // saved r29 = mirror dispatch table

		// Build 0x97-entry 68k opcode handler table at ECB+0x7fc.
		// Each entry = halfword from ROM+0x36dc42 OR'd with page base.
		const uint32 page_base = (uint32)ROMBase + 0x36e000;
		const uint8 *hw_src = ROMBaseHost + 0x36dc42;
		for (int i = 0; i < 0x97; i++) {
			uint16 hw = (hw_src[i*2] << 8) | hw_src[i*2 + 1];
			WriteMacInt32(ecb + 0x7fc + i * 4, hw | page_base);
		}

		WriteMacInt32(XLM_KERNEL_DATA, kdp);
		fprintf(stderr, "[NW-TRAMP] ECB pre-populated: dispatch=%08x r24=%08x r29=%08x "
		        "table[0x97] at ECB+0x7fc, XLM_KERNEL_DATA=%08x\n",
		        (uint32)ROMBase + 0x46f900, reset_68k,
		        (uint32)ROMBase + 0x480000, kdp);

		// PATH B DIAGNOSTIC (SS_NW_SYNTH_ENTRY) — may be removable.
		// Skips the nanokernel and enters DR Emulator directly. Dead end
		// at Mixed-Mode (0xFFC0 F-line needs nanokernel). Kept for
		// diagnostics. ROM patch at 0x310000 is Path-B-only.
		if (MachineEnvFlag("SS_NW_SYNTH_ENTRY")) {
			fprintf(stderr, "[NW-SYNTH] KDP=%08x ECB=%08x 68k-reset=%08x\n",
			        kdp, ecb, reset_68k);
			fprintf(stderr, "[NW-SYNTH] guest[0]=%08x guest[4]=%08x "
			        "(cold-start dispatch at ROM+0x36e964)\n",
			        ReadMacInt32(0), ReadMacInt32(4));
		}

		/* W2-4 DEC reload cadence (the storm root cause): [KDP+0xf2c] is the NK
		 * scheduler's timebase-frequency global (ticks/second). Evidence chain:
		 *   - timeslice re-arm 0x503249ac..c0: deadline = now + {0,[KDP+0xf2c]}
		 *   - duration->ticks helper 0x50323708: positive r8 -> ([0xf2c]/250)*r8/4
		 *     (ms->ticks), negative -> ([0xf2c]/0x3d090)*|r8|/4 (us->ticks; the
		 *     250000 literal is IN the ROM, pinning the units to ticks/sec)
		 *   - RDYQ init 0x503237c4 converts -0x412 (1042us timeslice) through it
		 *     into the run-queue quantum [KDP-0x9d4]
		 * NK cold-init ZEROES the word (0x50326fe8) and the config path that loads
		 * the real value on hardware never runs in the trampoline boot, so every
		 * timeslice deadline computed to now+0 -> mtdec 0 -> instant re-expiry ->
		 * the 250K/s DEC storm (EE-CHAIN-RECON.md D-6/D-7). Stage the frequency
		 * here, before the guest runs: NK init (0x50311368) copies it into the
		 * scheduler-mode table [KDP+0xf88] and RDYQ init derives the quantum.
		 * Verified live: seeding restores delivery->reprogram->quiet cadence and
		 * un-starves the 68k world. Same staging family as main.cpp's KDP+0xf6c
		 * timebase-frequency word. */
		WriteMacInt32(kdp + 0xf28, 0);
		WriteMacInt32(kdp + 0xf2c, (uint32)TimebaseSpeed);
		fprintf(stderr, "[NW-TRAMP] scheduler timebase-frequency staged: [KDP+0xf2c]=%u "
		        "(timeslice quantum source; DEC cadence)\n", (uint32)TimebaseSpeed);

		/* M7 item 1 — Execute68k newworld port (INTERRUPT-INJECTION-RECON.md Q1/Q5
		 * item 1, seed-proven boot 2): execute_68k (this file, gpr(29)/gpr(30) setup)
		 * reads the kernel-data emulator pair [KDP+0x1074] (pointer to 68k opcode
		 * dispatch table) / [KDP+0x1078] (emulator code base). On paravirtual the NK/
		 * emulator init populates them from the boot structure that rom_patches.cpp
		 * patch_nanokernel_boot stages (lp[0xa8>>2]=LA_DispatchTable=ROMBase+0x480000,
		 * lp[0xac>>2]=LA_EmulatorCode=ROMBase+0x460000, rom_patches.cpp:1514-1515); on
		 * the trampoline boot that init never runs and both words are NULL — the first
		 * synchronous Execute68k (boot sequencer's OP_NAME_REGISTRY -> FindLibSymbol ->
		 * Execute68k(proc1=0x50510000)) computed execute(0x558f*8) into zeroed low RAM
		 * and died at the 0x100000 fetch fault (P-M5). Stage the MIRROR-world values
		 * (the live kernel/emulator run from the 0x504xxxxx mirror, M6a Wave 1 — same
		 * reasoning as the ctx+0xfc/+0x1ec staging above):
		 *   [KDP+0x1074] := 0x50480000  (mirror LA_DispatchTable)
		 *   [KDP+0x1078] := 0x50460000  (mirror LA_EmulatorCode; recon residue R-II4:
		 *                   value by symmetry with rom_patches.cpp:1515, pair proven
		 *                   live by the SS_SEED_MEM discriminator boot)
		 * Same staging family as [KDP+0xf28]/[KDP+0xf2c] above. Structurally inert on
		 * paravirtual: inside the MachineProfileIsNewWorld() trampoline block. */
		WriteMacInt32(kdp + 0x1074, (uint32)ROMBase + 0x480000);
		WriteMacInt32(kdp + 0x1078, (uint32)ROMBase + 0x460000);
		fprintf(stderr, "[NW-TRAMP] Execute68k emulator pair staged: [KDP+0x1074]=%08x "
		        "(mirror dispatch table) [KDP+0x1078]=%08x (mirror emulator base)\n",
		        (uint32)ROMBase + 0x480000, (uint32)ROMBase + 0x460000);

		/* M7 Task B-2 (interrupt-injection plan, "Coordinator sign-off: the
		 * level-source staging", shape (i)) — the guest-memory half of the
		 * host-source-joins-the-PIC-rail staging.  Init-time PLATFORM
		 * CONSTANTS that Mac OS's native interrupt init (MPIC driver /
		 * Interrupt Manager) writes on a real boot, never per-interrupt event
		 * state (the fake-poke fence stays: every delivery still traverses
		 * PIC-IACK -> vector -> level test -> 68k chain).  Same sanctioned
		 * class as [KDP+0x1074/0x1078] and [KDP+0xf2c] above.
		 *
		 *   [[KDP-0x20]+0xf18] := 0xF3040000 — the NK-held PIC base the EXT
		 *     fallback reads at 0x50325f48 (lwz r22,0xf18(r20), [STATIC]
		 *     rom901.bin md5 d1a267a9); its IACK lwbrx is r22+0x200a0 and EOI
		 *     stwx r22+0x200b0 = the model's CPU0 IACK/EOI registers
		 *     (dev_openpic.h: CPU bank +0x20000, regs 0xA0/0xB0).  Value =
		 *     OPENPIC_CORE99_BASE (MacIO BAR + 0x40000, donor study Q6): the
		 *     guest mapping the real init would create for the MPIC — guest
		 *     addressing is physical here.  Survives NK cold-init (the IRP
		 *     page is the durable side of the Hnfo precedent; [KDP-0x20] is
		 *     live-proven re-read by the fallback).  Recorded in the sub-KDP
		 *     occupancy map (M6A-ONGOING-ENTRY-DESIGN.md).
		 *   [0x3f00+0x3f] := 1 — the lowmem vector->level table byte the
		 *     fallback's lbz r28,0x3f00(r26) reads (0x503260a4) for vector
		 *     0x3F (= OPENPIC_IRQ_HOST, main_unix.cpp/dev_openpic.h — the
		 *     reserved host input, vector=input identity).  Level 1: the host
		 *     tick source stands in for the platform's 60 Hz/VIA-class
		 *     interrupt, which is the 68k LEVEL-1 autovector chain (Q-I3:
		 *     CHRP level-1 @0xec50 -> via_int -> OP_IRQ -> Ticks); must be
		 *     >0 (post skips on 0 — the Task-B break link) and !=7 (the
		 *     deferred-slot leg 0x503260a8).  Lowmem may be wiped before the
		 *     68k world starts (the W2 68k-vector evidence) — SetInterruptFlag
		 *     re-asserts this byte once at the first edge and logs which copy
		 *     survived.
		 * Gating (binding constraint 3, stated): structurally tied to the PIC
		 * being registered — same env pair as the main_unix staging
		 * (SS_NW_HOST_IRQ + SS_NW_PIC, both re-parsed here with identical
		 * semantics), inside the MachineProfileIsNewWorld() trampoline block:
		 * gated-off and paravirtual boots are byte-identical. */
		{
			/* M7 Task C flip: SS_NW_HOST_IRQ is newworld-default-ON
			 * (explicit-"0" opt-out) — polarity must match main_unix's
			 * bring-up parse exactly. SS_NW_PIC stays default OFF (its
			 * flip is HELD per W2-3 stop-rule 3), so this staging remains
			 * test-cluster-only until the PIC flip lands. */
			const char *hirq_env = getenv("SS_NW_HOST_IRQ");
			const char *pic_env  = getenv("SS_NW_PIC");
			const bool hirq_on = !(hirq_env && strcmp(hirq_env, "0") == 0);
			const bool pic_on  = pic_env  && pic_env[0]  && pic_env[0]  != '0';
			if (hirq_on && pic_on) {
				WriteMacInt32(irp_base + 0xf18, 0xF3040000);  // OPENPIC_CORE99_BASE
				WriteMacInt8(0x3f00 + 0x3f, 1);
				fprintf(stderr, "[NW-TRAMP] PIC-rail level source staged: "
				        "[[KDP-0x20]+0xf18]=%08x=0xF3040000 (NK-held PIC base) "
				        "[0x3f3f]=1 (vector 0x3f -> 68k level 1)\n",
				        irp_base + 0xf18);
			}
		}

		/* SS_SEED_MEM (immediate form): apply the no-PC seeds now — the natural
		 * "post-init" point, after the nanokernel trampoline has populated the KDP /
		 * ECB. The PC-triggered form fires later at its target block entry. */
		ss_seed_mem_apply_immediate();

		/* M3a Task 3 + NK-syscall-surface Task A: finalize the exception entry
		 * table for newworld.
		 * Defaults: interrupt_entry = 0x50412b1c (probe-verified, M3A-ENTRY-TABLE.md);
		 *           syscall_entry   = 0x50314ac0 — newworld DEFAULT since Task C
		 *           (acceptance battery green pre/post-flip); opt-out with
		 *           SS_NW_SC_SURFACE=0 (polarity mirrors SS_NW_MM_SWITCH) restores
		 *           the abort-with-capture baseline (syscall_entry = 0).
		 * Precedence (plan rev 2 P-M1): SS_EXC_ENTRY > SS_NW_SC_SURFACE default > 0.
		 *
		 * The override x gate 2x2 (P-M1, pinned; gate now default-ON, =0 opts out):
		 *   gate=0,  no override       -> {0x50412b1c, 0}            (FATAL sc baseline)
		 *   default, no override       -> {0x50412b1c, 0x50314ac0}   (surface armed)
		 *   gate=0,  SS_EXC_ENTRY=I,S  -> {I, S}   override active REGARDLESS of gate —
		 *                                 the designed PROBE-S3 no-rebuild channel
		 *   default, SS_EXC_ENTRY=I    -> {I, 0x50314ac0}  no-comma form PRESERVES the
		 *                                 default syscall entry (see trap fix below) */
		{
			/* Task C flip: default ON; explicit "0" opts out (SS_NW_MM_SWITCH
			 * polarity — NOT MachineEnvFlag, which would read unset as off). */
			const char *sc_env = getenv("SS_NW_SC_SURFACE");
			const bool sc_surface = !(sc_env && strcmp(sc_env, "0") == 0);
			if (sc_surface) {
				g_exc_entry_table.syscall_entry = NW_SYSCALL_ENTRY_DEFAULT;
				fprintf(stderr, "[NW-SC] syscall surface armed (newworld default; "
				        "opt-out SS_NW_SC_SURFACE=0): "
				        "entry=0x%08x (primary copy, NK-published [KDP+0x390]); "
				        "shim=SPRG1:=caller r1, SPRG2:=caller LR\n",
				        g_exc_entry_table.syscall_entry);
			} else {
				fprintf(stderr, "[NW-SC] syscall surface OFF (SS_NW_SC_SURFACE=0 "
				        "opt-out): syscall_entry=0 — abort-with-capture baseline\n");
			}
			/* W2-4 step 0: SS_NW_DEC_PUBLISHED (newworld DEFAULT since the M7
			 * Task C cluster flip — the flip W2-4 reserved as its final
			 * acceptance; opt-out =0) re-points the DEC delivery target from
			 * the save-and-switch body 0x50412b1c (KDP shim) to the
			 * NK-published handler 0x50313200, [KDP+0x384] [PROBE✓] — primary
			 * copy, the publication precedent. Applied BEFORE the SS_EXC_ENTRY
			 * parse so the override precedence is unchanged (override > gate
			 * default > legacy default). The matching shim-shape switch lives
			 * in the delivery hook (the gate selects shim shape; the override
			 * selects entry value only — see exc_dec_published_enabled()).
			 * Live-inert at today's frontier: delivered_dec=0 on the boot path
			 * (no EE riser yet — W2L-3); the harness lane (run-exc.sh H8) is
			 * the end-to-end observable until W2-4 step 1 arms the riser. */
			if (exc_dec_published_enabled()) {
				g_exc_entry_table.interrupt_entry = NW_INTERRUPT_PUBLISHED_DEFAULT;
				fprintf(stderr, "[NW-DEC] published DEC route armed "
				        "(SS_NW_DEC_PUBLISHED=1): interrupt_entry=0x%08x "
				        "(primary copy, NK-published [KDP+0x384]); "
				        "shim=SPRG1:=caller r1, SPRG2:=caller LR (KDP shim "
				        "retired on this route)\n",
				        g_exc_entry_table.interrupt_entry);
			}
			/* TRAP FIX (plan rev 2 P-M1): the no-comma SS_EXC_ENTRY=0xINT form
			 * previously ZEROED syscall_entry — a post-flip trap (overriding the
			 * interrupt entry would have silently re-broken the resolved syscall
			 * surface). It now PRESERVES the syscall entry (gate default or 0);
			 * only an explicit ",0xSC" field overrides it.
			 * W2-1 (rev 2 F1): the parse itself moved to the shared helper
			 * exc_entry_table_apply_env_override() so the SS_TEST harness path
			 * (which never reaches this block) applies the same override. */
			if (!exc_entry_table_apply_env_override()) {
				fprintf(stderr, "[EXC] entry table: interrupt=0x%08x syscall=0x%08x\n",
				        g_exc_entry_table.interrupt_entry,
				        g_exc_entry_table.syscall_entry);
			}
			/* P-M1: SS_EXC_SC=legacy only acts on the UNRESOLVED-entry path
			 * (execute_syscall). With the entry resolved it is inert — say so
			 * loudly once instead of silently ignoring the knob. */
			const char *sc_legacy = getenv("SS_EXC_SC");
			if (sc_legacy && sc_legacy[0] == 'l' && g_exc_entry_table.syscall_entry != 0)
				fprintf(stderr, "[EXC] WARNING: SS_EXC_SC=legacy is INERT — syscall entry "
				        "resolved (0x%08x); legacy no-op applies only to the unresolved path "
				        "(reproduce the legacy datum with SS_NW_SC_SURFACE=0 SS_EXC_SC=legacy)\n",
				        g_exc_entry_table.syscall_entry);

			/* FE1F-service-surface Task A (plan rev 3): the program-interrupt
			 * (0x700) delivery surface — NEWWORLD DEFAULT since Task C
			 * (acceptance battery green pre/post-flip); explicit-"0"-only
			 * opt-out SS_NW_FE1F_SURFACE=0 (the SS_NW_SC_SURFACE polarity
			 * precedent — NOT MachineEnvFlag, which would read unset as off).
			 * Opted out, the Task-T/U parked stops stay and the 0x5000f248
			 * park baseline is byte-identical (Task C gate (e)).
			 *
			 * Env-flag matrix (rev 2 P-m5, behavior pinned): this surface is
			 * MEANINGLESS with SS_NW_MM_SWITCH=0 or SS_NW_SC_SURFACE=0 — the
			 * boot never reaches the FE1F callout without the MixedMode switch
			 * + the sc surface (the e3e0 routine sits 5 sc deliveries past the
			 * MixedMode round trip). Combined-opt-out decision: the surface
			 * still ARMS (harmless — the twi sites are unreachable on such a
			 * boot) but logs the misconfig loudly; a pre-FE1F A/B wants the
			 * upstream knob, not this one. The same gate also controls the
			 * PatchROM-time placeholder restore (rom_patches.cpp). */
			{
				const char *fe1f_env = getenv("SS_NW_FE1F_SURFACE");
				const bool fe1f_surface = !(fe1f_env && strcmp(fe1f_env, "0") == 0);
				if (fe1f_surface) {
					g_exc_entry_table.program_entry = NW_PROGRAM_ENTRY_DEFAULT;
					fprintf(stderr, "[NW-FE1F] FE1F surface armed (newworld default; "
					        "opt-out SS_NW_FE1F_SURFACE=0): program_entry=0x%08x "
					        "(primary copy, NK-published [KDP+0x37c]); twi/tw "
					        "trap-taken -> 0x700; "
					        "shim=SPRG1:=caller r1, SPRG2:=caller LR\n",
					        g_exc_entry_table.program_entry);
					const char *mm_env = getenv("SS_NW_MM_SWITCH");
					const bool mm_on = !(mm_env && strcmp(mm_env, "0") == 0);
					const bool sc_on = (g_exc_entry_table.syscall_entry != 0);
					if (!mm_on || !sc_on)
						fprintf(stderr, "[NW-FE1F] MISCONFIG: FE1F surface armed with "
						        "%s%s%s OFF — the boot cannot reach the FE1F callout; "
						        "surface stays armed but inert (P-m5)\n",
						        !mm_on ? "SS_NW_MM_SWITCH" : "",
						        (!mm_on && !sc_on) ? " and " : "",
						        !sc_on ? "the sc surface" : "");
				} else {
					fprintf(stderr, "[NW-FE1F] FE1F surface OFF (SS_NW_FE1F_SURFACE=0 "
					        "opt-out): program_entry=0 — twi unresolved (FATAL on "
					        "trap-taken), parked-stop slot baseline\n");
				}
			}
		}
	}
	WriteMacInt32(XLM_RUN_MODE, MODE_68K);

#if ENABLE_MON
	// Install "regs" command in cxmon
	mon_add_command("regs", dump_registers, "regs                     Dump PowerPC registers\n");
	mon_add_command("log", dump_log, "log                      Dump PowerPC emulation log\n");
#endif

#if EMUL_TIME_STATS
	emul_start_time = clock();
#endif
}

/*
 *  Deinitialize emulation
 */

void exit_emul_ppc(void)
{
#if EMUL_TIME_STATS
	clock_t emul_end_time = clock();

	printf("### Statistics for SheepShaver emulation parts\n");
	const clock_t emul_time = emul_end_time - emul_start_time;
	printf("Total emulation time : %.1f sec\n", double(emul_time) / double(CLOCKS_PER_SEC));
	printf("Total interrupt count: %d (%2.1f Hz)\n", interrupt_count,
		   (double(interrupt_count) * CLOCKS_PER_SEC) / double(emul_time));
	printf("Total ppc interrupt count: %d (%2.1f %%)\n", ppc_interrupt_count,
		   (double(ppc_interrupt_count) * 100.0) / double(interrupt_count));

#define PRINT_STATS(LABEL, VAR_PREFIX) do {								\
		printf("Total " LABEL " count : %d\n", VAR_PREFIX##_count);		\
		printf("Total " LABEL " time  : %.1f sec (%.1f%%)\n",			\
			   double(VAR_PREFIX##_time) / double(CLOCKS_PER_SEC),		\
			   100.0 * double(VAR_PREFIX##_time) / double(emul_time));	\
	} while (0)

	PRINT_STATS("Execute68k[Trap] execution", exec68k);
	PRINT_STATS("NativeOp execution", native_exec);
	PRINT_STATS("MacOS routine execution", macos_exec);

#undef PRINT_STATS
	printf("\n");
#endif

	delete ppc_cpu;
	ppc_cpu = NULL;
}

#if PPC_ENABLE_JIT && PPC_REENTRANT_JIT
// Initialize EmulOp trampolines
void init_emul_op_trampolines(basic_dyngen & dg)
{
	typedef void (*func_t)(dyngen_cpu_base, uint32);
	func_t func;

	// EmulOp
	emul_op_trampoline = dg.gen_start();
	func = &sheepshaver_cpu::call_execute_emul_op;
	dg.gen_invoke_CPU_T0(func);
	dg.gen_exec_return();
	dg.gen_end();

	// NativeOp
	native_op_trampoline = dg.gen_start();
	func = &sheepshaver_cpu::call_execute_native_op;
	dg.gen_invoke_CPU_T0(func);	
	dg.gen_exec_return();
	dg.gen_end();

	D(bug("EmulOp trampoline:   %p\n", emul_op_trampoline));
	D(bug("NativeOp trampoline: %p\n", native_op_trampoline));
}
#endif

/*
 *  Emulation loop
 */

void emul_ppc(uint32 entry)
{
#if 0
	ppc_cpu->start_log();
#endif
	// start emulation loop and enable code translation or caching
	ppc_cpu->execute(entry);
}

/*
 *  Handle PowerPC interrupt
 */

void TriggerInterrupt(void)
{
	idle_resume();
#if 0
  WriteMacInt32(0x16a, ReadMacInt32(0x16a) + 1);
#else
  // Trigger interrupt to main cpu only
  if (ppc_cpu)
	  ppc_cpu->trigger_interrupt();
#endif
}

void HandleInterrupt(powerpc_registers *r)
{
#ifdef USE_SDL_VIDEO
	// We must fill in the events queue in the same thread that did call SDL_SetVideoMode()
	SDL_PumpEvents();
#endif

	// Do nothing if interrupts are disabled
	if (int32(ReadMacInt32(XLM_IRQ_NEST)) > 0)
		return;

	// Update interrupt count
#if EMUL_TIME_STATS
	interrupt_count++;
#endif

	// Interrupt action depends on current run mode
	switch (ReadMacInt32(XLM_RUN_MODE)) {
	case MODE_68K:
		// 68k emulator active, trigger 68k interrupt level 1.
		// M3a rev-2 F3/M2: on NewWorld, WriteMacInt16(KDP+0x67c, 1) and the CR-mask
		// injection are paravirtual fake-delivery machinery; they corrupt live guest
		// CR / KDP interrupt level mid-NK-execution. Fenced on newworld
		// (plan 2026-06-10-machine-layer-m3a.md rev 2).
		if (!MachineProfileIsNewWorld()) {
			WriteMacInt16(ReadMacInt32(KERNEL_DATA_BASE + 0x67c), 1);
			uint32 cr_mask = ReadMacInt32(KERNEL_DATA_BASE + 0x674);
			if (cr_mask != 0)
				r->cr.set(r->cr.get() | cr_mask);
		}
		// Always tick Ticks on every VBL — PPC nanokernel spin-waits
		// (0x5031040c, 0x50313d34, etc.) poll Ticks directly; CR injection
		// only helps the 68k emulator dispatch path. Real hardware increments
		// Ticks on every VBL regardless. Safe: the 68k interrupt handler also
		// increments Ticks, but only after the nanokernel hands off — these
		// early-boot spin-waits never reach that handoff, so no double-count.
		WriteMacInt32(0x16a, ReadMacInt32(0x16a) + 1);
		// M8 Task B (Ticks rider, rev-2 A5): census the host keep-set writer so
		// guest-claimed Ticks = total 0x16c increments minus this delta. Counter
		// only (data-only, unconditional); printed in the consume-gated
		// [IRQ-CONSUME] atexit dump.
		g_exc_consume_stats.ticks_keepset++;
		// [NW-INT] tick-50 injection deleted (M3a): real DEC delivery via the exception core replaces it (plan 2026-06-10-machine-layer-m3a.md Task 4).
		break;
    
#if INTERRUPTS_IN_NATIVE_MODE
	case MODE_NATIVE:
		// M3a: on newworld, this arm does nothing — stale static entry (0x312b1c),
		// nested-execute path; M3a delivers via the exception core instead
		// (plan 2026-06-10-machine-layer-m3a.md Task 4). XLM_RUN_MODE stays MODE_68K
		// on newworld anyway (M3A-ENTRY-TABLE.md finding 3) — this fence is insurance.
		if (!MachineProfileIsNewWorld()) {
			// 68k emulator inactive, in nanokernel?
			if (r->gpr[1] != KernelDataAddr) {

				// Prepare for 68k interrupt level 1
				WriteMacInt16(ReadMacInt32(KERNEL_DATA_BASE + 0x67c), 1);
				WriteMacInt32(ReadMacInt32(KERNEL_DATA_BASE + 0x658) + 0xdc,
							  ReadMacInt32(ReadMacInt32(KERNEL_DATA_BASE + 0x658) + 0xdc)
							  | ReadMacInt32(KERNEL_DATA_BASE + 0x674));

				// Execute nanokernel interrupt routine (this will activate the 68k emulator)
				DisableInterrupt();
				if (ROMType == ROMTYPE_NEWWORLD)
					ppc_cpu->interrupt(ROMBase + 0x312b1c);
				else
					ppc_cpu->interrupt(ROMBase + 0x312a3c);
			}
		}
		break;
#endif
    
#if INTERRUPTS_IN_EMUL_OP_MODE
	case MODE_EMUL_OP:
		// 68k emulator active, within EMUL_OP routine, execute 68k interrupt routine directly when interrupt level is 0
		if ((ReadMacInt32(XLM_68K_R25) & 7) == 0) {
#if EMUL_TIME_STATS
			const clock_t interrupt_start = clock();
#endif
#if 1
			// Execute full 68k interrupt routine
			M68kRegisters r;
			uint32 old_r25 = ReadMacInt32(XLM_68K_R25);	// Save interrupt level
			WriteMacInt32(XLM_68K_R25, 0x21);			// Execute with interrupt level 1
			static const uint8 proc_template[] = {
				0x3f, 0x3c, 0x00, 0x00,			// move.w	#$0000,-(sp)	(fake format word)
				0x48, 0x7a, 0x00, 0x0a,			// pea		@1(pc)			(return address)
				0x40, 0xe7,						// move		sr,-(sp)		(saved SR)
				0x20, 0x78, 0x00, 0x064,		// move.l	$64,a0
				0x4e, 0xd0,						// jmp		(a0)
				M68K_RTS >> 8, M68K_RTS & 0xff	// @1
			};
			BUILD_SHEEPSHAVER_PROCEDURE(proc);
			Execute68k(proc, &r);
			WriteMacInt32(XLM_68K_R25, old_r25);		// Restore interrupt level
#else
			// Only update cursor
			if (HasMacStarted()) {
				if (InterruptFlags & INTFLAG_VIA) {
					ClearInterruptFlag(INTFLAG_VIA);
					ADBInterrupt();
					ExecuteNative(NATIVE_VIDEO_VBL);
				}
			}
#endif
#if EMUL_TIME_STATS
			interrupt_time += (clock() - interrupt_start);
#endif
		}
		break;
#endif
	}
}

void sheepshaver_cpu::call_execute_native_op(powerpc_cpu * cpu, uint32 selector) {
	static_cast<sheepshaver_cpu *>(cpu)->execute_native_op(selector);
}

// Execute NATIVE_OP routine
void sheepshaver_cpu::execute_native_op(uint32 selector)
{
#if EMUL_TIME_STATS
	native_exec_count++;
	const clock_t native_exec_start = clock();
#endif

	switch (selector) {
	case NATIVE_PATCH_NAME_REGISTRY:
		DoPatchNameRegistry();
		break;
	case NATIVE_VIDEO_INSTALL_ACCEL:
		VideoInstallAccel();
		break;
	case NATIVE_VIDEO_VBL:
		VideoVBL();
		break;
	case NATIVE_VIDEO_DO_DRIVER_IO:
		gpr(3) = (int32)(int16)VideoDoDriverIO(gpr(3), gpr(4), gpr(5), gpr(6), gpr(7));
		break;
	case NATIVE_ETHER_AO_GET_HWADDR:
		AO_get_ethernet_address(gpr(3));
		break;
	case NATIVE_ETHER_AO_ADD_MULTI:
		AO_enable_multicast(gpr(3));
		break;
	case NATIVE_ETHER_AO_DEL_MULTI:
		AO_disable_multicast(gpr(3));
		break;
	case NATIVE_ETHER_AO_SEND_PACKET:
		AO_transmit_packet(gpr(3));
		break;
	case NATIVE_ETHER_IRQ:
		EtherIRQ();
		break;
	case NATIVE_ETHER_INIT:
		gpr(3) = InitStreamModule((void *)gpr(3));
		break;
	case NATIVE_ETHER_TERM:
		TerminateStreamModule();
		break;
	case NATIVE_ETHER_OPEN:
		gpr(3) = ether_open((queue_t *)gpr(3), (void *)gpr(4), gpr(5), gpr(6), (void*)gpr(7));
		break;
	case NATIVE_ETHER_CLOSE:
		gpr(3) = ether_close((queue_t *)gpr(3), gpr(4), (void *)gpr(5));
		break;
	case NATIVE_ETHER_WPUT:
		gpr(3) = ether_wput((queue_t *)gpr(3), (mblk_t *)gpr(4));
		break;
	case NATIVE_ETHER_RSRV:
		gpr(3) = ether_rsrv((queue_t *)gpr(3));
		break;
	case NATIVE_NQD_SYNC_HOOK:
		gpr(3) = NQD_sync_hook(gpr(3));
		break;
	case NATIVE_NQD_UNKNOWN_HOOK:
		gpr(3) = NQD_unknown_hook(gpr(3));
		break;
	case NATIVE_NQD_BITBLT_HOOK:
		gpr(3) = NQD_bitblt_hook(gpr(3));
		break;
	case NATIVE_NQD_BITBLT:
		NQD_bitblt(gpr(3));
		break;
	case NATIVE_NQD_FILLRECT_HOOK:
		gpr(3) = NQD_fillrect_hook(gpr(3));
		break;
	case NATIVE_NQD_INVRECT:
		NQD_invrect(gpr(3));
		break;
	case NATIVE_NQD_FILLRECT:
		NQD_fillrect(gpr(3));
		break;
	case NATIVE_SERIAL_NOTHING:
	case NATIVE_SERIAL_OPEN:
	case NATIVE_SERIAL_PRIME_IN:
	case NATIVE_SERIAL_PRIME_OUT:
	case NATIVE_SERIAL_CONTROL:
	case NATIVE_SERIAL_STATUS:
	case NATIVE_SERIAL_CLOSE: {
		typedef int16 (*SerialCallback)(uint32, uint32);
		static const SerialCallback serial_callbacks[] = {
			SerialNothing,
			SerialOpen,
			SerialPrimeIn,
			SerialPrimeOut,
			SerialControl,
			SerialStatus,
			SerialClose
		};
		gpr(3) = serial_callbacks[selector - NATIVE_SERIAL_NOTHING](gpr(3), gpr(4));
		break;
	}
	case NATIVE_GET_RESOURCE:
		get_resource(ReadMacInt32(XLM_GET_RESOURCE));
		break;
	case NATIVE_GET_1_RESOURCE:
		get_resource(ReadMacInt32(XLM_GET_1_RESOURCE));
		break;
	case NATIVE_GET_IND_RESOURCE:
		get_resource(ReadMacInt32(XLM_GET_IND_RESOURCE));
		break;
	case NATIVE_GET_1_IND_RESOURCE:
		get_resource(ReadMacInt32(XLM_GET_1_IND_RESOURCE));
		break;
	case NATIVE_R_GET_RESOURCE:
		get_resource(ReadMacInt32(XLM_R_GET_RESOURCE));
		break;
	case NATIVE_MAKE_EXECUTABLE:
		MakeExecutable(0, gpr(4), gpr(5));
		break;
	case NATIVE_CHECK_LOAD_INVOC:
		check_load_invoc(gpr(3), gpr(4), gpr(5));
		break;
	case NATIVE_NAMED_CHECK_LOAD_INVOC:
		named_check_load_invoc(gpr(3), gpr(4), gpr(5));
		break;
	default:
		printf("FATAL: NATIVE_OP called with bogus selector %d\n", selector);
		QuitEmulator();
		break;
	}

#if EMUL_TIME_STATS
	native_exec_time += (clock() - native_exec_start);
#endif
}

/*
 *  Execute 68k subroutine (must be ended with EXEC_RETURN)
 *  This must only be called by the emul_thread when in EMUL_OP mode
 *  r->a[7] is unused, the routine runs on the caller's stack
 */

void Execute68k(uint32 pc, M68kRegisters *r)
{
	ppc_cpu->execute_68k(pc, r);
}

/*
 *  Execute 68k A-Trap from EMUL_OP routine
 *  r->a[7] is unused, the routine runs on the caller's stack
 */

void Execute68kTrap(uint16 trap, M68kRegisters *r)
{
	SheepVar proc_var(4);
	uint32 proc = proc_var.addr();
	WriteMacInt16(proc, trap);
	WriteMacInt16(proc + 2, M68K_RTS);
	Execute68k(proc, r);
}

/*
 *  Call MacOS PPC code
 */

uint32 call_macos(uint32 tvect)
{
	return ppc_cpu->execute_macos_code(tvect, 0, NULL);
}

uint32 call_macos1(uint32 tvect, uint32 arg1)
{
	const uint32 args[] = { arg1 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos2(uint32 tvect, uint32 arg1, uint32 arg2)
{
	const uint32 args[] = { arg1, arg2 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos3(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3)
{
	const uint32 args[] = { arg1, arg2, arg3 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos4(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos5(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4, arg5 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos6(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4, arg5, arg6 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}

uint32 call_macos7(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6, uint32 arg7)
{
	const uint32 args[] = { arg1, arg2, arg3, arg4, arg5, arg6, arg7 };
	return ppc_cpu->execute_macos_code(tvect, sizeof(args)/sizeof(args[0]), args);
}
