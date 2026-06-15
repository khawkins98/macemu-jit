#!/bin/bash
# ss-slot-boot.sh — parallel-boot slot protocol wrapper for SheepShaver diagnostic boots.
#
# Acquires a slot under /tmp/ss-slots/slotN (N=0..7) with an atomic mkdir lock + a
# lease file, generates per-slot prefs from a template, runs an isolated diagnostic
# boot with a SIGTERM timeout (so SS_TERM_DUMP atexit dumps fire), records the exit
# status in the lease, and releases the slot. Never touches other slots' processes.
#
# Usage:
#   ss-slot-boot.sh [options] [-- extra-emulator-args]
#
# Options:
#   --timeout N        Seconds before SIGTERM (default 45; 0 = no timeout)
#   --label NAME       Free-text label recorded in the lease (default: "diag")
#   --env 'K=V K=V'    Extra env vars for the emulator (repeatable; space-separated)
#   --config TEMPLATE  Prefs template file (default: built-in newworld diagnostic
#                      config — replicates /tmp/m2accept.prefs). `disk` lines in the
#                      template are COPIED into the slot run dir and rewritten.
#   --slot N           Request a specific slot (default: first free of 0..7)
#   --binary PATH      Emulator binary (default: <repo>/SheepShaver/src/Unix/SheepShaver,
#                      or $SS_SLOT_BINARY)
#   --grace N          Seconds after SIGTERM before SIGKILL escalation (default 10)
#   --expect 'P1;;P2'  Boot-log assertions: fixed-string patterns (';;'-separated)
#                      that MUST appear in the boot log. Prints one
#                      "EXPECT: n/m present, k absent-violations" line plus a final
#                      "BOOT-VERDICT: PASS|FAIL"; the exit status becomes the
#                      verdict (0 = all assertions hold). Missing patterns are
#                      listed as "EXPECT-MISS: <pat>" lines.
#   --absent 'P1;;P2'  Patterns that must NOT appear in the boot log (requires or
#                      complements --expect; violations listed as "ABSENT-HIT:").
#   -q | --quiet       Only print the final result lines
#
# Default emulator env (caller --env overrides): SS_TERM_DUMP=1 SS_NW_TRAMPOLINE=1
# SS_ROM_LENIENT=1, plus SS_JIT_DIAG_LOG=<rundir>/jit_diag.log (always per-slot).
#
# Output (stdout, machine-parseable):
#   SLOT=N RUNDIR=... EXIT=... LOG=<rundir>/boot.log DIAG=<rundir>/jit_diag.log
#   (+ EXPECT / BOOT-VERDICT lines when --expect/--absent are given)
#
# Exit status: the emulator's exit status (124 if timed out, like timeout(1));
# with --expect/--absent, the BOOT-VERDICT (0=PASS, 3=FAIL) instead — a timed-out
# diagnostic boot whose log contains the expected markers is a PASS.
# See SheepShaver/tools/README-slots.md for the full protocol.

set -euo pipefail

SLOTS_ROOT="${SS_SLOTS_ROOT:-/tmp/ss-slots}"
MAX_SLOT=7
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_BINARY="${SS_SLOT_BINARY:-$SCRIPT_DIR/../src/Unix/SheepShaver}"

TIMEOUT=45
GRACE=10
LABEL="diag"
ENV_EXTRA=()
TEMPLATE=""
WANT_SLOT=""
BINARY="$DEFAULT_BINARY"
QUIET=0
EXTRA_ARGS=()
EXPECT_RAW=""
ABSENT_RAW=""

usage() { sed -n '2,44p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --label)   LABEL="$2"; shift 2 ;;
        --env)     read -r -a _kv <<<"$2"; ENV_EXTRA+=("${_kv[@]}"); shift 2 ;;
        --config)  TEMPLATE="$2"; shift 2 ;;
        --slot)    WANT_SLOT="$2"; shift 2 ;;
        --binary)  BINARY="$2"; shift 2 ;;
        --grace)   GRACE="$2"; shift 2 ;;
        --expect)  EXPECT_RAW="$2"; shift 2 ;;
        --absent)  ABSENT_RAW="$2"; shift 2 ;;
        -q|--quiet) QUIET=1; shift ;;
        -h|--help) usage 0 ;;
        --)        shift; EXTRA_ARGS=("$@"); break ;;
        *)         echo "ss-slot-boot: unknown option: $1" >&2; usage 1 ;;
    esac
