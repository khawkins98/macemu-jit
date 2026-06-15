/*
 *  test_dolphin_bat_arena.cpp - Unit tests for the Dolphin Dynamic-BAT shadow-arena
 *  (SS_M18 S1, standalone). Links dolphin_bat_arena.cpp + paged_mmu.cpp only — no
 *  kpx_cpu, no emulator, no boot.
 *
 *  WHAT IS VALIDATED (the discipline gate):
 *    (A) PURE  — BAT pair decode (BL->size, field extraction) and the
 *        CanCreateHostMappingForGuestPages-equivalent feasibility gate, incl. the
 *        COARSE 256 MB block case and an adversarial misaligned-PA slow-path case.
 *    (B) ORACLE — for every test EA the arena's host mapping AGREES with
 *        paged_mmu_translate(): a sentinel stamped at phys[PA] (PA from the oracle)
 *        is read back through a BARE access at window[EA]. This binds the
 *        shadow-arena to the reference translator (Discriminator-A ladder item 2).
 *        Cases: a single coarse DBAT block, the COARSE "DBAT-covers-RAM+ROM" two-
 *        block case, a context switch (re-program BAT -> new PA), and identity
 *        revert when coverage is withdrawn.
 *    (C) HOOK  — a replay-fixture loader for the real NK MMU-trace oracle
 *        (/tmp/nk-mmu-trace.log: ordered mtsr/mtsrin/mtdbat/SDR1 + optional
 *        `expect EA PA` rows from QEMU/NK). Wired even though the trace is not yet
 *        harvested: SKIPs cleanly if absent. PROSPECTIVE — needs validation once a
 *        real trace exists.
 *
 *  Backport hygiene: the arena under test reimplements Dolphin (GPLv2, SHA
 *  144d19433aa734c19c34e5978a1b817d2aa12663); see dolphin_bat_arena.cpp header.
 */
#include "dolphin_bat_arena.h"
#include "paged_mmu.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int n_pass = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  CHECK FAILED: %s (line %d)\n", #cond, __LINE__); assert(cond); } n_pass++; } while (0)

#define HOST_PAGE 16384u

/* Build a DBAT upper word from BEPI + BL + valid bits, lower from BRPN + pp. */
static uint32_t mk_batu(uint32_t bepi, uint32_t bl, bool vs, bool vp)
{
	return (bepi & 0xFFFE0000u) | ((bl & 0x7FFu) << 2) | (vs ? 0x2u : 0u) | (vp ? 0x1u : 0u);
}
static uint32_t mk_batl(uint32_t brpn, uint32_t wimg, uint32_t pp)
{
	return (brpn & 0xFFFE0000u) | ((wimg & 0xFu) << 3) | (pp & 0x3u);
}

/* paged_mmu phys reader over an arbitrary host byte buffer (for HTAB rows). */
static uint8_t *g_pmem = NULL;
static size_t   g_pmem_size = 0;
static uint32_t pmem_read(uint32_t pa, void *opaque)
{
	(void)opaque;
	if (!g_pmem || (size_t)pa + 4 > g_pmem_size) return 0;
	uint32_t v; memcpy(&v, g_pmem + pa, 4); return v;
}

/* ===================================================================== *
 * (A) PURE — decode + feasibility.
 * ===================================================================== */
