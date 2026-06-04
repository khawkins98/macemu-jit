# Lead 3 — Lazy carry state machine for XER CA

Study of Dolphin's lazy-carry design and a within-block-only adaptation for the
SheepShaver PPC→ARM64 JIT. Compiled 2026-06-02.

Sources verified against Dolphin `master`:
- `Source/Core/Core/PowerPC/JitArm64/JitArm64_Integer.cpp`
- `Source/Core/Core/PowerPC/JitArm64/Jit.h` (declarations)
- `Source/Core/Core/PowerPC/JitCommon/JitBase.h` (`CarryFlag` enum, `js.carryFlag`)

Our side: `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`.

> Note on the brief: the task referenced XER.CA at byte offset 902. That is stale —
> in this tree XER is the struct `{uint8 so, ov, ca, byte_count}` based at
> `PPCR_XER = 1028`, so **CA is at offset 1030** (`PPCR_XER_CA`, ppc-jit.cpp:255;
> so=1028, ov=1029, ca=1030, cnt=1031). All numbers below use 1030.

---

## 1. Verified Dolphin implementation

### CarryFlag state machine (`JitCommon/JitBase.h`)

```cpp
enum class CarryFlag
{
  InPPCState,
  InHostCarry,
#ifdef _M_X86_64
  InHostCarryInverted,
#endif
#ifdef _M_ARM_64
  ConstantTrue,
  ConstantFalse,
#endif
};
```

Tracked per-block in `JitState`:

```cpp
CarryFlag carryFlag;
```

The ARM64 view has exactly the 4 states the brief described: `InPPCState`
(value lives in `ppcState.xer_ca`), `InHostCarry` (value lives in the host C
flag from the last `ADDS`/`SUBS`), `ConstantTrue`, `ConstantFalse`.

### ComputeCarry — three overloads (`JitArm64_Integer.cpp`)

```cpp
void JitArm64::ComputeCarry(ARM64Reg reg) {
  js.carryFlag = CarryFlag::InPPCState;
  if (!js.op->wantsCA) return;                 // dead-carry: skip entirely
  if (CanMergeNextInstructions(1) && js.op[1].wantsCAInFlags) {
    CMP(reg, 1);                               // leave CA in host C for next op
    js.carryFlag = CarryFlag::InHostCarry;
  } else {
    STRB(IndexType::Unsigned, reg, PPC_REG, PPCSTATE_OFF(xer_ca));
  }
}

void JitArm64::ComputeCarry(bool carry) {
  js.carryFlag = carry ? CarryFlag::ConstantTrue : CarryFlag::ConstantFalse;
}

void JitArm64::ComputeCarry() {               // carry already in host C
  js.carryFlag = CarryFlag::InPPCState;
  if (!js.op->wantsCA) return;
  js.carryFlag = CarryFlag::InHostCarry;
  if (CanMergeNextInstructions(1) && js.op[1].opinfo->type == ::OpType::Integer)
    return;                                    // keep in flags for the next op
  FlushCarry();
}
```

### LoadCarry / FlushCarry

```cpp
void JitArm64::LoadCarry() {                   // make host C reflect current CA
  switch (js.carryFlag) {
  case CarryFlag::InPPCState: {
    auto WA = gpr.GetScopedReg();
    LDRB(IndexType::Unsigned, WA, PPC_REG, PPCSTATE_OFF(xer_ca));
    CMP(WA, 1); break; }
  case CarryFlag::InHostCarry: break;          // already there — zero cost
  case CarryFlag::ConstantTrue:  CMP(WZR, WZR); break;   // sets C
  case CarryFlag::ConstantFalse: CMN(WZR, WZR); break;   // clears C
  }
}

void JitArm64::FlushCarry() {                  // commit CA to ppcState
  switch (js.carryFlag) {
  case CarryFlag::InPPCState: break;
  case CarryFlag::InHostCarry: { auto WA = gpr.GetScopedReg();
    CSET(WA, CC_CS); STRB(.., WA, PPC_REG, PPCSTATE_OFF(xer_ca)); break; }
  case CarryFlag::ConstantTrue: { auto WA = gpr.GetScopedReg();
    MOVI2R(WA, 1); STRB(.., WA, ..); break; }
  case CarryFlag::ConstantFalse: STRB(.., WZR, ..); break;
  }
  js.carryFlag = CarryFlag::InPPCState;
}
```

### Producers / consumers and the CARRY_IF_NEEDED gate

