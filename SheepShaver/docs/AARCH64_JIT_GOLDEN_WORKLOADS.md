# SheepShaver AArch64 JIT — Golden Workloads

> **Lineage note (macOS arm64 fork):** the run-commands below are the **upstream Linux dev
> harness** (`make run-tmux` / `run-jit-tmux`, Xvfb). On macOS arm64 those targets don't apply —
> launch the binary directly (see `CLAUDE.md` → Running) — but the *workloads and pass-conditions*
> still hold. `<repo>` = your macemu-jit checkout. **Boot status is current as of 2026-06-04:**
> SheepShaver boots Mac OS 8.6 to the Finder desktop with the **full native JIT** (ROM=0x500000,
> chaining on, no containment gates) — see `JIT-STATUS.md` / `CHANGELOG.md`.

## Purpose

This document defines the canonical workloads that validate the SheepShaver PPC JIT.

A JIT change is not good because it moves the frontier.
A JIT change is good if it improves or preserves these workloads.

---

## Workload 1: Interpreter parity (opcode equivalence harness)

**What it tests**: Exact semantic parity between interpreter and JIT for every implemented opcode.

**How to run**:
```bash
cd <repo>/SheepShaver
SS_USE_JIT=1 make test-opcodes
```

**Pass condition**:
- All harness vectors produce identical REGDUMP output for interpreter and JIT modes
- No SIGSEGV during JIT execution of test vectors
- Compiler test mode: `METRIC pass=N fail=0`

**Status**: ✅ Maintained. Run before and after any opcode handler change.

---

## Workload 2: Boot to desktop (interpreter)

**What it tests**: Full Mac OS 7.5 boot through ROM init, extension loading, Finder launch.
Validates: EMUL_OP handling, interrupt dispatch, block boundaries, memory model.

**How to run**:
```bash
cd <repo>/SheepShaver
make run-tmux TMUX_SESSION=ss-boot-interp PREFS_DIR=/tmp/ss-boot-interp VNC_PORT=5999
# Wait ~12s, connect VNC, verify desktop visible
```

**Pass condition**: Mac OS desktop visible via VNC. No SIGSEGV.

**Status**: ✅ Stable. Must not regress when any JIT dispatch path is changed.

---

## Workload 3: Boot to desktop (JIT)

**What it tests**: Same as Workload 2 but with SS_USE_JIT=1.
Additionally validates: JIT dispatch loop, PPCR_PC after JIT call, spcflag handling, block cache.

**How to run**:
```bash
cd <repo>/SheepShaver
make run-jit-tmux TMUX_SESSION=ss-boot-jit PREFS_DIR=/tmp/ss-boot-jit VNC_PORT=5999
# Wait ~12s, connect VNC, verify desktop visible
```

**Pass condition**: Mac OS desktop visible via VNC. No SIGSEGV. Crash log clean.

**Status**: ✅ Stable (2026-06-04). The full native JIT boots Mac OS 8.6 to the Finder desktop —
ROM=0x500000, chaining on, no containment gates, no skip list. Use this workload for JIT
boot-**regression** validation.

---

## Workload 4: ROM harness

**What it tests**: Broad PPC opcode coverage via random ROM code execution.
Validates: arithmetic, branches, memory ops, FPU basics across wide input range.

**How to run**:
```bash
cd <repo>/SheepShaver
make test-rom
```

**Pass condition**: No SIGSEGV. Score >= 95%.

**Status**: ✅ Maintained.

---

## Workload 5: VNC interaction smoke

**What it tests**: VNC connection, mouse/keyboard event delivery to Mac OS.
Validates: threading, ADB injection, SDL event drain, VNC framebuffer update.

**How to run**:
```bash
cd <repo>/SheepShaver
make run-tmux
# Connect VNC, click desktop icons, type text

# Automated boot + capture (isolated prefs + pristine disk):
cd <repo>/SheepShaver && make e2e
# see SheepShaver/e2e/README.md
```

**Pass condition**: Clicks and keystrokes reach Mac OS. No crash on input events. The E2E harness drives a boot, waits for the `[BOOT]`/`[READY]` signals, and captures VNC screenshots.

**Status**: ✅ Stable after VNC threading fix (commit a0f4cc7c). Automated coverage is the `SheepShaver/e2e/` harness (the older repo-level `qa/tests/vnc/` Gherkin runner was removed 2026-06-05 — Xvfb/Linux-oriented).

