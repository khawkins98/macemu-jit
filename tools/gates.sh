#!/bin/bash
# gates.sh — run a MILESTONE-WORKFLOW §6 gate tier and report ONE line per gate.
#
# Usage:
#   tools/gates.sh <inner|task|full> [--reason "why this tier"]
#
# Tiers (docs/MILESTONE-WORKFLOW.md §6):
#   inner  per-commit (~1 min warm):
#            build-ss · SS_HARNESS_BATCH=1 test-jit · machine suite
#   task   per-task (final commit): inner + plain test-jit (authoritative) + e2e-test
#   full   risk-based: task + paravirtual `make e2e` (boots the emulator!)
#
# Output contract (what agents read INSTEAD of full gate logs):
#   GATE <name>: PASS|FAIL (<detail>, <N>s)        — one line per gate
#   ...on FAIL: the failing gate's last 20 log lines, then stop (fail-fast)
#   GATES <tier>: PASS|FAIL (<k>/<n> gates[, reason: ...])  — final verdict
#
# Exit status: 0 iff every gate in the tier passed. Per-gate logs are kept in
# $GATES_TMPDIR (default: a fresh mktemp dir, path printed at the end) for audit.
#
# NOTE on `full`: paravirtual `make e2e` is REQUIRED only when the change touches
# code reachable on paravirtual. When every new line is structurally inside
# newworld/env gates, the inertness argument + a gated-off byte-identical A/B boot
# SUBSTITUTE for it — state which in the commit message (§6 "Risk-based").
# `make e2e` boots an emulator (isolated slot-style config) — needs a GUI session.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SS_DIR="$REPO_ROOT/SheepShaver"

TIER="${1:-}"
shift 2>/dev/null || true
REASON=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --reason) REASON="${2:-}"; shift 2 ;;
        -h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "gates.sh: unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$TIER" in
    inner|task|full) ;;
    *) echo "usage: tools/gates.sh <inner|task|full> [--reason \"...\"]" >&2; exit 2 ;;
esac

TMPDIR_GATES="${GATES_TMPDIR:-$(mktemp -d /tmp/gates.XXXXXX)}"
mkdir -p "$TMPDIR_GATES"

PASSED=0
TOTAL=0
FAILED_GATE=""

# run_gate <name> <detail-extractor: metric|pytest|machine|none> <cmd...>
run_gate() {
    local name="$1" extractor="$2"; shift 2
    local logf="$TMPDIR_GATES/$(echo "$name" | tr ' /' '__').log"
    local t0 t1 rc detail
    TOTAL=$((TOTAL + 1))
    t0=$(date +%s)
    "$@" > "$logf" 2>&1
    rc=$?
    t1=$(date +%s)

    detail="ok"
    case "$extractor" in
        metric)
            # METRIC lines: "METRIC pass=N" / "METRIC fail=N" / "METRIC total=N" /
            # "METRIC score=N" (one field per line; take the last of each).
            local mp mf mt ms
            mp=$(sed -n 's/^METRIC pass=\([0-9]*\)$/\1/p'  "$logf" | tail -1)
            mf=$(sed -n 's/^METRIC fail=\([0-9]*\)$/\1/p'  "$logf" | tail -1)
            mt=$(sed -n 's/^METRIC total=\([0-9]*\)$/\1/p' "$logf" | tail -1)
            ms=$(sed -n 's/^METRIC score=\([0-9]*\)$/\1/p' "$logf" | tail -1)
            if [[ -n "$ms" ]]; then
                detail="pass=${mp:-?} fail=${mf:-?} total=${mt:-?} score=$ms"
                # A score below 100 is a FAIL even if make exited 0.
                [[ "$ms" == "100" ]] || rc=1
            else
                detail="no METRIC score line"; rc=1
            fi
            ;;
        machine)
            local suites fails
            suites=$(grep -Ec '^(ALL PASS|.*ALL PASS)' "$logf")
            fails=$(grep -c 'FAIL' "$logf")
            detail="${suites} suites ALL PASS"
            [[ "$fails" -gt 0 ]] && { detail="${fails} FAIL line(s)"; rc=1; }
            ;;
        pytest)
            local p
            p=$(grep -Eo '[0-9]+ (passed|failed)[^"]*' "$logf" | tail -1)
            [[ -n "$p" ]] && detail="$p"
            grep -Eq '[0-9]+ failed' "$logf" && rc=1
            ;;
        none) ;;
    esac

    if [[ $rc -eq 0 ]]; then
        echo "GATE $name: PASS ($detail, $((t1 - t0))s)"
        PASSED=$((PASSED + 1))
        return 0
    else
        echo "GATE $name: FAIL ($detail, $((t1 - t0))s) — log: $logf"
        echo "---- last 20 lines of $name ----"
        tail -20 "$logf"
        echo "--------------------------------"
        FAILED_GATE="$name"
        return 1
    fi
}

verdict() {
    local status="$1" extra=""
    [[ -n "$FAILED_GATE" ]] && extra=", failed: $FAILED_GATE"
    [[ -n "$REASON" ]] && extra="$extra, reason: $REASON"
    echo "GATES $TIER: $status ($PASSED/$TOTAL gates$extra) [logs: $TMPDIR_GATES]"
}

[[ -n "$REASON" ]] && echo "gates.sh: tier=$TIER reason: $REASON"

# ---- inner tier (every tier includes it) ------------------------------------
run_gate "build-ss" none \
    make -C "$SS_DIR" build-ss \
 && run_gate "test-jit (batch)" metric \
    env SS_HARNESS_BATCH=1 make -C "$SS_DIR" test-jit \
 && run_gate "machine suite" machine \
    make -C "$SS_DIR/src/machine" test \
 || { verdict FAIL; exit 1; }

# ---- task tier ---------------------------------------------------------------
if [[ "$TIER" == "task" || "$TIER" == "full" ]]; then
    run_gate "test-jit (plain, authoritative)" metric \
        make -C "$SS_DIR" test-jit \
     && run_gate "e2e-test (offline)" pytest \
        make -C "$SS_DIR" e2e-test \
     || { verdict FAIL; exit 1; }
fi

# ---- full tier ---------------------------------------------------------------
if [[ "$TIER" == "full" ]]; then
    cat >&2 <<'EOF'
================================================================================
 REMINDER (MILESTONE-WORKFLOW §6 "Risk-based"): paravirtual `make e2e` is
 REQUIRED only when the change touches code reachable on paravirtual.
 If every new line is structurally inside newworld/env gates, the structural-
 inertness argument + the gated-off byte-identical A/B boot SUBSTITUTE for this
 boot — state which evidence you used in the commit message.
 This gate BOOTS THE EMULATOR (needs a GUI session; shares the machine —
 check `pgrep -lx SheepShaver` etiquette).
================================================================================
EOF
    run_gate "e2e (paravirtual smoke)" none \
        make -C "$SS_DIR" e2e \
     || { verdict FAIL; exit 1; }
fi

verdict PASS
exit 0
