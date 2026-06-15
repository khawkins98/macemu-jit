/*
 *  test_shm_arena_spike.cpp - Dolphin SHM-arena platform-feasibility spike
 *  (SS_M18 S3-Task-0 item-0). Standalone PASS/FALSIFY probe in the model of
 *  test_paged_mmu.cpp — no SS wiring, no NATMEM edit, no NK, no kpx_cpu link.
 *  INERT: linked into no emulator binary, structurally unreachable from the binary.
 *
 *  PURPOSE: prove or falsify, on macOS arm64 (16 KB host page), the Dolphin
 *  "separate SHM-backed arena with aliased views" primitive — a named memory
 *  entry aliased into multiple VAs, where a sub-range OVERWRITE remap moves a
 *  VIEW without holing the physical store (the BAT context-switch analogue).
 *
 *  BACKPORT HYGIENE (needs validation):
 *    Reimplemented-to-spec SYNTHETIC probe, NOT vendored Dolphin code. Models the
 *    mach calls Dolphin makes on macOS:
 *      - Source/Core/Common/MemArena.cpp   (mach_make_memory_entry_64 / vm_map,
 *                                           the macOS #ifdef)
 *      - Source/Core/Core/HW/Memmap.cpp    (InitFastmemArena, MapInMemoryRegion)
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
#include <pthread.h>
#include <signal.h>
#include <setjmp.h>

// ---------------------------------------------------------------------------
// Probe configuration.
// ---------------------------------------------------------------------------
static const mach_vm_size_t N_FULL  = 256ULL * 1024 * 1024;  // realistic guest RAM
static const mach_vm_size_t N_PROXY = 32ULL  * 1024 * 1024;  // fallback if 256 MiB flaky
#define HOST_PAGE_EXPECT 16384u
#define SWAP_COUNT 10000

#define SENT_A 0xA5A5A5A5u   // "before-swap" page sentinel (whole-page uniform)
#define SENT_B 0x5C5C5C5Cu   // "after-swap"  page sentinel
#define SENT_W 0x3C3C3C3Cu   // a third sentinel written through the window in S2

// Verdict bookkeeping.
static const char *g_deadend = NULL;       // set on first FALSIFY
static int g_size_was_proxy = 0;

static void fail(const char *deadend, const char *msg, kern_return_t kr)
{
	if (!g_deadend) g_deadend = deadend;
	if (kr != KERN_SUCCESS)
		fprintf(stderr, "  ! %s: %s (kr=%d %s)\n", deadend, msg, kr, mach_error_string(kr));
	else
		fprintf(stderr, "  ! %s: %s\n", deadend, msg);
}

// Fill a whole host page (16 KB) with a 32-bit sentinel, addressed through a view.
static void fill_page(uint8_t *view, mach_vm_size_t off, uint32_t sent)
{
	uint32_t *p = (uint32_t *)(view + off);
	for (unsigned i = 0; i < HOST_PAGE_EXPECT / 4; i++) p[i] = sent;
}

static uint32_t read_word(uint8_t *view, mach_vm_size_t off)
{
	return *(volatile uint32_t *)(view + off);
}

static void write_word(uint8_t *view, mach_vm_size_t off, uint32_t v)
{
	*(volatile uint32_t *)(view + off) = v;
}

// ---------------------------------------------------------------------------
// Re-alias helper: re-map sub-range [view+off, view+off+span) of an existing
// mapping to a DIFFERENT offset off2 of the SAME mem_entry. Tries the one-step
// mach_vm_map(FIXED|OVERWRITE) form first; if rejected, falls back to a two-step
// approach. Records the form that succeeded via *form_out.
//   form_out: 1 = mach_vm_map FIXED|OVERWRITE, 2 = vm_remap-style, 0 = failed
// ---------------------------------------------------------------------------
static kern_return_t realias_subrange(mach_vm_address_t view, mach_vm_size_t off,
                                      mach_vm_size_t span, mem_entry_name_port_t mem_entry,
                                      mach_vm_offset_t off2, int *form_out)
{
	mach_vm_address_t target = view + off;
	kern_return_t kr = mach_vm_map(mach_task_self(), &target, span, /*mask=*/0,
	                               VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
	                               mem_entry, off2, /*copy=*/FALSE,
	                               VM_PROT_READ | VM_PROT_WRITE,
	                               VM_PROT_READ | VM_PROT_WRITE,
	                               VM_INHERIT_NONE);
	if (kr == KERN_SUCCESS) { if (form_out) *form_out = 1; return kr; }

	// Fallback: map a fresh anywhere-alias of mem_entry@off2, then vm_remap it
	// OVER the window sub-range with OVERWRITE.
	mach_vm_address_t fresh = 0;
	kern_return_t kr2 = mach_vm_map(mach_task_self(), &fresh, span, 0,
	                                VM_FLAGS_ANYWHERE, mem_entry, off2, FALSE,
	                                VM_PROT_READ | VM_PROT_WRITE,
	                                VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
	if (kr2 != KERN_SUCCESS) { if (form_out) *form_out = 0; return kr; /* report first */ }

	vm_prot_t cur = 0, max = 0;
	mach_vm_address_t dst = view + off;
	kr2 = mach_vm_remap(mach_task_self(), &dst, span, 0,
	                    VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
	                    mach_task_self(), fresh, /*copy=*/FALSE, &cur, &max,
	                    VM_INHERIT_NONE);
	if (kr2 == KERN_SUCCESS) { if (form_out) *form_out = 2; return KERN_SUCCESS; }
	if (form_out) *form_out = 0;
	return kr2;
}

