/*
 *  openfirmware_ci.h - Open Firmware Client-Interface (CHRP CI) callback +
 *  Core99 device-tree model (SS_M18 Stage 2a).
 *
 *  WIRING: as of SS_M18 S2b T4 this is linked into the SheepShaver binary and
 *  bound into the r5 marshalling shim by the gated newworld launch seam
 *  (sheepshaver_glue.cpp) behind SS_M18_TRAMPOLINE (default OFF — unreachable at
 *  default). It is also still exercised standalone by the machine/ unit tests
 *  (test_openfirmware_ci.cpp, test_trampoline_ofci_wiring.cpp). It models the
 *  OF-CI dispatch surface the Trampoline's MacOS.elf producer drives (handed the
 *  CI callback pointer in r5 at launch) and the Core99 device tree it queries.
 *
 *  Authority: docs/planning/newsheep/FINDINGS-s2a-ofci-dt.md (rev-2,
 *  RESIDUE-PASS) + docs/superpowers/plans/2026-06-14-ss-m18-s2a-ofci-dt-task0.md.
 *
 *  SCOPE FENCE (binding, from the spec):
 *   - call-method backends (read-blocks/write-blocks/block-size disk -> S2b;
 *     /mmu claim/translate/map -> S1/S2b; display methods -> M5/fb) wire via
 *     INJECTED callbacks / test doubles ONLY. S2a owns the seam, not the real
 *     backends.
 *   - the interrupt-map / interrupt-map-mask tuple VALUE is NON-FINAL
 *     (Q-S2a.3 flagged-residue-to-S2b): the DT answers the query and getproplen
 *     returns a provisional length, but the tuple cell content is NEVER a
 *     pinned/asserted value here. The §5-Q8 PIC input numbers (0x24/0x25/0x19)
 *     are likewise QEMU-only and deferred.
 *   - finddevice does OF component-wise, unit-address-INSENSITIVE matching
 *     (ADV-1 binding): the short static spelling /pci/mac-io/interrupt-controller
 *     resolves against canonical /pci@f2000000/mac-io@c/interrupt-controller@40000
 *     by per-component node-name match, NOT exact-string + alias lookup.
 */
#ifndef OPENFIRMWARE_CI_H
#define OPENFIRMWARE_CI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A CHRP client-interface cell. 64-bit on our arm64 host so a host pointer
 * (a service/method name, a property name, a buffer) fits in a cell. */
typedef uint64_t of_cell;

typedef uint32_t of_phandle;   /* package handle (0 == invalid) */
typedef uint32_t of_ihandle;   /* instance handle (0 == invalid) */
#define OF_INVALID_PHANDLE ((of_phandle)0)
#define OF_INVALID_IHANDLE ((of_ihandle)0)

/* OF CI convention: callback returns 0 when the named service was recognised
 * and dispatched (even if the service itself returns a -1 result cell, e.g. a
 * finddevice miss), and -1 when the service NAME was not resolvable at all. */
#define OF_CI_OK   0
#define OF_CI_FAIL (-1)

/* Maximum cells in a CHRP CI array (name,n_args,n_rets,args...,rets...). */
#define OF_CI_MAX_CELLS 32

typedef struct of_ci_context of_ci_context;

/*
 * call-method INJECTED backend (the dev_cuda/adb_stub decoupling seam).
 *
 *   opaque   - the cookie registered alongside the backend
 *   method   - decoded method name (e.g. "read-blocks")
 *   ihandle  - the target instance handle (arg after the method name)
 *   in/n_in  - decoded input args (cells AFTER method-name + ihandle)
 *   out/n_out- output result cells (the catch-result is out[0] by OF convention;
 *              the backend fills out[1..] with method returns)
 *
 * Returns the OF catch-code placed in the FIRST return cell (0 == success).
 */
typedef int (*of_call_method_fn)(void *opaque, const char *method,
                                 of_ihandle ihandle,
                                 const of_cell *in, int n_in,
                                 of_cell *out, int n_out);

/* Build / tear down a context holding the Core99 device tree. */
of_ci_context *of_ci_create_core99(void);
void of_ci_destroy(of_ci_context *ctx);

/* SS_M18 OF-CI early-environment fidelity (gated trampoline launch path).
 *  - of_ci_set_memory_size: write the real guest RAMSize into the /memory reg
 *    SIZE cell (default 0 preserved for the standalone unit test).
 *  - of_ci_set_claim_arena: retarget the `claim` bump-allocator arena to a
 *    guest-physical [base, limit) window (limit==0 == unbounded). Honors the
 *    SS_M18_CLAIM_BASE env override (hex). */
void of_ci_set_memory_size(of_ci_context *ctx, uint32_t ram_size);
void of_ci_set_claim_arena(of_ci_context *ctx, uint32_t base, uint32_t limit);

/*  - of_ci_set_toolbox_parcels: publish the staged Mac OS ROM image base/size
 *    into /rom/macos AAPL,toolbox-parcels (two BE cells). The BootScript reads
 *    this to find the 4MB toolbox image (ConfigInfo at image+0x30D000). */
void of_ci_set_toolbox_parcels(of_ci_context *ctx, uint32_t rom_virt, uint32_t size);

/* Register a call-method backend (test double). Matched by exact method name.
 * Returns false if the backend table is full. */
bool of_ci_register_method(of_ci_context *ctx, const char *method,
                           of_call_method_fn fn, void *opaque);

/*
 * The OF-CI callback (what the Trampoline calls through r5).
 *
 *   array[0]               = service name  (host const char* cast to cell)
 *   array[1]               = n_args
 *   array[2]               = n_rets
 *   array[3 .. 3+n_args-1] = input args
 *   array[3+n_args ..]     = return cells (written here)
 *
 * Returns OF_CI_OK if the service name (and, for call-method/interpret, the
 * method name / forth literal) resolved to a handler, OF_CI_FAIL otherwise.
 * Every OF_CI_FAIL also bumps the unresolved counter.
 */
int of_ci_callback(of_ci_context *ctx, of_cell *array);

/* The anti-vacuity gate G2a.b headline: count of calls whose service/method
 * name did not resolve to a handler. MUST be 0 across the enumerated set. */
unsigned of_ci_unresolved_count(const of_ci_context *ctx);

/* ---- Direct DT introspection (for the unit test's coverage predicate) ---- */

/* Resolve a path to a phandle using component-wise unit-address-insensitive
 * matching. Returns OF_INVALID_PHANDLE on miss. */
of_phandle of_dt_finddevice(of_ci_context *ctx, const char *path);

/* getproplen-style length for (phandle,name): >= 0 if present, -1 if absent. */
int of_dt_getproplen(of_ci_context *ctx, of_phandle ph, const char *name);

/* True iff the property exists AND is flagged NON-FINAL (Q-S2a.3 provisional:
 * interrupt-map / interrupt-map-mask). The test asserts the query RESOLVES and
 * the length is NON-FINAL, and NEVER asserts the tuple cell content. */
bool of_dt_prop_is_nonfinal(of_ci_context *ctx, of_phandle ph, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* OPENFIRMWARE_CI_H */
