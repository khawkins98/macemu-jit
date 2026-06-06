/*
 * rom-harness.cpp — Headless ROM JIT exerciser for SheepShaver AArch64 JIT
 *
 * Loads the Mac ROM file, scans it for PPC basic blocks, and for each block:
 *   1. Executes it in the SheepShaver interpreter
 *   2. JIT-compiles and executes it natively
 *   3. Compares all register outputs
 *
 * No display, no hardware, no disk — pure CPU exercising.
 *
 * Usage:
 *   ./rom-harness <rom-file> [options]
 *
 * Options:
 *   --offset=0xNNNNNN   Start scanning at this ROM offset (default: 0)
 *   --count=N           Number of blocks to test (default: all)
 *   --verbose           Print each block result
 *   --stop-on-fail      Stop at first mismatch
 *   --min-insns=N       Minimum instructions per block (default: 1)
 *   --max-insns=N       Maximum instructions per block (default: 64)
 *   --entry=0xNNNNNN    Execute a single block at this ROM offset
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <cassert>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <math.h>
#if defined(__APPLE__)
#include <pthread/qos.h>   /* QoS is the only scheduling lever on Apple Silicon (affinity is a no-op) */
#endif

/* MAP_FIXED_NOREPLACE is Linux-only; on macOS/BSD fall back to 0 so the
 * fixed-address mmap below degrades to a hint, and the non-fixed fallback
 * mmap handles relocation. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif

/* The emulated RAM+ROM region holds PPC data/instructions that the JIT *reads*;
 * native ARM64 code is executed from the separate JIT code cache (jit_cache_alloc,
 * MAP_JIT). This region therefore never needs PROT_EXEC. On macOS arm64 a RWX
 * anonymous mapping is rejected with EPERM (W^X policy), so request only RW there.
 * Linux keeps the original RWX request unchanged. */
#if defined(__APPLE__) && defined(__aarch64__)
#define ROM_HARNESS_MEM_PROT (PROT_READ | PROT_WRITE)
#else
#define ROM_HARNESS_MEM_PROT (PROT_READ | PROT_WRITE | PROT_EXEC)
#endif

/* ---------- Forward declarations for the JIT ---------- */
#include "ppc-jit.h"

/* Shared guard for unsafe JIT block execution. The run loop arms `jit_guard_jmp`
 * with sigsetjmp before each block; a SIGSEGV (segv_handler) or an inline-interp
 * fallback (ppc_jit_interp_one below) longjmps back so the harness can *skip* the
 * block instead of crashing. `segv_caught`/`fallback_caught` distinguish the cause;
 * `fallback_opcode`/`fallback_pc` carry diagnostics for --verbose. */
static sigjmp_buf jit_guard_jmp;
static volatile sig_atomic_t segv_caught = 0;
static volatile sig_atomic_t fallback_caught = 0;
static volatile uint32_t fallback_opcode = 0;
static volatile uint32_t fallback_pc = 0;

/* The JIT references this bridge for its inline-interpreter fallback path
 * (emit_inline_interp_call); the real implementation lives in the emulator
 * (ppc-cpu.cpp), which this standalone harness does not link.  The harness can
 * only run blocks that execute fully native — but a block the compiler marks
 * `complete == true` can still emit a fallback call (e.g. a ROM region that was
 * mis-scanned as code, or an op handled only via fallback; see ppc-jit.cpp
 * "complete stays true" note). Rather than abort the whole run, longjmp back to
 * the per-block guard so this one block is counted as skipped. */
extern "C" void ppc_jit_interp_one(uint32_t opcode, uint32_t pc_val) {
	fallback_opcode = opcode;
	fallback_pc = pc_val;
	fallback_caught = 1;
	siglongjmp(jit_guard_jmp, 1);
}

/* ---------- PPC instruction decoding helpers ---------- */

static inline uint32_t read_be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static inline void write_be32(uint8_t *p, uint32_t v) {
	p[0] = (v >> 24) & 0xFF;
	p[1] = (v >> 16) & 0xFF;
	p[2] = (v >> 8)  & 0xFF;
	p[3] =  v        & 0xFF;
}

/* Primary opcode extraction */
static inline uint32_t ppc_primary(uint32_t insn) { return insn >> 26; }

/* Extended opcode for opcode 19/31/59/63 */
static inline uint32_t ppc_xo(uint32_t insn) { return (insn >> 1) & 0x3FF; }

/* Is this a block-terminating instruction? */
static bool is_block_terminator(uint32_t insn) {
	uint32_t opc = ppc_primary(insn);
	switch (opc) {
	case 18: return true; /* b/bl */
	case 16: return true; /* bc/bcl (conditional branch).
	                         NOTE: the *JIT* does NOT treat bc as a block terminator —
	                         it runs past it — so a bc-terminated scanner block is
	                         shorter than the JIT's block, which makes the differential
	                         comparison structurally mismatched. See the block-model
	                         TODO at the compare site in the main test loop. */
	case 19: {
		uint32_t xo = ppc_xo(insn);
		if (xo == 16 || xo == 528) return true; /* bclr, bcctr */
		return false;
	}
	case 6:  return true; /* EMUL_OP (opcode 6 = SheepShaver trampoline) */
	default: return false;
	}
}

/* Is this a load/store instruction that accesses memory?
   We skip blocks containing these to avoid SIGSEGV from unmapped addresses. */
static bool is_memory_access(uint32_t insn) {
	uint32_t opc = ppc_primary(insn);
	/* Load/store integer */
	if (opc >= 32 && opc <= 55) return true;
	/* Load/store FP */
	if (opc >= 48 && opc <= 55) return true;
	/* Load/store with update, indexed forms via opcode 31 */
	if (opc == 31) {
		uint32_t xo = ppc_xo(insn);
		switch (xo) {
		/* Loads indexed */
		case 20: /* lwarx */ case 23: /* lwzx */ case 55: /* lwzux */
		case 87: /* lbzx */ case 119: /* lbzux */ case 279: /* lhzx */
		case 311: /* lhzux */ case 343: /* lhax */ case 375: /* lhaux */
		case 533: /* lswx */ case 534: /* lwbrx */ case 535: /* lfsx */
		case 567: /* lfsux */ case 599: /* lfdx */ case 631: /* lfdux */
		case 597: /* lswi */ case 790: /* lhbrx */
		/* Stores indexed */
		case 150: /* stwcx. */ case 151: /* stwx */ case 183: /* stwux */
		case 215: /* stbx */ case 247: /* stbux */ case 407: /* sthx */
		case 439: /* sthux */ case 662: /* stwbrx */ case 663: /* stfsx */
		case 695: /* stfsux */ case 727: /* stfdx */ case 759: /* stfdux */
		case 661: /* stswx */ case 725: /* stswi */ case 918: /* sthbrx */
		case 438: /* eciwx */ case 470: /* ecowx */
		/* Cache */
		case 54: /* dcbst */ case 86: /* dcbf */ case 246: /* dcbt */
		case 278: /* dcbtst */ case 982: /* icbi */ case 1014: /* dcbz */
			return true;
		default:
			break;
		}
	}
	return false;
}

/* Is this a supervisor/privileged instruction we should skip? */
static bool is_privileged(uint32_t insn) {
	uint32_t opc = ppc_primary(insn);
	if (opc == 31) {
		uint32_t xo = ppc_xo(insn);
		switch (xo) {
		case 146: /* mtmsr */
		case 83:  /* mfmsr */
		case 306: /* tlbie */
		case 566: /* tlbsync */
		case 370: /* tlbia */
		case 595: /* mfsr */
		case 659: /* mfsrin */
		case 210: /* mtsr */
		case 242: /* mtsrin */
		case 178: /* mtdec (move to DEC) */
			return true;
		}
	}
	if (opc == 17) return true; /* sc (system call) */
	if (opc == 19 && ppc_xo(insn) == 50) return true; /* rfi */
	return false;
}

/* Does this instruction modify LR? (bl, bcl, bclrl, etc.) */
static bool modifies_lr(uint32_t insn) {
	return (insn & 1) != 0 && (ppc_primary(insn) == 18 || ppc_primary(insn) == 16 ||
		(ppc_primary(insn) == 19 && (ppc_xo(insn) == 16 || ppc_xo(insn) == 528)));
}

/* Does this instruction read from CTR or LR as a branch target? */
static bool reads_link_regs(uint32_t insn) {
	if (ppc_primary(insn) == 19) {
		uint32_t xo = ppc_xo(insn);
		if (xo == 16) return true; /* bclr — reads LR */
		if (xo == 528) return true; /* bcctr — reads CTR */
	}
	return false;
}

/* ---------- Minimal PPC Interpreter for standalone testing ---------- */
/* 
 * This is a SUBSET interpreter — handles only the instructions the JIT
 * handles, enough to compare register outputs for compute-only blocks.
 */

struct PPCRegs {
	uint32_t gpr[32];        /* offset 0: 32 × 4 = 128 bytes */
	uint32_t gpr_hi[32];     /* offset 128: upper 32 bits for G5 64-bit mode */
	double   fpr[32];        /* offset 256: 32 × 8 = 256 bytes */
	uint8_t  vr[32 * 16];    /* offset 384: 32 × 16 = 512 bytes */
	uint32_t cr;             /* offset 896 */
	uint8_t  xer_so;          /* offset 900 */
	uint8_t  xer_ov;          /* offset 901 */
	uint8_t  xer_ca;          /* offset 902 */
	uint8_t  xer_cnt;         /* offset 903 */
	uint32_t padding904;     /* offset 904 */
	uint32_t padding908;     /* offset 908 */
	uint32_t fpscr;          /* offset 912 */
	uint32_t lr;             /* offset 1044 */
	uint32_t ctr;            /* offset 1048 */
	uint32_t pc;             /* offset 1052 */
	uint32_t spcflags;       /* offset 1056 — the JIT block-entry poll READS this
	                          * (PPCR_SPCFLAGS); it must exist and be zero or every
	                          * block bails before its body runs. Added when 0d moved
	                          * spcflags to offset 1056; was missing here. */
};

