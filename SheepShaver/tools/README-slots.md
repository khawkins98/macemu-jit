# Parallel-Boot Slot Protocol

Diagnostic boots used to serialize ALL agent work because every recipe began with
`pkill -9 -x SheepShaver`. That was never necessary: diagnostic boots run with
`--config <file>` (own prefs, no disk, nogui), so isolation already works. The only
genuinely shared artifacts were the prefs default, `/tmp/jit_diag.log`, VNC ports
(when enabled), and — discovered while building this — the NVRAM file
`$HOME/.sheepshaver_nvram`. The slot protocol makes the isolation procedural:
**slots + leases + a reaper, no global pkill ever.**

Acceptance (2026-06-11): two concurrent 45s newworld diagnostic boots (slots 0 and 1)
produced byte-identical frontier signatures to a solo baseline (`[CUDA] packets=13 …
i2c=9`, 5× `[EXC] SC delivered`, term-dump present in both logs). The reaper found and
killed an orphaned emulator (wrapper SIGKILL'd) by its lease PID.

## The rule

> **Never `pkill`/`killall` SheepShaver by name. Ever.**
> Every kill must target a PID recorded in a slot lease (the reaper does this for you).
> Another agent — or the user — may be running an instance you can't see a reason for.

## Quick start (agent recipe — replaces the old pkill preamble)

```bash
# OLD (forbidden):  pkill -9 -x SheepShaver; ./SheepShaver --config /tmp/x.prefs ...
# NEW:
SheepShaver/tools/ss-slot-boot.sh --label my-experiment --timeout 45 \
    --env 'SS_PROBE_PC=0x50312250'
# stdout: SLOT=N RUNDIR=... EXIT=... LOG=<rundir>/boot.log DIAG=<rundir>/jit_diag.log
```

That single command: acquires a free slot under `/tmp/ss-slots/slotN` (N=0..7),
generates per-slot prefs (default = the standard newworld diagnostic config:
9.0.1 ROM, 256MB, nogui, machine newworld), runs the boot with the standard diagnostic
env (`SS_TERM_DUMP=1 SS_NW_TRAMPOLINE=1 SS_ROM_LENIENT=1`) plus your `--env` additions,
SIGTERMs it at the timeout (so the `SS_TERM_DUMP` atexit dumps fire — never SIGKILL
first), records the exit status in the lease, releases the slot, and prints the log
paths. Logs are kept under `slotN/runs/<timestamp>.<pid>/`.

Common variants:

```bash
# Custom prefs template ('disk' lines are COPIED into the slot dir and rewritten;
# 'rom'/'cdrom' are read-only media and stay pointing at the shared originals):
ss-slot-boot.sh --config my-template.prefs --timeout 120 --label hd-boot

# Extra emulator CLI args after --:
ss-slot-boot.sh --label x -- --someflag

# Pin a slot / pick a binary:
ss-slot-boot.sh --slot 3 --binary /path/to/SheepShaver
```

Exit status = the emulator's. With `SS_TERM_DUMP=1` a timed-out boot exits **1**
(SIGTERM is converted to `exit(1)` so atexit dumps run); 124 means the watchdog's
SIGTERM killed it raw (no SS_TERM_DUMP handler).

## Anatomy of a slot

```
/tmp/ss-slots/slotN/
  lock/        # mkdir-based mutex — EXISTS while leased, removed on release
  lease        # key=value record: wrapper_pid, emu_pid, label, start, rundir, env;
               #   exit_status/end appended on release; reaped_* appended by the reaper
  runs/<ts>.<pid>/
    prefs        # generated per-run prefs
    boot.log     # emulator stdout+stderr (term-dump lands here)
    jit_diag.log # SS_JIT_DIAG_LOG, forced per-slot (created lazily by the JIT)
    *.dsk        # per-run copies of any template 'disk' images
```

Isolation provided per run: own prefs (`--config`), own diag log
(`SS_JIT_DIAG_LOG`), own disk copies, and `HOME=<rundir>` for the emulator process —
on macOS the emulator writes NVRAM to `$HOME/.sheepshaver_nvram` regardless of
`--config` (a periodic watchdog thread saves it), so without the HOME redirect two
concurrent instances would race on the user's NVRAM file. Override with
`--env HOME=...` only if you know you need the real home dir. The RPC socket
(`/tmp/sheepshaver-<pid>`) is already per-PID; SDL coreaudio/Metal handle concurrent
instances fine (verified in the acceptance run).

## The reaper: `ss-reap.sh`

```bash
ss-reap.sh                  # kill+release slots whose lease wrapper PID is dead
ss-reap.sh --max-age 300    # also reap leases >300s old with a dead-or-zombie emulator
ss-reap.sh --dry-run        # report only
ss-reap.sh --all            # end-of-wave cleanup: terminate EVERY leased slot's
                            #   recorded emulator PID (TERM → grace → KILL), release all
```

Still lease-aware in every mode: it kills **recorded PIDs only**, after verifying the
PID is still a SheepShaver process (PID-reuse guard) — never pkill by name. It keeps
logs but deletes per-run `*.dsk`/`*.img` copies to reclaim space. Healthy leased slots
are listed and left alone. The wrapper also self-heals: at acquisition it reclaims a
slot whose lease wrapper AND emulator are both provably dead.

## Caveats and rules

- **e2e harness exception**: `make e2e` / `make e2e-bench` run their own isolated
  config OUTSIDE the slot system — their emulator has **no lease**. Do not reap, kill,
  or otherwise interfere while an e2e run is in flight. (`ss-reap.sh` won't touch an
  unleased PID, but don't "help" with manual kills either.)
- **User config is sacred**: never touch `~/.sheepshaver_prefs` or boot against the
  user's prefs/disks. Slot boots always use generated per-run prefs.
- **VNC port allocation**: slot N owns port **5900+N** (slot0→5900 … slot7→5907).
  The wrapper enforces this automatically: if a template enables `vncserver true`, the
  `vncport` line is rewritten to the slot's port. The user's interactive config keeps
  5999. Headless `nogui` diagnostic boots use no VNC port at all.
- **Before booting outside the slot system** (you generally shouldn't): check
  `pgrep -lx SheepShaver` and correlate any PIDs against `/tmp/ss-slots/slot*/lease`
  before assuming a process is yours to kill.
- `SS_SLOTS_ROOT` env var relocates the slot tree (default `/tmp/ss-slots`) — useful
  for tests of the protocol itself.
- Slot capacity is 8 concurrent boots; each diagnostic boot holds ~256MB guest RAM, so
  that is also a sensible resource ceiling.
