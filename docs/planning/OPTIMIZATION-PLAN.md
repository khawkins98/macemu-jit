# SheepShaver ARM64 JIT Optimization Plan

> **Status:** 🟡 Active · **Created:** 2026-06-03 · **Updated:** 2026-06-06
> **Why this doc exists:** SheepShaver JIT performance roadmap — done / open / deferred levers with measured baselines.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../CONTRIBUTING.md) → "Documentation Lifecycle")._


## Current Baseline (2026-06-03, post-RA)

| Metric | Pre-RA | Post-RA | Delta |
|--------|--------|---------|-------|
| Benchmark Mix | 548.6 | **634.3** | **+15.6%** |
| Dhrystones/sec | 1,348K | **1,475K** | **+9.4%** |
| CPU score | 62.7 | **64.2** | +2.2% |
| Math score | 11,539 | **12,354** | +7.1% |
| Harness | 235/235 | **302/302** (2026-06-06; count via `make harness-count`) | +FP, AltiVec (ev_mixed merges/vpkuhum/byte mults; +variable shift/rotate family +12; +saturating add/sub & signed averages +14, all 2026-06-06), carry-wrap, mullwo |

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

### P1a. Harden the register allocator — ✅ SUBSTANTIALLY STRENGTHENED (2026-06-05); gate basis revised

**Outcome (2026-06-05):** the RA eviction path is validated on the evidence that is
*independent of the verify oracle* — (1) a targeted harness battery (`lmw_stmw_wide` +
`evict_wb16` / `evict_rd_eq_ra` / `evict_mixed_alu`) forces >8-live `ra_evict` and passes the
JIT-vs-interp REGDUMP (the harness runs interp and JIT as **separate executions from clean
state**, so it has none of the oracle's replay confounds), and (2) a full chaining-on boot
reaches the Finder desktop, where eviction fires continuously and any corruption would crash
the boot. `SS_JIT_VERIFY` boots are **corroboration** (66450 RAM blocks checked, zero *real*
divergences after accounting for 6 oracle-artifact classes), not the proof. A *literal*
whole-boot clean verify gate was found to be **unachievable on this config** (see step 2) —
that's an oracle-tooling limitation (→ ROADMAP A1 / X1), not RA debt. P8 is unblocked.

**Calibration — "substantially strengthened," not "every eviction edge proven."** The targeted
(confound-free) harness coverage started strong for **mid-block, pure-register, single-exit,
Rc=0** eviction, and the two named eviction-adjacent surfaces now have **targeted vectors too**
(2026-06-05) — both confirmed fully JIT-compiled, no interp fallback, hand-checked REGDUMP:
- **(a) Eviction at control-flow exits/terminators** — ✅ `evict_branch_exit`: 16-live pressure
  with dirty r3–r10, then a conditional `bc` that **terminates the block** (verified `blocks=2`),
  forcing `ra_flush_all` of the dirty slots on the branch edge; the branch condition reads an
  evicted reg, so a bad flush also missteers it. (Still design-level only: the `bclr` Mixed-Mode
  bail path specifically — its `ra_flush_all()` is audited, not yet harness-targeted.)
- **(b) Eviction × deferred state** — ✅ `evict_rc1_cr0` (Rc=1 `add.` → `emit_update_cr0` under
  pressure, CR captured) and ✅ `evict_adde_carry` (`addic.` + `adde` chain → XER carry under
  pressure, XER/CR captured). (Lazy CR0 is dormant — disabled/eager, §0g — so this is the *eager*
  CR/XER surface.)

  **Still not exhaustive** (deliberately): not covered by a targeted vector — eviction across an
  interp-fallback boundary or spcflags poll, FP/VR-register interactions, the `bclr` bail path,
  and higher live-counts/clean-dirty interleavings. The boot gives functional assurance for these;
  more vectors are the cheap no-boot lever if a specific one becomes load-bearing.

**Why first**: P1 shipped a real +15.6%, but its subtlest path — `ra_evict`
under register pressure (>8 live GPRs in one block) — is **never exercised by
the harness**. The `lmw`/`stmw` vectors top out at 4 registers (r28–r31), under
`RA_NUM_REGS=8`, so eviction never fired. The harness proves block-exit flush for
small blocks; it said nothing about mid-block spill. P8 (cross-block pinning)
extends the RA's cross-block lifetime, so this must be verified before relying
on it.

**Effort**: Low (~1 hour)
**Risk**: This *closes* risk rather than adding it.

1. **Add >8-live-GPR eviction vectors.** ✅ **DONE + BROADENED (2026-06-05)** —
   `lmw_stmw_wide` (li r20–r31, stmw, zero, lmw r20: 12 GPRs) was the first; a
   **pure-register eviction battery** now widens coverage beyond load/store-multiple:
   `evict_wb16` (write r3–r18, then add early+late so r3–r10 are dirty-evicted then
   reloaded — spill/reload of dirty slots), `evict_rd_eq_ra` (`add rN,rN,rN` on
   evicted regs — the ra_load-before-ra_store ordering rule under pressure), and
   `evict_mixed_alu` (add/subf/and/or/xor variety). Each uses 16 distinct GPRs >
   `RA_NUM_REGS=8` in one straight-line block (capstone-verified encodings; no
   memory/chaining/loop confound). All pass `make test-jit` (**267/267, score=100**),
   and `evict_wb16` was confirmed **fully JIT-compiled** (`complete=1`, 360 bytes
   native, hit=24 miss=0 — not an interpreter fallback) with a hand-checked REGDUMP.
   CAVEAT: validates the JIT spill path in **jit mode** (`SS_HARNESS_MODE=jit`); the
   default `make test-opcodes` runs interpreter-determinism and does not exercise it.
2. ✅ **DONE (as corroboration) — `SS_JIT_VERIFY=1` boots, 2026-06-05.** Three boots
   (chaining-on; `SS_JIT_NO_CHAIN=1`; `SS_JIT_NO_CHAIN=1` + `SS_JIT_VERIFY_BUDGET=100000`
   for ~92k lines of coverage) found **zero real codegen divergences** — every reported
   divergence is one of **six oracle-artifact classes** (the differential oracle is *not*
   a clean gate; see 0b-extra4 for the full taxonomy + the budget/timing trap). The
   "exactly one `blr` residual" figure from before was itself a **latch artifact** (0b-extra5):
   once the latch was fixed the oracle revealed ~12 chaining-class artifacts, and the deep run
   surfaced a memory-RMW artifact (`100fd0e0`) that even passes a naive PC-match filter. None
   is an RA bug. **A literal whole-boot clean verify is unachievable here:** a low report
   budget makes the oracle go dark at ~22% of boot, and a high budget makes verify-every-block
   so slow it starves the guest timer into an early-boot ROM spin (never reaches Finder). So
   closure rests on the oracle-*independent* evidence above, not on this boot. Steps 1 and 4
   are done; step 3 stays parked (unreachable on a 32-bit guest).
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
4. ✅ **DONE — Pin the real invariant** (comment at `ppc-jit.cpp` `RA_NUM_REGS`): the allocator is safe
   because **`RA_NUM_REGS` (8) ≥ simultaneously-live RA operands in a single
   *emitted* instruction (≤3, e.g. `ADD hD,hA,hB`)** — NOT "distinct GPRs per
   opcode." `lmw` touches 12 GPRs and is still safe precisely because only one
   RA reg (`hR`) is live per emitted op; the address and loaded value ride in
   `RTMP0`/`RTMP1`.

### P1b. AltiVec element-order correctness (ev_mixed) — TESTED CLASS FIXED (2026-06-04)

**Status**: real correctness bug; the entire *tested* element-order class is fixed +
promoted (splats, merges, pack `vpkuhum`, byte multiplies `vmulo/eub`) — harness 264/100,
quarantine empty. Boot-pending for pack+multiplies (new NEON ops); real-AltiVec validation
→ A3. Untested siblings remain (halfword multiplies, `vpkuwum`, signed byte multiplies) —
signposted in code + ROADMAP A2. Live tracker: ROADMAP A2.

VRs are stored in the interpreter's `ev_mixed` byte order (bytes reversed within
each 32-bit word, word order preserved — `ppc-operands.hpp` `byte_element`/
`half_element`). `emit_load_vr` loads that raw via `LDR Q`, so any op that selects
or rearranges sub-word elements with raw NEON lanes is wrong.
- ✅ **Fixed**: `vspltb`/`vsplth` (remap the DUP index through `byte_element`/
  `half_element`). `vspltw`/`vsldoi`/element-symmetric ops were already correct.
