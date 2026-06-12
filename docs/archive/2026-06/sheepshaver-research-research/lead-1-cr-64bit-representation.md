# Lead 1 — CR fields as 64-bit values (Dolphin's `cr_val` trick)

> **Status:** 📖 Reference / archive · **Created:** 2026-06-02 · **Updated:** 2026-06-04
> **Why this doc exists:** Research lead: CR fields as 64-bit values (Dolphin cr_val) — verdict in backlog.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../../CONTRIBUTING.md) → "Documentation Lifecycle")._


Deep-dive study for the SheepShaver PPC→ARM64 JIT. Compiled 2026-06-02.
Companion to `docs/planning/sheepshaver-research/EMULATOR-RESEARCH-LEADS.md` (Lead 1).

**No code was changed by this study. This is analysis only.**

---

## 1. Verified Dolphin implementation (real code)

All quotes verified against `dolphin-emu/dolphin` `master`, fetched 2026-06-02.

### 1.1 The representation — `Source/Core/Core/PowerPC/ConditionRegister.h`

```cpp
enum CRBits
{
  CR_SO = 1,
  CR_EQ = 2,
  CR_GT = 4,
  CR_LT = 8,

  CR_SO_BIT = 0,
  CR_EQ_BIT = 1,
  CR_GT_BIT = 2,
  CR_LT_BIT = 3,

  CR_EMU_SO_BIT = 59,
  CR_EMU_LT_BIT = 62,
};

// Optimized CR implementation. Instead of storing CR in its PowerPC format
// (4 bit value, SO/EQ/LT/GT), we store instead a 64 bit value for each of
// the 8 CR register parts. This 64 bit value follows this format:
//   - SO iff. bit 59 is set
//   - EQ iff. lower 32 bits == 0
//   - GT iff. (s64)cr_val > 0
//   - LT iff. bit 62 is set
//
// This has the interesting property that sign-extending the result of an
// operation from 32 to 64 bits results in a 64 bit value that works as a
// CR value. ...
struct ConditionRegister
{
  static const std::array<u64, 16> s_crTable;
  u64 fields[8];

  static constexpr u64 PPCToInternal(u8 value)
  {
    u64 cr_val = 0x100000000;
    cr_val |= (u64) !!(value & CR_SO) << CR_EMU_SO_BIT;
    cr_val |= (u64) !(value & CR_EQ);
    cr_val |= (u64) !(value & CR_GT) << 63;
    cr_val |= (u64) !!(value & CR_LT) << CR_EMU_LT_BIT;
    return cr_val;
  }

  void SetField(u32 cr_field, u32 value) { fields[cr_field] = s_crTable[value]; }

  u32 GetField(u32 cr_field) const
  {
    const u64 cr_val = fields[cr_field];
    u32 ppc_cr = 0;
    // LT/SO
    ppc_cr |= (cr_val >> CR_EMU_SO_BIT) & (PowerPC::CR_LT | PowerPC::CR_SO);
    // EQ
    ppc_cr |= ((cr_val & 0xFFFFFFFF) == 0) << PowerPC::CR_EQ_BIT;
    // GT
    ppc_cr |= (static_cast<s64>(cr_val) > 0) << PowerPC::CR_GT_BIT;
    return ppc_cr;
  }
  // ... GetBit / SetBit / Set / Get omitted (slow path)
};
```

Note the **non-obvious encoding detail**: a freshly sign-extended 32-bit value is a valid
cr_val *only because* of how the four predicates are defined:

- **EQ** = "low 32 bits == 0" — true exactly when the 32-bit result was 0.
- **GT** = "(s64)cr_val > 0" — true when the result is positive and nonzero (the
  sign-extend keeps it positive; nonzero means low-32 ≠ 0). PPCToInternal sets **bit 63**
  for the *not-GT* case so that any reconstructed value with GT=0 is forced negative.
- **LT** = "bit 62 set" — sign-extending a negative 32-bit value sets bits 32..63,
  including bit 62.
- **SO** = "bit 59 set" — also set by sign extension of a negative value.

