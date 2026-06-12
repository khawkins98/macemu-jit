> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** historical research
>

# Lead 5 — Dolphin/PPSSPP Arm64Emitter: vendor or crib?

> **Status:** 📖 Reference / archive · **Created:** 2026-06-02 · **Updated:** 2026-06-04
> **Why this doc exists:** Research lead: vendor vs crib Dolphin/PPSSPP Arm64Emitter — verdict in backlog.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../../CONTRIBUTING.md) → "Documentation Lifecycle")._


Deep-dive on whether to vendor Dolphin's `Arm64Emitter` or use it as a reference for our
hand-written SheepShaver PPC→ARM64 emitter. Sources fetched 2026-06-02 from
`raw.githubusercontent.com/dolphin-emu/dolphin/master`:

- `Source/Core/Common/Arm64Emitter.h` (1495 lines)
- `Source/Core/Common/Arm64Emitter.cpp` (4474 lines)

Our side (branch `macos-arm64`):

- `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp` (4133 lines) — the JIT
- `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-codegen-aarch64.h` (259 lines) — the `a64_*` primitives
- `ppc-jit.h` (43 lines)

---

## 1. Verified Dolphin emitter capabilities

Two classes plus support types, all in one self-contained header/impl pair.

**`ARM64XEmitter`** (integer/branch/system) — ~265 `void`-returning emit methods
(`Arm64Emitter.h:603-1204`). Coverage:

- Full data-processing: ADD/SUB/ADC/SBC (+S forms), AND/ORR/EOR/BIC/ORN/EON, MUL/MADD/
  MSUB/SMULL/UMULL/SDIV/UDIV, shifts (LSL/LSR/ASR/ROR), bitfield (SBFM/UBFM/BFM + aliases
  UBFX/SBFX/BFI/UBFIZ), extends, REV/REV16/REV32/RBIT/CLZ.
- Conditional: `CSEL`/`CSINC`/`CSINV`/`CSNEG` and the aliases `CSET`/`CSETM`/`CINC`
  (`Arm64Emitter.h:787-810`).
- Branches with a typed `FixupBranch` mechanism (see §below): `B(cond)`, `CBZ`/`CBNZ`,
  `TBZ`/`TBNZ`, all `[[nodiscard]] FixupBranch`, resolved by `SetJumpTarget()`
  (`Arm64Emitter.h:697-704`).
- System: MRS/MSR, barriers (DMB/DSB/ISB), cache (`_CLREX`), HINT/BRK/NOP.
- ABI helpers: `ABI_PushRegisters`/`ABI_PopRegisters` (BitSet32-driven), and variadic
  `ABI_CallFunction(func, args...)` / `ABI_CallLambdaFunction` that marshal args into
  X0..X7 automatically (`Arm64Emitter.h:1075-1204`).

**`ARM64FloatEmitter`** (NEON/FP) — ~196 emit methods (`Arm64Emitter.h:1205-1474`):
FADD/FMUL/FSUB/FDIV/FMLA/FNMLS, FABS/FNEG/FSQRT, FCMP/FCSEL, FCVT family, FMOV,
LD1/ST1/LDP/STP (vector), DUP/INS/UMOV/SMOV, REV16/32/64, TBL, MIN/MAX (S/U/F),
FRECPE/FRSQRTE (the reciprocal-estimate ops paired-single code needs), BSL, MOVI/BIC.
This is full AdvSIMD coverage — directly usable for AltiVec/VMX someday.

**`LogicalImm` encoder** (`Arm64Emitter.h:525-601`) — the load-bearing piece for us.
`constexpr LogicalImm(u64 value, GPRSize size)` decomposes any value into the ARM64
bitmask-immediate `(N, immr, imms)` triple, or sets `valid=false`. Algorithm: for 32-bit,
duplicate the value into both halves; reject all-zeros/all-ones; normalize by rotating LSB
to bit 0 (`std::countr_zero(value & (value+1))`); derive element size and run length; verify
the pattern actually repeats (`rotr(value, element_size) == value`); pack into `r`, `s`, `n`.
Modern, branch-light, uses `<bit>` intrinsics. Consumed by `AND/ANDS/EOR/ORR/TST(reg, LogicalImm)`
(`Arm64Emitter.h:912-916`) and the higher-level `ANDI2R`/`ORRI2R`/`EORI2R` wrappers
(`.cpp:4084-4160`) that fall back to MOVI2R+scratch only when no logical-imm encoding exists.

