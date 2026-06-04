# SheepShaver ARM64 JIT Optimization Plan

## Current Baseline (2026-06-03, post-RA)

| Metric | Pre-RA | Post-RA | Delta |
|--------|--------|---------|-------|
| Benchmark Mix | 548.6 | **634.3** | **+15.6%** |
| Dhrystones/sec | 1,348K | **1,475K** | **+9.4%** |
| CPU score | 62.7 | **64.2** | +2.2% |
| Math score | 11,539 | **12,354** | +7.1% |
| Harness | 235/235 | 257/257 (2026-06-04) | +FP, AltiVec, carry-wrap vectors |

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

### AltiVec vsel fix — DONE (2026-06-04)

**Result**: BSL operands were swapped — `vsel` computed `(vC & vA) | (vB & ~vC)`
instead of PPC's `(vB & vC) | (vA & ~vC)`.  One-token swap.  Would have silently
corrupted any AltiVec software using `vsel`.

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
`RA_NUM_REGS=8`, so eviction never fired. The harness proves block-exit flush for
small blocks; it said nothing about mid-block spill. P8 (cross-block pinning)
extends the RA's cross-block lifetime, so this must be verified before relying
on it.

**Effort**: Low (~1 hour)
**Risk**: This *closes* risk rather than adding it.

1. **Add a >8-live-GPR eviction vector.** ✅ **DONE** — `lmw_stmw_wide` (li
   r20–r31, stmw, zero, lmw r20: 12 GPRs > `RA_NUM_REGS=8`) is in the harness and
   forces mid-block `ra_evict` of both clean and dirty slots. It **passes under
   `make test-jit` (score=100; `make harness-count`)**. CAVEAT: it only validates the JIT spill path in
   **jit mode** (`SS_HARNESS_MODE=jit`); the default `make test-opcodes` runs
   interpreter-determinism and does not exercise it.
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
   ✅ The inline COHERENCE WARNING comment (the "minimum") is **done** in
   ppc-jit.cpp — including the trap that the obvious fix (reusing
   `emit_load_gpr`/`emit_store_gpr`) corrupts the high word because `a64_mov_reg`
   is a 64-bit move. The actual RA-routing fix is **deferred** until a G5/PPC64
   path makes it reachable *and* `SS_JIT_VERIFY`-testable.
4. **Pin the real invariant** (comment near `RA_NUM_REGS`): the allocator is safe
   because **`RA_NUM_REGS` (8) ≥ simultaneously-live RA operands in a single
   *emitted* instruction (≤3, e.g. `ADD hD,hA,hB`)** — NOT "distinct GPRs per
   opcode." `lmw` touches 12 GPRs and is still safe precisely because only one
   RA reg (`hR`) is live per emitted op; the address and loaded value ride in
   `RTMP0`/`RTMP1`.

### P1b. AltiVec element-order correctness (ev_mixed) — PARTIALLY FIXED (2026-06-04)

**Status**: real correctness bug, partially fixed; the rest is parked + signposted
in code (`emit_load_vr` in ppc-jit.cpp) and CHANGELOG.

VRs are stored in the interpreter's `ev_mixed` byte order (bytes reversed within
each 32-bit word, word order preserved — `ppc-operands.hpp` `byte_element`/
`half_element`). `emit_load_vr` loads that raw via `LDR Q`, so any op that selects
or rearranges sub-word elements with raw NEON lanes is wrong.
- **Fixed**: `vspltb`/`vsplth` (remap the DUP index through `byte_element`/
  `half_element`). `vspltw`/`vsldoi`/element-symmetric ops were already correct.
- **Still broken** (repro vectors in `jit-test/gen-altivec-vectors.py`): `vmrgh*`/
  `vmrgl*` merges, `vpk*` packs, even/odd multiplies (`vmulo*`/`vmule*` — these
  also emit the *wrong* NEON op, e.g. vmuloub emits `MUL.8B` not `UMULL.8H`).