// ---------------------------------------------------------------------------
// S5 concurrent reader.
// ---------------------------------------------------------------------------
struct ReaderArgs {
	volatile uint8_t *addr;     // fixed window VA inside the swap range
	volatile int stop;
	volatile int saw_a;
	volatile int saw_b;
	volatile uint32_t torn_val; // first non-sentinel value seen (0 = none, tracked via torn flag)
	volatile int torn;
};

static void *reader_thread(void *p)
{
	ReaderArgs *a = (ReaderArgs *)p;
	volatile uint32_t *w = (volatile uint32_t *)a->addr;
	while (!a->stop) {
		uint32_t v = *w;
		if (v == SENT_A) a->saw_a = 1;
		else if (v == SENT_B) a->saw_b = 1;
		else if (!a->torn) { a->torn_val = v; a->torn = 1; }
	}
	return NULL;
}

static int run_spike(void)
{
	// -- S0 ------------------------------------------------------------------
	long pg = sysconf(_SC_PAGESIZE);
	printf("[S0] host _SC_PAGESIZE = %ld\n", pg);
	if (pg != (long)HOST_PAGE_EXPECT) {
		printf("[S0] SKIP (host page %ld != 16384 — not arm64-16K)\n", pg);
		printf("SHM-ARENA SPIKE: SKIP (non-target host)\n");
		return 0;
	}
	printf("[S0] PASS (16384)\n");

	// -- S1: SHM-backed physical store ---------------------------------------
	mach_vm_size_t want = N_FULL;
	memory_object_size_t entry_size = (memory_object_size_t)want;
	mem_entry_name_port_t mem_entry = MACH_PORT_NULL;
	kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &entry_size,
	                                              /*offset=*/0,
	                                              MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE,
	                                              &mem_entry, /*parent=*/MACH_PORT_NULL);
	if (kr != KERN_SUCCESS || entry_size < want) {
		// Try the proxy size before declaring DEAD-END-A.
		want = N_PROXY;
		entry_size = (memory_object_size_t)want;
		kr = mach_make_memory_entry_64(mach_task_self(), &entry_size, 0,
		                               MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE,
		                               &mem_entry, MACH_PORT_NULL);
		if (kr != KERN_SUCCESS || entry_size < want) {
			printf("[S1] FAIL\n");
			fail("DEAD-END-A", "mach_make_memory_entry_64 would not vend named entry", kr);
			printf("SHM-ARENA SPIKE: FALSIFIED @ DEAD-END-A\n");
			return 1;
		}
		g_size_was_proxy = 1;
	}
	printf("[S1] PASS (named entry size=0x%llx %s)\n",
	       (unsigned long long)entry_size, g_size_was_proxy ? "[32MiB proxy]" : "[256MiB]");
	mach_vm_size_t N = entry_size;

	// -- S2: aliased RW views ------------------------------------------------
	mach_vm_address_t view_phys = 0, view_window = 0;
	kr = mach_vm_map(mach_task_self(), &view_phys, N, 0, VM_FLAGS_ANYWHERE,
	                 mem_entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
	                 VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
	if (kr != KERN_SUCCESS) {
		printf("[S2] FAIL\n");
		fail("DEAD-END-B", "mach_vm_map view_phys failed", kr);
		printf("SHM-ARENA SPIKE: FALSIFIED @ DEAD-END-B\n");
		return 1;
	}
	kr = mach_vm_map(mach_task_self(), &view_window, N, 0, VM_FLAGS_ANYWHERE,
	                 mem_entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
	                 VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
	if (kr != KERN_SUCCESS) {
		printf("[S2] FAIL\n");
		fail("DEAD-END-B", "mach_vm_map view_window failed", kr);
		printf("SHM-ARENA SPIKE: FALSIFIED @ DEAD-END-B\n");
		return 1;
	}
	uint8_t *vp = (uint8_t *)view_phys;
	uint8_t *vw = (uint8_t *)view_window;

	int s2_ok = (view_phys != view_window);
	// Aligned + a couple of unaligned offsets.
	mach_vm_size_t s2_offs[] = { 0, HOST_PAGE_EXPECT, HOST_PAGE_EXPECT + 7, HOST_PAGE_EXPECT*2 + 13 };
	for (unsigned i = 0; i < 4; i++) {
		mach_vm_size_t o = s2_offs[i];
		write_word(vp, o, SENT_A + i);
		if (read_word(vw, o) != SENT_A + i) s2_ok = 0;     // phys -> window
		write_word(vw, o, SENT_W + i);
		if (read_word(vp, o) != SENT_W + i) s2_ok = 0;     // window -> phys
	}
	if (!s2_ok) {
		printf("[S2] FAIL\n");
		fail("DEAD-END-B", "views did not alias (copy, not alias)", KERN_SUCCESS);
		printf("SHM-ARENA SPIKE: FALSIFIED @ DEAD-END-B\n");
		return 1;
	}
	printf("[S2] PASS (view_phys=%p view_window=%p, bidirectional aliasing)\n", vp, vw);

	// Report the max_protection ceiling actually granted (Q7).
	{
		mach_vm_address_t qa = view_window;
		mach_vm_size_t qsz = 0;
		vm_region_basic_info_data_64_t info;
		mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t obj = MACH_PORT_NULL;
		if (mach_vm_region(mach_task_self(), &qa, &qsz, VM_REGION_BASIC_INFO_64,
		                   (vm_region_info_t)&info, &cnt, &obj) == KERN_SUCCESS) {
			printf("      view_window cur_prot=0x%x max_prot=0x%x (RW=0x%x)\n",
			       info.protection, info.max_protection, VM_PROT_READ | VM_PROT_WRITE);
		}
	}

	// -- S3: safe sub-range re-alias (the crux) ------------------------------
	// off  = window page we will overwrite; off2 = source physical page.
	mach_vm_size_t off  = 4 * HOST_PAGE_EXPECT;
	mach_vm_size_t off2 = 8 * HOST_PAGE_EXPECT;
	mach_vm_size_t span = HOST_PAGE_EXPECT;        // one host page
	mach_vm_size_t guard_lo = off - HOST_PAGE_EXPECT;
	mach_vm_size_t guard_hi = off + HOST_PAGE_EXPECT;

	fill_page(vp, off,  SENT_A);   // old physical page the window shows
	fill_page(vp, off2, SENT_B);   // the page we will swap IN
	fill_page(vp, guard_lo, 0x11111111u);
	fill_page(vp, guard_hi, 0x22222222u);

	// sanity: before remap the window shows off's page (SENT_A)
	int s3_pre = (read_word(vw, off) == SENT_A);

	int form = 0;
	kr = realias_subrange(view_window, off, span, mem_entry, off2, &form);
	if (kr != KERN_SUCCESS) {
		printf("[S3] FAIL\n");
		fail(kr == KERN_PROTECTION_FAILURE ? "DEAD-END-C" : "DEAD-END-C",
		     "OVERWRITE sub-range remap rejected", kr);
		printf("SHM-ARENA SPIKE: FALSIFIED @ DEAD-END-C\n");
		return 1;
	}
	// Assertion 1: new page lands (bare window read sees off2's SENT_B).
	int a1 = (read_word(vw, off) == SENT_B);
	// Assertion 2: store not holed — phys still has both pages.
	int a2 = (read_word(vp, off2) == SENT_B) && (read_word(vp, off) == SENT_A);
	// Assertion 3: no collateral — neighbour window pages still alias originals.
	int a3 = (read_word(vw, guard_lo) == 0x11111111u) && (read_word(vw, guard_hi) == 0x22222222u);
	if (!a1) { fail("DEAD-END-C2", "bare window read returned OLD page (overwrite no-op)", KERN_SUCCESS); }
	if (!(a1 && a2 && a3)) {
		printf("[S3] FAIL (pre=%d land=%d store_intact=%d no_collateral=%d)\n", s3_pre, a1, a2, a3);
		printf("SHM-ARENA SPIKE: FALSIFIED @ %s\n", a1 ? "DEAD-END-C" : "DEAD-END-C2");
		return 1;
	}
	printf("[S3] PASS (form=%s, land+store-intact+no-collateral)\n",
	       form == 1 ? "mach_vm_map FIXED|OVERWRITE" : "vm_remap OVERWRITE");

	// -- S4: 16 KB granularity ----------------------------------------------
	// (a) 16 KB-aligned/16 KB-span remap PASSES (re-swap off back to its own page).
	int s4form = 0;
	kr = realias_subrange(view_window, off, HOST_PAGE_EXPECT, mem_entry, off, &s4form);
	int s4_coarse = (kr == KERN_SUCCESS) && (read_word(vw, off) == SENT_A);

	// (b) sub-16-KB attempt: 4 KB span at a 4 KB-aligned-but-not-16-KB-aligned offset.
	mach_vm_size_t sub_off = off + 4096;     // inside the host page, 4 KB aligned
	int s4form_sub = 0;
	kern_return_t kr_sub = realias_subrange(view_window, sub_off, 4096, mem_entry, off2 + 4096, &s4form_sub);
	int sub_rejected = (kr_sub != KERN_SUCCESS);
	// If accepted, check whether only the targeted 4 KB changed (a surprise).
	int sub_surprise_only4k = 0;
	if (!sub_rejected) {
		// did neighbouring 4 KB within the same host page stay put?
		sub_surprise_only4k = (read_word(vw, off) == SENT_A);
	}
	if (s4_coarse && sub_rejected) {
		printf("[S4] PASS (16K-granular remap OK; sub-16K remap REJECTED kr=%d %s — granule=16384)\n",
		       kr_sub, mach_error_string(kr_sub));
	} else if (s4_coarse && !sub_rejected) {
		// Not a falsification: it RELAXES Discriminator-A. Record loudly, still PASS the gate.
		printf("[S4] PASS* (16K-granular OK; *** SURPRISE: sub-16K 4K remap ACCEPTED kr=0 form=%d only4k=%d — granule may be 4096)\n",
		       s4form_sub, sub_surprise_only4k);
	} else {
		printf("[S4] FAIL (coarse 16K remap failed kr=%d)\n", kr);
		fail("DEAD-END-D", "16 KB-granular remap failed", kr);
		printf("SHM-ARENA SPIKE: FALSIFIED @ DEAD-END-D\n");
		return 1;
	}
	long observed_granule = sub_rejected ? 16384 : 4096;

	// -- S5: overwrite under concurrent reader -------------------------------
	// Reset the two swap pages to clean uniform sentinels.
	fill_page(vp, off,  SENT_A);
	fill_page(vp, off2, SENT_B);
	// Make sure the window currently shows off's page.
	{ int rf = 0; realias_subrange(view_window, off, HOST_PAGE_EXPECT, mem_entry, off, &rf); }

	ReaderArgs ra;
	ra.addr = (volatile uint8_t *)(vw + off);   // fixed window VA inside swap range
	ra.stop = 0; ra.saw_a = 0; ra.saw_b = 0; ra.torn_val = 0; ra.torn = 0;
	pthread_t tid;
	if (pthread_create(&tid, NULL, reader_thread, &ra) != 0) {
		printf("[S5] FAIL (pthread_create)\n");
		fail("DEAD-END-E", "could not spawn reader", KERN_SUCCESS);
		printf("SHM-ARENA SPIKE: FALSIFIED @ DEAD-END-E\n");
		return 1;
	}

	int s5_remap_fail = 0;
	for (int i = 0; i < SWAP_COUNT; i++) {
		mach_vm_offset_t src = (i & 1) ? off2 : off;  // alternate the page shown at window+off
		int f = 0;
		kern_return_t k = realias_subrange(view_window, off, HOST_PAGE_EXPECT, mem_entry, src, &f);
		if (k != KERN_SUCCESS) { s5_remap_fail = 1; break; }
	}
	ra.stop = 1;
	pthread_join(tid, NULL);

	int s5_no_torn = !ra.torn;
	int s5_saw_both = ra.saw_a && ra.saw_b;
	int s5_no_hole = (read_word(vp, off) == SENT_A) && (read_word(vp, off2) == SENT_B);
	// (No crash is proven by reaching here — a SIGSEGV/SIGBUS would have aborted.)
	if (s5_remap_fail) {
		printf("[S5] FAIL (a swap remap failed mid-loop)\n");
		fail("DEAD-END-E", "remap failed under concurrent reader", KERN_SUCCESS);
		printf("SHM-ARENA SPIKE: FALSIFIED @ DEAD-END-E\n");
		return 1;
	}
	if (!s5_no_torn) {
		printf("[S5] FAIL (TORN read: observed 0x%08x — not a known sentinel)\n", ra.torn_val);
		fail("DEAD-END-E2", "non-atomic page substitution (torn read)", KERN_SUCCESS);
		printf("SHM-ARENA SPIKE: FALSIFIED @ DEAD-END-E2\n");
		return 1;
	}
	if (!s5_saw_both) {
		// Swaps not observed = inconclusive, treat as FALSIFIED (cannot attest visibility).
		printf("[S5] FAIL (reader never observed both sentinels: A=%d B=%d — swaps not visible)\n",
		       ra.saw_a, ra.saw_b);
		fail("DEAD-END-E2", "concurrent swaps not visible to reader", KERN_SUCCESS);
		printf("SHM-ARENA SPIKE: FALSIFIED @ DEAD-END-E2\n");
		return 1;
	}
	if (!s5_no_hole) {
		printf("[S5] FAIL (store holed under churn)\n");
		fail("DEAD-END-E2", "store holed under churn", KERN_SUCCESS);
		printf("SHM-ARENA SPIKE: FALSIFIED @ DEAD-END-E2\n");
		return 1;
	}
	printf("[S5] PASS (%d swaps: no crash, no torn read, saw both sentinels, store intact)\n", SWAP_COUNT);

	// -- Cleanup -------------------------------------------------------------
	mach_vm_deallocate(mach_task_self(), view_phys, N);
	mach_vm_deallocate(mach_task_self(), view_window, N);
	mach_port_deallocate(mach_task_self(), mem_entry);

	printf("---- spike summary ----\n");
	printf("  size:            %s\n", g_size_was_proxy ? "32 MiB proxy" : "256 MiB full");
	printf("  remap form:      %s\n", form == 1 ? "mach_vm_map(FIXED|OVERWRITE)" : "mach_vm_remap(OVERWRITE)");
	printf("  observed granule: %ld bytes\n", observed_granule);
	printf("  S5 atomicity:    single-remap proven; multi-entry transactional atomicity OWED to S3 live window\n");
	printf("SHM-ARENA SPIKE: PROVEN (macOS arm64 16K)\n");
	return 0;
}
#endif // SPIKE_ON_TARGET

int main(void)
{
#if SPIKE_ON_TARGET
	return run_spike();
#else
	printf("[S0] SKIP (non-macOS-arm64)\n");
	printf("SHM-ARENA SPIKE: SKIP (non-target host)\n");
	return 0;
#endif
}
