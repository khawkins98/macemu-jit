# BasiliskII → SheepShaver JIT cross-pollination

> **Status:** 📖 Reference — initial triage complete, pick up as needed · **Created:** 2026-06-05 · **Updated:** 2026-06-10
> **Why this doc exists:** Triage of which BasiliskII (68K JIT, the more mature same-host lineage from the upstream Linux ARM64 developer) techniques we want to borrow for the SheepShaver PPC JIT — and which heavy machinery to leave behind.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../CONTRIBUTING.md) → "Documentation Lifecycle")._

## Two ground rules

These set every priority below.

**Borrow what comes over clean; leave the heavy machinery.** BasiliskII's *codegen* is 68K — it
doesn't transfer. Its *infrastructure and methodology* do (same ARM64 host). But its heavy
apparatus — lazy state across blocks, the three-part PC (`pc_p`/`pc`/`pc_oldp`), block-lifecycle
states, validated-vs-direct handlers, the `jmpdep` graph — is a bad trade for us: it moves bugs
away from their cause (our own disabled lazy-CR0 is the proof), needs a wall of audit docs to stay
legible, and pays off most on 68K's condition-codes-everywhere ISA — far less on PPC, where CR
writes are opt-in (the `Rc` bit). We keep SheepShaver's immediate-writeback simplicity and take
only ideas that don't drag that web in. For PPC *codegen* the better source is MAME's `ppcdrc.cpp`
(IMPLEMENTATION-BACKLOG C2), not BasiliskII.

**On fast hardware, correctness beats throughput.** Host target is Apple Silicon (M-series); the
guest is 1990s PPC software that wanted 100–500 MHz chips. We already measure ≈G4-class
(Speedometer CPU ≈65, `docs/BENCHMARKS.md`) — faster than the guest ever ran on real hardware. So
throughput borrows earn little, and correctness / tooling / latency borrows earn most. (If a weak
Linux ARM target — Raspberry-Pi-class — ever becomes committed, the throughput items regain value;
revisit then. The throughput levers that still bite even on M-series are the FP register allocator
§P5b and HLE — *not* anything in this list, which targets the already-fast integer path.)

## What we want to do

| Borrow | Effort | Payoff | Priority | Feeds |
|---|---|---|---|---|
| **X1** verify & bisection tooling | Medium | **High** | 🔜 do soon | A1 · §0b-extra4 |
| **X6** golden-workload maturity ladder | Low | Medium | 🟡 do (cheap) | A1 |
| **X4** mid-block spcflags poll (long blocks) | Low | Medium (latency) | ⏸ measure-first | gated on B1 |
| **X5** barrier taxonomy as a doc lens | Low | Low | 🟡 opportunistic | — |
| **X2** intra-block CR0-liveness → lazy CR0 | Medium | Low (perf) | ⏸ defer | §0g |
| **X3** light inline cache for `bcctr`/`bclr` | Low (~1 day) | Low (unmeasured) | ⏸ defer | §P2 · §P9 · §R2 |

---

### X1 — Verify & bisection tooling
**Effort:** Medium · **Payoff:** High · **Priority:** 🔜 do soon

**Do:** port BasiliskII's three debug knobs — targeted-PC verify (`VERIFY_PCS`, so the differential
oracle is fast enough to actually run), force-a-family-to-the-interpreter bisection
(`BARRIER_FAMILIES`, to binary-search a codegen bug), and aligned dual interp/JIT trace (`PCTRACE`,
first-divergence by PC).

**Why it's top priority:** correctness is independent of host speed, and this de-risks every other
change. We have no family-bisection knob today, and `SS_JIT_VERIFY` is too slow to use routinely.
Clean dev infrastructure — no runtime coupling. Clears OPTIMIZATION-PLAN §0b-extra4.

---

### X6 — Golden-workload maturity ladder
**Effort:** Low · **Payoff:** Medium · **Priority:** 🟡 do (cheap)

**Do:** gate JIT changes on a ladder (L0 interp-baseline → L1 JIT-parity → L2 opcode-harness → L3
ROM-harness → L4 desktop-boot → L5 graphics/perf), layered onto our existing golden-workloads doc.

