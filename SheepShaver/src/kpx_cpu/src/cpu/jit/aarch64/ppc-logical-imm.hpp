/*
 *  ppc-logical-imm.hpp — ARM64 logical (bitmask) immediate encoder
 *
 *  Header-only helpers for the PPC → AArch64 JIT. Provides:
 *    - a64_encode_logical_imm(): encode a 32/64-bit value as the ARM64
 *      logical-immediate N:immr:imms triple, pre-positioned for OR-ing into
 *      the AND/ORR/EOR/TST (immediate) instruction word.
 *    - ppc_mask(): the PPC MB..ME contiguous (and wrap-around) mask used by
 *      rlwinm/rlwimi/rlwnm.
 *
 *  The encoding algorithm is the public ARM Architecture Reference Manual
 *  routine (the inverse of the AArch64 `DecodeBitMasks` pseudocode). It is
 *  the same well-known routine implemented by the Dolphin Emulator Project
 *  (Source/Core/Common/Arm64Emitter.h LogicalImm) and by oaknut; those were
 *  cross-referenced for correctness. This file is an independent
 *  implementation of the published ARM algorithm re-typed to this JIT's
 *  plain-`uint32_t`-register convention; no third-party source was copied.
 *
 *  Copyright (C) 2026 the macemu-jit contributors.
 *  Algorithm credit: ARM Ltd. (ARM ARM, DecodeBitMasks); cross-referenced
 *  against the Dolphin Emulator Project (GPL-2.0-or-later) and oaknut (MIT).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PPC_LOGICAL_IMM_HPP
#define PPC_LOGICAL_IMM_HPP

#include <stdint.h>
#include <stdbool.h>

/* PPC rlwinm/rlwimi/rlwnm 32-bit mask generator.
 *
 * Sets bits MB..ME inclusive, counting from the MSB (PPC bit 0 = 0x80000000).
 * When mb <= me the run is contiguous; when mb > me the mask wraps around
 * (the PPC mask is the union of [0..me] and [mb..31]). Matches the open-coded
 * loop currently inline in ppc-jit.cpp's rlwinm/rlwimi/rlwnm cases. */
static inline uint32_t ppc_mask(int mb, int me)
{
	uint32_t mask = 0;
	if (mb <= me) {
		for (int i = mb; i <= me; i++)
			mask |= (0x80000000U >> i);
	} else {
		for (int i = 0; i <= me; i++)
			mask |= (0x80000000U >> i);
		for (int i = mb; i <= 31; i++)
			mask |= (0x80000000U >> i);
	}
	return mask;
}

/* Encode `value` as an ARM64 logical (bitmask) immediate.
 *
 * On success, writes the N:immr:imms field, ALREADY POSITIONED in the
 * instruction word as (N << 22) | (immr << 16) | (imms << 10), into
 * *out_n_immr_imms and returns true. The caller ORs this directly into the
 * relevant opcode base, e.g.:
 *     AND  Wd,Wn,#imm  : 0x12000000 | (enc) | (Rn << 5) | Rd
 *     ORR  Wd,Wn,#imm  : 0x32000000 | (enc) | (Rn << 5) | Rd
 *     EOR  Wd,Wn,#imm  : 0x52000000 | (enc) | (Rn << 5) | Rd
 *     ANDS Wd,Wn,#imm  : 0x72000000 | (enc) | (Rn << 5) | Rd
 * (X-register forms set bit 31; for those the N field is meaningful, for
 * 32-bit forms N must be 0 and is forced so here.)
 *
 * Returns false if `value` is not representable as a logical immediate
 * (all-zeros and all-ones are never representable; for is_64bit==false the
 * value must also fit and replicate within 32 bits).
 *
 * is_64bit selects the 64-bit (sf=1) encoding domain; false selects 32-bit,
 * in which case only the low 32 bits of `value` are considered and N is 0. */
