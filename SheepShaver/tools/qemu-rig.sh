#!/bin/bash
# qemu-rig.sh — QEMU mac99 differential boot oracle
#
# Boots the 9.0.1 ROM on QEMU mac99 + 9.2.1 installer CD (the S1 reference)
# as a routine instrument: produces a per-run directory with trace logs,
# and a monitor socket for post-boot virtual-memory inspection.
#
# Usage:
#   ./qemu-rig.sh [--rundir DIR] [--timeout SECS] [--trace EVENTS_FILE]
#                 [--gdbstub] [--no-vnc]
#
# Defaults:
#   rundir  = /tmp/qemu-rig-<timestamp>
#   timeout = 50  (Finder boot is ~30s; extra slack for slow machines)
#   trace   = (none — for speed; supply --trace for device-event capture)
#   gdbstub = off (add --gdbstub to enable on port 1234)
#
# Outputs inside rundir:
#   qemu.pid      — QEMU process ID
#   qemu.log      — stdout/stderr from QEMU
#   mon.sock      — QEMU monitor Unix socket
#   trace.log     — trace-events output (if --trace supplied)
#   finder.ppm    — screenshot captured at boot + timeout/2 (if sips available)
#
# First-customer query (VIA-IFR task-0):
#   After boot, use qemu-mon.py to read virtual memory:
#     python3 qemu-mon.py --sock <rundir>/mon.sock \
#       "x /8wx 0x64"          # level-1 interrupt vector
#       "x /2wx 0x168"         # Ticks (confirms system is running)
#       "x /2wx 0xd90"         # $d94 flag tested at 0x5000ee98
#       "x /2wx 0x6e0"         # $6e4 vector chain pointer
#
# Architecture notes (from S1 + rig bringup, 2026-06-12):
#   - QEMU mac99 MacIO is at PCI BAR0 = 0x80000000 (NOT 0xF3000000 like real HW)
#   - VIA (Cuda) = 0x80016000, SCC (ESCC) = 0x80012000
#   - 68k low memory IS at virtual address 0 (NK maps it identically)
#   - Level-1 interrupt vector (virtual 0x64) = 0x47d0ba at Finder
#     (Mac OS 9.2.1 system-installed handler, not the ROM default)
#   - 60 Hz tick arrives via ppc_irq_set pin 5 level 1 (OpenPIC)
#   - The VIA MMIO registers are NOT directly read by the 68k tick handler
#     in QEMU mac99; the Cuda device handles the interrupt internally
#   - The ROM handler at 0x5000ee98 does `tst.l $d94.w; beq; movea.l $6e4.w,a0`
#     (AGENT-CONTEXT said "btst d6,(a4)" — that was WRONG; no such instruction
#     at 0x5000ee9a in the static ROM; it is mid-word of tst.l $d94.w)
#
# Limitations vs real hardware:
#   - OpenBIOS, not Apple OF: oracle valid from NK entry onward
#   - Bare HFS images (.dsk) are not OpenBIOS-bootable; use real-format CD/HD
#   - MacIO MMIO addresses differ from real hardware (PCI-assigned vs hardwired)
#
# See also: docs/planning/spikes/SPIKE-S1-QEMU-GATE-CHECK.md (S1 results)

set -euo pipefail

QEMU_BIN="${QEMU_BIN:-/opt/homebrew/bin/qemu-system-ppc}"
# Canonical test image: 9.2.1 installer with 9.0.1 ROM swapped in.
# If the S1 ephemeral cache doesn't exist, we rebuild it from persistent assets.
CD_IMAGE="${CD_IMAGE:-}"
ROM_NOTE=""

# Persistent paths
ASSETS_DIR="/Users/Shared/macemu"
SRC_ISO="$ASSETS_DIR/Apple Mac OS 9.2.1/macos_921_ppc.iso"
SRC_ROM="$ASSETS_DIR/2001-12-19 - Mac OS ROM 9.0.1.rom"
CACHED_ISO="/tmp/qemu-rig-cd_test_901.iso"   # shared cache, survives across rig runs

if [[ -z "$CD_IMAGE" ]]; then
    CD_IMAGE="$CACHED_ISO"
fi

