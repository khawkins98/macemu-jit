# ROM Harness — Headless JIT Exerciser

A standalone tool that loads a Mac ROM file and exercises the SheepShaver AArch64 JIT
by compiling and executing real ROM code blocks, comparing JIT output against a built-in
reference interpreter.

## What It Does

1. **Loads the ROM** — reads the raw 4MB ROM file into memory
2. **Scans for blocks** — finds PPC basic blocks (instruction sequences ending at branches)
3. **Filters** — skips blocks with memory access, privileged ops, or EMUL_OP trampolines
4. **Tests each block** — seeds registers with deterministic pseudo-random values, runs both
   interpreter and JIT, compares all 32 GPRs + CR + XER + LR + CTR + PC
5. **Reports** — shows pass/fail counts, JIT coverage stats, and detailed mismatch info

## Building

```bash
make        # builds rom-harness
make clean  # removes build artifacts
```

## Usage

```bash
# Full scan of all testable blocks
./rom-harness <rom-file>

# Quick smoke test (first 500 blocks, stop on first failure)
./rom-harness <rom-file> --count=500 --stop-on-fail --verbose

# Test a single block at a specific ROM offset
./rom-harness <rom-file> --entry=0x1910 --verbose

# Multi-pass with different random seeds
./rom-harness <rom-file> --passes=10 --seed=42

# Control block size
./rom-harness <rom-file> --min-insns=2 --max-insns=16
```

## Options

| Option | Description | Default |
|--------|------------|---------|
| `--offset=0xN` | Start scanning at ROM offset | 0 |
| `--count=N` | Max blocks to test (0=all) | 0 |
| `--verbose` | Print each block result | off |
| `--stop-on-fail` | Stop at first mismatch | off |
| `--min-insns=N` | Minimum block size | 1 |
| `--max-insns=N` | Maximum block size | 64 |
| `--entry=0xN` | Test single block at offset | — |
| `--seed=N` | Random seed for register init | 0xDEADBEEF |
| `--passes=N` | Number of random passes | 1 |
| `--compute-only` | Skip memory-access blocks | on |
| `--all-blocks` | Include memory-access blocks | off |

## Microbenchmark mode (`--bench` / `make bench`)

Fast, deterministic per-instruction timing of the JIT's codegen, so an
optimization can be A/B'd in **seconds, no boot**. Needs no ROM.

```
make bench                                   # run all kernels
make bench BARGS=--save-baseline=/tmp/bb.txt # record a baseline (before a change)
make bench BARGS=--compare=/tmp/bb.txt       # show % deltas vs the baseline (after)
make bench BARGS=--bench-iters=1000000       # more iterations = less noise
```

Three columns, computed by **differential timing** — each kernel is compiled at
two sizes (16 and 144 body instructions) and the fixed per-call cost
(prologue/epilogue + register reset) cancels in `(big − small) / (144 − 16)`:

