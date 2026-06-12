> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** historical research
>

# C4. W^X dual-mapping spike — can we drop `pthread_jit_write_protect_np`?

> **Status:** 📖 Reference / archive · **Created:** 2026-06-02 · **Updated:** 2026-06-04
> **Why this doc exists:** Spike: can dual-mapping drop the W^X toggle? (backlog C4) — empirical ~27% faster.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../../CONTRIBUTING.md) → "Documentation Lifecycle")._


**Question:** Can we eliminate `pthread_jit_write_protect_np` toggling on macOS arm64
by keeping separate RW and RX mappings of the same physical code cache (oaknut
`DualCodeBlock` style)?

**Verdict: YES** — the `vm_remap` dual-mapping approach works on this machine and is
faster. (Headline at bottom.)

---

## What oaknut / others do

oaknut's `DualCodeBlock` (`include/oaknut/dual_code_block.hpp`, MIT) keeps a writable
mirror (`wptr()`) and an executable mirror (`xptr()`) of the same physical pages. Per platform:

- **macOS:** `mmap(RW, MAP_ANON|MAP_PRIVATE)` (NOTE: **no `MAP_JIT`**) to get `m_wmem`,
  then `vm_remap(mach_task_self(), &m_xmem, size, ... VM_FLAGS_ANYWHERE|VM_FLAGS_RANDOM_ADDR,
  mach_task_self(), m_wmem, ...)` to create a second virtual mapping of the same physical
  memory, then `mprotect(m_xmem, size, PROT_READ|PROT_EXEC)`. **No `pthread_jit_write_protect_np`
  anywhere.** Coherence on invalidate uses `sys_icache_invalidate(xptr, size)`.
- **Linux / *BSD:** `memfd_create`/`shm_mkstemp` + `ftruncate`, then two `mmap(MAP_SHARED)` of
  the same fd — one `PROT_READ|PROT_WRITE`, one `PROT_READ|PROT_EXEC`.
- **Windows:** single `VirtualAlloc(PAGE_EXECUTE_READWRITE)` (RWX, no W^X).

The key insight: macOS has no `memfd_create`, and (as the
test below empirically shows) executing from a `MAP_SHARED` shm mapping faults, so Apple's route to a dual mapping is the Mach `vm_remap` call,
which aliases physical pages to a second VA range where you can `mprotect` independently.
By inference this is also dynarmic's path on Apple (it vendors oaknut); the broader claim of
"several Apple-Silicon dynarecs" was not independently verified for this spike (the web
searches errored out and were not retried). The empirical result below stands on its own
regardless.

Today SheepShaver (`jit-target-cache.hpp`) uses the other supported route: a single
`MAP_JIT` RWX mapping plus per-thread `pthread_jit_write_protect_np(0/1)` bracketing every
write, then `sys_icache_invalidate`.

---

## Test program design

`SheepShaver/spikes/wx-dual-mapping/dual_map_test.c` (+ `Makefile`). Four approaches, each
run in a **forked child** so a fatal signal at execute time (the realistic hardened-OS
failure mode — SIGBUS / codesign kill) is caught by the parent via `waitpid` instead of
killing the whole spike:

- **a — vm_remap (oaknut):** `mmap(RW)` + `vm_remap` + `mprotect(RX)`, no `MAP_JIT`, no toggle.
- **b — shm MAP_SHARED:** `shm_open` + `ftruncate` + two `mmap(MAP_SHARED)` (RW and RX).
- **a2 — MAP_JIT + remap:** task's literal 2a wording — `mmap(MAP_JIT, RWX)` then `vm_remap`
  to an RW alias.
- **c — CONTROL:** `MAP_JIT` + `pthread_jit_write_protect_np(0/1)` (what we ship today).

**Correctness probe** (a single write+exec can pass by luck; the JIT's real workload is code
*patching*): for each approach, emit `MOVZ w0,#42; RET` via the W alias → `sys_icache_invalidate`
on the **X alias** → call → expect 42; then **overwrite the same slot** with `#99` via the W
alias → invalidate → call → expect 99. A child only `exit(42)` ("WORKS") if rewrite-coherence holds.

**Benchmark:** 100,000 iterations of emit-4-bytes + `sys_icache_invalidate(8)` + execute, for
the toggle control vs the dual mapping. Both pay the same `sys_icache_invalidate` per iter, so
the delta isolates the two `pthread_jit_write_protect_np` calls.

---

## ACTUAL RESULTS on this machine

- Machine: macOS **26.4.1** (build 25E253), arm64 (Apple Silicon).
- Compiler: clang, `-O2 -std=gnu11`. Binary ad-hoc linker-signed (no entitlement).

Verbatim output:

```
=== W^X dual-mapping spike (macOS arm64) ===
ITERS per bench = 100000, slot = 4096 bytes

Correctness (each in forked child):
  [a  vm_remap (oaknut, no MAP_JIT, no toggle)] WORKS (write+exec+rewrite-coherent)
  [b  shm MAP_SHARED dual mmap] BLOCKED — killed by signal 10 (Bus error: 10)
a2: vm_remap kr=2
  [a2 MAP_JIT RX + vm_remap RW alias] SOFT FAIL (exit code 2 — see stderr above)
  [c  CONTROL: MAP_JIT + jit_write_protect toggle] WORKS (write+exec+rewrite-coherent)

Performance (only meaningful for approaches that WORK):
  toggle (today):    12.8 ms total, 127.6 ns/iter
  dual vm_remap:     9.3 ms total, 92.8 ns/iter

Note: both approaches pay sys_icache_invalidate per iter; the only
delta dual-mapping removes is the two pthread_jit_write_protect_np calls.

HEADLINE: vm_remap=WORKS shm=no mapjit_remap=no control=WORKS
```

