#!/usr/bin/env bash
# nw-northstar.sh — one repeatable NewWorld boot-progress SNAPSHOT.
#
# Boots the newworld diagnostic config with the full all-on gate cluster and
# captures the [NW-PROG …] readout (emitted at atexit by main_unix.cpp). This
# is the project's single "how far did the NewWorld boot get?" signal.
#
#   * REPORT-ONLY by default: always exits 0, so it never blocks fluid flow.
#     Wire it into gates.sh as an OBSERVE line, not a failing gate. A
#     regression shows up as a visible NORTHSTAR line, not a red build.
#   * Pass --gate to make it assert the current frontier checkpoint — exits
#     non-zero iff the verdict is REGRESSED (a crash BELOW the frontier, i.e. the
#     68k DR emulator never started). It gates on the DURABLE frontier marker
#     (DR68K), NOT on program_max — program_max=8 (PROGRAM#5/Start68k) is solid
#     since M9 and would pass even if the M10/M11 frontier broke.
#
# The all-on gate set (verified docs/AGENT-CONTEXT.md "Env-gate state"): the
# newworld profile defaults are implied; only the three opt-in / held gates
# need explicit =1. Hold the instrument set CONSTANT across snapshots — do NOT
# add SS_JIT_TRACE_RING here (ring-slowed boots park differently; the frontier
# class is timing-sensitive — AGENT-CONTEXT instrument caveats).
#
# Usage:
#   SheepShaver/tools/nw-northstar.sh [--timeout N] [--gate] [--history FILE]
#
# Env knobs:
#   NW_NS_TIMEOUT   boot timeout seconds (default 75)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

TIMEOUT="${NW_NS_TIMEOUT:-75}"
GATE=0
HISTORY=""
ALL_ON_ENV='SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1'

while [[ $# -gt 0 ]]; do
	case "$1" in
		--timeout) TIMEOUT="$2"; shift 2 ;;
		--gate)    GATE=1; shift ;;
		--history) HISTORY="$2"; shift 2 ;;
		*) echo "nw-northstar: unknown arg: $1" >&2; exit 2 ;;
	esac
done

SHA="$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo '?')"

# Boot via the sanctioned slot protocol (isolated prefs/logs/NVRAM, SIGTERM at
# the deadline so the atexit readout fires). Default template = newworld 9.0.1.
# NOTE: the boot legitimately exits non-zero — SS_TERM_DUMP=1 turns the
# timeout-SIGTERM into exit(1) so the atexit readout fires, and a post-EXT
# frontier crash exits 139. Tolerate it (|| true) or set -e aborts us silently
# before we ever read the readout.
OUT="$("$HERE/ss-slot-boot.sh" --label nw-northstar --timeout "$TIMEOUT" \
		--env "$ALL_ON_ENV" 2>&1 || true)"
RUNDIR="$(printf '%s\n' "$OUT" | sed -n 's/.*RUNDIR=\([^ ]*\).*/\1/p' | head -1)"

if [[ -z "$RUNDIR" || ! -f "$RUNDIR/boot.log" ]]; then
	echo "NORTHSTAR sha=$SHA ERROR: no boot.log (slot boot failed)" >&2
	printf '%s\n' "$OUT" | tail -5 >&2
	"$HERE/ss-reap.sh" >/dev/null 2>&1 || true
	[[ "$GATE" == "1" ]] && exit 3 || exit 0
fi

LOG="$RUNDIR/boot.log"
echo "=== NewWorld boot-progress snapshot (sha=$SHA, timeout=${TIMEOUT}s) ==="
grep -a '^\[NW-PROG' "$LOG" || echo "  (no [NW-PROG] atexit readout — boot crashed before atexit; classifying from durable in-boot markers below)"