**Two fix approaches** (neither done; both need a boot to verify real AltiVec
software, e.g. a LAME MP3 encoder under `SS_JIT_VERIFY=1` — see TESTING.md):
1. **Systematic**: `emit_load_vr` = `LDR Q`+`REV32.16B`, `emit_store_vr` =
   `REV32.16B`+`STR Q`, so NEON sees natural order and every op uses raw lanes
   (then revert the splat remap). Simplest, but +2 NEON ops per AltiVec op (perf
   hit) and must re-verify every op + lvx/stvx interop.
2. **Per-op**: make each broken op's codegen ev_mixed-aware (like the splat remap).
   Perf-neutral, but a careful derivation per op; multiplies also need UMULL/UMULL2
   + a deinterleave.
**Effort**: medium-high. **Reachability**: AltiVec is live (emulator advertises a
G4), so this corrupts real AltiVec software today.

---

## Open — Quick Wins (Priority 0)

### 0b. subfe/adde via ADCS — DONE (2026-06-03, correctness + perf)

**Result:** Fixed two verified carry-out bugs (backlog A1/A2) and reduced
adde from 10→4 instructions, subfe from 11→5.  Materialize CA into host C
flag with `CMP W(CA),#1`, then `ADCS` computes the full three-operand sum
with correct carry-out.  Test vectors `adde_carry_wrap` and
`subfe_carry_wrap` added.  `SS_JIT_VERIFY=1` boot clean.

### 0b-extra. Fix mullwo — DONE (2026-06-04, correctness)

**Result:** Case label was 715 (wrong XO); correct is 747.  The instruction
was never JIT-compiled — silently fell to interpreter.  Now uses SMULL +
ASR/CMP for overflow detection, sets XER OV/SO.  Test vectors added.

### 0b-extra2. `emit_update_cr0` cleanup — DONE (2026-06-04, B1)

**Result:** 19→11 ARM64 instructions.  3x CSET + 3x shifted ADD replaces
3x emit_load_imm32 + 3x CSEL.  BFI replaces LSL + AND + ORR for CR0 merge.
Uses ADD (not ADDS) to preserve NZCV from the initial CMP.

### 0b-extra3. LogicalImm encoder — DONE (2026-06-04, B2)

**Result:** `emit_and_imm32()` helper tries ARM64 bitmask-immediate encoding
(1 insn) before falling back to emit_load_imm32 + AND (2-3 insns).  Applied
to all 5 AND-mask sites: rlwinm, rlwimi (both mask and ~mask), rlwnm, andi.,
andis.  992/1024 PPC masks are encodable.  Encoder from `ppc-logical-imm.hpp`.

### 0c. isync: inline BLR instead of block break

**Expected impact**: Minor per instance, but isync appears after every
mtspr/mtmsr in the ROM toolbox.
**Effort**: Low-medium

Current: `return false` → block terminates, full prologue/epilogue overhead.
Better: emit inline `BLR` to `execute_invalidate_cache_range()` stub, continue
block.  Each block break costs ~12 instructions of overhead.

### 0d. Atomic spcflags — DONE (2026-06-03)

**Result**: CPU 65.2 (new high).  Replaced spinlock-based `basic_spcflags`
with `std::atomic<uint32>` using `fetch_or`/`fetch_and` + relaxed/release
ordering.  Eliminates lock contention between the 60 Hz VBL timer thread
and the JIT dispatch loop's per-block spcflags poll.

### 0e. Computed-goto Interpreter Dispatch (from upstream PERFORMANCE_AUDIT)

**Expected impact**: 20-40% on interpreter fallback paths
**Effort**: Medium
**Risk**: Low

For the interpreter fallback path (j2i transitions, inline interpreter calls),
a computed-goto dispatch loop eliminates the switch/case overhead.  Matters
because several instructions still fall through to the interpreter (bcctr,
lwarx, stwcx., mftb, icbi, isync).

### 0f. Eliminate trailing MOV in carry/div/OE ops — DONE (2026-06-03)

**Result**: 11 ops converted (subfc, addc, addco, subfco, addo, subfo, nego,
addze, subfze, addic, addic., subfic).  Mix 638 (new high).  Saves 1 MOV per
instruction by computing ADDS/SUBS directly into the RA destination register.
Safe because `emit_write_xer_ca_from_carry` and `emit_write_xer_ov_so_from_overflow`
read NZCV via CSET without clobbering it or RTMP0.
**Effort**: Low
**Risk**: Low (flag-read ordering preserved — verified by inspection)

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

