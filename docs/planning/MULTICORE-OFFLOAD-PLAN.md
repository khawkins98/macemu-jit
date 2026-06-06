# Plan: Multi-core offload — "multithreading light"

> **Status:** ⏸ Not started — exploratory / design · **Created:** 2026-06-06 · **Updated:** 2026-06-06
> **Why this doc exists:** Modern hosts have many cores; SheepShaver runs the guest on essentially one. This specs *what work can actually move to other host cores* to speed up high-utilization cases (games, large apps), how far that goes, and how the ambitious end of it connects to the supervisor-fidelity ([`MMU-NANOKERNEL-MP-PLAN.md`](MMU-NANOKERNEL-MP-PLAN.md)) work.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../CONTRIBUTING.md) → "Documentation Lifecycle")._

---

## TL;DR / verdict

The instinct — "use the dozens of idle cores" — is right that there's parallelism to exploit, but
**the biggest, safest wins are emulator-*internal*, not splitting guest execution.** Three things to
internalize before any work:

1. **The guest is one logical CPU running a cooperative OS.** You cannot naively spread guest
   execution across host cores: the cooperative "Blue" Mac OS world is a single thread of control,
   and the JIT + flat memory model assume one CPU (no cross-core ordering, no atomic emulation, no
   per-core W^X). Splitting *guest* work across cores is only meaningful at the granularity the
   guest OS itself treats as concurrent — **preemptive MP tasks** — which is the hard, gated Tier 3.
2. **The win that helps games/large apps most is offloading the emulator's *own* overhead** off the
   hot emulation core: background JIT compilation, device emulation, framebuffer compositing. Most
   classic games are single-threaded cooperative apps — they get *nothing* from guest SMP, but a lot
   from keeping the emulation thread unimpeded on a performance core. **This reframes the goal:** not
   "run the game on many cores," but "stop making the one game-thread share its core with the
   emulator's housekeeping."
3. **Some of this already exists.** Host helper threads already run the 60 Hz timer
   (`main_unix.cpp:1242`), NVRAM (`:1248`), audio (SDL callback), and video redraw on separate host
   threads — the OS scheduler already spreads them across cores. "Sound and drawing on other cores"
   is *partly already true*; the question is what *more* can be peeled off, and how cleanly.

**Tiering by feasibility (do top-down):**

| Tier | What | Effort | Payoff | Risk | Touches guest exec? |
|---|---|---|---|---|---|
| 0 | Scheduling/QoS hints (P-core for emul, RT audio, E-core compile) | **Low** | Low–Med | Low | No |
| 1 | Emulator-internal parallelism (background compile, async devices, Metal) | Med | **High** | Low–Med | No |
| 2 | Offload HLE'd subsystems to worker threads (clean handoff) | Med–High | Med | Med | No (native, not guest) |
| 3 | **True guest SMP** — MP tasks on separate host cores | **Very high** | Med (long tail) | **Very high** | **Yes** |

Recommended path: **Tier 0 → Tier 1 → Tier 2 selectively → Tier 3 only if the supervisor-fidelity
plan lands *and* a real MP workload demands it.** Tier 1 is where the ROI is, and most of it is
already in `OPTIMIZATION-PLAN.md` (R8/R9/R10) — this doc frames it as one coherent parallelism story.

---

## The hard constraint — why you can't just "run the guest on N cores"

Any scheme that runs guest (or guest-derived) code on multiple host cores *concurrently* must solve
all of these; they're why naive guest-SMP is Tier 3, not Tier 1:

- **Shared mutable flat memory + weak ordering.** Guest RAM is one flat host region
  (`DIRECT_ADDRESSING`/`NATMEM_OFFSET`; `vm.hpp:208`). PPC has a stronger memory model than ARM64;
  two host threads touching guest RAM need barriers that reproduce PPC ordering, or guest code races.
- **No atomic emulation today.** `lwarx`/`stwcx.` (load-reserve/store-conditional) currently fall
  back to the interpreter and assume a single CPU — "reservation state from CPU object"
  (`OPTIMIZATION-PLAN.md:438`). Real SMP needs real reservation/atomic emulation across cores.
- **Single-CPU assumptions throughout.** SMC invalidation, the shared JIT code cache + its W^X
  toggling, low-memory globals, and the nanokernel are all written for one CPU.
- **The cooperative world is indivisible.** The Blue/Process-Manager environment is one logical
  thread; there is no correct way to run two parts of it at once. Only the nanokernel's *preemptive
  MP tasks* are guest-sanctioned concurrent units (see Tier 3).

