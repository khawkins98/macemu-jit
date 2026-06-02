# Research Leads: Borrowing from Dolphin and Other PPC JITs

Leads for improving the SheepShaver PPC→ARM64 JIT (and emulation layer) by studying — and
where license-compatible, reusing — code from mature emulators, primarily **Dolphin**
(GameCube/Wii). Compiled 2026-06-02.

> **Caveat on code snippets:** the snippets below were gathered by web research against the
> Dolphin master branch and are illustrative. Verify each against the actual source before
> acting on it — file paths and URLs are given for that purpose.

---

## Why Dolphin is the right cousin

- The GameCube/Wii CPUs (**Gekko/Broadway**) are **PowerPC 750** derivatives — the same family
  as the G3 Macs SheepShaver emulates. Same ISA generation, same CR/XER semantics.
- Dolphin's **JitArm64** backend has full feature parity with its x86-64 JIT: it is the most
  mature PPC→ARM64 JIT in existence.
- It runs natively on Apple Silicon and solved the **same MAP_JIT / W^X problem** we did
  (Dolphin PR [#9441](https://github.com/dolphin-emu/dolphin/pull/9441), blog post
  ["Temptation of the Apple"](https://dolphin-emu.org/blog/2021/05/24/temptation-of-the-apple-dolphin-on-macos-m1/)).
- **Licensing:** Dolphin is GPL-2.0-or-later; SheepShaver is GPLv2. Code is **combinable**, not
  just study-only. The practical constraint is dependency tails (see Lead 5), not the license.
- **Historical note:** Dolphin and SheepShaver share no code or contributors — different
  communities entirely. The connection is purely that both target PPC750-class CPUs. (Fun
  confirmation of the hardware kinship: Mac OS 9.2 was run on an unmodified Wii in 2022 via
  Linux + Mac-on-Linux — virtualization on real PPC silicon, not emulation.
  [Hackaday writeup](https://hackaday.com/2022/11/24/its-macos-on-an-unmodified-wii/).)

Dolphin JIT layout: `Source/Core/Core/PowerPC/JitArm64/` —
[browse on GitHub](https://github.com/dolphin-emu/dolphin/tree/master/Source/Core/Core/PowerPC/JitArm64).

| File | Covers |
|------|--------|
| `Jit.cpp` / `Jit.h` | Block compile loop, per-instruction dispatch, `JitState` |
| `JitArm64_Integer.cpp` | Integer ALU, XER CA/OV handling |
| `JitArm64_LoadStore*.cpp` | Loads/stores (integer, FP, paired-single) |
| `JitArm64_Branch.cpp` | Branches, exits to dispatcher |
| `JitArm64_SystemRegisters.cpp` | mtspr/mfspr, CR/XER/FPSCR ops |
| `JitArm64_FloatingPoint.cpp` / `_Paired.cpp` | FP and paired-single arithmetic |
| `JitArm64_RegCache.cpp/h` | Register allocator |
| `JitArm64_BackPatch.cpp` | Fastmem + SIGSEGV fault backpatching |
| `JitArm64Cache.cpp` | Block linking and invalidation |
| `JitAsm.cpp` | Hand-emitted dispatcher / entry trampoline |
| `JitArm64_Tables.cpp` | Opcode → handler tables |

Reserved host registers (compare with our x27/x28 convention in BasiliskII and the regs
pinned in `ppc-jit.cpp`):

```cpp
// JitArm64_RegCache.h
constexpr ARM64Reg MEM_REG       = ARM64Reg::X28;  // guest memory base
constexpr ARM64Reg PPC_REG       = ARM64Reg::X29;  // &ppcState
constexpr ARM64Reg DISPATCHER_PC = ARM64Reg::W26;  // PC handed to dispatcher
```

---

## Prioritized leads

### Lead 1 — CR fields as 64-bit values (the `cr_val` trick)

**Source:** [`Source/Core/Core/PowerPC/ConditionRegister.h`](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/PowerPC/ConditionRegister.h)

**What it is:** Dolphin stores each of the 8 CR fields as a `u64` instead of a 4-bit
LT/GT/EQ/SO nibble. The encoding is chosen so that the *sign-extended result of an ALU op is
itself a valid CR value*:

```
SO  iff. bit 59 set
EQ  iff. lower 32 bits == 0
GT  iff. (s64)cr_val > 0
LT  iff. bit 62 set
```

A record-form instruction (`addi.`, `cmpw`, etc.) just sign-extends the 32-bit result into the
CR field slot — no per-bit packing. The 4-bit PPC view is only materialized on `mfcr` /
`mcrf` / serialization (`PPCToInternal()` / `GetField()` conversion helpers).

**What it buys us:** Our JIT currently computes and packs CR bits on every record-form op.
This is the single biggest structural idea in Dolphin's JIT — it makes the hottest PPC idiom
(compare + record) nearly free.

**How it maps to our code:** This is invasive — it changes the in-memory CR representation in
`powerpc_cpu`, so the interpreter, JIT, and any state serialization all need the conversion
helpers. Best treated as a measured experiment after profiling shows CR packing is hot.

**Effort/risk:** High effort, high payoff potential. Do last, after Leads 2–4.

### Lead 2 — Nesting-counter W^X toggle

**Source:** [`Source/Core/Common/MemoryUtil.cpp`](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Common/MemoryUtil.cpp)

**What it is:** Dolphin wraps all code emission in
`JITPageWriteEnableExecuteDisable()` / `JITPageWriteDisableExecuteEnable()` guarded by a
**thread-local nesting counter** — `pthread_jit_write_protect_np()` is only called when the
counter crosses 0, so nested emit regions (e.g. compiling a block that patches chain sites)
toggle protection exactly once.

```cpp
void JITPageWriteEnableExecuteDisable() {
  if (JITPageWriteNestCounter() == 0)
    pthread_jit_write_protect_np(0);
  JITPageWriteNestCounter()++;
}
```

**What it buys us:** We just fought W^X toggle overhead (commit 8f2acc9b eliminated it for
out-of-range blocks). A nesting counter is the general fix: emit helpers can be written
defensively (each brackets its own toggle) without paying per-call syscall cost.

**How it maps to our code:** `jit-target-cache.hpp` owns write-protect toggling;
`ppc-jit.cpp` compile + `patch_chain_sites()` are the nested callers. Small, self-contained
change.

**Effort/risk:** Low effort, low risk. **Do this first.**

### Lead 3 — Lazy carry state machine (XER CA)

**Source:** [`Source/Core/Core/PowerPC/JitArm64/JitArm64_Integer.cpp`](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/PowerPC/JitArm64/JitArm64_Integer.cpp) and `Jit.h` (`js.carryFlag`)

**What it is:** Carry is tracked in a 4-state enum — `InPPCState`, `InHostCarry`,
`ConstantTrue`, `ConstantFalse`. A carry-producing op leaves carry in the **host C flag**
(`ADDS`/`ADCS`); the store to `ppcState.xer_ca` (`CSET` + `STRB`) only happens when something
forces a flush (block exit, call, or an op that clobbers flags). Carry-consuming ops
(`adde`/`subfe`) load from whichever location the state machine says is current. A
`CARRY_IF_NEEDED` macro only emits the flag-setting variant when a later instruction in the
block actually wants CA.

**What it buys us:** `addc`/`adde`/`subfc`/`subfe` chains (common in 64-bit arithmetic and
Toolbox math) currently do LDRB/STRB on our XER struct (`ca` at offset 902) per instruction.
Within a block this collapses to pure register/flag operations.

**How it maps to our code:** This is a *deliberate* contrast with our immediate-writeback
style (see `JIT-STYLE-DECISION.md`). The contained version: keep immediate writeback as the
default, allow lazy carry **within a block only**, always flushed at block exit — preserving
our "no lazy state across block boundaries" rule.

**Effort/risk:** Medium effort, medium risk (must be airtight at exits/exceptions). Profile first.

### Lead 4 — Punt OE-form overflow to the interpreter

**Source:** same file, `FALLBACK_IF(inst.OE)` pattern throughout `JitArm64_Integer.cpp`.

**What it is:** Dolphin does **not** emit overflow (XER OV/SO) logic inline — OE-form
instructions (`addo`, `mullwo`, …) fall back to the interpreter. They're rare enough that
inline codegen isn't worth the complexity.

**What it buys us:** Permission to simplify. If our JIT emits OV/SO computation inline
(LDRB/STRB at offset 900), we can audit whether the OE forms even appear in hot paths
(rom-harness counters can answer this) and consider falling back instead — fewer emit paths
to keep correct.

**Effort/risk:** Trivial. Mostly a *deletion* opportunity.

### Lead 5 — Vendor or crib from `Arm64Emitter`

**Source:** [`Source/Core/Common/Arm64Emitter.h`](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Common/Arm64Emitter.h) / `.cpp` (~50KB header; classes `ARM64XEmitter`, `ARM64FloatEmitter`)

**What it is:** The battle-tested standalone ARM64 emitter shared (by ancestry) with PPSSPP —
hundreds of emit methods, NEON support, `LogicalImm` encoding, fixup branches.

**License note:** GPL-2.0-or-later → vendorable into SheepShaver (GPLv2). The real cost is the
dependency tail: it includes Dolphin's `Common/CodeBlock.h`, `BitSet.h`, `BitUtils.h`,
`Assert.h`, `SmallVector.h`, which would need vendoring or stubbing.

**What it buys us:** Probably *not* a wholesale replacement — our hand-written emitter in
`ppc-jit.cpp` works and is small. Value is as a **reference implementation**: when we need a
new instruction encoding (NEON for AltiVec someday, `LogicalImm` for `rlwinm` masks), crib the
encoding logic from here rather than the ARM ARM.

**Effort/risk:** Use as reference: zero risk. Vendoring: medium effort, questionable payoff.

### Lead 6 — Fastmem fault backpatching

**Source:** [`Source/Core/Core/PowerPC/JitArm64/JitArm64_BackPatch.cpp`](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/PowerPC/JitArm64/JitArm64_BackPatch.cpp)

**What it is:** Guest loads/stores emit a bare `LDR/STR [MEM_REG + addr]` fast path. MMIO /
unmapped accesses SIGSEGV; the handler looks up the faulting PC in a `m_fault_to_handler` map
and **patches the faulting instruction into a `BL slow_path`** (one-shot — never faults
again). Slow paths live in pre-emitted "far code."

**What it buys us:** Our DIRECT_ADDRESSING (`NATMEM_OFFSET + guest`) already gives us the fast
path. The backpatch idea matters if/when we want JIT-direct hardware/MMIO access instead of
exiting to C++ for non-RAM addresses.

**How it maps to our code:** Note the interaction with W^X — patching executable code from a
signal handler requires the write-protect toggle inside the handler. Defer until profiling
shows MMIO exits are hot.

**Effort/risk:** High effort, signal-handler subtlety. Long-term lead only.

### Lead 7 — Block linking / dispatcher details

**Source:** [`JitArm64Cache.cpp`](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/PowerPC/JitArm64/JitArm64Cache.cpp), [`JitAsm.cpp`](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/PowerPC/JitArm64/JitAsm.cpp)

**What it is:** Comparable to our `jit_bc_heads[]` + `patch_chain_sites()`:
- Exit sites for unknown targets emit `MOVI2R(DISPATCHER_PC, addr); BL dispatcher`, padded
  with `BRK` so they can be re-patched into direct branches once the target exists.
- Destroyed blocks have their entry overwritten with `BRK` to trap stale linked-from code.
- The dispatcher does inline hash lookup in emitted asm (PC + feature flags → block pointer),
  only calling C++ on a miss.

**What it buys us:** A correctness cross-check on our chain-patching design, plus two ideas:
`BRK`-on-destroy as a debugging tripwire for stale chains, and the inline asm dispatcher
lookup (we currently return to C++ for every non-chained dispatch).

**Effort/risk:** `BRK` tripwire: trivial. Inline dispatcher: medium; measure dispatch overhead
first (T2 counters from commit 7030a441 are the starting point).

---

## Contrasts & references (non-Dolphin)

| Project | Guest CPU | ARM64 JIT | License | Relevance |
|---------|-----------|-----------|---------|-----------|
| **QEMU** [`target/ppc/translate.c`](https://github.com/qemu/qemu/blob/master/target/ppc/translate.c) | Any PPC | TCG IR → aarch64 | GPLv2 | **Correctness oracle.** The most-reviewed PPC semantics reference (XER CA/OV, rlwinm masks, CR updates). Use when a vector fails and the ISA manual is ambiguous. |
| **RPCS3** ([arm64 blog](https://blog.rpcs3.net/2024/12/09/introducing-rpcs3-for-arm64/), PRs [#12115](https://github.com/RPCS3/rpcs3/pull/12115), [#15992](https://github.com/RPCS3/rpcs3/pull/15992)) | Cell PPU (PPC64) | LLVM-based; IR transformed from amd64 reference | GPLv2 | The "use LLVM" school — contrast with our hand-written approach. PR #12115 has per-thread W^X compliance notes for macOS. |
| **Cemu** ([PR #641](https://github.com/cemu-project/Cemu/pull/641)) | Espresso (PPC750-class!) | x86-64 only; portable-IR rework in progress | MPL-2.0 | Closest *guest* match after Dolphin. PR #641 is a case study in restructuring a PPC dynarec (typed registers, CR-as-bool-regs, DCE) to make a second backend tractable. |
| **Xenia** ([repo](https://github.com/xenia-project/xenia)) | Xenon (PPC64) | Incomplete arm64 backend | **BSD** | Permissive license — snippets freely reusable — but the arm64 backend is immature. |
| **PPSSPP** ([Arm64Emitter](https://github.com/hrydgard/ppsspp/blob/master/Common/Arm64Emitter.cpp)) | MIPS | Mature | GPLv2+ | Same emitter family as Dolphin's (shared author). Second reference copy of the emitter. |
| **Mac-on-Linux** | — | — (virtualization) | GPL | Historical only: how Mac OS 9 ran on a real Wii. Nothing to borrow. |

---

## Suggested investigation order

1. **Lead 2 (W^X nesting counter)** — small, directly continues the work in commit 8f2acc9b.
2. **Lead 4 (OE-form audit)** — possibly delete code; rom-harness can measure OE frequency.
3. **Lead 7 (`BRK` tripwire)** — cheap debugging win for chain-patch bugs.
4. **Lead 3 (lazy carry, within-block only)** — after profiling shows XER CA traffic is hot.
5. **Lead 1 (CR 64-bit representation)** — the big one; only after the JIT is otherwise stable
   and profiling justifies it.
6. **Leads 5–6** — keep as references; revisit for AltiVec/NEON and MMIO-in-JIT respectively.

---

## Post-investigation findings (2026-06-02)

Each lead was studied in depth by a dedicated agent that verified the actual Dolphin source
and read our code. Full analyses live in `docs/research/lead-*.md`. The investigations
**overturned several of the assumptions above**:

### Verdicts

| Lead | Verdict | Detail |
|------|---------|--------|
| 1 — CR 64-bit | **Defer; do the cheap cleanup instead** | Dolphin's 1-insn record form depends on a CR *register cache* we don't have; with CR in memory, mfcr regresses ~3→~40 insns. Tier-1 win available now: `emit_update_cr0` can go ~18→~8 insns with CSET/BFI, zero risk. |
| 2 — W^X nesting counter | **Downgraded to hygiene** | Our toggles are sequential, not nested — the counter solves a problem we don't have. Real win: hoist the per-word toggle out of the `patch_chain_sites()` loop. Caution: our `end_write` couples icache invalidation; a naive counter would drop it. |
| 3 — Lazy carry | **Not yet** | Payoff capped (our GPRs are memory-resident; no reg cache). Blocked behind the open truncation-epilogue corruption that forced lazy-CR0 off. Fix the `adde` bug first (see below). |
| 4 — OE-form punt | **REJECTED** | Workload inversion: Mac OS's ROM 68K emulator makes OE forms *hot* for us (they were ~95% of compile failures pre-8f2acc9b). Dolphin's punt is a maturity artifact — its x86-64 backend inlines OE. We already run the optimal hybrid. |
| 5 — Arm64Emitter | **Reference only + one targeted crib** | Port Dolphin's dependency-free `LogicalImm` encoder → rewrite `rlwinm`/`rlwimi` mask paths (3-6 insns → 1). Defer NEON until AltiVec. |
| 6 — Fastmem backpatch | **CLOSED — already satisfied / not applicable** | We already emit Dolphin's fast path. Mac hardware access is EMUL_OP traps, not MMIO faults — nothing to backpatch. One real gap: unmapped JIT access crashes instead of raising guest DSI. |
| 7 — Dispatch/linking | **BRK tripwire yes; inline dispatcher premature** | T2 data shows ~zero JIT-cache residency at steady state — execution falls back to the interpreter after ~15 s. Fix *residency* before optimizing dispatch. Chain-site pool exhausts silently (capacity 16384, dropped when full). |

### Bugs found during investigation

1. **`mullwo` silently ignores OE** (ppc-jit.cpp:1068, case 715) — accepted by the JIT but never
   sets XER OV/SO. Fix: punt to interpreter like `divwo`. *(Lead 4)*
2. **`adde` carry-out likely wrong** (ppc-jit.cpp:1401, case 138) — dead `MRS NZCV` + non-flag
   `ADD` for carry-in. Fix: immediate-writeback `ADCS` (correct *and* faster). *(Lead 3)*
3. **Chain-site pool exhaustion is silent** (ppc-jit.cpp:147) — sites dropped when the 16384-entry
   pool fills; only reset on full flush. Runtime chaining can silently stop on long runs.
   Fix: instrument now, then decide. *(Lead 7)*
4. **Doc drift**: CLAUDE.md says 8192-bucket block cache; source is 32768 buckets / 65536 pool.
   XER byte offsets in CLAUDE.md (900/902) are also stale — CA is at offset 1030. *(Leads 3, 7)*

### Revised action list

**Now (correctness):**
1. Fix `adde` carry-out with `ADCS`
2. Punt `mullwo` to the interpreter
3. Instrument chain-site pool exhaustion
4. Update CLAUDE.md (block-cache size, XER offsets)

**Now (cheap wins):**
5. `emit_update_cr0` cleanup with CSET/BFI (~18→~8 insns)
6. Port `LogicalImm` encoder; use immediates in `rlwinm`/`rlwimi`
7. Hoist W^X toggle out of `patch_chain_sites()` loop
8. Debug-gated BRK tripwire on destroyed blocks

**Next (strategic — the real finding):**
9. **Investigate JIT-loop residency** — why does execution leave the JIT after ~15 s and stay
   in the interpreter? This gates *every* throughput optimization in this document.

**Later (gated on residency + profiling):**
10. Lazy carry / lazy CR0 (after truncation-epilogue root cause is closed)
11. CR register cache, then possibly the 64-bit CR representation
12. Inline-asm dispatcher
