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
 * syscall_entry = 0 (unresolved; descope active — see M3A-ENTRY-TABLE.md §5).
 * Override via SS_EXC_ENTRY=0xINT[,0xSC] for no-rebuild iteration.
 * Paravirtual never reads this table. */
#define NW_INTERRUPT_ENTRY_DEFAULT 0x50412b1cu  /* M3A-ENTRY-TABLE.md probe-verified */
ExcEntryTable g_exc_entry_table = { NW_INTERRUPT_ENTRY_DEFAULT, 0u };

/* M3a Task 4 telemetry: DEC delivery counters (newworld only — paravirtual never
 * runs the hook). CPU-thread-only writers (check_spcflags context, plan §2g), so
 * plain uint64_t is fine; readers (heartbeat, crash dump) run on the same thread
 * or post-mortem. Exposed via SheepExcStats(). */
static uint64_t exc_stat_delivered_dec  = 0;
static uint64_t exc_stat_deferred_ee    = 0;
static uint64_t exc_stat_deferred_depth = 0;
static uint64_t exc_stat_deferred_native = 0;	// M6a W2: deferred during native excursion ([XLM_RUN_MODE]!=0)

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
	if (MachineProfileIsNewWorld() && VirtClockDECPending(&g_virt_clock))
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
	if (!VirtClockDECPending(&g_virt_clock))
		return false;
	// Deliverability rule (MACHINE-LAYER-PLAN §2d): never deliver inside nested
	// executes — depth 1 means we are at the outermost execute().
	if (current_execute_depth() != 1) {
		exc_stat_deferred_depth++;
		return false;
	}
	if (!ExcDeliverable(msr_reg())) {
		exc_stat_deferred_ee++;
		return false;
	}
	// M6a rung-2 W2 DEC fence (plan rev 3.1 item 3; red-team blocking risk):
	// defer while a MixedMode NATIVE EXCURSION is in flight — [XLM_RUN_MODE]
	// (0x2810) is maintained 1-forward/0-backward by the NK on exactly the
	// FE01/FE02 switch pair (MODE_NATIVE=1 during PPC-native windows,
	// MODE_68K=0 while the 68k world runs).  Rationale: under the W2
	// [KDP+0x65c] world-flip discipline the word holds the MMCB during a
	// native window; the KDP register-save shim below saves r7-r13 through
	// r6=[KDP+0x65c] — a DEC delivered mid-excursion would save into the
	// MMCB whose slots the NK switch-back save is about to rewrite (ECB-
	// clobber by symmetry: pre-W2 it would have CLOBBERED the parked
	// emulator ctx in the ECB).  Same deferral semantics as the depth/EE
	// paths above: latch stays set, EE-edge re-raises + the block-boundary
	// poll pick it up once [0x2810] returns to 0.  Newworld-gated by
	// construction (this function only runs on the newworld profile).
	// Inert while no native excursions run ([0x2810]=0 ⇒ no behavior change).
	if (ReadMacInt32(XLM_RUN_MODE) != 0) {
		exc_stat_deferred_native++;
		return false;
	}
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

	if (!exc_bare) {
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
		        exc_bare ? " (BARE)" : "");
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
	if (MachineProfileIsNewWorld() && VirtClockDECPending(&g_virt_clock))
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
	if (MachineProfileIsNewWorld() && VirtClockDECPending(&g_virt_clock))
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
	if (MachineProfileIsNewWorld() && VirtClockDECPending(&g_virt_clock))
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

// M3a Task 4 telemetry export (heartbeat + crash-path dump).
// M6a W2: + out[3] = deferred_native (the native-excursion DEC fence).
extern "C" void SheepExcStats(uint64_t out[4])
{
	out[0] = exc_stat_delivered_dec;
	out[1] = exc_stat_deferred_ee;
	out[2] = exc_stat_deferred_depth;
	out[3] = exc_stat_deferred_native;
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
		uint64_t exc[4];
		SheepExcStats(exc);
		fprintf(stderr, "[EXC] delivered_dec=%llu deferred_ee=%llu deferred_depth=%llu "
		        "deferred_native=%llu\n",
		        (unsigned long long)exc[0], (unsigned long long)exc[1],
		        (unsigned long long)exc[2], (unsigned long long)exc[3]);
	}
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
		// M6a Wave 1 rev-2 finding 6 NOTE (do NOT change in Wave 1): [KDP+0x5a4]
		// (=0x5036d218, 68k code base, seeded above) and [KDP+0x5f0/+0x5f4]
		// (=0x50366080, primary EMUL_RETURN) are DORMANT cross-world constants —
		// PRIMARY-world values in a mirror-world boot, on routes probe-verified
		// never to execute today (the jump68k dispatch tail / patched entry).
		// Left intentionally so rung 2 (ongoing-entry contract) doesn't trip on a
		// silent Wave-1 change; reconcile them when their routes go live
		// (M6A-DR-HANDOFF-ANALYSIS.md §3 rows "[KDP+0x5a4]" / "[KDP+0x65c]…+0x5f0").
		WriteMacInt32(kdp + 0x5f0, (uint32)ROMBase + 0x366080);  // EMUL_RETURN handler
		WriteMacInt32(kdp + 0x5f4, (uint32)ROMBase + 0x366080);
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

		/* SS_SEED_MEM (immediate form): apply the no-PC seeds now — the natural
		 * "post-init" point, after the nanokernel trampoline has populated the KDP /
		 * ECB. The PC-triggered form fires later at its target block entry. */
		ss_seed_mem_apply_immediate();

		/* M3a Task 3: initialize the exception entry table for newworld.
		 * Default: interrupt_entry = 0x50412b1c (probe-verified, M3A-ENTRY-TABLE.md).
		 *          syscall_entry   = 0 (unresolved — descope active).
		 * Override: SS_EXC_ENTRY=0xINT[,0xSC] for no-rebuild iteration. */
		{
			const char *exc_env = getenv("SS_EXC_ENTRY");
			if (exc_env && exc_env[0]) {
				char *endp = NULL;
				uint32_t ie = (uint32_t)strtoul(exc_env, &endp, 0);
				uint32_t sc = 0;
				if (endp && *endp == ',')
					sc = (uint32_t)strtoul(endp + 1, NULL, 0);
				g_exc_entry_table.interrupt_entry = ie;
				g_exc_entry_table.syscall_entry   = sc;
				fprintf(stderr, "[EXC] entry table override (SS_EXC_ENTRY): "
				        "interrupt=0x%08x syscall=0x%08x\n",
				        g_exc_entry_table.interrupt_entry,
				        g_exc_entry_table.syscall_entry);
			} else {
				fprintf(stderr, "[EXC] entry table: interrupt=0x%08x syscall=0x%08x\n",
				        g_exc_entry_table.interrupt_entry,
				        g_exc_entry_table.syscall_entry);
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
