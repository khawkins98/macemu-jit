# BasiliskII → SheepShaver JIT cross-pollination

> **Status:** 🟡 Open · **Created:** 2026-06-05 · **Updated:** 2026-06-05
> **Why this doc exists:** Triage of which BasiliskII (68K JIT, the more mature same-host lineage from the upstream Linux ARM64 developer) techniques are worth borrowing for the SheepShaver PPC JIT — and which heavy machinery to leave behind.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its
> marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../CONTRIBUTING.md)
> → "Documentation Lifecycle")._

## The framing (read this first)

Two facts shape every recommendation below:

1. **BasiliskII's *codegen* does not transfer — it's a different ISA (68K, CISC).** What
   transfers is its *infrastructure and methodology*, which is more valuable anyway because
   BasiliskII targets the **same host** (ARM64) and is the same developer's conventions. For PPC
   *codegen* specifically, the better source is MAME's `ppcdrc.cpp` (already tracked as
   IMPLEMENTATION-BACKLOG C2), not BasiliskII.

2. **SheepShaver was deliberately built simpler than BasiliskII** (immediate architectural
   writeback, no lazy state across blocks, single authoritative PC, no block-lifecycle state
   machine — see [`JIT-STYLE-DECISION.md`](../JIT-STYLE-DECISION.md)). That was the right call.
   So the bar for borrowing is: **does this idea come over clean, or does it drag the
   lazy-state / multi-PC / lifecycle web in with it?**

### Why *not* import the heavy machinery

BasiliskII's heavy apparatus (block-lifecycle states, lazy flags/registers across blocks,
the three-representation PC `pc_p`/`pc`/`pc_oldp`, validated-vs-direct handler split with
checksum validation, the `jmpdep` dependency graph) has a specific cost structure that is a
**bad trade for SheepShaver**:

- **Lazy state moves bugs away from their cause.** Immediate writeback keeps a block's output
  state always correct at its boundary; lazy state lets a miscompile in block A surface in block
  C after chaining. Our own one-line proof: **lazy CR0 was disabled after a boot regression that
  was never root-caused** (OPTIMIZATION-PLAN §0g) — a *tiny* dose of laziness already produced an
  untrackable bug.
- **The audit-doc apparatus *is* the tax.** BasiliskII needs `AUDIT_AREA1_BLOCK_LIFECYCLE`,
  `_AREA2_PC_OWNERSHIP`, `_AREA3_FLAGS_LIVENESS`, and a formal `RUNTIME_CONTRACT` precisely
  because the design has too many invariants to hold in one's head. Our single-PC / eager-CR
  model needs none of those.
- **The features interact combinatorially.** lazy flags × direct chaining × SMC invalidation ×
  interrupt safe-points each multiply; the validated-handler/checksum path exists only to claw
  back safety that immediate writeback gives us for free.
- **The payoff is ISA-dependent and *smaller for PPC*.** Lazy flags pay most when almost every
  instruction touches condition codes (68K / x86-UAE). On PPC, CR writes are opt-in via the `Rc`
  bit, so eager CR writeback is already cheap and lazy CR buys less. **Same cost, smaller
  return.** Plus several pieces are switched *off* even upstream on ARM64 (full PC-triple
  materialization "disabled but present"; validated entry as default), so the payoff isn't even
  banked there yet.

None of this makes BasiliskII wrong *for BasiliskII* — it inherited the complexity and faces an
ISA that rewards it. The asymmetry is the whole point: we'd be adopting the complexity fresh, by
hand, for an ISA that rewards it less, on a fork with two developers.

---

## What to borrow (filtered through "does it come over clean?")

### Tier 1 — clearly worth it, zero heavy machinery

