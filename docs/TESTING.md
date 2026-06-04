# SheepShaver ARM64 JIT — Functional & Conformance Testing

This document covers **correctness** testing — does the JIT compute the right
answer? For **performance** testing (Speedometer/MacBench numbers, regression
tracking) see [BENCHMARKS.md](BENCHMARKS.md). For the command-line diagnostic
env vars (`SS_JIT_DIAG_LOG`, watchpoints, ring buffer, etc.) see the
"Debugging & Investigation" section of the repo `CLAUDE.md`.

The two goals are different and need different tools: benchmarks measure speed;
the tools below measure *whether the emulation is faithful*.

---

## The core idea: you already have the oracle

The single most powerful correctness tool is built into the CPU core:

**`SS_JIT_VERIFY=1`** (gated in `ppc-cpu.cpp`) re-runs **every JIT block through
the interpreter** and compares register state after each block. It reports the
first divergences with the block PC and the offending opcodes. It is a
*differential oracle*: it turns **any program you run** into a JIT correctness
test, because the trusted interpreter is the reference.

This reframes the whole question. You don't need a self-grading test app — you
need **workloads that drive broad code coverage**, run once under VERIFY. "What's
the best functional test app?" becomes "what exercises the most code paths?"

Two caveats:
- **VERIFY is extremely slow** (every block runs twice + a comparison). Workflow:
  run a demanding workload *once* under `SS_JIT_VERIFY=1` to shake out
  divergences, then run it normally for speed/behaviour.
- It only catches **JIT ≠ interpreter**. If both are wrong identically it won't
  flag it (rare — the interpreter is decades-proven). It is a faithfulness check
  of the JIT against the interpreter, not against real silicon.

---

## Automated tests (fast, run these on every change)

| Test | Command | Gate |
|------|---------|------|
| **JIT↔interp equivalence — THE codegen gate** | `cd SheepShaver && make test-jit` | score=100 — count: `make harness-count` |
| Interpreter determinism (default) | `cd SheepShaver && make test-opcodes` | score=100 — *but see note* |
| Single opcode vector (JIT path) | `SS_TEST_HEX=<hex> SS_TEST_JIT=1 make test-opcodes` | — |
| ROM harness (headless JIT exerciser) | `cd SheepShaver && make test-rom` | no failures |
| Build | `cd SheepShaver && make build-ss` | clean |

> **MODE MATTERS — the default does NOT test the JIT.** The same vectors run
> two ways:
> - `make test-opcodes` (default, `SS_HARNESS_MODE=interp`) runs each vector
>   *twice through the interpreter* and diffs the REGDUMPs — it validates
>   **interpreter determinism only**. It does **not** exercise JIT codegen at all.
> - `make test-jit` (`SS_HARNESS_MODE=jit`) runs each vector through the
>   interpreter (reference) **and the JIT**, then diffs them. **This is the real
>   codegen correctness gate — run it on every `ppc-jit.cpp` change.**
>
> A green `make test-opcodes` (e.g. a cited "235/235") says *nothing* about JIT
> correctness — always state the mode. `make test-jit` is fast (seconds, no boot),
> so use it as the inner-loop gate. `make test` now runs both.

These are the fast correctness tier for codegen changes (use `test-jit`). **But
note their blind spots** (below) — passing them is necessary, not sufficient.

### Known coverage gaps in the automated harness

1. **The vectors are integer-heavy.** They concentrate on integer ALU,
   loads/stores, and branches. **Floating point (51 JIT ops) and AltiVec (156
   JIT ops) are comparatively under-tested**, and FP/vector encoding bugs are
   subtle. This is the biggest gap — the fix is more `test-jit` vectors plus the
   conformance apps below.
2. **Register-allocator eviction — now covered, but only in JIT mode.** The
   `lmw_stmw_wide` vector (12 GPRs > `RA_NUM_REGS=8`) forces mid-block `ra_evict`
   of both clean and dirty slots and passes under `make test-jit`. Caveat: it
   only validates the JIT spill path in **jit mode** — under default
   `test-opcodes` it merely checks interpreter determinism. (See
   `OPTIMIZATION-PLAN.md` P1a.)
3. **No timing/interrupt coverage.** The harness runs isolated instruction
   sequences; it can't catch VBL/spcflags/interrupt-delivery bugs. Only a real
   booted workload (especially games) exercises those.

---

## Testing AltiVec instructions (read this before adding vector vectors)

AltiVec is the most trap-laden corner of the harness — four rounds of vacuous or
wrong AltiVec vectors shipped in 2026-06 before this was written down. The rules:

