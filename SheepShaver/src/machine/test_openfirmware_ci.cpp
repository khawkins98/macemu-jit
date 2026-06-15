/*
 *  test_openfirmware_ci.cpp - unit tests for the OF-CI callback + Core99 DT
 *  (SS_M18 Stage 2a; standalone, no-boot, no kpx_cpu link — mirrors
 *  test_dev_openpic / test_paged_mmu: model + tests only, wiring deferred).
 *
 *  Oracle: the Q3 [QEMU-BEHAVIORAL] Trampoline trace as the expected-call
 *  sequence (dispatch-idiom ONLY, NEVER an address oracle). Spec:
 *  docs/planning/newsheep/FINDINGS-s2a-ofci-dt.md (rev-2).
 *
 *  P-M1 (anti-vacuity): the call battery below is a COMMITTED FIXTURE derived
 *  from the recon's Q-S2a.1 inventory (the 21/14/3 enumerated names + the
 *  finddevice/getprop/nextprop query set) — it is NEVER iterated from the
 *  dispatch table's registered handlers, so unresolved==0 is not a tautology.
 *
 *  P-M2 (carve-out): the interrupt-map / interrupt-map-mask rows assert only
 *  that the query RESOLVES and getproplen is NON-FINAL; the tuple cell content
 *  is NEVER asserted (it is a Q-S2a.3 flagged-residue-to-S2b).
 *
 *  Honest scope (G2a.c): this proves the DT answers the OpenBIOS-observed set,
 *  NOT "real-Core99 coverage proven". Real-Core99 query closure + the
 *  interrupt-map VALUE + the §5-Q8 PIC input numbers are NAMED hand-offs to the
 *  first integration boot (A1) and S2b — out of S2a scope.
 */
#include "openfirmware_ci.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

static inline of_cell P(const void *p) { return (of_cell)(uintptr_t)p; }

/* Build + drive one CHRP CI array; returns rets[0]. */
static of_cell ci(of_ci_context *ctx, const char *svc, int n_args, int n_rets,
                  of_cell a0, of_cell a1, of_cell a2, of_cell a3, int *rc_out)
{
	of_cell arr[OF_CI_MAX_CELLS];
	memset(arr, 0, sizeof(arr));
	arr[0] = P(svc);
	arr[1] = (of_cell)n_args;
	arr[2] = (of_cell)n_rets;
	arr[3] = a0; arr[4] = a1; arr[5] = a2; arr[6] = a3;
	int rc = of_ci_callback(ctx, arr);
	if (rc_out) *rc_out = rc;
	return arr[3 + n_args];
}

/* =====================================================================
 * COMMITTED FIXTURE (P-M1) — derived from FINDINGS-s2a-ofci-dt.md Q-S2a.1,
 * NOT from the implementation's tables.
 * ===================================================================== */

static const char *const FIX_DIRECT_SERVICES[] = {
	"getprop", "getproplen", "finddevice", "close", "setprop", "open",
	"seek", "parent", "exit", "read", "package-to-path",
	"instance-to-package", "write", "instance-to-path", "peer", "test",
	"canon", "quiesce", "nextprop", "child", "claim",
};
static const char *const FIX_CALL_METHODS[] = {
	"read-blocks", "dimensions", "set-hybernot-flag", "claim", "get-key-map",
	"set-colors", "fill-rectangle", "draw-rectangle", "size",
	"instantiate-rtas", "block-size", "write-blocks", "translate", "map",
};
static const char *const FIX_INTERPRET_LITERALS[] = { "key?", "key", "reset-all" };

/* finddevice battery: the SHORT static spellings (ADV-1) the Trampoline issues,
 * plus the one dynamic concrete canonical boot path. */
static const char *const FIX_FINDDEVICE_PATHS[] = {
	"/chosen", "/aliases", "/", "/rtas", "/cpus/@0", "/cpus/@0/l2-cache",
	"/cpus/@0/l2-cache/l2-cache", "/options", "/rom/macos",
	"/pci/mac-io/interrupt-controller",
	"/pci@f2000000/mac-io@c/ata-3@20000/cdrom@0",
};

