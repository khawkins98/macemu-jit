# SheepShaver PPC Opcode Equivalence Harness

> This harness is Tier 1 of the broader
> [Compatibility Testing Plan](../../docs/planning/sheepshaver-research/COMPATIBILITY-TESTING-PLAN.md) (planned extensions:
> x86-JIT third oracle, coverage audit, TestFloat FP vectors).

## Status

**209 deterministic test vectors** (functional plus fuzz/regression-style cases), all passing (`METRIC pass=209 fail=0 score=100`).

## How it works

1. Set `SS_TEST_HEX` to a space-separated hex sequence of PPC instructions (big-endian 32-bit words)
2. Set `SS_TEST_DUMP=1` to emit a `REGDUMP:` line with GPR0-31, CR, LR, CTR, XER
3. Optionally set `SS_TEST_INIT` to seed GPR0-31 + optional CR before execution
4. Optionally set `SS_TEST_JIT=1` to compile and execute via the AArch64 JIT

The harness runs each vector twice and diffs the output for determinism. `SS_TEST_JIT=1`
forces the AArch64 JIT path for single-vector fallback/native checks; unsupported or
barrier-worthy instructions should report `SS_TEST_JIT: fallback to interpreter` rather than
silently compiling as NOP.

## Running

```bash
# Full harness (legacy, authoritative gate — one process per vector)
./jit-test/run.sh

# Full harness (batch mode — one process per mode, ~3s vs ~32s)
SS_HARNESS_BATCH=1 ./jit-test/run.sh

# Single vector (interpreter)
SS_TEST_HEX="38600064 388000c8 7CA32214" SS_TEST_DUMP=1 src/Unix/SheepShaver

# Single vector (JIT)
SS_TEST_HEX="38600064 388000c8 7CA32214" SS_TEST_DUMP=1 SS_TEST_JIT=1 src/Unix/SheepShaver
```

## Batch mode (`SS_HARNESS_BATCH=1`)

Opt-in mode that runs all vectors in **one emulator process per mode** (two total: one
interpreter run, one JIT run) instead of spawning a fresh process per vector (~700 launches
→ 2). Timing: **~3s vs ~32s** (legacy). The METRIC contract (`pass=N fail=N total=N score=N`)
and per-vector REGDUMP content are identical in both modes.

### How it works

The emulator's `SS_TEST_HEX_FILE` batch path accepts a file of `name<TAB>hex` lines, runs
all vectors sequentially in one process, and emits a `=== VECTOR name ===` frame marker plus
a REGDUMP per vector to stderr. The harness splits the combined output into per-vector files
and runs the same scoring diff logic as legacy mode.

**Per-vector state reset** (inside the one process): each vector gets a fresh zero-filled
test RAM region, a fully reset register file (GPR/CR/XER/LR/CTR **and** FPR/VR/FPSCR/vrsave),
an interpreter cache invalidate, and a JIT cache flush. The CPU object and JIT init are
reused across vectors — this is mandatory, not just an optimization: vm_acquire's monotonic
bump allocator never reclaims, and per-vector CPU new/delete + JIT init/exit accumulates
global state that crashes the process after a few hundred iterations.

### Equivalence proof (not vacuous)

The equivalence was established by more than just matching scores:
- Per-vector REGDUMP content is **byte-identical** legacy-vs-batch for all 350 vectors in
  both interpreter and JIT modes.
- A deliberately corrupted vector output makes batch report that exact vector as fail
  (`pass=349`), confirming the diff/scoring logic is live, not vacuous.

### The FP/VR state-bleed lesson

During development, a batch run without FPR/VR/FPSCR/vrsave reset passed `score=100` even
though floating-point and vector register state bled between vectors. Both legacy mode and
the (broken) batch mode produced the same stale state, so their REGDUMPs matched and the
diff was clean. **A score-only equivalence check would have missed this entirely** — the bug
was only caught by comparing REGDUMP *content* byte-for-byte. This is why the equivalence
proof uses content diffing, not just score comparison. The lesson generalises: if both modes
share a bug identically, `score=100` passes vacuously. REGDUMP-content equivalence is the
real check.

### When to use which mode

| Mode | When to use |
|------|-------------|
| `SS_HARNESS_BATCH=1 make test-jit` | Inner development loop — fast feedback after each change (~3s) |
| `make test-jit` (legacy) | Authoritative gate — at least once per task/commit; process-per-vector isolation is the stronger contract |
| `SS_HARNESS_KEEP=1 SS_HARNESS_BATCH=1 make test-jit` | Preserve REGDUMPs in `/tmp/` for manual auditing |

## Metrics

- `METRIC pass=N` — vectors where both runs match
- `METRIC fail=N` — vectors with mismatch or error
- `METRIC total=N` — total vectors
- `METRIC score=N` — pass percentage

## Current vectors

| Vector | Opcodes tested |
|--------|---------------|
| `alu_add` | `li`, `add` |
| `alu_sub` | `li`, `subf` |
| `alu_and` | `li`, `and` |
| `alu_or` | `li`, `or` |
| `alu_xor` | `li`, `xor` |
| `li_wide` | `lis`, `ori` (32-bit immediate) |
| `shift_slw` | `li`, `slw` |
| `shift_srw` | `li`, `srw` |
| `cmp_beq` | `cmpw`, `beq`, conditional branch skip |
| `bdnz_loop` | `mtctr`, `addi`, `bdnz` (5 iterations) |
| `mul_basic` | `li`, `mullw` |
| `rlwinm_basic` | `rlwinm` rotate + mask |
| `nop` | NOP sanity |
| `neg_basic` | `neg` |
| `sraw_signext` | `sraw` with sign extension + XER CA |
| `stw_lwz` | `stw`/`lwz` memory round-trip |
| `stb_lbz` | `stb`/`lbz` byte round-trip |
| `sth_lhz` | `sth`/`lhz` halfword round-trip |
| `addic_dot` | `addic.` with CR0 update |
| `add_dot_neg` | `add.` with negative result → CR0.LT |
| `divw_basic` | `divw` integer divide |
| `mtctr_mfctr` | `mtctr`/`mfctr` SPR round-trip |
| `addic_carry` | `addic` carry flag (XER.CA) |
| `adde_carry` | `adde` extended add with carry |
| `rlwimi_insert` | `rlwimi` rotate and mask insert |
| `cntlzw_basic` | `cntlzw` count leading zeros |
| `extsh_basic` | `extsh` sign-extend halfword |
| `extsb_basic` | `extsb` sign-extend byte |
