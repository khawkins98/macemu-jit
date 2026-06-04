# Lead 2 — Dolphin's Nesting-Counter W^X Toggle

> **Status:** 📖 Reference / archive · **Created:** 2026-06-02 · **Updated:** 2026-06-04
> **Why this doc exists:** Research lead: Dolphin's nesting-counter W^X toggle — verdict in backlog.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../../CONTRIBUTING.md) → "Documentation Lifecycle")._


Deep-dive on whether to adopt Dolphin's thread-local nesting-counter pattern for
`pthread_jit_write_protect_np()` toggling in the SheepShaver PPC→ARM64 JIT.

Compiled 2026-06-02. Branch `macos-arm64`. Continues the W^X work in commit `8f2acc9b`.

**Headline finding:** Our toggle call sites are **sequential, not nested**. A nesting
counter would reduce syscall count in exactly **one** place — the per-word toggle inside
the `patch_chain_sites()` loop. The dominant W^X cost was already eliminated by `8f2acc9b`'s
early-out. This lead is cheap defensive hygiene, not a measurable speedup. The lead's
"biggest win, do first" framing overstates the payoff given our current evidence.

---

## 1. Verified Dolphin implementation

Fetched from the real source on 2026-06-02:
`https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Common/MemoryUtil.cpp`
and `MemoryUtil.h`.

### Counter + toggle (MemoryUtil.cpp)

```cpp
static int& JITPageWriteNestCounter()
{
  static thread_local int nest_counter = 0;
  return nest_counter;
}

void JITPageWriteEnableExecuteDisable()
{
#if defined(_M_ARM_64) && defined(__APPLE__)
  if (JITPageWriteNestCounter() == 0)
  {
    pthread_jit_write_protect_np(0);
  }
#endif
  JITPageWriteNestCounter()++;
}

void JITPageWriteDisableExecuteEnable()
{
  JITPageWriteNestCounter()--;

  if (JITPageWriteNestCounter() < 0)
    PanicAlertFmt("JITPageWriteNestCounter() underflowed");

#if defined(_M_ARM_64) && defined(__APPLE__)
  if (JITPageWriteNestCounter() == 0)
  {
    pthread_jit_write_protect_np(1);
  }
#endif
}
```

### RAII wrapper + comments (MemoryUtil.h)

```cpp
// Allows a thread to write to executable memory, but not execute the data.
void JITPageWriteEnableExecuteDisable();
// Allows a thread to execute memory allocated for execution, but not write to it.
void JITPageWriteDisableExecuteEnable();
// RAII Wrapper around JITPageWrite*Execute*(). When this is in scope the thread can
// write to executable memory but not execute it.
struct ScopedJITPageWriteAndNoExecute
{
  ScopedJITPageWriteAndNoExecute() { JITPageWriteEnableExecuteDisable(); }
  ~ScopedJITPageWriteAndNoExecute() { JITPageWriteDisableExecuteEnable(); }
};
```

### Key observations about the verified Dolphin code

1. **The counter gates *only* `pthread_jit_write_protect_np`.** Dolphin's helper does
   **not** bundle `sys_icache_invalidate` into the enable/disable pair. Cache/icache
   invalidation in Dolphin is a separate concern handled by the emitter
   (`JITPageWriteAndNoExecute` is purely the protection toggle; emitters call
   `Common::Arm64Emitter`'s `FlushIcache()` / `ARM64XEmitter::FlushIcacheSection()`
   independently). This is the single most important structural difference from our
   `jit-target-cache.hpp`, where `jit_cache_end_write()` couples *both* the toggle and
   `sys_icache_invalidate()` (see §3, correctness trap).

2. **It is thread-local** (`thread_local int`). Each thread carries its own counter,
   matching `pthread_jit_write_protect_np`'s per-thread semantics.

3. **Underflow is a hard error** (`PanicAlertFmt`), i.e. unbalanced enable/disable is
   treated as a bug, not silently tolerated.

4. **RAII is the intended call convention.** Callers use `ScopedJITPageWriteAndNoExecute`
   on the stack rather than hand-balancing enable/disable, which is what makes deep nesting
   safe (block compile that internally patches a chain site, etc.).

---

## 2. Our current implementation — file:line map of every toggle

All paths in `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/`.

### The primitives (`jit-target-cache.hpp`)

| Line | Symbol | Body (Apple arm64) |
|------|--------|--------------------|
| 35–37 | `jit_cache_begin_write()` | `pthread_jit_write_protect_np(0)` |
| 38–41 | `jit_cache_end_write(addr,len)` | `pthread_jit_write_protect_np(1)` **then** `sys_icache_invalidate(addr,len)` |
| 52–81 | `jit_cache_flush(start,len)` | no-op on Apple (returns early); DC CVAU / IC IVAU on Linux |

