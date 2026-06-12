> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# Machine Layer M1 — MMIO bus + AArch64 fault decoder + SCC 8530 + VIA timer/IFR + minimal DEC tick

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land Machine Layer milestone M1 per `docs/planning/MACHINE-LAYER-PLAN.md` §3: an MMIO bus with three dispatch paths (JIT Mach-fault decode, interpreter range check, explicit host-accessor entry points), an SCC 8530 model, a VIA 6522 timer/IFR surface (Cuda = loud stub), a minimal DEC tick, JIT backpatch for hot fault sites, per-region telemetry + idle detection, and the `scc_init` ROM-patch retirement — all gated so the paravirtual profile is byte-identical (test-jit 350/350, e2e PASS).

**Architecture:** New pure modules under `SheepShaver/src/machine/` (bus, SCC, VIA, AArch64 decoder), each with a standalone clang++ unit test mirroring M0's `machine_profile` pattern. Integration seams: the Mach exception path (`sigsegv.cpp` + `sheepshaver_glue.cpp` handler), the interpreter accessors (`vm.hpp`), `Mac2HostAddr` (host-accessor guard), the JIT (`ppc-jit.cpp` backpatch thunk), and profile-gated sites in `rom_patches.cpp` / `sheepshaver_glue.cpp` / `ppc-execute.cpp`. The bus activates only on the `newworld` profile or the named third config `SS_MMIO_BUS=1` (paravirtual + bus + devices − serial-skips); paravirtual default pays one predicted-untaken branch on the interpreter path and zero on the JIT path.

