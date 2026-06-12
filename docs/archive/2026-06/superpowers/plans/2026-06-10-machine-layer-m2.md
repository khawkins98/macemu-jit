> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# Machine Layer M2 — Virtual clock: guest TB/DEC + host event scheduler + DEC-expiry condition

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land Machine Layer milestone M2 per `docs/planning/MACHINE-LAYER-PLAN.md` §3: a guest-visible TB/DEC honoring `mtspr`/`mfspr` (absorbing the `SS_SYNTH_DEC` hack and M1's minimal DEC tick), a host event scheduler for device timers (ported from DingusPPC `core/timermanager.{cpp,h}` per backport hygiene), DEC-expiry raising the decrementer exception **condition only** (delivery is M3), the VIA 6522 timers re-wired onto the clock/scheduler (resolving conformance notes N6/N7/N8), and the `mdec_dat` nanokernel patch retired on the newworld profile with an observed-traffic DoD — all while the paravirtual profile stays byte-identical (test-jit 350/350, e2e PASS).

**Architecture:** Two new pure modules under `SheepShaver/src/machine/` — `virt_clock` (TB/DEC state, injected host-ns source, expiry latch + generation guard) and `event_sched` (DingusPPC TimerManager port: priority-queue of ns-deadline callbacks, `process_timers()` pump contract). Integration seams: the interpreter's SPR slow paths in `ppc-execute.cpp` (the JIT compiles **no** SPR/mftb forms — verified — so zero JIT work), a scheduler pump thread + clock bring-up in `main_unix.cpp`, a `MMIOBusWithRegion` locked-callback API in `mmio_bus`, and the profile-gated `mdec_dat` site in `rom_patches.cpp`. DEC expiry sets a latch + telemetry; **nothing** is wired into spcflags/interrupt delivery in M2.

**Tech Stack:** C++11 (machine-module standalone tests via clang++; `event_sched` uses `<functional>/<memory>/<queue>/<mutex>` exactly as the donor), pthreads (pump thread, condvar), `unsigned __int128` for overflow-safe ratio math (arm64 clang), autoconf build (`Makefile.in` SRCS + `./config.status Makefile`).

---

## Authoritative inputs (read before implementing any task)

| Doc | What it fixes |
|---|---|
| `docs/planning/MACHINE-LAYER-PLAN.md` §2c/§2g + M2 row | Clock/scheduler design, DEC≠PIC split, threading rules, DoD |
| `docs/planning/machine/ROM-PATCH-AUDIT.md` `mdec_dat` row | RETIRE@M2 disposition + "observed un-patched init traffic" enforcement rule |
| `docs/planning/machine/M1-DEVICE-CONFORMANCE.md` §6 N1–N9 | Timer deltas M2 must address: **N6** (T2CL read clears IFR.T2), **N7** (expiry at N+1 ticks), **N8** (IFR write must not stop the timer — fired-latch semantics). N1–N5/N9 are SCC notes, out of M2 scope (conditional on guests we don't have). |
| `docs/planning/spikes/SPIKE-S3-STALL-DEVICE-PROBE.md` §2.4 | `check_work`'s DEC deadline math — the live consumer contract the DEC unit test simulates |
| `LEARNINGS.md` 2026-06-10 entries | ignoresegv-illusion rule; SS_TEST_HEX bypasses init; weak-stub/strong-test-override pattern; stale-.o rule |
| DingusPPC `core/timermanager.{cpp,h}` @ `92bb6d10549529f9f4031a85c2bc136149535bdc` | The scheduler donor (GPL-3.0; see Task 2 citation block) |

## Codebase facts (verified 2026-06-10; line numbers may drift — pattern names are stable)

- **DEC today** (`SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-execute.cpp:1320–1340`): `execute_mfspr` `case 22` returns a synthetic down-counter `0u - (uint32)get_tb_ticks()` when `SS_SYNTH_DEC` truthy or `MachineProfileIsNewWorld()`, else 0 + stub-count. **`mtspr DEC` is dropped** (default case :1386–1388). `get_tb_ticks()` (:1418–1429) = `muldiv64(GetTicks_usec(), TimebaseSpeed, 1000000)`.
- **TB today**: `execute_mftbr` (:1431–1443) serves TBR 268/269 from `get_tb_ticks()`. `mtspr` 284/285 (TBL/TBU writes) fall to the drop-default. `TimebaseSpeed` is set in `main_unix.cpp` (default 25 MHz, declared extern via the include chain — ppc-execute.cpp already references it).
- **JIT** *(rev 2 — corrected)*: `ppc-jit.cpp` natively compiles `mfspr`/`mtspr` **only for LR/CTR/XER** (case 339 at :1957, case 467 at :1990); all unknown SPRs (incl. DEC=22, TBL=284, TBU=285) and **all `mftb`** (case 371 at :2409, explicit `return false`) fall back to the interpreter. **M2 makes no JIT changes.** The harness runs paravirtual (no `machine` pref) so DEC reads stay 0 in both interp and JIT REGDUMPs — no test-jit diff risk; the harness has no SPR-22 vector (verified — `T_mftb_basic` actually encodes mfspr spr=6, stubbed-0, deterministic).
- **`mdec_dat`** (`SheepShaver/src/rom_patches.cpp:1693–1707`, in `patch_nanokernel()`): pattern `7f f6 02 a6 …` searched in 0x310000–0x314000; patch sets `lp[0] = li r31,0` and NOPs `lp[3]`/`lp[4]` so the NK never programs the real decrementer. **(rev 2 — load-bearing fact)** the pattern exists in the **1.1/OldWorld NK** (ROM offset 0x312c24) but is **ABSENT from the 9.0.1 parcels ROM** (byte-verified against `/tmp/rom901_decompressed.bin`, incl. the patch-invariant middle words; corroborated by `[ROMPATCH] SKIP mdec` in every prior 9.0.1 boot log and HANDOFF §"same … mdec … skips"). On the 9.0.1 acceptance boot the retirement gate is therefore **not exercised** — the parcels NK is natively un-patched. Task 8's DoD is split accordingly.
- **`TimebaseSpeed`** *(rev 2 — corrected)*: a **zero-initialized** global (`main_unix.cpp:213`, `int64`); the 25 MHz default (or `cpuclock`-pref-derived value) is assigned inside `get_system_info()`, called at `main_unix.cpp:1396` — **after** `MachineProfileInit()` (:1340). Clock init must come after :1396 or it silently pins 25 MHz while `get_tb_ticks()` uses the real rate.
- **Known-broken bystander**: the `test-powerpc` target (`src/Unix/Makefile:277,292`) compiles `ppc-execute.cpp` without the machine modules and has been unlinkable since M1 (`MachineProfileIsNewWorld` unresolved); `g_virt_clock` adds a second unresolved symbol. It is not in `PROGS`/`all` — no gate breaks; do not "fix" it in M2.
- **VIA model as landed** (`SheepShaver/src/machine/dev_via6522.cpp`, header `src/include/dev_via6522.h`): lazy timers (`timer_remaining` at :57–67 fires at `dt >= cnt` = N ticks, conformance note N7 says hardware is N+1); `ifr_now()` (:70–80) ORs computed expiry into the latched IFR; IFR write-1-clear **stops** an expired timer (:148–156, note N8); T2CL read does **not** clear IFR.T2 (note N6). Cuda loud-stub warning is *latched*, not printed (`VIATakePendingWarning`) — §2g stdio rule; preserve that pattern for any new warning.
- **VIA clock hookup** (`SheepShaver/src/Unix/main_unix.cpp:1160–1164` + bus bring-up ~:1560–1600): `mmio_via_now_ticks = GetTicks_usec() * VIA_CLOCK_HZ / 1000000ull`; `VIAReset(&via, 0xF3016000, mmio_via_now_ticks, NULL)` at :1574; `MMIOBusActivate()` at :1587. Bring-up is gated on `MachineUsesMMIOBus()`.
- **Bus locking** (`SheepShaver/src/machine/mmio_bus.cpp`): one `pthread_mutex_t` per region, held around the device handler; per-region stats. No "run callback under region lock" API exists yet — Task 3 adds `MMIOBusWithRegion`.
- **Profile API** (`SheepShaver/src/include/machine_profile.h`): `MachineProfileIsNewWorld()`, `MachineUsesMMIOBus()`, `MachineEnvFlag(name)`. `MachineProfileInit()` is called once in `main_unix.cpp` (~:1338 region) — **after** the `SS_TEST_HEX`/`SS_TEST_HEX_FILE` harness early path (~:1201), so in harness runs the profile is the uninitialized default (paravirtual). LEARNINGS: SS_TEST_HEX bypasses init — never use the opcode harness to verify init-time behavior.
- **Iteration-speed tooling (landed this session; tree at `10f1e1bd`):** `SS_HARNESS_BATCH=1 make test-jit` runs the full 350-vector gate in ONE emulator process per mode via `SS_TEST_HEX_FILE` (commit `311b0920`; `jit-test/run.sh:130–150`, binary path near `main_unix.cpp:1201` — anchors may drift a few lines from this commit) — use for the inner loop; **the legacy per-process `make test-jit` remains the authoritative gate** and must run at least once per task/commit (`SS_HARNESS_KEEP=1` preserves REGDUMPs). `ccache` is wired into the configure **per checkout** (`src/Unix/Makefile:16–17`) — **the M2 worktree must re-run configure with `CC="ccache gcc" CXX="ccache g++"`** (same flags as CLAUDE.md) or warm rebuilds lose the ~12x speedup. `SS_SEED_MEM` landed in commit `1fcdc297` (immediate + PC-triggered guest-memory pokes, `sheepshaver_glue.cpp`/`ppc-cpu.cpp`) — available for no-rebuild seeding hypotheses during Task 8 debugging; M2 code does not touch its files.
- **Build**: add sources to `SheepShaver/src/Unix/Makefile.in` SRCS (next to the existing `../machine/*.cpp` entries), then `cd SheepShaver/src/Unix && ./config.status Makefile`. Machine standalone tests: `make -C SheepShaver/src/machine test` (`-std=c++11`; **Task 2's test needs no flag change — c++11 suffices for the donor**).
- **rom-harness isolation**: `SheepShaver/rom-harness/Makefile` compiles `ppc-jit.cpp` standalone — M2 touches `ppc-execute.cpp` (NOT compiled by rom-harness) and machine modules only, so no `#ifdef` build-fencing is needed this milestone. Verify with the Task 7 rom-harness build gate anyway.
- **check_work DEC math (S3 §2.4)** — the consumer contract: `mfspr r30,22; subf r29,r29,r30` (deadline = DEC − timeout), then loop `mfspr r30,22; subf. r30,r29,r30; ble done` — i.e. exit when `(int32)(DEC_now − deadline) ≤ 0`. Works for any monotone down-counter including one that wraps; the unit test simulates exactly this sequence.
- **NK boot ceiling**: the fidelity diagnostic boot currently dies at the `0x50326050` MMU/SR wall (M3/M5 territory; LEARNINGS 2026-06-10). The `mdec_dat` sites live in ROM 0x310000–0x314000 (NK init), which executes **before** that wall — so observed `mtspr DEC` traffic is expected to be live-testable. If it is not, Task 8 records a carried-forward DoD exactly as M1 did for the SCC WR telemetry.

## File map

| File | Status | Responsibility |
|---|---|---|
| `SheepShaver/src/include/virt_clock.h` / `src/machine/virt_clock.cpp` / `test_virt_clock.cpp` | create | Virtual TB/DEC: ratio math, mttb/mtdec state, expiry latch + gen guard, telemetry |
| `SheepShaver/src/include/event_sched.h` / `src/machine/event_sched.cpp` / `test_event_sched.cpp` | create | DingusPPC TimerManager port: ns-deadline priority queue, one-shot/cyclic, `process_timers()` |
| `SheepShaver/src/include/mmio_bus.h` + `src/machine/mmio_bus.cpp` + `test_mmio_bus.cpp` | modify | Add `MMIOBusWithRegion(addr, fn, opaque)` (scheduler callbacks mutate device state under the region lock) |
| `SheepShaver/src/include/dev_via6522.h` + `src/machine/dev_via6522.cpp` + `test_dev_via6522.cpp` | modify | Timer state machine (IDLE/RUNNING/FIRED), N6/N7/N8 fixes, optional scheduler binding (eager IFR latch) |
| `SheepShaver/src/machine/Makefile` + `.gitignore` | modify | Two new standalone tests; `test_dev_via6522` gains `event_sched.cpp` in its link line |
| `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-execute.cpp` | modify | DEC mfspr/mtspr + TBL/TBU mtspr + mftb routed through the clock on the active gate; SS_SYNTH_DEC absorbed |
| `SheepShaver/src/Unix/Makefile.in` | modify | SRCS += `virt_clock.cpp event_sched.cpp` |
| `SheepShaver/src/Unix/main_unix.cpp` | modify | Clock bring-up (both normal AND harness paths), scheduler pump thread, VIA rebinding, DEC→scheduler arm hook, atexit telemetry |
| `SheepShaver/src/rom_patches.cpp` | modify | `mdec_dat` retirement on newworld (profile check at site, M1 `scc_init` idiom) |
| docs: `MACHINE-LAYER-PLAN.md`, `ROM-PATCH-AUDIT.md`, `M1-DEVICE-CONFORMANCE.md`, `CHANGELOG.md` (+ LEARNINGS if warranted) | modify | M2 bookkeeping |

**Parallelization** *(rev 2 — corrected)*: Tasks 1 and 2 are independent (different files except `src/machine/Makefile`/`.gitignore` — have each append its own recipe and resolve the trivial conflict at merge, or run them in one worktree back-to-back). **Task 3 runs after Tasks 1+2 merge**: its test `#include`s and links `event_sched.cpp`, and its Step 5 gate runs all eight binaries — it cannot build in parallel isolation. Tasks 4–6 are sequential (shared emulator files). Task 9 (docs) can draft in parallel with Task 8.

**Standing rules (every task):**
- **No new `powerpc_registers` fields.** All M2 state lives in the machine layer (the JIT hardcodes struct offsets; none needed).
- Any kpx_cpu header change ⇒ clean PPC recompile before trusting test-jit (stale-.o footgun). M2 plans to touch only `ppc-execute.cpp` (a .cpp) — if a header edit becomes necessary, STOP and apply the rule.
- §2g locking + **the M2 lock-order rule**: device/region lock → scheduler queue mutex is the ONLY permitted order (a device handler may arm/cancel timers while holding its region lock; `process_timers()` **never** holds the queue mutex while invoking a callback — preserved from the donor — so callbacks may take region locks).
- Iteration loop: `SS_HARNESS_BATCH=1 make test-jit` for the inner loop; legacy `make test-jit` is the authoritative pre-commit gate. If any single edit→gate cycle blocks on builds/tooling more than twice in a row, STOP and escalate to the user — don't grind.
- Commit per task, `type(machine): subject` style; cite gate results in the body.
- Emulator launches (Tasks 7 Step e2e, Task 8): **ask the user first** — e2e is the sanctioned exception but still coordinate; diagnostic boots always require an explicit go.

---

### Task 1: `virt_clock` module (pure, standalone-tested)

The clock is profile-agnostic state + math; *who* consults it (and when) is decided at the seams (Task 4). Cold state is backward-compatible by construction: before any guest `mtspr DEC`, `ReadDEC` returns `0 − TB` — bit-identical to M1's synthetic down-counter.

**Files:**
- Create: `SheepShaver/src/include/virt_clock.h`
- Create: `SheepShaver/src/machine/virt_clock.cpp`
- Create: `SheepShaver/src/machine/test_virt_clock.cpp`
- Modify: `SheepShaver/src/machine/Makefile` + `.gitignore`

- [ ] **Step 1: Write the header**

`SheepShaver/src/include/virt_clock.h`:

```cpp
/*
 *  virt_clock.h - Machine Layer M2 virtual clock (MACHINE-LAYER-PLAN.md §2c).
 *
 *  Guest-visible TB (timebase) and DEC (decrementer) backed by an injected host
 *  monotonic-ns source at a fixed ratio (tb_freq_hz = TimebaseSpeed). DEC expiry
 *  raises an exception CONDITION (latch + telemetry) only — delivery is M3.
 *  DEC is a CPU-internal exception (vector 0x900); it never passes through the
 *  PIC (§2c). Threading: WriteDEC/ReadDEC run on the CPU thread; the scheduler
 *  pump thread may call VirtClockDECExpire() — generation-guarded, atomics only.
 *
 *  M2 scope note (rev 2 finding S7): the condition is modeled only as
 *  count-through-zero after an mtspr DEC write (fires at v+1 ticks; for an
 *  MSB-set v this matches the OEA 0->1 MSB transition). Two OEA condition
 *  sources are deliberately unmodeled until M3 owns delivery: (a) mtspr DEC
 *  itself writing an MSB-set value over an MSB-clear one signals immediately on
 *  real hardware; (b) the cold free-running counter wraps through the MSB
 *  transition every 2^32 ticks. No M2 consumer reads the condition.
 */

#ifndef VIRT_CLOCK_H
#define VIRT_CLOCK_H

#include <stdint.h>
#include <stdio.h>

struct VirtClock {
	uint64_t (*now_ns)(void *opaque);   // injected host monotonic nanoseconds
	void    *opaque;
	uint32_t tb_freq_hz;                // TB/DEC tick rate (TimebaseSpeed)
	int64_t  tb_offset;                 // guest mttbl/mttbu adjustment (tb units)

	// DEC state (CPU-thread-owned)
	uint32_t dec_set_value;             // last mtspr DEC value (0 = cold: free-run from 0)
	uint64_t dec_set_tb;                // TB at that write (0 = cold)

	// Expiry condition (cross-thread: atomics only).
	// (rev 2 finding C1) generation and armed-flag are FUSED into one word so the
	// "is this event current" check and the "consume the arm" step are a single CAS
	// - a separate load-then-CAS lets a stale scheduler event steal a fresh arm
	// (spurious pending + permanently lost expiry). dec_arm_word = (gen << 1) | armed.
	uint64_t dec_arm_word;
	uint32_t dec_pending;               // the M2 deliverable: the exception CONDITION latch

	// Eager-expiry hook (wired to the event scheduler by main_unix; may be NULL).
	// Called from VirtClockWriteDEC with the ns-until-expiry and the new generation.
	void (*on_dec_write)(void *cb_opaque, uint64_t ns_until_expiry, uint32_t gen);
	void *cb_opaque;

	// Telemetry (DoD observed-traffic asserts; dumped at exit)
	uint64_t mfspr_dec_reads, mtspr_dec_writes, tb_writes, dec_expiries;
};

extern void     VirtClockInit(VirtClock *c, uint32_t tb_freq_hz,
                              uint64_t (*now_ns)(void *), void *opaque);
static inline bool VirtClockReady(const VirtClock *c) { return c->now_ns != 0; }

extern uint64_t VirtClockNowNS(VirtClock *c);   // raw injected source (devices derive ticks)
extern uint64_t VirtClockTB(VirtClock *c);      // 64-bit TB = ns*freq/1e9 + tb_offset
extern uint32_t VirtClockReadDEC(VirtClock *c); // also performs the lazy expiry check
extern void     VirtClockWriteDEC(VirtClock *c, uint32_t v);
extern void     VirtClockWriteTBL(VirtClock *c, uint32_t v);
extern void     VirtClockWriteTBU(VirtClock *c, uint32_t v);

// Scheduler-side expiry (pump thread). Latches dec_pending iff gen is current
// and the arm is still outstanding. Safe from any thread.
extern void     VirtClockDECExpire(VirtClock *c, uint32_t gen);

// M3 will consume these; M2 only reports them.
extern bool     VirtClockDECPending(const VirtClock *c);
extern void     VirtClockClearDECPending(VirtClock *c);

extern void     VirtClockDumpStats(const VirtClock *c, FILE *f);

// The emulator's single instance (defined in virt_clock.cpp; init'd by main_unix).
extern VirtClock g_virt_clock;

#endif
```

- [ ] **Step 2: Write the failing test**

`SheepShaver/src/machine/test_virt_clock.cpp`:

```cpp
/* Standalone unit test for virt_clock.cpp. Simulates the check_work DEC deadline
 * math (SPIKE-S3 §2.4) as the consumer contract. Build: make -C SheepShaver/src/machine test */
#include "virt_clock.h"
#include <assert.h>
#include <string.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

static uint64_t fake_ns = 0;
static uint64_t fake_now(void *) { return fake_ns; }

static uint64_t armed_ns; static uint32_t armed_gen; static int arm_calls = 0;
static void fake_arm(void *, uint64_t ns, uint32_t gen) { armed_ns = ns; armed_gen = gen; arm_calls++; }

int main()
{
	VirtClock c;
	memset(&c, 0xAA, sizeof(c));            // poison: Init must fully reset
	VirtClockInit(&c, 25000000u, fake_now, 0);   // 25 MHz TB (TimebaseSpeed default)
	CHECK(VirtClockReady(&c));

	// --- TB ratio math: 1 second of ns = 25e6 ticks; large values don't overflow ---
	fake_ns = 1000000000ull;                              CHECK(VirtClockTB(&c) == 25000000ull);
	fake_ns = 1000000000000000ull;  /* ~11.6 days */      CHECK(VirtClockTB(&c) == 25000000000000ull);
	fake_ns = 0;

	// --- Cold-state DEC == M1 synthetic down-counter: 0 - TB ---
	fake_ns = 40;                            // 40 ns @ 25 MHz = 1 tick
	CHECK(VirtClockReadDEC(&c) == 0xFFFFFFFFu);           // 0 - 1
	fake_ns = 4000;                          // 100 ticks
	CHECK(VirtClockReadDEC(&c) == (uint32_t)(0u - 100u));
	CHECK(c.mfspr_dec_reads == 2);

	// --- mtspr DEC: countdown from the written value ---
	VirtClockWriteDEC(&c, 1000);
	CHECK(c.mtspr_dec_writes == 1);
	CHECK(arm_calls == 0);                   // no hook wired yet -> no arm
	CHECK(VirtClockReadDEC(&c) == 1000);
	fake_ns += 400 * 40;                     // +400 ticks
	CHECK(VirtClockReadDEC(&c) == 600);
	// Expiry: condition latches when the counter crosses below zero (elapsed > value)
	CHECK(!VirtClockDECPending(&c));
	fake_ns += 601 * 40;                     // elapsed = 1001 > 1000
	uint32_t d = VirtClockReadDEC(&c);
	CHECK(d == 0xFFFFFFFFu);                 // wrapped: 1000 - 1001
	CHECK(VirtClockDECPending(&c));
	CHECK(c.dec_expiries == 1);
	// One-shot per write: further reads do not re-latch / re-count
	fake_ns += 40;
	(void)VirtClockReadDEC(&c);
	CHECK(c.dec_expiries == 1);
	VirtClockClearDECPending(&c);
	CHECK(!VirtClockDECPending(&c));

	// --- Eager hook + generation guard ---
	c.on_dec_write = fake_arm; c.cb_opaque = 0;
	VirtClockWriteDEC(&c, 250);              // 250 ticks @25MHz = 10000 ns
	CHECK(arm_calls == 1 && armed_ns == (uint64_t)(250 + 1) * 40);   // expiry at v+1 ticks
	uint32_t gen1 = armed_gen;
	VirtClockWriteDEC(&c, 500);              // re-write: new generation
	CHECK(arm_calls == 2 && armed_gen == gen1 + 1);
	VirtClockDECExpire(&c, gen1);            // stale event: must no-op
	CHECK(!VirtClockDECPending(&c) && c.dec_expiries == 1);
	VirtClockDECExpire(&c, armed_gen);       // current event: latches
	CHECK(VirtClockDECPending(&c) && c.dec_expiries == 2);
	VirtClockDECExpire(&c, armed_gen);       // double-fire: no-op (arm consumed)
	CHECK(c.dec_expiries == 2);
	VirtClockClearDECPending(&c);

	// --- mttbl/mttbu: TB continues from the written value ---
	fake_ns = 1000000000ull;                 // raw TB = 25e6
	VirtClockWriteTBU(&c, 0);
	VirtClockWriteTBL(&c, 0x1000);           // guest sets TB = 0x1000
	CHECK(VirtClockTB(&c) == 0x1000);
	fake_ns += 40;                           // +1 tick
	CHECK(VirtClockTB(&c) == 0x1001);
	VirtClockWriteTBU(&c, 2);                // TBU write preserves current TBL
	CHECK((VirtClockTB(&c) >> 32) == 2 && (uint32_t)VirtClockTB(&c) == 0x1001);
	CHECK(c.tb_writes == 3);

	// --- The S3 §2.4 check_work consumer simulation (deadline = DEC - timeout) ---
	VirtClockWriteDEC(&c, 0);                // worst case: DEC just wrapped/cold-like
	uint32_t timeout = 0x8000;               // [KDP-0x438] >> 8 scale, representative
	uint32_t deadline = VirtClockReadDEC(&c) - timeout;
	int iters = 0;
	for (;;) {
		uint32_t now = VirtClockReadDEC(&c);
		if ((int32_t)(now - deadline) <= 0) break;   // subf. + ble
		fake_ns += 40 * 1024;                // 1024 ticks per "loop pass"
		if (++iters > 100000) break;
	}
	CHECK(iters > 0 && iters <= (int)(timeout / 1024) + 2);   // terminated by deadline, not the guard

	VirtClockDumpStats(&c, stdout);
	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
```

- [ ] **Step 3: Add the Makefile recipe, run, verify FAIL**

Append to `SheepShaver/src/machine/Makefile` (mirror the existing recipes; add `./test_virt_clock` to the `test:` target and the binary to `.gitignore`):

```makefile
test_virt_clock: test_virt_clock.cpp virt_clock.cpp ../include/virt_clock.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_virt_clock.cpp virt_clock.cpp
```

Run: `make -C SheepShaver/src/machine test_virt_clock` — Expected: FAIL (virt_clock.cpp missing).

- [ ] **Step 4: Implement `virt_clock.cpp`**

```cpp
/*
 *  virt_clock.cpp - Machine Layer M2 virtual clock (see virt_clock.h / MACHINE-LAYER-PLAN §2c).
 *
 *  Threading: dec_set_value/dec_set_tb/tb_offset are CPU-thread-owned (all guest
 *  SPR traffic runs on the CPU thread via the interpreter slow path). The
 *  gen/armed/pending trio crosses to the scheduler pump thread -> atomics.
 *  Ratio math uses unsigned __int128: ns up to host-uptime scale (~1e14) times
 *  tb_freq (25e6) overflows uint64.
 */

#include "virt_clock.h"
#include <string.h>

VirtClock g_virt_clock;   // zero-initialized: now_ns==NULL -> VirtClockReady()==false

void VirtClockInit(VirtClock *c, uint32_t tb_freq_hz, uint64_t (*now_ns)(void *), void *opaque)
{
	memset(c, 0, sizeof(*c));
	c->tb_freq_hz = tb_freq_hz ? tb_freq_hz : 25000000u;
	c->now_ns = now_ns;
	c->opaque = opaque;
}

uint64_t VirtClockNowNS(VirtClock *c)
{
	return c->now_ns ? c->now_ns(c->opaque) : 0;
}

static inline uint64_t ns_to_tb(const VirtClock *c, uint64_t ns)
{
	return (uint64_t)(((unsigned __int128)ns * c->tb_freq_hz) / 1000000000u);
}

static inline uint64_t tb_to_ns(const VirtClock *c, uint64_t tb)
{
	return (uint64_t)(((unsigned __int128)tb * 1000000000u) / c->tb_freq_hz);
}

uint64_t VirtClockTB(VirtClock *c)
{
	return ns_to_tb(c, VirtClockNowNS(c)) + (uint64_t)c->tb_offset;
}

/* Consume the outstanding arm and latch the condition. (rev 2 finding C1)
 * The generation check and the arm-consume are ONE compare-exchange on the
 * fused word - a stale event can never steal a fresh generation's arm. */
static void dec_fire(VirtClock *c, uint32_t gen)
{
	uint64_t expected = ((uint64_t)gen << 1) | 1;          // this gen, still armed
	if (!__atomic_compare_exchange_n(&c->dec_arm_word, &expected,
	                                 (uint64_t)gen << 1,    // same gen, disarmed
	                                 false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
		return;                                   // stale gen, or already fired
	__atomic_store_n(&c->dec_pending, 1, __ATOMIC_RELEASE);
	__atomic_fetch_add(&c->dec_expiries, 1, __ATOMIC_RELAXED);  // (rev 2 finding S5)
}

uint32_t VirtClockReadDEC(VirtClock *c)
{
	c->mfspr_dec_reads++;                          // CPU-thread-only counter
	uint64_t tb = VirtClockTB(c);
	uint64_t elapsed = tb - c->dec_set_tb;
	/* Lazy expiry check (works without a scheduler): the counter crossed below
	 * zero once elapsed exceeds the written value (expiry at v+1 ticks). */
	uint64_t w = __atomic_load_n(&c->dec_arm_word, __ATOMIC_ACQUIRE);
	if ((w & 1) && elapsed > c->dec_set_value)
		dec_fire(c, (uint32_t)(w >> 1));
	return c->dec_set_value - (uint32_t)elapsed;  // uint32 wrap = free-run past zero
}

void VirtClockWriteDEC(VirtClock *c, uint32_t v)
{
	c->mtspr_dec_writes++;                         // CPU-thread-only counter
	c->dec_set_tb = VirtClockTB(c);
	c->dec_set_value = v;
	uint32_t gen = (uint32_t)(__atomic_load_n(&c->dec_arm_word, __ATOMIC_RELAXED) >> 1) + 1;
	__atomic_store_n(&c->dec_arm_word, ((uint64_t)gen << 1) | 1, __ATOMIC_RELEASE);
	if (c->on_dec_write)
		c->on_dec_write(c->cb_opaque, tb_to_ns(c, (uint64_t)v + 1), gen);
}

void VirtClockWriteTBL(VirtClock *c, uint32_t v)
{
	c->tb_writes++;
	uint64_t cur = VirtClockTB(c);
	uint64_t want = (cur & 0xFFFFFFFF00000000ull) | v;
	c->tb_offset += (int64_t)(want - cur);
}

void VirtClockWriteTBU(VirtClock *c, uint32_t v)
{
	c->tb_writes++;
	uint64_t cur = VirtClockTB(c);
	uint64_t want = ((uint64_t)v << 32) | (uint32_t)cur;
	c->tb_offset += (int64_t)(want - cur);
}

void VirtClockDECExpire(VirtClock *c, uint32_t gen) { dec_fire(c, gen); }

bool VirtClockDECPending(const VirtClock *c)
{
	return __atomic_load_n(&c->dec_pending, __ATOMIC_ACQUIRE) != 0;
}

void VirtClockClearDECPending(VirtClock *c)
{
	__atomic_store_n(&c->dec_pending, 0, __ATOMIC_RELEASE);
}

void VirtClockDumpStats(const VirtClock *c, FILE *f)
{
	fprintf(f, "[VCLK] tb_freq=%uHz mfspr_dec=%llu mtspr_dec=%llu tb_writes=%llu "
	        "dec_expiries=%llu pending=%u\n",
	        c->tb_freq_hz,
	        (unsigned long long)c->mfspr_dec_reads,
	        (unsigned long long)c->mtspr_dec_writes,
	        (unsigned long long)c->tb_writes,
	        (unsigned long long)c->dec_expiries,
	        (unsigned)c->dec_pending);
}
```

- [ ] **Step 5: Run `make -C SheepShaver/src/machine test_virt_clock && SheepShaver/src/machine/test_virt_clock` — Expected: `RESULT: ALL PASS`**

- [ ] **Step 6: Commit**

```bash
git add SheepShaver/src/include/virt_clock.h SheepShaver/src/machine/virt_clock.cpp \
        SheepShaver/src/machine/test_virt_clock.cpp SheepShaver/src/machine/Makefile \
        SheepShaver/src/machine/.gitignore
git commit -m "feat(machine): M2 virtual clock - guest TB/DEC state, expiry condition latch, check_work consumer test"
```

---

### Task 2: `event_sched` — DingusPPC TimerManager port (pure, standalone-tested)

**Backport hygiene (memory rule + MACHINE-LAYER-PLAN §4):** this is a port of DingusPPC
`core/timermanager.cpp` + `core/timermanager.h` at commit
`92bb6d10549529f9f4031a85c2bc136149535bdc` (https://github.com/dingusdev/dingusppc),
GPL-3.0-or-later — combining with our GPLv2-or-later tree yields a combined GPLv3 work
(resolved in `DINGUSPPC-EVALUATION-PLAN.md`). **Keep their copyright header**, add an
adaptation note, cite file + SHA in the code comment AND in the CHANGELOG entry (Task 9).
Never PR anything back to DingusPPC. Adaptations (each marked in-line): class renamed
`TimerManager` → `EventScheduler`; singleton `get_instance()` removed (instance owned by
the machine layer); `loguru` dependency replaced with `fprintf(stderr, ...)` on the
`cancel_all` warning path only. **All queue/ordering/re-arm/callback logic is preserved
verbatim** — including the donor's contract that `process_timers()` never holds the queue
mutex while invoking a callback (this is what makes the M2 lock-order rule sound).

**Files:**
- Create: `SheepShaver/src/include/event_sched.h`
- Create: `SheepShaver/src/machine/event_sched.cpp`
- Create: `SheepShaver/src/machine/test_event_sched.cpp`
- Modify: `SheepShaver/src/machine/Makefile` + `.gitignore`

- [ ] **Step 1: Write the header**

`SheepShaver/src/include/event_sched.h` — the donor header with the marked adaptations:

```cpp
/*
 *  event_sched.h - Machine Layer M2 host event scheduler (MACHINE-LAYER-PLAN §2c).
 *
 *  Ported from DingusPPC core/timermanager.h
 *  (https://github.com/dingusdev/dingusppc @ 92bb6d10549529f9f4031a85c2bc136149535bdc),
 *  GPL-3.0-or-later. Combined work is GPLv3 (DINGUSPPC-EVALUATION-PLAN.md).
 *  Adaptations for SheepShaver (marked [SS] below): class renamed TimerManager ->
 *  EventScheduler; singleton get_instance() removed (the machine layer owns the
 *  instance); loguru removed. Queue, ordering, re-arm and callback semantics are
 *  preserved verbatim. Time unit: nanoseconds from the injected source.
 *
 *  DingusPPC - The Experimental PowerPC Macintosh emulator
 *  Copyright (C) 2018-26 The DingusPPC Development Team (see their CREDITS.MD)
 *  This program is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free Software
 *  Foundation, either version 3 of the License, or (at your option) any later
 *  version. Distributed WITHOUT ANY WARRANTY; see <https://www.gnu.org/licenses/>.
 */

#ifndef EVENT_SCHED_H
#define EVENT_SCHED_H

#include <atomic>
#include <algorithm>
#include <cinttypes>
#include <functional>
#include <memory>
#include <queue>
#include <vector>
#include <mutex>

#define USECS_TO_NSECS(us) (us) * 1000
#define MSECS_TO_NSECS(ms) (ms) * 1000000

typedef std::function<void()> timer_cb;
typedef std::function<void()> notify_changes_cb;

/** Extend std::priority_queue as suggested here:
    https://stackoverflow.com/a/36711682
    to be able to remove arbitrary elements.  (verbatim from the donor) */
template <typename T, class Container = std::vector<T>, class Compare = std::less<typename Container::value_type>>
class my_priority_queue : public std::priority_queue<T, Container, Compare> {
public:
    bool remove_by_id(const uint32_t id) {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (this->empty())
            return false;
        auto el = this->top();
        if (el->id == id) {
            std::priority_queue<T, Container, Compare>::pop();
            return true;
        }
        auto it = std::find_if(
            this->c.begin(), this->c.end(), [id](const T& el) { return el->id == id; });
        if (it != this->c.end()) {
            this->c.erase(it);
            std::make_heap(this->c.begin(), this->c.end(), this->comp);
            return true;
        }
        return false;
    }

    void push(T val) {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        std::priority_queue<T, Container, Compare>::push(val);
    }

    T pop() {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        T val = std::priority_queue<T, Container, Compare>::top();
        std::priority_queue<T, Container, Compare>::pop();
        return val;
    }

    std::recursive_mutex& get_mtx() { return mtx; }

private:
    std::recursive_mutex mtx;
};

typedef struct TimerInfo {
    uint32_t id;
    uint64_t timeout_ns;  // timer expiry
    uint64_t interval_ns; // 0 for one-shot timers
    timer_cb cb;          // timer callback
} TimerInfo;

// Custom comparator for sorting our timer queue in ascending order (verbatim)
class MyGtComparator {
public:
    bool operator()(const std::shared_ptr<TimerInfo>& l, const std::shared_ptr<TimerInfo>& r) {
        return l.get()->timeout_ns > r.get()->timeout_ns ||
            (l.get()->timeout_ns == r.get()->timeout_ns && l.get()->id > r.get()->id);
    }
};

class EventScheduler {   // [SS] was TimerManager; singleton removed
public:
    EventScheduler() {}

    // callback for retrieving current time
    void set_time_now_cb(const std::function<uint64_t()> &cb) { this->get_time_now = cb; }

    // callback for acknowledging changes in the timer queue (pump wake-up)
    void set_notify_changes_cb(const notify_changes_cb &cb) { this->notify_timer_changes = cb; }

    // return current virtual time in nanoseconds
    uint64_t current_time_ns() { return get_time_now(); }

    // creating and cancelling timers
    uint32_t add_absolute_timer(uint64_t timeout_ns, uint64_t interval, timer_cb cb);
    uint32_t add_oneshot_timer(uint64_t timeout, timer_cb cb);
    uint32_t add_immediate_timer(timer_cb cb);
    uint32_t add_cyclic_timer(uint64_t interval, timer_cb cb);
    uint32_t add_cyclic_timer(uint64_t interval, uint64_t delay, timer_cb cb);
    void cancel_timer(uint32_t id);
    void cancel_all_timers();

    // Fire all expired timers; return ns until the next expiry (0 = queue empty).
    // Contract (donor-preserved): the queue mutex is NEVER held while a callback
    // runs -> callbacks may take device/region locks (M2 lock-order rule).
    // Donor quirk (rev 2 finding S10, preserved): cancel_timer() does NOT
    // guarantee the callback won't run one last time (cancel can race the
    // top-of-queue snapshot here). Consumers must be generation-guarded - the
    // VIA and DEC consumers are; future consumers must not trust cancel alone.
    uint64_t process_timers();

private:
    my_priority_queue<std::shared_ptr<TimerInfo>, std::vector<std::shared_ptr<TimerInfo>>, MyGtComparator> timer_queue;

    std::function<uint64_t()>   get_time_now;
    std::function<void()>       notify_timer_changes;

    std::atomic<uint32_t> id{0};

    // FIXME: Do we need this? It gets written in main thread and read in audio thread.
    // [SS note, rev 2 finding S6: donor FIXME kept verbatim. Here it is written by
    // the pump thread and read by the CPU thread; the worst case is a skipped
    // notify (missed kick), bounded by the pump's 10ms cap. Not a correctness gate.]
    bool cb_active = false; // true if a timer callback is executing
};

#endif // EVENT_SCHED_H
```

- [ ] **Step 2: Write the failing test**

`SheepShaver/src/machine/test_event_sched.cpp`:

```cpp
/* Standalone unit test for the DingusPPC TimerManager port. Fake time source;
 * asserts ordering, one-shot/cyclic semantics, drift correction, cancel, and the
 * no-lock-during-callback contract (cancel/add from inside a callback). */
#include "event_sched.h"
#include <assert.h>
#include <stdio.h>
#include <vector>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

static uint64_t fake_ns = 0;

int main()
{
	EventScheduler es;
	es.set_time_now_cb([]() { return fake_ns; });
	int notifies = 0;
	es.set_notify_changes_cb([&]() { notifies++; });

	// Empty queue: slice 0.
	CHECK(es.process_timers() == 0);

	// --- Ordering + one-shot fires exactly once ---
	std::vector<int> fired;
	es.add_oneshot_timer(300, [&]() { fired.push_back(3); });
	es.add_oneshot_timer(100, [&]() { fired.push_back(1); });
	es.add_oneshot_timer(200, [&]() { fired.push_back(2); });
	CHECK(notifies == 3);
	uint64_t slice = es.process_timers();
	CHECK(fired.empty() && slice == 100);     // nothing expired; next expiry in 100ns
	fake_ns = 250;
	slice = es.process_timers();
	CHECK(fired.size() == 2 && fired[0] == 1 && fired[1] == 2);
	CHECK(slice == 50);                       // 300 - 250
	fake_ns = 300;
	CHECK(es.process_timers() == 0);          // last one fired; queue empty
	CHECK(fired.size() == 3 && fired[2] == 3);
	fake_ns = 400;
	es.process_timers();
	CHECK(fired.size() == 3);                 // one-shots never re-fire

	// --- Cyclic timer with drift correction (donor: timeout_ns_new <= now -> now+interval) ---
	fake_ns = 0;
	int cyc = 0;
	uint32_t cid = es.add_cyclic_timer(100, [&]() { cyc++; });
	fake_ns = 100; es.process_timers(); CHECK(cyc == 1);
	fake_ns = 200; es.process_timers(); CHECK(cyc == 2);
	fake_ns = 550; es.process_timers();       // missed beats: fires once, re-arms at now+interval
	CHECK(cyc == 3);
	uint64_t s2 = es.process_timers();
	CHECK(s2 == 100);                          // next at 650, not at 400
	es.cancel_timer(cid);
	fake_ns = 1000; es.process_timers(); CHECK(cyc == 3);

	// --- cancel by id: middle FIRST (rev 2 finding S8: cancelling the head first
	// would promote b to head and never exercise remove_by_id's erase/make_heap path) ---
	fired.clear();
	uint32_t a = es.add_oneshot_timer(100, [&]() { fired.push_back(10); });
	uint32_t b = es.add_oneshot_timer(200, [&]() { fired.push_back(20); });
	uint32_t cd = es.add_oneshot_timer(300, [&]() { fired.push_back(30); });
	(void)cd;
	es.cancel_timer(b);                        // middle while a is head (erase + make_heap)
	es.cancel_timer(a);                        // head (top-pop branch)
	fake_ns += 1000; es.process_timers();
	CHECK(fired.size() == 1 && fired[0] == 30);

	// --- add/cancel from INSIDE a callback (recursive_mutex + no-lock-during-cb) ---
	fired.clear();
	es.add_oneshot_timer(10, [&]() {
		es.add_oneshot_timer(5, [&]() { fired.push_back(99); });
	});
	fake_ns += 10; es.process_timers();        // outer fires, schedules inner at +5
	fake_ns += 5;  es.process_timers();
	CHECK(fired.size() == 1 && fired[0] == 99);

	// --- immediate timer fires on the next process_timers ---
	int imm = 0;
	es.add_immediate_timer([&]() { imm++; });
	es.process_timers();
	CHECK(imm == 1);

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
```

- [ ] **Step 3: Add Makefile recipe (sources `test_event_sched.cpp event_sched.cpp`), run, verify FAIL**

- [ ] **Step 4: Implement `event_sched.cpp`** — the donor .cpp with the marked adaptations:

```cpp
/*
 *  event_sched.cpp - host event scheduler (see event_sched.h for provenance).
 *  Ported from DingusPPC core/timermanager.cpp
 *  (https://github.com/dingusdev/dingusppc @ 92bb6d10549529f9f4031a85c2bc136149535bdc),
 *  GPL-3.0-or-later. [SS] adaptations: loguru -> fprintf(stderr) in cancel_all_timers;
 *  TimerManager -> EventScheduler; no singleton. Logic otherwise verbatim.
 */

#include "event_sched.h"

#include <cinttypes>
#include <memory>
#include <mutex>
#include <stdio.h>

uint32_t EventScheduler::add_absolute_timer(uint64_t timeout_ns, uint64_t interval, timer_cb cb)
{
    TimerInfo* ti = new TimerInfo;

    ti->id          = ++this->id;
    ti->timeout_ns  = timeout_ns;
    ti->interval_ns = interval;
    ti->cb          = cb;

    std::shared_ptr<TimerInfo> timer_desc(ti);

    // add new timer to the timer queue
    this->timer_queue.push(timer_desc);

    // notify listeners about changes in the timer queue
    if (!this->cb_active) {
        this->notify_timer_changes();
    }

    return ti->id;
}

uint32_t EventScheduler::add_oneshot_timer(uint64_t timeout, timer_cb cb)
{
    return EventScheduler::add_absolute_timer(this->get_time_now() + timeout, 0, cb);
}

uint32_t EventScheduler::add_immediate_timer(timer_cb cb)
{
    return EventScheduler::add_absolute_timer(0, 0, cb);
}

uint32_t EventScheduler::add_cyclic_timer(uint64_t interval, uint64_t delay, timer_cb cb)
{
    return EventScheduler::add_absolute_timer(this->get_time_now() + delay, interval, cb);
}

uint32_t EventScheduler::add_cyclic_timer(uint64_t interval, timer_cb cb)
{
    return this->add_cyclic_timer(interval, interval, cb);
}

void EventScheduler::cancel_timer(uint32_t id)
{
    this->timer_queue.remove_by_id(id);
    if (!this->cb_active) {
        this->notify_timer_changes();
    }
}

uint64_t EventScheduler::process_timers()
{
    std::shared_ptr<TimerInfo> cur_timer;
    uint64_t time_now = get_time_now();

{ // [ mtx scope
    std::lock_guard<std::recursive_mutex> lk(this->timer_queue.get_mtx());
    if (this->timer_queue.empty()) {
        return 0ULL;
    }

    // scan for expired timers
    cur_timer = this->timer_queue.top();
} // ] mtx scope
    while (cur_timer->timeout_ns <= time_now) {
        this->timer_queue.remove_by_id(cur_timer->id);
        uint64_t timeout_ns = cur_timer->timeout_ns;
        timer_cb cb = cur_timer->cb;

        // re-arm cyclic timers
        if (cur_timer->interval_ns) {
            std::lock_guard<std::recursive_mutex> lk(this->timer_queue.get_mtx());
            uint64_t timeout_ns_new = timeout_ns + cur_timer->interval_ns;
            if (timeout_ns_new <= time_now)
                timeout_ns_new = time_now + cur_timer->interval_ns;
            cur_timer->timeout_ns = timeout_ns_new;
            this->timer_queue.push(cur_timer);
        }

        this->cb_active = true;

        // invoke timer callback (queue mutex NOT held — M2 lock-order rule)
        cb();

        this->cb_active = false;

        // process next timer
{ // [ mtx scope
        std::lock_guard<std::recursive_mutex> lk(this->timer_queue.get_mtx());
        if (this->timer_queue.empty()) {
            return 0ULL;
        }

        cur_timer = this->timer_queue.top();
} // ] mtx scope
    }

    // return time slice in nanoseconds until next timer's expiry
    return cur_timer->timeout_ns - time_now;
}

void EventScheduler::cancel_all_timers()
{
    std::shared_ptr<TimerInfo> cur_timer;
    while (!this->timer_queue.empty()) {
        cur_timer = this->timer_queue.top();
        // [SS] was LOG_F(WARNING, ...)
        fprintf(stderr, "[ESCHED] Canceling timer id:%u ns:%llu\n",
                cur_timer->id, (unsigned long long)cur_timer->timeout_ns);
        this->timer_queue.pop();
    }
}
```

- [ ] **Step 5: Run `make -C SheepShaver/src/machine test_event_sched && SheepShaver/src/machine/test_event_sched` — Expected: `RESULT: ALL PASS`**

- [ ] **Step 6: Commit** — body must cite the donor file + SHA:

```bash
git add SheepShaver/src/include/event_sched.h SheepShaver/src/machine/event_sched.cpp \
        SheepShaver/src/machine/test_event_sched.cpp SheepShaver/src/machine/Makefile \
        SheepShaver/src/machine/.gitignore
git commit -F- <<'EOF'
feat(machine): M2 host event scheduler - DingusPPC TimerManager port

Ported from DingusPPC core/timermanager.{cpp,h} at commit
92bb6d10549529f9f4031a85c2bc136149535bdc (github.com/dingusdev/dingusppc),
GPL-3.0-or-later; combined work GPLv3 per DINGUSPPC-EVALUATION-PLAN.md.
Adaptations: TimerManager->EventScheduler, singleton removed, loguru->stderr.
Queue/re-arm/callback semantics verbatim (callbacks run without the queue
mutex held - the M2 lock-order rule depends on this donor contract).
EOF
```

---

### Task 3: VIA 6522 timer rework — clock/scheduler binding + N6/N7/N8 (pure, standalone-tested)

> **Sequencing (rev 2):** runs AFTER Tasks 1+2 are merged — the test links `event_sched.cpp`
> and the Step 5 gate runs all eight binaries.

Replaces the lazy expiry-on-read model with an explicit per-timer state machine
(IDLE → RUNNING → FIRED) whose RUNNING→FIRED transition latches the IFR bit **once** —
performed eagerly by a scheduler event when bound, and lazily by a read-side poll as the
backstop (idempotent; same transition). This resolves:
- **N7**: expiry at hardware-correct **N+1** ticks (`dt > cnt`, was `dt >= cnt`).
- **N6**: T2C-L read clears IFR.T2 (6522 datasheet acknowledge path).
- **N8**: IFR write-1-clear clears the flag **only** — it no longer stops/disarms the
  timer; re-assert is impossible anyway because FIRED is a consumed one-shot state.

**Files:**
- Modify: `SheepShaver/src/include/dev_via6522.h`
- Modify: `SheepShaver/src/machine/dev_via6522.cpp`
- Modify: `SheepShaver/src/machine/test_dev_via6522.cpp`
- Modify: `SheepShaver/src/include/mmio_bus.h` + `src/machine/mmio_bus.cpp` + `test_mmio_bus.cpp` (the `MMIOBusWithRegion` API)
- Modify: `SheepShaver/src/machine/Makefile` (`test_dev_via6522` link line gains `event_sched.cpp mmio_bus.cpp`)

- [ ] **Step 1: Add `MMIOBusWithRegion` to the bus (test-first)**

Append to `test_mmio_bus.cpp` (before the final printf; reuse the registered `scc` region):

```cpp
	// M2: run a callback under the owning region's lock (scheduler callbacks).
	static uint32_t with_region_called;
	struct WR { static void fn(void *op) { with_region_called = *(uint32_t *)op + 1; } };
	uint32_t token = 41;
	CHECK(MMIOBusWithRegion(0xF3012002, WR::fn, &token));
	CHECK(with_region_called == 42);
	CHECK(!MMIOBusWithRegion(0xF2000000, WR::fn, &token));   // no owning region
```

Declare in `mmio_bus.h` (next to `MMIOBusLookup`):

```cpp
// Run fn(opaque) while holding the lock of the region owning addr (M2: scheduler
// callbacks mutating device state - MACHINE-LAYER-PLAN §2g rule 1). Returns false
// (fn not called) if no region owns addr. Lock-order rule: fn may take the
// scheduler queue mutex (device -> queue), never the reverse.
extern bool MMIOBusWithRegion(uint32_t addr, void (*fn)(void *), void *opaque);
```

Implement in `mmio_bus.cpp` (next to `MMIOBusWrite`):

```cpp
bool MMIOBusWithRegion(uint32_t addr, void (*fn)(void *), void *opaque)
{
	int i = MMIOBusLookup(addr);
	if (i < 0) return false;
	pthread_mutex_lock(&regions[i].lock);
	fn(opaque);
	pthread_mutex_unlock(&regions[i].lock);
	return true;
}
```

Run `make -C SheepShaver/src/machine test_mmio_bus && SheepShaver/src/machine/test_mmio_bus` — ALL PASS.

- [ ] **Step 2: Rework the VIA header**

Replace the timer fields + add the binding API in `dev_via6522.h` (full new struct + decls — the rest of the header is unchanged):

```cpp
/* (header comment: append) M2: timers are an explicit state machine driven by the
 * virtual clock; expiry latches IFR once (eagerly via a bound EventScheduler, or
 * lazily on the next register read). Conformance notes N6/N7/N8 resolved here. */

#include "mmio_bus.h"

#define VIA_CLOCK_HZ 783360u

enum VIATimerState { VIA_TIMER_IDLE = 0, VIA_TIMER_RUNNING = 1, VIA_TIMER_FIRED = 2 };

class EventScheduler;   // event_sched.h not required by pure users

struct VIA6522 {
	uint32_t base;
	uint64_t (*now_ticks)(void *clock_opaque);   // monotonic VIA ticks (783360 Hz)
	void *clock_opaque;
	uint8_t  ora, orb, ddra, ddrb, acr, pcr, sr, ier, ifr_latched;
	uint8_t  t1l_l, t1l_h, t2l_l;
	uint64_t t1_load_time, t2_load_time;         // now_ticks at counter load
	uint16_t t1_count, t2_count;                 // programmed counts
	uint8_t  t1_state, t2_state;                 // VIATimerState
	uint32_t t1_gen, t2_gen;                     // arm generation (stale events no-op)
	// Scheduler binding (optional; NULL = lazy-only, e.g. unit tests without sched)
	EventScheduler *sched;
	bool (*locked_call)(uint32_t addr, void (*fn)(void *), void *opaque); // prod: MMIOBusWithRegion
	uint64_t cuda_touches;
	bool     cuda_warned;
	const char *cuda_warn_what;
};

extern void VIAReset(VIA6522 *v, uint32_t base,
                     uint64_t (*now_ticks)(void *), void *clock_opaque);
// Bind the scheduler AFTER VIAReset (and after the bus region exists, in prod).
// locked_call runs the expiry transition under the device's lock; tests may pass
// a direct-call shim. ticks->ns conversion is internal (VIA_CLOCK_HZ).
extern void VIABindScheduler(VIA6522 *v, EventScheduler *sched,
                             bool (*locked_call)(uint32_t addr, void (*fn)(void *), void *opaque));
extern uint64_t VIARead(void *opaque, uint32_t addr, unsigned size);
extern void VIAWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value);
extern const char *VIATakePendingWarning(VIA6522 *v);
```

- [ ] **Step 3: Update the unit test (failing first)**

Rewrite `test_dev_via6522.cpp`'s timer sections; keep the STM sequence; add the N-note
asserts and the eager-scheduler path. Full new file:

```cpp
/* Drives the 68k STM timeout-helper sequence (SPIKE-S3 §1.5) under a fake clock,
 * plus the M2 additions: N+1 expiry (N7), T2CL-read IFR acknowledge (N6), IFR
 * write-1-clear without timer disarm (N8), and eager IFR latch via a bound
 * EventScheduler with a fake time source. */
#include "dev_via6522.h"
#include "event_sched.h"
#include <assert.h>
#include <stdio.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

static uint64_t fake_ticks = 0;                       // VIA ticks (783360 Hz)
static uint64_t fake_clock(void *) { return fake_ticks; }
static uint64_t fake_sched_ns() {                     // scheduler sees the same instant in ns
	return (uint64_t)((unsigned __int128)fake_ticks * 1000000000u / VIA_CLOCK_HZ);
}
static bool direct_call(uint32_t, void (*fn)(void *), void *op) { fn(op); return true; }

static VIA6522 via;
#define BASE 0xF3016000u
static uint8_t rd(uint32_t off)            { return (uint8_t)VIARead(&via, BASE + off, 1); }
static void    wr(uint32_t off, uint8_t v) { VIAWrite(&via, BASE + off, 1, v); }

int main()
{
	VIAReset(&via, BASE, fake_clock, 0);

	// --- STM timeout helper sequence (S3 §1.5), lazy mode (no scheduler bound) ---
	wr(0x0600, rd(0x0600) | 0x08);   // DDRA: PA3 output
	wr(0x1e00, rd(0x1e00) & ~0x08);  // ORA no-handshake: PA3 low
	wr(0x1600, 0x00);                // ACR = 0: one-shot T2, SR off
	wr(0x1c00, 0x20);                // IER <- 0x20: bit7 clear => DISABLE T2 interrupt
	CHECK((rd(0x1c00) & 0x20) == 0);
	wr(0x1000, 0xFF);                // T2C-L latch
	wr(0x1200, 0xFF);                // T2C-H: load 0xFFFF, clear IFR.5, start
	CHECK((rd(0x1a00) & 0x20) == 0); // armed, not expired
	// N7: expiry at N+1 ticks — at dt == N it has NOT fired yet:
	fake_ticks += 0xFFFF;
	CHECK((rd(0x1a00) & 0x20) == 0);
	fake_ticks += 1;                 // dt == N+1
	CHECK((rd(0x1a00) & 0x20) == 0x20);
	CHECK(via.t2_state == VIA_TIMER_FIRED);
	// N8: IFR write-1-clear clears the flag; FIRED is consumed, no re-assert:
	wr(0x1a00, 0x20);
	CHECK((rd(0x1a00) & 0x20) == 0);
	fake_ticks += 0x100;
	CHECK((rd(0x1a00) & 0x20) == 0);

	// IFR bit 7 master: set only when (IFR & IER & 0x7F) != 0.
	wr(0x1c00, 0xA0);                // IER: enable T2
	wr(0x1000, 0x10); wr(0x1200, 0x00);  // T2 = 0x0010
	fake_ticks += 0x20;
	uint8_t ifr = rd(0x1a00);
	CHECK((ifr & 0x20) && (ifr & 0x80));
	// N6: T2C-L read acknowledges (clears) IFR.T2:
	(void)rd(0x1000);
	CHECK((rd(0x1a00) & 0x20) == 0);

	// T2C reads return the live decrementing count while RUNNING.
	wr(0x1000, 0x00); wr(0x1200, 0x01);  // T2 = 0x0100
	fake_ticks += 0x40;
	CHECK(rd(0x1200) == 0x00);            // high byte first: no acknowledge side effect
	CHECK(rd(0x1000) == 0xC0);            // 0x0100 - 0x40 (this read also acks — flag was clear)
	wr(0x1a00, 0x20);                     // clean up (timer still RUNNING; N8: stays running)
	fake_ticks += 0x200;                  // let it fire
	CHECK((rd(0x1a00) & 0x20) == 0x20);   // RUNNING -> FIRED still happens after an IFR clear
	wr(0x1a00, 0x20);

	// Stored-state registers round-trip + Cuda loud stub telemetry.
	wr(0x0000, 0x18); CHECK((rd(0x0000) & 0x18) == 0x18);
	CHECK(via.cuda_touches > 0);
	wr(0x1400, 0x55); CHECK(rd(0x1400) == 0x55);
	CHECK(via.cuda_touches >= 3);
	CHECK(VIATakePendingWarning(&via) != 0 && VIATakePendingWarning(&via) == 0);

	// T1 minimal: load via T1C-H, IFR bit 6 after expiry (N+1 rule applies).
	wr(0x0800, 0x10); wr(0x0a00, 0x00);
	fake_ticks += 0x11;
	CHECK((rd(0x1a00) & 0x40) == 0x40);
	(void)rd(0x0800);                     // (rev 2 S9) T1CL read acknowledges T1
	CHECK((rd(0x1a00) & 0x40) == 0);

	// --- Eager path: bound scheduler latches IFR with NO intervening register read ---
	EventScheduler es;
	es.set_time_now_cb(fake_sched_ns);
	es.set_notify_changes_cb([]() {});
	VIAReset(&via, BASE, fake_clock, 0);
	VIABindScheduler(&via, &es, direct_call);
	wr(0x1000, 0x40); wr(0x1200, 0x00);   // T2 = 0x0040: arms a one-shot at (0x41 ticks) in ns
	fake_ticks += 0x41;
	es.process_timers();                   // pump: expiry event fires under locked_call
	CHECK(via.t2_state == VIA_TIMER_FIRED);
	CHECK((via.ifr_latched & 0x20) == 0x20);   // latched WITHOUT any VIARead
	// Re-arm before expiry: stale event must no-op (generation guard).
	wr(0x1000, 0x10); wr(0x1200, 0x00);   // gen G: T2 = 0x10
	wr(0x1000, 0xF0); wr(0x1200, 0x00);   // gen G+1: T2 = 0xF0 (re-write while running)
	fake_ticks += 0x20;                    // past gen-G deadline, before gen-G+1's
	es.process_timers();                   // gen-G event fires -> must be ignored
	CHECK(via.t2_state == VIA_TIMER_RUNNING);
	CHECK((rd(0x1a00) & 0x20) == 0);

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
```

Update the Makefile recipe (the test now links the scheduler and the bus):

```makefile
test_dev_via6522: test_dev_via6522.cpp dev_via6522.cpp event_sched.cpp mmio_bus.cpp \
                  ../include/dev_via6522.h ../include/event_sched.h ../include/mmio_bus.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_dev_via6522.cpp dev_via6522.cpp event_sched.cpp mmio_bus.cpp
```

Run — Expected: FAIL (new APIs missing).

- [ ] **Step 4: Implement the VIA rework**

`dev_via6522.cpp` — replace `timer_remaining`/`ifr_now` and the timer write/read/IFR
cases; add the binding + expiry plumbing. The full new timer core:

```cpp
#include "dev_via6522.h"
#include "event_sched.h"
#include <string.h>

// ... (enum/IFR defines, VIAReset, cuda_touch, VIATakePendingWarning unchanged,
//      except VIAReset's memset now also zeroes the new fields - it already does,
//      and must additionally leave sched/locked_call NULL: memset covers it) ...

void VIABindScheduler(VIA6522 *v, EventScheduler *sched,
                      bool (*locked_call)(uint32_t addr, void (*fn)(void *), void *opaque))
{
	v->sched = sched;
	v->locked_call = locked_call;
}

static inline uint64_t via_ticks_to_ns(uint64_t ticks)
{
	return (uint64_t)((unsigned __int128)ticks * 1000000000u / VIA_CLOCK_HZ);
}

// RUNNING -> FIRED transition: latch the IFR bit exactly once. Idempotent under
// the device lock; both the lazy poll (reads) and the scheduler event call it.
static void timer_fire(VIA6522 *v, bool t1)
{
	uint8_t *state = t1 ? &v->t1_state : &v->t2_state;
	if (*state != VIA_TIMER_RUNNING) return;
	*state = VIA_TIMER_FIRED;
	v->ifr_latched |= (t1 ? IFR_T1 : IFR_T2);
}

// Lazy poll: fire if the deadline passed. N7: hardware expiry is at N+1 ticks.
static void timer_poll(VIA6522 *v, bool t1)
{
	uint8_t state = t1 ? v->t1_state : v->t2_state;
	if (state != VIA_TIMER_RUNNING) return;
	uint64_t dt = v->now_ticks(v->clock_opaque) - (t1 ? v->t1_load_time : v->t2_load_time);
	if (dt > (t1 ? v->t1_count : v->t2_count))
		timer_fire(v, t1);
}

// Live count for RUNNING; 0 otherwise (fence: no S3 consumer reads after expiry).
// (rev 3 Q1) Sample the clock ONCE: a separate poll-then-recompute pair lets time
// advance between the two reads and `cnt - dt` underflow to 0xFFxx near expiry.
static uint16_t timer_count_now(VIA6522 *v, bool t1)
{
	uint8_t state = t1 ? v->t1_state : v->t2_state;
	if (state != VIA_TIMER_RUNNING) return 0;
	uint64_t dt = v->now_ticks(v->clock_opaque) - (t1 ? v->t1_load_time : v->t2_load_time);
	uint16_t cnt = t1 ? v->t1_count : v->t2_count;
	if (dt > cnt) { timer_fire(v, t1); return 0; }
	if (dt == cnt) return 0;            // counter at 0, fires next tick (N7)
	return (uint16_t)(cnt - dt);
}

static uint8_t ifr_now(VIA6522 *v)
{
	timer_poll(v, false);
	timer_poll(v, true);
	uint8_t ifr = v->ifr_latched & 0x7F;
	if (ifr & v->ier & 0x7F) ifr |= 0x80;
	return ifr;
}

// Scheduler expiry events. The event captures {via, t1, gen}; it must run under
// the device's lock (locked_call -> MMIOBusWithRegion in prod, direct in tests)
// and no-op if the timer was re-armed/cleared since (generation guard).
struct VIATimerEvent { VIA6522 *v; bool t1; uint32_t gen; };
static void via_expiry_locked(void *opaque)
{
	VIATimerEvent *ev = (VIATimerEvent *)opaque;
	VIA6522 *v = ev->v;
	uint32_t cur = ev->t1 ? v->t1_gen : v->t2_gen;
	if (cur == ev->gen)
		timer_fire(v, ev->t1);
}

static void timer_arm(VIA6522 *v, bool t1, uint16_t count)
{
	if (t1) { v->t1_count = count; v->t1_load_time = v->now_ticks(v->clock_opaque);
	          v->t1_state = VIA_TIMER_RUNNING; v->t1_gen++; }
	else    { v->t2_count = count; v->t2_load_time = v->now_ticks(v->clock_opaque);
	          v->t2_state = VIA_TIMER_RUNNING; v->t2_gen++; }
	if (v->sched && v->locked_call) {
		// Capture by value; shared_ptr keeps the payload alive until the event runs.
		VIA6522 *vv = v; bool is_t1 = t1;
		uint32_t gen = t1 ? v->t1_gen : v->t2_gen;
		// N7: deadline is count+1 ticks from load.
		uint64_t ns = via_ticks_to_ns((uint64_t)count + 1);
		v->sched->add_oneshot_timer(ns, [vv, is_t1, gen]() {
			VIATimerEvent ev = { vv, is_t1, gen };
			vv->locked_call(vv->base, via_expiry_locked, &ev);
		});
	}
}
```

And the changed `VIARead`/`VIAWrite` cases (the rest stay as-is):

```cpp
	// VIARead:
	case R_T1CL:   { uint16_t r = timer_count_now(v, true);
	                 v->ifr_latched &= ~IFR_T1;          // (rev 2 S9) T1CL read acks T1, like T2's N6
	                 return r & 0xFF; }
	case R_T1CH:   { uint16_t r = timer_count_now(v, true); return r >> 8; }
	case R_T2CL:   { uint16_t r = timer_count_now(v, false);
	                 v->ifr_latched &= ~IFR_T2;          // N6: T2CL read acknowledges T2
	                 return r & 0xFF; }
	case R_T2CH:   { uint16_t r = timer_count_now(v, false); return r >> 8; }
	case R_IFR:    return ifr_now(v);

	// VIAWrite:
	case R_T1CH:
		v->t1l_h = b;
		timer_arm(v, true, (uint16_t)((b << 8) | v->t1l_l));
		v->ifr_latched &= ~IFR_T1;
		break;
	case R_T2CH:
		timer_arm(v, false, (uint16_t)((b << 8) | v->t2l_l));
		v->ifr_latched &= ~IFR_T2;
		break;
	case R_IFR:
		// N8: write-1-to-clear clears flags ONLY; a RUNNING timer keeps running
		// (its eventual RUNNING->FIRED still latches once), a FIRED one stays consumed.
		// (rev 3 Q2) Settle any already-passed deadline FIRST: without the poll, a
		// blind clear of a not-yet-fired-but-due timer is "resurrected" by the next
		// poll - on hardware the flag was set at the deadline, so the clear is final.
		if (b & IFR_T1) timer_poll(v, true);
		if (b & IFR_T2) timer_poll(v, false);
		v->ifr_latched &= ~(b & 0x7F);
		break;
```

Delete the now-dead `timer_remaining` and the old `t1_running/t2_running` mentions.
Keep the M1 scope-fence comment, updated: free-run-past-zero read-back is still
unmodeled (returns 0 after FIRED), N7/N6/N8 now hardware-faithful.

- [ ] **Step 5: Run `make -C SheepShaver/src/machine test` — Expected: ALL PASS for all eight binaries (six M1 + `test_virt_clock` + `test_event_sched`)**

- [ ] **Step 6: Commit** — `feat(machine): M2 VIA timer state machine on the event scheduler - N6/N7/N8 conformance fixes, MMIOBusWithRegion`

---

### Task 4: Interpreter seams — DEC/TB through the clock, `SS_SYNTH_DEC` absorbed

**Files:**
- Modify: `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-execute.cpp` (mfspr :1320–1340, mtspr :1358–1390, mftbr :1431–1443)

- [ ] **Step 1: Add the gate helper + include**

Add `#include "virt_clock.h"` next to the existing `#include "machine_profile.h"`.
Above `execute_mfspr` (next to the `get_tb_ticks` forward decl), add:

```cpp
/* M2 (MACHINE-LAYER-PLAN §2c): one gate for all virtual-clock SPR seams.
 * Resolution (once): SS_SYNTH_DEC, if set, is honored as a deprecated alias
 * (=0 forces the clock OFF on any profile - escape hatch; non-zero forces it ON,
 * absorbing the old synthetic-DEC experiment); otherwise the newworld profile
 * gets the clock, paravirtual stays frozen (byte-identical default). */
static inline bool ss_vclk_active(void)
{
	static const int active = []() -> int {
		const char *e = getenv("SS_SYNTH_DEC");
		if (e) {
			fprintf(stderr, "[VCLK] SS_SYNTH_DEC is deprecated (absorbed by the M2 "
			        "virtual clock); honoring it as a force-%s override\n",
			        e[0] != '0' ? "on" : "off");
			return e[0] != '0';
		}
		return MachineProfileIsNewWorld() ? 1 : 0;
	}();
	return active != 0;
}
```

(If the file's C++ dialect rejects the immediately-invoked lambda, use the M1 idiom: a
`static const int` initialized by a small file-static function.)

- [ ] **Step 2: Route DEC reads (mfspr case 22)**

Replace the entire `case 22:` block body (:1320–1340) with:

```cpp
	case 22: {	/* DEC (decrementer) — M2 virtual clock (MACHINE-LAYER-PLAN §2c) */
		/* Newworld (or SS_SYNTH_DEC force-on): a real down-counter honoring mtspr,
		 * with the expiry condition latched (delivery is M3). Cold state (no mtspr
		 * yet) is 0 - TB, bit-identical to M1's synthetic counter. VirtClockReady
		 * guards the SS_TEST_HEX-style early paths (clock is init'd there too, but
		 * belt-and-braces: an unready clock reads as the legacy synthetic value). */
		if (ss_vclk_active()) {
			d = VirtClockReady(&g_virt_clock)
			    ? VirtClockReadDEC(&g_virt_clock)
			    : (uint32)(0u - (uint32)get_tb_ticks());
			break;
		}
		d = 0;
		if (ss_stub_on()) ss_stub_mfspr[ss_stub_phase][spr & 1023]++;
		break;
	}
```

- [ ] **Step 3: Route DEC + TBL/TBU writes (mtspr)**

In `execute_mtspr`, insert **before** the `default:` in the `#ifdef SHEEPSHAVER` arm
(after the BAT case at :1383–1385):

```cpp
	case 22:	/* DEC — M2: honored on the virtual clock (was: dropped) */
		if (ss_vclk_active() && VirtClockReady(&g_virt_clock)) {
			VirtClockWriteDEC(&g_virt_clock, s);
			break;
		}
		if (ss_stub_on()) ss_stub_mtspr[ss_stub_phase][spr & 1023]++;
		break;
	case 284:	/* TBL write */
		if (ss_vclk_active() && VirtClockReady(&g_virt_clock)) {
			VirtClockWriteTBL(&g_virt_clock, s);
			break;
		}
		if (ss_stub_on()) ss_stub_mtspr[ss_stub_phase][spr & 1023]++;
		break;
	case 285:	/* TBU write */
		if (ss_vclk_active() && VirtClockReady(&g_virt_clock)) {
			VirtClockWriteTBU(&g_virt_clock, s);
			break;
		}
		if (ss_stub_on()) ss_stub_mtspr[ss_stub_phase][spr & 1023]++;
		break;
```

- [ ] **Step 4: Route TB reads (mftbr)**

Replace the two cases in `execute_mftbr` (:1436–1438):

```cpp
	case 268:
		d = (uint32)((ss_vclk_active() && VirtClockReady(&g_virt_clock))
		             ? VirtClockTB(&g_virt_clock) : get_tb_ticks());
		break;
	case 269:
		d = (uint32)(((ss_vclk_active() && VirtClockReady(&g_virt_clock))
		             ? VirtClockTB(&g_virt_clock) : get_tb_ticks()) >> 32);
		break;
```

(Paravirtual TB behavior is bit-identical: same source, same ratio, `tb_offset == 0`.)

- [ ] **Step 5: Build + fast gate (clock not yet initialized — must be inert)**

```bash
cd SheepShaver && make build-ss && SS_HARNESS_BATCH=1 make test-jit
```
Expected: build clean; score=100 (350/350). The harness runs paravirtual ⇒ `ss_vclk_active()`
false ⇒ DEC reads 0 exactly as before; the `g_virt_clock` global links from Task 5's
Makefile.in change — **if Task 5 hasn't landed yet in your worktree, add the two SRCS
entries first (Task 5 Step 1) so this links**; the tasks are sequenced 4→5 in one
subagent run for this reason, or do Step 1 of Task 5 here and note it in the commit.

**Behavior-change note (rev 2 finding I4 — record in the commit body):** today a harness run
with `SS_SYNTH_DEC=1` reads DEC as frozen-0 anyway (`TimebaseSpeed==0` on the un-init'd early
path makes `get_tb_ticks()` return 0). Post-M2 the harness path initializes the clock, so
`SS_SYNTH_DEC=1` harness runs would read live time-varying DEC values — any future SPR-22 test
vector must therefore never be combined with `SS_SYNTH_DEC=1` (test-jit diffs two separate
process runs). The default gates are unaffected (no SPR-22 vector exists; verified).

- [ ] **Step 6: Commit** — `feat(machine): M2 interpreter seams - DEC/TB mfspr+mtspr+mftb through the virtual clock, SS_SYNTH_DEC absorbed`

---

### Task 5: Build integration, clock bring-up, scheduler pump thread, VIA rebinding

**Files:**
- Modify: `SheepShaver/src/Unix/Makefile.in` (SRCS)
- Modify: `SheepShaver/src/Unix/main_unix.cpp`

- [ ] **Step 1: Add sources to the emulator build**

In `SheepShaver/src/Unix/Makefile.in` SRCS, next to the existing `../machine/*.cpp` entries, add:

```
../machine/virt_clock.cpp ../machine/event_sched.cpp \
```

Regenerate: `cd SheepShaver/src/Unix && ./config.status Makefile`.

- [ ] **Step 2: Clock bring-up — BOTH init paths**

Add includes near the other machine-layer includes in `main_unix.cpp`:

```cpp
#include "virt_clock.h"
#include "event_sched.h"
```

File-scope helpers (near `mmio_via_now_ticks` ~:1160):

```cpp
// M2: the single host time authority. GetTicks_usec() is the emulator's existing
// monotonic source; everything (TB, DEC, VIA ticks, scheduler deadlines) derives
// from it through the virtual clock so all guest-visible time is mutually consistent.
static uint64_t vclk_host_now_ns(void *)
{
	return GetTicks_usec() * 1000ull;
}
// Idempotent: callable from both the harness early path and normal init.
static void VirtClockInitHost(void)
{
	if (!VirtClockReady(&g_virt_clock))
		VirtClockInit(&g_virt_clock, TimebaseSpeed, vclk_host_now_ns, NULL);
}
```

Call `VirtClockInitHost()`:
1. In the `SS_TEST_HEX`/`SS_TEST_HEX_FILE` harness block (~:1202) **before** vector
   execution (LEARNINGS: this path exits before normal init; the clock must be ready in
   case a vector or `SS_SYNTH_DEC=1` run touches DEC). `TimebaseSpeed` is 0 there — the
   `VirtClockInit` 25 MHz fallback applies; acceptable, vectors don't measure wall time.
2. *(rev 2 findings I1/C2 — corrected placement)* In normal init **after
   `get_system_info()` at `main_unix.cpp:1396`** (NOT after `MachineProfileInit()` at
   :1340): `TimebaseSpeed` is a zero-initialized `int64` first assigned inside
   `get_system_info()` (default 25 MHz at :454, `cpuclock`-pref override :461–464).
   Initializing earlier would silently pin the clock at the 25 MHz fallback while
   `get_tb_ticks()` uses the real rate — the exact §2c inconsistency M2 exists to remove.
   Place the call after :1396 and before the scheduler block / MMIO bring-up (:1560).
   Cast note: `TimebaseSpeed` is `int64` → narrow explicitly: `(uint32_t)TimebaseSpeed`.

- [ ] **Step 3: Scheduler instance + pump thread**

File-scope (same region):

```cpp
// M2: host event scheduler (MACHINE-LAYER-PLAN §2c) + its pump thread (§2g row 3:
// "Event-scheduler callbacks - tick/timer thread"). Started only when the machine
// layer is live (MachineUsesMMIOBus() for devices, or newworld for DEC expiry).
static EventScheduler *g_event_sched = NULL;
static pthread_t sched_pump_thread;
static pthread_mutex_t sched_pump_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  sched_pump_cv  = PTHREAD_COND_INITIALIZER;
static bool sched_pump_quit = false;     // guarded by sched_pump_mtx (rev 2 I3: not volatile-as-sync)
static bool sched_pump_kicked = false;   // condvar predicate (rev 2 I3: lost-wakeup guard)
static bool sched_pump_started = false;

static void sched_pump_kick(void)
{
	pthread_mutex_lock(&sched_pump_mtx);
	sched_pump_kicked = true;            // (rev 2 I3) a kick before the wait is never lost
	pthread_cond_signal(&sched_pump_cv);
	pthread_mutex_unlock(&sched_pump_mtx);
}

static void *sched_pump_main(void *)
{
	// (rev 2 I3) Residual latency note: a timer added between process_timers()
	// returning and the predicate check below is caught by sched_pump_kicked; the
	// 10ms cap additionally bounds any CLOCK_REALTIME step (NTP) distortion.
	const uint64_t CAP_NS = 10000000ull;   // 10 ms re-check cap (idle floor)
	for (;;) {
		uint64_t slice = g_event_sched->process_timers();
		if (slice == 0 || slice > CAP_NS) slice = CAP_NS;
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += (long)(slice % 1000000000ull);
		ts.tv_sec  += (time_t)(slice / 1000000000ull) + ts.tv_nsec / 1000000000L;
		ts.tv_nsec %= 1000000000L;
		pthread_mutex_lock(&sched_pump_mtx);
		if (!sched_pump_quit && !sched_pump_kicked)
			pthread_cond_timedwait(&sched_pump_cv, &sched_pump_mtx, &ts);
		sched_pump_kicked = false;
		bool quit = sched_pump_quit;
		pthread_mutex_unlock(&sched_pump_mtx);
		if (quit) break;
	}
	return NULL;
}

static void sched_pump_stop(void)   // atexit: stop callbacks before the [VCLK] dump
{
	if (!sched_pump_started) return;
	pthread_mutex_lock(&sched_pump_mtx);
	sched_pump_quit = true;
	pthread_cond_signal(&sched_pump_cv);
	pthread_mutex_unlock(&sched_pump_mtx);
	pthread_join(sched_pump_thread, NULL);
}
// (rev 2 I6) Ordering caveat: M1's [MMIO] stats atexit was registered at bus
// bring-up (:1588), i.e. AFTER this task's block -> by LIFO it dumps BEFORE the
// pump stops. Cosmetic only (stats counters are monotonic; M2 callbacks only
// latch IFR bits); the [VCLK] dump below IS ordered after the stop.

// DEC eager-expiry hook (VirtClock on_dec_write): one-shot that latches the
// condition. Generation-guarded inside VirtClockDECExpire - stale events no-op.
static void vclk_dec_arm(void *, uint64_t ns_until_expiry, uint32_t gen)
{
	if (g_event_sched)
		g_event_sched->add_oneshot_timer(ns_until_expiry,
		                                 [gen]() { VirtClockDECExpire(&g_virt_clock, gen); });
}

static void vclk_dump_stats_atexit(void) { VirtClockDumpStats(&g_virt_clock, stderr); }
```

In normal init, after `VirtClockInitHost()` and **before** the MMIO bus bring-up block,
gated like this:

```cpp
	// M2: event scheduler + pump. Needed by the VIA timers (any bus config) and by
	// the DEC eager-expiry (newworld). Paravirtual default: none of this starts.
	if (MachineUsesMMIOBus() || MachineProfileIsNewWorld()) {
		g_event_sched = new EventScheduler();
		g_event_sched->set_time_now_cb([]() { return vclk_host_now_ns(NULL); });
		g_event_sched->set_notify_changes_cb(sched_pump_kick);
		g_virt_clock.on_dec_write = vclk_dec_arm;
		g_virt_clock.cb_opaque = NULL;
		if (pthread_create(&sched_pump_thread, NULL, sched_pump_main, NULL) != 0) {
			fprintf(stderr, "[ESCHED] FATAL: cannot start scheduler pump thread\n");
			QuitEmulator();
		}
		sched_pump_started = true;
		atexit(vclk_dump_stats_atexit);   // registered BEFORE stop => runs AFTER it (LIFO)
		atexit(sched_pump_stop);
		fprintf(stderr, "[ESCHED] event scheduler pump running (cap 10ms)\n");
	}
```

- [ ] **Step 4: Re-source the VIA clock + bind the scheduler**

In the bus bring-up block (~:1560–1600): replace the `mmio_via_now_ticks` body to derive
from the virtual clock, and bind the scheduler after registration:

```cpp
// VIA clock: virtual-clock ns -> VIA ticks (783360 Hz). 128-bit: host-uptime-scale
// ns * 783360 overflows uint64.
static uint64_t mmio_via_now_ticks(void *)
{
	return (uint64_t)((unsigned __int128)VirtClockNowNS(&g_virt_clock) * VIA_CLOCK_HZ
	                  / 1000000000u);
}
```

After `MMIOBusActivate()` (:1587):

```cpp
		// M2: VIA timers hang on the event scheduler (eager IFR latch; reads stay
		// the lazy backstop). Must be after region registration: expiry callbacks
		// run under the region lock via MMIOBusWithRegion.
		VIABindScheduler(&via, g_event_sched, MMIOBusWithRegion);
```

- [ ] **Step 5: Gates**

```bash
cd SheepShaver && make build-ss && SS_HARNESS_BATCH=1 make test-jit && make test-jit
```
Expected: clean build; both batch and legacy authoritative gates score=100 (350/350).
Paravirtual default starts no thread, allocs no scheduler, prints no `[ESCHED]`/`[VCLK]`
lines — verify by grepping a `make test-opcodes` run's stderr for `ESCHED|VCLK`: zero hits.

- [ ] **Step 6: Commit** — `feat(machine): M2 integration - clock bring-up (both init paths), scheduler pump thread, VIA scheduler binding`

---

### Task 6: `mdec_dat` nanokernel patch retirement on newworld

**Files:**
- Modify: `SheepShaver/src/rom_patches.cpp:1693–1707` (`mdec_dat` site in `patch_nanokernel()`)

- [ ] **Step 1: Profile-gate the patch (M1 `scc_init` idiom — enforcement option 1)**

Keep the `find_rom_data` search unconditional (the lenient skip-note logic stays); gate
only the byte-patching:

```cpp
	static const uint8 mdec_dat[] = {0x7f, 0xf6, 0x02, 0xa6, 0x2c, 0x08, 0x00, 0x00, 0x93, 0xe1, 0x06, 0x68, 0x7d, 0x16, 0x03, 0xa6};
	base = find_rom_data(0x310000, 0x314000, mdec_dat, sizeof(mdec_dat));
	if (base == 0 && !g_rom_904_lenient) return false;
	if (base) {
	if (MachineProfileIsNewWorld()) {
		// M2 (ROM-PATCH-AUDIT: mdec_dat RETIRE@M2): the virtual clock honors
		// mtspr/mfspr DEC, so the nanokernel's decrementer programming must run
		// and reach the clock - the patch would make the M2 clock dead code.
		// Paravirtual keeps the patch (DEC reads 0 there; the patched-out code
		// would otherwise busy the 60Hz-tick timing assumptions).
		fprintf(stderr, "[M2] mdec nanokernel patch retired (newworld profile): guest mtdec/mfdec reach the virtual clock\n");
	} else {
	D(bug("mdec %08lx\n", base));
	lp = (uint32 *)(ROMBaseHost + base);		// Don't modify DEC
	lp[0] = htonl(0x3be00000);					// li	r31,0
#if 1
	lp[3] = htonl(POWERPC_NOP);
	lp[4] = htonl(POWERPC_NOP);
#else
	lp[3] = htonl(0x39000040);					// li	r8,0x40
	lp[4] = htonl(0x990600e4);					// stb	r8,0xe4(r6)
#endif
	}
	} else fprintf(stderr, "[ROMPATCH] SKIP mdec (decrementer neutralize — absent in 9.0.x parcels ROMs; pattern is OldWorld/1.1-NK)\n");
```

(`MachineProfileIsNewWorld()` is already used in rom_patches.cpp since M0/M1 (e.g. :715,
:815) — include already present. `patch_nanokernel()` runs after `MachineProfileInit()`
(:1340 → InitAll → PatchROM) — verified.)

**(rev 2 finding D1 — scope honesty):** the `mdec_dat` pattern is **absent from the 9.0.1
parcels ROM** (byte-verified) — on the Task 8 acceptance boot this gate is **never
exercised** (`base==0` → the SKIP branch fires; the `[M2]` line cannot appear). The gate
is real and live for **1.1-ROM `machine newworld`** configs (pattern at ROM 0x312c24).
Do NOT move the `[M2]` print outside `if (base)` to make it appear — that would
manufacture a false pass. Task 8 splits the DoD accordingly.

- [ ] **Step 2: Build + authoritative gate** — `make build-ss && make test-jit` → 350/350 (the site is newworld-only; paravirtual byte-identical).

- [ ] **Step 3: Commit** — `feat(machine): M2 mdec_dat patch retirement on newworld - nanokernel DEC programming reaches the virtual clock`

---

### Task 7: Non-regression checkpoint (verification only — no commit)

- [ ] **Step 1:** `make -C SheepShaver/src/machine test` → ALL PASS, all **eight** binaries.
- [ ] **Step 2:** `cd SheepShaver && make build-ss && make test-jit` → legacy authoritative gate, score=100 (350/350). (`SS_HARNESS_BATCH=1` may be used for iteration, but this checkpoint records the legacy result.)
- [ ] **Step 3:** Interpreter suite: `make test-opcodes` → score=100; stderr contains no `[ESCHED]`/`[VCLK]`/`[MMIO]` lines (paravirtual inertness).
- [ ] **Step 3b (rev 2 finding D3 — positive seam check, no boot needed):** the only
  automated gate that *executes* M2 seam code:
  ```bash
  cd SheepShaver && SS_SYNTH_DEC=1 SS_TEST_HEX=7c7602a6 SS_TEST_JIT=0 ./SheepShaver 2>&1 | grep -i -A4 REGDUMP
  ```
  (Invoke the binary directly — do NOT go through the harness's run-twice-and-diff
  machinery: DEC is time-varying, the diff would spuriously fail.) `7c7602a6` =
  `mfspr r3,DEC`. Assert manually from the REGDUMP: **r3 ≠ 0** (the force-on override +
  harness-path clock init + `VirtClockReadDEC` all executed; pre-M2 this read 0 because
  `TimebaseSpeed==0` froze the synth counter on the harness path). One-off scripted
  assert — never add an SPR-22 vector to the harness table. Note: the pump thread and
  VIA scheduler binding still get their first execution only in Task 8 — known gap.
- [ ] **Step 4 (rev 2 finding D8 — honest downgrade):** `make -C rom-harness` clean build.
  Bench is N/A: rom-harness compiles only `rom-harness.cpp + ppc-jit.cpp`
  (rom-harness/Makefile:16) and M2 changes neither — record "bench N/A, no compiled file
  changed (verified by SRCS)" rather than a self-comparison.
- [ ] **Step 5:** `make e2e-test` → all offline tests pass.
- [ ] **Step 6:** `make e2e` (**coordinate with the user before launching** — sanctioned exception, but the session rule is ask-first) → lifecycle PASS, exit 0, no `[VCLK]`/`[ESCHED]` lines in the paravirtual log.
- [ ] **Step 7:** Report all results verbatim to the orchestrator. Any failure: stop, `superpowers:systematic-debugging`, do not proceed to Task 8.

---

### Task 8: Acceptance — observed DEC traffic on the 9.0.1 diagnostic boot (mdec retirement DoD)

**This task launches the emulator. Ask the user before each run** (NOT the e2e exception).
Headless, short, isolated config (CLAUDE.md recipe).

- [ ] **Step 1: Diagnostic prefs**

```bash
printf 'rom /Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom\nramsize 268435456\nnogui true\nmachine newworld\n' > /tmp/m2accept.prefs
```
(Verify the ROM filename with `ls /Users/Shared/macemu/` — the asset table lists `Mac OS ROM 9.0.1`.)

- [ ] **Step 2: DEC-frozen probe (rev 2 findings C4/D6 — relabeled; NOT an M1 reference):**
`SS_SYNTH_DEC=0 machine=newworld` is a configuration that never existed pre-M2 (clock
forced off + the NK natively un-patched on 9.0.1) — it is a *diagnostic*, not a rollback.
Its value: demonstrating the clock is load-bearing.

```bash
cd SheepShaver && SS_SYNTH_DEC=0 SS_ROM_LENIENT=1 SS_ROM_SKIP_JUMP68K=1 \
  perl -e 'alarm 90; exec @ARGV' ./SheepShaver --config /tmp/m2accept.prefs 2>&1 | tee /tmp/m2-baseline.log
```
Expected: **hangs at the NK init DEC wait (ROM ~0x3127a8 era; the alarm kills it)** —
frozen DEC makes the timed spin never elapse (the documented pre-SS_SYNTH_DEC behavior).
Record whatever actually happens as the comparison datum.

- [ ] **Step 3: Acceptance run (clock live) — the SEAM DoD (rev 2 findings D1/D2 — split):**

```bash
SS_ROM_LENIENT=1 SS_ROM_SKIP_JUMP68K=1 \
  perl -e 'alarm 120; exec @ARGV' ./SheepShaver --config /tmp/m2accept.prefs 2>&1 | tee /tmp/m2-accept.log
```

Assert from `/tmp/m2-accept.log`:
1. `[ROMPATCH] SKIP mdec` line present — the 9.0.1 parcels NK is **natively un-patched**
   (pattern absent); the Task 6 retirement gate is NOT exercised by this boot, and the
   `[M2] … retired` line must NOT be expected here.
2. **Observed seam traffic (the DoD false-pass guard):** the `[VCLK]` exit dump shows
   `mtspr_dec > 0` (the parcels NK's native decrementer programming reached the clock —
   13 pre-wall `mtdec` sites exist in the dump, 2 inside the documented-executed
   CPU-detect range) and `mfspr_dec > 0` (the 0x3127a8 wait alone guarantees this).
   Record the numbers. `tb_writes` is expected to be 0 — not a failure (no known guest
   TB write pre-wall).
3. `dec_expiries` and `pending` reported (≥ 0 — context datum for M3; no delivery is
   expected or attempted).
4. Boot reaches **at least** the M0/M1-era stage (the 0x50326050 wall) — no earlier
   regression vs the M1 acceptance record.
5. No `[ESCHED]`/pump-thread aborts; no new `[MMIO]` storms (compare region stats to the
   M1 acceptance numbers).
- **If `mtspr_dec == 0`:** do NOT fudge. Disassemble around the pre-wall `mtdec` sites
  (capstone recipe in CLAUDE.md) to determine whether they execute before the
  0x50326050 wall. If they provably don't, record a carried-forward live DoD in
  ROM-PATCH-AUDIT exactly as M1 did for the SCC WR telemetry (unit-level coverage:
  the Task 1 vectors), and report to the orchestrator — a finding, not a failure.

- [ ] **Step 4: The RETIREMENT DoD (rev 2 finding D1) — 1.1-ROM newworld probe (ask user; optional but cheap):**
The only config where the `mdec_dat` gate is live is a 1.1-ROM `machine newworld` boot
(pattern at ROM 0x312c24). If the user approves a second short run:

```bash
printf 'rom /Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom\nramsize 268435456\nnogui true\nmachine newworld\n' > /tmp/m2retire.prefs
perl -e 'alarm 60; exec @ARGV' ./SheepShaver --config /tmp/m2retire.prefs 2>&1 | tee /tmp/m2-retire.log
```
Assert: `[M2] mdec nanokernel patch retired` line present, and if the boot survives long
enough, `mtspr_dec > 0` in the `[VCLK]` dump. If the 1.1 newworld boot dies before the NK
DEC sites (likely — this config is a bus testbed, not a supported boot), record
"retirement gate live-verified to fire ([M2] line); traffic verification carried forward"
in ROM-PATCH-AUDIT. If the user declines the run, record the full carried-forward note.

- [ ] **Step 5: Commit any instrumentation added** — `test(machine): M2 acceptance instrumentation` (only if needed).

---

### Task 9: Documentation + bookkeeping

**Files:**
- Modify: `docs/planning/MACHINE-LAYER-PLAN.md` (header status + M2 row → done with acceptance numbers — **wording per rev 2 D9: "done; mdec retirement-gate live-verification carried forward" if Task 8 Step 4 didn't fully verify**; §2c note that the scheduler exists)
- Modify: `docs/planning/ROADMAP.md` (D3 status line — rev 2 D9: explicit file-list entry, M1 precedent)
- Modify: `docs/planning/machine/ROM-PATCH-AUDIT.md` (`mdec_dat` row → RETIRED@M2 **with the honest Task 8 outcome**: gate landed; NOT exercised on 9.0.1 (pattern absent — update the row's "what it neutralizes" note); 1.1-ROM verification result or carried-forward marker; M1 `scc_init` row as the template)
- Modify: `docs/planning/machine/M1-DEVICE-CONFORMANCE.md` (§6: N6/N7/N8 → "resolved in M2" with one-line what-changed + the T1CL-ack addition; N1–N5/N9 unchanged)
- Modify: `CHANGELOG.md` (M2 entry; **must cite the DingusPPC donor file + SHA** per backport hygiene)
- Modify: `docs/planning/HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` (rev 2 D5: one-line note at the SS_SYNTH_DEC recipe (~:808) — post-M2 the env var is a deprecated force-override and readings are no longer monotone free-run once the guest writes DEC)
- Modify: `LEARNINGS.md` (only if Task 8 produced a non-obvious finding)

- [ ] **Step 1: Update each doc.** CHANGELOG skeleton:

```markdown
### [SheepShaver] Machine Layer M2: virtual clock - guest TB/DEC + host event scheduler

- **Virtual clock** (`src/machine/virt_clock.cpp`) - guest-visible TB/DEC honoring
  mtspr/mfspr (incl. mttbl/mttbu); DEC-expiry latches the decrementer exception
  CONDITION only (delivery lands in M3); SS_SYNTH_DEC absorbed (deprecated alias,
  =0 escape hatch); M1's minimal DEC tick replaced; cold-state DEC bit-identical
  to the M1 synthetic counter.
- **Host event scheduler** (`src/machine/event_sched.cpp`) - ported from DingusPPC
  core/timermanager.{cpp,h} @ 92bb6d10549529f9f4031a85c2bc136149535bdc
  (github.com/dingusdev/dingusppc, GPL-3.0; combined work GPLv3). Pump thread with
  10ms cap + condvar wake; callbacks run without the queue mutex (lock-order rule:
  device -> queue only).
- **VIA 6522 timers re-clocked** - explicit IDLE/RUNNING/FIRED state machine on the
  scheduler (eager IFR latch) with lazy read-side backstop; conformance notes
  N6 (T2CL-read acknowledge), N7 (N+1 expiry), N8 (IFR clear no longer disarms)
  resolved. New MMIOBusWithRegion locked-callback bus API.
- **mdec_dat retired on newworld** - retirement gate landed (live only for 1.1-ROM
  newworld configs; the 9.0.1 parcels NK never matched the pattern and is natively
  un-patched). DEC programming in the parcels NK reaches the clock via the M2 seams
  (observed: mtspr_dec=N, mfspr_dec=N from the acceptance boot).
- **Paravirtual non-regression** - test-jit 350/350 (legacy authoritative; batch
  harness for iteration), machine unit tests ALL PASS (8 binaries), e2e PASS,
  no [VCLK]/[ESCHED] output on the default profile.
```

- [ ] **Step 2: Cross-tracker sweep** — grep `docs/` for `M2` mentions (MACHINE-LAYER-PLAN, ROADMAP D3 line) **and for `SS_SYNTH_DEC`** (rev 2 D5: stale recipes in HANDOFF / NEW-WORLD-ROM-SUPPORT-PLAN / SPRG0-KDP-DESIGN / SPIKE-S3 — annotate or update each) and update every tracker that names the milestone.

- [ ] **Step 3: Commit** — `docs(machine): mark Machine Layer M2 complete`

---

## Self-review record (kept per writing-plans skill)

- **Spec coverage vs the M2 row + user brief:** TB/DEC honoring mtspr/mfspr ✓ (T1/T4);
  SS_SYNTH_DEC + M1 minimal tick absorbed ✓ (T4, cold-state-identical by construction);
  host event scheduler for device timers ✓ (T2 port + T3 VIA consumer + T5 pump);
  DEC-expiry condition only ✓ (T1 latch + T5 arm hook; nothing touches spcflags/interrupt
  paths); mdec_dat retirement with observed-traffic DoD ✓ (T6 + T8, honest fallback);
  DingusPPC donor cited file+SHA ✓ (T2 header + commit + T9 CHANGELOG); N6/N7/N8 ✓ (T3);
  S3 §2.4 check_work DEC math as the consumer test ✓ (T1 Step 2); paravirtual
  byte-identical ✓ (gates in T4/T5/T6/T7 + inertness greps).
- **Type consistency:** `VirtClock*` APIs uniform across T1/T4/T5; `EventScheduler`
  methods match donor names used in T3/T5; `MMIOBusWithRegion(uint32_t, void(*)(void*),
  void*)` identical in T3 decl/impl/use; `VIABindScheduler` signature matches T3 test.
- **Known sharp edges flagged to implementers:** `g_virt_clock` link order (T4 Step 5
  note — SRCS entry may need to land with T4 in the same worktree session);
  `TimebaseSpeed` assignment order vs clock init (T5 Step 2 contingency); harness early
  path needs its own `VirtClockInitHost()` call (LEARNINGS SS_TEST_HEX rule); atexit LIFO
  ordering (stats registered before stop); the immediately-invoked-lambda fallback idiom
  (T4 Step 1); `unsigned __int128` for all ns×freq products (three sites: virt_clock,
  via ticks, test helper).

## Rev 2 corrections (red-team round, 2026-06-10 — 3 parallel code-verified reviewers)

All findings folded into the tasks above with `(rev 2 …)` markers. Reviewer mandates:
I = integration seams, S = concurrency/semantics, D = DoD honesty.

**CRITICAL:**
- **C1 (S1)** — `dec_gen` load-then-`dec_armed`-CAS window: a stale scheduler event could
  steal a fresh arm (spurious pending + permanently lost expiry; guest hang once M3
  delivers). Fixed: gen+armed fused into one atomic `dec_arm_word`, single CAS in
  `dec_fire` (Task 1).
- **C2 (D1)** — the `mdec_dat` pattern is **absent from the 9.0.1 parcels ROM** (byte-
  verified against `/tmp/rom901_decompressed.bin`; present in the 1.1 NK at 0x312c24) —
  Task 8's original "`[M2] … retired` line present" assert failed by construction and
  invited a manufactured pass. Fixed: DoD split into seam-DoD (9.0.1 native DEC traffic)
  + retirement-DoD (1.1-ROM newworld probe or carried-forward record); Task 6/8/9 and
  the facts block rewritten; skip-message text updated.

**MAJOR:**
- **I1/S2** — `TimebaseSpeed` is a zero-initialized `int64` first set inside
  `get_system_info()` (:1396), *after* `MachineProfileInit()` (:1340); the planned init
  point would silently pin 25 MHz vs the cpuclock-pref rate. Fixed: clock init moved
  after :1396 (Task 5 Step 2); narrowing cast noted.
- **I2** — Task 3 was not buildable in parallel with Tasks 1+2 (links `event_sched.cpp`;
  shared machine Makefile/.gitignore). Fixed: sequencing 1‖2 → 3 (Parallelization +
  Task 3 banner).
- **S3 (I3)** — pump-thread lost wakeup (signal with no waiter is dropped; no condvar
  predicate) + `volatile`-as-sync. Fixed: `sched_pump_kicked` predicate, mutex-guarded
  quit, documented 10ms-cap-bounded residual latency (Task 5 Step 3).
- **S4/D6 (C4)** — Task 8 Step 2 was mislabeled "M1 behavior reference" and its
  expectation was backwards: `SS_SYNTH_DEC=0` + newworld is a never-existed diagnostic
  config whose expected outcome is a **hang at the ~0x3127a8 NK DEC wait** (clock is
  load-bearing), not the 0x50326050 wall. Fixed: relabeled + expectation corrected;
  `SS_SYNTH_DEC=0`-on-newworld documented as diagnostic, not rollback.

**MINOR (all folded):**
- **S5** `dec_expiries` atomic fetch_add (cross-thread UB). **S6** donor `cb_active`
  FIXME kept verbatim + consequence note. **S7** OEA scope note in virt_clock.h
  (mtspr-MSB-transition + cold-wrap conditions deferred to M3). **S8** test_event_sched
  cancels middle-before-head to exercise the erase/make_heap path. **S9** T1CL read now
  acks IFR.T1 (symmetry with N6) + test. **S10** donor cancel-can't-prevent-last-fire
  contract note in event_sched.h. **I3 (facts)** JIT sentence corrected (native
  mfspr/mtspr for LR/CTR/XER only; mftb explicit fallback). **I4** harness
  `SS_SYNTH_DEC=1` time-dependence note (Task 4). **I5** `test-powerpc` known-broken
  bystander noted. **I6** [MMIO]-dump-before-pump-stop ordering caveat. **D3** new
  Task 7 Step 3b positive seam check (direct-binary `mfspr r3,DEC` ≠ 0); pump/VIA-binding
  remains Task-8-only (named gap). **D5** SS_SYNTH_DEC doc sweep + HANDOFF note added to
  Task 9. **D8** bench gate honestly downgraded to build-only (rom-harness compiles no
  M2 file). **D9** ROADMAP.md in Task 9 file list; M2-row wording pre-authorizes
  "carried forward". **D10** `tb_writes`==0 expected in Task 8, not a failure.

## Rev 3 corrections (execution-time quality review, 2026-06-10)

Two Important guest-visible defects found by the Tasks-3+4 code-quality review in
code this plan printed verbatim (the rev-2 red team missed both — frozen fake
clocks structurally cannot exhibit the TOCTOU):
- **Q1** — `timer_count_now` sampled the clock twice (poll, then recompute): with a
  live clock, `dt` can cross `cnt` between samples and `cnt - (uint16_t)dt`
  underflows to 0xFFxx — the guest reads the counter jumping UP near expiry.
  Fixed: single clock sample decides fire/return. Task 3 code block updated.
- **Q2** — IFR write-1-clear of an already-due-but-unfired timer was "resurrected"
  by the next poll (flag reappears after the guest cleared it). Fixed: the IFR
  write settles (polls) the addressed timer bits before clearing. Task 3 code
  block updated. Tests added for both boundaries.
Also recorded from that review (ride-along items, not fixed in M2): sub-tick-early
eager fire (truncating ticks→ns conversion; immaterial), VIAReset dropping a
scheduler binding silently (re-bind responsibility documented), and the cheap
interim §2g mitigation (skip the eager arm on the Mach-handler thread) the M3
hardening note should consider alongside pre-allocated event slots.

**Verified-OK highlights (for implementer confidence):** all Task 1/3 test arithmetic
traced exact (incl. 128-bit truncation at eager-fire boundaries — floor superadditivity);
paravirtual byte-identity of the stub-counting and TB math confirmed against
ppc-execute.cpp:1320–1443; donor port diff clean beyond declared adaptations; lock-order
(CPU: region→queue→pump_mtx; pump: queue alone then region alone) cycle-free; atexit
LIFO ordering of stop-vs-[VCLK]-dump correct; timespec carry math correct; rom-harness
SRCS isolation confirmed; `patch_nanokernel` runs after `MachineProfileInit`; the 9.0.1
ROM filename exists in /Users/Shared/macemu/.
