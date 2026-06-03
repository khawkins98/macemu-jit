# SheepShaver Runtime Diagnostics

Developer reference for the live diagnostics emitted by SheepShaver on macOS arm64
(`ppc-cpu.cpp` + `jit-heartbeat.hpp`). For the diag-log env vars and analysis tooling,
see the "JIT Diagnostic Logging" section of the repo-root CLAUDE.md and
`SheepShaver/tools/jit-analyze.py`.

## Terminal heartbeat

A liveness + stats line printed to **stderr** (and mirrored into the diag log file) on a
decaying cadence: **every 10s for the first minute of guest execution, then every 60s**.

```
[HB 30s] blocks=812M (28.4M/s) comp=3214 | jNK=89M jDR=655M jRAM=8M j2i=12K | rss=412MB cpu=98%
[HB 12m] blocks=4.1G (29.1M/s) comp=3514 | jNK=...                          | rss=455MB cpu=97%
```

| Field | Meaning |
|---|---|
| `blocks` / `(M/s)` | total guest blocks executed by this loop, and the rate since the last HB |
| `comp` | JIT-compiled block count (JIT mode only) |
| `jNK/jDR/jRAM` (or `iNK/iDR/iRAM`) | blocks executed per region: nanokernel/toolbox ROM, 68k DR emulator, guest RAM |
| `j2i` (or `i2j`) | JIT↔interpreter dispatch transitions |
| `rss` | host process resident memory |
| `cpu` | % of one core used since the last HB (user+sys) |

Interpreter mode (`SS_USE_JIT=0`) prints the `i*` variants so the two modes compare directly.

**Silence is a signal**: a fully-hung dispatch loop produces *no* HB lines (the heartbeat
is driven by block execution). If lines stop appearing, the loop is frozen.

Implementation: `src/kpx_cpu/src/cpu/jit/aarch64/jit-heartbeat.hpp`, called from the two
existing 5s heartbeat sites in `ppc-cpu.cpp`. Test vectors (`SS_TEST_HEX`) exit in
milliseconds and never produce HB lines.

## Warning matrix (anomaly detection)

Each heartbeat evaluates these rules. Findings append to the line as
`[SUSPECT: ...]` (yellow) or `[WARN: ...]` (red); the worst finding colors the whole
stderr line (ANSI, only when stderr is a TTY — redirected output and the log file always
get plain text).

| Signal | 🟡 SUSPECT | 🔴 WARN | Failure signature it catches |
|---|---|---|---|
| **Execution rate** | < 50% of running average | < 0.5M blk/s after 30s | guest stopped making progress |
| **Compile freeze** (JIT) | `comp` unchanged 2 HBs while rate > 1M/s | unchanged 5+ HBs | repeat-loop hang — same compiled blocks cycling forever (the extension-loading-hang signature) |
| **JIT↔interp transitions** | > 100K/s | > 1M/s | dispatch thrash (the pre-DR-JIT 2.4M/s bottleneck signature) |
| **Wild PC** (OTH region) | any growth since last HB | > 1% of all blocks | guest executing outside RAM/ROM — corrupted PC |
| **Memory (RSS)** | +10% between HBs after 60s | 2× initial, or > 2GB | leak / code-cache runaway |
| **CPU utilization** | < 80% after 30s | < 50% | process starved or waiting — e.g. host timer death (VBL) |

### Rules that are deliberately ABSENT

**"Same PC across consecutive heartbeats" is NOT a warning rule and must not be added.**
The heartbeat samples the PC at a point in time; the hottest PC in the system (the
nanokernel exception dispatcher at 0x50313d34) recurs across samples by chance. An entire
wrong "deadlock" theory plus a guest-corrupting fix was built on this misreading — see
LEARNINGS.md "session 5 part 2 — RETRACTION".

### Guards / known limitations

- No rules fire on the **first** heartbeat (no baseline yet)
- Rate and CPU rules need 30s of warmup (boot start is legitimately erratic)
- Rules whose inputs are unavailable (RSS/CPU sampling failure → shown as `rss=? cpu=?`)
  silently skip — missing data never produces a warning
- **Post-boot desktop idle** may yellow-flag the rate rule (an idle guest legitimately
  slows down). Treat SUSPECT as "look", not "broken".
- Thresholds are first-pass guesses encoded from LEARNINGS.md history. If a healthy boot
  shows color, tune the thresholds in `jit-heartbeat.hpp` and update this table — both
  copies of the matrix (this file + the header comment) must stay in sync.

## Other live diagnostics (pre-existing, ppc-cpu.cpp)

| Output | Trigger | Notes |
|---|---|---|
| `[JIT] diagnostic log: <path>` | first heartbeat | per-instance file; `/tmp/jit_diag.log` symlinks to latest run |
| `[JIT Ns] HOT-PC ...` | same sampled PC 3+ heartbeats | **sampling hint only** — see retraction note above |
| `[JIT Ns] STALL: comp=...` | `SS_JIT_RING_DUMP_ON_STALL=<n>` | one-shot trace-ring dump on compile freeze |
| `[JIT Ns] interrupt delivered` | each guest interrupt | diag log file only |
