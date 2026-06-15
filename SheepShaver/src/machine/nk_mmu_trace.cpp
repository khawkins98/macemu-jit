/*
 *  nk_mmu_trace.cpp - NanoKernel MMU-write oracle recorder (Operation NewSheep, SS_M18 Track A)
 *
 *  See nk_mmu_trace.h. The recorder is gated entirely on the SS_M18_NK_TRACE env var:
 *  when unset every entry point is a cheap early-out, so a default (gate-OFF) boot is
 *  byte-identical to baseline. When set, the live mtsr/mtsrin/mt{i,d}bat/SDR1 (and
 *  sc/EXT) writes intercepted in ppc-execute.cpp are appended, in order, to the file.
 */

#include "nk_mmu_trace.h"

#include <stdio.h>
#include <stdlib.h>

static const char *const kKindNames[NK_MMU_KIND_COUNT] = {
	"MTSR", "MTSRIN", "MTIBATU", "MTIBATL", "MTDBATU", "MTDBATL", "MTSDR1", "SC", "EXT"
};

const char *nk_mmu_trace_kind_name(int kind)
{
	if (kind < 0 || kind >= NK_MMU_KIND_COUNT)
		return "?";
	return kKindNames[kind];
}

int nk_mmu_classify_bat_spr(uint32_t spr, int *out_kind, uint32_t *out_idx)
{
	/* IBAT0U..IBAT3L = 528..535 ; DBAT0U..DBAT3L = 536..543. Within each block the
	 * pairs are interleaved U,L (even=upper, odd=lower). */
	if (spr < 528 || spr > 543)
		return 0;
	uint32_t off = spr - 528;       /* 0..15 */
	int is_data = (off >= 8);       /* 8..15 -> DBAT */
	uint32_t within = off & 7;      /* 0..7 within the I/D block */
	uint32_t pair = within >> 1;    /* 0..3 */
	int is_lower = (within & 1);
	int kind;
	if (is_data)
		kind = is_lower ? NK_MMU_MTDBATL : NK_MMU_MTDBATU;
	else
		kind = is_lower ? NK_MMU_MTIBATL : NK_MMU_MTIBATU;
	if (out_kind) *out_kind = kind;
	if (out_idx)  *out_idx  = pair;
	return 1;
}

size_t nk_mmu_trace_format(char *buf, size_t buflen, uint32_t seq, int kind,
                           uint32_t pc, uint32_t insn, uint32_t idx, uint32_t val)
{
	int n = snprintf(buf, buflen, "%u %s pc=%08x insn=%08x idx=%u val=%08x\n",
	                 seq, nk_mmu_trace_kind_name(kind), pc, insn, idx, val);
	/* snprintf returns the would-have-written length, which can exceed buflen on
	 * truncation. Clamp to the actual bytes in buf so callers' fwrite(buf,1,n) never
	 * reads past the buffer. */
	if (n < 0) return 0;
	if (buflen && (size_t)n >= buflen) return buflen - 1;
	return (size_t)n;
}

/* --- runtime recorder (env-gated, single trace file, append-ordered) --- */

/* Tri-state: -1 = not yet probed, 0 = disabled, 1 = enabled. */
static int   g_trace_state = -1;
static FILE *g_trace_file  = NULL;
static uint32_t g_trace_seq = 0;

int nk_mmu_trace_enabled(void)
{
	if (g_trace_state < 0) {
		const char *path = getenv("SS_M18_NK_TRACE");
		if (path && path[0]) {
			g_trace_file = fopen(path, "w");
			if (g_trace_file) {
				setvbuf(g_trace_file, NULL, _IOLBF, 0);   /* line-buffered: survive abort() */
				g_trace_state = 1;
				fprintf(stderr, "[NK-TRACE] recording NanoKernel MMU writes to %s\n", path);
			} else {
				fprintf(stderr, "[NK-TRACE] could not open SS_M18_NK_TRACE=%s — recorder OFF\n", path);
				g_trace_state = 0;
			}
		} else {
			g_trace_state = 0;
		}
	}
	return g_trace_state;
}

void nk_mmu_trace_record(int kind, uint32_t pc, uint32_t insn, uint32_t idx, uint32_t val)
{
	if (!nk_mmu_trace_enabled())
		return;
	char line[96];
	size_t n = nk_mmu_trace_format(line, sizeof(line), g_trace_seq++, kind, pc, insn, idx, val);
	fwrite(line, 1, n, g_trace_file);
	/* Also echo to stderr (logarithmically) so a boot's terminal shows the harvest is live. */
	if (g_trace_seq <= 32 || (g_trace_seq & (g_trace_seq - 1)) == 0)
		fprintf(stderr, "[NK-TRACE] #%u %s", g_trace_seq - 1, line);
}
