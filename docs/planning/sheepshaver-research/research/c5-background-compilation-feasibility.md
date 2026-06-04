# C5 — Background JIT compilation: internal feasibility audit

> **Status:** 📖 Reference / archive · **Created:** 2026-06-02 · **Updated:** 2026-06-04
> **Why this doc exists:** Internal feasibility/thread-safety audit for background JIT compilation (backlog C5).
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../../CONTRIBUTING.md) → "Documentation Lifecycle")._


**Scope:** what in *our* SheepShaver PPC→ARM64 JIT is thread-unsafe today, and what a
worker-thread compilation design would require. Read-only audit; companion survey agent
covers how other emulators (RPCS3 / Cemu / Ryujinx) do it. Pairs with backlog item C5,
`c4-wx-dual-mapping-spike.md`, and the C1 residency root-cause doc.

All file:line references are from the working tree on 2026-06-02 (branch `macos-arm64`) and
will drift — the ppc-jit.cpp / ppc-cpu.cpp files have uncommitted work by another agent.

---

## 1. Current threading model

SheepShaver is **already a multi-threaded process** — the single-threadedness that matters
for C5 is narrower: only one thread runs guest PPC code *and* the JIT compiler. The threads:

| Thread | Created at | Role | Touches JIT state? |
|---|---|---|---|
| **emul_thread** (main thread) | `main_unix.cpp:1254` — `emul_thread = pthread_self()`, then `emul_func()` at 1256 | Runs the PPC CPU loop (`powerpc_cpu::execute`, ppc-cpu.cpp), compiles AND executes JIT blocks, pumps SDL events via `HandleInterrupt`→`SDL_PumpEvents` | **YES — sole reader and writer today** |
| **tick_thread** | `main_unix.cpp:1213` (`tick_func`) | 60 Hz VBL: calls `TriggerInterrupt` → sets `spcflags` mask under spinlock; the emul_thread polls/services it | No JIT state; sets `spcflags` (lockless read by CPU loop) |
| **nvram_thread** | `main_unix.cpp:1219` (`nvram_func`) | NVRAM watchdog, 1 Hz flush | No |
| **redraw_thread** | `video_sdl2.cpp:1665` via `SDL_CreateThread` (`redraw_func`, loop at 3325) | Periodic framebuffer present (`present_sdl_video`) | No (reads guest framebuffer memory only) |
| **exc_thread** | `sigsegv.cpp:3152` (`handleExceptions`, detached) | Mach exception server: catches guest SIGSEGV/SIGBUS for fastmem/DSI recovery | No JIT cache writes, but can redirect the emul_thread's PC |

**Confirmation of the single-threaded-JIT claim (LEARNINGS "Task 2a"):** the compile→execute
cycle is strictly serial on one thread. `ppc-cpu.cpp:767` compiles a block
(`ppc_jit_aarch64_compile`) and *immediately* calls into it (`fn((void*)regs_ptr())` at 770)
on the same thread. Every W^X write bracket closes (`jit_cache_end_write`, re-protect +
icache invalidate) *before* any call into compiled code, and the block-cache publish
(`jit_bc_insert`, ppc-jit.cpp:4190) runs *after* `jit_cache_end_write` (4128) — all on the
emul_thread. No other thread reads or writes `jit_bc_*`, `chain_site_*`, `jit_cache_wp`, or
the counters. The claim holds.

**Implication for C5:** the process is *already* concurrent, so the memory-ordering and
per-thread-W^X questions below are not exotic — they are simply not yet exercised against the
JIT cache because no concurrent thread touches it. C5 introduces exactly one new actor: a
**compile worker thread** that writes the cache and publishes blocks while the emul_thread
reads/executes them.

---

## 2. Shared-state inventory (state × readers × writers × race consequence)

Today every row has **one reader and one writer, both the emul_thread**. The "race
consequence" column describes what breaks *if a worker thread compiles concurrently* with no
added synchronization.

