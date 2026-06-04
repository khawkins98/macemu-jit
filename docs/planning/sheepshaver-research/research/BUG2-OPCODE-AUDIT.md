# Bug #2 Opcode Audit: JIT Miscompilation Candidates

> **⚠️ ARCHIVED / HISTORICAL (2026-06-02 snapshot).** Kept for the audit methodology
> and the per-opcode reasoning only. "Bug #2" was since **resolved** — root-caused to
> the subfe/adde carry-out codegen bug (not the candidates first suspected here); see
> `LEARNINGS.md` and CLAUDE.md "Crorc Fix & Block Chaining". The figures below
> (3750-line ppc-jit.cpp, 209 interpreter-only vectors) are stale — the harness is now
> JIT-equivalence-gated (`make test-jit`, count via `make harness-count`). Do not treat
> the "candidates" as open bugs.

Date: 2026-06-02
Source: `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp` (3750 lines)
Harness: `SheepShaver/jit-test/run.sh` (209 vectors, interpreter-only)

---

## Critical Finding: The Harness Cannot Detect JIT Bugs

The test harness (`run.sh`) runs every vector **twice in interpreter mode** and
diffs the two REGDUMPs. It never runs JIT mode against interpreter mode. This
means `make test-opcodes` scoring 100 tells you **nothing about JIT
correctness**. Every existing vector (including carry-chain, shift-edge, and
CR-logic tests) only validates interpreter determinism.

To catch JIT bugs, vectors must be run with `SS_TEST_JIT=1` and compared
against `SS_TEST_JIT=0` output. Until this is implemented, all "coverage" is
illusory for the purposes of Bug #2.

---

## Opcode Histogram of the DR Region

The DR (68k) emulator code lives at ROM area offset 0x460000, constructed at
runtime by `rom_patches.cpp` (line 738: `memcpy(ROMBaseHost + ROM_SIZE,
ROMBaseHost + (ROM_SIZE - 0x100000), 0x100000)`). The entry point is set at
line 758: `LA_EmulatorCode = ROMBase + 0x460000`. The source code is copied
from ROM file offsets `0x300000-0x400000` to the ROM area at `0x400000-0x500000`.

However, the ROM file on disk (`1998-07-21 - Mac OS ROM 1.1.rom`) is only
0x1CFEF2 bytes (~1.9MB) -- a New World ROM, not the expected 4MB OldWorld ROM.
The source region at file offset 0x300000 is beyond the file, so the DR
emulator region **cannot be statically decoded** from this ROM file.

A scan of the entire 1.9MB ROM file (mixed code and data, all offsets decoded
as PPC) yields this approximate histogram:

### Primary opcodes (top 20)
```
 opc  name         count
   0  (data/illeg)  28501
  63  fp-double     17454
   8  subfic        13163
  16  bc            13140
  32  lwz           12994
  24  ori           12042
  31  XO31          11101
  12  addic         10693
  28  andi.         10552
   4  altivec       10213
  20  rlwimi         9861
  36  stw            9090
  48  lfs            8779
  44  sth            8758
  11  cmpi           8250
  18  b/bl           8204
  15  addis          7949
  40  lhz            7898
  14  addi           7610
  21  rlwinm         7373
```

### XO31 sub-opcodes (top 15)
```
  XO  name          count
 444  or/mr           747
 266  add             277
  32  cmpl            197
 467  mtspr           159
   0  cmp             157
   8  subfc           152
 954  extsb            83
 339  mfspr            82
  23  lwzx             58
  24  slw              56
 144  mtcrf            56
 922  extsh            53
  40  subf             50
  28  and              49
  10  addc             46
```

### OPC19 (CR/branch) sub-opcodes
```
  XO  name          count
  16  bclr            136
   0  mcrf             63
```

### Bug-candidate opcodes present in ROM
```
  adde  (XO31=138):    5
  subfe (XO31=136):   17
  crorc (XO19=417):    5
  slw   (XO31=24):    56
  srw   (XO31=536):   27
  sraw  (XO31=792):   28
```

**Caveat:** These counts are from the 1.9MB ROM file. The runtime-constructed
DR emulator code at offset 0x460000+ is not accessible for static scanning. The
bug patterns identified below are opcode-position-independent and apply wherever
these instructions appear in JIT-compiled code.

---

## Bug Candidate A (HIGH): crorc Missing AND #1 Mask -- Root Cause of Bug #2

