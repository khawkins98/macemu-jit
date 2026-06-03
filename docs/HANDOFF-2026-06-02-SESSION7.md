# SheepShaver ARM64 JIT — Session 7 Handoff: DR Emulator JIT Investigation

## Read first

1. `LEARNINGS.md` session 7 entries — boot measurements, opcode encoding fix, three ROM=0x500000 experiments
2. `docs/HANDOFF-2026-06-02-SESSION6.md` — session 6 parallel plan (still partially valid)
3. `CLAUDE.md` — updated prefs documentation, multi-instance warning

## What was accomplished

### Code changes (all committed to working tree, ready to commit)

1. **Comment cleanup** (documenter agent): removed session/bug number references from
   ppc-jit.cpp (3 sites) and ppc-cpu.cpp (5 sites), replaced with concept-based descriptions.

2. **DR emulator entry-poll suppression** (ppc-jit.cpp): blocks in ROM+0x460000..+0x500000
   skip `emit_entry_spcflags_poll()`. This is KEPT in the code — it's a necessary condition
   for ROM=0x500000 to work, just not sufficient on its own.

3. **Opcode encoding fix**: all references to `0x4C420020` corrected to `0x4CA80020`
   (`bclr BO=5, BI=8`). The old constant was `bclr 2,2` — completely wrong.

4. **LEARNINGS.md**: session 7 entries with boot measurements, opcode fix, and three
   experiment results.

5. **ROM range stays at 0x460000** — safe config, verified 233/233 and boot.

### What we learned

**The core imbalance** (session 7a, picked up from session a9186d5d):
- PPC JIT accelerates nanokernel 82x but DR (68k) emulator stays interpreted
- DR runs 1.8x SLOWER under JIT due to 2.4M transitions/s
- HD interpreter boot: ~12s. HD JIT boot: 100s+ (possibly at desktop with broken detection)
- Fix: JIT-compile the DR region

**Why ROM=0x500000 fails** (session 7b, this session):
- Three experiments all fail (entry-poll suppression, spcflags deferral, Option A inline check)
- Root cause: JIT separates the DR dispatch cycle into multiple blocks with spcflags
  checks between them; the interpreter runs the cycle as one block
- The mid-cycle spcflags check sets CR2.LT at the wrong time
- Fully deferring spcflags starves interrupt delivery entirely

**JIT codegen is correct**: SS_HARNESS_MODE=jit passes 233/233.

## The remaining bug

**Symptom**: With ROM=0x500000, boot hangs in infinite SCSI bus scan. jRAM=33 (no RAM
code ever executes). Interrupts fire normally (~55/s). SCSI EMUL_OPs execute but the
scan completion logic never triggers.

**Root cause**: The DR emulator's dispatch cycle spans multiple JIT blocks:
1. DR dispatch head (504613e0): lhau, addco., rlwimi, mtlr, lhau, sthu → bclr 5,8
2. Handler (toolbox ROM, below 0x460000): emulates one 68k instruction
3. Return to DR dispatch head

Between blocks 1→2 and 2→3, the C dispatcher calls `check_spcflags()`. If a VBL
interrupt is pending, `check_spcflags()` calls `HandleInterrupt()` which sets CR2.LT=1.
The NEXT DR dispatch block's bclr 5,8 sees CR2.LT=1 and falls through to the interrupt
path BEFORE the previous opcode's handler has finished its side effects.

In the interpreter, the decode-cache (Duff's device loop at ppc-cpu.cpp:1250-1261) runs
the ENTIRE dispatch cycle as one block boundary. Spcflags are checked AFTER the whole
cycle, not between the dispatch head and handler.

**Fix direction (not yet implemented)**:

Option B: "Interrupt-pending but not yet delivered" flag for the DR region.
When `check_spcflags()` is called and the next PC is in the DR emulator range,
instead of calling `HandleInterrupt()` (which sets CR2.LT), just set a deferred
flag. When the next PC is NOT in the DR emulator range (i.e., when the dispatch
cycle has completed and we're about to enter the interrupt path or return to the
nanokernel), call `HandleInterrupt()` then.

This preserves interrupt delivery (not starved like experiment 3) but defers the
CR2.LT injection until the correct point in the dispatch cycle (matching interpreter
timing).

Option C: "CR2.LT shadow register" approach.
Keep the current `check_spcflags()` and `HandleInterrupt()` calls, but have the JIT
save and restore CR2.LT around the DR dispatch block. The JIT restores the pre-
interrupt CR2.LT for the bclr 5,8 evaluation, then re-sets it afterward. This is
simpler than Option B but requires the bclr codegen to read from a shadow location.

Option D: Per-instruction spcflags check in DR blocks.
Emit spcflags checks AFTER each PPC instruction in DR blocks (matching the interpreter's
per-instruction check at `do_interpret:` line 1320). This is the most faithful to the
interpreter but expensive. Could be scoped to only the dispatch blocks (the 6 sites).

## Current configuration

- Branch: macos-arm64, HEAD = 77d47baa + uncommitted changes
- ROM range: 0x460000 (safe, DR emulator interpreted)
- JIT_BLOCK_CHAINING: 0
- Harness: 233/233, score=100 (both interpreter and JIT modes)
- Boot: verified working at ROM=0x460000 with HD boot
- Prefs: HD boot (macos921.dsk), nocdrom true, bootdriver 0
- Assets: /Users/Shared/macemu/

## Files modified (uncommitted)

| File | Changes |
|------|---------|
| `LEARNINGS.md` | Session 7 entries: boot times, opcode fix, experiments |
| `ppc-jit.cpp` | Entry-poll suppression for DR blocks, comment cleanup |
| `ppc-cpu.cpp` | Comment cleanup, ROM range comment update |
| `docs/CHAINING-VERIFICATION-PLAN.md` | Opcode encoding fix |
| `docs/HANDOFF-2026-06-02-SESSION7.md` | This file |
