/*
 *  ppc-cpu.cpp - PowerPC CPU definition
 *
 *  Kheperix (C) 2003-2005 Gwenole Beauchesne
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
#include <stdlib.h>
#include <assert.h>
#include "vm_alloc.h"
#include "cpu/vm.hpp"
#include "cpu/ppc/ppc-cpu.hpp"
#ifndef SHEEPSHAVER
#include "basic-kernel.hpp"
#endif

#if PPC_ENABLE_JIT
#include "cpu/jit/dyngen-exec.h"
#endif

#if ENABLE_MON
#include "mon.h"
#include "mon_disass.h"
#endif

#define DEBUG 0
#include "debug.h"

#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
extern uint8 *RAMBaseHost;
extern uint32 RAMSize;
extern uint32 ROMBase;
extern uint8 *ROMBaseHost;
#include "cpu/jit/aarch64/ppc-jit.h"
#include <cstddef>
/* Compile-time verification of the spcflags mask offset used by the JIT's
 * block-entry interrupt poll.  PPCR_SPCFLAGS in ppc-jit.cpp must match.
 * basic_spcflags has `uint32 mask` as its FIRST member, so
 * &spcflags == &spcflags.mask; the poll reads a W-word at this offset. */
static_assert(offsetof(powerpc_registers, spcflags) == 1056,
              "spcflags offset changed — update PPCR_SPCFLAGS in ppc-jit.cpp");

#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
static double jit_elapsed_s() {
	static struct timespec t0 = {0,0};
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	if (t0.tv_sec == 0) t0 = t;
	return (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9;
}
static FILE *jit_log_file = nullptr;
/* Open the diagnostic log.
 *
 * Path resolution:
 *   - SS_JIT_DIAG_LOG, if set and non-empty, is used verbatim (no symlink touched).
 *   - Otherwise a per-instance file /tmp/jit_diag.<YYYYMMDD-HHMMSS>.<pid>.log is
 *     created and the stable alias /tmp/jit_diag.log is re-pointed (symlink) at it.
 *
 * WHY per-instance files: the default used to be a single shared /tmp/jit_diag.log
 * opened with "w", so ANY concurrent SheepShaver process reaching a heartbeat would
 * truncate another instance's log mid-run — silently corrupting a concurrent
 * boot-time measurement.  Timestamp+pid naming makes each run's log unique; the
 * symlink keeps `tail -f /tmp/jit_diag.log` and jit-analyze.py defaults working.
 * Note: harness test vectors (SS_TEST_HEX) exit in milliseconds and never reach
 * the first 5s heartbeat, so they never create log files. */
static void jit_diag_log_open(void) {
	if (jit_log_file) return;
	const char *path = getenv("SS_JIT_DIAG_LOG");
	char ts_path[128];
	bool make_link = false;
	if (!path || !*path) {
		time_t now = time(NULL);
		struct tm tmv;
		localtime_r(&now, &tmv);
		snprintf(ts_path, sizeof(ts_path), "/tmp/jit_diag.%04d%02d%02d-%02d%02d%02d.%d.log",
		         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
		         tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)getpid());
		path = ts_path;
		make_link = true;
	}
	jit_log_file = fopen(path, "w");
	fprintf(stderr, "[JIT] diagnostic log: %s\n", path);
	if (jit_log_file && make_link) {
		/* Refresh the latest-run alias.  unlink() also replaces a stale regular
		 * file left behind by a pre-symlink build.  Failure is non-fatal: the
		 * real log still works, only the convenience alias is lost. */
		unlink("/tmp/jit_diag.log");
		if (symlink(path, "/tmp/jit_diag.log") != 0)
			fprintf(stderr, "[JIT] warning: could not update /tmp/jit_diag.log symlink: %s\n",
			        strerror(errno));
	}
}
/* Notable events (init, interrupts, stuck, flush) → stderr */
#define JIT_LOG(fmt, ...) fprintf(stderr, "[JIT %.2fs] " fmt "\n", jit_elapsed_s(), ##__VA_ARGS__)
/* Verbose events (heartbeat, per-interrupt detail) → file only */
#define JIT_FLOG(fmt, ...) do { if (jit_log_file) { fprintf(jit_log_file, "[JIT %.2fs] " fmt "\n", jit_elapsed_s(), ##__VA_ARGS__); } } while(0)
#endif

#if PPC_PROFILE_GENERIC_CALLS
uint32 powerpc_cpu::generic_calls_count[PPC_I(MAX)];
static int generic_calls_ids[PPC_I(MAX)];
const int generic_calls_top_ten = 20;

int generic_calls_compare(const void *e1, const void *e2)
{
	const int id1 = *(const int *)e1;
	const int id2 = *(const int *)e2;
	return powerpc_cpu::generic_calls_count[id2] - powerpc_cpu::generic_calls_count[id1];
}
#endif

#if PPC_PROFILE_REGS_USE
int register_info_compare(const void *e1, const void *e2)
{
	const powerpc_cpu::register_info *ri1 = (powerpc_cpu::register_info *)e1;
	const powerpc_cpu::register_info *ri2 = (powerpc_cpu::register_info *)e2;
	return ri2->count - ri1->count;
}
#endif

#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
/* Inline interpreter-call bridge for the AArch64 JIT (dyngen do_generic
 * equivalent).
 *
 * When the JIT hits an opcode it cannot compile natively (compile_one returns
 * false), instead of marking the block incomplete it emits an inline call to
 * this function for that one instruction.  The bridge decodes and executes the
 * single PPC instruction through the *same* interpreter handler the interpreter
 * loop uses (decode()->execute()), keeping the block "complete" and avoiding the
 * mixed JIT/interpreter execution of the same PC that corrupts the ROM's 68k
 * emulator.  See docs/superpowers/research/2026-06-02-dyngen-mechanisms.md GAP 3.
 *
 * s_active_cpu is set at the top of powerpc_cpu::execute(): the JIT only ever
 * runs from inside execute(), single-threaded, on one cpu object across nested
 * execute_depth calls, so it is always valid when the bridge fires.  The JIT
 * passes the regs pointer (RSTATE/x20), not the cpu object, and regs_ptr() has
 * no back-pointer to the cpu, so we recover the cpu this way rather than from
 * the regs pointer. */
static powerpc_cpu *s_active_cpu = NULL;

/* ---- In-memory execution trace ring (SS_JIT_TRACE_RING=1) -----------------
 *
 * Zero-I/O execution history for crash diagnosis.  Every JIT block execution,
 * interpreter block entry, and inline interpreter call is recorded into a
 * fixed-size ring (~20 stores per record — boot speed is essentially
 * unaffected, unlike SS_JIT_TRACE's fprintf+fflush per block).  The SIGSEGV
 * crash handler (sheepshaver_glue.cpp) calls ppc_jit_dump_trace_ring() to
 * write the ring to /tmp/ss_jit_ring.txt, giving the exact block-execution
 * history leading up to a deterministic crash.
 *
 * Record types:
 *   'J' — JIT block executed; from_pc/to_pc = block entry/exit, registers AFTER
 *   'I' — interpreter block entered; from_pc = entry PC, registers BEFORE
 *   'C' — inline interpreter call; from_pc = instruction PC, registers BEFORE
 *
 * Register fields use the ROM 68k (DR) emulator's conventions:
 *   r24 = 68k PC, r27 = opcode, r29 = handler address,
 *   a[0..7] = PPC r16..r23 = 68k A0..A7 (a7 = 68k stack pointer). */
struct jit_ring_rec {
	uint32 from_pc, to_pc;
	uint32 r1;  /* 68k A7 (stack pointer) in the DR emulator convention */
	uint32 r24, r27, r29, lr, ctr, cr;
	uint32 a[8];
	uint32 opcode;
	char   type;
};
#define JIT_RING_SIZE 0x40000  /* 256K records; power of 2 */
static jit_ring_rec *jit_ring = NULL;
static uint32 jit_ring_idx = 0;

extern "C" void ppc_jit_dump_trace_ring(void); /* defined below; used by the trigger */

