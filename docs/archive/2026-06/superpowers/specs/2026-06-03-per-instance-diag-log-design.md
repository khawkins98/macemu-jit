# Per-Instance JIT Diagnostic Log Files

**Date:** 2026-06-03
**Status:** Approved design, pending implementation
**Component:** SheepShaver PPC JIT diagnostics (`ppc-cpu.cpp`)

## Problem

The JIT diagnostic log defaults to a single shared path, `/tmp/jit_diag.log`, opened
with `fopen(..., "w")` (truncate). Any concurrent SheepShaver process that reaches a
heartbeat truncates another instance's log mid-run, silently corrupting measurements.
The current mitigation — requiring parallel agents to set `SS_JIT_DIAG_LOG` to distinct
paths — works only when everyone remembers to do it.

## Design

Each emulator instance writes its diagnostic log to a unique file named from its start
time and PID, and refreshes a stable symlink pointing at the most recent run.

### Naming

- Default log path becomes: `/tmp/jit_diag.<YYYYMMDD-HHMMSS>.<pid>.log`
  - Timestamp = wall-clock time when the log is first opened (first heartbeat).
  - PID guarantees uniqueness even if two instances start within the same second.
- `SS_JIT_DIAG_LOG`, when set and non-empty, overrides everything — exact current
  behavior, no symlink is created in this case.

### Latest-run symlink

- After successfully opening the timestamped file, the code refreshes a symlink:
  `/tmp/jit_diag.log` → `/tmp/jit_diag.<timestamp>.<pid>.log`
- `unlink()` is called first, which also cleanly replaces a stale regular file left by
  a pre-change build.
- Symlink creation failure is **non-fatal**: log a warning to stderr and continue.
  The real log file still works; only the convenience alias is lost.
- Rationale: `tools/jit-analyze.py`, `tail -f /tmp/jit_diag.log`, and all workflows
  documented in CLAUDE.md keep working unchanged — they always see the newest run.

### Scope of the change

All changes are inside `jit_diag_log_open()` in
`SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp` (~10 lines), plus documentation.

No harness changes are needed: test-vector processes (`SS_TEST_HEX`) exit within
milliseconds and never reach the 5-second heartbeat that lazily opens the log, so they
never create log files. This property is existing behavior and is preserved.

## Error Handling

| Failure | Behavior |
|---|---|
| `fopen()` of timestamped path fails | Same as today: `jit_log_file` stays NULL, `JIT_FLOG` lines are dropped, no crash |
| `symlink()` fails | Warning to stderr; logging continues to the real file |
| Stale regular file at `/tmp/jit_diag.log` | Removed by `unlink()` before `symlink()` |
| `SS_JIT_DIAG_LOG` set | Exact current behavior; no timestamp, no symlink |

## Not Doing

- No cleanup/rotation of old logs — files are small, `/tmp` is purged by macOS
  periodically, and old run logs are useful for before/after boot-time comparison.
- No changes to `jit-analyze.py` — the symlink keeps its default path valid.
- No changes to the rom-harness or jit-test harness.

## Testing

1. **Build:** `cd SheepShaver && make build-ss` compiles cleanly.
2. **Single run:** boot the emulator (user-run, per project rules), verify:
   - stderr line `[JIT] diagnostic log: /tmp/jit_diag.<timestamp>.<pid>.log`
   - the timestamped file exists and receives heartbeats
   - `/tmp/jit_diag.log` is a symlink to it
   - `python3 SheepShaver/tools/jit-analyze.py diag` works with no arguments
3. **Override:** run with `SS_JIT_DIAG_LOG=/tmp/custom.log`, verify the custom path is
   used and no symlink is touched.
4. **Harness regression:** `cd SheepShaver && make test-opcodes` still scores 100 and
   creates no `/tmp/jit_diag.*` files.
5. **Stale-file migration:** create a regular file at `/tmp/jit_diag.log`, run the
   emulator, verify it is replaced by a symlink.

## Follow-ups (non-blocking, from code review of 8aab88de)

- **Symlink-alias race**: two default-mode instances starting simultaneously can
  interleave `unlink()`/`symlink()`, leaving `/tmp/jit_diag.log` pointing at whichever
  run won (or emitting a spurious EEXIST warning). Per-instance logs are unaffected —
  only the convenience alias is non-deterministic. Fix if it ever matters:
  `symlink()` to a temp name, then atomic `rename()` over the target.
- **`localtime_r` return value unchecked**: returns NULL on failure → garbage filename.
  Negligible for a `time(NULL)` input; a free guard if the function is ever touched again.
- **Override aliasing the default path**: setting `SS_JIT_DIAG_LOG=/tmp/jit_diag.log`
  explicitly while a default-mode instance runs lets the latter's `unlink()` orphan the
  former's log. Exotic; documents itself away if nobody does that.

## Documentation Updates

- `CLAUDE.md`: update the "JIT Diagnostic Logging" section — default path is now
  per-instance; `/tmp/jit_diag.log` is a symlink to the latest run; `SS_JIT_DIAG_LOG`
  remains the explicit override.
- Comment block above `jit_diag_log_open()`: replace the "parallel agents must set
  distinct paths" requirement with a description of the new scheme.