On Linux the begin/end hooks are no-ops and icache maintenance lives in `jit_cache_flush`.
The W^X concern is Apple-only.

### Every toggle call site (`ppc-jit.cpp`)

| Lines | Function | Structure | Nestable? |
|-------|----------|-----------|-----------|
| 168 / 170 / 171 | `patch_chain_sites()` | `begin_write` / `flush` / `end_write` **inside the per-site `while` loop** — one toggle pair per patched word | **This is the only redundancy.** N matching sites ⇒ N toggle pairs. |
| 3783 / 3799 | `ppc_jit_aarch64_invalidate_range()` | `begin_write` once before the revert loop, `end_write` once after — **already coalesced** | Already optimal; demonstrates the fix. |
| 3895 / 3993 / 4061 / 4064 | `ppc_jit_aarch64_compile()` | `begin_write` at 3895 (after prologue ptr set); `end_write` at 3993 (early-bail path) **or** `flush`+`end_write` at 4061/4064 (success path) | Single bracket per compile. |

### Call-graph: are any of these actually nested at runtime?

No. Verified by tracing the call order:

- `ppc_jit_aarch64_compile()` brackets emission with `begin_write` (3895) … `end_write`
  (4064). It then calls `jit_bc_insert()` at **line 4126** — *after* `end_write` has
  already re-protected the cache.
- `jit_bc_insert()` (210) calls `patch_chain_sites()` (lines 223 and 243).
- `patch_chain_sites()` opens its **own** `begin_write`/`end_write` per word (168–171).

So the sequence is: `begin_write … end_write` (compile) → `begin_write … end_write` ×N
(chain patches). **Strictly sequential.** There is no point in our code today where one W^X
bracket lexically or dynamically contains another. (`record_chain_site` at compile-time,
line 693/702, only writes the pool struct in normal RAM — no toggle.)

### What `8f2acc9b` changed and why

Root cause it fixed: `ppc_jit_aarch64_compile()` ran the `begin_write`/`end_write` pair +
emitted a prologue for **every** block visit, including ROM/SheepMem PCs that immediately
fail the RAM range check. At ~11,000 block entries/sec during ROM init, the per-call
`pthread_jit_write_protect_np` + `sys_icache_invalidate` (~µs each) consumed ≈90% of CPU →
37× slowdown vs interpreter.

Fix: an **early-out RAM range check placed before** the `begin_write` bracket (now lines
~3849–3861), so out-of-range PCs return `false` at zero cost. JIT boot then reached the
Mac OS 8.6 Finder desktop. The commit also added chain-patch tracking (`patched` bool, kept
`patch_loc` after consumption) and `ppc_jit_aarch64_invalidate_range()` — whose loop is the
already-coalesced model for what a nesting counter would generalize.

**Crucially: the dominant W^X cost was a *per-call-that-shouldn't-have-happened* cost, not a
*nesting* cost. `8f2acc9b` killed it by not calling the toggle at all on the hot path.** A
nesting counter would not have helped that case.

---

## 3. Analysis

### Where we toggle redundantly

Exactly one place: **`patch_chain_sites()` (168–171)** toggles W^X once per patched word
inside its loop. When a freshly-compiled hot block satisfies several waiting chain sites,
we pay one `pthread_jit_write_protect_np(0)` + `(1)` + `sys_icache_invalidate` pair *per
site*. The fix is to hoist the bracket out of the loop (exactly what `invalidate_range`
already does at 3783/3799).

### Would a nesting counter simplify the code / reduce syscalls?

- **Syscall reduction:** Only in `patch_chain_sites`, and only when >1 site is patched in a
  call. A plain bracket-hoist achieves the same reduction there without any counter. The
  counter buys nothing additional *given the current sequential call graph*.
- **Simplification / future-proofing:** The counter's real value is *defensive*: it lets
  every emit helper bracket its own writes (RAII-style) without anyone reasoning about
  whether a caller already holds the cache writable. If we later restructure so that
  compile calls patch-during-emission (i.e. patch chain sites *while still inside* the
  compile bracket — which is plausible if we move `patch_chain_sites` before `end_write`
  to also coalesce its toggles into the compile bracket), nesting becomes real and the
  counter prevents the inner `end_write` from prematurely re-protecting the cache mid-emit.
  **That premature re-protect is a latent foot-gun the counter eliminates.**

### icache-invalidation ordering concern — the correctness trap

This is the one place a naive port breaks. **Dolphin's counter gates only the protect
toggle; it never touches icache.** Our `jit_cache_end_write()` does *two* things:
`pthread_jit_write_protect_np(1)` **and** `sys_icache_invalidate(addr,len)`.

