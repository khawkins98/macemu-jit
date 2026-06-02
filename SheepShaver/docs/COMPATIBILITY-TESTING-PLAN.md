# SheepShaver Compatibility Testing Plan

How we will measure correctness and compatibility of the AArch64 JIT — against the
interpreter, against the legacy x86 SheepShaver JIT, and against real-world Mac OS software.
Drafted 2026-06-02.

**This plan extends existing infrastructure — it does not replace it:**

| Existing asset | Role in this plan |
|---|---|
| `docs/AARCH64_JIT_GOLDEN_WORKLOADS.md` (7 workloads) | The canonical gate. New tiers below feed into it. |
| `jit-test/run.sh` (209 vectors, interp-vs-JIT diff) | Tier 1 foundation |
| `rom-harness/` (random ROM block exerciser) | Tier 2 foundation |
| `BasiliskII/qa/` (matrix scaffold, Gherkin features, VNC runner) | The automation pattern to mirror for SheepShaver app testing |
| `JIT-STATUS.md` | Where summary results land |
| `JIT-FPU-PLAN.md` | Tier 3 (FP) integrates with this |

---

## The oracle hierarchy

Compatibility is always measured as *agreement with an oracle*. We have four, in increasing
cost and decreasing precision:

1. **Our interpreter** (kpx_cpu) — cheap, always available, but shares bugs with the JIT
   when both misread the spec
2. **The x86 SheepShaver JIT** (dyngen-based) running under Rosetta 2 — independent
   implementation, same guest environment, catches "we misread the spec" bugs
3. **QEMU `target/ppc`** — fully independent, most-reviewed PPC implementation in existence
4. **Real Mac OS behavior** — the only oracle that matters to users; expensive and fuzzy
   (apps crash for many reasons)

A disagreement between our JIT and *two* oracles is a JIT bug with near certainty.
A disagreement with only the interpreter requires a third opinion before "fixing."

---

## Tier 1 — Instruction-level parity (exists, extend)

**Now:** `jit-test/run.sh` — 209 vectors, every vector run in interpreter and JIT mode,
REGDUMPs diffed. Gate: score=100.

**Extensions, in priority order:**

