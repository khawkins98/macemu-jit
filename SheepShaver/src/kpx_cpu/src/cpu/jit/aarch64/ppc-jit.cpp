/*
 *  ppc-jit.cpp — PPC → AArch64 direct codegen JIT
 *
 *  Compiles PPC basic blocks to native ARM64 instructions.
 *  Generated code is called as: void block(powerpc_registers *regs)
 *  with x0 = regs pointer. Block reads/writes GPR/CR/LR/CTR/PC via
 *  LDR/STR at known offsets from x0 (moved to callee-saved x20).
 */

#ifdef __aarch64__

/* Pull in the build configuration so the guest-memory addressing model
 * (NATMEM_OFFSET / DIRECT_ADDRESSING / REAL_ADDRESSING) is visible here. The
 * JIT prologue loads JIT_MEM_BASE (derived below) into the memory-base
 * register; without this, NATMEM_OFFSET is invisible and the JIT would emit a
 * base of 0 and fault under DIRECT addressing. */
#if defined(HAVE_CONFIG_H) && defined(__has_include)
#  if __has_include("config.h")
#    include "config.h"
#  endif
#elif defined(HAVE_CONFIG_H)
#  include "config.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

static double pjit_elapsed_s() {
	static struct timespec t0 = {0,0};
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	if (t0.tv_sec == 0) t0 = t;
	return (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9;
}
#define JIT_LOG(fmt, ...) fprintf(stderr, "[JIT %.2fs] " fmt "\n", pjit_elapsed_s(), ##__VA_ARGS__)
#include "ppc-jit.h"
#include "ppc-codegen-aarch64.h"
#include "jit-target-cache.hpp"

/* ---- Code cache ---- */
static uint8_t  *jit_cache_base = NULL;
static size_t    jit_cache_size = 0;
static uint32_t *jit_cache_wp   = NULL;
static uint32_t *jit_cache_end  = NULL;

/* ---- Block address cache (PC → compiled code) ----
 *
 * CONTRACT (see SheepShaver/docs/AARCH64_JIT_RUNTIME_CONTRACT.md):
 *   Compiled blocks are stored here by PPC entry PC so they are not
 *   recompiled on every execution.  Hash table with chaining:
 *   bucket = (pc >> 2) & JIT_BC_MASK.  Entries within a bucket are
 *   linked via .next.  An entry is valid when .code != NULL.
 *
 *   Flush discipline: the entire cache must be invalidated whenever
 *   Mac OS invalidates any region of PPC code (icbi/isync) or when
 *   the JIT code-cache write-pointer is reset (ppc_jit_aarch64_flush).
 */
#define JIT_BC_BUCKETS  32768               /* must be power of 2 */
#define JIT_BC_MASK     (JIT_BC_BUCKETS - 1)
#define JIT_BC_POOL     65536               /* max total entries across all chains */

/* ---- Block-to-block chaining (mechanism complete; default OFF) -------------
 *
 * JIT_BLOCK_CHAINING = 1: block exits whose target is already compiled branch
 * directly to the target's chain entry (B <chain_code>), bypassing the C
 * dispatcher.  Targets not yet compiled fall back to the standard LDP+RET
 * epilogue and are back-patched (patch_chain_sites) once the target exists.
 *
 * Interrupt safety: every block's chain entry begins with a spcflags poll
 * (emit_entry_spcflags_poll — the dyngen gen_start equivalent).  A chained
 * cycle therefore polls pending interrupts at every block boundary; when a
 * flag is set, the block returns to the dispatcher, which services it via
 * check_spcflags() and re-dispatches.  The poll masks to actionable bits only
 * (PPCR_SPCFLAGS_POLL_MASK = 0x0F) — SPCFLAG_JIT_EXEC_RETURN (bit 16) is not
 * cleared at the dispatch site and must not be polled (it would loop forever).
 *
 * With chaining ON both harness modes pass (227/227); what remains unproven is
 * a full boot in that configuration (boot verification is gated on the
 * 68k-region interrupt-timing work — see LEARNINGS.md).  Until a chained boot is
 * verified, the define stays 0; flip to 1 to test.
 *
 * History: chaining was originally written but never functional (a marking bug
 * excluded every chained block from execution).  Enabling it without the entry
 * poll hangs Mac OS I/O — see LEARNINGS.md chaining post-mortem for details. */
#define JIT_BLOCK_CHAINING 1

/* SS_JIT_NO_CHAIN=1: runtime kill-switch for block chaining (bisect aid).
 * Read once on first use; gates both compile-time chain emission
 * (emit_epilogue_with_pc) and runtime back-patching (patch_chain_sites).
 * Lets a single build answer "is chaining the variable that breaks boot?"
 * without recompiling. */
static inline bool jit_chain_runtime_disabled(void) {
	static int cached = -1;
	if (cached < 0) {
		const char *e = getenv("SS_JIT_NO_CHAIN");
		cached = (e && *e == '1') ? 1 : 0;
	}
	return cached == 1;
}

/* ---- Chain patch-site pool -----------------------------------------------
 * When emit_epilogue_with_pc() cannot chain at compile time (target not yet
 * in JIT cache), it records the location of the first LDP instruction in the
 * standard epilogue.  When the target block is later inserted, all matching
 * sites are back-patched: the LDP is overwritten with a direct B <chain_code>.
 * The remaining LDP+RET instructions become unreachable dead code.
 * On full cache flush, all sites are discarded (blocks are recompiled). */
/* ARM64 encoding of the first instruction in the standard block epilogue:
 * LDP x27, x28, [sp], #16  — used to restore the original epilogue when
 * reverting chain-patches during range-based JIT cache invalidation. */
#define JIT_EPILOGUE_FIRST_LDP 0xA8C17BFBU

#define JIT_CHAIN_SITE_POOL 16384
struct jit_chain_site {
	uint32_t  target_pc; /* PPC PC this site wants to chain to */
	uint32_t *patch_loc; /* ARM64 addr where B<chain_code> was (or will be) written */
	bool      patched;   /* true = B<chain_code> is live at patch_loc */
	int       next;      /* next site in same bucket, -1=end */
};
static struct jit_chain_site chain_site_pool[JIT_CHAIN_SITE_POOL];
static int chain_site_heads[JIT_BC_BUCKETS]; /* -1=empty; initialised by jit_bc_flush() before first use */
static int chain_site_pool_next = 0;

struct jit_bc_entry {
	uint32_t  pc;         /* PPC address this block was compiled from */
	uint32_t *code;       /* normal ABI entry point (with prologue) */
	uint32_t *chain_code; /* chain entry point (after prologue, for direct chaining) */
	int       n_insns;    /* number of compiled PPC instructions */
	bool      complete;   /* true iff every instruction in block is native */
	int       next;       /* index of next entry in chain, -1 = end */
};

static struct jit_bc_entry jit_bc_pool[JIT_BC_POOL];
static int jit_bc_heads[JIT_BC_BUCKETS];  /* -1=empty; initialised by jit_bc_flush() before first use */
static int jit_bc_pool_next = 0;          /* next free pool entry */

/* True once the bucket-head arrays have been set to their -1 "empty" sentinel.
 * These are static (zero-initialised) arrays, but 0 is a VALID pool index — the
 * empty sentinel is -1. Until jit_bc_flush() runs they contain all-zeros, which
 * makes jit_bc_lookup() walk idx=0 -> pool[0].next=0 -> idx=0 forever (a self
 * cycle), hanging the CPU thread. jit_bc_flush() is normally called from
 * ppc_jit_aarch64_init(), but ONLY after the code cache allocation succeeds; if
 * that allocation fails the arrays are left zero-filled and the first lookup
 * hangs. jit_bc_ensure_init() guarantees the sentinel is established before any
 * bucket walk, independent of code-cache allocation success. */
static bool jit_bc_ready = false;

static void jit_bc_flush(void) {
	for (int i = 0; i < JIT_BC_BUCKETS; i++) jit_bc_heads[i] = -1;
	jit_bc_pool_next = 0;
	/* Also clear chain patch sites — all recorded epilogues are now invalid */
	for (int i = 0; i < JIT_BC_BUCKETS; i++) chain_site_heads[i] = -1;
	chain_site_pool_next = 0;
	jit_bc_ready = true;
}

static inline void jit_bc_ensure_init(void) {
	if (!jit_bc_ready)
		jit_bc_flush();
}

/* ---- Secondary executable range (Mac ROM) ----
 * The Mac ROM is vm_protect()ed READ|EXECUTE after rom_patches are applied
 * (main_unix.cpp), so it is immutable during emulation: ROM blocks compiled by
 * the JIT can never go stale and never need SMC invalidation.  ROM toolbox code
 * is the dominant execution target during boot — compiling it is the single
 * largest JIT speedup available.
 * Registered once at init via ppc_jit_aarch64_set_rom_range(); zero size means
 * "not registered" (standalone harnesses never register it). */
static uint32_t      jit_rom_base = 0;
static uint32_t      jit_rom_size = 0;
static const uint8_t *jit_rom_host = NULL;

/* Resolve a guest PC to a host fetch pointer for instruction reads.
 * Returns NULL if the PC is not inside a JIT-compilable executable range. */
static inline const uint8_t *jit_fetch_ptr(uint32_t guest_pc, const uint8_t *ram, size_t ramsize)
{
	const uint32_t ram_base = (uint32_t)(uintptr_t)ram;
	if (guest_pc >= ram_base && guest_pc < ram_base + ramsize)
		return ram + (guest_pc - ram_base);
	if (jit_rom_size != 0 && guest_pc >= jit_rom_base &&
	    guest_pc < jit_rom_base + jit_rom_size)
		return jit_rom_host + (guest_pc - jit_rom_base);
	return NULL;
}

/* Record a chain patch site: when the target block at next_pc is compiled,
 * patch_loc (pointing to the first LDP of the standard epilogue) will be
 * overwritten with B <chain_code_of_next_pc>. */
static void record_chain_site(uint32_t next_pc, uint32_t *patch_loc) {
	jit_bc_ensure_init();
	if (chain_site_pool_next >= JIT_CHAIN_SITE_POOL) return; /* pool full, skip */
	int bucket = (next_pc >> 2) & JIT_BC_MASK;
	int idx = chain_site_pool_next++;
	chain_site_pool[idx].target_pc = next_pc;
	chain_site_pool[idx].patch_loc = patch_loc;
	chain_site_pool[idx].next      = chain_site_heads[bucket];
	chain_site_heads[bucket]       = idx;
}

/* When a new block is inserted at pc with chain_code, back-patch all
 * standard epilogues that were waiting to chain to this PC. */
static void patch_chain_sites(uint32_t pc, uint32_t *chain_code) {
	if (!chain_code) return;
#if !JIT_BLOCK_CHAINING
	/* Chaining disabled: no sites are ever recorded, nothing to patch.
	 * (record_chain_site is only called from the chaining paths.) */
	(void)pc;
	return;
#endif
	if (jit_chain_runtime_disabled()) return; /* SS_JIT_NO_CHAIN=1 */
	jit_bc_ensure_init();
	int bucket = (pc >> 2) & JIT_BC_MASK;
	int idx = chain_site_heads[bucket];
	while (idx >= 0) {
		struct jit_chain_site *site = &chain_site_pool[idx];
		if (site->target_pc == pc && site->patch_loc && !site->patched) {
			int32_t off = (int32_t)((uint8_t *)chain_code - (uint8_t *)site->patch_loc);
			if (off >= -(1 << 25) && off < (1 << 25)) {
				jit_cache_begin_write();
				*site->patch_loc = 0x14000000 | ((off >> 2) & 0x3FFFFFF); /* B offset */
				jit_cache_flush(site->patch_loc, sizeof(uint32_t));
				jit_cache_end_write(site->patch_loc, sizeof(uint32_t));
				site->patched = true; /* live — kept for range-invalidation reversal */
			}
		}
		idx = site->next;
	}
}

static void jit_bc_invalidate_pc(uint32_t pc) {
	jit_bc_ensure_init();
	int bucket = (pc >> 2) & JIT_BC_MASK;
	int prev = -1;
	int idx = jit_bc_heads[bucket];
	while (idx >= 0) {
		if (jit_bc_pool[idx].pc == pc) {
			/* Unlink from chain */
			if (prev >= 0)
				jit_bc_pool[prev].next = jit_bc_pool[idx].next;
			else
				jit_bc_heads[bucket] = jit_bc_pool[idx].next;
			jit_bc_pool[idx].code = NULL;
			return;
		}
		prev = idx;
		idx = jit_bc_pool[idx].next;
	}
}

static const struct jit_bc_entry *jit_bc_lookup(uint32_t pc) {
	jit_bc_ensure_init();
	int idx = jit_bc_heads[(pc >> 2) & JIT_BC_MASK];
	while (idx >= 0) {
		if (jit_bc_pool[idx].pc == pc && jit_bc_pool[idx].code)
			return &jit_bc_pool[idx];
		idx = jit_bc_pool[idx].next;
	}
	return NULL;
}

static void jit_bc_insert(uint32_t pc, uint32_t *code, uint32_t *chain_code, bool complete, int n_insns = 0) {
	jit_bc_ensure_init();
	/* Check if already exists */
	int bucket = (pc >> 2) & JIT_BC_MASK;
	int idx = jit_bc_heads[bucket];
	while (idx >= 0) {
		if (jit_bc_pool[idx].pc == pc) {
			jit_bc_pool[idx].code       = code;
			jit_bc_pool[idx].chain_code = chain_code;
			jit_bc_pool[idx].complete   = complete;
			jit_bc_pool[idx].n_insns    = n_insns;
			/* Existing entries can be refreshed after invalidation/recompile; satisfy
			 * any epilogues that were recorded while the target was unavailable. */
			patch_chain_sites(pc, chain_code);
			return;
		}
		idx = jit_bc_pool[idx].next;
	}
	/* New entry — allocate from pool */
	if (jit_bc_pool_next >= JIT_BC_POOL) {
		/* Pool exhausted — flush everything and start fresh */
		jit_bc_flush();
	}
	idx = jit_bc_pool_next++;
	jit_bc_pool[idx].pc         = pc;
	jit_bc_pool[idx].code       = code;
	jit_bc_pool[idx].chain_code = chain_code;
	jit_bc_pool[idx].complete   = complete;
	jit_bc_pool[idx].n_insns    = n_insns;
	jit_bc_pool[idx].next       = jit_bc_heads[bucket];
	jit_bc_heads[bucket]        = idx;
	/* Back-patch any standard epilogues in older blocks that were waiting
	 * to chain to this PC but couldn't at their compile time. */
	patch_chain_sites(pc, chain_code);
}

/* ---- Register offsets in powerpc_registers ----
   Determined from compiled struct layout on aarch64. */
#define PPCR_GPR(n) ((uint32_t)((n) * 4))
#define PPCR_GPR_HI(n) ((uint32_t)(128 + (n) * 4))  /* upper 32 bits for 64-bit G5 mode */
#define PPCR_CR     1024
#define PPCR_XER    1028
/* XER is a struct {uint8 so, ov, ca, byte_count} — use byte offsets */
#define PPCR_XER_SO   1028
#define PPCR_XER_OV   1029
#define PPCR_XER_CA   1030
#define PPCR_XER_CNT  1031
#define PPCR_FPSCR  1040
#define PPCR_LR     1044
#define PPCR_CTR    1048
#define PPCR_PC     1052
#define PPCR_SPCFLAGS 1056  /* basic_spcflags.mask — pending interrupt/event flags */
/* Actionable flag bits for the block-entry poll (dyngen gen_start equivalent).
 * These are exactly the bits powerpc_cpu::check_spcflags() clears/handles when
 * the dispatcher regains control at the JIT post-dispatch site (ppc-cpu.cpp
 * pdi_jit_post): EXEC_RETURN(1) | TRIGGER_INTERRUPT(2) | HANDLE_INTERRUPT(4) |
 * ENTER_MON(8) = 0x0F.  SPCFLAG_JIT_EXEC_RETURN(16) is DELIBERATELY EXCLUDED:
 * check_spcflags() does not clear it, and the aarch64 pdi_jit_post path does
 * not clear it either, so polling it would make the block return, find the bit
 * still set, re-dispatch, and spin forever.  Polling only actionable bits
 * guarantees the dispatcher clears every bit that can fire the poll. */
#define PPCR_SPCFLAGS_POLL_MASK 0x0F



/* Host register assignments */
#define RSTATE  20   /* x20 = regs pointer (callee-saved) */
#define RMEMBASE 19  /* x19 = guest-memory base (VMBaseDiff); callee-saved */
#define RTMP0    0
#define RTMP1    1
#define RTMP2    2
#define RTMP3    3

/* ---- Guest-memory addressing model ----
 *
 * SheepShaver maps Mac RAM/ROM into the host address space via one of two
 * models (see SheepShaver/src/Unix/sysdeps.h and cpu/vm.hpp):
 *
 *   REAL_ADDRESSING   : host pointer == 32-bit guest address (VMBaseDiff = 0).
 *                       Used on Linux/native builds without NATMEM_OFFSET.
 *   DIRECT_ADDRESSING : host = NATMEM_OFFSET + (uint32)guest_addr.
 *                       Used whenever NATMEM_OFFSET is configured (macOS arm64).
 *
 * The JIT computes a 32-bit guest effective address in a temp register, then
 * accesses host memory as [RMEMBASE, EA]. RMEMBASE is loaded once per block in
 * the prologue with JIT_MEM_BASE:
 *   - DIRECT : NATMEM_OFFSET (a fixed 64-bit constant).
 *   - REAL   : 0, so [0, EA] == [EA] and the codegen is identical to before.
 *
 * This mirrors sysdeps.h's REAL/DIRECT selection so a single codegen path works
 * on both Linux and macOS with no behavioral #ifdefs in the emitters. */
#if defined(REAL_ADDRESSING)
  #define JIT_MEM_BASE ((uint64_t)0)
#elif defined(DIRECT_ADDRESSING) && defined(NATMEM_OFFSET)
  #define JIT_MEM_BASE ((uint64_t)NATMEM_OFFSET)
#elif defined(NATMEM_OFFSET)
  #define JIT_MEM_BASE ((uint64_t)NATMEM_OFFSET)
#else
  /* No addressing macros visible (standalone harness compile): default to the
   * REAL model so the JIT keeps treating guest EAs as host pointers. */
  #define JIT_MEM_BASE ((uint64_t)0)
#endif

/* emit_load_mem_base() loads JIT_MEM_BASE into RMEMBASE; defined after the
 * shared emit_load_imm64() helper further below. */
static void emit_load_mem_base(void);

/* FPR offsets: FPR[n] at offset 128 + n*8 (each is a 64-bit double) */
#define PPCR_FPR(n) ((uint32_t)(256 + (n) * 8))

/* ARM64 FP register helpers */
/* LDR Dt, [Xn, #imm] (64-bit FP load, unsigned offset scaled by 8) */
static void emit_load_fpr(int fd, int fpr_num) {
	/* LDR Dt, [RSTATE, #offset] */
	uint32_t off = PPCR_FPR(fpr_num);
	emit32(0xFD400000 | ((off / 8) << 10) | (RSTATE << 5) | fd);
}

/* STR Dt, [Xn, #imm] */
static void emit_store_fpr(int fs, int fpr_num) {
	uint32_t off = PPCR_FPR(fpr_num);
	emit32(0xFD000000 | ((off / 8) << 10) | (RSTATE << 5) | fs);
}

/* ---- PPC instruction field extraction ---- */
static inline uint32_t PPC_OPC(uint32_t op)  { return op >> 26; }
static inline uint32_t PPC_RD(uint32_t op)   { return (op >> 21) & 0x1F; }
static inline uint32_t PPC_RS(uint32_t op)   { return (op >> 21) & 0x1F; }
static inline uint32_t PPC_RA(uint32_t op)   { return (op >> 16) & 0x1F; }
static inline uint32_t PPC_RB(uint32_t op)   { return (op >> 11) & 0x1F; }
static inline int16_t  PPC_SIMM(uint32_t op) { return (int16_t)(op & 0xFFFF); }
static inline uint16_t PPC_UIMM(uint32_t op) { return (uint16_t)(op & 0xFFFF); }
static inline uint32_t PPC_XO(uint32_t op)   { return (op >> 1) & 0x3FF; }

/* ---- Emit helpers ---- */

/* ---- Register allocator ----
 * Maps PPC GPRs to ARM64 callee-saved registers x21–x28 (8 slots).
 * Eliminates redundant LDR/STR when the same GPR is used across
 * consecutive instructions within a block.
 */
#define RA_NUM_REGS  8
#define RA_FIRST_REG 21  /* x21 */

static int  ra_ppc_to_host[32];   /* PPC GPR → ARM64 reg, -1 = not cached */
static int  ra_host_to_ppc[RA_NUM_REGS]; /* ARM64 slot → PPC GPR, -1 = free */
static bool ra_dirty[RA_NUM_REGS];       /* true = cached value modified, needs writeback */
static int  ra_lru[RA_NUM_REGS];         /* access counter for LRU eviction */
static int  ra_clock = 0;                /* monotonic access counter */

static void ra_reset(void) {
	for (int i = 0; i < 32; i++) ra_ppc_to_host[i] = -1;
	for (int i = 0; i < RA_NUM_REGS; i++) {
		ra_host_to_ppc[i] = -1;
		ra_dirty[i] = false;
		ra_lru[i] = 0;
	}
	ra_clock = 0;
}

/* Evict one slot: write back if dirty, mark free */
static void ra_evict(int slot) {
	int ppc = ra_host_to_ppc[slot];
	if (ppc >= 0) {
		if (ra_dirty[slot])
			a64_str_w_imm(RA_FIRST_REG + slot, RSTATE, PPCR_GPR(ppc));
		ra_ppc_to_host[ppc] = -1;
	}
	ra_host_to_ppc[slot] = -1;
	ra_dirty[slot] = false;
}

/* Find LRU slot to evict */
static int ra_find_lru(void) {
	int best = 0;
	for (int i = 1; i < RA_NUM_REGS; i++)
		if (ra_lru[i] < ra_lru[best]) best = i;
	return best;
}

/* Get ARM64 reg for reading PPC GPR n (loads from struct if not cached) */
static int ra_load(int n) {
	int host = ra_ppc_to_host[n];
	if (host >= 0) {
		ra_lru[host - RA_FIRST_REG] = ++ra_clock;
		return host;
	}
	/* Not cached — load from struct and cache it */
	int slot = -1;
	for (int i = 0; i < RA_NUM_REGS; i++) {
		if (ra_host_to_ppc[i] < 0) { slot = i; break; }
	}
	if (slot < 0) {
		slot = ra_find_lru();
		ra_evict(slot);
	}
	host = RA_FIRST_REG + slot;
	a64_ldr_w_imm(host, RSTATE, PPCR_GPR(n));
	ra_ppc_to_host[n] = host;
	ra_host_to_ppc[slot] = n;
	ra_dirty[slot] = false;
	ra_lru[slot] = ++ra_clock;
	return host;
}

/* Get ARM64 reg for writing PPC GPR n (marks dirty, allocates if needed) */
static int ra_store(int n) {
	int host = ra_ppc_to_host[n];
	if (host >= 0) {
		int slot = host - RA_FIRST_REG;
		ra_dirty[slot] = true;
		ra_lru[slot] = ++ra_clock;
		return host;
	}
	/* Allocate — same as load but don't bother loading old value */
	int slot = -1;
	for (int i = 0; i < RA_NUM_REGS; i++) {
		if (ra_host_to_ppc[i] < 0) { slot = i; break; }
	}
	if (slot < 0) {
		slot = ra_find_lru();
		ra_evict(slot);
	}
	host = RA_FIRST_REG + slot;
	ra_ppc_to_host[n] = host;
	ra_host_to_ppc[slot] = n;
	ra_dirty[slot] = true;
	ra_lru[slot] = ++ra_clock;
	return host;
}

/* Flush all dirty cached regs back to struct (call at block exit) */
static void ra_flush_all(void) {
	for (int i = 0; i < RA_NUM_REGS; i++) {
		if (ra_host_to_ppc[i] >= 0 && ra_dirty[i])
			a64_str_w_imm(RA_FIRST_REG + i, RSTATE, PPCR_GPR(ra_host_to_ppc[i]));
	}
}

static void emit_load_gpr(int rd, int n) {
	/* DISABLED: boot hang regression. Direct struct access. */
	a64_ldr_w_imm(rd, RSTATE, PPCR_GPR(n));
}

static void emit_store_gpr(int rs, int n) {
	/* DISABLED: boot hang regression. Direct struct access. */
	a64_str_w_imm(rs, RSTATE, PPCR_GPR(n));
}

/* 64-bit GPR access for G5/PPC64 instructions.
   Uses gpr[n] (low 32) + gpr_hi[n] (high 32) as a split 64-bit register.
   On little-endian ARM64: load low word, load high word, combine. */
static void emit_load_gpr64_tmp(int xd, int n, int tmp) {
	/* LDR Wd, [RSTATE, #gpr_lo] — low 32 bits, zero-extends to Xd */
	a64_ldr_w_imm(xd, RSTATE, PPCR_GPR(n));
	/* LDR Wtmp, [RSTATE, #gpr_hi] — high 32 bits, zero-extends to Xtmp */
	a64_ldr_w_imm(tmp, RSTATE, PPCR_GPR_HI(n));
	/* Combine into 64-bit Xd: Xd = (hi << 32) | lo
	 * ORR Xd, Xd, Xtmp, LSL #32: imm6=32 at bits 15-10 of the ORR shifted-reg form */
	emit32(0xAA000000 | (tmp << 16) | (0x20 << 10) | (xd << 5) | xd);
	/* NOTE: the ORR above is the correct and complete 64-bit combine.
	 * No BFI needed here. */
}

static void emit_load_gpr64(int xd, int n) {
	int tmp = (xd == RTMP0) ? RTMP1 : RTMP0;
	emit_load_gpr64_tmp(xd, n, tmp);
}

static void emit_store_gpr64(int xs, int n) {
	/* Store low 32: STR Ws, [RSTATE, #gpr_lo] */
	a64_str_w_imm(xs, RSTATE, PPCR_GPR(n));
	/* Store high 32: LSR Xtmp, Xs, #32; STR Wtmp, [RSTATE, #gpr_hi]
	 * LSR Xd,Xn,#32 = UBFM Xd,Xn,#immr=32,#imms=63 = 0xD360FC00|(Xn<<5)|Xd */
	int tmp = (xs == RTMP0) ? RTMP1 : RTMP0;
	emit32(0xD360FC00 | (xs << 5) | tmp); /* LSR Xtmp, Xs, #32 */
	a64_str_w_imm(tmp, RSTATE, PPCR_GPR_HI(n));
}

/* Emit LSL Xd,Xn,#imm (64-bit logical shift left, 1 <= imm <= 63) */
static void emit_lsl64_imm(int rd, int rn, int imm) {
	if (imm <= 0 || imm >= 64) return;
	int immr = (64 - imm) & 63;
	int imms = 63 - imm;
	emit32(0xD3400000 | (immr << 16) | (imms << 10) | (rn << 5) | rd);
}

/* Emit LSR Xd,Xn,#imm (64-bit logical shift right, 1 <= imm <= 63) */
static void emit_lsr64_imm(int rd, int rn, int imm) {
	if (imm <= 0 || imm >= 64) return;
	emit32(0xD3400000 | (imm << 16) | (63 << 10) | (rn << 5) | rd);
}

/* Load a 64-bit immediate into a 64-bit register (MOVZ + up to 3 MOVK) */
static void emit_load_imm64(int rd, uint64_t imm) {
	uint16_t p[4];
	for (int i = 0; i < 4; i++) p[i] = (imm >> (i * 16)) & 0xFFFF;
	int first = 0;
	for (int i = 0; i < 4; i++) { if (p[i]) { first = i; break; } }
	a64_movz(rd, p[first], first);
	for (int i = 0; i < 4; i++)
		if (i != first && p[i]) a64_movk(rd, p[i], i);
}

/* Load the guest-memory base (JIT_MEM_BASE) into RMEMBASE.
 * For REAL (JIT_MEM_BASE==0) this is a single MOVZ #0 so [RMEMBASE, EA]==[EA]. */
static void emit_load_mem_base(void) {
	emit_load_imm64(RMEMBASE, JIT_MEM_BASE);
}

static void emit_load_imm32(int rd, int32_t imm) {
	uint32_t u = (uint32_t)imm;
	uint16_t lo = u & 0xFFFF;
	uint16_t hi = (u >> 16) & 0xFFFF;
	if (imm >= 0 && imm < 65536) {
		a64_movz(rd, lo, 0);
	} else if (imm < 0 && imm >= -65536) {
		emit32(0x12800000 | ((uint32_t)(uint16_t)(~u) << 5) | rd); /* MOVN Wd, #~u */
	} else {
		a64_movz(rd, lo, 0);
		if (hi) a64_movk(rd, hi, 1);
	}
}

/* Update CR0 based on a 32-bit result in ARM64 register 'rd'.
   CR0: bit31=LT(negative), bit30=GT(positive nonzero), bit29=EQ(zero), bit28=SO(from XER) */
/* Read XER[SO] (byte at offset PPCR_XER_SO) into register rd as 0 or 1 */
static void emit_read_xer_so(int rd) {
	emit32(0x39400000 | (PPCR_XER_SO << 10) | (RSTATE << 5) | rd); /* LDRB Wt, [Xn, #off] */
}

/* Merge XER[SO] into a CR field nibble in reg (at bit 0 position) */
static void emit_or_xer_so_into_cr_nibble(int reg) {
	int tmp = (reg == RTMP0) ? RTMP1 : RTMP0;
	emit_read_xer_so(tmp);
	emit32(0x2A000000 | (tmp << 16) | (reg << 5) | reg); /* ORR Wd, Wreg, Wtmp */
}

static void emit_update_cr0(int result_reg) {
	/* Simple approach: compute CR0 nibble with conditional instructions */
	/* Compare result with 0 */
	emit32(0x7100001F | (result_reg << 5)); /* CMP Wn, #0 */
	/* CR0 = 0 by default */
	a64_movz(RTMP2, 0, 0);
	/* MOV W(RTMP0), #8; MOV W(RTMP1), #4; MOV W(RTMP2), #2 */
	/* CSEL based on condition */
	/* Simplest: use three conditional moves */
	a64_movz(RTMP2, 0, 0);
	emit_load_imm32(RTMP0, 8); /* LT value */
	emit_load_imm32(RTMP1, 4); /* GT value */
	/* CSEL RTMP2, RTMP0, RTMP2, LT (if signed less than) */
	emit32(0x1A800000 | (RTMP2 << 16) | (0xB << 12) | (RTMP0 << 5) | RTMP2); /* CSEL Wd,Wn,Wm,LT */
	/* CSEL RTMP2, RTMP1, RTMP2, GT (if signed greater than) */
	emit32(0x1A800000 | (RTMP2 << 16) | (0xC << 12) | (RTMP1 << 5) | RTMP2); /* CSEL Wd,Wn,Wm,GT */
	/* If EQ, set to 2 */
	emit_load_imm32(RTMP0, 2);
	emit32(0x1A800000 | (RTMP2 << 16) | (0x0 << 12) | (RTMP0 << 5) | RTMP2); /* CSEL Wd,Wn,Wm,EQ */
	/* OR in XER[SO] as bit 0 */
	emit_or_xer_so_into_cr_nibble(RTMP2);
	/* Shift nibble into CR0 position (bits 31:28) */
	emit_load_imm32(RTMP0, 28);
	emit32(0x1AC02000 | (RTMP0 << 16) | (RTMP2 << 5) | RTMP2); /* LSL Wd,Wn,Wm */
	/* Load CR, clear CR0 field, OR in new value */
	a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
	emit_load_imm32(RTMP1, 0x0FFFFFFF);
	emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND */
	emit32(0x2A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* ORR */
	a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
}


/* Read XER.CA byte into ARM64 register rd (0 or 1) */
static void emit_read_xer_ca(int rd) {
	emit32(0x39400000 | (PPCR_XER_CA << 10) | (RSTATE << 5) | rd); /* LDRB Wt, [Xn, #off] */
}

/* Write ARM64 carry flag (from last ADDS/SUBS) into XER.CA byte */
static void emit_write_xer_ca_from_carry(void) {
	/* CSET Wd, CS (carry set) — Wd = 1 if C=1, else 0 */
	emit32(0x1A9F37E0 | RTMP2); /* CSET W(RTMP2), CS = CSINC WZR, WZR, CC */
	emit32(0x39000000 | (PPCR_XER_CA << 10) | (RSTATE << 5) | RTMP2); /* STRB */
}

/* Set XER.CA byte to a specific value (0 or 1) */
static void emit_set_xer_ca(int val) {
	if (val) {
		emit_load_imm32(RTMP0, 1);
	} else {
		a64_movz(RTMP0, 0, 0);
	}
	emit32(0x39000000 | (PPCR_XER_CA << 10) | (RSTATE << 5) | RTMP0); /* STRB */
}

/* Write ARM64 overflow flag (from the last ADDS/SUBS) into XER.OV and
 * accumulate it into the sticky XER.SO byte.  Used by OE=1 arithmetic
 * (addco/subfco/addo...) — the 68k emulator inside the Mac ROM leans on
 * these to compute 68k condition codes, so they are extremely hot.
 * Must run while NZCV still holds the arithmetic flags (i.e. before any
 * CMP / flag-setting instruction such as emit_update_cr0).
 * Clobbers RTMP1 and RTMP2; preserves RTMP0 (the result) and NZCV. */
static void emit_write_xer_ov_so_from_overflow(void) {
	/* CSET Wd, VS — Wd = 1 if V=1 (signed overflow) */
	emit32(0x1A9F77E0 | RTMP2); /* CSET W(RTMP2), VS = CSINC WZR,WZR,VC */
	emit32(0x39000000 | (PPCR_XER_OV << 10) | (RSTATE << 5) | RTMP2); /* STRB → OV */
	/* SO is sticky: SO |= OV */
	emit32(0x39400000 | (PPCR_XER_SO << 10) | (RSTATE << 5) | RTMP1); /* LDRB ← SO */
	emit32(0x2A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);        /* ORR */
	emit32(0x39000000 | (PPCR_XER_SO << 10) | (RSTATE << 5) | RTMP1); /* STRB → SO */
}

/* Sync PPC FPSCR rounding mode (bits 30-31) to ARM64 FPCR (bits 22-23).
   PPC RN: 0=nearest, 1=toward zero, 2=+inf, 3=-inf
   ARM64 RMode: 0=nearest, 3=toward zero, 1=+inf, 2=-inf
   Called after any FPSCR write that might change the RN field. */
static void emit_sync_fpscr_rounding(void) {
	a64_ldr_w_imm(RTMP0, RSTATE, PPCR_FPSCR);
	/* Extract PPC RN field: bits 30-31 (least significant 2 bits of FPSCR) */
	emit32(0x12000400 | (RTMP0 << 5) | RTMP0); /* AND Wd, Wn, #3 */
	/* Map PPC RN to ARM64 RMode via conditional moves */
	a64_mov_reg(RTMP1, RTMP0); /* save PPC RN */
	a64_movz(RTMP0, 0, 0); /* default: ARM nearest (PPC 0 → ARM 0) */
	emit_load_imm32(RTMP2, 3);
	emit32(0x7100043F | (RTMP1 << 5)); /* CMP Wn, #1 */
	emit32(0x1A800000 | (RTMP0 << 16) | (0x0 << 12) | (RTMP2 << 5) | RTMP0); /* CSEL 3 if EQ (PPC zero→ARM 3) */
	emit_load_imm32(RTMP2, 1);
	emit32(0x7100083F | (RTMP1 << 5)); /* CMP Wn, #2 */
	emit32(0x1A800000 | (RTMP0 << 16) | (0x0 << 12) | (RTMP2 << 5) | RTMP0); /* CSEL 1 if EQ (PPC +inf→ARM 1) */
	emit_load_imm32(RTMP2, 2);
	emit32(0x71000C3F | (RTMP1 << 5)); /* CMP Wn, #3 */
	emit32(0x1A800000 | (RTMP0 << 16) | (0x0 << 12) | (RTMP2 << 5) | RTMP0); /* CSEL 2 if EQ (PPC -inf→ARM 2) */
	/* Shift to FPCR RMode position (bits 22-23) */
	emit_load_imm32(RTMP1, 22);
	emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* LSL */
	/* Read FPCR, clear RMode bits, set new value */
	emit32(0xD53B4400 | RTMP1); /* MRS Xt, FPCR */
	emit_load_imm32(RTMP2, ~(3 << 22));
	emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* AND: clear RMode */
	emit32(0x2A000000 | (RTMP0 << 16) | (RTMP1 << 5) | RTMP1); /* ORR: set new RMode */
	emit32(0xD51B4400 | RTMP1); /* MSR FPCR, Xt */
}


/* Load effective address: if rA==0, use 0; otherwise load GPR[rA].
   Always puts result in RTMP0. */
static void emit_load_ea_base(int ra_num) {
	if (ra_num == 0) {
		a64_movz(RTMP0, 0, 0);
	} else {
		emit_load_gpr(RTMP0, ra_num);
	}
}

/* ---- AltiVec Vector Register helpers ---- */
/* VR[n] at offset 384 + n*16, each 128-bit (16 bytes) */
#define PPCR_VR(n) ((uint32_t)(512 + (n) * 16))

/* Load 128-bit vector register into ARM64 Q register (NEON) */
static void emit_load_vr(int qd, int vr_num) {
	uint32_t off = PPCR_VR(vr_num);
	/* LDR Qt, [Xn, #imm] — 128-bit vector load, unsigned offset scaled by 16 */
	emit32(0x3DC00000 | ((off / 16) << 10) | (RSTATE << 5) | qd);
}

/* Store ARM64 Q register into VR[n] */
static void emit_store_vr(int qs, int vr_num) {
	uint32_t off = PPCR_VR(vr_num);
	emit32(0x3D800000 | ((off / 16) << 10) | (RSTATE << 5) | qs);
}

/* AltiVec field extraction */
static inline uint32_t VR_VD(uint32_t op) { return (op >> 21) & 0x1F; }
static inline uint32_t VR_VA(uint32_t op) { return (op >> 16) & 0x1F; }
static inline uint32_t VR_VB(uint32_t op) { return (op >> 11) & 0x1F; }
static inline uint32_t VR_VC(uint32_t op) { return (op >> 6) & 0x1F; }

/* Emit: store next_pc to regs->pc, epilogue, ret */
static void emit_epilogue_with_pc(uint32_t next_pc) {
	/* Flush register allocator: write all dirty cached GPRs back to struct */
	ra_flush_all();
	emit_load_imm32(RTMP0, (int32_t)next_pc);
	a64_str_w_imm(RTMP0, RSTATE, PPCR_PC);
#if JIT_BLOCK_CHAINING
	if (!jit_chain_runtime_disabled()) {
		/* Compile-time chaining: if the target PC is already in the JIT block
		 * cache and has a chain entry, branch directly to it instead of
		 * restoring callee-saved registers and returning to the dispatch loop.
		 * The callee-saved registers (x19–x28) remain valid on the stack from
		 * the current block's prologue — the chained block re-uses that frame. */
		const struct jit_bc_entry *chain_target = jit_bc_lookup(next_pc);
		if (chain_target && chain_target->chain_code) {
			int32_t off = (int32_t)((uint8_t *)chain_target->chain_code - (uint8_t *)jit_code_ptr);
			if (off >= -(1 << 25) && off < (1 << 25)) {
				/* Record the site BEFORE emitting B so that range-based invalidation
				 * can find and revert this patch if the target block is invalidated. */
				record_chain_site(next_pc, jit_code_ptr);
				chain_site_pool[chain_site_pool_next - 1].patched = true;
				emit32(0x14000000 | ((off >> 2) & 0x3FFFFFF)); /* B <offset> */
				return; /* no LDP+RET: caller re-uses current stack frame */
			}
		}
		/* Runtime back-patching: record this epilogue location so that when
		 * next_pc is compiled later, the first LDP can be patched to B chain_code.
		 * patch_loc = address of the first LDP instruction we are about to emit. */
		record_chain_site(next_pc, jit_code_ptr);
	}
#endif /* JIT_BLOCK_CHAINING */
	/* Standard epilogue: restore callee-saved regs and return to dispatch */
	a64_ldp_post(27, 28, A64_SP, 16);
	a64_ldp_post(25, 26, A64_SP, 16);
	a64_ldp_post(23, 24, A64_SP, 16);
	a64_ldp_post(21, 22, A64_SP, 16);
	a64_ldp_post(19, RSTATE, A64_SP, 16);
	a64_ldp_post(A64_FP, A64_LR, A64_SP, 16);
	a64_ret();
}

/* Inline interpreter-call bridge (defined in ppc-cpu.cpp). Decodes and executes
 * ONE PPC instruction through the interpreter handler, advancing regs->pc.
 * This is the dyngen do_generic / gen_invoke equivalent — see
 * docs/superpowers/research/2026-06-02-dyngen-mechanisms.md GAP 3. */
extern "C" void ppc_jit_interp_one(uint32_t opcode, uint32_t pc_val);

/* Emit a bare epilogue (LDP x6 + RET) WITHOUT storing a PC.
 * Used after an inline interpreter call (the bridge already advanced regs->pc)
 * and as the return tail of the block-entry spcflags poll.
 * This is emit_epilogue_with_pc minus the PC store and the chaining logic. */
static void emit_bare_epilogue(void) {
	a64_ldp_post(27, 28, A64_SP, 16);
	a64_ldp_post(25, 26, A64_SP, 16);
	a64_ldp_post(23, 24, A64_SP, 16);
	a64_ldp_post(21, 22, A64_SP, 16);
	a64_ldp_post(19, RSTATE, A64_SP, 16);
	a64_ldp_post(A64_FP, A64_LR, A64_SP, 16);
	a64_ret();
}

/* Emit the block-entry spcflags poll (dyngen gen_start equivalent).
 *
 * Placed at the chain entry point, immediately after the prologue and before
 * the block body.  Loads the spcflags mask, masks to the actionable bits
 * (PPCR_SPCFLAGS_POLL_MASK), and:
 *   - if zero  -> falls through into the block body (the common, fast case);
 *   - if nonzero -> stores this block's start PC into regs.pc and returns to
 *     the C dispatcher via the standard epilogue.  The dispatcher runs
 *     check_spcflags() (which clears/handles every actionable bit) and
 *     re-dispatches the same PC, at which point the poll passes.
 *
 * With JIT_BLOCK_CHAINING=0 the poll runs on every normal (ABI) block entry —
 * it never changes correct behaviour (the dispatcher already polled spcflags
 * before entering), but it proves the guard returns to C whenever a flag is
 * set, which is the safety valve required before block chaining is enabled.
 *
 * Uses RTMP0 only; emitted before any register-allocator state exists for the
 * block, so no flush is needed. */
static void emit_entry_spcflags_poll(uint32_t block_start_pc) {
	/* LDR  Wtmp0, [RSTATE, #PPCR_SPCFLAGS]  — load spcflags.mask */
	a64_ldr_w_imm(RTMP0, RSTATE, PPCR_SPCFLAGS);
	/* AND  Wtmp0, Wtmp0, #PPCR_SPCFLAGS_POLL_MASK (0x0F) — actionable bits only */
	emit32(0x12000C00 | (RTMP0 << 5) | RTMP0);
	/* CBZ  Wtmp0, <body> — skip the return sequence when no flags pending.
	 * Patched once the return sequence length is known. */
	uint32_t *cbz_loc = jit_code_ptr;
	emit32(0); /* placeholder CBZ */
	/* Flags pending: store block start PC and return to the dispatcher. */
	emit_load_imm32(RTMP0, (int32_t)block_start_pc);
	a64_str_w_imm(RTMP0, RSTATE, PPCR_PC);
	/* Standard epilogue: restore callee-saved regs and return to dispatch. */
	emit_bare_epilogue();
	/* Patch the CBZ to jump here (the block body follows). */
	int32_t off = (int32_t)((uint8_t *)jit_code_ptr - (uint8_t *)cbz_loc);
	*cbz_loc = 0x34000000 | (((off >> 2) & 0x7FFFF) << 5) | RTMP0; /* CBZ RTMP0, body */
}

/* Emit an inline call to the interpreter handler for one opcode at cur_pc,
 * then a bare epilogue. ABI: the BLR clobbers x0-x17 and NZCV but preserves
 * x19-x28, so RSTATE (x20) and RMEMBASE (x19) survive. All guest state is in
 * memory (the register allocator is disabled), so nothing live is lost.
 *
 * Caller MUST have flushed lazy CR0 and the register allocator first, because
 * lazy CR0 lives in NZCV which the call clobbers.
 *
 * Stack alignment: the block prologue pushes 6 STP pairs = 96 bytes (a multiple
 * of 16) from a 16-aligned SP, so SP is 16-aligned at the BLR. No extra
 * adjustment needed. */
static void emit_inline_interp_call(uint32_t op, uint32_t cur_pc) {
	/* Sync guest PC into regs->pc before the call. The bridge also sets it,
	 * but emitting it here keeps the contract explicit and matches the plan.
	 * Order matters: do this first (uses RTMP0 as scratch) before loading the
	 * argument registers, since w0 must end holding the opcode. */
	emit_load_imm32(RTMP0, (int32_t)cur_pc);
	a64_str_w_imm(RTMP0, RSTATE, PPCR_PC);
	/* Arguments: w0 = opcode, w1 = cur_pc.  RTMP0==x0, RTMP1==x1. */
	emit_load_imm32(RTMP0, (int32_t)op);   /* w0 = opcode */
	emit_load_imm32(RTMP1, (int32_t)cur_pc); /* w1 = cur_pc */
	/* Load the bridge address into x16 (intra-procedure-call scratch, safe to
	 * clobber across the call) and call it. */
	emit_load_imm64(16, (uint64_t)(uintptr_t)&ppc_jit_interp_one);
	a64_blr(16);
	/* The bridge advanced regs->pc; do not store a new PC, do not chain. */
	emit_bare_epilogue();
}

/* Emit: if lk=1, save pc+4 to PPCR_LR (bcl / bctrl / blrl semantics) */
static void emit_save_lr_if_link(uint32_t cur_pc, bool lk) {
	if (!lk) return;
	emit_load_imm32(RTMP0, (int32_t)(cur_pc + 4));
	a64_str_w_imm(RTMP0, RSTATE, PPCR_LR);
}

/* ---- Instruction offset map for intra-block branches ---- */
static uint32_t *insn_code_offset[512];  /* ARM64 code ptr at start of each PPC insn */
static uint32_t  insn_ppc_pc[512];       /* PPC PC of each compiled instruction */
static int       insn_count = 0;

/* ---- Lazy CR0 flags ----
 * Instead of materializing CR0 on every Rc=1 instruction (~15 ARM64 insns),
 * we defer the computation until CR0 is actually read (branch, mfcr) or
 * the block exits. The last Rc=1 result is kept as a CMP against zero in
 * the ARM64 NZCV flags register — we just remember that NZCV is valid.
 *
 * lazy_cr0_valid: true if ARM64 NZCV currently reflects the last Rc=1 result
 * lazy_cr0_reg: the ARM64 register that was CMP'd (needed for re-CMP after
 *               any instruction that clobbers NZCV)
 */
static bool lazy_cr0_valid = false;
static int  lazy_cr0_reg = -1;  /* ARM64 reg holding last Rc=1 result, -1 = none */

/* Materialize CR0 from current NZCV state (call only when lazy_cr0_valid) */
static void emit_materialize_cr0(void) {
	if (!lazy_cr0_valid) return;
	/* Re-CMP if needed — NZCV may have been clobbered by intervening insns.
	 * For safety, always re-CMP the saved register. */
	if (lazy_cr0_reg >= 0)
		emit32(0x7100001F | (lazy_cr0_reg << 5)); /* CMP Wn, #0 */

	/* Build CR0 nibble from ARM64 condition codes:
	 * CR0.LT = N, CR0.GT = !N && !Z, CR0.EQ = Z, CR0.SO = XER.SO */
	a64_movz(RTMP2, 0, 0);
	emit_load_imm32(RTMP0, 8); /* LT */
	emit_load_imm32(RTMP1, 4); /* GT */
	emit32(0x1A800000 | (RTMP2 << 16) | (0xB << 12) | (RTMP0 << 5) | RTMP2); /* CSEL RTMP2,RTMP0,RTMP2,LT */
	emit32(0x1A800000 | (RTMP2 << 16) | (0xC << 12) | (RTMP1 << 5) | RTMP2); /* CSEL RTMP2,RTMP1,RTMP2,GT */
	emit_load_imm32(RTMP0, 2); /* EQ */
	emit32(0x1A800000 | (RTMP2 << 16) | (0x0 << 12) | (RTMP0 << 5) | RTMP2); /* CSEL RTMP2,RTMP0,RTMP2,EQ */
	emit_or_xer_so_into_cr_nibble(RTMP2);
	emit_load_imm32(RTMP0, 28);
	emit32(0x1AC02000 | (RTMP0 << 16) | (RTMP2 << 5) | RTMP2); /* LSL */
	a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
	emit_load_imm32(RTMP1, 0x0FFFFFFF);
	emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND clear CR0 */
	emit32(0x2A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* ORR merge */
	a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
	lazy_cr0_valid = false;
	lazy_cr0_reg = -1;
}

/* Mark CR0 as pending — the result in 'reg' will be used to compute CR0 later */
static void lazy_update_cr0(int result_reg) {
	/* DISABLED: boot hang regression. Materialize immediately. */
	emit_update_cr0(result_reg);
}

/* Ensure CR0 is materialized — call before branches testing CR0, mfcr, block exits */
static void lazy_flush_cr0(void) {
	if (lazy_cr0_valid)
		emit_materialize_cr0();
}

/* Get ARM64 reg for writing PPC GPR n (marks dirty, allocates if needed) */

/* Find the ARM64 code offset for a PPC PC within the current block */
/* Find the ARM64 code pointer for a FORWARD intra-block branch target.
 *
 * MUST only be called with forward targets (target_pc > current instruction pc).
 * Backward targets must NOT be passed: insn_code_offset[] covers only the block
 * body (after the spcflags poll at chain_entry_start). Branching to an address in
 * this table for a backward target would skip the poll, creating a closed ARM64
 * loop that starves interrupt delivery. The name encodes this contract. */
static uint32_t *find_forward_code_for_pc(uint32_t target_pc) {
	for (int i = 0; i < insn_count; i++) {
		if (insn_ppc_pc[i] == target_pc)
			return insn_code_offset[i];
	}
	return NULL;
}


/* ---- Opcode miss tracking ---- */
static uint32_t jit_miss_count[64] = {0};  /* primary opcode histogram */
static uint32_t jit_xo_miss[1024] = {0};   /* XO opcode histogram for opc=31 */
static uint32_t jit_total_miss = 0;
static uint32_t jit_total_hit = 0;
static uint32_t jit_blocks_attempted = 0;
static uint32_t jit_blocks_complete = 0;
static uint32_t jit_last_fail_op = 0;
static uint32_t jit_cum_fail_opc[64] = {0};
static uint32_t jit_cum_fail_xo31[1024] = {0};
static uint32_t jit_cum_fail_total = 0;

static void jit_report_misses(void) {
	if (jit_total_miss == 0 && jit_total_hit == 0) return;
	fprintf(stderr, "PPC-JIT-A64: blocks=%u complete=%u (%.1f%%)\n",
		jit_blocks_attempted, jit_blocks_complete,
		jit_blocks_attempted ? jit_blocks_complete * 100.0 / jit_blocks_attempted : 0.0);
	fprintf(stderr, "PPC-JIT-A64: hit=%u miss=%u (%.1f%% coverage)\n",
		jit_total_hit, jit_total_miss,
		jit_total_hit * 100.0 / (jit_total_hit + jit_total_miss));
	fprintf(stderr, "PPC-JIT-A64: top missed primary opcodes:\n");
	/* Sort and print top 10 */
	for (int pass = 0; pass < 10; pass++) {
		uint32_t max_v = 0; int max_i = -1;
		for (int i = 0; i < 64; i++) {
			if (jit_miss_count[i] > max_v) { max_v = jit_miss_count[i]; max_i = i; }
		}
		if (max_i < 0 || max_v == 0) break;
		fprintf(stderr, "  opc=%d: %u misses\n", max_i, max_v);
		jit_miss_count[max_i] = 0; /* clear for next pass */
	}
	fprintf(stderr, "PPC-JIT-A64: top missed XO opcodes (opc=31):\n");
	for (int pass = 0; pass < 10; pass++) {
		uint32_t max_v = 0; int max_i = -1;
		for (int i = 0; i < 1024; i++) {
			if (jit_xo_miss[i] > max_v) { max_v = jit_xo_miss[i]; max_i = i; }
		}
		if (max_i < 0 || max_v == 0) break;
		fprintf(stderr, "  XO=%d: %u misses\n", max_i, max_v);
		jit_xo_miss[max_i] = 0;
	}
}

/* ---- Compile one PPC instruction ---- */
static bool compile_one(uint32_t op, uint32_t pc) {
	uint32_t opc = PPC_OPC(op);
	uint32_t rd, ra, rb;
	int16_t simm;
	uint16_t uimm;

	/* SS_JIT_SKIP_OPC: force interpreter for specific primary opcodes */
	{
		static uint8_t skip_opc[64] = {0};
		static int checked = 0;
		if (!checked) {
			checked = 1;
			const char *env = getenv("SS_JIT_SKIP_OPC");
			if (env) {
				char buf[256];
				strncpy(buf, env, sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
				char *tok = strtok(buf, ",");
				while (tok) {
					int n = atoi(tok);
					if (n >= 0 && n < 64) skip_opc[n] = 1;
					tok = strtok(NULL, ",");
				}
				fprintf(stderr, "[JIT] SS_JIT_SKIP_OPC: opcodes");
				for (int i = 0; i < 64; i++) if (skip_opc[i]) fprintf(stderr, " %d", i);
				fprintf(stderr, " forced to interpreter\n");
			}
		}
		if (skip_opc[opc]) return false;
	}

	/* SS_JIT_SKIP_XO: force interpreter for specific XO31 sub-opcodes */
	if (opc == 31) {
		static uint16_t skip_xo[1024] = {0};
		static int xo_checked = 0;
		if (!xo_checked) {
			xo_checked = 1;
			const char *env = getenv("SS_JIT_SKIP_XO");
			if (env) {
				char buf[256];
				strncpy(buf, env, sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
				char *tok = strtok(buf, ",");
				while (tok) {
					int n = atoi(tok);
					if (n >= 0 && n < 1024) skip_xo[n] = 1;
					tok = strtok(NULL, ",");
				}
				fprintf(stderr, "[JIT] SS_JIT_SKIP_XO: XO31 sub-opcodes");
				for (int i = 0; i < 1024; i++) if (skip_xo[i]) fprintf(stderr, " %d", i);
				fprintf(stderr, " forced to interpreter\n");
			}
		}
		uint32_t xo = (op >> 1) & 0x3FF;
		if (skip_xo[xo]) return false;
	}

	/* SS_JIT_SKIP_XO63: same as SKIP_XO but for opcode-63 (FP double) sub-opcodes.
	 * Uses both the 10-bit (X-form) and 5-bit (A-form) XO fields. */
	if (opc == 63) {
		static uint16_t skip_xo63[1024] = {0};
		static int xo63_checked = 0;
		if (!xo63_checked) {
			xo63_checked = 1;
			const char *env = getenv("SS_JIT_SKIP_XO63");
			if (env) {
				char buf[256];
				strncpy(buf, env, sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
				char *tok = strtok(buf, ",");
				while (tok) {
					int n = atoi(tok);
					if (n >= 0 && n < 1024) skip_xo63[n] = 1;
					tok = strtok(NULL, ",");
				}
				fprintf(stderr, "[JIT] SS_JIT_SKIP_XO63: XO63 sub-opcodes");
				for (int i = 0; i < 1024; i++) if (skip_xo63[i]) fprintf(stderr, " %d", i);
				fprintf(stderr, " forced to interpreter\n");
			}
		}
		uint32_t xo10 = (op >> 1) & 0x3FF;
		uint32_t xo5 = (op >> 1) & 0x1F;
		if (skip_xo63[xo10] || skip_xo63[xo5]) return false;
	}


	switch (opc) {

	case 14: /* addi / li */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		if (ra == 0) {
			emit_load_imm32(RTMP0, (int32_t)simm);
		} else {
			emit_load_gpr(RTMP0, ra);
			emit_load_imm32(RTMP1, (int32_t)simm);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADD Wd,Wn,Wm */
		}
		emit_store_gpr(RTMP0, rd);
		return true;

	case 15: /* addis / lis */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		if (ra == 0) {
			emit_load_imm32(RTMP0, (int32_t)simm << 16);
		} else {
			emit_load_gpr(RTMP0, ra);
			emit_load_imm32(RTMP1, (int32_t)simm << 16);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		}
		emit_store_gpr(RTMP0, rd);
		return true;

	case 23: /* rlwnm rA,rS,rB,MB,ME (rotate left word then AND mask) */
	{
		uint32_t rs = PPC_RS(op);
		ra = PPC_RA(op);
		rb = (op >> 11) & 0x1F;
		uint32_t mb = (op >> 6) & 0x1F;
		uint32_t me = (op >> 1) & 0x1F;
		emit_load_gpr(RTMP0, rs);
		emit_load_gpr(RTMP1, rb);
		/* Rotate left by rB: ROR Wd,Wn,Wm with negated count */
		emit32(0x4B0003E0 | (RTMP1 << 16) | RTMP1); /* NEG Wd,Wm (32-count) */
		emit32(0x1AC02C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ROR Wd,Wn,Wm */
		uint32_t mask = 0;
		if (mb <= me) { for (uint32_t i = mb; i <= me; i++) mask |= (0x80000000U >> i); }
		else { for (uint32_t i = 0; i <= me; i++) mask |= (0x80000000U >> i);
		       for (uint32_t i = mb; i <= 31; i++) mask |= (0x80000000U >> i); }
		if (mask != 0xFFFFFFFF) {
			emit_load_imm32(RTMP1, (int32_t)mask);
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		}
		emit_store_gpr(RTMP0, ra);
		if (op & 1) lazy_update_cr0(RTMP0);
		return true;
	}

	case 24: /* ori (and NOP = ori 0,0,0) */
		ra = PPC_RA(op); rd = PPC_RS(op); uimm = PPC_UIMM(op);
		if (rd == 0 && ra == 0 && uimm == 0) return true; /* NOP */
		emit_load_gpr(RTMP0, rd);
		if (uimm) {
			emit_load_imm32(RTMP1, (int32_t)(uint32_t)uimm);
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ORR */
		}
		emit_store_gpr(RTMP0, ra);
		return true;

	case 25: /* oris */
		ra = PPC_RA(op); rd = PPC_RS(op); uimm = PPC_UIMM(op);
		emit_load_gpr(RTMP0, rd);
		if (uimm) {
			emit_load_imm32(RTMP1, (int32_t)((uint32_t)uimm << 16));
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		}
		emit_store_gpr(RTMP0, ra);
		return true;

	case 26: /* xori rA,rS,UIMM */
		ra = PPC_RA(op); rd = PPC_RS(op); uimm = PPC_UIMM(op);
		emit_load_gpr(RTMP0, rd);
		if (uimm) {
			emit_load_imm32(RTMP1, (int32_t)(uint32_t)uimm);
			emit32(0x4A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* EOR */
		}
		emit_store_gpr(RTMP0, ra);
		return true;

	case 27: /* xoris rA,rS,UIMM */
		ra = PPC_RA(op); rd = PPC_RS(op); uimm = PPC_UIMM(op);
		emit_load_gpr(RTMP0, rd);
		if (uimm) {
			emit_load_imm32(RTMP1, (int32_t)((uint32_t)uimm << 16));
			emit32(0x4A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		}
		emit_store_gpr(RTMP0, ra);
		return true;

	case 28: /* andi. */
		ra = PPC_RA(op); rd = PPC_RS(op); uimm = PPC_UIMM(op);
		emit_load_gpr(RTMP0, rd);
		emit_load_imm32(RTMP1, (int32_t)(uint32_t)uimm);
		emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND */
		emit_store_gpr(RTMP0, ra);
		lazy_update_cr0(RTMP0); /* andi. always updates CR0 */
		return true;

	case 29: /* andis. — rA = rS & (UIMM << 16), always updates CR0 */
		ra = PPC_RA(op); rd = PPC_RS(op); uimm = PPC_UIMM(op);
		emit_load_gpr(RTMP0, rd);
		emit_load_imm32(RTMP1, (int32_t)((uint32_t)uimm << 16));
		emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND */
		emit_store_gpr(RTMP0, ra);
		lazy_update_cr0(RTMP0);
		return true;

	case 31: { /* XO-form extended opcodes */
		uint32_t xo = PPC_XO(op);
		rd = PPC_RD(op); ra = PPC_RA(op); rb = PPC_RB(op);
		switch (xo) {
		case 0: /* cmp (cmpw crD,rA,rB) — signed compare */
		{
			uint32_t crd = (op >> 23) & 0x7;
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | 0x1F); /* SUBS WZR,Wn,Wm */
			/* Build CR field with CSEL: signed LT/GT/EQ */
			a64_movz(RTMP0, 0, 0);
			emit_load_imm32(RTMP1, 8); /* LT */
			emit32(0x1A800000 | (RTMP0 << 16) | (0xB << 12) | (RTMP1 << 5) | RTMP0); /* CSEL LT */
			emit_load_imm32(RTMP1, 4); /* GT */
			emit32(0x1A800000 | (RTMP0 << 16) | (0xC << 12) | (RTMP1 << 5) | RTMP0); /* CSEL GT */
			emit_load_imm32(RTMP1, 2); /* EQ */
			emit32(0x1A800000 | (RTMP0 << 16) | (0x0 << 12) | (RTMP1 << 5) | RTMP0); /* CSEL EQ */
			emit_or_xer_so_into_cr_nibble(RTMP0);
			uint32_t shift = (7 - crd) * 4;
			if (shift) { emit_load_imm32(RTMP1, shift); emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			lazy_flush_cr0();
			a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CR);
			emit_load_imm32(RTMP2, ~(0xF << shift));
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
			emit32(0x2A000000 | (RTMP0 << 16) | (RTMP1 << 5) | RTMP1);
			a64_str_w_imm(RTMP1, RSTATE, PPCR_CR);
			return true;
		}
		case 266: /* add / add. */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, rd);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 40: /* subf (rD = rB - rA) */
			emit_load_gpr(RTMP0, rb);
			emit_load_gpr(RTMP1, ra);
			emit32(0x4B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SUB */
			emit_store_gpr(RTMP0, rd);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 28: /* and */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 444: /* or / or. (also mr) */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 316: /* xor */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x4A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 104: /* neg / neg. */
			emit_load_gpr(RTMP0, ra);
			emit32(0x4B0003E0 | (RTMP0 << 16) | RTMP0);
			emit_store_gpr(RTMP0, rd);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 26: /* cntlzw */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit32(0x5AC01000 | (RTMP0 << 5) | RTMP0); /* CLZ Wd, Wn */
			emit_store_gpr(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 922: /* extsh */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit32(0x13003C00 | (RTMP0 << 5) | RTMP0); /* SXTH Wd, Wn */
			emit_store_gpr(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 954: /* extsb */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit32(0x13001C00 | (RTMP0 << 5) | RTMP0); /* SXTB Wd, Wn */
			emit_store_gpr(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 715: /* mullw (with OE bit) */
		case 235: /* mullw */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x1B007C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* MUL Wd,Wn,Wm */
			emit_store_gpr(RTMP0, rd);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 491: /* divw */
		{
			/* PPC divw: if rB==0 or (rA==0x80000000 && rB==-1) the result is
			 * architecturally undefined; the reference interpreter returns
			 * (int32)rA >> 31 (all sign bits). Otherwise rA / rB. ARM SDIV
			 * alone gives 0 for div-by-0 and 0x80000000 for MIN/-1, which
			 * disagree with the interpreter, so both cases are guarded here.
			 *
			 * Register use: W0=rA, W1=rB, W2=SDIV result, W3=fallback/scratch. */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x1AC00C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP2); /* SDIV W2,W0,W1 */
			/* fallback = (int32)rA >> 31  → ASR W3, W0, #31 */
			emit32(0x13000000 | (31 << 16) | (0x1F << 10) | (RTMP0 << 5) | RTMP3);
			/* special if rB==0:  CMP W1,#0 ; CSEL W2 = (EQ) ? W3 : W2 */
			emit32(0x7100001F | (RTMP1 << 5));                         /* CMP W1, #0 */
			emit32(0x1A800000 | (RTMP2 << 16) | (RTMP3 << 5) | RTMP2); /* CSEL W2,W3,W2,EQ */
			/* special if MIN/-1: detect via (rA ^ 0x80000000) | (~rB) == 0.
			 * W0 = rA ^ 0x80000000; W1 = ~rB; W0 |= W1; CMP W0,#0; CSEL EQ. */
			emit_load_imm32(RTMP1, (int32_t)0x80000000);
			emit32(0x4A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* EOR W0,W0,#0x80000000(reg) */
			emit_load_gpr(RTMP1, rb);
			emit32(0x2A2103E1);                                        /* MVN W1, W1 (~rB) */
			emit32(0x2A010000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ORR W0,W0,W1 */
			emit32(0x7100001F | (RTMP0 << 5));                         /* CMP W0, #0 */
			emit32(0x1A800000 | (RTMP2 << 16) | (RTMP3 << 5) | RTMP2); /* CSEL W2,W3,W2,EQ */
			emit_store_gpr(RTMP2, rd);
			if (op & 1) lazy_update_cr0(RTMP2);
			return true;
		}
		case 19: /* mfcr rD */
			lazy_flush_cr0();
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			emit_store_gpr(RTMP0, rd);
			return true;
		case 144: /* mtcrf CRM,rS */
		{
			uint32_t crm = (op >> 12) & 0xFF;
			emit_load_gpr(RTMP0, PPC_RS(op));
			if (crm == 0xFF) {
				/* Move entire CR */
				a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
			} else {
				/* Selective CR field update */
				uint32_t mask = 0;
				for (int i = 0; i < 8; i++)
					if (crm & (0x80 >> i)) mask |= (0xF0000000U >> (i * 4));
				lazy_flush_cr0();
				a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CR);
				emit_load_imm32(RTMP2, (int32_t)~mask);
				emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* AND clear */
				emit_load_imm32(RTMP2, (int32_t)mask);
				emit32(0x0A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* AND source */
				emit32(0x2A000000 | (RTMP0 << 16) | (RTMP1 << 5) | RTMP1); /* ORR */
				a64_str_w_imm(RTMP1, RSTATE, PPCR_CR);
			}
			return true;
		}
		case 339: /* mfspr */
		{
			uint32_t spr = ((op >> 16) & 0x1F) | ((op >> 6) & 0x3E0);
			if (spr == 8) { /* LR */
				a64_ldr_w_imm(RTMP0, RSTATE, PPCR_LR);
				emit_store_gpr(RTMP0, rd);
				return true;
			}
			if (spr == 9) { /* CTR */
				a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CTR);
				emit_store_gpr(RTMP0, rd);
				return true;
			}
			if (spr == 1) { /* XER — pack {so,ov,ca,byte_count} into PPC format */
				emit32(0x39400000 | (PPCR_XER_SO << 10) | (RSTATE << 5) | RTMP0); /* LDRB so */
				emit_load_imm32(RTMP1, 31);
				emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* LSL #31 */
				emit32(0x39400000 | (PPCR_XER_OV << 10) | (RSTATE << 5) | RTMP1); /* LDRB ov */
				emit32(0x2A01781E | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ORR Wd, Wd, Wm LSL #30... */
				/* Actually: build it step by step */
				a64_movz(RTMP0, 0, 0);
				emit32(0x39400000 | (PPCR_XER_SO << 10) | (RSTATE << 5) | RTMP1);
				emit_load_imm32(RTMP2, 31); emit32(0x1AC02000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
				emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
				emit32(0x39400000 | (PPCR_XER_OV << 10) | (RSTATE << 5) | RTMP1);
				emit_load_imm32(RTMP2, 30); emit32(0x1AC02000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
				emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
				emit32(0x39400000 | (PPCR_XER_CA << 10) | (RSTATE << 5) | RTMP1);
				emit_load_imm32(RTMP2, 29); emit32(0x1AC02000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
				emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
				emit32(0x39400000 | (PPCR_XER_CNT << 10) | (RSTATE << 5) | RTMP1);
				emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
				emit_store_gpr(RTMP0, rd);
				return true;
			}
			/* Unknown SPR: fall back so the interpreter owns privileged/CPU-specific semantics. */
			return false;
		}
		case 467: /* mtspr */
		{
			uint32_t spr = ((op >> 16) & 0x1F) | ((op >> 6) & 0x3E0);
			if (spr == 8) { /* LR */
				emit_load_gpr(RTMP0, PPC_RS(op));
				a64_str_w_imm(RTMP0, RSTATE, PPCR_LR);
				return true;
			}
			if (spr == 9) { /* CTR */
				emit_load_gpr(RTMP0, PPC_RS(op));
				a64_str_w_imm(RTMP0, RSTATE, PPCR_CTR);
				return true;
			}
			if (spr == 1) { /* XER — unpack PPC format into {so,ov,ca,byte_count} */
				emit_load_gpr(RTMP0, PPC_RS(op));
				/* SO = bit 31 */
				a64_mov_reg(RTMP1, RTMP0);
				emit_load_imm32(RTMP2, 31); emit32(0x1AC02400 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
				emit32(0x39000000 | (PPCR_XER_SO << 10) | (RSTATE << 5) | RTMP1);
				/* OV = bit 30 */
				a64_mov_reg(RTMP1, RTMP0);
				emit_load_imm32(RTMP2, 30); emit32(0x1AC02400 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
				emit32(0x12000000 | (RTMP1 << 5) | RTMP1); /* AND #1 */
				emit32(0x39000000 | (PPCR_XER_OV << 10) | (RSTATE << 5) | RTMP1);
				/* CA = bit 29 */
				a64_mov_reg(RTMP1, RTMP0);
				emit_load_imm32(RTMP2, 29); emit32(0x1AC02400 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
				emit32(0x12000000 | (RTMP1 << 5) | RTMP1); /* AND #1 */
				emit32(0x39000000 | (PPCR_XER_CA << 10) | (RSTATE << 5) | RTMP1);
				/* byte_count = bits 6:0 */
				a64_mov_reg(RTMP1, RTMP0);
				emit_load_imm32(RTMP2, 0x7F); emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
				emit32(0x39000000 | (PPCR_XER_CNT << 10) | (RSTATE << 5) | RTMP1);
				return true;
			}
			/* Unknown SPR: fall back so the interpreter owns privileged/CPU-specific semantics. */
			return false;
		}
		case 824: /* srawi rA,rS,SH (arithmetic shift right immediate, set CA) */
		{
			uint32_t sh = (op >> 11) & 0x1F;
			uint32_t rs = PPC_RS(op);
			emit_load_gpr(RTMP0, rs);
			if (sh == 0) {
				emit_store_gpr(RTMP0, ra);
				emit_set_xer_ca(0);
			} else {
				/* CA = (rS < 0) && ((rS & ((1<<sh)-1)) != 0) */
				/* Save original for CA computation */
				a64_mov_reg(RTMP1, RTMP0); /* RTMP1 = original rS */
				/* ASR Wd, Wn, #sh = SBFM Wd,Wn,#sh,#31 (immr=sh<<16, imms=31<<10) */
				emit32(0x13000000 | (sh << 16) | (0x1F << 10) | (RTMP0 << 5) | RTMP0);
				emit_store_gpr(RTMP0, ra);
				/* Compute CA: test if source negative AND shifted-out bits nonzero */
				/* RTMP1 = original rS. Mask = (1<<sh)-1 */
				uint32_t mask = (1u << sh) - 1;
				emit_load_imm32(RTMP2, (int32_t)mask);
				emit32(0x6A000000 | (RTMP2 << 16) | (RTMP1 << 5) | 0x1F); /* TST Wn, mask (= ANDS WZR) */
				/* If Z=0 (bits nonzero) and rS < 0: CA=1 */
				/* Use: shifted_out_nonzero = (TST result != 0) = !Z */
				/* negative = (rS >> 31) */
				/* CA = shifted_out_nonzero & negative */
				a64_movz(RTMP0, 0, 0);
				emit_load_imm32(RTMP2, 1);
				/* CSEL RTMP0, 1, 0, NE (if shifted-out bits nonzero) */
				emit32(0x1A800000 | (RTMP0 << 16) | (0x1 << 12) | (RTMP2 << 5) | RTMP0);
				/* AND with sign bit of original */
				emit32(0x13010000 | (31 << 10) | (RTMP1 << 5) | RTMP1 | (31 << 16)); /* UBFX Wd, Wn, #31, #1 */
				emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND */
				/* RTMP0 = CA value (0 or 1). Write to XER.CA byte */
				emit32(0x39000000 | (PPCR_XER_CA << 10) | (RSTATE << 5) | RTMP0); /* STRB */
			}
			if (op & 1) { emit_load_gpr(RTMP0, ra); lazy_update_cr0(RTMP0); }
			return true;
		}
		case 24: /* slw rA,rS,rB (shift left word) */
		{
			/* PPC: shift amount is rB[26:31]; if bit 26 set (count >= 32) the
			 * result is 0. A 64-bit LSLV of the zero-extended 32-bit rS by
			 * (rB & 63) gives exactly this: for counts 32..63 the 32-bit value
			 * shifts out of the low word, so the stored low 32 bits are 0. */
			emit_load_gpr(RTMP0, PPC_RS(op));   /* X(RTMP0) = zero-extended rS */
			emit_load_gpr(RTMP1, rb);           /* X(RTMP1) = zero-extended rB */
			emit32(0x9AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* LSLV Xd,Xn,Xm */
			emit_store_gpr(RTMP0, ra);          /* stores low 32 bits */
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		}
		case 536: /* srw rA,rS,rB (shift right word) */
		{
			/* PPC: count rB[26:31]; count >= 32 → 0. 64-bit LSRV of the
			 * zero-extended 32-bit rS by (rB & 63): for counts 32..63 the only
			 * set bits are in [31:0], so the result is 0. */
			emit_load_gpr(RTMP0, PPC_RS(op));   /* X(RTMP0) = zero-extended rS */
			emit_load_gpr(RTMP1, rb);           /* X(RTMP1) = zero-extended rB */
			emit32(0x9AC02400 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* LSRV Xd,Xn,Xm */
			emit_store_gpr(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		}
		case 792: /* sraw rA,rS,rB (arithmetic shift right, set CA) */
		{
			uint32_t rs = PPC_RS(op);
			emit_load_gpr(RTMP0, rs);
			emit_load_gpr(RTMP1, rb);
			/* Save original for CA: RTMP2 = rS */
			a64_mov_reg(RTMP2, RTMP0);
			/* PPC sraw: if rB[5]=1 (shift>=32), result=sign-extend, CA=(rS<0) */
			/* ARM64 ASR with shift>=32 already produces sign-extended result */
			emit32(0x1AC02800 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ASR Wd,Wn,Wm */
			emit_store_gpr(RTMP0, ra);
			/* CA = (rS < 0) && (rS != (result << sh)) i.e. bits were shifted out */
			/* Simplified: CA = (rS < 0) && ((rS ^ (result << sh)) != 0) */
			/* For variable shift this is complex. Use: CA = (rS < 0) && (result << sh != rS) */
			/* LSL RTMP0 back by shift amount, compare with original */
			emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* LSL Wd,result,shift */
			/* If (rS != result<<sh) and (rS < 0), CA=1 */
			emit32(0x6B000000 | (RTMP2 << 16) | (RTMP0 << 5) | 0x1F); /* CMP result<<sh, rS */
			a64_movz(RTMP0, 0, 0);
			emit_load_imm32(RTMP1, 1);
			emit32(0x1A800000 | (RTMP0 << 16) | (0x1 << 12) | (RTMP1 << 5) | RTMP0); /* CSEL 1 if NE */
			/* AND with sign of original rS */
			emit32(0x53010000 | (RTMP2 << 5) | RTMP2 | (31 << 10) | (31 << 16)); /* UBFX bit31 */
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* AND */
			/* Write CA byte */
			emit32(0x39000000 | (PPCR_XER_CA << 10) | (RSTATE << 5) | RTMP0); /* STRB */
			if (op & 1) { emit_load_gpr(RTMP0, ra); lazy_update_cr0(RTMP0); }
			return true;
		}

		case 32: /* cmpl (cmplw crD,rA,rB) */
		{
			uint32_t crd = (op >> 23) & 0x7;
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | 0x1F);
			a64_movz(RTMP2, 0, 0);
			emit_load_imm32(RTMP0, 8);
			emit32(0x1A800000 | (RTMP2 << 16) | (0x3 << 12) | (RTMP0 << 5) | RTMP2); /* CC=LT */
			emit_load_imm32(RTMP0, 4);
			emit32(0x1A800000 | (RTMP2 << 16) | (0x8 << 12) | (RTMP0 << 5) | RTMP2); /* HI=GT */
			emit_load_imm32(RTMP0, 2);
			emit32(0x1A800000 | (RTMP2 << 16) | (0x0 << 12) | (RTMP0 << 5) | RTMP2); /* EQ */
			/* OR in XER[SO] as bit 0 */
			emit_or_xer_so_into_cr_nibble(RTMP2);
			uint32_t shift = (7 - crd) * 4;
			if (shift) { emit_load_imm32(RTMP0, shift); emit32(0x1AC02000 | (RTMP0 << 16) | (RTMP2 << 5) | RTMP2); }
			lazy_flush_cr0();
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			emit_load_imm32(RTMP1, ~(0xF << shift));
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit32(0x2A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
			return true;
		}

		case 23: /* lwzx rD,rA,rB */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) {
				emit_load_gpr(RTMP1, rb);
				emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			}
			a64_ldr_w_reg(RTMP1, RMEMBASE, RTMP0); /* LDR Wt, [Xn] */
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV */
			emit_store_gpr(RTMP1, rd);
			return true;

		case 151: /* stwx rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) {
				emit_load_gpr(RTMP2, rb);
				emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			}
			a64_str_w_reg(RTMP1, RMEMBASE, RTMP0); /* STR */
			return true;

		case 8: /* subfc rD,rA,rB (rD = rB - rA, set CA) */
			emit_load_gpr(RTMP0, rb);
			emit_load_gpr(RTMP1, ra);
			emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SUBS */
			emit_store_gpr(RTMP0, rd);
			emit_write_xer_ca_from_carry();
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 136: /* subfe rD,rA,rB (rD = ~rA + rB + CA; CA = carry-out of full sum) */
			/* Compute in 64-bit to get correct carry-out: the two-step
			 * ADDS+ADD approach loses the carry contribution from +CA. */
			emit_load_gpr(RTMP0, ra);
			emit32(0x2A2003E0 | (RTMP0 << 16) | RTMP0); /* MVN Wd, Wm → ~rA */
			emit32(0xD3407C00 | (RTMP0 << 5) | RTMP0);  /* UXTW Xd, Wn → zero-extend to 64 */
			emit_load_gpr(RTMP1, rb);
			emit32(0xD3407C00 | (RTMP1 << 5) | RTMP1);  /* UXTW Xd, Wn → zero-extend to 64 */
			emit32(0x8B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADD Xd, Xn, Xm (64-bit) */
			emit_read_xer_ca(RTMP1);
			emit32(0x8B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADD Xd, Xn, Xm (64-bit) */
			/* Carry-out = bit 32 of 64-bit result */
			emit32(0xD360FC00 | (RTMP0 << 5) | RTMP1);  /* LSR Xd, Xn, #32 → carry in RTMP1[0] */
			emit32(0x39000000 | (PPCR_XER_CA << 10) | (RSTATE << 5) | RTMP1); /* STRB CA */
			/* Truncate result to 32 bits */
			emit32(0x2A0003E0 | (RTMP0 << 16) | RTMP0); /* MOV Wd, Wn (truncate to 32) */
			emit_store_gpr(RTMP0, rd);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 10: /* addc rD,rA,rB (set CA) */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x2B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADDS */
			emit_store_gpr(RTMP0, rd);
			emit_write_xer_ca_from_carry();
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		/* ---- OE=1 (overflow-enabled) arithmetic ----
		 * 10-bit XO = 512 (OE bit) + base XO.  These set XER.OV from signed
		 * overflow and accumulate XER.SO, in addition to the base semantics.
		 * The Mac ROM's built-in 68k emulator uses addco/subfco in its hottest
		 * loops to derive 68k condition codes — without these, the blocks
		 * containing them (76%+19% of all compile failures) stay interpreted.
		 * SS_JIT_NO_OE=1: bisect switch — fall back to the interpreter for all
		 * OE variants (diagnostic only). */
		case 522: case 520: case 778: case 552: case 616:
		{
			static int no_oe = -1;
			if (no_oe < 0) { const char *e = getenv("SS_JIT_NO_OE"); no_oe = (e && *e == '1') ? 1 : 0; }
			if (no_oe) return false;
		}
		switch (xo) {
		case 522: /* addco rD,rA,rB (set CA, OV, SO) */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x2B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADDS */
			emit_store_gpr(RTMP0, rd);
			emit_write_xer_ca_from_carry();
			emit_write_xer_ov_so_from_overflow();
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		case 520: /* subfco rD,rA,rB (rD = rB - rA; set CA, OV, SO) */
			emit_load_gpr(RTMP0, rb);
			emit_load_gpr(RTMP1, ra);
			emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SUBS */
			emit_store_gpr(RTMP0, rd);
			emit_write_xer_ca_from_carry();
			emit_write_xer_ov_so_from_overflow();
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		case 778: /* addo rD,rA,rB (set OV, SO; CA unchanged) */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x2B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADDS (for V flag) */
			emit_store_gpr(RTMP0, rd);
			emit_write_xer_ov_so_from_overflow();
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		case 552: /* subfo rD,rA,rB (rD = rB - rA; set OV, SO; CA unchanged) */
			emit_load_gpr(RTMP0, rb);
			emit_load_gpr(RTMP1, ra);
			emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SUBS (for V flag) */
			emit_store_gpr(RTMP0, rd);
			emit_write_xer_ov_so_from_overflow();
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		case 616: /* nego rD,rA (rD = -rA; set OV, SO; overflow iff rA == 0x80000000) */
			emit_load_gpr(RTMP1, ra);
			a64_movz(RTMP0, 0, 0);
			emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SUBS 0 - rA */
			emit_store_gpr(RTMP0, rd);
			emit_write_xer_ov_so_from_overflow();
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		}
		return false; /* nested OE switch fell through (unreachable) */

		case 138: /* adde rD,rA,rB (rD = rA + rB + CA; CA = carry-out of full sum) */
			/* 64-bit sum for correct carry-out (same fix as subfe case 136). */
			emit_load_gpr(RTMP0, ra);
			emit32(0xD3407C00 | (RTMP0 << 5) | RTMP0);  /* UXTW Xd, Wn */
			emit_load_gpr(RTMP1, rb);
			emit32(0xD3407C00 | (RTMP1 << 5) | RTMP1);  /* UXTW Xd, Wn */
			emit32(0x8B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADD X, X, X */
			emit_read_xer_ca(RTMP1);
			emit32(0x8B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADD X, X, CA */
			emit32(0xD360FC00 | (RTMP0 << 5) | RTMP1);  /* LSR Xd, Xn, #32 */
			emit32(0x39000000 | (PPCR_XER_CA << 10) | (RSTATE << 5) | RTMP1); /* STRB CA */
			emit32(0x2A0003E0 | (RTMP0 << 16) | RTMP0); /* MOV Wd, Wn (truncate) */
			emit_store_gpr(RTMP0, rd);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 234: /* addme rD,rA (rD = rA + CA - 1, set CA) */
		{
			/* result = rA + CA_in + 0xFFFFFFFF (all 32-bit). Compute the full sum
			 * in 64-bit so the carry-out is simply bit 32 of the result; this
			 * avoids the ADDS/ADCS double-counting of CA. */
			emit_load_gpr(RTMP0, ra);       /* X(RTMP0) = zero-extended rA */
			emit_read_xer_ca(RTMP1);        /* X(RTMP1) = CA_in (0 or 1) */
			a64_add_reg(RTMP0, RTMP0, RTMP1); /* X = rA + CA_in (64-bit) */
			emit_load_imm32(RTMP2, -1);     /* W(RTMP2) = 0xFFFFFFFF, zero-extended */
			a64_add_reg(RTMP0, RTMP0, RTMP2); /* X = rA + CA_in + 0xFFFFFFFF */
			/* CA_out = bit 32 of the 64-bit sum */
			emit_lsr64_imm(RTMP1, RTMP0, 32); /* X(RTMP1) = sum >> 32 (0 or 1) */
			emit32(0x39000000 | (PPCR_XER_CA << 10) | (RSTATE << 5) | RTMP1); /* STRB CA */
			emit_store_gpr(RTMP0, rd);      /* store low 32 bits */
			if (op & 1) { emit_load_gpr(RTMP0, rd); lazy_update_cr0(RTMP0); }
			return true;
		}
		case 202: /* addze rD,rA (rD = rA + CA, set CA) */
		{
			emit_load_gpr(RTMP0, ra);
			emit_read_xer_ca(RTMP1); /* RTMP1 = CA (0 or 1) */
			emit32(0x2B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADDS Wd, rA, CA */
			emit_store_gpr(RTMP0, rd);
			emit_write_xer_ca_from_carry();
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		}
		case 232: /* subfme rD,rA (rD = ~rA + CA - 1, set CA) */
		{
			/* result = ~rA + CA_in + 0xFFFFFFFF (all 32-bit). 64-bit sum so the
			 * carry-out is bit 32 (avoids ADDS/ADCS CA double-count). */
			emit_load_gpr(RTMP0, ra);
			emit32(0x2A2003E0 | (RTMP0 << 16) | RTMP0); /* MVN Wd = ~rA (zero-extends) */
			emit_read_xer_ca(RTMP1);        /* X(RTMP1) = CA_in (0 or 1) */
			a64_add_reg(RTMP0, RTMP0, RTMP1); /* X = ~rA + CA_in */
			emit_load_imm32(RTMP2, -1);     /* 0xFFFFFFFF */
			a64_add_reg(RTMP0, RTMP0, RTMP2); /* X = ~rA + CA_in + 0xFFFFFFFF */
			emit_lsr64_imm(RTMP1, RTMP0, 32); /* carry-out = bit 32 */
			emit32(0x39000000 | (PPCR_XER_CA << 10) | (RSTATE << 5) | RTMP1); /* STRB CA */
			emit_store_gpr(RTMP0, rd);
			if (op & 1) { emit_load_gpr(RTMP0, rd); lazy_update_cr0(RTMP0); }
			return true;
		}
		case 200: /* subfze rD,rA (rD = ~rA + CA, set CA) */
		{
			emit_load_gpr(RTMP0, ra);
			emit32(0x2A2003E0 | (RTMP0 << 16) | RTMP0); /* MVN = ~rA */
			emit_read_xer_ca(RTMP1);
			emit32(0x2B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADDS Wd, ~rA, CA */
			emit_store_gpr(RTMP0, rd);
			emit_write_xer_ca_from_carry();
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		}
		case 476: /* nand rA,rS,rB → rA = ~(rS & rB) */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND Wd,Wn,Wm */
			emit32(0x2A2003E0 | (RTMP0 << 16) | RTMP0); /* ORN Wd,WZR,Wm = MVN */
			emit_store_gpr(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 124: /* nor rA,rS,rB */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ORR */
			emit32(0x2A2003E0 | (RTMP0 << 16) | RTMP0); /* MVN */
			emit_store_gpr(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 284: /* eqv rA,rS,rB (XNOR) */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x4A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* EOR */
			emit32(0x2A2003E0 | (RTMP0 << 16) | RTMP0); /* MVN */
			emit_store_gpr(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 60: /* andc rA,rS,rB */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x0A200000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* BIC Wd,Wn,Wm */
			emit_store_gpr(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 412: /* orc rA,rS,rB */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x2A200000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ORN Wd,Wn,Wm */
			emit_store_gpr(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 459: /* divwu rD,rA,rB (unsigned divide) */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x1AC00800 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* UDIV Wd,Wn,Wm */
			emit_store_gpr(RTMP0, rd);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 75: /* mulhw rD,rA,rB (high word of signed multiply) */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			/* SMULL Xd, Wn, Wm then LSR Xd, Xd, #32 (sign already encoded in 64-bit product) */
			emit32(0x9B207C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SMULL */
			emit32(0xD360FC00 | (RTMP0 << 5) | RTMP0); /* LSR Xd, Xn, #32 */
			emit_store_gpr(RTMP0, rd);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 11: /* mulhwu rD,rA,rB (high word of unsigned multiply) */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x9BA07C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* UMULL */
			emit32(0xD360FC00 | (RTMP0 << 5) | RTMP0); /* LSR Xd, Xn, #32 */
			emit_store_gpr(RTMP0, rd);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		case 87: /* lbzx rD,rA,rB */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_ldrb_reg(RTMP1, RMEMBASE, RTMP0); /* LDRB */
			emit_store_gpr(RTMP1, rd);
			return true;
		case 215: /* stbx rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP2, rb); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_strb_reg(RTMP1, RMEMBASE, RTMP0); /* STRB */
			return true;
		case 279: /* lhzx rD,rA,rB */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_ldrh_reg(RTMP1, RMEMBASE, RTMP0); /* LDRH */
			emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1); /* REV16 */
			emit_store_gpr(RTMP1, rd);
			return true;
		case 407: /* sthx rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1); /* REV16 */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP2, rb); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_strh_reg(RTMP1, RMEMBASE, RTMP0); /* STRH */
			return true;
		case 343: /* lhax rD,rA,rB (load halfword algebraic indexed) */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_ldrh_reg(RTMP1, RMEMBASE, RTMP0); /* LDRH */
			emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1); /* REV16 */
			emit32(0x13003C00 | (RTMP1 << 5) | RTMP1); /* SXTH */
			emit_store_gpr(RTMP1, rd);
			return true;
		case 371: /* mftb rD,TBR — move from time base.
		         * The interpreter uses the CPU object's timebase model, not
		         * the raw ARM64 counter. Fall through for correct time values. */
			return false;

		case 119: /* lbzux rD,rA,rB */
			/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			a64_ldrb_reg(RTMP1, RMEMBASE, RTMP0);
			emit_store_gpr(RTMP1, rd);
			return true;
		case 247: /* stbux rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP2, rb);
			emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			a64_strb_reg(RTMP1, RMEMBASE, RTMP0);
			return true;
		case 311: /* lhzux rD,rA,rB */
			/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			a64_ldrh_reg(RTMP1, RMEMBASE, RTMP0);
			emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1);
			emit_store_gpr(RTMP1, rd);
			return true;
		case 439: /* sthux rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1);
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP2, rb);
			emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			a64_strh_reg(RTMP1, RMEMBASE, RTMP0);
			return true;
		case 375: /* lhaux rD,rA,rB */
			/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			a64_ldrh_reg(RTMP1, RMEMBASE, RTMP0);
			emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1);
			emit32(0x13003C00 | (RTMP1 << 5) | RTMP1);
			emit_store_gpr(RTMP1, rd);
			return true;
		case 55: /* lwzux rD,rA,rB */
			/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			a64_ldr_w_reg(RTMP1, RMEMBASE, RTMP0);
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
			emit_store_gpr(RTMP1, rd);
			return true;
		case 183: /* stwux rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP2, rb);
			emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			a64_str_w_reg(RTMP1, RMEMBASE, RTMP0);
			return true;
		case 790: /* lhbrx rD,rA,rB (byte-reversed = native order on LE) */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_ldrh_reg(RTMP1, RMEMBASE, RTMP0); /* LDRH (native LE = byte-reversed for PPC) */
			emit_store_gpr(RTMP1, rd);
			return true;
		case 918: /* sthbrx rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP2, rb); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_strh_reg(RTMP1, RMEMBASE, RTMP0);
			return true;
		case 534: /* lwbrx rD,rA,rB (byte-reversed = native order on LE) */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_ldr_w_reg(RTMP1, RMEMBASE, RTMP0);
			emit_store_gpr(RTMP1, rd);
			return true;
		case 662: /* stwbrx rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP2, rb); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_str_w_reg(RTMP1, RMEMBASE, RTMP0);
			return true;

		case 535: /* lfsx frD,rA,rB */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_ldr_w_reg(RTMP1, RMEMBASE, RTMP0);
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
			emit32(0x1E270000 | (RTMP1 << 5) | 0);
			emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, rd);
			return true;
		case 567: /* lfsux frD,rA,rB */
			emit_load_ea_base(ra); emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			a64_ldr_w_reg(RTMP1, RMEMBASE, RTMP0);
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
			emit32(0x1E270000 | (RTMP1 << 5) | 0);
			emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, rd);
			return true;
		case 599: /* lfdx frD,rA,rB */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_ldr_x_reg(RTMP1, RMEMBASE, RTMP0);
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
			emit32(0x9E670000 | (RTMP1 << 5) | 0);
			emit_store_fpr(0, rd);
			return true;
		case 631: /* lfdux frD,rA,rB */
			emit_load_ea_base(ra); emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			a64_ldr_x_reg(RTMP1, RMEMBASE, RTMP0);
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
			emit32(0x9E670000 | (RTMP1 << 5) | 0);
			emit_store_fpr(0, rd);
			return true;
		case 663: /* stfsx frS,rA,rB */
			emit_load_fpr(0, PPC_RS(op));
			emit32(0x1E624000 | (0 << 5) | 0);
			emit32(0x1E260000 | (0 << 5) | RTMP1);
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP2, rb); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_str_w_reg(RTMP1, RMEMBASE, RTMP0);
			return true;
		case 695: /* stfsux frS,rA,rB */
			emit_load_fpr(0, PPC_RS(op));
			emit32(0x1E624000 | (0 << 5) | 0);
			emit32(0x1E260000 | (0 << 5) | RTMP1);
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP2, rb);
			emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			a64_str_w_reg(RTMP1, RMEMBASE, RTMP0);
			return true;
		case 727: /* stfdx frS,rA,rB */
			emit_load_fpr(0, PPC_RS(op));
			emit32(0x9E660000 | (0 << 5) | RTMP1);
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP2, rb); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_str_x_reg(RTMP1, RMEMBASE, RTMP0);
			return true;
		case 759: /* stfdux frS,rA,rB */
			emit_load_fpr(0, PPC_RS(op));
			emit32(0x9E660000 | (0 << 5) | RTMP1);
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP2, rb);
			emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			a64_str_x_reg(RTMP1, RMEMBASE, RTMP0);
			return true;
		case 1014: /* dcbz rA,rB — zero cache line (32 bytes) */
		{
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			/* Align to 32 bytes */
			emit_load_imm32(RTMP1, ~31);
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			/* Form host address: X(RTMP0) = RMEMBASE + (zero-extended guest EA).
			 * The AND above leaves RTMP0 as a 32-bit value zero-extended to 64;
			 * a 64-bit ADD with RMEMBASE yields the host pointer. */
			a64_add_reg(RTMP0, RMEMBASE, RTMP0);
			/* STP XZR,XZR,[Xn] four times = 32 bytes */
			emit32(0xA9000000 | (31 << 10) | (RTMP0 << 5) | 31); /* STP XZR,XZR,[Xn,#0] */
			emit32(0xA9010000 | (31 << 10) | (RTMP0 << 5) | 31); /* STP XZR,XZR,[Xn,#16] */
			return true;
		}

		/* Cache management — NOPs (no emulated cache hierarchy) */
		case 54:   /* dcbst — data cache block store */
		case 86:   /* dcbf  — data cache block flush */
		case 246:  /* dcbt  — data cache block touch (prefetch hint) */
		case 278:  /* dcbtst — data cache block touch for store */
		case 982:  /* icbi rA,rB — instruction cache block invalidate.
		            * Fall through to inline interpreter call so execute_icbi() runs
		            * and invalidates any stale JIT block at the target address.
		            * Without this, extension-loading overwrites RAM code but the JIT
		            * keeps running the old compiled translation → boot hangs. */
			return false;

		/* Memory barriers — NOPs (single-threaded emulator) */
		case 598:  /* sync  — synchronize */
		case 854:  /* eieio — enforce in-order execution of I/O */
			return true;
		case 512: /* mcrxr crD — move XER[SO,OV,CA] to CR field, clear XER flags */
		{
			uint32_t crd_f = (op >> 23) & 0x7;
			/* Build nibble: bit3=SO, bit2=OV, bit1=CA, bit0=0 */
			emit32(0x39400000 | (PPCR_XER_SO << 10) | (RSTATE << 5) | RTMP0); /* LDRB so */
			emit_load_imm32(RTMP1, 3); emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* LSL #3 */
			emit32(0x39400000 | (PPCR_XER_OV << 10) | (RSTATE << 5) | RTMP1); /* LDRB ov */
			emit_load_imm32(RTMP2, 2); emit32(0x1AC02000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* LSL #2 */
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* OR */
			emit32(0x39400000 | (PPCR_XER_CA << 10) | (RSTATE << 5) | RTMP1); /* LDRB ca */
			emit_load_imm32(RTMP2, 1); emit32(0x1AC02000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* LSL #1 */
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* OR */
			/* RTMP0 = {SO,OV,CA,0} nibble. Insert into CR field */
			uint32_t dst_sh = (7 - crd_f) * 4;
			if (dst_sh) { emit_load_imm32(RTMP1, dst_sh); emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			lazy_flush_cr0();
			a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CR);
			emit_load_imm32(RTMP2, ~(0xFU << dst_sh));
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
			emit32(0x2A000000 | (RTMP0 << 16) | (RTMP1 << 5) | RTMP1);
			a64_str_w_imm(RTMP1, RSTATE, PPCR_CR);
			/* Clear XER SO/OV/CA */
			a64_movz(RTMP0, 0, 0);
			emit32(0x39000000 | (PPCR_XER_SO << 10) | (RSTATE << 5) | RTMP0);
			emit32(0x39000000 | (PPCR_XER_OV << 10) | (RSTATE << 5) | RTMP0);
			emit32(0x39000000 | (PPCR_XER_CA << 10) | (RSTATE << 5) | RTMP0);
			return true;
		}
		case 20: /* lwarx rD,rA,rB — load word and reserve.
			 * Must set reservation state in the CPU object (not just regs struct).
			 * Fall through to interpreter for correct reservation semantics. */
			return false;
		case 150: /* stwcx. rS,rA,rB — store word conditional.
			 * Must check reservation state and set CR0 based on success/failure.
			 * The reservation lives in the CPU object, not the regs struct.
			 * Fall through to interpreter for correct atomic semantics. */
			return false;

		case 595: /* mfsr — move from segment register (supervisor, treat as NOP returning 0) */
			emit_load_imm32(RTMP0, 0);
			emit_store_gpr(RTMP0, rd);
			return true;
		case 659: /* mfsrin — same */
			emit_load_imm32(RTMP0, 0);
			emit_store_gpr(RTMP0, rd);
			return true;

		case 83: /* mfmsr rD — simplified: return 0 */
			emit_load_imm32(RTMP0, 0);
			emit_store_gpr(RTMP0, rd);
			return true;
		case 310: /* eciwx rD,rA,rB — external control in word: NOP */
			return true;
		case 438: /* ecowx rS,rA,rB — external control out word: NOP */
			return true;

		case 822: /* dss — data stream stop: NOP */
			return true;
		case 342: /* dst — data stream touch: NOP */
			return true;
		case 374: /* dstst — data stream touch for store: NOP */
			return true;


		case 597: /* lswi rD,rA,NB */
		{
			uint32_t nb_field = rb;
			uint32_t nb = nb_field == 0 ? 32 : nb_field;
			if (ra == 0) { a64_movz(RTMP0, 0, 0); }
			else { emit_load_gpr(RTMP0, ra); }
			uint32_t r = rd;
			uint32_t bytes_done = 0;
			while (bytes_done < nb) {
				if (nb - bytes_done >= 4) {
					a64_ldr_w_reg(RTMP1, RMEMBASE, RTMP0);
					emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
					emit_store_gpr(RTMP1, r);
					if (bytes_done + 4 < nb) emit32(0x11001000 | (RTMP0 << 5) | RTMP0);
					bytes_done += 4;
				} else {
					a64_movz(RTMP1, 0, 0);
					for (uint32_t b = 0; b < nb - bytes_done; b++) {
						/* LDRB W(RTMP2), [RMEMBASE, X(RTMP0)] then advance EA by 1 */
						a64_ldrb_reg(RTMP2, RMEMBASE, RTMP0);
						emit32(0x11000400 | (RTMP0 << 5) | RTMP0); /* ADD W(RTMP0), W(RTMP0), #1 */
						uint32_t sh = (3 - b) * 8;
						if (sh) { emit_load_imm32(3, sh); emit32(0x1AC02000 | (3 << 16) | (RTMP2 << 5) | RTMP2); }
						emit32(0x2A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
					}
					emit_store_gpr(RTMP1, r);
					bytes_done = nb;
				}
				r = (r + 1) & 31;
			}
			return true;
		}
		case 725: /* stswi rS,rA,NB */
		{
			uint32_t nb_field = rb;
			uint32_t nb = nb_field == 0 ? 32 : nb_field;
			if (ra == 0) { a64_movz(RTMP0, 0, 0); }
			else { emit_load_gpr(RTMP0, ra); }
			uint32_t r = PPC_RS(op);
			uint32_t bytes_done = 0;
			while (bytes_done < nb) {
				if (nb - bytes_done >= 4) {
					emit_load_gpr(RTMP1, r);
					emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
					/* STR W(RTMP1), [RMEMBASE, X(RTMP0)] then advance EA by 4 */
					a64_str_w_reg(RTMP1, RMEMBASE, RTMP0);
					emit32(0x11001000 | (RTMP0 << 5) | RTMP0); /* ADD W(RTMP0), W(RTMP0), #4 */
					bytes_done += 4;
				} else {
					emit_load_gpr(RTMP1, r);
					for (uint32_t b = 0; b < nb - bytes_done; b++) {
						a64_mov_reg(RTMP2, RTMP1);
						uint32_t sh = (3 - b) * 8;
						if (sh) { emit_load_imm32(3, sh); emit32(0x1AC02400 | (3 << 16) | (RTMP2 << 5) | RTMP2); }
						/* STRB W(RTMP2), [RMEMBASE, X(RTMP0)] then advance EA by 1 */
						a64_strb_reg(RTMP2, RMEMBASE, RTMP0);
						emit32(0x11000400 | (RTMP0 << 5) | RTMP0); /* ADD W(RTMP0), W(RTMP0), #1 */
					}
					bytes_done = nb;
				}
				r = (r + 1) & 31;
			}
			return true;
		}
		case 533: /* lswx: runtime NB — fall back so interpreter executes this insn */
		case 661: /* stswx: runtime NB — fall back so interpreter executes this insn */
			return false;



		/* === 64-bit G5/PPC970 XO31 instructions === */

		case 27: /* sld rA,rS,rB */
			emit_load_gpr64(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x9AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* LSL Xd,Xn,Xm */
			emit_store_gpr64(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		case 539: /* srd rA,rS,rB */
			emit_load_gpr64(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x9AC02400 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* LSR Xd,Xn,Xm */
			emit_store_gpr64(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		case 794: /* srad rA,rS,rB — shift right algebraic doubleword */
			emit_load_gpr64(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x9AC02800 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ASR Xd,Xn,Xm */
			emit_store_gpr64(RTMP0, ra);
			emit_set_xer_ca(0); /* simplified — full CA needs shifted-out-bits check */
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		case 826: /* sradi rA,rS,SH — shift right algebraic doubleword immediate */
		{
			uint32_t sh = ((op >> 11) & 0x1F) | (((op >> 1) & 1) << 5);
			emit_load_gpr64(RTMP0, PPC_RS(op));
			if (sh) emit32(0x9340FC00 | (sh << 10) | (RTMP0 << 5) | RTMP0); /* ASR Xd,Xn,#sh */
			emit_store_gpr64(RTMP0, ra);
			emit_set_xer_ca(0); /* simplified */
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;
		}

		case 58: /* cntlzd rA,rS */
			emit_load_gpr64(RTMP0, PPC_RS(op));
			emit32(0xDAC01000 | (RTMP0 << 5) | RTMP0); /* CLZ Xd,Xn */
			emit_store_gpr(RTMP0, ra);
			a64_str_w_imm(31, RSTATE, PPCR_GPR_HI(ra));
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		case 986: /* extsw rA,rS — sign-extend word to doubleword */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit32(0x93407C00 | (RTMP0 << 5) | RTMP0); /* SXTW Xd,Wn */
			emit_store_gpr64(RTMP0, ra);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		case 233: /* mulld rD,rA,rB */
			emit_load_gpr64(RTMP0, ra);
			emit_load_gpr64_tmp(RTMP1, rb, RTMP2); /* don't clobber RTMP0 operand */
			emit32(0x9B007C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* MUL Xd,Xn,Xm */
			emit_store_gpr64(RTMP0, rd);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		case 9: /* mulhdu rD,rA,rB */
			emit_load_gpr64(RTMP0, ra);
			emit_load_gpr64_tmp(RTMP1, rb, RTMP2); /* don't clobber RTMP0 operand */
			emit32(0x9BC07C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* UMULH Xd,Xn,Xm */
			emit_store_gpr64(RTMP0, rd);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		case 73: /* mulhd rD,rA,rB */
			emit_load_gpr64(RTMP0, ra);
			emit_load_gpr64_tmp(RTMP1, rb, RTMP2); /* don't clobber RTMP0 operand */
			emit32(0x9B407C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SMULH Xd,Xn,Xm */
			emit_store_gpr64(RTMP0, rd);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		case 457: /* divdu rD,rA,rB */
			emit_load_gpr64(RTMP0, ra);
			emit_load_gpr64_tmp(RTMP1, rb, RTMP2); /* don't clobber RTMP0 operand */
			emit32(0x9AC00800 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* UDIV Xd,Xn,Xm */
			emit_store_gpr64(RTMP0, rd);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		case 489: /* divd rD,rA,rB */
			emit_load_gpr64(RTMP0, ra);
			emit_load_gpr64_tmp(RTMP1, rb, RTMP2); /* don't clobber RTMP0 operand */
			emit32(0x9AC00C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SDIV Xd,Xn,Xm */
			emit_store_gpr64(RTMP0, rd);
			if (op & 1) lazy_update_cr0(RTMP0);
			return true;

		/* 64-bit load/store indexed */
		case 21: /* ldx rD,rA,rB */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_ldr_x_reg(RTMP1, RMEMBASE, RTMP0); /* LDR Xt,[Xn] */
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1); /* REV Xt */
			emit_store_gpr64(RTMP1, rd);
			return true;

		case 53: /* ldux rD,rA,rB */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			a64_ldr_x_reg(RTMP1, RMEMBASE, RTMP0);
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
			emit_store_gpr64(RTMP1, rd);
			return true;

		case 149: /* stdx rS,rA,rB */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0xAA000000 | (RTMP0 << 16) | (31 << 5) | RTMP2); /* save EA before 64-bit load clobbers RTMP0 */
			emit_load_gpr64(RTMP1, PPC_RS(op));
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
			a64_str_x_reg(RTMP1, RMEMBASE, RTMP2);
			return true;

		case 181: /* stdux rS,rA,rB */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			emit32(0xAA000000 | (RTMP0 << 16) | (31 << 5) | RTMP2); /* save EA before 64-bit load clobbers RTMP0 */
			emit_load_gpr64(RTMP1, PPC_RS(op));
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
			a64_str_x_reg(RTMP1, RMEMBASE, RTMP2);
			return true;

		case 84: /* ldarx rD,rA,rB — simplified as load */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_ldr_x_reg(RTMP1, RMEMBASE, RTMP0);
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
			emit_store_gpr64(RTMP1, rd);
			return true;

		case 214: /* stdcx. rS,rA,rB — simplified: always succeed */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0xAA000000 | (RTMP0 << 16) | (31 << 5) | RTMP2); /* save EA before 64-bit load clobbers RTMP0 */
			emit_load_gpr64(RTMP1, PPC_RS(op));
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
			a64_str_x_reg(RTMP1, RMEMBASE, RTMP2);
			/* CR0 = EQ (reserve succeeded) */
			lazy_flush_cr0();
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			emit_load_imm32(RTMP1, ~(0xF << 28)); emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_load_imm32(RTMP1, 0x20000000); emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
			return true;

		case 4: /* tw — trap word: fall back so interpreter can evaluate trap */
			return false;

									default:
			jit_xo_miss[(op >> 1) & 0x3FF]++;
			return false;
		}
	}

	case 32: /* lwz rD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		if (ra == 0) {
			emit_load_imm32(RTMP0, (int32_t)simm);
		} else {
			emit_load_gpr(RTMP0, ra);
			if (simm) {
				emit_load_imm32(RTMP1, (int32_t)simm);
				emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			}
		}
		/* LDR W(RTMP1), [X(RTMP0)] — load 32-bit from host address */
		a64_ldr_w_reg(RTMP1, RMEMBASE, RTMP0); /* LDR Wt, [Xn] */
		/* Byte-swap: PPC is big-endian, ARM64 is little-endian */
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV Wd, Wn */
		emit_store_gpr(RTMP1, rd);
		return true;

	case 36: /* stw rS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP1, rd);                  /* value to store */
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV (byte-swap) */
		emit_load_ea_base(ra);                     /* base address (rA==0 means absolute) */
		if (simm) {
			emit_load_imm32(RTMP2, (int32_t)simm);
			emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
		}
		/* STR Wt, [Xn] */
		a64_str_w_reg(RTMP1, RMEMBASE, RTMP0);
		return true;

	case 34: /* lbz rD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP1, (int32_t)simm); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		a64_ldrb_reg(RTMP1, RMEMBASE, RTMP0); /* LDRB Wt, [Xn] */
		emit_store_gpr(RTMP1, rd);
		return true;

	case 38: /* stb rS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP1, rd);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP2, (int32_t)simm); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
		a64_strb_reg(RTMP1, RMEMBASE, RTMP0); /* STRB Wt, [Xn] */
		return true;

	case 40: /* lhz rD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP1, (int32_t)simm); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		a64_ldrh_reg(RTMP1, RMEMBASE, RTMP0); /* LDRH Wt, [Xn] */
		emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1); /* REV16 Wd, Wn (byte-swap halfword) */
		emit_store_gpr(RTMP1, rd);
		return true;

	case 44: /* sth rS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP1, rd);
		emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1); /* REV16 */
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP2, (int32_t)simm); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
		a64_strh_reg(RTMP1, RMEMBASE, RTMP0); /* STRH Wt, [Xn] */
		return true;

	case 12: /* addic rD,rA,SIMM (sets XER[CA]) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		/* ADDS Wd, Wn, Wm (sets NZCV — we use C for carry-out) */
		emit32(0x2B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, rd);
		emit_write_xer_ca_from_carry();
		return true;

	case 21: /* rlwinm rA,rS,SH,MB,ME */
	{
		uint32_t rs = PPC_RS(op);
		ra = PPC_RA(op);
		uint32_t sh = (op >> 11) & 0x1F;
		uint32_t mb = (op >> 6) & 0x1F;
		uint32_t me = (op >> 1) & 0x1F;
		emit_load_gpr(RTMP0, rs);
		/* Rotate left by SH: ROR Wd,Wn,#(32-SH) */
		if (sh) {
			uint32_t ror_amt = (32 - sh) & 0x1F;
			/* EXTR Wd, Wn, Wn, #ror_amt = rotate right */
			emit32(0x13800000 | (RTMP0 << 16) | (ror_amt << 10) | (RTMP0 << 5) | RTMP0);
		}
		/* Apply mask MB..ME */
		uint32_t mask = 0;
		if (mb <= me) {
			for (uint32_t i = mb; i <= me; i++) mask |= (0x80000000U >> i);
		} else {
			for (uint32_t i = 0; i <= me; i++) mask |= (0x80000000U >> i);
			for (uint32_t i = mb; i <= 31; i++) mask |= (0x80000000U >> i);
		}
		if (mask != 0xFFFFFFFF) {
			emit_load_imm32(RTMP1, (int32_t)mask);
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND */
		}
		emit_store_gpr(RTMP0, ra);
		if (op & 1) lazy_update_cr0(RTMP0); /* rlwinm. updates CR0 */
		return true;
	}

	case 20: /* rlwimi rA,rS,SH,MB,ME (insert) */
	{
		uint32_t rs = PPC_RS(op);
		ra = PPC_RA(op);
		uint32_t sh = (op >> 11) & 0x1F;
		uint32_t mb = (op >> 6) & 0x1F;
		uint32_t me = (op >> 1) & 0x1F;
		/* Rotate rS left by SH */
		emit_load_gpr(RTMP0, rs);
		if (sh) {
			uint32_t ror_amt = (32 - sh) & 0x1F;
			emit32(0x13800000 | (RTMP0 << 16) | (ror_amt << 10) | (RTMP0 << 5) | RTMP0);
		}
		/* Compute mask */
		uint32_t mask = 0;
		if (mb <= me) {
			for (uint32_t i = mb; i <= me; i++) mask |= (0x80000000U >> i);
		} else {
			for (uint32_t i = 0; i <= me; i++) mask |= (0x80000000U >> i);
			for (uint32_t i = mb; i <= 31; i++) mask |= (0x80000000U >> i);
		}
		/* rA = (rotated_rS & mask) | (rA & ~mask) */
		emit_load_imm32(RTMP1, (int32_t)mask);
		emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* rotated & mask */
		emit_load_gpr(RTMP2, ra);
		emit_load_imm32(RTMP1, (int32_t)~mask);
		emit32(0x0A000000 | (RTMP1 << 16) | (RTMP2 << 5) | RTMP2); /* rA & ~mask */
		emit32(0x2A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* OR */
		emit_store_gpr(RTMP0, ra);
		if (op & 1) lazy_update_cr0(RTMP0); /* rlwimi. updates CR0 */
		return true;
	}

	case 33: /* lwzu rD,d(rA) — load word and update rA */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* effective addr */
		emit_store_gpr(RTMP0, ra); /* update rA */
		a64_ldr_w_reg(RTMP1, RMEMBASE, RTMP0); /* LDR Wt, [Xn] */
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV (byte-swap) */
		emit_store_gpr(RTMP1, rd);
		return true;

	case 37: /* stwu rS,d(rA) — store word and update rA */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP1, rd);
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV */
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP2, (int32_t)simm);
		emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* effective addr */
		emit_store_gpr(RTMP0, ra); /* update rA */
		a64_str_w_reg(RTMP1, RMEMBASE, RTMP0); /* STR */
		return true;


	case 8: /* subfic rD,rA,SIMM (rD = SIMM - rA, set CA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_imm32(RTMP0, (int32_t)simm);
		emit_load_gpr(RTMP1, ra);
		/* ARM64 SUBS: Wd = SIMM - rA, sets C = !borrow = (SIMM >= rA unsigned) = PPC CA */
		emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SUBS Wd, SIMM, rA */
		emit_store_gpr(RTMP0, rd);
		emit_write_xer_ca_from_carry();
		return true;


	case 10: /* cmpli (cmplwi) crD,rA,UIMM */
	{
		uint32_t crd = (op >> 23) & 0x7;
		ra = PPC_RA(op); uimm = PPC_UIMM(op);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)(uint32_t)uimm);
		/* Unsigned compare: CMP Wn, Wm */
		emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | 0x1F);
		/* Build CR field from unsigned comparison:
		   LT = unsigned less (ARM64 CC = carry clear)
		   GT = unsigned greater (ARM64 CC = carry set AND not zero)
		   EQ = equal */
		emit32(0xD53B4200 | RTMP2); /* MRS NZCV */
		a64_movz(RTMP0, 0, 0);
		emit_load_imm32(RTMP1, 8); /* LT */
		/* CSEL RTMP0, RTMP1, RTMP0, CC (unsigned less = carry clear) */
		emit32(0x1A800000 | (RTMP0 << 16) | (0x3 << 12) | (RTMP1 << 5) | RTMP0);
		emit_load_imm32(RTMP1, 4); /* GT */
		/* CSEL RTMP0, RTMP1, RTMP0, HI (unsigned greater) */
		emit32(0x1A800000 | (RTMP0 << 16) | (0x8 << 12) | (RTMP1 << 5) | RTMP0);
		emit_load_imm32(RTMP1, 2); /* EQ */
		/* CSEL RTMP0, RTMP1, RTMP0, EQ */
		emit32(0x1A800000 | (RTMP0 << 16) | (0x0 << 12) | (RTMP1 << 5) | RTMP0);
		/* OR in XER[SO] as bit 0 */
		emit_or_xer_so_into_cr_nibble(RTMP0);
		uint32_t shift = (7 - crd) * 4;
		if (shift) { emit_load_imm32(RTMP1, shift); emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		lazy_flush_cr0();
		a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CR);
		emit_load_imm32(RTMP2, ~(0xF << shift));
		emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
		emit32(0x2A000000 | (RTMP0 << 16) | (RTMP1 << 5) | RTMP1);
		a64_str_w_imm(RTMP1, RSTATE, PPCR_CR);
		return true;
	}

	case 11: /* cmpi (cmpwi) crD,rA,SIMM */
	{
		uint32_t crd = (op >> 23) & 0x7;
		ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		/* Signed compare: CMP Wn, Wm */
		emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | 0x1F); /* SUBS WZR,Wn,Wm */
		/* Build CR field using CSEL: LT/GT/EQ (signed conditions) */
		a64_movz(RTMP0, 0, 0);
		emit_load_imm32(RTMP1, 8); /* LT */
		/* CSEL RTMP0, RTMP1, RTMP0, LT (signed less than: N!=V) */
		emit32(0x1A800000 | (RTMP0 << 16) | (0xB << 12) | (RTMP1 << 5) | RTMP0);
		emit_load_imm32(RTMP1, 4); /* GT */
		/* CSEL RTMP0, RTMP1, RTMP0, GT (signed greater than: Z==0 && N==V) */
		emit32(0x1A800000 | (RTMP0 << 16) | (0xC << 12) | (RTMP1 << 5) | RTMP0);
		emit_load_imm32(RTMP1, 2); /* EQ */
		/* CSEL RTMP0, RTMP1, RTMP0, EQ */
		emit32(0x1A800000 | (RTMP0 << 16) | (0x0 << 12) | (RTMP1 << 5) | RTMP0);
		/* OR in XER[SO] as bit 0 */
		emit_or_xer_so_into_cr_nibble(RTMP0);
		uint32_t shift = (7 - crd) * 4;
		if (shift) { emit_load_imm32(RTMP1, shift); emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		/* Load current CR, clear target field, OR in new value */
		lazy_flush_cr0();
		a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CR);
		emit_load_imm32(RTMP2, ~(0xF << shift));
		emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* AND (clear) */
		emit32(0x2A000000 | (RTMP0 << 16) | (RTMP1 << 5) | RTMP1); /* ORR (merge) */
		a64_str_w_imm(RTMP1, RSTATE, PPCR_CR);
		return true;
	}

	/* bc (Branch Conditional) — intra-block forward-branch optimization
	 *
	 * INVARIANT: find_forward_code_for_pc() is called ONLY with forward targets
	 * (target_pc > pc). Backward branches MUST use emit_epilogue_with_pc() instead.
	 *
	 * Rationale: the spcflags poll (emit_entry_spcflags_poll) sits at chain_entry_start,
	 * BEFORE insn_code_offset[0]. insn_code_offset[] tracks only the block body — i.e.,
	 * addresses AFTER the poll. A direct native branch to insn_code_offset[i] bypasses
	 * the poll entirely. For forward branches this is safe (they only run once per block
	 * entry). For backward branches it creates a closed ARM64 loop that never returns to
	 * the C dispatcher, starving interrupt delivery (VBL, spcflags) indefinitely.
	 */
	case 16: /* bc/bdnz/bdz/beq/bne family */
	{
		uint32_t bo = (op >> 21) & 0x1F;
		uint32_t bi = (op >> 16) & 0x1F;
		int16_t bd = ((op & 0xFFFC) ^ 0x8000) - 0x8000;
		bool aa = (op >> 1) & 1;
		bool lk = op & 1;
		uint32_t target_pc = aa ? (uint32_t)(int32_t)bd : (pc + bd);

		/* PPC ISA BO field (5 bits, MSB-first):
		   BO[0] (0x10): 1=don't test condition
		   BO[1] (0x08): condition sense (branch if CR[BI]=BO[1])
		   BO[2] (0x04): 1=don't decrement/test CTR
		   BO[3] (0x02): CTR sense (0=CTR≠0, 1=CTR==0)
		   BO[4] (0x01): prediction hint */
		bool no_ctr_test = (bo & 0x04); /* BO[2]=1: skip CTR decrement+test */
		bool ctr_eq_zero = (bo & 0x02); /* BO[3]=1: branch if CTR==0 (bdz) */
		bool no_cond_test = (bo & 0x10); /* BO[0]=1: skip condition test */
		bool cond_bit_val = (bo & 0x08); /* BO[1]=1: branch if CR[BI]=1 */

		/* bcl supported: lk=1 saves pc+4 to LR before branching */

		if (!no_ctr_test && no_cond_test) {
			/* bdnz or bdz (CTR-only, no condition test) */
			emit_save_lr_if_link(pc, lk);
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CTR);
			emit32(0x51000400 | (RTMP0 << 5) | RTMP0); /* SUB Wd, Wn, #1 */
			a64_str_w_imm(RTMP0, RSTATE, PPCR_CTR);

			if (!ctr_eq_zero) {
				/* bdnz: branch if CTR != 0 */
				/* Forward-only: backward intra-block branches must return to the dispatcher
				 * so the spcflags poll at block entry runs each iteration (VBL hang fix). */
				uint32_t *target_code = (target_pc > pc) ? find_forward_code_for_pc(target_pc) : NULL;
				if (target_code) {
					int32_t offset = (int32_t)((uint8_t *)target_code - (uint8_t *)jit_code_ptr);
					if (offset >= -(1 << 20) && offset < (1 << 20)) { /* 19-bit signed ±1MB */
						emit32(0x35000000 | (((offset >> 2) & 0x7FFFF) << 5) | RTMP0); /* CBNZ */
						return true;
					}
				}
				/* Not taken: CBZ skips to after epilogue */
				uint32_t *skip_loc = jit_code_ptr;
				emit32(0); /* placeholder CBZ */
				lazy_flush_cr0();
				emit_epilogue_with_pc(target_pc); /* taken path */
				/* Patch skip: CBZ RTMP0, <here> */
				int32_t skip_off = (int32_t)((uint8_t *)jit_code_ptr - (uint8_t *)skip_loc);
				*skip_loc = 0x34000000 | (((skip_off >> 2) & 0x7FFFF) << 5) | RTMP0;
				return true;
			} else {
				/* bdz: branch if CTR == 0 */
				/* Forward-only: same spcflags poll reason as bdnz above. */
				uint32_t *target_code = (target_pc > pc) ? find_forward_code_for_pc(target_pc) : NULL;
				if (target_code) {
					int32_t offset = (int32_t)((uint8_t *)target_code - (uint8_t *)jit_code_ptr);
					if (offset >= -(1 << 20) && offset < (1 << 20)) { /* 19-bit signed ±1MB */
						emit32(0x34000000 | (((offset >> 2) & 0x7FFFF) << 5) | RTMP0); /* CBZ */
						return true;
					}
				}
				/* Not taken: CBNZ skips to after epilogue */
				uint32_t *skip_loc = jit_code_ptr;
				emit32(0); /* placeholder CBNZ */
				lazy_flush_cr0();
				emit_epilogue_with_pc(target_pc); /* taken path */
				int32_t skip_off = (int32_t)((uint8_t *)jit_code_ptr - (uint8_t *)skip_loc);
				*skip_loc = 0x35000000 | (((skip_off >> 2) & 0x7FFFF) << 5) | RTMP0;
				return true;
			}
		}

		if (no_ctr_test && !no_cond_test) {
			/* Pure conditional branch: test CR[BI] only, no CTR */
			uint32_t bit_pos = 31 - bi;
			emit_save_lr_if_link(pc, lk);
			lazy_flush_cr0();
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			if (bit_pos) {
				emit_load_imm32(RTMP1, bit_pos);
				emit32(0x1AC02400 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* LSR */
			}
			emit32(0x12000000 | (RTMP0 << 5) | RTMP0); /* AND #1 */
			/* BO[3] (bit 1 of BO): 1=branch if set, 0=branch if clear */
			bool branch_if_set = cond_bit_val;
			/* Forward-only: backward intra-block branches bypass the spcflags poll
			 * (poll is before insn_code_offset[0], not in find_forward_code_for_pc range).
			 * A backward branch must return to the dispatcher so the poll runs. */
			uint32_t *target_code = (target_pc > pc) ? find_forward_code_for_pc(target_pc) : NULL;
			if (target_code) {
				int32_t offset = (int32_t)((uint8_t *)target_code - (uint8_t *)jit_code_ptr);
				if (offset >= -(1 << 20) && offset < (1 << 20)) { /* 19-bit signed ±1MB */
					if (branch_if_set)
						emit32(0x35000000 | (((offset >> 2) & 0x7FFFF) << 5) | RTMP0); /* CBNZ */
					else
						emit32(0x34000000 | (((offset >> 2) & 0x7FFFF) << 5) | RTMP0); /* CBZ */
					return true;
				}
			}
			if (branch_if_set) {
				uint32_t *skip_loc = jit_code_ptr;
				emit32(0); /* placeholder CBZ */
				lazy_flush_cr0();
				emit_epilogue_with_pc(target_pc); /* taken path */
				/* Not-taken path: PC = pc + 4 */
				int32_t skip_off = (int32_t)((uint8_t *)jit_code_ptr - (uint8_t *)skip_loc);
				*skip_loc = 0x34000000 | (((skip_off >> 2) & 0x7FFFF) << 5) | RTMP0;
				lazy_flush_cr0();
				emit_epilogue_with_pc(pc + 4); /* not-taken path */
			} else {
				uint32_t *skip_loc = jit_code_ptr;
				emit32(0); /* placeholder CBNZ */
				lazy_flush_cr0();
				emit_epilogue_with_pc(target_pc); /* taken path */
				/* Not-taken path: PC = pc + 4 */
				int32_t skip_off = (int32_t)((uint8_t *)jit_code_ptr - (uint8_t *)skip_loc);
				*skip_loc = 0x35000000 | (((skip_off >> 2) & 0x7FFFF) << 5) | RTMP0;
				lazy_flush_cr0();
				emit_epilogue_with_pc(pc + 4); /* not-taken path */
			}
			return true;
		}

		if (no_ctr_test && no_cond_test) {
			/* Unconditional: BO=1x1xx → always branch */
			emit_save_lr_if_link(pc, lk);
			lazy_flush_cr0();
			emit_epilogue_with_pc(target_pc);
			return true;
		}

		if (!no_ctr_test && !no_cond_test) {
			/* Decrement CTR AND test condition — branch only if BOTH pass */
			/* bcl not yet implemented for this BO combo — fall to interpreter */
			if (lk) return false;

			/* Decrement CTR */
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CTR);
			emit32(0x51000400 | (RTMP0 << 5) | RTMP0); /* SUB Wd, Wn, #1 */
			a64_str_w_imm(RTMP0, RSTATE, PPCR_CTR);

			/* Compute ctr_ok: 1 if CTR passes test, 0 otherwise */
			emit32(0x7100001F | (RTMP0 << 5)); /* CMP Wn, #0 */
			if (ctr_eq_zero)
				emit32(0x1A9F17E0 | RTMP0); /* CSET RTMP0, EQ */
			else
				emit32(0x1A9F07E0 | RTMP0); /* CSET RTMP0, NE */

			/* Compute cond_ok: extract CR[BI], match against BO[1] */
			uint32_t bit_pos = 31 - bi;
			lazy_flush_cr0();
			a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CR);
			if (bit_pos) { emit_load_imm32(RTMP2, bit_pos); emit32(0x1AC02400 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); }
			emit32(0x12000000 | (RTMP1 << 5) | RTMP1); /* AND #1 */
			if (!cond_bit_val) {
				/* Invert: branch if CR[BI]=0 */
				emit_load_imm32(RTMP2, 1);
				emit32(0x4A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* EOR #1 */
			}

			/* Branch if ctr_ok AND cond_ok */
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND */
			uint32_t *skip_loc = jit_code_ptr;
			emit32(0); /* placeholder CBZ */
			lazy_flush_cr0();
			emit_epilogue_with_pc(target_pc); /* taken */
			int32_t skip_off = (int32_t)((uint8_t *)jit_code_ptr - (uint8_t *)skip_loc);
			*skip_loc = 0x34000000 | (((skip_off >> 2) & 0x7FFFF) << 5) | RTMP0;
			lazy_flush_cr0();
			emit_epilogue_with_pc(pc + 4); /* not taken */
			return true;
		}

		return false; /* unhandled BO pattern */
	}

	case 19: /* CR ops, bclr, bcctr, isync */
	{
		uint32_t xo = (op >> 1) & 0x3FF;
		switch (xo) {
		case 16: /* bclr — branch conditional to LR */
		{
			uint32_t bo = (op >> 21) & 0x1F;
			uint32_t bi = (op >> 16) & 0x1F;
			bool lk = op & 1;
			bool no_ctr_test = (bo & 0x04);   /* BO[2]=1: skip CTR decrement+test */
			bool ctr_eq_zero = (bo & 0x02);   /* BO[3]=1: branch if CTR==0 */
			bool no_cond_test = (bo & 0x10);  /* BO[0]=1: skip condition test */
			bool cond_bit_val = (bo & 0x08);  /* BO[1]=1: branch if CR[BI]=1 */

			/* bclrl/bdnzlr branches to the old LR, then optional LK writes LR=pc+4. */
			a64_ldr_w_imm(RTMP2, RSTATE, PPCR_LR); /* taken target = old LR */
			if (lk) { emit_load_imm32(RTMP1, (int32_t)(pc + 4)); a64_str_w_imm(RTMP1, RSTATE, PPCR_LR); }

			/* RTMP0 = branch decision (1=taken, 0=fall through). */
			a64_movz(RTMP0, 1, 0);

			if (!no_ctr_test) {
				a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CTR);
				emit32(0x51000400 | (RTMP0 << 5) | RTMP0); /* SUB Wd, Wn, #1 */
				a64_str_w_imm(RTMP0, RSTATE, PPCR_CTR);
				emit32(0x7100001F | (RTMP0 << 5)); /* CMP Wn, #0 */
				if (ctr_eq_zero)
					emit32(0x1A9F17E0 | RTMP0); /* CSET RTMP0, EQ */
				else
					emit32(0x1A9F07E0 | RTMP0); /* CSET RTMP0, NE */
			}

			if (!no_cond_test) {
				uint32_t bit_pos = 31 - bi;
				lazy_flush_cr0();
				a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CR);
				if (bit_pos) {
					emit_load_imm32(RTMP3, bit_pos);
					emit32(0x1AC02400 | (RTMP3 << 16) | (RTMP1 << 5) | RTMP1); /* LSR */
				}
				emit32(0x12000000 | (RTMP1 << 5) | RTMP1); /* AND #1 */
				if (!cond_bit_val) {
					emit_load_imm32(RTMP3, 1);
					emit32(0x4A000000 | (RTMP3 << 16) | (RTMP1 << 5) | RTMP1); /* EOR #1 */
				}
				if (!no_ctr_test)
					emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND ctr_ok & cond_ok */
				else
					a64_mov_reg(RTMP0, RTMP1);
			}

			emit_load_imm32(RTMP1, (int32_t)(pc + 4)); /* not-taken target */
			emit32(0x7100001F | (RTMP0 << 5)); /* CMP branch decision, #0 */
			/* RTMP1 = decision ? old_LR : pc+4 */
			emit32(0x1A800000 | (RTMP1 << 16) | (0x1 << 12) | (RTMP2 << 5) | RTMP1); /* CSEL NE */
			a64_str_w_imm(RTMP1, RSTATE, PPCR_PC);
			lazy_flush_cr0();
			ra_flush_all();
			a64_ldp_post(27, 28, A64_SP, 16);
			a64_ldp_post(25, 26, A64_SP, 16);
			a64_ldp_post(23, 24, A64_SP, 16);
			a64_ldp_post(21, 22, A64_SP, 16);
			a64_ldp_post(19, RSTATE, A64_SP, 16);
			a64_ldp_post(A64_FP, A64_LR, A64_SP, 16);
			a64_ret();
			return true;
		}
		case 528: /* bcctr — branch conditional to CTR */
		{
			uint32_t bo = (op >> 21) & 0x1F;
			uint32_t bi = (op >> 16) & 0x1F;
			bool lk = op & 1;
			if ((bo & 0x14) == 0x14) { /* unconditional bctr */
				if (lk) { emit_load_imm32(RTMP0, (int32_t)(pc + 4)); a64_str_w_imm(RTMP0, RSTATE, PPCR_LR); }
				a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CTR);
				a64_str_w_imm(RTMP0, RSTATE, PPCR_PC);
				lazy_flush_cr0();
				ra_flush_all();
				a64_ldp_post(27, 28, A64_SP, 16);
				a64_ldp_post(25, 26, A64_SP, 16);
				a64_ldp_post(23, 24, A64_SP, 16);
				a64_ldp_post(21, 22, A64_SP, 16);
				a64_ldp_post(19, RSTATE, A64_SP, 16);
				a64_ldp_post(A64_FP, A64_LR, A64_SP, 16);
				a64_ret();
				return true;
			}
			/* Conditional bcctr: test CR[BI] */
			{
				uint32_t bit_pos = 31 - bi;
				lazy_flush_cr0();
				a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
				if (bit_pos) { emit_load_imm32(RTMP1, bit_pos); emit32(0x1AC02400 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
				emit32(0x12000000 | (RTMP0 << 5) | RTMP0);
				bool branch_if_true = (bo >> 3) & 1;
				if (lk) { emit_load_imm32(RTMP1, (int32_t)(pc + 4)); a64_str_w_imm(RTMP1, RSTATE, PPCR_LR); }
				a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CTR);
				emit_load_imm32(RTMP2, (int32_t)(pc + 4));
				if (branch_if_true) {
					emit32(0x35000000 | (2 << 5) | RTMP0);
					a64_mov_reg(RTMP1, RTMP2);
				} else {
					emit32(0x34000000 | (2 << 5) | RTMP0);
					a64_mov_reg(RTMP1, RTMP2);
				}
				a64_str_w_imm(RTMP1, RSTATE, PPCR_PC);
				lazy_flush_cr0();
				ra_flush_all();
				a64_ldp_post(27, 28, A64_SP, 16);
				a64_ldp_post(25, 26, A64_SP, 16);
				a64_ldp_post(23, 24, A64_SP, 16);
				a64_ldp_post(21, 22, A64_SP, 16);
				a64_ldp_post(19, RSTATE, A64_SP, 16);
				a64_ldp_post(A64_FP, A64_LR, A64_SP, 16);
				a64_ret();
				return true;
			}
		}
		case 150: /* isync — fall through to interpreter so execute_isync() runs
		           * execute_invalidate_cache_range(), flushing any deferred icbi.
		           * Without this, icbi sets cache_range but isync-as-NOP never
		           * flushes it, leaving stale JIT blocks for overwritten code. */
			return false;

		case 0: /* mcrf crfD,crfS — copy CR field */
		{
			uint32_t crfD = (op >> 23) & 0x7;
			uint32_t crfS = (op >> 18) & 0x7;
			if (crfD == crfS) return true; /* NOP */
			lazy_flush_cr0();
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			uint32_t src_sh = (7 - crfS) * 4;
			uint32_t dst_sh = (7 - crfD) * 4;
			/* Extract source field */
			a64_mov_reg(RTMP1, RTMP0);
			if (src_sh) { emit_load_imm32(RTMP2, src_sh); emit32(0x1AC02400 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* LSR */ }
			emit_load_imm32(RTMP2, 0xF);
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* AND */
			/* Shift to destination position */
			if (dst_sh) { emit_load_imm32(RTMP2, dst_sh); emit32(0x1AC02000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* LSL */ }
			/* Clear destination field and OR in new value */
			emit_load_imm32(RTMP2, ~(0xFU << dst_sh));
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* AND clear dest */
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ORR merge */
			a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
			return true;
		}

		/* CR logical operations: crand, cror, crxor, crnor, crandc, creqv, crorc, crnand */
		case 257: /* crand  crbD,crbA,crbB */
		case 449: /* cror   crbD,crbA,crbB */
		case 193: /* crxor  crbD,crbA,crbB */
		case 33:  /* crnor  crbD,crbA,crbB */
		case 129: /* crandc crbD,crbA,crbB */
		case 289: /* creqv  crbD,crbA,crbB */
		case 417: /* crorc  crbD,crbA,crbB */
		case 225: /* crnand crbD,crbA,crbB */
		{
			uint32_t crbD = (op >> 21) & 0x1F;
			uint32_t crbA = (op >> 16) & 0x1F;
			uint32_t crbB = (op >> 11) & 0x1F;
			lazy_flush_cr0();
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			/* Extract bit A: (CR >> (31-crbA)) & 1 → RTMP1 */
			a64_mov_reg(RTMP1, RTMP0);
			if (31 - crbA) { emit_load_imm32(RTMP2, 31 - crbA); emit32(0x1AC02400 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); }
			emit_load_imm32(RTMP2, 1);
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* AND #1 */
			/* Extract bit B: (CR >> (31-crbB)) & 1 → RTMP2 */
			a64_mov_reg(RTMP2, RTMP0);
			if (31 - crbB) { uint32_t sh = 31 - crbB; emit_load_imm32(RTMP0, sh); emit32(0x1AC02400 | (RTMP0 << 16) | (RTMP2 << 5) | RTMP2); }
			emit_load_imm32(RTMP0, 1);
			emit32(0x0A000000 | (RTMP0 << 16) | (RTMP2 << 5) | RTMP2); /* AND #1 */
			/* Compute result bit → RTMP1 */
			switch (xo) {
			case 257: /* crand:  a & b */
				emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); break;
			case 449: /* cror:   a | b */
				emit32(0x2A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); break;
			case 193: /* crxor:  a ^ b */
				emit32(0x4A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); break;
			case 33:  /* crnor:  ~(a | b) = NOR */
				emit32(0x2A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* OR */
				emit32(0x2A2003E0 | (RTMP1 << 16) | RTMP1); /* MVN RTMP1 = ~RTMP1 */
				emit_load_imm32(RTMP2, 1);
				emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* AND #1 */
				break;
			case 129: /* crandc: a & ~b */
				emit32(0x0A200000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* BIC */ break;
			case 289: /* creqv:  ~(a ^ b) = XNOR */
				emit32(0x4A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* XOR */
				emit32(0x2A2003E0 | (RTMP1 << 16) | RTMP1); /* MVN RTMP1 = ~RTMP1 */
				emit_load_imm32(RTMP2, 1);
				emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* AND #1 */
				break;
			case 417: /* crorc:  a | ~b */
				/* ORN gives a | ~b over the FULL 32-bit register: with 1-bit inputs the
				 * result is 0xFFFFFFFE/0xFFFFFFFF, and the merge below ORs that whole
				 * value into CR (CR becomes ~0) — this was the root cause of the
				 * deterministic 68k-region boot crash (crorc miscompilation).
				 * It is also wrong on truth value: crorc(0,1) must be 0.
				 * Mask back to bit 0, same as the MVN-based ops above. */
				emit32(0x2A200000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* ORN */
				emit_load_imm32(RTMP2, 1);
				emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* AND #1 */
				break;
			case 225: /* crnand: ~(a & b) */
				emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* AND */
				emit32(0x2A2003E0 | (RTMP1 << 16) | RTMP1); /* MVN RTMP1 = ~RTMP1 */
				emit_load_imm32(RTMP2, 1);
				emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* AND #1 */
				break;
			}
			/* Write result bit into CR at position (31-crbD) */
			lazy_flush_cr0();
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR); /* reload CR */
			uint32_t dst_bit = 31 - crbD;
			emit_load_imm32(RTMP2, ~(1u << dst_bit));
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* AND: clear dest bit */
			if (dst_bit) { emit_load_imm32(RTMP2, dst_bit); emit32(0x1AC02000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* LSL result */ }
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ORR: merge */
			a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
			return true;
		}
		default:
			return false; /* unknown opcode 19 sub-op: stop compilation */
		}
	}

	case 17: /* sc — system call: fall back so interpreter raises it */
		return false;

	case 18: /* b/bl (unconditional branch) */
	{
		int32_t li = ((op & 0x03FFFFFC) ^ 0x02000000) - 0x02000000;
		bool lk = op & 1;
		bool aa = op & 2;
		uint32_t target = aa ? (uint32_t)li : (pc + li);
		/* Validate target is in compilable range — reject jumps to EMUL_OP trampolines */
		if ((target >> 26) == 6) return false; /* EMUL_OP opcode range */
		if (lk) {
			emit_load_imm32(RTMP0, (int32_t)(pc + 4));
			a64_str_w_imm(RTMP0, RSTATE, PPCR_LR);
		}
		lazy_flush_cr0();
		emit_epilogue_with_pc(target);
		return true;
	}

	case 42: /* lha rD,d(rA) — load halfword algebraic (sign-extended) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP1, (int32_t)simm); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		a64_ldrh_reg(RTMP1, RMEMBASE, RTMP0); /* LDRH Wt, [Xn] */
		emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1); /* REV16 (byte-swap) */
		/* Sign-extend from 16 to 32 bits */
		emit32(0x13003C00 | (RTMP1 << 5) | RTMP1); /* SXTH Wd, Wn */
		emit_store_gpr(RTMP1, rd);
		return true;

	case 2: /* tdi — trap doubleword immediate: fall back to interpreter */
	case 3: /* twi — trap word immediate: fall back so trap conditions are evaluated */
		return false;

	case 4: /* AltiVec via NEON */
	{
		uint32_t vxo = op & 0x7FF;
		uint32_t vao = op & 0x3F;
		uint32_t vd = VR_VD(op), va = VR_VA(op), vb = VR_VB(op), vc = VR_VC(op);
		switch (vxo) {
		case 0: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E208400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 64: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E608400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 128: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA08400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 10: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E20D400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1024: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E208400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1088: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E608400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1152: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA08400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 74: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA0D400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1028: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E201C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1092: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E601C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1156: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA01C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1220: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E201C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1284: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA01C00|(1<<16)|(0<<5)|0); emit32(0x6E205800|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1034: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E20F400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1098: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA0F400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 266: emit_load_vr(0,vb); emit32(0x4EA1D800|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 330: emit_load_vr(0,vb); emit32(0x6EA1D800|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 394: emit_load_vr(0,vb); emit32(0x4E218800|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 458: emit_load_vr(0,vb); emit32(0x4EA19800|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 6: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E208C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 70: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E608C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 134: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA08C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 198: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E20E400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 908: { int32_t s=((va&0x1F)|(va&0x10?0xFFFFFFE0:0)); emit_load_imm32(RTMP0,s); emit32(0x4E040C00|(RTMP0<<5)|0); emit_store_vr(0,vd); return true; }
		case 844: { int32_t s=((va&0x1F)|(va&0x10?0xFFFFFFE0:0)); emit_load_imm32(RTMP0,s&0xFFFF); emit32(0x4E020C00|(RTMP0<<5)|0); emit_store_vr(0,vd); return true; }
		case 780: { int32_t s=((va&0x1F)|(va&0x10?0xFFFFFFE0:0)); emit_load_imm32(RTMP0,s&0xFF); emit32(0x4E010C00|(RTMP0<<5)|0); emit_store_vr(0,vd); return true; }

		case 258: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E206400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmaxsb SMAX.16B */
		case 322: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E606400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmaxsh SMAX.8H */
		case 386: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA06400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmaxsw SMAX.4S */
		case 2: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E206400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmaxub UMAX.16B */
		case 66: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E606400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmaxuh UMAX.8H */
		case 130: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA06400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmaxuw UMAX.4S */
		case 770: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E206C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vminsb SMIN.16B */
		case 834: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E606C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vminsh SMIN.8H */
		case 898: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA06C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vminsw SMIN.4S */
		case 514: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E206C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vminub UMIN.16B */
		case 578: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E606C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vminuh UMIN.8H */
		case 642: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA06C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vminuw UMIN.4S */
		case 260: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E205400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vslb USHL.16B */
		case 324: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E605400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vslh USHL.8H */
		case 388: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA05400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vslw USHL.4S */
		case 772: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E20B800|(1<<5)|1); emit32(0x4E205400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsrb NEG+USHL.16B */
		case 836: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E60B800|(1<<5)|1); emit32(0x4E605400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsrh */
		case 900: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA0B800|(1<<5)|1); emit32(0x4EA05400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsrw */
		case 516: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E204400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsrab SSHL.16B (arith) */
		case 580: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E60B800|(1<<5)|1); emit32(0x4E604400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsrah */
		case 644: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA0B800|(1<<5)|1); emit32(0x4EA04400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsraw */
		case 4: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E205400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vrlb USHL.16B (rotate=shift by variable amount) */
		case 68: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E605400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vrlh USHL.8H */
		case 132: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA05400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vrlw USHL.4S */
		case 524: { uint32_t idx=va; emit_load_vr(0,vb); emit32(0x4E010400|((idx*2+1)<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vspltb DUP.16B */
		case 588: { uint32_t idx=va; emit_load_vr(0,vb); emit32(0x4E020400|((idx*4+2)<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vsplth DUP.8H */
		case 652: { uint32_t idx=va; emit_load_vr(0,vb); emit32(0x4E040400|((idx*8+4)<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vspltw DUP.4S */
		case 522: emit_load_vr(0,vb); emit32(0x4EA18800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vrfip FRINTP */
		case 586: emit_load_vr(0,vb); emit32(0x4E219800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vrfim FRINTM */
		case 198+768: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E20E400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgefp FCMGE */
		case 454: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA0E400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgtfp FCMGT */
		case 774: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E203400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgtsb CMGT.16B (signed) */
		case 838: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E603400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgtsh */
		case 902: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA03400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgtsw */
		case 518: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E203400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgtub CMHI.16B (unsigned) */
		case 582: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E603400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgtuh */
		case 646: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA03400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgtuw */
		case 1282: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E20A400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vavgub URHADD.16B */
		case 1346: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E60A400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vavguh */
		case 1410: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA0A400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vavguw */
		case 1794: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E201400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vavgsb SRHADD.16B */
		case 1858: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E601400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vavgsh */
		case 1922: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0EA01400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vavgsw */
		case 768: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E207C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vaddubs UQADD.16B (saturating) */
		case 832: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E607C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vadduhs UQADD.8H */
		case 896: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA07C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vadduws UQADD.4S */
		case 1792: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E202C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsububs UQSUB.16B */
		case 1856: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E602C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsubuhs UQSUB.8H */
		case 1920: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA02C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsubuws UQSUB.4S */
		case 512: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E207C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vaddsbs SQADD.16B */
		case 576: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E607C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vaddshs SQADD.8H */
		case 640: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA07C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vaddsws SQADD.4S */
		case 1536: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E202C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsubsbs SQSUB.16B */
		case 1600: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E602C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsubshs SQSUB.8H */
		case 12: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E20C400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmrghb ZIP1.16B */
		case 76: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E60C400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmrghh ZIP1.8H */
		case 140: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA0C400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmrghw ZIP1.4S */
		case 268: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E20C800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmrglb ZIP2.16B */
		case 332: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E60C800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmrglh ZIP2.8H */
		case 396: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA0C800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmrglw ZIP2.4S */

		case 846: { emit_load_vr(0,vb); emit32(0x4E21C800|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vcfsx SCVTF.4S */
		case 910: { emit_load_vr(0,vb); emit32(0x6E21C800|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vcfux UCVTF.4S */
		case 970: { emit_load_vr(0,vb); emit32(0x4EA1B800|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vctsxs FCVTZS.4S */
		case 906: { emit_load_vr(0,vb); emit32(0x6EA1B800|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vctuxs FCVTZU.4S */
		case 354: { emit_load_vr(0,vb); emit32(0x4E21D800|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vexptefp FRECPE (approx) */
		case 418: { emit_load_vr(0,vb); emit32(0x4EA1D800|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vlogefp (approx via FRECPE) */
		case 8: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E209C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmuloub UMULL.8H (odd bytes) */
		case 72: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E60A000|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmulouh UMULL.4S */
		case 264: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E20A000|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmuleub UMULL2.8H */
		case 328: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E60A000|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmuleuh UMULL2.4S */
		case 776: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E209C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmulosb SMULL.8H */
		case 840: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E60C000|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmulosh SMULL.4S */
		case 520: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E20C000|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmulesb SMULL2.8H */
		case 584: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E60C000|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmulesh SMULL2.4S */
		case 14: emit_load_vr(0,vb); emit32(0x0E212800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkuhum UZP1.8H (narrow) */
		case 78: emit_load_vr(0,vb); emit32(0x0E612800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkuwum UZP1.4S */
		case 398: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E216800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkshus SQXTUN.8B */
		case 462: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E616800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkswus SQXTUN.4H */
		case 270: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E214800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkshss SQXTN.8B */
		case 334: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E614800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkswss SQXTN.4H */
		case 142: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x2E212800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkuhus UQXTN.8B */
		case 206: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x2E612800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkuwus UQXTN.4H */
		case 814: emit_load_vr(0,vb); emit32(0x0E212800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vupkhsb SXTL.8H (unpack high signed byte) */
		case 878: emit_load_vr(0,vb); emit32(0x0E612800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vupkhsh SXTL.4S */
		case 942: emit_load_vr(0,vb); emit32(0x4E212800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vupklsb SXTL2.8H */
		case 1006: emit_load_vr(0,vb); emit32(0x4E612800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vupklsh SXTL2.4S */
		case 452: { uint32_t sh=(op>>6)&0xF; emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E010000|(sh<<11)); emit_store_vr(0,vd); return true; } /* vsldoi EXT.16B */
		case 1036: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E205400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsl SSHL.16B (shift left by register) */
		case 1100: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E20B800|(1<<5)|1); emit32(0x6E205400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsr NEG+USHL.16B (negate shift, then shift left = right shift) */
		case 1604: return true; /* mtvscr NOP */
		case 1540: emit_load_imm32(RTMP0,0); emit32(0x4E010C00|(RTMP0<<5)|0); emit_store_vr(0,vd); return true; /* mfvscr - return 0 */
case 782: /* vpkpx — pack pixel 32→16 bit (approximate narrow) */
			emit_load_vr(0, va); emit_load_vr(1, vb);
			emit32(0x0E612800 | (1 << 16) | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 974: /* vupkhpx — unpack high pixel (widen) */
			emit_load_vr(0, vb);
			emit32(0x2F10A400 | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 1038: /* vupklpx — unpack low pixel */
			emit_load_vr(0, vb);
			emit32(0x6F10A400 | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 1928: /* vsum4ubs */
			emit_load_vr(0, va); emit_load_vr(1, vb);
			emit32(0x6E202800 | (0 << 5) | 0);
			emit32(0x6E602800 | (0 << 5) | 0);
			emit32(0x4EA08400 | (1 << 16) | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 1672: /* vsum4sbs */
			emit_load_vr(0, va); emit_load_vr(1, vb);
			emit32(0x4E202800 | (0 << 5) | 0);
			emit32(0x4E602800 | (0 << 5) | 0);
			emit32(0x4EA08400 | (1 << 16) | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 1608: /* vsum4shs */
			emit_load_vr(0, va); emit_load_vr(1, vb);
			emit32(0x4E602800 | (0 << 5) | 0);
			emit32(0x4EA08400 | (1 << 16) | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 1800: /* vsum2sws */
			emit_load_vr(0, va); emit_load_vr(1, vb);
			emit32(0x4EA02800 | (0 << 5) | 0);
			emit32(0x0EA12800 | (0 << 5) | 0);
			emit32(0x4EA08400 | (1 << 16) | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 1932: /* vsumsws — sum all words */
			emit_load_vr(0, va); emit_load_vr(1, vb);
			emit32(0x4EB1B800 | (0 << 5) | 0);
			emit32(0x4EA08400 | (1 << 16) | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 1356: /* vslo — shift left by octet (approx: pass through) */
			emit_load_vr(0, va); emit_store_vr(0, vd); return true;
		case 1420: /* vsro — shift right by octet (approx) */
			emit_load_vr(0, va); emit_store_vr(0, vd); return true;
		default: break;
		}
		switch (vao) {
		case 46: emit_load_vr(0,va); emit_load_vr(1,vc); emit_load_vr(2,vb); emit32(0x4E21CC00|(1<<16)|(0<<5)|2); emit_store_vr(2,vd); return true;
		case 47: emit_load_vr(0,va); emit_load_vr(1,vc); emit_load_vr(2,vb); emit32(0x4EA1CC00|(1<<16)|(0<<5)|2); emit_store_vr(2,vd); return true;
		case 43: emit_load_vr(0,va); emit_load_vr(1,vb); emit_load_vr(2,vc); emit32(0x4E002000|(2<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 42: emit_load_vr(0,va); emit_load_vr(1,vb); emit_load_vr(2,vc); emit32(0x6E601C00|(1<<16)|(0<<5)|2); emit_store_vr(2,vd); return true;

		case 32: emit_load_vr(0,va); emit_load_vr(1,vc); emit_load_vr(2,vb); emit32(0x4E21CC00|(1<<16)|(0<<5)|2); emit_store_vr(2,vd); return true; /* vmhaddshs (approx via FMLA) */
		case 33: emit_load_vr(0,va); emit_load_vr(1,vc); emit_load_vr(2,vb); emit32(0x4E21CC00|(1<<16)|(0<<5)|2); emit_store_vr(2,vd); return true; /* vmhraddshs (approx) */
		case 34: emit_load_vr(0,va); emit_load_vr(1,vc); emit_load_vr(2,vb); emit32(0x4E609C00|(1<<16)|(0<<5)|0); emit32(0x4E608400|(2<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmladduhm MUL+ADD */
		case 36: emit_load_vr(0,va); emit_load_vr(1,vb); emit_load_vr(2,vc); emit32(0x4E209C00|(1<<16)|(0<<5)|0); emit32(0x4E208400|(2<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmsumubm (approx) */
		case 37: emit_load_vr(0,va); emit_load_vr(1,vb); emit_load_vr(2,vc); emit32(0x4E609C00|(1<<16)|(0<<5)|0); emit32(0x4E608400|(2<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmsumshm */
		case 38: emit_load_vr(0,va); emit_load_vr(1,vb); emit_load_vr(2,vc); emit32(0x6E609C00|(1<<16)|(0<<5)|0); emit32(0x6E608400|(2<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmsumshs */
		case 40: emit_load_vr(0,va); emit_load_vr(1,vb); emit_load_vr(2,vc); emit32(0x6E209C00|(1<<16)|(0<<5)|0); emit32(0x6E208400|(2<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmsumubm */
		case 41: emit_load_vr(0,va); emit_load_vr(1,vb); emit_load_vr(2,vc); emit32(0x4E609C00|(1<<16)|(0<<5)|0); emit32(0x4E608400|(2<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmsumuhm */
		default: return false;
		}
	}

	case 7: /* mulli rD,rA,SIMM */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x1B007C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* MUL Wd,Wn,Wm */
		emit_store_gpr(RTMP0, rd);
		return true;

	case 13: /* addic. rD,rA,SIMM (sets XER[CA] + CR0) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x2B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADDS */
		emit_store_gpr(RTMP0, rd);
		emit_write_xer_ca_from_carry();
		lazy_update_cr0(RTMP0);
		return true;

	case 35: /* lbzu rD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		a64_ldrb_reg(RTMP1, RMEMBASE, RTMP0);
		emit_store_gpr(RTMP1, rd);
		return true;

	case 39: /* stbu rS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP1, rd);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP2, (int32_t)simm);
		emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		a64_strb_reg(RTMP1, RMEMBASE, RTMP0);
		return true;

	case 41: /* lhzu rD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		a64_ldrh_reg(RTMP1, RMEMBASE, RTMP0);
		emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1);
		emit_store_gpr(RTMP1, rd);
		return true;

	case 43: /* lhau rD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		a64_ldrh_reg(RTMP1, RMEMBASE, RTMP0);
		emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1);
		emit32(0x13003C00 | (RTMP1 << 5) | RTMP1);
		emit_store_gpr(RTMP1, rd);
		return true;

	case 45: /* sthu rS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP1, rd);
		emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP2, (int32_t)simm);
		emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		a64_strh_reg(RTMP1, RMEMBASE, RTMP0);
		return true;

	case 49: /* lfsu frD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		a64_ldr_w_reg(RTMP1, RMEMBASE, RTMP0);
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
		emit32(0x1E270000 | (RTMP1 << 5) | 0);
		emit32(0x1E22C000 | (0 << 5) | 0);
		emit_store_fpr(0, rd);
		return true;

	case 51: /* lfdu frD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		a64_ldr_x_reg(RTMP1, RMEMBASE, RTMP0);
		emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
		emit32(0x9E670000 | (RTMP1 << 5) | 0);
		emit_store_fpr(0, rd);
		return true;

	case 53: /* stfsu frS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_fpr(0, rd);
		emit32(0x1E624000 | (0 << 5) | 0);
		emit32(0x1E260000 | (0 << 5) | RTMP1);
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP2, (int32_t)simm);
		emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		a64_str_w_reg(RTMP1, RMEMBASE, RTMP0);
		return true;

	case 55: /* stfdu frS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_fpr(0, rd);
		emit32(0x9E660000 | (0 << 5) | RTMP1);
		emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP2, (int32_t)simm);
		emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		a64_str_x_reg(RTMP1, RMEMBASE, RTMP0);
		return true;

	case 46: /* lmw rD,d(rA) — load multiple words */
	{
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP1, (int32_t)simm); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		for (uint32_t r = rd; r < 32; r++) {
			a64_ldr_w_reg(RTMP1, RMEMBASE, RTMP0); /* LDR Wt, [Xn] */
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV */
			emit_store_gpr(RTMP1, r);
			if (r < 31) emit32(0x11001000 | (RTMP0 << 5) | RTMP0); /* ADD Wn, Wn, #4 */
		}
		return true;
	}

	case 47: /* stmw rS,d(rA) — store multiple words */
	{
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP1, (int32_t)simm); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		for (uint32_t r = rd; r < 32; r++) {
			emit_load_gpr(RTMP1, r);
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV */
			a64_str_w_reg(RTMP1, RMEMBASE, RTMP0); /* STR Wt, [Xn] */
			if (r < 31) emit32(0x11001000 | (RTMP0 << 5) | RTMP0); /* ADD +4 */
		}
		return true;
	}

	case 48: /* lfs frD,d(rA) — load float single */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP1, (int32_t)simm); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		/* Load 32-bit float, byte-swap, convert to double */
		a64_ldr_w_reg(RTMP1, RMEMBASE, RTMP0); /* LDR Wt, [Xn] */
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV Wd */
		/* Move int to float reg: FMOV Sd, Wn */
		emit32(0x1E270000 | (RTMP1 << 5) | 0); /* FMOV S0, Wn */
		/* Convert single to double: FCVT Dd, Sd */
		emit32(0x1E22C000 | (0 << 5) | 0); /* FCVT D0, S0 */
		emit_store_fpr(0, rd);
		return true;

	case 50: /* lfd frD,d(rA) — load float double */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP1, (int32_t)simm); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		/* Load 64-bit, byte-swap */
		a64_ldr_x_reg(RTMP1, RMEMBASE, RTMP0); /* LDR Xt, [Xn] (64-bit) */
		emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1); /* REV Xd, Xn (64-bit byte-swap) */
		/* Move to FP reg: FMOV Dd, Xn */
		emit32(0x9E670000 | (RTMP1 << 5) | 0); /* FMOV D0, Xn */
		emit_store_fpr(0, rd);
		return true;

	case 52: /* stfs frS,d(rA) — store float single */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_fpr(0, rd);
		/* Convert double to single: FCVT Sd, Dd */
		emit32(0x1E624000 | (0 << 5) | 0);
		/* Move float to int: FMOV Wn, Sd */
		emit32(0x1E260000 | (0 << 5) | RTMP1);
		/* Byte-swap and store */
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV */
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP2, (int32_t)simm); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
		a64_str_w_reg(RTMP1, RMEMBASE, RTMP0); /* STR Wt, [Xn] */
		return true;

	case 54: /* stfd frS,d(rA) — store float double */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_fpr(0, rd);
		/* Move FP to int: FMOV Xn, Dd */
		emit32(0x9E660000 | (0 << 5) | RTMP1);
		/* Byte-swap 64-bit */
		emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1); /* REV Xd, Xn */
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP2, (int32_t)simm); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
		/* STR Xt, [Xn] (64-bit store) */
		a64_str_x_reg(RTMP1, RMEMBASE, RTMP0);
		return true;

	case 63: /* double-precision FP ops */
	{
		uint32_t xo10 = (op >> 1) & 0x3FF;
		uint32_t xo5 = (op >> 1) & 0x1F;
		uint32_t frd = PPC_RD(op);
		uint32_t fra = PPC_RA(op);
		uint32_t frb = (op >> 11) & 0x1F;
		uint32_t frc = (op >> 6) & 0x1F;

		/* X-form FP ops (10-bit XO) */
		switch (xo10) {
		case 72: /* fmr frD,frB — FP move register */
			emit_load_fpr(0, frb);
			emit_store_fpr(0, frd);
			return true;

		case 40: /* fneg frD,frB — FP negate */
			emit_load_fpr(0, frb);
			emit32(0x1E614000 | (0 << 5) | 0); /* FNEG Dd, Dn */
			emit_store_fpr(0, frd);
			return true;

		case 264: /* fabs frD,frB — FP absolute value */
			emit_load_fpr(0, frb);
			emit32(0x1E60C000 | (0 << 5) | 0); /* FABS Dd, Dn */
			emit_store_fpr(0, frd);
			return true;

		case 136: /* fnabs frD,frB — FP negative absolute */
			emit_load_fpr(0, frb);
			emit32(0x1E60C000 | (0 << 5) | 0); /* FABS */
			emit32(0x1E614000 | (0 << 5) | 0); /* FNEG */
			emit_store_fpr(0, frd);
			return true;

		case 0: /* fcmpu crD,frA,frB */
		{
			uint32_t crd = (op >> 23) & 0x7;
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E602000 | (1 << 16) | (0 << 5)); /* FCMP Dn, Dm */
			/* ARM64 FCMP sets NZCV: N=less, Z=equal, C=greater_or_unord, V=unordered */
			a64_movz(RTMP0, 0, 0);
			emit_load_imm32(RTMP1, 8); /* LT */
			emit32(0x1A800000 | (RTMP0 << 16) | (0xB << 12) | (RTMP1 << 5) | RTMP0); /* CSEL LT */
			emit_load_imm32(RTMP1, 4); /* GT */
			emit32(0x1A800000 | (RTMP0 << 16) | (0xC << 12) | (RTMP1 << 5) | RTMP0); /* CSEL GT */
			emit_load_imm32(RTMP1, 2); /* EQ */
			emit32(0x1A800000 | (RTMP0 << 16) | (0x0 << 12) | (RTMP1 << 5) | RTMP0); /* CSEL EQ */
			/* TODO: handle unordered (set FU bit) */
			/* OR in XER[SO] as bit 0 for VXSNAN etc — for now just copy SO */
			emit_or_xer_so_into_cr_nibble(RTMP0);
			uint32_t shift = (7 - crd) * 4;
			if (shift) { emit_load_imm32(RTMP1, shift); emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			lazy_flush_cr0();
			a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CR);
			emit_load_imm32(RTMP2, ~(0xF << shift));
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
			emit32(0x2A000000 | (RTMP0 << 16) | (RTMP1 << 5) | RTMP1);
			a64_str_w_imm(RTMP1, RSTATE, PPCR_CR);
			return true;
		}


		case 32: /* fcmpo crD,frA,frB — same as fcmpu for our purposes */
		{
			uint32_t crd = (op >> 23) & 0x7;
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E602000 | (1 << 16) | (0 << 5));
			a64_movz(RTMP0, 0, 0);
			emit_load_imm32(RTMP1, 8);
			emit32(0x1A800000 | (RTMP0 << 16) | (0xB << 12) | (RTMP1 << 5) | RTMP0);
			emit_load_imm32(RTMP1, 4);
			emit32(0x1A800000 | (RTMP0 << 16) | (0xC << 12) | (RTMP1 << 5) | RTMP0);
			emit_load_imm32(RTMP1, 2);
			emit32(0x1A800000 | (RTMP0 << 16) | (0x0 << 12) | (RTMP1 << 5) | RTMP0);
			uint32_t shift = (7 - crd) * 4;
			if (shift) { emit_load_imm32(RTMP1, shift); emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			lazy_flush_cr0();
			a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CR);
			emit_load_imm32(RTMP2, ~(0xF << shift));
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
			emit32(0x2A000000 | (RTMP0 << 16) | (RTMP1 << 5) | RTMP1);
			a64_str_w_imm(RTMP1, RSTATE, PPCR_CR);
			return true;
		}

		case 12: /* frsp frD,frB — round to single precision */
			emit_load_fpr(0, frb);
			emit32(0x1E624000 | (0 << 5) | 0); /* FCVT Sd, Dd */
			emit32(0x1E22C000 | (0 << 5) | 0); /* FCVT Dd, Sd */
			emit_store_fpr(0, frd);
			return true;

		case 14: /* fctiw frD,frB — convert to integer word (round per FPSCR) */
			emit_load_fpr(0, frb);
			emit32(0x9E780000 | (0 << 5) | RTMP0); /* FCVTZS Xd, Dn (toward zero) */
			/* Store as int in FPR (low 32 bits) */
			emit32(0x9E670000 | (RTMP0 << 5) | 0); /* FMOV Dd, Xn */
			emit_store_fpr(0, frd);
			return true;

		case 15: /* fctiwz frD,frB — convert to integer word (round toward zero) */
			emit_load_fpr(0, frb);
			emit32(0x9E780000 | (0 << 5) | RTMP0); /* FCVTZS Xd, Dn */
			emit32(0x9E670000 | (RTMP0 << 5) | 0); /* FMOV Dd, Xn */
			emit_store_fpr(0, frd);
			return true;

		case 583: /* mffs frD — move from FPSCR */
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_FPSCR);
			emit32(0x9E670000 | (RTMP0 << 5) | 0); /* FMOV Dd, Xn */
			emit_store_fpr(0, frd);
			return true;

		case 711: /* mtfsfi crfD,IMM — set FPSCR field to 4-bit immediate */
		{
			uint32_t crfD = (op >> 23) & 0x7;
			uint32_t imm = (op >> 12) & 0xF;
			uint32_t shift = (7 - crfD) * 4;
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_FPSCR);
			emit_load_imm32(RTMP1, ~(0xF << shift));
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* clear field */
			emit_load_imm32(RTMP1, imm << shift);
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* set field */
			a64_str_w_imm(RTMP0, RSTATE, PPCR_FPSCR);
			if (crfD == 7) emit_sync_fpscr_rounding(); /* field 7 contains RN */
			return true;
		}

		case 70: /* mtfsb0 crbD — clear FPSCR bit */
		{
			uint32_t crbD = (op >> 21) & 0x1F;
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_FPSCR);
			emit_load_imm32(RTMP1, ~(1u << (31 - crbD)));
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			a64_str_w_imm(RTMP0, RSTATE, PPCR_FPSCR);
			if (crbD >= 30) emit_sync_fpscr_rounding(); /* RN bits */
			return true;
		}

		case 38: /* mtfsb1 crbD — set FPSCR bit */
		{
			uint32_t crbD = (op >> 21) & 0x1F;
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_FPSCR);
			emit_load_imm32(RTMP1, 1u << (31 - crbD));
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			a64_str_w_imm(RTMP0, RSTATE, PPCR_FPSCR);
			if (crbD >= 30) emit_sync_fpscr_rounding();
			return true;
		}

		case 134: /* mtfsf FM,frB — move to FPSCR fields */
		{
			uint32_t fm = (op >> 17) & 0xFF;
			emit_load_fpr(0, frb);
			emit32(0x9E660000 | (0 << 5) | RTMP0); /* FMOV Xn, Dd */
			/* Build mask from FM (each bit enables a 4-bit field) */
			uint32_t mask = 0;
			for (int i = 0; i < 8; i++)
				if (fm & (1 << (7 - i))) mask |= (0xF << ((7 - i) * 4));
			a64_ldr_w_imm(RTMP1, RSTATE, PPCR_FPSCR);
			emit_load_imm32(RTMP2, ~mask);
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* clear target fields */
			emit_load_imm32(RTMP2, mask);
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* mask source */
			emit32(0x2A000000 | (RTMP0 << 16) | (RTMP1 << 5) | RTMP1); /* merge */
			a64_str_w_imm(RTMP1, RSTATE, PPCR_FPSCR);
			if (fm & 1) emit_sync_fpscr_rounding(); /* field 7 (RN) modified */
			return true;
		}

		case 23: /* fsel frD,frA,frC,frB — if frA >= 0 then frC else frB */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frc);
			emit_load_fpr(2, frb);
			/* Compare frA with 0.0 */
			emit32(0x1E602010 | (0 << 5)); /* FCMP Dn, #0.0 */
			/* FCSEL Dd, Dc, Db, GE */
			emit32(0x1E600C00 | (2 << 16) | (0xA << 12) | (1 << 5) | 0); /* FCSEL D0,D1,D2,GE */
			emit_store_fpr(0, frd);
			return true;
		
		case 64: /* mcrfs crD,crS — move from FPSCR field to CR field */
		{
			uint32_t crd_f = (op >> 23) & 0x7;
			uint32_t crs_f = (op >> 18) & 0x7;
			/* Read FPSCR field and write to CR field */
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_FPSCR);
			uint32_t src_sh = (7 - crs_f) * 4;
			uint32_t dst_sh = (7 - crd_f) * 4;
			a64_mov_reg(RTMP1, RTMP0);
			if (src_sh) { emit_load_imm32(RTMP2, src_sh); emit32(0x1AC02400 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); }
			emit_load_imm32(RTMP2, 0xF);
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
			if (dst_sh) { emit_load_imm32(RTMP2, dst_sh); emit32(0x1AC02000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); }
			lazy_flush_cr0();
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			emit_load_imm32(RTMP2, ~(0xFU << dst_sh));
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
			return true;
		}

		case 26: /* frsqrte frD,frB — reciprocal square root estimate */
			emit_load_fpr(0, frb);
			emit32(0x1E61C000 | (0 << 5) | 0); /* FSQRT Dd,Dn */
			emit32(0x1E6E1000 | 1); /* FMOV D1, #1.0 */
			emit32(0x1E611800 | (0 << 16) | (1 << 5) | 0); /* FDIV D0, D1, D0 */
			emit_store_fpr(0, frd);
			return true;

		case 22: /* fsqrt frD,frB — floating-point square root (double) */
			emit_load_fpr(0, frb);
			emit32(0x1E61C000 | (0 << 5) | 0); /* FSQRT Dd, Dn */
			emit_store_fpr(0, frd);
			return true;
		default: break; /* fall through to 5-bit XO check */
		}

		/* A-form FP ops (5-bit XO) */
		switch (xo5) {
		case 21: /* fadd frD,frA,frB */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E602800 | (1 << 16) | (0 << 5) | 0); /* FADD Dd, Dn, Dm */
			emit_store_fpr(0, frd);
			return true;

		case 20: /* fsub frD,frA,frB */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E603800 | (1 << 16) | (0 << 5) | 0); /* FSUB Dd, Dn, Dm */
			emit_store_fpr(0, frd);
			return true;

		case 25: /* fmul frD,frA,frC */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frc);
			emit32(0x1E600800 | (1 << 16) | (0 << 5) | 0); /* FMUL Dd, Dn, Dm */
			emit_store_fpr(0, frd);
			return true;

		case 18: /* fdiv frD,frA,frB */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E601800 | (1 << 16) | (0 << 5) | 0); /* FDIV Dd, Dn, Dm */
			emit_store_fpr(0, frd);
			return true;

		case 29: /* fmadd frD,frA,frC,frB = frA*frC+frB */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frc);
			emit_load_fpr(2, frb);
			emit32(0x1F400000 | (1 << 16) | (2 << 10) | (0 << 5) | 0); /* FMADD Dd,Dn,Dm,Da */
			emit_store_fpr(0, frd);
			return true;

		case 28: /* fmsub frD,frA,frC,frB = frA*frC-frB */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frc);
			emit_load_fpr(2, frb);
			emit32(0x1F608000 | (1 << 16) | (2 << 10) | (0 << 5) | 0); /* FNMSUB: Dn*Dm - Da */
			emit_store_fpr(0, frd);
			return true;

		case 31: /* fnmadd frD,frA,frC,frB = -(frA*frC+frB) */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frc);
			emit_load_fpr(2, frb);
			emit32(0x1F600000 | (1 << 16) | (2 << 10) | (0 << 5) | 0); /* FNMADD */
			emit_store_fpr(0, frd);
			return true;

		case 30: /* fnmsub frD,frA,frC,frB = -(frA*frC-frB) = frB - frA*frC */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frc);
			emit_load_fpr(2, frb);
			emit32(0x1F408000 | (1 << 16) | (2 << 10) | (0 << 5) | 0); /* FMSUB: Da - Dn*Dm */
			emit_store_fpr(0, frd);
			return true;

		/* 64-bit FP conversions (G5/PPC970) */
		case 814: /* fctid frD,frB — FP to 64-bit integer (round per FPSCR) */
			emit_load_fpr(0, frb);
			emit32(0x9E700000 | (0 << 5) | RTMP0); /* FCVTNS Xd, Dn (round to nearest) */
			/* Store as 64-bit integer in FPR slot (PPC stores int result in FPR) */
			emit32(0x9E670000 | (RTMP0 << 5) | 0); /* FMOV Dd, Xn */
			emit_store_fpr(0, frd);
			return true;

		case 815: /* fctidz frD,frB — FP to 64-bit integer (round toward zero) */
			emit_load_fpr(0, frb);
			emit32(0x9E780000 | (0 << 5) | RTMP0); /* FCVTZS Xd, Dn */
			emit32(0x9E670000 | (RTMP0 << 5) | 0); /* FMOV Dd, Xn */
			emit_store_fpr(0, frd);
			return true;

		case 846: /* fcfid frD,frB — 64-bit integer to FP */
			emit_load_fpr(0, frb);
			emit32(0x9E660000 | (0 << 5) | RTMP0); /* FMOV Xn, Dd */
			emit32(0x9E620000 | (RTMP0 << 5) | 0); /* SCVTF Dd, Xn */
			emit_store_fpr(0, frd);
			return true;

		default:
			return false;
		}
	}

		return true;

	case 30: /* rld* — 64-bit rotate/shift family */
	{
		uint32_t rs = PPC_RS(op);
		ra = PPC_RA(op);
		uint32_t sub = (op >> 1) & 0xF; /* bits 27-30 determine sub-instruction */
		/* For immediate variants (sub 0-7) the sub-opcode occupies bits 27-29 (3 bits)
		 * while bit 30 is SH[5].  Without normalisation, rldicl/rldicr/rldic/rldimi
		 * with sh>=32 (SH[5]=1) would have sub=1/3/5/7 instead of 0/2/4/6, causing
		 * the wrong handler to execute.  Strip the SH[5] bit by halving sub. */
		if (sub < 8) sub >>= 1;
		bool rc = op & 1;
		/* Load 64-bit source */
		emit_load_gpr64(RTMP0, rs);
		switch (sub) {
		case 0: /* rldicl — rotate left doubleword then clear left */
		case 1: /* rldicr — rotate left doubleword then clear right */
		case 2: /* rldic  — rotate left doubleword then clear (both sides) */
		case 3: /* rldimi — rotate left doubleword then mask insert */
		{
			uint32_t sh = ((op >> 11) & 0x1F) | ((op & 2) << 4); /* 6-bit shift: sh[0:4] | sh[5] */
			uint32_t mb_or_me = ((op >> 6) & 0x1F) | (op & 0x20); /* 6-bit mask field */
			/* ROL Xd,Xn,#sh = ROR Xd,Xn,#(64-sh) */
			if (sh > 0 && sh < 64)
				emit32(0x93C00000 | (RTMP0 << 16) | (((64 - sh) & 63) << 10) | (RTMP0 << 5) | RTMP0);
			/* Apply mask:
			 * rldicl (sub 0): clear left mb bits  — LSL mb; LSR mb
			 * rldicr (sub 1): clear right (63-me) bits — LSR (63-me); LSL (63-me)
			 * rldic  (sub 2): clear left mb AND clear right sh bits
			 * rldimi (sub 3): insert into RA with mask bits mb..(63-sh) */
			if (sub == 3) {
				/* rldimi: RA = (ROTL64(RS,sh) & mask) | (RA & ~mask)
				 * mask = ARM bits sh..(63-mb): computed at JIT compile time.
				 * Uses MOVZ/MOVK to load 64-bit mask, then AND + BIC + ORR. */
				int start_bit = (int)sh;
				int stop_bit  = 63 - (int)mb_or_me;
				uint64_t mask;
				if (start_bit <= stop_bit) {
					int nbits = stop_bit - start_bit + 1;
					mask = (nbits >= 64) ? ~UINT64_C(0)
					      : ((UINT64_C(1) << nbits) - 1) << start_bit;
				} else {
					/* Wrapping: bits 0..stop_bit and start_bit..63 */
					mask = ((UINT64_C(1) << (stop_bit + 1)) - 1) |
					       ~((UINT64_C(1) << start_bit) - 1);
				}
				emit_load_imm64(RTMP1, mask);
				/* RTMP0 = rotated & mask */
				emit32(0x8A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND Xd,Xn,Xm */
				/* Load full 64-bit RA without clobbering RTMP0: RTMP1 can be reused,
				 * and the mask is reloaded before BIC. */
				emit_load_gpr64_tmp(RTMP2, ra, RTMP1);
				emit_load_imm64(RTMP1, mask);
				/* RTMP2 = RA & ~mask */
				emit32(0x8A200000 | (RTMP1 << 16) | (RTMP2 << 5) | RTMP2); /* BIC Xd,Xn,Xm */
				/* RTMP0 = merged result */
				emit32(0xAA000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* ORR Xd,Xn,Xm */
			}
			/* rldicl (sub 0): clear top mb_or_me bits */
			if (sub == 0) {
				if (mb_or_me > 0 && mb_or_me < 64) {
					emit_lsl64_imm(RTMP0, RTMP0, mb_or_me);
					emit_lsr64_imm(RTMP0, RTMP0, mb_or_me);
				}
			/* rldicr (sub 1): clear bottom (63-me) bits */
			} else if (sub == 1) {
				uint32_t clrr = 63 - mb_or_me;
				if (clrr > 0 && clrr < 64) {
					emit_lsr64_imm(RTMP0, RTMP0, clrr);
					emit_lsl64_imm(RTMP0, RTMP0, clrr);
				}
			/* rldic (sub 2): clear top mb bits AND clear bottom sh bits */
			} else if (sub == 2) {
				if (mb_or_me > 0 && mb_or_me < 64) {
					emit_lsl64_imm(RTMP0, RTMP0, mb_or_me);
					emit_lsr64_imm(RTMP0, RTMP0, mb_or_me);
				}
				if (sh > 0 && sh < 64) {
					emit_lsr64_imm(RTMP0, RTMP0, sh);
					emit_lsl64_imm(RTMP0, RTMP0, sh);
				}
			}
			/* sub 3 (rldimi): mask-insert already done above, RTMP0 holds final result */
			emit_store_gpr64(RTMP0, ra);
			if (rc) lazy_update_cr0(RTMP0); /* uses low 32 bits for CR0 */
			return true;
		}
		case 8: /* rldcl — rotate left doubleword then clear left (register shift) */
		case 9: /* rldcr — rotate left doubleword then clear right (register shift) */
		{
			uint32_t rb = (op >> 11) & 0x1F; /* RB = shift register */
			uint32_t mb = ((op >> 6) & 0x1F) | (op & 0x20); /* 6-bit mask field */
			emit_load_gpr(RTMP1, rb);
			/* ROL Xd,Xn,Xm: ARM64 has RORV; ROL by sh = ROR by (64-sh)
			 * NEG RTMP2, RTMP1  (gives -sh; RORV uses low 6 bits so -sh ≡ 64-sh mod 64) */
			emit32(0xCB0003E0 | (RTMP1 << 16) | RTMP2); /* SUB Xd,XZR,Xn = NEG */
			emit32(0x9AC02C00 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* RORV Xd,Xn,Xm */
			/* Apply mask: clear left mb bits (rldcl) or clear right (63-me) bits (rldcr) */
			if (sub == 8) { /* rldcl: clear top mb bits */
				if (mb > 0 && mb < 64) {
					emit_lsl64_imm(RTMP0, RTMP0, mb);
					emit_lsr64_imm(RTMP0, RTMP0, mb);
				}
			} else { /* rldcr: clear bottom (63-me) bits */
				uint32_t clrr = 63 - mb;
				if (clrr > 0 && clrr < 64) {
					emit_lsr64_imm(RTMP0, RTMP0, clrr);
					emit_lsl64_imm(RTMP0, RTMP0, clrr);
				}
			}
			emit_store_gpr64(RTMP0, ra);
			if (rc) lazy_update_cr0(RTMP0);
			return true;
		}
		default:
			return false;
		}
	}

	case 58: /* ld/ldu/lwa — 64-bit load doubleword */
	{
		rd = PPC_RD(op);
		ra = PPC_RA(op);
		int32_t ds = (int16_t)(op & 0xFFFC); /* sign-extended 14-bit offset * 4 */
		uint32_t sub = op & 3;
		emit_load_ea_base(ra);
		if (ds) { emit_load_imm32(RTMP1, ds); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		if (sub == 2) {
			/* lwa — load word algebraic (sign-extend 32→64) */
			a64_ldr_w_reg(RTMP1, RMEMBASE, RTMP0); /* LDR Wt, [Xn] */
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV Wt, Wt (byte-swap) */
			emit_store_gpr(RTMP1, rd);
			/* Sign extend to hi: ASR Wt, Wt, #31 */
			emit32(0x131F7C00 | (RTMP1 << 5) | RTMP2); /* ASR Wd, Wn, #31 */
			a64_str_w_imm(RTMP2, RSTATE, PPCR_GPR_HI(rd));
		} else {
			/* ld — load doubleword */
			a64_ldr_x_reg(RTMP1, RMEMBASE, RTMP0); /* LDR Xt, [Xn] */
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1); /* REV Xt, Xt (byte-swap 64-bit) */
			emit_store_gpr64(RTMP1, rd);
			if (sub == 1 && ra != 0) { /* ldu: update rA */
				emit_load_ea_base(ra);
				if (ds) { emit_load_imm32(RTMP1, ds); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
				emit_store_gpr(RTMP0, ra);
			}
		}
		return true;
	}

	case 62: /* std/stdu — 64-bit store doubleword */
	{
		uint32_t rs = PPC_RS(op);
		ra = PPC_RA(op);
		int32_t ds = (int16_t)(op & 0xFFFC);
		uint32_t sub = op & 3;
		emit_load_ea_base(ra);
		if (ds) { emit_load_imm32(RTMP1, ds); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		/* Preserve EA: emit_load_gpr64(RTMP1, rs) uses RTMP0 as scratch for the high word. */
		emit32(0xAA000000 | (RTMP0 << 16) | (31 << 5) | RTMP2); /* MOV RTMP2, RTMP0 */
		emit_load_gpr64(RTMP1, rs);
		emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1); /* REV Xt, Xt (byte-swap) */
		a64_str_x_reg(RTMP1, RMEMBASE, RTMP2); /* STR Xt, [Xn] */
		if (sub == 1 && ra != 0) { /* stdu: update rA */
			emit_load_ea_base(ra);
			if (ds) { emit_load_imm32(RTMP1, ds); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			emit_store_gpr(RTMP0, ra);
		}
		return true;
	}

	case 56: /* lq — load quadword (128-bit, pair of 64-bit) */
	{
		rd = PPC_RD(op) & ~1; /* must be even register */
		ra = PPC_RA(op);
		int32_t dq = (int16_t)(op & 0xFFF0); /* sign-extended 12-bit offset * 16 */
		emit_load_ea_base(ra);
		if (dq) { emit_load_imm32(RTMP1, dq); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		/* Load first doubleword → GPR[rd] */
		a64_ldr_x_reg(RTMP1, RMEMBASE, RTMP0); /* LDR Xt, [Xn] */
		emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1); /* REV64 */
		/* Save EA to RTMP2 before emit_store_gpr64 clobbers RTMP0 via LSR */
		emit32(0xAA000000 | (RTMP0 << 16) | (31 << 5) | RTMP2); /* MOV RTMP2, RTMP0 */
		emit_store_gpr64(RTMP1, rd);
		/* Load second doubleword → GPR[rd+1]: use saved EA in RTMP2 */
		emit32(0x91002000 | (RTMP2 << 5) | RTMP2); /* ADD RTMP2, RTMP2, #8 */
		a64_ldr_x_reg(RTMP1, RMEMBASE, RTMP2);
		emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
		emit_store_gpr64(RTMP1, rd + 1);
		return true;
	}

	case 59: /* single-precision FP ops */
	{
		uint32_t xo5 = (op >> 1) & 0x1F;
		uint32_t frd = PPC_RD(op);
		uint32_t fra = PPC_RA(op);
		uint32_t frb = (op >> 11) & 0x1F;
		uint32_t frc = (op >> 6) & 0x1F;
		(void)fra; (void)frc; (void)frb; (void)frd;
		/* Single-precision: compute in double, round to single, store as double */
		switch (xo5) {
		case 21: /* fadds */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E602800 | (1 << 16) | (0 << 5) | 0); /* FADD (double) */
			/* Round to single: FCVT Sd, Dd then FCVT Dd, Sd */
			emit32(0x1E624000 | (0 << 5) | 0); /* FCVT Sd, Dd */
			emit32(0x1E22C000 | (0 << 5) | 0); /* FCVT Dd, Sd */
			emit_store_fpr(0, frd);
			return true;
		case 20: /* fsubs */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E603800 | (1 << 16) | (0 << 5) | 0);
			emit32(0x1E624000 | (0 << 5) | 0);
			emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, frd);
			return true;
		case 25: /* fmuls */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frc);
			emit32(0x1E600800 | (1 << 16) | (0 << 5) | 0);
			emit32(0x1E624000 | (0 << 5) | 0);
			emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, frd);
			return true;
		case 18: /* fdivs */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E601800 | (1 << 16) | (0 << 5) | 0);
			emit32(0x1E624000 | (0 << 5) | 0);
			emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, frd);
			return true;
		case 29: /* fmadds */
			emit_load_fpr(0, fra); emit_load_fpr(1, frc); emit_load_fpr(2, frb);
			emit32(0x1F400000 | (1 << 16) | (2 << 10) | (0 << 5) | 0);
			emit32(0x1E624000 | (0 << 5) | 0); emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, frd); return true;
		case 28: /* fmsubs = frA*frC - frB (single-precision) */
			emit_load_fpr(0, fra); emit_load_fpr(1, frc); emit_load_fpr(2, frb);
			emit32(0x1F608000 | (1 << 16) | (2 << 10) | (0 << 5) | 0); /* FNMSUB */
			emit32(0x1E624000 | (0 << 5) | 0); emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, frd); return true;
		case 31: /* fnmadds */
			emit_load_fpr(0, fra); emit_load_fpr(1, frc); emit_load_fpr(2, frb);
			emit32(0x1F600000 | (1 << 16) | (2 << 10) | (0 << 5) | 0);
			emit32(0x1E624000 | (0 << 5) | 0); emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, frd); return true;
		case 30: /* fnmsubs = -(frA*frC - frB) = frB - frA*frC (single-precision) */
			emit_load_fpr(0, fra); emit_load_fpr(1, frc); emit_load_fpr(2, frb);
			emit32(0x1F408000 | (1 << 16) | (2 << 10) | (0 << 5) | 0); /* FMSUB */
			emit32(0x1E624000 | (0 << 5) | 0); emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, frd); return true;
		case 24: /* fres frD,frB — reciprocal estimate */
			emit_load_fpr(0, frb);
			emit32(0x1E624000 | (0 << 5) | 0); /* FCVT Sd,Dd */
			emit32(0x1E20F800 | (0 << 5) | 0); /* FRECPE Sd,Sn */
			emit32(0x1E22C000 | (0 << 5) | 0); /* FCVT Dd,Sd */
			emit_store_fpr(0, frd); return true;

		case 22: /* fsqrts frD,frB — floating-point square root (single) */
			emit_load_fpr(0, frb);
			emit32(0x1E61C000 | (0 << 5) | 0); /* FSQRT Dd, Dn (double precision) */
			emit32(0x1E624000 | (0 << 5) | 0); /* FCVT Sd, Dd (round to single) */
			emit32(0x1E22C000 | (0 << 5) | 0); /* FCVT Dd, Sd (widen back) */
			emit_store_fpr(0, frd); return true;
		default:
			return false; /* unknown opcode: stop compilation */
		}
	}

	default:
		jit_miss_count[opc]++;
		return false;
	}
}

/* ---- Public API ---- */

bool ppc_jit_aarch64_init(size_t cache_size_kb)
{
	jit_cache_size = cache_size_kb * 1024;
	jit_cache_base = (uint8_t *)jit_cache_alloc(jit_cache_size);
	if (!jit_cache_base) {
		fprintf(stderr, "PPC-JIT-A64: failed to allocate %zu KB code cache; "
		        "falling back to interpreter (emulation continues, slower)\n", cache_size_kb);
		/* Establish the bucket-head sentinels even though the code cache is
		 * unavailable: jit_bc_lookup()/compile() are still reached from the
		 * execute loop and MUST NOT walk a zero-initialised (cyclic) bucket. */
		jit_bc_flush();
		return false;
	}
	jit_cache_wp = (uint32_t *)jit_cache_base;
	jit_cache_end = (uint32_t *)(jit_cache_base + jit_cache_size);
	jit_bc_flush();
#if defined(__APPLE__) && defined(__aarch64__)
	fprintf(stderr, "PPC-JIT-A64: code cache allocated (MAP_JIT) %zu KB at %p\n",
	        cache_size_kb, jit_cache_base);
#endif
	fprintf(stderr, "PPC-JIT-A64: code cache %zu KB at %p, block cache %d buckets / %d pool\n",
	        cache_size_kb, jit_cache_base, JIT_BC_BUCKETS, JIT_BC_POOL);
	return true;
}

void ppc_jit_aarch64_exit(void)
{
	jit_report_misses();
	if (jit_cache_base) {
		jit_cache_free(jit_cache_base, jit_cache_size);
		jit_cache_base = NULL;
	}
}

void ppc_jit_aarch64_flush(void)
{
	/* Reset code cache write pointer and invalidate block address cache.
	 * Called when the JIT must start completely fresh (cache full, explicit reset).
	 * Contract: see SheepShaver/docs/AARCH64_JIT_RUNTIME_CONTRACT.md — flush discipline. */
	JIT_LOG("JIT cache flush, blocks compiled so far: %d", jit_bc_pool_next);
	jit_cache_wp = (uint32_t *)jit_cache_base;
	jit_bc_flush();
}

void ppc_jit_aarch64_invalidate_pc(uint32_t pc)
{
	jit_bc_invalidate_pc(pc);
}

bool ppc_jit_aarch64_has_block(uint32_t pc)
{
	/* One hash-bucket walk; called from the interpreter inner loop after every
	 * interpreted block, so it must stay allocation-free and cheap. */
	const struct jit_bc_entry *e = jit_bc_lookup(pc);
	return e != NULL && e->complete;
}

ppc_jit_entry_fn ppc_jit_aarch64_lookup_fast(uint32_t pc)
{
	/* Dispatch fast path: hash lookup only, no compile, tiny stack frame.
	 * The dispatcher calls this before the full ppc_jit_aarch64_compile() —
	 * for already-compiled blocks (the overwhelmingly common case) this avoids
	 * compile()'s function-call and stack-frame overhead per block execution. */
	const struct jit_bc_entry *e = jit_bc_lookup(pc);
	return (e != NULL && e->complete) ? (ppc_jit_entry_fn)(void *)e->code : (ppc_jit_entry_fn)0;
}

int ppc_jit_aarch64_lookup_n_insns(uint32_t pc)
{
	const struct jit_bc_entry *e = jit_bc_lookup(pc);
	return (e != NULL && e->complete) ? e->n_insns : 0;
}

/* Guest RAM range, cached from the compile() parameters so that
 * ppc_jit_aarch64_is_compilable() can answer without them. */
static uint32_t jit_ram_base_cached = 0;
static uint32_t jit_ram_size_cached = 0;

bool ppc_jit_aarch64_is_compilable(uint32_t pc)
{
	/* Range check only (2-4 compares).  Called from the interpreter inner loop
	 * after every interpreted block: returns true if this PC belongs to the
	 * JIT's domain (guest RAM or the registered ROM range), regardless of
	 * whether a block has been compiled yet.  The dispatcher will compile it.
	 *
	 * This intentionally does NOT require an existing compiled block: code
	 * first reached from inside an interpreter session (e.g. toolbox routines
	 * called by the interpreter-only 68k emulator) would otherwise never meet
	 * the compiler at all. */
	if (!jit_cache_base)
		return false; /* JIT disabled/unavailable — keep everything interpreted */
	if (jit_ram_size_cached != 0 &&
	    pc >= jit_ram_base_cached && pc < jit_ram_base_cached + jit_ram_size_cached)
		return true;
	if (jit_rom_size != 0 && pc >= jit_rom_base && pc < jit_rom_base + jit_rom_size)
		return true;
	return false;
}

void ppc_jit_aarch64_invalidate_range(uint32_t start, uint32_t end)
{
	/* Range-based JIT block invalidation for icbi/isync handling.
	 *
	 * Step 1: Revert any live chain-patches (B instructions) whose target PC
	 * falls in [start, end).  If we only nullify the pool entry without reverting
	 * the B, calling blocks would still jump directly to the now-invalid ARM64
	 * code, bypassing the JIT gate and running stale translations.  Reverting
	 * restores the original LDP+RET epilogue so the JIT gate is re-entered on
	 * the next visit and the block is recompiled from fresh guest code.
	 *
	 * Step 2: Nullify pool entries for PCs in range.  jit_bc_lookup skips
	 * entries with code==NULL, causing a recompile on the next lookup. */
	jit_bc_ensure_init();
	/* Step 1 — revert live chain-patches targeting the invalidated range.
	 * jit_cache_begin_write makes the JIT region writable (Apple W^X).
	 * We collect the first/last patched addresses for the icache flush range. */
	uint32_t *flush_lo = NULL, *flush_hi = NULL;
	jit_cache_begin_write();
	for (int i = 0; i < chain_site_pool_next; i++) {
		struct jit_chain_site *s = &chain_site_pool[i];
		if (s->patched && s->patch_loc &&
		    s->target_pc >= start && s->target_pc < end) {
			*s->patch_loc = JIT_EPILOGUE_FIRST_LDP; /* restore LDP x27,x28,[sp],#16 */
			if (!flush_lo || s->patch_loc < flush_lo) flush_lo = s->patch_loc;
			if (!flush_hi || s->patch_loc > flush_hi) flush_hi = s->patch_loc;
			s->patched = false;
		}
	}
	/* jit_cache_end_write re-protects (W→X) and flushes the icache range.
	 * Must always be called to pair with jit_cache_begin_write above. */
	{
		void *fw_addr = flush_lo ? (void *)flush_lo : (void *)jit_cache_base;
		size_t fw_len = flush_lo ? (size_t)((uint8_t *)(flush_hi + 1) - (uint8_t *)flush_lo) : 0;
		jit_cache_end_write(fw_addr, fw_len);
	}
	/* Step 2 — nullify block pool entries for invalidated PCs */
	for (int i = 0; i < jit_bc_pool_next; i++) {
		if (jit_bc_pool[i].code &&
		    jit_bc_pool[i].pc >= start && jit_bc_pool[i].pc < end)
			jit_bc_pool[i].code = NULL;
	}
}

void ppc_jit_aarch64_set_rom_range(uint32_t guest_base, uint32_t size, const uint8_t *host_base)
{
	/* Register the Mac ROM as a second JIT-compilable range.  The ROM is
	 * write-protected (READ|EXECUTE) after rom_patches are applied, so blocks
	 * compiled from it are permanently valid — no SMC invalidation needed. */
	jit_rom_base = guest_base;
	jit_rom_size = size;
	jit_rom_host = host_base;
}

/* Periodic cumulative-blocker report.  Kept OUT of ppc_jit_aarch64_compile():
 * its 4 KB of local sort arrays would otherwise live in compile()'s stack frame,
 * forcing __chkstk probing on every call — and compile() is called per block
 * dispatch, making that measurable (profiled during boot). */
uint32_t ppc_jit_aarch64_blocks_compiled(void) { return jit_blocks_attempted; }

__attribute__((noinline))
static void jit_report_cum_blockers(void) {
	fprintf(stderr, "PPC-JIT-A64-CUM: %u fail opcodes in %u blocks (%u attempted), top blockers:\n",
	        jit_cum_fail_total, jit_blocks_attempted - jit_blocks_complete, jit_blocks_attempted);
	/* Copy arrays for sorted output without destroying data */
	uint32_t tmp_opc[64]; memcpy(tmp_opc, jit_cum_fail_opc, sizeof(tmp_opc));
	for (int pass = 0; pass < 15; pass++) {
		uint32_t max_v = 0; int max_i = -1;
		for (int i = 0; i < 64; i++) if (tmp_opc[i] > max_v) { max_v = tmp_opc[i]; max_i = i; }
		if (max_i < 0 || max_v == 0) break;
		fprintf(stderr, "  opc=%d: %u blocks\n", max_i, max_v);
		tmp_opc[max_i] = 0;
	}
	uint32_t tmp_xo[1024]; memcpy(tmp_xo, jit_cum_fail_xo31, sizeof(tmp_xo));
	fprintf(stderr, "PPC-JIT-A64-CUM: top XO31 blockers:\n");
	for (int pass = 0; pass < 10; pass++) {
		uint32_t max_v = 0; int max_i = -1;
		for (int i = 0; i < 1024; i++) if (tmp_xo[i] > max_v) { max_v = tmp_xo[i]; max_i = i; }
		if (max_i < 0 || max_v == 0) break;
		fprintf(stderr, "  XO=%d: %u blocks\n", max_i, max_v);
		tmp_xo[max_i] = 0;
	}
}

/* SS_JIT_DEBUG_PC=<hex>: trace every compile decision for one PC (diagnostic) */
static uint32_t jit_debug_pc(void) {
	static uint32_t v = 1; /* 1 = uninitialized (PC values are word-aligned, never 1) */
	if (v == 1) {
		const char *s = getenv("SS_JIT_DEBUG_PC");
		v = s ? (uint32_t)strtoul(s, NULL, 16) : 0;
	}
	return v;
}

bool ppc_jit_aarch64_compile(
	uint32_t pc,
	const uint8_t *ram,
	size_t ramsize,
	ppc_jit_block *out)
{
	const bool dbg = (jit_debug_pc() != 0 && pc == jit_debug_pc());
	/* Block address cache lookup — return cached block without recompiling.
	 * Contract: see AARCH64_JIT_RUNTIME_CONTRACT.md — block lifecycle. */
	const struct jit_bc_entry *cached = jit_bc_lookup(pc);
	if (cached) {
		if (dbg) fprintf(stderr, "JIT-DBG %08x: cache HIT complete=%d code=%p\n",
		                 pc, cached->complete, (void *)cached->code);
		out->code       = cached->code;
		out->chain_code = cached->chain_code;
		out->code_size    = 0; /* not tracked for cached entries */
		out->ppc_start_pc = pc;
		out->ppc_end_pc   = pc; /* not tracked for cached entries */
		out->n_insns      = 0; /* not tracked for cached entries */
		out->complete     = cached->complete;
		return true;
	}

	/* Code cache could not be allocated at init — JIT is permanently disabled,
	 * the interpreter handles everything. Bail out quietly; logging here would
	 * fire on every block and flood the terminal (the one-time fallback notice
	 * was already printed in ppc_jit_aarch64_init). */
	if (!jit_cache_base)
		return false;

	/* SS_JIT_INTERP_RANGE: force interpreter for a PC range (binary search aid).
	 * Usage: SS_JIT_INTERP_RANGE=10650000-10700000
	 * Any block with start PC in [lo, hi) is NOT JIT-compiled. */
	{
		static uint32_t interp_lo = 0, interp_hi = 0;
		static int checked = 0;
		if (!checked) {
			checked = 1;
			const char *env = getenv("SS_JIT_INTERP_RANGE");
			if (env) {
				sscanf(env, "%x-%x", &interp_lo, &interp_hi);
				fprintf(stderr, "[JIT] SS_JIT_INTERP_RANGE: [%08x..%08x) forced to interpreter\n",
				        interp_lo, interp_hi);
			}
		}
		if (interp_lo < interp_hi && pc >= interp_lo && pc < interp_hi)
			return false;
	}

	/* Cache the RAM range for ppc_jit_aarch64_is_compilable() (the interpreter
	 * handoff check, which has no access to the compile parameters). */
	jit_ram_base_cached = (uint32_t)(uintptr_t)ram;
	jit_ram_size_cached = (uint32_t)ramsize;

	/* Log first compile of each 64KB region (rate-limited — one line per 64K chunk).
	 * 65536 entries × 1 byte = 64KB static array covers all 32-bit guest space. */
	{
		static uint8_t seen_64k[65536] = {0};
		uint32_t chunk = pc >> 16;
		if (!seen_64k[chunk]) {
			seen_64k[chunk] = 1;
			JIT_LOG("first compile in 64KB region %08x (pc=%08x)", chunk << 16, pc);
		}
	}

	/* Fast out-of-range check: SheepMem, kernel data, and other non-compilable
	 * PCs bail BEFORE the W^X toggle + prologue — on macOS the
	 * pthread_jit_write_protect_np() pair is ~microseconds per call; paying it
	 * for every non-compilable block visit makes JIT mode dramatically slower
	 * than interpreter-only mode.  Compilable ranges: guest RAM and (when
	 * registered) the immutable Mac ROM. */
	if (jit_fetch_ptr(pc, ram, ramsize) == NULL) {
		if (dbg) fprintf(stderr, "JIT-DBG %08x: fetch_ptr NULL (out of range; rom_base=%08x rom_size=%08x)\n",
		                 pc, jit_rom_base, jit_rom_size);
		out->complete  = false;
		out->code      = NULL;
		out->chain_code = NULL;
		out->n_insns   = 0;
		return false;
	}

	/* Cache-full margin must exceed the worst-case single-block emission:
	 * 512 PPC insns × ~50 ARM64 insns each (pathological mfspr/mtspr packing)
	 * ≈ 100 KB.  256 KB gives comfortable headroom and is negligible against
	 * the full cache size. */
	if (!jit_cache_wp || jit_cache_wp >= jit_cache_end - (256 * 1024 / 4)) {
		/* Code cache full — flush everything and start over.
		 * This invalidates all cached blocks, which is safe because
		 * the code they point to is about to be overwritten. */
		fprintf(stderr, "PPC-JIT-A64: code cache full, flushing\n");
		ppc_jit_aarch64_flush();
		if (!jit_cache_wp) return false;
	}

	uint32_t *code_start = jit_cache_wp;
	jit_code_ptr = jit_cache_wp;

	/* W^X: make the JIT code cache writable on this (compile) thread for the
	 * duration of block emission. jit_cache_end_write() below flips it back to
	 * executable before this thread can call into the freshly compiled code.
	 * No-op on Linux. */
	jit_cache_begin_write();

	/* Prologue: save callee-saved regs, set x20 = regs ptr from x0 */
	a64_stp_pre(A64_FP, A64_LR, A64_SP, -16);
	a64_stp_pre(19, RSTATE, A64_SP, -16);  /* save x19, x20 */
	a64_stp_pre(21, 22, A64_SP, -16);      /* save x21, x22 */
	a64_stp_pre(23, 24, A64_SP, -16);      /* save x23, x24 */
	a64_stp_pre(25, 26, A64_SP, -16);      /* save x25, x26 */
	a64_stp_pre(27, 28, A64_SP, -16);      /* save x27, x28 */
	a64_mov_reg(RSTATE, A64_X0);
	/* Load the guest-memory base (VMBaseDiff) into RMEMBASE (x19). All guest
	 * memory accesses use register-offset addressing [RMEMBASE, EA]. The value
	 * is a fixed per-build constant (NATMEM_OFFSET on DIRECT, 0 on REAL), so a
	 * chained block re-using this frame inherits the same correct base. */
	emit_load_mem_base();

	/* Chain entry: code position after prologue.
	 * With JIT_BLOCK_CHAINING=0 this is recorded but never branched to —
	 * blocks are only entered through the ABI entry (code_start) by the
	 * dispatcher, which polls spcflags between blocks. */
	uint32_t *chain_entry_start = jit_code_ptr;

	/* Block-entry spcflags poll (dyngen gen_start equivalent): if any
	 * actionable interrupt/event flag is pending, return to the C dispatcher
	 * (which services it) instead of executing the block body.  The chain
	 * entry deliberately INCLUDES this poll so that, once block chaining is
	 * enabled, a chained cycle still returns to the dispatcher whenever a
	 * flag becomes set — closing the "chained loop never services interrupts"
	 * hole.  With chaining off it runs on every ABI entry and is behaviourally
	 * transparent (the dispatcher already polled before entering).
	 *
	 * EXCEPTION: DR emulator blocks (ROM+0x460000..+0x500000).  These form
	 * the 68k instruction dispatch loop; their bclr 5,8 interrupt gate
	 * expects CR2.LT to be updated only between dispatch cycles.  A block-
	 * entry poll would trigger check_spcflags → HandleInterrupt → CR2.LT=1
	 * BEFORE the handler for the current 68k instruction runs, causing the
	 * bclr to skip the handler and enter the interrupt path prematurely.
	 * The C dispatcher's between-block spcflags check is sufficient here. */
	bool in_dr_emulator = (jit_rom_size > 0x460000 &&
	                       pc >= jit_rom_base + 0x460000 &&
	                       pc <  jit_rom_base + 0x500000);
	if (!in_dr_emulator)
		emit_entry_spcflags_poll(pc);

	jit_blocks_attempted++;
	uint32_t cur_pc = pc;
	int n_compiled = 0;
	bool complete = true;
	insn_count = 0;
	lazy_cr0_valid = false;
	lazy_cr0_reg = -1;
	ra_reset();

	/* SS_JIT_MAX_INSNS=<n>: cap the number of PPC instructions compiled per block.
	 * Diagnostic knob for isolating cross-instruction state leaks within a block
	 * (e.g., lazy CR0 / RA / temp register clobbers).  n<=0 means unlimited. */
	static int s_max_insns_checked = 0;
	static int s_max_insns = 0;
	if (!s_max_insns_checked) {
		s_max_insns_checked = 1;
		const char *e = getenv("SS_JIT_MAX_INSNS");
		s_max_insns = (e && *e) ? atoi(e) : 0;
		if (s_max_insns > 0)
			fprintf(stderr, "[JIT] SS_JIT_MAX_INSNS=%d (diagnostic block-length cap)\n", s_max_insns);
	}

	for (int i = 0; i < 512; i++) {
		const uint8_t *p = jit_fetch_ptr(cur_pc, ram, ramsize);
		if (!p)
			break;

		uint32_t op = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		              ((uint32_t)p[2] << 8) | p[3];

		if (op == 0x4E800020) { /* blr — block terminator */
			lazy_flush_cr0();
			ra_flush_all();
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_LR);
			a64_str_w_imm(RTMP0, RSTATE, PPCR_PC);
			a64_ldp_post(27, 28, A64_SP, 16);
				a64_ldp_post(25, 26, A64_SP, 16);
				a64_ldp_post(23, 24, A64_SP, 16);
				a64_ldp_post(21, 22, A64_SP, 16);
				a64_ldp_post(19, RSTATE, A64_SP, 16);
				a64_ldp_post(A64_FP, A64_LR, A64_SP, 16);
				a64_ret();
			n_compiled++;
			cur_pc += 4;
			break;
		}

		/* Check if this is a block-terminating opcode */
		uint32_t term_opc = op >> 26;
		bool is_terminator = (term_opc == 18); /* b/bl */
		if (term_opc == 19) {
			uint32_t term_xo = (op >> 1) & 0x3FF;
			if (term_xo == 16 || term_xo == 528) is_terminator = true; /* bclr/bcctr */
		}

		if (op == 0x00000000) { /* illegal — end of test code / zero-filled memory */
			if (dbg) fprintf(stderr, "JIT-DBG %08x: zero opcode at +%#x (n_compiled=%d)\n",
			                 pc, (unsigned)(cur_pc - pc), n_compiled);
			if (n_compiled == 0) {
				/* Bail without emitting a usable block. Restore the cache to the
				 * executable state we entered with (W^X; no-op on Linux). The
				 * prologue bytes written so far are discarded — jit_cache_wp is
				 * not advanced, so they will be overwritten by the next compile. */
				jit_cache_end_write(code_start, (uint8_t *)jit_code_ptr - (uint8_t *)code_start);
				return false; /* don't compile empty blocks */
			}
			lazy_flush_cr0();
			emit_epilogue_with_pc(cur_pc);
			n_compiled++;
			cur_pc += 4;
			break;
		}

		/* Record instruction offset for intra-block branches */
		if (insn_count < 512) {
			insn_code_offset[insn_count] = jit_code_ptr;
			insn_ppc_pc[insn_count] = cur_pc;
			insn_count++;
		}

		if (!compile_one(op, cur_pc)) {
			if (dbg) fprintf(stderr, "JIT-DBG %08x: compile_one FALLBACK->inline-interp at +%#x op=%08x (opc=%u xo=%u)\n",
			                 pc, (unsigned)(cur_pc - pc), op, op >> 26, (op >> 1) & 0x3FF);
			jit_total_miss++;
			jit_miss_count[op >> 26]++;
			jit_cum_fail_opc[op >> 26]++;
			if ((op >> 26) == 31) jit_cum_fail_xo31[(op >> 1) & 0x3FF]++;
			jit_cum_fail_total++;
			/* Dyngen do_generic equivalent: instead of ending the block as
			 * incomplete (which forces mixed JIT/interpreter execution of the
			 * same PC and corrupts the ROM's 68k emulator), emit an inline call
			 * to the interpreter handler for this one instruction. The block
			 * stays COMPLETE — every instruction is accounted for, either by
			 * native codegen or by an inline handler call.
			 *
			 * Flush lazy CR0 + register allocator first: the BLR clobbers NZCV
			 * (where lazy CR0 lives) and x0-x17, and all guest state must be in
			 * the regs struct before the handler reads it. The handler may have
			 * been a branch, so we end the block here (bare epilogue, no PC
			 * store, no chaining) and break. This differs from dyngen, which
			 * continues compiling after an inline call (multiple inline calls per
			 * block); breaking after one costs one extra dispatch round-trip per
			 * fallback op but keeps the change minimal and correct. */
			lazy_flush_cr0();
			ra_flush_all();
			emit_inline_interp_call(op, cur_pc);
			n_compiled++;
			cur_pc += 4;
			/* complete stays true */
			break;
		}

		jit_total_hit++;
		n_compiled++;
		cur_pc += 4;

		/* Optional block-length cap (diagnostic): force an early boundary even if
		 * the guest code would naturally continue. */
		if (s_max_insns > 0 && n_compiled >= s_max_insns) {
			lazy_flush_cr0();
			emit_epilogue_with_pc(cur_pc);
			break;
		}

		/* Block-terminating opcodes: break after compiling them */
		if (is_terminator) break;
	}

	/* Track why blocks are incomplete */
	if (!complete && 0) {
	}

	/* Periodic report */
	if ((jit_blocks_attempted) % 10000000 == 0 && jit_blocks_attempted > 0)
		jit_report_misses();

	/* If the block didn't end with a control-flow exit, emit a fallback epilogue.
	 * Valid endings are RET (standard epilogue) or an unconditional B (compile-time
	 * chain to another block's code — emitted by emit_epilogue_with_pc when the
	 * branch target is already cached).  Treating chained endings as "no exit"
	 * was a critical bug: it marked every chained block incomplete, so GATE2
	 * permanently excluded exactly the hottest blocks (hot targets are compiled
	 * first, so hot blocks are the most likely to chain). */
	if (n_compiled > 0 && jit_code_ptr > code_start) {
		uint32_t last = *(jit_code_ptr - 1);
		bool ends_with_ret    = (last == 0xD65F03C0);
		bool ends_with_branch = ((last & 0xFC000000) == 0x14000000); /* B <imm26> */
		if (!ends_with_ret && !ends_with_branch) {
			/* Ran off the end (e.g. 512-insn limit) without a terminator:
			 * emit an epilogue resuming at cur_pc and mark partial. */
			lazy_flush_cr0();
			emit_epilogue_with_pc(cur_pc);
			complete = false;
		}
	}

	size_t code_bytes = (uint8_t *)jit_code_ptr - (uint8_t *)code_start;
	jit_cache_flush(code_start, code_bytes);
	/* W^X: flip the region back to executable and invalidate the icache for the
	 * bytes just written (on Apple). Must happen before any call into code_start. */
	jit_cache_end_write(code_start, code_bytes);
	jit_cache_wp = jit_code_ptr;

	out->code = code_start;
	out->code_size = code_bytes;
	out->ppc_start_pc = pc;
	out->ppc_end_pc = cur_pc;
	out->n_insns = n_compiled;

	/* Cumulative miss report — doesn't clear counters */
	{
		static uint32_t cum_opc[64] = {0};
		static uint32_t cum_xo31[1024] = {0};
		static uint32_t cum_total = 0;
		static uint32_t cum_report_at = 10000000;
		
		if (!complete) {
			/* Record the opcode that caused the failure */
			const uint8_t *fail_p = jit_fetch_ptr(cur_pc, ram, ramsize);
			if (fail_p) {
				uint32_t fail_op = ((uint32_t)fail_p[0] << 24) | ((uint32_t)fail_p[1] << 16) |
				                   ((uint32_t)fail_p[2] << 8) | fail_p[3];
				uint32_t fail_opc = fail_op >> 26;
				cum_opc[fail_opc]++;
				if (fail_opc == 31) cum_xo31[(fail_op >> 1) & 0x3FF]++;
				cum_total++;
			}
		}
		
		if (jit_blocks_attempted >= cum_report_at) {
			cum_report_at += 10000000;
			jit_report_cum_blockers();
		}
	}

	if (dbg) fprintf(stderr, "JIT-DBG %08x: compiled n=%d complete=%d\n", pc, n_compiled, complete);
	out->complete = complete;
	if (complete && n_compiled > 0) jit_blocks_complete++;

	/* Insert into block address cache so future executions skip recompilation.
	 * chain_entry_start is the code position immediately after the prologue;
	 * other blocks can branch directly there to skip the callee-save overhead.
	 * Contract: see AARCH64_JIT_RUNTIME_CONTRACT.md — block lifecycle. */
	if (n_compiled > 0)
		jit_bc_insert(pc, code_start, chain_entry_start, complete, n_compiled);

	out->chain_code = chain_entry_start;

	return n_compiled > 0;
}

#endif /* __aarch64__ */
