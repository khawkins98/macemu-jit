#!/bin/bash
# run-exc.sh — Wave-2 W2-1 deliverability vectors (H1–H5).
#
# The EE/interrupt-delivery chain exercised at harness level: real mtmsr/rfi
# EE edges driving real DEC deliveries (and the sc class) with NO boot — plan
# docs/superpowers/plans/2026-06-11-wave2-interrupt-chain.md Task W2-1
# (+ rev 2 F1/F2/F11/F12).
#
# SEPARATE from run.sh's legacy table BY DESIGN: these vectors are
# env-dependent (SS_TEST_DEC_PENDING / SS_TEST_MSR / SS_TEST_EXC_STUB /
# SS_EXC_ENTRY / SS_MACHINE) and MUST NOT enter the 353-table, whose contract
# is env-free determinism. Each vector runs as its own process (no batch):
# the env differs per vector, and H5-unresolved's observable is the FATAL
# capture + exit code, which is only clean process-isolated (rev 2 F12).
#
# Every gating vector runs in BOTH modes (interp + JIT) and the REGDUMPs are
# diffed byte-for-byte, then the interp REGDUMP is checked against the pinned
# expected fields. Honest note: mtmsr/rfi/sc are non-compilable, so the JIT
# runs devolve to the interpreter for the edge instructions themselves — the
# equivalence diff is the containment proof that the JIT dispatch path
# (block-entry polls, fallback handoff) delivers identically, not a codegen
# exercise of mtmsr.
#
# Capture-stub ABI (SS_TEST_EXC_STUB at guest 0x1000C000):
#   mfmsr r20; mfspr r21,srr0; mfspr r22,srr1; blr
# REGDUMP has no MSR/SRR0/SRR1 — the stub lands them in GPRs.
#
# Delivery-point geometry (why the vectors end in `b .` — branch-to-self):
# the interpreter polls spcflags at BLOCK boundaries, and the EE-edge raise is
# TWO polls from delivery (poll 1 converts TRIGGER->HANDLE, poll 2 delivers).
# A `b .` after the edge instruction parks the PC for exactly those two polls,
# so the restart SRR0 is deterministic (H1: 0x1000400C = the b-self after
# mtmsr; H3: the rfi target). Without delivery a b-self would spin — which is
# why H2 (the no-edge control) uses a straight-line tail instead, and why
# every run is under a hard timeout.
#
# Output: EXC-VECTOR <name> PASS/FAIL lines + the standard METRIC line.
# H4 (deferral telemetry) is reported but NON-GATING per the plan — it rides
# the legacy HandleInterrupt fall-through (SDL_PumpEvents etc.), kept out of
# the metric so an SDL environment quirk cannot redden the lane.
#
# Depth-deferral honesty (carried from the plan): EXC_DECIDE_DEFER_DEPTH
# (execute_depth > 1) is NOT harness-reachable — it needs a nested EMUL_OP
# execute. Covered at unit level (test_exc_chain U2) and by live telemetry
# (W2-2 P4). Recorded here, not hidden.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UNIX_DIR="$(cd "$SCRIPT_DIR/../src/Unix" && pwd)"
BIN="$UNIX_DIR/SheepShaver"
RUN_DIR="/tmp/ss-exc-test-$$"
mkdir -p "$RUN_DIR"
trap 'rm -rf "$RUN_DIR"' EXIT

if [ ! -x "$BIN" ]; then
    echo "ERROR: $BIN not built — run 'make build-ss' first" >&2
    echo "METRIC pass=0 fail=0 total=0 score=0"
    exit 1
fi

# Hard per-run timeout (a broken delivery turns the b-self vectors into spins).
if command -v timeout >/dev/null 2>&1; then
    ss_timeout() { timeout -k 5s 15s "$@"; }
elif command -v gtimeout >/dev/null 2>&1; then
    ss_timeout() { gtimeout -k 5s 15s "$@"; }
