#!/bin/bash
# qemu-rig.sh — QEMU mac99 differential boot oracle
#
# Boots the 9.0.1 ROM on QEMU mac99 + 9.2.1 installer CD (the S1 reference)
# as a routine instrument: produces a per-run directory with trace logs,
# a monitor socket for post-boot virtual-memory inspection, and a ladder
# probe file that captures key addresses at multiple points during the boot.
#
# Usage:
#   ./qemu-rig.sh [--rundir DIR] [--timeout SECS] [--trace EVENTS_FILE]
#                 [--ladder SECS[,SECS,...]] [--gdbstub] [--no-vnc]
#
# Defaults:
#   rundir  = /tmp/qemu-rig-<timestamp>
#   timeout = 50  (Finder boot is ~30s; extra slack for slow machines)
#   trace   = (none — for speed; supply --trace for device-event capture)
#   ladder  = "3,5,10,30" — probe the interrupt vector and Hnfo record at each
#             of these offsets (seconds after QEMU launch).  Values past --timeout
#             are ignored.  Set to "" to disable ladder probes.
#   gdbstub = off (add --gdbstub to enable on port 1234)
#
# Outputs inside rundir:
#   qemu.pid      — QEMU process ID
#   qemu.log      — stdout/stderr from QEMU
#   mon.sock      — QEMU monitor Unix socket
#   trace.log     — trace-events output (if --trace supplied)
#   probes.txt    — ladder probe results (one block per stage)
#   finder.ppm    — screenshot captured at --timeout
#   finder.png    — PNG version of above (if sips available)
#
# Key probe addresses (see Architecture notes for interpretation):
#   0x64          — 68k level-1 autovector (changes as boot progresses)
#   0x168         — Ticks (non-zero once Mac OS is ticking)
#   0x68ffefd0    — [KDP+0xfd0]: pointer to Hnfo record (when KDP=0x68ffe000)
#   0x68ff4f14    — hnfo_rec+0x14: source-table pointer (NIL = bug, must be set)
#   0x68ff4f28    — hnfo_rec+0x28: pending interrupt bits (0x80000000 = NK set)
#   0xd90         — $d94 flag (tst.l at 0x5000ee98; non-zero = dispatch $6e4)
#   0x6e0         — $6e4 VBL chain pointer (non-zero at Finder)
#
# Architecture notes (from S1 + rig bringup, 2026-06-12; corrections 2026-06-12):
#   - QEMU mac99 MacIO is at PCI BAR0 = 0x80000000 (NOT 0xF3000000 like real HW)
#   - VIA (Cuda) = 0x80016000, SCC (ESCC) = 0x80012000
#   - 68k low memory IS at virtual address 0 (NK maps it identically)
#   - *(0x64) EVOLVES during boot — probe multiple stages to see the ladder:
#       ~3s  : 0x0000xxxx (ROM/NK writing; may still be transitioning)
#       ~5s  : 0x5000ec50 (9.0.1 ROM default — primary dispatch table; `jmp 0x5000ef20`)
#       ~10s : 0x0047d0ba (Mac OS 9.2.1 system handler, RAM-installed)
#     **SheepShaver early-boot** shows 0x5000ed08 (secondary NK PIC dispatch table)
#     at the *first* EXT interrupt — before Mac OS switches it.  The QEMU 5s value
#     (0x5000ec50) may reflect a different 9.0.1 ROM initialization path than ours,
#     or a later stage — exact cause is unresolved.  Use SS_PROBE_68K=0x5000ed08
#     in SheepShaver to observe our actual handler, not this rig.
#   - Hnfo record (KDP+0xfd0 chain, when KDP=0x68ffe000):
#       0x68ffefd0 = pointer to hnfo_rec (= 0x68ff4f00 in our trampoline)
#       hnfo_rec+0x14 = source-table pointer (NIL in our boot — the VIA-IFR bug)
#       hnfo_rec+0x28 = pending bits (0x80000000 set by NK/init in our boot)
#     CAVEAT: QEMU's KDP may differ from our 0x68ffe000 so these addresses may
#     not be meaningful in the QEMU context — read the pointer at 0x68ffefd0 first
#     and dereference only if it is a plausible address.
#   - 60 Hz tick arrives via ppc_irq_set pin 5 level 1 (OpenPIC)
#   - VIA MMIO registers NOT directly read by 68k tick handler in QEMU mac99
#   - ROM dispatch at 0x5000ee98 is `tst.l $d94.w; beq; movea.l $6e4.w,a0`
#     (AGENT-CONTEXT said "btst d6,(a4)" — WRONG; corrected 2026-06-12)
#
# Limitations vs real hardware:
#   - OpenBIOS, not Apple OF: oracle valid from NK entry onward
#   - Bare HFS images (.dsk) are not OpenBIOS-bootable; use real-format CD/HD
#   - MacIO MMIO addresses differ from real hardware (PCI-assigned vs hardwired)
#   - QEMU boots Mac OS 9.2.1 (from the CD); SheepShaver boots 9.0.1 system.
#     Values at Finder may differ.  Values at ~5s (before Mac OS installs handlers)
#     are the best early-ROM comparison point.
#
# See also: docs/planning/spikes/SPIKE-S1-QEMU-GATE-CHECK.md (S1 results)
#           docs/planning/machine/VIA-IFR-RECON.md §5 (session-2 probe findings)

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
LADDER_ARG="3,5,10,30"   # default ladder stages (seconds after QEMU launch)

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rundir)   RUNDIR="$2"; shift 2 ;;
        --timeout)  TIMEOUT="$2"; shift 2 ;;
        --trace)    TRACE_FILE="$2"; shift 2 ;;
        --ladder)   LADDER_ARG="$2"; shift 2 ;;
        --gdbstub)  GDBSTUB=1; shift ;;
        --no-vnc)   NO_VNC=1; shift ;;
        -h|--help)
            sed -n '3,80p' "$0" | grep '^#' | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

