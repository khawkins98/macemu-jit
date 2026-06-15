/*
 *  paged_mmu.h - PowerPC classic 32-bit MMU translation core (SS_M18 S1 Task A2)
 *
 *  Mechanism-agnostic, standalone translation function: BAT match first, then
 *  segment + hashed-PTE (HTAB) walk. This is NOT wired into the emulator (no
 *  caller yet) — it is a pure function exercised in isolation by test_paged_mmu.
 *
 *  Authority: docs/superpowers/plans/2026-06-14-ss-m18-s1-impl-paged-mmu.md Task A2.
 *  High BATs (SPR 560-575) are NOT modelled here — they are dead code on the
 *  target machine (plan AD-2) and are omitted from translation by design.
 */
#ifndef PAGED_MMU_H
#define PAGED_MMU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The HTAB walk must read guest physical memory (the PTEGs). The translate
 * signature is pinned (no memory param), so the physical-word reader is
 * installed out-of-band. `pa` is a 4-byte-aligned guest physical address; the
 * reader returns the 32-bit PTE word stored there (host-native; the synthetic
 * test stores plain uint32 words, so endianness is out of scope of the walk). */
typedef uint32_t (*paged_mmu_phys_reader)(uint32_t pa, void *opaque);
void paged_mmu_set_phys_reader(paged_mmu_phys_reader fn, void *opaque);

/*
 * Translate effective address `ea` to physical address.
 *
 *   sr[16]   - segment registers SR0..SR15 (T=0 memory segments assumed)
 *   bat[16]  - BAT registers in SPR_IBAT0U..SPR_DBAT3L order (528..543):
 *              [0..7] = IBAT0U,IBAT0L..IBAT3U,IBAT3L; [8..15] = DBAT0U..DBAT3L.
 *              Data translation here scans the 4 DBAT pairs.
 *   sdr1     - HTAB base/mask (HTABORG bits 0-15, HTABMASK bits 23-31).
 *   ea       - the 32-bit effective address.
 *   msr_dr   - data-translation enable (MSR[DR]); 0 => real mode, PA == EA.
 *   out_pa   - resolved physical address (only valid on a true return).
 *
 * Returns true on resolve (out_pa written), false on translation fault.
 */
bool paged_mmu_translate(const uint32_t sr[16], const uint32_t bat[16],
                         uint32_t sdr1, uint32_t ea, uint32_t msr_dr,
                         uint32_t *out_pa);

#ifdef __cplusplus
}
#endif

#endif /* PAGED_MMU_H */
