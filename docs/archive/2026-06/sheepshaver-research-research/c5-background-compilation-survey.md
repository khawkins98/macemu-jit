> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** historical research
>

# C5 — Background / Asynchronous JIT Compilation: Cross-Emulator Survey

> **Status:** 📖 Reference / archive · **Created:** 2026-06-02 · **Updated:** 2026-06-04
> **Why this doc exists:** Cross-emulator survey of background/async JIT compilation (backlog C5).
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../../CONTRIBUTING.md) → "Documentation Lifecycle")._


Survey of how other emulators and VMs move JIT compilation off the executing thread, written
to inform SheepShaver's backlog item **C5** (`IMPLEMENTATION-BACKLOG.md` §C5: use a second host
core to compile PPC blocks while the CPU thread keeps executing). C5 is gated on **C1** (the
dual-cache gate restructure, `c1-residency-root-cause.md`) and synergizes with **C4** (the
verified `vm_remap` dual-mapping W^X result, `c4-wx-dual-mapping-spike.md`).

**Sourcing note (read this first).** `WebSearch`/`WebFetch` were returning persistent HTTP 529
(server-side overload) for the entire research session, so every per-emulator finding below was
verified by reading the **actual source** via the GitHub API (`gh api …/contents/…`), not blog
posts. This is a strength, not a gap: e.g. the RPCS3 "compiling modules" progress bar is cited
from the `g_progr_pdone++` line in the real worker loop, which is more authoritative than a
writeup *about* the progress bar. The one section without fetched source — V8/HotSpot/JSC — is
explicitly labelled as the established canonical pattern (and is corroborated 1:1 by Ryujinx's
concrete code), and carries no fabricated URLs.