**Why we want it:** our recurring failure mode is regressions shipped on a green opcode harness
(vsel, ev_mixed, vacuous FP). A laddered gate attacks exactly that discipline gap, and it's the
precondition for safely attempting X2. Methodology value is independent of host speed.

---

### X4 — Mid-block spcflags poll for long blocks
**Effort:** Low · **Payoff:** Medium (latency) · **Priority:** ⏸ measure-first

**Do:** inject a spcflags/tick check every ~N instructions inside long blocks, instead of only at
block entry.

**Why it survives the fast-host filter but waits:** fast hardware kills *throughput* pressure but
not *latency* — a long block (the DR-emulator dispatch loops are the candidate) defers the 60 Hz
VBL/interrupt poll, causing timer jitter or audio glitches (arguably worse on a fast host, where it
runs as one uninterruptible burst). One-knob change. Low priority only because it's conditional:
justified only if the B1 profiler shows long blocks actually deferring interrupts. Measure first.

---

### X5 — Barrier taxonomy as a doc lens
**Effort:** Low · **Payoff:** Low · **Priority:** 🟡 opportunistic

**Do:** classify our "return false → interpreter" fallbacks B0–B3 to see which are genuinely
interpreter-only (B3) vs. native-safe-by-proof (B1).

**Why it's low priority:** pure clarity, not runtime machinery; might reclaim a handful of
fallbacks (a throughput micro-win nobody feels — do it for the clarity). Cheap enough to fold into
other work when we're already in the dispatch code.

---

### X2 — Intra-block CR0-liveness → re-enable lazy CR0
**Effort:** Medium · **Payoff:** Low (perf) · **Priority:** ⏸ defer

**Do:** a backward intra-block scan that elides CR0 writes never consumed before they're
overwritten — used as the ownership *proof* that makes re-enabling OPTIMIZATION-PLAN §0g safe (no
cross-block lazy state).

**Why it's low priority:** the perf payoff (5–15% on Rc=1-heavy code) is imperceptible on a host
already ~10× over the guest's target clock, and lazy CR0 carries a real, un-root-caused regression.
If we build the liveness pass, build it as a correctness/clarity artifact behind X1 — not for the
perf flip. Gated on B1.

---

### X3 — Light inline cache for `bcctr`/`bclr`
**Effort:** Low (~1 day) · **Payoff:** Low (unmeasured) · **Priority:** ⏸ defer

**Do:** the §R2 guarded inline direct-mapped cache for indirect targets (last-seen target PC →
block ptr + guard, fall back to the hash dispatcher on miss). The **light** form only — *not*
BasiliskII's edge-profiling + `jmpdep` apparatus.

**Why it's low priority despite being cheap:** the headline "98.5% of JIT misses" is
*compile-frequency, not runtime-weighted* (§P2 itself says impact is "Unknown"), and the win is
pure throughput the user can't feel on fast hardware. Keep it warm as a cheap option behind the B1
execution-weighted profiler; it earns no correctness halo (the interpreter fallback is already
safe). Sidesteps the Mixed-Mode-Manager RE that stalled native `bcctr`.

---

## Explicitly leave behind

These drag the heavy web in, or have low payoff for PPC — **we do not want these:**

- **`jmpdep` dependency graph** — our targeted chain-patch reversion already handles the common
  invalidation case; full flush only fires on *repeated* invalidation. Revisit only if a profile
  shows flush storms.
- **Block-lifecycle state machine**, **multi-representation PC** (`pc_p`/`pc`/`pc_oldp`),
  **validated-vs-direct handler split + checksum validation** — the lazy-state safety apparatus we
  don't need.
- **x86-UAE flag-format plumbing** (carry-inversion, FLAGX bit-29, `COPY_CARRY` masking) —
  accidental complexity from BasiliskII's x86 lineage; we hand-wrote PPC→ARM64 flags directly.
- **Cross-block lazy flags/registers as the default** (BasiliskII's Phase-5 direction). Our
  controlled equivalents — X2 intra-block CR0-liveness and §P8 cross-block pinning of r1/SP, r2/RTOC
  — are the ownership-proven version; the speculative version is rejected by
  [`JIT-STYLE-DECISION.md`](../JIT-STYLE-DECISION.md).