done

log() { [[ $QUIET -eq 1 ]] || echo "ss-slot-boot: $*" >&2; }
die() { echo "ss-slot-boot: ERROR: $*" >&2; exit 2; }

[[ -x "$BINARY" ]] || die "emulator binary not executable: $BINARY"
[[ "$TIMEOUT" =~ ^[0-9]+$ ]] || die "--timeout must be an integer"
[[ -z "$TEMPLATE" || -f "$TEMPLATE" ]] || die "template not found: $TEMPLATE"

# Validate caller env tokens early (K=V form).
for kv in "${ENV_EXTRA[@]:-}"; do
    [[ -z "$kv" || "$kv" == *=* ]] || die "--env token is not K=V: $kv"
done

mkdir -p "$SLOTS_ROOT"

# ---------------------------------------------------------------------------
# Slot acquisition (atomic mkdir lock). We may reclaim a slot whose lease
# processes are BOTH provably dead (lease-aware, never by process name).
# ---------------------------------------------------------------------------
lease_pid_alive() {  # $1 = pid (may be empty)
    [[ -n "${1:-}" ]] && kill -0 "$1" 2>/dev/null
}

lease_get() {  # $1 = lease file, $2 = key
    sed -n "s/^$2=//p" "$1" 2>/dev/null | tail -1
}

try_acquire() {  # $1 = slot number; sets SLOT/SLOTDIR/LOCKDIR on success
    local n="$1" dir="$SLOTS_ROOT/slot$1" lock
    lock="$dir/lock"
    mkdir -p "$dir"
    if mkdir "$lock" 2>/dev/null; then
        SLOT="$n"; SLOTDIR="$dir"; LOCKDIR="$lock"
        return 0
    fi
    # Locked: reclaim only if the lease's wrapper AND emulator are both dead.
    local lease="$dir/lease"
    if [[ -f "$lease" ]]; then
        local wpid epid
        wpid="$(lease_get "$lease" wrapper_pid)"
        epid="$(lease_get "$lease" emu_pid)"
        if ! lease_pid_alive "$wpid" && ! lease_pid_alive "$epid"; then
            log "slot$n lease is stale (wrapper=$wpid emu=$epid both dead) — reclaiming"
            rmdir "$lock" 2>/dev/null || true
            if mkdir "$lock" 2>/dev/null; then
                SLOT="$n"; SLOTDIR="$dir"; LOCKDIR="$lock"
                return 0
            fi
        fi
    fi
    return 1
}

SLOT="" SLOTDIR="" LOCKDIR=""
if [[ -n "$WANT_SLOT" ]]; then
    try_acquire "$WANT_SLOT" || die "slot $WANT_SLOT is busy"
else
    for n in $(seq 0 "$MAX_SLOT"); do
        try_acquire "$n" && break
    done
    [[ -n "$SLOT" ]] || die "no free slot (0..$MAX_SLOT) — try ss-reap.sh, or wait"
fi
log "acquired slot$SLOT"

# ---------------------------------------------------------------------------
# Per-run directory + lease
# ---------------------------------------------------------------------------
RUNDIR="$SLOTDIR/runs/$(date +%Y%m%d-%H%M%S).$$"
mkdir -p "$RUNDIR"
LEASE="$SLOTDIR/lease"
BOOTLOG="$RUNDIR/boot.log"
DIAGLOG="$RUNDIR/jit_diag.log"
PREFS="$RUNDIR/prefs"

EMU_PID=""
RELEASED=0
release_slot() {  # $1 = exit status to record
    [[ $RELEASED -eq 1 ]] && return 0
    RELEASED=1
    {
        echo "exit_status=${1:-unknown}"
        echo "end=$(date +%s)"
    } >> "$LEASE" 2>/dev/null || true
    rmdir "$LOCKDIR" 2>/dev/null || true
}