/* getprop coverage battery: (short path, property) pairs. */
struct prop_q { const char *path; const char *prop; };
static const struct prop_q FIX_GETPROP[] = {
	{ "/",                                "model" },
	{ "/",                                "compatible" },
	{ "/",                                "#address-cells" },
	{ "/memory",                          "reg" },
	{ "/memory",                          "device_type" },
	{ "/AAPL,ROM",                        "reg" },
	{ "/cpus/@0",                         "device_type" },
	{ "/cpus/@0/l2-cache",                "cache-unified" },
	{ "/pci/mac-io",                      "reg" },
	{ "/pci/mac-io",                      "compatible" },
	{ "/pci/mac-io/interrupt-controller", "device_type" },
	{ "/pci/mac-io/interrupt-controller", "#interrupt-cells" },
	{ "/pci/mac-io/escc",                 "reg" },
	{ "/pci/mac-io/via-cuda",             "reg" },
	{ "/rom/macos",                       "AAPL,toolbox-parcels" },
	{ "/chosen",                          "bootpath" },
	{ "/display",                         "display-type" },
	{ "/ethernet",                        "device_type" },
	/* P-M3 PCI-config-probe over-provision (dynamic getprop family) */
	{ "/pci@f2000000/mac-io@c/ata-3@20000", "vendor-id" },
	{ "/pci@f2000000/mac-io@c/ata-3@20000", "device-id" },
	{ "/pci@f2000000/mac-io@c/ata-3@20000", "class-code" },
};

/* call-method recording double (one shared double, registered under all 14). */
static const char *cm_last_method;
static of_ihandle cm_last_ihandle;
static of_cell    cm_last_in[8];
static int        cm_last_n_in;
static int        cm_invocations;

static int cm_double(void *opaque, const char *method, of_ihandle ih,
                     const of_cell *in, int n_in, of_cell *out, int n_out)
{
	(void)opaque;
	cm_last_method = method;
	cm_last_ihandle = ih;
	cm_last_n_in = n_in;
	for (int i = 0; i < n_in && i < 8; i++) cm_last_in[i] = in[i];
	if (n_out >= 2) out[1] = 0xC0FFEE;   /* a method return value */
	cm_invocations++;
	return 0;                            /* catch-result OK */
}

/* recursive nextprop full-tree-walk (G2a.c runtime breadth, not static x1) */
static int walk_node(of_ci_context *ctx, of_phandle ph, int depth)
{
	int props = 0;
	char prev[64]; prev[0] = '\0';
	for (;;) {
		char buf[64]; buf[0] = '\0';
		int rc;
		of_cell r = ci(ctx, "nextprop", 3, 1, ph, P(prev), P(buf), 0, &rc);
		CHECK(rc == OF_CI_OK);
		if ((int)(int64_t)r != 1) break;
		props++;
		/* every enumerated prop must be answerable by getproplen */
		int len = of_dt_getproplen(ctx, ph, buf);
		CHECK(len >= 0);
		strcpy(prev, buf);
		if (props > 64) break; /* safety */
	}
	/* recurse children via the child/peer services */
	int nodes = 1;
	int crc;
	of_cell child = ci(ctx, "child", 1, 1, ph, 0, 0, 0, &crc);
	CHECK(crc == OF_CI_OK);
	of_phandle c = (of_phandle)child;
	while (c != OF_INVALID_PHANDLE && depth < 16) {
		nodes += walk_node(ctx, c, depth + 1);
		int prc;
		of_cell peer = ci(ctx, "peer", 1, 1, c, 0, 0, 0, &prc);
		CHECK(prc == OF_CI_OK);
		c = (of_phandle)peer;
	}
	return nodes;
}

