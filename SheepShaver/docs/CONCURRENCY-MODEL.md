# SheepShaver concurrency model — why this fork is *not* multicore-sensitive

> **Status:** 📖 Reference · **Created:** 2026-06-06 · **Updated:** 2026-06-06
> **Why this doc exists:** "Is SheepShaver multicore-sensitive? Should I pin it to one core? Is the
> JIT thread-safe?" comes up repeatedly. This is the canonical, code-grounded answer — and the one
> real caveat that matters for any future multithreading work. Cite this instead of re-litigating it.

## The short answer

**This fork's runtime is not multicore-sensitive, and you should not pin it to one core.** It is a
**single-guest-CPU** emulator: guest code runs on exactly one host thread, and a small set of host
*helper* threads do I/O around it through explicit, correct synchronization. There is no guest SMP,
no concurrent guest execution, and therefore no class of guest-visible data race for the host core
count to expose.

The "pin SheepShaver to a single core for stability" advice you'll find in forums is about **older /
upstream builds** (most plausibly host-scheduler thrash or platform-specific atomics on long-dead
configs), **not** this AArch64 fork. Don't carry that cargo-cult into our design discussions.

There is exactly **one** caveat — the JIT code cache is single-writer *by construction, not by lock*
— and it is a property to **preserve**, not a bug to fix. See [§The one real caveat](#the-one-real-caveat).

## Execution model: one guest CPU = one host thread

The guest is the cooperative classic-Mac-OS "Blue" world — a single thread of control. SheepShaver
runs it on one thread (`emul_thread` / `emul_func`, `main_unix.cpp`). The JIT and interpreter only
ever execute from inside `powerpc_cpu::execute()`, guarded by `s_active_cpu`
(`ppc-cpu.cpp:151`: *"the JIT only ever runs from inside execute(), single-threaded, on one cpu
object across nested execute_depth calls"*). **No second thread ever runs guest code or compiles
into the JIT cache.** That single fact is the root of everything below.

## The host helper threads, and how each shared touchpoint is synchronized

Everything else SheepShaver spawns is a *host* helper doing emulator I/O. Each one's contact with
guest-visible state is explicitly synchronized — so the OS scheduler is free to place them on any
cores it likes. *(Line numbers approximate — these files change; cite the mechanism.)*

| Helper thread | Shared touchpoint | Synchronization (mechanism) |
|---|---|---|
| 60 Hz timer (`tick_func`) | `spcflags` (sets the interrupt-trigger bit) | **`std::atomic<uint32>`** with `fetch_or`/`fetch_and`, release/relaxed ordering — `spcflags.hpp` (the "P0d" lock-free spcflags) |
| 60 Hz timer | `InterruptFlags` | **`atomic_or`/`atomic_and`** (`main_unix.cpp` `SetInterruptFlag`/`ClearInterruptFlag`) |
| 60 Hz timer → CPU | interrupt *delivery* | **async-signal-safe `pthread_kill(emul_thread, SIGUSR2)`** guarded by `ready_for_signals` (`TriggerInterrupt`, `main_unix.cpp`) — no lock, signal-safe by construction |
| Video redraw | guest framebuffer (`the_buffer`) | **`pthread_mutex` / spinlock** (`frame_buffer_lock`, `video_x.cpp`) — CPU writes / redraw reads under the lock, so no tearing |
| NVRAM watchdog | XPRAM low-mem | periodic `memcmp` + save; touches no hot guest state |
| Audio (SDL callback) | audio device | SDL-managed callback lock; not main guest memory |

The CPU thread *reads* `spcflags` on the JIT's block-entry poll (`emit_entry_spcflags_poll`); the
timer thread *writes* it atomically. That producer/consumer pair is the only hot cross-thread
interaction, and it is lock-free and correctly ordered. **The helpers race nothing.**

## Why "pin to one core" doesn't apply here

The recurring claim is "the JIT is multicore-sensitive." For this fork it's false, and specifically:

- Guest execution is single-threaded, so there is **no guest memory race** for more cores to surface.
- The one shared hot variable (`spcflags`) is `std::atomic` (this was deliberate work — the "P0d"
  change replaced an earlier spinlock; see `OPTIMIZATION-PLAN.md` §0d).
- Interrupt delivery is a signal, not shared mutable state read in a race.
- There is **no thread-affinity / `sched_setaffinity` / core-pinning code anywhere** — the design
  doesn't depend on core placement, so it neither needs nor benefits from pinning.

If a *specific* build ever shows instability that pinning "fixes," treat that as a **bug to root-cause
in our code**, not as confirmation of the folklore — and update this doc with the finding.

## The one real caveat

**The JIT code cache is single-writer by *assumption*, not by lock.** Because compilation only ever
happens on `emul_thread`, there are intentionally **no locks** on:
- the cache write pointer `jit_cache_wp` (`ppc-jit.cpp`),
- the block-hash allocator `jit_bc_pool_next` and its bucket lists,
- the chain-site pool,
- and the W^X toggle, which on macOS is `pthread_jit_write_protect_np` — **per-thread** state.

This is **correct and optimal for a single writer** — it is not a latent bug. But it is the precise
reason that **adding a second thread that writes the JIT cache (e.g. background compilation, R9) is
not free**. Before any such thread is safe, in this order:

1. **R8 dual-W^X mapping** — replace the per-thread `pthread_jit_write_protect_np` toggle (which would
   collide between threads, flipping one thread's region to executable mid-write) with separate
   RW/RX address aliases.
2. **Atomic / locked cache allocation** — CAS or mutex around `jit_cache_wp` and `jit_bc_pool_next`
   so two compilers can't overwrite each other.
3. **Cross-thread invalidation** — SMC/`icbi` invalidation must reach a thread executing stale code.

This is the mechanical, code-grounded reason the multithreading dependency is **R8 → R9**, and it
lives in detail in [`../../docs/planning/MULTICORE-OFFLOAD-PLAN.md`](../../docs/planning/MULTICORE-OFFLOAD-PLAN.md)
(the "Concurrency baseline & Tier-1 prerequisites" section).

## FAQ

- **Should I pin SheepShaver to one core?** No. The design doesn't depend on core placement.
- **Is the JIT thread-safe?** The guest path is single-threaded *by design*; the cache has no locks
  *because it has exactly one writer*. Don't add a second writer without the R8→R9 prerequisites above.
- **Did the community "multicore sensitivity" reports apply to us?** No — older/upstream builds.
- **So adding worker threads is dangerous?** No — *I/O* helper threads (disk, net, blit, audio) are
  fine and partly already exist; they don't write the JIT cache. Only a second *compiler* thread hits
  the caveat.

## References

- `docs/planning/MULTICORE-OFFLOAD-PLAN.md` — the parallelism plan; this model is its foundation.
- `OPTIMIZATION-PLAN.md` §0d (atomic spcflags), R8 (dual W^X), R9 (background compile).
- Source of truth: `main_unix.cpp` (threads, `TriggerInterrupt`, `InterruptFlags`),
  `kpx_cpu/src/cpu/spcflags.hpp` (atomic spcflags), `ppc-cpu.cpp` (`s_active_cpu`),
  `ppc-jit.cpp` (cache write pointer / pools), `jit-target-cache.hpp` (per-thread W^X toggle),
  `video_x.cpp` (`frame_buffer_lock`).
