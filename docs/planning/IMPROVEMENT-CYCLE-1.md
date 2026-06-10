# SheepShaver ARM64 JIT — Improvement Cycle 1 (2026-06-04)

> **Status:** ⏸ Stale — absorbed into later work · **Created:** 2026-06-04 · **Updated:** 2026-06-10
> **Why this doc exists:** Sequencing plan for improvement cycle 1 — the current round of JIT correctness + perf work.
>
> **⚠️ Note (2026-06-10):** This cycle's correctness items (carry-chain, AltiVec, FP) were
> completed through sessions 5–8. The OS-ceiling work (item group 3) is now driven by the
> **[Machine Layer](MACHINE-LAYER-PLAN.md)** rather than this cycle plan. Perf items from
> the **[Optimization Plan](OPTIMIZATION-PLAN.md)** remain the reference for JIT perf work.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../CONTRIBUTING.md) → "Documentation Lifecycle")._


> Produced by a multi-agent audit: 6 read-only auditors → 14 adversarial
> verifications (8 confirmed, 6 refuted) → synthesis. Findings here are
> *post-verification* (refuted claims dropped). Main-loop spot-checks noted inline.
>
> **Verified by main loop:** the harness is green (score=100, both `make test-jit` and
> `make test-opcodes`). The absolute vector count drifts as vectors are added/pruned (it was
> 238 mid-cycle, 255 post-AltiVec-merge) — get the live number from `make harness-count`,
> don't hardcode it. Score=100 is the invariant, not the count.

## Executive Summary

**Constraint:** another agent is actively editing `ppc-jit.cpp`, `emul_op.cpp`,
`rom_patches.cpp`, `main_unix.cpp`, `sheepshaver_glue.cpp` (uncommitted). All
codegen work is **sequenced** until that churn settles; this cycle lands only
non-colliding work (docs, test vectors, tooling).

Baseline (2026-06-03, post-RA): **+15.6%** Speedometer Mix (548.6→634.3) from the
register allocator, plus 0d/0f/0b quick wins. The fast feedback tiers now exist:
`make test-jit` (JIT correctness gate) and `make bench` (jit-bench per-instruction
timing).

**Work by risk profile:**
- **Immediate safe** (no collision): docs reconciliation, FP/AltiVec test vectors,
  Paranoia FP conformance + CI, microbench kernel completion.
- **Sequenced codegen** (post-churn): P1a boot verification, lazy CR0 (0g), isync
  inline (0c), cross-block pinning (P8), bclr chaining (P9).
- **Deferred** (blocked/prereq): P0 profiler (gates P2/P8/P9 prioritization), P2
  bcctr (needs P0 + Mixed-Mode RE), HLE, CI, 0e computed-goto, gpr64 RA-routing.

---

## Confirmed findings (post-verification)