int main()
{
	of_ci_context *ctx = of_ci_create_core99();

	const int N_DIRECT = (int)(sizeof(FIX_DIRECT_SERVICES) / sizeof(char *));
	const int N_METHOD = (int)(sizeof(FIX_CALL_METHODS) / sizeof(char *));
	const int N_LIT    = (int)(sizeof(FIX_INTERPRET_LITERALS) / sizeof(char *));

	/* Fixture provenance guard: the committed battery is the recon's 21/14/3. */
	CHECK(N_DIRECT == 21);
	CHECK(N_METHOD == 14);
	CHECK(N_LIT == 3);

	/* =================================================================
	 * A. Dispatch resolution — G2a.b: unresolved == 0 across 21 + 14 + 3.
	 *    Driven from the COMMITTED fixture (P-M1), not the dispatch table.
	 * ================================================================= */

	/* (a) 21 direct services with a representative arg set each. */
	for (int i = 0; i < N_DIRECT; i++) {
		int rc;
		ci(ctx, FIX_DIRECT_SERVICES[i], 4, 1, 0, 0, 0, 0, &rc);
		CHECK(rc == OF_CI_OK);
	}
	CHECK(of_ci_unresolved_count(ctx) == 0);

	/* (b) 14 call-method names against an opened ihandle + injected double. */
	for (int i = 0; i < N_METHOD; i++)
		CHECK(of_ci_register_method(ctx, FIX_CALL_METHODS[i], cm_double, NULL));

	/* open an instance to call against */
	int orc;
	of_cell ih = ci(ctx, "open", 1, 1,
	                P("/pci/mac-io/via-cuda"), 0, 0, 0, &orc);
	CHECK(orc == OF_CI_OK);
	CHECK(ih != OF_INVALID_IHANDLE);

	for (int i = 0; i < N_METHOD; i++) {
		of_cell arr[OF_CI_MAX_CELLS];
		memset(arr, 0, sizeof(arr));
		/* call-method <name> <ihandle> <in0> <in1> ; n_rets=2 */
		arr[0] = P("call-method");
		arr[1] = 4;                /* method, ihandle, in0, in1 */
		arr[2] = 2;                /* catch-result + 1 return */
		arr[3] = P(FIX_CALL_METHODS[i]);
		arr[4] = ih;
		arr[5] = 0x1000 + i;       /* decoded in0 (e.g. block#) */
		arr[6] = 0x40 + i;         /* decoded in1 (e.g. count)  */
		cm_invocations = 0;
		int rc = of_ci_callback(ctx, arr);
		CHECK(rc == OF_CI_OK);
		/* the double observed the DECODED args (P-M1 substance) */
		CHECK(cm_invocations == 1);
		CHECK(strcmp(cm_last_method, FIX_CALL_METHODS[i]) == 0);
		CHECK(cm_last_ihandle == (of_ihandle)ih);
		CHECK(cm_last_n_in == 2);
		CHECK(cm_last_in[0] == (of_cell)(0x1000 + i));
		CHECK(cm_last_in[1] == (of_cell)(0x40 + i));
		CHECK(arr[7] == 0);        /* rets[0] catch-result */
		CHECK(arr[8] == 0xC0FFEE); /* rets[1] method return */
	}
	CHECK(of_ci_unresolved_count(ctx) == 0);

	/* (c) 3 interpret literals. */
	for (int i = 0; i < N_LIT; i++) {
		int rc;
		ci(ctx, "interpret", 1, 1, P(FIX_INTERPRET_LITERALS[i]), 0, 0, 0, &rc);
		CHECK(rc == OF_CI_OK);
	}
	CHECK(of_ci_unresolved_count(ctx) == 0);

	/* Negative controls: an unknown service / method / literal MUST be
	 * counted UNRESOLVED (proves the gate is not vacuous). */
	{
		int rc;
		ci(ctx, "no-such-service", 0, 1, 0, 0, 0, 0, &rc);
		CHECK(rc == OF_CI_FAIL);
		CHECK(of_ci_unresolved_count(ctx) == 1);

		of_cell arr[OF_CI_MAX_CELLS];
		memset(arr, 0, sizeof(arr));
		arr[0] = P("call-method"); arr[1] = 2; arr[2] = 1;
		arr[3] = P("no-such-method"); arr[4] = ih;
		CHECK(of_ci_callback(ctx, arr) == OF_CI_FAIL);
		CHECK(of_ci_unresolved_count(ctx) == 2);

		ci(ctx, "interpret", 1, 1, P("frobnicate"), 0, 0, 0, &rc);
		CHECK(rc == OF_CI_FAIL);
		CHECK(of_ci_unresolved_count(ctx) == 3);
	}

	/* Fresh context for the coverage half so the negative-control unresolved
	 * count doesn't bleed into G2a.c's assertions. */
	of_ci_destroy(ctx);
	ctx = of_ci_create_core99();

	/* =================================================================
	 * B. DT coverage — G2a.c: every finddevice / getprop / nextprop in the
	 *    OpenBIOS-observed set is answered. (Honest: answers-the-observed-set,
	 *    NOT real-Core99-proven.)
	 * ================================================================= */

	/* finddevice: every SHORT static spelling resolves (ADV-1 component-wise). */
	for (int i = 0; i < (int)(sizeof(FIX_FINDDEVICE_PATHS)/sizeof(char*)); i++) {
		int rc;
		of_cell ph = ci(ctx, "finddevice", 1, 1,
		                P(FIX_FINDDEVICE_PATHS[i]), 0, 0, 0, &rc);
		CHECK(rc == OF_CI_OK);
		CHECK((int64_t)ph != -1);            /* resolved, not a miss */
		CHECK((of_phandle)ph != OF_INVALID_PHANDLE);
	}

	/* ADV-1 substance: the short spelling resolves to the SAME node as the
	 * fully-qualified canonical unit-address form (component-wise match, NOT
	 * exact-string + alias lookup). */
	{
		of_phandle a = of_dt_finddevice(ctx, "/pci/mac-io/interrupt-controller");
		of_phandle b = of_dt_finddevice(ctx,
			"/pci@f2000000/mac-io@c/interrupt-controller@40000");
		CHECK(a != OF_INVALID_PHANDLE);
		CHECK(a == b);
		/* a unit-address-only component (/cpus/@0) resolves the cpu node */
		CHECK(of_dt_finddevice(ctx, "/cpus/@0") != OF_INVALID_PHANDLE);
		/* a genuinely-absent path is a clean miss (service still resolves) */
		int rc;
		of_cell miss = ci(ctx, "finddevice", 1, 1, P("/nonexistent"), 0,0,0,&rc);
		CHECK(rc == OF_CI_OK);
		CHECK((int64_t)miss == -1);
	}

	/* getprop coverage: every (path, prop) pair is answered (len >= 0). */
	for (int i = 0; i < (int)(sizeof(FIX_GETPROP)/sizeof(FIX_GETPROP[0])); i++) {
		of_phandle ph = of_dt_finddevice(ctx, FIX_GETPROP[i].path);
		CHECK(ph != OF_INVALID_PHANDLE);
		char buf[256];
		int rc;
		of_cell len = ci(ctx, "getprop", 4, 1,
		                 ph, P(FIX_GETPROP[i].prop), P(buf), sizeof(buf), &rc);
		CHECK(rc == OF_CI_OK);
		CHECK((int64_t)len >= 0);
		/* getproplen agrees */
		CHECK(of_dt_getproplen(ctx, ph, FIX_GETPROP[i].prop) == (int)(int64_t)len);
	}

	/* A couple of pinned VALUE spot-checks (mutation-resistant, NOT echoes). */
	{
		of_phandle macio = of_dt_finddevice(ctx, "/pci/mac-io");
		char buf[64];
		int rc;
		of_cell len = ci(ctx, "getprop", 4, 1,
		                 macio, P("device_type"), P(buf), sizeof(buf), &rc);
		CHECK(rc == OF_CI_OK);
		CHECK(strcmp(buf, "mac-io") == 0);
		CHECK((int64_t)len == (int)strlen("mac-io") + 1);
		of_phandle pic = of_dt_finddevice(ctx, "/pci/mac-io/interrupt-controller");
		ci(ctx, "getprop", 4, 1, pic, P("device_type"), P(buf), sizeof(buf), &rc);
		CHECK(strcmp(buf, "open-pic") == 0);
	}

	/* P-M2 carve-out: interrupt-map / -mask resolve + getproplen NON-FINAL;
	 * the tuple cell content is NEVER asserted. */
	{
		of_phandle pic = of_dt_finddevice(ctx, "/pci/mac-io/interrupt-controller");
		CHECK(pic != OF_INVALID_PHANDLE);
		int rc;
		of_cell l1 = ci(ctx, "getproplen", 2, 1, pic, P("interrupt-map"), 0,0,&rc);
		CHECK(rc == OF_CI_OK);
		CHECK((int64_t)l1 >= 0);                                 /* query resolves */
		CHECK(of_dt_prop_is_nonfinal(ctx, pic, "interrupt-map"));/* marked NON-FINAL */
		of_cell l2 = ci(ctx, "getproplen", 2, 1, pic,
		                P("interrupt-map-mask"), 0, 0, &rc);
		CHECK(rc == OF_CI_OK);
		CHECK((int64_t)l2 >= 0);
		CHECK(of_dt_prop_is_nonfinal(ctx, pic, "interrupt-map-mask"));
		/* a FINAL property is NOT flagged non-final (the marker discriminates) */
		CHECK(!of_dt_prop_is_nonfinal(ctx, pic, "device_type"));
	}

	/* nextprop full-tree-walk (G2a.c runtime breadth, NOT static x1). */
	{
		of_phandle root = of_dt_finddevice(ctx, "/");
		CHECK(root != OF_INVALID_PHANDLE);
		int nodes = walk_node(ctx, root, 0);
		CHECK(nodes >= 18);   /* the full Core99 + scaffolding node set */
	}

	/* =================================================================
	 * C. S2a-impl adversary regression rows (each FAILS before its fix).
	 * ================================================================= */

	/* C1 — setprop must actually mutate (write-then-read-back). Before the fix
	 * setprop fell into the generic accept-else and reported success while
	 * storing nothing, so getprop returned stale/absent data. */
	{
		of_phandle macos = of_dt_finddevice(ctx, "/rom/macos");
		CHECK(macos != OF_INVALID_PHANDLE);
		/* (a) overwrite an EXISTING property and read the new value back */
		const unsigned char newval[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
		int rc;
		of_cell wlen = ci(ctx, "setprop", 4, 1, macos,
		                  P("AAPL,toolbox-parcels"), P(newval), 4, &rc);
		CHECK(rc == OF_CI_OK);
		CHECK((int64_t)wlen == 4);            /* IEEE-1275 returns the new length */
		unsigned char rbuf[16]; memset(rbuf, 0, sizeof(rbuf));
		of_cell rlen = ci(ctx, "getprop", 4, 1, macos,
		                  P("AAPL,toolbox-parcels"), P(rbuf), sizeof(rbuf), &rc);
		CHECK(rc == OF_CI_OK);
		CHECK((int64_t)rlen == 4);
		CHECK(memcmp(rbuf, newval, 4) == 0);  /* the blind spot: read-back */
		/* (b) create a BRAND-NEW property name and read it back */
		const unsigned char rsv[8] = { 1,2,3,4,5,6,7,8 };
		CHECK(of_dt_getproplen(ctx, macos, "AAPL,reserved-memory-space") == -1);
		ci(ctx, "setprop", 4, 1, macos,
		   P("AAPL,reserved-memory-space"), P(rsv), 8, &rc);
		CHECK(rc == OF_CI_OK);
		CHECK(of_dt_getproplen(ctx, macos, "AAPL,reserved-memory-space") == 8);
		unsigned char rbuf2[16]; memset(rbuf2, 0, sizeof(rbuf2));
		ci(ctx, "getprop", 4, 1, macos,
		   P("AAPL,reserved-memory-space"), P(rbuf2), sizeof(rbuf2), &rc);
		CHECK(memcmp(rbuf2, rsv, 8) == 0);
	}

	/* C2 — finddevice must honor the query's @unit-address (ADV-1). Two
	 * same-name siblings (disk@0 / disk@1) under /scratch: the addressed query
	 * must resolve the RIGHT one (before the fix both resolved disk@0), the
	 * omit query resolves the first. */
	{
		of_phandle d0 = of_dt_finddevice(ctx, "/scratch/disk@0");
		of_phandle d1 = of_dt_finddevice(ctx, "/scratch/disk@1");
		of_phandle dom = of_dt_finddevice(ctx, "/scratch/disk");
		CHECK(d0 != OF_INVALID_PHANDLE);
		CHECK(d1 != OF_INVALID_PHANDLE);
		CHECK(d0 != d1);          /* the addressed queries resolve DISTINCT nodes */
		CHECK(dom == d0);         /* ADV-1: omit query resolves the first sibling */
	}

	/* C3 — nextprop returns -1 (invalid previous), not 0 (end), for an unknown
	 * non-empty `previous`. */
	{
		of_phandle macio = of_dt_finddevice(ctx, "/pci/mac-io");
		CHECK(macio != OF_INVALID_PHANDLE);
		char buf[64];
		int rc;
		of_cell r = ci(ctx, "nextprop", 3, 1,
		               macio, P("nonexistent-prop"), P(buf), 0, &rc);
		CHECK(rc == OF_CI_OK);
		CHECK((int64_t)r == -1);  /* invalid previous (was 0 before the fix) */
		/* the normal "" -> first walk still works */
		of_cell r0 = ci(ctx, "nextprop", 3, 1, macio, P(""), P(buf), 0, &rc);
		CHECK((int64_t)r0 == 1);
	}

	of_ci_destroy(ctx);

	printf("test_openfirmware_ci: RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
