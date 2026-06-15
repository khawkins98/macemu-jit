/*
 *  nk_mmu_trace.h - NanoKernel MMU-write oracle recorder (Operation NewSheep, SS_M18 Track A)
 *
 *  Records, when SS_M18_NK_TRACE=<path> is set, the ORDERED sequence of the real
 *  NanoKernel's MMU-programming writes (mtsr / mtsrin / mt{i,d}bat{u,l} / mtspr SDR1)
 *  plus sc/EXT supervisor events, with {seq, kind, pc, insn, idx, value}. This is the
 *  S1/S3 OFFLINE ORACLE FIXTURE: the live (SR/BAT/SDR1) map the shadow-arena MMU must
 *  reproduce, harvested directly from the producer instead of forged.
 *
 *  The recorder is INERT unless the env var is set (gate-OFF byte-identical). The
 *  format/classification logic (nk_mmu_trace_format, nk_mmu_classify_spr) is pure and
 *  standalone-unit-testable (test_nk_mmu_trace.cpp), independent of the emulator.
 */

#ifndef NK_MMU_TRACE_H
#define NK_MMU_TRACE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event kinds, in the order the NK programs them (informational; not load-bearing). */
enum NkMmuEventKind {
	NK_MMU_MTSR = 0,	/* mtsr   SR[idx] = val                                  */
	NK_MMU_MTSRIN,		/* mtsrin SR[idx] = val   (idx = rB>>28)                 */
	NK_MMU_MTIBATU,		/* mtspr  IBATidxU = val                                 */
	NK_MMU_MTIBATL,		/* mtspr  IBATidxL = val                                 */
	NK_MMU_MTDBATU,		/* mtspr  DBATidxU = val                                 */
	NK_MMU_MTDBATL,		/* mtspr  DBATidxL = val                                 */
	NK_MMU_MTSDR1,		/* mtspr  SDR1 = val      (idx = 0)                      */
	NK_MMU_SC,			/* sc                     (idx = 0, val = gpr0 selector) */
	NK_MMU_EXT,			/* external-interrupt EXT vector entry (idx, val = vec)  */
	NK_MMU_KIND_COUNT
};

/* Canonical short name for a kind (or "?" if out of range). Pure. */
const char *nk_mmu_trace_kind_name(int kind);

/* Classify an mtspr SPR number that targets a BAT register into an NkMmuEventKind and
 * a 0..3 pair index. Returns 1 and writes out_kind/out_idx if spr is an I/D BAT
 * (528..543); returns 0 otherwise (caller handles SDR1 / non-BAT separately). Pure. */
int nk_mmu_classify_bat_spr(uint32_t spr, int *out_kind, uint32_t *out_idx);

/* Format one ordered event line into buf. Returns the number of bytes that WOULD be
 * written (excluding the NUL), like snprintf. Stable, greppable, single-line:
 *   "<seq> <KIND> pc=<pc8> insn=<insn8> idx=<idx> val=<val8>\n"
 * Pure — no globals, no I/O — so it is exercised directly by the unit test. */
size_t nk_mmu_trace_format(char *buf, size_t buflen, uint32_t seq, int kind,
                           uint32_t pc, uint32_t insn, uint32_t idx, uint32_t val);

/* Runtime recorder. nk_mmu_trace_enabled() reads SS_M18_NK_TRACE once (caching the
 * result); when unset the recorder is a no-op and emits nothing (gate-OFF byte-
 * identical). nk_mmu_trace_record() appends one formatted line to the trace file. */
int  nk_mmu_trace_enabled(void);
void nk_mmu_trace_record(int kind, uint32_t pc, uint32_t insn, uint32_t idx, uint32_t val);

#ifdef __cplusplus
}
#endif

#endif /* NK_MMU_TRACE_H */
