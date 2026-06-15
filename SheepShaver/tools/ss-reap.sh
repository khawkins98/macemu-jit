#!/bin/bash
# ss-reap.sh — lease-aware reaper for the SheepShaver slot protocol.
#
# Kills ONLY SheepShaver PIDs whose slot lease is stale, and releases their
# slots. NEVER uses pkill-by-name; every kill targets a PID recorded in a
# lease, verified to still be a SheepShaver process (PID-reuse guard).
#
# A leased slot is STALE when:
#   * the lease's wrapper_pid is dead (orphaned emulator), or
#   * --max-age N is given, the lease is older than N seconds, AND the
#     emulator process is dead or a zombie.
#
# Usage:
#   ss-reap.sh [--max-age N] [--dry-run] [--all]
#
#   --max-age N   Also treat leases older than N seconds with a dead-or-zombie
#                 emulator as stale.
#   --all         End-of-wave cleanup: terminate EVERY leased slot's recorded
#                 emulator PID (SIGTERM, then SIGKILL after grace) and release
#                 all slots. Still lease-aware — no pkill by name, and PIDs are
#                 verified to be SheepShaver before any signal is sent.
#   --dry-run     Report what would be done without killing/cleaning.
#
# Cleanup keeps logs (boot.log / jit_diag.log / lease records) but deletes
# per-run disk-image copies (*.dsk, *.img) to reclaim space.
#
# DO NOT run during a `make e2e` — the e2e harness runs its own isolated
# config OUTSIDE the slot system; its emulator has no lease but must not be
# touched (this script won't touch it, but --all impatience plus a manual
# pkill habit will. No pkill. Ever.)

set -euo pipefail

SLOTS_ROOT="${SS_SLOTS_ROOT:-/tmp/ss-slots}"
MAX_SLOT=7
GRACE=10
MAX_AGE=""
DRY_RUN=0
ALL=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --max-age) MAX_AGE="$2"; shift 2 ;;
        --all)     ALL=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "ss-reap: unknown option: $1" >&2; exit 2 ;;
    esac
done

[[ -d "$SLOTS_ROOT" ]] || { echo "ss-reap: no slots root at $SLOTS_ROOT — nothing to do"; exit 0; }

lease_get() { sed -n "s/^$2=//p" "$1" 2>/dev/null | tail -1; }

pid_alive()  { [[ -n "${1:-}" ]] && kill -0 "$1" 2>/dev/null; }
pid_state()  { ps -p "${1:-0}" -o state= 2>/dev/null | tr -d ' ' ; }
pid_is_ss()  { [[ "$(ps -p "${1:-0}" -o comm= 2>/dev/null)" == *SheepShaver* ]]; }

kill_emu() {  # $1 = pid; SIGTERM → grace → SIGKILL. PID identity verified by caller.
    local pid="$1"
    kill -TERM "$pid" 2>/dev/null || true
    for _ in $(seq 1 "$GRACE"); do
        pid_alive "$pid" || return 0
        sleep 1
    done
    kill -KILL "$pid" 2>/dev/null || true
}

DID_ANYTHING=0
for n in $(seq 0 "$MAX_SLOT"); do
    dir="$SLOTS_ROOT/slot$n"
    lock="$dir/lock"
    lease="$dir/lease"
    [[ -d "$lock" ]] || continue            # not leased — nothing to reap

    if [[ ! -f "$lease" ]]; then
        echo "slot$n: LOCK WITHOUT LEASE (half-acquired or corrupt) — releasing lock"
        [[ $DRY_RUN -eq 1 ]] || rmdir "$lock" 2>/dev/null || true
        DID_ANYTHING=1
        continue
    fi

    wpid="$(lease_get "$lease" wrapper_pid)"
    epid="$(lease_get "$lease" emu_pid)"
    label="$(lease_get "$lease" label)"
    start="$(lease_get "$lease" start)"
    age="?"
    [[ -n "$start" ]] && age=$(( $(date +%s) - start ))

    stale_reason=""
    if [[ $ALL -eq 1 ]]; then
        stale_reason="--all requested"
    elif ! pid_alive "$wpid"; then
        stale_reason="wrapper pid $wpid dead"
    elif [[ -n "$MAX_AGE" && "$age" != "?" && "$age" -gt "$MAX_AGE" ]]; then
        st="$(pid_state "$epid")"
        if ! pid_alive "$epid" || [[ "$st" == Z* ]]; then
            stale_reason="lease age ${age}s > ${MAX_AGE}s and emulator dead-or-zombie (state=${st:-gone})"
        fi
    fi

    if [[ -z "$stale_reason" ]]; then
        echo "slot$n: leased and healthy (label=$label wrapper=$wpid emu=$epid age=${age}s) — leaving alone"
        continue
    fi

    DID_ANYTHING=1
    echo "slot$n: STALE (label=$label wrapper=$wpid emu=$epid age=${age}s): $stale_reason"

    # Kill the emulator only if the recorded PID is alive AND still SheepShaver.
    if pid_alive "$epid"; then
        if pid_is_ss "$epid"; then
            if [[ $DRY_RUN -eq 1 ]]; then
                echo "slot$n:   would kill SheepShaver pid $epid (TERM, then KILL after ${GRACE}s)"
            else
                echo "slot$n:   killing SheepShaver pid $epid (TERM, then KILL after ${GRACE}s)"
                kill_emu "$epid"
            fi
        else
            echo "slot$n:   pid $epid is alive but NOT SheepShaver (PID reuse) — not killing"
        fi
    else
        echo "slot$n:   emulator pid ${epid:-<none>} already dead"
    fi

    if [[ $DRY_RUN -eq 1 ]]; then
        echo "slot$n:   would release slot + delete per-run disk copies (logs kept)"
        continue
    fi

    # Record the reap in the lease, release the lock, drop bulky disk copies.
    {
        echo "reaped_by=$$ ($(whoami))"
        echo "reaped_at=$(date '+%Y-%m-%d %H:%M:%S')"
        echo "reaped_reason=$stale_reason"
    } >> "$lease" 2>/dev/null || true
    rmdir "$lock" 2>/dev/null || true
    find "$dir/runs" \( -name '*.dsk' -o -name '*.img' \) -type f -delete 2>/dev/null || true
    echo "slot$n:   released (logs kept under $dir/runs)"
done

[[ $DID_ANYTHING -eq 1 ]] || echo "ss-reap: nothing stale; all slots free or healthy"