---

## Workload 6: Speedometer 4.02 (PPC-native)

**What it tests**: Full PPC-native benchmark workload. Validates JIT correctness and performance
across CPU-intensive code paths (arithmetic, FPU, string, graphics primitives).

**How to run**:
```bash
cd <repo>/SheepShaver
make run-tmux PREFS_DIR=/tmp/ss-bench
# Connect VNC
# Navigate Benchmark.hda → Speedometer 4.02
# Run benchmark, read scores
```

**Pass condition**: Completes all benchmark categories without crash. Reports valid scores.

**JIT maturity ladder** (per JIT-APPROACH-RESET):
- Interpreter score: baseline
- JIT with containment gates: comparable to interpreter for full-system workloads (correctness first)
- JIT with block cache/chaining: expected speedup on hot native PPC loops; tight-loop microbench is ~737 MIPS
- Revalidated lazy CR0/register allocation: target for future optimization phases

**Status**: ✅ JIT boot-to-desktop is stable and a Speedometer 4.02 baseline is recorded
(1.88× over interpreter, 2026-06-04 — see `docs/BENCHMARKS.md`). A repeatable full MacBench run
is still outstanding.

---

## Workload 7: Application compatibility smoke (Prince of Persia)

**What it tests**: 68K application compatibility via Mac OS 68K-in-PPC emulator in ROM.
Validates: EMUL_OP dispatch, resource manager, 68K→PPC context switches.

**How to run**:
```bash
cd <repo>/SheepShaver
make run-tmux PREFS_DIR=/tmp/ss-bench
# Connect VNC, launch Prince of Persia from Benchmark.hda
```

**Pass condition**: Game starts, intro screen renders without crash.

**Status**: ⚠️ Crashes during `PatchNativeResourceManager` when loading game resources.
Root cause: GetNamedResource/Get1NamedResource native hooks use invalid tvec for this ROM/OS path.
Fix: those two hooks disabled (commit fbb716a0). Residual crash is in Mac ROM/68K emulator path.

---

## Automated E2E layer

Automated boot/desktop coverage runs through the `SheepShaver/e2e/` harness — see
[`SheepShaver/e2e/README.md`](../e2e/README.md). It boots in an isolated config (its own prefs
+ a pristine per-run disk image, never the user's), waits for the `[BOOT]`/`[READY]` boot
signals, and captures VNC screenshots; `make e2e` is the entry point.

> The older repository-level VNC/Gherkin story tree (`qa/tests/vnc/` + `BasiliskII/qa/`) was
> **removed 2026-06-05** — it was Xvfb/Linux-oriented and superseded by `SheepShaver/e2e/`.
> Extend the E2E harness for new desktop/app flows rather than reviving a separate story tree.

---

## Maturity ladder

A change is mature when all workloads below it are green:

```
L0  Interpreter-only boot                  (Workload 2 green)
L1  JIT dispatch enabled, complete-block gate present  (Workload 3 progresses)
L2  Block cache/chaining added                         (hot-loop + boot-progress workloads green)
L3  Lazy CR0/register allocation revalidated           (all harnesses green + boot proof)
L4  Complete-block policy revisited only with proof     (all fallback/barrier semantics audited)
```

Do not report performance numbers until the workload's maturity level is declared.

---

## Known blockers

| Workload | Blocker |
|----------|---------|
| 3 (JIT boot) | ✅ Cleared 2026-06-04 — boots to Finder with full native JIT |
| 6 (Speedometer) | Baseline recorded (1.88×); repeatable full MacBench run still outstanding |
| 7 (PoP) | PatchNativeResourceManager crash in ROM path (unverified on macOS) |

---

## Related plans

- [`COMPATIBILITY-TESTING-PLAN.md`](../../docs/planning/sheepshaver-research/COMPATIBILITY-TESTING-PLAN.md) — how these workloads grow
  into full compatibility measurement (x86-JIT oracle, OS boot matrix, TestFloat FP vectors,
  app compatibility rings)
- [`research/IMPLEMENTATION-BACKLOG.md`](../../docs/planning/sheepshaver-research/research/IMPLEMENTATION-BACKLOG.md) — JIT work items
  from the 2026-06 research effort (C1 residency fix gates several workloads above)