else
    ss_timeout() {
        perl -e '
            my $kill = 15; my $hard = 5;
            my $pid = fork();
            if ($pid == 0) { exec @ARGV or die "exec: $!"; }
            my $done = 0;
            local $SIG{ALRM} = sub {
                if (!$done) { kill "TERM", $pid; alarm $hard; $done = 1; }
                else { kill "KILL", $pid; }
            };
            alarm $kill;
            waitpid($pid, 0);
            alarm 0;
            exit(($? & 127) ? 128 + ($? & 127) : $? >> 8);
        ' -- "$@"
    }
fi

# Common env for every vector. SS_EXC_BARE=1: skip the KDP register-save shim —
# the harness maps no KDP/ECB guest memory (the shim would abort on ECB=0); the
# BARE transition is the architectural surface under test here.
BASE_ENV=(SS_MACHINE=newworld SS_EXC_BARE=1 SS_TEST_DUMP=1)

# run_one <name> <mode:interp|jit> <hex> <env...>
#   stderr+stdout -> $RUN_DIR/<name>.<mode>.log
#   REGDUMP line  -> $RUN_DIR/<name>.<mode>.regdump
#   returns the emulator exit code
run_one() {
    local name="$1" mode="$2" hex="$3"; shift 3
    local jit_env=""
    [ "$mode" = "jit" ] && jit_env="1"
    ss_timeout env "${BASE_ENV[@]}" "$@" \
        SS_TEST_HEX="$hex" \
        SS_TEST_JIT="$jit_env" \
        "$BIN" > "$RUN_DIR/$name.$mode.log" 2>&1
    local rc=$?
    grep "^REGDUMP:" "$RUN_DIR/$name.$mode.log" > "$RUN_DIR/$name.$mode.regdump" || true
    return $rc
}

PASS=0
FAIL=0
TOTAL=0

report() { # <name> <ok:0/1> <detail>
    TOTAL=$((TOTAL + 1))
    if [ "$2" = 1 ]; then
        PASS=$((PASS + 1)); echo "EXC-VECTOR $1 PASS $3"
    else
        FAIL=$((FAIL + 1)); echo "EXC-VECTOR $1 FAIL $3"
        sed -e 's/^/    | /' "$RUN_DIR/$1.interp.log" 2>/dev/null | tail -6
    fi
}

# check_vector <name> <hex> <expected-tokens...> — runs both modes, diffs the
# REGDUMPs, asserts each expected token appears in the interp REGDUMP.
# Per-vector env rides the EXC_ENV array (set by the caller).
EXC_ENV=()
check_vector() {
    local name="$1" hex="$2"; shift 2
    run_one "$name" interp "$hex" "${EXC_ENV[@]}"
    run_one "$name" jit    "$hex" "${EXC_ENV[@]}"
    if [ ! -s "$RUN_DIR/$name.interp.regdump" ]; then
        report "$name" 0 "(no interp REGDUMP)"; return
    fi
    if [ ! -s "$RUN_DIR/$name.jit.regdump" ]; then
        report "$name" 0 "(no JIT REGDUMP)"; return
    fi
    if ! diff -q "$RUN_DIR/$name.interp.regdump" "$RUN_DIR/$name.jit.regdump" >/dev/null; then
        report "$name" 0 "(interp vs JIT REGDUMP differ — codegen-bug class, see plan stop-rule)"
        return
    fi
    local tok
    for tok in "$@"; do
        if ! grep -q " $tok" "$RUN_DIR/$name.interp.regdump"; then
            report "$name" 0 "(missing expected $tok)"; return
        fi
    done
    report "$name" 1 "(interp=jit; $# fields pinned)"
}

# The shared delivery env: latch armed, EE off at entry (0x7072), entry table
# -> capture stub, sc entry explicitly unresolved (",0").
DELIVERY_ENV=(SS_TEST_DEC_PENDING=1 SS_TEST_MSR=0x00007072
              SS_EXC_ENTRY=0x1000C000,0 SS_TEST_EXC_STUB=1)