1. **Three-way vectors (adds the x86 JIT oracle).**
   Build x86_64 SheepShaver (the upstream dyngen JIT) and run it under Rosetta 2 on the same
   machine. The `SS_TEST_HEX`/`SS_TEST_DUMP` harness mechanism lives in shared CPU-core code
   (`ppc-cpu.cpp`), so the same vectors drive both builds:
   ```
   vector → interp REGDUMP  ─┐
   vector → arm64-JIT REGDUMP ├─ 3-way diff: any 2-vs-1 split localizes the bug
   vector → x86-JIT REGDUMP  ─┘  (run via: arch -x86_64 ./SheepShaver-x86 …)
   ```
   - Work: an x86_64 configure/build lane + a `run-3way.sh` wrapper that runs both binaries
     and diffs three dumps. The x86 build needs the same `SS_TEST_*` env plumbing (verify it
     exists in upstream; if not, it's a small backport).
   - Where disagreements are most likely (and most valuable): XER CA/OV corner cases, CR
     flag combinations, FPSCR — exactly where we found bugs A1-A3.

2. **Coverage audit.** Map the 209 vectors against the opcode handler list in `ppc-jit.cpp`
   (every `case` in `compile_one`). Every handled opcode needs at least: one basic vector,
   one edge-case vector (carry wrap, sign overflow, zero operand), and — for Rc forms —
   one CR-checking vector. Output: a coverage table in `jit-test/README.md`; close gaps.

3. **Random differential fuzzing (risu-style).** `rom-harness` already does this against ROM
   code; add a mode generating *synthetic* random-but-valid instruction sequences (constrained
   to implemented opcodes, no memory ops or with a scratch page) and diff interp vs JIT for
   N=100k sequences nightly. QEMU's `risu` tool is the established reference for this
   methodology but targets ppc64/Linux — we replicate the idea, not the tool.

## Tier 2 — System-level parity (exists, formalize)

**Now:** Golden Workloads 2/3 (boot to desktop interp/JIT), 4 (ROM harness), 5 (VNC smoke).

**Extensions:**

1. **Mac OS version boot matrix.** Boot each supported OS to desktop under interpreter,
   ARM64 JIT, and x86-JIT-under-Rosetta:
   | Guest OS | Why it matters |
   |---|---|
   | Mac OS 7.5.5 | Smallest, fastest boot; current baseline |
   | Mac OS 8.1 | Last 68K-bootable; heaviest mixed-mode (68K emulator) use |
   | Mac OS 8.6 | Common community choice; nanokernel changes |
   | Mac OS 9.0.4 | Most-used SheepShaver target |
   | Mac OS 9.2.2 | Latest supported; most demanding |
   Record per cell: boots? time-to-desktop? errors in console? Store results as a table in
   `JIT-STATUS.md`. Mirror `BasiliskII/qa/matrix.md`'s format and runner-script pattern.

2. **Boot-time metric as a compatibility canary.** Time-to-desktop regression >20% = treat
   as failure even if it boots (the >180s JIT boot finding shows timing IS a compat signal —
   timeouts in guest drivers can turn slowness into hangs).

## Tier 3 — FP/FPSCR correctness (new, integrates with JIT-FPU-PLAN.md)

1. **TestFloat-derived vectors.** Berkeley TestFloat is the established IEEE-754 compliance
   suite. We don't run it in the guest (no PPC build needed); instead, use `testfloat_gen` on
   the host to generate input/expected-output pairs for f32/f64 add/sub/mul/div/sqrt/convert,
   then wrap them as `SS_TEST_HEX` vectors (load operands via `SS_TEST_INIT`-style FPR
   seeding — needs harness FPR support, currently GPR-only).
   - Priority: rounding modes × {add, mul, div}, NaN propagation, denormal handling,
     FPSCR exception bits (FX, OX, UX, ZX, XX).
2. **Graphing Calculator / Infini-D as guest-level FP smoke** — period apps notoriously
   sensitive to FP bugs.

## Tier 4 — Application compatibility matrix (formalize Workload 7)

The community-established measure (E-Maculation forum lists) made systematic:

1. **Test set, three rings:**
   - **Ring 0 (every JIT change):** Finder operations, SimpleText, Calculator, Control Panels
   - **Ring 1 (weekly / before merge):** Prince of Persia (existing Workload 7), Photoshop 5,
     Word 98, Internet Explorer 4.5, AppleWorks, Myst
   - **Ring 2 (release):** the full E-Maculation known-working list for SheepShaver
     (~30 titles), each launched + basic interaction
2. **Per-app record:** launches? / basic use OK? / known crash signature? Compare columns:
   interpreter, ARM64 JIT, x86 JIT (Rosetta). **An app that works under interpreter + x86 JIT
   but not ours = our bug, by definition.** That column comparison is the whole point.
3. **Automation:** mirror `BasiliskII/qa/` — Gherkin feature per app, VNC runner for input
   scripting, screenshot-based pass/fail. The infrastructure exists; it needs SheepShaver
   cases and assets.
4. **Crash triage protocol:** app crashes under JIT → capture guest PC → `rom-harness
   --entry=<PC>` the failing block → extract the instruction sequence into a Tier 1 vector →
   now it's a regression test forever.

## Tier 5 — Performance benchmarks (existing Workload 6, plus)

Not compatibility per se, but regressions here often reveal correctness issues (timing-
dependent guest code):

- **Speedometer 4.02** (Workload 6 — exists)
- **MacBench 5.0** (in JIT agent's next-session plan): CPU, FPU, and Disk scores; compare
  interpreter / ARM64 JIT / x86-JIT-Rosetta columns
- Track in `JIT-STATUS.md`: score table per build, flag >10% regressions

---

## What "compatibility parity with the x86 JIT" means, concretely

The eventual claim we want to make: *"The ARM64 JIT runs everything the x86 JIT runs."*
Operationally:

1. Tier 1 three-way diff: zero vectors where x86-JIT + interpreter agree and ARM64-JIT differs
2. Tier 2 boot matrix: every OS version that boots under x86 JIT boots under ARM64 JIT
3. Tier 4 Ring 1: every app that passes under x86 JIT passes under ARM64 JIT
4. Tier 5: ARM64 JIT ≥ x86-JIT-under-Rosetta performance (this should be easy — Rosetta
   double-translates)

## Sequencing

| Phase | When | Items |
|---|---|---|
| 1 | Now (cheap, high value) | Tier 1.2 coverage audit; Tier 4 crash-triage protocol (document it); Tier 2.2 boot-time canary |
| 2 | After C1 lands (JIT residency fixed — JIT actually executes enough to test) | Tier 1.1 x86 build + 3-way harness; Tier 2.1 OS boot matrix |
| 3 | After JIT-FPU work begins | Tier 3 TestFloat vectors + harness FPR seeding |
| 4 | Stabilization / pre-release | Tier 4 Rings 1-2 automation; Tier 5 MacBench |

## Open questions / prerequisites

- Does upstream x86 SheepShaver build cleanly on current macOS under Rosetta? (configure
  age, SDL versions). If not, an x86 Linux VM/container is the fallback host for the x86 lane.
- Harness FPR seeding (`SS_TEST_INIT` covers GPRs only) — needed for Tier 3.
- ROM/OS image licensing — test assets stay local, paths via env vars (existing
  `BasiliskII/qa` pattern).
- Where do nightly results live? Proposal: `JIT-STATUS.md` summary table + `qa/reports/`
  directory with per-run artifacts, mirroring BasiliskII.
