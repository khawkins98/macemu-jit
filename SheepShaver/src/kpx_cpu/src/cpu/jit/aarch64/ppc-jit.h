/*
 *  ppc-jit.h — PPC → AArch64 direct codegen JIT interface
 */

#ifndef PPC_JIT_H
#define PPC_JIT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __aarch64__

struct powerpc_registers;

struct ppc_jit_block {
	uint32_t *code;       /* normal ABI entry point (with prologue) */
	uint32_t *chain_code; /* chain entry point (after prologue) */
	size_t    code_size;
	uint32_t  ppc_start_pc;
	uint32_t  ppc_end_pc;
	int       n_insns;
	bool      complete;
};

bool ppc_jit_aarch64_init(size_t cache_size_kb);
void ppc_jit_aarch64_exit(void);
void ppc_jit_aarch64_flush(void);
void ppc_jit_aarch64_invalidate_pc(uint32_t pc);
void ppc_jit_aarch64_invalidate_range(uint32_t start, uint32_t end);
/* Cheap query: does a complete native block exist for this PC? */
bool ppc_jit_aarch64_has_block(uint32_t pc);
/* Cheap query: is this PC in the JIT's compilable domain (RAM or registered ROM)?
 * Used by the interpreter inner loop to hand execution back to the dispatcher. */
bool ppc_jit_aarch64_is_compilable(uint32_t pc);

typedef void (*ppc_jit_entry_fn)(void *regs);

/* Dispatch fast path: return the native entry point for an already-compiled
 * complete block, or NULL.  Hash lookup only — never compiles. */
ppc_jit_entry_fn ppc_jit_aarch64_lookup_fast(uint32_t pc);
/* Return the PPC instruction count for an already-compiled block, or 0.
 * Used by SS_JIT_VERIFY to re-run the same number of interpreter steps. */
int ppc_jit_aarch64_lookup_n_insns(uint32_t pc);
/* Register the (immutable) Mac ROM as a second JIT-compilable range. */
void ppc_jit_aarch64_set_rom_range(uint32_t guest_base, uint32_t size, const uint8_t *host_base);

/* Compilation stats for heartbeat logging */
uint32_t ppc_jit_aarch64_blocks_compiled(void);
/* Live stats for status bar display */
void ppc_jit_aarch64_get_stats(int *out_blocks, int *out_pool_size,
                               size_t *out_cache_used, size_t *out_cache_total);

bool ppc_jit_aarch64_compile(
	uint32_t pc,
	const uint8_t *ram,
	size_t ramsize,
	ppc_jit_block *out
);

#endif /* __aarch64__ */
#endif /* PPC_JIT_H */
