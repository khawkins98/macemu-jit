/*
 *  a64_mmio_decode.h - decoder for the JIT's guest-memory access forms (SPIKE-S2 §4.1).
 *  The JIT emits exactly one form family: LDR/STR{B,H,,X} Rt, [Xn, Wm, UXTW]
 *  ({0x38,0x78,0xB8,0xF8} {60=load,20=store} 4800, fixed-bits mask 0xFFE0FC00) —
 *  see ppc-codegen-aarch64.h:150-181. Anything else is NOT a JIT guest access.
 */

#ifndef A64_MMIO_DECODE_H
#define A64_MMIO_DECODE_H

#include <stdint.h>

struct A64MemAccess {
	bool     is_load;
	unsigned size_log2;   // 0=B 1=H 2=W 3=X
	unsigned rt, rn, rm;
};

// Returns true iff insn is one of the 8 JIT-emitted register-offset UXTW forms.
extern bool A64DecodeMMIOAccess(uint32_t insn, A64MemAccess *out);

// Returns true iff insn is the REV/REV16 the JIT pairs with a width-size_log2
// access on register rt (REV16 W for H, REV W for W, REV X for X). Byte accesses
// have no swap insn; callers must not ask (size_log2==0 returns false).
// Matches only source==dest swaps (the load-path form); store-side REVs use a
// distinct source register and are never verified through this function.
extern bool A64IsPairedSwap(uint32_t insn, unsigned size_log2, unsigned rt);

// Byte-swap an architectural value to/from raw memory order for a given width.
extern uint64_t A64SwapForWidth(uint64_t v, unsigned size_log2);

#endif
