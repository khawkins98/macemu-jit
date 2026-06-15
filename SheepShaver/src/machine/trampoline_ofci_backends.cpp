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

/* ---- /mmu MINIMALLY-REAL backend state (SS_M18 S1-bringup) -----------------
 * The guest-physical apertures the V=P identity translate/map honor, published
 * by the gated launch seam (tramp_ofci_set_mmu_extent). Until set, all extents
 * are zero -> the standalone unit test keeps the historical identity behaviour
 * for in-aperture==[low guest RAM] and falls through to NOT-MAPPED logging for
 * the rest (it never sets extents, so its assertions on the recorded calls are
 * unchanged in the success path).
 *
 * NON-ACCEPTANCE: V=P is a BRINGUP approximation. It is correct only while the
 * NK's intended map is identity; a non-identity SR/BAT install must route to
 * S1's paged_mmu_translate. See the S1 Task-0 plan. */
static uint32_t g_ram_base = 0, g_ram_size = 0;   /* NATMEM/DT RAM window      */
static uint32_t g_rom_base = 0, g_rom_size = 0;   /* ROM aperture              */
static int      g_translate_calls = 0;
static int      g_map_calls       = 0;

/* Recorded /mmu map table. `map` RECORDS each (virt,phys,size,mode) request so a
 * later `translate` of a mapped virtual range returns the RECORDED phys (honoring
 * a non-identity phys!=virt map) rather than blind V=P identity. This is the point
 * where V=P ends — the first real divergence the Trampoline establishes via map
 * (e.g. mapping a low virtual window onto an MMIO physical aperture). Lookup order
 * in translate: recorded mappings FIRST, then V=P identity for the RAM/ROM
 * apertures, then NOT-MAPPED. Still NON-ACCEPTANCE: this records OF-level /mmu map
 * fidelity; it is NOT the live-NK SR/BAT translation window (owed to S1). */
#define MMU_MAP_MAX 32
static struct { uint32_t virt, phys, size, mode; } g_maps[MMU_MAP_MAX];
static int g_map_count = 0;

/* A sane cacheable, read/write WIMG+PP mode default for V=P RAM/ROM at this
 * early stage (WIMG=0b0010 M=coherent, PP=0b10 RW). Bringup placeholder — the
 * real per-EA mode comes from the NK's PTE/BAT once S1's live MMU is wired. */
#define MMU_MODE_CACHEABLE_RW  0x12u

void tramp_ofci_set_stop_fn(tramp_ofci_stop_fn fn) { g_stop_fn = fn; }
int  tramp_ofci_collision_count(void)              { return g_collisions; }

void tramp_ofci_set_mmu_extent(uint32_t ram_base, uint32_t ram_size,
                               uint32_t rom_base, uint32_t rom_size)
{
	g_ram_base = ram_base; g_ram_size = ram_size;
	g_rom_base = rom_base; g_rom_size = rom_size;
	g_map_count = 0;                       /* fresh launch -> drop recorded maps */
}

/* If `virt` falls in a recorded map range, return 1 and write the recorded phys
 * (offset-preserving) + mode through the out params. Most-recent record wins on
 * overlap (the Trampoline re-maps a range only to change it). */
static int mmu_lookup_recorded(uint32_t virt, uint32_t *out_phys, uint32_t *out_mode)
{
	for (int i = g_map_count - 1; i >= 0; i--) {
		uint32_t b = g_maps[i].virt, e = b + g_maps[i].size;
		if (e < b) e = 0xffffffffu;                 /* size wrap -> clamp */
		if (virt >= b && virt < e) {
			if (out_phys) *out_phys = g_maps[i].phys + (virt - b);
			if (out_mode) *out_mode = g_maps[i].mode;
			return 1;
		}
	}
	return 0;
}

int tramp_ofci_mmu_translate_count(void) { return g_translate_calls; }
int tramp_ofci_mmu_map_count(void)       { return g_map_calls; }

/* Is `virt` inside a V=P identity aperture? At this pre-NK-install stage the
 * Trampoline addresses (a) low guest-physical [0, RAMSize) where the loaded
 * MacOS.elf image (~0x200000), BSS, and the claim arena (0x01000000..) live,
 * (b) the DT-declared RAM window [RAMBase, RAMBase+RAMSize), and (c) the ROM
 * aperture. All three are identity-mapped (phys==virt) before the NK programs
 * its own segments. Returns 1 if identity-valid. */