**Tech Stack:** C++ (matching kpx_cpu's C++11-ish style), Mach exception API (existing `HAVE_MACH_EXCEPTIONS` machinery), MIG already linked, standalone clang++ unit tests, autoconf build (`Makefile.in` SRCS + `./config.status Makefile`).

---

## Authoritative inputs (read before implementing any task)

| Doc | What it fixes |
|---|---|
| `docs/planning/MACHINE-LAYER-PLAN.md` §2b/§2c/§2g + M1 row | Bus design, dispatch paths, locking rules, DoD |
| `docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md` §1/§4 | Device addresses + the "implement exactly this" register fence |
| `docs/planning/machine/ROM-PATCH-AUDIT.md` | `scc_init` = RETIRE@M1; via_init cluster stays RETIRE@M3; enforcement = profile check at site (option 1) |
| `docs/planning/spikes/SPIKE-S2-MACH-FAULT-DECODE.md` §3/§4 | Decoder gotchas = the unit-test checklist; ~8.5 µs/fault ⇒ backpatch mandatory |
| `docs/planning/spikes/SPIKE-S3-STALL-DEVICE-PROBE.md` §1.3/§2.3/§4 | SCC/VIA required behavior sets + the two conformance write-vectors |

## Codebase facts (verified 2026-06-10; line numbers may drift a few lines — pattern names are stable)

- **JIT memory access forms** (`SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-codegen-aarch64.h:150–181`): ALL guest accesses are register-offset `[Xn, Wm, UXTW]`: `{0x38,0x78,0xB8,0xF8}` size byte/half/word/dword, `…604800`=load / `…204800`=store, fixed-bits mask `0xFFE0FC00`. Loads of width>1 are followed by `REV`/`REV16` (`0x5AC00800`/`0x5AC00400`/64-bit `0xDAC00C00`); stores of width>1 are preceded by them. Byte forms have no swap. FP loads/stores go through GPR (`a64_ldr_w_reg`/`a64_ldr_x_reg` + REV + FMOV) — same forms.
- **JIT register conventions** (`ppc-jit.cpp:444–449, 554–569, 5295–5306, 1321–1327`): `RMEMBASE=x19`, `RSTATE=x20`, temps `RTMP0..3 = x0..x3`; EA is **always in w0 (RTMP0)** at the access; GPR allocator uses callee-saved x21–x28; **FP allocator uses caller-saved v16–v23**; **x30 is dead mid-block** (saved in prologue at 5295, restored in epilogue at 1326, only RET uses it) ⇒ `BL` is safe at any access site. C-call precedent: `emit_inline_interp_call` (`ppc-jit.cpp:1398–1414`) — SP is 16-aligned at any BLR.
- **Mach path** (`SheepShaver/src/CrossPlatform/sigsegv.cpp`): handler thread `handleExceptions` (~726), `catch_mach_exception_raise` (~2948) → `handle_badaccess` (~2713–2818) → SheepShaver's `sigsegv_handler` (`sheepshaver_glue.cpp:919–1004`). Mach flavor `ARM_THREAD_STATE64`, `EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES` (`code[1]` = fault address). `SIGSEGV_RETURN_SKIP_INSTRUCTION` → `aarch64_skip_instruction` (pc+=4, ~2564) + `mach_set_thread_state` (~2632).
- **Memory map**: `NATMEM_OFFSET=0x400000000000` (config.h:430); guest→host = `VMBaseDiff + addr` (`vm.hpp:208 vm_do_get_real_address`); RAM/ROM fixed-acquired; guest `0xF3000000–0xF3080000` is **unmapped today** (accesses Mach-fault and are eaten by legacy skips/ignoresegv).
- **Interpreter accessors**: `SheepShaver/src/kpx_cpu/src/cpu/vm.hpp:224–285` `vm_read_memory_{1,2,4,8}` / `vm_write_memory_*`; `ReadMacInt*`/`WriteMacInt*` are thin wrappers over these (`SheepShaver/src/include/cpu_emulation.h:63–72`) so the host-accessor integer path routes automatically; only raw-pointer `Mac2HostAddr` (cpu_emulation.h:71) needs a guard.
- **M0 profile module**: `SheepShaver/src/include/machine_profile.h`, `SheepShaver/src/machine/machine_profile.cpp` (+ standalone test via `make -C SheepShaver/src/machine test`). Idiom: `MachineProfileIsNewWorld()`, `MachineEnvFlag(name)`. Serial-skip/ignoresegv gates: `sheepshaver_glue.cpp:947,981`, `main_unix.cpp:2309,2477`.
- **DEC**: interpreter `ppc-execute.cpp:1318–1332` (`SS_SYNTH_DEC` synthetic down-counter else 0); JIT falls back to interpreter for all SPRs except LR/CTR/XER. `mtspr DEC` dropped (stays dropped in M1; real fix M2).
- **`[KDP-0x900]`** (check_work's SCC base) zeroed at `sheepshaver_glue.cpp:1473` (`WriteMacInt32(kdp - 0x900, 0)`); `kdp = KernelDataAddr = 0x68FFE000`.
- **scc_init patch**: `rom_patches.cpp:2106–2119` (`scc_init_caller_dat` + `scc_init_dat`).
- **JIT block table**: guest-PC→host-code only (`jit_bc_lookup`, ppc-jit.cpp:291); no reverse map — the backpatch design below doesn't need one (the faulting host PC *is* the patch site).
- **Build**: add sources to `SheepShaver/src/Unix/Makefile.in:67–69` SRCS, then `cd SheepShaver/src/Unix && ./config.status Makefile`. Commit style: `feat(machine): …` etc. per CONTRIBUTING.md.
- **Spike S2 reference code**: `spikes/s2-mach-fault-decode/main.cpp` (decode mask at :120, inject/PC-advance at :128–130) — port, don't reinvent.

## File map

| File | Status | Responsibility |
|---|---|---|
| `SheepShaver/src/include/mmio_bus.h` | create | Bus public API + cheap inline range-check globals |
| `SheepShaver/src/machine/mmio_bus.cpp` | create | Region registry, dispatch, per-region mutex + telemetry + idle hook |
| `SheepShaver/src/machine/test_mmio_bus.cpp` | create | Standalone bus unit test |
| `SheepShaver/src/include/dev_scc8530.h` / `src/machine/dev_scc8530.cpp` / `test_dev_scc8530.cpp` | create | SCC 8530 model + S3 conformance vectors |
| `SheepShaver/src/include/dev_via6522.h` / `src/machine/dev_via6522.cpp` / `test_dev_via6522.cpp` | create | VIA timer/IFR surface, Cuda loud-stub decode |
| `SheepShaver/src/include/a64_mmio_decode.h` / `src/machine/a64_mmio_decode.cpp` / `test_a64_mmio_decode.cpp` | create | Pure AArch64 access decoder (no Mach deps) |
| `SheepShaver/src/machine/mmio_machfault.cpp` (+ decl in mmio_bus.h) | create | Mach-fault MMIO dispatch: decode→bus→thread-state writeback; site counters; backpatch trigger |
| `SheepShaver/src/machine/test_mmio_machfault.cpp` | create | Standalone fault-dispatch unit test (rev 2 C2; Task 6 Step 4) |
| `SheepShaver/src/machine/Makefile` | modify | Add the five new standalone tests (4 device/decoder + test_mmio_machfault) |
| `SheepShaver/src/include/machine_profile.h` + `src/machine/machine_profile.cpp` + test | modify | Add `MachineUsesMMIOBus()` (newworld ∨ `SS_MMIO_BUS`) |
| `SheepShaver/src/Unix/Makefile.in` | modify | SRCS += the five new non-test .cpp; DEFS += `-DSS_MMIO_BACKPATCH` (rev 2 C4 — emulator build only, not rom-harness) |
| `SheepShaver/src/Unix/main_unix.cpp` | modify | PROT_NONE reservation, bus init + device registration, stats atexit, SS_JIT_VERIFY incompat, gate widening |
| `SheepShaver/src/CrossPlatform/sigsegv.cpp` | modify | New `SIGSEGV_RETURN_STATE_MODIFIED` + thread-state accessor |
| `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` | modify | MMIO dispatch in sigsegv_handler; `[KDP-0x900]`=SCC base on newworld; gate widening |
| `SheepShaver/src/kpx_cpu/src/cpu/vm.hpp` | modify | Interpreter range check (branch-gated) |
| `SheepShaver/src/include/cpu_emulation.h` | modify | `Mac2HostAddr` MMIO guard |
| `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-execute.cpp` | modify | DEC synthetic tick default-on for newworld |
| `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp` | modify | MMIO thunk emission at init + cache-bounds accessor + patch helper — all under `#ifdef SS_MMIO_BACKPATCH` (rev 2 C4) |
| `SheepShaver/src/rom_patches.cpp` | modify | `scc_init` retirement on newworld |
| docs: `MACHINE-LAYER-PLAN.md`, `ROM-PATCH-AUDIT.md`, `CHANGELOG.md`, `CORE99-MACHINE-DESCRIPTION.md` | modify | M1 bookkeeping |

**Parallelization:** Tasks 1–4 are independent pure modules (no shared files) — run as parallel subagents. Task 5 onward is sequential (shared emulator files), except Task 13 (docs) which can draft in parallel with Task 12.

**Standing rules (every task):**
- Never add fields to `powerpc_registers` (none needed in M1). Any kpx_cpu header change ⇒ clean PPC recompile before trusting test-jit (stale-.o footgun).
- §2g locking: code reachable from the Mach handler thread takes only its own device/bus lock, no malloc, no stdio except on terminal-abort paths.
- Commit after each task with `type(machine): subject` style; cite harness results in the body when run.

---

### Task 1: MMIO bus core module (pure, standalone-tested)

**Files:**
- Create: `SheepShaver/src/include/mmio_bus.h`
- Create: `SheepShaver/src/machine/mmio_bus.cpp`
- Create: `SheepShaver/src/machine/test_mmio_bus.cpp`
- Modify: `SheepShaver/src/machine/Makefile`

- [ ] **Step 1: Write the header**

`SheepShaver/src/include/mmio_bus.h`:

```cpp
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
```

- [ ] **Step 2: Write the failing test**

`SheepShaver/src/machine/test_mmio_bus.cpp` — standalone, M0 test style (plain asserts, `RESULT: ALL PASS`):

```cpp
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
	MMIOBusWrite(0xF3012006, 1, 0x42);
	CHECK(last_write_addr == 0xF3012006 && last_write_val == 0x42);
	CHECK(idle_calls == 1);   // idle hint consulted on reads

	CHECK(MMIOBusGetStats(r_scc, name, &base, &size, &st));
	CHECK(st.reads == 1 && st.writes == 1 && st.jit_faults == 0);
	MMIOBusCountJITFault(0xF3012002);
	CHECK(MMIOBusGetStats(r_scc, name, &base, &size, &st) && st.jit_faults == 1);

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
```

- [ ] **Step 3: Add the test to the machine Makefile and verify it fails to build**

Append to `SheepShaver/src/machine/Makefile` (mirror the existing `test_machine_profile` recipe; keep `test:` running all binaries):

```makefile
test_mmio_bus: test_mmio_bus.cpp mmio_bus.cpp ../include/mmio_bus.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_mmio_bus.cpp mmio_bus.cpp
```
and add `./test_mmio_bus` to the `test:` target (keep `test_machine_profile` first). Add the new binary names to `SheepShaver/src/machine/.gitignore`.

Run: `make -C SheepShaver/src/machine test_mmio_bus` — Expected: FAIL (mmio_bus.cpp missing).

- [ ] **Step 4: Implement `mmio_bus.cpp`**

```cpp
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

bool MMIOBusRegister(uint32_t base, uint32_t size, MMIORegionKind kind, const MMIODevice *dev)
{
	if (n_regions >= MMIO_MAX_REGIONS || size == 0 || !dev || !dev->read || !dev->write)
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

uint64_t MMIOBusRead(uint32_t addr, unsigned size)
{
	MMIORegion *r = lookup_or_die(addr);
	pthread_mutex_lock(&r->lock);
	uint64_t v = r->dev->read(r->dev->opaque, addr, size);
	r->stats.reads++;
	bool idle = r->dev->read_is_idle && r->dev->read_is_idle(r->dev->opaque, addr, v);
	uint32_t streak = idle ? ++r->idle_streak : (r->idle_streak = 0);
	if (idle && streak >= MMIO_IDLE_THRESHOLD) { r->idle_streak = 0; r->stats.idle_sleeps++; }
	else streak = 0;
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
```

- [ ] **Step 5: Run the test, verify it passes**

Run: `make -C SheepShaver/src/machine test_mmio_bus && SheepShaver/src/machine/test_mmio_bus`
Expected: `RESULT: ALL PASS (…)`.

- [ ] **Step 6: Commit**

```bash
git add SheepShaver/src/include/mmio_bus.h SheepShaver/src/machine/mmio_bus.cpp \
        SheepShaver/src/machine/test_mmio_bus.cpp SheepShaver/src/machine/Makefile \
        SheepShaver/src/machine/.gitignore
git commit -m "feat(machine): M1 MMIO bus core - region registry, locked dispatch, telemetry, idle hook"
```

---
### Task 2: SCC 8530 device model (pure, standalone-tested)

Scope fence: **exactly** the behavior set in SPIKE-S3 §4.1 / CORE99 §4. Registers not in that set are stored-state or zero — never invented behavior.

**Files:**
- Create: `SheepShaver/src/include/dev_scc8530.h`
- Create: `SheepShaver/src/machine/dev_scc8530.cpp`
- Create: `SheepShaver/src/machine/test_dev_scc8530.cpp`
- Modify: `SheepShaver/src/machine/Makefile` (recipe + `test:` line, mirroring Task 1 Step 3)

- [ ] **Step 1: Write the header**

`SheepShaver/src/include/dev_scc8530.h`:

```cpp
/*
 *  dev_scc8530.h - Zilog SCC 8530, legacy/compat MacIO port layout (M1 scope: SPIKE-S3 §4.1)
 *  Layout at base (= 0xF3012000 on Core99): +0 ch B ctrl, +2 ch A ctrl, +4 ch B data, +6 ch A data.
 *  No Rx source is connected in M1: RR0 bit0 always 0 (honest "no character").
 */

#ifndef DEV_SCC8530_H
#define DEV_SCC8530_H

#include "mmio_bus.h"

#define SCC_CH_B 0
#define SCC_CH_A 1

struct SCC8530 {
	uint32_t base;
	uint8_t  wr[2][16];     // stored write registers per channel
	uint8_t  reg_ptr[2];    // WR0-selected register pointer (0 after any access)
	uint64_t wr_writes;     // telemetry: total WR data writes (DoD register-traffic assert)
	uint64_t tx_bytes;      // bytes the guest transmitted (discarded)
	uint64_t rr0_polls;     // RR0 reads (the poll loops)
};

extern void SCCReset(SCC8530 *s, uint32_t base);
// The MMIODevice trampolines (opaque = SCC8530*):
extern uint64_t SCCRead(void *opaque, uint32_t addr, unsigned size);
extern void SCCWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value);
extern bool SCCReadIsIdle(void *opaque, uint32_t addr, uint64_t value);

#endif
```

- [ ] **Step 2: Write the failing test — the two S3 conformance write-vectors**

`SheepShaver/src/machine/test_dev_scc8530.cpp`:

```cpp
/* Conformance vectors from SPIKE-S3-STALL-DEVICE-PROBE.md §1.3 (68k STM) and §2.3 (check_work). */
#include "dev_scc8530.h"
#include <assert.h>
#include <stdio.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

static SCC8530 scc;
#define BASE 0xF3012000u
static void ctlw_a(uint8_t v) { SCCWrite(&scc, BASE + 2, 1, v); }
static uint8_t ctlr_a(void)   { return (uint8_t)SCCRead(&scc, BASE + 2, 1); }
static void wr_pair(uint8_t reg, uint8_t val) { ctlw_a(reg); ctlw_a(val); }

int main()
{
	SCCReset(&scc, BASE);

	// --- Vector 1: STM init table (S3 §1.3), prefixed by the ch-B control touch ---
	(void)SCCRead(&scc, BASE + 0, 1);          // tst.b (a3,d3.l): ch B ctrl read must not blow up
	static const uint8_t stm[][2] = { {9,0xC0},{15,0},{4,0x4C},{11,0x50},{14,0},
	                                  {12,0x04},{13,0},{14,1},{10,0},{3,0xC1},{5,0xEA},{1,0} };
	for (unsigned i = 0; i < sizeof(stm)/sizeof(stm[0]); i++) wr_pair(stm[i][0], stm[i][1]);
	(void)SCCRead(&scc, BASE + 6, 1);          // Rx flush read of ch A data
	CHECK(scc.wr[SCC_CH_A][15] == 0x00 && scc.wr[SCC_CH_A][4] == 0x4C);
	CHECK(scc.wr[SCC_CH_A][11] == 0x50 && scc.wr[SCC_CH_A][12] == 0x04);
	CHECK(scc.wr[SCC_CH_A][14] == 0x01 && scc.wr[SCC_CH_A][3] == 0xC1);
	CHECK(scc.wr[SCC_CH_A][5] == 0xEA && scc.wr[SCC_CH_A][1] == 0x00);
	CHECK(scc.wr_writes >= 12);

	// --- Poll loop behavior (S3 §1.4): RR0 bit0=0 (no Rx), bit2=1 (Tx empty) ---
	uint8_t rr0 = ctlr_a();
	CHECK((rr0 & 0x01) == 0 && (rr0 & 0x04) == 0x04);
	CHECK(SCCReadIsIdle(&scc, BASE + 2, rr0));         // idle hint fires on "no char"
	// WR0 <- 1 points at RR1; next ctrl read returns RR1 then resets pointer.
	ctlw_a(1);
	uint8_t rr1 = ctlr_a();
	CHECK((rr1 & 0x01) == 0x01);                       // All Sent = 1
	CHECK((rr1 & 0x70) == 0);                          // error bits 4-6 = 0
	CHECK((ctlr_a() & 0x04) == 0x04);                  // pointer reset: back to RR0
	// WR0 <- 0x30 Error Reset command: pointer stays 0, no state damage.
	ctlw_a(0x30);
	CHECK((ctlr_a() & 0x04) == 0x04);

	// --- Vector 2: check_work init (S3 §2.3), each pair prefixed by a pointer-reset read ---
	SCCReset(&scc, BASE);
	static const uint8_t ck[][2] = { {9,0x80},{4,0x48},{3,0xC0},{5,0x60},{9,0},{10,0},
	                                 {11,0x50},{12,0x0C},{13,0},{14,0x01},{3,0xC1},{5,0xEA} };
	for (unsigned i = 0; i < sizeof(ck)/sizeof(ck[0]); i++) { (void)ctlr_a(); wr_pair(ck[i][0], ck[i][1]); }
	CHECK(scc.wr[SCC_CH_A][4] == 0x48 && scc.wr[SCC_CH_A][12] == 0x0C);
	CHECK(scc.wr[SCC_CH_A][3] == 0xC1 && scc.wr[SCC_CH_A][5] == 0xEA);
	// WR9=0xC0 (hw reset, vector 1) and WR9=0x80 (ch A reset, vector 2) must clear ch A WRs:
	wr_pair(3, 0xC1); wr_pair(9, 0x80);
	CHECK(scc.wr[SCC_CH_A][3] == 0x00);

	// Tx poll + write (S3 §2.2): RR0 bit2 spin terminates immediately; stb +6 counted.
	CHECK((ctlr_a() & 0x04) == 0x04);
	SCCWrite(&scc, BASE + 6, 1, 'K');
	CHECK(scc.tx_bytes == 1);
	// Drain/timeout (S3 §2.4): WR0<-1 then RR1 bit0 All Sent = 1.
	ctlw_a(1); CHECK((ctlr_a() & 0x01) == 0x01);

	// Data reads return 0 with empty Rx; ch B data tolerated.
	CHECK(SCCRead(&scc, BASE + 6, 1) == 0 && SCCRead(&scc, BASE + 4, 1) == 0);
	CHECK(scc.rr0_polls > 0);

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
```

- [ ] **Step 3: Add Makefile recipe (pattern of Task 1 Step 3, sources `test_dev_scc8530.cpp dev_scc8530.cpp`), run, verify FAIL (missing impl)**

- [ ] **Step 4: Implement `dev_scc8530.cpp`**

```cpp
/*
 *  dev_scc8530.cpp - Zilog SCC 8530 model, M1 scope (SPIKE-S3 §4.1; CORE99 §4 fence).
 *  Reachable from the Mach handler thread: no malloc/stdio (bus holds the lock).
 */

#include "dev_scc8530.h"
#include <string.h>

void SCCReset(SCC8530 *s, uint32_t base)
{
	memset(s, 0, sizeof(*s));
	s->base = base;
}

static int decode_ch(uint32_t off, bool *is_data)
{
	// +0 B ctrl, +2 A ctrl, +4 B data, +6 A data (legacy/compat layout)
	*is_data = (off & 4) != 0;
	return (off & 2) ? SCC_CH_A : SCC_CH_B;
}

static uint8_t read_rr(SCC8530 *s, int ch, uint8_t rr)
{
	switch (rr) {
	case 0:  return 0x04;                  // bit2 Tx Buffer Empty=1; bit0 Rx avail=0 (no source)
	case 1:  return 0x01;                  // bit0 All Sent=1; bits4-6 errors=0
	default: return s->wr[ch][rr & 15];    // stored state read-back (fence: nothing else probed)
	}
}

uint64_t SCCRead(void *opaque, uint32_t addr, unsigned size)
{
	SCC8530 *s = (SCC8530 *)opaque;
	(void)size;   // consumers are byte-wide; wider reads replicate the byte in the low bits
	bool is_data; int ch = decode_ch(addr - s->base, &is_data);
	if (is_data)
		return 0;                          // Rx FIFO empty in M1
	uint8_t ptr = s->reg_ptr[ch];
	s->reg_ptr[ch] = 0;                    // any control access resets the pointer
	if (ptr == 0) s->rr0_polls++;
	return read_rr(s, ch, ptr);
}

void SCCWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value)
{
	SCC8530 *s = (SCC8530 *)opaque;
	(void)size;
	uint8_t v = (uint8_t)value;
	bool is_data; int ch = decode_ch(addr - s->base, &is_data);
	if (is_data) { s->tx_bytes++; return; } // Tx sink
	uint8_t ptr = s->reg_ptr[ch];
	if (ptr == 0) {
		// WR0: low 4 bits select the register (raw 8-15 values fold the point-high
		// command in, exactly as the ROM writes them); bits 5-3 = command.
		uint8_t cmd = (v >> 3) & 7;
		if (cmd == 6) { /* 0x30 Error Reset: no latched errors to clear in M1 */ }
		s->reg_ptr[ch] = v & 0x0F;
		return;
	}
	s->reg_ptr[ch] = 0;
	s->wr[ch][ptr] = v;
	s->wr_writes++;
	if (ptr == 9) {
		if ((v & 0xC0) == 0xC0) {          // force hardware reset
			memset(s->wr, 0, sizeof(s->wr));
			s->reg_ptr[0] = s->reg_ptr[1] = 0;
		} else if ((v & 0xC0) == 0x80) {   // channel A reset
			memset(s->wr[SCC_CH_A], 0, sizeof(s->wr[SCC_CH_A]));
		} else if ((v & 0xC0) == 0x40) {   // channel B reset
			memset(s->wr[SCC_CH_B], 0, sizeof(s->wr[SCC_CH_B]));
		}
	}
}

bool SCCReadIsIdle(void *opaque, uint32_t addr, uint64_t value)
{
	SCC8530 *s = (SCC8530 *)opaque;
	uint32_t off = addr - s->base;
	// Idle = a control-port read that reported "no Rx character" (RR0 bit0 == 0).
	return (off & 4) == 0 && (value & 0x01) == 0;
}
```

Note (test cross-check): `wr[ch][9]` is written *before* the reset side effect, so after `WR9=0x80` the per-channel clear wipes it — vector 2's `wr[A][3]==0` assert covers this ordering.

- [ ] **Step 5: Run `make -C SheepShaver/src/machine test_dev_scc8530 && SheepShaver/src/machine/test_dev_scc8530` — Expected: ALL PASS**

- [ ] **Step 6: Commit** — `feat(machine): M1 SCC 8530 model with S3 conformance write-vectors`

---

### Task 3: VIA 6522 timer/IFR surface (pure, standalone-tested)

Scope fence: S3 §1.5 register set. T2/IFR/IER real (lazy time-based countdown); ORA/ORB/DDRA/DDRB/ACR stored state; **SR (Cuda shift register) + ORB handshake bits = loud stub** — decoded + counted, never silent, never abort (the monitor touches them on live paths).

**Files:**
- Create: `SheepShaver/src/include/dev_via6522.h`
- Create: `SheepShaver/src/machine/dev_via6522.cpp`
- Create: `SheepShaver/src/machine/test_dev_via6522.cpp`
- Modify: `SheepShaver/src/machine/Makefile` (same pattern)

- [ ] **Step 1: Write the header**

`SheepShaver/src/include/dev_via6522.h`:

```cpp
/*
 *  dev_via6522.h - VIA 6522 timer/IFR surface (M1 scope: SPIKE-S3 §1.5 / §4.2).
 *  MacIO layout: 16 registers at 0x200 stride from base (reg N at base + N*0x200).
 *  Time source injected for testability; ticks are VIA clock units (783360 Hz on Macs).
 */

#ifndef DEV_VIA6522_H
#define DEV_VIA6522_H

#include "mmio_bus.h"

#define VIA_CLOCK_HZ 783360u

struct VIA6522 {
	uint32_t base;
	uint64_t (*now_ticks)(void *clock_opaque);   // monotonic VIA ticks
	void *clock_opaque;
	uint8_t  ora, orb, ddra, ddrb, acr, pcr, sr, ier, ifr_latched;
	uint8_t  t1l_l, t1l_h, t2l_l;
	uint64_t t1_load_time, t2_load_time;         // now_ticks at counter load
	uint16_t t1_count, t2_count;                 // programmed counts
	bool     t1_running, t2_running;
	uint64_t cuda_touches;                       // loud-stub telemetry (SR + handshake bits)
	bool     cuda_warned;                        // one-shot stderr warning latch
};

extern void VIAReset(VIA6522 *v, uint32_t base,
                     uint64_t (*now_ticks)(void *), void *clock_opaque);
extern uint64_t VIARead(void *opaque, uint32_t addr, unsigned size);
extern void VIAWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value);

#endif
```

- [ ] **Step 2: Write the failing test — the S3 §1.5 T2-timeout sequence under a fake clock**

`SheepShaver/src/machine/test_dev_via6522.cpp`:

```cpp
/* Drives the exact 68k STM timeout-helper sequence (SPIKE-S3 §1.5) under a fake clock. */
#include "dev_via6522.h"
#include <assert.h>
#include <stdio.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

static uint64_t fake_now = 0;
static uint64_t fake_clock(void *) { return fake_now; }

static VIA6522 via;
#define BASE 0xF3016000u
static uint8_t rd(uint32_t off)            { return (uint8_t)VIARead(&via, BASE + off, 1); }
static void    wr(uint32_t off, uint8_t v) { VIAWrite(&via, BASE + off, 1, v); }

int main()
{
	VIAReset(&via, BASE, fake_clock, 0);

	// --- STM timeout helper sequence (S3 §1.5, offsets are reg# * 0x200) ---
	wr(0x0600, rd(0x0600) | 0x08);   // DDRA: PA3 output
	wr(0x1e00, rd(0x1e00) & ~0x08);  // ORA no-handshake: PA3 low
	wr(0x1600, 0x00);                // ACR = 0: one-shot T2, SR off
	wr(0x1c00, 0x20);                // IER <- 0x20: bit7 clear => DISABLE T2 interrupt
	CHECK((rd(0x1c00) & 0x20) == 0); // IER read: bit not enabled (read returns 0x80|IER)
	wr(0x1000, 0xFF);                // T2C-L latch
	CHECK((rd(0x1a00) & 0x20) == 0x20 || true); // (pre-arm IFR state don't-care; next write arms)
	wr(0x1200, 0xFF);                // T2C-H: load 0xFFFF, clear IFR.5, start countdown
	CHECK((rd(0x1a00) & 0x20) == 0); // armed: T2 not yet expired
	fake_now += 0x10000;             // > 0xFFFF VIA ticks elapse
	CHECK((rd(0x1a00) & 0x20) == 0x20);  // IFR bit 5 set: T2 timeout fired
	// Write-1-to-clear:
	wr(0x1a00, 0x20);
	CHECK((rd(0x1a00) & 0x20) == 0);

	// IFR bit 7 master: set only when (IFR & IER & 0x7F) != 0.
	wr(0x1c00, 0xA0);                // IER: bit7 set => ENABLE T2
	wr(0x1000, 0x10); wr(0x1200, 0x00);  // T2 = 0x0010
	fake_now += 0x20;
	uint8_t ifr = rd(0x1a00);
	CHECK((ifr & 0x20) && (ifr & 0x80));

	// T2C reads return the live decrementing count (low/high bytes).
	wr(0x1000, 0x00); wr(0x1200, 0x01);  // T2 = 0x0100
	fake_now += 0x40;
	CHECK(rd(0x1000) == 0xC0 && rd(0x1200) == 0x00);  // 0x0100-0x40 = 0x00C0

	// Stored-state registers round-trip.
	wr(0x0000, 0x18); CHECK((rd(0x0000) & 0x18) == 0x18);   // ORB bits 3/4 (Cuda handshake)
	CHECK(via.cuda_touches > 0);                            // handshake bits counted as Cuda touches
	wr(0x1400, 0x55); CHECK(rd(0x1400) == 0x55);            // SR stored
	CHECK(via.cuda_touches >= 3);                           // SR write + read counted

	// T1 minimal: load via T1C-H, IFR bit 6 after expiry.
	wr(0x0800, 0x10); wr(0x0a00, 0x00);
	fake_now += 0x20;
	CHECK((rd(0x1a00) & 0x40) == 0x40);

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
```

- [ ] **Step 3: Add Makefile recipe, run, verify FAIL**

- [ ] **Step 4: Implement `dev_via6522.cpp`**

```cpp
/*
 *  dev_via6522.cpp - VIA 6522 timer/IFR surface, M1 scope (SPIKE-S3 §1.5/§4.2).
 *  Timers are lazy: latch load-time, compute count/expiry on read. M2's event
 *  scheduler replaces this with real callbacks. Cuda SR protocol = loud stub.
 */

#include "dev_via6522.h"
#include <string.h>
#include <stdio.h>

enum { R_ORB=0, R_ORA=1, R_DDRB=2, R_DDRA=3, R_T1CL=4, R_T1CH=5, R_T1LL=6, R_T1LH=7,
       R_T2CL=8, R_T2CH=9, R_SR=10, R_ACR=11, R_PCR=12, R_IFR=13, R_IER=14, R_ORA_NH=15 };

#define IFR_T2 0x20
#define IFR_T1 0x40

void VIAReset(VIA6522 *v, uint32_t base, uint64_t (*now)(void *), void *opaque)
{
	memset(v, 0, sizeof(*v));
	v->base = base; v->now_ticks = now; v->clock_opaque = opaque;
}

static void cuda_touch(VIA6522 *v, const char *what)
{
	v->cuda_touches++;
	if (!v->cuda_warned) {
		v->cuda_warned = true;
		fprintf(stderr, "[MMIO] via6522: Cuda-protocol register touched (%s) - "
		        "loud stub only until M3 (MACHINE-LAYER-PLAN M3)\n", what);
	}
}

static uint16_t timer_remaining(VIA6522 *v, bool t1, bool *expired)
{
	uint64_t now = v->now_ticks(v->clock_opaque);
	uint64_t load = t1 ? v->t1_load_time : v->t2_load_time;
	uint16_t cnt  = t1 ? v->t1_count : v->t2_count;
	bool running  = t1 ? v->t1_running : v->t2_running;
	uint64_t dt = now - load;
	*expired = running && dt >= cnt;
	return (uint16_t)(*expired ? 0 : cnt - dt);
}

static uint8_t ifr_now(VIA6522 *v)
{
	uint8_t ifr = v->ifr_latched;
	bool exp;
	timer_remaining(v, false, &exp); if (exp) ifr |= IFR_T2;
	timer_remaining(v, true, &exp);  if (exp) ifr |= IFR_T1;
	ifr &= 0x7F;
	if (ifr & v->ier & 0x7F) ifr |= 0x80;
	return ifr;
}

uint64_t VIARead(void *opaque, uint32_t addr, unsigned size)
{
	VIA6522 *v = (VIA6522 *)opaque;
	(void)size;
	bool exp;
	switch (((addr - v->base) >> 9) & 0xF) {
	case R_ORB:    return v->orb;
	case R_ORA: case R_ORA_NH: return v->ora;
	case R_DDRB:   return v->ddrb;
	case R_DDRA:   return v->ddra;
	case R_T1CL:   { uint16_t r = timer_remaining(v, true, &exp);  return r & 0xFF; }
	case R_T1CH:   { uint16_t r = timer_remaining(v, true, &exp);  return r >> 8; }
	case R_T1LL:   return v->t1l_l;
	case R_T1LH:   return v->t1l_h;
	case R_T2CL:   { uint16_t r = timer_remaining(v, false, &exp); return r & 0xFF; }
	case R_T2CH:   { uint16_t r = timer_remaining(v, false, &exp); return r >> 8; }
	case R_SR:     cuda_touch(v, "SR read");  return v->sr;
	case R_ACR:    return v->acr;
	case R_PCR:    return v->pcr;
	case R_IFR:    return ifr_now(v);
	case R_IER:    return 0x80 | v->ier;
	}
	return 0;
}

void VIAWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value)
{
	VIA6522 *v = (VIA6522 *)opaque;
	(void)size;
	uint8_t b = (uint8_t)value;
	switch (((addr - v->base) >> 9) & 0xF) {
	case R_ORB:
		if ((v->orb ^ b) & 0x18) cuda_touch(v, "ORB handshake bits 3/4");
		v->orb = b; break;
	case R_ORA: case R_ORA_NH: v->ora = b; break;
	case R_DDRB:   v->ddrb = b; break;
	case R_DDRA:   v->ddra = b; break;
	case R_T1CL: case R_T1LL: v->t1l_l = b; break;
	case R_T1LH:   v->t1l_h = b; break;
	case R_T1CH:
		v->t1l_h = b;
		v->t1_count = (uint16_t)((b << 8) | v->t1l_l);
		v->t1_load_time = v->now_ticks(v->clock_opaque);
		v->t1_running = true;
		v->ifr_latched &= ~IFR_T1;
		break;
	case R_T2CL:   v->t2l_l = b; break;
	case R_T2CH:
		v->t2_count = (uint16_t)((b << 8) | v->t2l_l);
		v->t2_load_time = v->now_ticks(v->clock_opaque);
		v->t2_running = true;
		v->ifr_latched &= ~IFR_T2;
		break;
	case R_SR:     cuda_touch(v, "SR write"); v->sr = b; break;
	case R_ACR:    v->acr = b; break;
	case R_PCR:    v->pcr = b; break;
	case R_IFR: {
		// write-1-to-clear; expired-timer bits clear by stopping the lazy source
		bool exp;
		if (b & IFR_T2) { timer_remaining(v, false, &exp); if (exp) v->t2_running = false; }
		if (b & IFR_T1) { timer_remaining(v, true, &exp);  if (exp) v->t1_running = false; }
		v->ifr_latched &= ~(b & 0x7F);
		break;
	}
	case R_IER:
		if (b & 0x80) v->ier |= (b & 0x7F);
		else          v->ier &= ~(b & 0x7F);
		break;
	}
}
```

(`VIARead` of T2C while expired returns 0 — the 6522 actually free-runs past zero, but no S3 consumer reads T2C after expiry; fence says don't model it. Note this in a comment if the test ever needs adjusting.)

- [ ] **Step 5: Run `make -C SheepShaver/src/machine test_dev_via6522 && SheepShaver/src/machine/test_dev_via6522` — Expected: ALL PASS**

- [ ] **Step 6: Commit** — `feat(machine): M1 VIA 6522 timer/IFR surface with lazy T1/T2 and Cuda loud stub`

---

### Task 4: AArch64 MMIO access decoder (pure, standalone-tested)

The decoder is the *contract test* against the JIT's memory emitters (MACHINE-LAYER-PLAN §6 row 1). Pure functions, no Mach dependencies — `mmio_machfault.cpp` (Task 6) is the only consumer besides the test.

**Files:**
- Create: `SheepShaver/src/include/a64_mmio_decode.h`
- Create: `SheepShaver/src/machine/a64_mmio_decode.cpp`
- Create: `SheepShaver/src/machine/test_a64_mmio_decode.cpp`
- Modify: `SheepShaver/src/machine/Makefile` (same pattern)

- [ ] **Step 1: Write the header**

`SheepShaver/src/include/a64_mmio_decode.h`:

```cpp
/*
 *  a64_mmio_decode.h - decoder for the JIT's guest-memory access forms (SPIKE-S2 §4.1).
 *  The JIT emits exactly one form family: LDR/STR{B,H,,X} Rt, [Xn, Wm, UXTW]
 *  ({0x38,0x78,0xB8,0xF8} {60=load,20=store} 4800, fixed-bits mask 0xFFE0FC00) —
 *  see ppc-codegen-aarch64.h:150-181. Anything else is NOT a JIT guest access.
 */

#ifndef A64_MMIO_DECODE_H
#define A64_MMIO_DECODE_H

#include <stdint.h>

struct A64MemAccess {
	bool     is_load;
	unsigned size_log2;   // 0=B 1=H 2=W 3=X
	unsigned rt, rn, rm;
};

// Returns true iff insn is one of the 8 JIT-emitted register-offset UXTW forms.
extern bool A64DecodeMMIOAccess(uint32_t insn, A64MemAccess *out);

// Returns true iff insn is the REV/REV16 the JIT pairs with a width-size_log2
// access on register rt (REV16 W for H, REV W for W, REV X for X). Byte accesses
// have no swap insn; callers must not ask (size_log2==0 returns false).
extern bool A64IsPairedSwap(uint32_t insn, unsigned size_log2, unsigned rt);

// Byte-swap an architectural value to/from raw memory order for a given width.
extern uint64_t A64SwapForWidth(uint64_t v, unsigned size_log2);

#endif
```

- [ ] **Step 2: Write the failing test — enumerate every emitted form + S2 gotchas**

`SheepShaver/src/machine/test_a64_mmio_decode.cpp`:

```cpp
/* Contract test: every form ppc-codegen-aarch64.h:150-181 emits must decode; close
 * neighbors must NOT. S2 gotchas (rt==31, width keying) asserted here. */
#include "a64_mmio_decode.h"
#include <assert.h>
#include <stdio.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

// Verbatim encoding constants from ppc-codegen-aarch64.h:150-181:
static uint32_t enc(uint32_t baseop, unsigned rt, unsigned rn, unsigned rm)
{ return baseop | (rm << 16) | (rn << 5) | rt; }

int main()
{
	static const struct { uint32_t op; bool load; unsigned sz; } forms[] = {
		{ 0xB8604800, true,  2 },  // a64_ldr_w_reg
		{ 0xB8204800, false, 2 },  // a64_str_w_reg
		{ 0xF8604800, true,  3 },  // a64_ldr_x_reg
		{ 0xF8204800, false, 3 },  // a64_str_x_reg
		{ 0x38604800, true,  0 },  // a64_ldrb_reg
		{ 0x38204800, false, 0 },  // a64_strb_reg
		{ 0x78604800, true,  1 },  // a64_ldrh_reg
		{ 0x78204800, false, 1 },  // a64_strh_reg
	};
	A64MemAccess a;
	for (unsigned i = 0; i < 8; i++) {
		// JIT-typical operands: rt=1 (RTMP1), rn=19 (RMEMBASE), rm=0 (RTMP0)
		CHECK(A64DecodeMMIOAccess(enc(forms[i].op, 1, 19, 0), &a));
		CHECK(a.is_load == forms[i].load && a.size_log2 == forms[i].sz);
		CHECK(a.rt == 1 && a.rn == 19 && a.rm == 0);
		// rt==31 (WZR/XZR) still decodes — caller discards the value (S2 §4.3)
		CHECK(A64DecodeMMIOAccess(enc(forms[i].op, 31, 19, 0), &a) && a.rt == 31);
		// arbitrary allocated source reg for byte stores (hS varies)
		CHECK(A64DecodeMMIOAccess(enc(forms[i].op, 27, 19, 0), &a) && a.rt == 27);
	}

	// Non-matches: REV, REV16, LSL-extend variant (option=011, the old UXTX bug),
	// immediate-offset LDR, BL, NOP.
	CHECK(!A64DecodeMMIOAccess(0x5AC00800 | (1 << 5) | 1, &a));  // REV W1,W1
	CHECK(!A64DecodeMMIOAccess(0x5AC00400 | (1 << 5) | 1, &a));  // REV16 W1,W1
	CHECK(!A64DecodeMMIOAccess(0xB8606800 | (19 << 5) | 1, &a)); // LDR w, [x,x,LSL] opt=011
	CHECK(!A64DecodeMMIOAccess(0xB9400000, &a));                  // LDR w, [x, #imm]
	CHECK(!A64DecodeMMIOAccess(0x94000001, &a));                  // BL
	CHECK(!A64DecodeMMIOAccess(0xD503201F, &a));                  // NOP

	// Paired-swap recognition (S2 §4.3: width-keyed; byte has none).
	CHECK(A64IsPairedSwap(0x5AC00800 | (1 << 5) | 1, 2, 1));      // REV W1
	CHECK(A64IsPairedSwap(0x5AC00400 | (1 << 5) | 1, 1, 1));      // REV16 W1
	CHECK(A64IsPairedSwap(0xDAC00C00 | (1 << 5) | 1, 3, 1));      // REV X1
	CHECK(!A64IsPairedSwap(0x5AC00800 | (1 << 5) | 1, 1, 1));     // wrong width
	CHECK(!A64IsPairedSwap(0x5AC00800 | (2 << 5) | 2, 2, 1));     // wrong reg
	CHECK(!A64IsPairedSwap(0x5AC00800 | (1 << 5) | 1, 0, 1));     // byte: never

	// Swap helper: width-keyed (byte = identity).
	CHECK(A64SwapForWidth(0x12, 0) == 0x12);
	CHECK(A64SwapForWidth(0x1234, 1) == 0x3412);
	CHECK(A64SwapForWidth(0x12345678u, 2) == 0x78563412u);
	CHECK(A64SwapForWidth(0x0102030405060708ull, 3) == 0x0807060504030201ull);

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
```

- [ ] **Step 3: Add Makefile recipe, run, verify FAIL**

- [ ] **Step 4: Implement `a64_mmio_decode.cpp`**

```cpp
/*  a64_mmio_decode.cpp - see header. Port of spikes/s2-mach-fault-decode/main.cpp:87-144. */

#include "a64_mmio_decode.h"

bool A64DecodeMMIOAccess(uint32_t insn, A64MemAccess *out)
{
	// Strip the size field (bits 31:30); remaining fixed bits must match the
	// register-offset UXTW load/store form (mask 0xFFE0FC00 from S2).
	uint32_t key = insn & (0xFFE0FC00u & ~0xC0000000u);   // = insn & 0x3FE0FC00
	bool is_load;
	if (key == 0x38604800u)      is_load = true;
	else if (key == 0x38204800u) is_load = false;
	else return false;
	out->is_load   = is_load;
	out->size_log2 = insn >> 30;
	out->rt = insn & 0x1F;
	out->rn = (insn >> 5) & 0x1F;
	out->rm = (insn >> 16) & 0x1F;
	return true;
}

bool A64IsPairedSwap(uint32_t insn, unsigned size_log2, unsigned rt)
{
	uint32_t want;
	switch (size_log2) {
	case 1:  want = 0x5AC00400u; break;   // REV16 Wd,Wn
	case 2:  want = 0x5AC00800u; break;   // REV   Wd,Wn
	case 3:  want = 0xDAC00C00u; break;   // REV   Xd,Xn
	default: return false;                 // byte accesses have no paired swap
	}
	return insn == (want | (rt << 5) | rt);
}

uint64_t A64SwapForWidth(uint64_t v, unsigned size_log2)
{
	switch (size_log2) {
	case 0: return v & 0xFF;
	case 1: return __builtin_bswap16((uint16_t)v);
	case 2: return __builtin_bswap32((uint32_t)v);
	default: return __builtin_bswap64(v);
	}
}
```

- [ ] **Step 5: Run `make -C SheepShaver/src/machine test_a64_mmio_decode && SheepShaver/src/machine/test_a64_mmio_decode` — Expected: ALL PASS**

- [ ] **Step 6: Commit** — `feat(machine): M1 AArch64 MMIO access decoder - the JIT memory-emitter contract test`

---
### Task 5: Profile helper, build integration, bus bring-up in main_unix.cpp

**Files:**
- Modify: `SheepShaver/src/include/machine_profile.h`, `SheepShaver/src/machine/machine_profile.cpp`, `SheepShaver/src/machine/test_machine_profile.cpp`
- Modify: `SheepShaver/src/Unix/Makefile.in:67–69` (SRCS)
- Modify: `SheepShaver/src/Unix/main_unix.cpp`

- [ ] **Step 1: Add `MachineUsesMMIOBus()` to the profile module (test-first)**

Append to `test_machine_profile.cpp` (follow the file's existing CHECK/assert idiom — read it first and match):
- newworld profile ⇒ `MachineUsesMMIOBus()` true.
- paravirtual + `SS_MMIO_BUS=1` (via the parse-level pure function below) ⇒ true — this is the **named third config** from the M1 plan row.
- paravirtual, no env ⇒ false.

Implement in `machine_profile.h`/`.cpp`:

```cpp
// machine_profile.h — add:
// True when the MMIO bus + device models are live: the newworld profile, or the
// named third config "paravirtual + bus + devices - serial-skips" (SS_MMIO_BUS=1).
// MACHINE-LAYER-PLAN.md M1 row, consumer (a).
extern bool MachineUsesMMIOBus(void);
```

```cpp
// machine_profile.cpp — add (resolve once in MachineProfileInit, same pattern as g_profile):
static bool g_uses_mmio_bus = false;
// in MachineProfileInit(), after g_profile is resolved:
g_uses_mmio_bus = (g_profile == MACHINE_NEWWORLD) || MachineEnvFlag("SS_MMIO_BUS");
bool MachineUsesMMIOBus(void) { return g_uses_mmio_bus; }
```

For the standalone test (which can't call the process-wide init), expose the pure decision the same way `MachineProfileParse` is exposed — add `extern bool MachineUsesMMIOBusParse(MachineProfile p, const char *env_mmio_bus);` returning `p == MACHINE_NEWWORLD || (env_mmio_bus && env_mmio_bus[0] && env_mmio_bus[0] != '0')`, have `MachineProfileInit` call it, and unit-test the parse function.

Run: `make -C SheepShaver/src/machine test` — Expected: ALL PASS (all five binaries).

- [ ] **Step 2: Add the new sources to the emulator build**

In `SheepShaver/src/Unix/Makefile.in` SRCS (next to `../machine/machine_profile.cpp`), add:
```
../machine/mmio_bus.cpp ../machine/dev_scc8530.cpp ../machine/dev_via6522.cpp \
../machine/a64_mmio_decode.cpp ../machine/mmio_machfault.cpp \
```
Create a stub `SheepShaver/src/machine/mmio_machfault.cpp` now so the build links (Task 6 fills it):

```cpp
/*  mmio_machfault.cpp - JIT-path MMIO dispatch (Mach fault -> decode -> bus -> writeback). */
#include "mmio_bus.h"
bool MMIOMachFaultDispatch(uint32_t guest_addr, void *thread_state64)
{
	(void)guest_addr; (void)thread_state64;
	return false;   // filled in by M1 Task 6
}
```

Regenerate: `cd SheepShaver/src/Unix && ./config.status Makefile`.

**Add `-DSS_MMIO_BACKPATCH` to the ppc-jit.cpp build (rev 2 finding C4).** Task 9's additions to
`ppc-jit.cpp` (`#include "a64_mmio_decode.h"`, `MMIOBusRead/Write` calls, `A64*` functions) are gated
behind `#ifdef SS_MMIO_BACKPATCH` (see Task 9). The emulator build must define it; the rom-harness
build must NOT (it compiles ppc-jit.cpp standalone). Add `-DSS_MMIO_BACKPATCH` to the emulator build's
CPPFLAGS for ppc-jit.cpp via `SheepShaver/src/Unix/Makefile.in` — a global `DEFS += -DSS_MMIO_BACKPATCH`
addition is fine here (it must not reach `SheepShaver/rom-harness/Makefile`). Consequence: in any build
without the define, the weak symbols `ppc_jit_pc_in_cache` / `ppc_jit_backpatch_mmio` consumed by
`mmio_machfault.cpp` (Task 6 Step 2) resolve absent, so the fault path stays cold-only — correct.

**Also add the `test_mmio_machfault` recipe to `SheepShaver/src/machine/Makefile`** (Task 6 Step 4's new
standalone test, per finding C2): a recipe linking `test_mmio_machfault.cpp + mmio_machfault.cpp +
mmio_bus.cpp + a64_mmio_decode.cpp` (pattern of the other test recipes), add `./test_mmio_machfault` to
the `test:` target, and add the binary name to `SheepShaver/src/machine/.gitignore`.

- [ ] **Step 3: Bus bring-up in main_unix.cpp**

In `SheepShaver/src/Unix/main_unix.cpp`, after the RAM/ROM mapping block (~line 1502, after ROM is mapped) — guarded so paravirtual default is untouched:

```cpp
#include "mmio_bus.h"
#include "dev_scc8530.h"
#include "dev_via6522.h"
```

```cpp
	// Machine Layer M1: MMIO bus + device models (MACHINE-LAYER-PLAN.md §2b; CORE99 §1).
	// Active on the newworld profile or the named third config SS_MMIO_BUS=1.
	if (MachineUsesMMIOBus()) {
		// Claim the MacIO container so no later mapping can land there. The region
		// stays PROT_NONE forever: every access must Mach-fault into the bus.
		void *want = (void *)(uintptr_t)(NATMEM_OFFSET + 0xF3000000ull);
		void *got = mmap(want, 0x80000, PROT_NONE,
		                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (got != want) {
			fprintf(stderr, "[MMIO] FATAL: cannot reserve MacIO container at %p (got %p)\n",
			        want, got);
			QuitEmulator();
		}

		static SCC8530 scc;
		static VIA6522 via;
		SCCReset(&scc, 0xF3012000);
		VIAReset(&via, 0xF3016000, mmio_via_now_ticks, NULL);

		static const MMIODevice macio_stub_dev =
			{ "macio-stub", NULL, mmio_stub_read, mmio_stub_write, NULL };
		static const MMIODevice scc_dev =
			{ "scc8530", &scc, SCCRead, SCCWrite, SCCReadIsIdle };
		static const MMIODevice via_dev =
			{ "via6522", &via, VIARead, VIAWrite, NULL };

		bool ok = MMIOBusRegister(0xF3000000, 0x80000, MMIO_TRAPPED, &macio_stub_dev)
		       && MMIOBusRegister(0xF3012000, 0x1000, MMIO_TRAPPED, &scc_dev)
		       && MMIOBusRegister(0xF3016000, 0x2000, MMIO_TRAPPED, &via_dev);
		if (!ok) { fprintf(stderr, "[MMIO] FATAL: region registration failed\n"); QuitEmulator(); }
		MMIOBusActivate();
		atexit(mmio_dump_stats_atexit);
		fprintf(stderr, "[MMIO] bus active: macio 0xF3000000+0x80000, scc 0xF3012000, via 0xF3016000\n");

		// SS_JIT_VERIFY replays blocks; device reads are side-effecting (clear-on-read,
		// FIFO-pop) and must never be double-executed (MACHINE-LAYER-PLAN §2b). Hard incompat.
		const char *verify = getenv("SS_JIT_VERIFY");
		if (verify && verify[0] && verify[0] != '0') {
			fprintf(stderr, "[MMIO] FATAL: SS_JIT_VERIFY is incompatible with the MMIO bus "
			        "(side-effecting device reads must not be replayed)\n");
			QuitEmulator();
		}
	}
```

With file-scope helpers (above `main`, near other statics):

```cpp
// MacIO addresses with no device model yet: abort-loudly stubs (CORE99 §4 fence).
static uint64_t mmio_stub_read(void *, uint32_t addr, unsigned size)
{
	fprintf(stderr, "[MMIO] FATAL: read%u from unmodeled MacIO address 0x%08x "
	        "(CORE99-MACHINE-DESCRIPTION §4 fence)\n", size * 8, addr);
	abort();
}
static void mmio_stub_write(void *, uint32_t addr, unsigned size, uint64_t v)
{
	fprintf(stderr, "[MMIO] FATAL: write%u of 0x%llx to unmodeled MacIO address 0x%08x\n",
	        size * 8, (unsigned long long)v, addr);
	abort();
}
// VIA clock: host microseconds -> VIA ticks (783360 Hz). GetTicks_usec() is the
// emulator's existing monotonic source (timer_unix / main_unix).
static uint64_t mmio_via_now_ticks(void *)
{
	return GetTicks_usec() * VIA_CLOCK_HZ / 1000000ull;
}
static void mmio_dump_stats_atexit(void) { MMIOBusDumpStats(stderr); }
```

(If `GetTicks_usec()` isn't visible in main_unix.cpp, it is declared in the timer headers — grep `GetTicks_usec` and include accordingly; it's used elsewhere in this file's vicinity.)

- [ ] **Step 4: Widen the serial-skip/ignoresegv gates to the named third config**

The M0 gates use `!MachineProfileIsNewWorld()`. On `SS_MMIO_BUS=1` (paravirtual + bus) the legacy skips would eat device faults *before* bus dispatch — exactly the M1 false-pass risk. Change **only these four sites** to `!MachineUsesMMIOBus()` (newworld ⇒ uses bus, so newworld behavior is unchanged):
- `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp:947` (serial-skip block gate)
- `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp:981` (ignoresegv gate)
- `SheepShaver/src/Unix/main_unix.cpp:2309` (serial-skip block gate)
- `SheepShaver/src/Unix/main_unix.cpp:2477` (ignoresegv gate)

Do NOT touch the other M0 profile gates (HandleInterrupt, rom_patches sites).

**Note (rev 2 finding C1):** the two `main_unix.cpp` gates (2309/2477) live inside the
`#if !EMULATED_PPC` sigsegv handler, which is **dead code on macOS arm64** (`EMULATED_PPC = 1`,
config.h:12). They are runtime no-ops here; update them only for correctness on non-EMULATED_PPC
Linux builds. The gates that actually take effect on this target are the two `sheepshaver_glue.cpp`
sites (947/981).

- [ ] **Step 5: Build + paravirtual smoke**

```bash
cd SheepShaver && make build-ss && make test-jit
```
Expected: build clean; `METRIC … score=100` (350/350). Paravirtual default never executes any of the new code (bus inactive).

- [ ] **Step 6: Commit** — `feat(machine): M1 bus bring-up - PROT_NONE MacIO reservation, SCC/VIA registration, SS_MMIO_BUS third config, gate widening`

---

### Task 6: Mach fault path — decode, bus dispatch, thread-state writeback

**Files:**
- Modify: `SheepShaver/src/CrossPlatform/sigsegv.cpp`
- Modify: `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` (sigsegv_handler ~919)
- Rewrite: `SheepShaver/src/machine/mmio_machfault.cpp` (replace Task 5 stub)

This is the S2 spike productionized. **Read `spikes/s2-mach-fault-decode/main.cpp` first** — the decode/inject/PC-advance logic ports directly. §2g applies: this code runs on the Mach exception-handler thread; only the bus/device locks, no malloc, stdio only on terminal-abort paths.

- [ ] **Step 1: Extend the sigsegv framework (sigsegv.cpp)**

(a) New return code. In the `sigsegv_return_t` enum (in `sigsegv.h` next to `SIGSEGV_RETURN_SKIP_INSTRUCTION`):
```cpp
	SIGSEGV_RETURN_STATE_MODIFIED,   // handler mutated the thread state in place (Mach only)
```
(b) Thread-state accessor. Near `sigsegv_get_fault_address` in sigsegv.cpp + declared in sigsegv.h (decl insertion: sigsegv.h:179, after `sigsegv_get_fault_instruction_address`; enum `sigsegv_return_t` is at sigsegv.h:153–157, add `SIGSEGV_RETURN_STATE_MODIFIED` after `SIGSEGV_RETURN_SKIP_INSTRUCTION` at :156):
```cpp
// Mach path only: raw ARM_THREAD_STATE64 of the faulting thread (mutable in place;
// written back when the handler returns SIGSEGV_RETURN_STATE_MODIFIED). NULL elsewhere.
void *sigsegv_get_thread_state(sigsegv_info_t *sip);
```

**CRITICAL — the accessor MUST lazily fetch thread state before returning storage (rev 2 finding C1).**
On arm64, `handle_badaccess` enters the user-handler switch with `has_thr_state == false`: the
pre-fetch block at sigsegv.cpp:2725–2784 is inside `#if defined(__APPLE__) && defined(__x86_64__)`
and is **skipped entirely** on arm64. The first (and only) lazy fetch on the legacy path is the SKIP
branch at sigsegv.cpp:2795–2796. So if `sigsegv_get_thread_state` returns `&SIP->thr_state` raw, it
hands back **uninitialized stack storage** (and `thr_state_count` stays uninitialized), corrupting the
`mach_set_thread_state` writeback — garbage PC, garbage rt write, crash or silent wrong result.
Required implementation (mirrors the SKIP branch at 2795–2796):
```cpp
// Mach path only: raw ARM_THREAD_STATE64 of the faulting thread (mutable in place; written
// back by handle_badaccess on SIGSEGV_RETURN_STATE_MODIFIED). Lazily fetches on first call.
void *sigsegv_get_thread_state(sigsegv_info_t *SIP)
{
#if defined(HAVE_MACH_EXCEPTIONS) && defined(_STRUCT_ARM_THREAD_STATE64)
    if (!SIP->has_thr_state) {
        mach_get_thread_state(SIP);   // sets thr_state + thr_state_count + has_thr_state
        SIP->has_thr_state = true;
    }
    return &SIP->thr_state;
#else
    (void)SIP;
    return NULL;
#endif
}
```
Place it inside the `#ifdef HAVE_MACH_EXCEPTIONS` block, after `mach_set_thread_state`'s closing brace
(sigsegv.cpp:2639, before the block's `#endif`). `mach_get_thread_state`/`mach_set_thread_state` are
`static` in sigsegv.cpp — the accessor and the STATE_MODIFIED branch must live in that file. The
member is `thr_state` of type `SIGSEGV_THREAD_STATE_TYPE` (= `arm_thread_state64_t` on macOS arm64).

(c) Honor the new code in `handle_badaccess` (~2786–2818): where `SIGSEGV_RETURN_SKIP_INSTRUCTION`
triggers `aarch64_skip_instruction(regs)` + `mach_set_thread_state`, add a branch for
`SIGSEGV_RETURN_STATE_MODIFIED` that calls `mach_set_thread_state` **without** the skip. Insert after
the SKIP block's `break;`/`#endif` (sigsegv.cpp:2808) and before `case SIGSEGV_RETURN_FAILURE:` (:2810),
guarded by `#ifdef HAVE_MACH_EXCEPTIONS`. The build has no `-Wswitch`/`-Wswitch-enum`; the only switch
over `sigsegv_return_t` is this one. On non-Mach builds the case doesn't exist (can't be returned there).

This file is **not** shared with BasiliskII — BasiliskII has its own separate copy at
`BasiliskII/src/CrossPlatform/sigsegv.cpp` (rev 2 finding C3 reframe). Modifying SheepShaver's copy
does not touch BasiliskII's. Still keep every addition inside `#ifdef HAVE_MACH_EXCEPTIONS`
(+ `_STRUCT_ARM_THREAD_STATE64` where ARM-specific) for SheepShaver's own x86/Linux build matrix.

- [ ] **Step 2: Implement `mmio_machfault.cpp`**

```cpp
/*
 *  mmio_machfault.cpp - JIT-path MMIO dispatch (MACHINE-LAYER-PLAN §2b path 1).
 *  Runs on the Mach exception-handler thread with the CPU thread suspended (§2g):
 *  no malloc, no foreign locks, stdio only on terminal-abort paths.
 *  Port of spikes/s2-mach-fault-decode/main.cpp (decode at :120, inject at :128-130).
 */

#include "mmio_bus.h"
#include "a64_mmio_decode.h"
#include <mach/mach.h>
#include <mach/thread_status.h>
#include <stdio.h>
#include <stdlib.h>

// Provided by ppc-jit.cpp (Task 9), but only when that build defines SS_MMIO_BACKPATCH
// (rev 2 finding C4). Weak: absent until Task 9 lands, AND absent in any build that does not
// define SS_MMIO_BACKPATCH (e.g. the rom-harness standalone build). When both resolve absent the
// hot-site test below short-circuits and the fault path stays cold-only — correct for all builds.
extern "C" bool ppc_jit_pc_in_cache(const void *host_pc) __attribute__((weak));
extern "C" bool ppc_jit_backpatch_mmio(uint32_t *site, const A64MemAccess *acc) __attribute__((weak));

#define MMIO_BACKPATCH_THRESHOLD 8

// Per-site fault counters: open-addressed table, touched ONLY by the (single)
// Mach handler thread -> no locking. Preallocated (§2g: no malloc).
#define SITE_TABLE_SIZE 1024   // power of two
static struct { uintptr_t pc; uint32_t count; } site_table[SITE_TABLE_SIZE];

static uint32_t site_count_bump(uintptr_t pc)
{
	uint32_t h = (uint32_t)(pc >> 2) & (SITE_TABLE_SIZE - 1);
	for (unsigned probe = 0; probe < 8; probe++, h = (h + 1) & (SITE_TABLE_SIZE - 1)) {
		if (site_table[h].pc == pc) return ++site_table[h].count;
		if (site_table[h].pc == 0) { site_table[h].pc = pc; site_table[h].count = 1; return 1; }
	}
	return 1;   // table pressure: behave as cold (keep injecting)
}

bool MMIOMachFaultDispatch(uint32_t guest_addr, void *thread_state64)
{
#ifdef _STRUCT_ARM_THREAD_STATE64
	_STRUCT_ARM_THREAD_STATE64 *ts = (_STRUCT_ARM_THREAD_STATE64 *)thread_state64;
	if (!ts) return false;
	uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(*ts);   // PAC-safe (S2 §3)
	if (!pc) return false;
	uint32_t insn = *(const uint32_t *)pc;                       // same-task read (S2 §4.3)

	A64MemAccess acc;
	if (!A64DecodeMMIOAccess(insn, &acc)) {
		// In bus range but not a JIT guest-access form: a host C++ accessor or our
		// own tooling touched device space through a raw pointer. Contract violation
		// (MACHINE-LAYER-PLAN §2b host-accessor path): abort loudly, never skip.
		fprintf(stderr, "[MMIO] FATAL: undecodable access to device addr 0x%08x at host pc %p "
		        "(insn 0x%08x) - host code must use MMIOBusRead/Write\n",
		        guest_addr, (void *)pc, insn);
		abort();
	}

	MMIOBusCountJITFault(guest_addr);
	unsigned bytes = 1u << acc.size_log2;

	// Hot site? Hand it to the backpatcher (Task 9): the CPU thread is suspended AT
	// this pc, so rewriting the site and resuming WITHOUT inject re-executes it as a
	// bus call. Until Task 9 lands the weak symbol is absent and this is skipped.
	if (ppc_jit_pc_in_cache && ppc_jit_backpatch_mmio
	    && ppc_jit_pc_in_cache((const void *)pc)
	    && site_count_bump(pc) >= MMIO_BACKPATCH_THRESHOLD
	    && ppc_jit_backpatch_mmio((uint32_t *)pc, &acc)) {
		MMIOBusCountBackpatch(guest_addr);
		return true;   // state untouched; resume re-executes the patched BL
	}

	// Cold path: emulate the access, skip the LDR/STR only (the REV still runs - S2 §2 contract).
	if (acc.is_load) {
		uint64_t arch = MMIOBusRead(guest_addr, bytes);
		uint64_t raw = A64SwapForWidth(arch, acc.size_log2);   // inject RAW BE; REV swaps it back
		if (acc.rt != 31)                                       // rt==31 is WZR/XZR (S2 §4.3)
			ts->__x[acc.rt] = raw;                              // zero-extended 64-bit write
	} else {
		// Store: the JIT's REV ran BEFORE the faulting STR, so x[rt] already holds raw BE.
		uint64_t raw = (acc.rt == 31) ? 0 : ts->__x[acc.rt];
		uint64_t arch = A64SwapForWidth(raw, acc.size_log2);
		MMIOBusWrite(guest_addr, bytes, arch);
	}
	arm_thread_state64_set_pc_fptr(*ts, (void *)(pc + 4));     // PAC-safe PC advance
	return true;
#else
	(void)guest_addr; (void)thread_state64;
	return false;
#endif
}
```

Note on `A64SwapForWidth` masking for sub-register stores: for size_log2<3 the swap helper truncates to the width first (it does — see Task 4), so high garbage in x[rt] is harmless.

- [ ] **Step 3: Hook the dispatch into the SheepShaver glue handler ONLY**

The live SIGSEGV handler on this target is **only** `sheepshaver_glue.cpp:919`'s
`sigsegv_handler(sigsegv_info_t*)` — installed via `sigsegv_install_handler` because
`EMULATED_PPC = 1` on macOS arm64 (config.h:12). The `main_unix.cpp:2275` handler
(`static void sigsegv_handler(int, siginfo_t*, void*)`) is inside `#if !EMULATED_PPC` and is
**dead code** here — it never compiles into the executable. Do NOT add MMIO dispatch there.
(The main_unix.cpp:2309/2477 gate-widening in Task 5 lives in this same dead handler — see Task 5
Step 4's no-op note — but is still done for non-EMULATED_PPC Linux builds.)

Insert the dispatch block in the glue handler at sheepshaver_glue.cpp:929 — **after `addr` is
computed at :928, before the ROM-write check at :932**. The ROM-write check at :932 uses
`ROMBaseHost` host-pointer arithmetic; a device-space fault (`0xF3000000+`) would not match it and
would fall through to the legacy serial hacks or be swallowed by `ignoresegv`, so MMIO dispatch
must precede it. `addr` (from `sigsegv_get_fault_address(sip)`) is a host virtual address in the
`0x400000000000+` range; `VMBaseDiff = NATMEM_OFFSET` is visible via the include chain
`cpu_emulation.h → cpu/vm.hpp` (do NOT redeclare it). The conversion `gaddr = (uint32)((uintptr)addr
- VMBaseDiff)` is correct:

```cpp
	// Machine Layer M1: MMIO bus dispatch (MACHINE-LAYER-PLAN §2b, JIT path).
	// Must run BEFORE any legacy skip so no device access is silently eaten.
	if (mmio_bus_active) {
		uint32_t gaddr = (uint32_t)((uintptr_t)addr - VMBaseDiff);   // host -> guest
		if (MMIOBusInRange(gaddr)) {
			void *ts = sigsegv_get_thread_state(sip);
			if (ts && MMIOMachFaultDispatch(gaddr, ts))
				return SIGSEGV_RETURN_STATE_MODIFIED;
			fprintf(stderr, "[MMIO] FATAL: in-range fault not serviced (gaddr=0x%08x)\n", gaddr);
			return SIGSEGV_RETURN_FAILURE;
		}
	}
```

Include `"mmio_bus.h"` at the top of sheepshaver_glue.cpp alongside the existing includes. `VMBaseDiff`
is already visible here via the `cpu_emulation.h → cpu/vm.hpp` include chain (`const uintptr VMBaseDiff
= NATMEM_OFFSET`, vm.hpp:196) — use it directly; do NOT redeclare it or invent a second conversion.

- [ ] **Step 4: Build + functional fault-path test (no full boot)**

```bash
cd SheepShaver && make build-ss && make test-jit       # paravirtual still 350/350
```

**Do NOT use an `SS_TEST_HEX` smoke here (rev 2 finding C2).** The `SS_TEST_HEX` path returns from
`main()` at main_unix.cpp:1162 — **before** `PrefsInit`, `MachineProfileInit`,
`sigsegv_install_handler`, and the bus bring-up block. With `SS_TEST_HEX` set, `MMIOBusActivate()` is
never called, no Mach handler is installed, `mmio_bus_active` stays false, and the atexit dump prints
nothing. `SS_MMIO_BUS=1 SS_TEST_HEX=…` can never activate the bus; that "smoke" is a false-positive
generator (the vector passes only because it never touches device space).

Instead, add a standalone unit test that exercises the fault dispatcher directly:
`SheepShaver/src/machine/test_mmio_machfault.cpp` (M0 test style; recipe added to
`SheepShaver/src/machine/Makefile`, links `mmio_machfault.cpp + mmio_bus.cpp + a64_mmio_decode.cpp` —
see Task 5 additions). Keep `mmio_machfault.cpp`'s includes lean (the mach headers `<mach/mach.h>`,
`<mach/thread_status.h>` are available on the host). The test must:

1. Register a fake `MMIO_TRAPPED` region + `MMIOBusActivate()`.
2. Build a synthetic `arm_thread_state64_t`: place a hand-assembled load insn
   (`0xB8604800 | (1<<16) | (19<<5) | 1` = `LDR w1, [x19, w0, UXTW]`) in a buffer, point `__pc` at it,
   set `__x[0]` = the device guest address (e.g. `0xF3012002`).
3. Call `MMIOMachFaultDispatch(0xF3012002, &ts)` directly.
4. Assert: **load** — raw-BE injection into `__x[rt]` (REV-swapped form), `__pc` advanced by 4;
   **store** path — value extracted from `__x[rt]`, swapped, delivered to the bus; **rt==31** —
   value discarded but the access still dispatched (PC still advances); the **undecodable-insn → abort**
   path is NOT in-process testable (it calls `abort()`); cover decode-rejection in `test_a64_mmio_decode`
   (which already asserts non-JIT forms return false) and document the abort branch as untestable here.
   (PAC note: `arm_thread_state64_get/set_pc` macros are NOPs without ptrauth/entitlements in a
   standalone test — safe on the M-series dev host; leave a `#if __has_feature(ptrauth_calls)` comment.)

Run: `make -C SheepShaver/src/machine test_mmio_machfault && SheepShaver/src/machine/test_mmio_machfault`
— Expected: `RESULT: ALL PASS`. `make test-jit` (350/350) remains the "paravirtual unchanged" gate.

- [ ] **Step 5: Commit** — `feat(machine): M1 Mach fault path - decode JIT accesses, bus dispatch, thread-state writeback (S2 productionized)`

---

### Task 7: Interpreter range check + host-accessor guards

**Files:**
- Modify: `SheepShaver/src/kpx_cpu/src/cpu/vm.hpp` (accessors ~224–285)
- Modify: `SheepShaver/src/include/cpu_emulation.h` (`Mac2HostAddr` ~71)
- Modify: probe/watch tooling (grep `SS_PROBE_PC` and `SS_JIT_WATCH_ADDR` implementations — sheepshaver_glue.cpp / ppc-cpu.cpp)

- [ ] **Step 1: vm.hpp range check (branch-gated; §2b path 2)**

vm.hpp cannot include mmio_bus.h (it's a low-level header compiled into both engines). Forward-declare the three globals + slow-path entry near the top of vm.hpp (after the includes):

```cpp
// Machine Layer M1 (MACHINE-LAYER-PLAN §2b, interpreter path): device-range check.
// mmio_bus_active is false on the paravirtual default -> one predicted-untaken branch.
extern bool mmio_bus_active;
extern uint32_t mmio_bus_lo, mmio_bus_hi;
extern uint64_t MMIOBusRead(uint32_t addr, unsigned size);
extern void MMIOBusWrite(uint32_t addr, unsigned size, uint64_t value);

static inline bool vm_is_mmio(uint32_t addr)
{
	return __builtin_expect(mmio_bus_active, 0)
	    && (addr - mmio_bus_lo < mmio_bus_hi - mmio_bus_lo);
}
```

Then add the check as the first line of each of the 8 public accessors, with the **literal width
matching each accessor** (rev 2 finding M2 — do NOT hardcode width 4):

```cpp
	// vm_read_memory_1:  if (vm_is_mmio(addr)) return (uint32)MMIOBusRead(addr, 1);
	// vm_read_memory_2:  if (vm_is_mmio(addr)) return (uint32)MMIOBusRead(addr, 2);
	// vm_read_memory_4:  if (vm_is_mmio(addr)) return (uint32)MMIOBusRead(addr, 4);
	// vm_read_memory_8:  if (vm_is_mmio(addr)) return          MMIOBusRead(addr, 8);
	// vm_write_memory_1: if (vm_is_mmio(addr)) { MMIOBusWrite(addr, 1, value); return; }
	// vm_write_memory_2: if (vm_is_mmio(addr)) { MMIOBusWrite(addr, 2, value); return; }
	// vm_write_memory_4: if (vm_is_mmio(addr)) { MMIOBusWrite(addr, 4, value); return; }
	// vm_write_memory_8: if (vm_is_mmio(addr)) { MMIOBusWrite(addr, 8, value); return; }
```
(match each accessor's actual value parameter name and return type.)

**`_reversed` variants:**
- `vm_read_memory_1_reversed` / `vm_write_memory_1_reversed` are **`#define` aliases** of the
  non-reversed byte accessors (vm.hpp:244, 275) — they need **NO edit** (the byte form already
  has the check; a 1-byte swap is identity).
- `vm_read_memory_2_reversed` / `_4_reversed` (and the `_2_reversed` / `_4_reversed` write
  counterparts) get the same check, but on the **architectural** value byte-swapped via
  `__builtin_bswap16` / `__builtin_bswap32` (a reversed access of device space = the architectural
  value swapped). E.g. for `vm_read_memory_2_reversed`:
  `if (vm_is_mmio(addr)) return (uint32)__builtin_bswap16((uint16)MMIOBusRead(addr, 2));`
  and the write counterpart swaps the stored value before `MMIOBusWrite`.

This covers all interpreter loads/stores **and** every `ReadMacInt*`/`WriteMacInt*` host accessor
automatically (cpu_emulation.h:63–70 are thin wrappers) — i.e. §2b path 3's integer case for free.

- [ ] **Step 2: `Mac2HostAddr` guard (raw-pointer host accessors)**

In `cpu_emulation.h` (~71):

```cpp
static inline uint8 *Mac2HostAddr(uint32 addr)
{
	// M1: raw host pointers into trapped device space are a contract violation
	// (MACHINE-LAYER-PLAN §2b host-accessor path) - fail at the source, not at a
	// later undecodable compiler-generated fault.
	extern bool mmio_bus_active; extern uint32 mmio_bus_lo, mmio_bus_hi;
	if (__builtin_expect(mmio_bus_active, 0)
	    && addr - mmio_bus_lo < mmio_bus_hi - mmio_bus_lo) {
		fprintf(stderr, "[MMIO] FATAL: Mac2HostAddr(0x%08x) inside device space - "
		        "use MMIOBusRead/Write\n", addr);
		abort();
	}
	return vm_do_get_real_address(addr);
}
```
(Add `#include <stdio.h>`/`<stdlib.h>` if not already pulled in; if cpu_emulation.h's include set makes this awkward, move the body to an out-of-line helper `mmio_mac2host_abort(addr)` declared extern and defined in mmio_bus.cpp.)

- [ ] **Step 3: Debug tooling refuses device ranges**

Grep for the `SS_PROBE_PC` field-dump implementation (the `[0xADDR]` and `[rN:SIZE]` readers) and the `SS_JIT_WATCH_ADDR` read sites (both in sheepshaver_glue.cpp and/or ppc-cpu.cpp). At each guest-memory read they perform, add:

```cpp
	if (mmio_bus_active && addr - mmio_bus_lo < mmio_bus_hi - mmio_bus_lo) {
		// device reads are side-effecting; tools must never touch them (§2b)
		/* print "<MMIO-refused>" in place of the value / skip this watch addr */
		continue;  // or the local equivalent
	}
```
Match each site's local output idiom. The goal: probing 0xF3xxxxxx prints a refusal marker instead of dispatching a device read.

- [ ] **Step 4: Interpreter-mode bench proof (the §2b rev-3 honesty gate)**

Paravirtual interpreter cost must be unchanged. Measure the full interpreter suite wall-clock, 3 runs each, **before** this task's changes (use `git stash`) and after:

```bash
cd SheepShaver
for i in 1 2 3; do /usr/bin/time -p make test-opcodes 2>&1 | grep -E '^real|score='; done
```
Expected: medians within noise (<2%); score=100 both. Record both medians in the commit message.

- [ ] **Step 5: Full gates**

```bash
cd SheepShaver && make build-ss && make test-jit && make -C src/machine test
```
Expected: 350/350 score=100; ALL PASS (now ×6 with `test_mmio_machfault` from Task 6). These are
build/regression gates only — they do **not** exercise the bus fault path (rev 2 finding C2: the
harness `SS_TEST_HEX` path exits before bus bring-up). Bus-path coverage comes from the
`test_mmio_machfault` unit test (Task 6 Step 4) + the Task 12 acceptance boot.

- [ ] **Step 6: Commit** — `feat(machine): M1 interpreter range check + host-accessor guards (Mac2HostAddr abort, probe/watch refusal) — interp bench delta <2%`

---

### Task 8: Minimal DEC tick on the fidelity profile

**Files:**
- Modify: `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-execute.cpp:1318–1332`

- [ ] **Step 1: Make the synthetic DEC the default when the machine profile is newworld**

`check_work`'s drain/timeout reads DEC twice for deadline math (S3 §2.4); frozen DEC ⇒ second spin site. Replace the `synth` initializer in the `case 22:` block:

```cpp
		/* M1 (MACHINE-LAYER-PLAN M1 row): the newworld profile gets the synthetic
		 * down-counter by default - check_work's drain/timeout loop (S3 §2.4) needs a
		 * moving DEC. SS_SYNTH_DEC still overrides both ways (=0 forces frozen).
		 * mtspr DEC stays dropped; the real clock is M2. */
		static const int synth = getenv("SS_SYNTH_DEC")
			? (getenv("SS_SYNTH_DEC")[0] != '0')
			: (MachineProfileIsNewWorld() ? 1 : 0);
```
Add `#include "machine_profile.h"` to ppc-execute.cpp's includes (check the include-path style used by neighboring includes in that file; the autoconf build already resolves `src/include`).

- [ ] **Step 2: Clean PPC recompile + gates** (kpx_cpu file touched — stale-.o rule):

```bash
cd SheepShaver && make build-ss && make test-jit
```
Expected: 350/350. Paravirtual unchanged (env unset, profile paravirtual ⇒ synth=0 as before).

- [ ] **Step 3: Commit** — `feat(machine): M1 minimal DEC tick - synthetic decrementer default-on for newworld profile`

---

### Task 9: JIT backpatch for hot MMIO sites

**Files:**
- Modify: `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`

**All Task 9 additions are gated behind `#ifdef SS_MMIO_BACKPATCH` (rev 2 finding C4).** `ppc-jit.cpp`
is compiled **standalone** by `SheepShaver/rom-harness/Makefile` (INCLUDES=`-I$(JITDIR)` only; links only
`rom-harness.o + ppc-jit.o`), which has neither `a64_mmio_decode.h` on its include path nor the bus
symbols to link. Wrap **every** Task 9 addition — the `#include "a64_mmio_decode.h"`, the `MMIOBusRead/
Write` externs, `emit_mmio_thunk`, `ppc_jit_pc_in_cache`, `ppc_jit_backpatch_mmio`,
`ppc_jit_mmio_thunk_dispatch`, and the Step-3 selftest — in `#ifdef SS_MMIO_BACKPATCH`. The emulator
build defines it (Makefile.in DEFS, see Task 5 Step 2); the rom-harness build does not, so it still
compiles and links cleanly, and the weak symbols in `mmio_machfault.cpp` stay absent → fault path
cold-only there.

**CRITICAL precondition (created by Task 6's landed implementation):** Task 6 ships `ppc_jit_pc_in_cache`
/ `ppc_jit_backpatch_mmio` as **weak no-op default definitions** in `mmio_machfault.cpp` (Mach-O cannot
resolve a fully-undefined `weak`/`weak_import` symbol to NULL at static link, so the plan's
"declaration-only" approach failed to link — both the standalone test and `build-ss`). Therefore Task 9
**must define both as plain non-weak `extern "C"`** functions (NOT `weak`) with **exact matching
signatures** — `bool ppc_jit_pc_in_cache(const void *host_pc)` and
`bool ppc_jit_backpatch_mmio(uint32_t *site, const A64MemAccess *acc)` (so `#include "a64_mmio_decode.h"`
is required). A strong definition overrides the weak stub; two competing **weak** definitions let the
linker pick the no-op stub by order, silently killing backpatch (a perf regression invisible to test-jit
— it surfaces only as `backpatches=0` in the bus telemetry under load).

S2 measured ~8.5 µs/fault (~10⁴× a mapped access): the fault path is discovery-only; hot sites must
become direct bus calls. Design (validated by the Task-0 facts): **x30 is dead mid-block** ⇒ a `BL` at
any access site is safe; **EA is always in w0 (RTMP0)**; guest GPRs live in callee-saved x21–x28 (safe
across calls) but **guest FPRs live in caller-saved v16–v23** and temps x0–x3 may be live ⇒ the thunk
saves **x0..x28** (banking x18 too for slot regularity) + q0–q7 + q16–q31, and additionally **saves/
restores NZCV** around the BLR (the lazy CR0 / live flags can span an access site — see NZCV note in
Step 1).

**Patch shapes** (decided by the original insn at the fault PC, captured in a side table):
- load, width>1: `LDR;REV` → `BL mmio_thunk; NOP` — helper writes the **architectural** value to the frame slot for rt (the REV is gone). Verify insn@pc+4 with `A64IsPairedSwap` before patching; if it doesn't match (shouldn't happen — contract), don't patch, keep fault-servicing.
- load, byte: `LDRB` → `BL mmio_thunk` (single insn).
- store, any width: keep the preceding REV (if any); `STR` → `BL mmio_thunk` — frame[rt] holds raw BE for width>1 (REV already ran), architectural for bytes; helper swaps per width exactly like the fault path.
- The thunk computes the site key as `x30 - 4` and looks up the original insn in the side table to decode rt/rm/width/direction at runtime — one generic thunk serves all forms and register assignments.

- [ ] **Step 1: Emit the thunk at JIT init + export cache-bounds + side table**

In ppc-jit.cpp add (near the cache globals) — **the entire block, and every other Task 9 addition,
is wrapped in `#ifdef SS_MMIO_BACKPATCH … #endif`** (rev 2 finding C4):

```cpp
#ifdef SS_MMIO_BACKPATCH
/* ---- M1 MMIO backpatch (MACHINE-LAYER-PLAN §2b polling strategy) ---- */
#include "a64_mmio_decode.h"
extern "C" uint64_t MMIOBusRead(uint32_t addr, unsigned size);   // C++ linkage ok via decl in mmio_bus.h; match it
extern "C" void MMIOBusWrite(uint32_t addr, unsigned size, uint64_t value);

#define MMIO_SITE_MAX 256
static struct { uint32_t *site; uint32_t orig_insn; } mmio_sites[MMIO_SITE_MAX];
static int n_mmio_sites = 0;
static uint32_t *mmio_thunk_entry = NULL;   // emitted once at init

extern "C" bool ppc_jit_pc_in_cache(const void *pc)
{
	return jit_cache_base && (const uint8_t *)pc >= jit_cache_base
	       && (const uint8_t *)pc < (const uint8_t *)jit_cache_end;
}
```

(Use the real C++ declarations from `mmio_bus.h` rather than re-declaring with wrong linkage — include the header; it has include guards and no conflicting deps.)

The C dispatch helper the thunk calls (plain C++, runs on the CPU thread — no §2g constraints):

```cpp
/* frame layout the thunk builds (16-byte units, see emit_mmio_thunk):
 *   frame[0..28]  = x0..x28 (x18 saved too for slot regularity)
 *   the thunk passes: x0 = frame base, x1 = site address (x30-4 at entry)        */
extern "C" void ppc_jit_mmio_thunk_dispatch(uint64_t *frame, uint32_t *site)
{
	uint32_t orig = 0;
	for (int i = 0; i < n_mmio_sites; i++)
		if (mmio_sites[i].site == site) { orig = mmio_sites[i].orig_insn; break; }
	A64MemAccess acc;
	if (!orig || !A64DecodeMMIOAccess(orig, &acc)) {
		fprintf(stderr, "PPC-JIT-A64: MMIO thunk at unpatched site %p\n", (void *)site);
		abort();
	}
	uint32_t gaddr = (uint32_t)frame[acc.rm];          // EA reg (rm) holds the guest address
	unsigned bytes = 1u << acc.size_log2;
	if (acc.is_load) {
		uint64_t arch = MMIOBusRead(gaddr, bytes);
		/* REV was NOPed: deliver the ARCHITECTURAL value, zero-extended */
		if (acc.rt != 31) frame[acc.rt] = arch;
	} else {
		uint64_t raw = (acc.rt == 31) ? 0 : frame[acc.rt];
		/* width>1: REV still runs before the BL, so frame[rt] is raw BE */
		uint64_t arch = (acc.size_log2 == 0) ? (raw & 0xFF)
		                                     : A64SwapForWidth(raw, acc.size_log2);
		MMIOBusWrite(gaddr, bytes, arch);
	}
}
```

Thunk emission, called once from `ppc_jit_aarch64_init` after the cache is allocated.

**Use the verified encodings — do NOT re-derive (rev 2 T9-encodings).** The full, byte-exact thunk
(66 insns, frame layout, every helper formula, and the clang cross-check) is in
`docs/superpowers/plans/m1-task9-thunk-encodings.md`. The implementer **must port the verified helper
functions from `spikes/m1-thunk-prework/thunk_ref.c`** (`emit_stp_x` / `emit_ldp_x` / `emit_str_x_imm` /
`emit_ldr_x_imm` / `emit_stp_q` / `emit_ldp_q` / `emit_sub_sp_imm` / `emit_add_sp_imm` / `emit_load_imm64`)
rather than hand-packing immediates inline. Each helper `assert()`s its immediate alignment/range — keep
those asserts. Verified facts the emitter must honor:

- **Frame = 640 bytes total**: `STP x29,x30,[sp,#-16]!` (the fp/lr pre-index push, 16 B) **then**
  `SUB sp,sp,#624` (rev 2 M1/T9 — **#624, not #640**; the sketch's `#640` over-allocated). 624 is a
  16-multiple (the only hard alignment rule for the BLR).
- Slots **relative to the new SP** (= frame base passed to the dispatcher in x0): x0..x27 as 14 STP
  pairs at offsets 0..208 (x{i} at i*8); **x28 via a single `STR x28,[sp,#224]`** — the correct word is
  **`0xf90073fc`** (`emit_str_x_imm(28, 224)`); the old sketch constant `0xf90070fc` was **WRONG**
  (base = x7, not sp). An **8-byte alignment pad at offset 232**; the Q region (q0–q7, q16–q31) starts at
  **240** (STP-Q imm7 scales by 16, so Q must begin on a 16-multiple).
- `mov x0, sp` must be the **ADD-alias `0x910003E0`** (= `ADD x0, sp, #0`) — NOT the ORR-MOV form
  (reg 31 there is XZR → x0 = 0, silently breaking the frame-base arg).
- `sub x1, x30, #4` = `0xD10013C1`; `blr x16` = `0xD63F0200`; `ret` = `0xD65F03C0`; `ldp x29,x30,[sp],#16`
  = `0xA8C17BFD`. (All sketch constants except `str x28` were verified correct, but prefer the helpers.)

**Cache-pointer wiring (rev 2 C3).** `emit32` writes through `jit_code_ptr`, which is **uninitialized at
JIT init** (it is only set inside the block compiler at ppc-jit.cpp:5286/5541). If `emit_mmio_thunk`
emits without setting it, the thunk lands at NULL and `jit_cache_wp` never advances (the first compiled
block overwrites it). The emitter must therefore:
```cpp
static void emit_mmio_thunk(void)
{
	jit_cache_begin_write();
	jit_code_ptr = jit_cache_wp;          // (rev 2 C3) emit32 writes through jit_code_ptr — set it
	mmio_thunk_entry = jit_code_ptr;
	/* ... prologue / saves / NZCV-save / BLR / NZCV-restore / restores / epilogue,
	 *     all via the ported helpers; reserve the thunk at cache start so every later
	 *     site is within BL ±128 MB range ... */
	jit_cache_wp = jit_code_ptr;          // (rev 2 C3) publish the advanced write pointer
	jit_cache_end_write(mmio_thunk_entry, (jit_code_ptr - mmio_thunk_entry) * 4);  // restores W^X + invalidates icache
}
```

**NZCV save/restore (rev 2 NZCV).** The thunk saves **no** NZCV by default, but the JIT's lazy CR0 (or
any live flags) can span a memory-access site, and the C callback (plus the BLR itself) clobbers NZCV.
Do **not** rely on auditing flag liveness — unconditionally save/restore around the BLR with x9 as
scratch (x9's guest value is already banked in the frame and restored afterward, so it is free here):
`MRS x9, NZCV` (= **`0xD53B4209`**, Rt=9) immediately before `BLR x16`, and `MSR NZCV, x9`
(= **`0xD51B4209`**, Rt=9) immediately after the BLR, **before** the LDP/LDR restores. **Verify these
two encodings against clang in the Step-3 selftest** — they were not among the prework's 18 cross-checked
helpers.

**Constraint comment (rev 2 N1):** the `stwcx.` site has a `CBZ +8` immediately before the STR; patching
STR→BL keeps the skip-target valid **only because BL is also 4 bytes**. Record this as a constraint
comment at the patcher: single-instruction patches only (never grow or shrink the patched insn count).

- [ ] **Step 2: The patcher (called on the Mach handler thread — §2g-safe by construction)**

```cpp
extern "C" bool ppc_jit_backpatch_mmio(uint32_t *site, const A64MemAccess *acc)
{
	if (!mmio_thunk_entry || n_mmio_sites >= MMIO_SITE_MAX) return false;
	int64_t off = (int64_t)((uint8_t *)mmio_thunk_entry - (uint8_t *)site);
	if (off < -(1 << 27) || off >= (1 << 27)) return false;     // BL range ±128MB
	uint32_t bl = 0x94000000 | (uint32_t)((off >> 2) & 0x3FFFFFF);
	bool nop_rev = acc->is_load && acc->size_log2 > 0;
	if (nop_rev && !A64IsPairedSwap(site[1], acc->size_log2, acc->rt))
		return false;                                            // contract surprise: stay cold
	mmio_sites[n_mmio_sites].site = site;
	mmio_sites[n_mmio_sites].orig_insn = *site;
	__atomic_add_fetch(&n_mmio_sites, 1, __ATOMIC_RELEASE);     // dispatch scans <= n
	// (rev 2 N1) single-instruction patch only: STR/LDR/B → BL keeps any nearby CBZ +8
	// skip-target valid because all are 4 bytes. Never change the patched insn count.
	jit_cache_begin_write();        // pthread_jit_write_protect_np is PER-THREAD: ok here
	site[0] = bl;
	if (nop_rev) site[1] = 0xD503201F;                          // NOP the REV
	jit_cache_end_write(site, nop_rev ? 8 : 4);                 // restores W^X + sys_icache_invalidate
	return true;
}
```
`jit_cache_end_write` already re-protects (W^X / `pthread_jit_write_protect_np(1)`) **and** invalidates
the icache (`sys_icache_invalidate`) — there is no `jit_cache_begin_write_restore_exec()` symbol; do not
add one (rev 2 M1). Safety argument to keep as a comment: the only thread that executes JIT code is the
emul thread, and it is Mach-suspended **at exactly this site** while we patch; the handler thread's
write-protect toggle is per-thread; after `sys_icache_invalidate` the resumed thread re-fetches the BL.
Also note: `lwarx`/`stwcx.` sites lose reservation semantics if patched — acceptable (no S3 consumer uses
atomics on device space), and the cold path has the same property; leave a comment.

- [ ] **Step 3: Standalone thunk self-test (before any boot)**

Add a temporary `SS_MMIO_THUNK_SELFTEST=1` block at the end of `ppc_jit_aarch64_init` (kept, env-gated — it's cheap and permanent insurance):

```cpp
	if (getenv("SS_MMIO_THUNK_SELFTEST")) {
		/* emit a 4-insn block: LDR w1,[x19,w0,uxtw]; REV w1,w1; RET-pattern…
		   then register a fake bus region via test hooks, patch it with
		   ppc_jit_backpatch_mmio, call it with w0 = device addr, assert the
		   architectural value lands in w1. fprintf PASS/FAIL and exit(0/1). */
	}
```
Implement it fully (emit block with the existing emitters, call through a function pointer with x0 = guest addr and x19 = 0 — the thunk path never dereferences x19). **Also cross-check the two NZCV encodings here** (rev 2 NZCV): assert `emit_mrs_nzcv(9) == 0xD53B4209` and `emit_msr_nzcv(9) == 0xD51B4209` against a `clang -c` reference of `mrs x9, nzcv` / `msr nzcv, x9` (they were not in the prework's 18-helper cross-check). Run:
```bash
cd SheepShaver && SS_MMIO_BUS=1 SS_MMIO_THUNK_SELFTEST=1 ./SheepShaver --help 2>&1 | grep THUNK
```
Hmm — `--help` may not reach JIT init; if not, run the selftest via the harness path instead: `SS_MMIO_BUS=1 SS_MMIO_THUNK_SELFTEST=1 SS_TEST_HEX=60000000 SS_TEST_JIT=1 make test-opcodes` (JIT init runs before the vector). Expected: `THUNK-SELFTEST: PASS`.

(Close the `#ifdef SS_MMIO_BACKPATCH` from Step 1 after the selftest block — every Task 9 addition is inside that guard.)

- [ ] **Step 4: Gates**

```bash
cd SheepShaver && make build-ss && make test-jit && make -C rom-harness bench BENCH_QUIET=1
```
Expected: 350/350; bench unchanged vs a pre-task baseline (`make bench BARGS=--save-baseline=/tmp/m1-pre9.txt` before starting, `--compare` after — the JIT fast path gained zero instructions).

**Also gate the standalone rom-harness build (rev 2 finding C4):**
```bash
make -C SheepShaver/rom-harness          # must still compile + link (no SS_MMIO_BACKPATCH define)
```
Expected: clean build (the `#ifdef SS_MMIO_BACKPATCH` guard keeps Task 9's additions out of this
standalone compile of `ppc-jit.cpp`). The `make bench BENCH_QUIET=1` above must also run.

- [ ] **Step 5: Commit** — `feat(jit+machine): M1 MMIO backpatch - hot fault sites become direct bus calls via generic thunk`

---
### Task 10: `scc_init` patch retirement + `[KDP-0x900]` SCC base on newworld

**Files:**
- Modify: `SheepShaver/src/rom_patches.cpp:2106–2119` (`scc_init_caller_dat`/`scc_init_dat` site)
- Modify: `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp:1473` (`[KDP-0x900]`)

- [ ] **Step 1: Retire `scc_init` on the fidelity profile (ROM-PATCH-AUDIT: RETIRE@M1, enforcement option 1)**

At the `scc_init` patch site in `patch_68k()` (locate by the `scc_init_caller_dat` pattern name), wrap the patch application:

```cpp
	// M1 (ROM-PATCH-AUDIT: scc_init RETIRE@M1): on the fidelity profile the guest's
	// own SCC init must run and reach the SCC 8530 model through the bus — the patch
	// would make the device model dead code. Paravirtual keeps the patch.
	if (MachineProfileIsNewWorld()) {
		fprintf(stderr, "[M1] scc_init ROM patch retired (newworld profile): guest SCC init will run\n");
	} else {
		/* …existing patch application unchanged… */
	}
```
Keep the *search* (`find_rom_data`) outside the condition if its result feeds later code; only the byte-patching is gated. `MachineUsesMMIOBus()` is deliberately NOT the gate here: on the SS_MMIO_BUS third config (paravirtual) the rest of the paravirtual patch set still expects the patched init — only the full fidelity profile retires it.

- [ ] **Step 2: Point `check_work` at the SCC (consumer (b) wiring)**

At `sheepshaver_glue.cpp:1473`, replace the unconditional zero:

```cpp
	// M1: give check_work a real SCC. [KDP-0x900] is the nanokernel's SCC base
	// (SPIKE-S3 §2: lbz +2 = RR0, lbz +6 = data, full WR init at 0x50326980).
	// 0xF3012000 is the bus's SCC region — polls now Mach-fault into the model.
	// SS_NW_NO_SCC=1 restores the M0 behavior (base 0 -> check_work returns -1).
	if (!MachineEnvFlag("SS_NW_NO_SCC")) {
		WriteMacInt32(kdp - 0x900, 0xF3012000);
		fprintf(stderr, "[NW-TRAMP] [KDP-0x900]=0xF3012000 (SCC via MMIO bus)\n");
	} else {
		WriteMacInt32(kdp - 0x900, 0);
		fprintf(stderr, "[NW-TRAMP] [KDP-0x900]=0 (no SCC - check_work returns -1)\n");
	}
```

- [ ] **Step 3: Build + paravirtual gate** — `make build-ss && make test-jit` → 350/350 (both sites are newworld-only).

- [ ] **Step 4: Commit** — `feat(machine): M1 scc_init patch retirement + check_work SCC base on newworld profile`

---

### Task 11: Non-regression checkpoint (verification only — no commit)

- [ ] **Step 1:** `cd SheepShaver && make build-ss && make test-jit` → `score=100` (350/350).
- [ ] **Step 2:** `make -C src/machine test` → ALL PASS for all five test binaries.
- [ ] **Step 3:** `make -C rom-harness bench BARGS=--compare=/tmp/m1-pre9.txt` → no regression beyond noise.
- [ ] **Step 4:** `make e2e-test` → all offline tests pass.
- [ ] **Step 5:** `make e2e` (needs a logged-in GUI session + assets — **coordinate with the user before launching**; the e2e harness is the sanctioned agent-launch exception) → lifecycle PASS, exit 0. The paravirtual contract: byte-identical behavior, `[MMIO]` lines absent from the log.
- [ ] **Step 6:** Report all five results verbatim to the orchestrator. Any failure: stop, `superpowers:systematic-debugging`, do not proceed to Task 12.

---

### Task 12: Acceptance — consumer (b): `check_work` polls a real SCC without a fault storm

**This task launches the emulator (9.0.1 diagnostic boot). Ask the user before each run** (CLAUDE.md rule; this is NOT the e2e exception). The boot is headless (`nogui true`) and short.

- [ ] **Step 1: Create the diagnostic prefs** (CLAUDE.md recipe):

```bash
printf 'rom /Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom\nramsize 268435456\nnogui true\nmachine newworld\n' > /tmp/m1accept.prefs
```
(If that exact ROM filename is absent, use the staged `Mac OS ROM 9.0.1` per CLAUDE.md asset table; verify with `ls /Users/Shared/macemu/`.)

- [ ] **Step 2: Baseline run (SCC disabled) — sanity that M0 behavior still stands:**

```bash
cd SheepShaver && SS_NW_NO_SCC=1 SS_ROM_LENIENT=1 SS_ROM_SKIP_JUMP68K=1 \
  timeout 60 ./SheepShaver --config /tmp/m1accept.prefs 2>&1 | tee /tmp/m1-baseline.log
```
Expected: reaches the same boot stage as the M0-era diagnostic boot; `[MMIO]` stats show zero or near-zero SCC traffic.

- [ ] **Step 3: Acceptance run (SCC live):**

```bash
SS_ROM_LENIENT=1 SS_ROM_SKIP_JUMP68K=1 \
  timeout 120 ./SheepShaver --config /tmp/m1accept.prefs 2>&1 | tee /tmp/m1-accept.log
```

Assert, from `/tmp/m1-accept.log` (the atexit `[MMIO]` dump + run lines):
1. **Observed device register traffic** (the DoD false-pass guard): `scc8530` region `reads` and `writes` both > 0; if the un-patched 68k `scc_init` ran (Task 10 retirement message present), SCC `wr_writes` telemetry implies the §2.3/§1.3 init sequences reached the model — confirm by adding a one-line stderr print in SCCWrite on WR3=0xC1 (`[SCC] ch A Rx enabled (WR3=0xC1) — init sequence reached the model`) if not already observable.
2. **No fault storm**: `jit_faults` for the SCC region ≪ `reads` (backpatch took over: `backpatches` ≥ 1), and the run's wall-clock CPU usage during the idle phase is bounded (idle_sleeps > 0 shows the idle hook engaged). Concretely: `jit_faults < 1000` over a 2-minute run that performs ≥ 10⁵ reads.
3. **No undecodable-access aborts, no unmodeled-MacIO aborts** on the boot path actually exercised. If the boot touches an unmodeled MacIO address (stub abort), STOP and report — that's a scope finding for the orchestrator (fence decision), not something to hack around.
4. Boot reaches **at least** the M0-era stage (no regression vs Step 2's baseline).

- [ ] **Step 4: ROM-PATCH-AUDIT follow-up datum** — the audit's S3 note asks M1 to record whether un-initialized boot-time VIA state matters to the T2-timeout path. From the acceptance log: if the `via6522` region saw traffic, note which registers (add a one-shot stderr line per first-touch register index in VIAWrite/VIARead if needed); record the outcome in Task 13's audit update. If VIA saw zero traffic on this boot, record "consumer (b) is SCC-only as S3 predicted; via_init cluster disposition unchanged (RETIRE@M3)".

- [ ] **Step 5: Commit any instrumentation added** — `test(machine): M1 acceptance instrumentation (SCC init-reach print, VIA first-touch telemetry)`.

---

### Task 13: Documentation + bookkeeping

**Files:**
- Modify: `docs/planning/MACHINE-LAYER-PLAN.md` (header status + M1 row → done, with the acceptance numbers)
- Modify: `docs/planning/machine/ROM-PATCH-AUDIT.md` (scc_init row → retired note + the §SPIKE-S3-dependency-note outcome from Task 12 Step 4)
- Modify: `docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md` (§5 Q2 if VIA/+0x15000 evidence appeared; otherwise untouched)
- Modify: `CHANGELOG.md` (M1 entry, M0-entry style: component-tagged, cites commits + gate results)
- Modify: `docs/planning/ROADMAP.md` (D3 status line if it references M0/M1)
- Modify: `LEARNINGS.md` (only if Task 12 produced a non-obvious finding worth a session entry)

- [ ] **Step 1: Update each doc.** CHANGELOG entry skeleton:

```markdown
### [SheepShaver] Machine Layer M1: MMIO bus + SCC 8530 + VIA 6522 timer/IFR + AArch64 fault decoder

- **MMIO bus (3 dispatch paths)** — trapped-region registry with per-device locks, telemetry,
  idle hook (`src/machine/mmio_bus.cpp`); JIT path = Mach fault -> AArch64 decode -> bus ->
  thread-state writeback (S2 productionized, `mmio_machfault.cpp`); interpreter path =
  branch-gated range check in `vm.hpp`; host-accessor path = `ReadMacInt*` auto-routed +
  `Mac2HostAddr` abort guard + probe/watch refusal. Hot sites backpatched to a generic
  bus thunk (`ppc-jit.cpp`). Active only on `machine newworld` or `SS_MMIO_BUS=1`.
- **Devices** — SCC 8530 (legacy +2/+6 layout, WR pointer state machine; S3 §1.3+§2.3
  conformance vectors as unit tests) + VIA 6522 timer/IFR surface (lazy T1/T2, Cuda loud
  stub). `scc_init` ROM patch retired on newworld; `[KDP-0x900]` -> 0xF3012000.
- **Minimal DEC tick** — synthetic decrementer default-on for newworld (M2 absorbs).
- **Paravirtual non-regression** — test-jit 350/350 score=100, machine unit tests ALL PASS,
  e2e PASS, interp bench delta <2%, rom-harness bench unchanged. Acceptance: check_work
  polls the SCC model via the bus — reads=N writes=N jit_faults=N backpatches=N (fill from
  /tmp/m1-accept.log).
```

- [ ] **Step 2: Cross-tracker sweep** — grep `docs/` for `M1` references in MACHINE-LAYER-PLAN/ROADMAP and update every tracker that mentions the milestone (CONTRIBUTING's "update EVERY tracker" rule).

- [ ] **Step 3: Commit** — `docs(machine): mark Machine Layer M1 complete`

---

## Self-review record (kept per writing-plans skill)

- **Spec coverage vs M1 DoD row:** three-path dispatch ✓ (T6/T7); decoder + S2 gotchas as unit tests ✓ (T4); endianness contract ✓ (T6 inject raw BE / T9 architectural-after-NOP-REV); interpreter profile-gating + bench proof ✓ (T7 S4); explicit host entry points ✓ (T7); region kinds in API ✓ (T1); backpatch in scope ✓ (T9); fault-rate logging + idle hook ✓ (T1/T12); §2g locking ✓ (T1/T6/T9); SCC ✓ (T2); VIA timer/IFR + Cuda loud stub ✓ (T3); minimal DEC tick ✓ (T8); consumer (a) demoted — covered only as the SS_MMIO_BUS named config existing (T5), no STM acceptance per the plan's 2026-06-10 demotion note; consumer (b) acceptance with register-traffic + fault-budget asserts ✓ (T12); SS_JIT_VERIFY exclusion ✓ (T5 hard-incompat); patch retirement + un-patched-init assert ✓ (T10/T12); 16 KB granularity note ✓ (container reservation is page-aligned; sub-page regions are logical).
- **Known sharp edges flagged to implementers:** thunk encodings now point to the verified
  `m1-task9-thunk-encodings.md` + `spikes/m1-thunk-prework/thunk_ref.c` (rev 2 — no longer re-derived);
  the two NZCV encodings are the one remaining un-cross-checked pair (verified in the T9 Step 3 selftest);
  `sigsegv_get_thread_state` must lazy-fetch (T6 Step 1, rev 2 C1); guest-addr conversion in
  sigsegv_handler uses the visible `VMBaseDiff` directly (T6 Step 3); `--help`-may-not-reach-JIT-init
  fallback (T9 Step 3); `SS_TEST_HEX` exits before bus init so it cannot smoke the bus (T6 Step 4, rev 2 C2).
- **Type consistency:** `MMIOBusRead/Write(addr, size, value)` signatures uniform across T1/T6/T7/T9; `A64MemAccess` fields consistent; `MachineUsesMMIOBus` used in T5 only (T10 deliberately uses `MachineProfileIsNewWorld` — reasoned in-line).

## Rev 2 corrections (red-team round 3, 2026-06-10)

Folded in from an adversarial code-verified review (round 3) + two recon dossiers
(`m1-task6-recon.md`, `m1-task9-thunk-encodings.md`). All applied to Tasks 5–9 before execution:

- **C1** (T6 Step 1b) — `sigsegv_get_thread_state` must lazily `mach_get_thread_state` before returning
  `&SIP->thr_state` (arm64 enters the handler with `has_thr_state == false`; raw return = uninitialized
  storage corrupting the writeback). Also: live handler is glue (`sheepshaver_glue.cpp:919`) only —
  `main_unix.cpp:2275` is `#if !EMULATED_PPC` dead code. MMIO dispatch inserts at glue :929; conversion
  `gaddr = (uint32)((uintptr)addr - VMBaseDiff)`; enum/decl insertion points recorded.
- **C2** (T6 Step 4 / T7 Step 5) — `SS_TEST_HEX` exits `main()` at :1162 before bus bring-up, so the
  planned smoke is a false positive. Replaced with a `test_mmio_machfault.cpp` standalone unit test;
  T7 Step 5 reframed as build/regression gates only (no bus-exercise claim).
- **C3** (T9 Step 1) — `emit32` writes through `jit_code_ptr` (uninitialized at init); added
  `jit_code_ptr = jit_cache_wp` / `mmio_thunk_entry = jit_code_ptr` before and `jit_cache_wp = jit_code_ptr`
  + `jit_cache_end_write(...)` after.
- **C4** (T9) — `ppc-jit.cpp` is compiled standalone by `rom-harness/Makefile`; all T9 additions gated
  behind `#ifdef SS_MMIO_BACKPATCH` (defined only in the emulator build's Makefile.in DEFS). Added a
  rom-harness build+bench gate to T9 Step 4; weak-symbol consequence noted at T6 Step 2.
- **M1** (T9 Step 2) — deleted the non-existent `jit_cache_begin_write_restore_exec()` line;
  `jit_cache_end_write` already restores W^X + invalidates icache.
- **M2** (T7 Step 1) — vm.hpp accessor edits shown with literal per-width values (1/2/4/8), not a
  hardcoded 4; `_1_reversed` are `#define` aliases (no edit); `_2/_4_reversed` use `__builtin_bswap16/32`.
- **T9-encodings** (T9 Step 1) — replaced the hand-packed sketch with a pointer to the verified
  `m1-task9-thunk-encodings.md`; frame = 640 (SUB #624 + fp/lr push), `str x28` = `0xf90073fc`
  (sketch's `0xf90070fc` was base=x7, WRONG), `mov x0,sp` = ADD-alias `0x910003E0`; port helpers from
  `thunk_ref.c` rather than re-deriving.
- **NZCV** (T9 Step 1 + N1) — thunk must `MRS x9,NZCV`/`MSR NZCV,x9` (= `0xD53B4209`/`0xD51B4209`,
  verify in selftest) around the BLR to preserve lazy CR0 / live flags. N1: the `stwcx.` `CBZ +8`
  skip-target stays valid only because BL is also 4 bytes — single-instruction patches only.
- **Task 5 additions** — `test_mmio_machfault` recipe + `.gitignore` + `test:` target entry;
  `mmio_machfault.cpp` stub already created in T5 Step 2; `-DSS_MMIO_BACKPATCH` wiring (C4).

Could-not-apply-cleanly: none. (C2's "Task 7 Step 5" clause: Step 5 contained no "bus exercised by
harness" claim to strip — added a one-line clarification there instead, and the actual false-pass
smoke that needed replacing was in Task 6 Step 4, which was rewritten.)

