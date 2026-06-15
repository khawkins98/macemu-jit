/*
 *  test_trampoline_ofci_shim.cpp - SS_M18 Stage 2b T2 planted-array micro-test
 *                                  (G2b.shim). Standalone: links the pure
 *                                  marshalling core + openfirmware_ci.cpp; NO
 *                                  emulator/kpx_cpu dependency.
 *
 *  Operation NewSheep. Drives a planted guest BE-32 CHRP cell array THROUGH the
 *  shim's bridge — the EXEC_NATIVE "sheep" guest-entry opcode (encode/decode,
 *  exactly the opcode the T3 loader writes at r5 and execute_sheep decodes) plus
 *  the marshalling core — and asserts the rev-2 G(T2) contract:
 *    (a) array[0] reaches the callback as a valid host const char*;
 *    (b) each pointer arg cell is translated guest->host PER THE DESCRIPTOR SCHEMA;
 *    (c) scalar args are zero-extended 32->64;
 *    (d) rets[] VALUE cells are written back BE-32;
 *    (e) the callback int returns (becomes r3);
 *  plus the refusal paths (sentinel / over-cap / unbound) and a link/ABI smoke
 *  through the REAL of_ci_callback.
 *
 *  Built with -DTRAMPOLINE_OFCI_SHIM_STANDALONE_TEST (emulator wrapper excluded).
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "trampoline_ofci_shim.h"
#include "openfirmware_ci.h"

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg) do { g_checks++; if (!(cond)) { \
	g_fail++; fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); } } while (0)

/* ---- flat mock RAM + guest<->host translation ---------------------------- */
#define MOCK_RAM_SIZE 0x10000u
static uint8_t g_ram[MOCK_RAM_SIZE];

static uint8_t *mock_xlate(uint32_t guest_addr, void *ctx)
{
	(void)ctx;
	if (guest_addr >= MOCK_RAM_SIZE) return NULL;
	return g_ram + guest_addr;
}

static void put_be32(uint32_t off, uint32_t v)
{
	g_ram[off + 0] = (uint8_t)(v >> 24);
	g_ram[off + 1] = (uint8_t)(v >> 16);
	g_ram[off + 2] = (uint8_t)(v >> 8);
	g_ram[off + 3] = (uint8_t)(v);
}
static uint32_t get_be32(uint32_t off)
{
	return ((uint32_t)g_ram[off + 0] << 24) | ((uint32_t)g_ram[off + 1] << 16) |
	       ((uint32_t)g_ram[off + 2] << 8)  | (uint32_t)g_ram[off + 3];
}
static void put_str(uint32_t off, const char *s) { strcpy((char *)(g_ram + off), s); }

/* ---- a recording mock callback (precise marshalling assertions) ---------- */
struct CapturedCall {
	int    invoked;
	of_cell host[OF_CI_MAX_CELLS];
};
static CapturedCall g_cap;

static int mock_cb(of_ci_context *ctx, of_cell *array)
{
	(void)ctx;
	g_cap.invoked++;
	memcpy(g_cap.host, array, sizeof(g_cap.host));
	/* write a known return value into the first ret slot:
	 * rets start at array[3 + n_args]. */
	uint32_t n_args = (uint32_t)array[1];
	array[3 + n_args] = (of_cell)0xCAFEu;   /* a scalar return (phandle-like) */
	return 0x1234;                          /* becomes r3 / out_ret */
}