**1. Use the generator, not hand-encoded hex.** `jit-test/gen-altivec-vectors.py`
encodes correctly and documents the traps. Run it, splice the output into
`run.sh`, gate with `make test-jit`. To add an op: add one `p(...)` line and
re-run. Hand-encoding repeatedly produced illegal/no-op instructions.

**2. VX-form XO is UNSHIFTED.** Unlike X/A-form (XO at bits 21-30, emitted `xo<<1`),
VX-form AltiVec ops put an 11-bit XO at bits 21-31 with no shift. Shifting it
yields an illegal no-op that leaves `vD` untouched — a silent vacuous pass.

**3. The result must reach a GPR, with op-dependent, non-uniform operands.**
REGDUMP captures GPRs only (not VRs/FPRs), so every vector must
`op -> stvx v2 -> lwz result into r5/r6`. And the operands must actually exercise
the op:
   - **Vacuous** = result never reaches a GPR (→ r5=0 in both modes → trivial pass).
   - **Masking** = result reaches a GPR but operands are *symmetric* so the op's
     defining property isn't tested (e.g. `vmuleub` with byte-uniform inputs:
     even/odd selection gives the same product). Use **distinct per-lane** operands
     (`lvx` a `00 01 02 … 0F` pattern), not `vspltisb` uniform splats, for any op
     whose semantics depend on element *position*.

**4. The ev_mixed byte-order trap (the deep one).** The VR is stored in the
interpreter's `ev_mixed` order (`ppc-operands.hpp`): bytes are reversed *within*
each 32-bit word, word order preserved. The JIT's `emit_load_vr` loads that raw,
so NEON lane `i` holds PPC element `byte_element(i)`, not `i`. **Any op whose
semantics depend on sub-word byte position is suspect.** Status (2026-06-04):
   - **Correct/fixed:** `vspltw`, `vsldoi`, `vspltb`/`vsplth` (remapped), the full
     `vmrgh*`/`vmrgl*` merge family (byte/halfword via the per-op `REV32.16B`
     normalize in `emit_vmrg`; words are plain `ZIP.4S`), `vpkuhum` (same normalize
     + `UZP2.16B`), and all element-symmetric ops (`vand`/`vor`/`vadduwm`/…).
   - **Confirmed BROKEN (parked):** `vmulo*`/`vmule*` (even/odd multiplies — also
     emit the wrong NEON op). `vpkuwum` (word pack) shares `vpkuhum`'s old ignore-vA
     bug, un-vectored.
   See the `case 12` / `emit_vmrg` note in ppc-jit.cpp; the global load/store REV32
   approach is ruled out (per-op is the path). Live tracker: ROADMAP A2.

**5. Differential gate is the oracle, but it can't catch masking.** `make test-jit`
diffs JIT vs interpreter — a real divergence fails, a correct op passes. But a
*masking* vector passes while testing nothing, and a *vacuous* one passes too.
The harness preflight catches malformed/duplicate vectors, **not** vacuousness —
that defense is the generator's distinct-operand construction. When in doubt, run
the vector with `SS_TEST_HEX=... SS_TEST_DUMP=1 SS_TEST_JIT=0` and confirm r5 holds
the value the comment claims.

**6. Validate against real AltiVec software** (the gold standard). The harness
covers individual ops; a real app exercises them in combination and at scale.
Best target: an AltiVec LAME-based **MP3 encoder** (DSP-heavy, deterministic).
   - **Differential boot:** `SS_JIT_VERIFY=1 ./src/Unix/SheepShaver` re-runs every
     JIT block through the interpreter and prints the exact block/opcode that
     diverges — turns the encoder into a precise AltiVec bug-finder.
   - **A/B output:** encode the same WAV with JIT (default, G4/AltiVec on) and with
     `SS_USE_JIT=0` (interpreter); deterministic encoders should byte-match — any
     difference is a JIT bug.
   - Expect misbehaviour today: LAME uses the still-broken merges/packs/multiplies,
     so garbled audio or a crash *confirms* the parked bugs bite real software.
   - Other exercisers: QuickTime AltiVec codecs, Photoshop AltiVec filters,
     GraphicConverter.

---

## Conformance apps — fill the FP/AltiVec gap

### Paranoia (floating-point) — highest priority

William Kahan's **Paranoia** is the canonical floating-point correctness torture
test. A Mac port exists. It is **self-grading** (prints defects / flaws /
serious-defects / failures) and systematically probes rounding, guard digits,
overflow/underflow — exactly the FP-JIT surface the integer harness skips.