static void test_decode_and_feasibility(void)
{
	printf("Battery A: BAT decode + feasibility gate\n");

	/* 128 KB block (BL=0): size, base, validity. */
	bat_block_t b = dba_decode_bat_pair(mk_batu(0x00200000u, 0, true, false),
	                                    mk_batl(0x01000000u, 0x2 /*WIMG=I*/, 2));
	CHECK(b.valid);
	CHECK(b.size    == 0x20000u);        /* 128 KB */
	CHECK(b.ea_base == 0x00200000u);
	CHECK(b.pa_base == 0x01000000u);
	CHECK(dba_block_fast_mappable(&b, HOST_PAGE));   /* 128 KB-aligned -> 16 KB-safe */

	/* COARSE 256 MB block (BL=0x7FF) — the Discriminator-A fast-path case. */
	bat_block_t c = dba_decode_bat_pair(mk_batu(0x90000000u, 0x7FFu, true, false),
	                                    mk_batl(0x30000000u, 0, 2));
	CHECK(c.valid);
	CHECK(c.size    == 0x10000000u);     /* 256 MB */
	CHECK(c.ea_base == 0x90000000u);
	CHECK(c.pa_base == 0x30000000u);
	CHECK(dba_block_fast_mappable(&c, HOST_PAGE));   /* trivially 16 KB-safe */

	/* Invalid pair (neither Vs nor Vp). */
	bat_block_t z = dba_decode_bat_pair(mk_batu(0x90000000u, 0x7FFu, false, false),
	                                    mk_batl(0x30000000u, 0, 0));
	CHECK(!z.valid);
	CHECK(!dba_block_fast_mappable(&z, HOST_PAGE));

	/* Adversarial: a synthetic 4 KB-granular block whose PA is 4 KB-aligned but NOT
	 * 16 KB-aligned -> the feasibility gate must reject it (slow path / "fine" fork).
	 * (Real BATs cannot express this, but the gate is the safety net Dolphin keeps.) */
	bat_block_t f; memset(&f, 0, sizeof(f));
	f.valid = true; f.ea_base = 0x00004000u; f.pa_base = 0x00005000u; f.size = 0x4000u;
	CHECK(!dba_block_fast_mappable(&f, HOST_PAGE));  /* pa_base 0x5000 not 16K-aligned */

	/* Same but a 16 KB-aligned variant passes. */
	f.pa_base = 0x00008000u;
	CHECK(dba_block_fast_mappable(&f, HOST_PAGE));

	printf("  [A] decode + feasibility OK\n");
}

/* ===================================================================== *
 * (B) ORACLE — arena host mapping agrees with paged_mmu_translate().
 *
 * For a BAT-covered EA: translate via the reference (DR=1) to get PA, stamp a
 * unique sentinel at phys[PA], then assert a BARE read at window[EA] returns it.
 * ===================================================================== */
static dba_arena_t *g_arena = NULL;
static size_t       g_arena_size = 0;

/* Helper: translate EA with a zero SR set (BAT-only path), stamp phys[PA], read win[EA]. */
static void check_bat_lands(const char *name, const uint32_t sr[16], const uint32_t bat[16],
                            uint32_t ea, uint32_t sentinel)
{
	uint8_t *win  = dba_arena_window(g_arena);
	uint8_t *phys = dba_arena_phys(g_arena);

	uint32_t pa = 0xDEADBEEF;
	bool ok = paged_mmu_translate(sr, bat, /*sdr1=*/0, ea, /*msr_dr=*/1, &pa);
	CHECK(ok);                                   /* oracle resolves the EA */
	CHECK((uint64_t)pa + 4 <= g_arena_size);     /* in-arena */

	/* Stamp the oracle PA in the backing store, read back through the bare window. */
	memcpy(phys + pa, &sentinel, 4);
	uint32_t got = 0;
	memcpy(&got, win + ea, 4);
	CHECK(got == sentinel);                      /* arena agrees with paged_mmu */

	printf("  [B] %-18s ea=%08x -> pa=%08x  win-read=%08x (== sentinel)\n",
	       name, ea, pa, got);
}

