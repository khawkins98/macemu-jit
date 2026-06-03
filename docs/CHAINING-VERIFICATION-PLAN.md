# JIT Block Chaining Verification Plan

Analysis by chaining-analyzer agent (session 4, 2026-06-02). See LEARNINGS.md for full
threat model. Summary: chaining=1 is safe with ROM=0x460000 — spcflags poll present at
every chain entry, ROM is immutable (no SMC hazard).

---

## Tier 1 — Basic Correctness (do first)

**Goal**: Prove chained execution reaches and maintains a running Mac OS desktop.

**Setup**:
- ppc-cpu.cpp: `ppc_jit_aarch64_set_rom_range(ROMBase, 0x460000, ROMBaseHost)`
- ppc-jit.cpp: `#define JIT_BLOCK_CHAINING 1`
- `make clean && make -j8`
- Harness must be 230/230 score=100 before booting

**Test**:
```bash
pkill -9 -x SheepShaver 2>/dev/null
cd SheepShaver/src/Unix && SS_USE_JIT=1 ./SheepShaver
```

**Pass criteria**:
- Reaches Finder desktop (SDL window shows Mac OS 8.6 desktop, menubar visible)
- No crash in 5 minutes after desktop appears
- Mouse/keyboard input responsive (click Finder, open Apple menu)

**Failure modes to watch**:
- Hang at startup spinner → interrupt not delivered, spcflags poll broken
- Crash in ROM range (SIGBUS/SIGILL) → wrong chain target hit
- Crash during app launch → block invalidation race

---

## Tier 2 — Interrupt Delivery Verification (after Tier 1 passes)

**Goal**: Confirm timer interrupts are delivered at correct rate under sustained JIT chains.

**Background**: Mac OS maintains a 60Hz tick counter at guest address `0x16a` (Time Manager
global `Ticks`). Under heavy chaining, if the spcflags poll ever stops working, Ticks
freezes and the entire time-based subsystem breaks (beachballs, hung I/O, broken sound).

**Setup**: Same as Tier 1 (chaining=1, ROM=0x460000). Boot to Finder.

**Test A — Tick counter**:
After desktop, run in a terminal (while SheepShaver is live):
```bash
# Sample Ticks at guest 0x16a every 5 seconds for 60 seconds
# NATMEM_OFFSET = 0x400000000000
python3 -c "
import ctypes, time
addr = 0x400000000000 + 0x16a
mem = (ctypes.c_uint32).from_address(addr)
import struct
for i in range(12):
    raw = mem.value
    ticks = struct.unpack('>I', struct.pack('<I', raw))[0]
    print(f't={i*5}s  Ticks=0x{ticks:08x}  ({ticks})')
    time.sleep(5)
"
```
Expected: Ticks advances ~300 counts per 5 seconds (60Hz × 5s). A frozen or slow counter
means interrupt delivery is broken.

**Test B — Timer-intensive workload**:
With the emulator running and desktop visible, perform:
1. Open SimpleText (or any app) — requires timer for cursor blink
2. Play system alert sound (requires Sound Manager timer)
3. Leave for 10+ minutes; verify no freeze/beachball

**Pass criteria**: Ticks advances at ≥50Hz average over 60s. No visible freeze.

**Failure**: Ticks frozen or advancing <10Hz → spcflags poll broken or SPCFLAG_CPU_TRIGGER_INTERRUPT
(bit 1) not being set. Debug: add `printf` to `TriggerInterrupt()` and count calls per second.

---

## Tier 3 — Interrupt Latency Bound (optional, diagnostic only)

**Goal**: Empirically verify the ≤1-block interrupt latency claim.

**Background**: The safety analysis claims chained execution can defer interrupt delivery
by at most one block body (≤512 PPC instructions). Tier 3 measures the actual worst-case
latency observed in practice.

**Implementation** (temporary debug instrumentation in ppc-jit.cpp):

1. Add a global counter: `uint64_t g_chain_streak = 0;` and `uint64_t g_chain_streak_max = 0;`

2. In `emit_entry_spcflags_poll`, add an ARM64 debug path:
   - If spcflags == 0 (no interrupt): increment `g_chain_streak`
   - If spcflags != 0 (interrupt delivered): update `g_chain_streak_max = max(streak, max)`, reset streak to 0

3. After 60 seconds of running, print `g_chain_streak_max` to stderr.

**Interpretation**:
- streak_max × avg_block_size (in PPC instructions) = max interrupt latency in instructions
- At 1GHz effective PPC speed, 512 instructions ≈ 500ns — well within any Mac OS tolerance
- Any value under 10,000 blocks is benign for a 60Hz interrupt

**Pass criteria**: streak_max < 10,000 (no infinite loops of chains without returning)

**Failure**: streak_max = UINT64_MAX or unbounded → chain entry poll is not being reached;
block is looping via something other than the chain mechanism.

---

## Tier 4 — Full ROM Range (requires DR emulator interrupt fix first)

**Goal**: Extend JIT range to 0x500000 with chaining=1, booting cleanly to desktop.

**Prerequisite — Option A fix (Task #12, ~100-150 lines in ppc-jit.cpp)**:

Root cause confirmed (session 4, dr-emulator-analyzer): JIT polls spcflags at BLOCK ENTRY;
the DR emulator's dispatch variants (6 sites, ROM+0x466080/84/c0/e0/100/120) check for
interrupts via `bclr BO=5,BI=8` (opcode 0x4CA80020) at cycle END. The JIT fires the
interrupt one full dispatch step early — before the current 68k handler completes.

This corrupts multi-step instructions (block 504613e0 advances r24 by 4 bytes in two lhau
fetches per cycle; early interrupt causes extension word to be dispatched as an opcode).
This is the exact mechanism behind Bug #2 / A3 = 0x103ffffe corruption.

**The fix (Option A)**:
In `compile_one()` in ppc-jit.cpp, when the current PC is in ROM+0x460000–0x500000:
- Detect opcode `0x4CA80020` (`bclr BO=5,BI=8`, bits: primary=19, BO=5, BI=8, XO=16)
- Before emitting the ARM64 for the branch, emit an inline spcflags-check-and-inject:
  load spcflags; if TRIGGER set → call check_spcflags bridge (sets CR2.LT); then fall through
- Then emit the `bclr 5,8` normally — CR2.LT is now correct at evaluation time
- All 6 dispatch variants use the same encoding; single detection point covers all

**After fix, test sequence**:
1. `make clean && make -j8`
2. Harness: 233/233, score=100
3. Boot with ROM=0x500000, chaining=0 first (isolate fix correctness)
4. Boot with ROM=0x500000, chaining=1 (full config)
5. Run 10+ min, watch for spurious CD-ROM eject or A3 corruption

**Test**: Same as Tier 1 but with ROM range 0x500000. Watch for:
- No spurious CD-ROM eject (Bug #2 / A3 = 0x103ffffe written to stack)
- Mac OS Ticks advancing normally (Tier 2 check)
- Stable desktop for 10+ minutes with 68k app load (extensions, Classic apps)

---

## Current Status

| Tier | Status | Config |
|------|--------|--------|
| 1 | Pending (Path 1 boot in progress with chaining=0 first) | ROM=0x460000, chaining=1 |
| 2 | Not started | ROM=0x460000, chaining=1 |
| 3 | Not started (optional) | ROM=0x460000, chaining=1, debug build |
| 4 | Blocked on dr-emulator-analyzer findings | ROM=0x500000, chaining=1 |
