# SheepShaver ARM64 JIT Optimization Plan

## Current Baseline (2026-06-03, post-RA)

| Metric | Pre-RA | Post-RA | Delta |
|--------|--------|---------|-------|
| Benchmark Mix | 548.6 | **634.3** | **+15.6%** |
| Dhrystones/sec | 1,348K | **1,475K** | **+9.4%** |
| CPU score | 62.7 | **64.2** | +2.2% |
| Math score | 11,539 | **12,354** | +7.1% |
| Harness | 235/235 | 235/235 | — |

Target: **3-5x** over interpreter, approaching G4/1.8GHz class.

---

## Completed

### P1: Register Allocator — DONE (2026-06-03)

**Result**: Mix +15.6%, Dhrystones +9.4%, verified with SS_JIT_VERIFY=1.

The RA maps PPC GPRs to ARM64 x21-x28 (8 callee-saved registers) within a
block.  All 32-bit instruction handlers now call `ra_load`/`ra_store` directly,
operating on RA-assigned host registers in emit32() encodings — zero MOV
bounce.  `emit_load_gpr`/`emit_store_gpr` are RA-aware fallbacks (check the
cache first) used only by the 64-bit G5 accessors.

Key constraint discovered during bringup: `ra_load` for source operands must
precede `ra_store` for the destination — otherwise `ra_store` allocates without
loading the old value, and a subsequent `ra_load` for the same GPR (when
rD == rA) returns uninitialized data.

The bclr Mixed Mode bail path includes `ra_flush_all()` before
`emit_bare_epilogue()`.  All other exit paths (block terminators, interp
fallbacks, spcflags poll) were audited and confirmed safe.

### P0a: bclr Mixed Mode guard: AND+CBZ → TBZ — DONE (2026-06-03)

**Result**: 1 instruction saved per function return.  No measurable Speedometer
delta (bclr not the bottleneck on this workload), but strictly better codegen.

Single `TBZ W(LR), #0, skip` replaces `AND W0, W(LR), #1` + `CBZ W0, skip`.
TBZ encoding uses imm14 (not imm19 like CBZ) — bit position 0 means both
b5/b40 fields are zero.

---

## Open — Quick Wins (Priority 0)

### 0b. subfe/adde: 64-bit sum → ADDS+ADCS (3-4 insns instead of 8)

**Expected impact**: Minor (carry ops are ~2% of dynamic instruction mix)
**Effort**: Low
**Risk**: Low (carry semantics already verified by harness)

Current: UXTW + UXTW + ADD X + ADD X + LSR + STRB (8 instructions).
Better: ADDS (A+B, sets C) + ADCS (result+CA, reads C and produces final C).

```
MVN   W0, Wa            ; ~rA
ADDS  W0, W0, Wb        ; ~rA + rB, sets C
ADCS  W0, W0, Wca       ; + CA + C_from_ADDS, sets C = final carry
CSET  Wca, CS           ; extract carry
```

### 0c. isync: inline BLR instead of block break

**Expected impact**: Minor per instance, but isync appears after every
mtspr/mtmsr in the ROM toolbox.
**Effort**: Low-medium

Current: `return false` → block terminates, full prologue/epilogue overhead.
Better: emit inline `BLR` to `execute_invalidate_cache_range()` stub, continue
block.  Each block break costs ~12 instructions of overhead.

### 0d. Verify Atomic spcflags (from upstream PERFORMANCE_AUDIT)

**Expected impact**: Potentially significant if currently mutex-based
**Effort**: Low (check and change if needed)

The upstream audit found that mutex-based spcflags synchronization was a
bottleneck on RPi.  Verify SheepShaver uses atomic operations for spcflags
(the VBL timer thread sets TRIGGER_INTERRUPT from a signal handler — if
this goes through a mutex, every 60Hz tick takes a lock).

### 0e. Computed-goto Interpreter Dispatch (from upstream PERFORMANCE_AUDIT)

**Expected impact**: 20-40% on interpreter fallback paths
**Effort**: Medium
**Risk**: Low

For the interpreter fallback path (j2i transitions, inline interpreter calls),
a computed-goto dispatch loop eliminates the switch/case overhead.  Matters
because several instructions still fall through to the interpreter (bcctr,
lwarx, stwcx., mftb, icbi, isync).

### 0f. Lazy CR0 Re-enable

**Expected impact**: 5-15% on Rc=1-heavy code (andi., add., rlwinm., etc.)
**Effort**: Low-medium (code exists, same "boot hang regression" disable as RA)
**Risk**: Medium (interacts with RA flush ordering and NZCV clobbers)