# Extract the scalars for the one-line verdict + history. NOTE: every grep here
# may legitimately find nothing (a clean park has no SIGSEGV/EXT lines) — under
# `set -euo pipefail` a non-matching grep aborts the script, so each is wrapped
# in an if-guard or `|| true`, never a bare `grep && VAR=1`.
# SCALAR columns (program_max/dec_expiries/irq_fired) come from the atexit
# readout — RELIABLE only when atexit fires. On the crash branch (~half of runs,
# crash before atexit) they default to 0 and are MEANINGLESS; read the DURABLE
# markers below (dr68k/ext/probe/segv from in-boot greps) as the truth, and the
# verdict, not the raw scalars. (Note: the atexit dr68k= scalar is itself correct
# when printed, but we deliberately use the durable DR68K grep for the verdict +
# history so crash-branch rows stay consistent.)
val() { grep -a "^\[NW-PROG $1\]" "$LOG" 2>/dev/null | sed -n "s/.*$2=\([0-9]*\).*/\1/p" | head -1 || true; }
PROG="$(val nk-stage program_max)"; PROG="${PROG:-0}"
DEXP="$(val sched dec_expiries)";   DEXP="${DEXP:-0}"
IRQ="$(val irq irq_fired)";         IRQ="${IRQ:-0}"
SEGV=0;  if grep -aq '^SIGSEGV' "$LOG"; then SEGV=1; fi
# DURABLE in-boot markers — printed DURING boot, so they survive a crash that
# never reaches the atexit readout (the boot is non-deterministic; ~half of runs
# crash at the known post-EXT wall before atexit — retro 2026-06-13). These, not
# the atexit scalars, are the reliable "how far did we get" evidence.
DR68K=0; if grep -aq '\[DR68K\] first instruction' "$LOG"; then DR68K=1; fi   # 68k DR emulator started
EXT=0;   if grep -aq 'EXT delivered #1' "$LOG"; then EXT=1; fi                # interrupt delivered
PROBE=0; if grep -aq 'PROBE68K 0x5000ed08 match' "$LOG"; then PROBE=1; fi     # M10 criterion fired
EA="$(grep -a -A2 '^SIGSEGV' "$LOG" 2>/dev/null | sed -n 's/.*ea \(0x[0-9a-f]*\).*/\1/p' | head -1 || true)"

# Verdict classification — the KEY design point (retro 2026-06-13): a bare
# SIGSEGV is NOT a regression. The current frontier has a known post-EXT
# wild-execution wall (variable ea: 0x100000 / 0x55590000 / …) reached AFTER the
# 68k DR emulator starts and EXT delivers; the crashing branch made MORE progress
# than a clean park. We classify by the DURABLE markers (did we reach the
# frontier?), crash-resilient, NOT by the presence of SIGSEGV or the maybe-absent
# atexit readout. A real regression = the boot died BELOW the frontier (DR never
# started). Advance DR68K→the next durable marker as later milestones move on.
FRONTIER="$DR68K"   # reaching the 68k DR emulator == "at the current frontier"
if [[ "$FRONTIER" == "0" ]]; then
	VERDICT="REGRESSED(died-early:no-DR68k${SEGV:+,segv@${EA:-?}})"
elif [[ "$SEGV" == "1" ]]; then
	VERDICT="ok(post-frontier-wall@${EA:-?})"   # known, expected at this milestone
elif [[ "$EXT" == "1" ]]; then
	VERDICT="ok(EXT-delivered,parked)"
else
	VERDICT="ok(at-frontier,parked,no-EXT-this-branch)"
fi

echo "[NW-PROG verdict]  sha=$SHA program_max=$PROG dr68k=$DR68K dec_expiries=$DEXP irq_fired=$IRQ ext=$EXT probe=$PROBE segv=$SEGV ea=${EA:-none} -> $VERDICT"

if [[ -n "$HISTORY" ]]; then
	# dr68k/ext/probe/segv are the DURABLE columns (in-boot greps, crash-resilient).
	# program_max/dec_expiries/irq_fired are atexit-only scalars and may read 0 on a
	# crash-branch row even though the boot reached the frontier — trust the verdict.
	[[ -f "$HISTORY" ]] || echo -e "# sha\tprogram_max*\tdr68k\tdec_expiries*\tirq_fired*\text\tprobe\tsegv\tverdict  (*=atexit-only, 0 on crash branch)" > "$HISTORY"
	echo -e "$SHA\t$PROG\t$DR68K\t$DEXP\t$IRQ\t$EXT\t$PROBE\t$SEGV\t$VERDICT" >> "$HISTORY"
fi

"$HERE/ss-reap.sh" >/dev/null 2>&1 || true

if [[ "$GATE" == "1" ]]; then
	case "$VERDICT" in
		ok*) echo "NORTHSTAR: PASS ($VERDICT)"; exit 0 ;;
		*)   echo "NORTHSTAR: FAIL ($VERDICT)"; exit 3 ;;
	esac
fi
echo "NORTHSTAR: observed (report-only)"
exit 0