### P2: Native bcctr — HIGHEST MISS COUNT but COMPLEX (98.5% of JIT misses)

**Expected impact**: Unknown — miss count is compile-time, not runtime-weighted.
bcctr is a block terminator either way; the win is eliminating the dispatcher
round-trip.
**Effort**: HIGH (not medium — see below)
**Risk**: Medium-high

**Miss data (2026-06-03):** opc=19 accounts for 44,667 of 45,332 total misses
(98.5%). This is almost entirely `bcctr` (XO19=528) and `isync` (XO19=150).

**Attempted 2026-06-03:** Unconditional `bctr` with bit-0 Mixed Mode guard
(same TBZ pattern as bclr) — **stalled during extension loading.**
`SS_JIT_VERIFY=1` showed the interpreter's `execute_bcctr` does far more than
`PC = CTR`: it handles Mixed Mode Manager transitions (CallUniversalProc) that
modify GPR0, GPR2, GPR12, LR, CTR, and CR.  A simple TBZ guard cannot
replicate this.

**Path forward:** native bcctr requires either:
1. Emitting an inline call to the interpreter's CallUniversalProc handler
   when CTR points to a Mixed Mode routine (more than just bit 0 — needs
   the full routine descriptor check), OR
2. Restricting native bctr to ROM-only blocks where CTR targets are known
   to be PPC code (not Mixed Mode), OR
3. Understanding the Mixed Mode dispatch deeply enough to emit the full
   transition inline.

This is no longer a "re-enable" — it's new work requiring Mixed Mode Manager
reverse engineering.

### P3: Reduce Interpreter Fallbacks

**Expected impact**: 5-10% (fewer block breaks = longer native runs)
**Effort**: Medium per instruction
**Risk**: Low (each can be independently tested)

Currently falling through to interpreter:
- `lwarx`/`stwcx.` (XO=20/150): Need reservation state from CPU object
- `mftb` (XO=371): Need the interpreter's time-base model
- `icbi` (XO=982): Could emit inline call to invalidate JIT blocks
- `isync` (XO19=150): See P0c above

### P4: Code Cache Sizing — DONE (2026-06-03)

**Result**: Default increased from 64 MB to 256 MB.  Configurable via
`jitcachesize` pref (accepts K/M/G suffixes) or `SS_JIT_CACHE_KB` env var.
MAP_JIT memory is virtual — no physical cost until touched.  Eliminates the
2+ full flushes per session that caused recompilation churn.

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

### P5b: FP Register Allocator

**Expected impact**: HIGH — jit-bench shows FP ops at 2.84 ns/insn vs integer
ALU at 0.095 ns/insn (30x gap).  Every FP instruction reloads/stores FPRs
through the `powerpc_registers` struct because there is no FP register cache.
An FP RA mapping PPC FPRs to ARM64 d8-d15 (callee-saved) would eliminate
this round-trip.
**Effort**: High (new RA for FPR space, flush discipline, FP block exit)
**Risk**: Medium
**Measured by**: jit-bench `fp-add` / `fp-fma` kernels (2.84 ns/insn baseline)

### P5c: AltiVec ev_mixed Element-Order Fixes

**Status**: Partially done (2026-06-04).  `vspltb`/`vsplth` fixed by remapping
the DUP index through the interpreter's `ev_mixed` byte order.  `vspltw` was
already correct (word order preserved).

**Remaining**: `vmrghb`/`vmrglb`/`vmrghw`/`vmrglw` (merges), `vpkuhum` (pack),
and `vmuloub`/`vmuleub` (even/odd multiplies — these have an ADDITIONAL bug:
`MUL.8B` instead of `UMULL.8H`).  Systematic fix option: `emit_load_vr`/
`emit_store_vr` do `REV32.16B` to convert ev_mixed↔natural lane order, at the
cost of 2 extra NEON ops per AltiVec instruction.  Repro vectors in
`gen-altivec-vectors.py`.
**Effort**: Medium.  **Risk**: Medium (must re-verify every AltiVec op + boot).

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