- ✅ **Fixed — merges `vmrgh/l {b,h,w}`** (2026-06-04, boot-verified, promoted to
  the scored gate 255→261): the encodings were garbage (`0x..C400`, not ZIP);
  corrected to `ZIP1`/`ZIP2`. Words are a plain `ZIP.4S` (word_element identity);
  byte/halfword use the **per-op `REV32.16B` normalize** (`emit_vmrg`): `REV32.16B`
  both inputs → `ZIP1`/`ZIP2.{16B,8H}` → `REV32.16B` back. The `ev_mixed` layout is
  exactly `REV32.16B` at the byte level vs natural element order.
- ✅ **Fixed — `vpkuhum`** (2026-06-04, boot-verified): pack low bytes = `UZP2.16B` on the
  `REV32.16B`-normalized inputs (reuses `emit_vmrg`). It had ignored vA entirely. Promoted
  261→262.
- ✅ **Fixed — byte multiplies `vmuloub`/`vmuleub`** (2026-06-04, boot-verified): two bugs —
  non-widening `MUL.8B` and no ev_mixed even/odd select. `emit_vmul_byte`: REV32.16B → UZP1
  (even)/UZP2(odd).16B → UMULL.8H → REV32.8H. Promoted 262→264; quarantine now empty.
- 🟡 **Still broken / untested** (no test vector): halfword multiplies `vmul{o,e}{u,s}h` (need the
  hw→word analogue: UZP on `.8H` + `[SU]MULL.4S`, word_element identity so no output rev), the
  word pack `vpkuwum` (ignores vA), and signed byte multiplies `vmulosb`/`vmulesb` (share
  `emit_vmul_byte` w/ SMULL — emitted as *prospective*, no signed test vector).

