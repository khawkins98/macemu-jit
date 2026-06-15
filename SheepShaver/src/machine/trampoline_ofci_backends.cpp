/*
 *  trampoline_ofci_backends.cpp - SS_M18 Stage 2b, T4: the pinned OF-CI
 *  call-method backends. See include/trampoline_ofci_backends.h for the contract
 *  + the backend ownership map.
 *
 *  PURE (no emulator/kpx_cpu dependency): the of_call_method_fn backends log +
 *  return identity/plausible/benign results and the /mmu stub trips
 *  [S2B-SHIM-COLLIDE] on a reserved-shim-page overlap. Compiled into the binary
 *  AND exercised standalone by test_trampoline_ofci_wiring.cpp.
 *
 *  of_ci_callback's call-method path hands us (opaque, method, ihandle, in/n_in,
 *  out/n_out); it OVERWRITES out[0] with our return (the OF catch-result) AFTER we
 *  return, so method return VALUES go in out[1..] (out[0] is the catch slot).
 */

#include "trampoline_ofci_backends.h"
#include "trampoline_loader.h"   /* TRAMP_SHIM_ENTRY (macro only - no link dep) */

#include <stdio.h>
#include <string.h>

/* The shim's reserved guest page [TRAMP_SHIM_ENTRY, +0x1000): the T3 launch seam
 * writes the EXEC_NATIVE intercept opcode here (= r5). If the Trampoline ever
 * claims/maps a range overlapping it, the recording stub trips the tripwire. */
#define SHIM_LO  (TRAMP_SHIM_ENTRY)
#define SHIM_HI  (TRAMP_SHIM_ENTRY + 0x1000u)

static tramp_ofci_stop_fn g_stop_fn   = NULL;
static int                g_collisions = 0;

void tramp_ofci_set_stop_fn(tramp_ofci_stop_fn fn) { g_stop_fn = fn; }
int  tramp_ofci_collision_count(void)              { return g_collisions; }

/* Does the half-open guest range [base, base+size) overlap the reserved shim
 * page? size==0 is treated as a single byte (a point claim still trips). */
static int range_hits_shim(uint32_t base, uint32_t size)
{
	if (size == 0) size = 1;
	uint32_t end = base + size;
	if (end < base) end = 0xffffffffu;          /* wrap: clamp to top of memory */
	return (base < SHIM_HI) && (end > SHIM_LO);
}

static int shim_collide(const char *method, uint32_t base, uint32_t size)
{
	g_collisions++;
	fprintf(stderr,
	        "[S2B-SHIM-COLLIDE] /mmu %s claims/maps [0x%08x,0x%08x) overlapping the "
	        "reserved r5-shim page [0x%08x,0x%08x) - STOP (Stop-rule #13)\n",
	        method, base, base + (size ? size : 1), (uint32_t)SHIM_LO, (uint32_t)SHIM_HI);
	if (g_stop_fn) g_stop_fn();                  /* live boot: abort(); test: NULL */
	return TRAMP_OFCI_MMU_COLLIDE;
}

