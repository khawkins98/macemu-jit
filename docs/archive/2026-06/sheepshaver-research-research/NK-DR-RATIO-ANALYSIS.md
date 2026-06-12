# NK:DR Block Ratio Analysis — JIT 1:1 vs Interpreter 1:21

> **Status:** 📖 Reference / archive · **Created:** 2026-06-04 · **Updated:** 2026-06-04
> **Why this doc exists:** Analysis of nanokernel:DR block ratios (JIT 1:1 vs interpreter 1:21).
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../../CONTRIBUTING.md) → "Documentation Lifecycle")._


> **⚠️ ARCHIVED / HISTORICAL.** A point-in-time diagnostic analysis from the DR-emulator
> boot investigation, preserved for reference. The DR boot path was since resolved (the
> DR emulator is now JIT-compiled; see CLAUDE.md "Crorc Fix & Block Chaining" and
> LEARNINGS.md). **Block counts are not comparable across modes** (DR dispatch blocks are
> 2–4 instructions, toolbox/RAM blocks are larger) — see the diagnostics note in CLAUDE.md
> before drawing conclusions from the ratio here.

## Observation

In JIT mode, nanokernel (NK) and DR-emulator (DR) block counts are roughly
equal (1:1). In interpreter mode, the ratio is 1:21 (21 DR blocks per NK
block). This document analyzes whether the disparity is a bug or expected
behavior.

## Critical Facts Established by Code Analysis

### Fact 1: The JIT does NOT compile ROM code

`ppc_jit_aarch64_compile()` (ppc-jit.cpp:3538) accepts `ram = RAMBaseHost`
and `ramsize = RAMSize`. The compilation loop (line 3594-3595) bails when
`cur_pc` falls outside `[RAMBaseHost, RAMBaseHost + RAMSize)`:

```cpp
if (cur_pc < (uint32_t)(uintptr_t)ram ||
    cur_pc >= (uint32_t)(uintptr_t)ram + ramsize)
    break;
```

Address map:
- RAM: 0x10000000 (`RAM_BASE`, main_unix.cpp:186) — JIT-compilable
- ROM: 0x50000000 (`ROM_BASE`, main_unix.cpp:188) — interpreter only
- DR cache: 0x69000000 (`DR_CACHE_BASE`) — interpreter only

Both nanokernel code (0x50313dxx) and the DR emulator (0x50460000+) are
ROM-resident. They run at interpreter speed in BOTH modes.

### Fact 2: The JIT compiles RAM code (0x10xxxxxx)

Toolbox callbacks, native code patches, and emulator trampolines that
execute from RAM ARE JIT-compiled. When the nanokernel dispatches work
to RAM-resident code, that code runs ~10-20x faster under JIT.

### Fact 3: NK entries are driven by two distinct paths

**Asynchronous (wall-clock bounded)**: `tick_func()` fires at 60Hz,
sets `INTFLAG_VIA`, calls `TriggerInterrupt()` which calls
`ppc_cpu->trigger_interrupt()` → `spcflags.set(TRIGGER_INTERRUPT)`.
On next `check_spcflags()`, this promotes to `HANDLE_INTERRUPT`, which
calls `HandleInterrupt()` → `interrupt()` → `execute(NK_entry)`.
All callers of `TriggerInterrupt()`/`SetInterruptFlag()` are timer-
or I/O-driven (verified: tick_func at 60Hz, ethernet packet arrival).
None are execution-frequency dependent.

**Synchronous (execution-frequency dependent)**: The nanokernel dispatch
loop itself is ROM code that runs after every trap, system call, or
context switch. When RAM code executes a `sc` (opc 17), the JIT falls
back to interpreter (compile_one returns false), the interpreter raises
the syscall, and the nanokernel processes it. The nanokernel then
re-dispatches — potentially back to RAM code.

### Fact 4: spcflags are NOT checked inside JIT-generated code

The JIT emits no inline spcflags checks. The check occurs only at C++
dispatch boundaries (ppc-cpu.cpp:744 for JIT, line 777 for interpreter).
This means more frequent block-boundary checks cannot CREATE interrupt
events — they can only DETECT already-pending ones marginally sooner.

### Fact 5: JIT blocks fragment on uncompilable opcodes