```cpp
#define CARRY_IF_NEEDED_COND(carry, inst_no_carry, inst_with_carry, ...) \
  do { if ((carry) && js.op->wantsCA) inst_with_carry(__VA_ARGS__); \
       else inst_no_carry(__VA_ARGS__); } while (0)
```

```cpp
void JitArm64::addcx(...) {                    // producer
  CARRY_IF_NEEDED(ADD, ADDS, R(d), R(a), R(b));   // ADDS only if CA wanted
  ComputeCarry();
}
void JitArm64::addex(...) {                    // consumer + producer
  if (js.carryFlag == CarryFlag::ConstantFalse) {
    CARRY_IF_NEEDED(ADD, ADDS, R(d), R(a), RB);
  } else {
    LoadCarry();                               // get CA into host C
    CARRY_IF_NEEDED(ADC, ADCS, R(d), R(a), RB); // single ADCS does +rB+CA
  }
  ComputeCarry();
}
void JitArm64::subfex(...) {                   // mirror with SUB/SBC
  if (js.carryFlag == CarryFlag::ConstantTrue) CARRY_IF_NEEDED(SUB, SUBS, ...);
  else { LoadCarry(); CARRY_IF_NEEDED(SBC, SBCS, ...); }
  ComputeCarry();
}
```

Three structural levers: (a) `wantsCA` dead-carry elimination — if no later
instruction reads CA before the next write, **no CA store is emitted at all**;
(b) constant folding (`ConstantTrue/False`) when carry-in is statically known;
(c) `ADC`/`SBC` collapse the carry-in add into the arithmetic op itself rather
than a separate `LDRB + ADD`.

### Flush points (where lazy state cannot survive)

Dolphin's `FlushCarry()` is invoked at every block exit, before any helper call,
and before any instruction that clobbers the host flags — driven by the
`wantsCA`/`wantsCAInFlags` analysis precomputed during instruction-stream
decoding (PPCAnalyst). OE-form overflow is *not* tracked lazily; OE ops
`FALLBACK_IF(inst.OE)` to the interpreter (Lead 4).

---

## 2. Our current implementation (file:line, instruction counts)

GPRs are **memory-resident**: there is no GPR register cache. Every op begins
with `emit_load_gpr` (LDR from the regs struct, ppc-jit.cpp:439) and ends with
`emit_store_gpr` (STR, :444). This is the dominant cost per op and lazy carry
does **not** remove it.

The CA primitives (ppc-jit.cpp):
- `emit_read_xer_ca(rd)` :572 — `LDRB Wt,[RSTATE,#1030]` (1 insn)
- `emit_write_xer_ca_from_carry()` :577 — `CSET Wd,CS` + `STRB` (2 insns)
- `emit_set_xer_ca(val)` :584 — `MOVZ`/`MOV #1` + `STRB` (2 insns)

CA always materializes to memory immediately after every producer. Per-op CA
overhead (excluding the GPR LDR/STR and the arithmetic itself):

| Op | XO | line | CA-specific insns emitted | sequence |
|----|----|------|---------------------------|----------|
| subfc  | 8   | 1321 | 2 | `SUBS`(arith) + CSET + STRB |
| addc   | 10  | 1340 | 2 | `ADDS`(arith) + CSET + STRB |
| addco  | 522 | 1355 | 2 (+OV/SO ~5) | ADDS + CSET + STRB + OV/SO dance |
| subfco | 520 | 1365 | 2 (+OV/SO ~5) | SUBS + CSET + STRB + OV/SO dance |
| adde   | 138 | 1401 | **5** | ADDS + **MRS NZCV(dead)** + LDRB + ADD + CSET + STRB |
| addze  | 202 | 1431 | 3 | LDRB + ADDS + CSET + STRB |
| subfze | 200 | 1457 | 3 | MVN + LDRB + ADDS + CSET + STRB |
| addme  | 234 | 1414 | 4 | LDRB + ADD + ADD + LSR + STRB (64-bit bit-32 trick) |
| subfme | 232 | 1441 | 4 | MVN + LDRB + ADD + ADD + LSR + STRB |
| subfe  | 136 | 1329 | 3 | MVN + ADDS + LDRB + ADD + CSET + STRB |
| addic  | 12  | 2129 | 2 | ADDS + CSET + STRB |
| addic. | 13  | 2926 | 2 (+CR0) | ADDS + CSET + STRB + cr0 |
| subfic | 8(D)| 2227 | 2 | SUBS + CSET + STRB |