## Open — From Emulator Research (2026-06-03, Dolphin/RPCS3/Box64/FEX survey)

### Quick Wins (1-2 days each)

**R1. Software link stack for blr prediction** (Dolphin/RPCS3) — PARTIAL (2026-06-04)
Compile-time link stack infrastructure added (bl pushes, blr pops+compares).
**Finding:** bl is always a block terminator, so the compile-time stack is empty
by the time the callee's blr compiles.  The fast-path never fires.

**R1b. Runtime link stack** (follow-up to R1)
The Dolphin/RPCS3 approach uses a **runtime** stack: `bl` emits ARM64 instructions
that push the return PC onto a small stack in the regs struct; `blr` emits
instructions that pop+compare and branch directly on match.  This persists across
block boundaries (the stack lives in memory, not compile-time state).  Requires:
1. Add `link_stack[8]` + `link_stack_top` fields to the regs struct (or a global)
2. At `bl`: emit `LDR W(top), [RSTATE, #LS_TOP]; STR W(pc+4), [RSTATE, #LS_BASE + top*4]; ADD top, top, #1; STR W(top), [RSTATE, #LS_TOP]` (~4 insns)
3. At unconditional `blr`: emit `LDR W(top), [RSTATE, #LS_TOP]; SUB top, top, #1; LDR W(pred), [RSTATE, #LS_BASE + top*4]; CMP W(LR), W(pred); B.NE miss; B chain_entry` (~6 insns)
4. Miss path: standard store-PC-and-return-to-dispatcher (existing code)
Effort: ~1 day.  Risk: low (miss fallback = current behavior).
**Source:** Dolphin `JitArm64_Branch.cpp` return address stack; RPCS3 SPU `spu_runtime`.

**R2. Inline direct-mapped cache at indirect branch sites** (Dolphin JitArm64)
Emit 2-instruction probe (load cached PC, compare) before falling back to hash
dispatcher.  Removes full hash lookup for repeated indirect targets (bctr).
Effort: ~1 day.  Risk: low (miss path is current behavior).

**R3. Batch W^X toggles** — N/A (2026-06-04)
Investigated: SheepShaver's code cache uses plain `mmap(PROT_RWX)` without
`MAP_JIT` or `pthread_jit_write_protect_np`.  No W^X toggling exists to batch.
Only relevant if the project migrates to MAP_JIT for hardened distribution.

**R4. mach_absolute_time for event scheduling** (Dolphin)
Replace gettimeofday/clock_gettime with direct CNTVCT_EL0 reads via
`mach_absolute_time`.  Eliminates syscall overhead in timing paths.
Effort: <1 day.  Risk: none.

### Medium Effort (3-5 days)

**R5. Deferred CR0 via native NZCV** (Box64 NativeFlags + Dolphin CR fastpath)
Keep Rc=1 results in ARM64 NZCV flags with a dirty bit per CR field; only
materialize when a branch/mfcr reads it.  Eliminates 11-14 instructions per
Rc=1 op.  This is the "lazy CR0" problem we hit — Box64's approach of tracking
which instructions clobber NZCV may be the solution.
Effort: 3 days.  Risk: moderate (CR touched everywhere).

**R6. Flat dispatch table replacing hash** (RPCS3 `vm::g_exec_addr`)
PC-indexed array of block pointers covering ROM+RAM; indirect branches resolve
with a single indexed load.  O(1) dispatch vs hash probe chain.
Effort: 2-3 days.  Risk: moderate (memory footprint ~32 MB; must handle invalidation).

**R7. Pin hot GPRs across blocks** (RPCS3 GHC convention, FEX-Emu)
Extend register pinning beyond r1/r2 to r3-r10 (args), r13 (SDA) using all
x19-x28 callee-saved slots.  Fewer loads/stores at block boundaries.
This is our existing P8 — validated by RPCS3's approach.
Effort: 3 days.  Risk: moderate.

