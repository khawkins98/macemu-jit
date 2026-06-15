/*
 *  dolphin_bat_arena.cpp - Dolphin "Dynamic BAT" shadow-arena (SS_M18 S1, standalone)
 *
 *  See dolphin_bat_arena.h for the contract and provenance. Compiled ONLY by the
 *  machine/ unit-test Makefile; NOT linked into the SheepShaver binary (inert until
 *  a separate S1-integration spike wires it). No kpx_cpu / ppc-jit / emulator
 *  dependency — it unit-tests standalone, like paged_mmu.cpp and the SHM spikes.
 *
 *  PORT PROVENANCE (backport hygiene — needs validation against the live NK trace):
 *    Reimplemented-to-spec from Dolphin (NOT vendored), GPLv2, pinned SHA
 *    144d19433aa734c19c34e5978a1b817d2aa12663:
 *      - HW/Memmap.cpp  MemoryManager::UpdateDBATMappings (~L233) -> dba_arena_update
 *      - HW/Memmap.cpp  CanCreateHostMappingForGuestPages (~L343) -> dba_block_fast_mappable
 *      - PowerPC/MMU.cpp UpdateBATs/DBATUpdated (~L1470/1535)     -> dba_decode_bat_pair
 *      - Common/MemArena.cpp (mach_make_memory_entry_64 / vm_map) -> arena create/remap
 *    The host VM primitive (named SHM entry + aliased views + mach_vm_map
 *    FIXED|OVERWRITE sub-range re-alias) is PROVEN on macOS arm64 16K by
 *    test_shm_arena_spike.cpp (acd89dce) and test_shm_arena_multientry.cpp (cd7a62b6).
 *    The remap policy (identity baseline + BAT-covered re-alias) is SPEC-DERIVED and
 *    cross-checked against paged_mmu_translate(); the live-NK mapping set is NOT yet
 *    observed (the QEMU rig stalls pre-NK-MMU-install, Discriminator-A Evidence B), so
 *    coverage adequacy on a real boot remains OWED to the live S1 integration (G1.e).
 */
#include "dolphin_bat_arena.h"

#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------------- *
 * Pure BAT decode + feasibility (host-independent).
 * ------------------------------------------------------------------------- */
bat_block_t dba_decode_bat_pair(uint32_t batu, uint32_t batl)
{
	bat_block_t b;
	memset(&b, 0, sizeof(b));

	/* Vs (bit 30) or Vp (bit 31). */
	b.valid = (batu & 0x3u) != 0;
	if (!b.valid)
		return b;

	uint32_t bl   = (batu >> 2) & 0x7FFu;        /* BL, bits 19-29            */
	b.size        = (bl + 1u) << 17;             /* (BL+1) * 128 KB           */
	b.ea_base     = batu & 0xFFFE0000u;          /* BEPI, bits 0-14           */
	b.pa_base     = batl & 0xFFFE0000u;          /* BRPN, bits 0-14           */
	b.wimg        = (batl >> 3) & 0xFu;          /* WIMG, bits 25-28          */
	b.pp          = batl & 0x3u;                 /* PP, bits 30-31            */
	return b;
}

bool dba_block_fast_mappable(const bat_block_t *blk, uint32_t host_page)
{
	if (!blk->valid || blk->size == 0 || host_page == 0)
		return false;
	uint32_t mask = host_page - 1u;
	/* host_page must be a power of two; all three quantities must be aligned to it
	 * so a single host page covers a contiguous, uniformly-mapped guest run. */
	if ((host_page & mask) != 0)
		return false;
	if ((blk->ea_base & mask) != 0) return false;
	if ((blk->pa_base & mask) != 0) return false;
	if ((blk->size    & mask) != 0) return false;
	return true;
}

/* ------------------------------------------------------------------------- *
 * The host arena (macOS arm64 16K only; graceful NULL elsewhere).
 * ------------------------------------------------------------------------- */
#if defined(__APPLE__) && defined(__MACH__) && defined(__aarch64__)
#define DBA_ON_TARGET 1
#else
#define DBA_ON_TARGET 0
#endif

#if DBA_ON_TARGET
#include <mach/mach.h>
#include <mach/mach_vm.h>

#define DBA_MAX_MAPPED 8   /* 4 DBAT pairs is the live max; headroom for IBAT too */

struct dba_arena {
	mem_entry_name_port_t mem_entry;
	mach_vm_address_t     window;     /* EA view base */
	mach_vm_address_t     phys;       /* PA view base */
	mach_vm_size_t        size;

	/* Currently re-aliased window sub-ranges (for identity-restore on refresh). */
	struct { uint32_t ea_base; uint32_t size; } mapped[DBA_MAX_MAPPED];
	int n_mapped;
};

/* Atomic one-step re-alias of window[off..off+span) onto backing offset off2.
 * The proven primitive (test_shm_arena_*): no unmap-then-map, no transient hole. */
static kern_return_t realias(dba_arena_t *a, uint32_t off, uint32_t span, uint32_t off2)
{
	mach_vm_address_t target = a->window + off;
	return mach_vm_map(mach_task_self(), &target, span, /*mask=*/0,
	                   VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
	                   a->mem_entry, off2, /*copy=*/FALSE,
	                   VM_PROT_READ | VM_PROT_WRITE,
	                   VM_PROT_READ | VM_PROT_WRITE,
	                   VM_INHERIT_NONE);
}

