/*
 *  test_shm_arena_multientry.cpp - Dolphin SHM-arena MULTI-ENTRY transaction spike
 *  (SS_M18 S3-Task-0, the live-MMU de-risk follow-on to test_shm_arena_spike.cpp).
 *  Standalone PASS/FALSIFY probe in the model of test_paged_mmu.cpp / the base spike —
 *  no SS wiring, no NATMEM edit, no NK, no kpx_cpu/paged_mmu link.
 *  INERT: linked into no emulator binary, structurally unreachable from the binary.
 *
 *  PURPOSE: the base spike proved the SINGLE-remap primitive. The REAL NK risk is the
 *  MULTI-ENTRY transaction: a BAT context switch (UpdateDBATMappings) re-aliases a SET of
 *  regions per mtspr-storm. This spike proves/falsifies, on macOS arm64 (16 KB host page):
 *    M-S1  atomic per-region SET re-alias (N>=8 regions, each via the one-step
 *          mach_vm_map(FIXED|OVERWRITE) form — NO unmap-then-map, no transient hole).
 *    M-S2  cross-region concurrent reader over >=10,000 multi-region transactions
 *          (reader hits DIFFERENT regions than the one being remapped; every read must
 *          classify as a valid old-or-new sentinel FOR THAT REGION — no torn/garbage).
 *    M-S3  mixed-perm 4x4KB fork inside ONE 16 KB host page (the Discriminator-A decision):
 *          can macOS arm64 give the four constituent 4 KB guest pages different perms?
 *    M-S4  remap latency (ns/remap) for the per-region atomic OVERWRITE.
 *
 *  BACKPORT HYGIENE (needs validation):
 *    Reimplemented-to-spec SYNTHETIC probe, NOT vendored Dolphin code. Models the
 *    mach calls Dolphin makes on macOS, extended to the multi-region transaction:
 *      - Source/Core/Common/MemArena.cpp   (mach_make_memory_entry_64 / vm_map)
 *      - Source/Core/Core/HW/Memmap.cpp    (MemoryManager::UpdateDBATMappings =
 *                                           unmap-set/map-set; CanCreateHostMappingForGuestPages)
 *    Dolphin repo: https://github.com/dolphin-emu/dolphin  (GPLv2)
 *    Pinned SHA:   144d19433aa734c19c34e5978a1b817d2aa12663
 *    Status:       NEEDS VALIDATION (platform-feasibility attestation only).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__MACH__) && defined(__aarch64__)
#define SPIKE_ON_TARGET 1
#else
#define SPIKE_ON_TARGET 0
#endif

#if SPIKE_ON_TARGET
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/mach_time.h>
#include <pthread.h>

// ---------------------------------------------------------------------------
// Probe configuration.
// ---------------------------------------------------------------------------
static const mach_vm_size_t N_FULL  = 256ULL * 1024 * 1024;  // realistic guest RAM
static const mach_vm_size_t N_PROXY = 32ULL  * 1024 * 1024;  // fallback if 256 MiB flaky
#define HOST_PAGE_EXPECT 16384u
#define NREG          8        // regions re-aliased per transaction (>= 8 per directive)
#define REGION_SIZE   HOST_PAGE_EXPECT
#define TXN_COUNT     10000    // multi-region transactions for M-S2

// Per-region sentinel: unique per (region, variant), recognisable tag.
//   0x5E0RRVVV form is overkill; keep it compact and collision-free for r<16, v<2.
static inline uint32_t sentinel(int r, int variant)
{
	return 0x5E000000u | ((uint32_t)r << 4) | (uint32_t)variant;
}

// Verdict bookkeeping.
static const char *g_deadend = NULL;
static int g_size_was_proxy = 0;

static void fail(const char *deadend, const char *msg, kern_return_t kr)
{
	if (!g_deadend) g_deadend = deadend;
	if (kr != KERN_SUCCESS)
		fprintf(stderr, "  ! %s: %s (kr=%d %s)\n", deadend, msg, kr, mach_error_string(kr));
	else
		fprintf(stderr, "  ! %s: %s\n", deadend, msg);
}

static void fill_page(uint8_t *view, mach_vm_size_t off, uint32_t sent)
{
	uint32_t *p = (uint32_t *)(view + off);
	for (unsigned i = 0; i < REGION_SIZE / 4; i++) p[i] = sent;
}

static inline uint32_t read_word(volatile uint8_t *view, mach_vm_size_t off)
{
	return *(volatile uint32_t *)(view + off);
}

// ATOMIC one-step re-alias: re-map [view+off, +span) to backing offset off2 of mem_entry
// via mach_vm_map(FIXED|OVERWRITE) ONLY. No unmap-then-map fallback — the directive
// requires the atomic form. Returns the kr; never holes the mapping.
static kern_return_t atomic_realias(mach_vm_address_t view, mach_vm_size_t off,
                                    mach_vm_size_t span, mem_entry_name_port_t mem_entry,
                                    mach_vm_offset_t off2)
{
	mach_vm_address_t target = view + off;
	return mach_vm_map(mach_task_self(), &target, span, /*mask=*/0,
	                   VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
	                   mem_entry, off2, /*copy=*/FALSE,
	                   VM_PROT_READ | VM_PROT_WRITE,
	                   VM_PROT_READ | VM_PROT_WRITE,
	                   VM_INHERIT_NONE);
}