# ---- H1: mtmsr EE 0->1 edge => real DEC delivery -----------------------------
# lis r3,0; ori r3,r3,0xf072; mtmsr r3; b .
# Delivery parks at the b-self: SRR0(r21)=0x1000400C (the plan's pinned value),
# SRR1(r22)=0xf072 (pre-exception MSR), handler MSR(r20)=0x1040 (PEM clear mask).
EXC_ENV=("${DELIVERY_ENV[@]}")
check_vector H1_mtmsr_edge "3C600000 6063F072 7C600124 48000000" \
    GPR3=0000f072 GPR20=00001040 GPR21=1000400c GPR22=0000f072 LR=10008000

# ---- H2: no-edge control (anti-vacuous) --------------------------------------
# Same shape, mtmsr 0x7072 (EE stays 0): no trigger, stub never runs,
# r20/r21/r22 stay 0. Straight-line tail (a b-self would spin undelivered).
EXC_ENV=("${DELIVERY_ENV[@]}")
check_vector H2_no_edge_control "3C600000 60637072 7C600124" \
    GPR3=00007072 GPR20=00000000 GPR21=00000000 GPR22=00000000

# ---- H3: rfi EE 0->1 edge => delivery at the rfi target ----------------------
# srr0 := 0x1000401C (the b-self), srr1 := 0xf072, rfi from MSR 0x7072.
# rfi restores EE=1 with the latch pending => delivery at the rfi target:
# r21=0x1000401C, r22=0xf072, r20=0x1040.
EXC_ENV=("${DELIVERY_ENV[@]}")
check_vector H3_rfi_edge "3C801000 6084401C 7C9A03A6 3CA00000 60A5F072 7CBB03A6 4C000064 48000000" \
    GPR4=1000401c GPR5=0000f072 GPR20=00001040 GPR21=1000401c GPR22=0000f072

# ---- H6 (W2-3): EXT delivery via the mtmsr edge -------------------------------
# The level-held EXT source (SS_TEST_EXT_PENDING — the PIC-output flag seam)
# with NO DEC pending; same H1 geometry. ENTRY DISCRIMINATION is the gating
# trick: interrupt_entry=0 (an EXT delivery wrongly routed to the shared
# entry FATALs unresolved => no REGDUMP => FAIL), external_entry=stub.
# r22=0xf072 also pins SRR1.EE=1 (the NK EXT body's punch-through guard).
EXC_ENV=(SS_TEST_EXT_PENDING=1 SS_TEST_MSR=0x00007072
         SS_EXC_ENTRY=0,0,0x1000C000 SS_TEST_EXC_STUB=1 SS_TEST_EXC_STATS=1)
check_vector H6_ext_mtmsr_edge "3C600000 6063F072 7C600124 48000000" \
    GPR20=00001040 GPR21=1000400c GPR22=0000f072 LR=10008000
if grep -q "^EXCSTAT: delivered_dec=0 .* delivered_ext=1$" \
        "$RUN_DIR/H6_ext_mtmsr_edge.interp.log" 2>/dev/null; then
    report H6_ext_stats 1 "(delivered_ext=1, delivered_dec=0 — EXT branch + external_entry consumed)"
else
    report H6_ext_stats 0 "(EXCSTAT delivered_ext=1 not observed)"
fi

# ---- H7 (W2-3): dual-pending priority — DEC before EXT (U13 live analogue) ----
# DEC latch armed AND EXT level held; one edge => exactly ONE delivery, and it
# must be the DEC (one-shot-vs-level-held justification at the hook). The
# EXCSTAT tuple is the discriminator (the REGDUMP alone cannot tell the source
# when both entries resolve to the same stub).
EXC_ENV=(SS_TEST_DEC_PENDING=1 SS_TEST_EXT_PENDING=1 SS_TEST_MSR=0x00007072
         SS_EXC_ENTRY=0x1000C000,0,0x1000C000 SS_TEST_EXC_STUB=1 SS_TEST_EXC_STATS=1)