**File:** ppc-jit.cpp line 2452
**Opcode:** crorc (opcode 19, XO=417) -- `crbD = crbA | ~crbB`
**Status:** BROKEN in this worktree (fix exists in commit 485f9860, 73 commits
ahead of HEAD on macos-arm64)

```c
case 417: /* crorc:  a | ~b */
    emit32(0x2A200000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* ORN */ break;
```

ARM64 ORN computes `Wn OR NOT Wm` over the full 32-bit register. With 1-bit
inputs (0 or 1), the NOT produces 0xFFFFFFFE or 0xFFFFFFFF. The downstream
merge (lines 2462-2467) clears only the single destination bit in CR, then ORs
the full 32-bit result -- corrupting the entire CR word.

**This is the documented root cause of Bug #2.** Commit 485f9860 confirms the
causal chain: crorc in the DR emulator's context-switch code at ROM+0x46e100
corrupted CR, which propagated through 68k context save/restore. The fix adds
`AND #1` after ORN, matching the existing pattern in crnor/creqv/crnand.

**Comparison with siblings:**
- crnor (line 2437): ORR + MVN + **AND #1** -- correct
- creqv (line 2445): EOR + MVN + **AND #1** -- correct
- crnand (line 2453): AND + MVN + **AND #1** -- correct
- crandc (line 2444): BIC -- correct (BIC with 1-bit inputs stays 1-bit)
- crorc (line 2452): ORN -- **MISSING AND #1**

**Fix:** Add `AND #1` after the ORN:
```c
case 417: /* crorc:  a | ~b */
    emit32(0x2A200000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* ORN */
    emit_load_imm32(RTMP2, 1);
    emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* AND #1 */
    break;
```

---

## Bug Candidate B (HIGH): adde/subfe Carry-Out Computed from Wrong Stage

**File:** ppc-jit.cpp lines 1185-1197 (adde), lines 1166-1176 (subfe)
**Opcodes:** adde (XO31=138), subfe (XO31=136)

This is a **separate** real miscompilation from Bug #2 (crorc). It may or may
not contribute to the A3 register corruption symptom, but it is a confirmed
divergence between JIT and interpreter.

### adde (rD = rA + rB + XER.CA, set XER.CA)

```c
case 138: /* adde */
    emit_load_gpr(RTMP0, ra);
    emit_load_gpr(RTMP1, rb);
    emit32(0x2B000000 | ...);   /* ADDS rA+rB  -- sets C flag (C1) */
    emit32(0xD53B4200 | RTMP2); /* MRS NZCV    -- saves C1, DEAD CODE */
    emit_read_xer_ca(RTMP1);
    emit32(0x0B000000 | ...);   /* ADD +CA     -- does NOT set flags */
    emit_store_gpr(RTMP0, rd);
    emit_write_xer_ca_from_carry(); /* reads STALE C1, not true carry-out */
```

The problem: `0x0B000000` is ADD (no flag update), not `0x2B000000` (ADDS).
The `MRS NZCV` at line 1190 saves the flags but is never restored -- dead code.
`emit_write_xer_ca_from_carry()` reads the C flag from the first ADDS (rA+rB),
ignoring the second addition (+CA).

### Confirmed interpreter divergence

The interpreter (`ppc-execute.cpp` line 122-126) computes carry correctly:
```cpp
template<> struct op_carry<op_add> {
    static inline bool apply(uint32 a, uint32 b, uint32 c) {
        uint64 carry = (uint64)a + (uint64)b + (uint64)c;
        return (carry >> 32) != 0;
    }
};
```

This is a full 3-operand carry: `(a + b + c) > 0xFFFFFFFF`. The JIT only
checks carry from `a + b`, which diverges when `a + b + c` wraps but `a + b`
does not (or vice versa).

**Concrete failure case:** rA=0x7FFFFFFF, rB=0x80000000, CA=1.
- ADDS: 0x7FFFFFFF + 0x80000000 = 0xFFFFFFFF, C1=0
- True result: 0xFFFFFFFF + 1 = 0x00000000, true carry=1
- JIT writes XER.CA = C1 = **0** (wrong, should be 1)

The **result value** (rD) is correct; only carry-out is wrong.

### subfe has the same bug (lines 1166-1176)

ADDS at line 1170, then plain ADD at line 1172, then
`emit_write_xer_ca_from_carry()` reads stale C flag.