**Fix approach — settled: per-op (approach B).** Make each op's codegen ev_mixed-aware
locally (the merges proved it: a *local* `REV32.16B` normalize works). The *global*
load/store `REV32.16B` variant (old approach 1) was **empirically ruled out** — it
changes the in-JIT VR convention for every op at once and re-breaks correct ones
(ROADMAP A2). All *tested* ops are done; remaining per-op work is the untested siblings
(halfword multiplies need UZP on `.8H` + `[SU]MULL.4S`; `vpkuwum`; signed byte mults need a
signed test vector). Final sign-off still needs a real-AltiVec boot under `SS_JIT_VERIFY=1`
(e.g. a LAME MP3 encoder — see TESTING.md / ROADMAP A3).
**Effort**: medium (per remaining op). **Reachability**: AltiVec is live (emulator
advertises a G4), so the remaining ops corrupt real AltiVec software today.

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

### 0b-extra4. SS_JIT_VERIFY oracle confound taxonomy — 5/6 classes FIXED (2026-06-05); class 6 (memory RMW) open

The differential oracle re-runs the interpreter for one block and diffs it against the JIT's
post-block state. The P1a whole-boot sweeps (2026-06-05) showed its divergences are *all* false
positives in **six structural classes** (none a codegen bug). The root causes were two design
shortcuts in the replay (`ppc-cpu.cpp`): the interp re-run **(a)** stopped on "PC left
`[start,end)`" instead of mirroring the JIT block's actual path/exit, and **(b)** restores
*registers only — never guest memory* — so the JIT's stores are still live when the interp
replays. **Fix (i) (commit `5ac5e676`) addressed (a)**; **(b) is the remaining work (fix ii)**.

| # | Class | Tell | Status |
|---|-------|------|--------|
| 1 | **Block chaining** | `jit_state` is end-of-chain (e.g. `li r4,1` → jit r4=0x80); jit PC in ROM dispatcher | ✅ workaround: `SS_JIT_NO_CHAIN=1` (now warns if unset) |
| 2 | **blr/bclr return** | `GPR1` off by the in-block `addi r1,r1,N`; `LR`/`PC` = block-addr vs return-addr | ✅ **fixed (i)** — replay stops at the JIT's real exit |
| 3 | **Intra-block loop** | exact off-by-one-iteration; jit PC = loop-back target | ✅ **fixed (i)** — replay stops at the first `bc`/branch (one iteration) |
| 4 | **Mid-block conditional path** | PC mismatch + path-dependent regs | ✅ **fixed (i)** — replay stops at the `bc` (opcode 16), both arms |
| 5 | **PC bookkeeping** | PC-only divergence, zero data | ✅ **fixed (i)** |
| 6 | **Memory RMW (no restore)** | **PC matches**, one GPR off by a constant; value **steps by 2** across visits | 🟡 **open (fix ii)** — `100fd0e0`/`10106b50`/`1011e734`/`1018b04c` |

**Fix (i) landed (2026-06-05, `5ac5e676`):** the replay now mirrors the JIT's single-block
control flow — execute each insn, stop when PC left the sequential path (`pc != cur+4`) OR the
insn was a `bc` (opcode 16; **both** arms `emit_epilogue_with_pc` to the dispatcher, so `bc`
ends the block taken-or-not, and the compiler emits `n_insns`-inflating dead code past it).
Plus a no-op-skip guard (`memcmp(jit_state,pre_state)` for the spcflags-poll bail) and a
`SS_JIT_NO_CHAIN` warning. Boot-validated: classes 2/4/5 (and 3) are **clean boot-wide**
(ARTIFACT-PC count 0). A **per-block report dedup** (`7314bc9c`) stops a memory-RMW block from
consuming the whole budget. **Only class 6 remains.**

**Class 6 is the dangerous one** — it passes a naive "PC-match ⇒ real bug" filter. Discriminator:
a *real* missing-`addi` bug steps the counter by 1; the double-apply artifact steps by **2**
(JIT store + replay store), proving the JIT op executed. The rigorous filter is per-record
(a record with a GPR line but no PC line), not block-level set difference.

**The proper fix is the X1 verify/bisection tooling (ROADMAP A1):** ✅ make the replay mirror the
JIT block's path and stop at its real terminator (kills 2/3/4/5) — **done, fix (i)**; 🟡
snapshot+restore the touched guest memory around the replay (kills 6) — **fix (ii), open** (the
design under review is an interp-first reorder + RAM-write journal, gated entirely behind
`SS_JIT_VERIFY` so normal boots are untouched). Also note the **budget/timing trap**: the
report budget gates *entry*, so a low budget makes the oracle go dark mid-boot, while a high
budget (`SS_JIT_VERIFY_BUDGET`) makes verify-every-block starve the guest timer into an
early-boot ROM spin — neither reaches Finder with full coverage. A targeted/sampled verify
(only register-pressure blocks) is the way around it. **Do not gate P1a on this** — P1a closed
on oracle-independent evidence (see P1a outcome).
**Effort**: Medium (replay rework + memory snapshot). **Risk**: Low (tooling only).