| State (ppc-jit.cpp) | Reader(s) | Writer(s) | Race consequence under a naive worker |
|---|---|---|---|
| `jit_bc_heads[]` / `jit_bc_pool[]` / `jit_bc_pool_next` (block cache; :110-112) | `jit_bc_lookup` (:227), `jit_bc_invalidate_pc` (:207), `ppc_jit_aarch64_has_block` (:3816) | `jit_bc_insert` (:238), `jit_bc_flush` (:125) | **Torn entry / chain corruption.** `jit_bc_insert` writes 5 fields (pc, code, chain_code, complete, n_insns) then links the bucket head (:267-268). A concurrent `jit_bc_lookup` walking the bucket can observe a head pointer that links to a half-initialized pool slot, or a stale `next`. `jit_bc_pool_next++` (:261) is a non-atomic RMW → two writers (or torn publish) clobber a slot. **Use-after-publish:** lookup may return `code` whose bytes/icache aren't yet visible to the reader's core (see §3). |
| `chain_site_pool[]` / `chain_site_heads[]` / `chain_site_pool_next` (:97-99) and `patch_chain_sites()` (:180) — **the hard one** | `patch_chain_sites` | `record_chain_site` (:167), `patch_chain_sites` | **Mutates already-executable code:** `*site->patch_loc = B<off>` (:197) overwrites a live block's epilogue while another thread may be executing that exact block. Cross-thread SMC. **HOWEVER — dormant today:** `JIT_BLOCK_CHAINING=0`, so `record_chain_site` is never called and `patch_chain_sites` early-returns (:182-187). In the smallest viable C5 design this race **does not exist**. It only returns if chaining is ever re-enabled. |
| `jit_cache_wp` (write pointer; :37) and the cache-full flush | bounds-check (:3969) | bump at `code_start = jit_cache_wp` / `jit_cache_wp = jit_code_ptr` (:3979, 4129); reset by `ppc_jit_aarch64_flush` (:3807) | **Bump race:** two compilers racing `jit_cache_wp` overwrite each other's code region. With a single worker as sole writer this is moot. **Flush hazard (distinct, serious):** `ppc_jit_aarch64_flush` resets `jit_cache_wp` to base and calls `jit_bc_flush` (nulls all entries) — if the emul_thread is mid-execution of a block whose bytes are about to be overwritten, or holds a `code` pointer just nulled, that's a cross-thread **use-after-free of executable memory**. The in-line cache-full flush (:3973-3975) has the same hazard. |
| Counters: `jit_blocks_attempted`, `jit_blocks_complete`, `jit_cum_fail_*` (:4138-4183) | report path | compile path | Benign: lost-update on statistics only. Make relaxed-atomic to avoid UB; no correctness impact. |
| **W^X toggle state** — `pthread_jit_write_protect_np(0/1)` (jit-target-cache.hpp:35-41) | n/a (CPU/MMU state) | both write sites | **Per-thread.** A worker that calls `pthread_jit_write_protect_np(0)` makes the region writable *on the worker only*; the emul_thread's view is unaffected. So a worker *can* keep its own view permanently writable while the emul_thread keeps it executable — but this is fragile (see §4). The toggle is *not* a shared race; it is a per-thread-view-management problem. |
| `jit_rom_base/size/host`, `jit_ram_*_cached` (:147-149, 3826) | compile, `is_compilable` | set once at init / first compile | Set-once / publish-once. Need a one-time release barrier; otherwise benign. |

---

## 3. Execution-side contract & ARM64 code-publication requirements

**What the emul_thread assumes today** when `jit_bc_lookup` returns an entry and it branches
into `entry->code`: that the entry is fully initialized, that the code bytes are completely
written, and that the instruction cache is coherent. All three hold today because the *same
thread* wrote the code, ran `sys_icache_invalidate` (which ends in `ISB`), and then published
the pointer — program order on one core guarantees visibility.

**Could a half-inserted block be executed?** Today no (same thread, publish-last). Under a
worker with no barriers: **yes** — the classic two failures:

1. **Stale data / torn pointer:** the consumer's core sees the published `code` pointer (or
   bucket head) before it sees the 5 struct-field stores or the emitted code bytes. The fix
   is acquire/release: the worker does a **release-store** when publishing the bucket head
   (`jit_bc_heads[bucket]`), and the consumer does an **acquire-load** of the head in
   `jit_bc_lookup`. Equivalent: a seqlock around insert/lookup. This orders the *data*
   stream.