### Correctness (latent, non-critical, properly gated)
1. **gpr64 RA coherence hole** — `emit_load_gpr64`/`emit_store_gpr64` bypass the RA
   low-word cache. PPC64-only ⇒ unreachable on 32-bit Mac OS. Already documented +
   deferred (P1a #3). Fix ~50 lines (32-bit zero-extend MOV on the cached path).
2. **Lazy CR0 (0g) re-enable is gated on P1a** — re-CMP from an *evicted*
   `lazy_cr0_reg` would produce wrong flags. The `lmw_stmw_wide` eviction vector
   passes `make test-jit`, but an `SS_JIT_VERIFY=1` boot (P1a #2) is the real gate.
   Re-enabling unsafely ⇒ silent CR0 corruption / boot hang.

### Refuted (NOT active bugs — recorded so they aren't re-raised)
- "Missing `lazy_flush_cr0()` before carry ops" — refuted: lazy CR0 is disabled, so
  `lazy_cr0_valid` is never true.
- "RA flush violation in the spcflags-poll path" — refuted: the TBZ correctly skips
  the flush on the normal (no-flags) path.
- "CBZ patch offset overflow" — refuted: the ≤256 KB per-block guarantee precludes
  it (a defensive bounds assert would still be nice-to-have).

### Documentation (maintenance-contract violations, confirmed)
- **Vector count: docs say 235/236, harness is 238** (verified). Grew during P1a
  (`lmw_stmw_wide`, jit-test/run.sh) plus subsequent additions; docs not updated.
- **Stale rom-harness README** DR-emulator limitation (DR has been JIT-compiled
  since 2026-06-03, ROM=0x500000).
- **`BOOT-TEST-METHOD.md`** referenced by BENCHMARKS.md but absent from the
  TESTING.md artifact table.

### Test coverage (confirmed gaps)
| Domain | Coverage | Gap |
|--------|----------|-----|
| FP double | ~19% (5/26) | fsqrt, fres, frsqrte, fctiw, fsel, fmsub, fnmadd, fnmsub, record-form |
| FP single | ~12% (1/8) | fadds, fsubs, fmuls, fdivs, fmadds |
| AltiVec | ~6% (13/207) | saturating, shifts, multiply, compare, permute, pack/unpack |
| Paranoia FP | 0% | canonical self-grading FP oracle, not wired |
| Microbench | 3/5 kernels, integer-only | load/store, call/return, FP kernels |

---

## The cycle

### Phase 1 — Immediate safe (no collision; land now)
1. **Reconcile vector counts** → 238/238, dated, in TESTING.md, OPTIMIZATION-PLAN.md,
   CLAUDE.md (local), rom-harness README.
2. **Expand FP/AltiVec test-jit vectors** (~20 new) — biggest correctness-coverage win.
3. **Wire Paranoia** as `make test-paranoia` (+ optional CI workflow).
4. **Add microbench kernels** — FP (`fadd` chain, `fmadd` recurrence) now; load-store
   & call-return when their addressing/driver land (see jit-bench v2 notes).
5. **Fix stale README DR limitation**; **catalog BOOT-TEST-METHOD.md**.

### Phase 2 — Sequenced codegen (after churn settles)
1. **P1a full verification** — `SS_JIT_VERIFY=1` boot; write the gpr64 RA-routing
   design; date P1a items 2–4.
2. **Re-enable lazy CR0 (0g)** — gated on P1a boot; measure with `jit-bench rc1`
   (expect 0.50 → ~0.42–0.48 ns/insn).
3. **0c isync inline** (+ optional LogicalImm encoder) — avoid block-break overhead.
4. **P8 cross-block pinning** (r1/r2 → x21/x22) — biggest remaining RA lever; gated
   on P1a; measure on jit-bench.
5. **P9 bclr chaining** — gated on P0 (confirm bclr is hot) + P1a.

### Phase 3 — Deferred (blocked / lower priority)
- **P0 execution-weighted profiler** — *the* prerequisite for P2/P8/P9 prioritization
  and HLE candidate discovery. No collision risk; should lead the next cycle.
- **P2 native bcctr** — only after P0 confirms it's execution-hot AND the Mixed-Mode
  dispatch is reverse-engineered (boot-hang history).
- **HLE framework**, **GitHub Actions CI**, **0e computed-goto**, **gpr64 RA fix**.

---

## Success criteria
- **Phase 1:** all docs at 238/238 dated; FP/AltiVec coverage from <10% → >50%;
  Paranoia runnable via `make test-paranoia`; FP microbench kernels reporting; `make
  test-jit` and `make bench` green throughout.
- **Phase 2:** P1a boot clean; 0g re-enabled with a measured jit-bench win; P8 design
  finalized.
- **Phase 3:** P0 profiler shipped; P2 go/no-go made on weighted data.

## Next cycle
Lead with the **P0 profiler** (unblocks honest perf prioritization), then execute the
Phase-2 codegen queue against real hot-spot data instead of priors.