/* ---- Boot-time region profiling ----
 *
 * WHY: JIT boot takes 10-22+ min vs ~10s for the interpreter, yet the JIT executes
 * blocks faster per-block.  That means either (a) the guest executes far more
 * instructions under JIT, or (b) per-block/transition overhead dominates.  The DR
 * (68k) emulator at ROM+0x460000..+0x500000 is NOT JIT-compilable, so JIT-mode boot
 * ping-pongs between JIT code (nanokernel/RAM) and interpreted code (DR emulator).
 * These counters measure the work split and the transition rate in BOTH modes so
 * the two can be compared directly.
 *
 * Logged by the heartbeat every ~5s as:
 *   [JIT ...]    jNK=n jDR=n jRAM=n | iNK=n iDR=n iRAM=n | j2i=n i2j=n
 *   [INTERP ...] iNK=n iDR=n iRAM=n        (pure interpreter mode, SS_USE_JIT=0)
 *
 * Region key: NK = nanokernel+toolbox ROM (JIT-compilable), DR = 68k DR emulator
 * (interpreter-only), RAM = guest RAM (JIT-compilable), OTH = everything else. */
enum { RGN_NK = 0, RGN_DR = 1, RGN_RAM = 2, RGN_OTH = 3 };
static uint64 rgn_jit_blocks[4];     /* JIT-executed blocks, by block entry PC */
static uint64 rgn_interp_blocks[4];  /* interpreter-executed blocks, by block entry PC */
static uint64 rgn_jit_to_interp;     /* JIT dispatch fell through to interpreter */
static uint64 rgn_interp_to_jit;     /* interpreter loop handed off to JIT */

static inline int rgn_classify(uint32 pc) {
	/* ROMBase is 0x50000000 on this port; DR emulator at ROM+0x460000..+0x500000 */
	if (pc < 0x50000000) return RGN_RAM;
	if (pc < 0x50460000) return RGN_NK;
	if (pc < 0x50500000) return RGN_DR;
	return RGN_OTH;
}

static void jit_ring_init_once(void) {
	static bool done = false;
	if (done) return;
	done = true;
	const char *e = getenv("SS_JIT_TRACE_RING");
	if (e && *e == '1')
		jit_ring = (jit_ring_rec *)calloc(JIT_RING_SIZE, sizeof(jit_ring_rec));
}