// Region layout in the named store (the "physical" backing):
//   region r owns two backing pages — A (variant 0) and B (variant 1) — laid out
//   contiguously well past the window's own linear footprint so they never collide.
static mach_vm_offset_t backing_off(int r, int variant)
{
	// Park backings high in the store: base + (2*r + variant) pages.
	mach_vm_offset_t base = 1024ULL * REGION_SIZE;     // 16 MiB in
	return base + (mach_vm_offset_t)(2 * r + variant) * REGION_SIZE;
}

// Window offset of region r (the fixed VA the "guest bare access" hits).
static mach_vm_offset_t window_off(int r)
{
	return (mach_vm_offset_t)(16 + r) * REGION_SIZE;   // 16 pages in, contiguous
}

// ---------------------------------------------------------------------------
// M-S2 cross-region concurrent reader.
// Reads a ROTATING set of region offsets, classifying each read against THAT
// region's two known sentinels. Any value that is neither A nor B for that region
// is a torn/garbage read (transient-hole / non-atomic substitution).
// ---------------------------------------------------------------------------
struct ReaderArgs {
	volatile uint8_t *base;     // window base VA
	volatile int stop;
	volatile long reads;
	volatile int  torn;         // set on first bad read
	volatile uint32_t torn_val;
	volatile int  torn_reg;
	volatile int  saw_a;        // saw at least one A sentinel
	volatile int  saw_b;        // saw at least one B sentinel
};

static void *reader_thread(void *p)
{
	ReaderArgs *a = (ReaderArgs *)p;
	int r = 0;
	while (!a->stop) {
		// rotate across regions
		r = (r + 1) & (NREG - 1);
		uint32_t v = read_word(a->base, window_off(r));
		uint32_t sa = sentinel(r, 0), sb = sentinel(r, 1);
		if (v == sa) a->saw_a = 1;
		else if (v == sb) a->saw_b = 1;
		else if (!a->torn) { a->torn_val = v; a->torn_reg = r; a->torn = 1; }
		a->reads++;
	}
	return NULL;
}

static double g_ns_per_tick = 0.0;
static inline uint64_t now_ns(void) { return (uint64_t)(mach_absolute_time() * g_ns_per_tick); }