/* ---- /mmu RECORDING STUB (NON-ACCEPTANCE; real backend owed to S1) ---------- */
static int mmu_backend(void *opaque, const char *method, of_ihandle ih,
                       const of_cell *in, int n_in, of_cell *out, int n_out)
{
	(void)opaque;
	/* Log every request - the recording half of the stub. */
	fprintf(stderr, "[S2B-MMU-STUB] /mmu %s ih=%u n_in=%d", method, (unsigned)ih, n_in);
	for (int i = 0; i < n_in; i++)
		fprintf(stderr, " in[%d]=0x%08x", i, (uint32_t)in[i]);
	fprintf(stderr, "  (NON-ACCEPTANCE recording stub - real /mmu owed to S1)\n");

	if (strcmp(method, "translate") == 0) {
		/* translate (virt -- catch phys mode): identity phys = virt. */
		uint32_t virt = (n_in >= 1) ? (uint32_t)in[0] : 0;
		if (n_out >= 2) out[1] = (of_cell)virt;     /* phys = virt (identity) */
		if (n_out >= 3) out[2] = (of_cell)0;        /* mode placeholder */
		return 0;
	}
	if (strcmp(method, "claim") == 0) {
		/* claim (virt size align -- catch base): return the requested base. */
		uint32_t base = (n_in >= 1) ? (uint32_t)in[0] : 0;
		uint32_t size = (n_in >= 2) ? (uint32_t)in[1] : 1;
		if (range_hits_shim(base, size))
			return shim_collide(method, base, size);
		if (n_out >= 2) out[1] = (of_cell)base;     /* base = requested */
		return 0;
	}
	if (strcmp(method, "map") == 0) {
		/* map (phys virt size mode -- catch): benign ok; tripwire on the mapped
		 * virtual range overlapping the reserved shim page. */
		uint32_t virt = (n_in >= 2) ? (uint32_t)in[1] : 0;
		uint32_t size = (n_in >= 3) ? (uint32_t)in[2] : 1;
		if (range_hits_shim(virt, size))
			return shim_collide(method, virt, size);
		return 0;
	}
	/* Any other /mmu method is still recorded; benign ok. */
	return 0;
}

/* ---- disk S4 STUB (deferred; NK reached before disk IM-init) ---------------- */
static int disk_backend(void *opaque, const char *method, of_ihandle ih,
                        const of_cell *in, int n_in, of_cell *out, int n_out)
{
	(void)opaque; (void)ih; (void)in; (void)n_in;
	fprintf(stderr, "[S2B-DISK-STUB] disk %s (S4 deferred - NK reached before disk "
	        "IM-init matters)\n", method);
	if (strcmp(method, "block-size") == 0) {
		if (n_out >= 2) out[1] = (of_cell)512;      /* a plausible block size */
	}
	/* read-blocks/write-blocks: benign ok, no data moved (buffer aliases guest). */
	return 0;
}

/* ---- display NO-OP RESOLVE --------------------------------------------------- */
static int display_backend(void *opaque, const char *method, of_ihandle ih,
                           const of_cell *in, int n_in, of_cell *out, int n_out)
{
	(void)opaque; (void)ih; (void)in; (void)n_in;
	if (strcmp(method, "dimensions") == 0) {
		if (n_out >= 2) out[1] = (of_cell)640;
		if (n_out >= 3) out[2] = (of_cell)480;
	}
	/* set-colors/fill-rectangle/draw-rectangle: no-op ok (resolve, no framebuffer). */
	return 0;
}

/* ---- instantiate-rtas MINIMAL STUB ------------------------------------------ */
static int rtas_backend(void *opaque, const char *method, of_ihandle ih,
                        const of_cell *in, int n_in, of_cell *out, int n_out)
{
	(void)opaque; (void)ih; (void)method; (void)in; (void)n_in;
	/* instantiate-rtas (real-base -- catch rtas-base): minimal handle. */
	if (n_out >= 2) out[1] = (of_cell)0;            /* rtas base placeholder */
	return 0;
}

int tramp_ofci_install_backends(of_ci_context *ctx)
{
	if (!ctx) return 0;
	static const struct { const char *m; of_call_method_fn fn; } TBL[] = {
		/* /mmu recording stub */
		{ "translate",        mmu_backend },
		{ "claim",            mmu_backend },
		{ "map",              mmu_backend },
		/* disk S4 stub */
		{ "read-blocks",      disk_backend },
		{ "write-blocks",     disk_backend },
		{ "block-size",       disk_backend },
		/* display no-op resolve */
		{ "dimensions",       display_backend },
		{ "set-colors",       display_backend },
		{ "fill-rectangle",   display_backend },
		{ "draw-rectangle",   display_backend },
		/* RTAS minimal stub */
		{ "instantiate-rtas", rtas_backend },
	};
	int n = 0;
	for (unsigned i = 0; i < sizeof(TBL) / sizeof(TBL[0]); i++)
		if (of_ci_register_method(ctx, TBL[i].m, TBL[i].fn, NULL)) n++;
	return n;
}
