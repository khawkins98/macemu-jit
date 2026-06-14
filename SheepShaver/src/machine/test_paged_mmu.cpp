/*
 *  test_paged_mmu.cpp - Unit tests for the PPC classic MMU translation core
 *  (SS_M18 S1 Task A2). Standalone: links only paged_mmu.cpp, no kpx_cpu.
 *
 *  Oracle honesty: the `oracle_translate` below is a SPEC-DERIVED reference
 *  written to be STRUCTURALLY INDEPENDENT of paged_mmu_translate() — a literal
 *  range-check BAT + step-by-step PTEG walk with a different code shape, so a
 *  real bug in either implementation produces a PA/fault diff rather than mutual
 *  self-agreement. It is, however, a SAME-AUTHOR differential-implementation
 *  test: the external PearPC/QEMU cross-check is OWED and NOT done here (see the
 *  plan's A2 "PearPC/QEMU cross-check deferred as owed").
 *
 *  Battery (every row asserts BOTH equal-PA AND equal resolve-vs-fault):
 *    (1) two-context — same EA under two SR programmings -> two distinct PAs
 *        (an identity-only translator FAILS this row by construction);
 *    (2) COARSE-BAT — a 256 MB-class DBAT block resolves to the BAT physical PA.
 *  No high-BAT rows: SPR 560-575 are dead code on the target machine (plan AD-2).
 */
#include "paged_mmu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

// ---------------------------------------------------------------------------
// Synthetic guest physical memory backing the HTAB (plain uint32 words).
// ---------------------------------------------------------------------------
#define PMEM_BYTES 0x100000u
static uint32_t pmem[PMEM_BYTES / 4];

static uint32_t pmem_read(uint32_t pa, void *opaque)
{
	(void)opaque;
	if (pa + 4 > PMEM_BYTES)
		return 0;
	return pmem[pa / 4];
}

static void pmem_write(uint32_t pa, uint32_t val) { pmem[pa / 4] = val; }

// Compute the primary PTEG byte address for (vsid, page_index) given SDR1.
// (Shared test-setup helper; both translators then look it up independently.)
static uint32_t pteg_base(uint32_t vsid, uint32_t page_index, uint32_t sdr1, bool secondary)
{
	uint32_t htaborg  = sdr1 & 0xFFFF0000;
	uint32_t htabmask = sdr1 & 0x1FF;
	uint32_t hash = (vsid ^ page_index) & 0x7FFFF;
	if (secondary) hash = (~hash) & 0x7FFFF;
	return (htaborg & 0xFE000000)
	     | ((((htaborg & 0x01FF0000) >> 16) | ((hash >> 10) & htabmask)) << 16)
	     | ((hash & 0x3FF) << 6);
}

// Install a valid PTE (slot 0 of the PTEG) mapping (vsid, page_index) -> rpn.
static void install_pte(uint32_t vsid, uint32_t page_index, uint32_t rpn,
                        uint32_t sdr1, bool secondary)
{
	uint32_t api  = (page_index >> 10) & 0x3F;
	uint32_t base = pteg_base(vsid, page_index, sdr1, secondary);
	uint32_t pte0 = (1u << 31) | (vsid << 7) | ((secondary ? 1u : 0u) << 6) | api;
	uint32_t pte1 = (rpn & 0xFFFFF) << 12;
	pmem_write(base, pte0);
	pmem_write(base + 4, pte1);
}