Currently every `Rc=1` instruction immediately computes the full CR0 field
(~12 ARM64 instructions: CMP + 3 CSELs + XER.SO merge + shift + CR load/mask/OR/store).
Lazy CR0 defers this — it records the result register and only materializes CR0
when something reads it (a branch testing CR0, mfcr, or block exit).

In back-to-back `Rc=1` sequences (common in the 68k DR emulator), only the
last one's CR0 matters — all earlier computations are wasted.  Lazy CR0
eliminates them.

**Approach**: same playbook as the RA re-enable:
1. Fix flush discipline: `lazy_flush_cr0()` before every CR0 read, block exit,
   and interpreter fallback (audit all paths)
2. Verify the saved `lazy_cr0_reg` survives RA eviction — if an RA-assigned
   register is used for lazy CR0, eviction must flush CR0 first
3. Re-enable, run `SS_JIT_VERIFY=1`, then boot + benchmark

---

## Open — Medium Effort

### P2: Native bcctr

**Expected impact**: 5-15% on code with switch/case or dispatch tables
**Effort**: Medium (need to fix the conditional bcctr codegen bug)
**Risk**: Low (bcctr is always a block terminator, easy to fall back)

`bcctr` currently falls through to the interpreter.  The conditional path had
a codegen bug (FIXED — was the last fix for HD boot, now uses interpreter
fallback).  The unconditional `bctr` path was correct.  Optimization:
- Native conditional bcctr with correct CR bit evaluation
- Add Mixed Mode guard (check CTR bit 0, bail if odd)
- The unconditional bctr path can be re-enabled immediately

### P3: Reduce Interpreter Fallbacks

**Expected impact**: 5-10% (fewer block breaks = longer native runs)
**Effort**: Medium per instruction
**Risk**: Low (each can be independently tested)

Currently falling through to interpreter:
- `lwarx`/`stwcx.` (XO=20/150): Need reservation state from CPU object
- `mftb` (XO=371): Need the interpreter's time-base model
- `icbi` (XO=982): Could emit inline call to invalidate JIT blocks
- `isync` (XO19=150): See P0c above

### P4: Code Cache Sizing

**Expected impact**: 2-5% (eliminates recompilation during boot)
**Effort**: Low (change one constant)
**Risk**: Low

The 64MB code cache fills and flushes 1-2 times during boot.  Options:
- Increase to 128MB or 256MB (MAP_JIT memory is virtual, cost is low)
- Implement LRU eviction instead of full flush

---

## Open — High Effort

### P5: Constant Folding

**Expected impact**: 5-10% on code-heavy phases (extension loading, app launch)
**Effort**: High (requires multi-instruction analysis)
**Risk**: Medium

Common PPC pattern for loading 32-bit constants:
```
lis  r3, 0x1234    ; r3 = 0x12340000
ori  r3, r3, 0x5678 ; r3 = 0x12345678
```
Could detect `lis+ori` and emit a single `MOVZ+MOVK` pair.  Other patterns:
`li + slwi`, `addi rX, rX, 0` (NOP), dead stores.

### P6: Instruction Scheduling

**Expected impact**: 3-5% (better ARM64 pipeline utilization)
**Effort**: High (requires dependency analysis)
**Risk**: Medium

Lower priority because ARM64's OoO engine already handles most reordering.

### P7: Byte-Swap Optimization

**Expected impact**: 2-3% (1 fewer instruction per memory access)
**Effort**: Medium
**Risk**: Low

Every guest memory access includes REV/REV16.  Possible improvements:
- Fused load+REV on some μarch
- Pre-byte-swap known-constant addresses (ROM) at compile time

---

## Measurement Plan

For each optimization, measure:
1. **Harness**: 235/235, score=100 (correctness gate)
2. **Boot test**: HD boot to Finder desktop (no regression)
3. **Speedometer 4.02**: Full benchmark, compare PR/Mix/CPU/Dhrystones
4. **SS_JIT_VERIFY=1**: Differential verification (mandatory for RA/CR0 changes)
5. **Heartbeat**: blocks/s, blocks compiled, j2i transitions

## Upstream References

- [PERFORMANCE_AUDIT.md](https://github.com/rcarmo/macemu-jit/blob/master/PERFORMANCE_AUDIT.md) — BasiliskII/RPi optimization audit (14/27 implemented)
- [JIT-FPU-PLAN.md](https://github.com/rcarmo/macemu-jit/blob/master/JIT-FPU-PLAN.md) — 68K FPU JIT plan (shadow register pattern, validates our RA approach)
- Key warnings: LTO must stay disabled on macOS ARM64; do NOT remove PIE/stack-protector