#### Fix (ii) — approach + decision (documented 2026-06-06; DEFERRED, low value)

**Decision:** designed + review-vetted, **not implemented — deferred as low-value.** The broad
verify sweep (2026-06-05) found the four residual `SUSPECT` blocks are *all* memory-RMW readback
artifacts and **zero real codegen bugs hide behind them**, so fix (ii) buys oracle *completeness*,
not bug-finding. It also touches the interpreter store path + dispatch ordering (real-boot blast
radius), so it needs an explicit greenlight before landing. Pick it up only if a future need wants
the verify oracle artifact-free (e.g. bisecting a real regression past early boot).

**Approach (turnkey when picked up — full spec:
`docs/superpowers/specs/2026-06-05-verify-memory-snapshot-fix-ii-design.md`):**
1. **Interp-first reorder.** Run the interp replay on the *pristine* pre-state before the JIT
   executes the block (snapshot regs, run interp, capture post-state, restore regs, run JIT,
   `memcmp`). Removes the read-after-JIT-write hazard for the register compare with no memory
   bookkeeping.
2. **RAM-write journal.** The interp replay now writes memory, so wrap `vm_write_memory_{1,2,4,8}`
   (the interp store chokepoint, `vm.hpp`) under a `verify_journal_active` flag: record
   `(addr,size,old_bytes)` before each store; after the replay, replay the journal in reverse to
   restore, then run the JIT. All gated behind `SS_JIT_VERIFY` so normal boots are untouched.
3. **Two landing-blocker must-fixes** (from review): (a) an `ends_in_fallback` bit on `jit_bc_entry`
   so the replay stops at a host-side fallback instead of over-running `n_insns`; (b) journal sized
   ≥ **16384** entries with **loud-abort-on-overflow** (a single `stmw` writes 32 words; a silent
   drop leaves guest RAM corrupted — the exact failure (ii) exists to prevent).
4. **Caveat:** the oracle stays **register-only** even after (ii) — it removes the memory-RMW
   *register* false-positives; it does not compare the JIT-vs-interp *memory* writes themselves.

**Verify sweep result (2026-06-05, `SS_JIT_VERIFY=1 SS_JIT_NO_CHAIN=1`, HD boot) — honestly
scoped:** with fix (i) + dedup, the oracle reports **exactly four `SUSPECT` blocks**
(`100fd0e0`/`10106b50`/`1011e734`/`1018b04c`) and four `ARTIFACT-PC` blocks. **All four
`SUSPECT`s are class-6 memory-RMW**: three are *encoding-certified* (load+store same base reg &
offset — `0(r8)`, `10(r31)`, `4(r3)`); `1018b04c` is most-likely (indexed `[r3+r4]` with the
`stwx` base reloaded → runtime-contingent, but the value-chain confirms artifact and the broken-
`lwzx` hypothesis is refuted). **No real codegen bug in the covered region — but NOT whole-boot.**
Under verify the guest **stalled in an early-boot spin ~10 s in** (`comp=10826` frozen 10 s→3 min;
only **8 distinct block PCs** ever verified, ≈0.07 % of compiled blocks; never reached Finder) —
the documented timer-starvation trap; "nothing new after that" = nothing executed, not
reassurance. **Deeper coverage is far cheaper via the offline rom-harness** (`make test-jit`,
`rom-harness --passes/--seed/--count`, no timer dependency) — prefer that over more boot sweeps.
Fix (ii) re-framed: **DEFER pending greenlight** — it is the tool that would *certify* the four
artifacts and unblock deeper boot-time verify, not mere polish. Ready-to-implement design (two
landing-blocker must-fixes + register-only caveat):
`docs/superpowers/specs/2026-06-05-verify-memory-snapshot-fix-ii-design.md`.

### 0b-extra5. SS_JIT_VERIFY suppression latch decay — DONE (2026-06-05)

**Concern (from colleague review, 2026-06-04):** the cascade suppression latch
(`verify_suppressed`) is cleared when a clean block is verified, but if no
verifiable block follows a divergence (e.g. the remaining blocks are all
link-call-terminated and skipped), suppression becomes permanent for the rest
of the boot — silently disabling the oracle.

**Result:** the latch was in fact *unconditionally* permanent, not just in the
edge case described — the `!verify_suppressed` entry gate guarded its own clearing
branch, so the `else { verify_suppressed = false }` was unreachable and the oracle
went silent after the **first** divergence of every run. Replaced the `bool` with a
`verify_suppress_blocks` countdown (`ppc-cpu.cpp`). Key correctness detail beyond the
proposed fix: the countdown is decremented **only on in-range (RAM, 0x10000000–
0x20000000) blocks** — execution is overwhelmingly ROM (0x50xxxxxx), so an
unconditional per-block decay (the originally-proposed `countdown=100`) would drain
during ROM before suppressing any RAM block, making it a no-op. Window is short
(N=2) and flagged as a tunable with a symptom guide in the code comment.
**Effort**: Low.  **Risk**: None (tooling-only; gated behind `SS_JIT_VERIFY=1`).
CHANGELOG 2026-06-05.

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
**divw follow-up — DONE (2026-06-06).** `divw` built its result in RTMP2 then MOV'd
to hD; the final CSEL now writes hD directly (commit on `p0-profiler`). Proven by
the deterministic `a64/op` microbench metric: the `compute` kernel dropped exactly
**−0.164 ARM64-insns/op** while all 5 untouched kernels read **+0.000** — a
zero-noise, host-independent measurement (the timing columns were all `NOISY` on
the loaded host at the time, which is precisely why the deterministic metric
matters). Correctness: test-jit 302/302 + differential interp-vs-JIT on the
div-by-0 / MIN÷-1 / normal paths.