# Allow callers to inject extra QEMU args without editing this script.
# Example: QEMU_EXTRA_ARGS="-nographic -vga none" ./qemu-rig.sh
IFS=' ' read -ra QEMU_EXTRA_ARGS_ARR <<< "${QEMU_EXTRA_ARGS:-}"

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

if [[ ${#QEMU_EXTRA_ARGS_ARR[@]} -gt 0 && -n "${QEMU_EXTRA_ARGS_ARR[0]}" ]]; then
    QEMU_ARGS+=("${QEMU_EXTRA_ARGS_ARR[@]}")
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
echo "Waiting ${TIMEOUT}s..."

MON="$(dirname "$0")/qemu-mon.py"
PROBE_OUTPUT="$RUNDIR/probes.txt"

# Helper: run a standard probe set against the monitor socket at a given label.
# Appends output to PROBE_OUTPUT.
run_probe_stage() {
    local label="$1"
    local sock="$2"
    {
        echo "======================================================================"
        echo "=== Probes at ${label}s after launch ==="
        echo "======================================================================"
        echo ""

        echo "--- *(0x64) Level-1 interrupt vector ---"
        VEC_LINE=$(python3 "$MON" --sock "$sock" "x /1wx 0x64" 2>/dev/null || true)
        echo "  ${VEC_LINE:-"(read failed)"}"
        VEC=$(printf '%s' "$VEC_LINE" | grep -oE '0x[0-9a-f]+' | head -1)
        echo ""

        echo "--- *(0x168) Ticks ---"
        python3 "$MON" --sock "$sock" "x /1wx 0x168" 2>/dev/null || echo "  (read failed)"
        echo ""

        echo "--- Hnfo record chain (valid when KDP=0x68ffe000) ---"
        echo "  [0x68ffefd0] KDP+0xfd0 pointer:"
        HNFO_PTR_LINE=$(python3 "$MON" --sock "$sock" "x /1wx 0x68ffefd0" 2>/dev/null || true)
        echo "    ${HNFO_PTR_LINE:-"(read failed)"}"
        HNFO_PTR=$(printf '%s' "$HNFO_PTR_LINE" | grep -oE '0x[0-9a-f]+' | head -1)
        if [[ -n "$HNFO_PTR" && "$HNFO_PTR" != "0x00000000" ]]; then
            echo "  (Hnfo record pointer = $HNFO_PTR — reading field offsets from known addr)"
        fi
        echo "  [0x68ff4f14] hnfo_rec+0x14 source-table-ptr:"
        python3 "$MON" --sock "$sock" "x /1wx 0x68ff4f14" 2>/dev/null || echo "    (read failed)"
        echo "  [0x68ff4f28] hnfo_rec+0x28 pending-bits:"
        python3 "$MON" --sock "$sock" "x /1wx 0x68ff4f28" 2>/dev/null || echo "    (read failed)"
        echo "  [0x68ff4fa8] hnfo_rec+0xa8 source-dev-ptr:"
        python3 "$MON" --sock "$sock" "x /1wx 0x68ff4fa8" 2>/dev/null || echo "    (read failed)"
        echo ""

        echo "--- *(0xd90) \$d94 flag (non-zero → \$6e4 dispatch runs) ---"
        python3 "$MON" --sock "$sock" "x /1wx 0xd90" 2>/dev/null || echo "  (read failed)"
        echo ""

        echo "--- *(0x6e0) \$6e4 VBL chain pointer ---"
        python3 "$MON" --sock "$sock" "x /2wx 0x6e0" 2>/dev/null || echo "  (read failed)"
        echo ""

        if [[ -n "$VEC" && "$VEC" != "0x00000000" ]]; then
            echo "--- Level-1 handler disassembly at $VEC (32 insns) ---"
            python3 "$MON" --sock "$sock" --disasm "$VEC" --count 32 2>/dev/null \
                || echo "  (disasm failed)"
            echo ""
        fi
    } | tee -a "$PROBE_OUTPUT"
}

# Build sorted ladder from LADDER_ARG, filtering points past TIMEOUT.
IFS=',' read -ra RAW_LADDER <<< "$LADDER_ARG"
LADDER=()
for T in "${RAW_LADDER[@]}"; do
    T="${T// /}"  # strip spaces
    if [[ "$T" =~ ^[0-9]+$ ]] && [[ "$T" -lt "$TIMEOUT" ]]; then
        LADDER+=("$T")
    fi
done
# Always include TIMEOUT as the final stage.
LADDER+=("$TIMEOUT")

# Walk the ladder: sleep to each stage, probe if monitor is ready.
PREV=0
for STAGE in "${LADDER[@]}"; do
    DELTA=$(( STAGE - PREV ))
    if [[ "$DELTA" -gt 0 ]]; then
        sleep "$DELTA"
    fi
    PREV="$STAGE"
    if [[ -S "$RUNDIR/mon.sock" ]]; then
        run_probe_stage "$STAGE" "$RUNDIR/mon.sock"
    else
        echo "(monitor not ready at ${STAGE}s)" | tee -a "$PROBE_OUTPUT"
    fi
done

# Final screenshot after the last ladder stage.
if [[ -S "$RUNDIR/mon.sock" ]]; then
    python3 "$MON" --sock "$RUNDIR/mon.sock" "screendump $RUNDIR/finder.ppm" 2>/dev/null || true
    if [[ -f "$RUNDIR/finder.ppm" ]] && command -v sips &>/dev/null; then
        sips -s format png "$RUNDIR/finder.ppm" --out "$RUNDIR/finder.png" 2>/dev/null || true
    fi
fi

echo ""
echo "Done. QEMU PID $QPID still running."
echo "Attach: python3 $(dirname "$0")/qemu-mon.py --sock $RUNDIR/mon.sock '<command>'"
echo "Kill:   kill $QPID"
