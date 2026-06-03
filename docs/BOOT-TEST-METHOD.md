# Boot Test Methodology

Reproducible procedure for testing JIT boot configurations across sessions.

## Prefs (ISO boot — use this for all tests)

```
rom /Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom
cdrom /Users/Shared/macemu/Mac OS 8.6 Internal Edition.iso
ramsize 268435456
screen win/800/600
nosound true
bootdriver -62
nocdrom false
vncserver true
vncport 5999
```

Use the ISO, not the HD image. The HD image (`macos921.dsk`) gets corrupted by
every `kill -9` and triggers Disk First Aid on the next boot, which blocks on a
dialog click we can't send through VNC. The ISO is read-only.

## Test procedure

1. `pkill -9 -x SheepShaver; sleep 1`
2. Start: `SS_JIT_DIAG_LOG=/tmp/jit_test.log ./SheepShaver &>/dev/null &`
3. **t=20s**: VNC screenshot + heartbeat sample
4. **t=90s**: VNC screenshot + heartbeat sample
5. `pkill -9 -x SheepShaver`

### VNC screenshot capture

```bash
python3 /tmp/vnc_capture.py 2>/dev/null
sips -s format png /tmp/ss_vnc_frame.ppm --out /tmp/ss_test.png 2>/dev/null
```

The `vnc_capture.py` script connects to localhost:5999, does an RFB handshake,
requests a raw framebuffer update, and writes a PPM file. Requires libvncserver
compiled in (check for "VNC server enabled" in stderr).

## Pass/fail criteria

| Result | t=20s screen | t=90s screen | Heartbeat pattern |
|--------|-------------|-------------|-------------------|
| **PASS** | Progress bar or desktop | Finder desktop (menu bar) | All counters growing, then rate drops |
| **FAIL-STUCK** | Progress bar | Same progress bar | jNK or jDR frozen, jRAM growing |
| **FAIL-LOOP** | Happy Mac or blank | Same | jRAM=0, SCSIGet in stderr |
| **FAIL-CRASH** | — | — | Process exited |

## Reading heartbeats

Format: `[JIT <time>] blocks=N pc=X | jNK=N jDR=N jRAM=N jOTH=N | iNK=N iDR=N iRAM=N iOTH=N | j2i=N i2j=N`

Key diagnostics:
- **jNK frozen**: nanokernel not executing — interrupts may not be reaching the NK dispatcher
- **jDR frozen**: 68k emulator not running — boot past the 68k-heavy phase or stuck in RAM code
- **jRAM growing fast, others frozen**: tight loop in RAM-resident Mac OS code (extension hang)
- **j2i count**: JIT→interpreter transitions. Should be low (~100/s). If millions/s, spcflags issue.
- **pc field**: sampled at heartbeat time. If same PC across heartbeats, that's the stuck instruction.

## Baselines

| Config | Boot result | Time to desktop |
|--------|------------|-----------------|
| Interpreter (`SS_USE_JIT=0`) | PASS | ~2 min (ISO) |
| JIT, ROM=0x460000, chaining=0 | FAIL-STUCK | hangs at "Starting Up..." |
| JIT, ROM=0x500000, chaining=1 | FAIL-STUCK | hangs at "Starting Up..." |

The "Starting Up..." hang is a **pre-existing JIT bug** confirmed present at commit
77d47baa (before session 7). It affects RAM-resident Mac OS extension code, not ROM or
DR emulator code. Our subfe/adde/mftb fixes are correct and independent of this hang.

## Binary search template

### SS_JIT_INTERP_RANGE

The `SS_JIT_INTERP_RANGE` env var forces the JIT to decline compilation for any block
whose start PC falls in the specified range. The interpreter handles those blocks instead.
This enables binary-searching which RAM code region causes a boot hang.

```bash
# Force interpreter for guest PCs in [0x10650000, 0x10700000):
SS_JIT_INTERP_RANGE=10650000-10700000 ./SheepShaver

# On startup, the JIT logs:
# [JIT] SS_JIT_INTERP_RANGE: [10650000..10700000) forced to interpreter
```

Format: `SS_JIT_INTERP_RANGE=<lo_hex>-<hi_hex>` (no `0x` prefix, lowercase or uppercase hex).
The range is half-open: lo is inclusive, hi is exclusive.

### boot_test() helper

To isolate a failing code region (worked for the subfe bug):

```bash
# Test function — returns PASS/FAIL based on jRAM growth
boot_test() {
    local label=$1; shift
    pkill -9 -x SheepShaver 2>/dev/null; sleep 1
    "$@" SS_JIT_DIAG_LOG=/tmp/jit_test.log ./SheepShaver &>/dev/null &
    local pid=$!
    sleep 90
    local jnk=$(grep '^\[JIT.*blocks=' /tmp/jit_test.log | tail -1 | grep -o 'jNK=[0-9]*' | cut -d= -f2)
    # For the extension hang: check if jNK is still growing (healthy) vs frozen (stuck)
    local jnk_prev=$(grep '^\[JIT.*blocks=' /tmp/jit_test.log | head -3 | tail -1 | grep -o 'jNK=[0-9]*' | cut -d= -f2)
    kill -9 $pid 2>/dev/null
    if [ "${jnk:-0}" -gt "${jnk_prev:-0}" ] && [ "$((jnk - jnk_prev))" -gt 1000000 ]; then
        echo "${label}: PASS (jNK growing: ${jnk_prev} -> ${jnk})"
    else
        echo "${label}: FAIL (jNK frozen at ${jnk})"
    fi
}

# Binary search example using SS_JIT_INTERP_RANGE:
boot_test "interp-10650000-10700000" env SS_JIT_INTERP_RANGE=10650000-10700000
```