**X1. 🟡 Verification & bisection tooling.** Borrow BasiliskII's three debug knobs:
- `B2_JIT_VERIFY_PCS=pc[-pc],…` — verify only *suspect* PCs, so differential verify is fast
  enough to actually run (ours, `SS_JIT_VERIFY`, runs every block twice through the interpreter
  and is "extremely slow", so it's rarely used).
- `B2_JIT_BARRIER_FAMILIES=…` — force a chosen opcode family through the interpreter to
  **binary-search which family causes a codegen bug.** We have **no equivalent** "force family X
  to interpreter" switch today; this is the fastest way to localize the next bug.
- `B2_JIT_PCTRACE` — dual interp/JIT register+flag trace aligned by PC, find first divergence.

Pure dev infrastructure: no runtime state, no architectural coupling, no web. **Do this first** —
it makes every other borrow safer and faster to land, and it directly improves the Track A
verification loop. *Source: BasiliskII `AARCH64_JIT_BRINGUP.md` (verification framework, barrier
bisection, PC trace).* **Ties into:** ROADMAP **A1**; helps clear OPTIMIZATION-PLAN **§0b-extra4**
(the residual `blr`/`bclr` verify false positive).

**X2. 🟡 Backward CR0-liveness — scoped as intra-block dead-flag elimination first.** BasiliskII's
full version is cross-block lazy flags (heavy). The *clean subset* is a backward scan **within a
block** that elides CR0 writes never consumed before they're overwritten. That needs **no
cross-block state**, and — crucially — the liveness pass **is the safety proof we were missing**
when lazy CR0 regressed. This reframes OPTIMIZATION-PLAN **§0g** from "risky, disabled" to
"ownership-proven": materialize CR0 only for proven consumers. This is "complexity by proof" done
right; the cross-block extension stays an optional later stretch, never a prerequisite. *Source:
BasiliskII `AUDIT_AREA3_FLAGS_LIVENESS.md` (`needed_flags`, backward liveness, forced-importance
flush).* **Ties into:** OPTIMIZATION-PLAN **§0g**, ROADMAP **B2**.

### Tier 2 — worth it, but take the *light* version (not BasiliskII's implementation)

**X3. 🟡 Guarded resolution of `bcctr`/`bclr` — via the R2 inline cache, *not* edge profiling.**
This is the single biggest perf lever (`bcctr` = **98.5% of JIT misses**, falls back to the
interpreter; native `bcctr` stalled on Mixed-Mode-Manager RE — §P2). BasiliskII solves the same
*class* of problem (indirect successor unknown at compile time) **without RE**: a guarded cached
indirect target with a downgrade-on-miss path. **But its implementation is heavy** (edge-profiling
counters + stable-edge promotion + the `jmpdep` dependency graph). Take the **light** form: our own
**§R2 inline direct-mapped cache** (last-seen target PC → block ptr + guard, Dolphin JitArm64
model), informed by BasiliskII's guard/threshold/downgrade *design* but **without** importing the
profiling apparatus. *Source: BasiliskII edge-profiling (`BasiliskII-next-phase-plan.md`) as
reference; implement as OPTIMIZATION-PLAN §R2.* **Ties into:** OPTIMIZATION-PLAN **§P2 / §P9 / §R2**,
ROADMAP **B2/B3**.

### Tier 3 — cheap and conditional

**X4. ⏸ Mid-block spcflags poll for long blocks.** We poll spcflags **only at block entry**;
BasiliskII injects a tick/spcflags check every ~64 instructions so long blocks don't defer
interrupts. One-knob change — **justified only if** profiling (ROADMAP **B1**) shows long blocks
(the DR-emulator dispatch loops are the candidate) deferring interrupts. Measure before adding.
*Source: BasiliskII `AARCH64_JIT_BRINGUP.md` (mid-block tick injection).*

**X5. ⏸ Barrier taxonomy (B0–B3) — as a mental model / doc lens, not code.** Borrow the
classification to audit which of our "return false → interpreter" fallbacks are genuinely
interpreter-only (B3) vs. native-safe-by-proof (B1). Might reclaim a handful of fallbacks. It's a
classification exercise, not runtime machinery. *Source: BasiliskII `AARCH64_JIT_BARRIER_CLASSES.md`.*

### Methodology borrow

**X6. ⏸ Golden-workload maturity ladder (L0→L5).** BasiliskII gates JIT changes on a ladder
(L0 interp-baseline → L1 JIT-parity → L2 opcode-harness → L3 ROM-harness → L4 desktop-boot → L5
graphics/perf). We have a golden-workloads doc already; the *laddered gating* concept is a
low-cost organizing borrow for deciding when a change is "mature enough to ship." *Source:
BasiliskII `AARCH64_JIT_GOLDEN_WORKLOADS.md` (maturity ladder).*

---

## Does the fast target hardware change the ranking? (Yes — re-ranked 2026-06-05)

Target host = Apple Silicon (M-series) now, possibly fast Linux ARM64 later. Guest = Mac OS 8.6/9
+ 1990s PPC software that wanted **100–500 MHz** chips. Current JIT measures **Speedometer CPU
≈65** (≈ a G4 ~800 MHz–1 GHz; `docs/BENCHMARKS.md`) — i.e. **the emulator already runs the guest
faster than nearly every PowerPC Mac that ever shipped it.** Two independent sub-agent reviews
(perf-economics + strategic-ROI lenses, 2026-06-05) converged on the same conclusion:

- **The fast host doesn't re-rank the list — it confirms the list is mostly *not* a perf set.**
  Four of six borrows (X1, X4, X5, X6) were never primarily throughput borrows, and they are
  exactly the four whose value is **independent of host speed** (correctness, latency, clarity,
  methodology). They rise to the top.
- **The two genuine throughput borrows (X2, X3) are the two the hardware deflates.** A 5–15% gain
  (X2) is imperceptible on a host already ~10× over the guest's target clock. And — the stronger,
  hardware-independent argument — **X2/X3 are premature by our own rule**: they feed Track-B levers
  gated on the **B1 execution-weighted profiler, which is not built**, and X3's headline "98.5% of
  JIT misses" is *compile-frequency, not runtime-weighted* (OPTIMIZATION-PLAN §P2 says impact is
  "Unknown"). Tuning them now violates "performance by earned sophistication."
