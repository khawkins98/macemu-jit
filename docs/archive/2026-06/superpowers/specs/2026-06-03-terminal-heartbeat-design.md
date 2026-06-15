> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone shipped
>

# Terminal Heartbeat for SheepShaver Runs

**Date:** 2026-06-03
**Status:** Approved design
**Component:** SheepShaver PPC CPU loop diagnostics

## Problem

stderr (the terminal) only shows one-off events (init, HOT-PC, STALL). A quiet terminal is
ambiguous between "healthy boot in progress" and "hung". The detailed heartbeat exists but
goes only to the diag log file, which requires a second terminal to tail.

## Design

A heartbeat line printed to **stderr** (and appended to the diag log file) on a decaying
cadence: **every 10s for the first minute, then every 60s**.

### Line format

```
[HB 30s] blocks=812M (28.4M/s) comp=3214 | jNK=89M jDR=655M jRAM=8M j2i=12K | rss=412MB cpu=98%
[HB 12m] blocks=4.1G (29.1M/s) comp=3514 | jNK=...                          | rss=455MB cpu=97%
```

Interpreter mode (SS_USE_JIT=0) prints `iNK/iDR/iRAM/i2j` instead of `comp=`/`j*` fields so
the two modes stay visually comparable.

Stats: total blocks + blocks/sec (existing counters), compiled block count (existing),
region mix + transitions (existing counters), host RSS in MB (`task_info` on macOS,
`getrusage` elsewhere), CPU% of one core since the last heartbeat (`getrusage` deltas).

### Architecture

- **New header** `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/jit-heartbeat.hpp` —
  header-only, owns everything: `hb_state` struct, cadence logic (`10s → 60s`), RSS/CPU
  sampling, count formatting (K/M/G), line emission to stderr + optional FILE*.
- **Call sites** in `ppc-cpu.cpp`: one `#include`, plus a ~4-line block inside each of the
  two existing 5s heartbeat sites (JIT loop and interpreter loop). The 5s file-heartbeat
  check is the driver — no new timers, no new threads, no added per-block cost. Since 10
  and 60 are multiples of 5, the cadence falls out of the existing check.

### Cadence rule

`next_due` starts at 10s; after each emission `next_due += (next_due < 60 ? 10 : 60)`,
with catch-up (`while next_due <= now: next_due += 60`) so a debugger pause doesn't cause
a burst of lines. Emission only happens when the guest is executing blocks (same condition
as the existing heartbeat) — a fully-hung dispatch loop produces no HB lines, which is
itself the signal (existing documented behavior).

### Error handling

- RSS/CPU syscall failure → print `rss=? cpu=?`, never abort or skip the line
- diag log file not open / NULL → stderr only
- All code inside the existing `#if defined(__aarch64__) && defined(USE_AARCH64_JIT)` guard

## Not Doing

- No BasiliskII equivalent (can be ported later; header is self-contained)
- No env var to tune cadence (YAGNI — hardcoded 10s/60s; add a knob only if someone needs it)
- No changes to the existing 5s file heartbeat or its format (other agent's tooling parses it)

## Testing

- Syntax-only compile check now (other agent's boot is running; no rebuild)
- Harness regression after rebuild: 235/235, and test vectors must produce NO HB lines
  (they exit in <10s)
- Real verification: a user boot showing 10s,20s,...,60s,2m,3m rhythm on the terminal

## Follow-ups

- Consider porting to BasiliskII's newcpu.cpp once proven useful here
