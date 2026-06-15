/*
 *  test_trampoline_ofci_wiring.cpp - SS_M18 Stage 2b T4 micro-test (G(T4)).
 *
 *  Operation NewSheep. Drives a planted guest BE-32 CHRP `call-method` array
 *  END-TO-END through the bridge the live boot uses:
 *      guest BE-32 array  -> tramp_ofci_marshal (the r5 shim marshaller)
 *                         -> of_ci_callback (the dispatcher)
 *                         -> the pinned of_call_method_fn backend
 *  exactly as ss_ofci_shim_invoke -> of_ci_callback -> backend will at the gated
 *  boot, but WITHOUT a live emulator (flat mock RAM + injected xlate). Asserts the
 *  rev-2 G(T4) contract:
 *    (i)   the dispatcher routes by service/method name to the PINNED backend
 *          (a /mmu `translate` reaches mmu_backend; a `getprop` resolves in the DT);
 *    (ii)  the /mmu recording stub LOGS the request ([S2B-MMU-STUB]) and returns
 *          its identity claim (translate phys == virt);
 *    (iii) a planted /mmu `claim` of the shim's reserved range trips the
 *          [S2B-SHIM-COLLIDE] tripwire (Stop-rule #13).
 *
 *  Standalone: links the pure marshalling core + openfirmware_ci.cpp +
 *  trampoline_ofci_backends.cpp; NO emulator/kpx_cpu dependency. Built with
 *  -DTRAMPOLINE_OFCI_SHIM_STANDALONE_TEST (the emulator shim wrapper excluded).
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "trampoline_ofci_shim.h"
#include "trampoline_ofci_backends.h"
#include "trampoline_loader.h"     /* TRAMP_SHIM_ENTRY */
#include "openfirmware_ci.h"

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg) do { g_checks++; if (!(cond)) { \
	g_fail++; fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); } } while (0)

/* ---- flat mock RAM + guest<->host translation (mirrors the shim test) ------- */
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
	g_ram[off + 0] = (uint8_t)(v >> 24); g_ram[off + 1] = (uint8_t)(v >> 16);
	g_ram[off + 2] = (uint8_t)(v >> 8);  g_ram[off + 3] = (uint8_t)(v);
}
static uint32_t get_be32(uint32_t off)
{
	return ((uint32_t)g_ram[off + 0] << 24) | ((uint32_t)g_ram[off + 1] << 16) |
	       ((uint32_t)g_ram[off + 2] << 8)  | (uint32_t)g_ram[off + 3];
}
static void put_str(uint32_t off, const char *s) { strcpy((char *)(g_ram + off), s); }

