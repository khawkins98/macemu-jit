# Contributing to macemu-jit (macOS arm64 fork)

Guide for humans and AI agents working on the SheepShaver/BasiliskII AArch64 JIT.

## Before You Start

1. Read `CLAUDE.md` — build commands, test commands, asset locations, architecture overview
2. Read `LEARNINGS.md` — non-obvious findings that will save you from repeating mistakes
3. Check `docs/OPTIMIZATION-PLAN.md` — what's done, what's open, what was tried and deferred
4. Check `SheepShaver/docs/research/IMPLEMENTATION-BACKLOG.md` — ready-to-implement items

## Commit Style

```
type(scope): short description — context/metric

Longer explanation if needed.
```

**Types:** `feat`, `fix`, `perf`, `docs`, `test`, `bench`, `fix+perf` (combined)

**Scopes:** `jit`, `ss` (SheepShaver), `b2` (BasiliskII), `prefs`, omit for cross-cutting

**Examples from this project:**
```
perf: atomic spcflags — replace spinlock with std::atomic (P0d)
fix+perf: adde/subfe carry-out via ADCS (P0b) — fixes backlog A1/A2
feat: clean guest shutdown, session timing, disk debug cleanup, docs
jit: re-enable register allocator + TBZ optimization for bclr
docs: reprioritize P2 (bcctr) from miss data, update changelog + handbook
test: wire JIT-equivalence harness as the codegen gate (make test-jit)
```

**Include in the commit message body:**
- Harness result (`make test-jit` score=100; count via `make harness-count`)
- Benchmark delta if measurable (`Mix 638, +15.6%`)
- `SS_JIT_VERIFY=1` status for RA/CR/flag changes
- What was tried and reverted, if applicable

## Change Checklists

### JIT codegen change (ppc-jit.cpp)

- [ ] `make test-jit` passes (score=100) — NOT `make test-opcodes` (that only tests the interpreter)
- [ ] `SS_JIT_VERIFY=1 ./SheepShaver` boot for 10-20s with no divergence (for RA/flag/branch changes)
- [ ] Normal boot to Finder desktop
- [ ] `make bench` before/after if the change affects codegen performance
- [ ] Add harness test vector for the affected instruction if one doesn't exist
- [ ] Add/update inline ARM64 mnemonic comments on emit32() calls
- [ ] Update `docs/OPTIMIZATION-PLAN.md` if completing or investigating a plan item
- [ ] Update `CHANGELOG.md` for user-visible changes
- [ ] Update `LEARNINGS.md` if the change reveals a non-obvious finding

### New instruction handler

- [ ] Call `ra_load()` for ALL source operands BEFORE `ra_store()` for the destination
- [ ] Call `lazy_flush_cr0()` before reading CR (branches, mfcr, block exits)
- [ ] Call `ra_flush_all()` before `emit_bare_epilogue()` on every exit path
- [ ] Add ARM64 mnemonic comments on emit32() lines
- [ ] Add harness test vector in `SheepShaver/jit-test/run.sh`
- [ ] If the instruction sets XER CA/OV, use `emit_write_xer_ca_from_carry()` /
  `emit_write_xer_ov_so_from_overflow()` BEFORE any instruction that clobbers NZCV

### Emulator feature change (shutdown, prefs, networking, etc.)

- [ ] Update `CHANGELOG.md`
- [ ] Update `SheepShaver/docs/USER-HANDBOOK.md` if it affects user-facing behavior
- [ ] Update prefs table in USER-HANDBOOK.md if adding/changing a pref
- [ ] Update env var table in USER-HANDBOOK.md if adding an env var
- [ ] Boot test to Finder desktop

### Documentation change

- [ ] One source of truth per number — don't duplicate harness counts or benchmark scores
  in multiple files without a clear "current as of" date
- [ ] State the harness MODE when citing a score (`test-jit` vs `test-opcodes`); for the
  count use `make harness-count`, do not hardcode a number that will drift
- [ ] Update `CLAUDE.md` Key Documentation table if adding a new doc file

### New test vector (jit-test/run.sh)

