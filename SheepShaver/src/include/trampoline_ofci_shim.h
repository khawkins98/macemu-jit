/*
 *  trampoline_ofci_shim.h - SS_M18 Stage 2b, T2: the guest-callable r5
 *  marshalling shim (LOAD-BEARING).
 *
 *  Operation NewSheep. The real Trampoline (MacOS.elf) calls the OpenFirmware
 *  Client Interface through a single guest pointer in r5 (a guest `bctrl` to the
 *  shim's guest-entry address). r5 is the GUEST address of this shim's entry. The
 *  committed-inert of_ci_callback(ctx, of_cell*) is 2-arg / host-64-bit-cell and
 *  is NOT directly callable from guest PPC. This shim is the width-, endianness-,
 *  arity-, and address-space bridge between the two:
 *
 *    guest BE-32 CHRP cell array (string/buffer cells = GUEST addresses)
 *        <-- this shim -->  host of_cell[] (64-bit cells; array[0] + pointer args
 *                           = HOST pointers)  -->  of_ci_callback
 *
 *  THE GUEST -> HOST BRIDGE MECHANISM (rev-2 T-2). The shim's guest entry is an
 *  EXEC_NATIVE "sheep" opcode (major opcode 6 = POWERPC_EMUL_OP, low-6-bits == 2,
 *  NATIVE_OP_field == NATIVE_OF_CI_SHIM, FN==1 so the routine returns via LR).
 *  The active aarch64 JIT does NOT natively emit major-opcode-6; compile_one()
 *  falls back to emit_inline_interp_call() -> ppc_jit_interp_one() ->
 *  jit_interp_one() -> decode() (the SHEEP instr_info registered by
 *  sheepshaver_cpu::init_decoder for major opcode 6) -> execute_sheep() ->
 *  (low-6-bits == 2) execute_native_op(NATIVE_OF_CI_SHIM) -> ss_ofci_shim_invoke.
 *  The T3 loader writes ss_ofci_shim_opcode() (big-endian) at the shim's guest
 *  entry address (= r5).
 *
 *  Plan: docs/superpowers/plans/2026-06-15-ss-m18-s2b-impl-loader-launch.md (rev-2),
 *        "The marshalling-shim contract the r5 entry resolves" + T2 + G(T2).
 *  Contracts: docs/planning/newsheep/FINDINGS-s2b-loader-handoff.md Q-S2b.3.
 *
 *  Two layers (mirrors trampoline_loader.{h,cpp}):
 *    (1) a PURE marshalling core (tramp_ofci_*), no emulator dependency, guest<->host
 *        translation injected as a function pointer + the OF-CI callback injected as a
 *        function pointer (so it does NOT link openfirmware_ci.o - T4 owns that link).
 *        Exercised standalone by test_trampoline_ofci_shim.cpp.
 *    (2) an emulator-facing wrapper (ss_ofci_shim_*) bound to Mac2HostAddr + the live
 *        context; compiled into the binary, INERT until T4 binds the context+callback.
 */

#ifndef TRAMPOLINE_OFCI_SHIM_H
#define TRAMPOLINE_OFCI_SHIM_H

#include <stddef.h>
#include <stdint.h>

#include "openfirmware_ci.h"   /* of_cell, of_ci_context, OF_CI_MAX_CELLS (types only; no link) */