static void test_arena_oracle_agreement(void)
{
	printf("Battery B: arena host mapping == paged_mmu_translate oracle\n");

	paged_mmu_set_phys_reader(pmem_read, NULL);

	g_arena = dba_arena_create(32u * 1024 * 1024, &g_arena_size);
	if (!g_arena) {
		printf("  [B] SKIP (no arena on this host — non-macOS-arm64-16K)\n");
		return;
	}
	printf("  [B] arena: %zu bytes\n", g_arena_size);

	uint32_t sr[16]; memset(sr, 0, sizeof(sr));   /* BAT path doesn't consult SR */
	uint32_t bat[16];

	/* --- B1: single coarse DBAT block, EA 0x00800000 -> PA 0x01000000 (2 MB). --- */
	memset(bat, 0, sizeof(bat));
	bat[8] = mk_batu(0x00800000u, /*BL: 2MB -> (16)-1 */ 0x00Fu, true, false);
	bat[9] = mk_batl(0x01000000u, 0, 2);
	{
		bat_block_t b = dba_decode_bat_pair(bat[8], bat[9]);
		CHECK(b.size == 0x200000u);               /* (0xF+1)*128KB = 2 MB */
	}
	int slow = -1;
	int fast = dba_arena_update(g_arena, bat, HOST_PAGE, &slow);
	CHECK(fast == 1); CHECK(slow == 0);
	check_bat_lands("b1-base",  sr, bat, 0x00800000u, 0x11110001u);
	check_bat_lands("b1-mid",   sr, bat, 0x00800000u + 0x12340u, 0x11110002u);
	check_bat_lands("b1-high",  sr, bat, 0x00800000u + 0x1FFFFu, 0x11110003u);

	/* --- B2: COARSE "DBAT-covers-RAM+ROM" — two simultaneous DISJOINT blocks. --- */
	memset(bat, 0, sizeof(bat));
	bat[8]  = mk_batu(0x00000000u, 0x00Fu, true, false);  /* "RAM": 2 MB EA0 -> PA0     */
	bat[9]  = mk_batl(0x00000000u, 0, 2);
	bat[10] = mk_batu(0x01000000u, 0x00Fu, true, false);  /* "ROM": 2 MB EA -> distinct PA */
	bat[11] = mk_batl(0x00400000u, 0x2 /*I*/, 2);
	fast = dba_arena_update(g_arena, bat, HOST_PAGE, &slow);
	CHECK(fast == 2); CHECK(slow == 0);
	check_bat_lands("b2-ram",   sr, bat, 0x00010000u, 0x22220001u);
	check_bat_lands("b2-rom",   sr, bat, 0x01000000u + 0x8000u, 0x22220002u);

	/* --- B3: context switch — re-program DBAT0 to a NEW PA; same EA must move. --- */
	memset(bat, 0, sizeof(bat));
	bat[8] = mk_batu(0x00800000u, 0x00Fu, true, false);
	bat[9] = mk_batl(0x00C00000u, 0, 2);                  /* same EA, different PA */
	fast = dba_arena_update(g_arena, bat, HOST_PAGE, &slow);
	CHECK(fast == 1); CHECK(slow == 0);
	check_bat_lands("b3-newctx", sr, bat, 0x00800000u + 0x4000u, 0x33330001u);
	/* Distinctness: B1 mapped EA 0x800000 to PA 0x1000000; B3 maps it to 0x00C00000.
	 * Identity-only or a stale window would FAIL check_bat_lands above by construction. */

	/* --- B4: withdraw coverage -> EA reverts to identity (window[EA] == phys[EA]). --- */
	memset(bat, 0, sizeof(bat));
	fast = dba_arena_update(g_arena, bat, HOST_PAGE, &slow);
	CHECK(fast == 0);
	{
		uint8_t *win  = dba_arena_window(g_arena);
		uint8_t *phys = dba_arena_phys(g_arena);
		uint32_t ea = 0x00800000u + 0x4000u;
		uint32_t marker = 0x44440001u;
		memcpy(phys + ea, &marker, 4);            /* stamp identity PA == EA */
		uint32_t got = 0; memcpy(&got, win + ea, 4);
		CHECK(got == marker);                     /* reverted to identity */
		printf("  [B] b4-revert        ea=%08x -> identity win-read=%08x\n", ea, got);
	}

	/* --- B5: out-of-arena block counts as slow path (not fast-mapped). --- */
	memset(bat, 0, sizeof(bat));
	bat[8] = mk_batu(0x90000000u, 0x7FFu, true, false);   /* 256 MB @ EA 0x90000000 */
	bat[9] = mk_batl(0x30000000u, 0, 2);                  /* both exceed 32 MB arena */
	fast = dba_arena_update(g_arena, bat, HOST_PAGE, &slow);
	CHECK(fast == 0); CHECK(slow == 1);
	printf("  [B] b5-oob           256MB block out-of-arena -> slow=%d (expected 1)\n", slow);

	dba_arena_destroy(g_arena);
	g_arena = NULL;
}

