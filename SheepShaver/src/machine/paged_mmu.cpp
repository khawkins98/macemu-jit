/*
 *  paged_mmu.cpp - PowerPC classic 32-bit MMU translation core (SS_M18 S1 Task A2)
 *
 *  Pure, mechanism-agnostic effective->physical translation:
 *    1. BAT match first (the 4 DBAT pairs: BEPI/BL/BRPN, Vs/Vp validity).
 *    2. segment + hashed-PTE (HTAB) walk: SR -> VSID, primary/secondary PTEG
 *       via SDR1 HTABORG/HTABMASK, PTE match (V/VSID/H/API), RPN -> PA.
 *
 *  Reference: PowerPC Microprocessor Family: The Programming Environments
 *  (PEM) §7.4 (block address translation) and §7.5 (page address translation),
 *  cross-read against PearPC ppc_mmu.cc. This is a SPEC-DERIVED implementation;
 *  no external oracle is linked here (the PearPC/QEMU cross-check is OWED, see
 *  test_paged_mmu.cpp). High BATs (SPR 560-575) are NOT modelled (plan AD-2:
 *  dead code on the target machine).
 *
 *  This function is standalone — it has no caller in the emulator yet.
 */
#include "paged_mmu.h"

static paged_mmu_phys_reader g_phys_reader = 0;
static void *g_phys_opaque = 0;

void paged_mmu_set_phys_reader(paged_mmu_phys_reader fn, void *opaque)
{
	g_phys_reader = fn;
	g_phys_opaque = opaque;
}

static inline uint32_t phys_read32(uint32_t pa)
{
	if (!g_phys_reader)
		return 0;
	return g_phys_reader(pa, g_phys_opaque);
}

/* --- BAT block address translation (one upper/lower pair) ------------------ *
 * Upper:  BEPI[0:14]  resv[15:18]  BL[19:29]  Vs[30]  Vp[31]
 * Lower:  BRPN[0:14]  resv[15:24]  WIMG[25:28]  resv[29]  PP[30:31]
 * Block size = (BL+1) * 128 KB; the in-block "don't care" mask is
 *   hmask = (BL << 17) | 0x1FFFF.  (BL=0 => 0x1FFFF = 128 KB; BL=0x7FF => 256 MB-1.) */
static bool bat_pair_match(uint32_t batu, uint32_t batl, uint32_t ea, uint32_t *out_pa)
{
	// Validity: either supervisor (Vs) or user (Vp) — privilege is not modelled.
	if ((batu & 0x3) == 0)
		return false;

	uint32_t bl    = (batu >> 2) & 0x7FF;     // BL field, bits 19-29
	uint32_t hmask = (bl << 17) | 0x1FFFF;    // in-block offset mask
	uint32_t bepi  = batu & 0xFFFE0000;       // BEPI, bits 0-14
	uint32_t brpn  = batl & 0xFFFE0000;       // BRPN, bits 0-14

	if ((ea & ~hmask) != (bepi & ~hmask))
		return false;

	*out_pa = (brpn & ~hmask) | (ea & hmask);
	return true;
}

/* --- hashed-PTE (HTAB) page table walk ------------------------------------ *
 * Build the PTEG physical address from SDR1 + the 19-bit hash, then scan the
 * 8 PTEs of the PTEG for a (V, VSID, H, API) match. */
static bool htab_walk(uint32_t vsid, uint32_t page_index, uint32_t api,
                      uint32_t sdr1, bool secondary, uint32_t ea, uint32_t *out_pa)
{
	uint32_t htaborg  = sdr1 & 0xFFFF0000;    // bits 0-15
	uint32_t htabmask = sdr1 & 0x1FF;         // bits 23-31 (9 bits)

	uint32_t hash = (vsid ^ page_index) & 0x7FFFF;   // 19-bit primary hash
	if (secondary)
		hash = (~hash) & 0x7FFFF;

	// PTEG physical address (PEM Fig 7-21):
	//   PA[0:6]   = HTABORG[0:6]
	//   PA[7:15]  = HTABORG[7:15] | (Hash[0:8] & HTABMASK)
	//   PA[16:25] = Hash[9:18]
	//   PA[26:31] = 0  (PTEG = 64 bytes)
	uint32_t pteg_addr =
		  (htaborg & 0xFE000000)
		| ((((htaborg & 0x01FF0000) >> 16) | ((hash >> 10) & htabmask)) << 16)
		| ((hash & 0x3FF) << 6);

	uint32_t want_h = secondary ? 1u : 0u;

	for (int i = 0; i < 8; i++) {
		uint32_t pte0 = phys_read32(pteg_addr + i * 8);
		uint32_t pte1 = phys_read32(pteg_addr + i * 8 + 4);

		bool     v       = (pte0 >> 31) & 1;
		uint32_t pte_vsid= (pte0 >> 7) & 0xFFFFFF;
		uint32_t pte_h   = (pte0 >> 6) & 1;
		uint32_t pte_api = pte0 & 0x3F;

		if (v && pte_vsid == vsid && pte_h == want_h && pte_api == api) {
			uint32_t rpn = (pte1 >> 12) & 0xFFFFF;   // RPN, bits 0-19
			*out_pa = (rpn << 12) | (ea & 0xFFF);
			return true;
		}
	}
	return false;
}

bool paged_mmu_translate(const uint32_t sr[16], const uint32_t bat[16],
                         uint32_t sdr1, uint32_t ea, uint32_t msr_dr,
                         uint32_t *out_pa)
{
	uint32_t pa = 0;

	// (1) BAT match first — scan the 4 DBAT pairs (bat[8..15]). BAT translation
	//     applies regardless of MSR[DR] (block translation is independent).
	for (int p = 0; p < 4; p++) {
		uint32_t batu = bat[8 + p * 2];
		uint32_t batl = bat[8 + p * 2 + 1];
		if (bat_pair_match(batu, batl, ea, &pa)) {
			*out_pa = pa;
			return true;
		}
	}

	// (2) Translation disabled => real mode, PA == EA.
	if (!msr_dr) {
		*out_pa = ea;
		return true;
	}

	// (3) Segment + hashed-PTE walk.
	uint32_t srval = sr[(ea >> 28) & 0xF];

	// T=1 (direct-store / I/O controller) segments are not modelled.
	if (srval & 0x80000000)
		return false;

	uint32_t vsid       = srval & 0x00FFFFFF;          // VSID, SR bits 8-31
	uint32_t page_index = (ea >> 12) & 0xFFFF;         // EA bits 4-19 (16 bits)
	uint32_t api        = (page_index >> 10) & 0x3F;   // top 6 bits of page index

	if (htab_walk(vsid, page_index, api, sdr1, /*secondary=*/false, ea, &pa)) {
		*out_pa = pa;
		return true;
	}
	if (htab_walk(vsid, page_index, api, sdr1, /*secondary=*/true, ea, &pa)) {
		*out_pa = pa;
		return true;
	}

	return false;   // page fault
}