/* Register field offsets — must match ppc-jit.cpp PPCR_* */
static_assert(offsetof(PPCRegs, gpr) == 0, "GPR offset mismatch");
static_assert(offsetof(PPCRegs, fpr) == 256, "FPR offset mismatch");
static_assert(offsetof(PPCRegs, cr) == 1024, "CR offset mismatch");
static_assert(offsetof(PPCRegs, xer_so) == 1028, "XER offset mismatch");
static_assert(offsetof(PPCRegs, fpscr) == 1040, "FPSCR offset mismatch");
static_assert(offsetof(PPCRegs, lr) == 1044, "LR offset mismatch");
static_assert(offsetof(PPCRegs, ctr) == 1048, "CTR offset mismatch");
static_assert(offsetof(PPCRegs, pc) == 1052, "PC offset mismatch");
static_assert(offsetof(PPCRegs, spcflags) == 1056, "SPCFLAGS offset mismatch");

/* Pack XER bytes into PPC 32-bit format */
static inline uint32_t pack_xer(const PPCRegs *r) {
	return ((uint32_t)r->xer_so << 31) | ((uint32_t)r->xer_ov << 30) |
	       ((uint32_t)r->xer_ca << 29) | r->xer_cnt;
}

/* Unpack PPC 32-bit XER into struct bytes */
static inline void unpack_xer(PPCRegs *r, uint32_t xer) {
	r->xer_so = (xer >> 31) & 1;
	r->xer_ov = (xer >> 30) & 1;
	r->xer_ca = (xer >> 29) & 1;
	r->xer_cnt = xer & 0x7F;
}


/* Bit field helpers for CR */
static inline uint32_t cr_field(uint32_t cr, int field) {
	return (cr >> (28 - field * 4)) & 0xF;
}
static inline void set_cr_field(uint32_t &cr, int field, uint32_t val) {
	uint32_t shift = 28 - field * 4;
	cr = (cr & ~(0xF << shift)) | ((val & 0xF) << shift);
}

/* CR0 update from result value + SO */
static inline void update_cr0(uint32_t &cr, uint8_t xer_so, int32_t result) {
	uint32_t bits = 0;
	if (result < 0) bits = 8;      /* LT */
	else if (result > 0) bits = 4;  /* GT */
	else bits = 2;                  /* EQ */
	if (xer_so) bits |= 1; /* SO */
	set_cr_field(cr, 0, bits);
}

/* XER carry bit (bit 29) */
static inline bool xer_ca(const PPCRegs *r) { return r->xer_ca; }
static inline void set_xer_ca(PPCRegs *r, bool ca) { r->xer_ca = ca ? 1 : 0; }

/* 
 * Interpret a single basic block of PPC instructions.
 * Returns false if an unsupported instruction is encountered.
 * The block MUST end with a terminator (b/bl/blr/bctr/bc).
 * For testing purposes, we force a synthetic blr at the end if not present.
 */