**`MOVI2R` constant materialization** (`.cpp:1835-1993`) — best-of search across five
approaches: MOVZ-base, MOVN-base, ADR, ADRP (+ optimized ADRP+ADD pairing), and ORR-from-ZR
via a `LogicalImm`. It enumerates the 16-bit parts that differ from each base, counts
instructions required, and picks the minimum. Result: many 64-bit constants materialize in
1-2 instructions where a naive MOVZ+3×MOVK would take 4. `TryORRI2R`/`TryADDI2R`/`TryCMPI2R`
(`.cpp:4366-4420`) expose the single-instruction-if-possible variants.

**`FixupBranch`** (`Arm64Emitter.h:359`, struct) — forward-branch record holding the emit
location + branch type; `SetJumpTarget(fb)` back-patches the offset when the target PC is
known. This is the clean version of the open-coded `skip_loc`/`skip_off` pattern we hand-roll.

**Dependency tail** (the real cost of vendoring; `.h:14-22`, `.cpp:17-21`):
`Common/ArmCommon.h` (CCFlags enum), `Assert.h` (ASSERT macros), `BitSet.h` (BitSet32, used
pervasively by ABI_Push/Pop), `BitUtils.h` (`ExtractBit`, used by LogicalImm), `CodeBlock.h`
(`ARM64CodeBlock` base — buffer mgmt, `ARM64XEmitter` inherits write-pointer state from here),
`Common.h`/`CommonTypes.h` (u8/u32/u64 typedefs), `MathUtil.h`, `SmallVector.h` (used by
MOVI2R and ABI marshalling). Vendoring the whole emitter pulls in 6-8 Common headers and an
`ARM64Reg`-typed API (`enum class ARM64Reg`) that does not match our `int`-register convention.

---

## 2. Our emitter inventory

Two-layer, hand-written, no external deps. ~33 primitives + ~30 PPC-level emit helpers.

**Low level — `ppc-codegen-aarch64.h` (33 `a64_*` primitives):**
- Core: `emit32(insn)` raw word writer (`:18`); `a64_mov_reg`, `a64_movz`, `a64_movk` (`:69-80`).
- ALU reg-reg only: `a64_add/adds/sub/subs/and/orr/eor_reg` (`:84-114`). **No immediate
  forms, no CSEL/CSET, no MUL/DIV, no shift/bitfield primitives.**
- Mem: `a64_ldr/str_imm` (X), `_w_imm` (W), `_w_reg`/`_x_reg`/`ldrb/strb/ldrh/strh_reg` (`:119-179`).
- Branch: `a64_b`, `a64_b_cond`, `a64_blr`, `a64_br`, `a64_ret`, `a64_nop` (`:184-225`).
- Frame/cache: `a64_stp_pre`/`ldp_post`, `a64_ic_ivau`/`dc_cvau`/`dsb_ish`/`isb` (`:230-255`).

**Mid level — `ppc-jit.cpp` emit helpers (~30):** `emit_load/store_gpr[64]`, `emit_load_fpr/
store_fpr`, `emit_load/store_vr` (`:657-664`), `emit_load_imm32` (`:510`), `emit_load_imm64`
(`:494`), `emit_lsl64_imm`/`emit_lsr64_imm` (`:480-491`, hand-built UBFM literals),
`emit_update_cr0`, `emit_read/write_xer_ca/so/ov`, `emit_sync_fpscr_rounding`, branch/
epilogue helpers.