- **X4 is the latency exception.** Fast hardware kills *throughput* pressure but not *latency* —
  a long block that defers the 60 Hz VBL/interrupt poll still causes timer jitter / audio glitches
  (arguably worse on a fast host: one uninterruptible burst). So X4's motivation survives. Still
  measure-first (gate on B1).
- **"Correctness IS the product."** For a fidelity emulator that a fast host already makes fast
  *enough*, a 100%-correct-but-2×-slower JIT beats fast-but-subtly-wrong. This is why X1/X6 top the
  list and why the fast host *raises* their relative value.

**Two caveats that keep X2/X3 as DEFER, not DROP:**
1. **Weak future hosts.** If Raspberry-Pi-class Linux ARM ever becomes a *committed* target, the
   M-series headroom vanishes and X2/X3 regain real value — **re-run this verdict then.** (Today
   it's a "maybe", and it cuts against the Silicon-Sheep Apple-Silicon-only direction.)
2. **X3 is cheap (~1 day, low-risk).** Keep it warm as a DO-IF-CHEAP option behind B1 — just keep
   it the light §R2 form, never BasiliskII's edge-profiling/`jmpdep` web.

**Meta-finding:** the perf that *does* still bite on fast hardware lives **outside these six
borrows** — the FP register allocator (§P5b; ~30× internal FP-vs-integer gap) and HLE/vectorization
of the scalar-copy cliff. These borrows target the already-fast integer path. If perf is the goal,
those are the levers, not X2/X3.

**Re-ranked order of attack (supersedes the line below):** **X1 → X6 → X4 (measure-first) → X5**,
with **X2 demoted** to a correctness exercise (build the liveness *proof*, skip the perf flip) and
**X3 frozen** behind the B1 profiler.

---

## Explicitly leave behind

These drag the heavy web in, or have low payoff for PPC — **do not import**:

- **`jmpdep` dependency graph** — our targeted chain-patch reversion already handles the common
  invalidation case; full-flush only fires on *repeated* invalidation. Revisit only if flush
  storms show up in a profile.
- **Block-lifecycle state machine** (`BI_ACTIVE`/`BI_NEED_RECOMP`/…), **multi-representation PC**
  (`pc_p`/`pc`/`pc_oldp`), **validated-vs-direct handler split + checksum validation** — all part
  of the lazy-state safety apparatus we don't need.
- **x86-UAE flag-format plumbing** (carry-inversion normalization, FLAGX bit-29 format,
  `COPY_CARRY` masking) — accidental complexity from BasiliskII's x86 lineage; we hand-wrote
  PPC→ARM64 flags directly and don't inherit it.
- **Cross-block lazy flags/registers as the default** — the Phase-5 BasiliskII direction. Our
  controlled equivalents (X2 intra-block CR0 liveness; §P8 cross-block pinning of r1/SP, r2/RTOC)
  are the ownership-proven version; the speculative version is rejected by JIT-STYLE-DECISION.

---

## Quick map to existing backlog

| Borrow | Light? | Existing item it feeds | Track |
|---|---|---|---|
| X1 verify/bisection tooling | ✅ clean | OPTIMIZATION-PLAN §0b-extra4; ROADMAP A1 | A |
| X2 backward CR0-liveness | ✅ clean (intra-block) | OPTIMIZATION-PLAN §0g (lazy CR0 re-enable) | B |
| X3 guarded indirect branch | ⚠️ take §R2, not edge-profiling | OPTIMIZATION-PLAN §P2 / §P9 / §R2 | B |
| X4 mid-block spcflags poll | ✅ clean, conditional | gated on ROADMAP B1 profiler | B |
| X5 barrier taxonomy | ✅ doc lens only | audit of interpreter fallbacks | — |
| X6 maturity ladder | ✅ methodology | `AARCH64_JIT_GOLDEN_WORKLOADS.md` (SS) | A |

**Order of attack:** see *"Does the fast target hardware change the ranking?"* above — the
hardware-aware order is **X1 → X6 → X4 (measure-first) → X5**, with X2 demoted to a correctness
exercise and X3 frozen behind the B1 profiler. (The earlier "X1 → X2 → X3 as the big perf lever"
framing was wrong for a fast host — X3 is the weakest survivor, X2's perf case evaporates.)