build_test_iso() {
    echo "Building test ISO (9.2.1 + 9.0.1 ROM swap)..." >&2
    if [[ ! -f "$SRC_ISO" ]]; then
        echo "ERROR: source ISO not found: $SRC_ISO" >&2; exit 1
    fi
    if [[ ! -f "$SRC_ROM" ]]; then
        echo "ERROR: source ROM not found: $SRC_ROM" >&2; exit 1
    fi
    command -v hmount >/dev/null 2>&1 || { echo "ERROR: hfsutils not installed (brew install hfsutils)" >&2; exit 1; }
    command -v python3 >/dev/null 2>&1 || { echo "ERROR: python3 not found" >&2; exit 1; }

    local WORK_ISO="${CD_IMAGE}.tmp"
    cp "$SRC_ISO" "$WORK_ISO"

    # Build MacBinary wrapper for the ROM (type tbxi, creator chrp)
    local MACBIN
    MACBIN=$(mktemp /tmp/qemu-rig-rombin.XXXXXX)
    python3 - "$SRC_ROM" "$MACBIN" << 'PYEOF'
import os, struct, sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
name = "Mac OS ROM"
nb = name.encode("mac_roman")[:63]
h = bytearray(128)
h[1] = len(nb); h[2:2+len(nb)] = nb
h[65:69] = b'tbxi'
h[69:73] = b'chrp'
struct.pack_into(">I", h, 83, len(data))
struct.pack_into(">I", h, 87, 0)                        # no resource fork
mt = int(os.stat(src).st_mtime) + 2082844800
struct.pack_into(">I", h, 91, mt); struct.pack_into(">I", h, 95, mt)
h[122] = 129; h[123] = 129                              # MacBinary III version
def crc16(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if (c & 0x8000) else (c << 1) & 0xFFFF
    return c
struct.pack_into(">H", h, 124, crc16(bytes(h[0:124])))
pad = lambda b: b + b"\x00" * ((128 - len(b) % 128) % 128)
open(dst, "wb").write(bytes(h) + pad(data))
PYEOF

    # Swap ROM into the ISO copy using hfsutils
    hmount "$WORK_ISO" 1 2>/dev/null
    hdel ':System Folder:Mac OS ROM' 2>/dev/null || true
    hcopy -m "$MACBIN" ':System Folder:Mac OS ROM'
    humount 2>/dev/null || true
    rm -f "$MACBIN"
    mv "$WORK_ISO" "$CD_IMAGE"
    echo "Test ISO built: $CD_IMAGE" >&2
}

if [[ ! -f "$CD_IMAGE" ]]; then
    build_test_iso
fi
ROM_NOTE="9.2.1 CD + 9.0.1 ROM ($(basename "$SRC_ROM" 2>/dev/null || echo 'see S1 spike'))"

RUNDIR=""
TIMEOUT=50
TRACE_FILE=""
GDBSTUB=0
NO_VNC=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rundir)   RUNDIR="$2"; shift 2 ;;
        --timeout)  TIMEOUT="$2"; shift 2 ;;
        --trace)    TRACE_FILE="$2"; shift 2 ;;
        --gdbstub)  GDBSTUB=1; shift ;;
        --no-vnc)   NO_VNC=1; shift ;;
        -h|--help)
            sed -n '3,60p' "$0" | grep '^#' | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$RUNDIR" ]]; then
    RUNDIR="/tmp/qemu-rig-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$RUNDIR"

if [[ ! -f "$CD_IMAGE" ]]; then
    echo "ERROR: CD image could not be built. Check assets in $ASSETS_DIR and hfsutils install." >&2
    exit 1
fi

# Build QEMU command
QEMU_ARGS=(
    -M mac99 -m 512
    -drive "file=$CD_IMAGE,format=raw,media=cdrom"
    -boot d
    -display none
    -monitor "unix:$RUNDIR/mon.sock,server,nowait"
)

if [[ -n "$TRACE_FILE" ]]; then
    QEMU_ARGS+=(
        -trace "events=$TRACE_FILE,file=$RUNDIR/trace.log"
    )
fi

if [[ "$GDBSTUB" == "1" ]]; then
    QEMU_ARGS+=(-s)
    echo "GDB stub enabled on port 1234"
fi

