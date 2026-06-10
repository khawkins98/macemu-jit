/*  a64_mmio_decode.cpp - see header. Port of spikes/s2-mach-fault-decode/main.cpp:87-144. */

#include "a64_mmio_decode.h"

bool A64DecodeMMIOAccess(uint32_t insn, A64MemAccess *out)
{
	// Strip the size field (bits 31:30); remaining fixed bits must match the
	// register-offset UXTW load/store form (mask 0xFFE0FC00 from S2).
	uint32_t key = insn & (0xFFE0FC00u & ~0xC0000000u);   // = insn & 0x3FE0FC00
	bool is_load;
	if (key == 0x38604800u)      is_load = true;
	else if (key == 0x38204800u) is_load = false;
	else return false;
	out->is_load   = is_load;
	out->size_log2 = insn >> 30;
	out->rt = insn & 0x1F;
	out->rn = (insn >> 5) & 0x1F;
	out->rm = (insn >> 16) & 0x1F;
	return true;
}

bool A64IsPairedSwap(uint32_t insn, unsigned size_log2, unsigned rt)
{
	uint32_t want;
	switch (size_log2) {
	case 1:  want = 0x5AC00400u; break;   // REV16 Wd,Wn
	case 2:  want = 0x5AC00800u; break;   // REV   Wd,Wn
	case 3:  want = 0xDAC00C00u; break;   // REV   Xd,Xn
	default: return false;                 // byte accesses have no paired swap
	}
	return insn == (want | (rt << 5) | rt);
}

uint64_t A64SwapForWidth(uint64_t v, unsigned size_log2)
{
	switch (size_log2) {
	case 0: return v & 0xFF;
	case 1: return __builtin_bswap16((uint16_t)v);
	case 2: return __builtin_bswap32((uint32_t)v);
	default: return __builtin_bswap64(v);
	}
}