### 1.2 Record-form update — `JitArm64_Integer.cpp` (lines 33–43)

```cpp
void JitArm64::ComputeRC0(ARM64Reg reg)
{
  gpr.BindCRToRegister(0, false);
  SXTW(gpr.CR(0), reg);
}

void JitArm64::ComputeRC0(u32 imm)
{
  gpr.BindCRToRegister(0, false);
  MOVI2R(gpr.CR(0), s64(s32(imm)));
}
```

**A record-form CR0 update is literally one instruction (`SXTW`)** into a CR field that
lives in a host register (`gpr.CR(0)`), because Dolphin keeps CR fields *register-allocated*,
not in memory.

### 1.3 The SO subtlety (verified, important for us)

The PPC spec says record-form CR0 = `{LT, GT, EQ, SO}` where **SO is copied from XER[SO]**.
But `SXTW` puts the *sign bit* of the result into bit 59 (the SO slot), **not** XER[SO].
This is a deliberate Dolphin design decision, not a bug:

- For Dolphin's emulated workloads, CR0.SO produced by record-form ops is essentially never
  consumed before the next CR0-setting op, so Dolphin trades exact SO fidelity for the
  one-instruction `SXTW`. The cr_val SO bit after a record-form op reflects the sign of the
  result, which only differs from XER[SO] when XER[SO] was already sticky-set.
- `mfcr` (`JitArm64_SystemRegisters.cpp`, lines 692–705) reads SO straight out of bit 59:

  ```cpp
  // SO and LT
  if (i == 0) {
    MOVI2R(XB, PowerPC::CR_SO | PowerPC::CR_LT);
    AND(XA, XB, CR, ArithOption(CR, ShiftType::LSR, PowerPC::CR_EMU_SO_BIT));
  } else {
    AND(XC, XB, CR, ArithOption(CR, ShiftType::LSR, PowerPC::CR_EMU_SO_BIT));
    ORR(XA, XC, XA, ArithOption(XA, ShiftType::LSL, 4));
  }
  // EQ:  ORR WC, WA, #(1<<EQ_BIT); CMP WCR, WZR; CSEL WA, WC, WA, EQ
  // GT:  ORR WC, WA, #(1<<GT_BIT); CMP CR,  ZR;  CSEL WA, WC, WA, GT
  ```

**Implication for SheepShaver:** our interpreter (`record_cr0` → `cr.set_so()`) copies XER[SO]
into CR0.SO exactly per spec, and our opcode harness checks it. A faithful `SXTW`-only port
would diverge on CR0.SO for negative results. We would have to add an explicit SO merge
(an extra `BFI`/`ORR` from the XER.SO byte), which erodes the "one instruction" win.

### 1.4 Individual-bit ops use BFI/UBFX/TBNZ on the cr_val — `JitArm64_SystemRegisters.cpp`

`GetCRFieldBit` (line 23) / `SetCRFieldBit` (line 53) handle `crand/cror/...`, `mcrf`,
branch-condition extraction by operating directly on the 64-bit field:

```cpp
// GetCRFieldBit:
//   SO bit -> UBFX(out, CR, CR_EMU_SO_BIT, 1)        // bit 59
//   LT bit -> UBFX(out, CR, CR_EMU_LT_BIT, 1)        // bit 62
//   EQ bit -> CMP low32==0 / CSET
//   GT bit -> CMP (s64)CR>0 / CSET
// SetCRFieldBit:
//   SO -> BFI(CR, in, CR_EMU_SO_BIT, 1)  [+ EOR if negate]
//   LT -> BFI(CR, in, CR_EMU_LT_BIT, 1)  [+ EOR if negate]
//   EQ/GT -> clear/insert low32, fix bit 63 so GT stays consistent
```

Branch-on-CR avoids materialization entirely (lines 172/179):
`TBNZ(CR, CR_EMU_SO_BIT)` / `TBZ(...)` for SO, `TBNZ(CR, CR_EMU_LT_BIT)` for LT — single
test-and-branch instructions straight off the cr_val.

