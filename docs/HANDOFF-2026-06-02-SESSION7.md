# SheepShaver ARM64 JIT — Session 7 Handoff (2026-06-02 through 2026-06-03)

## Status: RESOLVED — Mac OS 8.6 boots to Finder desktop with full native JIT

No skip list. No workarounds. ROM=0x500000 (full range including DR emulator),
block chaining enabled. Harness: 235/235, score=100.

## 8 bugs found and fixed

| # | Bug | One-line description |
|---|-----|---------------------|
| 1 | subfe/adde carry-out | 64-bit arithmetic needed for three-operand CA computation |
| 2 | mftb TBU/TBL | CNTVCT_EL0 upper/lower halves were identical (both returned low 32 bits) |
| 3 | DR emulator entry-poll | Block-entry spcflags poll injected CR2.LT mid-dispatch-cycle |
| 4 | icbi NOP | Stale JIT translations survived code rewrites |
| 5 | isync NOP | Deferred icbi invalidation never flushed |
| 6 | UXTW addressing | Defensive 32-bit address extension for register-offset mem access |
| 7 | fmsub/fnmsub encoding swap | PPC fmsub mapped to ARM64 FMSUB (wrong sign); PPC fnmsub had the reverse error |
| 8 | lwarx/stwcx./mftb fallback | CPU-object reservation state and timebase model require interpreter |

Bugs 1-6 were found by address-range binary search and ROM block decoding. Bugs 7-8
were found by opcode-based bisection (SS_JIT_SKIP_OPC, SS_JIT_SKIP_XO, SS_JIT_SKIP_XO63)
after address-based search failed due to extension ASLR.

## Configuration

- Branch: `macos-arm64`
- ROM range: 0x500000 (full, DR emulator JIT-compiled)
- JIT_BLOCK_CHAINING: 1 (enabled)
- No skip list needed
- lwarx/stwcx./mftb use interpreter fallback (negligible performance cost: lwarx/stwcx.
  are rare outside atomic loops, mftb appears only in time-reading sequences)

## Diagnostic tools (useful for future work)

| Tool | Purpose |
|------|---------|
| `SS_JIT_SKIP_OPC=n,n,...` | Force interpreter for specific primary opcodes |
| `SS_JIT_SKIP_XO=n,n,...` | Force interpreter for specific XO31 sub-opcodes |
| `SS_JIT_SKIP_XO63=n,n,...` | Force interpreter for specific XO63 sub-opcodes |
| `SS_JIT_MAX_INSNS=n` | Cap JIT block length |
| `SS_JIT_VERIFY=1` | Per-block register verification against interpreter |
| `SS_JIT_ROM_SIZE=<hex>` | Override ROM JIT range |
| `SS_JIT_INTERP_RANGE=lo-hi` | Force interpreter for a PC range |
| `SS_JIT_NO_CHAIN=1` | Disable block chaining |
| `SS_JIT_NO_ROM=1` | Keep ROM interpreter-only |
| `SS_JIT_CHAIN_LOG=1` | Log dispatch call chain at stuck-loop entry |
| `SS_EMULOP_TRACE=1` | Log SCSI EMUL_OP return values |
| Heartbeat: `M/s comp=N` | Block rate and unique-blocks-compiled count |

## Read next

1. `LEARNINGS.md` — session 7 FINAL entry at top
2. `docs/PPC-ARM64-JIT-LESSONS.md` — architectural narrative (7 bug classes)
3. `JIT-STATUS.md` — current pass/fail status
