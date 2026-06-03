# Lessons from PPC→ARM64 JIT: What We Actually Learned

This isn't a simple port. The upstream `rcarmo/macemu-jit` targets Linux ARM64
(Raspberry Pi / Orange Pi). This fork targets macOS ARM64 (Apple Silicon). But
the bugs we found aren't Apple-specific — they're architectural gaps between
PowerPC and ARM64 that would bite any PPC JIT on any ARM64 platform.

## The Three Bug Classes

### 1. Carry-Flag Semantics: PPC's Three-Operand Problem

PPC has instructions like `subfe` and `adde` that compute a three-operand sum
(`~rA + rB + CA`) and set the carry-out flag from the *full* 33-bit unsigned
result. ARM64's `ADDS` only handles two operands at a time. The naive approach:

```
ADDS  result, operandA, operandB    ; sets carry from A+B
ADD   result, result, carry_in      ; adds CA but DOESN'T update carry
CSET  ca_out, CS                    ; reads carry from the ADDS — wrong!
```

The carry from `ADDS(A, B)` doesn't account for the `+CA` term. For the PPC
idiom `subfe rX,rX,rX` (carry-to-mask), `~rX + rX = 0xFFFFFFFF` which *never*
carries on ARM64 — so `CA` was always written as 0, regardless of input.

**Fix**: compute the full sum in 64 bits: `UXTW + ADD X + ADD X`, extract
bit 32 as carry-out. This is correct for all input combinations.

**Impact**: the Mac ROM's 68k emulator uses `subfe` in address calculation
chains. Corrupted CA caused wrong 68k dispatch addresses → infinite SCSI
scanning loop during boot.

**Lesson**: any PPC instruction that reads AND writes CA in one operation
(subfe, adde, subfme, addme, subfze, addze) needs careful carry-chain
handling. Two-operand `ADDS` is only sufficient when there's no carry-in
term (addc, subfc, addic, subfic).

### 2. Register-Width Mismatch: 64-bit ARM64 vs 32-bit PPC

PPC's Time Base Register is a 64-bit counter split into two 32-bit halves:
TBL (lower, SPR 268) and TBU (upper, SPR 269). ARM64's equivalent
`CNTVCT_EL0` is a single 64-bit register. The JIT read CNTVCT_EL0 and
stored it as a 32-bit GPR — giving TBL and TBU identical values (both
the lower 32 bits).

Mac OS reads the time base using a standard polling pattern:
```
loop: mftb r3, TBU    ; read upper
      mftb r4, TBL    ; read lower
      mftb r5, TBU    ; read upper again
      cmplw r5, r3    ; upper changed?
      bne loop        ; retry if rollover during read
```

With TBU returning the same value as TBL, the consistency check always
passes — but the actual time value is garbage, causing timeouts and
scheduling logic to malfunction.

**Fix**: `LSR X, X, #32` for TBU reads; implicit W-register truncation
for TBL reads.

**Lesson**: any PPC SPR that maps to a differently-sized ARM64 system
register needs explicit width handling. Other candidates: DEC (decrementer),
DABR (data address breakpoint), and any MSR/SRR fields.

### 3. Emulator-in-Emulator: The DR Dispatch Cycle

The Mac ROM contains a PPC-coded 68k instruction emulator (the "DR emulator").
When the PPC JIT compiles this emulator, we get a three-level stack:
ARM64 native → PPC JIT → 68k emulation.

The DR emulator's interrupt gate (`bclr 5,8`) checks CR2.LT at a specific
point in its dispatch cycle. The JIT's block-boundary interrupt checks fire
at a different point — BETWEEN the dispatch head and the handler — setting
CR2.LT too early and causing the handler to be skipped.

This isn't a codegen bug — it's a **semantic boundary problem**. The JIT's
block boundaries don't align with the emulator's dispatch cycle boundaries.
The fix (suppressing the block-entry spcflags poll for DR emulator blocks)
was necessary but turned out to be insufficient on its own — the subfe
carry bug was the actual blocker, not the timing.

**Lesson**: when JIT-compiling an emulator dispatch loop, the JIT's
interrupt-check points must align with the emulated architecture's
interrupt delivery points. This is a known problem in emulator literature
(QEMU, Dolphin, and others all handle it with various strategies).

## The Investigation Method

Every bug was found using the same procedure:

1. **Binary search the code range** — use `SS_JIT_ROM_SIZE` or equivalent
   to narrow from megabytes to ~256 bytes of suspect code
2. **Decode the PPC opcodes** at the failing address using the JIT's own
   fetch-and-dump mechanism (the ROM is compressed; raw file reads don't work)
3. **Read the JIT codegen** for each decoded instruction — compare against
   the PPC architecture manual
4. **Fix and verify** — harness test (235/235) + boot test (VNC screenshot
   at 20s and 90s)

This method is mechanical and repeatable. Each bug takes 1-4 hours to find
once the binary search isolates the region. The hard part is recognizing
that a bug EXISTS (the symptoms are always "boot hangs" or "infinite loop"
with no crash or error message).

## What's NOT Apple-Specific

All three bug classes affect any ARM64 platform:
- The `ADDS` carry semantics are architectural ARM64, not Apple Silicon
- `CNTVCT_EL0` is the same on Linux ARM64
- Block-boundary interrupt timing is a JIT design issue, not a platform one

The only Apple-specific concern is `MAP_JIT` for code cache allocation
(required on macOS, not needed on Linux) and the `__PAGEZERO` 4GB mapping
that prevents low-address `REAL_ADDRESSING`.

## Current Status

- **subfe/adde carry**: FIXED — all three-operand CA instructions use 64-bit sums
- **mftb TBU/TBL**: FIXED — upper/lower halves correctly extracted
- **DR emulator timing**: FIXED — entry-poll suppression for DR blocks
- **Extension-loading hang**: OPEN — pre-existing bug, traced to ROM epilogue
  at 0x50132ec8 calling RAM code at 0x10662304 in a loop. Same hunt method applies.
- **Boot status**: reaches "Starting Up..." with ~10% progress bar, stuck there
- **Interpreter**: boots to Finder desktop in ~2 min (8.6 ISO) — proves the
  remaining hang is JIT-specific
