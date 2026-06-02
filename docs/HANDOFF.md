# SheepShaver ARM64 JIT — Session Handoff (2026-06-02, evening)

## What happened this session

### Commits pushed to macos-arm64

```
37431371 docs: record W^X root cause, JIT boot completion, and trace methodology
8f2acc9b fix: eliminate W^X overhead for out-of-range blocks, boot to desktop with JIT
7030a441 docs: add T2 counter read confirming JIT stabilizes at ~66,836 blocks
cf5b2766 docs: correct regs_for_jit fragility finding and add JIT counter data
fdf94ce2 Revert "fix: conditionalize EMUL_OP branch check on REAL_ADDRESSING"
...
```

### Key accomplishments this session

1. **MILESTONE: JIT boots Mac OS 8.6 to Finder desktop** (`8f2acc9b`).
   - First boot: 2:40 elapsed, CPU drops to ~0%, idle_wait() confirmed.
   - Third boot: 6:01 elapsed, CPU drops to ~0%, Finder running.

2. **Root cause found and fixed**: `ppc_jit_aarch64_compile()` was calling
   `pthread_jit_write_protect_np(0/1)` + emitting the block prologue for EVERY block,
   including ROM/SheepMem blocks that immediately fail the out-of-range check.
   On Apple Silicon, each W^X pair + `sys_icache_invalidate` costs ~microseconds.
   At ~11,000 ROM blocks/second during early boot, this consumed ~97% of CPU in
   protection overhead. Fix: early-out range check before `jit_cache_begin_write()`.

3. **Chain-patch tracking added** (`8f2acc9b`): `jit_chain_site` now has a `patched`
   bool that keeps `patch_loc` alive after a B-patch is applied. This enables correct
   range-based invalidation when icbi handling is re-enabled:
   `ppc_jit_aarch64_invalidate_range(start, end)` reverts live B-patches for targets
   in range + nullifies their pool entries.

4. **SS_JIT_TRACE diagnostic tool**: `SS_JIT_TRACE=/tmp/trace.txt` logs `I <pc>`
   (interpreter block entry) and `J <from> <to> <r1> <r3>` (JIT execution). Used to
   identify the W^X overhead as the root cause via differential trace analysis.

### Current state (after session)

- **JIT boots to desktop**: both `SS_USE_JIT=0` (interpreter) and default (JIT) work.
- **Harness**: both 209/209 (interpreter and JIT modes).
- **icbi/isync**: still NOPs in the JIT (reverted). The chain-patch fix enables correct
  range invalidation; connect it by making icbi return false (fall to interpreter) and
  calling `ppc_jit_aarch64_invalidate_range` from `invalidate_cache_range()`.
- **Boot time**: JIT cold boot ~2:40-6:00 (vs interpreter ~12-47s). Variance is high;
  extension loading time dominates. The JIT is not yet faster than interpreter.

### What to do next (priority order)

1. **Visual confirmation of JIT desktop** — Ken should see Mac OS Finder with JIT active.
   Run `SS_USE_JIT=1 ./SheepShaver` (default) and verify the window shows Finder.

2. **Performance analysis** — JIT should be faster than interpreter for extension loading.
   Profiling shows cache fills 1-2 times per boot. Consider:
   - Increasing JIT cache size (currently 4MB, `ppc_jit_aarch64_init(4096)`)
   - Reducing block recompilation via range-based icbi invalidation

3. **Re-enable icbi/isync SMC handling** — `ppc-jit.cpp` case 982 (`icbi`) should
   return false (fall to interpreter). Connect `invalidate_cache_range()` to call
   `ppc_jit_aarch64_invalidate_range(start, end)`. Test with harness + boot.

4. **The `(target >> 26) == 6` check** in the `b` instruction handler — still rejects
   branches to 0x18000000-0x1BFFFFFF as "EMUL_OP trampolines." Under macOS
   DIRECT_ADDRESSING, these are valid RAM addresses. Conditionalize on `#ifdef
   REAL_ADDRESSING`. This was tried and reverted (didn't change boot behavior) but is
   still technically wrong.

5. **MacBench / performance metrics** — once booted with JIT, measure performance vs
   interpreter and vs historical Jagmn JIT (~470% MacBench vs ~96% interpreter).

## How to run

```bash
# Build
cd SheepShaver/src/Unix && make -j8

# Run (JIT on by default)
./SheepShaver

# Run interpreter only
SS_USE_JIT=0 ./SheepShaver

# Run with PC trace (debugging)
SS_JIT_TRACE=/tmp/trace.txt SS_USE_JIT=1 ./SheepShaver

# Harness
cd SheepShaver && ./jit-test/run.sh              # interpreter 209/209
SS_HARNESS_MODE=jit ./jit-test/run.sh            # JIT 209/209
```

## Test assets

- ROM: `~/Downloads/New_World_Mac_Roms/New World ROM/1998-07-21 - Mac OS ROM 1.1.rom`
- Mac OS 8.6 ISO: `~/Downloads/Mac OS 8.6 Internal Edition.iso`
- Mac OS 9.2.2 ISO: `~/Downloads/macos-922-uni/macos-922-uni.iso`
- Prefs: `~/.sheepshaver_prefs` (jit false — irrelevant, SS_USE_JIT env var controls it)
