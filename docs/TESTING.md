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
| JIT opcode harness (235 vectors) | `cd SheepShaver && make test-opcodes` | score=100 |
| Single opcode vector | `SS_TEST_HEX=<hex> SS_TEST_JIT=1 make test-opcodes` | — |
| ROM harness (headless JIT exerciser) | `cd SheepShaver && make test-rom` | no failures |
| Build | `cd SheepShaver && make build-ss` | clean |

These are the correctness gate for any codegen change. **But note their blind
spots** (below) — passing them is necessary, not sufficient.

### Known coverage gaps in the automated harness

1. **The harness is integer-heavy.** The 235 vectors concentrate on integer ALU,
   loads/stores, and branches. **Floating point (51 JIT ops) and AltiVec (156
   JIT ops) are comparatively under-tested**, and FP/vector encoding bugs are
   subtle. This is the biggest gap — see "Conformance apps" below.
2. **The register-allocator eviction path is unexercised.** The `lmw`/`stmw`
   vectors top out at 4 registers, under `RA_NUM_REGS=8`, so `ra_evict` (the
   spill/flush path under register pressure) never fires in the harness. See
   `OPTIMIZATION-PLAN.md` item **P1a** (adding an `lmw r20` 12-register vector
   closes this and takes the harness to 236/236).
3. **No timing/interrupt coverage.** The harness runs isolated instruction
   sequences; it can't catch VBL/spcflags/interrupt-delivery bugs. Only a real
   booted workload (especially games) exercises those.

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
