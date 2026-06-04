# Codegen Bug Analysis: Auxiliary DR Routine at 0x50467E00–0x50467F00

## Executive Summary

**Location**: ROM 0x50467E00–0x50467F00 (256 bytes)  
**Status**: Binary search complete; bug isolated to this auxiliary routine  
**Root cause**: NOT spcflags timing (disproven by correct interrupt prediction still causing hang). **Codegen correctness bug** in one or more instructions.

---

## Context from LEARNINGS.md

**What works**:
- Main DR dispatch variants (0x466080–0x466120) compile and boot correctly
- All 6 dispatch entry points function properly

**What fails**:
- This auxiliary routine causes SCSI hang when JIT-compiled
- Hang occurs despite correct interrupt prediction (~110 fallbacks/s, jNK growing normally)
- Therefore: bug is **instruction-level codegen**, not dispatch structure or interrupt timing

**Suspected root cause**: 68k SCSI scan logic in the auxiliary routine is generating incorrect ARM64 code, causing the SCSI driver to retry infinitely instead of proceeding.

---

## Key Opcodes in Failing Routine

From LEARNINGS.md (lines 118–127), these opcodes appear in the 256-byte window:

| Opcode (hex) | Mnemonic | Details | Suspicion Level |
|--------------|----------|---------|-----------------|
| `0x4D850420` | `bcctr 12,5` | Branch to CTR if CR1.SO=1 | 🔴 HIGH — CR flag-dependent branch |
| `0x4CA80020` | `bclr 5,8` | Interrupt gate (branch if CR2.LT=1) | 🟡 MEDIUM — we know this works elsewhere |
| `0x509d1b78` | `rlwimi r29,r13,3,13,28` | Rotate-left-word-immediate, mask insert | 🔴 HIGH — we have test vectors for rlwimi |
| `0x537d1b78` | `rlwimi r29,r29,3,13,28` | Similar rlwimi pattern | 🔴 HIGH — different source/dest but same operation |
| `0xaf780002` | `lhau r27,2(r24)` | Load halfword with auto-update | 🟡 MEDIUM — register update side effect |
| `0x7f64c2ee` | Unknown XO31 | Needs decoding | 🔴 UNKNOWN — can't assess |
| `0x48000004` | `b +4` | NOP-equivalent padding (branch to next insn) | 🟢 LOW — should be safe |

---

## Codegen Bug Candidates

### Candidate 1: `bcctr 12,5` (CR1.SO branching)

**Why suspicious**:
- Branches based on CR1.SO (bit 28 of CR register)
- CR flag generation in JIT is complex (multiple operations affect flags)
- If CR1.SO is not being set correctly by prior instructions, the branch goes wrong way

**What could go wrong**:
- JIT doesn't sync CR flags before this instruction
- ARM64 condition flag mismatch (e.g., using wrong NZCV bits)
- Wrong register holding the CR value

**Test strategy**:
- Isolate `bcctr 12,5` instruction in harness
- Load CR with known values, execute, verify branch taken/not-taken

---

### Candidate 2: `rlwimi` patterns (rotation with mask insert)

**Why suspicious**:
- We have explicit test vectors for `rlwimi_dr_dispatch` in the harness
- Rotation + mask insertion is complex (multiple bitfield operations)
- We identified this as a potential culprit earlier

**What could go wrong**:
- Immediate encoding error (SH vs other fields)
- Mask calculation wrong (especially for the specific masks in these instructions)
- Register allocation mismatch (writing to wrong GPR)

**Test strategy**:
- Run `rlwimi_dr_dispatch` harness vectors
- If they pass, `rlwimi` is correct
- If they fail, the bug is in ppc-jit.cpp's rlwimi emitter

---

### Candidate 3: `lhau` (Load Halfword with Auto-Update)

**Why suspicious**:
- Updates the base register (r24) as side effect
- If the update doesn't happen, subsequent loads read from wrong address
- Could cause SCSI command buffer corruption