on_signal() {
    # Forward termination to OUR emulator only, then release.
    if [[ -n "$EMU_PID" ]] && kill -0 "$EMU_PID" 2>/dev/null; then
        kill -TERM "$EMU_PID" 2>/dev/null || true
        for _ in $(seq 1 "$GRACE"); do
            kill -0 "$EMU_PID" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "$EMU_PID" 2>/dev/null || true
    fi
    release_slot "interrupted"
    exit 130
}
trap on_signal INT TERM
trap 'release_slot "wrapper-error"' EXIT

# ---------------------------------------------------------------------------
# Prefs generation
# ---------------------------------------------------------------------------
if [[ -n "$TEMPLATE" ]]; then
    # Copy template, copying any `disk` images into the run dir (isolation —
    # mirrors the e2e harness's pristine-per-run-disk rule). `cdrom`/`rom`
    # are read-only media and are left pointing at the shared originals.
    : > "$PREFS"
    while IFS= read -r line || [[ -n "$line" ]]; do
        if [[ "$line" =~ ^disk[[:space:]]+(.+)$ ]]; then
            src="${BASH_REMATCH[1]}"
            if [[ -f "$src" ]]; then
                dst="$RUNDIR/$(basename "$src")"
                log "copying disk image $(basename "$src") into slot run dir"
                cp "$src" "$dst"
                echo "disk $dst" >> "$PREFS"
            else
                log "warning: disk path in template not found, kept as-is: $src"
                echo "$line" >> "$PREFS"
            fi
        else
            echo "$line" >> "$PREFS"
        fi
    done < "$TEMPLATE"
else
    # Built-in default: the standard newworld diagnostic config (m2accept.prefs).
    cat > "$PREFS" <<'EOF'
rom /Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom
ramsize 268435456
nogui true
machine newworld
EOF
fi

# VNC port allocation rule: if the (templated) prefs enable the VNC server,
# force the per-slot port 5900+N so concurrent GUI slots never collide.
if grep -q '^vncserver[[:space:]]*true' "$PREFS"; then
    VNC_PORT=$((5900 + SLOT))
    grep -v '^vncport' "$PREFS" > "$PREFS.tmp" && mv "$PREFS.tmp" "$PREFS"
    echo "vncport $VNC_PORT" >> "$PREFS"
    log "VNC enabled in template — assigned per-slot port $VNC_PORT"
fi

# ---------------------------------------------------------------------------
# Launch
# ---------------------------------------------------------------------------
# HOME=<rundir>: on macOS the emulator writes NVRAM to $HOME/.sheepshaver_nvram
# regardless of --config (xpram_unix.cpp non-__linux__ branch), and a periodic
# watchdog thread saves it — the ONE remaining shared-write artifact between
# concurrent instances. Redirecting HOME puts it per-slot. Override with
# --env HOME=... if a template needs the real home dir.
ENV_DEFAULTS=(SS_TERM_DUMP=1 SS_NW_TRAMPOLINE=1 SS_ROM_LENIENT=1 "HOME=$RUNDIR")
ENV_ALL=("${ENV_DEFAULTS[@]}")
for kv in "${ENV_EXTRA[@]:-}"; do
    [[ -n "$kv" ]] && ENV_ALL+=("$kv")     # later entries override earlier ones in env(1)
done
ENV_ALL+=("SS_JIT_DIAG_LOG=$DIAGLOG")      # always per-slot; not overridable

cat > "$LEASE" <<EOF
slot=$SLOT
label=$LABEL
wrapper_pid=$$
start=$(date +%s)
start_human=$(date '+%Y-%m-%d %H:%M:%S')
rundir=$RUNDIR
binary=$BINARY
timeout=$TIMEOUT
env=${ENV_ALL[*]}
EOF

log "launching (timeout=${TIMEOUT}s label=$LABEL) → $BOOTLOG"
env "${ENV_ALL[@]}" "$BINARY" --config "$PREFS" ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} \
    > "$BOOTLOG" 2>&1 &
EMU_PID=$!
echo "emu_pid=$EMU_PID" >> "$LEASE"

