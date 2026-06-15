/*
 *  trampoline_ofci_backends.h - SS_M18 Stage 2b, T4: the pinned OF-CI
 *  call-method backends (the of_call_method_fn owners of_ci_callback dispatches
 *  to). Operation NewSheep.
 *
 *  T4 wires the committed-inert of_ci_callback into the live boot: at the gated
 *  newworld boot the launch seam creates the Core99 DT context
 *  (of_ci_create_core99()), installs THESE backends, and binds the context +
 *  of_ci_callback into the T2 marshalling shim. DT queries + interpret literals
 *  resolve inside of_ci_callback directly (no backend); the call-method services
 *  below need a registered of_call_method_fn or the dispatcher counts them
 *  UNRESOLVED.
 *
 *  Backend ownership map (pinned, plan T4):
 *    /mmu  claim/translate/map  -> RECORDING STUB. NON-ACCEPTANCE (Stop-rule #7):
 *          logs each request ([S2B-MMU-STUB]), returns an identity/plausible claim.
 *          The REAL /mmu is owed to S1; "build-complete on the stub" is a
 *          false-clean. The stub ASSERTS-FAIL ([S2B-SHIM-COLLIDE], Stop-rule #13)
 *          if the Trampoline ever claim/maps a range overlapping the shim's
 *          reserved page (TRAMP_SHIM_ENTRY) - a silent post-S1 collision becomes a
 *          logged tripwire.
 *    disk  read-blocks/write-blocks/block-size -> S4 STUB (deferred; the NK is
 *          reached before disk IM-init matters).
 *    display dimensions/set-colors/fill-rectangle/draw-rectangle -> no-op resolve.
 *    instantiate-rtas -> minimal RTAS stub handle.
 *  interpret key?/key/reset-all + the DT query family resolve in of_ci_callback.
 *
 *  This TU is PURE (no emulator/kpx_cpu dependency): it is exercised standalone by
 *  test_trampoline_ofci_wiring.cpp and compiled into the SheepShaver binary (the
 *  gated boot is the only live consumer).
 *
 *  Plan: docs/superpowers/plans/2026-06-15-ss-m18-s2b-impl-loader-launch.md (rev-2) T4 + G(T4).
 */

#ifndef TRAMPOLINE_OFCI_BACKENDS_H
#define TRAMPOLINE_OFCI_BACKENDS_H

#include "openfirmware_ci.h"   /* of_ci_context, of_call_method_fn, of_cell */

#ifdef __cplusplus
extern "C" {
#endif

/* Install the pinned S2b call-method backends onto ctx (the /mmu recording stub,
 * the disk S4 stub, the display no-op, the RTAS stub). Returns the number of
 * method names registered. Idempotent per (ctx) only in the sense that calling it
 * twice double-registers (the dispatcher matches the first) - call ONCE. */
int tramp_ofci_install_backends(of_ci_context *ctx);

/* The /mmu recording-stub catch code returned when a claim/map overlaps the shim's
 * reserved page. Distinct from 0 (OF success) so a caller (and the micro-test) can
 * see the tripwire fired in the catch-result cell. */
#define TRAMP_OFCI_MMU_COLLIDE (-73)

/* STOP hook the /mmu recording stub calls on a [S2B-SHIM-COLLIDE] tripwire. The
 * live boot installs an abort()ing hook (convert the silent post-S1 fault into a
 * loud, logged STOP - Stop-rule #13); the micro-test leaves it NULL and asserts
 * the catch code + collision count instead. */
typedef void (*tramp_ofci_stop_fn)(void);
void tramp_ofci_set_stop_fn(tramp_ofci_stop_fn fn);

/* Number of [S2B-SHIM-COLLIDE] tripwires that have fired (test observability). */
int  tramp_ofci_collision_count(void);

/* SS_M18 S1-bringup: publish the guest-physical apertures the /mmu MINIMALLY-REAL
 * translate/map honor as V=P identity (the recording stub is upgraded to a
 * bringup-real backend behind SS_M18_TRAMPOLINE). The backend TU is PURE (no
 * emulator globals), so the gated launch seam pushes RAMBase/RAMSize/ROMBase/
 * ROM size here right after tramp_ofci_install_backends().
 *
 * NON-ACCEPTANCE: this is a V=P bringup approximation, NOT S1's full live
 * (SR/BAT/SDR1) paged MMU. It is correct ONLY while the NK's intended map is
 * identity; the moment a non-identity segment/BAT is installed it must be
 * replaced by paged_mmu_translate against the live regs. See
 * docs/superpowers/plans/2026-06-15-ss-m18-s1-task0-live-mmu.md. */
void tramp_ofci_set_mmu_extent(uint32_t ram_base, uint32_t ram_size,
                               uint32_t rom_base, uint32_t rom_size);

/* Counts of MINIMALLY-REAL /mmu calls (test/diagnostic observability). */
int  tramp_ofci_mmu_translate_count(void);
int  tramp_ofci_mmu_map_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TRAMPOLINE_OFCI_BACKENDS_H */