**How we encode logical immediates today: we don't.** Every masked op materializes the mask
into a scratch register and does a register-register AND. Concretely in `rlwinm`
(`ppc-jit.cpp:2154-2164`): build the 32-bit mask in C, `emit_load_imm32(RTMP1, mask)`
(1-2 MOVZ/MOVK), then `emit32(0x0A000000 | ...)` (`AND Wd,Wn,Wm`). `rlwimi`
(`:2183-2197`) does it **twice** (mask and `~mask`) — 4-6 instructions plus the OR. There is
no `LogicalImm`/bitmask encoder anywhere in the tree (grep for `bitmask|logical.?imm|encode_imm`
returns nothing).

**How we materialize constants:** `emit_load_imm64` (`:494-502`) = MOVZ at first nonzero
16-bit lane + MOVK for each remaining nonzero lane (up to 4 instructions, no MOVN/ADRP/ORR
optimization). `emit_load_imm32` (`:510`) special-cases small positive values, else MOVZ+MOVK.

**How we handle branch fixups:** open-coded. We stash `uint32_t *skip_loc = jit_code_ptr`,
emit a placeholder, then back-patch with `*skip_loc = 0x34000000 | (((skip_off>>2)&0x7FFFF)<<5) | reg`
for CBZ etc. (`ppc-jit.cpp:2347-2416`). Repeated literal-encoding inline, no shared helper.

**NEON/FP:** no primitives at all. AltiVec/VMX paths emit NEON via raw `emit32(literal)`
(152 hits for `0x4E/0x6E/...`-style words and `VR_*` accessors in `ppc-jit.cpp`).

---

## 3. Side-by-side comparisons

### (a) Logical immediate — `rlwinm rA,rS,0,0,28` (mask `0xFFFFFFF8`)

Our code (`ppc-jit.cpp:2154-2164`):
```
emit_load_imm32(RTMP1, 0xFFFFFFF8);  // MOVZ + MOVK   (2 insns)
emit32(0x0A000000 | (RTMP1<<16) | (RTMP0<<5) | RTMP0);  // AND Wd,Wn,Wm
```
3 instructions + clobbers a scratch register.

Dolphin (`ANDI2R`, `.cpp:4084-4119`): `0xFFFFFFF8` is a contiguous run → `LogicalImm` valid →
emits a single `AND Wd,Wn,#0xFFFFFFF8`. **1 instruction, no scratch.** Every `rlwinm` mask is
by construction a (rotated) contiguous run of ones — exactly the bitmask-immediate domain —
so essentially all of them encode in one instruction.

### (b) Constant materialization — load `0x4000_0000_0000` (NATMEM_OFFSET)

Ours (`emit_load_imm64`, `:494`): lanes are `{0,0,0x4000,0}` → MOVZ #0x4000,lsl#32. Here 1
insn (only one nonzero lane), so we match. But for e.g. `0xFFFF_FFFF_FFFF_0000` ours emits
MOVZ+MOVK+MOVK+MOVK (4); Dolphin's MOVN-base path emits `MOVN x,#0xFFFF` (1).

### (c) Conditional select / CSET

Ours: no CSEL/CSET primitive. Boolean results are produced via compare-then-conditional-branch
+ materialize, or via the `skip_loc` back-patch dance (`:2347+`). Dolphin: `CSET(Rd, cond)`
(`Arm64Emitter.h:793`) is one instruction, branch-free — the idiomatic way to turn a CR/XER
bit into a 0/1 GPR value. We currently can't express it without adding the primitive.

### (d) NEON for future AltiVec

Ours: zero NEON primitives; VMX ops hand-emit raw words. Dolphin `ARM64FloatEmitter` has the
full set (FMLA, FRECPE/FRSQRTE for paired-single, LD1/ST1, DUP/INS, TBL, MIN/MAX). If/when we
implement VMX in the JIT, this is the single biggest body of reusable encoding logic.

---

## 4. Gaps in our emitter

1. **No logical-immediate encoder.** Biggest concrete inefficiency. Hits every `rlwinm`/
   `rlwimi`/`andi.`/`ori`/`andis.` — among the hottest PPC idioms. Costs an extra
   instruction and a scratch-register clobber each time.
2. **Suboptimal constant materialization** — no MOVN/ADRP/ORR-from-bitmask paths; worst case
   4 instructions where 1-2 suffice.