static int run_spike(void)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	g_ns_per_tick = (double)tb.numer / (double)tb.denom;

	// -- S0: page-size attestation ------------------------------------------
	long pg = sysconf(_SC_PAGESIZE);
	printf("[M-S0] host _SC_PAGESIZE = %ld\n", pg);
	if (pg != (long)HOST_PAGE_EXPECT) {
		printf("[M-S0] SKIP (host page %ld != 16384 — not arm64-16K)\n", pg);
		printf("MULTI-ENTRY SPIKE: SKIP (non-target host)\n");
		return 0;
	}
	printf("[M-S0] PASS (16384)\n");

	// -- Setup: named SHM store + linear window + linear phys alias ----------
	mach_vm_size_t want = N_FULL;
	memory_object_size_t entry_size = (memory_object_size_t)want;
	mem_entry_name_port_t mem_entry = MACH_PORT_NULL;
	kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &entry_size, 0,
	                       MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE,
	                       &mem_entry, MACH_PORT_NULL);
	if (kr != KERN_SUCCESS || entry_size < want) {
		want = N_PROXY; entry_size = (memory_object_size_t)want;
		kr = mach_make_memory_entry_64(mach_task_self(), &entry_size, 0,
		         MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE,
		         &mem_entry, MACH_PORT_NULL);
		if (kr != KERN_SUCCESS || entry_size < want) {
			fail("DEAD-END-A", "mach_make_memory_entry_64 would not vend named entry", kr);
			printf("MULTI-ENTRY SPIKE: FALSIFIED @ DEAD-END-A\n");
			return 1;
		}
		g_size_was_proxy = 1;
	}
	mach_vm_size_t N = entry_size;

	mach_vm_address_t view_phys = 0, view_window = 0;
	kr = mach_vm_map(mach_task_self(), &view_phys, N, 0, VM_FLAGS_ANYWHERE,
	                 mem_entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
	                 VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
	if (kr == KERN_SUCCESS)
		kr = mach_vm_map(mach_task_self(), &view_window, N, 0, VM_FLAGS_ANYWHERE,
		                 mem_entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
		                 VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
	if (kr != KERN_SUCCESS) {
		fail("DEAD-END-B", "could not map phys/window aliases", kr);
		printf("MULTI-ENTRY SPIKE: FALSIFIED @ DEAD-END-B\n");
		return 1;
	}
	uint8_t *vp = (uint8_t *)view_phys;
	uint8_t *vw = (uint8_t *)view_window;
	printf("[setup] store=%s  view_phys=%p view_window=%p\n",
	       g_size_was_proxy ? "32MiB proxy" : "256MiB", vp, vw);

	// Stamp every region's A and B backing pages with their per-region sentinels.
	for (int r = 0; r < NREG; r++) {
		fill_page(vp, backing_off(r, 0), sentinel(r, 0));
		fill_page(vp, backing_off(r, 1), sentinel(r, 1));
	}
	// Initial transaction: alias every window region to its A backing (variant 0).
	for (int r = 0; r < NREG; r++) {
		kr = atomic_realias(view_window, window_off(r), REGION_SIZE, mem_entry, backing_off(r, 0));
		if (kr != KERN_SUCCESS) {
			fail("DEAD-END-A2", "initial per-region atomic alias failed", kr);
			printf("MULTI-ENTRY SPIKE: FALSIFIED @ DEAD-END-A2\n");
			return 1;
		}
	}

	// =======================================================================
	// M-S1: atomic per-region SET re-alias (the design directive).
	// Re-alias ALL NREG regions from their A backing to their B backing, each via
	// the atomic one-step OVERWRITE. After the set: every region reads its NEW (B)
	// sentinel; the store still holds BOTH A and B pages for every region (never
	// holed); and — by construction — no region was ever unmapped (atomic only).
	// =======================================================================
	{
		// pre: every region shows its A sentinel
		int pre_ok = 1;
		for (int r = 0; r < NREG; r++)
			if (read_word(vw, window_off(r)) != sentinel(r, 0)) pre_ok = 0;

		int set_ok = 1;
		for (int r = 0; r < NREG; r++) {
			kr = atomic_realias(view_window, window_off(r), REGION_SIZE, mem_entry, backing_off(r, 1));
			if (kr != KERN_SUCCESS) { set_ok = 0; fail("DEAD-END-MS1", "atomic OVERWRITE in set rejected", kr); break; }
			// after EACH remap, every OTHER region must still be readable (no hole)
			for (int q = 0; q < NREG; q++) {
				uint32_t v = read_word(vw, window_off(q));
				int swapped = (q <= r);  // already moved to B
				if (v != sentinel(q, swapped ? 1 : 0)) { set_ok = 0; break; }
			}
			if (!set_ok) { fail("DEAD-END-MS1", "transient hole / collateral during set", KERN_SUCCESS); break; }
		}
		// post: every region shows B; store still has BOTH A and B per region
		int land_ok = 1, store_ok = 1;
		for (int r = 0; r < NREG; r++) {
			if (read_word(vw, window_off(r)) != sentinel(r, 1)) land_ok = 0;
			if (read_word(vp, backing_off(r, 0)) != sentinel(r, 0)) store_ok = 0;
			if (read_word(vp, backing_off(r, 1)) != sentinel(r, 1)) store_ok = 0;
		}
		if (pre_ok && set_ok && land_ok && store_ok) {
			printf("[M-S1] PASS (%d regions atomically re-aliased as a set; all land, store intact, no transient hole)\n", NREG);
		} else {
			printf("[M-S1] FAIL (pre=%d set=%d land=%d store_intact=%d)\n", pre_ok, set_ok, land_ok, store_ok);
			printf("MULTI-ENTRY SPIKE: FALSIFIED @ DEAD-END-MS1\n");
			return 1;
		}
	}

	// =======================================================================
	// M-S2: cross-region concurrent reader over >=10,000 multi-region txns.
	// Reader rotates across regions classifying each read; main thread runs
	// transactions that toggle every region between A and B. Reader must never
	// see a value that is not a valid sentinel for the region it read.
	// =======================================================================
	{
		ReaderArgs ra;
		ra.base = vw; ra.stop = 0; ra.reads = 0; ra.torn = 0; ra.torn_val = 0;
		ra.torn_reg = -1; ra.saw_a = 0; ra.saw_b = 0;
		pthread_t tid;
		if (pthread_create(&tid, NULL, reader_thread, &ra) != 0) {
			fail("DEAD-END-MS2", "could not spawn reader", KERN_SUCCESS);
			printf("MULTI-ENTRY SPIKE: FALSIFIED @ DEAD-END-MS2\n");
			return 1;
		}

		int remap_fail = 0;
		uint64_t lat_sum = 0; long lat_n = 0;     // M-S4 timing piggybacks here
		for (int t = 0; t < TXN_COUNT && !remap_fail; t++) {
			int variant = t & 1;                  // whole-set toggle A<->B
			for (int r = 0; r < NREG; r++) {
				uint64_t t0 = now_ns();
				kr = atomic_realias(view_window, window_off(r), REGION_SIZE, mem_entry, backing_off(r, variant));
				uint64_t t1 = now_ns();
				lat_sum += (t1 - t0); lat_n++;
				if (kr != KERN_SUCCESS) { remap_fail = 1; fail("DEAD-END-MS2", "remap failed mid-transaction", kr); break; }
			}
		}
		ra.stop = 1;
		pthread_join(tid, NULL);

		double ns_per_remap = lat_n ? (double)lat_sum / (double)lat_n : 0.0;

		int no_torn   = !ra.torn;
		int saw_both  = ra.saw_a && ra.saw_b;
		// store still holds every A and B page (no hole under churn)
		int no_hole = 1;
		for (int r = 0; r < NREG; r++) {
			if (read_word(vp, backing_off(r, 0)) != sentinel(r, 0)) no_hole = 0;
			if (read_word(vp, backing_off(r, 1)) != sentinel(r, 1)) no_hole = 0;
		}
		if (remap_fail) {
			printf("[M-S2] FAIL (a remap failed mid-transaction)\n");
			printf("MULTI-ENTRY SPIKE: FALSIFIED @ DEAD-END-MS2\n");
			return 1;
		}
		if (!no_torn) {
			printf("[M-S2] FAIL (TORN read: region %d saw 0x%08x — not a valid sentinel for that region)\n",
			       ra.torn_reg, ra.torn_val);
			fail("DEAD-END-MS2", "non-atomic substitution / transient hole (torn read)", KERN_SUCCESS);
			printf("MULTI-ENTRY SPIKE: FALSIFIED @ DEAD-END-MS2\n");
			return 1;
		}
		if (!saw_both) {
			printf("[M-S2] FAIL (reader never observed both variants: A=%d B=%d — swaps not visible)\n",
			       ra.saw_a, ra.saw_b);
			fail("DEAD-END-MS2", "concurrent swaps not visible to reader", KERN_SUCCESS);
			printf("MULTI-ENTRY SPIKE: FALSIFIED @ DEAD-END-MS2\n");
			return 1;
		}
		if (!no_hole) {
			printf("[M-S2] FAIL (store holed under churn)\n");
			printf("MULTI-ENTRY SPIKE: FALSIFIED @ DEAD-END-MS2\n");
			return 1;
		}
		printf("[M-S2] PASS (%d txns x %d regions = %ld remaps; reader did %ld reads, no torn read, saw both variants, store intact)\n",
		       TXN_COUNT, NREG, (long)TXN_COUNT * NREG, ra.reads);

		// -- M-S4: latency report (measured across the M-S2 loop) -----------
		printf("[M-S4] PASS (per-region atomic OVERWRITE remap: %.0f ns/remap over %ld remaps; multi-BAT switch of %d regions ~= %.0f ns)\n",
		       ns_per_remap, lat_n, NREG, ns_per_remap * NREG);
	}

	// =======================================================================
	// M-S3: mixed-perm 4x4KB fork inside ONE 16 KB host page (Discriminator-A).
	// Attempt to give the four constituent 4 KB guest pages of a single host page
	// DIFFERENT perms. Report the EXACT kernel behaviour. Likely NOT permitted on
	// a 16 KB-page host -> confirms such pages must fall to the slow path.
	// =======================================================================
	{
		mach_vm_address_t hp = view_window + window_off(0);  // one 16 KB host page
		// Try to set the 2nd 4 KB sub-page (offset +4096, len 4096) read-only.
		kern_return_t kp = mach_vm_protect(mach_task_self(), hp + 4096, 4096,
		                                   /*set_maximum=*/FALSE, VM_PROT_READ);
		// Read back the protection map around the host page.
		mach_vm_address_t qa = hp; mach_vm_size_t qsz = 0;
		vm_region_basic_info_data_64_t info;
		mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t obj = MACH_PORT_NULL;
		kern_return_t kq = mach_vm_region(mach_task_self(), &qa, &qsz, VM_REGION_BASIC_INFO_64,
		                                  (vm_region_info_t)&info, &cnt, &obj);
		int independent_4k = 0;
		if (kp == KERN_SUCCESS && kq == KERN_SUCCESS) {
			// If the kernel honoured a 4 KB-granular protect, the region containing
			// hp would END at hp+4096 (split), not span the whole 16 KB page.
			if (qsz == 4096 || qa + qsz == hp + 4096) independent_4k = 1;
		}
		printf("[M-S3] mixed-perm 4KB protect kr=%d (%s); region@hp base=0x%llx size=0x%llx prot=0x%x\n",
		       kp, mach_error_string(kp),
		       (unsigned long long)qa, (unsigned long long)qsz,
		       kq == KERN_SUCCESS ? info.protection : 0xFFFFFFFFu);
		if (independent_4k) {
			printf("[M-S3] SURPRISE: macOS arm64 PERMITTED independent per-4KB perms inside a 16KB host page "
			       "(Discriminator-A relaxes; fine-grained mapping viable). FORK = fine-grained.\n");
		} else {
			printf("[M-S3] CONFIRMED (as expected): independent per-4KB perms NOT honoured inside a 16KB host page "
			       "(protect %s). CanCreateHostMappingForGuestPages must fall mixed-perm 16KB pages to the SLOW PATH. "
			       "FORK = coarse 16KB-granular.\n",
			       kp == KERN_SUCCESS ? "rounded to the whole host page" : "rejected");
		}
		// Restore RW so cleanup is clean.
		mach_vm_protect(mach_task_self(), hp, REGION_SIZE, FALSE, VM_PROT_READ | VM_PROT_WRITE);
	}

	// -- Cleanup -------------------------------------------------------------
	mach_vm_deallocate(mach_task_self(), view_phys, N);
	mach_vm_deallocate(mach_task_self(), view_window, N);
	mach_port_deallocate(mach_task_self(), mem_entry);

	printf("---- multi-entry spike summary ----\n");
	printf("  store:           %s\n", g_size_was_proxy ? "32 MiB proxy" : "256 MiB full");
	printf("  remap form:      mach_vm_map(FIXED|OVERWRITE), atomic one-step (no unmap-then-map)\n");
	printf("  M-S1 set size:   %d regions / transaction\n", NREG);
	printf("  M-S2 churn:      %ld atomic remaps under a concurrent cross-region reader\n", (long)TXN_COUNT * NREG);
	printf("MULTI-ENTRY SPIKE: PROVEN (macOS arm64 16K)\n");
	return 0;
}
#endif // SPIKE_ON_TARGET

int main(void)
{
#if SPIKE_ON_TARGET
	return run_spike();
#else
	printf("[M-S0] SKIP (non-macOS-arm64)\n");
	printf("MULTI-ENTRY SPIKE: SKIP (non-target host)\n");
	return 0;
#endif
}
