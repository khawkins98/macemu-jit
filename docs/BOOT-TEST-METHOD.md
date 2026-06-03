# Boot Test Methodology

Standard procedure for testing JIT boot configurations.

## Configuration

- **ROM**: OldWorld (`1998-07-21 - Mac OS ROM 1.1.rom`)
- **Boot media**: Mac OS 8.6 ISO (read-only, no corruption risk)
- **Prefs**: `bootdriver -62`, `nocdrom false`, VNC on port 5999

## Test procedure

1. Kill any running SheepShaver: `pkill -9 -x SheepShaver`
2. Start with desired config + `SS_JIT_DIAG_LOG=/tmp/jit_test.log`
3. **Screenshot at t=20s** — should show Happy Mac or progress bar
4. **Screenshot at t=90s** — should show desktop if boot succeeded
5. **Check heartbeats** — jNK/jDR/jRAM all growing = healthy; any frozen = stuck
6. Kill at 90s regardless

## Pass/fail criteria

- **PASS**: VNC screenshot at 90s shows Finder desktop (menu bar visible)
- **FAIL-STUCK**: Progress bar visible but not advancing; jNK or jDR frozen
- **FAIL-LOOP**: SCSIGet/SCSISelect repeating in stderr; jRAM=0
- **FAIL-CRASH**: Process exited before 90s

## Baseline (interpreter)

Interpreter boots 8.6 ISO to Finder desktop in ~2 minutes. Confirmed working.

## VNC capture

```bash
python3 /tmp/vnc_capture.py 2>&1
sips -s format png /tmp/ss_vnc_frame.ppm --out /tmp/ss_test.png
```

## Quick test function

```bash
boot_test() {
    local label=$1; shift
    pkill -9 -x SheepShaver 2>/dev/null; sleep 1
    SS_JIT_DIAG_LOG=/tmp/jit_test.log "$@" \
        /path/to/SheepShaver &>/dev/null &
    local pid=$!
    sleep 20
    python3 /tmp/vnc_capture.py 2>/dev/null
    sips -s format png /tmp/ss_vnc_frame.ppm --out "/tmp/ss_${label}_20s.png" 2>/dev/null
    sleep 70
    python3 /tmp/vnc_capture.py 2>/dev/null
    sips -s format png /tmp/ss_vnc_frame.ppm --out "/tmp/ss_${label}_90s.png" 2>/dev/null
    grep '^\[JIT.*blocks=' /tmp/jit_test.log | tail -3
    kill -9 $pid 2>/dev/null
}
```