If we add a counter so that an inner `end_write` becomes a full no-op when the counter is
still > 0, we would **silently drop the icache invalidation for the inner-written region.**
That region's freshly-written instructions could then be executed from a stale icache →
intermittent, near-undebuggable wrong-code execution.

The required invariant for any counter we add:

> The counter gates **only** `pthread_jit_write_protect_np`. `sys_icache_invalidate(addr,
> len)` must fire **unconditionally** on every `end_write` call, with that call's own
> `addr`/`len`. The W^X→X transition may be deferred to the outermost leave; the icache
> invalidation may **not** be deferred or skipped.

The DSB/ISB ordering (write → DC CVAU → IC IVAU → ISB, or on Apple the
`sys_icache_invalidate` equivalent) must still complete after the bytes are written and
before execution. Because `sys_icache_invalidate` stays unconditional and per-region, that
ordering is preserved regardless of nesting depth. The only thing deferred is *re-arming
write protection*, which is safe to defer (the region stays writable longer, never shorter).

### Per-thread / single-thread concern

Our JIT is **single-threaded** (confirmed in `LEARNINGS.md` §"Task 2a", lines ~352–354:
"The JIT is single-threaded … compiles and executes on the SAME thread, serially"). The
emulated CPU thread is the only one that compiles or patches. So:

- We do **not** strictly need `thread_local` for correctness today — a plain `static int`
  would work. But making the counter `thread_local` (matching Dolphin) costs nothing and is
  the safe default if a second JIT-touching thread ever appears (e.g. a future async
  compiler or the 60 Hz interrupt path patching from a different context).
- `pthread_jit_write_protect_np` is inherently per-thread; a global counter shared across
  threads would be *wrong* if multithreaded. `thread_local` keeps the counter aligned with
  the syscall's own scope. **Use `thread_local`.**

---

## 4. Concrete implementation sketch

Two options. **Option A** (hoist) is the minimal fix for the only real redundancy.
**Option B** (counter) is the general, defensive version that also unlocks safely moving
`patch_chain_sites` inside the compile bracket later.

### Option A — hoist the bracket out of the `patch_chain_sites` loop (smallest)

In `ppc-jit.cpp`, `patch_chain_sites()` (158–177): toggle once, track flush range,
invalidate once — mirroring `invalidate_range`:

```cpp
static void patch_chain_sites(uint32_t pc, uint32_t *chain_code) {
    if (!chain_code) return;
    jit_bc_ensure_init();
    int bucket = (pc >> 2) & JIT_BC_MASK;
    uint32_t *flush_lo = NULL, *flush_hi = NULL;
    bool wrote = false;
    for (int idx = chain_site_heads[bucket]; idx >= 0; idx = chain_site_pool[idx].next) {
        struct jit_chain_site *site = &chain_site_pool[idx];
        if (site->target_pc == pc && site->patch_loc && !site->patched) {
            int32_t off = (int32_t)((uint8_t *)chain_code - (uint8_t *)site->patch_loc);
            if (off >= -(1 << 25) && off < (1 << 25)) {
                if (!wrote) { jit_cache_begin_write(); wrote = true; }
                *site->patch_loc = 0x14000000 | ((off >> 2) & 0x3FFFFFF); /* B offset */
                if (!flush_lo || site->patch_loc < flush_lo) flush_lo = site->patch_loc;
                if (!flush_hi || site->patch_loc > flush_hi) flush_hi = site->patch_loc;
                site->patched = true;
            }
        }
    }
    if (wrote) {
        size_t len = (size_t)((uint8_t *)(flush_hi + 1) - (uint8_t *)flush_lo);
        jit_cache_flush(flush_lo, len);
        jit_cache_end_write(flush_lo, len);
    }
}
```

This is ~one-for-one with the existing `invalidate_range` body and needs no new primitive.

### Option B — nesting counter in `jit-target-cache.hpp` (general, defensive)

Split the two responsibilities. The counter gates **only** the protect toggle;
icache invalidation stays unconditional.

