/*
 *  test_trampoline_launch.cpp - SS_M18 Stage 2b T3 launch-seam static assertion
 *                               (the gated-ON checkable half of G(T3)). Standalone:
 *                               links the pure marshalling-shim core for the
 *                               EXEC_NATIVE encode/decode; NO emulator dependency.
 *
 *  Operation NewSheep. The T3 launch seam (sheepshaver_glue.cpp init_emul_ppc
 *  newworld block) cannot be exercised by a live boot pre-S1 (the Trampoline needs
 *  T4's wired backend + S1's live /mmu to progress — DEFERRED-TO-post-S1). What IS
 *  statically checkable, and what this test pins, is the LAUNCH ABI the seam sets:
 *    (1) the reserved r5 shim entry (TRAMP_SHIM_ENTRY) is page-aligned, ABOVE the
 *        staged-ELF footprint, and DISJOINT from both PT_LOAD ranges — so writing
 *        the shim opcode there cannot clobber the placed Trampoline image;
 *    (2) the EXEC_NATIVE intercept opcode the seam writes at TRAMP_SHIM_ENTRY (via
 *        ss_ofci_shim_opcode() == tramp_ofci_encode_native_op(NATIVE_OF_CI_SHIM))
 *        round-trips through the decode execute_sheep() uses, when stored big-endian
 *        in mock guest RAM exactly as WriteMacInt32 stores it;
 *    (3) the pinned CHRP entry constants (r2 = 0x1001e8, entry = 0x20f078).
 *
 *  Built with -DTRAMPOLINE_OFCI_SHIM_STANDALONE_TEST -DTRAMPOLINE_LOADER_STANDALONE_TEST
 *  (both emulator wrappers excluded; this is a pure-constant + encode/decode test).
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "trampoline_loader.h"      /* TRAMP_* launch + ELF-footprint constants */
#include "trampoline_ofci_shim.h"   /* tramp_ofci_encode/decode_native_op */

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg) do { g_checks++; if (!(cond)) { \
	g_fail++; fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); } } while (0)

/* The selector the emulator wrapper encodes (thunks.h NATIVE_OF_CI_SHIM). The
 * standalone build cannot include thunks.h (emulator), so we re-derive the opcode
 * from the SAME pure encoder ss_ofci_shim_opcode() uses; the exact selector value
 * is covered by the T2 micro-test's link/ABI smoke. Here we prove the round-trip
 * over the big-endian guest store, for any in-range selector. */
static void store_be32(uint8_t *p, uint32_t v)   /* mirrors WriteMacInt32 */
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)(v);
}
static uint32_t load_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

int main(void)
{
	/* ---- Test 1: reserved shim region placement (the r5 entry) ------------- */
	{
		CHECK((TRAMP_SHIM_ENTRY & 0xFFFu) == 0, "shim entry is page-aligned");
		CHECK(TRAMP_SHIM_ENTRY >= TRAMP_APERTURE_TOP, "shim entry is at/above the ELF top");
		/* disjoint from the data PT_LOAD [0x100000, 0x100000+memsz) */
		CHECK(TRAMP_SHIM_ENTRY >= TRAMP_DATA_VADDR + TRAMP_DATA_MEMSZ ||
		      TRAMP_SHIM_ENTRY + 4 <= TRAMP_DATA_VADDR, "shim entry disjoint from data segment");
		/* disjoint from the exec PT_LOAD [0x200000, 0x210260) */
		CHECK(TRAMP_SHIM_ENTRY >= TRAMP_APERTURE_TOP ||
		      TRAMP_SHIM_ENTRY + 4 <= TRAMP_EXEC_VADDR, "shim entry disjoint from exec segment");
		CHECK(TRAMP_SHIM_ENTRY == 0x211000u, "shim entry pinned at 0x211000");
	}

	/* ---- Test 2: the shim opcode round-trips through a BE-32 guest store ----
	 * Exactly the path the T3 seam uses: WriteMacInt32(TRAMP_SHIM_ENTRY, opcode)
	 * then execute_sheep() decodes the big-endian guest word. */
	{
		const uint32_t sels[] = { 0u, 1u, 0x3fu };  /* any in-range selector */
		for (unsigned k = 0; k < sizeof(sels) / sizeof(sels[0]); k++) {
			uint32_t op = tramp_ofci_encode_native_op(sels[k]);
			uint8_t word[4];
			store_be32(word, op);                 /* == WriteMacInt32 */
			uint32_t read_back = load_be32(word);  /* == the guest BE-32 fetch */
			CHECK(read_back == op, "opcode survives the BE-32 guest store/fetch");
			uint32_t dsel = 0; int dfn = 0;
			CHECK(tramp_ofci_decode_native_op(read_back, &dsel, &dfn) == 1,
			      "stored opcode decodes as a SHEEP EXEC_NATIVE op");
			CHECK(dsel == (sels[k] & 0x3fu), "decode round-trips the selector");
			CHECK(dfn == 1, "decode reports FN=1 (return via LR after the marshal)");
		}
	}

	/* ---- Test 3: the pinned CHRP entry ABI --------------------------------- */
	{
		CHECK(TRAMP_LAUNCH_R2 == 0x1001e8u, "r2 pinned to 0x1001e8 (TOC/SDA base)");
		CHECK(TRAMP_ENTRY == 0x20f078u, "entry pinned to 0x20f078 (PIC self-reloc stub)");
		/* the forge entry the seam bypasses is ROMBase+0x310000; the launch entry
		 * is the ELF entry, NOT the NanoKernel — Stop-rule #3 (do not guess NK). */
		CHECK(TRAMP_ENTRY != 0x310000u, "launch entry is the ELF entry, not NanoKernelEntry");
	}

	fprintf(stderr, "test_trampoline_launch: %d checks, %d failures\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
