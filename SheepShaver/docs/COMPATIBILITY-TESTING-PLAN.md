# SheepShaver Compatibility Testing Plan

How we will measure correctness and compatibility of the AArch64 JIT — against the
interpreter, against the legacy x86 SheepShaver JIT, and against real-world Mac OS software.
Drafted 2026-06-02.

**This plan extends existing infrastructure — it does not replace it:**

| Existing asset | Role in this plan |
|---|---|
| [`AARCH64_JIT_GOLDEN_WORKLOADS.md`](AARCH64_JIT_GOLDEN_WORKLOADS.md) (7 workloads) | The canonical gate. New tiers below feed into it. |
| [`jit-test/run.sh`](../jit-test/run.sh) ([README](../jit-test/README.md)) (209 vectors, interp-vs-JIT diff) | Tier 1 foundation |
| [`rom-harness/`](../rom-harness/README.md) (random ROM block exerciser) | Tier 2 foundation |
| [`qa/tests/vnc/`](../../qa/tests/vnc/README.md) (shared repo-level VNC/Gherkin runner, stories, [SheepShaver profile](../../qa/tests/vnc/profiles/sheepshaver.json)) | **The automation layer to use** — per [Golden Workloads §"Shared QA/reporting layer"](AARCH64_JIT_GOLDEN_WORKLOADS.md), do NOT grow a separate story tree |
| [`BasiliskII/qa/`](../../BasiliskII/qa/README.md) ([matrix](../../BasiliskII/qa/matrix.md)) | The matrix-document format to mirror for the OS boot matrix (Tier 2) |
| [`JIT-STATUS.md`](../../JIT-STATUS.md) | Where summary results land |
| [`JIT-FPU-PLAN.md`](../../JIT-FPU-PLAN.md) | Tier 3 (FP) integrates with this |
| [`docs/research/IMPLEMENTATION-BACKLOG.md`](research/IMPLEMENTATION-BACKLOG.md) | C1 gates Phase 2 of this plan; see also [`RESEARCH-HANDOFF.md`](research/RESEARCH-HANDOFF.md) |
| [`AARCH64_JIT_PLAN.md`](../AARCH64_JIT_PLAN.md) / [`JIT-NEXT-PHASE.md`](../JIT-NEXT-PHASE.md) | Overall JIT plan this testing supports |
| [`src/kpx_cpu/src/test/test-powerpc.cpp`](../src/kpx_cpu/src/test/test-powerpc.cpp) ([original docs](../doc/PowerPC-Testsuite.txt)) | **The original maintainer's PowerPC Emulator Tester** — dormant in-tree, see Tier 1.4 |

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

4. **Revive the original maintainer's PowerPC Emulator Tester.** Gwenolé Beauchesne's
   self-contained test suite is dormant in our tree at
   [`src/kpx_cpu/src/test/test-powerpc.cpp`](../src/kpx_cpu/src/test/test-powerpc.cpp)
   (2,242 lines; documented in [`doc/PowerPC-Testsuite.txt`](../doc/PowerPC-Testsuite.txt)).
   It is the closest thing to an *established* SheepShaver compatibility tool in existence:
   - Generates **2M+ tests** with operand values specifically chosen to exercise condition
     code changes — per-instruction-form generators for add/sub/mul/div, shifts, rotates
     (rlwinm/rlwimi!), logical ops, compares, CR-logical ops, and AltiVec
   - Two modes: **record** a golden results file on real PPC hardware, or **verify** an
     emulator against that file. The maintainer's reference file was recorded on a real
     PowerPC 7410 (PowerBook G4) — including architecturally *unspecified* result behavior
   - Terminates blocks with EMUL_OP `0x18000000`, which our CPU core already supports
   - **Work to revive:** (a) add build wiring (it has none in our Unix Makefile); (b) run it
     interpreter-only first to re-baseline; (c) make it drive the JIT path (same
     `SS_TEST_JIT`-style gate the harness uses, or directly via `ppc_jit_aarch64_compile`);
     (d) try to recover the original G4-recorded results file
     (`ppc-testresults.dat.bz2`, md5 `3e29432abb6e21e625a2eef8cf2f0840`) from
     web.archive.org — if found, we get *real-hardware* ground truth, the strongest oracle
     possible; if not, record mode under the interpreter still gives us 2M+ interp-vs-JIT
     differential vectors, dwarfing our current 209
   - This likely **supersedes item 1.3** (synthetic fuzzing) — it is exactly that, already
     written by the person who knew the CPU core best

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
3. **Automation:** use the shared repo-level `qa/tests/vnc/` tooling (stories, the
   `sheepshaver.json` profile, screenshot assertions, PDF reports) — per the Golden Workloads
   doc, SheepShaver-specific details belong in the profile/Makefile/matrix wrapper, NOT in a
   duplicated Gherkin story tree.
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
| 1 | Now (cheap, high value) | **Tier 1.4 revive test-powerpc.cpp** (build wiring + interp baseline); Tier 1.2 coverage audit; Tier 4 crash-triage protocol (document it); Tier 2.2 boot-time canary |
| 2 | After C1 lands (JIT residency fixed — JIT actually executes enough to test) | Tier 1.4 JIT mode + 2M-test differential run; Tier 1.1 x86 build + 3-way harness; Tier 2.1 OS boot matrix |
| 3 | After JIT-FPU work begins | Tier 3 TestFloat vectors + harness FPR seeding |
| 4 | Stabilization / pre-release | Tier 4 Rings 1-2 automation; Tier 5 MacBench |

## Open questions / prerequisites

- ~~Can the original G4-recorded `ppc-testresults.dat.bz2` be recovered?~~ **RESOLVED
  (2026-06-02): recovered from the Wayback Machine and verified bit-for-bit** (decompressed
  md5 `3e29432abb6e21e625a2eef8cf2f0840` matches both `test-powerpc.cpp:21` and the wiki doc).
  Now preserved in-tree:
  [`src/kpx_cpu/src/test/ppc-testresults.dat.bz2`](../src/kpx_cpu/src/test/ppc-testresults.dat.bz2)
  with [provenance](../src/kpx_cpu/src/test/RESULTS-FILE-PROVENANCE.md). Tier 1.4 has
  real-hardware ground truth available from day one.
- Does `test-powerpc.cpp` still compile against the current kpx_cpu core (it predates years
  of changes)? Budget for bit-rot fixes.
- Does upstream x86 SheepShaver build cleanly on current macOS under Rosetta? (configure
  age, SDL versions). If not, an x86 Linux VM/container is the fallback host for the x86 lane.
- Harness FPR seeding (`SS_TEST_INIT` covers GPRs only) — needed for Tier 3.
- ROM/OS image licensing — test assets stay local, paths via env vars (existing
  `BasiliskII/qa` pattern).
- Where do nightly results live? Proposal: `JIT-STATUS.md` summary table + `qa/reports/`
  directory with per-run artifacts, mirroring BasiliskII.