check_vector H7_dual_pending_dec_first "3C600000 6063F072 7C600124 48000000" \
    GPR20=00001040 GPR21=1000400c GPR22=0000f072
if grep -q "^EXCSTAT: delivered_dec=1 .* delivered_ext=0$" \
        "$RUN_DIR/H7_dual_pending_dec_first.interp.log" 2>/dev/null; then
    report H7_dual_stats 1 "(delivered_dec=1, delivered_ext=0 — DEC-before-EXT order held)"
else
    report H7_dual_stats 0 "(EXCSTAT delivered_dec=1/delivered_ext=0 not observed)"
fi

# ---- H5: sc-class regression (rev 2 F12: process-isolated) -------------------
# H5r resolved-to-stub: sc delivers; SRR0 ownership = sc+4 (r21=0x10004004).
EXC_ENV=(SS_EXC_ENTRY=0x1000C000,0x1000C000 SS_TEST_EXC_STUB=1)
check_vector H5_sc_resolved "44000002" \
    GPR20=00001040 GPR21=10004004 GPR22=0000f072

# H5u unresolved: the observable is the FATAL capture + nonzero exit code
# (SIGABRT), per mode. Never batched, never REGDUMP-based.
h5u_one() { # <mode>
    local mode="$1" rc
    run_one H5_sc_unresolved "$mode" "44000002" \
        SS_EXC_ENTRY=0x1000C000,0 SS_TEST_EXC_STUB=1
    rc=$?
    [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] && [ "$rc" -ne 137 ] &&
        grep -q "\[EXC\] FATAL: sc at pc=10004000 with unresolved syscall entry" \
            "$RUN_DIR/H5_sc_unresolved.$mode.log" &&
        grep -q "\[EXC\] FATAL: sc capture:" "$RUN_DIR/H5_sc_unresolved.$mode.log" &&
        ! grep -q "^REGDUMP:" "$RUN_DIR/H5_sc_unresolved.$mode.log"
}
if h5u_one interp && h5u_one jit; then
    report H5_sc_unresolved 1 "(FATAL+capture+abort in both modes, no REGDUMP)"
else
    report H5_sc_unresolved 0 "(expected FATAL+nonzero exit in both modes)"
fi

# ---- H4: deferral telemetry (NON-GATING per the plan) ------------------------
# EE raised then dropped before the poll: the hook runs with EE=0 =>
# EXC_DECIDE_DEFER_EE => deferred_ee=1 in the EXCSTAT 6-tuple; stub untouched.
EXC_ENV=("${DELIVERY_ENV[@]}" SS_TEST_EXC_STATS=1)
run_one H4_deferral_telemetry interp \
    "3C600000 6063F072 3C800000 60847072 7C600124 7C800124 48000004" \
    "${EXC_ENV[@]}"
if grep -q "^EXCSTAT: delivered_dec=0 deferred_ee=1 " \
        "$RUN_DIR/H4_deferral_telemetry.interp.log" &&
   grep -q " GPR20=00000000" "$RUN_DIR/H4_deferral_telemetry.interp.regdump"; then
    echo "EXC-VECTOR H4_deferral_telemetry PASS (non-gating: deferred_ee=1 observed)"
else
    echo "EXC-VECTOR H4_deferral_telemetry WARN (non-gating: telemetry not observed)"
fi

echo "NOTE: depth-deferral (execute_depth>1) is NOT harness-reachable — unit U2 + live W2-2 P4 cover it."

SCORE=0
[ "$TOTAL" -gt 0 ] && SCORE=$((PASS * 100 / TOTAL))
echo "METRIC pass=$PASS fail=$FAIL total=$TOTAL score=$SCORE"
[ "$FAIL" -eq 0 ]