echo "=== QEMU rig: $RUNDIR ===" | tee "$RUNDIR/qemu.log"
echo "CD: $CD_IMAGE" | tee -a "$RUNDIR/qemu.log"
echo "ROM: $ROM_NOTE" | tee -a "$RUNDIR/qemu.log"
echo "" | tee -a "$RUNDIR/qemu.log"

"$QEMU_BIN" "${QEMU_ARGS[@]}" >> "$RUNDIR/qemu.log" 2>&1 &
QPID=$!
echo "$QPID" > "$RUNDIR/qemu.pid"
echo "QEMU PID: $QPID"
echo "Monitor: $RUNDIR/mon.sock"
echo "Waiting ${TIMEOUT}s for Finder boot..."

# Wait for boot + optionally screenshot
sleep $((TIMEOUT / 2))

# Take a mid-boot screenshot if the monitor socket exists
if [[ -S "$RUNDIR/mon.sock" ]]; then
    python3 "$(dirname "$0")/qemu-mon.py" \
        --sock "$RUNDIR/mon.sock" \
        "screendump $RUNDIR/midboot.ppm" \
        2>/dev/null || true
fi

sleep $((TIMEOUT / 2))

# Final screenshot + key probes
if [[ -S "$RUNDIR/mon.sock" ]]; then
    PROBE_OUTPUT="$RUNDIR/probes.txt"
    {
        echo "=== Post-boot probes ($TIMEOUT s after launch) ==="
        echo ""
        echo "--- Ticks (virtual 0x168) ---"
        python3 "$(dirname "$0")/qemu-mon.py" --sock "$RUNDIR/mon.sock" "x /2wx 0x168" || echo "(read failed)"
        echo ""
        echo "--- Level-1 interrupt vector (virtual 0x64) ---"
        # Read ONCE and reuse — vector changes during boot so two reads can differ.
        # The probe is intended for a stable post-boot system; the header note warns
        # about short-timeout (mid-boot) runs where the value may still be changing.
        VEC_LINE=$(python3 "$(dirname "$0")/qemu-mon.py" --sock "$RUNDIR/mon.sock" "x /2wx 0x64" 2>/dev/null || true)
        echo "${VEC_LINE:-"(read failed)"}"
        VEC=$(printf '%s' "$VEC_LINE" | grep -o '0x[0-9a-f]*' | head -1)
        if [[ "$TIMEOUT" -lt 30 ]]; then
            echo "  NOTE: timeout=${TIMEOUT}s — system may still be mid-boot; vector may not be stable"
        fi
        if [[ -n "$VEC" ]]; then
            echo "--- Level-1 handler disassembly at $VEC (same read as above) ---"
            python3 "$(dirname "$0")/qemu-mon.py" --sock "$RUNDIR/mon.sock" \
                --disasm "$VEC" --count 128 || echo "(disasm failed)"
        fi
        echo ""
        echo "--- \$d94 flag (tested at ROM 0x5000ee98; 0=no-tick-dispatch) ---"
        python3 "$(dirname "$0")/qemu-mon.py" --sock "$RUNDIR/mon.sock" "x /2wx 0xd90" || echo "(read failed)"
        echo ""
        echo "--- \$6e4 vector chain pointer ---"
        python3 "$(dirname "$0")/qemu-mon.py" --sock "$RUNDIR/mon.sock" "x /2wx 0x6e0" || echo "(read failed)"
        echo ""
        echo "--- Screenshot ---"
        python3 "$(dirname "$0")/qemu-mon.py" --sock "$RUNDIR/mon.sock" "screendump $RUNDIR/finder.ppm" || echo "(screenshot failed)"
        if [[ -f "$RUNDIR/finder.ppm" ]] && command -v sips &>/dev/null; then
            sips -s format png "$RUNDIR/finder.ppm" --out "$RUNDIR/finder.png" 2>/dev/null && \
                echo "PNG: $RUNDIR/finder.png" || true
        fi
    } | tee "$PROBE_OUTPUT"
fi

echo ""
echo "Done. QEMU PID $QPID still running."
echo "Attach: python3 $(dirname "$0")/qemu-mon.py --sock $RUNDIR/mon.sock '<command>'"
echo "Kill:   kill $QPID"
