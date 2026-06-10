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

/* Raw replay of a backpatched site's original access against guest memory.
 *
 * Endianness reasoning (the double-negative, spelled out):
 *  - Guest memory holds PPC BIG-ENDIAN bytes. AArch64 data accesses are
 *    little-endian, so the JIT pairs each width>1 LDR with a REV (raw -> arch)
 *    and each width>1 STR with a REV before it (arch -> raw).
 *  - LOAD: at patch time the paired REV after the LDR was NOPed, so the thunk
 *    must deliver the ARCHITECTURAL value to frame[rt]. Assembling the memory
 *    bytes MSB-first (a big-endian read) yields exactly REV(little-endian LDR)
 *    == the architectural value — for every width, including bytes where the
 *    two views coincide. Written as a full 64-bit slot store: the real LDR Wt
 *    zero-extends into Xt and the thunk restores the whole X register from the
 *    slot. rt==31 is WZR/XZR: discard.
 *  - STORE: the JIT's REV ran BEFORE the BL, so frame[rt] already holds the
 *    RAW byte-reversed register image (and for bytes, where no REV exists,
 *    raw == architectural anyway). The original STR would write its low
 *    1<<size_log2 register bytes to memory in little-endian register order:
 *    mem[i] = (reg >> 8*i) & 0xFF. Replicating that loop verbatim lands the
 *    architectural value big-endian in guest memory — byte-identical to what
 *    the unpatched STR produced. (Host-endianness independent by construction;
 *    deliberately not a memcpy.) Bytes beyond the access width are untouched. */
void mmio_thunk_raw_access(uint64_t *frame, const A64MemAccess *acc,
                           uint8_t *host_base)
{
	uint32_t gaddr = (uint32_t)frame[acc->rm];
	uint8_t *p = host_base + gaddr;
	unsigned bytes = 1u << acc->size_log2;
	if (acc->is_load) {
		uint64_t arch = 0;
		for (unsigned i = 0; i < bytes; i++)
			arch = (arch << 8) | p[i];            // big-endian assembly == post-REV value
		if (acc->rt != 31)
			frame[acc->rt] = arch;                // zero-extended, full slot (LDR Wt -> Xt)
	} else {
		uint64_t raw = (acc->rt == 31) ? 0 : frame[acc->rt];
		for (unsigned i = 0; i < bytes; i++)
			p[i] = (uint8_t)(raw >> (8 * i));     // STR's little-endian byte order
	}
}