dba_arena_t *dba_arena_create(size_t size, size_t *actual_size)
{
	dba_arena_t *a = (dba_arena_t *)calloc(1, sizeof(*a));
	if (!a)
		return NULL;

	memory_object_size_t entry_size = (memory_object_size_t)size;
	a->mem_entry = MACH_PORT_NULL;
	kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &entry_size,
	                       /*offset=*/0,
	                       MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE,
	                       &a->mem_entry, /*parent=*/MACH_PORT_NULL);
	if (kr != KERN_SUCCESS || entry_size < (memory_object_size_t)size) {
		free(a);
		return NULL;
	}
	a->size = entry_size;

	/* Two aliased RW views of the same named store. */
	kr = mach_vm_map(mach_task_self(), &a->phys, a->size, 0, VM_FLAGS_ANYWHERE,
	                 a->mem_entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
	                 VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
	if (kr == KERN_SUCCESS)
		kr = mach_vm_map(mach_task_self(), &a->window, a->size, 0, VM_FLAGS_ANYWHERE,
		                 a->mem_entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
		                 VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
	if (kr != KERN_SUCCESS) {
		if (a->phys) mach_vm_deallocate(mach_task_self(), a->phys, a->size);
		mach_port_deallocate(mach_task_self(), a->mem_entry);
		free(a);
		return NULL;
	}

	/* Window is identity-aliased by construction (both views map offset 0). */
	a->n_mapped = 0;
	if (actual_size)
		*actual_size = (size_t)a->size;
	return a;
}

void dba_arena_destroy(dba_arena_t *a)
{
	if (!a)
		return;
	if (a->window) mach_vm_deallocate(mach_task_self(), a->window, a->size);
	if (a->phys)   mach_vm_deallocate(mach_task_self(), a->phys, a->size);
	if (a->mem_entry != MACH_PORT_NULL)
		mach_port_deallocate(mach_task_self(), a->mem_entry);
	free(a);
}

uint8_t *dba_arena_window(dba_arena_t *a) { return a ? (uint8_t *)a->window : NULL; }
uint8_t *dba_arena_phys(dba_arena_t *a)   { return a ? (uint8_t *)a->phys   : NULL; }
size_t   dba_arena_size(dba_arena_t *a)   { return a ? (size_t)a->size      : 0; }

int dba_arena_update(dba_arena_t *a, const uint32_t bat[16],
                     uint32_t host_page, int *out_slow)
{
	int fast = 0, slow = 0;
	if (!a) { if (out_slow) *out_slow = 0; return 0; }

	/* (1) Teardown: restore every previously-mapped window range to identity
	 *     (window[ea] -> phys[ea]). This mirrors Dolphin's UnmapFromMemoryRegion
	 *     of m_dbat_mapped_entries before re-applying the new set. */
	for (int i = 0; i < a->n_mapped; i++)
		realias(a, a->mapped[i].ea_base, a->mapped[i].size, a->mapped[i].ea_base);
	a->n_mapped = 0;

	/* (2) Apply: scan the 4 DBAT pairs (bat[8..15]) and re-alias each feasible,
	 *     in-arena block so window+EA lands on phys+PA. */
	for (int p = 0; p < 4; p++) {
		bat_block_t blk = dba_decode_bat_pair(bat[8 + p * 2], bat[8 + p * 2 + 1]);
		if (!blk.valid)
			continue;
		if (!dba_block_fast_mappable(&blk, host_page)) { slow++; continue; }
		/* Out-of-arena (EA or PA range exceeds the store) -> slow path. */
		if ((uint64_t)blk.ea_base + blk.size > a->size ||
		    (uint64_t)blk.pa_base + blk.size > a->size) { slow++; continue; }

		if (realias(a, blk.ea_base, blk.size, blk.pa_base) != KERN_SUCCESS) {
			slow++;
			continue;
		}
		if (a->n_mapped < DBA_MAX_MAPPED) {
			a->mapped[a->n_mapped].ea_base = blk.ea_base;
			a->mapped[a->n_mapped].size    = blk.size;
			a->n_mapped++;
		}
		fast++;
	}

	if (out_slow)
		*out_slow = slow;
	return fast;
}

#else /* !DBA_ON_TARGET — graceful no-op so the module compiles on any host. */

struct dba_arena { int unused; };

dba_arena_t *dba_arena_create(size_t, size_t *actual_size)
{
	if (actual_size) *actual_size = 0;
	return NULL;
}
void     dba_arena_destroy(dba_arena_t *) {}
uint8_t *dba_arena_window(dba_arena_t *) { return NULL; }
uint8_t *dba_arena_phys(dba_arena_t *)   { return NULL; }
size_t   dba_arena_size(dba_arena_t *)   { return 0; }
int      dba_arena_update(dba_arena_t *, const uint32_t *, uint32_t, int *out_slow)
{
	if (out_slow) *out_slow = 0;
	return 0;
}

#endif /* DBA_ON_TARGET */