**mulhw/mulhwu follow-up — DONE (2026-06-06).** Same pattern eliminated: the high
word now shifts directly into hD (`LSR X(hD), X0, #32` after SMULL/UMULL) instead
of `LSR RTMP0` + `MOV hD, RTMP0` — strictly −1 ARM64 insn/op (cannot regress).
Correctness: test-jit 302/302 + differential interp-vs-JIT on the signed/unsigned
high-word edge cases (−1×−1→0, 0x80000000×2→0xffffffff signed vs 1 unsigned, etc.).
Note: `mulhw`/`mulhwu` are **not** hot (absent from the boot + Speedometer profiles),
so this is a correctness-neutral tidy-up completing 0f — not a hot-path win. The 0f
family is now fully swept.

### 0h. rlwinm `slwi`/`srwi` → single ARM64 UBFM — DONE (2026-06-06)

`rlwinm` always emitted **EXTR (rotate) + AND (mask)** (2 insns). The two most common
forms are a single ARM64 bitfield op: `slwi rA,rS,n` (`mb==0 && me==31-sh`) → `LSL`
(UBFM), `srwi rA,rS,n` (`me==31 && mb==32-sh`) → `LSR` (UBFM). Added exact-condition
fast paths (the (SH,MB,ME) triple fully determines the op — no false positives), general
EXTR+AND path unchanged for the rest. **Hot, unlike 0f/divw's follow-ups**: the
`0x1ed6e310` matrix block alone has 4× `slwi` (array-index ×2/×4), and `0x10643c54` has
`slwi`/`clrlwi`. Validated: test-jit 302/302; **exhaustive differential interp-vs-JIT
248/248** (slwi+srwi × sh=1..31 × {0x80000001,0xFFFFFFFF,0x12345678,0x1}, CR0 via Rc=1);
deterministic A/B on a new `shift` microbench kernel — **a64/op 2.000 → 1.000 (−50%)**,
zero-noise (timing corroborated ~3× faster on that kernel).

**clrlwi/clrrwi extension — DONE (2026-06-06).** The `sh==0` masked forms now AND
**directly from rS** into rA instead of `mov rA,rS` + `AND rA,rA` — −1 insn when rs≠ra
(e.g. `clrlwi r0,r3,0x1e` in `0x10643c54`). Restructured the shared path (compute mask
first; `and_src = sh ? hA(rotated) : hS`; pure-`mr` full-mask case still emits one MOV).
Validated: test-jit 302/302; differential interp-vs-JIT — **248/248** clrlwi+clrrwi
(×31 widths × 4 edge values, CR0 via Rc=1) **plus** the shared rotate+mask paths
(general rotate, wrap-around mask, pure mr, rotlw full-mask) all match. Remaining
single-UBFM candidates (`extrwi`/`extlwi`) deferred — lower frequency.

### 0g. Lazy CR0 Re-enable

> **Cross-pollination (X2):** intra-block backward CR0-liveness — borrowed from BasiliskII's
> `needed_flags` analysis — is the *missing safety proof* for re-enabling this cleanly (elide
> CR0 writes never consumed before overwrite; no cross-block lazy state needed). See
> `docs/planning/sheepshaver-research/BASILISKII-CROSS-POLLINATION.md`.

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

> **Cross-pollination (X3):** the §R2 guarded inline direct-mapped cache is the *light*
> alternative — it sidesteps the Mixed-Mode-Manager RE that stalled the native-bcctr attempt,
> and is the form to prefer over BasiliskII's heavier edge-profiling. See
> `docs/planning/sheepshaver-research/BASILISKII-CROSS-POLLINATION.md`.

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
- `lwarx`/`stwcx.` (XO=20/150): Need reservation state from CPU object → **now P3a (top priority)**
- `mftb` (XO=371): Need the interpreter's time-base model
- `icbi` (XO=982): Could emit inline call to invalidate JIT blocks
- `isync` (XO19=150): See P0c above

### P3a: Native `lwarx`/`stwcx.` — DONE (2026-06-06); correctness/architecture win, perf-neutral

