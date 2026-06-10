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

	// ---- mmio_thunk_raw_access: non-MMIO-EA fallback semantics ----
	// Pins the endianness double-negative: loads deliver ARCHITECTURAL (the
	// paired REV was NOPed); stores receive RAW post-REV register images and
	// must land guest memory byte-identical to the original STR (LE register
	// byte order == architectural value big-endian in memory).
	{
		uint64_t frame[32];
		uint8_t mem[64];                      // fake guest memory; host_base = mem
		const unsigned RM = 0, RT = 5;        // EA reg, data reg
		A64MemAccess ra;
		ra.rn = 19; ra.rm = RM;

		// LOADS: guest-BE memory bytes -> architectural value in frame[rt],
		// zero-extended across the full 64-bit slot (stale bits cleared).
		struct { unsigned sz; uint8_t bytes[8]; uint64_t arch; } lcases[] = {
			{ 0, {0xAB},                                          0xAB },
			{ 1, {0x12, 0x34},                                    0x1234 },
			{ 2, {0x11, 0x22, 0x33, 0x44},                        0x11223344 },
			{ 3, {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}, 0x0102030405060708ull },
		};
		for (unsigned i = 0; i < 4; i++) {
			for (unsigned k = 0; k < sizeof mem; k++) mem[k] = 0xEE;
			for (unsigned k = 0; k < (1u << lcases[i].sz); k++) mem[8 + k] = lcases[i].bytes[k];
			frame[RM] = 8;                                  // guest EA
			frame[RT] = 0xFFFFFFFFFFFFFFFFull;              // stale high bits must vanish
			ra.is_load = true; ra.size_log2 = lcases[i].sz; ra.rt = RT;
			mmio_thunk_raw_access(frame, &ra, mem);
			CHECK(frame[RT] == lcases[i].arch);
		}
		// rt==31 (WZR/XZR): value discarded, frame untouched.
		frame[RM] = 8; frame[RT] = 0x5555;
		ra.is_load = true; ra.size_log2 = 2; ra.rt = 31;
		mmio_thunk_raw_access(frame, &ra, mem);
		CHECK(frame[RT] == 0x5555);

		// STORES: frame[rt] = raw post-REV image (with garbage above the access
		// width — must be ignored); memory must end up architectural-BE, with
		// neighboring sentinel bytes untouched.
		struct { unsigned sz; uint64_t raw; uint8_t bytes[8]; } scases[] = {
			{ 0, 0xFFFFFFFFFFFFFFABull, {0xAB} },                            // byte: no REV, arch==raw low byte
			{ 1, 0xFFFFFFFF00003412ull, {0x12, 0x34} },                      // arch 0x1234, REV16'd
			{ 2, 0xFFFFFFFF44332211ull, {0x11, 0x22, 0x33, 0x44} },          // arch 0x11223344, REV'd
			{ 3, 0x0807060504030201ull, {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08} }, // arch 0x01..08
		};
		for (unsigned i = 0; i < 4; i++) {
			unsigned n = 1u << scases[i].sz;
			for (unsigned k = 0; k < sizeof mem; k++) mem[k] = 0xEE;
			frame[RM] = 8; frame[RT] = scases[i].raw;
			ra.is_load = false; ra.size_log2 = scases[i].sz; ra.rt = RT;
			mmio_thunk_raw_access(frame, &ra, mem);
			for (unsigned k = 0; k < n; k++) CHECK(mem[8 + k] == scases[i].bytes[k]);
			CHECK(mem[7] == 0xEE && mem[8 + n] == 0xEE);    // width respected
		}
		// Store rt==31: STR WZR/XZR semantics — writes zeros.
		for (unsigned k = 0; k < sizeof mem; k++) mem[k] = 0xEE;
		frame[RM] = 8;
		ra.is_load = false; ra.size_log2 = 2; ra.rt = 31;
		mmio_thunk_raw_access(frame, &ra, mem);
		CHECK(mem[8] == 0 && mem[9] == 0 && mem[10] == 0 && mem[11] == 0 && mem[12] == 0xEE);

		// Round trip: store raw image then load it back -> architectural value.
		for (unsigned k = 0; k < sizeof mem; k++) mem[k] = 0xEE;
		frame[RM] = 16; frame[RT] = 0x78563412;             // raw of arch 0x12345678
		ra.is_load = false; ra.size_log2 = 2; ra.rt = RT;
		mmio_thunk_raw_access(frame, &ra, mem);
		frame[RT] = 0;
		ra.is_load = true;
		mmio_thunk_raw_access(frame, &ra, mem);
		CHECK(frame[RT] == 0x12345678);
	}

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