**Bottom line up front.** The model that fits SheepShaver is **Cemu's**: compile-on-miss with a
single background worker, a read-only function-pointer table the CPU thread checks at every block
boundary, `try_lock`-and-enqueue on a miss, lock-free compilation off-thread, and a short locked
critical section that checks an invalidation epoch and then installs the code pointer. C1's gate
restructure already builds the CPU-thread half of this for free ("prefer a compiled block at every
transition" via the read-only `jit_bc_lookup`). C5 is then almost entirely "move
`ppc_jit_aarch64_compile` to a worker thread." A call-counter trigger (Ryujinx-style) is a cheap
bolt-on to avoid wasting the worker on one-shot init code; an AOT ROM sweep (RPCS3/Rosetta-style)
is deferred to a warm-start follow-up. C4 (process-wide dual mapping) is effectively a
**prerequisite**, not merely a synergy, because the per-thread `pthread_jit_write_protect_np` model
has a genuine cross-thread W^X hazard.

---

## 1. Per-emulator findings (verified from source)

### 1.1 Cemu — compile-on-miss, single background worker (THE model for us)

**Source:** `cemu-project/Cemu`, `src/Cafe/HW/Espresso/Recompiler/PPCRecompiler.cpp` (MPL-2.0),
read at `main`. This is a PowerPC (Espresso/Broadway) → x86-64 / AArch64 recompiler — the closest
architectural analogue to SheepShaver of any emulator surveyed.

**Architecture (verbatim from the code):**

- **Shared state** is a single struct guarded by one `FSpinlock recompilerSpinlock`:
  ```cpp
  std::queue<MPTR> targetQueue;                       // miss PCs awaiting compile
  std::vector<ppcInvalidationRange> invalidationRanges;
  std::thread workerThread;                           // ONE recompiler thread
  ```
- **The handoff table** is `ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[address/4]` —
  a per-guest-instruction-word function pointer with three states: a sentinel
  `PPCRecompiler_leaveRecompilerCode_unvisited`, a sentinel `…_visited`, or a real compiled
  code pointer. The executing recompiled code and the interpreter both index this table.
- **CPU-thread miss path** — `PPCRecompiler_visitAddressNoBlock(enterAddress)`:
  1. Lock-free read: if the table slot is not `unvisited`, return immediately (already queued or
     compiled).
  2. `if (!recompilerSpinlock.try_lock()) return;` — **never blocks the CPU thread.** If the worker
     holds the lock, the CPU thread just keeps interpreting and will re-offer the PC next time.
  3. Re-check under lock, then `targetQueue.emplace(enterAddress)` and flag the slot `visited`.
- **Worker** — `PPCRecompiler_thread()`: pops a PC from `targetQueue` (under the spinlock), then
  calls `PPCRecompiler_recompileAtAddress()` which runs the *entire* compile (boundary tracking,
  IML gen, register alloc, backend emit) **with the spinlock released** — the expensive work does
  not block the CPU thread.
- **Install + invalidation race** — `PPCRecompiler_makeRecompiledFunctionActive()`: re-acquires
  the spinlock for a *short* critical section that (a) confirms the entry slot is still `visited`
  (not invalidated mid-compile), (b) walks `invalidationRanges` and **drops the freshly compiled
  function** if its address range was invalidated during the compile, and only then (c) writes the
  real code pointer(s) into `ppcRecompilerDirectJumpTable[…]`. Comment in the source:
  > "its possible that the range has been invalidated during the time it took to translate the
  > function … check if the current range got invalidated during the time it took to recompile it"
- **Invalidation** (guest self-modifying code / code reload) records a range in
  `invalidationRanges` under the lock; in-flight compiles touching that range are discarded at
  install time; entries are reset to `unvisited` so the interpreter re-queues them if still hot.

This is *exactly* model (a) compile-on-miss with the invalidation-epoch discipline, and it maps
1:1 onto our post-C1 structures (see §4). There is even a `PPCREC_FORCE_SYNCHRONOUS_COMPILATION`
compile flag that flips to inline-on-CPU-thread compilation, confirming async vs inline is a clean
seam in their design.

### 1.2 Ryujinx / ARMeilleure — tiered: inline baseline + background optimizer + persistent cache

**Source:** `alula/Ryujinx` mirror (the original `Ryujinx/Ryujinx` was taken down in 2024; this
is a source mirror), `src/ARMeilleure/Translation/Translator.cs`, `…/TranslatorQueue.cs`,
`…/RejitRequest.cs`, `…/PTC/Ptc.cs` (MIT). AArch32/AArch64 → x86-64/AArch64.

This is the **canonical tiered-compilation pattern** realized in a CPU emulator:

- **Tier 0 (baseline, inline):** first time a guest address is hit, `GetOrTranslate()` calls
  `Translate(address, mode, highCq: false)` — a fast **LowCq** translation compiled *synchronously
  on the executing thread*. Execution proceeds immediately.
- **Counter trigger:** every LowCq function's entry block is emitted with `EmitRejitCheck()`, an
  inline counter; after `MinsCallForRejit = 100` calls it invokes
  `NativeInterface.EnqueueForRejit(address)` → pushes a `RejitRequest` onto `Queue`
  (a `TranslatorQueue`). This is the "hot block" detector — one-shot init code never reaches 100.
- **Tier 1 (optimizing, background):** 1–4 `CPU.BackgroundTranslatorThread.N` threads
  (`threadCount = min(4, (ProcessorCount-6)/3)`, the last one at `ThreadPriority.Lowest`) run
  `BackgroundTranslate()`, which dequeues requests and calls `Translate(…, highCq: true)` — the
  full optimizing compile.
- **Install = atomic pointer swap:** `Volatile.Write(ref FunctionTable.GetValue(guestAddress),
  (ulong)func.FuncPointer)`. The dispatcher reads this table; the swap from LowCq→HighCq pointer
  is a single atomic store. The old function is retired via `EnqueueForDeletion` (deferred free).
- **Invalidation:** `InvalidateJitCacheRegion` calls `ClearRejitQueue(allowRequeue: true)` (stop
  the workers from rejitting a region being invalidated) then `Volatile.Write(…, FunctionTable.Fill)`
  to reset table slots.
- **PTC (Profiled Translation Cache):** `Ptc.cs` + `PtcProfiler.cs` persist *compiled native code*
  plus a profile of which addresses reached HighCq to disk, keyed by a title hash. On the next run
  the cache is reloaded and a background "PTC load" pass re-materializes HighCq functions before
  the game even starts — a **warm start**. This is the persisted-AOT idea, off-thread, across runs.

Takeaways for us: (i) the call-counter is the cheap, proven hot-block trigger; (ii) the install is
a plain atomic store of a function pointer into a table; (iii) PTC is the template if we ever want
warm-start ROM caching.

### 1.3 RPCS3 — parallel LLVM **module** compilation, AOT before execution (the progress bar)

**Source:** `RPCS3/rpcs3`, `rpcs3/Emu/Cell/PPUThread.cpp` (GPL-2.0), read at `master`. Cell PPU
(PowerPC) → LLVM → host. This is the famous "Compiling PPU Modules" progress bar.

- **Granularity: module-level, not block-level.** At game boot `ppu_initialize()` builds a
  `workload` of PPU *modules* (PRX/OVL/executable segments). Each `workload` entry is an entire
  module compiled by its own LLVM `jit_compiler` instance.
- **Parallelism: a work-stealing thread group.**
  ```cpp
  const u32 thread_count = std::min(::size32(workload), rpcs3::utils::get_max_threads());
  named_thread_group threads(fmt::format("PPUW.%u.", …), thread_count, thread_op(…), …);
  …
  for (u32 i = work_cv++; i < workload.size(); i = work_cv++, g_progr_pdone++) { … ppu_initialize2(jit2, part, …); … }
  threads.join();
  ```
  `work_cv` is an `atomic_t<u32>` cursor; each worker grabs the next module index atomically
  (work-stealing), compiles it with a **private** LLVM instance (`jit_compiler jit2`), and bumps
  `g_progr_pdone++` — **that increment is the progress bar.** `g_progr_ptotal`/`g_progr_pdone`/
  `g_progr_ftotal_bits` are the progress counters the UI reads.
- **Handoff = link-then-execute, not concurrent.** This is **AOT**: compilation is a *barrier*
  before execution, not concurrent with it. After `threads.join()`, a serial pass *links/loads*
  each compiled module (`jits[…]->add(cache_path + obj_name)`) and a `__resolve_symbols` resolver
  writes resolved function pointers into the shared executable dispatch table
  (`vm::g_exec_addr`). Only then does the PPU execution thread start. (RPCS3 also keeps an
  interpreter, but the headline path precompiles everything first.)
- **On-disk cache:** compiled modules are cached on disk keyed by hash, so the progress bar only
  appears on first run of a title (then it is a warm start, like Ryujinx PTC).
- **macOS note:** the workers bracket emission with `pthread_jit_write_protect_np(false/true)` —
  the same per-thread W^X primitive we use, applied on a worker. Relevant to our §5 W^X analysis.

Takeaway: RPCS3 is the **AOT-sweep** model (c), parallelized across many cores, run as a startup
barrier. It maximizes throughput and warm-start but does not overlap compile with execution.

### 1.4 Dolphin — **inline** on the CPU thread (no async), and why that is fine

**Source:** `dolphin-emu/dolphin`, `Source/Core/Core/PowerPC/Jit64/Jit.cpp` (GPL-2.0+). PowerPC
(Gekko/Broadway) → x86-64 (the JitArm64 backend is structurally identical).

- **No compile thread exists.** `Jit64::Jit(em_address)` is called straight from the dispatcher on
  a block-cache miss; it allocates a `JitBlock`, `DoJit()` emits it, and execution falls into it —
  all on the CPU thread.
- **Block chaining is in-asm:** compiled blocks `JMP` directly to each other
  (`asm_routines.dispatcher` / `dispatcher_no_timing_check`), returning to the C++ dispatcher only
  on a true miss (the design C1 holds up as the target structure). Source comment: *"Blocks do NOT
  use call/ret, they only jmp to each other and to the dispatcher when necessary."*
- **Why Dolphin never needed async compilation:** Dolphin's per-block compile cost is tiny
  relative to how long each block runs, and its blocks are large (it compiles whole basic-block
  runs with aggressive inlining). The compile is a one-time blip amortized over millions of
  executions; the steady state is ~100% in already-compiled code reached via in-asm jumps. There is
  no recurring "compiler stall" to hide. (Dolphin's *async* effort went into the GPU/shader path,
  not the CPU JIT — "Ubershaders," a different problem.)

Takeaway: if your block-cache residency is high and per-block compile is cheap, inline wins on
simplicity. The reason SheepShaver wants async anyway is specific: our complete-rate is low (~25%,
LEARNINGS L537) and compilation currently stalls the *guest*, so even infrequent compiles are felt;
fixing C1 first is what makes Dolphin-style residency achievable, after which C5 hides the
remaining compile stalls.

### 1.5 QEMU — per-vCPU translation, shared TB cache, atomic jump patching (MTTCG)

**Source:** `qemu/qemu`, `docs/devel/multi-thread-tcg.rst` (GPL-2.0).

- TCG **translation is single-threaded per vCPU**: each vCPU thread translates the code it is about
  to run, inline. There is no separate "compiler thread"; MTTCG is about running *multiple guest
  CPUs* on multiple host threads, not about offloading compilation.
- The **translation-block cache is shared** across vCPUs. The hot lookup path is lock-free: the
  per-vCPU `tb_jmp_cache` is updated with atomic accesses; the fallback shared lookup hash uses a
  QHT (lock-free hash table) that atomically inserts new TBs.
- **Direct block-to-block jump patching is done atomically** (doc, line ~154: *"The direct jump
  themselves are updated atomically by the TCG"*). This is the single-word atomic patch idiom —
  the same one we need for chain sites.
- **Cross-thread invalidation requires quiescing:** "vCPUs are quiescent when changes are being
  made to shared global structures" — TB invalidation (self-modifying code, page changes) reverses
  direct-jump patches and removes TBs from page/jump lists, done while other vCPUs are stopped.

Relevance to us is narrow but precise: QEMU is **not** the async-compile model (each thread
compiles its own code). The transferable lessons are the **lock-free shared lookup** (atomic
insert into a shared cache) and **atomic single-word jump patching** — both directly applicable to
making `jit_bc_*` and chain patching safe for a producer/consumer split. SheepShaver only has one
guest CPU, so MTTCG's multi-vCPU machinery is not needed; only its concurrency primitives are.

---

## 2. The canonical tiered-compilation pattern (V8 / HotSpot / JavaScriptCore)

*This section is the established managed-runtime pattern, not re-fetched this session (WebSearch
down). It is included because it is the conceptual parent of every CPU-JIT design above, and it is
corroborated 1:1 by Ryujinx's concrete code in §1.2. No source URLs are claimed for it.*

All three production JS/JVM engines share one shape:

1. **A baseline tier executes immediately.** Interpreter (HotSpot's template interpreter, JSC's LLInt)
   or a quick non-optimizing baseline JIT. No waiting — code runs the instant it is reached.
2. **Profiling counters drive tier-up.** Invocation/back-edge counters (HotSpot's
   `CompileThreshold`, V8's feedback vectors, JSC's tier-up counters). When a function/loop is hot,
   a **compile request is enqueued**, not run inline.
3. **Optimizing compilers run on dedicated background threads.** HotSpot's C1/C2 compiler threads,
   V8's TurboFan/Maglev concurrent compile jobs, JSC's DFG/FTL threads. The mutator/execution
   thread never blocks on optimization.
4. **Installation is an atomic publish.** The optimized code is made live by an atomic pointer
   store (entry-point / code-pointer swap). Subsequent calls take the fast path; in-flight calls
   finish on the old code.
5. **On-Stack Replacement (OSR) + safepoints handle the *running* frame.** For long-running loops,
   OSR transfers a currently-executing baseline frame into optimized code at a loop back-edge.
   Deoptimization is the reverse, performed at **safepoints** — points where the runtime can safely
   inspect/rewrite a thread's state. This is the part CPU emulators mostly *don't* need: a
   block-structured guest re-enters the dispatcher at every block boundary, which is a natural
   safepoint, so "wait for the next block boundary" replaces OSR/safepoint machinery.

**Canonical handoff mechanism:** atomic pointer swap into a per-function (or per-PC) code table,
plus block-boundary re-dispatch instead of OSR. Ryujinx (§1.2) is literally this pattern with
`Volatile.Write` as the atomic publish and the call counter as the tier-up trigger.

---

## 3. Model comparison for SheepShaver

| Model | Who does it | Fit for SheepShaver | Cost / risk |
|---|---|---|---|
| **(a) Compile-on-miss queue** — interpreter runs, posts miss PCs to a queue, worker compiles, blocks appear later | **Cemu** (1:1 analogue) | **Best.** C1's gate restructure already builds the CPU-thread half. Minimal new machinery. | One worker, one queue, one epoch counter. Lowest risk. |
| **(b) Tiered (counter-gated)** — everything interprets; hot blocks (exec counter) get queued | Ryujinx, V8/HotSpot/JSC | **As a refinement of (a).** Bolt a "queue after Nth sighting" counter onto the miss path to skip one-shot init code. | Tiny: one counter per cache entry. Pure win over naive (a). |
| **(c) AOT sweep** — worker pre-compiles the whole ROM range at startup | RPCS3 (parallel), Rosetta 2 | **Deferred.** Attacks warm-start and complete-rate, not the steady-state stall. C1 already concluded AOT is necessary-but-not-sufficient and must follow the gate fix. | Largest engineering; needs the ROM-range scan + persistence. Revisit after (a)+(b). |

**Recommendation: implement (a), with (b)'s counter as a same-PR refinement, and keep (c) as a
later warm-start complement.** This is the Cemu design with a Ryujinx counter — both proven in
PowerPC and CPU-emulation contexts respectively.

---

## 4. Recommended design + synchronization sketch

**Prerequisites (do not start C5 before these):**
- **C1 gate restructure landed.** The interpreter must already "prefer a compiled block at every
  transition" via the read-only `jit_bc_lookup`. That read-only check at the block boundary *is*
  the consumer side of the handoff — C5 only adds the producer (worker) side. Without C1 the worker
  would compile blocks the CPU thread still ignores.
- **C4 dual mapping adopted (effective prerequisite, see §5).** The worker writes via the
  process-wide RW alias; the CPU thread executes via the RX alias; no per-thread W^X toggle.

### 4.1 Data structures and ownership

- **Block cache `jit_bc_heads[]` / `jit_bc_pool[]` → single-writer.** Make the **worker the sole
  writer** of cache inserts. The CPU thread only ever does the **read-only** `jit_bc_lookup`. This
  eliminates the multi-writer race the backlog worries about: there is exactly one producer of new
  entries and (effectively) one consumer.
- **The published pointer is `entry->code`.** This is the handoff point, exactly like Cemu's
  `directJumpTable[pc/4]` and Ryujinx's `FunctionTable`.
  - Worker: emit bytes via the RW alias → `sys_icache_invalidate(xptr, size)` → **release-store**
    `entry->code = fn` (after a release fence, or as an atomic store-release).
  - CPU thread: **acquire-load** `entry->code`; if non-NULL **and** `entry->complete`, run it. The
    acquire/release pair guarantees the emitted instructions are visible before the pointer is.
  - This is the canonical atomic-pointer install of §2 and QEMU's atomic-insert (§1.5).
- **Work queue: SPSC.** One producer (CPU thread) → one consumer (worker). A lock-free single-
  producer/single-consumer ring, or Cemu's `try_lock`+`std::queue` (simplest, proven). The CPU
  thread must **never block** on enqueue: use `try_lock` and, on failure, just keep interpreting
  (the PC will be re-offered next time around — Cemu's exact behavior).
- **Pending flag (dedup).** Mark the cache entry "pending" (a distinct sentinel, like Cemu's
  `visited`) when enqueued, so the same PC is not posted twice and is not also compiled inline.
- **(b) counter (optional, recommended).** A small per-entry `seen_count`; only enqueue on the Nth
  sighting (e.g. N=8–16). Skips one-shot ROM init code, mirroring Ryujinx's `MinsCallForRejit=100`.
- **Invalidation epoch.** A process-wide `std::atomic<uint64_t> jit_generation`. The CPU thread
  bumps it on any code-cache flush / guest SMC (`SPCFLAG_JIT_EXEC_RETURN` paths). The worker
  **snapshots** `jit_generation` at compile start and, at install time under a short lock,
  **drops** the result if the epoch moved — Cemu's `invalidationRanges` discipline, simplified to a
  global generation counter (we can start coarse and refine to ranges later).

### 4.2 The two handoff points (named explicitly)

1. **Block publish (worker → CPU thread).** Safe with *standard* maintenance, **not** a CMODX
   case: the address being published has never been executed before, so no stale instruction
   prefetch exists on any core. Sequence: emit → `sys_icache_invalidate` / DSB → release-store
   `entry->code`. The CPU thread picks it up at its next block-boundary `jit_bc_lookup`.
2. **Chain patching (live code mutation).** `patch_chain_sites()` rewrites a *branch in
   already-executable code that the CPU thread may be running right now.* This is the only true
   concurrent-modify-while-execute case and is the headline risk (§6). **Conservative split: the
   worker compiles block *bodies* only; the CPU thread performs all live chain patching at its own
   block boundaries**, where it controls its own i-cache and no other core is executing that code.
   With this split, no cross-core concurrent modify/execute ever occurs and the ARM CMODX exemption
   does not even need to hold.

### 4.3 Chain-site bookkeeping ownership (avoid a second writer)

To keep "single writer of cache state" honest: the **worker** records chain-*site* metadata
(unresolved targets) into the chain-site pool as part of compiling a body — it owns the pool's
*inserts*. The **CPU thread** consumes that metadata to perform the actual live patch at a block
boundary (it owns the *mutation* of executable bytes). So the pool has one inserter (worker) and
one patch-applier (CPU thread) operating at disjoint times relative to a given site (a site is
inserted before its block is ever reached, patched only once the CPU thread arrives). If that
disjointness is ever in doubt, guard the pool's small critical sections with the same short lock as
the `entry->code` install — the pool is small and the sections are O(1).

### 4.4 Control flow (post-C1 + C5)

```
CPU thread (powerpc_cpu::execute, post-C1):
  at each block boundary:
    e = jit_bc_lookup(pc)                       # read-only, acquire-load e->code
    if e && acquire(e->code) && e->complete:
        apply any pending chain patches for e   # CPU thread owns live patching
        run native block
    else:
        if e == NULL or e->state == idle:
            if ++e->seen_count >= N and try_enqueue(pc):  # SPSC, non-blocking
                e->state = pending
        interpret this block                    # baseline tier keeps running

Worker thread:
  loop:
    pc = queue.dequeue()                          # blocks/sleeps when idle
    gen0 = jit_generation.load()
    fn, code = ppc_jit_aarch64_compile(pc)        # writes via RW alias, NO cpu-thread stall
    sys_icache_invalidate(xptr, size)
    lock(install_mtx):                            # short critical section
        if jit_generation.load() != gen0: drop; continue   # invalidated mid-compile
        insert into jit_bc_* (sole writer)
        release-store e->code = fn; e->complete = true; e->state = ready
```

---

## 5. W^X under threading — why C4 is a prerequisite, not a nicety

`pthread_jit_write_protect_np` is **per-thread** state (confirmed in the C4 spike and by RPCS3
calling it per-worker, §1.3). With the single-MAP_JIT + per-thread-toggle model we ship today, a
genuine hazard appears the moment a worker writes while the CPU thread executes: the worker would
make the shared RWX page writable *from its thread* while the CPU thread is fetching instructions
from the *same physical page* on another core. The per-thread toggle does not coordinate across
threads, and the semantics of one thread holding a page writable while another executes it are
exactly the W^X invariant the OS is enforcing — fragile at best.

The C4 result (`c4-wx-dual-mapping-spike.md`, verified WORKS on macOS 26.4.1 arm64) removes this:
a process-wide **RW alias** (worker writes here) and **RX alias** (CPU executes here) of the same
physical pages, no toggling. The spike explicitly notes this is *friendlier to a multi-threaded
JIT* — the worker can write via the RW alias on its own thread with zero interaction with the CPU
thread's execution via the RX alias. So **adopt C4 before, or together with, C5.** This is the
backlog's "C4 + C5 are the natural end-state," restated as a hard dependency.

Note C4's adoption cost (touching every emit/branch site in `ppc-jit.cpp` to split write-alias from
exec-alias) is borne once and benefits C5 directly — the worker only ever holds the *write* alias,
the CPU thread only ever branches to the *exec* alias.

---

## 6. Risks (chain-patch CMODX first — it is the one that bites)

1. **CMODX: concurrent modification of live executable code (HIGHEST RISK).** Patching a branch in
   code another core is executing is ARM "concurrent modification and execution of instructions"
   (per the Arm ARM §B2.2.5 — *verify the exact permitted-instruction list and the required cache-
   maintenance sequence against the spec before implementing; do not rely on this doc's recollection
   of it*). Only a small set of instructions (branch-class and a few others) may be modified
   concurrently, and even then the modifying PE must perform the prescribed DC/IC maintenance and
   the executing PE will not observe the change without context synchronization it may not perform.
   A naive "worker patches live chains" design produces **intermittent, cross-core, timing-dependent
   crashes** that are nearly impossible to reproduce. **Mitigation (designed into §4.2):** the
   worker never patches live code; the CPU thread does all live chain patching at its own block
   boundaries, eliminating concurrent modify/execute entirely. The block-*publish* path is **not**
   CMODX (the address was never executed before) and is safe with standard `sys_icache_invalidate`.

2. **Invalidation mid-compile.** Guest SMC / code-cache flush can invalidate a block while the
   worker is compiling it. **Mitigation:** the `jit_generation` epoch (§4.1) — snapshot at compile
   start, drop the result at install if it moved. This is Cemu's `invalidationRanges` check,
   verified in §1.1.

3. **Memory ordering on the handoff pointer.** If `entry->code` is published before the emitted
   bytes (and the i-cache invalidate) are visible, the CPU thread runs garbage. **Mitigation:** the
   acquire/release discipline in §4.1; `sys_icache_invalidate` before the release-store.

4. **Multi-writer cache corruption.** Two threads mutating `jit_bc_heads[]`/`jit_bc_pool[]` linked
   lists races. **Mitigation:** worker is the sole writer; CPU thread is read-only (§4.1). This is
   the cleanest available guarantee and removes the backlog's stated worry outright.

5. **Queue backpressure / CPU-thread stall.** If enqueue ever blocks, we have reintroduced the very
   stall we are removing. **Mitigation:** non-blocking `try_enqueue` (Cemu's `try_lock`); on failure
   keep interpreting and re-offer the PC later. The worker falling behind degrades to "more
   interpretation," never to a stall.

6. **Worker starvation / priority.** The worker must not steal cycles from the single guest CPU
   thread on a busy machine. **Mitigation:** run the worker at lower priority (Ryujinx runs its last
   translator thread at `ThreadPriority.Lowest`; RPCS3 workers use `scoped_priority low_prio(-1)`).

7. **Stale inbound chains after invalidation (defense in depth).** A block destroyed while another
   block's chain still points at it. **Mitigation:** the existing backlog item B4 (debug-gated
   `BRK` tripwire on destroyed blocks) becomes more valuable under threading — keep it.

---

## Appendix — sources (all read via `gh api` this session; WebSearch/WebFetch were down)

| Emulator | File (repo @ branch) | Key symbols cited |
|---|---|---|
| Cemu | `cemu-project/Cemu` `src/Cafe/HW/Espresso/Recompiler/PPCRecompiler.cpp` @ main | `PPCRecompiler_visitAddressNoBlock`, `targetQueue`, `recompilerSpinlock`, `PPCRecompiler_thread`, `PPCRecompiler_makeRecompiledFunctionActive` (invalidation-range check), `ppcRecompilerDirectJumpTable` |
| Ryujinx | `alula/Ryujinx` (mirror) `src/ARMeilleure/Translation/Translator.cs`, `PTC/Ptc.cs` | `GetOrTranslate`, `Translate(highCq:false/true)`, `EmitRejitCheck`, `MinsCallForRejit=100`, `BackgroundTranslate`, `Volatile.Write(FunctionTable…)`, `ClearRejitQueue`, `PtcProfiler` |
| RPCS3 | `RPCS3/rpcs3` `rpcs3/Emu/Cell/PPUThread.cpp` @ master | `named_thread_group("PPUW…")`, `work_cv` (atomic cursor), `g_progr_pdone`/`g_progr_ptotal` (the progress bar), `ppu_initialize2`, `__resolve_symbols`, `vm::g_exec_addr` |
| Dolphin | `dolphin-emu/dolphin` `Source/Core/Core/PowerPC/Jit64/Jit.cpp` | `Jit64::Jit`/`DoJit` (inline on CPU thread), `asm_routines.dispatcher` (in-asm block linking) |
| QEMU | `qemu/qemu` `docs/devel/multi-thread-tcg.rst` | per-vCPU translation, QHT lock-free TB cache, "direct jumps updated atomically" (line ~154), quiesce-for-invalidation |
| V8/HotSpot/JSC | — (established pattern, not re-fetched; corroborated by Ryujinx code) | baseline-immediate + counter tier-up + background optimizer + atomic install + OSR/safepoints |
| ARM CMODX | Arm ARM §B2.2.5 (cited from knowledge; **verify against spec before implementing**) | concurrent modification & execution permitted-instruction set + required cache maintenance |