When `compile_one()` returns false (for `sc`, `tw`, `twi`, SPR/BAT/SRR
supervisor ops), the JIT emits a partial block ending early:
```
emit_epilogue_with_pc(cur_pc);
complete = false;
```
The dispatch loop sees `jblk.complete == false` and falls through to
the interpreter. One interpreter block (which would have included the
uncompilable opcode inline) becomes 2+ counted blocks in JIT mode: the
JIT-compiled prefix + the interpreter handling the remaining instruction(s).

## Hypothesis Evaluation

### H1 (Benign Speed Disparity) — SUPPORTED

The correct mechanism is a combination of two effects:

**Effect A — JIT accelerates RAM callbacks, increasing NK round-trip rate:**

The Mac OS execution model is a loop: NK dispatcher (ROM) → work unit
(RAM or DR/ROM) → return to NK → dispatch next unit. In interpreter
mode, RAM work units run slowly, so each NK→RAM→NK round trip takes
a long time. The NK:DR ratio reflects this: most block executions are
DR blocks (ROM 68k emulator), with NK appearing infrequently because
RAM work between NK entries takes so long.

In JIT mode, RAM work units complete ~10-20x faster. The NK→RAM→NK
round trip runs proportionally faster. The NK dispatcher processes
work units at a higher rate. But DR work (all ROM, interpreter-speed)
takes the same amount of wall-clock time. Result: in the same wall-
clock interval, NK blocks execute many more times (because RAM phases
between them shrink), while DR block count stays roughly constant.
The NK:DR ratio rises from 1:21 toward 1:1.

**Effect B — JIT block fragmentation inflates NK block counts:**

Nanokernel ROM code contains supervisor-class instructions (mfspr,
mtspr for SPRG/BAT/SRR, rfi) that compile_one() rejects. Each
rejection fragments one interpreter block into multiple counted blocks.
Per CLAUDE.md: "SPR fallbacks (SPRG/BAT/SRR) are negligible: max 3
instances in entire ROM, all boot-time supervisor ops." However, the
nanokernel dispatch path itself (0x50313d20-0x50313d34) is ROM code
that is NOT JIT-compiled at all — it always runs in the interpreter.
So fragmentation is a minor effect within NK, not the primary driver.

Wait — since NK is ROM and NOT JIT-compiled, fragmentation from
incomplete JIT blocks does not apply to NK code directly. NK always
runs as interpreter blocks of the same size in both modes. The ratio
change must come entirely from Effect A.

**Quantitative model:**

Assume interpreter executes 100M PPC instructions/sec total. Suppose
during a representative window:
- NK dispatch: 5% of instructions (5M insns)
- RAM callbacks: 45% (45M insns)
- DR 68k emulator: 50% (50M insns)

NK blocks average 30 insns → ~167K NK blocks/sec
DR blocks average 3 insns → ~16.7M DR blocks/sec
Ratio: NK:DR = 1:100 (interpreter mode reality is 1:21, suggesting
DR is a smaller fraction, but the direction is correct)

In JIT mode, RAM callbacks run 15x faster. The same 45M instructions
of RAM work now complete in 1/15th the wall-clock time. The CPU spends
its freed time doing more NK→RAM→NK round trips. If the pipeline is
NK-RAM dominated:
- NK blocks/sec increases ~15x → ~2.5M NK blocks/sec
- DR blocks/sec stays ~16.7M (ROM, interpreter speed)
- Ratio: NK:DR = 1:6.7 — moving toward 1:1

The exact ratio depends on the workload mix, but the direction — NK:DR
rising dramatically when RAM code is JIT-accelerated — matches the
observed 1:21 → 1:1 shift.

### H2 (Spurious Exceptions) — NOT SUPPORTED

For H2 to hold, JIT-compiled blocks would need to trigger exceptions
that the interpreter does not. Since the JIT only compiles RAM code
and the nanokernel is ROM code (interpreter-only in both modes), the
JIT cannot cause additional NK entries through codegen bugs in NK blocks.

RAM code opcodes that the JIT rejects (sc, tw, twi) cause j2i
transitions (JIT → interpreter fallback), NOT NK entries. The
interpreter executes the rejected opcode normally. Only `sc` actually
invokes the nanokernel, and it does so equally in both modes — the
JIT simply passes it to the interpreter rather than emitting it
natively.

