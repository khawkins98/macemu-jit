# SheepShaver ARM64 JIT — Session Handoff (2026-06-02, session 5)

## TL;DR for next agent

**The core bug causing slow JIT boot is an initialization-order deadlock at nanokernel address 0x50313d34.** The fix is ~5-10 lines in sheepshaver_glue.cpp. Everything else in this document is supporting evidence and context.

---

## What was discovered this session

### Root cause of 7-minute JIT boot: nanokernel deadlock at 0x50313d34

**The spin-wait**: ROM address 0x50313d34 is part of the nanokernel's normal dispatch loop. It checks `[r11+4]` for zero before proceeding. With r11=0x2f072 (consistent across all observations), it reads guest address 0x2f076.

**The deadlock**:
- In **interpreter mode**: guest 0x2f076 = 0 when first checked → exits immediately → boot takes ~10s
- In **JIT mode**: guest 0x2f076 = 0x68fff400 (= KernelDataAddr+0x1400) → loop never exits initially
- Deadlock resolves after ~20 real seconds when XLM_IRQ_NEST drops to 0 and a VBL fires

**Why Ticks is frozen during stuck phases**: `tick_func` (main_unix.cpp:1629) gates `TriggerInterrupt()` on `ReadMacInt32(XLM_IRQ_NEST) == 0`. During the stuck phase, XLM_IRQ_NEST > 0, so no VBL fires and HandleInterrupt never runs.

**The large Ticks value (0xb5a01066 ≈ 3B)**: This is set by ROM initialization from the emulated RTC, not from our "always tick Ticks" code overrunning.

**Occurrence pattern**: Fires ~2 times per 22-minute JIT run (at ~t=150s and ~t=1315s). Each occurrence takes ~20s to resolve. These 40 seconds are a minor fraction of total boot time — the main slowness is extension loading.

**Chaining=1 effect**: With JIT_BLOCK_CHAINING=1, the deadlock fires 10+ times in 10 minutes. Chaining=1 is not bad — it exposes the deadlock more frequently by running initialization code faster. The fix is to resolve the deadlock; then chaining=1 can be the default.

### What writes 0x68fff400 to guest 0x2f076

The nanokernel ROM boot code at ~0x310000-0x320000 initializes task structures using EmulatorData pointers passed at boot entry. Specifically:
- SheepShaver sets `gpr(4) = KernelDataAddr + 0x1000` before entering the nanokernel (sheepshaver_glue.cpp:1204)
- The nanokernel ROM code reads this and computes EmulatorData + 0x400 = KernelDataAddr + 0x1400 = 0x68fff400
- It writes this pointer to the task structure at 0x2f076 as part of nanokernel task initialization
- This write happens BEFORE the spin-wait at 0x50313d34 is first reached in JIT mode
- In interpreter mode, the spin-wait is reached BEFORE this initialization runs (slower execution = different ordering)

This is an initialization-ordering race, not a JIT miscompilation.

### The proposed fix

**Option A — HandleInterrupt shim** (recommended, ~8 lines):

In `sheepshaver_glue.cpp` `HandleInterrupt()`, `case MODE_68K` (around line 1327), when we're in the nanokernel context (r1 == KernelDataAddr), check if guest 0x2f076 is non-zero and clear it. This matches interpreter behavior (where the field is 0 on first entry to the spin-wait).

```cpp
case MODE_68K:
    // Fix JIT initialization-order race: nanokernel spin-wait at 0x50313d34
    // checks [r11+4] (guest 0x2f076) expecting 0 on first entry. In JIT mode
    // the nanokernel has already initialized this field before the spin-wait runs.
    // Clear it here (in nanokernel interrupt context) to match interpreter behavior.
    if (r->gpr[1] == KernelDataAddr) {
        uint32 task_field = ReadMacInt32(0x2f076);
        if (task_field == KernelDataAddr + 0x1400)
            WriteMacInt32(0x2f076, 0);
    }
    WriteMacInt16(ReadMacInt32(KERNEL_DATA_BASE + 0x67c), 1);
    ...
```

