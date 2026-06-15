/*
 *  dolphin_bat_arena.h - Dolphin "Dynamic BAT" shadow-arena (SS_M18 S1, standalone)
 *
 *  The bulk of the S1 live-MMU mechanism, built with ZERO live-boot dependency.
 *  Given a set of PowerPC BAT descriptors (the 4 DBAT upper/lower pairs) plus the
 *  segment/SDR1 state, it maintains a HOST shadow mapping so that a *bare* guest
 *  access at `window_base + EA` lands on `phys_base + PA` — exactly the Dolphin
 *  technique where access-time codegen never changes and the cost is paid at the
 *  (rare) BAT map-change instead of per access.
 *
 *  This is the macemu reimplementation of Dolphin's:
 *    - MemoryManager::UpdateDBATMappings()  (Source/Core/Core/HW/Memmap.cpp ~L233)
 *    - MMU::UpdateBATs() / DBATUpdated()     (Source/Core/Core/PowerPC/MMU.cpp ~L1470/1535)
 *    - CanCreateHostMappingForGuestPages()   (Source/Core/Core/HW/Memmap.cpp ~L343)
 *  Dolphin repo: https://github.com/dolphin-emu/dolphin  (GPLv2)
 *  Pinned SHA:   144d19433aa734c19c34e5978a1b817d2aa12663  (per DONOR-NOTES.md)
 *  Status:       NEEDS VALIDATION against the live NK MMU trace (see below). The
 *                host arena primitive is PROVEN by test_shm_arena_spike.cpp
 *                (acd89dce) + test_shm_arena_multientry.cpp (cd7a62b6); the BAT
 *                decode + remap policy here is SPEC-DERIVED and cross-checked
 *                against paged_mmu_translate() (machine/paged_mmu.cpp). It is NOT
 *                wired into the live JIT/boot (that is a separate spike, out of
 *                scope) — this module is structurally inert in the binary.
 *
 *  Authority: docs/superpowers/plans/2026-06-14-ss-m18-s1-impl-paged-mmu.md (Task B
 *  mechanism, built standalone behind a pinned interface) +
 *  docs/planning/newsheep/FINDINGS-discriminator-a.md (COARSE verdict -> shadow-arena).
 */
#ifndef DOLPHIN_BAT_ARENA_H
#define DOLPHIN_BAT_ARENA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- *
 * BAT descriptor decode (pure; no host mapping involved).
 *
 * Upper:  BEPI[0:14]  resv[15:18]  BL[19:29]  Vs[30]  Vp[31]
 * Lower:  BRPN[0:14]  resv[15:24]  WIMG[25:28]  resv[29]  PP[30:31]
 * Block size = (BL+1) * 128 KB; both BEPI and BRPN are 128 KB-granular fields,
 * so a well-formed block is always 128 KB-aligned (hence 16 KB host-page-safe).
 * ------------------------------------------------------------------------- */
typedef struct {
	uint32_t ea_base;   /* BEPI — block-aligned effective base                */
	uint32_t pa_base;   /* BRPN — block-aligned physical base                 */
	uint32_t size;      /* (BL+1) * 128 KB                                    */
	uint32_t wimg;      /* cache attributes (informational; not used by remap)*/
	uint32_t pp;        /* protection bits (informational)                    */
	bool     valid;     /* true iff Vs or Vp set                              */
} bat_block_t;

/* Decode one upper/lower BAT pair. valid=false (size=0) if neither Vs nor Vp set. */
bat_block_t dba_decode_bat_pair(uint32_t batu, uint32_t batl);

/* Feasibility gate — the CanCreateHostMappingForGuestPages() equivalent. A block
 * is fast-mappable iff ea_base, pa_base, AND size are all multiples of host_page,
 * so a single host page maps cleanly onto contiguous, equally-permissioned guest
 * pages. (Well-formed BATs always pass on a 16 KB host; an adversarial misaligned
 * input falls to the slow path — Discriminator-A's "fine" fork.) */
bool dba_block_fast_mappable(const bat_block_t *blk, uint32_t host_page);

/* ------------------------------------------------------------------------- *
 * The shadow arena.
 *
 * A single named SHM store of `size` bytes, aliased into two VA views:
 *   - WINDOW: EA-indexed; the bare-access target (window_base + EA).
 *   - PHYS:   PA-indexed; the physical backing store (phys_base + PA).
 * The window starts fully identity-aliased (window[i] -> phys[i]). Each
 * dba_arena_update() re-aliases BAT-covered sub-ranges so the bare access lands
 * on the right backing page; ranges that fall out of coverage revert to identity.
 *
 * NOTE on bounds: this standalone arena models ONE contiguous EA/PA space of
 * `size` bytes (sized to guest RAM in the eventual port; ROM/MMIO would be
 * separate reservations). EAs and PAs are taken modulo nothing — blocks whose
 * EA or PA range exceeds the arena are counted as slow-path (out-of-arena), which
 * keeps the test self-contained without a 4 GiB reservation.
 * ------------------------------------------------------------------------- */
typedef struct dba_arena dba_arena_t;

/* Create the arena. Returns NULL on failure or on a non-(macOS arm64 16K) host.
 * *actual_size receives the granted store size (a proxy smaller than `size` may
 * be vended; the caller must honor it). */
dba_arena_t *dba_arena_create(size_t size, size_t *actual_size);
void         dba_arena_destroy(dba_arena_t *a);

uint8_t *dba_arena_window(dba_arena_t *a);  /* EA view (bare access target)  */
uint8_t *dba_arena_phys(dba_arena_t *a);    /* PA view (backing store)       */
size_t   dba_arena_size(dba_arena_t *a);

/* The map-change hook (Dolphin UpdateDBATMappings analogue). Given the full
 * bat[16] (SPR_IBAT0U..SPR_DBAT3L order; the data path scans the 4 DBAT pairs
 * bat[8..15]), rebuild the window so a bare access at window+EA lands on phys+PA
 * for every BAT-covered, fast-mappable, in-arena EA. Previously-mapped ranges no
 * longer covered revert to identity.
 *
 * Returns the number of blocks fast-mapped; *out_slow (if non-NULL) receives the
 * count that fell to the slow path (infeasible or out-of-arena). */
int dba_arena_update(dba_arena_t *a, const uint32_t bat[16],
                     uint32_t host_page, int *out_slow);

#ifdef __cplusplus
}
#endif

#endif /* DOLPHIN_BAT_ARENA_H */