2. **Stale instruction stream (the ARM64-specific one):** the ARM ARM "concurrent
   modification and execution of instructions" rules require that when thread A writes
   instructions and thread B executes them, *both* PEs participate in the coherence:

   - **Producer (worker):** after writing code bytes — `DC CVAU` (clean dcache to PoU) for
     each line, `DSB ISH`, `IC IVAU` (invalidate icache to PoU) for each line, `DSB ISH`.
     On macOS this is exactly what `sys_icache_invalidate()` does
     (jit-target-cache.hpp:38-41); the helper also issues the trailing `ISB`, but that ISB
     runs **on the worker's core**, which does not help the consumer.
   - **Consumer (emul_thread):** must issue its **own context synchronization (`ISB`)** after
     it acquire-loads the new block pointer and before it branches into the new code. A
     freshly-allocated VA may *appear* to work without it, but it is spec-required and the
     omission is a real, rare, hard-to-debug fault. Today this is invisible because the
     producer and consumer are the same core (the producer's ISB suffices).

   **Net new requirement for C5:** the CPU thread's dispatch must become
   *acquire-load block pointer → ISB → branch* for any block it did not itself compile. The
   cheapest correct form: an `ISB` immediately before the indirect call when the block was
   obtained from the shared cache (a published flag distinguishes worker-produced from
   self-produced, or just always ISB on the lookup-hit path — one ISB per block dispatch is
   cheap relative to a block).

`DMB ISH` alone is insufficient for the instruction stream — it orders data accesses, not the
icache/pipeline; the `IC IVAU` + `DSB` (producer) and `ISB` (consumer) are the load-bearing
operations.

---

## 4. W^X cross-thread design options

Two viable mechanisms; the C4 spike already decided which is cleaner.

**Option A — keep MAP_JIT, exploit per-thread `pthread_jit_write_protect_np`.**
The toggle is per-thread (verified, jit-target-cache.hpp doc + LEARNINGS Task 2a). Design: the
worker calls `pthread_jit_write_protect_np(0)` once at startup and *never* re-protects, so its
view of the MAP_JIT region is permanently RW; the emul_thread never toggles, so its view stays
RX. Each thread sees the same physical pages through its own per-thread protection state.

- **Does this actually work with MAP_JIT?** In principle yes — the per-thread W^X state is
  exactly designed to let different threads hold different access to the same JIT region. But
  it is **fragile in our codebase**: today *both* write sites bracket with
  `begin_write`/`end_write`, and any code path that runs on the worker but assumes the
  emul_thread's "executable" invariant (or vice-versa) silently breaks. It also forbids the
  worker from ever executing emitted code (its view is non-executable) — fine for a
  pure-compile worker, but a footgun. Apple documents per-thread W^X but this exact
  "permanently-writable worker" pattern is not a blessed idiom; it would need its own spike.

**Option B — C4 dual mapping (`vm_remap`), process-wide RW alias + RX alias.**
The C4 spike **proved this works on this machine (macOS 26.4.1 arm64), is rewrite-coherent,
needs no MAP_JIT and no `pthread_jit_write_protect_np`, and is ~27% faster per write-batch.**
The worker writes through the RW alias (`w_ptr`); the emul_thread executes through the RX
alias (`x_ptr`); no toggling on either thread. Crucially the spike flagged that dual mapping
is **process-wide, not per-thread** — explicitly noting this is *"friendlier to a
multi-threaded JIT."* That is precisely the C5 use case.

**Verdict:** **Option B (C4 dual mapping) is the clean substrate for cross-thread
compilation.** It removes the per-thread-view fragility entirely: any thread may write via the
RW alias, any thread may execute via the RX alias, with no per-thread state to get wrong. The
ARM64 publication requirements of §3 still apply unchanged (the dcache/icache coherence and
consumer ISB are orthogonal to which alias you write through). **C4 should land before C5** —
doing C5 on the per-thread-toggle substrate means building on a fragile idiom that C4 deletes.

---

## 5. Handoff-point design in ppc-cpu.cpp

Where the emul_thread would (a) post compile requests and (b) pick up completed blocks. Both
compose with the C1 gate restructure.

**(a) Posting a compile request — at the two JIT-miss points.** Today `ppc-cpu.cpp:767` and
`:810` call `ppc_jit_aarch64_compile(pc, …)` *synchronously* (stall-to-compile). Under C5:

- Replace the synchronous compile with a **read-only lookup** (`jit_bc_lookup`, already exists
  via `ppc_jit_aarch64_has_block`). On hit → execute (with the §3 ISB). On miss → **enqueue
  the PC** on the SPSC request queue (non-blocking) and **fall through to the interpreter** for
  this PC right now. The guest never stalls on compilation; the interpreter covers the cold
  PC while the worker compiles it in the background.
- This is the *same* read-only-lookup shape C1 Step 2 introduces at the line-848 transition
  ("prefer a compiled block at every transition"). C1 and C5 share the lookup primitive: C1
  makes the CPU thread *consult* completed blocks at every transition; C5 makes the *filling*
  of those blocks asynchronous. **C1 is a hard prerequisite** — until the CPU thread actually
  re-enters compiled blocks (instead of staying interpreter-cache-resident, the dual-cache
  trap), background-compiled blocks would be produced and then ignored, exactly as the backlog
  notes ("Gated on C1").

**(b) Picking up completed blocks — implicitly, via lookup.** No separate "collection" step is
needed: the worker publishes into `jit_bc_heads[]` (release-store); the next time the CPU
thread reaches that PC and does its acquire-load lookup, the block is simply there. The only
added cost on the hot path is the acquire-load + one ISB on a hit (§3). De-dup: the worker (or
the enqueue site) checks "already requested / already present" so a hot cold-PC isn't enqueued
hundreds of times before the worker drains it (a small "pending" marker in the bc entry or a
bounded dedup set).

**Interaction with the line-848 transition (C1):** C1's read-only check at the
interpreter-cache-hit continue path becomes the natural enqueue site too — if the lookup misses
*and* the PC is compilable (`ppc_jit_aarch64_is_compilable`, used at :858), post a request.
One code path serves both "prefer compiled block" (C1) and "request compilation of hot
interpreted code" (C5).

---

## 6. Smallest viable design + prerequisites

**Design (single worker, SPSC, no chaining):**

1. **One compile worker thread.** The emulated PPC is single-stream; one extra core that only
   *compiles* is the entire win. Created alongside the other helper threads in
   `main_unix.cpp`.
2. **Lock-free SPSC ring** of `uint32_t` PCs: producer = emul_thread (enqueue on JIT miss),
   consumer = worker. Bounded; full → drop the request (the PC stays interpreted, gets
   re-requested later — degrade, don't stall). A small dedup marker avoids flooding.
3. **Worker compiles** via the existing `ppc_jit_aarch64_compile` body, writing through the
   **C4 RW alias**, then publishes the bc entry with a **release-store** on the bucket head.
4. **Emul_thread dispatch** does an **acquire-load** lookup; on a worker-produced hit, **`ISB`
   then branch** (§3). On miss, enqueue + interpret.
5. **Chaining stays disabled** (`JIT_BLOCK_CHAINING=0`). This removes `patch_chain_sites`'
   cross-thread "mutate live executable code" race entirely — the single hardest problem is
   simply *not in scope*.
6. **Flush is CPU-thread-only, at a safepoint.** The worker **never** flushes. Cache-full and
   explicit flush run only on the emul_thread, and only when the worker is known-idle (drain
   the queue / quiesce the worker first), so no block the emul_thread is executing is pulled
   out from under it. With a 64 MB cache, flushes are rare (LEARNINGS: stabilizes after the
   initial burst), so a stop-the-worker safepoint is acceptable.
7. **Counters → relaxed atomics** (avoid UB; no correctness weight).

**Must land first (in order):**
- **C1 (gate restructure)** — *hard* prerequisite. Without it, compiled blocks (foreground or
  background) are ignored due to the dual-cache re-entry trap; C5 would compile into a void.
- **C4 (dual mapping)** — *strong* prerequisite. It is the clean cross-thread W^X substrate;
  building C5 on the per-thread-toggle idiom (Option A) means building on a fragile pattern C4
  deletes. C4 is already proven working on this machine.

**Explicitly deferred:** block chaining (and therefore cross-thread `patch_chain_sites`). Only
revisit if a designed interrupt strategy re-enables chaining — at which point chain patching
must be deferred to the CPU thread at a safepoint, never done from the worker.

---

## 7. Effort and risk

**Effort (assuming C1 + C4 landed):** roughly **2–4 focused days** for the smallest design.
- SPSC ring + worker thread lifecycle (create/join, drain-on-flush): ~0.5 day.
- Enqueue-on-miss + interpret-fallback rewiring at the two ppc-cpu.cpp JIT sites: ~0.5 day
  (mostly already shaped by C1).
- Release/acquire publication in `jit_bc_insert`/`jit_bc_lookup` + consumer ISB: ~0.5 day.
- Safepoint flush coordination (quiesce worker): ~0.5 day.
- Bring-up/debug against the opcode harness (must stay 100) and a JIT boot to desktop: ~1 day.
  Note the opcode harness is single-process/single-vector and **cannot exercise the worker**
  (LEARNINGS): boot-level testing is the only real coverage, mirroring the "compiles fine,
  executes wrong, only visible in whole-system boot" class already documented for chaining.

**Risk:**
- **Medium-high if C4 is skipped** (per-thread W^X fragility) — mitigated by doing C4 first.
- **Medium: the consumer-ISB omission** — works in testing, faults rarely in the field; easy
  to forget. Make it a single, commented, unconditional ISB on the shared-lookup-hit path.
- **Medium: flush/use-after-free** — the worker-never-flushes + safepoint rule is simple but
  must be airtight; a debug-gated BRK tripwire on nulled blocks (backlog B4) would catch
  violations loudly.
- **Low payoff ceiling honesty:** the win is bounded by how much compile time is actually on
  the critical path. LEARNINGS shows JIT compilation is a *burst* at boot (~66k blocks in the
  first ~15s) that then stabilizes; background compilation mainly improves *boot-time
  responsiveness* (the guest interprets instead of stalling during that burst), not steady
  state. Quantify the foreground-compile stall with a counter (how many guest-microseconds/sec
  are spent inside `ppc_jit_aarch64_compile`) **before** committing — if it is small after C1,
  C5's value is small.

---

## Executive summary

1. **The JIT is genuinely single-threaded today** — only the emul_thread (main thread)
   compiles and executes; tick/nvram/redraw/Mach-exception threads exist but never touch JIT
   state. The process is already concurrent, so cross-thread memory ordering is a live concern,
   just not yet exercised against the cache.
2. **Publication is safe today by program order:** code is written → `sys_icache_invalidate`
   (ends in ISB) → block published, all on one core. A worker breaks this.
3. **Two real cross-thread races** in the smallest design: torn `jit_bc_insert`/`jit_bc_lookup`
   (fix: release/acquire on the bucket head, or seqlock), and the cache-flush
   use-after-free (fix: worker never flushes; flush only at a CPU-thread safepoint).
4. **The "hard one" — `patch_chain_sites` mutating live executable code — is DORMANT** because
   `JIT_BLOCK_CHAINING=0`. Keep chaining disabled and that entire race is out of scope.
5. **ARM64 requires a consumer-side `ISB`:** the CPU thread must acquire-load the new block
   pointer, then ISB, then branch. The worker's `sys_icache_invalidate` (DC CVAU/DSB/IC
   IVAU/DSB/ISB) handles the data+icache, but its ISB runs on the wrong core.
6. **W^X: per-thread toggle (Option A) works in principle but is fragile;** the C4 dual-mapping
   `vm_remap` substrate (proven working, ~27% faster, *process-wide*) is the clean answer —
   worker writes RW alias, CPU executes RX alias, no toggling.
7. **Handoff:** at the two JIT-miss sites in ppc-cpu.cpp, replace synchronous compile with
   read-only lookup → on miss enqueue PC (SPSC) + interpret now; pickup is implicit via the
   next acquire-load lookup. Same lookup primitive C1 introduces.
8. **Prerequisites (ordered): C1 is a hard gate** (else compiled blocks are ignored — the
   dual-cache trap); **C4 is a strong gate** (clean W^X substrate).
9. **Smallest viable design:** one worker, lock-free SPSC PC queue, release/acquire block
   publication, consumer ISB, chaining stays off, flush is a CPU-thread safepoint, counters →
   relaxed atomics.
10. **Effort ~2–4 days after C1+C4; payoff is mainly boot-time responsiveness, not steady
    state** — measure foreground-compile stall (µs/sec in `ppc_jit_aarch64_compile`) before
    committing; testing is boot-level since the opcode harness can't exercise a worker.