| Column | What it is | Noise |
|---|---|---|
| **`ns/insn`** | wall-time per PPC op — **median** across `BENCH_ROUNDS` interleaved rounds (each round = min-of-5 after warm-up) | host-dependent (see below) |
| **`cv%`** | coefficient of variation of `ns/insn` across rounds — the host-noise estimate | — |
| **`a64/op`** | emitted **ARM64 instructions per PPC op** (from the JIT's own `code_size`) | **ZERO — deterministic** |

**The noise gate.** Timing is only `<1%` on a *quiet, cool* machine. Under load
(parallel builds/boots, a co-tenant agent, thermal pressure) it swings wildly —
a fixed, unchanged binary has been measured at **±25%**. So:
- a `cv%` above `BENCH_CV_NOISE_PCT` (3%) is flagged with `*`, and `--compare`
  prints **`NOISY`** instead of a timing delta rather than reporting a number the
  host already proves untrustworthy;
- **`a64/op` is deterministic** — it is the JIT's exact emitted-instruction count,
  identical run-to-run regardless of host load. It catches every
  redundant-instruction-removal win (trailing-MOV elimination, leaner CR0, etc.)
  with **zero noise**, and works on a loaded laptop or in CI. Use it as the
  primary signal for codegen-size optimizations; use `ns/insn` (on a quiet host)
  only for timing-only wins it can't see — instruction scheduling, equal-count
  substitutions, latency reordering.

On Apple Silicon the bench pins itself to a performance core via QoS
(`QOS_CLASS_USER_INTERACTIVE` — thread affinity is a no-op there) and times with
`CLOCK_THREAD_CPUTIME_ID` (excludes time a co-tenant preempted us). Neither
corrects DVFS/thermal; that is what the `cv%` gate is for.

Kernels target specific optimizations: `carry-chain` (adde — 0b/0f), `rc1`
(add. — 0g lazy-CR0), `alu` (RA throughput), `fp-add`/`fp-fma` (FP latency —
these run ~30× slower per insn than integer ALU because the JIT has no FP
register allocator yet; every FP op round-trips the FPRs through the regs struct),
`compute` (mullw/divw/add recurrence — models the runtime-dominant Speedometer
hot block `0x1ed7befc`).

### Maintenance (per `docs/TESTING.md`)
- **Baselines are per-machine** (ns depends on the host CPU) — **do not commit
  them**. Record a baseline on your machine right before an optimization, compare
  right after.
- `--compare` **warns if the baseline file is older than `ppc-jit.cpp`**, so you
  never A/B against a stale baseline.
- **To add a kernel:** add a `k_*()` emitter and a `BENCH_KERNELS[]` row in
  `rom-harness.cpp`, then re-baseline. Register-only kernels work anywhere;
  memory kernels (`k_loadstore`) are deferred — guest data access needs the
  DIRECT_ADDRESSING base set up (macOS can't map the low 4 GB).

## Architecture

- **No SheepShaver dependencies** — compiles standalone against only the JIT source
- **Built-in reference interpreter** — subset PPC interpreter covering all ops the JIT handles
- **Register layout** matches the JIT's expected struct offsets exactly (verified with static_assert)
- **SIGSEGV protection** — JIT crashes are caught and reported, never abort the harness

## Interpreting Results

```
Score: 663/766
```

- **Score** = passed / (passed + failed)
- **Skipped** blocks are excluded from the score (incomplete JIT, unsupported interp ops)
- **JIT compile fail** = JIT couldn't compile the block (incomplete, unknown opcode)
- **Interp unsupported** = harness interpreter doesn't handle an opcode in the block
- **Span mismatch** = the JIT compiled/ran more instructions than the scanner's block (a `bc`
  inside the block: terminator to the scanner, fall-through to the JIT). Skipped because interp and
  JIT would run different instruction spans — see the block-model note below.
- **JIT fallback** = a block the compiler marked `complete` still emitted an inline-interp
  fallback call at runtime (`ppc_jit_interp_one`), which this standalone harness can't execute.
  These are **skipped, not fatal** — previously the bridge `abort()`ed, killing the whole run
  on the first such block and making broad random sweeps impossible. (The underlying cause —
  `complete` not distinguishing fallback-ending blocks — is tracked as fix-(ii) must-fix (a) in
  `docs/superpowers/specs/2026-06-05-verify-memory-snapshot-fix-ii-design.md`.)

> **⚠️ Most raw-ROM-scan failures are a block-model mismatch, NOT codegen bugs.** The scanner ends
> a block at the first **terminator**, and it counts a conditional branch (`bc`, opcode 16) as a
> terminator — so a `bc`-terminated block is, to the scanner, `blk.n_insns` long ending at the
> `bc`. **The JIT does not treat `bc` as a block terminator** (both arms fall through to the
> dispatcher), so it compiles and runs *past* the `bc` — `jblk.n_insns` can be much larger. The
> harness then compares a short interp run against a longer JIT run of the **same start PC**:
> registers diverge because the two ran *different instruction spans*, not because either is wrong.
> (This is the exact analog of the `SS_JIT_VERIFY` "fix (i)" block-exit problem — see
> `docs/superpowers/specs/2026-06-05-verify-memory-snapshot-fix-ii-design.md` and OPTIMIZATION-PLAN
> §0b-extra4.) **This is now gated:** the harness compares only when `jblk.n_insns == blk.n_insns`
> and reports the dropped blocks as **`Span mismatch`** (so the coverage cost is visible). On the
> OldWorld ROM this cleaned the signal from ~46 failures/seed to **~7–8 span-matched, trustworthy
> failures** (≈711 bc-terminated blocks now skipped instead of producing false diffs). A future
> upgrade (ROADMAP A1) could *recover* that dropped coverage by running the interpreter for the
> JIT's instruction count instead of skipping. Note this means GPR diffs are no longer cascade
> artifacts — the remaining failures are genuine span-matched divergences worth refereeing.
>
> **Worked example (2026-06-05, verified).** The single-instruction block `42424642`
> (`bc BO=18,BI=9`, AA=1) "fails" here — but the JIT compiled **5** instructions past it while the
> harness interp ran **1**. The `bc` codegen itself is correct: a non-vacuous real-emulator test
> `SS_TEST_HEX="38600002 7C6903A6 42424642"` (li r3,2; mtctr r3; bc) gives **identical** interp and
> JIT REGDUMP (CTR 2→1, branch taken to the correct AA=1 target). So this is a harness
> block-comparison artifact, *not* a JIT bug and *not* a harness-interp ISA bug.
>
> To confirm any *genuinely* suspicious failure, use the **real** emulator as referee:
> `SS_TEST_HEX=<hex...> SS_TEST_DUMP=1 SS_TEST_JIT={0,1} ./src/Unix/SheepShaver`, diff the REGDUMP.
> The scanner also runs mis-scanned **data** as code (string tables `50425831`=`"PBX1"`,
> `206d656d`=`" mem"`). Net: the absolute failure count is a noisy upper bound for regression
> deltas (same seed, before/after), **not** an "N JIT bugs" figure — and it is currently dominated
> by the block-model mismatch on legitimate branch-terminated blocks.

## Bugs Found By This Harness

1. **CR logical NOP-default** — `mcrf`, `crand`, `cror`, etc. silently treated as NOPs
2. **Missing XER[SO] in comparisons** — `cmp`, `cmpi`, `cmpli`, `cmpl` didn't set CR[SO]
3. **Wrong NZCV→CR mapping in cmpi** — raw ARM64 NZCV used instead of proper signed LT/GT/EQ
4. **bdz not implemented** — only bdnz was handled, bdz (BO=0b01111) fell into wrong path
5. **bc epilogue skip-over bug** — CBZ/CBNZ +8 didn't skip the full multi-instruction epilogue
