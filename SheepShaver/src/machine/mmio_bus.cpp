/*
 *  mmio_bus.cpp - Machine Layer M1 MMIO bus (see mmio_bus.h / MACHINE-LAYER-PLAN.md §2b, §2g)
 *  Locking: one pthread_mutex per region, held only around the device handler.
 *  Reachable from the Mach exception-handler thread: no malloc, no stdio except abort paths.
 */

#include "mmio_bus.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct MMIORegion {
	uint32_t base, size;
	MMIORegionKind kind;
	const MMIODevice *dev;
	pthread_mutex_t lock;
	MMIORegionStats stats;       // mutated under lock (jit_faults/backpatches: __atomic)
	uint32_t idle_streak;
};

static MMIORegion regions[MMIO_MAX_REGIONS];
static int n_regions = 0;
bool mmio_bus_active = false;
uint32_t mmio_bus_lo = 0, mmio_bus_hi = 0;

struct MMIOAperture { uint32_t base, size; const char *label; };
static MMIOAperture apertures[MMIO_MAX_APERTURES];
static int n_apertures = 0;

bool MMIOApertureInRange(uint32_t addr)
{
	for (int i = 0; i < n_apertures; i++)
		if (addr - apertures[i].base < apertures[i].size) return true;
	return false;
}

bool MMIOBusRegister(uint32_t base, uint32_t size, MMIORegionKind kind, const MMIODevice *dev)
{
	if (size == 0 || !dev) return false;

	// MMIO_APERTURE: real guest RAM — record in a separate registry only.
	// Must NOT enter the trap hull (mmio_bus_lo/hi) or the dispatch table.
	if (kind == MMIO_APERTURE) {
		if (n_apertures >= MMIO_MAX_APERTURES) return false;
		apertures[n_apertures++] = { base, size, dev->name };
		return true;
	}

	if (n_regions >= MMIO_MAX_REGIONS || !dev->read || !dev->write)
		return false;
	for (int i = 0; i < n_regions; i++) {
		uint32_t b = regions[i].base, e = b + regions[i].size;
		uint32_t nb = base, ne = base + size;
		bool disjoint = (ne <= b) || (nb >= e);
		bool contained = (nb >= b && ne <= e) || (b >= nb && e <= ne);
		if (!disjoint && !contained)
			return false;   // partial overlap forbidden
	}
	MMIORegion *r = &regions[n_regions++];
	r->base = base; r->size = size; r->kind = kind; r->dev = dev;
	pthread_mutex_init(&r->lock, NULL);
	memset(&r->stats, 0, sizeof(r->stats));
	r->idle_streak = 0;
	if (mmio_bus_lo == mmio_bus_hi) { mmio_bus_lo = base; mmio_bus_hi = base + size; }
	else {
		if (base < mmio_bus_lo) mmio_bus_lo = base;
		if (base + size > mmio_bus_hi) mmio_bus_hi = base + size;
	}
	return true;
}

void MMIOBusActivate(void) { mmio_bus_active = (n_regions > 0); }

int MMIOBusLookup(uint32_t addr)
{
	int best = -1; uint32_t best_size = 0xFFFFFFFFu;
	for (int i = 0; i < n_regions; i++) {
		if (addr - regions[i].base < regions[i].size && regions[i].size < best_size) {
			best = i; best_size = regions[i].size;
		}
	}
	return best;
}

static MMIORegion *lookup_or_die(uint32_t addr)
{
	int i = MMIOBusLookup(addr);
	if (i < 0) {
		fprintf(stderr, "[MMIO] FATAL: access to unregistered device address 0x%08x "
		        "(bus hull 0x%08x-0x%08x)\n", addr, mmio_bus_lo, mmio_bus_hi);
		abort();   // abort-loudly rule (MACHINE-LAYER-PLAN §2b / §6)
	}
	return &regions[i];
}

// Out-of-line abort for cpu_emulation.h's Mac2HostAddr guard (keeps stdio/stdlib
// out of that widely-included header). Reached only on a contract violation.
void mmio_mac2host_abort(uint32_t addr)
{
	fprintf(stderr, "[MMIO] FATAL: Mac2HostAddr(0x%08x) inside device space - "
	        "use MMIOBusRead/Write\n", addr);
	abort();
}

uint64_t MMIOBusRead(uint32_t addr, unsigned size)
{
	MMIORegion *r = lookup_or_die(addr);
	pthread_mutex_lock(&r->lock);
	uint64_t v = r->dev->read(r->dev->opaque, addr, size);
	r->stats.reads++;
	bool idle = r->dev->read_is_idle && r->dev->read_is_idle(r->dev->opaque, addr, v);
	uint32_t streak = idle ? ++r->idle_streak : (r->idle_streak = 0);
	if (idle && streak >= MMIO_IDLE_THRESHOLD) { r->idle_streak = 0; r->stats.idle_sleeps++; }
	else streak = 0;   // local streak is nonzero only when we crossed the threshold this call (doubles as the post-unlock sleep flag)
	pthread_mutex_unlock(&r->lock);
	if (streak)
		usleep(MMIO_IDLE_SLEEP_US);   // outside the lock; the power-management hook (§2b)
	return v;
}

void MMIOBusWrite(uint32_t addr, unsigned size, uint64_t value)
{
	MMIORegion *r = lookup_or_die(addr);
	pthread_mutex_lock(&r->lock);
	r->dev->write(r->dev->opaque, addr, size, value);
	r->stats.writes++;
	r->idle_streak = 0;
	pthread_mutex_unlock(&r->lock);
}

bool MMIOBusWithRegion(uint32_t addr, void (*fn)(void *), void *opaque)
{
	int i = MMIOBusLookup(addr);
	if (i < 0) return false;
	pthread_mutex_lock(&regions[i].lock);
	fn(opaque);
	pthread_mutex_unlock(&regions[i].lock);
	return true;
}

void MMIOBusCountJITFault(uint32_t addr)
{
	int i = MMIOBusLookup(addr);
	if (i >= 0) __atomic_add_fetch(&regions[i].stats.jit_faults, 1, __ATOMIC_RELAXED);
}

void MMIOBusCountBackpatch(uint32_t addr)
{
	int i = MMIOBusLookup(addr);
	if (i >= 0) __atomic_add_fetch(&regions[i].stats.backpatches, 1, __ATOMIC_RELAXED);
}

bool MMIOBusGetStats(int idx, char name_out[32], uint32_t *base, uint32_t *size,
                     MMIORegionStats *out)
{
	if (idx < 0 || idx >= n_regions) return false;
	strncpy(name_out, regions[idx].dev->name, 31); name_out[31] = 0;
	*base = regions[idx].base; *size = regions[idx].size; *out = regions[idx].stats;
	return true;
}

void MMIOBusDumpStats(FILE *f)
{
	if (!n_regions) return;
	fprintf(f, "[MMIO] bus stats (%d regions, hull 0x%08x-0x%08x):\n",
	        n_regions, mmio_bus_lo, mmio_bus_hi);
	for (int i = 0; i < n_regions; i++) {
		const MMIORegionStats *s = &regions[i].stats;
		fprintf(f, "[MMIO]   %-12s 0x%08x+0x%06x reads=%llu writes=%llu "
		        "jit_faults=%llu backpatches=%llu idle_sleeps=%llu\n",
		        regions[i].dev->name, regions[i].base, regions[i].size,
		        (unsigned long long)s->reads, (unsigned long long)s->writes,
		        (unsigned long long)s->jit_faults, (unsigned long long)s->backpatches,
		        (unsigned long long)s->idle_sleeps);
	}
}