// ---------------------------------------------------------------------------
// Structurally-independent reference oracle (range-check BAT + explicit walk).
// ---------------------------------------------------------------------------
static bool oracle_translate(const uint32_t sr[16], const uint32_t bat[16],
                             uint32_t sdr1, uint32_t ea, uint32_t msr_dr,
                             uint32_t *out_pa)
{
	// Real addressing mode (MSR[DR]=0): translation OFF, BATs AND page table
	// bypassed, PA == EA (PEM §7.4.1). MUST precede the BAT scan. [paged_mmu
	// adversary Finding 1 — both impl and oracle previously had BAT-before-DR.]
	if (!msr_dr) { *out_pa = ea; return true; }

	// --- BAT: explicit block-range containment (vs the impl's mask compare) ---
	for (int p = 0; p < 4; p++) {
		uint32_t u = bat[8 + p * 2];
		uint32_t l = bat[8 + p * 2 + 1];
		bool valid = (u & 0x2) || (u & 0x1);            // Vs or Vp
		if (!valid) continue;
		uint32_t bl   = (u >> 2) & 0x7FF;
		uint64_t size = (uint64_t)(bl + 1) * 0x20000ull; // (BL+1) * 128 KB
		uint32_t bepi = u & 0xFFFE0000;
		uint32_t base = bepi & (uint32_t)~(size - 1);    // block-aligned start
		uint32_t brpn = (l & 0xFFFE0000) & (uint32_t)~(size - 1);
		if (ea >= base && (uint64_t)ea < (uint64_t)base + size) {
			*out_pa = brpn + (ea - base);
			return true;
		}
	}

	// --- Page walk: rebuild the VA fields step by step ---
	uint32_t seg = sr[ea >> 28];
	if (seg & 0x80000000u) return false;                 // T=1 not modelled
	uint32_t vsid   = seg & 0x00FFFFFFu;
	uint32_t pindex = (ea & 0x0FFFF000u) >> 12;          // EA bits 4-19
	uint32_t want_api = pindex >> 10;                    // top 6 bits

	for (int pass = 0; pass < 2; pass++) {
		bool secondary = (pass == 1);
		uint32_t base = pteg_base(vsid, pindex, sdr1, secondary);
		for (uint32_t slot = 0; slot < 8; slot++) {
			uint32_t w0 = pmem_read(base + slot * 8, 0);
			uint32_t w1 = pmem_read(base + slot * 8 + 4, 0);
			if ((w0 & 0x80000000u) == 0) continue;       // not valid
			uint32_t e_vsid = (w0 & 0x7FFFFF80u) >> 7;
			uint32_t e_h    = (w0 & 0x00000040u) >> 6;
			uint32_t e_api  =  w0 & 0x0000003Fu;
			if (e_vsid != vsid) continue;
			if (e_h != (secondary ? 1u : 0u)) continue;
			if (e_api != want_api) continue;
			uint32_t rpn = w1 >> 12;
			*out_pa = (rpn << 12) | (ea & 0xFFFu);
			return true;
		}
	}
	return false;
}

// One battery row: assert impl == oracle on BOTH outcome and PA.
static void check_row(const char *name, const uint32_t sr[16], const uint32_t bat[16],
                      uint32_t sdr1, uint32_t ea, uint32_t msr_dr, uint32_t expect_pa,
                      bool expect_resolve)
{
	uint32_t pa_i = 0xDEADBEEF, pa_o = 0xDEADBEEF;
	bool r_i = paged_mmu_translate(sr, bat, sdr1, ea, msr_dr, &pa_i);
	bool r_o = oracle_translate(sr, bat, sdr1, ea, msr_dr, &pa_o);

	CHECK(r_i == r_o);                       // impl agrees with oracle on resolve/fault
	CHECK(r_i == expect_resolve);            // ... and both match the row's intent
	if (expect_resolve) {
		CHECK(pa_i == pa_o);                 // impl PA == oracle PA
		CHECK(pa_i == expect_pa);            // ... and the spec-computed PA
	}
	printf("  [row] %-16s ea=%08x dr=%u -> %s pa=%08x (impl) / %08x (oracle)\n",
	       name, ea, msr_dr, r_i ? "RESOLVE" : "FAULT", pa_i, pa_o);
}