### Correctly-implemented siblings (for contrast):
- **addme** (line 1198): ADDS + ADCS chain -- correct
- **addze** (line 1224): single ADDS -- correct
- **subfme** (line 1234): ADDS + ADCS chain -- correct
- **subfze** (line 1247): single ADDS -- correct

**Fix:** Use ADDS+ADCS chain (matching addme/subfme pattern):
```c
case 138: /* adde rD,rA,rB (rD = rA + rB + CA) */
    emit_load_gpr(RTMP0, ra);
    emit_load_gpr(RTMP1, rb);
    emit_read_xer_ca(RTMP2);
    emit32(0x2B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADDS rA+rB */
    emit32(0x3A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* ADCS +CA */
    emit_store_gpr(RTMP0, rd);
    emit_write_xer_ca_from_carry();
    if (op & 1) lazy_update_cr0(RTMP0);
    return true;
```

---

## Bug Candidate C (MEDIUM): slw/srw Shift Count >= 32

**File:** ppc-jit.cpp lines 1064-1078
**Opcodes:** slw (XO31=24), srw (XO31=536)

PPC `slw`: if shift count (rB[0:5], 6 bits) >= 32, result is 0.
ARM64 `LSLV Wd,Wn,Wm`: shift count is taken mod 32 from Wm[0:4] (5 bits).

So `slw r3,r4,r5` where r5=32: PPC produces 0, ARM64 produces r4 unchanged.

```c
case 24: /* slw rA,rS,rB */
    emit_load_gpr(RTMP0, PPC_RS(op));
    emit_load_gpr(RTMP1, rb);
    emit32(0x1AC02000 | ...); /* LSL Wd,Wn,Wm -- count mod 32! */
    emit_store_gpr(RTMP0, ra);
    return true;
```

**Existing test vectors:** `fuzz_slw_32` and `fuzz_srw_32` test exactly this
case, but only run in interpreter mode.

**Fix:** Test bit 5 of shift amount; if set, force result to 0. Or use 64-bit
shift (which correctly produces 0 when count >= 32 for a 32-bit value in Xn).

---

## Bug Candidate D (MEDIUM): sraw Shift Count >= 32

**File:** ppc-jit.cpp lines 1080-1107
**Opcode:** sraw (XO31=792)

Comment at line 1087 claims "ARM64 ASR with shift>=32 already produces
sign-extended result" -- this is **incorrect** for `ASRV Wd,Wn,Wm` (32-bit
variant), which takes count mod 32. Shift by 32 gives shift by 0 (no shift),
not the all-ones/all-zeros that PPC requires.

PPC `sraw` with shift >= 32: result = all sign bits of rS, CA = (rS < 0).

---

## Bug Candidate E (LOW): srad/sradi XER.CA Hardcoded to 0

**File:** ppc-jit.cpp lines 1700-1717
**Opcodes:** srad (XO31=794), sradi (XO31=826)

Both emit `emit_set_xer_ca(0)` with comment "simplified". This breaks carry
chains involving 64-bit arithmetic shifts. G5/PPC970 instructions; unlikely in
OldWorld ROM but possible in emulated OS code.

---

## Opcodes with Zero Harness Coverage

These are JIT-compiled opcodes that appear in NO test vector. Many are
load/store update-indexed forms, FP indexed forms, 64-bit ops, or CR logicals.

### CR logicals (OPC19, no test vectors):
- crnor (XO=33), crandc (XO=129), crnand (XO=225), crorc (XO=417), bcctr (XO=528)

### Load/store update-indexed (XO31, no test vectors):
- lbzux (119), stbux (247), lhzux (311), sthux (439), lhaux (375),
  lwzux (55), stwux (183)

### FP indexed (XO31, no test vectors):
- lfsx (535), lfsux (567), lfdx (599), lfdux (631),
  stfsx (663), stfsux (695), stfdx (727), stfdux (759)

### FP update (primary, no test vectors):
- lfsu (49), lfdu (51), stfsu (53), stfdu (55)

### FPSCR management (XO63, no test vectors):
- mtfsfi (711), mtfsb0 (70), mtfsb1 (38), mtfsf (134), mcrfs (64)

### 64-bit ops (no test vectors):
- ld (58), std (62), rld* (30), ldx (21), ldux (53), stdx (149),
  stdux (181), sld (27), srd (539), srad (794), sradi (826),
  cntlzd (58), extsw (986), mulld (233), divdu (457), divd (489),
  ldarx (84), stdcx. (214)

