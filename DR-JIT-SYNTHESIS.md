# Synthesized Strategies for JIT-Compiling the 68K DR Emulator Region

## ⚠️ INVALIDATED — Root Cause is NOT Spcflags Timing

**Status**: This document represents research based on a **disproven assumption**. Experimental evidence shows:
- Correct interrupt prediction was achieved (~110 fallbacks/s, normal rate)
- Boot **still hangs** despite proper spcflags delivery
- **Root cause is a codegen bug**, not interrupt timing

**This document is retained as a diagnostic baseline** showing what was ruled out and the investigation methodology. The actual problem appears to be instruction-level codegen error (likely `rlwimi` in the DR dispatch loop at ROM 504613e8).

---

## Invalidated Problem Statement

The initial hypothesis (below) was **disproven by empirical testing**:

The 68K emulator (DR region, ROM+0x460000..0x500000) stays interpreted and runs **1.8x slower** under JIT mode due to transition overhead. The hypothesis was that the PPC JIT accelerates the nanokernel 82x, but splits the exception dispatch cycle into multiple blocks with spcflags checks between them, causing interrupts to fire at the wrong time.

**Hypothesis symptoms** (now known to be incorrect):
- Option A alone: SCSI loop hangs (boot blocked)
- Full deferral: interrupts starved (VBL never fires)
- Entry-poll suppression: still hangs (C dispatcher's between-block check fires)

**Evidence disproving the hypothesis**:
- Interrupt prediction was corrected to normal rates (~110 fallbacks/s)
- Boot still hangs with correct interrupt delivery
- **Therefore: interrupt timing is not the root cause**

**Measurement summary** (from LEARNINGS.md, session 7):
- 2.4M JIT↔interpreter transitions per second during boot (now known to be a measurement artifact, not the cause)
- NK:DR ratio: 1:21 in interpreter, 1:1 in JIT 
- Interpreter: 12s HD boot, 318s CD boot
- JIT: 100+ seconds on HD (unfinished) — **with correct interrupts, still hangs**

---

## Research Synthesis — What Was Ruled Out

Three cross-emulator reports were commissioned to validate the interrupt-timing hypothesis. While the hypothesis was **disproven**, the research established a valuable diagnostic baseline:

| Report | Finding | Status |
|--------|---------|--------|
| **Cross-Emulator** | Consensus: interrupts checked only at block boundaries. Multiple exit points normal, ~3% overhead. | ✓ **Confirmed** — interrupt delivery model is sound. Not the root cause. |
| **QEMU 68K** | Atomic instruction translation; CC state synced before exceptions; block terminators explicit. | ✓ **Confirmed** — instruction-level atomicity is correct. Suggests the hang is at instruction codegen, not dispatch structure. |
| **Dolphin PPC** | Three-tier dispatch, downcount mechanism, selective state flush. | ✓ **Confirmed as reference** — architectural patterns are proven, but not the immediate problem. |

**What the research proved**:
- The cross-emulator consensus on interrupt delivery is architecturally sound
- The problem is **not** spcflags timing, interrupt latency, or dispatch structure
- The problem is **likely instruction-level codegen**—a single instruction (rlwimi at ROM 504613e8) generating incorrect ARM64 code

**Next investigation focus**:
- Binary-search ROM range with `SS_JIT_ROM_SIZE` to isolate which instructions cause hang
- Compare `rlwimi` codegen (ROM 504613e8, dispatch loop) against test vectors
- Trace SCSI EMUL_OP execution to determine if hang is SCSI-specific or general emulation failure

---

## ❌ The Three Strategies Are No Longer Applicable

The research team proposed three strategies for fixing spcflags timing and dispatch-cycle atomicity. **These strategies are architecturally sound but do not solve the actual problem.**

**Why they're no longer valid**:
- Correct interrupt delivery was achieved with proper spcflags timing
- Boot still hangs despite correct interrupts
- The hang is caused by something else: a **codegen bug**, not interrupt delivery

**Historical value of the strategies**:
- They validate the interrupt-delivery architecture is correct
- They prove the problem is downstream (instruction codegen, not dispatch structure)
- Future reference if interrupt delivery needs optimization (e.g., for cycle accuracy)

---

## Real Problem: Codegen Bug (Likely in rlwimi)

**Current hypothesis** (based on uncommitted changes):

The 68K emulator (DR region) contains a **tight dispatch loop** with instruction `rlwimi r27,r29,3,13,28` at ROM address **504613e8**. This instruction is likely generating incorrect ARM64 code.

**Evidence**:
1. Uncommitted test vectors added for `rlwimi` with exact dispatch-loop patterns
2. ROM binary search capability (`SS_JIT_ROM_SIZE`) allows isolating which instructions cause hang
3. SCSI EMUL_OP tracing to verify if hang is SCSI-specific or general

**Investigation methodology**:

1. **Binary-search the ROM range** to isolate which code causes hang:
   ```bash
   # Find safe upper bound
   SS_JIT_ROM_SIZE=0x480000 make run-jit  # Does it boot past SCSI?
   SS_JIT_ROM_SIZE=0x490000 make run-jit  # Further?
   # Narrow down to exact instruction address
   ```

2. **Test `rlwimi` codegen**:
   - The test harness now includes `rlwimi_dr_dispatch` vectors (ROM 504613e8)
   - If harness shows `rlwimi` failures, that's the bug location
   - If harness passes, `rlwimi` is correct and hang is a different instruction

3. **Compare SCSI EMUL_OP traces**:
   - Interpreter trace: expected SCSI call sequence
   - JIT trace: if it diverges or stops, identifies where JIT goes wrong

4. **Narrow to instruction type**:
   - Once the hanging address is known, disassemble ROM at that address
   - Test that specific instruction in the harness with values from the actual boot sequence

---

## Next Steps (Not Strategies 1–3)

**Phase 1: Binary-search to isolate the codegen bug**
- Use `SS_JIT_ROM_SIZE` to narrow down which ROM range causes hang
- Goal: identify a small range (e.g., 0x504610000..0x50461200) that hangs
- Tool: Set ROM range, boot, measure blocks/sec and VBL interrupt delivery

**Phase 2: Test the suspected instruction**
- Once ROM address is known, disassemble and identify the instruction
- Add test vectors to the harness or run single-instruction tests
- Compare JIT-generated ARM64 code to expected semantics

**Phase 3: Fix the codegen**
- Once the bug is identified (e.g., wrong register allocation, incorrect condition flag generation, etc.), fix it in ppc-jit.cpp
- Re-validate with harness and boot test

---

## Why the Original Hypothesis Failed

The original diagnosis was based on:
- High JIT↔interpreter transition count (2.4M/s)
- NK:DR ratio imbalance (1:1 JIT vs 1:21 interpreter)
- Assumption: more transitions = more spcflags checks firing at wrong times

**What was disproven**:
- With correct interrupt prediction, the transition count should drop
- Boot hanging despite correct interrupts means the interrupt delivery wasn't the issue
- The codegen must have a logic error that causes incorrect behavior, not timing

**Lesson**: High transition counts can be a symptom of a deeper bug, not the cause. Fixing the symptom (transitions) without fixing the root cause (codegen) leaves the hang in place.

---

## Diagnostic Summary

**Status**: Research invalidated. Root cause identified as **codegen bug**, not spcflags timing.

**Investigation path**:
1. ✅ Cross-emulator research validated interrupt delivery architecture is sound
2. ✅ Empirical testing with correct interrupts proved boot still hangs
3. ✅ **Binary-search COMPLETE** — failing region isolated to **0x50466000–0x50468000** (2KB)
4. 🔄 Narrowing to 1KB block (in progress)
5. 🔄 Disassemble failing block and identify instruction(s)
6. ⏳ Test instruction codegen against harness vectors
7. ⏳ Fix the codegen and re-validate

**Binary-search results**:
- Safe: 0x460000–0x464000 (first 16KB)
- Failing: 0x466000–0x468000 (2KB range, ROM address 0x50466000–0x50468000)
- Next: Narrow to 1KB block within this range

**Tools available**:
- `SS_JIT_ROM_SIZE=0xHEX` — narrow down which ROM instructions cause hang
- `rlwimi_dr_dispatch` harness vectors — validate `rlwimi` codegen at 504613e8
- `SS_EMULOP_TRACE=1` — trace SCSI EMUL_OP execution to verify if hang is SCSI-specific
- `objdump` / disassembler — once 1KB block identified, disassemble to find exact instructions

**Next critical step**:
- Narrow failing region to 1KB block (e.g., 0x50466000–0x50467000 or 0x50467000–0x50468000)
- Disassemble that block and list all instructions
- Compare instructions against test harness vectors to identify codegen mismatch
