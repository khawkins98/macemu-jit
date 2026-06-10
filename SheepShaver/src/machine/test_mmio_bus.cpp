/* Standalone unit test for mmio_bus.cpp. Build: make -C SheepShaver/src/machine test */
#include "mmio_bus.h"
#include <assert.h>
#include <string.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

static uint64_t last_read_addr; static unsigned last_read_size;
static uint64_t fake_read(void *, uint32_t addr, unsigned size)
{ last_read_addr = addr; last_read_size = size; return 0xA5; }
static uint32_t last_write_addr; static uint64_t last_write_val;
static void fake_write(void *, uint32_t addr, unsigned size, uint64_t v)
{ last_write_addr = addr; (void)size; last_write_val = v; }
static int idle_calls = 0;
static bool fake_idle(void *, uint32_t, uint64_t) { idle_calls++; return false; }
static bool always_idle(void *, uint32_t, uint64_t) { return true; }

int main()
{
	// Inactive by default; range check false everywhere.
	CHECK(!mmio_bus_active);
	CHECK(!MMIOBusInRange(0xF3012002));

	static const MMIODevice scc = { "scc-test", 0, fake_read, fake_write, fake_idle };
	static const MMIODevice stub = { "macio-stub", 0, fake_read, fake_write, 0 };
	// Container first, then contained region (most-specific wins).
	CHECK(MMIOBusRegister(0xF3000000, 0x80000, MMIO_TRAPPED, &stub));
	CHECK(MMIOBusRegister(0xF3012000, 0x1000, MMIO_TRAPPED, &scc));
	// Partial overlap rejected.
	CHECK(!MMIOBusRegister(0xF307F000, 0x2000, MMIO_TRAPPED, &stub));
	MMIOBusActivate();
	CHECK(mmio_bus_active);
	CHECK(MMIOBusInRange(0xF3000000) && MMIOBusInRange(0xF307FFFF));
	CHECK(!MMIOBusInRange(0xF2FFFFFF) && !MMIOBusInRange(0xF3080000));

	// Most-specific dispatch.
	int r_scc = MMIOBusLookup(0xF3012002);
	int r_stub = MMIOBusLookup(0xF3016000);
	CHECK(r_scc >= 0 && r_stub >= 0 && r_scc != r_stub);
	char name[32]; uint32_t base, size; MMIORegionStats st;
	CHECK(MMIOBusGetStats(r_scc, name, &base, &size, &st));
	CHECK(strcmp(name, "scc-test") == 0 && base == 0xF3012000 && size == 0x1000);

	CHECK(MMIOBusRead(0xF3012002, 1) == 0xA5);
	CHECK(last_read_addr == 0xF3012002 && last_read_size == 1);
	// Size propagation for wide accesses.
	(void)MMIOBusRead(0xF3012002, 8);
	CHECK(last_read_size == 8);
	MMIOBusWrite(0xF3012006, 1, 0x42);
	CHECK(last_write_addr == 0xF3012006 && last_write_val == 0x42);
	CHECK(idle_calls == 2);   // idle hint consulted on each read (both reads above)

	CHECK(MMIOBusGetStats(r_scc, name, &base, &size, &st));
	CHECK(st.reads == 2 && st.writes == 1 && st.jit_faults == 0);
	MMIOBusCountJITFault(0xF3012002);
	CHECK(MMIOBusGetStats(r_scc, name, &base, &size, &st) && st.jit_faults == 1);

	// --- Idle-streak / sleep path: device whose reads always report "no work" ---
	static const MMIODevice lazy = { "idle-test", 0, fake_read, fake_write, always_idle };
	CHECK(MMIOBusRegister(0xF3020000, 0x1000, MMIO_TRAPPED, &lazy));   // contained in stub
	int r_lazy = MMIOBusLookup(0xF3020000);
	CHECK(r_lazy >= 0);
	for (int i = 0; i < MMIO_IDLE_THRESHOLD; i++)
		(void)MMIOBusRead(0xF3020000, 1);
	CHECK(MMIOBusGetStats(r_lazy, name, &base, &size, &st) && st.idle_sleeps == 1);
	// A write resets the streak: THRESHOLD-1 more idle reads stay at 1 sleep...
	MMIOBusWrite(0xF3020000, 1, 0);
	for (int i = 0; i < MMIO_IDLE_THRESHOLD - 1; i++)
		(void)MMIOBusRead(0xF3020000, 1);
	CHECK(MMIOBusGetStats(r_lazy, name, &base, &size, &st) && st.idle_sleeps == 1);
	// ...and one more read crosses the threshold again.
	(void)MMIOBusRead(0xF3020000, 1);
	CHECK(MMIOBusGetStats(r_lazy, name, &base, &size, &st) && st.idle_sleeps == 2);

	// --- Reverse-direction containment: small region first, then its container ---
	static const MMIODevice inner = { "inner", 0, fake_read, fake_write, 0 };
	static const MMIODevice outer = { "outer", 0, fake_read, fake_write, 0 };
	CHECK(MMIOBusRegister(0xF3040000, 0x100, MMIO_TRAPPED, &inner));
	CHECK(MMIOBusRegister(0xF3040000, 0x1000, MMIO_TRAPPED, &outer));
	int r_inner = MMIOBusLookup(0xF3040000);
	CHECK(r_inner >= 0);
	CHECK(MMIOBusGetStats(r_inner, name, &base, &size, &st) && strcmp(name, "inner") == 0);

	// M2: run a callback under the owning region's lock (scheduler callbacks).
	static uint32_t with_region_called;
	struct WR { static void fn(void *op) { with_region_called = *(uint32_t *)op + 1; } };
	uint32_t token = 41;
	CHECK(MMIOBusWithRegion(0xF3012002, WR::fn, &token));
	CHECK(with_region_called == 42);
	CHECK(!MMIOBusWithRegion(0xF2000000, WR::fn, &token));   // no owning region

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