static int mmu_addr_is_identity(uint32_t virt)
{
	if (g_ram_size && virt < g_ram_size)                       return 1; /* low GPA */
	if (g_ram_size && virt >= g_ram_base &&
	    virt < g_ram_base + g_ram_size)                        return 1; /* RAM win */
	if (g_rom_size && virt >= g_rom_base &&
	    virt < g_rom_base + g_rom_size)                        return 1; /* ROM     */
	return 0;
}

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
		/* translate (virt -- catch phys mode): MINIMALLY-REAL V=P.
		 * If virt is in a guest RAM/ROM identity aperture -> phys=virt with a
		 * cacheable-RW mode. Else -> NOT-MAPPED (phys=0, mode=0) and a distinct
		 * log: we do NOT fabricate a PA for an address outside the V=P regime
		 * (Stop-rule #2/#3). A NOT-MAPPED hit here is itself the FINDING that the
		 * Trampoline/NK has moved past identity -> S1's live paged MMU is owed. */
		uint32_t virt = (n_in >= 1) ? (uint32_t)in[0] : 0;
		g_translate_calls++;
		uint32_t rec_phys = 0, rec_mode = 0;
		if (mmu_lookup_recorded(virt, &rec_phys, &rec_mode)) {
			/* Honor a recorded (possibly non-identity) /mmu map FIRST. */
			if (n_out >= 2) out[1] = (of_cell)rec_phys;
			if (n_out >= 3) out[2] = (of_cell)rec_mode;
			fprintf(stderr, "[S2B-MMU-MAP] translate virt=0x%08x -> phys=0x%08x "
			        "mode=0x%08x (recorded map) #%d\n",
			        virt, rec_phys, rec_mode, g_translate_calls);
		} else if (mmu_addr_is_identity(virt)) {
			if (n_out >= 2) out[1] = (of_cell)virt;            /* phys = virt   */
			if (n_out >= 3) out[2] = (of_cell)MMU_MODE_CACHEABLE_RW;
			fprintf(stderr, "[S2B-MMU-MINREAL] translate virt=0x%08x -> phys=0x%08x "
			        "mode=0x%02x (V=P identity, bringup) #%d\n",
			        virt, virt, MMU_MODE_CACHEABLE_RW, g_translate_calls);
		} else {
			if (n_out >= 2) out[1] = (of_cell)0;               /* no phys       */
			if (n_out >= 3) out[2] = (of_cell)0;               /* no mode       */
			fprintf(stderr, "[S2B-MMU-MINREAL] translate virt=0x%08x -> NOT-MAPPED "
			        "(outside V=P aperture RAM[0x%08x,+0x%08x)/ROM[0x%08x,+0x%08x); "
			        "live (SR/BAT/SDR1) MMU owed to S1) #%d\n",
			        virt, g_ram_base, g_ram_size, g_rom_base, g_rom_size,
			        g_translate_calls);
		}
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
		/* Apple /mmu map ( mode size virt phys -- ) : RECORD the (virt,phys,size,
		 * mode) so a later translate honors it. Arg order pinned from the real
		 * Trampoline trace: the claim that precedes the map returns virt=in[2],
		 * size=in[1]; phys=in[3] is the (MMIO/RAM) physical base, mode=in[0] the
		 * WIMG+PP (0x2a = cache-inhibited+guarded for an MMIO window). The earlier
		 * (phys virt size mode) reading mislabeled mode as phys.
		 *
		 * A phys==virt map is the identity no-op the NATMEM page already provides.
		 * A phys!=virt map is the FIRST real V=P divergence (e.g. low virt -> MMIO
		 * phys): we RECORD it in g_maps so translate returns the recorded phys. We
		 * do NOT remap host pages here (that is S1's live window) — this is OF-level
		 * map fidelity only (NON-ACCEPTANCE). Tripwire fires on a virtual range
		 * overlapping the reserved shim page. */
		uint32_t mode = (n_in >= 1) ? (uint32_t)in[0] : 0;
		uint32_t size = (n_in >= 2) ? (uint32_t)in[1] : 1;
		uint32_t virt = (n_in >= 3) ? (uint32_t)in[2] : 0;
		uint32_t phys = (n_in >= 4) ? (uint32_t)in[3] : 0;
		g_map_calls++;
		if (range_hits_shim(virt, size))
			return shim_collide(method, virt, size);
		if (g_map_count < MMU_MAP_MAX) {
			g_maps[g_map_count].virt = virt; g_maps[g_map_count].phys = phys;
			g_maps[g_map_count].size = size; g_maps[g_map_count].mode = mode;
			g_map_count++;
		}
		fprintf(stderr, "[S2B-MMU-MAP] map virt=0x%08x -> phys=0x%08x size=0x%08x "
		        "mode=0x%08x (%s; recorded #%d/%d) #%d\n",
		        virt, phys, size, mode,
		        (phys == virt) ? "V=P identity" : "NON-IDENTITY",
		        g_map_count, MMU_MAP_MAX, g_map_calls);
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