**Design rule for everything below:** offload only work with a **clean input/output boundary and
explicit synchronization** back to guest state. Never speculatively run guest execution concurrently
outside Tier 3's controlled model.

---

## What's already parallel (the honest baseline)

- **Timer thread** — 60.15 Hz VBL pacing (`tick_func`, `main_unix.cpp:1603`, spawned `:1242`).
- **NVRAM thread** (`:1248`), **video redraw thread**, **audio** (SDL callback), **ethernet/serial**
  helpers. These are **host** threads doing emulator I/O; the macOS scheduler already places them on
  separate cores. They do **not** run guest task contexts.

So the literal "put sound/drawing on another core" is largely *already happening*. The opportunities
are (a) doing it *better* (QoS, less main-thread coupling) and (b) moving *new* categories of work
off the emulation core.

---

## Tier 0 — Scheduling / QoS hints (quick win)

**Effort: Low · Payoff: Low–Med · Risk: Low**

macOS doesn't offer hard core pinning; it steers work via **QoS classes** (which effectively map to
P-cores vs E-cores on Apple Silicon). Concrete, cheap moves:
- Mark the **emulation thread** `USER_INTERACTIVE` (keep it hot on a P-core).
- Mark **audio** real-time / `USER_INTERACTIVE` (glitch-free), **redraw** `USER_INTERACTIVE`.
- Mark **background compilation / NVRAM / housekeeping** `UTILITY`/`BACKGROUND` (let them ride
  E-cores). R9 already notes "use GCD QoS to pin compilation to E-cores" (`OPTIMIZATION-PLAN.md:623`).

Measure with the **B1 execution-weighted profiler** before/after; QoS is a knob, not a guaranteed
win. This is the "dedicate certain things to certain cores" idea in the form macOS actually supports.

---

## Tier 1 — Emulator-internal parallelism (the real ROI)

**Effort: Med · Payoff: High · Risk: Low–Med**

Pure or near-pure work with no guest-coherence problem — the right place to spend cores. Most is
already specced in `OPTIMIZATION-PLAN.md`; this plan unifies them as the parallelism story:

- **Background JIT compilation (R9 / C5, Cemu/Dolphin tiered model).** The emulation thread runs a
  baseline/interpret path for cold blocks while a **worker thread compiles optimized ARM64** on
  another core; hot blocks swap in when ready. Compilation is *pure* (guest bytes → native code), so
  it parallelizes cleanly. Depends on **R8 dual W^X mapping** (`:615`) so one thread can write code
  while another executes. **This is the single highest-value multi-core lever** and directly helps
  games (compile spikes stop stalling the hot loop).
- **Metal framebuffer compositing (R10, `:627`).** Move scaling/compositing/format-conversion to the
  GPU + a dedicated thread, off the emulation core.
- **Async device emulation.** Push more device work (sound mixing, network packet processing) fully
  off the emulation thread with lock-free handoff queues, beyond today's helper threads.
- **Speculative/AOT block compilation & profiling** on spare cores (compile likely-next blocks ahead
  of demand). Lower priority; gated on R9 infrastructure.

These need **no** MMU/nanokernel/MP work and **no** cross-core guest coherence — they're the safe
wins. Start here.

---

## Tier 2 — Offload HLE'd subsystems to worker threads

**Effort: Med–High · Payoff: Med · Risk: Med**

Once a hot OS routine is **HLE'd** (replaced with a native implementation — see the Selective-HLE
section, `OPTIMIZATION-PLAN.md:634`), that native code *can* run on a worker thread instead of inline
on the emulation core — **if** it has a clean boundary. Candidates: bulk blitting/`QuickDraw`
(extends C3 acceleration), sound mixing, (de)compression, memory fills/moves. Discipline:
- Only routines with explicit inputs/outputs and no mid-routine guest callbacks.
- Synchronize results back to guest memory with barriers before returning control.
- The win is real for *throughput-bound* subsystems (the FP/AltiVec media + scalar-copy cliff the
  earlier perf analysis flagged as where speed still bites even on fast hardware).

This is the bridge between "emulator-internal" (Tier 1) and "guest-visible" (Tier 3): the work is
native, not guest code, but it's doing the guest's job on another core.

---

## Tier 3 — True guest SMP (MP tasks on separate cores) — the gated holy grail

**Effort: Very high · Payoff: Med (long tail) · Risk: Very high**

The strongest form of "dispatch parts of the kernel to different cores": run the guest's own
**preemptive Multiprocessing-Services (MP) tasks** on separate host threads/cores, instead of
time-slicing them on one emulated CPU.

