/*
 *  trampoline_ofci_shim.cpp - SS_M18 Stage 2b, T2: the guest-callable r5
 *  marshalling shim (LOAD-BEARING).
 *
 *  See include/trampoline_ofci_shim.h for the full contract + the bridge
 *  mechanism (EXEC_NATIVE "sheep" opcode -> execute_native_op).
 *
 *  Two layers:
 *    (1) a PURE marshalling core (tramp_ofci_*): guest BE-32 CHRP array <-> host
 *        of_cell[]; guest<->host translation + the OF-CI callback both injected as
 *        function pointers (no emulator / openfirmware_ci link). Unit-tested.
 *    (2) an emulator-facing wrapper (ss_ofci_shim_*) bound to Mac2HostAddr + the
 *        live context; compiled out of the standalone test build.
 */

#include "trampoline_ofci_shim.h"

#include <stdio.h>
#include <string.h>

/* ---- big-endian cell access (the guest array is 32-bit BE words) ---------- */
static inline uint32_t rd_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline void wr_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int ofci_fail(char *err, size_t errsz, const char *msg)
{
	if (err && errsz) { strncpy(err, msg, errsz - 1); err[errsz - 1] = 0; }
	return -1;
}

/* ================================================================== *
 *  Pointer-arg descriptor SCHEMA (rev-2 T-1).                         *
 *                                                                     *
 *  Covers the bounded OF inventory the Trampoline drives. arg index 0 *
 *  is the FIRST input arg (CHRP cell array[3]). Bit i set => that cell *
 *  is a guest pointer to translate guest->host. ret_ptr is 0 across    *
 *  the inventory (phandles/ihandles/lengths are scalars - T-1b);       *
 *  preserved in the schema so a future guest-visible return pointer is *
 *  a one-line addition, not a silent truncation.                       *
 * ================================================================== */
static const TrampOfciArgDesc SERVICE_DESC[] = {
	/* name                  arg_ptr (bit i = arg index i is a guest ptr)   ret_ptr */
	/* getprop(phandle, name*, buf*, buflen)                              */
	{ "getprop",             (1u << 1) | (1u << 2),                         0 },
	/* getproplen(phandle, name*)                                         */
	{ "getproplen",          (1u << 1),                                     0 },
	/* finddevice(path*)                                                  */
	{ "finddevice",          (1u << 0),                                     0 },
	/* setprop(phandle, name*, buf*, len)                                 */
	{ "setprop",             (1u << 1) | (1u << 2),                         0 },
	/* nextprop(phandle, prev-name*, buf*)                                */
	{ "nextprop",            (1u << 1) | (1u << 2),                         0 },
	/* open(path*)                                                        */
	{ "open",                (1u << 0),                                     0 },
	/* canon(path*, buf*, buflen)                                         */
	{ "canon",               (1u << 0) | (1u << 1),                         0 },
	/* package-to-path(phandle, buf*, buflen)                             */
	{ "package-to-path",     (1u << 1),                                     0 },
	/* instance-to-path(ihandle, buf*, buflen)                            */
	{ "instance-to-path",    (1u << 1),                                     0 },
	/* read(ihandle, buf*, len)                                           */
	{ "read",                (1u << 1),                                     0 },
	/* write(ihandle, buf*, len)                                          */
	{ "write",               (1u << 1),                                     0 },
	/* interpret(forth-literal*, ...)                                     */
	{ "interpret",           (1u << 0),                                     0 },
	/* call-method(method-name*, ihandle, nested-args...) - arg0 is the   */
	/* method name (ptr); nested args (index 2+) via tramp_ofci_method_desc */
	{ "call-method",         (1u << 0),                                     0 },
	/* All-scalar services kept explicit for documentation (arg_ptr 0):
	 * peer/child/parent(phandle); instance-to-package(ihandle);
	 * instantiate-rtas(real-base); close/seek/exit/test/quiesce(scalars);
	 * claim(virt,size,align). Absent names default to all-scalar anyway. */
	{ "peer",                0,                                             0 },
	{ "child",               0,                                             0 },
	{ "parent",              0,                                             0 },
	{ "instance-to-package", 0,                                             0 },
	{ "instantiate-rtas",    0,                                             0 },
	{ "claim",               0,                                             0 },
};
static const int N_SERVICE_DESC = (int)(sizeof(SERVICE_DESC) / sizeof(SERVICE_DESC[0]));

/* call-method nested-arg descriptors: bit i = NESTED input arg index i
 * (== CHRP arg index 2+i; the method name is arg0, the ihandle is arg1). */