### 1.5 The crucial enabler: CR fields live in registers

`mfcr` ends each field with:

```cpp
if (js.op->crDiscardable[i])
  gpr.DiscardCRRegisters(BitSet8{i});
else if (!js.op->crInUse[i])
  gpr.StoreCRRegisters(BitSet8{i}, WC);
```

Dolphin's register cache (`JitArm64_RegCache`) keeps CR fields in host GPRs across
instructions in a block, flushing to `ppcState` only when needed. **The 1-instruction
record form depends on this.** With CR fields in memory the cost is `SXTW` + `STR`
(2 insns), and reads pay a `LDR` + reconstruct.

---

## 2. Our current implementation (real code, file:line)

### 2.1 In-memory representation — packed `uint32`

`SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-registers.hpp:30-101`

```cpp
class powerpc_cr_register {
    uint32 cr;                       // standard PPC packed layout: CR0 in bits 31..28
public:
    void set(int crfd, uint32 v);    // cr = (cr & ~(0xf<<sh)) | (v<<sh)
    uint32 get(int crfd) const;      // (cr >> sh) & 0xf
    void set_so(int crfd, bool v);   // OR/clear the SO bit of a field
    void compute(int crfd, int32 v); // set LT/GT/EQ from sign of v (does NOT touch SO)
    void set(uint32 v) { cr = v; }   // whole-register
    uint32 get() const  { return cr; }
    bool test(int condition) const { return (cr << condition) & 0x80000000; }
};
```

One word, big-endian-PPC bit order: field `crfd` occupies bits `28 - 4*crfd .. 31 - 4*crfd`.
This is the canonical PPC packed format, identical to what `mfcr` returns.

### 2.2 Interpreter — `record_cr0` / `record_cr` / `record_cr1`

`SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-execute.cpp` — e.g. line 181, 216, 243, 300, 360, 406:

```cpp
record_cr0((int32)d);                                    // record-form integer ops
record_cr(crfd, (CT)a < (CT)b ? -1 : ((CT)a > (CT)b ? +1 : 0));   // cmp/cmpl (line 360)
record_cr1();                                            // FP record forms
```