# ---------------------------------------------------------------------------
# Watchdog: SIGTERM at timeout (NOT SIGKILL — SS_TERM_DUMP's atexit dumps must
# fire), SIGKILL only after the grace period. Pure bash; no GNU timeout needed.
# ---------------------------------------------------------------------------
TIMED_OUT=0
if [[ "$TIMEOUT" -gt 0 ]]; then
    (
        sleep "$TIMEOUT"
        if kill -0 "$EMU_PID" 2>/dev/null; then
            kill -TERM "$EMU_PID" 2>/dev/null || true
            for _ in $(seq 1 "$GRACE"); do
                kill -0 "$EMU_PID" 2>/dev/null || exit 0
                sleep 1
            done
            kill -KILL "$EMU_PID" 2>/dev/null || true
        fi
    ) &
    WATCHDOG_PID=$!
fi

set +e
wait "$EMU_PID"
EXIT_STATUS=$?
set -e

if [[ "$TIMEOUT" -gt 0 ]]; then
    if kill -0 "$WATCHDOG_PID" 2>/dev/null; then
        # Emulator finished before the deadline.
        kill "$WATCHDOG_PID" 2>/dev/null || true
        wait "$WATCHDOG_PID" 2>/dev/null || true
    else
        TIMED_OUT=1
    fi
fi
# 143 = died of our SIGTERM; report 124 (timeout convention) if the watchdog fired.
# Note: with SS_TERM_DUMP=1 the emulator turns SIGTERM into exit(1), so a
# timed-out diagnostic boot normally reports its own exit status 1.
if [[ $TIMED_OUT -eq 1 && $EXIT_STATUS -eq 143 ]]; then
    EXIT_STATUS=124
fi

trap - EXIT
STATUS_NOTE="$EXIT_STATUS"
[[ $TIMED_OUT -eq 1 ]] && STATUS_NOTE="$EXIT_STATUS (timed out)"
release_slot "$STATUS_NOTE"

echo "SLOT=$SLOT RUNDIR=$RUNDIR EXIT=$EXIT_STATUS LOG=$BOOTLOG DIAG=$DIAGLOG"

# ---------------------------------------------------------------------------
# Boot-log assertions (--expect / --absent). Fixed-string grep over the boot
# log; ';;'-separated patterns. With assertions present, the VERDICT governs
# the exit status (a timed-out diagnostic boot with the right markers PASSES).
# Without them, behavior is unchanged (backward compatible).
# ---------------------------------------------------------------------------
if [[ -n "$EXPECT_RAW" || -n "$ABSENT_RAW" ]]; then
    set +e
    split_pats() {  # $1 = raw ';;'-separated string → one pattern per line
        [[ -n "$1" ]] && printf '%s\n' "${1//;;/$'\n'}"
    }
    expect_total=0 expect_hit=0 absent_viol=0
    miss_lines=() viol_lines=()
    while IFS= read -r pat; do
        [[ -n "$pat" ]] || continue
        expect_total=$((expect_total + 1))
        if grep -qF -- "$pat" "$BOOTLOG" 2>/dev/null; then
            expect_hit=$((expect_hit + 1))
        else
            miss_lines+=("EXPECT-MISS: $pat")
        fi
    done < <(split_pats "$EXPECT_RAW")
    while IFS= read -r pat; do
        [[ -n "$pat" ]] || continue
        if grep -qF -- "$pat" "$BOOTLOG" 2>/dev/null; then
            absent_viol=$((absent_viol + 1))
            viol_lines+=("ABSENT-HIT: $pat")
        fi
    done < <(split_pats "$ABSENT_RAW")

    echo "EXPECT: $expect_hit/$expect_total present, $absent_viol absent-violations"
    for l in ${miss_lines[@]+"${miss_lines[@]}"} ${viol_lines[@]+"${viol_lines[@]}"}; do
        echo "$l"
    done
    if [[ $expect_hit -eq $expect_total && $absent_viol -eq 0 ]]; then
        echo "BOOT-VERDICT: PASS"
        exit 0
    else
        echo "BOOT-VERDICT: FAIL"
        exit 3
    fi
fi

exit "$EXIT_STATUS"
