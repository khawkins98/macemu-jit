/*
 *  mmio_bus.h - Machine Layer M1 MMIO bus (MACHINE-LAYER-PLAN.md §2b)
 *
 *  Registry of guest physical address ranges -> device handlers.
 *  Three dispatch paths land here: JIT Mach-fault decode (mmio_machfault.cpp),
 *  interpreter range check (vm.hpp), explicit host accessors (MMIOBusRead/Write).
 *  Values at this API are ARCHITECTURAL (what the PPC load yields after its REV);
 *  the JIT fault path byte-swaps to/from raw BE at the injection layer.
 */

#ifndef MMIO_BUS_H
#define MMIO_BUS_H

#include <stdint.h>
#include <stdio.h>

enum MMIORegionKind {
	MMIO_TRAPPED  = 0,   // unmapped, fault-dispatched device registers
	MMIO_APERTURE = 1    // real memory, direct access (future Metal framebuffer); M1: registry-only
};

struct MMIODevice {
	const char *name;
	void *opaque;
	// size in bytes: 1,2,4,8. addr is the absolute guest address.
	uint64_t (*read)(void *opaque, uint32_t addr, unsigned size);
	void (*write)(void *opaque, uint32_t addr, unsigned size, uint64_t value);
	// Optional idle hint: return true if this read found "no work" (e.g. SCC RR0 bit0==0).
	// NULL = never idle. Bus sleeps briefly after MMIO_IDLE_THRESHOLD consecutive idles.
	bool (*read_is_idle)(void *opaque, uint32_t addr, uint64_t value);
};

#define MMIO_MAX_REGIONS    16
#define MMIO_IDLE_THRESHOLD 256
#define MMIO_IDLE_SLEEP_US  200

// Registration (init-time only, single-threaded). Overlap with an existing region
// is allowed only if fully contained (most-specific wins); else returns false.
extern bool MMIOBusRegister(uint32_t base, uint32_t size, MMIORegionKind kind,
                            const MMIODevice *dev);

// Activation gate. False by default; set by MMIOBusActivate() exactly once at startup.
// Hot paths read mmio_bus_active first (predicted-untaken branch on paravirtual).
extern bool mmio_bus_active;
extern uint32_t mmio_bus_lo, mmio_bus_hi;   // [lo, hi) hull of all regions
extern void MMIOBusActivate(void);

static inline bool MMIOBusInRange(uint32_t addr)
{
	return addr - mmio_bus_lo < mmio_bus_hi - mmio_bus_lo;
}

// Dispatch. Unregistered address inside the hull -> loud abort (PC-less variant;
// callers with a guest PC should log it first). Takes the owning region's lock.
extern uint64_t MMIOBusRead(uint32_t addr, unsigned size);
extern void MMIOBusWrite(uint32_t addr, unsigned size, uint64_t value);

// Out-of-line abort helper for cpu_emulation.h's Mac2HostAddr device-space guard.
extern void mmio_mac2host_abort(uint32_t addr);

// Lookup without dispatch (for tests and for the fault path's "is this ours" check).
// Returns region index or -1.
extern int MMIOBusLookup(uint32_t addr);

// Telemetry: per-region atomic counters.
struct MMIORegionStats {
	uint64_t reads, writes, jit_faults, backpatches, idle_sleeps;
};
extern bool MMIOBusGetStats(int region_index, char name_out[32], uint32_t *base,
                            uint32_t *size, MMIORegionStats *out);
extern void MMIOBusDumpStats(FILE *f);
// Fault-path bookkeeping (called by mmio_machfault.cpp):
extern void MMIOBusCountJITFault(uint32_t addr);
extern void MMIOBusCountBackpatch(uint32_t addr);

// Mach-fault dispatch entry (implemented in mmio_machfault.cpp; declared here so
// sheepshaver_glue.cpp needs only this header). regs = ARM_THREAD_STATE64 __x base,
// pc accessors handled inside. Returns true if the access was serviced and the
// thread state was modified in place.
extern bool MMIOMachFaultDispatch(uint32_t guest_addr, void *thread_state64);

#endif