### String load/store (XO31, no test vectors):
- lswi (597), stswi (725)

### Misc (no test vectors):
- lwarx (20), stwcx. (150), mfmsr (83), mfsr (595), mfsrin (659),
  eciwx (310), ecowx (438), dst (342), dstst (374), dss (822)

---

## Proposed New Test Vectors

All vectors below MUST be run with `SS_TEST_JIT=1` and compared against
interpreter output (`SS_TEST_JIT=0`) to detect JIT divergence.

### Priority 1: adde carry-out (Bug Candidate B)

```
# Setup: r3=0xFFFFFFFF, r4=1, r5=0. addc sets CA=1, then adde should propagate.
# addc r6,r3,r4 -> r6=0, CA=1. adde r7,r5,r5 -> r7=0+0+1=1, CA=0.
# Hex: 3860FFFF 38800001 38A00000 7CC31814 7CE52914
# Expected: r7=1, XER.CA=0
# JIT bug: CA=1 (stale from addc's ADDS) instead of 0

# Edge case: adde where a+b wraps but a+b+c doesn't carry
# r3=0x7FFFFFFF, r4=0x80000000. addc r6,r3,r4 -> r6=0xFFFFFFFF, CA=0.
# adde r7,r3,r4 -> 0xFFFFFFFF+0=0xFFFFFFFF, CA should be 0.
# This case is correct in JIT because CA_in=0.

# Edge case: adde where a+b doesn't wrap but a+b+c does
# Need CA=1 going in, and a+b=0xFFFFFFFF.
# li r3,0; subfic r3,r3,0 -> r3=0, CA=1. Then: li r4,-1; li r5,0; adde r6,r4,r5
# r6 = 0xFFFFFFFF + 0 + 1 = 0, true CA=1. JIT: ADDS(0xFFFFFFFF,0)=C=0, wrong.
# Hex: 38600000 20630000 3880FFFF 38A00000 7CC52114
# Expected: r6=0, XER.CA=1
# JIT produces: r6=0, XER.CA=0 (WRONG)
```

### Priority 2: crorc all truth-table entries (Bug Candidate A)

```
# crorc crbD,crbA,crbB = crbA | ~crbB
# Test: cr bit 0 = 0, cr bit 1 = 0 -> crorc 2,0,1 = 0 | ~0 = 1
# Need: set CR bits precisely, apply crorc, read CR via mfcr
# (Existing vectors crand_basic/crxor_cror test some CR ops
#  but crorc is untested even in interpreter mode)
```

### Priority 3: slw by 32 (Bug Candidate C)

```
# lis r3,0x1234; ori r3,r3,0x5678; li r4,32; slw r5,r3,r4
# Hex: 3C601234 60635678 38800020 7C651830
# Expected: r5=0 (PPC: shift >= 32 clears result)
# JIT produces: r5=0x12345678 (ARM64: shift 32 mod 32 = 0)
```

---

## Summary

| ID | Severity | Opcode | Bug | Lines | Explains Bug #2? |
|----|----------|--------|-----|-------|-------------------|
| A | HIGH | crorc | Missing AND #1 after ORN | 2452 | YES (confirmed) |
| B | HIGH | adde/subfe | Carry-out from stale ADDS | 1185-1197, 1166-1176 | Possible (separate) |
| C | MEDIUM | slw/srw | ARM shift mod 32 vs PPC >= 32 | 1064-1078 | Unlikely |
| D | MEDIUM | sraw | Same mod-32 issue, false comment | 1080-1107 | Unlikely |
| E | LOW | srad/sradi | XER.CA hardcoded 0 | 1700-1717 | No (G5 only) |

**Candidate A (crorc)** is the documented root cause of Bug #2 per commit
485f9860. The fix exists but is not yet in this worktree.

**Candidate B (adde/subfe)** is a confirmed interpreter-vs-JIT divergence that
produces correct results but wrong carry flags. This is a real bug that could
cause silent pointer corruption in multi-word arithmetic chains. It is NOT the
same bug as Bug #2 but should be fixed regardless.

**Candidates C/D (slw/srw/sraw)** produce wrong results for shift counts >= 32,
which is uncommon but not impossible in ROM code. The harness has vectors for
these edge cases but can only detect the bug once JIT-vs-interpreter comparison
is implemented.