3. **No CSEL/CSET** — forces branchy boolean materialization.
4. **No shift/bitfield/MUL/DIV primitives** — open-coded as `emit32(literal)` throughout
   (`emit_lsl64_imm`, ASR in cmp paths, EXTR for rotate at `:2151`).
5. **No reusable FixupBranch** — branch back-patching is copy-pasted literal encoding.
6. **No NEON primitives** — VMX is all raw literals.

None of these are correctness bugs (the harness passes at 100); they are density/clarity costs.

---

## 5. Vendor-vs-reference analysis

**Wholesale vendoring: not worth it.** The emitter is `ARM64Reg`-typed and `CodeBlock`-based;
our JIT is `int`-register and owns its own `jit_code_ptr`. Adopting it means either rewriting
all ~4100 lines of `ppc-jit.cpp` to the Dolphin API, or building an adapter shim — plus
vendoring 6-8 `Common/` headers (BitSet32, SmallVector, Assert, BitUtils, CodeBlock, ArmCommon,
CommonTypes, MathUtil). High effort, and it discards a working, debugged, harness-validated
emitter. License is fine (GPL-2.0-or-later into GPLv2); the dependency tail and API mismatch
are the blockers, exactly as Lead 5 predicted.

**Targeted cribbing: high value, low risk.** Port the *algorithms*, re-typed to our `int`-reg
`a64_*` convention, no Dolphin headers pulled in:

| Crib | From | Effort | Payoff |
|------|------|--------|--------|
| `LogicalImm(value,size)` decoder → `a64_try_logical_imm()` + `a64_and/orr/eor_imm()` + `emit_andi2r()` | `Arm64Emitter.h:525-601`, `.cpp:4084-4160` | ~60 lines, self-contained, needs only `std::countr_zero/one`+`rotr` (C++20 `<bit>`) | Collapses rlwinm/rlwimi/andi./ori to 1 insn; removes scratch clobbers |
| `MOVI2RImpl` best-of search | `.cpp:1835-1983` | medium; ADRP path needs care vs MAP_JIT addresses | Tighter constant loads; lower priority |
| `FixupBranch` + `SetJumpTarget` | `.h:359`, branch section | small | Replaces `skip_loc` copy-paste; clarity |
| `ARM64FloatEmitter` NEON encoders | `.h:1205-1474` | large | Only when VMX/AltiVec JIT is on the roadmap |

The `LogicalImm` decoder is `constexpr` and dependency-free apart from `Common::ExtractBit<6>`
(trivially `(x>>6)&1`) — it ports almost verbatim. This is the one piece worth lifting now.

---

## 6. Recommendation

**Reference, not vendor.** Keep our hand-written emitter. Do one targeted crib now:

1. **Port the `LogicalImm` bitmask encoder** (`Arm64Emitter.h:525-601`) into
   `ppc-codegen-aarch64.h` as `a64_try_logical_imm(uint64_t val, int is64, uint32_t *enc)`,
   add `a64_and_imm/orr_imm/eor_imm` primitives, and an `emit_andi2r(rd,rn,imm,scratch)`
   helper mirroring Dolphin's fallback (`.cpp:4084-4119`). Then rewrite the mask paths in
   `rlwinm` (`ppc-jit.cpp:2161-2164`) and `rlwimi` (`:2192-2197`) to use it. Validate with the
   209-vector opcode harness (must stay at score 100) and rom-harness. Small, contained,
   immediately removes 1-3 instructions + a scratch clobber from the hottest mask ops.

2. **Defer** MOVI2R search, FixupBranch refactor, and NEON — file the function names/URLs
   above and revisit FixupBranch opportunistically and NEON when VMX JIT work begins.

Do not pull any `Common/` headers; port algorithms re-typed to our `int`-register API.

URLs for future cribbing:
- LogicalImm: https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Common/Arm64Emitter.h (lines ~525-601)
- ANDI2R/ORRI2R/MOVI2R: https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Common/Arm64Emitter.cpp (ANDI2R ~4084, MOVI2RImpl ~1835)
- PPSSPP second copy (same family): https://github.com/hrydgard/ppsspp/blob/master/Common/Arm64Emitter.cpp
