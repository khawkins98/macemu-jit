# SheepShaver ARM64 JIT Optimization Plan

Current performance: **1.88x** over interpreter (Speedometer PR 40.4 vs 21.5).
Equivalent to a real Power Mac G4 700MHz–1GHz class.

Target: **3-5x** over interpreter, approaching G4/1.8GHz class.

## Priority 1: Register Allocator Re-enable

**Expected impact**: 1.5-2x on integer-heavy code (biggest single win)
**Effort**: Low-medium (code exists, was disabled due to "boot hang regression")
**Risk**: Medium (the regression may have been one of the 12 bugs we fixed)

The RA maps PPC GPRs to ARM64 x21-x28 (8 registers) within a block. Currently
every GPR access goes through memory (`emit_load_gpr` → LDR from struct,
`emit_store_gpr` → STR to struct). With the RA, hot GPRs stay in ARM64 registers
across multiple instructions, eliminating load/store pairs.

**Approach**:
1. Re-enable by changing `emit_load_gpr`/`emit_store_gpr` to call `ra_load`/`ra_store`
2. Run harness (235/235) + ISO boot + HD boot
3. If it works: benchmark. If it hangs: use the opcode bisection tools to find which
   instruction interacts badly with the RA

**Why it was disabled**: The comment says "boot hang regression" with no further detail.
The RA interacts with `lazy_flush_cr0` and `ra_flush_all` at block boundaries. With
lazy CR0 currently disabled (all CR0 updates are immediate), the RA should be safer.

## Priority 2: Native bcctr

**Expected impact**: 5-15% on code with switch/case or dispatch tables
**Effort**: Medium (need to fix the conditional bcctr codegen bug)
**Risk**: Low (bcctr is always a block terminator, easy to fall back)

`bcctr` currently falls through to the interpreter. The conditional path had a codegen
bug (FIXED — was the last fix for HD boot, now uses interpreter fallback). The
unconditional `bctr` path was correct. Optimization opportunity:
- Native conditional bcctr with correct CR bit evaluation
- Add Mixed Mode guard (check CTR bit 0, bail if odd)
- The unconditional bctr path can be re-enabled immediately

`bcctr` is used for PPC switch/case tables (`mtctr` + `bctr`) and Mixed Mode
dispatch. Making it native eliminates a block break at every dispatch table call.

## Priority 3: Reduce Interpreter Fallbacks

**Expected impact**: 5-10% (fewer block breaks = longer native runs)
**Effort**: Medium per instruction
**Risk**: Low (each can be independently tested)

Currently falling through to interpreter:
- `lwarx`/`stwcx.` (XO=20/150): Need reservation state from CPU object. Could be
  implemented natively by adding reservation fields to the regs struct.
- `mftb` (XO=371): Need the interpreter's time-base model. Could be implemented by
  reading a shared counter that the VBL timer updates.
- `icbi` (XO=982): Need to invalidate JIT blocks. Could emit an inline call to
  `ppc_jit_aarch64_invalidate_pc()` instead of falling through.
- `isync` (XO19=150): Need to flush deferred icbi. Could be a NOP if icbi does
  immediate invalidation instead of deferred.

Each fallback terminates the block (~12 instructions of prologue/epilogue overhead)
and returns to the C dispatcher. Eliminating them allows blocks to span these
instructions and execute longer native sequences.

## Priority 4: Code Cache Sizing

**Expected impact**: 2-5% (eliminates recompilation during boot)
**Effort**: Low (change one constant)
**Risk**: Low

The 64MB code cache fills and flushes 1-2 times during boot. Each flush discards
all compiled blocks, forcing recompilation. Options:
- Increase to 128MB or 256MB (MAP_JIT memory is virtual, cost is low)
- Implement LRU eviction instead of full flush
- Track cache pressure and resize dynamically

## Priority 5: Constant Folding

**Expected impact**: 5-10% on code-heavy phases (extension loading, app launch)
**Effort**: High (requires multi-instruction analysis)
**Risk**: Medium (cross-instruction optimization can introduce bugs)

Common PPC pattern for loading 32-bit constants:
```
lis  r3, 0x1234    ; r3 = 0x12340000
ori  r3, r3, 0x5678 ; r3 = 0x12345678
```

The JIT compiles these as two separate sequences (MOVZ+MOVK for each). A peephole
optimizer could detect the `lis+ori` pattern and emit a single `MOVZ+MOVK` pair
for the full 32-bit constant.

