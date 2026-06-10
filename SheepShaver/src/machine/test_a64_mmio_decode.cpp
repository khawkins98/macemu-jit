/* Contract test: every form ppc-codegen-aarch64.h:150-181 emits must decode; close
 * neighbors must NOT. S2 gotchas (rt==31, width keying) asserted here. */
#include "a64_mmio_decode.h"
#include <assert.h>
#include <stdio.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

// Verbatim encoding constants from ppc-codegen-aarch64.h:150-181:
static uint32_t enc(uint32_t baseop, unsigned rt, unsigned rn, unsigned rm)
{ return baseop | (rm << 16) | (rn << 5) | rt; }

int main()
{
	static const struct { uint32_t op; bool load; unsigned sz; } forms[] = {
		{ 0xB8604800, true,  2 },  // a64_ldr_w_reg
		{ 0xB8204800, false, 2 },  // a64_str_w_reg
		{ 0xF8604800, true,  3 },  // a64_ldr_x_reg
		{ 0xF8204800, false, 3 },  // a64_str_x_reg
		{ 0x38604800, true,  0 },  // a64_ldrb_reg
		{ 0x38204800, false, 0 },  // a64_strb_reg
		{ 0x78604800, true,  1 },  // a64_ldrh_reg
		{ 0x78204800, false, 1 },  // a64_strh_reg
	};
	A64MemAccess a;
	for (unsigned i = 0; i < 8; i++) {
		// JIT-typical operands: rt=1 (RTMP1), rn=19 (RMEMBASE), rm=0 (RTMP0)
		CHECK(A64DecodeMMIOAccess(enc(forms[i].op, 1, 19, 0), &a));
		CHECK(a.is_load == forms[i].load && a.size_log2 == forms[i].sz);
		CHECK(a.rt == 1 && a.rn == 19 && a.rm == 0);
		// rt==31 (WZR/XZR) still decodes — caller discards the value (S2 §4.3)
		CHECK(A64DecodeMMIOAccess(enc(forms[i].op, 31, 19, 0), &a) && a.rt == 31);
		// arbitrary allocated source reg for byte stores (hS varies)
		CHECK(A64DecodeMMIOAccess(enc(forms[i].op, 27, 19, 0), &a) && a.rt == 27);
		// rm==31 extracts as 31 (offset register field independent of rt/rn)
		CHECK(A64DecodeMMIOAccess(enc(forms[i].op, 1, 19, 31), &a) && a.rm == 31);
	}

	// Non-matches: REV, REV16, LSL-extend variant (option=011, the old UXTX bug),
	// immediate-offset LDR, BL, NOP.
	CHECK(!A64DecodeMMIOAccess(0x5AC00800 | (1 << 5) | 1, &a));  // REV W1,W1
	CHECK(!A64DecodeMMIOAccess(0x5AC00400 | (1 << 5) | 1, &a));  // REV16 W1,W1
	CHECK(!A64DecodeMMIOAccess(0xB8606800 | (19 << 5) | 1, &a)); // LDR w, [x,x,LSL] opt=011
	CHECK(!A64DecodeMMIOAccess(0xB9400000, &a));                  // LDR w, [x, #imm]
	CHECK(!A64DecodeMMIOAccess(0x94000001, &a));                  // BL
	CHECK(!A64DecodeMMIOAccess(0xD503201F, &a));                  // NOP

	// Paired-swap recognition (S2 §4.3: width-keyed; byte has none).
	CHECK(A64IsPairedSwap(0x5AC00800 | (1 << 5) | 1, 2, 1));      // REV W1
	CHECK(A64IsPairedSwap(0x5AC00400 | (1 << 5) | 1, 1, 1));      // REV16 W1
	CHECK(A64IsPairedSwap(0xDAC00C00 | (1 << 5) | 1, 3, 1));      // REV X1
	CHECK(!A64IsPairedSwap(0x5AC00800 | (1 << 5) | 1, 1, 1));     // wrong width
	CHECK(!A64IsPairedSwap(0x5AC00800 | (2 << 5) | 2, 2, 1));     // wrong reg
	CHECK(!A64IsPairedSwap(0x5AC00800 | (1 << 5) | 1, 0, 1));     // byte: never

	// Swap helper: width-keyed (byte = identity).
	CHECK(A64SwapForWidth(0x12, 0) == 0x12);
	CHECK(A64SwapForWidth(0x1234, 1) == 0x3412);
	CHECK(A64SwapForWidth(0x12345678u, 2) == 0x78563412u);
	CHECK(A64SwapForWidth(0x0102030405060708ull, 3) == 0x0807060504030201ull);

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