### Strategic (1-2 weeks, highest payoff)

**R8. Dual W^X mapping** (Dolphin/RPCS3 ARM64 macOS)
`mmap` MAP_JIT code cache, create second RW mapping of same physical pages.
Emit through RW alias, execute through RX alias, never toggle again.
Our research doc c4 measured ~27% compilation speedup.
Effort: 1 week.  Risk: higher (macOS VM behavior, spike exists in `spikes/wx-dual-mapping/`).

**R9. Async background JIT compilation** (Dolphin tiered JIT, Cemu)
Interpret or run baseline blocks while a background thread compiles optimized
native code.  Use GCD QoS to pin compilation to E-cores.
Our research doc c5 has the feasibility analysis and thread-safety audit.
Effort: 1-2 weeks.  Risk: high (thread safety for block cache).

**R10. Metal framebuffer compositing** (Dolphin Metal backend)
Upload guest framebuffer via `MTLTexture.replace` + fullscreen quad instead of
CPU-side SDL pixel conversion.  Frees CPU cycles from format conversion.
Effort: 1 week.  Risk: moderate (new rendering path alongside SDL).

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

### P0b. Microbenchmark harness — DONE (2026-06-03)

**Result**: `jit-bench` shipped as `rom-harness --bench` / `make bench`.
Differential per-instruction timing (compile each kernel at 16 and 144 body
instrs; `(call_big−call_small)/(144−16)` cancels prologue/epilogue), min-of-5,
**<1% run-to-run noise**. Discriminating on first run: `alu` 0.10 < `rc1` 0.50 <
`carry-chain` 0.95 ns/insn. `--save-baseline`/`--compare` A/B; per the TESTING.md
contract, `--compare` warns if the baseline predates `ppc-jit.cpp`. Kernels:
`carry-chain` (0b/0f), `rc1` (0g lazy-CR0), `alu` (RA).

Surfaced + fixed two standalone-harness bugs: a missing `ppc_jit_interp_one`
link stub, and a **missing `spcflags` field in `PPCRegs`** (the 0d atomic change
moved it to offset 1056) that made the JIT entry poll read past the struct and
bail before every block body — silently breaking rom-harness's normal mode too.
A `static_assert` now turns that drift into a compile error.

**Deferred to v2**: a `load-store` kernel (guest data access needs the
DIRECT_ADDRESSING base configured — macOS can't map the low 4 GB), and a
`call-return` kernel for P9 (needs a multi-block driver — a single straight-line
block can't isolate the dispatcher round-trip).

**Caveat (still applies)**: microbenches can mislead (you optimize the bench, not
the workload). Treat deltas as directional and confirm headline wins with a real
Speedometer/boot run. Pairs with P0 (the profiler says *which* kernels are real
hot spots).

For each optimization, measure:
1. **JIT codegen gate**: `make test-jit` (`SS_HARNESS_MODE=jit`) — runs every
   vector through the interpreter AND the JIT and diffs them. **score=100** (count: `make harness-count`). This is the gate that actually exercises codegen; run
   it on every `ppc-jit.cpp` change. NOTE: plain `make test-opcodes` only checks
   interpreter determinism and proves nothing about the JIT — never cite a bare pass count
   without the mode.
2. **Boot test**: HD boot to Finder desktop (no regression)
3. **Speedometer 4.02**: Full benchmark, compare PR/Mix/CPU/Dhrystones
4. **SS_JIT_VERIFY=1**: Differential verification (mandatory for RA/CR0 changes)
5. **Heartbeat**: blocks/s, blocks compiled, j2i transitions

## Upstream References

- [PERFORMANCE_AUDIT.md](https://github.com/rcarmo/macemu-jit/blob/master/PERFORMANCE_AUDIT.md) — BasiliskII/RPi optimization audit (14/27 implemented)
- [JIT-FPU-PLAN.md](https://github.com/rcarmo/macemu-jit/blob/master/JIT-FPU-PLAN.md) — 68K FPU JIT plan (shadow register pattern, validates our RA approach)
- Key warnings: LTO must stay disabled on macOS ARM64; do NOT remove PIE/stack-protector
