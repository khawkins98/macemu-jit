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

## Open — Hardening (correctness debt on P1; gates P8)

### P1a. Harden the register allocator

**Why first**: P1 shipped a real +15.6%, but its subtlest path — `ra_evict`
under register pressure (>8 live GPRs in one block) — is **never exercised by
the harness**. The `lmw`/`stmw` vectors top out at 4 registers (r28–r31), under
`RA_NUM_REGS=8`, so eviction never fires. 235/235 proves block-exit flush for
small blocks; it says nothing about mid-block spill. P8 (cross-block pinning)
extends the RA's cross-block lifetime, so this must be verified before relying
on it.

**Effort**: Low (~1 hour)
**Risk**: This *closes* risk rather than adding it.

1. **Add a >8-live-GPR eviction vector.** `lmw r20, d(r1)` loads r20–r31 (12
   regs) → forces ≥4 evictions, including a clean-drop (the base reg) and
   dirty-flushes (earlier-loaded regs). The REGDUMP diff catches any wrong value
   immediately. (This is the vector that takes the harness to 236/236.)
2. **Run `SS_JIT_VERIFY=1` boot** — the only check that exercises eviction +
   cross-block flush on a real workload.
3. **Document/fix the 64-bit accessor coherence hole.** `emit_load_gpr64` /
   `emit_store_gpr64` read/write `PPCR_GPR(n)` (the low word) **directly**,
   bypassing the RA's low-word cache. A 32-bit op leaves a dirty low word in
   x21–x28; a subsequent 64-bit read sees the stale struct word. Reachable only
   from PPC64 doubleword opcodes (`rld*`/`sld`/`srd`/`cntlzd`/`ld`/`std`/`lwa`)
   — a 32-bit Mac OS guest never issues these, so it is **not reachable today**
   — but it is **mandatory before any G5/PPC64 path**. Fix: route the low word
   through `ra_load`/`ra_store` (the high word is fine — the RA never caches it).
   At minimum, comment both helpers so the next person doesn't trip on it.
4. **Pin the real invariant** (comment near `RA_NUM_REGS`): the allocator is safe
   because **`RA_NUM_REGS` (8) ≥ simultaneously-live RA operands in a single
   *emitted* instruction (≤3, e.g. `ADD hD,hA,hB`)** — NOT "distinct GPRs per
   opcode." `lmw` touches 12 GPRs and is still safe precisely because only one
   RA reg (`hR`) is live per emitted op; the address and loaded value ride in
   `RTMP0`/`RTMP1`.

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

### 0f. Eliminate trailing MOV in carry/div/OE ops

**Expected impact**: Minor per instruction, but carry/OE ops are hot in the
68k DR emulator loops (addco/subfco are 76%+19% of compile targets there)
**Effort**: Low
**Risk**: Low (flag-read ordering must be preserved)

Several carry and overflow ops route through RTMP0 for the ADDS/SUBS (to set
NZCV for carry/overflow extraction), then MOV the result to the RA destination:
```
ADDS  W0, W(hA), W(hB)   ; sets NZCV
; ... extract carry from NZCV ...
MOV   W(hD), W0           ; ← eliminable
```
Where the flag-read ordering permits, compute directly into the RA host reg:
```
ADDS  W(hD), W(hA), W(hB)   ; sets NZCV, result already in hD
```
Same opportunity in divw (SDIV into RTMP2, then MOV to hD) and the mulhw/mulhwu
64-bit product (SMULL/UMULL + LSR into RTMP0, then MOV to hD).  The `addi`/`addis`
with `ra==0` already demonstrates this pattern — extend it uniformly.

### 0g. Lazy CR0 Re-enable

**Expected impact**: 5-15% on Rc=1-heavy code (andi., add., rlwinm., etc.)
**Effort**: Low-medium (code exists, same "boot hang regression" disable as RA)
**Risk**: Medium — and the risk is *real*, not nominal: lazy CR0 was disabled for
a boot-hang regression that, like the RA's, was **never root-caused**. Re-enabling
inherits that latent failure, so it demands the same rigor that made the RA
re-enable safe: full flush-discipline audit + mandatory `SS_JIT_VERIFY=1` boot,
not just a green harness. It also interacts with RA flush ordering and NZCV
clobbers. Do not ship on harness-pass alone.

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

### P8: Cross-Block Register Pinning (r1/SP, r2/RTOC)

**Expected impact**: Potentially large — eliminates per-block reload of the two
most-accessed GPRs in every PPC program
**Effort**: High (requires ABI contract at chain boundaries)
**Risk**: Medium-high
**Depends on**: P1a (RA eviction hardening). Pinning extends the RA's cross-block
lifetime, so the eviction/flush path must be verified before relying on it.

Currently the RA resets at every block boundary — `ra_reset()` clears all
cached GPRs, so every block reloads r1 (stack pointer) and r2 (RTOC) from
the struct even if the previous block just stored them.  With block chaining,
the callee-saved RA registers (x21-x28) survive across chain sites.

