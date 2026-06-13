/* T-F6 unit test: MMIO_APERTURE must NOT extend the trap hull.
 *
 * Design contract (Task B):
 *   MMIOBusRegister(base, size, MMIO_APERTURE, dev) records the aperture in
 *   a separate registry and does NOT modify mmio_bus_lo/mmio_bus_hi or add
 *   to the region dispatch table.  MMIOBusInRange(aperture_addr) must return
 *   false so that the Mach fault handler does not intercept guest writes to
 *   the framebuffer.
 *
 * This test FAILS before Task B (current mmio_bus.cpp extends the hull for
 * all kinds unconditionally) and PASSES after Task B fixes that.
 *
 * Build: make -C SheepShaver/src/machine test_mmio_aperture
 * Run:   ./SheepShaver/src/machine/test_mmio_aperture
 */

#include "mmio_bus.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int n_pass = 0, n_fail = 0;
#define CHECK(label, cond) do { \
    if (cond) { n_pass++; } \
    else { n_fail++; fprintf(stderr, "FAIL: %s\n", label); } \
} while (0)

static uint64_t nop_read(void *, uint32_t, unsigned) { return 0; }
static void     nop_write(void *, uint32_t, unsigned, uint64_t) {}

int main()
{
    static const MMIODevice trapped_dev = { "macio-stub", 0, nop_read, nop_write, 0 };
    static const MMIODevice aperture_dev = { "fb-aperture", 0, nop_read, nop_write, 0 };

    /* --- Baseline: register one MMIO_TRAPPED device and activate --- */
    CHECK("register trapped ok",
          MMIOBusRegister(0xF3000000, 0x80000, MMIO_TRAPPED, &trapped_dev));
    MMIOBusActivate();

    uint32_t hull_lo_before = mmio_bus_lo;
    uint32_t hull_hi_before = mmio_bus_hi;

    CHECK("hull covers trapped region (lo)",  hull_lo_before == 0xF3000000);
    CHECK("hull covers trapped region (hi)",  hull_hi_before == 0xF3080000);
    CHECK("InRange: trapped addr",   MMIOBusInRange(0xF3040000));
    CHECK("InRange: below hull",    !MMIOBusInRange(0xF2FFFFFF));

    /* --- Register MMIO_APERTURE at 0x81000000 (16 MB) --- */
    CHECK("register aperture ok",
          MMIOBusRegister(0x81000000, 16 * 1024 * 1024, MMIO_APERTURE, &aperture_dev));

    /* Hull must be UNCHANGED — aperture does not extend the trap range. */
    CHECK("hull lo unchanged after aperture", mmio_bus_lo == hull_lo_before);
    CHECK("hull hi unchanged after aperture", mmio_bus_hi == hull_hi_before);

    /* InRange must return false for aperture addresses. */
    CHECK("InRange: aperture base is false",  !MMIOBusInRange(0x81000000));
    CHECK("InRange: aperture mid  is false",  !MMIOBusInRange(0x81400000));
    CHECK("InRange: aperture last is false",  !MMIOBusInRange(0x81FFFFFF));

    /* Trapped region still dispatches normally. */
    CHECK("InRange: trapped still true",  MMIOBusInRange(0xF3040000));

    /* Aperture address must not appear in the region dispatch table. */
    CHECK("Lookup: aperture base not found", MMIOBusLookup(0x81000000) < 0);

    if (n_fail == 0)
        printf("RESULT: ALL PASS (%d checks)\n", n_pass);
    else
        printf("RESULT: %d FAIL  %d pass\n", n_fail, n_pass);

    return n_fail ? 1 : 0;
}