Other foldable patterns:
- `li + slwi` (load small constant, shift) → single MOVZ with shift
- `addi rX, rX, 0` (NOP) → skip entirely
- Dead stores (store to register that's immediately overwritten)

## Priority 6: Instruction Scheduling

**Expected impact**: 3-5% (better ARM64 pipeline utilization)
**Effort**: High (requires instruction dependency analysis)
**Risk**: Medium

ARM64 has an out-of-order execution pipeline, but the JIT emits instructions in
PPC program order. Reordering independent operations could improve ILP:
- Hoist loads before dependent operations
- Interleave independent ALU ops
- Schedule byte-swap (REV) earlier to hide latency

This is a lower priority because ARM64's OoO engine already does much of this
in hardware.

## Priority 7: Byte-Swap Optimization

**Expected impact**: 2-3% (1 fewer instruction per memory access)
**Effort**: Medium
**Risk**: Low

Every guest memory access includes REV (32-bit) or REV16 (16-bit) for big-endian
conversion. Options:
- Use ARM64 load/store with byte-reversal (`LDR` with `REV` fused in some μarch)
- For consecutive loads from the same struct, load once and extract fields
- For known-constant addresses (ROM), pre-byte-swap at compile time

## Measurement Plan

For each optimization, measure:
1. **Harness**: 235/235, score=100 (correctness gate)
2. **Boot test**: ISO + HD boot to Finder desktop (no regression)
3. **Speedometer 4.02**: Full benchmark, compare PR score
4. **Boot time**: Timed ISO cold boot to desktop
5. **Heartbeat**: blocks/s, blocks compiled, j2i transitions

Track results in `docs/BENCHMARKS.md` with before/after for each change.

## Current Baseline (2026-06-03)

| Metric | Value |
|--------|-------|
| Speedometer PR | 40.424 |
| CPU score | 62.732 |
| Math score | 10310.327 |
| Benchmark Mix | 558.501 |
| JIT/Interpreter ratio | 1.88x |
| Equivalent real Mac | G4 700MHz–1GHz |
| Bugs fixed | 12 |
| Harness | 235/235 |

## Priority 0: Optimize Existing Fixes (Quick Wins)

These are performance regressions introduced by our correctness fixes that can
be tightened without changing behavior.

### 0a. bclr Mixed Mode guard: AND+CBZ → TBZ (HIGHEST IMPACT)

Every `bclr` (function return) currently pays 4 extra ARM64 instructions:
```
AND  W0, W(LR), #1     ; isolate bit 0
CBZ  W0, skip           ; branch if clear
; ... bail path ...
```

Replace with single `TBZ` (test bit and branch):
```
TBZ  W(LR), #0, skip   ; 1 instruction, zero-cycle on taken path
```

Saves 1 instruction on the hottest code path in the system. ~5-10 billion
bclr executions during a boot — even 1 cycle each adds up.

### 0b. subfe/adde: 64-bit sum → ADDS+ADCS (3-4 insns instead of 8)

Current: UXTW + UXTW + ADD X + ADD X + LSR + STRB (8 instructions)
Better: ADDS (A+B, sets C) + ADCS (result+CA, reads C and produces final C)

ARM64 ADCS reads the carry flag from ADDS and produces the correct carry-out
for the full three-operand sum. This is 3 instructions total:
```
MVN   W0, Wa            ; ~rA
ADDS  W0, W0, Wb        ; ~rA + rB, sets C
ADCS  W0, W0, Wca       ; + CA + C_from_ADDS, sets C = final carry
CSET  Wca, CS           ; extract carry
```

### 0c. isync: inline BLR instead of block break

Current: `return false` → block terminates, full prologue/epilogue overhead.
Better: emit inline `BLR` to `execute_invalidate_cache_range()` stub, continue block.

isync appears after every mtspr/mtmsr in the ROM toolbox. Each block break costs
~12 instructions of prologue/epilogue overhead. An inline BLR costs ~4 instructions
(save/restore caller-saved regs around the call).

## Priority 0d: Verify Atomic spcflags (from upstream PERFORMANCE_AUDIT)

**Expected impact**: Potentially significant if currently mutex-based
**Effort**: Low (check and change if needed)

The upstream audit found that mutex-based spcflags synchronization was a
bottleneck on RPi. Verify SheepShaver uses atomic operations for spcflags
(the VBL timer thread sets TRIGGER_INTERRUPT from a signal handler — if
this goes through a mutex, every 60Hz tick takes a lock).

## Priority 0e: Computed-goto Interpreter Dispatch (from upstream PERFORMANCE_AUDIT)

**Expected impact**: 20-40% on interpreter fallback paths
**Effort**: Medium
**Risk**: Low

The upstream audit's biggest unimplemented item. For our interpreter fallback
path (j2i transitions, inline interpreter calls), a computed-goto dispatch
loop eliminates the switch/case overhead. This matters because several
instructions still fall through to the interpreter (bcctr, lwarx, stwcx.,
mftb, icbi, isync).

Source: https://github.com/rcarmo/macemu-jit/blob/master/PERFORMANCE_AUDIT.md

## Upstream References

- [PERFORMANCE_AUDIT.md](https://github.com/rcarmo/macemu-jit/blob/master/PERFORMANCE_AUDIT.md) — BasiliskII/RPi optimization audit (14/27 implemented)
- [JIT-FPU-PLAN.md](https://github.com/rcarmo/macemu-jit/blob/master/JIT-FPU-PLAN.md) — 68K FPU JIT plan (shadow register pattern, validates our RA approach)
- Key warnings: LTO must stay disabled on macOS ARM64; do NOT remove PIE/stack-protector
