/*
 *  test_logical_imm.c — exhaustive validation of a64_encode_logical_imm()
 *
 *  Three independent oracles, all must agree:
 *    1. ARM ARM DecodeBitMasks() reimplemented here (structurally distinct
 *       from the encoder — reconstructs the mask FROM N:immr:imms) for a
 *       round-trip check.
 *    2. The clang integrated assembler: `and w0,w1,#<mask>` is assembled and
 *       the bits[22:10] compared to our encoder. Unencodable masks make the
 *       assembler error, confirming rejection.
 *    3. Known-value rejection table (0, all-ones).
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/kpx_cpu/src/cpu/jit/aarch64/ppc-logical-imm.hpp"

static int failures = 0;

/* ---- Oracle 1: ARM ARM DecodeBitMasks (independent reconstruction) ----
 * Given the positioned encoding (N<<22)|(immr<<16)|(imms<<10) and a width,
 * reconstruct the immediate value. Returns false if the field is illegal
 * (matches hardware's UNDEFINED cases). This is the inverse direction from
 * the encoder, written from the published pseudocode, NOT as encoder^-1. */
static bool decode_bitmasks(uint32_t enc, unsigned width, uint64_t *out)
{
	unsigned N    = (enc >> 22) & 1;
	unsigned immr = (enc >> 16) & 0x3F;
	unsigned imms = (enc >> 10) & 0x3F;

	/* len = highest set bit of (N:~imms<6:0>); element size = 1<<len. */
	unsigned bits6 = (N << 6) | ((~imms) & 0x3F);
	int len = -1;
	for (int b = 6; b >= 0; b--) {
		if (bits6 & (1u << b)) { len = b; break; }
	}
	if (len < 1) return false;          /* reserved / undefined */
	unsigned esize = 1u << len;
	if (esize > width) return false;    /* N=1 illegal for 32-bit */

	unsigned levels = esize - 1;        /* len ones */
	unsigned S = imms & levels;
	unsigned R = immr & levels;
	if (S == levels) return false;      /* all-ones element: UNDEFINED */

	/* welem = (1<<(S+1)) - 1  : S+1 contiguous ones, bottom justified. */
	uint64_t welem = (S + 1 >= 64) ? ~0ULL : ((1ULL << (S + 1)) - 1);
	/* ROR welem right by R within esize. */
	uint64_t emask = (esize >= 64) ? ~0ULL : ((1ULL << esize) - 1);
	uint64_t rotated = ((welem >> R) | (welem << (esize - R))) & emask;

	/* Replicate the esize-bit element across `width` bits. */
	uint64_t result = 0;
	for (unsigned pos = 0; pos < width; pos += esize)
		result |= rotated << pos;
	*out = (width == 64) ? result : (result & 0xFFFFFFFFULL);
	return true;
}

/* ---- Oracle 2: clang assembler. Batch all encodable masks, assemble, and
 * read bits[22:10] of each `and w0,w1,#imm`. ---- */
static void clang_assembler_crosscheck(const uint32_t *masks,
                                       const uint32_t *encs, int count)
{
	const char *asmf = "/tmp/li_oracle.s";
	const char *objf = "/tmp/li_oracle.o";
	FILE *f = fopen(asmf, "w");
	if (!f) { perror("fopen asm"); failures++; return; }
	fprintf(f, "\t.text\n\t.globl _t\n_t:\n");
	for (int i = 0; i < count; i++)
		fprintf(f, "\tand w0, w1, #0x%x\n", masks[i]);
	fclose(f);

	char cmd[256];
	snprintf(cmd, sizeof cmd,
	         "clang -target arm64-apple-macos -c %s -o %s 2>/tmp/li_oracle.err",
	         asmf, objf);
	if (system(cmd) != 0) {
		printf("  [oracle2] clang failed to assemble encodable masks:\n");
		system("cat /tmp/li_oracle.err");
		failures++;
		return;
	}

	/* Dump and grep the AND-immediate words. objdump prints raw bytes; we
	 * read them back via otool/objdump text. Simpler: re-disassemble and
	 * extract encoding via `objdump -d --no-show-raw-insn` is lossy, so we
	 * read the .text section bytes directly with otool -t. */
	FILE *p = popen("otool -t /tmp/li_oracle.o 2>/dev/null | "
	                "awk 'NR>1{for(i=2;i<=NF;i++)print $i}'", "r");
	if (!p) { perror("popen otool"); failures++; return; }
	uint32_t words[2048]; int n = 0;
	char tok[32];
	while (n < count && fscanf(p, "%31s", tok) == 1) {
		/* otool -t prints little-endian 32-bit words as 8 hex digits. */
		if (strlen(tok) == 8)
			words[n++] = (uint32_t)strtoul(tok, NULL, 16);
	}
	pclose(p);
	if (n != count) {
		printf("  [oracle2] read %d words, expected %d (otool parse)\n", n, count);
		failures++;
		return;
	}
	int mism = 0;
	for (int i = 0; i < count; i++) {
		uint32_t field = words[i] & 0x007FFC00; /* N:immr:imms positioned */
		if (field != encs[i]) {
			if (mism < 5)
				printf("  [oracle2] mask 0x%08x: clang field 0x%06x != ours 0x%06x\n",
				       masks[i], field, encs[i]);
			mism++;
		}
	}
	if (mism) { failures++; printf("  [oracle2] %d mismatches\n", mism); }
	else printf("  [oracle2] clang assembler agrees on all %d encodable masks\n", count);
}