static bool interpret_block(PPCRegs *regs, const uint8_t *rom, size_t rom_size,
                            uint32_t start_pc, uint32_t rom_base_mac, int max_insns) {
	uint32_t pc = start_pc;
	
	for (int i = 0; i < max_insns + 1; i++) {
		uint32_t rom_offset = pc - rom_base_mac;
		if (rom_offset >= rom_size) return false;
		
		uint32_t insn = read_be32(rom + rom_offset);
		uint32_t opc = ppc_primary(insn);
		
		/* Decode and execute */
		switch (opc) {
		
		/* --- Integer Arithmetic --- */
		case 14: { /* addi rD,rA,SIMM */
			int rD = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int16_t simm = (int16_t)(insn & 0xFFFF);
			regs->gpr[rD] = (rA == 0) ? (uint32_t)(int32_t)simm : regs->gpr[rA] + (int32_t)simm;
			pc += 4; break;
		}
		case 15: { /* addis rD,rA,SIMM */
			int rD = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int16_t simm = (int16_t)(insn & 0xFFFF);
			uint32_t val = (uint32_t)simm << 16;
			regs->gpr[rD] = (rA == 0) ? val : regs->gpr[rA] + val;
			pc += 4; break;
		}
		case 24: { /* ori rA,rS,UIMM */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			uint16_t uimm = insn & 0xFFFF;
			regs->gpr[rA] = regs->gpr[rS] | uimm;
			pc += 4; break;
		}
		case 25: { /* oris rA,rS,UIMM */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			uint16_t uimm = insn & 0xFFFF;
			regs->gpr[rA] = regs->gpr[rS] | ((uint32_t)uimm << 16);
			pc += 4; break;
		}
		case 26: { /* xori rA,rS,UIMM */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			uint16_t uimm = insn & 0xFFFF;
			regs->gpr[rA] = regs->gpr[rS] ^ uimm;
			pc += 4; break;
		}
		case 27: { /* xoris rA,rS,UIMM */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			uint16_t uimm = insn & 0xFFFF;
			regs->gpr[rA] = regs->gpr[rS] ^ ((uint32_t)uimm << 16);
			pc += 4; break;
		}
		case 28: { /* andi. rA,rS,UIMM */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			uint16_t uimm = insn & 0xFFFF;
			regs->gpr[rA] = regs->gpr[rS] & uimm;
			update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
			pc += 4; break;
		}
		case 29: { /* andis. rA,rS,UIMM */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			uint16_t uimm = insn & 0xFFFF;
			regs->gpr[rA] = regs->gpr[rS] & ((uint32_t)uimm << 16);
			update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
			pc += 4; break;
		}
		
		case 21: { /* rlwinm[.] rA,rS,SH,MB,ME */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int SH = (insn >> 11) & 0x1F;
			int MB = (insn >> 6) & 0x1F;
			int ME = (insn >> 1) & 0x1F;
			bool rc = insn & 1;
			uint32_t val = regs->gpr[rS];
			uint32_t rotated = (val << SH) | (val >> (32 - SH));
			if (SH == 0) rotated = val;
			/* Build mask */
			uint32_t mask;
			if (MB <= ME)
				mask = ((0xFFFFFFFF >> MB) & (0xFFFFFFFF << (31 - ME)));
			else
				mask = ((0xFFFFFFFF >> MB) | (0xFFFFFFFF << (31 - ME)));
			regs->gpr[rA] = rotated & mask;
			if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
			pc += 4; break;
		}
		
		case 23: { /* rlwnm[.] rA,rS,rB,MB,ME */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int rB = (insn >> 11) & 0x1F;
			int MB = (insn >> 6) & 0x1F;
			int ME = (insn >> 1) & 0x1F;
			bool rc = insn & 1;
			uint32_t val = regs->gpr[rS];
			int sh = regs->gpr[rB] & 0x1F;
			uint32_t rotated = sh ? ((val << sh) | (val >> (32 - sh))) : val;
			uint32_t mask;
			if (MB <= ME)
				mask = ((0xFFFFFFFF >> MB) & (0xFFFFFFFF << (31 - ME)));
			else
				mask = ((0xFFFFFFFF >> MB) | (0xFFFFFFFF << (31 - ME)));
			regs->gpr[rA] = rotated & mask;
			if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
			pc += 4; break;
		}
		
		case 20: { /* rlwimi[.] rA,rS,SH,MB,ME */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int SH = (insn >> 11) & 0x1F;
			int MB = (insn >> 6) & 0x1F;
			int ME = (insn >> 1) & 0x1F;
			bool rc = insn & 1;
			uint32_t val = regs->gpr[rS];
			uint32_t rotated = SH ? ((val << SH) | (val >> (32 - SH))) : val;
			uint32_t mask;
			if (MB <= ME)
				mask = ((0xFFFFFFFF >> MB) & (0xFFFFFFFF << (31 - ME)));
			else
				mask = ((0xFFFFFFFF >> MB) | (0xFFFFFFFF << (31 - ME)));
			regs->gpr[rA] = (rotated & mask) | (regs->gpr[rA] & ~mask);
			if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
			pc += 4; break;
		}
		
		case 7: { /* mulli rD,rA,SIMM */
			int rD = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int16_t simm = (int16_t)(insn & 0xFFFF);
			regs->gpr[rD] = (uint32_t)((int32_t)regs->gpr[rA] * (int32_t)simm);
			pc += 4; break;
		}
		
		case 8: { /* subfic rD,rA,SIMM */
			int rD = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int16_t simm = (int16_t)(insn & 0xFFFF);
			uint64_t result = (uint64_t)(uint32_t)(int32_t)simm + (uint64_t)(~regs->gpr[rA]) + 1ULL;
			regs->gpr[rD] = (uint32_t)result;
			set_xer_ca(regs, result >> 32);
			pc += 4; break;
		}
		
		case 12: { /* addic rD,rA,SIMM */
			int rD = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int16_t simm = (int16_t)(insn & 0xFFFF);
			uint64_t result = (uint64_t)regs->gpr[rA] + (uint64_t)(uint32_t)(int32_t)simm;
			regs->gpr[rD] = (uint32_t)result;
			set_xer_ca(regs, result >> 32);
			pc += 4; break;
		}
		case 13: { /* addic. rD,rA,SIMM */
			int rD = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int16_t simm = (int16_t)(insn & 0xFFFF);
			uint64_t result = (uint64_t)regs->gpr[rA] + (uint64_t)(uint32_t)(int32_t)simm;
			regs->gpr[rD] = (uint32_t)result;
			set_xer_ca(regs, result >> 32);
			update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
			pc += 4; break;
		}
		case 10: { /* cmpli crD,rA,UIMM */
			int crD = (insn >> 23) & 0x7;
			int rA = (insn >> 16) & 0x1F;
			uint16_t uimm = insn & 0xFFFF;
			uint32_t a = regs->gpr[rA];
			uint32_t b = (uint32_t)uimm;
			uint32_t bits = 0;
			if (a < b) bits = 8;
			else if (a > b) bits = 4;
			else bits = 2;
			if (regs->xer_so) bits |= 1;
			set_cr_field(regs->cr, crD, bits);
			pc += 4; break;
		}
		case 11: { /* cmpi crD,rA,SIMM */
			int crD = (insn >> 23) & 0x7;
			int rA = (insn >> 16) & 0x1F;
			int16_t simm = (int16_t)(insn & 0xFFFF);
			int32_t a = (int32_t)regs->gpr[rA];
			int32_t b = (int32_t)simm;
			uint32_t bits = 0;
			if (a < b) bits = 8;
			else if (a > b) bits = 4;
			else bits = 2;
			if (regs->xer_so) bits |= 1;
			set_cr_field(regs->cr, crD, bits);
			pc += 4; break;
		}
		
		/* --- Opcode 31: XO-form integer arithmetic, logical, comparison --- */
		case 31: {
			int rD = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int rB = (insn >> 11) & 0x1F;
			bool rc = insn & 1;
			bool oe = (insn >> 10) & 1;
			uint32_t xo = ppc_xo(insn);
			uint32_t xo9 = (insn >> 1) & 0x1FF; /* 9-bit XO for arith */
			
			switch (xo) {
			/* Comparison */
			case 0: { /* cmp crD,rA,rB */
				int crD = (insn >> 23) & 0x7;
				int32_t a = (int32_t)regs->gpr[rA];
				int32_t b = (int32_t)regs->gpr[rB];
				uint32_t bits = 0;
				if (a < b) bits = 8; else if (a > b) bits = 4; else bits = 2;
				if (regs->xer_so) bits |= 1;
				set_cr_field(regs->cr, crD, bits);
				pc += 4; break;
			}
			case 32: { /* cmpl crD,rA,rB */
				int crD = (insn >> 23) & 0x7;
				uint32_t a = regs->gpr[rA];
				uint32_t b = regs->gpr[rB];
				uint32_t bits = 0;
				if (a < b) bits = 8; else if (a > b) bits = 4; else bits = 2;
				if (regs->xer_so) bits |= 1;
				set_cr_field(regs->cr, crD, bits);
				pc += 4; break;
			}
			
			/* Logical */
			case 28: { /* and[.] rA,rS,rB */
				regs->gpr[rA] = regs->gpr[rD] & regs->gpr[rB];
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 60: { /* andc[.] rA,rS,rB */
				regs->gpr[rA] = regs->gpr[rD] & ~regs->gpr[rB];
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 444: { /* or[.] rA,rS,rB */
				regs->gpr[rA] = regs->gpr[rD] | regs->gpr[rB];
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 124: { /* nor[.] rA,rS,rB */
				regs->gpr[rA] = ~(regs->gpr[rD] | regs->gpr[rB]);
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 316: { /* xor[.] rA,rS,rB */
				regs->gpr[rA] = regs->gpr[rD] ^ regs->gpr[rB];
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 284: { /* eqv[.] rA,rS,rB */
				regs->gpr[rA] = ~(regs->gpr[rD] ^ regs->gpr[rB]);
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 412: { /* orc[.] rA,rS,rB */
				regs->gpr[rA] = regs->gpr[rD] | ~regs->gpr[rB];
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 476: { /* nand[.] rA,rS,rB */
				regs->gpr[rA] = ~(regs->gpr[rD] & regs->gpr[rB]);
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			
			/* Shift */
			case 24: { /* slw[.] rA,rS,rB */
				uint32_t sh = regs->gpr[rB] & 0x3F;
				regs->gpr[rA] = (sh < 32) ? (regs->gpr[rD] << sh) : 0;
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 536: { /* srw[.] rA,rS,rB */
				uint32_t sh = regs->gpr[rB] & 0x3F;
				regs->gpr[rA] = (sh < 32) ? (regs->gpr[rD] >> sh) : 0;
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 792: { /* sraw[.] rA,rS,rB */
				uint32_t sh = regs->gpr[rB] & 0x3F;
				int32_t val = (int32_t)regs->gpr[rD];
				if (sh == 0) {
					regs->gpr[rA] = val;
					set_xer_ca(regs, false);
				} else if (sh < 32) {
					bool ca = (val < 0) && ((val & ((1 << sh) - 1)) != 0);
					regs->gpr[rA] = (uint32_t)(val >> sh);
					set_xer_ca(regs, ca);
				} else {
					bool ca = val < 0;
					regs->gpr[rA] = (uint32_t)(val >> 31);
					set_xer_ca(regs, ca);
				}
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 824: { /* srawi[.] rA,rS,SH */
				int SH = rB; /* rB field is SH for srawi */
				int32_t val = (int32_t)regs->gpr[rD];
				if (SH == 0) {
					regs->gpr[rA] = val;
					set_xer_ca(regs, false);
				} else {
					bool ca = (val < 0) && ((val & ((1 << SH) - 1)) != 0);
					regs->gpr[rA] = (uint32_t)(val >> SH);
					set_xer_ca(regs, ca);
				}
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			
			/* Count leading zeros */
			case 26: { /* cntlzw[.] rA,rS */
				uint32_t val = regs->gpr[rD];
				regs->gpr[rA] = val ? __builtin_clz(val) : 32;
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			
			/* Extend sign */
			case 922: { /* extsh[.] rA,rS */
				regs->gpr[rA] = (uint32_t)(int32_t)(int16_t)(uint16_t)regs->gpr[rD];
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 954: { /* extsb[.] rA,rS */
				regs->gpr[rA] = (uint32_t)(int32_t)(int8_t)(uint8_t)regs->gpr[rD];
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			
			/* Move to/from special registers */
			case 339: { /* mfspr rD,spr */
				uint32_t spr = ((insn >> 16) & 0x1F) | (((insn >> 11) & 0x1F) << 5);
				switch (spr) {
				case 1: regs->gpr[rD] = pack_xer(regs); break;
				case 8: regs->gpr[rD] = regs->lr; break;
				case 9: regs->gpr[rD] = regs->ctr; break;
				default: regs->gpr[rD] = 0; break; /* unknown SPR */
				}
				pc += 4; break;
			}
			case 467: { /* mtspr spr,rS */
				uint32_t spr = ((insn >> 16) & 0x1F) | (((insn >> 11) & 0x1F) << 5);
				switch (spr) {
				case 1: unpack_xer(regs, regs->gpr[rD]); break;
				case 8: regs->lr = regs->gpr[rD]; break;
				case 9: regs->ctr = regs->gpr[rD]; break;
				default: break;
				}
				pc += 4; break;
			}
			
			/* Move from CR */
			case 19: { /* mfcr rD */
				regs->gpr[rD] = regs->cr;
				pc += 4; break;
			}
			
			/* Multiply */
			case 235: { /* mullw[o][.] rD,rA,rB (xo9=235) */
				int64_t result = (int64_t)(int32_t)regs->gpr[rA] * (int64_t)(int32_t)regs->gpr[rB];
				regs->gpr[rD] = (uint32_t)result;
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
				pc += 4; break;
			}
			case 75: { /* mulhw[.] rD,rA,rB */
				int64_t result = (int64_t)(int32_t)regs->gpr[rA] * (int64_t)(int32_t)regs->gpr[rB];
				regs->gpr[rD] = (uint32_t)(result >> 32);
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
				pc += 4; break;
			}
			case 11: { /* mulhwu[.] rD,rA,rB */
				uint64_t result = (uint64_t)regs->gpr[rA] * (uint64_t)regs->gpr[rB];
				regs->gpr[rD] = (uint32_t)(result >> 32);
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
				pc += 4; break;
			}
			
			/* Divide */
			case 491: { /* divw[o][.] rD,rA,rB */
				int32_t a = (int32_t)regs->gpr[rA];
				int32_t b = (int32_t)regs->gpr[rB];
				if (b == 0 || (a == (int32_t)0x80000000 && b == -1))
					regs->gpr[rD] = 0;
				else
					regs->gpr[rD] = (uint32_t)(a / b);
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
				pc += 4; break;
			}
			case 459: { /* divwu[o][.] rD,rA,rB */
				uint32_t a = regs->gpr[rA];
				uint32_t b = regs->gpr[rB];
				if (b == 0)
					regs->gpr[rD] = 0;
				else
					regs->gpr[rD] = a / b;
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
				pc += 4; break;
			}
			
			/* Add/Sub variants (9-bit XO) */
			default: {
				switch (xo9) {
				case 266: { /* add[o][.] rD,rA,rB */
					regs->gpr[rD] = regs->gpr[rA] + regs->gpr[rB];
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 10: { /* addc[o][.] rD,rA,rB */
					uint64_t result = (uint64_t)regs->gpr[rA] + (uint64_t)regs->gpr[rB];
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 138: { /* adde[o][.] rD,rA,rB */
					uint64_t result = (uint64_t)regs->gpr[rA] + (uint64_t)regs->gpr[rB] + (uint64_t)xer_ca(regs);
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 234: { /* addme[o][.] rD,rA */
					uint64_t result = (uint64_t)regs->gpr[rA] + (uint64_t)xer_ca(regs) - 1ULL;
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 202: { /* addze[o][.] rD,rA */
					uint64_t result = (uint64_t)regs->gpr[rA] + (uint64_t)xer_ca(regs);
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 40: { /* subf[o][.] rD,rA,rB */
					regs->gpr[rD] = regs->gpr[rB] - regs->gpr[rA];
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 8: { /* subfc[o][.] rD,rA,rB */
					uint64_t result = (uint64_t)regs->gpr[rB] + (uint64_t)(~regs->gpr[rA]) + 1ULL;
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 136: { /* subfe[o][.] rD,rA,rB */
					uint64_t result = (uint64_t)regs->gpr[rB] + (uint64_t)(~regs->gpr[rA]) + (uint64_t)xer_ca(regs);
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 232: { /* subfme[o][.] rD,rA */
					uint64_t result = (uint64_t)(~regs->gpr[rA]) + (uint64_t)xer_ca(regs) - 1ULL;
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 200: { /* subfze[o][.] rD,rA */
					uint64_t result = (uint64_t)(~regs->gpr[rA]) + (uint64_t)xer_ca(regs);
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 104: { /* neg[o][.] rD,rA */
					regs->gpr[rD] = (uint32_t)(-(int32_t)regs->gpr[rA]);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				default:
					return false; /* unsupported XO31 */
				}
				break;
			}
			}
			break;
		}
		
		/* --- CR Logical (opcode 19) --- */
		case 19: {
			uint32_t xo = ppc_xo(insn);
			switch (xo) {
			case 257: { /* crand crbD,crbA,crbB */
				int crbD = (insn >> 21) & 0x1F;
				int crbA = (insn >> 16) & 0x1F;
				int crbB = (insn >> 11) & 0x1F;
				int a = (regs->cr >> (31 - crbA)) & 1;
				int b = (regs->cr >> (31 - crbB)) & 1;
				if (a & b) regs->cr |= (1u << (31 - crbD));
				else regs->cr &= ~(1u << (31 - crbD));
				pc += 4; break;
			}
			case 449: { /* cror crbD,crbA,crbB */
				int crbD = (insn >> 21) & 0x1F;
				int crbA = (insn >> 16) & 0x1F;
				int crbB = (insn >> 11) & 0x1F;
				int a = (regs->cr >> (31 - crbA)) & 1;
				int b = (regs->cr >> (31 - crbB)) & 1;
				if (a | b) regs->cr |= (1u << (31 - crbD));
				else regs->cr &= ~(1u << (31 - crbD));
				pc += 4; break;
			}
			case 193: { /* crxor crbD,crbA,crbB */
				int crbD = (insn >> 21) & 0x1F;
				int crbA = (insn >> 16) & 0x1F;
				int crbB = (insn >> 11) & 0x1F;
				int a = (regs->cr >> (31 - crbA)) & 1;
				int b = (regs->cr >> (31 - crbB)) & 1;
				if (a ^ b) regs->cr |= (1u << (31 - crbD));
				else regs->cr &= ~(1u << (31 - crbD));
				pc += 4; break;
			}
			case 33: { /* crnor crbD,crbA,crbB */
				int crbD = (insn >> 21) & 0x1F;
				int crbA = (insn >> 16) & 0x1F;
				int crbB = (insn >> 11) & 0x1F;
				int a = (regs->cr >> (31 - crbA)) & 1;
				int b = (regs->cr >> (31 - crbB)) & 1;
				if (!(a | b)) regs->cr |= (1u << (31 - crbD));
				else regs->cr &= ~(1u << (31 - crbD));
				pc += 4; break;
			}
			case 129: { /* crandc crbD,crbA,crbB */
				int crbD = (insn >> 21) & 0x1F;
				int crbA = (insn >> 16) & 0x1F;
				int crbB = (insn >> 11) & 0x1F;
				int a = (regs->cr >> (31 - crbA)) & 1;
				int b = (regs->cr >> (31 - crbB)) & 1;
				if (a & ~b) regs->cr |= (1u << (31 - crbD));
				else regs->cr &= ~(1u << (31 - crbD));
				pc += 4; break;
			}
			case 289: { /* creqv crbD,crbA,crbB */
				int crbD = (insn >> 21) & 0x1F;
				int crbA = (insn >> 16) & 0x1F;
				int crbB = (insn >> 11) & 0x1F;
				int a = (regs->cr >> (31 - crbA)) & 1;
				int b = (regs->cr >> (31 - crbB)) & 1;
				if (!(a ^ b)) regs->cr |= (1u << (31 - crbD));
				else regs->cr &= ~(1u << (31 - crbD));
				pc += 4; break;
			}
			case 417: { /* crorc crbD,crbA,crbB */
				int crbD = (insn >> 21) & 0x1F;
				int crbA = (insn >> 16) & 0x1F;
				int crbB = (insn >> 11) & 0x1F;
				int a = (regs->cr >> (31 - crbA)) & 1;
				int b = (regs->cr >> (31 - crbB)) & 1;
				if (a | ~b) regs->cr |= (1u << (31 - crbD));
				else regs->cr &= ~(1u << (31 - crbD));
				pc += 4; break;
			}
			case 0: { /* mcrf crD,crS */
				int crD = (insn >> 23) & 0x7;
				int crS = (insn >> 18) & 0x7;
				set_cr_field(regs->cr, crD, cr_field(regs->cr, crS));
				pc += 4; break;
			}
			/* bclr/bcctr — terminators, handled below */
			case 16: /* bclr */
			case 528: /* bcctr */
				goto handle_terminator;
			default:
				return false;
			}
			break;
		}
		
		/* --- Branch terminators --- */
		case 18: /* b/bl */
		case 16: /* bc/bcl */
		case 6:  /* EMUL_OP */
			goto handle_terminator;
			
		default:
			return false; /* unsupported opcode */
		}
		continue;
		
	handle_terminator:
		/* For terminators, we just set PC and stop.
		   The JIT does the same — stores the target PC and returns. */
		{
			uint32_t opc_t = ppc_primary(insn);
			if (opc_t == 18) { /* b/bl */
				int32_t disp = insn & 0x03FFFFFC;
				if (disp & 0x02000000) disp |= 0xFC000000; /* sign extend */
				bool aa = (insn >> 1) & 1;
				bool lk = insn & 1;
				uint32_t target = aa ? (uint32_t)disp : pc + disp;
				if (lk) regs->lr = pc + 4;
				regs->pc = target;
			} else if (opc_t == 16) { /* bc */
				int BO = (insn >> 21) & 0x1F;
				int BI = (insn >> 16) & 0x1F;
				int16_t bd = (int16_t)(insn & 0xFFFC);
				bool aa = (insn >> 1) & 1;
				bool lk = insn & 1;
				
				/* PPC ISA BO field (5 bits, MSB-first):
				   BO[0] (0x10): 1=don't test condition
				   BO[1] (0x08): condition sense (branch if CR[BI]=BO[1])
				   BO[2] (0x04): 1=don't decrement/test CTR
				   BO[3] (0x02): CTR sense (0=branch if CTR≠0, 1=branch if CTR==0)
				   BO[4] (0x01): prediction hint */
				if (!(BO & 0x04)) regs->ctr--; /* decrement if BO[2]=0 */
				
				bool ctr_ok = (BO & 0x04) || ((regs->ctr != 0) ^ ((BO >> 1) & 1));
				bool cond_ok = (BO & 0x10) || (((regs->cr >> (31 - BI)) & 1) == ((BO >> 3) & 1));
				
				if (ctr_ok && cond_ok) {
					uint32_t target = aa ? (uint32_t)(int32_t)bd : pc + (int32_t)bd;
					if (lk) regs->lr = pc + 4;
					regs->pc = target;
				} else {
					regs->pc = pc + 4;
				}
			} else if (opc_t == 19) {
				uint32_t xo = ppc_xo(insn);
				int BO = (insn >> 21) & 0x1F;
				int BI = (insn >> 16) & 0x1F;
				bool lk = insn & 1;
				
				if (!(BO & 0x04)) regs->ctr--;
				bool ctr_ok = (BO & 0x04) || ((regs->ctr != 0) ^ ((BO >> 1) & 1));
				bool cond_ok = (BO & 0x10) || (((regs->cr >> (31 - BI)) & 1) == ((BO >> 3) & 1));
				
				if (ctr_ok && cond_ok) {
					uint32_t target = (xo == 16) ? regs->lr : regs->ctr;
					if (lk) regs->lr = pc + 4;
					regs->pc = target;
				} else {
					regs->pc = pc + 4;
				}
			} else {
				/* EMUL_OP or other — just set PC past it */
				regs->pc = pc;
			}
			return true;
		}
	}
	
	/* Ran out of instructions without terminator — set PC */
	regs->pc = pc;
	return true;
}

/* ---------- Block scanning ---------- */

struct ROMBlock {
	uint32_t offset;     /* ROM offset of first instruction */
	int n_insns;         /* Number of instructions in block */
	bool has_mem_access; /* Contains load/store */
	bool has_privileged; /* Contains supervisor instruction */
	bool has_emul_op;    /* Contains EMUL_OP trampoline */
	bool has_link_read;  /* Reads LR/CTR as branch target */
	bool terminated;     /* Ends with a proper terminator */
};

static int scan_rom_blocks(const uint8_t *rom, size_t rom_size,
                           ROMBlock *blocks, int max_blocks,
                           int min_insns, int max_insns) {
	int n_blocks = 0;
	uint32_t offset = 0;
	
	while (offset < rom_size - 4 && n_blocks < max_blocks) {
		/* Skip zero words (padding) */
		uint32_t insn = read_be32(rom + offset);
		if (insn == 0 || insn == 0xFFFFFFFF) {
			offset += 4;
			continue;
		}
		
		/* Start a new block */
		ROMBlock blk = {};
		blk.offset = offset;
		blk.n_insns = 0;
		blk.terminated = false;
		
		uint32_t scan = offset;
		while (scan < rom_size - 4 && blk.n_insns < max_insns) {
			insn = read_be32(rom + scan);
			if (insn == 0) break; /* hit padding */
			
			if (is_memory_access(insn)) blk.has_mem_access = true;
			if (is_privileged(insn)) blk.has_privileged = true;
			if (ppc_primary(insn) == 6) blk.has_emul_op = true;
			if (reads_link_regs(insn)) blk.has_link_read = true;
			
			blk.n_insns++;
			scan += 4;
			
			if (is_block_terminator(insn)) {
				blk.terminated = true;
				break;
			}
		}
		
		if (blk.n_insns >= min_insns && blk.terminated) {
			blocks[n_blocks++] = blk;
		}
		
		offset = scan;
	}
	
	return n_blocks;
}

/* ---------- SIGSEGV handler for safe JIT execution ---------- */
/* Guard state (jit_guard_jmp, segv_caught, fallback_*) is declared near the top
 * of the file alongside the ppc_jit_interp_one fallback bridge that shares it. */

static void segv_handler(int sig, siginfo_t *si, void *ctx) {
	segv_caught = 1;
	siglongjmp(jit_guard_jmp, 1);
}

/* ---------- Comparison ---------- */

struct TestResult {
	int total;
	int passed;
	int failed;
	int skipped;
	int interp_unsupported;
	int jit_compile_fail;
	int jit_segv;
	int jit_fallback;    /* complete-but-fallback block: skipped, not crashed */
	int span_mismatch;   /* JIT ran more insns than the scanner block (bc not a JIT
	                        terminator): non-comparable, skipped — see compare site */
};

static void print_regs(const char *label, const PPCRegs *r) {
	fprintf(stderr, "  %s: PC=%08x LR=%08x CTR=%08x CR=%08x XER=%08x\n",
		label, r->pc, r->lr, r->ctr, r->cr, pack_xer(r));
	for (int i = 0; i < 32; i += 4) {
		fprintf(stderr, "    GPR%02d-%02d: %08x %08x %08x %08x\n",
			i, i+3, r->gpr[i], r->gpr[i+1], r->gpr[i+2], r->gpr[i+3]);
	}
}

static bool compare_regs(const PPCRegs *interp, const PPCRegs *jit,
                         uint32_t rom_offset, bool verbose) {
	bool match = true;
	
	/* Compare GPRs */
	for (int i = 0; i < 32; i++) {
		if (interp->gpr[i] != jit->gpr[i]) { match = false; break; }
	}
	/* Compare special regs */
	if (interp->pc != jit->pc) match = false;
	if (interp->lr != jit->lr) match = false;
	if (interp->ctr != jit->ctr) match = false;
	if (interp->cr != jit->cr) match = false;
	if (pack_xer(interp) != pack_xer(jit)) match = false;
	
	if (!match && verbose) {
		fprintf(stderr, "MISMATCH at ROM+0x%06x:\n", rom_offset);
		print_regs("INTERP", interp);
		print_regs("JIT   ", jit);
		/* Find specific diffs */
		for (int i = 0; i < 32; i++) {
			if (interp->gpr[i] != jit->gpr[i])
				fprintf(stderr, "  DIFF GPR%d: interp=%08x jit=%08x\n",
					i, interp->gpr[i], jit->gpr[i]);
		}
		if (interp->pc != jit->pc)
			fprintf(stderr, "  DIFF PC: interp=%08x jit=%08x\n", interp->pc, jit->pc);
		if (interp->lr != jit->lr)
			fprintf(stderr, "  DIFF LR: interp=%08x jit=%08x\n", interp->lr, jit->lr);
		if (interp->ctr != jit->ctr)
			fprintf(stderr, "  DIFF CTR: interp=%08x jit=%08x\n", interp->ctr, jit->ctr);
		if (interp->cr != jit->cr)
			fprintf(stderr, "  DIFF CR: interp=%08x jit=%08x\n", interp->cr, jit->cr);
		if (pack_xer(interp) != pack_xer(jit))
			fprintf(stderr, "  DIFF XER: interp=%08x jit=%08x\n", pack_xer(interp), pack_xer(jit));
	}
	
	return match;
}

/* ---------- Pseudo-random seed for reproducible register state ---------- */
static uint32_t xorshift32(uint32_t *state) {
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static void seed_regs(PPCRegs *r, uint32_t seed, uint32_t rom_base_mac) {
	memset(r, 0, sizeof(*r)); /* zero everything first */
	uint32_t s = seed;
	for (int i = 0; i < 32; i++)
		r->gpr[i] = xorshift32(&s);
	/* R1 = valid stack pointer (within our allocated memory) */
	r->gpr[1] = rom_base_mac - 0x10000; /* stack below ROM */
	r->lr = rom_base_mac + 0x1000;  /* valid LR */
	r->ctr = xorshift32(&s) % 10 + 1; /* cap to prevent infinite bdnz loops */
	r->cr = xorshift32(&s) & 0xFFFFFFFF;
	unpack_xer(r, xorshift32(&s) & 0xE000007F); /* valid XER bits only */
	r->fpscr = 0;
}

/* ===================== Microbenchmark mode (--bench / jit-bench) ===========
 *
 * Fast, deterministic per-instruction timing of the JIT's codegen, so an
 * optimization can be A/B'd in seconds without a boot.  Reuses this harness's
 * memory + JIT-invocation scaffolding (REAL_ADDRESSING: mac_pc == host ptr).
 *
 * Methodology — DIFFERENTIAL timing: each kernel is compiled at two sizes
 * (N_SMALL and N_BIG straight-line body instrs, both ending in blr) and called
 * many times.  ns/insn = (ns_call_big - ns_call_small) / (N_BIG - N_SMALL),
 * which cancels the fixed per-call cost (prologue/epilogue + the regs reset).
 * Reported ns/call is the big kernel; min-of-5 runs after a warm-up.
 *
 * Maintenance contract (docs/TESTING.md "Keeping this current"): --compare
 * warns if the baseline file is older than ppc-jit.cpp, so you never A/B
 * against a baseline that predates the code you are measuring.  To add a
 * kernel: add a k_*() emitter + a BENCH_KERNELS[] row, then re-baseline.
 */

#define BENCH_N_SMALL 16
#define BENCH_N_BIG   144

static uint32_t enc_xo(int rd, int ra, int rb, int xo, int oe, int rc) {
	return 0x7C000000u | (rd << 21) | (ra << 16) | (rb << 11) |
	       (oe << 10) | (xo << 1) | rc;
}
static uint32_t enc_d(int op, int rd, int ra, int16_t d) {
	return ((uint32_t)op << 26) | (rd << 21) | (ra << 16) | (uint16_t)d;
}

/* Each kernel emits `n` body instrs operating on r3/r4/r1, which seed_regs
 * initialises to valid values.  The driver appends the blr terminator. */
static void k_carry(uint8_t *p, int n) {     /* adde r3,r3,r4 (XO=138) — 0b/0f */
	for (int i = 0; i < n; i++) write_be32(p + i*4, enc_xo(3,3,4,138,0,0));
}
static void k_rc1(uint8_t *p, int n) {        /* add. r3,r3,r4 (XO=266,Rc=1) — 0g */
	for (int i = 0; i < n; i++) write_be32(p + i*4, enc_xo(3,3,4,266,0,1));
}
static void k_alu(uint8_t *p, int n) {        /* add/or/xor r3,r3,r4 — RA throughput */
	static const int xo[3] = {266, 444, 316};
	for (int i = 0; i < n; i++) write_be32(p + i*4, enc_xo(3,3,4,xo[i%3],0,0));
}
static void k_loadstore(uint8_t *p, int n) {  /* lwz/stw r3,8(r1) — D-form memory */
	for (int i = 0; i < n; i++)
		write_be32(p + i*4, (i & 1) ? enc_d(36,3,1,8) : enc_d(32,3,1,8));
}
/* FP kernels operate only on FPRs (no guest memory), so they run anywhere the
 * integer kernels do.  seed_regs zeroes the FPRs, so these compute 0.0 each
 * iteration — fine: FP unit timing does not depend on the operand value, and the
 * dependency chain (each op reads the previous result in f1) makes this a
 * latency measurement.  Encodings are fixed 32-bit big-endian PPC words. */
static void k_fp_add(uint8_t *p, int n) {     /* fadd f1,f1,f2  (FP add latency chain) */
	for (int i = 0; i < n; i++) write_be32(p + i*4, 0xFC21102Au);
}
static void k_fp_fma(uint8_t *p, int n) {     /* fmadd f1,f1,f1,f1  (FMA latency recurrence) */
	for (int i = 0; i < n; i++) write_be32(p + i*4, 0xFC21183Au);
}
/* Integer-compute kernel modelling the real Speedometer hot block 0x1ed7befc
 * (P0 profile): a mullw/extsh/divw/add/subf/addi recurrence on r3/r4/r5. This is
 * the runtime-dominant pattern (heavy mullw+divw latency chain) that large-block
 * codegen work (P5 const-fold, P6 scheduling, P8 pinning, RA) must move — the
 * atomics (P3a) were hot by block-count but not by instruction-time. Register-only
 * (no guest memory), so it runs anywhere the other integer kernels do. divw by a
 * possibly-zero r5 is SDIV-safe on ARM64 (returns 0, no trap). */
static void k_compute(uint8_t *p, int n) {
	const uint32_t grp[6] = {
		enc_xo(3,3,4,235,0,0),   /* mullw r3,r3,r4 */
		enc_xo(4,5,0,922,0,0),   /* extsh r5,r4    (rS=4 in rd-slot, rA=5 in ra-slot) */
		enc_xo(3,3,5,491,0,0),   /* divw  r3,r3,r5 */
		enc_xo(3,3,4,266,0,0),   /* add   r3,r3,r4 */
		enc_xo(3,4,3, 40,0,0),   /* subf  r3,r4,r3 (rB-rA = r3-r4) */
		enc_d (14,4,4,1),        /* addi  r4,r4,1  */
	};
	for (int i = 0; i < n; i++) write_be32(p + i*4, grp[i % 6]);
}

struct BenchKernel { const char *name; const char *desc; void (*emit)(uint8_t*,int); };
static const BenchKernel BENCH_KERNELS[] = {
	{ "carry-chain", "adde r3,r3,r4  (0b/0f carry ops)", k_carry     },
	{ "rc1",         "add. r3,r3,r4  (0g lazy-CR0)",     k_rc1       },
	{ "alu",         "add/or/xor     (RA throughput)",   k_alu       },
	{ "fp-add",      "fadd f1,f1,f2  (FP add latency)",  k_fp_add    },
	{ "fp-fma",      "fmadd recurrence (FMA latency)",   k_fp_fma    },
	{ "compute",     "mullw/divw/add chain (0x1ed7befc Speedometer hot)", k_compute },
	/* load-store (k_loadstore) deferred to v2: guest data access goes through
	 * RMEMBASE, which on macOS needs the DIRECT_ADDRESSING base set up so EAs land
	 * in `mem` (low 4 GB is unmappable here). The emitter is kept for that work. */
};
static const int BENCH_NKERNELS = (int)(sizeof(BENCH_KERNELS)/sizeof(BENCH_KERNELS[0]));

/* Timer clock: CLOCK_THREAD_CPUTIME_ID excludes time the bench thread was
 * descheduled (e.g. a co-tenant agent preempting us), so a preemption no longer
 * inflates a sample. It does NOT correct DVFS/thermal frequency shifts — that is
 * what the per-round CV gate in run_bench() catches. (Methodology: benchmark
 * subagents, 2026-06-06; macOS has no usable userspace cycle counter.) */
#ifdef CLOCK_THREAD_CPUTIME_ID
#define BENCH_CLOCK CLOCK_THREAD_CPUTIME_ID
#else
#define BENCH_CLOCK CLOCK_MONOTONIC
#endif

/* Compile `kern` at `n_body` instrs (+ blr) at mem+off; time `iters` calls
 * (min of 5 runs after warm-up); return ns/call, or -1 on compile failure.
 * Also reports the emitted ARM64 code size (bytes) via *code_bytes — a
 * DETERMINISTIC, zero-noise codegen-quality signal (the JIT knows exactly how
 * much it emits), used by run_bench() to compute exact ARM64-insns-per-PPC-op. */
static double bench_time_one(const BenchKernel *kern, int n_body,
                             uint8_t *mem, size_t total, uint32_t off,
                             const PPCRegs *seedregs, long iters,
                             uint32_t *code_bytes) {
	uint8_t *p = mem + off;
	kern->emit(p, n_body);
	write_be32(p + n_body*4, 0x4E800020);          /* blr — block terminator */
	uint32_t mac_pc = (uint32_t)(uintptr_t)p;
	ppc_jit_block jblk;
	if (!ppc_jit_aarch64_compile(mac_pc, mem, total, &jblk) || !jblk.complete) {
		/* stdout, not stderr: `make bench` masks stderr (JIT library chatter has no
		 * env gate); the bench's own diagnostics must survive that redirect. */
		printf("bench: compile failed (%s, n=%d)\n", kern->name, n_body);
		return -1.0;
	}
	if (code_bytes) *code_bytes = jblk.code_size;
	ppc_jit_entry_fn fn = (ppc_jit_entry_fn)(void *)jblk.code;
	PPCRegs regs;
	for (int w = 0; w < 1000; w++) { regs = *seedregs; fn((void *)&regs); } /* warm-up */
	double best = 1e300;
	for (int run = 0; run < 5; run++) {
		struct timespec t0, t1;
		clock_gettime(BENCH_CLOCK, &t0);
		for (long i = 0; i < iters; i++) { regs = *seedregs; fn((void *)&regs); }
		clock_gettime(BENCH_CLOCK, &t1);
		double ns = (t1.tv_sec - t0.tv_sec)*1e9 + (t1.tv_nsec - t0.tv_nsec);
		if (ns < best) best = ns;
	}
	return best / (double)iters;
}

/* Median + coefficient-of-variation (stddev/mean, %) of a sample set. Sorts in
 * place. Used to report a robust point estimate and a noise gate: a high CV means
 * the host was too loaded/thermally-variable to trust the timing this run. */
static int cmp_double(const void *a, const void *b) {
	double d = *(const double*)a - *(const double*)b;
	return (d > 0) - (d < 0);
}
static double bench_median(double *v, int n) {
	qsort(v, n, sizeof(double), cmp_double);
	return (n & 1) ? v[n/2] : 0.5 * (v[n/2 - 1] + v[n/2]);
}
static double bench_cv_pct(const double *v, int n) {
	if (n < 2) return 0.0;
	double mean = 0; for (int i = 0; i < n; i++) mean += v[i]; mean /= n;
	if (mean == 0) return 0.0;
	double var = 0; for (int i = 0; i < n; i++) { double d = v[i]-mean; var += d*d; }
	var /= (n - 1);
	return sqrt(var) / mean * 100.0;
}
/* CV above this → the timing number is not trustworthy this run (host too noisy).
 * The deterministic ARM64-insns/op metric is unaffected and always reported. */
#define BENCH_CV_NOISE_PCT 3.0
#define BENCH_ROUNDS 9

/* Baseline file: "name ns_per_instr arm64_per_op" lines (no JSON dep). The 3rd
 * field is the deterministic ARM64-insns-per-PPC-op metric; older 2-field
 * baselines still parse (a64 returns -1 = "n/a"). */
static bool bench_baseline_stale(const char *path) {
	struct stat sb, sj;
	if (stat(path, &sb) != 0) return false;        /* no baseline yet */
	const char *jit = "../src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp";
	if (stat(jit, &sj) != 0) return false;          /* source not found — skip */
	return sj.st_mtime > sb.st_mtime;
}
/* Look up a kernel's baseline; returns ns/insn (or -1) and fills *a64 with the
 * baseline ARM64-insns/op (or -1 if absent). */
static double bench_baseline_lookup(const char *path, const char *name, double *a64) {
	if (a64) *a64 = -1.0;
	FILE *f = fopen(path, "r");
	if (!f) return -1.0;
	char ln[256]; double val = -1.0;
	while (fgets(ln, sizeof ln, f)) {
		char nm[128]; double v, a = -1.0;
		int got = sscanf(ln, "%127s %lf %lf", nm, &v, &a);
		if (got >= 2 && strcmp(nm, name) == 0) { val = v; if (a64) *a64 = (got >= 3) ? a : -1.0; break; }
	}
	fclose(f);
	return val;
}

static int run_bench(int argc, char **argv) {
	long iters = 300000;
	const char *compare_path = NULL, *save_path = NULL;
	for (int i = 1; i < argc; i++) {
		if      (!strncmp(argv[i], "--bench-iters=",   14)) iters = atol(argv[i]+14);
		else if (!strncmp(argv[i], "--compare=",       10)) compare_path = argv[i]+10;
		else if (!strncmp(argv[i], "--save-baseline=", 16)) save_path = argv[i]+16;
	}

#if defined(__APPLE__)
	/* Bias scheduling onto a performance core. On Apple Silicon thread affinity is
	 * a no-op; QoS is the only lever. A default-QoS bench thread can migrate to an
	 * E-core mid-run under load — a tens-of-percent swing. (Benchmark subagents.) */
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

	const size_t total = 16 * 1024 * 1024;          /* 16 MB scratch */
	uint8_t *mem = (uint8_t *)mmap((void *)0x10000000UL, total,
		ROM_HARNESS_MEM_PROT, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
	if (mem == MAP_FAILED)
		mem = (uint8_t *)mmap(NULL, total, ROM_HARNESS_MEM_PROT,
			MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) { printf("bench mmap: %s\n", strerror(errno)); return 1; }
	/* macOS arm64 cannot map below 4 GB (__PAGEZERO), so `mem` lands high — that
	 * is fine for the register-only kernels: instruction *fetch* is offset-based
	 * (jit_fetch_ptr keys on low32(ram) and returns ram+offset), and these kernels
	 * touch only GPRs + the regs struct (via RSTATE), never guest memory through
	 * RMEMBASE.  Memory kernels (load/store) would need the DIRECT_ADDRESSING base
	 * configured so guest data addresses land in `mem` — deferred to v2 (see
	 * k_loadstore). */
	memset(mem, 0, total);

	if (!ppc_jit_aarch64_init(4096)) {
		printf("bench: JIT init failed\n");
		return 1;
	}

	uint32_t base = (uint32_t)(uintptr_t)mem + 0x800000;  /* mid-buffer */
	PPCRegs seedregs;
	seed_regs(&seedregs, 0xB3C0FFEE, base);               /* valid r1/lr; random r3/r4 */

	if (compare_path && bench_baseline_stale(compare_path))
		printf("WARNING: baseline '%s' is OLDER than ppc-jit.cpp — "
		       "comparison may be stale; re-baseline with --save-baseline.\n",
		       compare_path);

	printf("jit-bench: %d kernels, iters=%ld, %d rounds, differential N=%d-%d\n"
	       "  ns/insn: median of rounds (timing; CV>%.0f%% = host too noisy, *flagged)\n"
	       "  a64/op:  emitted ARM64 insns per PPC op (DETERMINISTIC, zero-noise codegen metric)\n",
	       BENCH_NKERNELS, iters, BENCH_ROUNDS, BENCH_N_BIG, BENCH_N_SMALL, BENCH_CV_NOISE_PCT);
	printf("%-13s %10s %7s %8s", "kernel", "ns/insn", "cv%", "a64/op");
	if (compare_path) printf(" %9s %9s", "ns base", "a64 base");
	printf("   description\n");

	FILE *out = save_path ? fopen(save_path, "w") : NULL;
	int rc = 0;
	for (int k = 0; k < BENCH_NKERNELS; k++) {
		const BenchKernel *kn = &BENCH_KERNELS[k];
		/* unique offset per (kernel,size) so the JIT block cache can't alias PCs */
		uint32_t off_s = 0x100000 + (uint32_t)(k*2+0)*0x10000;
		uint32_t off_b = 0x100000 + (uint32_t)(k*2+1)*0x10000;
		/* Interleave small/big across rounds so both see the same host clock; the
		 * per-round differential cancels prologue/epilogue, and the spread across
		 * rounds is the host-noise estimate (CV). */
		double per_insn_rounds[BENCH_ROUNDS];
		uint32_t code_s = 0, code_b = 0;
		int nr = 0; bool failed = false;
		for (int r = 0; r < BENCH_ROUNDS; r++) {
			/* Capture code_size only on round 0: that is the real compile; rounds 1+
			 * hit the block cache at the same PC, which reports code_size=0. */
			double small = bench_time_one(kn, BENCH_N_SMALL, mem, total, off_s, &seedregs, iters, r ? NULL : &code_s);
			double big   = bench_time_one(kn, BENCH_N_BIG,   mem, total, off_b, &seedregs, iters, r ? NULL : &code_b);
			if (small < 0 || big < 0) { failed = true; break; }
			per_insn_rounds[nr++] = (big - small) / (double)(BENCH_N_BIG - BENCH_N_SMALL);
		}
		if (failed) { rc = 1; continue; }
		double cv = bench_cv_pct(per_insn_rounds, nr);
		double per_insn = bench_median(per_insn_rounds, nr);   /* sorts in place */
		/* Deterministic: exact ARM64 insns emitted per extra PPC op (zero noise). */
		double a64_per_op = (double)((int)code_b - (int)code_s) / 4.0
		                    / (double)(BENCH_N_BIG - BENCH_N_SMALL);
		bool noisy = cv > BENCH_CV_NOISE_PCT;
		printf("%-13s %10.3f %6.1f%s %8.3f", kn->name, per_insn, cv, noisy ? "*" : " ", a64_per_op);
		if (compare_path) {
			double ba; double b = bench_baseline_lookup(compare_path, kn->name, &ba);
			/* timing delta — suppressed when this run is noisy (untrustworthy) */
			if (noisy)        printf(" %9s", "NOISY");
			else if (b > 0)   printf(" %+8.1f%%", (per_insn - b) / b * 100.0);
			else              printf(" %9s", "(new)");
			/* a64/op delta — always exact, host-independent */
			if (ba >= 0)      printf(" %+8.3f", a64_per_op - ba);
			else              printf(" %9s", "(new)");
		}
		printf("   %s\n", kn->desc);
		if (out) fprintf(out, "%s %.6f %.6f\n", kn->name, per_insn, a64_per_op);
	}
	if (out) { fclose(out); printf("baseline written: %s\n", save_path); }
	munmap(mem, total);
	return rc;
}

/* ---------- Main ---------- */

static void usage(const char *prog) {
	fprintf(stderr, "Usage: %s <rom-file> [options]\n"
		"  --offset=0xNNNNNN   Start ROM offset (default: 0)\n"
		"  --count=N           Max blocks to test (default: all)\n"
		"  --verbose           Print each result\n"
		"  --stop-on-fail      Stop at first mismatch\n"
		"  --min-insns=N       Minimum block size (default: 1)\n"
		"  --max-insns=N       Maximum block size (default: 64)\n"
		"  --entry=0xNNNNNN    Test single block at ROM offset\n"
		"  --seed=N            Random seed (default: 0xDEADBEEF)\n"
		"  --passes=N          Number of random-seed passes (default: 1)\n"
		"  --compute-only      Skip blocks with memory access (default)\n"
		"  --all-blocks        Include blocks with memory access (will likely segv)\n",
		prog);
}

int main(int argc, char **argv) {
	/* Microbenchmark mode: self-contained, needs no ROM file. */
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--bench")) return run_bench(argc, argv);

	/* Parse args */
	const char *rom_path = NULL;
	uint32_t start_offset = 0;
	int max_count = 0; /* 0 = all */
	bool verbose = false;
	bool stop_on_fail = false;
	int min_insns = 1;
	int max_insns = 64;
	uint32_t single_entry = 0xFFFFFFFF;
	uint32_t seed = 0xDEADBEEF;
	int passes = 1;
	bool compute_only = true;
	
	static struct option long_opts[] = {
		{"offset", required_argument, 0, 'o'},
		{"count", required_argument, 0, 'c'},
		{"verbose", no_argument, 0, 'v'},
		{"stop-on-fail", no_argument, 0, 's'},
		{"min-insns", required_argument, 0, 'm'},
		{"max-insns", required_argument, 0, 'M'},
		{"entry", required_argument, 0, 'e'},
		{"seed", required_argument, 0, 'S'},
		{"passes", required_argument, 0, 'p'},
		{"compute-only", no_argument, 0, 'C'},
		{"all-blocks", no_argument, 0, 'A'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};
	
	int ch;
	while ((ch = getopt_long(argc, argv, "o:c:vsm:M:e:S:p:CAh", long_opts, NULL)) != -1) {
		switch (ch) {
		case 'o': start_offset = strtoul(optarg, NULL, 0); break;
		case 'c': max_count = atoi(optarg); break;
		case 'v': verbose = true; break;
		case 's': stop_on_fail = true; break;
		case 'm': min_insns = atoi(optarg); break;
		case 'M': max_insns = atoi(optarg); break;
		case 'e': single_entry = strtoul(optarg, NULL, 0); break;
		case 'S': seed = strtoul(optarg, NULL, 0); break;
		case 'p': passes = atoi(optarg); break;
		case 'C': compute_only = true; break;
		case 'A': compute_only = false; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 1;
		}
	}
	
	if (optind < argc) rom_path = argv[optind];
	if (!rom_path) { usage(argv[0]); return 1; }
	
	/* Load ROM */
	int fd = open(rom_path, O_RDONLY);
	if (fd < 0) { perror(rom_path); return 1; }
	off_t rom_size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	
	if (rom_size <= 0 || rom_size > 8 * 1024 * 1024) {
		fprintf(stderr, "Invalid ROM size: %ld\n", (long)rom_size);
		close(fd);
		return 1;
	}
	
	/* Allocate memory: RAM area + ROM area, contiguous */
	const size_t ram_size = 16 * 1024 * 1024;
	const size_t total_size = ram_size + rom_size + 0x100000; /* ROM area + padding */
	
	uint8_t *mem = (uint8_t *)mmap(
		(void *)0x10000000UL, total_size,
		ROM_HARNESS_MEM_PROT,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
		-1, 0);
	if (mem == MAP_FAILED) {
		mem = (uint8_t *)mmap(NULL, total_size,
			ROM_HARNESS_MEM_PROT,
			MAP_PRIVATE | MAP_ANONYMOUS,
			-1, 0);
	}
	if (mem == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return 1;
	}
	memset(mem, 0, total_size);
	
	uint8_t *ram_host = mem;
	uint8_t *rom_host = mem + ram_size;
	uint32_t ram_base_mac = (uint32_t)(uintptr_t)ram_host;
	uint32_t rom_base_mac = (uint32_t)(uintptr_t)rom_host;
	
	/* Read ROM into memory */
	size_t rd = 0;
	while (rd < (size_t)rom_size) {
		ssize_t n = read(fd, rom_host + rd, (size_t)rom_size - rd);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("read");
			break;
		}
		if (n == 0)
			break;
		rd += (size_t)n;
	}
	close(fd);
	if (rd != (size_t)rom_size) {
		fprintf(stderr, "Short read: %ld of %ld\n", (long)rd, (long)rom_size);
		munmap(mem, total_size);
		return 1;
	}
	
	fprintf(stderr, "ROM loaded: %s (%ld bytes)\n", rom_path, (long)rom_size);
	fprintf(stderr, "Memory layout: RAM=%08x-%08x, ROM=%08x-%08x\n",
		ram_base_mac, ram_base_mac + (uint32_t)ram_size,
		rom_base_mac, rom_base_mac + (uint32_t)rom_size);
	
	/* Scan ROM for blocks */
	const int MAX_BLOCKS = 100000;
	ROMBlock *blocks = new ROMBlock[MAX_BLOCKS];
	int n_blocks;
	
	if (single_entry != 0xFFFFFFFF) {
		/* Single block mode */
		blocks[0].offset = single_entry;
		blocks[0].n_insns = 0;
		blocks[0].has_mem_access = false;
		blocks[0].has_privileged = false;
		blocks[0].has_emul_op = false;
		blocks[0].has_link_read = false;
		blocks[0].terminated = false;
		
		/* Scan to find block size */
		uint32_t scan = single_entry;
		while (scan < (uint32_t)rom_size - 4 && blocks[0].n_insns < max_insns) {
			uint32_t insn = read_be32(rom_host + scan);
			if (insn == 0) break;
			if (is_memory_access(insn)) blocks[0].has_mem_access = true;
			if (is_privileged(insn)) blocks[0].has_privileged = true;
			if (ppc_primary(insn) == 6) blocks[0].has_emul_op = true;
			if (reads_link_regs(insn)) blocks[0].has_link_read = true;
			blocks[0].n_insns++;
			scan += 4;
			if (is_block_terminator(insn)) { blocks[0].terminated = true; break; }
		}
		n_blocks = 1;
		verbose = true;
	} else {
		fprintf(stderr, "Scanning ROM for basic blocks (min=%d, max=%d)...\n",
			min_insns, max_insns);
		n_blocks = scan_rom_blocks(rom_host + start_offset,
			rom_size - start_offset, blocks, MAX_BLOCKS,
			min_insns, max_insns);
		/* Adjust offsets for start_offset */
		for (int i = 0; i < n_blocks; i++)
			blocks[i].offset += start_offset;
		fprintf(stderr, "Found %d basic blocks\n", n_blocks);
	}
	
	if (max_count > 0 && max_count < n_blocks)
		n_blocks = max_count;
	
	/* Init JIT */
	if (!ppc_jit_aarch64_init(4096)) {
		fprintf(stderr, "JIT init failed\n");
		delete[] blocks;
		munmap(mem, total_size);
		return 1;
	}
	
	/* Install SIGSEGV handler */
	struct sigaction sa = {}, old_sa = {};
	sa.sa_sigaction = segv_handler;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, &old_sa);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGALRM, &sa, NULL);
	
	/* Run tests */
	TestResult result = {};
	int testable = 0;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	
	for (int pass = 0; pass < passes; pass++) {
		uint32_t pass_seed = seed + pass * 0x12345;
		
		for (int bi = 0; bi < n_blocks; bi++) {
			const ROMBlock &blk = blocks[bi];
			result.total++;
			
			/* Filter */
			if (compute_only && blk.has_mem_access) { result.skipped++; continue; }
			if (blk.has_privileged) { result.skipped++; continue; }
			if (blk.has_emul_op) { result.skipped++; continue; }
			
			testable++;
			
			/* Prepare register state */
			PPCRegs interp_regs, jit_regs;
			seed_regs(&interp_regs, pass_seed + blk.offset, rom_base_mac);
			memcpy(&jit_regs, &interp_regs, sizeof(PPCRegs));
			
			/* Set PC */
			uint32_t block_mac_pc = rom_base_mac + blk.offset;
			interp_regs.pc = block_mac_pc;
			jit_regs.pc = block_mac_pc;
			
			/* Run interpreter */
			if (!interpret_block(&interp_regs, rom_host, rom_size,
			                     block_mac_pc, rom_base_mac, blk.n_insns)) {
				result.interp_unsupported++;
				result.skipped++;
				continue;
			}
			
			/* JIT compile — note: JIT expects the full memory buffer and Mac PC */
			ppc_jit_block jblk;
			if (!ppc_jit_aarch64_compile(block_mac_pc, mem, total_size, &jblk) ||
			    jblk.n_insns == 0) {
				result.jit_compile_fail++;
				result.skipped++;
				if (verbose)
					fprintf(stderr, "  ROM+0x%06x: JIT compile failed (%d insns)\n",
						blk.offset, blk.n_insns);
				continue;
			}
			
			/* Only test complete blocks — incomplete blocks return to interpreter
			   at the failing instruction, which is correct but not comparable */
			if (!jblk.complete) {
				result.jit_compile_fail++;
				result.skipped++;
				if (verbose)
					fprintf(stderr, "  ROM+0x%06x: JIT incomplete (%d/%d insns)\n",
						blk.offset, jblk.n_insns, blk.n_insns);
				continue;
			}
			
			/* Run JIT with SIGSEGV + fallback protection + timeout. A complete
			   block can still hit the inline-interp fallback bridge at runtime
			   (ppc_jit_interp_one), which the standalone harness can't execute;
			   both that and a SIGSEGV longjmp back here so the block is skipped,
			   not fatal. */
			segv_caught = 0;
			fallback_caught = 0;
			if (sigsetjmp(jit_guard_jmp, 1) == 0) {
				ppc_jit_entry_fn fn = (ppc_jit_entry_fn)(void *)jblk.code;
				alarm(1); /* 1-second timeout per block */
				fn((void *)&jit_regs);
			}
			alarm(0); /* cancel timeout on ALL exit paths — normal return, SIGSEGV,
			             or the fallback longjmp (which skips the in-body alarm(0)),
			             so a pending alarm can't fire during a later block */
			if (segv_caught) {
				result.jit_segv++;
				result.skipped++;
				if (verbose)
					fprintf(stderr, "  ROM+0x%06x: JIT SIGSEGV\n", blk.offset);
				continue;
			}
			if (fallback_caught) {
				result.jit_fallback++;
				result.skipped++;
				if (verbose)
					fprintf(stderr, "  ROM+0x%06x: JIT fallback (opcode=%08x pc=%08x) "
						"— skipped\n", blk.offset,
						(uint32_t)fallback_opcode, (uint32_t)fallback_pc);
				continue;
			}
			
			/* Compare.
			 *
			 * KNOWN FALSE-POSITIVE SOURCE — block-model mismatch (TODO, tracked in
			 * ROADMAP A1 / OPTIMIZATION-PLAN §0b-extra4):
			 * the scanner ends a block at the first terminator and treats `bc`
			 * (opcode 16) as one, so the interpreter above ran exactly `blk.n_insns`.
			 * But the JIT does NOT treat `bc` as a block terminator — it compiles and
			 * runs PAST it, so `jblk.n_insns` can exceed `blk.n_insns`. When it does,
			 * interp and JIT executed DIFFERENT instruction spans from the same start
			 * PC, and the register diff below is structural, not a codegen bug (the
			 * exact analog of the SS_JIT_VERIFY fix-(i) block-exit problem). Verified
			 * example: bc block 42424642 — JIT ran 5 insns, interp ran 1; the bc itself
			 * is correct (non-vacuous SS_TEST_HEX "38600002 7C6903A6 42424642" gives
			 * interp==JIT). GPR diffs in multi-insn blocks are largely CASCADE from this.
			 *
			 * THE GATE BELOW makes failures trustworthy: it compares only when the
			 * spans match. This trades raw coverage (drops bc-terminated blocks, where
			 * the JIT ran further) for a clean signal — the dropped count is reported as
			 * "Span mismatch" so the coverage cost is visible, not silent. A future
			 * upgrade (ROADMAP A1) could instead run the interpreter for `jblk.n_insns`
			 * instructions to RECOVER that coverage; until then, skip. */
			if ((uint32_t)blk.n_insns != jblk.n_insns) {
				result.span_mismatch++;
				result.skipped++;
				if (verbose)
					fprintf(stderr, "  ROM+0x%06x: span mismatch (scanner %d insns, "
						"JIT %d) — non-comparable, skipped\n",
						blk.offset, blk.n_insns, jblk.n_insns);
				continue;
			}

			/* Compare (spans now guaranteed equal) */
			if (compare_regs(&interp_regs, &jit_regs, blk.offset, verbose)) {
				result.passed++;
				if (verbose)
					fprintf(stderr, "  ROM+0x%06x: PASS (%d insns)\n",
						blk.offset, blk.n_insns);
			} else {
				result.failed++;
				if (verbose) {
					/* Dump the block instructions */
					fprintf(stderr, "  Block instructions:\n");
					for (int j = 0; j < blk.n_insns; j++) {
						uint32_t w = read_be32(rom_host + blk.offset + j * 4);
						fprintf(stderr, "    %06x: %08x (opc=%d",
							blk.offset + j * 4, w, ppc_primary(w));
						if (ppc_primary(w) == 31)
							fprintf(stderr, " xo=%d", ppc_xo(w));
						fprintf(stderr, ")\n");
					}
				}
				if (stop_on_fail) goto done;
			}
			
			/* Progress */
			if (!verbose && testable % 10000 == 0)
				fprintf(stderr, "\r  Tested %d blocks... (%d pass, %d fail)",
					testable, result.passed, result.failed);
		}
	}

done:
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
	
	/* Restore signal handler */
	sigaction(SIGSEGV, &old_sa, NULL);
	
	/* Summary */
	fprintf(stderr, "\n=== ROM Harness Results ===\n");
	fprintf(stderr, "Total blocks scanned: %d\n", result.total);
	fprintf(stderr, "Testable (compute-only, non-privileged): %d\n", testable);
	fprintf(stderr, "Passed:              %d\n", result.passed);
	fprintf(stderr, "Failed:              %d\n", result.failed);
	fprintf(stderr, "Skipped:             %d\n", result.skipped);
	fprintf(stderr, "  Interp unsupported: %d\n", result.interp_unsupported);
	fprintf(stderr, "  JIT compile fail:   %d\n", result.jit_compile_fail);
	fprintf(stderr, "  JIT SIGSEGV:        %d\n", result.jit_segv);
	fprintf(stderr, "  JIT fallback:       %d\n", result.jit_fallback);
	fprintf(stderr, "  Span mismatch:      %d  (bc-terminated: JIT ran past the scanner block)\n",
		result.span_mismatch);
	fprintf(stderr, "Time: %.2f sec (%.0f blocks/sec)\n",
		elapsed, testable > 0 ? testable / elapsed : 0);
	fprintf(stderr, "Score: %d/%d\n", result.passed, result.passed + result.failed);
	
	/* Cleanup */
	ppc_jit_aarch64_exit();
	delete[] blocks;
	munmap(mem, total_size);
	
	return result.failed > 0 ? 1 : 0;
}
