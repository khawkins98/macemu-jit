# Heartbeat Anomaly Warnings (two-tier, colored)

**Date:** 2026-06-03
**Status:** Approved design
**Component:** `jit-heartbeat.hpp` (extends the terminal heartbeat of the same date)

## Problem

The heartbeat shows stats but the reader must know what "bad" looks like. The project has
accumulated hard-won knowledge of failure signatures (LEARNINGS.md); encode them so the
heartbeat itself says "this looks suspicious."

## Design

Each heartbeat evaluates a fixed rule table against the stats it just computed. Findings
append to the HB line as `[SUSPECT: ...]` (any yellow finding) or `[WARN: ...]` (any red
finding); the worst severity colors the whole line via ANSI codes **only when stderr is a
TTY**. The diag log file always gets the plain-text version (no escape codes).

**The warning matrix is canonical in `SheepShaver/docs/DIAGNOSTICS.md`** and mirrored in
the rule comments in `jit-heartbeat.hpp`. Summary:

| Signal | 🟡 SUSPECT | 🔴 WARN | Failure signature it catches |
|---|---|---|---|
| Execution rate | < 50% of running avg | < 0.5M blk/s after 30s | guest stopped making progress |
| Compile freeze (JIT) | comp unchanged 2 HBs @ >1M blk/s | unchanged 5+ HBs | repeat-loop hang (extension-loading hang) |
| JIT↔interp transitions | > 100K/s | > 1M/s | dispatch thrash (old DR bottleneck) |
| Wild PC (OTH region) | any growth | > 1% of all blocks | execution outside RAM/ROM |
| Memory (RSS) | +10% between HBs after 60s | 2× initial or > 2GB | leak / code-cache runaway |
| CPU utilization | < 80% after 30s | < 50% | process starved/waiting (VBL-timer death) |

**Deliberately NOT a rule:** same PC across consecutive heartbeats — documented sampling
artifact (LEARNINGS.md session 5 retraction). This spec exists partly to stop that rule
from ever being added.

**Guards:** no rules fire on the first heartbeat (no baseline); rate/CPU rules need 30s of
warmup; rules silently skip when their input is unavailable (RSS/CPU sampling failure).
Known limitation: post-boot desktop idle may yellow-flag the rate rule.

## Architecture

All inside `jit-heartbeat.hpp`: `hb_state` grows baseline fields (running-average rate,
prev compiled/transitions/OTH/RSS, first RSS, emission count); `hb_tick()` runs the rules
between stat computation and line emission. No call-site changes in ppc-cpu.cpp. No new
files except the dev doc.

## Documentation

- **`SheepShaver/docs/DIAGNOSTICS.md`** (new): heartbeat format, the warning matrix with
  rationale per rule, color/TTY behavior, pointer to diag-log env vars in CLAUDE.md
- **CLAUDE.md** (on-disk, untracked): one-line pointer to DIAGNOSTICS.md in the JIT
  Diagnostic Logging section

## Testing

- Syntax-only compile check; harness regression (vectors never reach a heartbeat)
- Real verification: user boot — healthy boot should show NO color; if it does, thresholds
  need tuning (that feedback loop is expected and cheap)