/* Confirm clang REJECTS a value as a logical immediate. */
static bool clang_rejects(uint32_t value)
{
	FILE *f = fopen("/tmp/li_rej.s", "w");
	fprintf(f, "\t.text\n_t:\n\tand w0, w1, #0x%x\n", value);
	fclose(f);
	int rc = system("clang -target arm64-apple-macos -c /tmp/li_rej.s "
	                "-o /tmp/li_rej.o 2>/dev/null");
	return rc != 0;
}

int main(void)
{
	printf("== a64_encode_logical_imm exhaustive PPC-mask test ==\n");

	uint32_t enc_masks[1024];
	uint32_t enc_vals[1024];
	int encodable = 0;
	int rt_fail = 0;

	for (int mb = 0; mb < 32; mb++) {
		for (int me = 0; me < 32; me++) {
			uint32_t mask = ppc_mask(mb, me);
			uint32_t enc;
			bool ok = a64_encode_logical_imm(mask, false, &enc);
			if (!ok) continue;
			/* Oracle 1: round-trip via independent decoder. */
			uint64_t back;
			if (!decode_bitmasks(enc, 32, &back) ||
			    (uint32_t)back != mask) {
				if (rt_fail < 5)
					printf("  [oracle1] mb=%d me=%d mask=0x%08x enc=0x%06x "
					       "decoded=0x%08x\n", mb, me, mask, enc,
					       (uint32_t)back);
				rt_fail++;
			}
			enc_masks[encodable] = mask;
			enc_vals[encodable]  = enc;
			encodable++;
		}
	}

	printf("PPC masks encodable: %d / 1024\n", encodable);
	printf("Round-trip (oracle1) failures: %d\n", rt_fail);
	if (rt_fail) failures++;

	/* Concrete gate: expect exactly 992 encodable (32 of 1024 produce
	 * 0xFFFFFFFF: mb=0,me=31 plus the 31 wrap pairs me==mb-1). */
	if (encodable != 992) {
		printf("FAIL: expected exactly 992 encodable PPC masks, got %d\n",
		       encodable);
		failures++;
	} else {
		printf("OK: 992 encodable as predicted (32 all-ones masks rejected)\n");
	}

	/* Oracle 2: clang assembler cross-check on all encodable masks. */
	clang_assembler_crosscheck(enc_masks, enc_vals, encodable);

	/* Oracle 3: known unencodable values must return false AND clang agrees. */
	struct { uint64_t v; const char *name; } rej[] = {
		{ 0x00000000ULL, "zero" },
		{ 0xFFFFFFFFULL, "all-ones-32" },
	};
	for (size_t i = 0; i < sizeof(rej)/sizeof(rej[0]); i++) {
		uint32_t e;
		bool ours = a64_encode_logical_imm(rej[i].v, false, &e);
		bool clang_rej = clang_rejects((uint32_t)rej[i].v);
		if (ours) {
			printf("FAIL: %s (0x%llx) wrongly encodable by us\n",
			       rej[i].name, (unsigned long long)rej[i].v);
			failures++;
		} else if (!clang_rej) {
			printf("WARN: %s rejected by us but clang accepted?\n", rej[i].name);
		} else {
			printf("OK: %s rejected by both encoder and clang\n", rej[i].name);
		}
	}

	/* Spot-check a few known-encodable non-PPC values for breadth. */
	struct { uint64_t v; bool exp; const char *name; } spot[] = {
		{ 0x0000000FULL, true,  "0xF (4-bit run)" },
		{ 0xFFFFFFF8ULL, true,  "0xFFFFFFF8 (rotated run)" },
		{ 0x80000001ULL, true,  "0x80000001 (wrap run)" },
		{ 0x55555555ULL, true,  "0x55555555 (period-2)" },
		{ 0x00010001ULL, true,  "0x00010001 (period-16)" },
		{ 0x00000007ULL, true,  "0x7" },
		{ 0x000000F0ULL, true,  "0xF0" },
		{ 0x12345678ULL, false, "0x12345678 (arbitrary)" },
	};
	for (size_t i = 0; i < sizeof(spot)/sizeof(spot[0]); i++) {
		uint32_t e;
		bool ours = a64_encode_logical_imm(spot[i].v, false, &e);
		bool clang_ok = !clang_rejects((uint32_t)spot[i].v);
		if (ours != spot[i].exp) {
			printf("FAIL: %s expected encodable=%d got %d\n",
			       spot[i].name, spot[i].exp, ours);
			failures++;
		} else if (ours != clang_ok) {
			printf("FAIL: %s ours=%d disagrees with clang=%d\n",
			       spot[i].name, ours, clang_ok);
			failures++;
		} else {
			printf("OK: %s encodable=%d (clang agrees)\n", spot[i].name, ours);
		}
	}

	printf("\n%s (%d failure group(s))\n",
	       failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
	return failures ? 1 : 0;
}