**Pre-existing-bug note (verify, do not fix here):** `adde` (case 138, :1401)
does `ADDS rA+rB`, then `MRS NZCV` into RTMP2 (the value is then *never read* —
dead), then `LDRB CA; ADD result+CA` (a plain non-flag ADD), then
`CSET CS;STRB`. The final CSET reads C from the *plain ADD*, which does not set
flags — so the recorded carry-out reflects neither sum correctly. Suspected
failing vector: rA=0xFFFFFFFF, rB=0, CA=1 (true CA_out=1). A lazy `ADCS` path
would be both faster and correct, which strengthens the case for this work — but
confirm with `SS_TEST_HEX` first and fix separately.

### Existing lazy-NZCV precedent (directly relevant)

`lazy_cr0_*` (:735–797) is a *scaffolded but DISABLED* lazy NZCV scheme for CR0.
`lazy_update_cr0()` (:768) currently calls `emit_update_cr0()` immediately;
`emit_materialize_cr0()` (:739) and the `lazy_flush_cr0()` flush points
(:995,1101,1117,1313,1793,2071,2288, and every branch/exit at 2372–2489) are all
wired up but inert. This is the template a lazy-carry scheme would copy.

**Why it was disabled (decisive for the verdict):** commit `98fd7989` ("restore
Gate 2 + disable lazy CR0/regalloc"). The root cause named is *not* lazy CR0
itself — it is a **partial-block truncation epilogue** that "corrupts state
during boot but doesn't affect the short-sequence opcode harness." Lazy CR0 and
register allocation were disabled *as containment* alongside the real culprit,
plus a separate regalloc bug ("load-side regalloc enabled while store-side
disabled → stale cache reads"). So the natural experiment is ambiguous: lazy
NZCV was never proven to be the direct cause, but it was bundled into a state-
corruption episode whose root cause (truncation epilogue) is **still open**
(LEARNINGS.md:27–29, TODO in the commit). Any new lazy state inherits that same
unresolved truncation-epilogue exposure.

---

## 3. Within-block lazy carry design for SheepShaver

Respecting docs/planning/JIT-STYLE-DECISION.md: lazy state **never crosses a block boundary**.
Mirror `lazy_cr0`'s shape with a compile-time-only state variable:

```c
enum { CA_IN_PPCSTATE, CA_IN_HOSTCARRY, CA_CONST_TRUE, CA_CONST_FALSE };
static int lazy_ca_state = CA_IN_PPCSTATE;   // reset to PPCSTATE at block entry
```

- **Producers** (`addc/subfc/addic/subfic/addco/subfco`): after the arithmetic
  `ADDS/SUBS`, set `lazy_ca_state = CA_IN_HOSTCARRY` instead of emitting
  `CSET+STRB`. Defer the store.
- **Consumers** (`adde/subfe/addze/subfze/addme/subfme`): emit a `LoadCarry`
  equivalent based on `lazy_ca_state`, then use **`ADCS`/`SBCS`** to fold the
  carry-in into the arithmetic in one instruction (replacing the current
  `LDRB + ADD` and the broken `adde` path), then set state to `CA_IN_HOSTCARRY`.
- **Flush** (`emit_flush_ca`): if `CA_IN_HOSTCARRY` → `CSET CS;STRB`; if a const
  → `MOV/WZR;STRB`; if `CA_IN_PPCSTATE` → nothing. Reset to `CA_IN_PPCSTATE`.

Because we have **no `wantsCA` lookahead** (no PPCAnalyst pass), the dead-carry
elimination lever is unavailable without adding a backward scan. The realistic
within-block win is (a) eliminating redundant STRB→LDRB round-trips in a carry
*chain* (adc/adde/adde…), and (b) the `ADCS`/`SBCS` collapse.

---

## 4. Flush-point inventory (which emit paths clobber the host C flag)

Any flag-setter (`ADDS/SUBS/CMP/CMN/CCMP`, `MSR NZCV`) destroys `CA_IN_HOSTCARRY`.
The lazy-CR0 flush sites are a complete map of "leaving the block"; lazy carry
needs the union of those **plus every intervening flag clobber**.

| Site | clobbers host C? | required before |
|------|------------------|-----------------|
| `emit_update_cr0` (`CMP Wn,#0` at :744 / cr0 build) | YES | flush CA, or order CA-store before CR0 |
| any `ADDS/SUBS` for the *next* arithmetic op | YES | consumer must `LoadCarry` first |
| `CMP/CMN` in compares (cmp/cmpl, 23 sites) | YES | flush CA before |
| `MSR NZCV` (none today writes; `MRS` at :1429,:2273 read-only) | n/a | — |
| `emit_epilogue_with_pc` (:698) — all block exits | leaves block | flush CA |
| every branch taken/not-taken (:2372–2489, paired w/ `lazy_flush_cr0`) | leaves block | flush CA |
| `lazy_flush_cr0` sites (:995,1101,1117,1313,1793,1820,2071,2288,2321,2372,2391,2403,2426–2489) | proxy for "exit/serialize" | flush CA at same points |
| helper/`BL` calls | none found via grep (no `0x94…`/`BLR` emit in arith paths) | flush CA before any future helper |
| FPSCR `MRS/MSR FPCR` (:656,:660) | no NZCV | — |
| `mtspr`/`mfspr` XER writes/reads | YES (direct CA memory) | flush CA before, reload after |

Practical rule: **flush CA at exactly the `lazy_flush_cr0()` call sites, and
additionally before any non-consumer flag-setter within the block.** Simplest
safe policy: only keep `CA_IN_HOSTCARRY` alive across an *immediately adjacent*
carry consumer; otherwise flush. This caps the optimization to true carry chains
and removes the clobber-tracking burden.

---

## 5. Verification plan

- **Single-opcode vectors (`SS_TEST_HEX`, `SS_TEST_JIT=1`) are the floor, not the
  gate.** They run one opcode and produce complete blocks, so they **cannot**
  exercise cross-instruction lazy state — the entire risk surface. Use them only
  to confirm each op in isolation (and to land the `adde` bug fix).
- **The real gate is rom-harness with multi-instruction blocks:**
  `./rom-harness/rom-harness <rom> --count=20000 --min-insns=4 --max-insns=64
  --stop-on-fail --passes=2`. This is the only path that generates
  addc→adde→adde chains and diffs JIT vs interpreter XER.CA.
- Add targeted hand-built blocks: a 3-deep `addc;adde;adde` 96-bit add; a
  `subfc;subfe` chain; a carry producer followed by a `cmpw` (flag-clobber) then
  a late `adde` reading the flushed CA.
- Full opcode harness must stay 209/209; boot to desktop must be re-verified
  (`make run-jit`), since the open truncation-epilogue bug interacts with any
  new block-scoped state.

---

## 6. Effort, risk, payoff

- **Effort:** Medium. The flush scaffolding already exists (clone `lazy_cr0`'s
  shape and flush sites). Touch ~12 op cases + add `emit_flush_ca` and a
  `LoadCarry` equivalent.
- **Payoff:** Bounded. GPRs stay memory-resident, so lazy carry removes only
  CA-specific traffic, not the dominant LDR/STR. Per-op: `adde` 5→~2 (and fixes
  the bug), `addc` 2→~1 within a chain, isolated ops unchanged. This "removes the
  CA dance"; it does **not** "collapse to register ops" (Dolphin's framing
  assumes a regcache we lack). XER.CA traffic must also be shown hot — rom-harness
  counters per XO (Lead 4 method) should confirm carry-chain frequency first.
- **Risk:** Medium-to-elevated, for one reason: the analogous lazy-NZCV scheme is
  disabled because of an **unresolved** partial-block truncation epilogue that
  corrupts boot-time state. New block-scoped lazy state lives in exactly that
  blast radius. Truncated/partial blocks must flush CA on the truncation path too.

---

## 7. Recommendation

**Do not enable lazy carry yet. Sequence it behind two prerequisites.**

1. First fix the `adde` carry-out bug (Section 2) with an immediate-writeback
   `ADCS` — correct, faster, and independently shippable. Verify with a crafted
   `SS_TEST_HEX` vector. This captures most of the per-op `adde` win with **zero**
   lazy-state risk.
2. Then resolve (or explicitly fence) the partial-block truncation epilogue that
   forced `lazy_cr0` off (LEARNINGS.md:27, commit 98fd7989 TODO). Until that root
   cause is closed, do not introduce a second block-scoped lazy state — it would
   re-open the same corruption class that the immediate-writeback rule was adopted
   to avoid.
3. Only after (1) and (2), and only if rom-harness XO counters show carry chains
   are hot, prototype within-block lazy CA **behind a compile flag**, gated on the
   rom-harness differential (`--min-insns=4`), never on single-opcode vectors.

This honors the immediate-writeback default (docs/planning/JIT-STYLE-DECISION.md): lazy carry
becomes "performance by earned sophistication," earned only after the truncation
bug is closed and profiling justifies it. Net order vs the other leads: Lead 2
(W^X counter) and the `adde` fix first; Lead 3 lazy carry stays last among the
medium-risk items.