**What shipped** (commit on `p0-profiler`): `lwarx` (case 20) and `stwcx.` (case 150) now compile
natively instead of falling through to the interpreter, for the single-CPU model (`KPX_MAX_CPUS==1`),
matching `ppc-execute.cpp` exactly. Reservation state (`reserve_valid`/`reserve_addr`) lives in the
shared regs struct (RSTATE+offset, offsets pinned by `static_assert`s in `ppc-cpu.cpp`:
`reserve_valid==1060`, `reserve_addr==1064`), so a JIT-`lwarx` / interp-`stwcx.` pair stays consistent.
- `lwarx rD,rA,rB`: `EA=(rA|0)+rB; rD=mem[EA]; reserve_valid=1; reserve_addr=EA`.
- `stwcx. rS,rA,rB`: `EA=(rA|0)+rB; CR0=0; if(reserve_valid){ if(reserve_addr==EA){ mem[EA]=rS;
  CR0.EQ=1 } reserve_valid=0 } CR0.SO=XER.SO`. CR0 written directly (bits 31:28, nibble 8·LT+4·GT+2·EQ+SO).
- `sync` between them was already a NOP (correct single-threaded).

**Validation** — correctness proven three ways:
- `make test-jit` 302/302 (no regression).
- Differential interp-vs-JIT (`SS_TEST_HEX`), all paths byte-identical: success (`CR=0x20000000`
  EQ, store observed via `lwz` readback), fail-no-reservation (`CR=0`), fail-addr-mismatch (`CR=0`,
  no store). The isolated JIT run reports `hit=2 miss=0` — the fallback is gone.
- e2e smoke: boot → Finder → clean shutdown, exit 0 (the merged atomic loop with its backward `bne`
  progresses; the block-structure change is safe).

