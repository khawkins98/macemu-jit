# PPC subfe Carry-Out Bug on ARM64: Root Cause Report

> ## ✅ RESOLVED (2026-06-03) — do NOT re-investigate
> This bug is **fixed and shipped.** The `subfe`/`adde` carry-out is now computed with a single
> `ADCS` (CMP loads CA into the host carry flag, then `ADCS` for the full three-operand sum) —
> `ppc-jit.cpp` cases 136/138; `docs/planning/OPTIMIZATION-PLAN.md` §0b is **DONE**. The infinite
> SCSI-scan boot hang is gone: SheepShaver boots Mac OS 8.6 to the Finder desktop with the full
> 68K DR-emulator region JIT-compiled (ROM=0x500000, no skip list). This report is kept as the
> durable root-cause record.

## Summary

A carry-flag computation error in the PPC→ARM64 JIT's `subfe` instruction caused the Mac ROM's built-in 68k emulator to malfunction when JIT-compiled, producing an infinite SCSI scanning loop during boot. The fix is straightforward but the bug was subtle — it only manifests for specific input patterns and corrupts downstream state silently.

**Affected**: Any PPC→ARM64 JIT that uses the two-step `ADDS + ADD` pattern for `subfe`.  
**NOT Apple Silicon specific**: the ARM64 ADDS behavior is architectural (correct per spec).

---

## The Instruction

```
subfe rD, rA, rB    ; rD = ~rA + rB + CA; CA_out = carry of full sum
```

PPC's `subfe` (subtract from extended) computes a three-operand unsigned sum and sets the carry-out flag (XER.CA) from the 33rd bit. It's commonly used in multi-precision arithmetic chains and as the idiom `subfe rX,rX,rX` (carry-to-mask: produces 0 if CA=1, -1 if CA=0).

## The Bug

The naive ARM64 codegen:
```asm
MVN   W0, Wa        ; W0 = ~rA
ADDS  W0, W0, Wb    ; W0 = ~rA + rB; sets ARM64 C flag
; read old CA...
ADD   W0, W0, Wca   ; W0 = ~rA + rB + CA (no flag update)
; write CA from ARM64 C flag ← BUG: this is carry of (~rA+rB), not (~rA+rB+CA)
CSET  Wt, CS        ; Wt = ARM64 carry (from ADDS only)
STRB  Wt, [xer_ca]
```

The ARM64 carry flag after `ADDS W0, W0, Wb` reflects whether `~rA + rB >= 2^32`. The subsequent `ADD W0, W0, Wca` does NOT update flags. So the stored CA reflects the partial sum, ignoring the +CA contribution.

**For `subfe rX,rX,rX`**: `~rX + rX = 0xFFFFFFFF` (always, for any X). This never carries on ARM64. So CA_out is always written as 0, regardless of CA_in.

Correct behavior: CA_out should equal CA_in (since 0xFFFFFFFF + 0 = 0xFFFFFFFF < 2^32, but 0xFFFFFFFF + 1 = 0x100000000 >= 2^32).

## The Fix

Compute the full sum in 64 bits to get the exact carry-out:

```asm
MVN   W0, Wa          ; W0 = ~rA (32-bit)
UXTW  X0, W0          ; zero-extend to 64-bit
UXTW  X1, Wb          ; zero-extend rB to 64-bit  
ADD   X0, X0, X1      ; X0 = (uint64)(~rA) + (uint64)(rB)
; read old CA into W1...
ADD   X0, X0, X1      ; X0 = full 64-bit sum including CA
LSR   X1, X0, #32     ; X1 = bit 32 = carry-out
STRB  W1, [xer_ca]    ; store correct CA
MOV   W0, W0          ; truncate result to 32-bit (implicit on ARM64 W-reg write)
```

This produces the correct carry for all input combinations, not just the `subfe rX,rX,rX` idiom.

## Why It Matters for 68k Emulation

The Mac ROM's built-in 68k emulator (the "DR emulator" at ROM offsets 0x460000-0x500000) uses `subfe` in its address calculation chains — computing effective addresses for 68k instruction operands. With corrupted CA, the wrong memory addresses are computed, causing:

1. SCSI scan completion flag never read correctly → infinite retry loop
2. 68k instruction dispatch addresses wrong → wrong handlers called silently
3. Multi-precision address calculations (A-register pre/post-decrement) off by one

The bug was invisible with ROM=0x460000 (DR emulator interpreted) because the interpreter computes CA correctly. It only manifested when extending the JIT range to include the DR emulator region.

## Detection Strategy

This class of bug (partial carry computation) is extremely difficult to catch with unit tests because:
- Individual opcodes produce correct RESULTS (the subfe result value is correct)
- Only the CARRY FLAG output is wrong
- Carry corruption only matters when a subsequent instruction reads CA
- The `subfe rX,rX,rX` idiom produces the correct result (0 or -1) regardless of CA — it's only the CA output that's wrong

**What caught it**: binary-searching the ROM range (SS_JIT_ROM_SIZE env var) to isolate the exact 256-byte region containing the bug, then decoding all instructions in that region.

## Applicability

Any PPC JIT targeting ARM64 should audit all instructions that set XER.CA:
- `subfe` (XO=136) ← this bug
- `adde` (XO=138) — likely has the same issue if using ADDS + ADD
- `subfze` (XO=200), `addze` (XO=202) — simpler (one operand is 0) but check
- `subfme` (XO=232), `addme` (XO=234) — one operand is -1, check
- `subfc` (XO=8), `addc` (XO=10) — two-operand, ADDS is sufficient (no +CA)

The `adde` instruction (`rD = rA + rB + CA`) likely has the same bug pattern if implemented as `ADDS rA+rB; ADD +CA`. Check it.