This is the strongest single recommendation: it directly tests the part of the
JIT the automated harness covers least, and it gives a clear pass/fail without
needing VERIFY.

**Status (2026-06-04):** the in-harness FP gap is now partly closed *without* a
boot — `jit-test/run.sh` has 18 real FP-arithmetic vectors (fadd/fsub/fmul/fdiv,
the fma family, frsp/fctiwz/fneg/fabs/fmr, and the single-precision forms; run
`make test-jit`). These check exact results against the interpreter. Paranoia
adds what they can't: rounding-mode edge cases, guard-digit behaviour, and
overflow/underflow corners.

**How to run it** (manual — it needs a guest binary + a boot, so it is not a
`make` target; agents can't boot, the user runs it):
1. Get a Mac Paranoia build (Macintosh Garden / Repository — a PowerPC or FAT
   binary; the C source also compiles under CodeWarrior/MPW in-guest).
2. Copy it onto a mounted disk image (or the shared ExtFS folder).
3. Boot SheepShaver normally (JIT on by default) and run Paranoia.
4. **Pass = zero "serious defects" and zero "failures"** in its summary; "flaws"
   on transcendental-ish ops can be acceptable, but compare JIT vs interpreter
   (`SS_USE_JIT=0`) — any *difference* between the two runs is a JIT bug, since
   the interpreter is the reference.

**Automating it** (a `make test-paranoia` + CI) is deferred: it requires a
prepared disk image with the binary and a headless boot harness, which is real
infrastructure, not a quick win (the cycle-1 audit overestimated its ease). The
18 in-harness FP vectors are the boot-free FP-correctness coverage for now.

### AltiVec

There's no famous userland AltiVec conformance suite, but any AltiVec-accelerated
app exercises the 156 NEON-mapped vector ops. Run one **under `SS_JIT_VERIFY=1`**:
- Photoshop 5.5+/6 with AltiVec plugins
- QuickTime playback (AltiVec codecs)
- GraphicConverter

Reminder (see CLAUDE.md / the G3-vs-G4 discussion): SheepShaver advertises a
**7400 (G4) with AltiVec** by default (`PVR = 0x000c0000`), so Mac OS turns
AltiVec on and these apps will actually issue vector instructions.

---

## Broad-coverage workloads — the real all-around tests

Run any of these **once under `SS_JIT_VERIFY=1`** for a correctness sweep, then
normally for behaviour/speed.

### A compiler build (best all-around, and a genuine developer workload)

CodeWarrior (the era-standard IDE), MPW, or Retro68. Building a large project is:
- Heavy **integer + FP + memory + file I/O** across a huge variety of code paths.
- **Deterministic** — same source → same binary, so output mismatch = a bug.
- **Self-checking** — it either builds and the result runs, or it doesn't.

Building a big open-source project under VERIFY is probably the **single
highest-coverage correctness test** available.

### Game timedemos (full-stack + timing stress)

Marathon, DOOM, Quake. These exercise the *whole stack at once* — CPU + QuickDraw
+ sound + interrupt/timing — and are **reproducible** (a timedemo yields an FPS
number *and* a pass/fail: visual glitches or crashes are instant signal). Games
are notorious for exposing emulation **timing** bugs (tight VBL/spin loops) that
benchmarks and the harness never touch.

### Application torture tests

Photoshop, Bryce, KPT — FPU + AltiVec + QuickDraw + large memory. Classic
PowerPC-emulation torture tests.

---

## Diagnostic / subsystem exercisers

The "more than a benchmark" category — they walk subsystems and report results:

- **MacBench 5.0** (also on the benchmark list) — CPU/FPU/disk/graphics/publishing.
- **TechTool Pro**, **Snooper** (Maxa), **Norton Utilities** — RAM/CPU/FPU/disk
  diagnostics.

**Honest caveat:** under an emulator these mostly test the *emulated devices*
(disk, video, NVRAM via the `EMUL_OP` layer) rather than the CPU JIT. Good
coverage for the device/host-integration layer; weaker signal for JIT codegen
correctness. Use them to complement, not replace, the VERIFY-driven workloads.

---

## Double duty: correctness *and* optimization targeting

The same demanding workloads feed both engineering tracks:

- **Under `SS_JIT_VERIFY=1`** → correctness divergence report (this document).
- **Under the mix-aware execution profiler** (`OPTIMIZATION-PLAN.md` item P0) →
  execution-weighted hot-routine data that decides the 0g/P8/P9 ordering and
  surfaces HLE candidates. A Photoshop AltiVec filter or a Marathon timedemo is
  exactly where you'd discover whether a routine clears the HLE gate.

So picking good workloads pays off twice — run each one under both lenses.

---

## Practical shortlist

1. **Paranoia** — fills the FP-conformance gap; self-grading; cheap; do it first.
2. **One compiler build** (CodeWarrior or Retro68) under `SS_JIT_VERIFY=1` —
   highest-coverage all-around correctness driver.
3. **One game timedemo** (Marathon/Quake) — full-stack + timing stress;
   reproducible.
4. **One AltiVec app** (Photoshop + AltiVec plugin, or QuickTime) under VERIFY —
   exercises the 156 vector ops the harness barely touches.
5. Keep **Speedometer / MacBench** for the speed numbers (see BENCHMARKS.md).

All of the above are classic-Mac abandonware (Macintosh Garden / Macintosh
Repository); the installed Mac OS 8.6 disk runs them.

---

## When VERIFY reports a divergence

1. Note the block PC and opcodes from the divergence report.
2. Reproduce in isolation: pull the opcode(s) into an `SS_TEST_HEX` vector and
   diff JIT vs interpreter (`SS_TEST_JIT=1` vs `0`).
3. If it reproduces, add it as a permanent harness vector in
   `SheepShaver/jit-test/run.sh` (regression lock-in), then fix.
4. Disassemble the actual guest instructions before theorising — see the lldb /
   address-arithmetic workflow in `CLAUDE.md` (never infer behaviour from
   register state alone).

This is the loop that turns a real-world divergence into a permanent, fast
regression test.

---

## Keeping this current (maintenance contract)

This project has a documented history of test/doc rot — claims that were true
once and silently went stale:
- "235/235" was cited as a JIT correctness gate for months; it was
  interpreter-determinism (`SS_HARNESS_MODE=interp`) the whole time and never
  exercised the JIT.
- A `the register allocator is disabled` source comment outlived the re-enable.
- Plan items marked "will do X" outlived X being done.

**Principle: prefer enforcement over prose.** A check that fails is worth ten
sentences that ask a human to remember. Where a fact can be machine-verified,
verify it; where it can't, *date* it so staleness is visible.

### Enforcement already in place (keep it; extend its ethos)
- `jit-test/run.sh` self-validates its own vector table before running (no
  duplicate names, every vector has bytecode + sentinel, strict token format).
  New test infrastructure should self-check the same way.
- `make test` runs **both** harness modes + the ROM harness, so the JIT gate
  can't be silently skipped.

### Per-change checklist (changing `ppc-jit.cpp` codegen)
1. Run **`make test-jit`** (the JIT gate) — not just `test-opcodes`.
2. New/changed opcode codegen → add a **`test-jit` vector** that exercises it
   (and a **bench kernel** if it's perf-relevant). A change with no vector that
   would catch its failure mode is undertested.
3. Optimized a hot pattern → add/refresh the matching **bench kernel** and
   **re-baseline** (`jit-bench` writes a dated baseline; a stale baseline is a
   stale comparison).
4. Update the relevant `OPTIMIZATION-PLAN.md` item's status **with a date**, and
   delete any now-false "will do" wording. Don't leave a finished item phrased as
   pending.

### Freshness rules (so staleness is visible, not silent)
- **Date every claim** in the plan ("DONE (YYYY-MM-DD)") and every benchmark
  number. An undated number is presumed stale.
- **One source of truth per number.** Performance numbers live in
  `BENCHMARKS.md`; other docs *link*, never copy (the "235/235" bug was one wrong
  number duplicated across files).
- **`jit-bench` self-checks baseline freshness**: it warns if its baseline file
  is older than `ppc-jit.cpp`, so you never compare against a baseline that
  predates the code you're measuring.
- When you cite a harness score anywhere, **state the mode** (`test-jit` vs
  `test-opcodes`). A bare "236/236" is ambiguous and has burned us before.

### Where each artifact lives (so this doc stays the index)
| Artifact | File | Kept current by |
|---|---|---|
| JIT correctness gate | `jit-test/run.sh` + `make test-jit` | per-change checklist #1–2 |
| Microbench + kernels | `rom-harness/` (`jit-bench`) | checklist #3 + baseline self-check |
| Perf numbers | `docs/BENCHMARKS.md` | re-baseline on perf changes |
| Optimization status | `docs/planning/OPTIMIZATION-PLAN.md` | checklist #4 + dated items |
| This strategy | `docs/TESTING.md` | review when a tier is added/changed |
