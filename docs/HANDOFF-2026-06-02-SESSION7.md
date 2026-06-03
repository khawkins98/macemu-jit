# SheepShaver ARM64 JIT — Session 7 Handoff: DR Emulator JIT — RESOLVED

## Read first

1. `LEARNINGS.md` session 7 entries — subfe carry bug, boot verification
2. `CLAUDE.md` — updated status reflecting full boot with DR emulator JIT-compiled

## What was accomplished

### The fix: subfe/adde carry-out codegen bug

**Root cause**: `subfe rD,rA,rB` computes `rD = ~rA + rB + CA` with carry-out from the full
33-bit unsigned sum. The JIT used ADDS (~rA + rB) then ADD (+CA), but read carry from the
ADDS — which only reflects carry from the partial sum. For the common idiom `subfe r4,r4,r4`
(carry-to-mask), ~r4 + r4 = 0xFFFFFFFF which NEVER carries on ARM64, so CA was always
written as 0 regardless of input CA.

**Fix**: Compute the full sum in 64 bits (UXTW + two 64-bit ADDs), extract bit 32 as carry.

**Impact**: Corrupted CA propagated through the DR emulator's address calculation chains
(subfe/adde sequences for 68k multi-precision arithmetic), causing wrong dispatch addresses
and infinite SCSI retry loops.

### Code changes

1. **subfe/adde carry fix** (ppc-jit.cpp): 64-bit carry computation for subfe
2. **Comment cleanup** (ppc-jit.cpp, ppc-cpu.cpp): removed session/bug number references
3. **DR emulator entry-poll suppression** (ppc-jit.cpp): kept in code, necessary condition
4. **Opcode encoding fix**: 0x4C420020 corrected to 0x4CA80020 (`bclr BO=5, BI=8`)
5. **Default ROM range**: updated from 0x460000 to 0x500000
6. **Block chaining**: enabled by default

### Boot verification

- **VNC screenshot confirms**: Mac OS 8.6 Finder desktop reached with full JIT
- ROM=0x500000 (full range, DR emulator JIT-compiled)
- Block chaining=1 (enabled)
- jDR=37M blocks/s (was 0 — previously interpreted)
- j2i transitions reduced from 2.4M/s to 6K/s
- SCSI scan completes normally

## Prior investigation (sessions 5-7a) — superseded

The earlier sessions investigated spcflags timing, entry-poll suppression, and CR2.LT
shadow approaches (Options A-D in the original handoff). These were all addressing the
wrong root cause — the actual bug was a carry computation error in the subfe/adde codegen
that produced wrong dispatch addresses in the DR emulator's 68k arithmetic chains.

## Current configuration

- Branch: macos-arm64
- ROM range: 0x500000 (full, DR emulator JIT-compiled)
- JIT_BLOCK_CHAINING: 1 (enabled by default)
- Harness: 235/235, score=100
- Boot: verified to Finder desktop (VNC screenshot)
- Assets: /Users/Shared/macemu/