/* ========================================================================== */
int main(void)
{
	char err[160];

	/* ---- Test 1: the EXEC_NATIVE bridge opcode round-trips ----------------
	 * The same encoder ss_ofci_shim_opcode() uses and the same decode
	 * execute_sheep() uses — proving "through the r5 EXEC_NATIVE entry". */
	{
		const uint32_t sels[] = { 0u, 1u, 42u, 0x3fu };
		for (unsigned k = 0; k < sizeof(sels) / sizeof(sels[0]); k++) {
			uint32_t sel = sels[k];
			uint32_t op  = tramp_ofci_encode_native_op(sel);
			uint32_t want = 0x18000000u | (1u << 12) | ((sel & 0x3fu) << 6) | 2u;
			CHECK(op == want, "encode matches POWERPC_NATIVE_OP(FN=1) formula");
			CHECK((op >> 26) == 6u, "major opcode == 6 (SHEEP)");
			CHECK((op & 0x3fu) == 2u, "low-6-bits == 2 (EXEC_NATIVE)");
			uint32_t dsel = 0; int dfn = 0;
			CHECK(tramp_ofci_decode_native_op(op, &dsel, &dfn) == 1, "decode recognizes SHEEP op");
			CHECK(dsel == (sel & 0x3fu), "decode round-trips the selector");
			CHECK(dfn == 1, "decode reports FN=1 (return via LR)");
		}
		/* a non-SHEEP opcode (ori r0,r0,0 == 0x60000000) is NOT a native op */
		CHECK(tramp_ofci_decode_native_op(0x60000000u, NULL, NULL) == 0, "non-SHEEP opcode rejected");
	}

	/* ---- Test 2: the pointer-arg descriptor SCHEMA ------------------------ */
	{
		const TrampOfciArgDesc *gp = tramp_ofci_service_desc("getprop");
		CHECK(gp != NULL, "getprop has a descriptor");
		/* getprop(phandle, name*, buf*, buflen): bits 1 and 2 are pointers */
		CHECK(gp && gp->arg_ptr == ((1u << 1) | (1u << 2)), "getprop arg_ptr = name|buf");
		CHECK(gp && gp->ret_ptr == 0, "getprop ret_ptr = 0 (scalar)");

		const TrampOfciArgDesc *fd = tramp_ofci_service_desc("finddevice");
		CHECK(fd && (fd->arg_ptr & (1u << 0)), "finddevice arg0 (path) is a pointer");

		const TrampOfciArgDesc *cm = tramp_ofci_service_desc("call-method");
		CHECK(cm && (cm->arg_ptr & (1u << 0)), "call-method arg0 (method name) is a pointer");

		const TrampOfciArgDesc *rb = tramp_ofci_method_desc("read-blocks");
		CHECK(rb && (rb->arg_ptr & (1u << 0)), "read-blocks nested arg0 (buffer) is a pointer");
		const TrampOfciArgDesc *tr = tramp_ofci_method_desc("translate");
		CHECK(tr && tr->arg_ptr == 0, "/mmu translate nested args all scalar");

		CHECK(tramp_ofci_service_desc("no-such-service") == NULL, "absent service => NULL (all-scalar)");
	}

	/* ---- Test 3: getprop marshalling end-to-end through a planted array ----
	 * Layout at guest 0x1000:
	 *   [0]=0x2000 (->"getprop")  [1]=4 n_args  [2]=1 n_rets
	 *   [3]=0x11 phandle(scalar)  [4]=0x3000 (->"model")  [5]=0x4000 buf(ptr)
	 *   [6]=0x40 buflen(scalar)   [7]=ret slot                                */
	{
		memset(g_ram, 0xAA, sizeof(g_ram));   /* poison: catch un-set cells */
		memset(&g_cap, 0, sizeof(g_cap));
		put_str(0x2000, "getprop");
		put_str(0x3000, "model");
		put_be32(0x1000 + 4 * 0, 0x2000);
		put_be32(0x1000 + 4 * 1, 4);
		put_be32(0x1000 + 4 * 2, 1);
		put_be32(0x1000 + 4 * 3, 0x11);
		put_be32(0x1000 + 4 * 4, 0x3000);
		put_be32(0x1000 + 4 * 5, 0x4000);
		put_be32(0x1000 + 4 * 6, 0x40);
		put_be32(0x1000 + 4 * 7, 0);

		of_ci_context *ctx = of_ci_create_core99();
		CHECK(ctx != NULL, "of_ci_create_core99 context built");

		int out_ret = -999;
		int rc = tramp_ofci_marshal(mock_cb, ctx, 0x1000, mock_xlate, NULL,
		                            &out_ret, err, sizeof(err));
		CHECK(rc == 0, "marshal getprop succeeded");
		CHECK(g_cap.invoked == 1, "callback invoked exactly once");
		/* (a) array[0] is a valid host const char* == "getprop" */
		CHECK(g_cap.host[0] == (of_cell)(uintptr_t)(g_ram + 0x2000), "host[0] = translated name ptr");
		CHECK(strcmp((const char *)(uintptr_t)g_cap.host[0], "getprop") == 0, "host[0] reads 'getprop'");
		/* header cells */
		CHECK(g_cap.host[1] == 4, "host[1] = n_args");
		CHECK(g_cap.host[2] == 1, "host[2] = n_rets");
		/* (c) scalar args zero-extended */
		CHECK(g_cap.host[3] == 0x11, "host[3] phandle scalar zero-extended");
		CHECK(g_cap.host[6] == 0x40, "host[6] buflen scalar zero-extended");
		/* (b) pointer args translated guest->host per schema */
		CHECK(g_cap.host[4] == (of_cell)(uintptr_t)(g_ram + 0x3000), "host[4] name ptr translated");
		CHECK(strcmp((const char *)(uintptr_t)g_cap.host[4], "model") == 0, "host[4] reads 'model'");
		CHECK(g_cap.host[5] == (of_cell)(uintptr_t)(g_ram + 0x4000), "host[5] buffer ptr translated");
		/* (e) callback int returned */
		CHECK(out_ret == 0x1234, "out_ret carries the callback int (-> r3)");
		/* (d) rets[] value cell written back BE-32 */
		CHECK(get_be32(0x1000 + 4 * 7) == 0xCAFEu, "ret cell written back BE-32");

		of_ci_destroy(ctx);
	}

	/* ---- Test 4: refusal paths (Stop-rule #9 sentinel + over-cap + unbound) */
	{
		of_ci_context *ctx = of_ci_create_core99();
		int out_ret = 0;

		/* sentinel array pointer */
		CHECK(tramp_ofci_marshal(mock_cb, ctx, TRAMP_OFCI_SENTINEL, mock_xlate, NULL,
		                         &out_ret, err, sizeof(err)) < 0, "sentinel array ptr refused");

		/* sentinel service-name pointer */
		memset(g_ram, 0, sizeof(g_ram));
		put_be32(0x1000 + 4 * 0, TRAMP_OFCI_SENTINEL);
		put_be32(0x1000 + 4 * 1, 0);
		put_be32(0x1000 + 4 * 2, 0);
		CHECK(tramp_ofci_marshal(mock_cb, ctx, 0x1000, mock_xlate, NULL,
		                         &out_ret, err, sizeof(err)) < 0, "sentinel service ptr refused");

		/* sentinel pointer ARG (getprop name) */
		put_str(0x2000, "getprop");
		put_be32(0x1000 + 4 * 0, 0x2000);
		put_be32(0x1000 + 4 * 1, 4);
		put_be32(0x1000 + 4 * 2, 1);
		put_be32(0x1000 + 4 * 3, 0x11);
		put_be32(0x1000 + 4 * 4, TRAMP_OFCI_SENTINEL);
		put_be32(0x1000 + 4 * 5, 0x4000);
		put_be32(0x1000 + 4 * 6, 0x40);
		CHECK(tramp_ofci_marshal(mock_cb, ctx, 0x1000, mock_xlate, NULL,
		                         &out_ret, err, sizeof(err)) < 0, "sentinel pointer arg refused");

		/* over-cap: 3 + n_args + n_rets > OF_CI_MAX_CELLS */
		put_be32(0x1000 + 4 * 0, 0x2000);
		put_be32(0x1000 + 4 * 1, 40);
		put_be32(0x1000 + 4 * 2, 0);
		CHECK(tramp_ofci_marshal(mock_cb, ctx, 0x1000, mock_xlate, NULL,
		                         &out_ret, err, sizeof(err)) < 0, "over-cap CHRP array refused");

		/* unbound: null callback */
		CHECK(tramp_ofci_marshal(NULL, ctx, 0x1000, mock_xlate, NULL,
		                         &out_ret, err, sizeof(err)) < 0, "null callback refused");
		/* null translation */
		CHECK(tramp_ofci_marshal(mock_cb, ctx, 0x1000, NULL, NULL,
		                         &out_ret, err, sizeof(err)) < 0, "null xlate refused");

		of_ci_destroy(ctx);
	}

	/* ---- Test 5: link/ABI smoke through the REAL of_ci_callback -----------
	 * finddevice("/") routed through the marshaller into the real callback,
	 * proving the shim's host of_cell[] is ABI-compatible with of_ci_callback. */
	{
		memset(g_ram, 0, sizeof(g_ram));
		put_str(0x2000, "finddevice");
		put_str(0x3000, "/");
		put_be32(0x1000 + 4 * 0, 0x2000);
		put_be32(0x1000 + 4 * 1, 1);   /* n_args = 1 (path) */
		put_be32(0x1000 + 4 * 2, 1);   /* n_rets = 1 (phandle) */
		put_be32(0x1000 + 4 * 3, 0x3000);
		put_be32(0x1000 + 4 * 4, 0);

		of_ci_context *ctx = of_ci_create_core99();
		int out_ret = -999;
		int rc = tramp_ofci_marshal(of_ci_callback, ctx, 0x1000, mock_xlate, NULL,
		                            &out_ret, err, sizeof(err));
		CHECK(rc == 0, "real of_ci_callback invoked through the shim (link/ABI smoke)");
		CHECK(out_ret == 0, "finddevice('/') returned ok via the real callback");
		/* the root phandle written back to the ret cell is non-zero / non-(-1) */
		uint32_t ph = get_be32(0x1000 + 4 * 4);
		CHECK(ph != 0 && ph != 0xFFFFFFFFu, "finddevice('/') yielded a real root phandle");
		of_ci_destroy(ctx);
	}

	printf("test_trampoline_ofci_shim: %d checks, %d failures\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