int main()
{
	uint32_t sr[16];
	uint32_t bat[16];

	paged_mmu_set_phys_reader(pmem_read, 0);

	// =====================================================================
	// (1) TWO-CONTEXT: same EA, two SR programmings -> two distinct PAs.
	//     Identity-only translation is impossible to satisfy here.
	// =====================================================================
	printf("Battery 1: two-context (HTAB)\n");
	memset(bat, 0, sizeof(bat));               // no BAT — force the page walk
	memset(pmem, 0, sizeof(pmem));

	const uint32_t SDR1 = 0x00080000u;         // HTABORG=0x80000, HTABMASK=0
	const uint32_t EA   = 0x10ABC123u;         // seg 1, pindex 0xABC, api 2, off 0x123
	const uint32_t PIDX = (EA & 0x0FFFF000u) >> 12;
	const uint32_t VSID_A = 0x00111111u;
	const uint32_t VSID_B = 0x00222222u;
	const uint32_t RPN_A  = 0x05000u;
	const uint32_t RPN_B  = 0x07000u;

	for (int i = 0; i < 16; i++) sr[i] = 0;

	// Context A
	memset(pmem, 0, sizeof(pmem));
	sr[1] = VSID_A;
	install_pte(VSID_A, PIDX, RPN_A, SDR1, /*secondary=*/false);
	check_row("ctxA-htab", sr, bat, SDR1, EA, /*dr=*/1,
	          (RPN_A << 12) | (EA & 0xFFF), true);

	// Context B — same EA, different segment programming -> different PA
	memset(pmem, 0, sizeof(pmem));
	sr[1] = VSID_B;
	install_pte(VSID_B, PIDX, RPN_B, SDR1, /*secondary=*/false);
	check_row("ctxB-htab", sr, bat, SDR1, EA, /*dr=*/1,
	          (RPN_B << 12) | (EA & 0xFFF), true);

	// The decisive distinctness assertion: identity-only MUST fail by construction.
	CHECK(((RPN_A << 12) | (EA & 0xFFF)) != ((RPN_B << 12) | (EA & 0xFFF)));
	CHECK(((RPN_A << 12) | (EA & 0xFFF)) != EA);   // and neither equals the EA

	// Secondary-PTEG path: install only in the secondary PTEG, must still resolve.
	memset(pmem, 0, sizeof(pmem));
	sr[1] = VSID_A;
	install_pte(VSID_A, PIDX, RPN_A, SDR1, /*secondary=*/true);
	check_row("ctxA-2ndary", sr, bat, SDR1, EA, /*dr=*/1,
	          (RPN_A << 12) | (EA & 0xFFF), true);

	// Page fault: nothing installed -> both must FAULT.
	memset(pmem, 0, sizeof(pmem));
	sr[1] = VSID_A;
	check_row("ctxA-fault", sr, bat, SDR1, EA, /*dr=*/1, 0, false);

	// =====================================================================
	// (2) COARSE-BAT: a 256 MB DBAT block resolves to the BAT physical PA,
	//     independent of MSR[DR] and with no PTE installed.
	// =====================================================================
	printf("Battery 2: coarse BAT (256 MB block)\n");
	for (int i = 0; i < 16; i++) sr[i] = 0;
	memset(bat, 0, sizeof(bat));
	memset(pmem, 0, sizeof(pmem));

	// DBAT0: map EA 0x90000000..0x9FFFFFFF (256 MB) -> phys 0x30000000.
	bat[8] = 0x90000000u | (0x7FFu << 2) | 0x2u;   // BEPI | BL=0x7FF | Vs
	bat[9] = 0x30000000u | 0x2u;                   // BRPN | PP

	const uint32_t BEA = 0x90001000u;
	check_row("dbat-mid", sr, bat, SDR1, BEA, /*dr=*/1, 0x30001000u, true);
	// Block low edge and high edge.
	check_row("dbat-low", sr, bat, SDR1, 0x90000000u, /*dr=*/1, 0x30000000u, true);
	check_row("dbat-high", sr, bat, SDR1, 0x9FFFF000u, /*dr=*/1, 0x3FFFF000u, true);
	// Real mode (DR=0): translation OFF — a DBAT that WOULD match an EA must NOT
	// translate; PA == EA identity. This row actively proves Finding-1's fix
	// (BEA=0x90001000 is inside DBAT0's block, yet under DR=0 must stay identity).
	check_row("dbat-dr0-identity", sr, bat, SDR1, BEA, /*dr=*/0, BEA, true);
	// Just outside the block -> also real-mode identity under DR=0.
	check_row("dbat-miss-rm", sr, bat, SDR1, 0xA0000000u, /*dr=*/0, 0xA0000000u, true);

	printf("\ntest_paged_mmu: ALL %d CHECKS PASSED\n", n_pass);
	return 0;
}