Pinning r1→x21 and r2→x22 across chained blocks would eliminate two LDR/STR
pairs per block transition.  Requires:
- A stable RA→host-register mapping contract at chain entry points
- Chain patching must preserve or restore the pinned registers
- `ra_reset()` at chain entry must pre-populate the pinned mappings
- Invalidation must account for pinned state

This is the biggest remaining lever for the RA — the per-block overhead of
reloading hot registers dominates now that intra-block allocation is done.

### P9: Indirect-Branch Chaining for bclr

**Expected impact**: 5-10% on function-return-heavy code
**Effort**: High
**Risk**: Medium

Every `bclr` (function return) currently round-trips the C dispatcher: the
JIT block stores the LR target to `regs.pc`, restores callee-saved regs,
returns to C, C looks up the target block, and re-enters with the full
prologue.  An indirect-branch chain would instead:
1. Look up the LR target in the block cache at runtime (inline hash probe)
2. If found: branch directly to the target's chain entry (skip dispatcher)
3. If not found: fall back to the dispatcher

This is independent of the RA work.  The main challenge is the inline hash
probe code size and the correctness of cache-miss fallback.  Dolphin's PPC
JIT uses a similar technique for `blr` fastpath.

---

## Open — Orthogonal: Selective HLE (high-level emulation of hot routines)

This is a *different axis* from everything above: instead of making the JIT's
translated output better, replace a few specific, hot, well-understood guest
routines with native C++/NEON fast-paths. **`gfxaccel.cpp` already proves the
pattern** — native QuickDraw fillrect/bitblt/invrect hooks (opt-in via the
`gfxaccel` pref).

**Why this reaches what JIT-internals cannot**: `compile_one` is strictly
per-PowerPC-instruction — no loop or idiom analysis. It maps guest AltiVec → NEON
1:1, but it will **never** vectorize a scalar copy loop, because that requires
recognizing the loop *as* a memcpy. So for bulk-data routines the gap isn't
"native vs. JIT," it's "vectorized vs. scalar" — a 4–16× cliff that no amount of
RA/CR0/chaining work can touch. P8/P9 lift the scalar-bound majority; HLE reaches
the vectorizable slice. **They are additive, not competing.**

**The gate (all three must hold)**:
1. **Hot** — surfaced by the P0 profiler below (execution-weighted, not
   compile-frequency).
2. **Scalar in the guest** — an already-AltiVec routine is a *non-candidate*: the
   JIT already emits NEON 1:1, so HLE buys little. The profiler's instruction-mix
   tag decides this.
3. **Bulk data** — so wide NEON loads/stores give a large factor.

**Candidates**: `BlockMove`/`BlockMoveData`, scalar `CopyBits`/blit paths not
already covered by gfxaccel, possibly sound mixing.

**Risk**: per-routine semantic fidelity — an HLE patch must match the OS exactly
or it silently corrupts. This is why it's gated on measurement, not done
speculatively. It does **not** mean reimplementing the Toolbox (that's the
Executor/WINE model — a different project, and a compatibility *loss*); it means
patching a handful of bulk-data primitives.

---

## Measurement Plan

### P0. Build the mix-aware execution profiler FIRST (prerequisite)

The priorities below are currently estimated from *compile frequency* (how often a
block is compiled), which is biased — a block compiled once but executed a million
times is what actually matters. Before investing in any 5–15%-estimate item, build
an **execution-weighted, instruction-mix-tagged** hot-block profiler. It is
incremental on infrastructure that already exists (block cache keyed by PC, the
HOT-PC heartbeat sampler, the per-region NK/DR/RAM/OTH counters):

1. Per-block execution counter (or accumulate sampled guest PCs into a histogram);
   dump the top-N hottest blocks **with their disassembly** (the JIT already has it).
2. Tag each hot block by instruction mix — the JIT sees every opcode at compile
   time, so it can cheaply mark a block `scalar-bulk-data` / `AltiVec` / `integer-ALU`.
3. Attribute hot PCs to named routines via the trap-dispatch / NameRegistry ranges.

This one artifact serves *both* tracks: it tells you which JIT-internals item
(0g / P8 / P9) to do first **and** which routines clear the HLE gate above. It
converts the rest of this plan from priors into evidence.



For each optimization, measure:
1. **Harness**: 235/235 today, 236/236 once the P1a `lmw r20` vector lands;
   score=100 (correctness gate)
2. **Boot test**: HD boot to Finder desktop (no regression)
3. **Speedometer 4.02**: Full benchmark, compare PR/Mix/CPU/Dhrystones
4. **SS_JIT_VERIFY=1**: Differential verification (mandatory for RA/CR0 changes)
5. **Heartbeat**: blocks/s, blocks compiled, j2i transitions

## Upstream References

- [PERFORMANCE_AUDIT.md](https://github.com/rcarmo/macemu-jit/blob/master/PERFORMANCE_AUDIT.md) — BasiliskII/RPi optimization audit (14/27 implemented)
- [JIT-FPU-PLAN.md](https://github.com/rcarmo/macemu-jit/blob/master/JIT-FPU-PLAN.md) — 68K FPU JIT plan (shadow register pattern, validates our RA approach)
- Key warnings: LTO must stay disabled on macOS ARM64; do NOT remove PIE/stack-protector