static inline void jit_ring_record(powerpc_registers *r, char type,
                                   uint32 from_pc, uint32 to_pc, uint32 opcode) {
	if (!jit_ring) return;
	jit_ring_rec *rec = &jit_ring[jit_ring_idx & (JIT_RING_SIZE - 1)];
	jit_ring_idx++;
	rec->type = type; rec->from_pc = from_pc; rec->to_pc = to_pc; rec->opcode = opcode;
	rec->r1 = r->gpr[1];
	rec->r24 = r->gpr[24]; rec->r27 = r->gpr[27]; rec->r29 = r->gpr[29];
	rec->lr = r->lr; rec->ctr = r->ctr; rec->cr = r->cr.get();
	for (int i = 0; i < 8; i++) rec->a[i] = r->gpr[16 + i];

	/* SS_JIT_WATCH_ADDR=<hex>[,<hex>...]: generic software watchpoints on up to
	 * 4 guest words.  After every recorded event, read each (4-aligned) word
	 * and report every change, identifying the block/event that made it.
	 * SS_JIT_WATCH_DUMPS=<n> (default 3): how many of the first changes also
	 * dump the trace ring (set 0 when watching busy locations like stack slots).
	 * Used for the bug-#2 hunt: watch the CD-ROM DrvSts flags word and the
	 * Device Manager argument slot simultaneously. */
	{
		static int      awatch_state = -1;   /* -1 unread, 0 off, N = count */
		static uint32   awatch_addr[4];
		static uint32   awatch_last[4];
		static bool     awatch_have_last[4];
		static int      awatch_dumps = 0;
		static int      awatch_dump_budget = 3;
		if (awatch_state < 0) {
			const char *e = getenv("SS_JIT_WATCH_ADDR");
			awatch_state = 0;
			if (e && *e) {
				char buf[128]; strncpy(buf, e, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
				char *save = NULL;
				for (char *tok = strtok_r(buf, ",", &save); tok && awatch_state < 4;
				     tok = strtok_r(NULL, ",", &save))
					awatch_addr[awatch_state++] = (uint32)strtoul(tok, NULL, 16) & ~3u;
			}
			const char *d = getenv("SS_JIT_WATCH_DUMPS");
			if (d) awatch_dump_budget = atoi(d);
		}
		for (int w = 0; w < awatch_state; w++) {
			uint32 now = vm_read_memory_4(awatch_addr[w]);
			if (awatch_have_last[w] && now != awatch_last[w]) {
				fprintf(stderr, "ADDR-WATCH: [%08x] %08x -> %08x  record #%u type=%c block %08x->%08x sp=%08x r24=%08x\n",
				        awatch_addr[w], awatch_last[w], now, jit_ring_idx, type, from_pc, to_pc,
				        rec->r1, rec->r24);
				fflush(stderr);
				/* Dump the ring on the first few changes so the lead-up is captured. */
				if (awatch_dumps < awatch_dump_budget) { awatch_dumps++; ppc_jit_dump_trace_ring(); }
			}
			awatch_last[w] = now;
			awatch_have_last[w] = true;
		}
	}

	/* SS_JIT_WATCH_STUB=1: software watchpoint on the Mixed Mode switch-back
	 * stub.  ROM block 0x5010bb90 writes 0xFE020000 (the Mixed Mode F-line
	 * trap) at [r1 - 0x70]; the 68k routine called via Mixed Mode later
	 * RTSes to that stub.  The deterministic boot crash is that the stub
	 * reads back as 00000000.  This watchpoint:
	 *   - arms after every execution of block 0x5010bb90 (stub freshly written),
	 *   - verifies the write actually landed,
	 *   - after every subsequent block, checks the stub still holds 0xFE020000,
	 *   - on the first change, prints the culprit block + disarms.
	 * In a healthy flow the first change happens only after the 68k has
	 * consumed the stub (r24 reaches the stub address). */
	{
		static int   watch_enabled = -1;
		static uint32 watch_addr = 0;
		static uint32 watch_arm_idx = 0;
		if (watch_enabled < 0) {
			const char *e = getenv("SS_JIT_WATCH_STUB");
			watch_enabled = (e && *e == '1') ? 1 : 0;
		}
		if (watch_enabled == 1) {
			if (watch_addr != 0) {
				uint32 now = vm_read_memory_4(watch_addr);
				if (now != 0xFE020000) {
					fprintf(stderr, "STUB-WATCH: [%08x] changed FE020000 -> %08x\n"
					        "  culprit record #%u: type=%c block %08x -> %08x sp=%08x r24=%08x\n"
					        "  (armed at record #%u)\n",
					        watch_addr, now, jit_ring_idx, type, from_pc, to_pc,
					        rec->r1, rec->r24, watch_arm_idx);
					fflush(stderr);
					watch_addr = 0; /* disarm; next 5010bb90 re-arms */
				}
			}
			if (from_pc == 0x5010bb90) {
				watch_addr = r->gpr[1] - 0x70;
				watch_arm_idx = jit_ring_idx;
				uint32 v = vm_read_memory_4(watch_addr);
				if (v != 0xFE020000) {
					fprintf(stderr, "STUB-WATCH: ARM FAILED — [%08x] = %08x right after stub-writer block (write itself broken)\n",
					        watch_addr, v);
					fflush(stderr);
					watch_addr = 0;
				}
			}
		}
	}

	/* SS_JIT_RING_DUMP_TRIGGER=1: dump the ring shortly after the ROM's 68k
	 * (DR) emulator starts executing 68k code located in the guest STACK
	 * region (Mixed Mode switch-back stubs live there).  ROM-anchored
	 * condition: the executing block is in the DR emulator range AND the 68k
	 * PC (r24) is in the stack region.  A countdown delays the dump so the
	 * records show what the stub execution actually does.  Used to capture
	 * what a WORKING (interpreter) run executes at a stack stub, vs the
	 * zeros a broken JIT run finds there. Fires once. */
	{
		static int trigger_state = -1;     /* -1 unread, 0 off, 1 armed, 2 counting, 3 done */
		static int trigger_countdown = 0;
		if (trigger_state < 0) {
			const char *e = getenv("SS_JIT_RING_DUMP_TRIGGER");
			trigger_state = (e && *e == '1') ? 1 : 0;
		}
		if (trigger_state == 1 &&
		    from_pc >= 0x50460000 && from_pc < 0x50500000 &&
		    rec->r24 >= 0x103f0000 && rec->r24 < 0x10400000) {
			trigger_state = 2;
			trigger_countdown = 100;
			fprintf(stderr, "RING TRIGGER: DR emulator executing stack code, r24=%08x sp=%08x (dump in 100 records)\n",
			        rec->r24, rec->r1);
		}
		if (trigger_state == 2 && --trigger_countdown <= 0) {
			trigger_state = 3;
			ppc_jit_dump_trace_ring();
			fprintf(stderr, "RING TRIGGER: dump complete\n");
		}
	}
}

/* EMUL_OP entry/return recorder — called from sheepshaver_glue.cpp's
 * execute_emul_op() in BOTH interpreter and JIT mode.  Record layout reuse:
 *   from_pc = 68k PC, to_pc = opcode = EMUL_OP number,
 *   r27 field = 68k D0, r29 field = 68k D1, r1 = sp, a[] = A0-A7.
 * Type 'E' = before EmulOp (inputs), 'R' = after (results; D0 = result code). */
extern "C" void ppc_jit_ring_record_emulop(char type, uint32 pc68k, uint32 op,
                                           uint32 d0, uint32 d1, uint32 sp, const uint32 *a_regs) {
	if (!jit_ring) return;
	jit_ring_rec *rec = &jit_ring[jit_ring_idx & (JIT_RING_SIZE - 1)];
	jit_ring_idx++;
	rec->type = type; rec->from_pc = pc68k; rec->to_pc = op; rec->opcode = op;
	rec->r1 = sp;
	rec->r24 = pc68k; rec->r27 = d0; rec->r29 = d1;
	rec->lr = 0; rec->ctr = 0; rec->cr = 0;
	for (int i = 0; i < 8; i++) rec->a[i] = a_regs[i];
	/* Driver EMUL_OPs (SONY/DISK/CDROM OPEN/PRIME/CONTROL/STATUS, ops 10-21):
	 * A0 = IOParam pointer.  Capture the request so working-vs-broken boots can
	 * be compared at the I/O-request level (A3-corruption / spurious CD-eject):
	 *   lr field  = ioBuffer   [a0+0x20]
	 *   ctr field = ioReqCount [a0+0x24]   ('R' records: ioActCount [a0+0x28])
	 *   cr field  = ioPosOffset[a0+0x2e]   ('R' records: ioResult   [a0+0x10])
	 * Only for driver ops — a0 is not a pointer for other EMUL_OPs. */
	if (op >= 10 && op <= 21) {
		uint32 a0 = a_regs[0];
		if (type == 'E') {
			rec->lr  = vm_read_memory_4(a0 + 0x20);
			rec->ctr = vm_read_memory_4(a0 + 0x24);
			rec->cr  = vm_read_memory_4(a0 + 0x2e);
		} else {
			rec->lr  = vm_read_memory_4(a0 + 0x28);
			rec->ctr = vm_read_memory_4(a0 + 0x10);
			rec->cr  = vm_read_memory_4(a0 + 0x2e);
		}
	}
}

extern "C" void ppc_jit_dump_trace_ring(void) {
	if (!jit_ring || jit_ring_idx == 0) return;
	FILE *f = fopen("/tmp/ss_jit_ring.txt", "w");
	if (!f) return;
	uint32 n = jit_ring_idx < JIT_RING_SIZE ? jit_ring_idx : JIT_RING_SIZE;
	uint32 start = jit_ring_idx - n;
	for (uint32 i = 0; i < n; i++) {
		const jit_ring_rec *rec = &jit_ring[(start + i) & (JIT_RING_SIZE - 1)];
		fprintf(f, "%c %08x %08x op=%08x sp=%08x r24=%08x r27=%08x r29=%08x lr=%08x ctr=%08x cr=%08x "
		           "a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x a6=%08x a7=%08x\n",
		        rec->type, rec->from_pc, rec->to_pc, rec->opcode, rec->r1,
		        rec->r24, rec->r27, rec->r29, rec->lr, rec->ctr, rec->cr,
		        rec->a[0], rec->a[1], rec->a[2], rec->a[3],
		        rec->a[4], rec->a[5], rec->a[6], rec->a[7]);
	}
	fclose(f);
	fprintf(stderr, "JIT trace ring: %u records (of %u total) dumped to /tmp/ss_jit_ring.txt\n",
	        n, jit_ring_idx);
}

void powerpc_cpu::jit_interp_one(uint32 opcode, uint32 pc_val)
{
	/* The bridge owns the guest PC: set it so the handler observes the correct
	 * PC, then let the handler advance it (increment_pc or branch semantics). */
	pc() = pc_val;
	jit_ring_record(regs_ptr(), 'C', pc_val, 0, opcode);
	const instr_info_t *ii = decode(opcode);
	ii->execute(this, opcode);
}

void powerpc_cpu::jit_set_active()
{
	s_active_cpu = this;
}

extern "C" void ppc_jit_interp_one(uint32_t opcode, uint32_t pc_val)
{
	/* s_active_cpu is set by execute() and by jit_set_active() (test harness).
	 * A NULL here means the JIT was driven without registering a cpu — a bug. */
	assert(s_active_cpu != NULL);
	s_active_cpu->jit_interp_one(opcode, pc_val);
}
#endif

static int ppc_refcount = 0;

#ifdef DO_CONVENTION_CALL_STATICS
template<> bool nv_mem_fun1_t<void, powerpc_cpu, uint32>::do_convention_call_init_done = false;
template<> int nv_mem_fun1_t<void, powerpc_cpu, uint32>::do_convention_call_code_len = 0;
template<> int nv_mem_fun1_t<void, powerpc_cpu, uint32>::do_convention_call_pf_offset = 0;
#endif

void powerpc_cpu::set_register(int id, any_register const & value)
{
	if (id >= powerpc_registers::GPR(0) && id <= powerpc_registers::GPR(31)) {
		gpr(id - powerpc_registers::GPR_BASE) = value.i;
		return;
	}
	if (id >= powerpc_registers::FPR(0) && id <= powerpc_registers::FPR(31)) {
		fpr(id - powerpc_registers::FPR_BASE) = value.d;
		return;
	}
	switch (id) {
	case powerpc_registers::CR:			cr().set(value.i);		break;
	case powerpc_registers::FPSCR:		fpscr() = value.i;		break;
	case powerpc_registers::XER:		xer().set(value.i);		break;
	case powerpc_registers::LR:			lr() = value.i;			break;
	case powerpc_registers::CTR:		ctr() = value.i;		break;
	case basic_registers::PC:
	case powerpc_registers::PC:			pc() = value.i;			break;
	case basic_registers::SP:
	case powerpc_registers::SP:			gpr(1)= value.i;		break;
	default:							abort();				break;
	}
}

any_register powerpc_cpu::get_register(int id)
{
	any_register value;
	if (id >= powerpc_registers::GPR(0) && id <= powerpc_registers::GPR(31)) {
		value.i = gpr(id - powerpc_registers::GPR_BASE);
		return value;
	}
	if (id >= powerpc_registers::FPR(0) && id <= powerpc_registers::FPR(31)) {
		value.d = fpr(id - powerpc_registers::FPR_BASE);
		return value;
	}
	switch (id) {
	case powerpc_registers::CR:			value.i = cr().get();	break;
	case powerpc_registers::FPSCR:		value.i = fpscr();		break;
	case powerpc_registers::XER:		value.i = xer().get();	break;
	case powerpc_registers::LR:			value.i = lr();			break;
	case powerpc_registers::CTR:		value.i = ctr();		break;
	case basic_registers::PC:
	case powerpc_registers::PC:			value.i = pc();			break;
	case basic_registers::SP:
	case powerpc_registers::SP:			value.i = gpr(1);		break;
	default:							abort();				break;
	}
	return value;
}

#if KPX_MAX_CPUS != 1
uint32 powerpc_registers::reserve_valid = 0;
uint32 powerpc_registers::reserve_addr = 0;
uint32 powerpc_registers::reserve_data = 0;
#endif

void powerpc_cpu::init_registers()
{
	assert((((uintptr)&vr(0)) % 16) == 0);
	for (int i = 0; i < 32; i++) {
		gpr(i) = 0;
		fpr(i) = 0;
	}
	cr().set(0);
	fpscr() = 0;
	xer().set(0);
	lr() = 0;
	ctr() = 0;
	pc() = 0;
}

void powerpc_cpu::init_flight_recorder()
{
#if PPC_FLIGHT_RECORDER
	log_ptr = 0;
	log_ptr_wrapped = false;
#endif
}

void powerpc_cpu::do_record_step(uint32 pc, uint32 opcode)
{
#if PPC_FLIGHT_RECORDER
	log[log_ptr].pc = pc;
	log[log_ptr].opcode = opcode;
#ifdef SHEEPSHAVER
	log[log_ptr].sp = gpr(1);
	log[log_ptr].r24 = gpr(24);
#endif
#if PPC_FLIGHT_RECORDER >= 2
	for (int i = 0; i < 32; i++) {
		log[log_ptr].r[i] = gpr(i);
		log[log_ptr].fr[i] = fpr(i);
	}
	log[log_ptr].lr = lr();
	log[log_ptr].ctr = ctr();
	log[log_ptr].cr = cr().get();
	log[log_ptr].xer = xer().get();
	log[log_ptr].fpscr = fpscr();
#endif
	log_ptr++;
	if (log_ptr == LOG_SIZE) {
		log_ptr = 0;
		log_ptr_wrapped = true;
	}
#endif
}

#if PPC_FLIGHT_RECORDER
void powerpc_cpu::start_log()
{
	logging = true;
	invalidate_cache();
}

void powerpc_cpu::stop_log()
{
	logging = false;
	invalidate_cache();
}

void powerpc_cpu::dump_log(const char *filename)
{
	if (filename == NULL)
		filename = "ppc.log";

	FILE *f = fopen(filename, "w");
	if (f == NULL)
		return;

	int start_ptr = 0;
	int log_size = log_ptr;
	if (log_ptr_wrapped) {
		start_ptr = log_ptr;
		log_size = LOG_SIZE;
	}

	for (int i = 0; i < log_size; i++) {
		int j = (i + start_ptr) % LOG_SIZE;
#if PPC_FLIGHT_RECORDER >= 2
		fprintf(f, " pc %08x  lr %08x ctr %08x  cr %08x xer %08x ", log[j].pc, log[j].lr, log[j].ctr, log[j].cr, log[j].xer);
		fprintf(f, " r0 %08x  r1 %08x  r2 %08x  r3 %08x ", log[j].r[0], log[j].r[1], log[j].r[2], log[j].r[3]);
		fprintf(f, " r4 %08x  r5 %08x  r6 %08x  r7 %08x ", log[j].r[4], log[j].r[5], log[j].r[6], log[j].r[7]);
		fprintf(f, " r8 %08x  r9 %08x r10 %08x r11 %08x ", log[j].r[8], log[j].r[9], log[j].r[10], log[j].r[11]);
		fprintf(f, "r12 %08x r13 %08x r14 %08x r15 %08x ", log[j].r[12], log[j].r[13], log[j].r[14], log[j].r[15]);
		fprintf(f, "r16 %08x r17 %08x r18 %08x r19 %08x ", log[j].r[16], log[j].r[17], log[j].r[18], log[j].r[19]);
		fprintf(f, "r20 %08x r21 %08x r22 %08x r23 %08x ", log[j].r[20], log[j].r[21], log[j].r[22], log[j].r[23]);
		fprintf(f, "r24 %08x r25 %08x r26 %08x r27 %08x ", log[j].r[24], log[j].r[25], log[j].r[26], log[j].r[27]);
		fprintf(f, "r28 %08x r29 %08x r30 %08x r31 %08x\n", log[j].r[28], log[j].r[29], log[j].r[30], log[j].r[31]);
		fprintf(f, "opcode %08x\n", log[j].opcode);
#else
		fprintf(f, " pc %08x opc %08x", log[j].pc, log[j].opcode);
#ifdef SHEEPSHAVER
		fprintf(f, " sp %08x r24 %08x", log[j].sp, log[j].r24);
#endif
		fprintf(f, "| ");
#if !ENABLE_MON
		fprintf(f, "\n");
#endif
#endif
#if ENABLE_MON
		disass_ppc(f, log[j].pc, log[j].opcode);
#endif
	}
	fclose(f);
}
#endif

#if ENABLE_MON
static uint32 mon_read_byte_ppc(uintptr addr)
{
	return *((uint8 *)addr);
}

static void mon_write_byte_ppc(uintptr addr, uint32 b)
{
	uint8 *m = (uint8 *)addr;
	*m = b;
}
#endif

void powerpc_cpu::initialize()
{
#ifdef SHEEPSHAVER
	printf("PowerPC CPU emulator by Gwenole Beauchesne\n");
#endif

#if PPC_PROFILE_REGS_USE
	reginfo = new register_info[32];
	for (int i = 0; i < 32; i++) {
		reginfo[i].id = i;
		reginfo[i].count = 0;
	}
#endif

	init_flight_recorder();
	init_decoder();
	init_registers();
	init_decode_cache();
	execute_depth = 0;

	// Initialize block lookup table
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
	my_block_cache.initialize();
#endif

	// Init cache range invalidate recorder
	cache_range.start = cache_range.end = 0;

	// Init syscalls handler
	execute_do_syscall = NULL;

	// Init field2mask
	for (int i = 0; i < 256; i++) {
		uint32 mask = 0;
		if (i & 0x01) mask |= 0x0000000f;
		if (i & 0x02) mask |= 0x000000f0;
		if (i & 0x04) mask |= 0x00000f00;
		if (i & 0x08) mask |= 0x0000f000;
		if (i & 0x10) mask |= 0x000f0000;
		if (i & 0x20) mask |= 0x00f00000;
		if (i & 0x40) mask |= 0x0f000000;
		if (i & 0x80) mask |= 0xf0000000;
		field2mask[i] = mask;
	}

#if ENABLE_MON
	mon_init();
	mon_read_byte = mon_read_byte_ppc;
	mon_write_byte = mon_write_byte_ppc;
#endif

#if PPC_PROFILE_COMPILE_TIME
	compile_count = 0;
	compile_time = 0;
	emul_start_time = clock();
#endif
}

#if PPC_ENABLE_JIT
void powerpc_cpu::enable_jit(uint32 cache_size)
{
	use_jit = true;
	if (cache_size)
		codegen.set_cache_size(cache_size);
	codegen.initialize();
}
#endif

// Memory allocator returning powerpc_cpu objects aligned on 16-byte boundaries
// FORMAT: [ alignment ] magic identifier, offset to malloc'ed data, powerpc_cpu data
void *powerpc_cpu::operator new(size_t size)
{
	const int ALIGN = 16;

	// Allocate enough space for powerpc_cpu data + signature + align pad
	uint8 *ptr = (uint8 *)malloc(size + ALIGN * 2);
	if (ptr == NULL)
		throw std::bad_alloc();

	// Align memory
	int ofs = 0;
	while ((((uintptr)ptr) % ALIGN) != 0)
		ofs++, ptr++;

	// Insert signature and offset
	struct aligned_block_t {
		uint32 pad[(ALIGN - 8) / 4];
		uint32 signature;
		uint32 offset;
		uint8  data[sizeof(powerpc_cpu)];
	};
	aligned_block_t *blk = (aligned_block_t *)ptr;
	blk->signature = 0x53435055;		/* 'SCPU' */
	blk->offset = ofs + (&blk->data[0] - (uint8 *)blk);
	assert((((uintptr)&blk->data) % ALIGN) == 0);
	return &blk->data[0];
}

void powerpc_cpu::operator delete(void *p)
{
	uint32 *blk = (uint32 *)p;
	assert(blk[-2] == 0x53435055);		/* 'SCPU' */
	void *ptr = (void *)(((uintptr)p) - blk[-1]);
	free(ptr);
}

#ifdef SHEEPSHAVER
powerpc_cpu::powerpc_cpu()
#if PPC_ENABLE_JIT
	: codegen(this)
#endif
#else
powerpc_cpu::powerpc_cpu(task_struct *parent_task)
	: basic_cpu(parent_task)
#if PPC_ENABLE_JIT
	, codegen(this)
#endif
#endif
{
#if PPC_ENABLE_JIT
	use_jit = false;
#endif
	spcflags().init();
	++ppc_refcount;
	initialize();
}

powerpc_cpu::~powerpc_cpu()
{
	--ppc_refcount;
#if PPC_PROFILE_COMPILE_TIME
	clock_t emul_end_time = clock();

	const char *type = NULL;
#if PPC_ENABLE_JIT
	if (use_jit)
		type = "compile";
#endif
#if PPC_DECODE_CACHE
	if (!type)
		type = "predecode";
#endif
	if (type) {
		printf("### Statistics for block %s\n", type);
		printf("Total block %s count : %d\n", type, compile_count);
		uint32 emul_time = emul_end_time - emul_start_time;
		printf("Total emulation time : %.1f sec\n",
			   double(emul_time) / double(CLOCKS_PER_SEC));
		printf("Total %s time : %.1f sec (%.1f%%)\n", type,
			   double(compile_time) / double(CLOCKS_PER_SEC),
			   100.0 * double(compile_time) / double(emul_time));
		printf("\n");
	}
#endif

#if PPC_PROFILE_GENERIC_CALLS
	if (use_jit && ppc_refcount == 0) {
		uint64 total_generic_calls_count = 0;
		for (int i = 0; i < PPC_I(MAX); i++) {
			generic_calls_ids[i] = i;
			total_generic_calls_count += generic_calls_count[i];
		}
		qsort(generic_calls_ids, PPC_I(MAX), sizeof(int), generic_calls_compare);
		printf("Rank      Count Ratio Name\n");
		for (int i = 0; i < generic_calls_top_ten; i++) {
			uint32 mnemo = generic_calls_ids[i];
			uint32 count = generic_calls_count[mnemo];
			const instr_info_t *ii = powerpc_ii_table;
			while (ii->mnemo != mnemo)
				ii++;
			printf("%03d: %10lu %2.1f%% %s\n", i, count, 100.0*double(count)/double(total_generic_calls_count), ii->name);
		}
	}
#endif

#if PPC_PROFILE_REGS_USE
	printf("\n### Statistics for register usage\n");
	uint64 tot_reg_count = 0;
	for (int i = 0; i < 32; i++)
		tot_reg_count += reginfo[i].count;
	qsort(reginfo, 32, sizeof(register_info), register_info_compare);
	uint64 cum_reg_count = 0;
	for (int i = 0; i < 32; i++) {
		cum_reg_count += reginfo[i].count;
	    printf("r%-2d : %16llu %2.1f%% [%3.1f%%]\n",
			   reginfo[i].id, reginfo[i].count,
			   100.0*double(reginfo[i].count)/double(tot_reg_count),
			   100.0*double(cum_reg_count)/double(tot_reg_count));
	}
	delete[] reginfo;
#endif

	kill_decode_cache();

#if ENABLE_MON
	mon_exit();
#endif
}

void powerpc_cpu::dump_registers()
{
	fprintf(stderr, " r0 %08x   r1 %08x   r2 %08x   r3 %08x\n", gpr(0), gpr(1), gpr(2), gpr(3));
	fprintf(stderr, " r4 %08x   r5 %08x   r6 %08x   r7 %08x\n", gpr(4), gpr(5), gpr(6), gpr(7));
	fprintf(stderr, " r8 %08x   r9 %08x  r10 %08x  r11 %08x\n", gpr(8), gpr(9), gpr(10), gpr(11));
	fprintf(stderr, "r12 %08x  r13 %08x  r14 %08x  r15 %08x\n", gpr(12), gpr(13), gpr(14), gpr(15));
	fprintf(stderr, "r16 %08x  r17 %08x  r18 %08x  r19 %08x\n", gpr(16), gpr(17), gpr(18), gpr(19));
	fprintf(stderr, "r20 %08x  r21 %08x  r22 %08x  r23 %08x\n", gpr(20), gpr(21), gpr(22), gpr(23));
	fprintf(stderr, "r24 %08x  r25 %08x  r26 %08x  r27 %08x\n", gpr(24), gpr(25), gpr(26), gpr(27));
	fprintf(stderr, "r28 %08x  r29 %08x  r30 %08x  r31 %08x\n", gpr(28), gpr(29), gpr(30), gpr(31));
	fprintf(stderr, " f0 %02.5f   f1 %02.5f   f2 %02.5f   f3 %02.5f\n", fpr(0), fpr(1), fpr(2), fpr(3));
	fprintf(stderr, " f4 %02.5f   f5 %02.5f   f6 %02.5f   f7 %02.5f\n", fpr(4), fpr(5), fpr(6), fpr(7));
	fprintf(stderr, " f8 %02.5f   f9 %02.5f  f10 %02.5f  f11 %02.5f\n", fpr(8), fpr(9), fpr(10), fpr(11));
	fprintf(stderr, "f12 %02.5f  f13 %02.5f  f14 %02.5f  f15 %02.5f\n", fpr(12), fpr(13), fpr(14), fpr(15));
	fprintf(stderr, "f16 %02.5f  f17 %02.5f  f18 %02.5f  f19 %02.5f\n", fpr(16), fpr(17), fpr(18), fpr(19));
	fprintf(stderr, "f20 %02.5f  f21 %02.5f  f22 %02.5f  f23 %02.5f\n", fpr(20), fpr(21), fpr(22), fpr(23));
	fprintf(stderr, "f24 %02.5f  f25 %02.5f  f26 %02.5f  f27 %02.5f\n", fpr(24), fpr(25), fpr(26), fpr(27));
	fprintf(stderr, "f28 %02.5f  f29 %02.5f  f30 %02.5f  f31 %02.5f\n", fpr(28), fpr(29), fpr(30), fpr(31));
	fprintf(stderr, " lr %08x  ctr %08x   cr %08x  xer %08x\n", lr(), ctr(), cr().get(), xer().get());
	fprintf(stderr, " pc %08x fpscr %08x\n", pc(), fpscr());
	fflush(stderr);
}

void powerpc_cpu::dump_instruction(uint32 opcode)
{
	fprintf(stderr, "[%08x]-> %08x\n", pc(), opcode);
}

void powerpc_cpu::fake_dump_registers(uint32)
{
	dump_registers();
}

void powerpc_registers::interrupt_copy(powerpc_registers &oregs, powerpc_registers const &iregs)
{
	for (int i = 0; i < 32; i++) {
		oregs.gpr[i] = iregs.gpr[i];
		oregs.fpr[i] = iregs.fpr[i];
	}
	oregs.cr	= iregs.cr;
	oregs.fpscr	= iregs.fpscr;
	oregs.xer	= iregs.xer;
	oregs.lr	= iregs.lr;
	oregs.ctr	= iregs.ctr;
	oregs.pc	= iregs.pc;

	uint32 vrsave = iregs.vrsave;
	oregs.vrsave  = vrsave;
	if (vrsave) {
		for (int i = 31; i >= 0; i--) {
			if (vrsave & 1)
				oregs.vr[i] = iregs.vr[i];
			vrsave >>= 1;
		}
	}
}

bool powerpc_cpu::check_spcflags()
{
	if (spcflags().test(SPCFLAG_CPU_EXEC_RETURN)) {
		spcflags().clear(SPCFLAG_CPU_EXEC_RETURN);
		return false;
	}
#ifdef SHEEPSHAVER
	if (spcflags().test(SPCFLAG_CPU_HANDLE_INTERRUPT)) {
		spcflags().clear(SPCFLAG_CPU_HANDLE_INTERRUPT);
		static bool processing_interrupt = false;
		if (!processing_interrupt) {
			processing_interrupt = true;
			powerpc_registers r;
			powerpc_registers::interrupt_copy(r, regs());
			HandleInterrupt(&r);
			powerpc_registers::interrupt_copy(regs(), r);
			processing_interrupt = false;
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
			/* File only — interrupts arrive at 60Hz during VBL spinwait, too noisy for stderr */
			JIT_FLOG("interrupt delivered, pc=%08x", (uint32_t)pc());
#endif
		}
	}
	if (spcflags().test(SPCFLAG_CPU_TRIGGER_INTERRUPT)) {
		spcflags().clear(SPCFLAG_CPU_TRIGGER_INTERRUPT);
		spcflags().set(SPCFLAG_CPU_HANDLE_INTERRUPT);
	}
#endif
	if (spcflags().test(SPCFLAG_CPU_ENTER_MON)) {
		spcflags().clear(SPCFLAG_CPU_ENTER_MON);
#if ENABLE_MON
		// Start up mon in real-mode
		const char *arg[] = {
			"mon",
#ifdef SHEEPSHAVER
			"-m",
#endif
			"-r",
			NULL
		};
		mon(sizeof(arg)/sizeof(arg[0]) - 1, arg);
#endif
	}
	return true;
}

#if DYNGEN_DIRECT_BLOCK_CHAINING
void * powerpc_cpu::call_compile_chain_block(powerpc_cpu * the_cpu, block_info *sbi)
{
	return the_cpu->compile_chain_block(sbi);
}

void * PF_CONVENTION powerpc_cpu::compile_chain_block(block_info *sbi)
{
	// Block index is stuffed into the source basic block pointer,
	// which is aligned at least on 4-byte boundaries
	const int n = ((uintptr)sbi) & 3;
	sbi = (block_info *)(((uintptr)sbi) & ~3L);

	const uint32 tpc = sbi->li[n].jmp_pc;
	block_info *tbi = my_block_cache.find(tpc);
	if (tbi == NULL)
		tbi = compile_block(tpc);
	assert(tbi && tbi->pc == tpc);

	dg_set_jmp_target(sbi->li[n].jmp_addr, tbi->entry_point);
	return tbi->entry_point;
}
#endif

void powerpc_cpu::execute(uint32 entry)
{
	bool invalidated_cache = false;
	pc() = entry;
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	/* Make this cpu reachable from the JIT inline-interpreter-call bridge
	 * (ppc_jit_interp_one).  Single-threaded; nested execute() calls share the
	 * same cpu object, so re-assigning here is harmless. */
	s_active_cpu = this;
	FILE *jit_trace_fp_for_cpu = NULL; /* set by trace block at pdi_execute, read by JIT gate */
#endif
#if PPC_EXECUTE_DUMP_STATE
	const bool dump_state = true;
#endif
	execute_depth++;
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
	if (execute_depth == 1 || (PPC_ENABLE_JIT && PPC_REENTRANT_JIT)) {
#if PPC_ENABLE_JIT
		if (use_jit) {
			block_info *bi = my_block_cache.find(pc());
			if (bi == NULL)
				bi = compile_block(pc());
			for (;;) {
				// Execute all cached blocks
				for (;;) {
					codegen.execute(bi->entry_point);

					if (!spcflags().empty()) {
						if (!check_spcflags())
							goto return_site;

						// Force redecoding if cache was invalidated
						if (spcflags().test(SPCFLAG_JIT_EXEC_RETURN)) {
							spcflags().clear(SPCFLAG_JIT_EXEC_RETURN);
							invalidated_cache = true;
							break;
						}
					}

					// Don't check for backward branches here as this
					// is now done by generated code. Besides, we will
					// get here if the fast cache lookup failed too.
					if ((bi = my_block_cache.find(pc())) == NULL)
						break;
				}

				// Compile new block
				bi = compile_block(pc());
			}
		}
#endif
#if PPC_DECODE_CACHE
		block_info *bi = my_block_cache.find(pc());
		if (bi != NULL)
			goto pdi_execute;
		for (;;) {
#if PPC_PROFILE_COMPILE_TIME
			compile_count++;
			clock_t start_time;
			start_time = clock();
#endif
			bi = my_block_cache.new_blockinfo();
			bi->init(pc());

			// Predecode a new block
			block_info::decode_info *di;
			const instr_info_t *ii;
			uint32 dpc;
			di = bi->di = decode_cache_p;
			dpc = pc() - 4;
			do {
				uint32 opcode = vm_read_memory_4(dpc += 4);
				ii = decode(opcode);
#if PPC_EXECUTE_DUMP_STATE
				if (dump_state) {
					di->opcode = opcode;
					di->execute = nv_mem_fun(&powerpc_cpu::dump_instruction);
					di++;
				}
#endif
#if PPC_FLIGHT_RECORDER
				if (is_logging()) {
					di->opcode = opcode;
					di->execute = nv_mem_fun(&powerpc_cpu::record_step);
					di++;
				}
#endif
				di->opcode = opcode;
				di->execute = ii->execute;
				di++;
#if PPC_EXECUTE_DUMP_STATE
				if (dump_state) {
					di->opcode = 0;
					di->execute = nv_mem_fun(&powerpc_cpu::fake_dump_registers);
					di++;
				}
#endif
				if (di >= decode_cache_end_p) {
					// Invalidate cache and move current code to start
					invalidate_cache();
					const int blocklen = di - bi->di;
					memmove(decode_cache_p, bi->di, blocklen * sizeof(*di));
					bi->di = decode_cache_p;
					di = bi->di + blocklen;
				}
			} while ((ii->cflow & CFLOW_END_BLOCK) == 0);
			bi->end_pc = dpc;
			bi->min_pc = dpc;
			bi->max_pc = entry;
			bi->size = di - bi->di;
			my_block_cache.add_to_cl_list(bi);
			my_block_cache.add_to_active_list(bi);
			decode_cache_p += bi->size;
#if PPC_PROFILE_COMPILE_TIME
			compile_time += (clock() - start_time);
#endif

			// Execute all cached blocks
		  pdi_execute:
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
			/* PC trace: log block-start PCs for differential JIT vs interpreter debugging.
			 * SS_JIT_TRACE=/path → each line is: "I <pc>" (interpreter block entry)
			 * or "J <from_pc> <to_pc> <r1> <r3>" (JIT block execution, logged post-run). */
			{
				static FILE *jit_trace_fp = (FILE *)(uintptr_t)1;
				if (jit_trace_fp == (FILE *)(uintptr_t)1) {
					const char *path = getenv("SS_JIT_TRACE");
					jit_trace_fp = path ? fopen(path, "w") : NULL;
					jit_ring_init_once(); /* SS_JIT_TRACE_RING=1: in-memory ring */
				}
				if (jit_trace_fp) { fprintf(jit_trace_fp, "I %08x\n", pc()); fflush(jit_trace_fp); }
				jit_trace_fp_for_cpu = jit_trace_fp; /* share with JIT gate below */
				jit_ring_record(regs_ptr(), 'I', pc(), 0, 0);
			}
#endif
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
			/* AArch64 direct-codegen JIT: try to execute block natively.
			 *
			 * Gates in this block — see SheepShaver/docs/AARCH64_JIT_RUNTIME_CONTRACT.md:
			 *
			 * GATE 1 (SS_USE_JIT=0): OVERRIDE — JIT enabled by default; set SS_USE_JIT=0
			 *   to force interpreter execution for diagnostics or regressions.
			 *
			 * GATE 2 (jblk.complete): CONTAINMENT — only execute fully compiled blocks.
			 *   Status: overcautious; partial blocks are safe (truncation epilogue writes
			 *   valid PPCR_PC and interpreter can resume from there). Candidate for removal.
			 *   Expiry: remove when parity harness confirms partial-block execution is correct.
			 *
			 * GATE 3 (PC range check): DIAGNOSTIC — detects JIT compiler bugs that produce
			 *   out-of-range PCs.  Should log before skipping, not silently continue.
			 */
			{
				static bool jit_init_done = false;
				static const char *jit_env = getenv("SS_USE_JIT");
				static bool jit_enabled = !(jit_env && jit_env[0] == '0' && jit_env[1] == '\0');
				if (!jit_enabled) goto skip_jit; /* GATE 1: SS_USE_JIT=0 diagnostic override */
				if (!jit_init_done) {
					/* 64 MB code cache: large enough to hold translations of both
					 * the hot RAM working set and the ROM toolbox without recurring
					 * full flushes (each flush forces recompilation of everything). */
					ppc_jit_aarch64_init(65536);
					/* Register the Mac ROM as a JIT-compilable range.  ROM is
					 * write-protected after patching (main_unix.cpp), so compiled
					 * ROM blocks are permanently valid.  The range stops at
					 * +0x460000 to EXCLUDE the ROM's built-in 68k (DR) emulator.
					 * Session 7 proved entry-poll suppression alone is insufficient:
					 * the C dispatcher's between-block spcflags check also fires
					 * mid-dispatch-cycle (between DR dispatch and toolbox handler),
					 * setting CR2.LT before bclr 5,8 can evaluate it. Fully deferring
					 * spcflags starves interrupts (jNK goes flat). The fix needs
					 * per-instruction spcflags delivery that matches interpreter
					 * timing — see session 7 LEARNINGS entry and HANDOFF.
					 * SS_JIT_NO_ROM=1: bisect switch — keep ROM interpreter-only. */
					{
						const char *no_rom = getenv("SS_JIT_NO_ROM");
						if (!(no_rom && *no_rom == '1')) {
							/* SS_JIT_ROM_SIZE: override ROM JIT range (hex).
							 * Binary-search between 0x460000 (safe) and 0x500000
							 * (broken) to locate DR emulator hang region. */
							uint32_t rom_jit_size = 0x500000;
							const char *rom_size_env = getenv("SS_JIT_ROM_SIZE");
							if (rom_size_env) {
								rom_jit_size = (uint32_t)strtoul(rom_size_env, NULL, 16);
								if (rom_jit_size < 0x100000) rom_jit_size = 0x100000;
								if (rom_jit_size > 0x500000) rom_jit_size = 0x500000;
							}
							fprintf(stderr, "[JIT] ROM JIT range: [%08x..%08x] (size=0x%x%s)\n",
								ROMBase, ROMBase + rom_jit_size, rom_jit_size,
								rom_size_env ? ", SS_JIT_ROM_SIZE override" : "");
							ppc_jit_aarch64_set_rom_range(ROMBase, rom_jit_size, ROMBaseHost);
						}
					}
					jit_init_done = true;
					JIT_LOG("JIT initialized");
				}
				ppc_jit_block jblk;
				/* GATE 2: execute only complete native blocks. Incomplete blocks are
				 * compile-time probes only; skip_jit lets the interpreter execute the
				 * first uncompiled/fallback-only instruction at the original PC.
				 *
				 * Two-tier dispatch: ppc_jit_aarch64_lookup_fast() is a hash-lookup-only
				 * fast path (tiny stack frame, no compile machinery) covering the
				 * overwhelmingly common already-compiled case; the full compile() runs
				 * only on lookup miss. */
				uint32 jit_block_start_pc = pc(); /* block entry PC, for trace + GATE3 */
				ppc_jit_entry_fn fn = ppc_jit_aarch64_lookup_fast(pc());
				if (!fn && ppc_jit_aarch64_compile(pc(), RAMBaseHost, RAMSize, &jblk) && jblk.complete)
					fn = (ppc_jit_entry_fn)(void*)jblk.code;
				if (fn) {
					/* ---- Dispatch call-chain ring (SS_JIT_CHAIN_LOG=1) ----
					 * Records last 8 block-entry PCs into a ring buffer.
					 * When the target address is hit, dumps the ring to stderr.
					 * TEMPORARY diagnostic — remove after boot-hang investigation. */
					{
						static bool chain_log_enabled = false;
						static bool chain_log_checked = false;
						static uint32 dispatch_ring[8];
						static int dispatch_ring_idx = 0;
						if (__builtin_expect(!chain_log_checked, false)) {
							chain_log_checked = true;
							chain_log_enabled = (getenv("SS_JIT_CHAIN_LOG") && atoi(getenv("SS_JIT_CHAIN_LOG")));
						}
						if (__builtin_expect(chain_log_enabled, false)) {
							dispatch_ring[dispatch_ring_idx & 7] = jit_block_start_pc;
							dispatch_ring_idx++;
						}
						fn((void*)regs_ptr());
						if (__builtin_expect(chain_log_enabled && jit_block_start_pc == 0x50132ec8, false)) {
							static int chain_log_budget = 20;
							if (chain_log_budget > 0) {
								chain_log_budget--;
								uint32 exit_pc = pc();
								fprintf(stderr, "[CHAIN] exit=%08x ring:", exit_pc);
								for (int i = 0; i < 8; i++)
									fprintf(stderr, " %08x", dispatch_ring[(dispatch_ring_idx - 8 + i) & 7]);
								fprintf(stderr, "\n");
							}
						}
					}
				  pdi_jit_post:
					/* Time-based heartbeat — file only, no stderr spam */
					{
						static uint64_t jit_block_count = 0;
						static double last_t = 0;
						static uint32_t last_pc = 0;
						static int stuck_count = 0;
						jit_block_count++;
						/* Region profiling: classify by block ENTRY pc (jit_block_start_pc),
						 * since the exit pc may be in a different region. */
						rgn_jit_blocks[rgn_classify(jit_block_start_pc)]++;
						/* Heartbeat: check the clock only every 4096 blocks — clock_gettime
						 * per block (~25M/s) would itself cost ~0.5s/s of wall time. */
						if ((jit_block_count & 0xFFF) == 0) {
						double now = jit_elapsed_s();
						if (now - last_t >= 5.0) {
							jit_diag_log_open();
							uint32_t cur_pc = (uint32_t)pc();
							static uint64 prev_block_count = 0;
							uint64 delta = jit_block_count - prev_block_count;
							double dt = now - last_t;
							double rate = (dt > 0) ? delta / dt / 1e6 : 0;
							prev_block_count = jit_block_count;
							uint32_t compiled = ppc_jit_aarch64_blocks_compiled();
							fprintf(jit_log_file, "[JIT %.1fs] blocks=%llu pc=%08x %.0fM/s comp=%u | jNK=%llu jDR=%llu jRAM=%llu | iDR=%llu | j2i=%llu\n",
							        now, (unsigned long long)jit_block_count, cur_pc, rate, compiled,
							        (unsigned long long)rgn_jit_blocks[RGN_NK], (unsigned long long)rgn_jit_blocks[RGN_DR],
							        (unsigned long long)rgn_jit_blocks[RGN_RAM],
							        (unsigned long long)rgn_interp_blocks[RGN_DR],
							        (unsigned long long)rgn_jit_to_interp);
							fflush(jit_log_file);
							/* NOTE: "same PC at consecutive heartbeats" is a SAMPLING HINT, not
							 * proof of a hang — hot dispatch PCs (e.g. the nanokernel exception
							 * dispatcher at 0x50313d34) recur by chance.  Do not treat
							 * repeated sampling as proof of a hang without corroboration. */
							if (cur_pc == last_pc) {
								stuck_count++;
								if (stuck_count >= 2) {
									fprintf(stderr, "[JIT %.1fs] HOT-PC pc=%08x sampled %d consecutive heartbeats (may be sampling artifact)\n",
									        now, cur_pc, stuck_count + 1);
									fprintf(stderr, "  r1=%08x r9=%08x r10=%08x r11=%08x r12=%08x cr=%08x\n",
									        gpr(1), gpr(9), gpr(10), gpr(11), gpr(12), cr().get());
								}
							} else {
								stuck_count = 0;
								last_pc = cur_pc;
							}
							last_t = now;
						}
						}
					}
					jit_ring_record(regs_ptr(), 'J', jit_block_start_pc, pc(), 0);
					/* Log JIT block: from, to, then the 68k-emulator-relevant state
					 * (r24=68k PC, r27=68k opcode, r29=handler addr, LR, CR, XER).
					 * fflush so the final entries survive a crash. */
					if (jit_trace_fp_for_cpu) {
						/* In the ROM 68k (DR) emulator's register convention:
						 * r24 = 68k PC, r27 = opcode, r29 = handler address,
						 * r8-r15 = 68k D0-D7, r16-r23 = 68k A0-A7.
						 * a7 (r23) is the 68k stack pointer — the register whose
						 * corruption (a7=0) is the deterministic boot-crash signature. */
						fprintf(jit_trace_fp_for_cpu, "J %08x %08x r24=%08x r27=%08x r29=%08x cr=%08x so=%d ov=%d ca=%d a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x a6=%08x a7=%08x\n",
						        jit_block_start_pc, pc(), gpr(24), gpr(27), gpr(29), cr().get(),
						        xer().get_so(), xer().get_ov(), xer().get_ca(),
						        gpr(16), gpr(17), gpr(18), gpr(19), gpr(20), gpr(21), gpr(22), gpr(23));
						fflush(jit_trace_fp_for_cpu);
					}
					/* GATE 3: PC range diagnostic (log-only, rate-limited).
					 * A result PC outside RAM/ROM/SheepMem is either a legitimate
					 * branch into other Mac OS space (kernel data, DR emulator —
					 * the interpreter handles those natively) or a JIT bug.  Either
					 * way the fast-dispatch fallback below routes it correctly:
					 * compile() refuses non-compilable PCs, so execution falls back
					 * to the interpreter block cache for that PC.
					 * Do NOT evict the source block: legitimate out-of-range
					 * branches are normal control flow, and evicting forces a
					 * pointless recompile on the block's next visit. */
					uint32 jit_pc = pc();
					if (jit_pc >= (uint32)(uintptr_t)RAMBaseHost + RAMSize &&
					    !(jit_pc >= (uint32)ROMBase && jit_pc < (uint32)ROMBase + 0x600000)) {
						static int gate3_log_budget = 10;
						if (gate3_log_budget > 0) {
							gate3_log_budget--;
							fprintf(stderr, "PPC-JIT-A64: GATE3: out-of-range PC 0x%08x after block at 0x%08x — interpreter dispatch%s\n",
							        jit_pc, jit_block_start_pc,
							        gate3_log_budget == 0 ? " (further messages suppressed)" : "");
						}
					}
					if (!spcflags().empty()) {
						if (!check_spcflags()) goto return_site;
					}
					/* Fast dispatch: if next PC is already in JIT cache, stay in the
					 * JIT loop without touching the interpreter block cache.
					 * This eliminates my_block_cache.find() + pdi_execute overhead for
					 * hot block-to-block transitions where both blocks are JIT-compiled. */
					jit_block_start_pc = pc();
					fn = ppc_jit_aarch64_lookup_fast(pc());
					if (!fn && ppc_jit_aarch64_compile(pc(), RAMBaseHost, RAMSize, &jblk) && jblk.complete)
						fn = (ppc_jit_entry_fn)(void*)jblk.code;
					if (fn) {
						fn((void*)regs_ptr());
						goto pdi_jit_post;
					}
					/* Region profiling: JIT dispatch could not handle this PC (typically
					 * the DR emulator range) — falling through to the interpreter. */
					rgn_jit_to_interp++;
					bi = my_block_cache.find(pc());
					if (bi) goto pdi_execute;
					continue;
				}
			}
#endif
		  skip_jit:
			for (;;) {
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
				/* Region profiling: count interpreted blocks by entry PC, and emit a
				 * heartbeat in pure-interpreter mode (SS_USE_JIT=0) where the JIT-side
				 * heartbeat never runs.  Clock checked every 4096 blocks to keep the
				 * per-block cost to one increment + one branch. */
				{
					static uint64 interp_block_count = 0;
					static double interp_last_t = 0;
					rgn_interp_blocks[rgn_classify(bi->pc)]++;
					interp_block_count++;
					if ((interp_block_count & 0xFFF) == 0) {
						double now = jit_elapsed_s();
						if (now - interp_last_t >= 5.0) {
							jit_diag_log_open();
							fprintf(jit_log_file, "[INTERP %.1fs] blocks=%llu pc=%08x | iNK=%llu iDR=%llu iRAM=%llu iOTH=%llu | j2i=%llu i2j=%llu\n",
							        now, (unsigned long long)interp_block_count, (uint32)bi->pc,
							        (unsigned long long)rgn_interp_blocks[RGN_NK], (unsigned long long)rgn_interp_blocks[RGN_DR],
							        (unsigned long long)rgn_interp_blocks[RGN_RAM], (unsigned long long)rgn_interp_blocks[RGN_OTH],
							        (unsigned long long)rgn_jit_to_interp, (unsigned long long)rgn_interp_to_jit);
							fflush(jit_log_file);
							interp_last_t = now;
						}
					}
				}
#endif
				const int r = bi->size % 4;
				di = bi->di + r;
				int n = (bi->size + 3) / 4;
				switch (r) {
				case 0: do {
						di += 4;
						di[-4].execute(this, di[-4].opcode);
				case 3: di[-3].execute(this, di[-3].opcode);
				case 2: di[-2].execute(this, di[-2].opcode);
				case 1: di[-1].execute(this, di[-1].opcode);
					} while (--n > 0);
				}

				if (!spcflags().empty()) {
					if (!check_spcflags())
						goto return_site;

					// Force redecoding if cache was invalidated
					if (spcflags().test(SPCFLAG_JIT_EXEC_RETURN)) {
						spcflags().clear(SPCFLAG_JIT_EXEC_RETURN);
						invalidated_cache = true;
						break;
					}
				}

#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
				/* JIT handoff: exit the interpreter loop when the next PC is in the
				 * JIT's compilable domain (RAM / registered ROM), so the outer
				 * dispatch compiles and runs it natively.  This must test
				 * compilABILITY, not "already compiled": code first reached from
				 * inside an interpreter session (e.g. toolbox routines called by the
				 * interpreter-only 68k emulator) has no block yet and would otherwise
				 * never meet the compiler — leaving most of the OS interpreted and
				 * JIT mode slower than pure interpreter mode.
				 * Cost: 2-4 compares per interpreted block. */
				if (bi->pc != pc() && ppc_jit_aarch64_is_compilable(pc()))
					break;
#endif

				if ((bi->pc != pc()) && ((bi = my_block_cache.find(pc())) == NULL))
					break;
			}
		}
#else
		goto do_interpret;
#endif
	}
#endif
  do_interpret:
	for (;;) {
		uint32 opcode = vm_read_memory_4(pc());
		const instr_info_t *ii = decode(opcode);
#if PPC_EXECUTE_DUMP_STATE
		if (dump_state)
			dump_instruction(opcode);
#endif
#if PPC_FLIGHT_RECORDER
		if (is_logging())
			record_step(opcode);
#endif
#ifdef __MINGW32__
		assert(ii->execute.default_call_conv_ptr() != 0);
#else
		assert(ii->execute.ptr() != 0);
#endif
		ii->execute(this, opcode);
#if PPC_EXECUTE_DUMP_STATE
		if (dump_state)
			dump_registers();
#endif
		if (!spcflags().empty() && !check_spcflags())
			goto return_site;
	}
  return_site:
	// Tell upper level we invalidated cache?
	if (invalidated_cache)
		spcflags().set(SPCFLAG_JIT_EXEC_RETURN);
	--execute_depth;
}

void powerpc_cpu::execute()
{
	execute(pc());
}

void powerpc_cpu::init_decode_cache()
{
#if PPC_DECODE_CACHE
	decode_cache = (block_info::decode_info *)vm_acquire(DECODE_CACHE_SIZE);
	if (decode_cache == VM_MAP_FAILED) {
		fprintf(stderr, "powerpc_cpu: Could not allocate decode cache\n");
		abort();
	}

	D(bug("powerpc_cpu: Allocated decode cache: %d KB at %p\n", DECODE_CACHE_SIZE / 1024, decode_cache));
	decode_cache_p = decode_cache;
	decode_cache_end_p = decode_cache + DECODE_CACHE_MAX_ENTRIES;
#if FLIGHT_RECORDER
	// Leave enough room to last call to record_step()
	decode_cache_end_p -= 2;
#endif
#if PPC_EXECUTE_DUMP_STATE
	// Leave enough room to last calls to dump state functions
	decode_cache_end_p -= 2;
#endif
#endif
}

void powerpc_cpu::kill_decode_cache()
{
#if PPC_DECODE_CACHE
	vm_release(decode_cache, DECODE_CACHE_SIZE);
#endif
}

void powerpc_cpu::invalidate_cache()
{
	D(bug("Invalidate all cache blocks\n"));
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
	my_block_cache.clear();
	my_block_cache.initialize();
	spcflags().set(SPCFLAG_JIT_EXEC_RETURN);
#endif
#if PPC_ENABLE_JIT
	codegen.invalidate_cache();
#endif
#if PPC_DECODE_CACHE
	decode_cache_p = decode_cache;
#endif
}

void powerpc_block_info::invalidate()
{
#if PPC_DECODE_CACHE
	// Don't do anything if this is a predecoded block
	if (di)
		return;
#endif
#if DYNGEN_DIRECT_BLOCK_CHAINING
	for (int i = 0; i < MAX_TARGETS; i++) {
		link_info * const tli = &li[i];
		uint32 tpc = tli->jmp_pc;
		// For any jump within page boundaries, reset the jump address
		// to the target block resolver (trampoline)
		if (tpc != INVALID_PC && ((tpc ^ pc) >> 12) == 0)
			dg_set_jmp_target(tli->jmp_addr, tli->jmp_resolve_addr);
	}
#endif
}

void powerpc_cpu::invalidate_cache_range(uintptr start, uintptr end)
{
	D(bug("Invalidate cache block [%08x - %08x]\n", start, end));
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
#if DYNGEN_DIRECT_BLOCK_CHAINING
	if (use_jit) {
		// Invalidate on page boundaries
		start &= -4096;
		end = (end + 4095) & -4096;
		D(bug("    at page boundaries [%08x - %08x]\n", start, end));
	}
#endif
	spcflags().set(SPCFLAG_JIT_EXEC_RETURN);
	my_block_cache.clear_range(start, end);
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	ppc_jit_aarch64_invalidate_range((uint32_t)start, (uint32_t)end);
#endif
#endif
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	/* Evict aarch64 JIT blocks whose start PC falls in the invalidated range.
	 * Mac OS signals code modification via icbi/isync (the interpreter's
	 * execute_icbi/execute_isync funnel here); without this, blocks compiled
	 * from pre-write memory would keep executing stale translations forever.
	 * Relevant when guest code is written at runtime (extension loading,
	 * relocated stubs, self-modifying application code). */
	ppc_jit_aarch64_invalidate_range((uint32_t)start, (uint32_t)end);
#endif
}