`record_cr0` calls `cr.compute(0, v)` then `cr.set_so(0, xer.so)` — i.e. our interpreter
**does** copy XER[SO] into CR0.SO per spec (the point Dolphin's `SXTW` skips).

### 2.3 JIT record-form CR0 — the hot path (~18 instructions, NOT lazy)

`SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`

CR lives in `powerpc_cpu` at struct offset **`PPCR_CR = 1024`** (`ppc-jit.cpp:250`), accessed
with `LDR/STR` word loads.

`emit_update_cr0(result_reg)` (`ppc-jit.cpp:538-568`) emits, for every `Rc=1` op:

```
CMP Wresult, #0
MOVZ RTMP2, 0
MOVZ RTMP2, 0            (redundant)
MOV  RTMP0, #8 (LT)
MOV  RTMP1, #4 (GT)
CSEL RTMP2, RTMP0, RTMP2, LT
CSEL RTMP2, RTMP1, RTMP2, GT
MOV  RTMP0, #2 (EQ)
CSEL RTMP2, RTMP0, RTMP2, EQ
LDRB RTMPx, [RSTATE, XER_SO]      (emit_or_xer_so_into_cr_nibble)
ORR  RTMP2, RTMP2, RTMPx
MOV  RTMP0, #28
LSL  RTMP2, RTMP2, RTMP0
LDR  RTMP0, [RSTATE, #CR]
MOV  RTMP1, #0x0FFFFFFF
AND  RTMP0, RTMP0, RTMP1
ORR  RTMP0, RTMP0, RTMP2
STR  RTMP0, [RSTATE, #CR]
```

**≈18-19 ARM64 instructions per record-form op**, including a CR load + store and an XER.SO
byte load. Called from every `add./and./or./...` site, e.g. `ppc-jit.cpp:985`
(`if (op & 1) lazy_update_cr0(RTMP0);`).

### 2.4 The lazy-CR0 path exists but is DISABLED

`ppc-jit.cpp:725-777` implements a within-block lazy scheme (`lazy_cr0_valid`,
`lazy_cr0_reg`, `emit_materialize_cr0`, `lazy_flush_cr0`). But:

```cpp
// ppc-jit.cpp:768
static void lazy_update_cr0(int result_reg) {
    /* DISABLED: boot hang regression. Materialize immediately. */
    emit_update_cr0(result_reg);
}
```

**History (verified via git):** commit `98fd7989` "fix: restore Gate 2 + disable lazy
CR0/regalloc — fixes JIT boot hang" disabled lazy CR0 **and** the register allocator
**together**, and its stated root cause was **Gate 2 / partial-block truncation**, not lazy
CR0 per se. The lazy scheme was introduced in `efef33b0`. So the "lazy CR0 broke boot"
attribution is **circumstantial** — it was reverted in a bundle with regalloc and a block
boundary change. The most likely real hazard (and the one a transient-cr_val scheme shares):
**some CR consumer reads `[RSTATE, #CR]` from memory without going through `lazy_flush_cr0`**
— e.g. an interpreter fallback for an unhandled opcode, an exception/interrupt exit, or a
chain to another block — seeing stale CR0.

### 2.5 cmp (`cmpw`) — `ppc-jit.cpp:955-979`

```
LDR Wa, [RSTATE, GPR(ra)]
LDR Wb, [RSTATE, GPR(rb)]
SUBS WZR, Wa, Wb
MOVZ RTMP0, 0
MOV  RTMP1,#8; CSEL RTMP0,RTMP1,RTMP0,LT
MOV  RTMP1,#4; CSEL RTMP0,RTMP1,RTMP0,GT
MOV  RTMP1,#2; CSEL RTMP0,RTMP1,RTMP0,EQ
LDRB ..XER_SO..; ORR RTMP0,..              (emit_or_xer_so_into_cr_nibble)
[LSL into field position if crd!=7]
lazy_flush_cr0()
LDR  RTMP1, [RSTATE, #CR]
MOV  RTMP2, ~(0xF<<shift); AND
ORR  RTMP1, RTMP1, RTMP0
STR  RTMP1, [RSTATE, #CR]
```

≈16-18 instructions; same CSEL-nibble-then-pack-into-memory shape as `emit_update_cr0`.

### 2.6 CR consumers (everything that touches the layout)

- **mfcr** `ppc-jit.cpp:1077` — `lazy_flush_cr0(); LDR [CR]; store to GPR`. Trivial today
  because CR is already packed.
- **mtcrf** `ppc-jit.cpp:1082` — writes packed value (or masked fields) straight to `[CR]`.
- **Branch-on-CR** `ppc-jit.cpp:2307-2474` — extracts `CR[BI]` bit from the packed word
  (`bc/beq/bne/bdnz...`).
- **CR-logical ops** (`crand/cror/crxor/crnand/crnor/creqv/crandc/crorc`, `mcrf`, `mcrxr`) —
  individual CR-bit manipulation; would each need cr_val↔packed conversion under Dolphin's
  scheme.
- **Interpreter** `ppc-execute.cpp` — `record_cr0/cr/cr1/cr6` (lines 181…1408).
- **rom-harness REGDUMP** — `SheepShaver/rom-harness/rom-harness.cpp` dumps `cr` as a packed
  hex word; opcode harness `REGDUMP` diffs likewise. Any in-memory change to `u64[8]` would
  require converting back to packed before printing or **every harness comparison breaks**.
- **68K emulator interface / save-state:** SheepShaver has no portable CPU-state snapshot for
  the kpx PPC core (no serialization of `powerpc_cpu` to disk), so that consumer is **not** a
  concern here — unlike Dolphin, which lists Set/Get as the serialization path.

---

## 3. Adoption analysis

### 3.1 What changes if we adopt the full Dolphin layout (`u64 fields[8]`)

1. `powerpc_cr_register` storage changes from `uint32 cr` to `u64 fields[8]` (32 bytes,
   changing `powerpc_cpu` layout and `PPCR_CR`/struct offsets the JIT hardcodes).
2. Every interpreter `record_*`, `cr.get/set`, `cr.test` rewritten to the encoding.
3. Every JIT CR site rewritten:
   - record-form: `SXTW` (+ SO merge + `STR` since we have no CR register cache) ≈ 3 insns.
   - cmp: `SXTW Wb; SUB Xd, Xb, Xa; STR` into the field ≈ 4 insns.
   - mfcr: now **more expensive** — must reconstruct packed format from 8 fields
     (Dolphin spends ~5 insns/field = ~40 insns) instead of one `LDR`.
   - branch-on-CR: `TBNZ/TBZ` on the field — cheaper and cleaner than today's packed extract.
   - mtcrf / CR-logical: need `s_crTable`-style conversion and bit fiddling.
4. rom-harness + opcode-harness REGDUMP must convert `u64[8]` → packed before printing.

### 3.2 Instruction-count estimate per record-form op

| Path | Today | Dolphin-faithful, CR in memory | Dolphin, CR in registers |
|------|-------|-------------------------------|--------------------------|
| record-form CR0 | ~18 | `SXTW`+SO-merge+`STR` ≈ 3 | `SXTW` = 1 |
| cmpw | ~17 | `SXTW`+`SUB`+`STR` ≈ 4 | `SXTW`+`SUB` ≈ 2 |
| mfcr | ~3 | ~40 (8× reconstruct) | ~40 |
| branch-on-CR | ~6 | 1 (`TBNZ`) | 1 |

The headline win (18→1) **only** materializes with a CR register cache we do not have.
With CR in memory the realistic record-form win is **~18 → ~3** (still a 6× reduction on the
hottest idiom), but **mfcr regresses** from 3 to ~40 instructions. Net depends on the
dynamic ratio of record-form/compare ops to `mfcr`. In Mac OS / Toolbox code, record-form +
compare + conditional branch dominate; `mfcr` is comparatively rare — so the trade is likely
favorable, but it must be measured (rom-harness opcode counters) before committing.

### 3.3 Lower-risk middle ground (recommended framing)

**Keep packed `uint32 cr` in memory (no layout change, no harness/interpreter churn). Use the
cr_val trick only transiently inside a block, with strict flush discipline** — this is exactly
the disabled lazy-CR0 mechanism, generalized. Two viable increments:

- **Increment A — fix and re-enable the existing lazy CR0.** It already exists
  (`ppc-jit.cpp:725-777`). The work is auditing **every** path that can exit a block or read
  `[CR]` (interpreter fallback for unhandled opcodes, exception/IRQ exit, mfcr, branch-on-CR,
  block chaining) and proving each calls `lazy_flush_cr0()` first. The current code already
  flushes at mfcr/mtcrf/cmp; the gap is almost certainly fallback/exception exits. This buys
  the record-form win **without** any memory-layout change and keeps "no lazy state across
  block boundaries."
- **Increment B — cheaper packing without lazy state.** Even immediate-writeback can drop
  from ~18 to ~8 insns: compute the 4-bit nibble with `CSET`/`CSINC` instead of three
  `MOV`+`CSEL` pairs, fold the redundant second `MOVZ RTMP2,0` (line 547), and `BFI` the
  nibble into the loaded CR word instead of `MOV #mask; AND; ORR`. Zero risk, no layout change.

---

## 4. Concrete implementation sketch

### 4.1 Increment B (zero-risk packing cleanup) — do this regardless

Rewrite `emit_update_cr0` to:

```
CMP Wresult, #0
CSET RTMP2, LT            ; LT -> RTMP2 = 1/0
CSINC ...                 ; or build {LT,GT,EQ} nibble via CSET + ORR shifts
... assemble nibble bits 3..1, OR XER.SO byte into bit 0 ...
LDR  RTMP0, [RSTATE,#CR]
BFI  RTMP0, nibble, #(28), #4
STR  RTMP0, [RSTATE,#CR]
```

Target ~8-9 insns; behaviour bit-identical (still copies XER.SO into CR0.SO, so harness-safe).

### 4.2 Increment A (re-enable lazy CR0, packed memory unchanged)

- Re-enable `lazy_update_cr0` to set `lazy_cr0_valid/lazy_cr0_reg` instead of materializing.
- **Guarantee a flush at every block-leaving edge**, not just the known consumers:
  - before any `return false` that falls back to the interpreter,
  - in the epilogue emitter (`record_chain_site` / block exit) — emit `lazy_flush_cr0()`
    unconditionally,
  - before any helper call that could read `ppcState.cr` (exceptions, mtspr, sc).
- Keep a debug assertion / `BRK` tripwire (cf. Lead 7) at flush points during bring-up.
- The saved `lazy_cr0_reg` must be a register guaranteed live until flush — today emit uses
  RTMP scratch that intervening ops clobber, which is why `emit_materialize_cr0` re-`CMP`s.
  That re-CMP is only valid if the operand register is preserved; verify this holds across
  the ops emitted between set and flush (a likely source of the original regression).

### 4.3 Full Dolphin layout (`u64 fields[8]`) — only if profiling demands it

Add `PPCToInternal` / `GetField` (port verbatim — GPL-compatible), change storage, rewrite all
CR sites, make REGDUMP convert to packed. Defer the CR register cache as a separate, larger
project; without it the encoding alone yields only the ~18→3 win and a slower mfcr.

---

## 5. Effort, risk, expected payoff

| Option | Effort | Risk | Payoff |
|--------|--------|------|--------|
| Increment B (packing cleanup) | Low (1 function) | Very low — bit-identical | ~18→~8 insns/record-form |
| Increment A (re-enable lazy CR0) | Medium | **Medium-high** — same flush-discipline hazard that was reverted in `98fd7989`; correctness depends on covering *every* block exit | ~18→~3 amortized; cmp/branch also cheaper |
| Full Dolphin `u64[8]` (memory) | High | High — touches interpreter, all JIT CR sites, REGDUMP; mfcr regresses; SO-fidelity decision | ~18→3 record-form, but no "free" win without reg cache |
| Dolphin layout + CR register cache | Very high | Very high | The real 18→1 prize; matches Dolphin |

**Key correctness caveats discovered:**
- Dolphin's 1-insn `SXTW` does **not** copy XER[SO] into CR0.SO; ours does and the harness
  checks it. A faithful port needs an explicit SO merge.
- The headline 1-instruction figure requires CR-in-registers, which SheepShaver lacks.
- The "lazy CR0 caused the boot hang" claim is unproven — it was reverted bundled with
  regalloc and a block-boundary change. But the structural hazard (a CR consumer that reads
  memory without flushing) is real and is the thing to nail down.

---

## 6. Recommendation

1. **Do Increment B now** (cheap `emit_update_cr0` cleanup). It is risk-free, halves the
   hottest CR path, and is independent of everything else. It also makes any later comparison
   fairer.
2. **Hold Increment A** until rom-harness counters confirm record-form CR0 packing is actually
   hot in booted/steady-state Mac OS (not just bring-up). If pursued, treat it as a
   flush-discipline audit, not a perf tweak: enumerate every block-exit edge and prove each
   flushes, with a `BRK` tripwire during bring-up. This is the highest-value *contained* step
   and directly resurrects existing code.
3. **Defer the full `u64[8]` layout.** It is invasive, regresses `mfcr`, forces a REGDUMP
   conversion, and — without a CR register cache — does not deliver Dolphin's marquee win. The
   leads doc's ordering (Lead 1 last, after the JIT is otherwise stable and profiling
   justifies it) stands. If ever attempted, the **CR-in-registers cache is the real prize**,
   not the encoding by itself.

This matches the repo's stated philosophy (`docs/planning/JIT-STYLE-DECISION.md`): immediate writeback by
default, lazy state *within a block only* and always flushed at exits — *simple by default,
complexity by proof, performance by earned sophistication.*