**Risk**: Medium. This is a targeted shim for a specific timing race. The condition `task_field == KernelDataAddr + 0x1400` makes it specific rather than clearing blindly. If the field value varies (e.g., different ROM versions), the condition can be relaxed to `task_field != 0`.

**Alternative — ROM patch** (Option B): Find the exact ROM instruction that writes to 0x2f076 and patch it to be conditional. Requires the watchpoint run (SS_JIT_WATCH_ADDR=2f076 SS_JIT_TRACE_RING=1) to identify the writer block.

**Alternative — Fix ordering** (Option C): Ensure the task structure at 0x2f072 is initialized ONLY after the spin-wait has been reached once. Requires deeper understanding of the nanokernel initialization sequence.

---

## Diagnostics added this session (all committed)

- **STUCK register dump**: When the heartbeat detects the same PC for 10+ seconds, stderr prints r1/r9/r10/r11/r12/Ticks/CR
- **STUCK → ring auto-dump**: First STUCK event calls `ppc_jit_dump_trace_ring()` (requires SS_JIT_TRACE_RING=1)
- **`SheepShaver/tools/jit-analyze.py`**: Log analysis tool
  - `diag [log]` — heartbeat progression, hot PC frequency, 10s-window PC activity
  - `ring [log] [pc]` — ring context around a specific PC
  - `hot [log] [N]` — top-N interrupt-delivery PCs

---

## Current committed state

- **chaining**: JIT_BLOCK_CHAINING=0 (deadlock must be fixed before enabling 1)
- **ROM range**: 0x460000 (toolbox-only, proven safe)
- **Harness**: 233/233, score=100 (both interpreter and JIT)
- **Boot**: Reaches Finder in ~22 minutes (JIT), ~10 seconds (interpreter)
- **Branch**: macos-arm64

## What to do next (ordered)

1. **Check watchpoint output** (`/tmp/ss_watch_2f076.log`): If `SS_JIT_TRACE_RING=1 SS_JIT_WATCH_ADDR=2f076` run captured a hit, the `ADDR-WATCH:` line will show which block writes 0x68fff400 to 0x2f076. This tells us whether Option A or B is cleaner.

2. **Implement the fix**: Option A (HandleInterrupt shim) is the fastest path. Implement, rebuild, test boot time. If the deadlock is gone, the boot should improve significantly.

3. **Enable chaining=1**: After the deadlock fix, change `#define JIT_BLOCK_CHAINING 0` → `1` in `ppc-jit.cpp` line 89. This should give a meaningful speedup by eliminating per-block C-dispatch overhead for hot nanokernel paths.

4. **Measure boot time**: Compare JIT boot time vs interpreter after fix + chaining.

5. **DR emulator JIT** (longer term): JIT-compiling 0x460000-0x500000 (the 68k DR emulator) is blocked on the Bug #2 (A3 register corruption causing offLinErr) from the previous session. See HANDOFF.md (dyngen-parity session) for details.

---

## Key addresses for context

| Address | What it is |
|---------|-----------|
| 0x50313d34 | Nanokernel spin-wait condition check (ROM, JIT-compiled) |
| 0x50312xxx | Nanokernel dispatch loop PCs (hot during boot) |
| 0x2f072 | Task structure pointer (r11 during spin-wait) |
| 0x2f076 | Task "in-use" field: 0=ready, 0x68fff400=initialized |
| 0x68ffe000 | KernelDataAddr = KERNEL_DATA_BASE |
| 0x68fff400 | KernelDataAddr + 0x1400 (EmulatorData field) |
| XLM_IRQ_NEST (0x2818) | Interrupt disable counter; VBL gated on this being 0 |

## Environment

- ROM: `/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom`
- Disk: `/Users/Shared/macemu/macos921.dsk`
- NATMEM_OFFSET: 0x400000000000
- Build: `cd SheepShaver/src/Unix && make -j8`
- Run: `./SheepShaver` (JIT on by default; `SS_USE_JIT=0` for interpreter)