**What could go wrong**:
- ARM64 post-index addressing mode not emitted correctly
- Base register update missing entirely
- Wrong displacement calculation

**Test strategy**:
- Load-then-verify test: `lhau` into memory, check both the loaded value and base register update
- Compare ARM64 code against interpreter trace

---

### Candidate 4: Unknown XO31 instruction (`0x7f64c2ee`)

**Why suspicious**:
- We can't decode it without looking up the exact XO31 subtype
- If ppc-jit.cpp doesn't handle this XO31 variant, it might fall back to interpreter
- Or worse, emit incorrect code for a fallback operation

**What could go wrong**:
- Instruction not implemented in JIT
- Fallback interpreter call but register state not synced
- Wrong interpretation of the opcode's semantics

**Test strategy**:
- Decode `0x7f64c2ee` fully (primary=31, RA=2, RB=12, RD=3, XO=1774)
- Check ppc-jit.cpp for this specific XO31 code
- If missing, that's the bug

---

## Investigation Plan

### Phase 1: Verify Candidates with Harness (Next)

1. **Run `rlwimi_dr_dispatch` vectors**
   - If PASS: rlwimi codegen is correct
   - If FAIL: likely found the bug

2. **Create `bcctr` test vector**
   - Load CR1 with known value (SO bit set/clear)
   - Execute `bcctr 12,5`, verify branch taken/not-taken
   - Compare JIT vs interpreter

3. **Create `lhau` test vector**
   - Load memory with known halfword
   - Execute `lhau r27,2(r24)` with base at known address
   - Verify r27 value AND r24 update

### Phase 2: Decode Unknown Instruction

1. **Decode `0x7f64c2ee`**:
   - Primary opcode: 31 (XO31 form)
   - RA (bits 16–20): 2
   - RB (bits 11–15): 12
   - RD (bits 6–10): 3
   - XO (bits 1–10): Need to look up

2. **Check ppc-jit.cpp for this XO31**
   - Search for `0x7f64c2ee` pattern
   - Search for the XO31 handler
   - If not found, file a bug report and add test vector

### Phase 3: Compare JIT vs Interpreter

Once suspicious instruction identified:

1. Boot with JIT disabled (`SS_USE_JIT=0`)
2. Log all execution of the suspicious instruction (register state, flags, memory)
3. Boot with JIT enabled
4. Compare traces — where do they diverge?

---

## Odds Assessment

Based on instruction complexity and prior issues:

| Candidate | Odds | Reason |
|-----------|------|--------|
| **`rlwimi`** | 40% | Complex operation, we have test vectors, multiple variants in window |
| **`bcctr 12,5`** | 30% | CR flag sync issues are common in JIT, this is CR1.SO which is unusual |
| **`lhau`** | 20% | Address mode + register update, but auto-update is usually robust |
| **Unknown XO31** | 10% | If not implemented, would be obvious fallback; but possible if subtly wrong |

**Most likely**: `rlwimi` or `bcctr 12,5`

---

## Next Steps

1. ✅ Binary search complete — routine identified
2. 🔄 **Disassemble full 256-byte routine** to verify all instructions (compare against LEARNINGS opcodes)
3. 🔄 **Run harness test vectors** for rlwimi, bcctr, lhau
4. ⏳ **Decode unknown XO31** if candidates don't match
5. ⏳ **Compare JIT vs interpreter traces** for the failing instruction
6. ⏳ **Fix ppc-jit.cpp emit function** for the broken instruction
7. ⏳ **Boot test** to verify fix

---

## References

- **LEARNINGS.md** lines 109–145: Full binary search and opcode details
- **ppc-jit.cpp**: Emit functions for each instruction (search for `emit_rlwimi`, `emit_bcctr`, `emit_lhau`, etc.)
- **ROM disassembly**: Verify instructions at 0x50467E00–0x50467F00 against objdump