/* ===================================================================== *
 * (C) HOOK — NK MMU-trace / QEMU differential replay fixture.
 *
 * Format (one directive per line; '#' comments; blank lines ignored):
 *   mtsr     <n>   <hex_value>        # SR[n]      = value
 *   mtsrin   <ea>  <hex_value>        # SR[ea>>28] = value
 *   mtdbatu  <p>   <hex_value>        # bat[8+2p]   = value (p in 0..3)
 *   mtdbatl  <p>   <hex_value>        # bat[8+2p+1] = value
 *   mtibatu  <p>   <hex_value>        # bat[2p]
 *   mtibatl  <p>   <hex_value>
 *   sdr1     <hex_value>
 *   update                            # apply current BAT state to the arena
 *   expect   <hex_ea>  <hex_pa>       # diff paged_mmu(EA) AND window[EA] vs PA
 *
 * The `expect` rows are the QEMU/NK oracle outputs. When present they validate the
 * arena+paged_mmu end-to-end; when the file is absent the whole battery SKIPs.
 * PROSPECTIVE: needs validation once /tmp/nk-mmu-trace.log is harvested.
 * ===================================================================== */
static void test_trace_replay(const char *path)
{
	printf("Battery C: NK MMU-trace replay fixture (%s)\n", path);
	FILE *f = fopen(path, "r");
	if (!f) {
		printf("  [C] SKIP (no trace at %s — hook wired, fixture not yet harvested)\n", path);
		return;
	}
	if (!g_arena) {
		g_arena = dba_arena_create(256u * 1024 * 1024, &g_arena_size);
		if (!g_arena) g_arena = dba_arena_create(32u * 1024 * 1024, &g_arena_size);
	}
	paged_mmu_set_phys_reader(pmem_read, NULL);

	uint32_t sr[16];  memset(sr, 0, sizeof(sr));
	uint32_t bat[16]; memset(bat, 0, sizeof(bat));
	uint32_t sdr1 = 0;
	int n_expect = 0, n_ok = 0;
	char line[256];

	int applied = 0;
	while (fgets(line, sizeof(line), f)) {
		char op[32]; unsigned a = 0, b = 0;
		if (line[0] == '#' || line[0] == '\n') continue;

		/* Real harvester format (nk_mmu_trace.cpp): "<seq> OP pc=.. insn=.. idx=N val=HEX".
		 * Recognised by the "val=" token; the BAT/SR index comes from "idx=". */
		char *vp = strstr(line, "val=");
		if (vp && sscanf(line, "%*u %31s", op) == 1) {
			unsigned idx = 0, val = 0;
			char *ip = strstr(line, "idx=");
			if (ip) sscanf(ip, "idx=%x", &idx);
			sscanf(vp, "val=%x", &val);
			if      (!strcmp(op, "MTDBATU")) bat[8 + (idx & 3) * 2] = val;
			else if (!strcmp(op, "MTDBATL")) bat[8 + (idx & 3) * 2 + 1] = val;
			else if (!strcmp(op, "MTIBATU")) bat[(idx & 3) * 2] = val;
			else if (!strcmp(op, "MTIBATL")) bat[(idx & 3) * 2 + 1] = val;
			else if (!strcmp(op, "MTSR"))    sr[idx & 0xF] = val;
			else if (!strcmp(op, "MTSRIN"))  sr[(val >> 28) & 0xF] = val;  /* EA->SR# */
			else if (!strcmp(op, "MTSDR1") || !strcmp(op, "SDR1")) sdr1 = val;
			/* The harvester logs each write individually; refresh the shadow after
			 * every BAT/SR write so the arena tracks the live sequence. */
			dba_arena_update(g_arena, bat, HOST_PAGE, NULL);
			applied++;
			continue;
		}

		int nf = sscanf(line, "%31s %x %x", op, &a, &b);
		if (nf < 1) continue;

		if      (!strcmp(op, "mtsr")   && nf >= 3) sr[a & 0xF] = b;
		else if (!strcmp(op, "mtsrin") && nf >= 3) sr[(a >> 28) & 0xF] = b;
		else if (!strcmp(op, "mtdbatu")&& nf >= 3) bat[8 + (a & 3) * 2] = b;
		else if (!strcmp(op, "mtdbatl")&& nf >= 3) bat[8 + (a & 3) * 2 + 1] = b;
		else if (!strcmp(op, "mtibatu")&& nf >= 3) bat[(a & 3) * 2] = b;
		else if (!strcmp(op, "mtibatl")&& nf >= 3) bat[(a & 3) * 2 + 1] = b;
		else if (!strcmp(op, "sdr1")   && nf >= 2) sdr1 = a;
		else if (!strcmp(op, "update"))            dba_arena_update(g_arena, bat, HOST_PAGE, NULL);
		else if (!strcmp(op, "expect") && nf >= 3) {
			n_expect++;
			uint32_t pa = 0xDEADBEEF;
			bool ok = paged_mmu_translate(sr, bat, sdr1, a, 1, &pa);
			bool match = ok && pa == b;
			if (g_arena && match) {
				/* end-to-end: stamp the oracle PA, read the bare window. */
				uint8_t *win = dba_arena_window(g_arena), *ph = dba_arena_phys(g_arena);
				if ((uint64_t)pa + 4 <= g_arena_size && (uint64_t)a + 4 <= g_arena_size) {
					uint32_t s = 0xC0DE0000u | (n_expect & 0xFFFF), got = 0;
					memcpy(ph + pa, &s, 4); memcpy(&got, win + a, 4);
					match = match && (got == s);
				}
			}
			if (match) n_ok++;
			else fprintf(stderr, "  [C] expect MISMATCH ea=%08x want_pa=%08x got_pa=%08x ok=%d\n", a, b, pa, ok);
		}
	}
	fclose(f);
	if (g_arena) { dba_arena_destroy(g_arena); g_arena = NULL; }

	if (n_expect == 0)
		printf("  [C] trace applied (%d harvested writes); no `expect` rows yet "
		       "(state-only replay — PROSPECTIVE, needs `expect EA PA` oracle rows)\n", applied);
	else {
		CHECK(n_ok == n_expect);
		printf("  [C] %d/%d expect rows matched (paged_mmu + arena vs NK/QEMU oracle)\n", n_ok, n_expect);
	}
}

int main(int argc, char **argv)
{
	/* A scratch phys buffer so paged_mmu's HTAB reader never dereferences garbage
	 * (BAT-only rows don't use it, but trace fixtures might). */
	g_pmem_size = 1u << 20;
	g_pmem = (uint8_t *)calloc(1, g_pmem_size);

	test_decode_and_feasibility();
	test_arena_oracle_agreement();
	test_trace_replay(argc > 1 ? argv[1] : "/tmp/nk-mmu-trace.log");

	free(g_pmem);
	printf("\ntest_dolphin_bat_arena: ALL %d CHECKS PASSED\n", n_pass);
	return 0;
}