Interpretation:

- **a (oaknut vm_remap): WORKS**, including rewrite-coherence, with no `MAP_JIT` and no
  `pthread_jit_write_protect_np`. This is the answer.
- **b (shm MAP_SHARED): BLOCKED** — executing from a `MAP_SHARED` shm mapping faults with
  Bus error (SIGBUS) at the call. macOS does not permit execution from this mapping type.
  (This is exactly why oaknut uses `vm_remap`, not the Linux memfd route, on Apple.)
- **a2 (MAP_JIT + vm_remap): SOFT FAIL** — `vm_remap` returns `kr=2` (`KERN_PROTECTION_FAILURE`)
  when the source is a `MAP_JIT` region. `MAP_JIT` pages cannot be re-aliased this way; you must
  start from a plain RW `MAP_ANON|MAP_PRIVATE` region as oaknut does. The task's literal 2a wording
  (remap *from* MAP_JIT) is not the working recipe.
- **c (control): WORKS** — baseline confirmed.

Runs were repeatable across multiple invocations.

---

## Performance comparison

| Approach | total (100k) | per iter | vs control |
|---|---|---|---|
| `MAP_JIT` + toggle (today) | 12.8 ms | 127.6 ns | — |
| dual `vm_remap` (oaknut)   |  9.3 ms |  92.8 ns | **~27% faster, ~35 ns/iter saved** |

The ~35 ns/iter delta is the cost of the two `pthread_jit_write_protect_np` calls per write
batch that dual-mapping removes. Both approaches still pay `sys_icache_invalidate` per batch
(unchanged). Real-world saving scales with the number of write *batches* (block compiles +
chain-patch sites), not bytes emitted; it is a modest steady-state win and a larger win for
patch-heavy workloads (chain back-filling) where many tiny writes each currently toggle.

---

## What adopting it would involve

Confined to `jit-target-cache.hpp` (read-only here; not modified). Today the file exposes a
single base pointer plus `jit_cache_begin_write`/`jit_cache_end_write` no-op-or-toggle hooks and
`jit_cache_alloc`. To adopt dual mapping on the `__APPLE__ && __aarch64__` branch:

1. **`jit_cache_alloc`** allocates `mmap(RW, MAP_ANON|MAP_PRIVATE)` as the write base, then
   `vm_remap`s to an exec alias, then `mprotect(RX)` on the alias. It must return/track **both**
   pointers and the constant W→X offset `(xbase - wbase)`.
2. **Callers must write through the W alias and execute/branch through the X alias.** This is the
   real cost: every place that currently writes emitted bytes into the cache (in `ppc-jit.cpp`)
   and every place that computes a branch/chain target or an entry pointer must apply the offset
   to pick the correct alias. Today base==exec==write (one pointer); dual mapping splits that.
   The clean shape is a helper pair `w_ptr(off)` / `x_ptr(off)` and a rule: *emit to W, branch to X*.
3. **`jit_cache_begin_write`/`jit_cache_end_write` become no-ops** (drop the toggle); keep
   `sys_icache_invalidate` on the **X alias** address in `end_write`.
4. Linux/BSD branch unchanged (it already has no toggle). The vm_remap branch is macOS-only.
5. **Scope semantics change (safer, but name it):** `pthread_jit_write_protect_np` is
   *per-thread* state; dual mapping is *process-wide* — any thread may write via the W alias and
   execute via the X alias with no per-thread bracketing. This is actually friendlier to a
   multi-threaded JIT, but it is a behavioral change reviewers should be aware of.

Caveats to verify before shipping: `vm_remap` consumes ~2x the virtual address space per cache
(fine — the code cache is small); the W and X bases are not contiguous, so any code that assumes a
single contiguous `[base, base+size)` for *both* roles needs the offset applied. ASLR via
`VM_FLAGS_RANDOM_ADDR` is preserved. No entitlement needed (ad-hoc signing is enough — same as today).

This matches backlog C4's "If yes: replaces Lead 2/B3 entirely and removes a whole class of W^X bugs."

---

## Verdict

**YES — dual mapping via `vm_remap` works on macOS 26.4.1 arm64**, is rewrite-coherent, needs no
`MAP_JIT` and no `pthread_jit_write_protect_np`, requires no extra entitlement, and is ~27% faster
per write-batch in the microbenchmark. The Linux-style `MAP_SHARED`/shm route is **blocked** (SIGBUS
on execute), and remapping *from* a `MAP_JIT` region is **blocked** (`KERN_PROTECTION_FAILURE`) — so
oaknut's exact recipe (plain RW mmap → `vm_remap` → `mprotect RX`) is the one and only working path.

Adoption is mechanically simple in `jit-target-cache.hpp` but touches every code-emit and
branch-target site in `ppc-jit.cpp` to honor the write-alias/exec-alias split. Recommended as a
follow-up once C1 instrumentation confirms W^X toggling is a measurable cost in the real workload.
```