int main(void)
{
	char err[160];

	of_ci_context *ctx = of_ci_create_core99();
	CHECK(ctx != NULL, "of_ci_create_core99 context built");
	int n = tramp_ofci_install_backends(ctx);
	CHECK(n == 11, "install_backends registered the 11 pinned call-method names");

	/* ---- (i)+(ii): /mmu translate routes to the MINIMALLY-REAL backend ------
	 * SS_M18 S1-bringup: the /mmu backend is V=P (phys==virt) for addresses
	 * inside a published guest RAM/ROM aperture, and NOT-MAPPED outside it.
	 * Publish a RAM window covering VIRT so the in-aperture identity path is
	 * exercised; a second sub-case asserts the out-of-aperture NOT-MAPPED path.
	 * call-method array at 0x1000:
	 *   [0]=0x2000(->"call-method") [1]=n_args=3 [2]=n_rets=4
	 *   [3]=0x2100(->"translate" method name) [4]=0x7 ihandle
	 *   [5]=0x68001234 virt(scalar nested arg0)
	 *   [6]=catch [7]=phys [8]=mode [9]=out3 (rets) -- the real 9.0.1 Trampoline
	 *   passes n_rets=4. out[3] CARRIES phys on success (the IC vectormasktable
	 *   marshaller 0x20dcc0 routes out[3] -> r4=&G->[0xa8] expecting phys; proven by
	 *   the 0x011830a8 watchpoint 2026-06-15). A nonzero phys is also Forth-truthy
	 *   for the boot-info success branch; a NOT-MAPPED miss writes out[3]=0 (false). */
	{
		/* RAM window [0x68000000, +0x01000000) covers VIRT below; ROM unused. */
		tramp_ofci_set_mmu_extent(0x68000000u, 0x01000000u, 0x50000000u, 0x00500000u);

		memset(g_ram, 0, sizeof(g_ram));
		put_str(0x2000, "call-method");
		put_str(0x2100, "translate");
		const uint32_t VIRT = 0x68001234u;
		put_be32(0x1000 + 4 * 0, 0x2000);
		put_be32(0x1000 + 4 * 1, 3);
		put_be32(0x1000 + 4 * 2, 4);
		put_be32(0x1000 + 4 * 3, 0x2100);
		put_be32(0x1000 + 4 * 4, 0x7);
		put_be32(0x1000 + 4 * 5, VIRT);
		put_be32(0x1000 + 4 * 6, 0xAAAAAAAA);   /* catch (overwritten) */
		put_be32(0x1000 + 4 * 7, 0xAAAAAAAA);   /* phys */
		put_be32(0x1000 + 4 * 8, 0xAAAAAAAA);   /* mode */
		put_be32(0x1000 + 4 * 9, 0xAAAAAAAA);   /* out3 (= phys on hit) */

		int out_ret = -999;
		int rc = tramp_ofci_marshal(of_ci_callback, ctx, 0x1000, mock_xlate, NULL,
		                            &out_ret, err, sizeof(err));
		CHECK(rc == 0, "marshal of /mmu translate call-method succeeded");
		CHECK(out_ret == OF_CI_OK, "dispatcher resolved call-method -> backend (OF_CI_OK)");
		/* catch-result cell == 0 (the backend's success code), proving the dispatcher
		 * routed to a registered backend (an UNREGISTERED method -> OF_CI_FAIL). */
		CHECK(get_be32(0x1000 + 4 * 6) == 0, "catch-result == 0 (routed to mmu_backend)");
		/* (ii) V=P identity: phys == virt written back BE-32 for in-aperture virt */
		CHECK(get_be32(0x1000 + 4 * 7) == VIRT, "/mmu translate returned phys == virt (V=P in-aperture)");
		CHECK(get_be32(0x1000 + 4 * 8) != 0, "/mmu translate returned a non-zero cacheable-RW mode");
		CHECK(get_be32(0x1000 + 4 * 9) == VIRT, "/mmu translate out[3] carries phys==virt in-aperture (IC marshaller routes out[3] to r4=&G->[0xa8]; also Forth-truthy for boot-info)");
		CHECK(of_ci_unresolved_count(ctx) == 0, "translate did NOT bump the unresolved counter");

		/* (ii-b) out-of-aperture virt -> NOT-MAPPED (phys==0), honest about the
		 * V=P boundary (does NOT fabricate a PA). */
		const uint32_t OOB = 0x90000000u;
		put_be32(0x1000 + 4 * 5, OOB);
		put_be32(0x1000 + 4 * 7, 0xAAAAAAAA);
		put_be32(0x1000 + 4 * 9, 0xAAAAAAAA);   /* out3 (overwritten -> 0/false on miss) */
		out_ret = -999;
		rc = tramp_ofci_marshal(of_ci_callback, ctx, 0x1000, mock_xlate, NULL,
		                        &out_ret, err, sizeof(err));
		CHECK(rc == 0 && out_ret == OF_CI_OK, "marshal of out-of-aperture translate succeeded");
		CHECK(get_be32(0x1000 + 4 * 7) == 0, "/mmu translate out-of-aperture -> phys == 0 (NOT-MAPPED)");
		CHECK(get_be32(0x1000 + 4 * 9) == 0, "/mmu translate out-of-aperture out[3]==0 (NOT-MAPPED -> false; boot-info discards)");
	}

	/* ---- (i): getprop resolves in the DT (no backend) ----------------------- */
	{
		memset(g_ram, 0, sizeof(g_ram));
		put_str(0x2000, "finddevice");
		put_str(0x3000, "/pci/mac-io");
		put_be32(0x1000 + 4 * 0, 0x2000);
		put_be32(0x1000 + 4 * 1, 1);
		put_be32(0x1000 + 4 * 2, 1);
		put_be32(0x1000 + 4 * 3, 0x3000);
		put_be32(0x1000 + 4 * 4, 0);
		int out_ret = -999;
		int rc = tramp_ofci_marshal(of_ci_callback, ctx, 0x1000, mock_xlate, NULL,
		                            &out_ret, err, sizeof(err));
		CHECK(rc == 0 && out_ret == OF_CI_OK, "finddevice('/pci/mac-io') resolved in the DT");
		uint32_t ph = get_be32(0x1000 + 4 * 4);
		CHECK(ph != 0 && ph != 0xFFFFFFFFu, "finddevice yielded a real mac-io phandle");

		/* getprop(ph, "device_type", buf, buflen) through the same path */
		memset(g_ram, 0, sizeof(g_ram));
		put_str(0x2000, "getprop");
		put_str(0x3000, "device_type");
		put_be32(0x1000 + 4 * 0, 0x2000);
		put_be32(0x1000 + 4 * 1, 4);
		put_be32(0x1000 + 4 * 2, 1);
		put_be32(0x1000 + 4 * 3, ph);
		put_be32(0x1000 + 4 * 4, 0x3000);
		put_be32(0x1000 + 4 * 5, 0x4000);   /* buf */
		put_be32(0x1000 + 4 * 6, 0x40);     /* buflen */
		put_be32(0x1000 + 4 * 7, 0);
		rc = tramp_ofci_marshal(of_ci_callback, ctx, 0x1000, mock_xlate, NULL,
		                        &out_ret, err, sizeof(err));
		CHECK(rc == 0 && out_ret == OF_CI_OK, "getprop resolved in the DT (no backend)");
		int len = (int)get_be32(0x1000 + 4 * 7);
		CHECK(len > 0, "getprop returned a property length");
		CHECK(strcmp((const char *)(g_ram + 0x4000), "mac-io") == 0,
		      "getprop wrote 'mac-io' through the aliased guest buffer");
		CHECK(of_ci_unresolved_count(ctx) == 0, "DT queries left unresolved counter at 0");
	}

	/* ---- (iii): /mmu claim of the reserved shim page trips the tripwire ------
	 * The stub leaves the STOP hook NULL (default), so the collision is observable
	 * via the catch code + tramp_ofci_collision_count() instead of an abort. */
	{
		CHECK(tramp_ofci_collision_count() == 0, "no collision before the planted claim");
		memset(g_ram, 0, sizeof(g_ram));
		put_str(0x2000, "call-method");
		put_str(0x2100, "claim");
		put_be32(0x1000 + 4 * 0, 0x2000);
		put_be32(0x1000 + 4 * 1, 4);             /* n_args: method, ihandle, base, size */
		put_be32(0x1000 + 4 * 2, 2);             /* n_rets: catch, base */
		put_be32(0x1000 + 4 * 3, 0x2100);        /* "claim" */
		put_be32(0x1000 + 4 * 4, 0x9);           /* ihandle */
		put_be32(0x1000 + 4 * 5, TRAMP_SHIM_ENTRY + 0x10); /* base INSIDE the shim page */
		put_be32(0x1000 + 4 * 6, 0x100);         /* size */
		put_be32(0x1000 + 4 * 7, 0);             /* catch */
		put_be32(0x1000 + 4 * 8, 0);             /* base ret */

		int out_ret = -999;
		int rc = tramp_ofci_marshal(of_ci_callback, ctx, 0x1000, mock_xlate, NULL,
		                            &out_ret, err, sizeof(err));
		CHECK(rc == 0, "marshal of the colliding /mmu claim completed (no marshal refusal)");
		CHECK(out_ret == OF_CI_OK, "dispatcher still routed the claim to mmu_backend");
		/* the catch-result cell carries the collide code (the tripwire's signal) */
		CHECK((int32_t)get_be32(0x1000 + 4 * 7) == TRAMP_OFCI_MMU_COLLIDE,
		      "catch-result == TRAMP_OFCI_MMU_COLLIDE (the shim-page tripwire fired)");
		CHECK(tramp_ofci_collision_count() == 1, "[S2B-SHIM-COLLIDE] tripwire fired exactly once");
	}

	/* ---- (iii control): a /mmu claim well OUTSIDE the shim page does NOT trip - */
	{
		memset(g_ram, 0, sizeof(g_ram));
		put_str(0x2000, "call-method");
		put_str(0x2100, "claim");
		put_be32(0x1000 + 4 * 0, 0x2000);
		put_be32(0x1000 + 4 * 1, 4);
		put_be32(0x1000 + 4 * 2, 2);
		put_be32(0x1000 + 4 * 3, 0x2100);
		put_be32(0x1000 + 4 * 4, 0x9);
		put_be32(0x1000 + 4 * 5, 0x68000000);    /* high kernel arena, NOT the shim page */
		put_be32(0x1000 + 4 * 6, 0x1000);
		put_be32(0x1000 + 4 * 7, 0);
		put_be32(0x1000 + 4 * 8, 0);
		int out_ret = -999;
		int rc = tramp_ofci_marshal(of_ci_callback, ctx, 0x1000, mock_xlate, NULL,
		                            &out_ret, err, sizeof(err));
		CHECK(rc == 0 && out_ret == OF_CI_OK, "non-colliding /mmu claim resolved");
		CHECK(get_be32(0x1000 + 4 * 7) == 0, "non-colliding claim catch-result == 0");
		CHECK(get_be32(0x1000 + 4 * 8) == 0x68000000, "claim returned the requested base (identity)");
		CHECK(tramp_ofci_collision_count() == 1, "no NEW collision from the safe claim");
	}

	of_ci_destroy(ctx);
	printf("test_trampoline_ofci_wiring: %d checks, %d failures\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