**Performance — neutral within run-to-run noise; NOT a measured speedup.** e2e-bench (Speedometer,
profiler off, n=3 each side, clean A/B): CPU 66.0→64.7, Graphics 42.8→42.9, Disk 9.34→9.57,
Math 12504→12996 — a Math-up/CPU-down split that is the signature of variance, not signal (the
distributions overlap, and P3a *cannot* mechanistically move the CPU/Math subtests: their hot path is
the `0x1ed` compute blocks, which contain no atomics). Boot-to-splash was 6.5s both before and after.
**Why neutral**: atomics are ~5–8% of *block-executions* but a small fraction of *instruction-time*
(2–4-insn atomic blocks vs 20–163-insn compute blocks), so removing the fallback is real but sits
below wall-clock noise. P3a earns its keep as **correctness + architecture** (completes a
known-incomplete fallback on the #1 concentrated hot block, removes the interp transitions), not as a
throughput lever. The throughput ceiling for compute workloads is large-block codegen (P1a/P5/P6/P8).

### P4: Code Cache Sizing — DONE (2026-06-03)

**Result**: Default increased from 64 MB to 256 MB.  Configurable via
`jitcachesize` pref (accepts K/M/G suffixes) or `SS_JIT_CACHE_KB` env var.
MAP_JIT memory is virtual — no physical cost until touched.  Eliminates the
2+ full flushes per session that caused recompilation churn.

**Bug fix (2026-06-04, colleague review #2):** the `jitcachesize` pref value
(bytes after K/M/G parsing) was passed to `SS_JIT_CACHE_KB` without dividing
by 1024 — `jitcachesize 256M` was interpreted as 256 TB.  Fixed with byte→KB
conversion + bounds (min 1 MB, max 1 GB).

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

Tracked as **P1b** in the "Open — Hardening" section above (single source of
truth). Summary: the entire *tested* ev_mixed class — `vspltb`/`vsplth`, the `vmrg*` merge
family, `vpkuhum`, and the byte multiplies `vmulo/eub` — is fixed (per-op normalize: ZIP/UZP
+ widening). Only untested siblings remain (halfword mults, `vpkuwum`, signed byte mults).

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

**R1c. Restore RAM LR-prediction (icbi-safe) — follow-up to the 2026-06-06 correctness fix.**
The LR-prediction direct-`B chain_code` had an icbi/SMC staleness bug (it wasn't a registered
chain_site, so `invalidate_range` couldn't revert it). Fixed (`72c5e525`) by restricting the
direct branch to **ROM** targets only — so RAM-return prediction is currently **disabled** (correct
but unoptimized). To re-enable it safely: register the LR-prediction `B` as a chain_site whose
**revert word branches to the miss/standard-dispatcher path** (NOT the default LDP epilogue — that
position would double the epilogue/corrupt the stack). Needs `revert_word` added to
`jit_chain_site` + `invalidate_range` writing `s->revert_word`. Validate: `make test-jit` + a boot
that exercises icbi on a RAM return target. (Also re-unblocks the persistent-JIT-cache idea —
CROSS-EMULATOR-IDEATION #10/#L.)

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
*Cross-pollination (X3):* this is the **light** implementation of the BasiliskII
"guarded indirect target" idea — preferred over its heavier edge-profiling + jmpdep
machinery; also the practical workaround for P2 (native bcctr). See
`docs/planning/sheepshaver-research/BASILISKII-CROSS-POLLINATION.md`.

**R3. Batch W^X toggles** — superseded by R8 (corrected 2026-06-06)
~~Investigated: SheepShaver's code cache uses plain `mmap(PROT_RWX)`...~~ **That note
was wrong.** The code cache *does* use `MAP_JIT` + per-thread `pthread_jit_write_protect_np`
toggling (`jit-target-cache.hpp:34` `JIT_CACHE_MAP_FLAGS = …|MAP_JIT`; `jit_cache_begin_write`/
`end_write` at :35-39; live calls at `ppc-jit.cpp:260/4457/4639`). So a real W^X toggle exists
and *could* be batched — but the better fix is **R8 (dual RW/RX mapping)**, which removes the
per-thread toggle entirely (and is required for background compilation, R9). See R8.

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

> **Concrete first target (2026-06-06):** `CopyBits` HLE — the hottest QuickDraw raster op, today
> run via the PPC-JIT emulating the ROM's 68K DR-emulator (the worst layer in the stack). Safe (no
> SMC risk, unlike `BlockMove`-for-code, which bypasses the icbi/isync flush path) and the on-ramp
> to Metal blits (R10). Histogram-probe first. Nominated alongside **idle-skipping** (a desktop-
> citizenship win) and constant-propagation / carry-via-NZCV as backlog levers. Full reality-checked
> idea bank + effort/payoff/blocked grid:
> `docs/planning/sheepshaver-research/CROSS-EMULATOR-IDEATION.md`.

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

### P0. Mix-aware execution profiler — ✅ DONE + boot-validated (2026-06-06)

**Built + gated behind `SS_JIT_PROFILE`** (zero codegen/runtime cost off — `make test-jit` 302/302).
Per-block 64-bit exec counter at the **chain entry** (counts dispatched *and* chained), pc-keyed slot
table (262144) + compile-time **instruction-mix tag**, dumped top-40 at exit (`atexit`, since normal
`Quit→exit(0)` doesn't call `ppc_jit_aarch64_exit`). Code: `ppc-jit.cpp` (`jit_prof_*`/
`jit_mix_classify`/`jit_profile_dump`). Reviewed (register-safety on the chained path proven clean;
NZCV-neutral `ADD`). **Boot-validated** via the E2E smoke (isolated ISO boot → clean shutdown) under
`SS_JIT_PROFILE=/path`: complete profile, **70936 blocks / 843M block-executions, 0 dropped**.

**First boot-to-Finder-to-shutdown profile (the data this prereq existed to produce):**
- **Hottest single hot spot — RAM `lwarx`/`stwcx.` atomic primitives** `0x10643c30–0x10643c78`
  (2–4-insn blocks, ~1.8% *each*, ≈ **~8% combined**). Disassembled (`SS_JIT_PROFILE_DISASM`):
  `0x10643c30–48` is an **atomic fetch-and-add** (`li r0,0; lwarx r5,0,r4; addc r5,r5,r3; sync;
  stwcx. r5,0,r4; bne retry; …; blr`); `0x10643c54` an **atomic bit-set/test-and-set** (`lwarx …
  slw …`); `0x10643c78` another store-conditional. The companion load/store block `0x106a9848`
  (1.8%) is the **cross-TOC call glue** (`lwz r12,-0x578(r2); … mtctr; bctr`) that dispatches them.
  These are OS refcount/lock atomics, hammered during boot **and** idle.
- **ROM/DR 68k-dispatch core** `0x50467xxx`/`0x50466xxx` (`lha; rlwimi; mtlr; mtctr; bgtctr cr1`,
  ~1.2%→0.9%): inherent 68k-interpretation cost → the **HLE / DR-path** levers.
- **Broad RAM working set** `0x1060xxxx`/`0x106axxxx` at a *uniform* ~0.9% across ~25 blocks: the
  steady-state OS **event/idle loop** (regular C frames: `mflr/stmw/stwu` prologues + flag checks +
  `bl`). Per-block small, but **in aggregate ~22% — the single largest slice by far**, just diffuse.
- **Caveat:** this is a boot→idle→shutdown profile (weighted to boot + idle, not a compute workload).
  For throughput-lever ranking, also capture an **`e2e-bench` (Speedometer) profile** — the
  workload-weighted complement. Routine-name attribution (hot PC → trap/NameRegistry) still TODO.

**Compute-workload profile — `e2e-bench` (Mac OS 9 + Speedometer), 2026-06-06 — resolves the confound:**
Ran the same profiler under the Speedometer suite (**116078 blocks / 10.28G block-executions**,
Speedometer scores: CPU 62.6, Graphics 39.7, Disk 9.2, Math 12020.8). The picture changes decisively:
- **The dominant hot path is now the workload's own large integer/FP blocks** `0x1ed6xxxx`–`0x1ed8xxxx`
  (20–163 insns, top block 2.7%, top ~5 ≈ 11.5%): Speedometer's CPU/Math kernels — `mullw/divw/add`
  chains, `lhau/sthu` compare-swap sort, `lwzx/lhax/mullw` matrix. **Already compiled natively** → their
  lever is **codegen quality** (RA hardening P1a, constant folding P5, scheduling P6, cross-block
  pinning P8), not fallback elimination.
- **The atomics persist under compute** — `0x10907720` is byte-identical `lwarx`/`stwcx.` fetch-and-add
  to the boot profile's `0x10643c30` (relocated in OS 9), still executing 100M+ times *during* the
  benchmark (~5% combined incl. its `0x10980144` TOC glue). **Work-driven, not idle-driven.**
- **The diffuse `0x1060xxxx` idle loop is GONE** under compute → it *was* idle-specific. So idle-skipping
  is a **boot / background-citizen** win (good for a backgrounded VM), **not a throughput lever**.

**Confound resolved → two distinct levers (and a measurement lesson):**
1. **Native `lwarx`/`stwcx.` (P3a) — DONE, but perf-neutral.** Atomics stay hot under real compute
   (~5%) *and* boot (~8%) *of block-executions*, so eliminating the interp fallback removes real
   transitions (verified `hit=2 miss=0`). **But the e2e-bench A/B measured the wall-clock impact as
   neutral within noise** — atomics are a small fraction of *instruction-time* (2–4-insn blocks vs the
   20–163-insn compute blocks that dominate). The lesson: *block-execution share ≠ runtime share*; a
   hot tiny block is a correctness/architecture target, not necessarily a throughput one. Shipped as a
   correctness win (completes a known-incomplete fallback). Full result in §P3a above.
2. **Large-block codegen quality** — the real *throughput* ceiling under compute (the `0x1ed`
   cluster). Pursue via P1a/P5/P6/P8. Diffuse across many blocks, so lower per-item ROI but larger
   total — and, unlike P3a, it actually sits on the runtime-dominant path.

**Idle-skipping**: re-scoped to a boot/background-citizen optimization, not a throughput play.

**Next:** P3a (native atomics) is DONE — correctness win, perf-neutral (see §P3a). The remaining
throughput lever is **large-block codegen quality** (P1a/P5/P6/P8) on the runtime-dominant `0x1ed`
compute cluster — that, not fallback elimination, is where measurable Speedometer gains live.

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

> **Cross-link (2026-06-06):** the "what's hot × what's slow" join this P0 profiler
> needs is exactly the *perf* half of the **unified test-session instrumentation**
> design (`docs/superpowers/specs/2026-06-06-unified-test-session-instrumentation-design.md`,
> Phase 3): it joins the HOT-PC liveness feed ("hot") with the microbench ns/insn
> records ("slow") into a ranked optimization-target table. Build P0 and that perf
> join together — same data, one analyzer (`jit-analyze.py`).

### P0b. Microbenchmark harness — DONE (2026-06-03)

**Result**: `jit-bench` shipped as `rom-harness --bench` / `make bench`.
Differential per-instruction timing (compile each kernel at 16 and 144 body
instrs; `(call_big−call_small)/(144−16)` cancels prologue/epilogue).
`--save-baseline`/`--compare` A/B; `--compare` warns if the baseline predates
`ppc-jit.cpp`. Kernels: `carry-chain` (0b/0f), `rc1` (0g lazy-CR0), `alu` (RA),
`fp-add`/`fp-fma`, `compute` (the Speedometer hot block `0x1ed7befc`).

**Noise correction + deterministic metric (2026-06-06).** The original "<1%
run-to-run noise" claim holds only on a *quiet, cool* host. Under load
(parallel builds/boots, a co-tenant agent, thermal pressure) a **fixed, unchanged
binary swung ±25%** — P/E-core migration + DVFS, not a bench-algorithm bug. Tier-1
methodology fixes (from 3 lateral benchmark subagents) now in `rom-harness.cpp`:
1. **`a64/op` — deterministic ARM64-insns-per-PPC-op** from the JIT's own
   `code_size`. **Zero noise, host-independent, CI-gateable.** This is the primary
   signal for codegen-*size* wins (trailing-MOV removal, leaner CR0, etc.) — it
   reads exactly +0.000 on untouched kernels and the exact delta on changed ones
   (validated: the divw 0f follow-up = −0.164, while every timing column was
   `NOISY`). It cannot see timing-only wins (scheduling, equal-count swaps).
2. **Noise gate** — `BENCH_ROUNDS` interleaved rounds, median `ns/insn` + `cv%`;
   `cv>3%` is flagged `*` and `--compare` prints `NOISY` instead of a bogus delta.
3. **QoS P-core pin** (`QOS_CLASS_USER_INTERACTIVE` — the only Apple-Silicon
   scheduling lever; affinity is a no-op) + `CLOCK_THREAD_CPUTIME_ID`.

Takeaway: use **`a64/op` for codegen-size A/B (any host)**; use **`ns/insn` only on
a quiet host** for timing-only wins. Deferred Tier-2/3 (subagent reports): two-binary
interleaved timing A/B, deterministic hot-trace replay, fallback/block-count deltas,
trend only Speedometer compute subtests.

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

- [`PERFORMANCE_AUDIT.md`](../../PERFORMANCE_AUDIT.md) (local copy; [upstream](https://github.com/rcarmo/macemu-jit/blob/master/PERFORMANCE_AUDIT.md)) — BasiliskII/RPi optimization audit (14/27 implemented)
- [`JIT-FPU-PLAN.md`](JIT-FPU-PLAN.md) — 68K FPU JIT plan (shadow register pattern, validates our RA approach)
- Key warnings: LTO must stay disabled on macOS ARM64; do NOT remove PIE/stack-protector
