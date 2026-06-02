# SheepShaver ARM64 JIT — Session Handoff (2026-06-02, performance session)

## Session summary

This session's goal: make the JIT faster. It turned into equal parts performance work
and deep correctness debugging — the performance changes exposed latent bugs that had
been masked since the JIT was written.

### What landed (single commit, see git log)

1. **ROM toolbox compilation** — the Mac ROM below ROMBase+0x460000 (PPC toolbox +
   nanokernel, the bulk of what Mac OS 8.x executes) is now JIT-compiled. Previously
   100% interpreted. The range stops at +0x460000 to exclude the ROM's built-in 68k
   emulator (see "The 68k emulator problem" below).

2. **OE=1 overflow arithmetic** — addco/subfco/addo/subfo/nego. These were 95% of all
   block-compile failures (they're the hot loops of the 68k emulator and also appear in
   toolbox code). 9 new harness vectors.

3. **64 MB code cache** (was 4 MB) + 4× bigger block cache pools. No more recompile storms.

4. **Chaining post-mortem** — found that block-to-block chaining NEVER worked (a marking
   bug excluded every chained block from the JIT). Fixing the marking exposed that chaining
   is architecturally unsafe (chained loops bypass interrupt polling). Chaining is now
   cleanly disabled (`JIT_BLOCK_CHAINING=0`) with the requirements for re-enabling documented.

5. **SMC invalidation infrastructure** — `ppc_jit_aarch64_invalidate_range()` exists and
   `invalidate_cache_range()` calls it, but icbi/isync remain native NOPs: falling them
   back to the interpreter was measured at 42× boot slowdown (isync is ubiquitous in OS
   code). The known SMC gap (stale translations if code is rewritten in place) is
   pre-existing upstream behavior, documented at the icbi case.

6. **JIT↔interpreter handoff** — the interpreter inner loop now exits when the next PC
   is JIT-compilable (RAM/registered-ROM range check). Without this, interpreter-only
   regions (the 68k emulator) captured all execution permanently.

7. **Debug tooling** — `tools/screenshot.sh` (framebuffer capture via lldb, no Screen
   Recording permission needed), `SS_JIT_TRACE`, `SS_JIT_DEBUG_PC`, `SS_JIT_NO_OE`,
   `SS_JIT_NO_ROM`.

### Boot timing results (this build)

| Mode | Boot to desktop |
|---|---|
| Pure interpreter (warm NVRAM) | ~10 s |
| JIT (any of today's configurations) | 160 s – 7 min |

**Boot time is the JIT's worst case** (68k-heavy + one-shot compiles + boundary
thrashing — see LEARNINGS.md). The JIT's value is steady-state performance, which is
unmeasured. MacBench / app responsiveness is the next session's first task.

### The 68k emulator problem (the session's hardest bug — fully documented in LEARNINGS.md)

The Mac ROM contains a built-in 68k emulator (an interpreter written in PPC) at
ROM+0x460000+. Its dispatch loop polls CR bit 8, which `HandleInterrupt()` sets
asynchronously to deliver interrupts. JIT-compiling those dispatch/handler blocks changes
the interrupt-delivery interleaving and corrupts multi-step 68k instruction emulation —
even though every individual instruction compiles correctly (verified exhaustively).

**Do not extend the JIT ROM range past +0x460000** without first designing an interrupt
strategy for JIT-compiled interpreter loops.

## What to do next (priority order)

1. **Register allocation** — the biggest remaining performance multiplier (likely 2-3×).
   Every PPC register access is currently a memory load/store. The `ra_*` infrastructure
   exists in ppc-jit.cpp but is disabled (upstream containment). Re-enable behind the
   harness gate, family by family.

2. **Block chaining with interrupt safety** — second biggest win (~10-30%). Options
   documented at the JIT_BLOCK_CHAINING define: chain-entry interrupt checks (with
   correct actionable-flag semantics), forward-only chaining, or periodic unlinking.

3. **MacBench / Speedometer in the guest** — get real benchmark numbers vs the
   kanjitalk755 x86_64-Rosetta build and the historical Jagmn JIT (~470%).

4. **Boot more OS versions** — Mac OS 9.2.2 ISO is at ~/Downloads/macos-922-uni/.

5. **The 68k emulator JIT design** — only if 68k-heavy workloads matter; needs the
   interrupt interleaving problem solved by design.

## How to run

```bash
cd SheepShaver/src/Unix && make -j8        # build
./SheepShaver                              # JIT on by default
SS_USE_JIT=0 ./SheepShaver                 # interpreter only
../../tools/screenshot.sh                  # capture guest display (from repo root: tools/screenshot.sh)

# Harnesses (both must stay 218/218):
cd SheepShaver && ./jit-test/run.sh
SS_HARNESS_MODE=jit ./jit-test/run.sh
```

## Test assets

- ROM: `~/Downloads/New_World_Mac_Roms/New World ROM/1998-07-21 - Mac OS ROM 1.1.rom`
- Mac OS 8.6 ISO: `~/Downloads/Mac OS 8.6 Internal Edition.iso`
- Mac OS 9.2.2 ISO: `~/Downloads/macos-922-uni/macos-922-uni.iso`
- Prefs: `~/.sheepshaver_prefs`
