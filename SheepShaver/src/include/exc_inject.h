/*
 *  exc_inject.h - SS_M18 S3 (Operation NewSheep) T2: the host->NK EXT-injection
 *                 shim (LOAD-BEARING).
 *
 *  Pure module (no globals, no emulator dependencies, stdint only) so the
 *  PLANTED-TABLE micro-test (test_exc_inject) can exercise the exact resolve +
 *  inject path the runtime uses.
 *
 *  Architecture (FINDINGS-s3-two-supervisor.md §★, BINDING): pure REPLACE for
 *  the synchronous NK<->host seams PLUS this permanent, bounded EXT-injection
 *  shim for the ASYNCHRONOUS seam (host device-IRQ -> NK EXT vector). Pure
 *  REPLACE would sever the only device-IRQ->CPU-EXT path when T3 retires
 *  deliver_pending_dec_exception() + g_exc_entry_table; this shim re-routes that
 *  exact async seam: poll the KEPT host-IRQ pending flag, resolve the NK's REAL
 *  EXT vector from the LIVE KDP table, inject via the KEPT ExcEnter() RE-POINTED
 *  at the NK vector (NOT g_exc_entry_table). PEM mask math (exc_core) is LAW and
 *  UNTOUCHED — this module only CONSUMES ExcEnter.
 */

#ifndef EXC_INJECT_H
#define EXC_INJECT_H

#include <stdint.h>
#include "exc_core.h"

/* KDP-relative offset of the live NK EXT vector slot. The runtime KDP vector
 * table base is KDP+0x360; the EXT (vector 0x500) slot is KDP+0x374 — the cell
 * that holds the NK's published external-interrupt handler (0x50314880 in the
 * 9.0.1 boot) once the install has run (plan rev-2 TECHNICAL MINOR; glue:141
 * "[KDP+0x374] = 0x50314880 [PROBE✓]"). T2 resolves from this slot — NEVER a
 * hardcoded constant (QEMU/hardcode-as-address-oracle = Stop-rule #6/#9). */
#define NK_KDP_EXT_VECTOR_OFFSET  0x374u

/* Caller-supplied 32-bit big-endian guest reader (the emulator passes a
 * ReadMacInt32 wrapper; the standalone test plants a synthetic table). ctx is
 * forwarded verbatim. */
typedef uint32_t (*ExcGuestRead32)(uint32_t guest_addr, void *ctx);

/* Resolve the NK's REAL EXT vector from the live KDP table by reading
 * [kdp_base + NK_KDP_EXT_VECTOR_OFFSET]. Returns the resolved vector, or 0 when
 * UNRESOLVED. The caller treats 0 as STOP-and-report: the install has not run
 * (the EXPECTED gated-ON state this milestone, pre-S2b — there is no loader yet
 * to install the KDP table). Sentinels mapped to 0 (Stop-rule #6): 0x00000000,
 * 0xDEADBEEF, 0xFFFFFFFF. */
uint32_t ExcResolveNkExtVector(uint32_t kdp_base, ExcGuestRead32 read32, void *ctx);

/* Build the host->NK EXT-injection transition. Resolves the EXT vector from the
 * live KDP table, builds a LOCAL one-field ExcEntryTable (external_entry =
 * resolved vector; g_exc_entry_table is DELIBERATELY NOT consulted — this module
 * never references it) and computes the transition via ExcEnter(EXC_EXTERNAL).
 *
 * *out_vector receives the resolved vector (0 on unresolved). On unresolved the
 * returned transition has pc == EXC_PC_UNRESOLVED (caller STOPs, does NOT
 * deliver). */
ExcTransition ExcInjectExternal(uint32_t restart_pc, uint32_t cur_msr,
                                uint32_t kdp_base, ExcGuestRead32 read32, void *ctx,
                                uint32_t *out_vector);

/* SS_M18 S3-impl T4 (Operation NewSheep) — the SYNCHRONOUS sc + program seams, same
 * pattern as the EXT injector. The NK publishes its sc (vector 0xC00) handler at
 * KDP+0x390 (=0x50314ac0 in the 9.0.1 boot) and its program (vector 0x700) handler at
 * KDP+0x37c (=0x50314700); both are runtime-installed, NEVER hardcoded (Stop-rule
 * #6/#9). Under the master gate the forged g_exc_entry_table.{syscall,program}_entry
 * constants are NULLED, so execute_syscall / execute_illegal's twi-arm resolve LIVE
 * here and ExcEnter re-pointed at a LOCAL table (g_exc_entry_table never consulted).
 * Sentinel 0/DEADBEEF/~0 => 0 => pc==EXC_PC_UNRESOLVED (install not run — pre-S2b). */
#define NK_KDP_SC_VECTOR_OFFSET       0x390u
#define NK_KDP_PROGRAM_VECTOR_OFFSET  0x37cu

uint32_t ExcResolveNkVector(uint32_t kdp_base, uint32_t slot_offset,
                            ExcGuestRead32 read32, void *ctx);

ExcTransition ExcInjectSyscall(uint32_t restart_pc, uint32_t cur_msr,
                               uint32_t kdp_base, ExcGuestRead32 read32, void *ctx,
                               uint32_t *out_vector);

ExcTransition ExcInjectProgram(uint32_t restart_pc, uint32_t cur_msr,
                               uint32_t kdp_base, ExcGuestRead32 read32, void *ctx,
                               uint32_t *out_vector);

#endif /* EXC_INJECT_H */