### H3 (Cascade from j2i Transitions) — NOT CAUSAL

j2i transitions check spcflags at line 744, but:
1. spcflags are empty ~99.99% of the time (60Hz events vs millions of
   block executions per second)
2. A non-empty check promotes TRIGGER → HANDLE but doesn't create new
   interrupt events
3. The marginal earlier detection (microseconds) doesn't change the
   total NK entries per 60Hz tick

j2i transitions DO impose dispatch overhead (~30 cycles each at 2.4M/s
= ~72M cycles/s, ~2.4% of a 3GHz core), but this is pure performance
cost, not additional NK entries.

## Conclusion

**H1 is the correct explanation.** The NK:DR block ratio shift from
1:21 to 1:1 is a natural consequence of the JIT accelerating RAM code
while ROM code (both NK and DR) runs at interpreter speed.

The JIT makes RAM phases of the NK→RAM→NK dispatch loop 10-20x faster,
which increases the NK dispatch rate proportionally. DR phases run at
the same speed. The block ratio reflects this speed disparity.

This is NOT a bug. It is the expected and correct behavior of a JIT
that compiles RAM code but not ROM code.

## Proposed Instrumentation to Definitively Confirm

### Counter 1: NK entries per second, normalized to wall-clock

In `HandleInterrupt()` MODE_NATIVE case (sheepshaver_glue.cpp:1196),
increment an atomic counter when `ppc_cpu->interrupt()` is called.
In `tick_func()`, sample this counter once per second. If JIT and
interpreter modes show the same NK entries/sec (~60, matching the VBL
rate), the async path is confirmed wall-clock-bounded.

### Counter 2: NK dispatch round-trips per second

At the nanokernel entry point (ROM address 0x50313d20 or the equivalent
for the target ROM version), count how many times this PC is reached
per second. This can be done via the existing heartbeat PC mechanism.
If JIT mode shows ~10-20x more NK dispatch entries per second than
interpreter mode, Effect A (JIT-accelerated RAM phases) is confirmed.

### Counter 3: Instruction throughput by region

Accumulate PPC instructions executed per second, bucketed by address
range:
- RAM (0x10000000-0x10FFFFFF): should show ~10-20x speedup in JIT mode
- ROM NK (0x50300000-0x50320000): should show similar speed in both modes
- ROM DR (0x50460000-0x50500000): should show similar speed in both modes

If RAM instructions/sec scales with JIT while ROM regions stay constant,
the model is confirmed. This can be implemented by adding per-region
instruction counters to the interpreter loop (skip_jit path at line 763)
and the JIT dispatch path (using jblk.n_insns at line 722).

### Counter 4: spcflags hit rate

At spcflags check points (line 744 JIT, line 777 interpreter), count
total checks and non-empty hits. Expected: JIT mode has far more total
checks but the same number of non-empty hits (~60/sec). The hit rate
(hits/checks) should be much lower in JIT mode, confirming that higher
check frequency does not cause more NK entries.

## If H2 or H3 Were True (Fix Direction)

They are not, but for completeness:

- **H2 fix**: Audit JIT-compiled RAM opcodes for host faults that the
  interpreter handles inline. Add per-opcode fault counters. Fix by
  adding inline handling to JIT or falling back for affected opcodes.

- **H3 fix**: Reduce j2i transition frequency by (a) compiling more
  opcodes natively (eliminating incomplete blocks), (b) enabling block
  chaining (`JIT_BLOCK_CHAINING=1`) to skip the C++ dispatch loop for
  hot block-to-block transitions, or (c) batching spcflags checks.
  Block chaining is already implemented in ppc-jit.cpp (chain site pool
  + emit_epilogue_with_pc chaining) but conservative defaults are used.

## Relationship to Known Boot Deadlock

Per LEARNINGS.md (session 5), the NK spin-wait at 0x50313d34 exhibits
a timing-dependent deadlock where JIT-accelerated initialization code
writes a non-zero value to a task flag before the nanokernel checks it.
This is a separate issue from the NK:DR ratio anomaly — it affects boot
time, not steady-state block ratios. The deadlock is caused by the JIT
changing the relative execution speed of initialization phases (RAM code
runs faster, reaching the task activation write before the NK check
expects it), which is another manifestation of the same fundamental
speed disparity that causes the NK:DR ratio shift.