**Why MP tasks are the *only* safe guest granularity:** the cooperative Blue world is indivisible,
but MP tasks are units the nanokernel *itself* treats as concurrently schedulable. So they're the
one place guest-level core offload is even semantically defensible.

**What it requires (all of it):**
1. **The entire supervisor-fidelity stack** — specifically sub-plan C of
   [`MMU-NANOKERNEL-MP-PLAN.md`](MMU-NANOKERNEL-MP-PLAN.md), which itself needs the faithful
   nanokernel exception/interrupt/decrementer model (that doc's sub-plan B). Without a real
   preemptive nanokernel, there are no MP tasks to place on cores.
2. **A second emulated-CPU context per host thread** — each running its own JIT/interpreter against
   the shared guest memory.
3. **Cross-core memory coherence** — real `lwarx`/`stwcx.` reservations across threads, PPC→ARM64
   barrier mapping, and SMC/code-cache safety under concurrent execution. This is its own substantial
   project, largely independent of the MMU.

**Honest payoff:** a **long tail.** Most classic Mac software — including nearly all games — is
single-threaded cooperative and spawns no MP tasks, so it gains nothing here. The beneficiaries are
the specific pro/media apps that used MP Services. **Tier 3 is the glamorous version of the user's
idea, but Tiers 0–2 deliver more to the workloads people actually run.**

---

## How this connects to the MMU / nanokernel / MP plan

- **Tier 3 *is* the multi-core extension of that plan's sub-plan C** (preemptive MP tasks) — run the
  tasks on cores rather than time-sliced on one CPU. It is fully **gated** on that plan's sub-plan B
  (nanokernel/exception fidelity) being real.
- **The MMU (that plan's sub-plan A) is mostly orthogonal** to parallelism. Identity mapping is fine
  for Tiers 0–2 and even for Tier 3's task placement; the cross-core *coherence* problem (atomics +
  barriers) is a separate concern that exists with or without a real MMU.
- **Tiers 0–2 need none of that plan** — they're pure emulator-internal parallelism and can proceed
  independently and immediately.

So the relationship is: **Tiers 0–2 = standalone perf work (start now); Tier 3 = the payoff that the
supervisor-fidelity plan, if it ever lands, would unlock — plus a coherence project on top.**

---

## Recommended sequencing & first step

1. **Tier 0 now, measured.** Add QoS classes; A/B with the B1 profiler on a heavy workload (a game,
   a large app). Cheap, reversible, and it quantifies how much the emulation thread is currently
   losing to housekeeping contention — which sizes the whole effort.
2. **Tier 1 next — land R8 (dual W^X) then R9 (background compilation).** Highest ROI; directly
   smooths the compile-spike stalls games feel. R10 (Metal compositing) in parallel.
3. **Tier 2 opportunistically**, as specific throughput-bound subsystems get HLE'd.
4. **Tier 3 only** after the supervisor-fidelity plan delivers MP fidelity *and* a concrete MP
   workload is shown (the MMU/nanokernel/MP plan's "MP demand probe") to need it. Treat the
   cross-core coherence work as its own gated project.

**Cheapest decisive probe (do before Tier 1):** with the B1 profiler, measure how much wall-clock the
emulation thread spends *blocked on or interleaved with* compilation and device work on a heavy
workload. If it's large, Tier 1 is clearly worth it; if small (the host is already fast enough — see
the fast-hardware analysis in `BASILISKII-CROSS-POLLINATION.md`), even this may be lower priority
than Track A/C.

---

## References

- [`MMU-NANOKERNEL-MP-PLAN.md`](MMU-NANOKERNEL-MP-PLAN.md) — supervisor fidelity; Tier 3's prerequisite.
- `docs/planning/OPTIMIZATION-PLAN.md` — **R8** dual W^X (`:615`), **R9** async background JIT +
  GCD-QoS-to-E-cores (`:621`), **R10** Metal compositing (`:627`), **Selective HLE** (`:634`),
  `lwarx`/`stwcx.` reservation gap (`:438`); **B1 profiler** gates measurement.
- `docs/planning/sheepshaver-research/BASILISKII-CROSS-POLLINATION.md` — the fast-hardware framing
  (why throughput matters mainly for games/media, the workloads this plan targets).
- `docs/planning/JIT-STYLE-DECISION.md` — "simple by default; complexity by proof" — the bar Tier 3
  must clear.
- `SheepShaver/src/Unix/main_unix.cpp:1242/1248/1603` — existing host helper threads;
  `kpx_cpu/src/cpu/vm.hpp:208` — flat memory model.