```c
#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#define JIT_CACHE_MAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT)

/* Thread-local W^X nesting depth. pthread_jit_write_protect_np() is a per-thread
 * toggle, so the counter must be per-thread too. Our JIT is single-threaded today
 * (see LEARNINGS.md "Task 2a"), but thread_local is the correct default and costs
 * nothing. Mirrors Dolphin's JITPageWriteNestCounter (Common/MemoryUtil.cpp). */
static __thread int jit_wx_nest = 0;

static inline void jit_cache_begin_write(void) {
    if (jit_wx_nest == 0)
        pthread_jit_write_protect_np(0); /* writable on this thread */
    jit_wx_nest++;
}

static inline void jit_cache_end_write(void *addr, size_t len) {
    /* icache invalidation is NEVER deferred: the bytes just written at
     * [addr,len) must be coherent before they can execute, regardless of how
     * many outer write brackets are still open. Only the W->X re-protect is
     * gated by the nesting counter. (Dolphin's counter gates the toggle ONLY
     * and leaves icache handling to the emitter — we keep that separation.) */
    if (len)
        sys_icache_invalidate(addr, len);

    if (jit_wx_nest > 0)
        jit_wx_nest--;
    if (jit_wx_nest == 0)
        pthread_jit_write_protect_np(1); /* executable again */
    /* else: underflow — caller imbalance; in debug builds assert/log here. */
}
#else
#define JIT_CACHE_MAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS)
static inline void jit_cache_begin_write(void) {}
static inline void jit_cache_end_write(void *addr, size_t len) { (void)addr; (void)len; }
#endif
```

Notes on the sketch:
- **`sys_icache_invalidate` is unconditional and per-region** — the correctness trap from §3
  is closed.
- Re-protection is deferred to the outermost `end_write`. The cache stays *writable longer*
  on a nested path, never shorter, so no executing thread sees a stale write window.
- A debug `assert(jit_wx_nest > 0)` (or a `fprintf`+abort) on the underflow branch matches
  Dolphin's `PanicAlertFmt` and catches unbalanced brackets early.
- With Option B in place, `patch_chain_sites` can keep its current per-word
  `begin_write`/`end_write` and the toggles automatically coalesce **if** the calls ever
  become nested inside a compile bracket — but as shown in §2 they are sequential today, so
  Option B alone gives **no** syscall reduction without also restructuring the call graph.
  To get the `patch_chain_sites` win you still want Option A's loop hoist *or* you move the
  `patch_chain_sites` call to before `end_write` in compile (then B's counter coalesces it).

### Recommended combined form

Do **Option A** (hoist) for the immediate, guaranteed syscall reduction in
`patch_chain_sites`, **and** adopt **Option B's** split `end_write` (unconditional icache +
gated toggle) as defensive infrastructure. Option B is harmless on its own and removes the
latent "inner end_write re-protects mid-emit" foot-gun for any future restructuring.

---

## 5. Effort, risk, payoff

| | Assessment |
|---|---|
| **Effort** | Low. Option A: ~15 lines, modeled directly on existing `invalidate_range`. Option B: ~15 lines in `jit-target-cache.hpp`. No new files, no API changes, no call-site churn. |
| **Risk** | Low-to-moderate. The *only* real risk is the icache trap (§3): a naive counter that no-ops the inner `end_write` would drop icache invalidation and cause stale-code execution. The sketch closes it by keeping `sys_icache_invalidate` unconditional. Underflow/imbalance is the secondary risk — guard with a debug assert. Linux path unchanged (no-ops). |
| **Payoff** | Small and bounded. Measurable speedup limited to multi-site `patch_chain_sites` calls; the dominant W^X cost was already removed by `8f2acc9b`'s early-out. Real value is *code hygiene* and *future-proofing*, not throughput. |
| **Verification** | Both harness modes must stay 209/209 (`make test-opcodes`; set `SS_TEST_JIT=1`). Boot-to-desktop must still reach Finder. A targeted check: count `pthread_jit_write_protect_np` calls via dtrace/sample before/after to confirm `patch_chain_sites` coalescing. |

---

## 6. Recommendation

**Adopt — but as hygiene, not as the headline performance win the lead implies.**

The research lead frames Lead 2 as "the biggest win, do this first." Our own evidence
contradicts that: commit `8f2acc9b` already eliminated the dominant W^X cost by *not
toggling at all* on the hot ROM path, and our toggle call sites are sequential, so there is
no nesting to collapse anywhere except the single `patch_chain_sites` loop.

Concrete plan:

1. **Do Option A** (hoist the `patch_chain_sites` bracket out of its loop). This is the only
   change with a guaranteed, if modest, syscall reduction, and it brings `patch_chain_sites`
   into consistency with the already-correct `invalidate_range`.
2. **Do Option B** (split `end_write` into unconditional icache + counter-gated toggle) as
   cheap insurance. It eliminates the latent mid-emit re-protect foot-gun and makes future
   "patch while compiling" restructurings safe by construction. Use `thread_local`/`__thread`
   even though we are single-threaded today.
3. **Keep `sys_icache_invalidate` unconditional and per-region** — this is the load-bearing
   correctness constraint that distinguishes a correct port from a subtly broken one, and it
   is the one way our coupled `end_write` differs from Dolphin's toggle-only helper.

Net: low effort, low risk, small payoff. Worth doing because it is cheap and removes a real
foot-gun — *not* because it will move the performance needle. Sequence it ahead of the
heavier leads (3, 1) only because it is trivial, not because it is high-impact.