static const TrampOfciArgDesc METHOD_DESC[] = {
	/* read-blocks(buffer*, block, count)                                 */
	{ "read-blocks",         (1u << 0),                                     0 },
	/* write-blocks(buffer*, block, count)                                */
	{ "write-blocks",        (1u << 0),                                     0 },
	/* block-size() - no nested args                                      */
	{ "block-size",          0,                                             0 },
	/* set-colors(color-table*, index, count)                            */
	{ "set-colors",          (1u << 0),                                     0 },
	/* /mmu translate/claim/map + display dimensions/fill-rectangle/
	 * draw-rectangle: all nested args scalar. Explicit for documentation. */
	{ "translate",           0,                                             0 },
	{ "map",                 0,                                             0 },
	{ "dimensions",          0,                                             0 },
	{ "fill-rectangle",      0,                                             0 },
	{ "draw-rectangle",      0,                                             0 },
};
static const int N_METHOD_DESC = (int)(sizeof(METHOD_DESC) / sizeof(METHOD_DESC[0]));

/* ---- EXEC_NATIVE "sheep" guest-entry opcode (the bridge mechanism) -------- */
uint32_t tramp_ofci_encode_native_op(uint32_t native_selector)
{
	/* 0x18000000 == POWERPC_EMUL_OP (architectural base of the SheepShaver
	 * extended opcode space - major opcode 6). FN=1 in bit 12, selector in
	 * bits 6..11, low-6-bits == 2 (EXEC_NATIVE). */
	return 0x18000000u | (1u << 12) | ((native_selector & 0x3fu) << 6) | 2u;
}

int tramp_ofci_decode_native_op(uint32_t opcode, uint32_t *selector, int *fn)
{
	if ((opcode >> 26) != 6) return 0;        /* not a SHEEP opcode */
	if ((opcode & 0x3f) != 2) return 0;       /* low-6-bits != EXEC_NATIVE */
	if (selector) *selector = (opcode >> 6) & 0x3fu;
	if (fn)       *fn       = (int)((opcode >> 12) & 1u);
	return 1;
}

const TrampOfciArgDesc *tramp_ofci_service_desc(const char *service)
{
	if (!service) return NULL;
	for (int i = 0; i < N_SERVICE_DESC; i++)
		if (strcmp(service, SERVICE_DESC[i].name) == 0) return &SERVICE_DESC[i];
	return NULL;
}

const TrampOfciArgDesc *tramp_ofci_method_desc(const char *method)
{
	if (!method) return NULL;
	for (int i = 0; i < N_METHOD_DESC; i++)
		if (strcmp(method, METHOD_DESC[i].name) == 0) return &METHOD_DESC[i];
	return NULL;
}

/* ================================================================== *
 *  The marshalling core.                                              *
 * ================================================================== */