static inline bool a64_encode_logical_imm(uint64_t value, bool is_64bit,
                                          uint32_t *out_n_immr_imms)
{
	/* For the 32-bit domain, the hardware replicates the low 32 bits across
	 * the 64-bit datapath. Mirror that: work on a value duplicated into both
	 * halves so the element-size search below sees the true period. */
	if (!is_64bit) {
		value &= 0xFFFFFFFFULL;
		value |= value << 32;
	}

	/* All-zeros and all-ones have no logical-immediate encoding. */
	if (value == 0 || value == ~0ULL)
		return false;

	/* Determine the element size: the smallest power-of-two field (2..64)
	 * that the 64-bit value is a periodic replication of. */
	unsigned int size = 64;
	while (size > 2) {
		unsigned int half = size / 2;
		uint64_t mask = (1ULL << half) - 1;     /* half is <=32 here */
		if ((value & mask) != ((value >> half) & mask))
			break;          /* period is `size`, not smaller */
		size = half;
	}

	/* Reduce to one element of the chosen size. */
	uint64_t elt_mask = (size == 64) ? ~0ULL : ((1ULL << size) - 1);
	uint64_t elt = value & elt_mask;

	/* Count the run of ones (population) and the trailing zeros. A valid
	 * bitmask element is a single contiguous run of `ones` ones, rotated.
	 * Detect by: rotate the element right until bit 0 is 1 and bit (ones-1)
	 * is the top of a contiguous bottom-justified run. The canonical trick:
	 *   - ones  = popcount(elt)
	 *   - the element is a rotated contiguous run iff, after rotating so the
	 *     run is bottom-justified, elt == (1<<ones)-1.
	 * Find the rotation: if bit 0 is 0, the run is already not wrapping —
	 * shift out trailing zeros. If bit 0 is 1, the run may wrap the top;
	 * rotate down by the count of low ones so the wrap rejoins at the bottom. */
	unsigned int ones = 0;
	for (unsigned int i = 0; i < size; i++)
		if (elt & (1ULL << i)) ones++;
	if (ones == 0 || ones == size)
		return false;       /* empty or full element: not encodable */

	unsigned int rotate;    /* right-rotate applied to bottom-justified run */
	int wraps = (elt & 1ULL) && (elt & (1ULL << (size - 1)));
	if (wraps) {
		/* Run wraps the top of the element (both bit 0 and bit size-1 set):
		 * count contiguous low ones and rotate them off the bottom so the
		 * run rejoins, becoming bottom-justified. */
		unsigned int low_ones = 0;
		while (low_ones < size && (elt & (1ULL << low_ones)))
			low_ones++;
		/* Rotate the value RIGHT by low_ones: the contiguous wrapped run
		 * becomes the TOP `ones` bits (top-justified, no wrap). */
		uint64_t rot = ((elt >> low_ones) | (elt << (size - low_ones))) & elt_mask;
		/* Now bring it down to bit 0 by shifting out the trailing zeros. */
		unsigned int tz = 0;
		while (tz < size && !(rot & (1ULL << tz)))
			tz++;
		elt = rot >> tz;
		/* Total right-rotation applied to original = low_ones + tz. The
		 * encoding immr reproduces by left-rotating, = size - (low_ones+tz). */
		rotate = (size - ((low_ones + tz) & (size - 1))) & (size - 1);
	} else {
		unsigned int tz = 0;
		while (tz < size && !(elt & (1ULL << tz)))
			tz++;
		elt >>= tz;
		/* Run sits `tz` bits up; reproduce by left-rotating the bottom-
		 * justified run by tz, i.e. immr = (size - tz) mod size. */
		rotate = (size - tz) & (size - 1);
	}

	/* Verify the normalized element is a bottom-justified contiguous run. */
	uint64_t run_mask = (ones >= 64) ? ~0ULL : ((1ULL << ones) - 1);
	if (elt != run_mask)
		return false;       /* not a single contiguous run */

	/* Pack per ARM ARM:
	 *   N    = (size == 64)
	 *   imms = (-(2*size) & 0x3F) low bits encode size, OR-ed with (ones-1).
	 *          For size==64 the size term is 0 (N carries it).
	 *   immr = rotate. */
	unsigned int n = (size == 64) ? 1 : 0;
	unsigned int size_term = (unsigned int)(-(int)(2 * size)) & 0x3F;
	unsigned int imms = size_term | (ones - 1);
	unsigned int immr = rotate & (size - 1);

	if (!is_64bit && n != 0)
		return false;       /* 32-bit form requires N==0 */

	*out_n_immr_imms = ((uint32_t)n << 22) |
	                   ((uint32_t)(immr & 0x3F) << 16) |
	                   ((uint32_t)(imms & 0x3F) << 10);
	return true;
}

#endif /* PPC_LOGICAL_IMM_HPP */
