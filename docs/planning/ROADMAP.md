# Roadmap / Work Tracker — `macos-arm64`

> **Status:** 🟡 Active · **Created:** 2026-06-04 · **Updated:** 2026-06-07 (added project arc / phase framing)
> **Why this doc exists:** The single tracker for all outstanding work, arranged into four tracks so context survives across pickups.


The single place to **arrange and track outstanding work** so context survives across
pickups (when we focus on one task we don't lose the others). When you pick up or finish a
task, update its **Status** line. This is the **map**, not the territory — deep analysis,
design, and per-item detail live in the linked docs. Keep entries to a few lines + a pointer.

**Legend:** 🔜 next up · 🟡 open · ⏸ deferred/optional · ✅ done

---

## Project arc — where this is going

The motivating idea: **SheepShaver was built for a resource-constrained era; we are not.** An
M-series Mac has orders of magnitude more CPU, RAM, and I/O than the machines SheepShaver
targeted. That headroom lets us *widen* what we emulate — model more of the complete PowerPC
Mac stack, more faithfully — rather than only making the existing narrow slice faster. The
strategy is deliberately sequenced so each phase rests on the one before it:

Lifecycle (the README narrative): **run it → drive/test it → measure it → widen what it runs → make it
fast.** The four phases below are that lifecycle grouped for tracking — **phase 2 "Instrumentation"
covers both the *drive/test* and *measure* lifecycle stages** (the harnesses and the benchmarks):

| Phase | Thrust | State |
|-------|--------|-------|
| **1. Foundation (run)** | Native **AArch64 JIT** on macOS — SheepShaver boots Mac OS 8.6/9 to Finder with full PPC→ARM64 codegen, on Apple Silicon. | ✅ done |
| **2. Instrumentation (drive/test + measure)** | **Tools to control/validate + empirical benchmarks** — differential opcode harness (`make test-jit`), E2E boot/workload harness + guest-UI introspection, Speedometer/MacBench capture, kernel microbench (`a64/op`), per-block/mix profiler. The safety net that makes everything after it measurable. | ✅ done (maintained) |
| **3. Widen emulation** | Emulate **more of the full PowerPC Mac stack** — the structural gaps SheepShaver never closed (AltiVec reachable by guests ✅ first win; broader OS/software: New World ROM → 9.1/9.2, fuller device/OS modeling). Correctness first, measured against the Phase-2 benchmarks. **Current primary thrust.** | 🔜 next |
| **4. Optimize** | *Then* make it faster — per-block overhead ceiling, cross-block pinning, a vector register allocator (P-VRA), HLE — with Phase-2 benchmarks gating every change as a regression check. | 🟡 levers open, paced behind Phase 3 |
| **Cross-cutting: Silicon Sheep** | A first-class macOS desktop experience (Tauri launcher/VM manager + Inspector). Runs alongside all phases. | ⏸ researched / in progress |

**Reference for Phase 3:** [DingusPPC](https://github.com/dingusdev/dingusppc) is the active
reference for fuller PowerPC-Mac-stack emulation (interpreter-only, so ideas not codegen;
GPLv3 reuse is license-feasible — see `docs/planning/DINGUSPPC-EVALUATION-PLAN.md`).

> **Phase-3 priority decision (2026-06-07):** see `docs/planning/COMPATIBILITY-PAYOFF-DINGUSPPC-REVISIT.md`.
> Bottom line: the biggest compatibility payoff is **not** the OS-version frontier (NewWorld ROM →
> maybe MMU) but **making the already-booting 8.6–9.0.4 range run wanted software *usably*** —
> `CopyBits` HLE + idle-skipping + `.ndrv` video (the "attack the worst layer" levers, gated by a
> half-day call/rect histogram probe). "ROM-first, MMU-deferred" ordering is CONFIRMED but re-ranked
> as a *lower-EV exploratory* track. NOTE these top levers blur the Phase-3/Phase-4 line: "runs
> unusably slow" is arguably a *compatibility* gap, but the work is perf-shaped — a strategy call.

> **How this maps to the four tracks below.** The phases are the *narrative*; Tracks A–D are
> the *tactical backlog*. Phase 2 ≈ Track A (verification) now in maintenance. Phase 3 (widen)
> is new work that will mostly land as new Track-A correctness items + targeted Track-D breadth.
> Phase 4 ≈ Track B (perf). Silicon Sheep ≈ Track C.

---

## Workstreams at a glance

The project has grown four parallel tracks. They are largely independent — pick by appetite,
not by order. The only hard sequencing is *within* a track (noted per item).

| # | Track | What it buys | State | Lead doc |
|---|-------|--------------|-------|----------|
| **A** | **Correctness & verification** | Trust that the booting emulator isn't silently wrong | 🔜 active front | `docs/TESTING.md` |
| **B** | **Performance / JIT optimization** | Faster guest execution | 🟡 cheap wins landed; big levers open | `docs/planning/OPTIMIZATION-PLAN.md` |
| **C** | **Desktop integration ("SiliconSheep")** | A Parallels-like, first-class macOS app experience | ⏸ researched, not started | `docs/planning/DESKTOP_INTEGRATION_PLAN.md` |
| **D** | **Platform breadth** | BasiliskII/68K on macOS; Linux/ARM re-convergence | ⏸ optional | per-item below |

> **Cross-cutting theme — verification is the bottleneck, not the fix.** The `vsel` bug, the
> AltiVec `ev_mixed` bugs, and the "vacuous FP vectors" all came from one root: the harness
> silently passed wrong codegen (results hid in VRs/FPRs/memory the REGDUMP doesn't check).
> Track **A** pays off across every other track — especially **B**, where a perf change that
> corrupts state must be caught, not shipped.

> **⚠️ Cross-cutting decision (gates C *and* D) — do we hard-fork?** Track C's
> `docs/planning/DESKTOP_INTEGRATION_PLAN.md` proposes a pruned, Apple-Silicon-only hard fork ("Silicon
> Sheep") that *removes* BeOS/AmigaOS/Windows/SDL1 and tracks upstream by cherry-pick only.
> That premise is **in direct tension with Track D**: a pruned macOS-only fork is unlikely to
> invest in D1 (68K BasiliskII) or D2 (Linux/ARM re-convergence + VDE test rig). Decide the
> repo's identity — *is this branch the product, or the seed of a separate fork?* — **before**
> starting either C-Tier-1 or any D backport, because the answer can invalidate the other track.

---

# Track A — Correctness & verification  ← *picking up first*

## A1. 🟡 Testing infrastructure — trustworthy JIT verification *(in progress)*

**Progress (2026-06-04):** ✅ REGDUMP now dumps all 32 **FPR** (raw 64-bit) + 32 **VR** (raw
128-bit); `run.sh` raw-diffs the line, so `make test-jit` finally compares FP/AltiVec results
(interp vs JIT). Still **255/255 score=100** — which now *proves* the current suite has no
hidden FP/VR divergence (commit `9439666a`). Side effect: FP/VR results are no longer
"vacuous" (they're captured), so the vacuousness-guard scope below narrows to memory-only /
otherwise-unobservable results.
✅ **Quarantine lane added + widened to the full worklist** (commits `4f5825bd`, `aef9fb34`):
`QUARANTINE_ORDER` vectors run in JIT mode but don't count toward score, so `score=100` stays
meaningful while confirmed bugs are *tracked*. Originally held **all 9** `ev_mixed` divergences;
all 9 (merges `vmrgh{b,h,w}`/`vmrgl{b,h,w}`, pack `vpkuhum`, byte multiplies `vmulo/eub`) have
since been fixed and promoted — **the quarantine lane is now empty**. The mechanism (a fix flips
its vector `xfail`→`xpass` and `make test-jit` prints "promote to TEST_ORDER") stays available for
the next confirmed divergence; it was also used to rule out approach A — see A2.
✅ **`SS_JIT_VERIFY` now compares FPR + VR** (commit `6837f642`): the boot-time oracle catches
FP/AltiVec divergence in real software (`SS_JIT_VERIFY=1 ./SheepShaver`), not just GPR/flags —
the in-the-wild validation path for A2.
**Remaining A1:** ✅ the merge vectors are now on **distinct operands** (vA=`00..0F`, vB=`10..1F`)
— done as part of the A2 byte/hw merge fix. Still open: the scored **arithmetic** word-op vectors
(`vadduwm`/`vsubuwm`/`vmaxsw`/…) use uniform palindrome operands (`0x05..`/`0x03..`) that can't
catch a byteswap/lane bug — regenerate with **distinct AND carry-inducing** operands before
trusting any *broad* VR-codegen change. Safe + here-verifiable.

**Why:** the recurring failure mode (above). Three rounds of vacuous/masking AltiVec vectors
slipped the harness; FP vectors were vacuous for months. Fixing more codegen on top of a
harness that can't catch mistakes just produces the next silent bug.

**Scope (✅ = landed this cycle):**
- ✅ **REGDUMP compares FPR + VR** (harness) and ✅ **`SS_JIT_VERIFY` compares FPR + VR** (boot
  oracle) — the two blind spots that hid every FP/AltiVec bug are closed.
- ✅ **Quarantine lane** tracks the `ev_mixed` divergences as `xfail` (the A2 gate); down to 3
  (pack + 2 multiplies) after the merge family landed.
- 🟡 **Stronger AltiVec operands** — ✅ done for the *merges* (distinct operands). Still open for
  the scored **arithmetic** word-ops: regenerate with **distinct AND carry-inducing** operands
  (small-distinct still masks byteswap-carry; see A2 testing-gap note). Needed before any *broad*
  VR change.
- 🟡 **Vacuousness guard** on `run.sh` — **shallow tier ✅ landed (2026-06-05)**: both harness
  preflights (`SheepShaver/jit-test/run.sh` + `BasiliskII/jit-test/run.sh`) now reject an
  **all-NOP body** (PPC `60000000` / 68K `4E71`) — exercises only decode/dispatch, asserts
  nothing — with `nop`/`nop_triplet` allow-listed (verified: rejects synthetic all-NOP, zero
  false positives across SS 264 / B2 452 vectors; CHANGELOG 2026-06-05). **Deep tier still
  open**, and narrowed to **memory-only / otherwise-unobservable** results (FP/VR are captured;
  the generators already build FP/VR results into a GPR). Evaluated and *deliberately not
  pursued now*: the highest-value case (FP/VR observability) is already defended at generation
  time, leaving only the trivial-operand / store-without-load-back residue, which needs a
  sentinel/mutation harness design for modest incremental yield. Lower urgency.
- 🔜 **Broaden AltiVec / FP operand coverage *(highest residual-bug density — do this first)*** —
  the suite has ~13 scored AltiVec + 9 quarantined ops out of ~100+, and is integer-heavy overall;
  FP/AltiVec are exactly the paths boot-to-Finder exercises *least*, so they're the likeliest place
  a residual codegen bug still hides. New curated vectors here are permanent CI assets (vs.
  throwaway triage). Grow `gen-altivec-vectors.py` (+ FP) alongside A2. *(Strategy-review verdict
  2026-06-06: ranked above the rom-harness GPR triage below.)*
  - 🐛 **HUNT PAID OFF (2026-06-06) — confirmed AltiVec shift/rotate bug family.** Leading with the
    zero-coverage variable-shift family (where NEON ≠ AltiVec 1:1) found **real, oracle-validated**
    codegen bugs in minutes: (1) variable shifts lack mod-element-width masking (`vslb` shift 9 →
    JIT 0, interp 0x08); (2) logical right shifts `vsr{b,h,w}` emit the wrong shift (`vsrb` shifts
    *left*; `vsrh/vsrw` arithmetic not logical); (3) rotates `vrl{b,h,w}` use plain shift (suspected,
    needs lvx repro). Full repros + fix plan: `docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md`.
    ✅ **FULLY FIXED (2026-06-06) — all 12 ops:** byte/halfword/word shifts (`vsl/vsr/vsra{b,h,w}`,
    mask mod width + truncating `USHL/SSHL`) + rotates (`vrl{b,h,w}`, synthesized
    `(x<<k)|(x>>>(w-k))`). Capstone-verified, validated byte-asymmetric (ev_mixed byte order
    covered); **12 strong vectors added, `make test-jit` 282/282** + boot smoke. Proof the AltiVec/FP
    surface is *live*, not theater — and it establishes the fix loop (capstone encoding →
    `SS_TEST_HEX` repro → committed vector → `test-jit`).
  - 🐛 **SWEEP CLUSTER 2 (2026-06-06) — saturating add/sub + signed averages, 14 more bugs FIXED.**
    A broad sweep (every untested VX op, JIT vs real interp) + sub-agent audit found another cluster:
    sat-add emitted `SABA/UABA` (abs-diff, wrong op), sat-sub had **signed/unsigned swapped**, signed
    averages emitted `SMAXP`. Fixed → `UQADD/SQADD/UQSUB/SQSUB/SRHADD` (capstone-verified); 14
    vectors, `make test-jit` **296/296** + ISO boot smoke. Compares confirmed correct. **Remaining
    (structural, overlap A2):** pack-saturate/pixel-pack-unpack/sum-across (scrambled labels, 2-source
    ev_mixed narrows, pixel-field expansion, saturation) + FP-conv UIMM-scale. Worklist:
    `docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md` "Broad-sweep results". **(26 AltiVec subtotal; +1 FP below = 27 codegen fixes this session.)**
  - 🐛 **FP SWEEP (2026-06-06) — `fctiw`/`fctiwz` conversion FIXED; rest of FP clean.** Pivoted the
    method to the under-tested FP ops (vs real interp, edge-case operands). `fsel`/`fnabs`/single
    fused (`fmsubs`/`fnmadds`/`fnmsubs`) all clean. `fctiw`/`fctiwz` both emitted 64-bit `FCVTZS Xd`
    → `fctiw` ignored FPSCR rounding (cloned `fctiwz`), overflow mis-saturated, NaN→0. Fixed: 32-bit
    `FCVTAS Wd` (`fctiw`, round-half-away = `frin` = default RN) / `FCVTZS Wd` (`fctiwz`) + NaN→`0x80000000`
    fixup; 5 vectors, **`make test-jit` 302/302**. `fsqrt`/`fres`/`frsqrte` un-sweepable (interp lacks
    them / estimates). **27 codegen bugs fixed total this session.**
    - 🟡 **OPEN (adversarial-review finding, filed) — `fctiw`/`fctid` non-default FPSCR[RN].** `fctiw`
      hardcodes `FCVTAS` (ties-away), correct only for RN=0 (the practical default); RN=1/2/3 diverge
      (review confirmed via `mtfsfi`). `fctid` is *also* wrong at RN=0 (uses fixed `FCVTNS`=ties-even,
      but the interp's RN=0 is ties-**away**). **Fix:** runtime 4-way dispatch on FPSCR[RN] →
      `FCVTAS`(0)/`FCVTZS`(1)/`FCVTPS`(2)/`FCVTMS`(3) (note PPC RN=0 is away, *not* ARM nearest-even,
      so `FRINTI`+FPCR.RMode won't work — the `emit_sync_fpscr_rounding` map sends RN=0→ARM-even).
      Validate all 4 modes vs interp. **Practical risk low** (Mac OS ABI default RN=0; compilers don't
      emit RN changes) but genuine (the RN-sync machinery exists because RN *does* change at runtime).
- 🟡 **rom-harness — span-gate ✅ done; recover coverage + triage survivors next** *(2026-06-06)*.
  The standalone differential rom-harness now completes broad sweeps (skip-not-abort fix,
  `c1a10c0a`). Its failures were dominated by a **block-model mismatch** (scanner ends a block at
  `bc` opcode 16; the JIT runs *past* it → mismatched spans, same root as `SS_JIT_VERIFY` fix-(i)).
  ✅ **Span gate landed:** compares only when `jblk.n_insns == blk.n_insns`, reports the dropped
  blocks as `Span mismatch` (visible, not silent) — on the OldWorld ROM this cut ~46 failures/seed
  to **~7–8 span-matched, trustworthy failures** (≈711 bc-terminated blocks skipped). The remaining
  failures are now genuine span-matched divergences (PC/CR-dominated), no longer cascade artifacts.
  ✅ **Survivor triage done (2026-06-06) — integer path differentially CLEAN, zero real JIT bugs.**
  Refereed 3 distinct survivor patterns via the real emulator (`SS_TEST_HEX`/`SS_TEST_INIT`,
  `SS_TEST_JIT={0,1}`): a pure `bl` (`4bffd1b9`), `extsh r3,r7`+`bl` (harness claimed `jit` clobbered
  source r7), and `mr;addi;li;bl` (harness claimed `jit` zeroed untouched r9/r11). In **all three**
  real-interp == JIT == correct (e.g. `extsh r3,r7` leaves r7 intact, r3=`0000197d`). So every
  survivor is a **harness-side artifact**: all are `b`/`bl`-terminated, and the harness's *JIT* run
  follows the branch at runtime (into the relative target = real ROM code that mutates registers)
  while its reference interp stops at the block end — a *runtime* branch-follow the *compile-time*
  span gate doesn't catch. **Remaining harness-side follow-up:** stop the JIT following the branch in
  the harness (disable chaining / snapshot regs at block exit before any follow), OR (a) recover the
  span-gate coverage by running the interp for the JIT's instruction count. Neither is a JIT bug;
  both are rom-harness polish. **Net: the rom-harness has served its purpose for the integer path —
  pivot effort to AltiVec/FP vectors (higher residual-bug density).**
- 🟡 **Unified test-session instrumentation** *(design, 2026-06-06)* — the four oracles (test-jit,
  rom-harness, `SS_JIT_VERIFY`, E2E/bench) + diagnostics each write ad-hoc text to a manually-chosen
  path; there's no shared run-stamp, schema, or cross-oracle reconciliation. Spec proposes one
  `SS_RUN_DIR` convention + a JSONL record schema (with an `oracle`-trust field) + a reconciliation
  analyzer that **auto-referees disagreements via `SS_TEST_HEX`** (automating the manual triage done
  2026-06-06) and **joins HOT-PC "what's hot" with microbench "what's slow"** for optimization leads.
  Builds on the benchmark-export run-stamp pattern + `jit-analyze.py`. **Phase 0+2 (schema + auto-
  referee) are the must-haves; 1/3 are convenience.**
  ✅ **Phase 0+2 delivered in practical form (2026-06-06): `tools/jit-diff-sweep.py`** — a committed,
  reusable differential AltiVec/FP op-sweep that runs the real emulator interp-vs-JIT (its own
  auto-referee), emits run-stamped JSONL + `summary.json` under `$SS_RUN_DIR` (the schema), tracks
  known-broken ops + skips un-referee-able ones, and prints copy-paste repros for FAILs (current:
  pass=38/FAIL=0/known-broken=3). 🟡 **Remaining:** Phase 1 orchestrator (`make test-session`
  aggregating all oracles) + in-emulator C++ JSONL emitters (so `SS_JIT_VERIFY`/heartbeat feed the
  same run dir). Design: `docs/superpowers/specs/2026-06-06-unified-test-session-instrumentation-design.md`.
- 🟡 **Paranoia FP conformance** runner wiring + CI (18 in-harness FP vectors landed; the
  self-grading torture run is still manual — needs a boot rig + disk image).
- ⏸ **(stretch) golden-result oracle** — revive the PowerPC Emulator Tester against recovered
  G4 golden results (catches bugs shared by *both* interp and JIT). See IMPLEMENTATION-BACKLOG T1.
- 🟡 **Borrow BasiliskII's verify/bisection tooling** — targeted-PC verify (fast enough to run)
  and aligned dual-trace divergence. **Partly already present (2026-06-05 audit):** the
  "force opcode family → interpreter" bisection knob *exists* — `SS_JIT_SKIP_OPC` (primary),
  `SS_JIT_SKIP_XO` (XO31), `SS_JIT_SKIP_XO63` (FP), `SS_JIT_SKIP_XO19` (CR/branch) — so manual
  bisection works today; ✅ **targeted-PC verify scoping landed (2026-06-05)** —
  `SS_JIT_VERIFY_PC=LO:HI` restricts the oracle to one PC range (re-check a suspect block
  without the whole-boot slowdown). Remaining gap: **automated aligned dual-trace**. Clean dev
  infrastructure, no runtime coupling; helps clear OPTIMIZATION-PLAN
  §0b-extra4. Detail: `docs/planning/sheepshaver-research/BASILISKII-CROSS-POLLINATION.md` X1.
  - **Sharpened by the P1a sweeps (2026-06-05):** the current `SS_JIT_VERIFY` differential
    oracle has **6 false-positive classes** (full taxonomy in OPTIMIZATION-PLAN §0b-extra4) and
    **cannot produce a clean whole-boot run** (low report budget → goes dark at ~22%; high
    `SS_JIT_VERIFY_BUDGET` → verify-every-block starves the guest timer into an early-boot ROM
    spin). Concrete X1 work items that emerged: ✅ (i) make the interp replay **mirror the JIT
    block's path + real terminator** (kills chaining/return/loop/conditional/PC) — **DONE
    2026-06-05 (`5ac5e676`)**, boot-validated: those 5 classes clean boot-wide; plus a per-block
    report dedup (`7314bc9c`) so one memory-RMW block can't exhaust the budget; ✅ (iii) a
    **targeted/sampled** verify (`SS_JIT_VERIFY_PC`) — landed; 🟡 (ii) **snapshot+restore the
    touched guest memory** around the replay (kills the memory-RMW class — the one that fools a
    PC-match filter) — **the only remaining confound**; design (interp-first reorder + RAM-write
    journal, verify-gated) **designed + review-vetted, spec'd, awaiting greenlight** —
    `docs/superpowers/specs/2026-06-05-verify-memory-snapshot-fix-ii-design.md`. **Verify sweep
    (2026-06-05), honestly scoped:** the oracle reports exactly four `SUSPECT` blocks, **all four
    class-6 memory-RMW** (3 encoding-certified, 1 most-likely) — **no real codegen bug in the
    covered region, but NOT whole-boot**: verify stalled the guest in an early-boot spin ~10 s in
    (`comp=10826` frozen, only 8 block PCs ever verified, ≈0.07 %; never reached Finder — the
    timer-starvation trap). **Deeper differential coverage is cheaper offline** (`make test-jit` /
    rom-harness, no timer dependency) — prefer that over boot sweeps. (ii) is the *certification*
    tool (turn artifacts "provably non-divergent") + deeper-verify enabler; **DEFER pending
    greenlight**, not low-value polish. Diagnostic knobs already landed: `SS_JIT_NO_CHAIN` (now logs a
    marker), `SS_JIT_VERIFY_BUDGET`, `SS_JIT_VERIFY_PC=LO:HI` (targeted scope), and a
    **SUSPECT/ARTIFACT-PC classifier** on each divergence (PC-match ⇒ real-bug candidate;
    PC-mismatch ⇒ structural) so the log is triagable (`grep '[VERIFY] SUSPECT'`) without
    waiting on fix (i). ✅ **Classifier + PC-scope boot-validated (2026-06-05)** against this
    session's ground truth: 7 structural blocks → `ARTIFACT-PC` (20/20), memory-RMW `100fd0e0` →
    `SUSPECT` (200/200, GPR10 stepping by 2). These make the *existing* confounded oracle usable;
    (i)+(ii) still needed to make it *clean*.

**Verifiable here** (harness/build, no boot needed). **Unblocks A2 and de-risks all of Track B.**
**Detail:** `docs/TESTING.md`; harness `SheepShaver/jit-test/run.sh`; generators `gen-*-vectors.py`.

---

## A2. 🟡 AltiVec `ev_mixed` correctness — tested ops COMPLETE; sweep (2026-06-06) reopened untested pack/pixel siblings

**Why:** **silent data corruption in the emulator that works** (SheepShaver). AltiVec is live
(the emulator advertises a G4). Only triggers when guest software uses the affected ops
(media codecs, AltiVec-era apps) — latent, not a boot-blocker, but the worst failure mode.

**Status:** splats (`vspltb`/`vsplth`) FIXED + merged; `vspltw`/`vsldoi` were already correct.
✅ **ALL merges `vmrgh/l {b,h,w}` FIXED + boot-verified + promoted (2026-06-04).**
- *Word merges* (`vmrghw`/`vmrglw`): the 6 `vmrgh*`/`vmrgl*` encodings were garbage
  (`0x..C400`/`0x..C800` — not permute ops); replaced with correct `ZIP1`/`ZIP2`. Word merges
  are correct as a plain `ZIP.4S` (`word_element` is identity under `ev_mixed`, no byte remap).
- *Byte/halfword merges* (`vmrgh{b,h}`/`vmrgl{b,h}`): needed more than the encoding. The fix is
  the **`REV32.16B` normalize** sequence (`emit_vmrg` in `ppc-jit.cpp`): the `ev_mixed` layout
  (bytes reversed within each 32-bit word) is *exactly* `REV32.16B` vs natural element order, so
  `REV32.16B` both inputs → `ZIP1`/`ZIP2.{16B,8H}` → `REV32.16B` back. (This is the **per-op**
  version of approach A — a *local* normalize works where the *global* load/store one failed,
  see below.)
- Verified `xfail→xpass` with **distinct operands** (vA=`00..0F`, vB=`10..1F`), promoted to the
  scored gate (**255→261, score=100**), and boot-verified under `SS_JIT_VERIFY` (zero VR/FPR
  divergence; the lone GPR/LR/PC divergence is the documented `blr`-boundary false positive).

✅ **pack `vpkuhum` FIXED + promoted + boot-verified (2026-06-04):** it ignored vA *and* used the
wrong op; the low-byte modulo pack is `UZP2.16B` on the `REV32.16B`-normalized inputs (reuses
`emit_vmrg`). xfail→xpass with distinct operands, scored gate 261→262.

✅ **even/odd byte multiplies `vmuloub`/`vmuleub` FIXED + promoted + boot-verified (2026-06-04):**
two bugs — non-widening `MUL.8B` (must widen 8×8→16) and no ev_mixed even/odd select. Fix
(`emit_vmul_byte`): `REV32.16B` normalize → `UZP1`(even)/`UZP2`(odd)`.16B` select → `UMULL.8H`
widen → `REV32.8H` output. Distinct operands exercise BOTH bugs (even-lane products >255 catch a
non-widening op). xfail→xpass, scored gate 262→264. **The ev_mixed quarantine lane is now EMPTY.**

**Siblings — ✅ FIXED 2026-06-07:** halfword multiplies `vmul{o,e}{u,s}h` (`emit_vmul_hword`:
`UZP` on `.8H` + `[SU]MULL.4S`, no output rev) + vectored, and all 7 packs (saturating +
`vpkuwum`). 🟡 **Still prospective** (no signed test vector): signed byte multiplies
`vmulosb`/`vmulesb` (share `emit_vmul_byte` with `SMULL`). 🔜 **New derivations remain:** AltiVec
pixel (`vpkpx`/`vupk{h,l}px`) + sum-across `vsum*`. All flagged in `ppc-jit.cpp`.

**🐛 REOPENED by the broad sweep (2026-06-06) — ✅ LARGELY CLOSED 2026-06-07.** The A1 sweep proved
several more `ev_mixed`/2-source ops were wrong (they never had vectors, so "class complete" only
covered the *tested* ops). Fixed 2026-06-07 with the per-op normalize approach + crossing vectors:
- ✅ **Pack-saturate** `vpk{sh,sw,uh,uw}{ss,us}` (398/462/270/334/142/206) + `vpkuwum` (78) —
  `emit_vpk_h2b`/`emit_vpk_w2h` (the old `fcvtn`/mislabeled-SQXTUN encodings fixed).
- ✅ **Pixel** `vpkpx` (782), `vupkhpx`/`vupklpx` (846/974) — `emit_vpkpx`/`emit_vupkpx`. (846 was
  indeed routed to a float-convert case — vcfsx/vcfux were at wrong XOs; also fixed.)
- 🔜 **Sum-across** `vsumsws`/`vsum2sws`/`vsum4sbs` (1928/1672/1800): `case` labels *rotated* + no
  saturation — STILL OPEN (new derivation: horizontal reduce + saturate).
**Next A2 step:** sum-across (+ scaled converts vcfsx/vcfux at their real XOs). Full
verified worklist (canonical XO, emitted-vs-correct NEON): `docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md`
"Broad-sweep results". Cheap adjacent cleanups: dead cases `1794/1858/1922` in `ppc-jit.cpp`
(unreachable, misleading comments) and the missing `vsubuws` (XO 1664) JIT case (falls back to interp).

**Root cause:** VRs are stored in the interpreter's `ev_mixed` byte order (bytes reversed
within each word). `emit_load_vr` loads raw via `LDR Q`; the JIT then indexes NEON lanes with
the raw PPC element → wrong element for any sub-word-rearranging op.

**⛔ Approach A (REV32.16B normalize at load/store, GLOBAL) — RULED OUT (2026-06-04).**
Tried on a throwaway branch: added `REV32.16B` to `emit_load_vr`/`emit_store_vr` + reverted the
splat remap, built, ran the harness. Result: **all 9 quarantine vectors stayed `xfail`**, and
`score=100` was *misleading* (the masking gap below). A *blanket* load/store transform changes
the in-JIT VR convention for every op at once and re-breaks the already-correct ones; it is the
wrong granularity.

**✅ Approach B (per-op) is the path — CONFIRMED for the merges.** Fix each op's actual NEON
sequence locally. The merges proved the technique: a **per-op, local** `REV32.16B` normalize
(`emit_vmrg`) is exactly approach A applied *inside one op*, and it works — the global version
failed only because it was global. Remaining targets, same method (derive against the
interpreter, flip the quarantine vector `xfail→xpass`):
- ✅ **`vpkuhum`** (halfword→byte pack): DONE — `REV32.16B` normalize + `UZP2.16B` + `REV32.16B`
  back (low byte = odd lane). `vpkuwum` (word→halfword) is the same shape, un-vectored.
- ✅ **`vmuleub`/`vmuloub`** (even/odd byte multiply): DONE — `emit_vmul_byte` (REV32.16B → UZP1/
  UZP2 select → UMULL.8H → REV32.8H). Signed/halfword siblings remain untested (above).

**⚠️ Testing gap found (A1 follow-up):** the scored word-op vectors (`av_vadduwm`/`vsubuwm`/
`vmaxsw`/…) use **uniform operands** (`0x05050505`/`0x03030303`) which are byteswap-palindromes,
so they can't catch a byteswap/lane bug — that's why approach A falsely scored 100. Regenerate
them with **distinct AND carry-inducing** operands (small-distinct like `00..0F`/`10..1F` still
masks it — sums don't carry across byte boundaries, so byteswap stays invisible; use values that
force inter-byte carries). Only matters for *broad* VR-codegen changes, not per-op approach B.

**Order of attack (simplest → hardest):** ✅ word merges → ✅ byte/halfword merges → ✅ pack
(`vpkuhum`) → ✅ byte multiplies (`vmuleub`/`vmuloub`) → ✅ **all packs + halfword multiplies
(2026-06-07)**. **All tested ops done.** Leftover: a signed byte-mult vector (`vmulosb/esb`,
prospective) + the new-derivation families (pixel, sum-across) — overlaps A1 "broaden AltiVec coverage".

**Done when:** ✅ all quarantine vectors `xpass`+promoted (quarantine now empty); ✅ all
packs + byte/halfword multiplies fixed+vectored (2026-06-07). **Still pending:** (a) the
`SS_JIT_VERIFY=1` boot — **NOTE:** AltiVec is dormant (no guest issues it, detection deferred —
B5/#23), so a verify boot does *not* exercise these ops; they're guarded by the differential
harness instead. (b) real-AltiVec software validation depends on detection landing first (→ A3/B5).
Harness coverage is one (saturation/signedness-crossing) pattern per op; a 2026-06-07 adversarial
sweep added 27 sets across the saturating packs (zero divergence).

**Depends on:** A1. **Needs boot verification** (yours) — the harness AltiVec coverage is partial.
**Detail:** `docs/planning/OPTIMIZATION-PLAN.md` §P1b/P5c; `CHANGELOG.md` [SheepShaver] 2026-06-04; `ppc-jit.cpp`.

---

## A3. 🟡 Validation gaps in shipped features

Shipped as build/boot-verified, **not exercised**:
- **VDE networking** — compiles + links, but never network-tested (does it actually move packets?).
- **SDL3 default** — boots to Finder, but not put through real app use / mode changes / fullscreen.
- **Wayland fix** (`91d58b12`) — inert on 64-bit; needs a **32-bit ARM / real-addressing rig**
  to exercise (`docs/UPSTREAM-LINEAGE-SYNC.md` §6.1; overlaps Track D).
- **AltiVec under real software** — run LAME/QuickTime/Photoshop under `SS_JIT_VERIFY` to catch
  real `ev_mixed` breakage in the wild (depends on A1's VR/FPR verify). See `docs/TESTING.md`.

---

## A4. 🟡 Diagnostics trustworthiness & build hygiene

Verification-adjacent: noisy/miscalibrated diagnostics undermine the boot-time oracle work in
A1. The heartbeat warning matrix is miscalibrated — `comp frozen` WARN **false-fires on a
healthy idle desktop**, training readers to ignore red — and its thresholds live un-synced in
three places. Plus a set of cheap build/diag-hygiene fixes (gate the interp-site heartbeat in
JIT mode, complete macOS `make clean`, fix the mislabeled `hb-test.cpp`, Linux RSS latch, window
the OTH rule, prune `/tmp` logs, quiet-mode env gate).

**From colleague review (2026-06-04):**
- ✅ **`jitcachesize` unit bug** — pref value (bytes after K/M/G parse) was passed as KB to the
  JIT init, causing `256M` → 256 TB. Fixed with byte→KB conversion + bounds (min 1 MB, max 1 GB).
- ✅ **VERIFY suppression latch decay** (2026-06-05) — the latch was in fact *unconditionally*
  permanent (the `!verify_suppressed` entry gate guarded its own clearing branch), silencing the
  oracle after the first divergence of every run. Replaced with an in-range-only
  `verify_suppress_blocks` countdown. Tracked in OPTIMIZATION-PLAN 0b-extra5; CHANGELOG 2026-06-05.
- ✅ **Bench diagnostics unmasked** (2026-06-05) — `make bench` now shows JIT chatter by default
  (masking is opt-in via `BENCH_QUIET=1`), and the bench's own warnings/errors (incl. the
  stale-baseline warning) moved to **stdout** so they survive any masking. CHANGELOG 2026-06-05.

**Detail:** `docs/planning/sheepshaver-research/research/IMPLEMENTATION-BACKLOG.md` Tier D
(+ correctness items A6/A7 there: duplicate SMC-invalidation call, XO63 FP-control semantics).

---

## A5. 🟡 End-to-end VNC test harness — system-level boot/run/shutdown regression

**Status (2026-06-04): P1 LANDED + verified live; both boot-detection AND shutdown are proper
host→guest hooks now.** `make e2e` boots an isolated **read-only ISO** of Mac OS 8.6/9, waits for a
deterministic boot-ready signal, requests a clean shutdown via a signal, and asserts clean exit —
`PASS: clean lifecycle: booted to Finder, clean shutdown, exit 0` (repeatable on both 8.6 and 9.0.4).
- **Boot detection** — a proper OS-call hook: `[BOOT] idle` from the `SynchIdleTime`/`OP_IDLE_TIME(_2)`
  patch, enriched with frontmost-app + modal state to reject dialog false-positives.
- **Shutdown** — a proper host→guest hook (✅ solved, spec §14): `SIGUSR1` → the emulator injects
  ADB Power key (with dwell) → waits for the Shut Down dialog → Return, so the guest runs its real
  shutdown from its own event loop. No VNC menu-clicking. The earlier trap/power-key dead-ends (§12)
  were corrected by the dwell + dialog-confirm insight.
- **Medium** — read-only ISO is the default (§13): can't get dirty → no repair-prompt/dialog
  false-positives, no pristine-copy-per-run, reproducible. Disk-boot retained for writable scenarios.
Harness is Python + `vncdotool` (VNC only for optional screenshots now), checked-in config, **79
offline unit tests** (the drive/gate/retry logic is now covered via a `FakeRunner`). Run it
locally: `SheepShaver/e2e/README.md`.

**Status update (2026-06-05): matured well past P1.** Benchmark + perf-trend history export,
trustworthy gates (honest PASS + settle-based gate), and first-run onboarding (`make e2e-setup`
doctor) all landed (itemized below). **The core system-level aim — "does it boot, run a real app,
and shut down cleanly?" — is done and solid.** The genuinely-open work is **P2 visual regression
(recommended next)**, CI, and the P3 DSL.

**Open:**
- **▶ P2 (recommended next) — golden-image visual regression.** Perceptual-hash the booted desktop
  (+ the benchmark result) against a known-good reference, masked for dynamic regions
  (clock/cursor). This is the **one failure class the lifecycle/number gate is blind to** — a boot
  that renders garbage / a video regression still PASSes today (the SDL3 port had exactly these
  bugs). We already capture the screenshots; this just adds the gate. Pair with scripted app-launch.
- ✅ **P2 (benchmark automation) — DONE (2026-06-05).** `make e2e-bench` boots the small Mac OS 9 +
  Speedometer disk (`e2e-macos9-mini-boot.dsk`, ~142 MB sparse, copy-per-run = instant clonefile), drives the
  full Speedometer 4.02 suite over VNC (splash→registration→Cmd+A→choose-disk, gated on a new `[APP]
  frontApp` signal so it doesn't race the variable launch), captures `benchmark-result.png` (PR/CPU),
  and shuts down via the hook. Verified PASS (PR 29.375, CPU 66.976). See spec §15.
- ✅ **Benchmark-finished hook + `[APP]` debounce — DONE (2026-06-05, spec §17).** The idle hook's
  `[APP]` signal now also fires on a front-window **modal change**, so Speedometer's "tests are done!"
  dialog is a deterministic finish signal — `run_benchmark` waits for it (no fixed sleep) and reports
  the measured suite duration (non-OCR perf proxy). App-change emits debounced ~0.5 s.
- ⚠️ **Boot-breaking regression fixed (2026-06-05, commit `5d87d713`).** The live-JIT-stats
  window-title feature called `SDL_SetWindowTitle` from the Redraw Thread (Cocoa main-thread-only) →
  abort/VBL-stall/boot-hang. Removed from both backends; see LEARNINGS.
- ✅ **Score parsing + perf-trend history — DONE (2026-06-05).** The benchmark drives Cmd-T "Save
  Text Report", then extracts that file off the run-copy HFS disk **host-side via hfsutils** (no
  extra boot), parses CPU/Graphics/Disk/Math/FPU (MacRoman-decoded), and archives each run under
  `artifacts/benchmark-history/` + an append-only `history.csv`, printing the delta vs the prior
  run. `SS_E2E_RUNS=N` runs N times and reports the **median ± per-metric CV%** (a less-noisy
  trend, flagging metrics whose CV% > 5). `PR` deliberately dropped — it's a disk-weighted
  composite that inherits Disk's run-to-run noise. See `sse2e/bench_export.py` + the
  benchmark-export spec (`docs/superpowers/specs/2026-06-05-benchmark-result-export-design.md`).
- ✅ **Trustworthy gates + honest PASS — DONE (2026-06-05).** A green `e2e-bench` now requires the
  guest's real clean-shutdown signatures (not just exit 0); the back-to-Finder gate keys off a
  **settle window** (Speedometer absent across N consecutive frames), not the noisy one-off
  `frontApp='Finder'` frame the idle hook spuriously emits; and the drive/gate/retry logic is
  unit-tested via a `FakeRunner` (no boot). A reactor-leak hang on the benchmark failure path was
  also fixed (`api.shutdown()` in the `finally`).
- ✅ **First-run onboarding (DX) — DONE (2026-06-05).** `make e2e-setup` doctor gained a
  GUI-session (Aqua) check + a "tree not configured" detection; a tracked README **"Building the
  emulator"** section (the configure incantation no longer lives only in gitignored CLAUDE.md); an
  env-var reference table; and `sse2e/harness.py` scaffolding (`check_preconditions` + reactor-safe
  `hard_exit`) so a new scenario entry point is a thin, correct-by-construction wrapper.
- ✅ **VNC-click "SDL3 regression" — RESOLVED as a misdiagnosis (2026-06-05).** Instant
  `mousePress` works on **both** SDL2 and SDL3 (verified by menu-driving "About This Computer"); the
  earlier "clicks don't register" was off-target clicks + a noisy frontApp signal, not a backend
  bug. No SDL3 click regression exists. See LEARNINGS (2026-06-05).
- ✅ **SDL3 is now the validated default backend (2026-06-05).** Re-bootstrapped the stale
  `configure` (`NO_CONFIGURE=1 ./autogen.sh`) so the build actually links SDL3 (it had silently
  built SDL2). Fixed the one SDL3-only shutdown crash this surfaced — a double-`SDL_DestroyMutex` in
  `VideoExit()` (commit 3daa9c98, spec §18). `make e2e` smoke PASS ×2 on SDL3 (boot → Finder → clean
  shutdown, exit 0); `make test-jit` 264/264. Opt back to SDL2 with `--with-sdl2` if needed.
- **P3** — declarative scenario DSL ("Playwright-for-VNC").
- *(deferred)* **Bootable benchmark ISO** — a read-only bootable HFS CD version of the above
  (harder: needs blessing + HFS mastering on modern macOS). The writable small disk covers the need
  for now (spec §13).
- **GitHub/CI integration (future — complications known).** The harness is already a clean CLI
  gate (`make e2e`, exit 0/1; env-resolved `SS_E2E_ROM`/`SS_E2E_DISK`; artifacts dir for upload).
  Blockers before it runs in CI: (1) **no headless macOS** — SDL needs a live WindowServer, so it
  requires a **self-hosted Mac with auto-login**, not a github-hosted/headless runner (no Xvfb
  equiv); (2) **asset provisioning** — ROM + a multi-GB disk aren't in git, CI must fetch them;
  (3) a **small dedicated test disk** (~200–400 MB vs the 4 GB master) is the biggest readiness
  win (faster fetch + per-run copy); (4) ~2 min/run. The offline pytest suite (`pytest -q`, no
  boot) *can* run on any runner today. A starter self-hosted workflow is sketched in the session
  notes; not committed pending the small-disk + asset-fetch pieces.

**Guest UI introspection — Plan 1 + Plan 2 shipped (2026-06-06).** A read-only, env-gated
(`SS_UI_DUMP_DIR`) host-side dump of the guest UI with global (VNC-clickable) coordinates,
read straight from Toolbox structures at the idle hook — no screenshot/OCR.
- ✅ **Plan 1** — window list (bounds, title, dialog class, modality, active/visible).
- ✅ **Plan 2** — dialog items/DITL (click named buttons), control state (value/hilite/checked/dimmed),
  menu bar + Command-keys, `screen.depth`, `role:"desktop"`, **and control-list items for non-dialog
  (document/movable-modal) windows** (`ac4363a2`) — so `find_item`/`click_item` work on any window.
- ✅ **S4 (first workload shipped)** — `scenario.run_workload` + `sse2e/workload.py` + `run_workload.py`
  (`make e2e-workload`): generic boot+attach → type-select launch → **screenshot/pHash-gated** launch,
  render-timing, quit → clean shutdown → per-workload `history.csv`. **Fractal Carbon is entry #1**,
  boot-validated (launched 1.2 s, render stable 18.3 s, clean shutdown; `b4a0a594`). CarbonLib 1.6 was
  resolved via the introspection-driven SMI installer + extracted/reused (`CarbonLib_1.6_extension.bin`)
  and baked host-side into the workload boot disk. *Finding baked into the design:* a Carbon app's
  fullscreen canvas isn't a standard `WindowRecord`, so the workload gates on screenshot/pHash, not the
  window list.
- 🟡 **S5 (next)** — grow the workload library (POV-Ray, MacBench; Word once Office is installed) on the
  same framework; perf-trend each; optional golden-image regression (`WorkloadSpec.golden_image`).
- 🟡 **S5b — Fractal Carbon AltiVec perf + detection e2e test.** Promote FC from "render-time only" to an
  AltiVec-aware workload: (1) **perf** — record render time as the standing benchmark (already wired);
  (2) **AltiVec detection** — once the Carbon-menu driver lands (see the 🔧 item below), read FC's File-menu
  "Turn AltiVec Code On/Off — (detected|not detected)" string to assert whether the guest detects AltiVec,
  and drive the toggle; (3) **execution proof** — verify with the `[JIT-COMPILED-MIX] AltiVec=N` profiler
  metric (`SS_JIT_PROFILE`) whether vector ops actually run. **✅ RESOLVED 2026-06-07 — real-app AltiVec
  ACHIEVED:** with the corrected `SS_FORCE_ALTIVEC` (gestalt vector bit `0x10`, not the earlier wrong
  `0x40`), FC detects AltiVec via `Gestalt('ppcf')`, takes its vector path, and our JIT runs it —
  `[JIT-COMPILED-MIX] AltiVec=160`, multiple AltiVec hot blocks (~74M exec each). (The earlier "FC uses a
  non-gestalt probe / 0 AltiVec" claim was a confounded experiment — wrong gestalt bit; see LEARNINGS.)
  So FC IS a positive AltiVec perf A/B vehicle now (AltiVec-on vs forced-off). Remaining for the e2e test:
  the Carbon-menu driver to read/assert FC's "(detected)" string + drive the toggle headlessly, and a
  decision on whether to ship the gestalt-register as a real (non-throwaway) opt-in pref. Depends on: Carbon-menu driver.
- 🔧 **TO IMPROVE — Carbon-app menu introspection.** `SS_UI_DUMP_DIR` returns an **empty menu bar for
  Carbon apps** (confirmed 2026-06-07 driving Fractal Carbon): Backend A reads the classic Toolbox
  global menu list (`MenuList`/`GetMenuBar`), but Carbon apps own their menus via the Carbon Event/HIToolbox
  manager, not those classic structures — so we can't enumerate or `click_item` a Carbon app's menu actions
  host-side. This blocked driving FC to an explicit AltiVec/Calculate mode for the AltiVec-execution test
  (task #26) — we could only watch its passive default. Options to investigate: (a) Backend B
  Toolbox-trap oracle could hook `CountMenuItems`/`GetMenuItemText` per registered MenuRef rather than the
  global list; (b) read the Carbon menu bar via the HIToolbox menu-tracking globals (needs RE); (c) accept
  the limit and drive Carbon apps by known cmd-keys / screenshot-pHash regions instead of named menu items.
  Pairs with the existing Carbon-canvas limitation noted below (fullscreen canvas isn't a `WindowRecord`).
- ⏸ **Plan 3** — Backend B (Toolbox-trap oracle), `compare()`/`overlay()` calibration, ParamText, socket transport.
See `SheepShaver/docs/UI-INTROSPECTION.md` (canonical reference),
`docs/planning/UI-INTROSPECTION-REVIEW-SYNTHESIS.md` (action plan),
`docs/HOST-SIDE-MAC-SOFTWARE-INSTALL.md` (getting workload apps + system libs onto disks host-side),
`SheepShaver/e2e/README.md` (the harness toolkit map), `SheepShaver/e2e/AGENT-API.md` (drive the guest from
code — the agent surface), and `docs/planning/E2E-TOOLKIT-REVIEW-AND-MCP-PROPOSAL.md` (toolkit review + the
parked MCP-server proposal).

**🔜 New (2026-06-05) — connect the harness to per-instruction JIT correctness.** The lifecycle
+ benchmark harness proves the emulator runs *as a system*; it does not prove the JIT is
*arithmetically* correct. Two items close that gap (and stop the shiny system harness from
creating a false sense of coverage):

- **A5-V. `SS_JIT_VERIFY`-under-E2E mode.** ⚠️ **Deprioritized (2026-06-05) — boot-time verify is
  a trap.** The original idea: boot the golden workload with `SS_JIT_VERIFY=1`, parse the divergence
  log, and **fail on any divergence not on a known-false-positive allowlist** (today: exactly the
  `blr`-boundary block). The catch we now understand: verifying *every block twice through the
  interpreter* starves the guest 60 Hz timer, so the early-boot ROM spin-wait never clears and the
  run rarely reaches a workload — only a vanishingly small fraction of blocks ever get verified
  before it stalls. The cheaper, deeper substitute is **offline differential coverage** —
  `make test-jit` (interp-vs-JIT REGDUMP diff) and `rom-harness --passes/--seed/--count` (no timer
  dependency, deterministic, runs in seconds). So the e2e harness's lane is **system-level**, not
  per-instruction; this item stays a backlog idea, not a near-term build. **Complements:** A1 (the
  per-op suite is the real backstop — see A5-C).

- **A5-C. Don't let the system harness mask the per-instruction coverage holes.** `make e2e`
  passing is necessary, not sufficient — the open per-op gaps stay the real safety net and must
  still be closed in A1/A2, NOT considered covered because the boot is green:
  - **A2 leftovers:** ✅ halfword multiplies `vmul{o,e}{u,s}h` + word pack `vpkuwum` fixed+vectored
    (2026-06-07). 🟡 signed byte multiplies `vmulosb`/`vmulesb` still prospective (no signed vector).
    🔜 pixel + sum-across families (new derivations).
  - **A1 leftover:** the scored *arithmetic* word-op vectors (`vadduwm`/`vsubuwm`/`vmaxsw`/…)
    still use byteswap-palindrome operands (`0x05..`/`0x03..`) that can't catch a byteswap/lane
    bug — regenerate with **distinct AND carry-inducing** operands.
  These are harness-verifiable here (no boot). A5-V is the backstop that catches what the per-op
  suite *doesn't* cover; it is not a substitute for growing that suite.

**Detail:** spec §12 in `docs/superpowers/specs/2026-06-04-e2e-vnc-harness-design.md`, plan in
`docs/superpowers/plans/`, run guide in `SheepShaver/e2e/README.md`, code in `SheepShaver/e2e/`.

**Why:** A1/A2 verify the JIT *per instruction*; nothing automatically checks the emulator *as a
running system* — "does it still boot to the Finder and shut down cleanly after a codegen
change?" That's today a manual human-at-VNC step. This is the highest-leverage item for **agent
autonomy in testing**: it turns the one check agents currently *can't* self-serve (boot/run) into
a scriptable pass/fail, so an agent can validate its own JIT change end-to-end instead of handing
every boot back to the user.

**Approach:** spawn SheepShaver with an *isolated* config (own prefs + pristine-disk-per-run),
drive it over the **built-in bidirectional VNC server** (`vnc_server.cpp` already wires
kbd/ptr→ADB), and assert with **hybrid observability** — host-log signals as the deterministic
spine (booted / exit 0 / atexit report) + masked perceptual-hash screenshots as a tolerant visual
gate (exact frame-hash rejected as flaky). Canonical v1: `boot → Special ▸ Shut Down → assert
clean exit` (also regression-tests the 2026-06-03 clean-shutdown feature).

**Effort:** P1 (lifecycle smoke) ~1–2 days; P2 (golden-diff + app launch) +1–2 days; P3 (scenario
framework) larger, only if earned. Needs a small pristine test disk image (asset) + `vncdotool`/
`imagehash` deps. **Policy:** relaxes the no-boot rule to "ask the user first" (done in
CONTRIBUTING + CLAUDE.md). **Complements** A1's boot-rig need (Paranoia FP conformance runner).
**Detail:** `docs/superpowers/specs/2026-06-04-e2e-vnc-harness-design.md`.

---

# Track B — Performance / JIT optimization

Full plan, with per-lever effort/payoff and measured baselines, lives in
**`docs/planning/OPTIMIZATION-PLAN.md`** (and the Dolphin/RPCS3/Box64/FEX survey items therein +
`docs/planning/sheepshaver-research/research/IMPLEMENTATION-BACKLOG.md`). Selected ideas from the
**BasiliskII (68K) JIT** — the more mature, same-host lineage — are triaged for SheepShaver in
`docs/planning/sheepshaver-research/BASILISKII-CROSS-POLLINATION.md` (what to borrow clean, and
what heavy machinery to leave behind); the borrows feed A1/B2/B3 below. The cheap, ready wins from the first
pass have largely **landed** (subfe/adde via ADCS, mullwo, CR0 cleanup/B1, LogicalImm/B2, code-cache
sizing, atomic spcflags). What remains is the bigger, measurement-gated work. Tracked here as
buckets so they don't fall off the map:

## B1. ✅ P0 — Execution-weighted profiler — DONE (2026-06-06/07)

**Done:** `SS_JIT_PROFILE` mix-aware profiler + `SS_JIT_PROFILE_DISASM` + `[JIT-RUN-PROFILE]`
(guest-MIPS) + the deterministic **`a64/op`** microbench metric. Boot-validated; it directly drove
P3a and P5b below. Remaining offshoots: routine-name attribution; the SiliconSheep "Developer
Inspector" data layer (Track C). **Detail:** `OPTIMIZATION-PLAN.md` §P0/§P0b.

## B2. 🟡 Medium levers — fallback & branch handling

- **`lwarx`/`stwcx.` — ✅ native (P3a, 2026-06-07).** Were interpreter fallbacks; now compiled
  natively (correctness/architecture win — ~5% of compute, not idle-driven). `mftb`/`isync`
  fallbacks remain.
- **Native `bcctr`** (98.5% of JIT misses) — complex; needs Mixed-Mode-Manager RE.
  *Light alternative:* the §R2 guarded inline direct-mapped cache (BasiliskII X3).
- **`isync` inline BLR** (0c), **lazy CR0 re-enable** (0g — `rc1`=12 a64/op, the broadest remaining
  per-op lever; disabled after a boot regression). ⚠️ **New prereq (2026-06-07):** re-enabling lazy
  CR0 would break divw/lwzx/mulhw/lwarx together (they hold RTMP/NZCV across `ra_store`); the divw
  `ra_store` hoist was step 1. **Detail:** `OPTIMIZATION-PLAN.md` §0g/§P2/§P3/§R2.

## B3. 🟡 High-effort levers

- **FP register allocator — ✅ DONE (P5b, 2026-06-07).** Speedometer Math +16% (~1.89× interp),
  Fractal Carbon +8% MIPS. ✅ 2026-06-07: update/indexed FP memory + fsel/frsp/frsqrte/fsqrt all
  zero-copy (no `emit_*_fpr` bridge in any hot FP op). Remaining FP follow-up: cross-block FP pinning.
- **Per-block prologue/epilogue overhead — 🔴 the new top lever** (the ~12–14 `a64/guest-op` ceiling
  the profiler surfaced): caller-save only the clobbered regs / block-merging / **bclr chaining (P9)**.
- Remaining: constant folding (P5), instruction scheduling (P6), byte-swap opt (P7), **cross-block
  register pinning** (r1/SP, r2/RTOC — P8). **Detail:** `OPTIMIZATION-PLAN.md` §P5–P9.

## B5. ✅ AltiVec detection SOLVED (2026-06-07) — gestalt `'ppcf'` register-it-ourselves; real-app AltiVec achieved

> **RESOLVED.** The gate was the gestalt `'ppcf'` vector bit, AND it's registerable from host code under
> the existing OldWorld 1.1 ROM — **no NewWorld ROM port needed.** `SS_FORCE_ALTIVEC=1` registers `'ppcf'`
> via `_NewGestalt $A3AD` with a 68k SelectorFunction returning the vector bit (`1<<4 = 0x10`). With it,
> **Fractal Carbon detects AltiVec, takes its vector path, and our AArch64 JIT compiles+runs the PPC
> AltiVec instructions** — `[JIT-COMPILED-MIX] AltiVec=160`, multiple AltiVec hot blocks (~74M exec each).
> So the JIT's AltiVec codegen is now validated **end-to-end by a real app**, not just the harness.
> **False-start caveat (kept as a lesson):** the first attempt wrote bit `0x40` (= `gestaltPowerPCHas64Bit`
> Support`) instead of `0x10`, producing a self-consistent WRONG conclusion ("FC ignores gestalt; 0
> AltiVec") that survived until a research subagent checked Apple's `Gestalt.h` (the constant is bit
> *number* 4). Verify magic numbers against the primary source; `1<<N` ≠ N. Full story: LEARNINGS 2026-06-07.
> **Open follow-ups:** (a) decide whether to ship `SS_FORCE_ALTIVEC` as a real opt-in pref (vs throwaway) —
> note 8.6/9.0 here don't do VR context save/restore, so weigh multitasking-safety; (b) wire FC's
> "(detected)" assertion + toggle into the S5b e2e test once the Carbon-menu driver lands.

**Historical (the investigation that led here — falsified theories retained for the record):**
The AltiVec JIT codegen (extensively hardened — 54 differential vectors, 27 bugs fixed) was **only
exercised by the test harness** until the gestalt unlock above; the work below traces how the gate was found.

- ❌ **`mfmsr[VEC]` READ is NOT the gate — falsified.** Advertising MSR[VEC]=1 (`mfmsr`→`0x0200f072`)
  and booting Fractal Carbon left the profile at **0 AltiVec blocks**. (Kept the JIT `mfmsr`→`0xf072`
  divergence fix, `cd6df179`.)
- ❌ **`mtmsr[VEC]` WRITE is NOT the gate either — falsified 2026-06-07 (task #21, probe commit
  `112481f2`).** Instrumented `execute_illegal` (`SS_LOG_ILLEGAL=1`, decodes `mtmsr` op31/XO146 +
  tests bit `0x02000000`); confirmed `mtmsr` reaches the handler (not NOP-stubbed: JIT falls through
  to inline-interp), then booted **Mac OS 9 + Fractal Carbon** via the E2E workload: **zero `mtmsr`,
  zero illegal opcodes** all run. `mtmsr`-enable is *downstream* of detection — nothing tries to
  enable the vector unit because nothing detects it.
- 🎯 **The gate is the gestalt `'ppcf'` (`0x70706366`) vector bit — and detection is NOT pure-PVR
  (established 2026-06-07, #23).** *DECODED-image measurement:* `'ppcf'` = **0× in the current ROM
  (`1998 Mac OS ROM 1.1`, CHRP-LZSS), 2× in `2001 Mac OS ROM 9.0.1` (CHRP-parcels)** — the 1998 ROM has
  no PowerPC-processor-features gestalt at all. *Terminology fix:* both ROMs are `ROMTYPE_NEWWORLD`
  ("OldWorld" was a loose label); the real axis is **ROM vintage/format**, not OldWorld-vs-NewWorld.
  *Key deduction:* the **PVR is already `0x000c0000` (7400 = G4 with AltiVec)** and the gestalt CPU-type
  is patched to match, **yet the vector bit stays clear** → the handler reads **more than PVR** /
  needs a `'ppcf'` gestalt the 1998 ROM lacks; there's a **co-requirement**. **This flips the earlier
  lean: a newer (G4-era) ROM is now a *plausible* AltiVec unlock, not a non-factor.**
- 🔬 **Decisive next test (boot-level): boot a newer ROM that carries `'ppcf'`** and check whether
  AltiVec lights up (profiler MIX_ALTIVEC > 0). **BUT this is blocked on ROADMAP D3** — the staged
  `2001 Mac OS ROM 9.0.1` (parcels) does **not** boot: `PatchROM()` fails at the first `find_rom_data`
  (`patch_nanokernel_boot` `sr_init_dat`; **D3 Phase 0 done 2026-06-07** via `SS_ROM_PATCH_TRACE`, points
  to D3 Phase 1 = incomplete parcels decode). See `NEW-WORLD-ROM-SUPPORT-PLAN.md`. Cheaper alternative
  that needs no NewWorld boot: a PVR-read (`mfspr` 287) hook (à la `SS_LOG_ILLEGAL`) under the *current*
  ROM to confirm the handler isn't even consulting PVR.
- ⚠️ **Full enable is foundational, not a hack.** Even if detection flips, a usable AltiVec needs VR
  save/restore on context switch (vector state would otherwise corrupt across task switches). That
  VR-context modeling is the real Phase-3 cost. [DingusPPC](https://github.com/dingusdev/dingusppc) is
  the reference for how a fuller PPC model handles MSR[VEC]/VRSAVE/context.
- ✅ **Codegen-first ordering (already mostly done):** the open AltiVec families (pack/pixel done;
  sum-across + `fctiw`/`fctid` rounding remain) are closed via the `SS_TEST_HEX` differential harness
  — needs **no** detection — so flipping `'ppcf'` later is a pure win, not silent corruption (task #22).

Real workloads waiting on this: Fractal Carbon, Power Fractal, Photoshop AltiVecCore, SoundJam.
**Detail:** LEARNINGS 2026-06-07 ("AltiVec detection is NOT an `mtmsr`/MSR[VEC] path"); the in-code
DORMANT banner atop the KNOWN-AltiVec note in `ppc-jit.cpp`; `docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md`.

## B4. 🟡 Strategic / from-research levers

Runtime link stack (R1b), **dual W^X mapping** (R8 — ~27% compile speedup measured), async
background JIT compilation (R9, Cemu model), Metal framebuffer compositing (R10), JIT-residency
gate restructure (C1), selective HLE of hot routines.
**Detail:** `docs/planning/OPTIMIZATION-PLAN.md` (research section + HLE) + `IMPLEMENTATION-BACKLOG.md` Tier C.

**Multi-core offload ("multithreading light").** R8/R9/R10 above are the safe, high-ROI slice of a
broader parallelism story: what work can move off the hot emulation core (background compile, async
devices, Metal compositing) vs. what can't (the cooperative guest is one logical CPU). Tiered by
feasibility, with the gated "true guest SMP" end (MP tasks on separate cores) tied back to D3's
supervisor-fidelity work. **Detail:** `docs/planning/MULTICORE-OFFLOAD-PLAN.md`.

**Cross-emulator idea bank (Dolphin/RPCS3/Cemu/QEMU/Rosetta).** A reality-checked lateral-ideation
pass with an effort/payoff/blocked grid. The two near-term nominees are promoted to tracked items
**B5 (`CopyBits` HLE)** and **B6 (idle-skipping)** below; the bank also holds do-anyway dual-W^X (R8),
constant-prop, carry-via-NZCV, and fastmem-SIGBUS, and records brainstormed "wins" that were already
shipped (chaining, AltiVec byte-mults) so they aren't re-chased.
**Detail:** `docs/planning/sheepshaver-research/CROSS-EMULATOR-IDEATION.md`.

## B5. 🔜 `CopyBits` HLE — native/Metal blitter *(nominated from the idea bank)*

Replace the hottest QuickDraw raster op with a native ARM64/NEON blit via the existing NativeOp
mechanism (the path the ethernet driver + video accel already use), instead of running it through
the PPC-JIT-emulating-the-ROM's-68K-DR-emulator (the worst layer in the stack). **Effort:** Med ·
**Payoff:** High (games/media — the workloads that justify throughput) · **Blocked by:** a half-day
call-frequency + rect-size histogram (go/no-go). Safe (no SMC risk, unlike `BlockMove`-for-code);
the natural on-ramp to Metal-accelerated blits (R10).
**Detail:** `docs/planning/sheepshaver-research/CROSS-EMULATOR-IDEATION.md` (keeper #1) +
OPTIMIZATION-PLAN "Selective HLE".

## B6. 🔜 Idle-skipping / busy-wait detection *(nominated from the idea bank)*

Detect the cooperative guest's hot idle spins (the `0x5031040c` VBL spin, `WaitNextEvent` null-event
poll) and `WFE`/nanosleep to the next 60 Hz tick instead of spinning at full ARM64 speed (reuses the
existing spcflags poll points). **Effort:** Med · **Payoff:** High (battery/thermal — a backgrounded
VM shouldn't peg a P-core) · **Blocked by:** none. Primarily serves **Track C** (a "behaves like a
real macOS app" win) but is JIT-implemented, so it lives here.
**Detail:** `docs/planning/sheepshaver-research/CROSS-EMULATOR-IDEATION.md` (keeper #2).

**Regression tracking:** baselines + how to A/B → `docs/BENCHMARKS.md` (Speedometer baseline
recorded 2026-06-04, 1.88× over interp; MacBench 5.0 + app-launch timings still TODO) and
`SheepShaver/rom-harness/` (`make bench`).

---

# Track C — Desktop integration ("SiliconSheep") — the Parallels-like experience

🟡 **Active — Tier 1 complete, C2.0 RPC landed, Inspector shipped (2026-06-07).** Full Parallels-
like VM manager: master-detail layout, immediate-apply settings, multi-VM, Platinum styling,
pixel art icons, Inspector window (4 panels), bug report bundle, 19 tests. C2.0 bidirectional
UDS RPC gives sub-16ms launcher↔emulator IPC. Framework pivoted from Cocoa/ObjC to Tauri for cross-platform door
and CLI-only build (no Xcode.app). Competitive teardown, host-guest channel audit, UX flows,
and Tauri architecture all researched; findings synthesized into the plan.

**Tiers (full detail in `docs/planning/DESKTOP_INTEGRATION_PLAN.md`):**
- **C1. Tier 1 — First-run wizard + VM library** (launcher-only, no emulator changes): guided
  4-screen wizard (ROM drop → disk → review → boot), VM card grid with APFS `clonefile`
  duplicate, settings sidebar with hot-reload indicators, fullscreen escape overlay, dark mode,
  Gatekeeper `xattr -cr` button, disk backup, drag-and-drop file import, coach marks.
- **C2. Tier 2 — Enhanced integration** (emulator IPC needed): true hot-reload via UDS RPC
  (extend existing `rpc_unix.cpp`), multi-folder `extfs`, Unicode clipboard (`utxt`/`UT16`),
  direct framebuffer screenshots, disk-image snapshots, network assistant.
- **C3. Tier 3 — Coherence Lite** (novel research): guest WindowList polling (0x9D6, proven in
  e2e code), frontmost app tracking (CurApName 0x910), host title-bar overlays, per-window
  Dock entries. True seamless windows infeasible near-term (no guest agent for Mac OS 9).
- **C4. 🔜 Tier 4 — Automation & Scripting** (added 2026-06-05): drive/observe the guest *without
  screenshots*. **Layer A** launcher control surface (`siliconsheep` CLI / AppleScript / Shortcuts
  / MCP server / headless CI mode — UTM/Lume/Tart model); **Layer B** guest control bridge
  (structured input + RAM observation + AppleEvents via `Execute68kTrap`); **Layer C** optional
  "SiliconSheep Tools" guest agent for *managed* images (reframes part of the Infeasible list).
  Has a full per-layer integration design (file:line anchors, phased build orders, effort/risk).
  **Key finding:** the bidirectional launcher↔emulator RPC does *not* exist yet, so **Layer A's A0
  (make the RPC bidirectional) is the prerequisite** for the rest — and the lowest-risk first slice
  (A0 → `STATUS` → CLI → MCP) also unblocks **A5** (E2E in CI) and **A5-V** (SS_JIT_VERIFY-under-E2E).
  Feeds Track A's verification work; shares one idle-hook command-mailbox spine.

- **C5. 🟡 Multi-VM support.** SiliconSheep currently enforces one running VM at a time
  (`AppState.running: Option<RunningVm>`). Each VM already launches as a separate OS process
  with its own `.sheepvm` prefs and random VNC port, so concurrent execution is architecturally
  supported — the restriction is in the launcher, not the emulator. Change `Option` to a
  `HashMap<String, RunningVm>`, update Start/Stop/screenshot to target by ID, and guard against
  two VMs sharing a disk image (data corruption). Low effort (~30 min), high user-perception
  impact. Gated only on the P0 window-management bugs (unsaved-changes confirmation, orphaned
  settings window on delete).

- **C6. ✅ Bidirectional UDS RPC (C2.0).** Flip the emulator from RPC client to server on
  the existing `rpc_unix.cpp` framework. Non-blocking poll in the 60 Hz video refresh gives
  sub-16ms command latency. New method IDs: SET_PREF, INPUT_LOCKOUT, FRAMESKIP, MOUSE_GRAB,
  GET_STATE, READ_MEMORY, DUMP_REGISTERS, INSERT_DISK. SiliconSheep connects as a UDS client.
  Replaces the env-var, prefs-file, and runtime_control file-polling mechanisms with one clean
  channel. ~150 LOC emulator + ~100 LOC Tauri. **Unblocks all Tier 2 items, Inspector Tier 2
  panels, hot-reload, hot disk insertion, Tier 4 automation.** Same pattern as QEMU QMP and
  VirtualBox COM/XPCOM. **Detail:** `DESKTOP_INTEGRATION_PLAN.md` §C2.0.

**Framework:** Tauri v2 (Rust + pnpm + TypeScript). **Repo strategy:** hard-fork decision
deferred but recognized as increasingly inevitable with Track C divergence.
**Detail:** `docs/planning/DESKTOP_INTEGRATION_PLAN.md`, `docs/planning/HOST-GUEST-CHANNELS.md`.

---

# Track D — Platform breadth (optional)

## D1. ⏸ BasiliskII on macOS — independent of SheepShaver

**What B2 is for:** emulating **68K Macs / classic Mac OS 7.x–8.x** — a different machine class
from SheepShaver (PowerPC, 8.x–9.x). **Not needed for SheepShaver** (separate build trees; SS
compiles none of B2's 68K JIT). B2 has been broken on macOS for a while with zero effect on SS.

**Status:** does not build on macOS. Configure host-routing + `main_unix.cpp` Linux-isms fixed;
the 68K JIT backend (`compemu_support_arm.cpp`) is unported (Linux `uc_mcontext`, undeclared
`uae_vm_jit_write_protect` W^X, `uae_vm_page_size`, `_XOPEN_SOURCE`).

- **D1a. Interpreter-only build** — cheap/mechanical; restores B2 usability on macOS *now*. 68K
  on Apple Silicon doesn't need a JIT for speed. Likely just the mechanical guards + `--disable-jit`.
- **D1b. JIT-backend port** — optional perf; mirror SheepShaver's *proven* macOS W^X
  (`jit-target-cache.hpp`). Higher effort + heavy boot verification.

**Detail:** `docs/planning/BasiliskII-MACOS-AARCH64-JIT-PORT.md`.

## D2. ⏸ Linux / ARM re-convergence (deferred backlog)

Upstream backports left on the shelf until a dedicated Linux pass: sheep_net `module_init`
modernization (`650d3a82`), `linux/sched.h` guard (`6787dce8`), etherhelpertool malloc-leak fix
(`db897d0d`), REAL_ADDRESSING kernel-relative reads (`533cf6fa`), and a **Parallels Ubuntu ARM64
rig** to validate the Linux JIT + VDE (also exercises the Wayland fix from A3).
**Detail:** `docs/UPSTREAM-LINEAGE-SYNC.md` §6 / §6.1.

## D3. ⏸ Break the Mac OS 9.0.4 ceiling — newer guest OS compatibility (exploratory)

SheepShaver tops out at ~9.0.4. Getting to 9.1/9.2.2 is **two walls in sequence**, both
deferred/exploratory:
- **First wall — New World "parcels" ROM support.** The newer ROMs 9.1+/9.2.x need are
  parcels-format, which `PatchROM()` rejects. A cheap, decisive Phase-0 diagnostic is ready to run.
  **Detail:** `docs/planning/NEW-WORLD-ROM-SUPPORT-PLAN.md`.
- **Second wall (maybe) — supervisor-level fidelity.** 9.2.x may also depend on machinery SS
  deliberately stubs: the **MMU** (faked V=P), the **nanokernel** exception/interrupt model
  (bypassed; host-signal timer instead of a real decrementer), and **preemptive MP tasks**
  (absent). **Feasibility settled 2026-06-07 (2 opposed agents):** the MMU *is* implementable
  without gutting the flat model — **shadow-arena / "Dynamic BAT"** (Dolphin-proven; cost on the rare
  map-change, not per-access) — but it's **deferred "never-unless-proven"**: no evidence any wanted 9.x
  software needs non-identity translation, untestable until a New World ROM boots. Hinge = host 16 KB vs
  PPC 4 KB page. The `SS_STUB_TRACE` probe (shipped) measured **zero runtime supervisor pressure on 9.0.4**.
  **Detail:** `docs/planning/MMU-NANOKERNEL-MP-PLAN.md` (canonical, with the headline verdict + dossier);
  evidence memos `MMU-WITHOUT-GUTTING-FLATMEM.md` (design) + `MMU-DEFERRAL-REDTEAM.md` (red-team).

Run the New World Phase 0 before committing — the first wall (ROM) is the gate; the MMU "second wall" has
no runtime footprint on the OS we run today and a ready design if it ever surfaces. Large, exploratory,
deferred vs. the EV compatibility levers.

## D4. 🟡 DingusPPC comparative investigation (exploratory)

Evaluate [dingusdev/dingusppc](https://github.com/dingusdev/dingusppc) as a **reference emulator**
for architecture and validation ideas we can adapt to SheepShaver on Apple Silicon. This is not a
port target and not a code-import plan; it is a structured "borrow-or-reject" pass focused on:
MMU/TLB modeling, exception/timer fidelity, test harness discipline, and where their interpreter-
heavy design should **not** influence our JIT execution path.

Initial goal: produce a ranked "adopt now / defer / avoid" matrix with concrete first experiments
and explicit non-goals to prevent scope creep, including Dingus's CLI-debugger/profiler ergonomics
as potential inputs to our own verification workflow.
**Detail:** `docs/planning/DINGUSPPC-EVALUATION-PLAN.md`.

## D5. 🟡 Snow comparative investigation (exploratory)

Evaluate [twvd/snow](https://github.com/twvd/snow) as a **reference emulator** for two things:
(1) hardware-fidelity strategies relevant to classic-mac emulation (especially if BasiliskII/68K
work is resumed), and (2) debugger UX/tooling patterns we can adapt for SheepShaver verification.

The scope is comparative and strategic (borrow/defer/avoid), not a port plan and not code import.
Primary focus: traceability tooling (instruction history/trap history/peripheral views/watchpoints),
hardware-model discipline, and any low-risk debug instrumentation ideas transferable to this repo.
**Detail:** `docs/planning/SNOW-EVALUATION-PLAN.md`.

## D6. 🟡 Infinite Mac comparative investigation (exploratory)

Evaluate [mihaip/infinite-mac](https://github.com/mihaip/infinite-mac) as a reference for
**host/runtime integration strategy**: browser+WASM emulator orchestration, dynamic file/media
injection, and multi-instance networking setup.

This is explicitly a "borrow-or-reject" pass for architecture ideas, not a web-port plan. It also
gets a nod as a discovery source: its curated emulator stack includes DingusPPC and Snow, which
helps us identify adjacent projects worth evaluating.

Initial focus: dynamic uploads/CD-ROM mounting flows, chunked/remote media delivery patterns, and
network topology constraints (including the project’s AppleTalk zone model and where it does/does
not map to our native networking goals).
**Detail:** `docs/planning/INFINITE-MAC-EVALUATION-PLAN.md`.

---

## ✅ Done (recent — for context, newest first)
- AltiVec `ev_mixed`: **entire tested element-order class fixed** — splats, merges `vmrgh/l
  {b,h,w}`, pack `vpkuhum`, byte multiplies `vmulo/eub` — promoted to the scored gate (**264/100**,
  quarantine empty; all boot-verified — zero VR/FPR divergence). Per-op `REV32.16B`
  normalize: merges=ZIP, pack=UZP2 (`emit_vmrg`); byte mults=UZP1/2+UMULL.8H+REV32.8H
  (`emit_vmul_byte`). Siblings then flagged; ✅ halfword mults + `vpkuwum` fixed 2026-06-07
  (`emit_vmul_hword`/`emit_vpk_w2h`); signed byte mults still prospective.
- Doc hygiene: fork-wide `CHANGELOG.md`, `docs/ARCHITECTURE.md` extracted, handoff docs retired
  (durables → DIAGNOSTICS/LEARNINGS), harness counts de-hardcoded, memories pruned.
- Upstream backports: **VDE networking**, **SDL3 default backend** (boot-verified), **Wayland
  detection** — all from kanjitalk755 (`docs/UPSTREAM-LINEAGE-SYNC.md`).
- JIT correctness: AltiVec `vsel` + splat fixes; subfe/adde carry-out; mullwo.
- JIT perf (cheap wins): CR0 cleanup, LogicalImm encoder, code-cache sizing, atomic spcflags,
  trailing-MOV elimination, register allocator (P1). See `docs/planning/OPTIMIZATION-PLAN.md` §Completed.
- BasiliskII: configure host-routing fix + `main_unix.cpp` guards (partial — see D1).