int tramp_ofci_marshal(tramp_ofci_callback_fn cb, of_ci_context *ctx,
                       uint32_t guest_array_ptr,
                       tramp_ofci_xlate_fn xlate, void *xctx,
                       int *out_ret, char *err, size_t errsz)
{
	if (!cb || !ctx) return ofci_fail(err, errsz, "shim unbound (no callback/context)");
	if (!xlate)       return ofci_fail(err, errsz, "no guest->host translation");
	if (guest_array_ptr == TRAMP_OFCI_SENTINEL)
		return ofci_fail(err, errsz, "[S2B-SHIM-SENTINEL] array pointer is 0xDEADBEEF");

	uint8_t *gp = xlate(guest_array_ptr, xctx);
	if (!gp) return ofci_fail(err, errsz, "guest array translation returned NULL");

	uint32_t n_args = rd_be32(gp + 4 * 1);
	uint32_t n_rets = rd_be32(gp + 4 * 2);

	/* Over-cap refusal: name + n_args + n_rets header cells must fit. */
	if ((uint64_t)3 + n_args + n_rets > (uint64_t)OF_CI_MAX_CELLS)
		return ofci_fail(err, errsz, "CHRP array exceeds OF_CI_MAX_CELLS");

	of_cell host[OF_CI_MAX_CELLS];
	memset(host, 0, sizeof(host));

	/* array[0]: service name guest string ptr -> host const char*. */
	uint32_t svc_guest = rd_be32(gp + 4 * 0);
	if (svc_guest == TRAMP_OFCI_SENTINEL)
		return ofci_fail(err, errsz, "[S2B-SHIM-SENTINEL] service name ptr is 0xDEADBEEF");
	const char *service = NULL;
	if (svc_guest) {
		service = (const char *)xlate(svc_guest, xctx);
		if (!service) return ofci_fail(err, errsz, "service name translation returned NULL");
	}
	host[0] = (of_cell)(uintptr_t)service;
	host[1] = (of_cell)n_args;
	host[2] = (of_cell)n_rets;

	const TrampOfciArgDesc *svc = tramp_ofci_service_desc(service);
	bool is_call_method = (service && strcmp(service, "call-method") == 0);
	const TrampOfciArgDesc *meth = NULL;   /* resolved lazily for call-method */

	/* Inbound args. */
	for (uint32_t i = 0; i < n_args; i++) {
		uint32_t cell = rd_be32(gp + 4 * (3 + i));
		bool is_ptr;
		if (is_call_method) {
			if (i == 0)        is_ptr = true;   /* method name ptr */
			else if (i == 1)   is_ptr = false;  /* ihandle scalar */
			else {
				/* nested arg (CHRP index 2+ -> nested index i-2). The method
				 * name was translated at i==0, so host[3] is the host string. */
				if (!meth) meth = tramp_ofci_method_desc((const char *)(uintptr_t)host[3]);
				uint32_t nested = i - 2;
				is_ptr = meth ? ((meth->arg_ptr >> nested) & 1u) != 0 : false;
			}
		} else {
			is_ptr = svc ? ((svc->arg_ptr >> i) & 1u) != 0 : false;
		}

		if (is_ptr) {
			if (cell == TRAMP_OFCI_SENTINEL)
				return ofci_fail(err, errsz, "[S2B-SHIM-SENTINEL] pointer arg is 0xDEADBEEF");
			if (cell == 0) {
				host[3 + i] = 0;                /* a null guest pointer stays null */
			} else {
				uint8_t *hp = xlate(cell, xctx);
				if (!hp) return ofci_fail(err, errsz, "pointer arg translation returned NULL");
				host[3 + i] = (of_cell)(uintptr_t)hp;
			}
		} else {
			host[3 + i] = (of_cell)(uint32_t)cell;  /* scalar zero-extend 32->64 */
		}
	}

	/* Return cells start zeroed (memset above). Invoke the callback. */
	int ret = cb(ctx, host);
	if (out_ret) *out_ret = ret;

	/* Outbound: htonl each rets[] VALUE cell back to the guest BE-32 array.
	 * T-1b: buffer contents already aliased guest memory through xlate, so no
	 * copy-back. A guest-visible RETURN pointer (ret_ptr) would need host->guest
	 * translation here - none in the bounded inventory (ret_ptr == 0 throughout),
	 * recorded as a residue rather than silently truncated. */
	for (uint32_t j = 0; j < n_rets; j++) {
		of_cell rc = host[3 + n_args + j];
		bool ret_is_ptr = svc ? ((svc->ret_ptr >> j) & 1u) != 0 : false;
		if (ret_is_ptr)
			return ofci_fail(err, errsz, "guest-visible return pointer needs host->guest "
			                 "translation (unsupported in the bounded inventory)");
		wr_be32(gp + 4 * (3 + n_args + j), (uint32_t)rc);
	}
	return 0;
}

/* ================================================================== *
 *  Emulator-facing wrapper.                                          *
 * ================================================================== */
#ifndef TRAMPOLINE_OFCI_SHIM_STANDALONE_TEST

#include "sysdeps.h"
#include "cpu_emulation.h"   /* Mac2HostAddr */
#include "thunks.h"          /* NATIVE_OF_CI_SHIM */

static of_ci_context        *s_ctx = NULL;
static tramp_ofci_callback_fn s_cb  = NULL;

static uint8_t *ss_ofci_xlate(uint32_t addr, void * /*ctx*/)
{
	return Mac2HostAddr(addr);
}

void ss_ofci_shim_bind(of_ci_context *ctx, tramp_ofci_callback_fn cb)
{
	s_ctx = ctx;
	s_cb  = cb;
}

uint32_t ss_ofci_shim_invoke(uint32_t guest_array_ptr_r3)
{
	int ret = -1;
	char err[160] = {0};
	if (tramp_ofci_marshal(s_cb, s_ctx, guest_array_ptr_r3,
	                       ss_ofci_xlate, NULL, &ret, err, sizeof err) < 0) {
		fprintf(stderr, "[S2B-SHIM] marshal refused (r3=0xffffffff): %s\n", err);
		return (uint32_t)-1;
	}
	return (uint32_t)ret;
}

uint32_t ss_ofci_shim_opcode(void)
{
	/* Same encoder the micro-test uses, applied to the real enum selector.
	 * tramp_ofci_encode_native_op's 0x18000000 base == POWERPC_EMUL_OP. */
	return tramp_ofci_encode_native_op((uint32_t)NATIVE_OF_CI_SHIM);
}

#endif /* TRAMPOLINE_OFCI_SHIM_STANDALONE_TEST */