#ifdef __cplusplus
extern "C" {
#endif

/* The OF-CI callback, injected as a function pointer (decoupling seam: the shim
 * core does NOT reference of_ci_callback directly, so it needs no link against
 * openfirmware_ci.o - T4 binds the real callback). Signature == of_ci_callback. */
typedef int (*tramp_ofci_callback_fn)(of_ci_context *ctx, of_cell *array);

/* Guest -> host address translation (emulator: Mac2HostAddr; test: flat mock RAM). */
typedef uint8_t *(*tramp_ofci_xlate_fn)(uint32_t guest_addr, void *ctx);

/* Sentinel that must NEVER appear where a real guest address is expected
 * (Stop-rule #9): a translation upstream produced garbage. The marshaller
 * REFUSES (returns < 0) rather than dereference it. */
#define TRAMP_OFCI_SENTINEL 0xDEADBEEFu

/* Marshal one guest BE-32 CHRP cell array through the OF-CI callback.
 *
 *   cb             - the injected OF-CI callback (of_ci_callback at T4).
 *   ctx            - the live OF-CI context (of_ci_create_core99()).
 *   guest_array_ptr- GUEST address of the BE-32 cell array (PPC r3 at the call).
 *   xlate / xctx   - guest -> host translation.
 *   out_ret        - receives the callback's int (becomes r3).
 *
 * Inbound:  reads the guest BE-32 cells; builds a host of_cell[] (<= OF_CI_MAX_CELLS):
 *           array[0] = translated host const char* service name; array[1/2] = n_args/
 *           n_rets; pointer arg cells (per the descriptor schema) translated guest->host;
 *           scalar args zero-extended 32->64.
 * Outbound: htonl's each rets[] VALUE cell back to the guest BE-32 array (T-1b: buffer
 *           contents alias guest memory via xlate, so NO copy-back).
 *
 * Returns 0 on success (and *out_ret = callback int), < 0 on refusal (over-cap,
 * sentinel pointer, unbound callback, or null translation) with a message in err.
 */
int tramp_ofci_marshal(tramp_ofci_callback_fn cb, of_ci_context *ctx,
                       uint32_t guest_array_ptr,
                       tramp_ofci_xlate_fn xlate, void *xctx,
                       int *out_ret, char *err, size_t errsz);

/* ---- Pointer-arg descriptor SCHEMA (rev-2 T-1) ---------------------------- *
 * A generic shim cannot know which arg/ret cells are guest pointers vs scalars.
 * This table pins it per (service) and per call-method (method). Bit i of
 * arg_ptr set => input arg index i is a guest pointer (translate guest->host);
 * bit i of ret_ptr set => return cell i is a guest-visible pointer/handle. An
 * absent name defaults to ALL-SCALAR (safe: a stray pointer cell would be passed
 * zero-extended, never dereferenced as a bogus host pointer). */
typedef struct {
	const char *name;
	uint32_t    arg_ptr;
	uint32_t    ret_ptr;
} TrampOfciArgDesc;

/* ---- The EXEC_NATIVE "sheep" guest-entry opcode (the bridge mechanism) ----- *
 * Pure encode/decode of the guest opcode the T3 loader writes at the shim entry
 * (= r5). Shared by the emulator wrapper (ss_ofci_shim_opcode) and the micro-test
 * so both go through ONE encoder. Encoding (see thunks.cpp POWERPC_NATIVE_OP):
 *   0x18000000 (POWERPC_EMUL_OP) | (FN=1 << 12) | (selector << 6) | 2
 * FN=1 => execute_sheep does pc()=lr() (return to the bctrl caller). */
uint32_t tramp_ofci_encode_native_op(uint32_t native_selector);
/* Returns 1 iff `opcode` is a SHEEP EXEC_NATIVE op (major opcode 6, low-6-bits
 * == 2), writing the NATIVE_OP selector to *selector and the FN bit to *fn
 * (NULL-tolerant). Mirrors execute_sheep()'s decode exactly. */
int      tramp_ofci_decode_native_op(uint32_t opcode, uint32_t *selector, int *fn);

/* Look up the descriptor for a service name (returns NULL => all-scalar). */
const TrampOfciArgDesc *tramp_ofci_service_desc(const char *service);
/* Look up the descriptor for a call-method method name; bit i refers to the
 * NESTED input arg index (CHRP arg index 2+i). NULL => all-scalar nested args. */
const TrampOfciArgDesc *tramp_ofci_method_desc(const char *method);

#ifndef TRAMPOLINE_OFCI_SHIM_STANDALONE_TEST
/* ------------------------------------------------------------------ *
 *  Emulator-facing wrapper. Bound to Mac2HostAddr + the live context.*
 *  INERT until T4 binds (ctx + callback NULL => ss_ofci_shim_invoke    *
 *  refuses). Linked into the SheepShaver binary; the only caller is    *
 *  sheepshaver_cpu::execute_native_op(NATIVE_OF_CI_SHIM).              *
 * ------------------------------------------------------------------ */

/* T4 binds the live context + callback (of_ci_create_core99() + of_ci_callback). */
void     ss_ofci_shim_bind(of_ci_context *ctx, tramp_ofci_callback_fn cb);

/* The intercept body execute_native_op(NATIVE_OF_CI_SHIM) calls: marshals the
 * guest array at r3 and returns the new r3. Refuses (r3 = (uint32)-1, logs
 * [S2B-SHIM]) when unbound or on a marshal refusal. */
uint32_t ss_ofci_shim_invoke(uint32_t guest_array_ptr_r3);

/* The big-endian guest-entry opcode the T3 loader writes at r5
 * (= POWERPC_NATIVE_OP(1, NATIVE_OF_CI_SHIM)). */
uint32_t ss_ofci_shim_opcode(void);
#endif /* TRAMPOLINE_OFCI_SHIM_STANDALONE_TEST */

#ifdef __cplusplus
}
#endif

#endif /* TRAMPOLINE_OFCI_SHIM_H */