- [ ] No duplicate names in TEST_ORDER
- [ ] Strict 4-hex-word token format for bytecodes
- [ ] Unique sentinel (no reuse of existing vector hex)
- [ ] Comment explaining what the vector tests
- [ ] Passes in both `make test-opcodes` and `make test-jit`

## Key Invariants

**RA ordering:** `ra_load()` for sources before `ra_store()` for destination. Violating
this when `rD == rA` returns uninitialized data (caused a black-screen boot hang).

**RA flush:** Every code path that exits a JIT block must call `ra_flush_all()`. This
includes: `emit_epilogue_with_pc()` (already calls it), `emit_bare_epilogue()` (callers
must flush first), interpreter fallbacks (caller flushes before `emit_inline_interp_call`).

**lazy_cr0_reg vs RA eviction:** If lazy CR0 is ever re-enabled, `ra_evict()` must
materialize CR0 before evicting `lazy_cr0_reg` (infrastructure already in place).

**NZCV is shared:** Any ARM64 instruction that sets flags (ADDS, SUBS, CMP, TST, ANDS)
clobbers NZCV. Flag-reading helpers (`emit_write_xer_ca_from_carry`,
`emit_write_xer_ov_so_from_overflow`) use CSET which reads but does NOT clobber NZCV.

**emit_load_gpr64 coherence hole:** The 64-bit GPR accessors bypass the RA cache for
the low word. Safe today (PPC64 ops unreachable from 32-bit guests). Must fix before
enabling G5/PPC64 paths.

**One emulator instance at a time:** Instances share prefs, disk images, and SDL window.
Kill strays with `pkill -9 -x SheepShaver`. Agents must not launch emulator instances.

## Benchmarking

**Quick A/B (seconds, no boot):**
```bash
cd SheepShaver/rom-harness
make bench BARGS=--save-baseline=/tmp/before.txt
# ... make change, rebuild ...
make bench BARGS=--compare=/tmp/before.txt
```

**Full benchmark (minutes, requires boot):**
Boot SheepShaver, run Speedometer 4.02. Compare Benchmark Mix (integer) and
Dhrystones (stable metrics). PR composite is volatile — don't use it for JIT
codegen comparisons.

## Files That Often Need Coordinated Updates

| If you change... | Also update... |
|------------------|----------------|
| `ppc-jit.cpp` (instruction handler) | `jit-test/run.sh` (test vector), `OPTIMIZATION-PLAN.md` |
| `ppc-jit.cpp` (RA/flush/block structure) | `LEARNINGS.md`, `OPTIMIZATION-PLAN.md` |
| Prefs parsing (`prefs.cpp`) | `USER-HANDBOOK.md` prefs table |
| Emul ops (`emul_op.cpp`, `emul_op.h`) | `CHANGELOG.md`, `USER-HANDBOOK.md` |
| Harness (`jit-test/run.sh`) | nothing — the count is derived (`make harness-count`); docs are de-hardcoded |
| Build system (`Makefile`, `configure.ac`) | `CLAUDE.md` build commands |
| Env vars (any `getenv()` call) | `USER-HANDBOOK.md` env var table, `CLAUDE.md` |
| Research findings | `docs/OPTIMIZATION-PLAN.md`, `research/IMPLEMENTATION-BACKLOG.md` |
| Benchmark results | `docs/BENCHMARKS.md` |

## Project Structure (key files)

```
CLAUDE.md                          # build/test/debug reference (gitignored, local)
CONTRIBUTING.md                    # this file
LEARNINGS.md                       # session-by-session non-obvious findings
docs/OPTIMIZATION-PLAN.md          # perf roadmap: done, open, deferred
docs/BENCHMARKS.md                 # Speedometer/boot timing data
docs/TESTING.md                    # test strategy, maintenance contract
CHANGELOG.md           # user-visible changes
SheepShaver/docs/USER-HANDBOOK.md  # user guide
SheepShaver/docs/research/         # Dolphin/RPCS3/MAME research + backlog
SheepShaver/src/kpx_cpu/.../ppc-jit.cpp  # THE JIT (4500+ lines)
SheepShaver/jit-test/run.sh        # opcode test harness (count: make harness-count)
SheepShaver/rom-harness/           # standalone JIT exerciser + microbench
```
