# SheepShaver ARM64 JIT — Session 3 Handoff (2026-06-02)

## Accomplishments This Session

1. **Opcode Coverage Audit** (Haiku agent)
   - Scanned ROM 0x460000–0x500000 for PPC instructions
   - Found 25 uncovered primary opcodes, 942 uncovered XO-form sub-opcodes
   - Focused on load/store-with-update forms: sthu/lhau/lhzu (4,900–5,600 instances each)
   - Also uncovered: mcrxr (7 instances, XER→CR), lhbrx/lwbrx (8/12 instances, endian safety)

2. **Test Vector Expansion**
   - Added `lhau_basic` (load halfword algebraic with update)
   - Added `lhzu_basic` (load halfword zero with update)
   - Both verified correct in JIT
   - **Harness: 229/229 (was 227), score=100** with chaining=0

3. **A3 Register Mapping Confirmed** (Haiku agent)
   - 68k A3 lives in PPC r19 (gpr[19])
   - Offset in powerpc_registers struct: 0x4c (76 bytes)
   - Verified via ppc-registers.hpp, ppc-cpu.cpp line 150, PPCR_GPR macro

4. **Bug #2 Investigation (Systematic Debugging Phase 1–2)**
   - Enabled chaining (`JIT_BLOCK_CHAINING=1`) and full ROM range (0x500000)
   - Ran 95+ seconds: 100% CPU, SCSI loop active, zero watch events, no crash
   - Reverted to chaining=0 (conservative, per LEARNINGS.md "68k-in-JIT unsafe" warning)
   - Harness still 229/229, chaining=0 is known-safe configuration

## Current State

- **Working tree**: ppc-cpu.cpp ROM range = 0x500000 (full), JIT_BLOCK_CHAINING = 0 (safe)
- **Harness**: 229/229, score=100 (interpreter + JIT modes)
- **Boot status**: Unknown (session focused on opcode coverage, not full boot)
- **Bug #2 status**: Unresolved (no watch events; unclear if bug manifests in this config)

## Next Session: Three Paths

### Path 1: Conservative Boot Verification (Recommended to start)
```bash
# Revert ROM range to 0x460000 (toolbox-only, proven safe)
# in ppc-cpu.cpp line ~1042: change 0x500000 → 0x460000
# Rebuild, boot to desktop with SS_USE_JIT=1
# This establishes: interpreter boots fully, JIT boots partially, no crashes
```

### Path 2: Progressive Feature Enabling (Investigate chaining safety)
```bash
# Start from Path 1 (0x460000 + chaining=0 boots cleanly)
# Then flip ONE variable at a time:
# 1. Enable chaining (JIT_BLOCK_CHAINING=1), test
# 2. Expand ROM range to 0x500000, test
# 3. Both together, test
# Watch for when/if crash reappears (will isolate the culprit)
```

### Path 3: Opcode Audit (Prevent future bugs)
```bash
# Create test vectors for remaining uncovered opcodes:
# - mcrxr (XER exception/carry state → CR0)
# - lhbrx / lwbrx (byte-reversed halfword/word loads, endian safety)
# - mtcrf with FXM ≠ 0xFF (selective CR field writes)
# - Harness run after each addition
# Current target: 250+ vectors total, catch corner cases before boot
```

## Key Files Modified This Session

- `SheepShaver/jit-test/run.sh`: Added lhau_basic, lhzu_basic test vectors (lines 891–902)
- `LEARNINGS.md`: Documented opcode audit findings and chaining test results
- `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`: Toggled JIT_BLOCK_CHAINING (reverted to 0)

## Critical Unknowns

1. **Bug #2 manifestation**: Does A3 corruption (0x103ffffe) still occur with full ROM + chaining=1?
   - Previous session: observed spurious CD-ROM eject, corrupted A3 on stack
   - This session: 95s run, no watch events, no eject
   - Hypothesis: crorc fix eliminated it, OR it requires specific conditions we haven't met

2. **68k-Emulator-in-JIT safety**: LEARNINGS.md (2026-06-02 later) concluded interrupt-delivery interleaving makes JIT-compiling the 68k emulator unsafe. Does this hold?
   - Evidence against: no crash in 95s with ROM 0x500000
   - Evidence for: the interleaving issue is real; one boot attempt isn't enough proof

3. **Watch mechanism**: Why no events during SCSI loop when previous session caught them?
   - Possible: different code path, different register state, condition-dependent

## Recommendations for Next Session

1. **Start with Path 1** (boot verification at 0x460000 + chaining=0) to establish a clean baseline
2. **Then Path 3** (opcode vectors) in parallel—this directly prevents new bugs, independent of Bug #2
3. **Only if Path 1 succeeds**: proceed to Path 2 (progressive chaining/ROM range)
4. **Do NOT commit any chaining or full-ROM changes** until Bug #2 is either fixed or formally understood

## Assets & Commands

```bash
# Harness (from repo root or SheepShaver/)
./jit-test/run.sh                    # interpreter + JIT, 229 vectors, score=100

# Manual opcode test (from SheepShaver/)
SS_TEST_HEX=38601234 SS_TEST_JIT=0 make test-opcodes  # test specific vector in interpreter

# Boot test (from SheepShaver/src/Unix)
SS_USE_JIT=1 ./SheepShaver          # JIT mode
SS_USE_JIT=0 ./SheepShaver          # interpreter only

# Assets
ROM: /Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom
ISO: /Users/Shared/macemu/Mac OS 8.6 Internal Edition.iso
Prefs: ~/.sheepshaver_prefs
```

---

**Session Duration**: ~2 hours  
**Harness Coverage**: 227 → 229 vectors  
**Commits Ready**: lhau/lhzu test vectors (not yet committed; verify boot first)  
**Bug #2 Status**: Still under investigation; no crash observed this session
