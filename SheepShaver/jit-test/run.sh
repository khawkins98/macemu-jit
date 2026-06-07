#!/bin/bash
# SheepShaver PPC opcode equivalence test harness
# Phase 1: interpreter determinism validation
# Phase 2+: interpreter vs JIT comparison
#
# AUTHORITATIVE VECTOR COUNT: this file (the TEST_ORDER entries) IS the source of
# truth for how many vectors exist. Get the live number with `make harness-count`
# (from SheepShaver/). Docs cite dated snapshots that drift — do not trust them
# over this file or the target.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UNIX_DIR="$(cd "$SCRIPT_DIR/../src/Unix" && pwd)"
RUN_DIR="/tmp/ss-jit-test-$$"
mkdir -p "$RUN_DIR"

# ---- Build -------------------------------------------------------------------
cd "$UNIX_DIR"

if [ ! -x ./configure ] && [ -x ./autogen.sh ]; then
    NO_CONFIGURE=1 ./autogen.sh >"$RUN_DIR/autogen.log" 2>&1 || true
fi

if [ ! -f config.h ] || [ ! -f Makefile ]; then
    if [ ! -x ./configure ]; then
        echo "METRIC build_ok=0"
        echo "METRIC pass=0"
        echo "METRIC fail=0"
        echo "METRIC total=0"
        echo "METRIC score=0"
        rm -rf "$RUN_DIR"
        exit 0
    fi
    if ! ./configure --enable-sdl-video --enable-sdl-audio \
      >"$RUN_DIR/configure.log" 2>&1; then
        echo "METRIC build_ok=0"
        echo "METRIC pass=0"
        echo "METRIC fail=0"
        echo "METRIC total=0"
        echo "METRIC score=0"
        rm -rf "$RUN_DIR"
        exit 0
    fi
fi

BIN="$UNIX_DIR/SheepShaver"

# Use a prebuilt binary if one already exists (e.g. the macOS/clang build,
# whose link line differs from the Linux/GTK one below). The Linux rebuild
# path uses g++ and X11/GTK link flags that do not exist on macOS, so only
# attempt it when there is no usable binary already in place.
if [ -x "$BIN" ]; then
    echo "METRIC build_ok=1"
else
    # Build base objects (make will fail on link due to missing JIT, that's ok)
    make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >"$RUN_DIR/build.log" 2>&1 || true
    # Compile JIT-specific objects with USE_AARCH64_JIT
    JITDIR="../kpx_cpu/src/cpu/jit/aarch64"
    g++ -c -o obj/ppc-jit.o "$JITDIR/ppc-jit.cpp" -I"$JITDIR" -DHAVE_CONFIG_H -g -O2 -std=c++11 >>"$RUN_DIR/build.log" 2>&1
    g++ -c -o obj/ppc-cpu.o ../kpx_cpu/src/cpu/ppc/ppc-cpu.cpp -I../include -I. -I.. -I../CrossPlatform -I../kpx_cpu/include -I../kpx_cpu/src -DHAVE_CONFIG_H -DUSE_AARCH64_JIT -g -O2 >>"$RUN_DIR/build.log" 2>&1
    g++ -c -o obj/sheepshaver_glue.o ../kpx_cpu/sheepshaver_glue.cpp -I../include -I. -I.. -I../CrossPlatform -I../kpx_cpu/include -I../kpx_cpu/src -DHAVE_CONFIG_H -DUSE_AARCH64_JIT -g -O2 >>"$RUN_DIR/build.log" 2>&1
    # Final link
    if ! g++ -o SheepShaver obj/*.o -lpthread -lm -lSDL2 -lgtk-x11-2.0 -lgdk-x11-2.0 -lpangocairo-1.0 -latk-1.0 -lcairo -lgdk_pixbuf-2.0 -lgio-2.0 -lpangoft2-1.0 -lpango-1.0 -lgobject-2.0 -lglib-2.0 -lharfbuzz -lvncserver -lfontconfig -lfreetype >>"$RUN_DIR/build.log" 2>&1; then
        echo "METRIC build_ok=0"
        echo "METRIC pass=0"
        echo "METRIC fail=0"
        echo "METRIC total=0"
        echo "METRIC score=0"
        tail -20 "$RUN_DIR/build.log" >&2
        rm -rf "$RUN_DIR"
        exit 0
    fi
    echo "METRIC build_ok=1"
fi

if [ ! -x "$BIN" ]; then
    echo "METRIC build_ok=0"
    echo "METRIC pass=0 fail=0 total=0 score=0"
    rm -rf "$RUN_DIR"
    exit 0
fi

# ---- Headless display --------------------------------------------------------
# Xvfb is an X11 server (Linux). It does not exist on macOS; the binary runs
# with "nogui true" so no display is required. Only launch Xvfb where available.
if command -v Xvfb >/dev/null 2>&1; then
    if ! pgrep -x Xvfb >/dev/null 2>&1; then
        Xvfb :99 -screen 0 640x480x24 &>/dev/null &
        sleep 1
    fi
fi

# ---- Portable timeout --------------------------------------------------------
# GNU coreutils `timeout` is not present on macOS by default. Prefer gtimeout
# (Homebrew coreutils), else fall back to a perl-based SIGTERM-then-SIGKILL
# wrapper that mimics `timeout -k 5s 15s`.
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
        ' -- "$@"
    }
fi

# ---- Harness mode ------------------------------------------------------------
# Default mode ("interp") preserves upstream's Linux behavior exactly: each
# vector runs twice through the interpreter and the two REGDUMPs are compared
# for determinism.
#
# SS_HARNESS_MODE=jit changes the equivalence test: each vector is run once in
# interpreter mode and once in JIT mode (SS_TEST_JIT=1), and the JIT REGDUMP is
# compared against the interpreter REGDUMP. This is the real correctness gate
# for the aarch64 JIT codegen. It does not affect the default Linux usage.
SS_HARNESS_MODE="${SS_HARNESS_MODE:-interp}"

# ---- Test runner -------------------------------------------------------------
# run_ppc_test <name> <hex> <outfile> [jit]
#   The optional 4th argument, when "jit", sets SS_TEST_JIT=1 so the run drives
#   the aarch64 JIT instead of the interpreter.
run_ppc_test() {
    local name="$1"
    local hex="$2"   # space-separated 32-bit PPC hex words
    local outfile="$3"
    local want_jit="${4:-}"

    local td="$RUN_DIR/test-${name}"
    mkdir -p "$td"

    # Minimal prefs — no ROM needed for test mode (SS_TEST_HEX bypasses boot)
    cat > "$td/prefs" <<EOF
nogui true
nosound true
nocdrom true
noclipconversion true
ramsize 16777216
EOF

    # Kill any stale SheepShaver processes
    pkill -f "SheepShaver --config $td/prefs" 2>/dev/null || true

    local jit_env=""
    if [ "$want_jit" = "jit" ]; then
        jit_env="1"
    fi

    # Run with test mode env vars. SS_TEST_JIT is only set for JIT runs; the
    # interpreter run leaves it unset so the glue pins SS_USE_JIT=0 itself.
    SDL_VIDEODRIVER=x11 DISPLAY=:99 HOME="$td" \
      SS_TEST_HEX="$hex" \
      SS_TEST_DUMP=1 \
      SS_TEST_JIT="$jit_env" \
      ss_timeout "$BIN" --config "$td/prefs" \
      > "$td/emu.log" 2>&1 || true

    # Extract REGDUMP line
    grep "^REGDUMP:" "$td/emu.log" > "$outfile" 2>/dev/null || true
}

# ---- Test vectors ------------------------------------------------------------
# PPC instruction encodings (big-endian 32-bit words, space-separated)
# Each vector ends implicitly with blr (appended by the harness in C)

# Test vectors are stored as plain shell variables named T_<name> (set below)
# and looked up via indirect expansion, so this harness runs on the stock
# macOS bash 3.2 (which lacks associative arrays / `declare -A`).
TEST_ORDER=()

# --- Integer ALU ---
# li r3,100; li r4,200; add r5,r3,r4
T_alu_add="38600064 388000c8 7CA32214"
TEST_ORDER+=(alu_add)

# li r3,50; li r4,30; subf r5,r4,r3  (r5 = r3 - r4 = 20)
T_alu_sub="38600032 3880001e 7CA42050"
TEST_ORDER+=(alu_sub)

# li r3,0xFF; li r4,0x0F; and r5,r3,r4
T_alu_and="386000ff 3880000f 7C651838"
TEST_ORDER+=(alu_and)

# li r3,0xA0; li r4,0x05; or r5,r3,r4
T_alu_or="386000a0 38800005 7C651B78"
TEST_ORDER+=(alu_or)

# li r3,0xFF; li r4,0x0F; xor r5,r3,r4
T_alu_xor="386000ff 3880000f 7C651A78"
TEST_ORDER+=(alu_xor)

# --- Load immediate ---
# lis r3,0x1234; ori r3,r3,0x5678  → r3 = 0x12345678
T_li_wide="3C601234 60635678"
TEST_ORDER+=(li_wide)

# --- Shift ---
# li r3,1; li r4,4; slw r5,r3,r4  → r5 = 16
T_shift_slw="38600001 38800004 7C652030"
TEST_ORDER+=(shift_slw)

# li r3,256; li r4,4; srw r5,r3,r4  → r5 = 16
T_shift_srw="38600100 38800004 7C652430"
TEST_ORDER+=(shift_srw)

# --- Compare + branch ---
# li r3,10; li r4,10; cmpw cr0,r3,r4; beq +8; li r5,1; b +8; li r5,2; nop
T_cmp_beq="3860000a 3880000a 7C032000 41820008 38a00001 48000008 38a00002 60000000"
TEST_ORDER+=(cmp_beq)

# --- Counter loop (bdnz) ---
# li r3,0; li r4,5; mtctr r4; addi r3,r3,1; bdnz -4
T_bdnz_loop="38600000 38800005 7C8903A6 38630001 4200FFFC"
TEST_ORDER+=(bdnz_loop)

# --- Multiply ---
# li r3,7; li r4,6; mullw r5,r3,r4  → r5 = 42
T_mul_basic="38600007 38800006 7CA321D6"
TEST_ORDER+=(mul_basic)

# --- Rotate/mask ---
# li r3,0xFF; rlwinm r4,r3,4,0,27
T_rlwinm_basic="386000ff 546421b6"
TEST_ORDER+=(rlwinm_basic)

# --- NOP (sanity) ---
T_nop="60000000"
TEST_ORDER+=(nop)


# --- Negate ---
# li r3,42; neg r5,r3  → r5 = 0xFFFFFFD6
T_neg_basic="3860002a 7CA300D0"
TEST_ORDER+=(neg_basic)

# --- Arithmetic shift ---
# li r3,-1; li r4,16; sraw r5,r3,r4  → r5 = 0xFFFFFFFF, XER.CA=1
T_sraw_signext="3860ffff 38800010 7C652630"
TEST_ORDER+=(sraw_signext)

# --- Store/load round-trip ---
# li r3,0xBEEF; stw r3,0x100(r1); li r3,0; lwz r5,0x100(r1)
T_stw_lwz="3860beef 90610100 38600000 80A10100"
TEST_ORDER+=(stw_lwz)

# --- Byte store/load ---
# li r3,0x42; stb r3,0x200(r1); li r3,0; lbz r5,0x200(r1)
# stb rS,d(rA) = 0x98000000 | ... ; lbz rD,d(rA) = 0x88000000 | ...
T_stb_lbz="38600042 98610200 38600000 88A10200"
TEST_ORDER+=(stb_lbz)

# --- Halfword store/load ---
# li r3,0x1234; sth r3,0x300(r1); li r3,0; lhz r5,0x300(r1)
# sth = 0xB0000000; lhz = 0xA0000000
T_sth_lhz="38601234 B0610300 38600000 A0A10300"
TEST_ORDER+=(sth_lhz)

# --- Record form (sets CR0) ---
# li r3,42; addic. r5,r3,0  → CR0 should have GT bit set (positive result)
# addic. rD,rA,SIMM = 0x34000000 | (rD<<21) | (rA<<16) | SIMM
T_addic_dot="3860002a 34A30000"
TEST_ORDER+=(addic_dot)

# --- CR record with negative ---
# li r3,-1; add. r5,r3,r3  → r5 = -2, CR0.LT set
# add. rD,rA,rB = 0x7C000215 | (rD<<21) | (rA<<16) | (rB<<11)
T_add_dot_neg="3860ffff 7CA31A15"
TEST_ORDER+=(add_dot_neg)

# --- Divide ---
# li r3,100; li r4,7; divw r5,r3,r4  → r5 = 14
# divw rD,rA,rB = 0x7C0003D6 | (rD<<21) | (rA<<16) | (rB<<11)
T_divw_basic="38600064 38800007 7CA323D6"
TEST_ORDER+=(divw_basic)

# --- Counter branch (bctrl pattern) ---
# li r3,0; lis r4,hi(target); ori r4,r4,lo(target); mtctr r4; bctrl
# Can't easily encode absolute target, so just test mtctr+mfctr round-trip
# li r3,0xDEAD; mtctr r3; li r3,0; mfctr r5
# mtctr r3 = mtspr 9,r3 = 0x7C6903A6; mfctr r5 = mfspr 9,r5 = 0x7CA902A6
T_mtctr_mfctr="3860dead 7C6903A6 38600000 7CA902A6"
TEST_ORDER+=(mtctr_mfctr)

# --- XER carry flag ---
# Test addic (add immediate carrying): li r3,-1; addic r5,r3,2 → r5=1, XER.CA=1
# addic rD,rA,SIMM = 0x30000000 | (rD<<21) | (rA<<16) | (SIMM&0xFFFF)
T_addic_carry="3860ffff 30A30002"
TEST_ORDER+=(addic_carry)

# --- Extended ops ---
# adde (add extended with carry): li r3,5; li r4,3; addic r5,r3,-1 (set CA); adde r6,r4,r3
# adde rD,rA,rB = 0x7C000114 | (rD<<21) | (rA<<16) | (rB<<11)
T_adde_carry="38600005 38800003 30A3ffff 7CC42114"
TEST_ORDER+=(adde_carry)

# --- rlwimi (rotate left word immediate then mask insert) ---
# li r3,0xFF00; li r5,0x00FF; rlwimi r5,r3,0,24,31  → insert low byte of r3 into r5
# rlwimi rA,rS,SH,MB,ME = 0x50000000 | (rS<<21) | (rA<<16) | (SH<<11) | (MB<<6) | (ME<<1)
# rlwimi r5,r3,0,24,31 = 0x5065043E
T_rlwimi_insert="3860ff00 38a000ff 5065043E"
TEST_ORDER+=(rlwimi_insert)

# --- cntlzw (count leading zeros) ---
# li r3,0x100; cntlzw r5,r3  → r5 = 23 (0x100 = bit 8, 31-8=23)
# cntlzw rA,rS = 0x7C000034 | (rS<<21) | (rA<<16)
T_cntlzw_basic="38600100 7C650034"
TEST_ORDER+=(cntlzw_basic)

# --- extsh (extend sign halfword) ---
# li r3,0x8000; extsh r5,r3  → r5 = 0xFFFF8000
# extsh rA,rS = 0x7C000734 | (rS<<21) | (rA<<16)
T_extsh_basic="38608000 7C650734"
TEST_ORDER+=(extsh_basic)

# --- extsb (extend sign byte) ---
# li r3,0x80; extsb r5,r3  → r5 = 0xFFFFFF80
# extsb rA,rS = 0x7C000774 | (rS<<21) | (rA<<16)
T_extsb_basic="38600080 7C650774"
TEST_ORDER+=(extsb_basic)


# --- Carry/overflow ALU ---
# addc r5,r3,r4: li r3,-1; li r4,2; addc r5,r3,r4 → r5=1, XER.CA=1
# addc = 0x7C000014 | (5<<21)|(3<<16)|(4<<11)
T_addc_basic="3860ffff 38800002 7CA32014"
TEST_ORDER+=(addc_basic)

# subfc r5,r4,r3: li r3,10; li r4,3; subfc r5,r4,r3 → r5=7
# subfc = 0x7C000010 | (5<<21)|(4<<16)|(3<<11)
T_subfc_basic="3860000a 38800003 7CA41810"
TEST_ORDER+=(subfc_basic)

# --- OE=1 (overflow-enabled) arithmetic ---
# These drive XER.OV + sticky XER.SO in addition to base semantics.
# The Mac ROM's built-in 68k emulator uses addco/subfco heavily.

# addco r5,r3,r4 = addc | OE(0x400): li r3,-1; li r4,2 → r5=1, CA=1, OV=0
T_addco_basic="3860ffff 38800002 7CA32414"
TEST_ORDER+=(addco_basic)

# addco overflow: r3=INT_MAX (lis+ori), r4=1 → r5=0x80000000, CA=0, OV=1, SO=1
T_addco_overflow="3C607FFF 6063FFFF 38800001 7CA32414"
TEST_ORDER+=(addco_overflow)

# addco. (Rc=1) overflow: CR0 should be LT|SO (result negative, SO set)
T_addco_rc_overflow="3C607FFF 6063FFFF 38800001 7CA32415"
TEST_ORDER+=(addco_rc_overflow)

# subfco r5,r4,r3 = subfc | OE: li r3,10; li r4,3 → r5=7, CA=1, OV=0
T_subfco_basic="3860000a 38800003 7CA41C10"
TEST_ORDER+=(subfco_basic)

# subfco overflow: r4=INT_MIN (lis), r3=1 → r5 = 1-INT_MIN overflows, OV=1, SO=1
T_subfco_overflow="3C808000 38600001 7CA41C10"
TEST_ORDER+=(subfco_overflow)

# addo r5,r3,r4 = add | OE: INT_MAX + 1 → OV=1, SO=1, CA untouched (0)
T_addo_overflow="3C607FFF 6063FFFF 38800001 7CA32614"
TEST_ORDER+=(addo_overflow)

# subfo r5,r4,r3 = subf | OE: li r3,10; li r4,3 → r5=7, OV=0
T_subfo_basic="3860000a 38800003 7CA41C50"
TEST_ORDER+=(subfo_basic)

# nego r5,r3 = neg | OE: li r3,5 → r5=-5, OV=0
T_nego_basic="38600005 7CA304D0"
TEST_ORDER+=(nego_basic)

# nego overflow: r3=INT_MIN → r5=INT_MIN (unchanged), OV=1, SO=1
T_nego_overflow="3C808000 7CA304D0"
TEST_ORDER+=(nego_overflow)

# --- CR logical operations (crand/cror/crxor/crnor/crandc/creqv/crorc/crnand + mcrf) ---
# These had ZERO coverage when crorc shipped broken (missing AND #1 after ORN turned
# CR into 0xffffffff — the root cause of the deterministic 68k-region boot crash,
# see LEARNINGS.md 2026-06-02). Setup for all: li r3,5; li r4,5; cmpw cr1,r3,r4
# → CR bit 4 (CR1.LT) = 0, CR bit 6 (CR1.EQ) = 1. Result read back via mfcr r6.

# crorc all four input combos: crorc 0,6,6 (1|~1=1); crorc 1,6,4 (1|~0=1);
# crorc 2,4,6 (0|~1=0); crorc 3,4,4 (0|~0=1) → CR0 = 1101 = 0xD
T_crorc_all_combos="38600005 38800005 7C832000 4C063342 4C262342 4C443342 4C642342 7CC00026"
TEST_ORDER+=(crorc_all_combos)

# crand 0,6,6 → 1&1=1 (bit 0 set)
T_crand_basic="38600005 38800005 7C832000 4C063202 7CC00026"
TEST_ORDER+=(crand_basic)

# cror 1,4,4 → 0|0=0 (bit 1 clear)
T_cror_basic="38600005 38800005 7C832000 4C242382 7CC00026"
TEST_ORDER+=(cror_basic)

# crxor 2,6,4 → 1^0=1 (bit 2 set)
T_crxor_basic="38600005 38800005 7C832000 4C462182 7CC00026"
TEST_ORDER+=(crxor_basic)

# crnor 3,6,4 → ~(1|0)=0 (bit 3 clear)
T_crnor_basic="38600005 38800005 7C832000 4C662042 7CC00026"
TEST_ORDER+=(crnor_basic)

# crandc 7,6,4 → 1&~0=1 (bit 7 set)
T_crandc_basic="38600005 38800005 7C832000 4CE62102 7CC00026"
TEST_ORDER+=(crandc_basic)

# creqv 8,6,6 → ~(1^1)=1 (bit 8 set)
T_creqv_basic="38600005 38800005 7C832000 4D063242 7CC00026"
TEST_ORDER+=(creqv_basic)

# crnand 9,6,6 → ~(1&1)=0 (bit 9 clear)
T_crnand_basic="38600005 38800005 7C832000 4D2631C2 7CC00026"
TEST_ORDER+=(crnand_basic)

# mcrf 5,1: copy CR field 1 (=0010 from equal compare) into CR field 5
T_mcrf_basic="38600005 38800005 7C832000 4E840000 7CC00026"
TEST_ORDER+=(mcrf_basic)

# subfic r5,r3,100: li r3,30; subfic r5,r3,100 → r5=70
# subfic = 0x20000000 | (5<<21)|(3<<16)|100
T_subfic_basic="3860001e 20A30064"
TEST_ORDER+=(subfic_basic)

# --- Logical ops ---
# andc r5,r3,r4: li r3,0xFF; li r4,0x0F; andc r5,r3,r4 → r5=0xF0
# andc = 0x7C000078 | (3<<21)|(5<<16)|(4<<11)
T_andc_basic="386000ff 3880000f 7C652078"
TEST_ORDER+=(andc_basic)

# nor r5,r3,r3: li r3,0; nor r5,r3,r3 → r5=0xFFFFFFFF
# nor = 0x7C0000F8 | (3<<21)|(5<<16)|(3<<11)
T_nor_basic="38600000 7C6518F8"
TEST_ORDER+=(nor_basic)

# nand r5,r3,r4: li r3,-1; li r4,0xFF; nand r5,r3,r4 → r5=0xFFFFFF00
# nand = 0x7C0003B8 | (3<<21)|(5<<16)|(4<<11)
T_nand_basic="3860ffff 388000ff 7C6523B8"
TEST_ORDER+=(nand_basic)

# eqv r5,r3,r4: li r3,0xFF; li r4,0xFF; eqv r5,r3,r4 → r5=0xFFFFFFFF (XNOR)
# eqv = 0x7C000238 | (3<<21)|(5<<16)|(4<<11)
T_eqv_basic="386000ff 388000ff 7C652238"
TEST_ORDER+=(eqv_basic)

# orc r5,r3,r4: li r3,0; li r4,0xFF; orc r5,r3,r4 → r5=0xFFFFFF00
# orc = 0x7C000338 | (3<<21)|(5<<16)|(4<<11)
T_orc_basic="38600000 388000ff 7C652338"
TEST_ORDER+=(orc_basic)

# --- Multiply/divide ---
# divwu r5,r3,r4: li r3,100; li r4,7; divwu r5,r3,r4 → r5=14
# divwu = 0x7C000396 | (5<<21)|(3<<16)|(4<<11)
T_divwu_basic="38600064 38800007 7CA32396"
TEST_ORDER+=(divwu_basic)

# mulhw r5,r3,r4: lis r3,0x1000; lis r4,0x1000; mulhw r5,r3,r4 → r5=0x01000000
# mulhw = 0x7C000096 | (5<<21)|(3<<16)|(4<<11)
T_mulhw_basic="3C601000 3C801000 7CA32096"
TEST_ORDER+=(mulhw_basic)

# --- Rotate ---
# rlwnm r5,r3,r4,0,31: li r3,1; li r4,8; rlwnm r5,r3,r4,0,31 → r5=256
# rlwnm = 0x5C000000 | (3<<21)|(5<<16)|(4<<11)|(0<<6)|(31<<1)
T_rlwnm_basic="38600001 38800008 5C65203E"
TEST_ORDER+=(rlwnm_basic)

# --- CR logical ---
# cmpwi cr0,r3,0; cmpwi cr1,r4,0; crand 0,0,4 (AND cr0.lt with cr1.lt)
# cmpwi cr0,r3,0 = 0x2C030000; cmpwi cr1,r4,0 = 0x2C840000
# crand 0,0,4 = 0x4C000202
T_crand_basic2="3860ffff 3880ffff 2C030000 2C840000 4C000202"
TEST_ORDER+=(crand_basic2)

# crxor 0,0,0 (clear CR bit 0) then cror 0,0,4 (OR)
# crxor 0,0,0 = 0x4C000182; cror 0,0,4 = 0x4C000382
T_crxor_cror="3860ffff 2C030000 4C000182 4C000382"
TEST_ORDER+=(crxor_cror)

# --- FP operations ---
# fadd: load 2.0 and 3.0 via lis/stw/lfs, add them
# This is complex in hex. Use simpler approach: stfd a known pattern.
# li r3,0x4000; stw r3,0x100(r1); li r3,0; stw r3,0x104(r1); lfd f1,0x100(r1)

# --- Load/store indexed ---
# lwzx: li r3,0xBEEF; stw r3,0(r1); li r4,0; lwzx r5,r1,r4
# lwzx = 0x7C00002E | (5<<21)|(1<<16)|(4<<11)
T_lwzx_basic="3860beef 90610000 38800000 7CA1202E"
TEST_ORDER+=(lwzx_basic)

# lbzx: li r3,0x42; stb r3,0x200(r1); li r4,0x200; lbzx r5,r1,r4
# lbzx = 0x7C0000AE | (5<<21)|(1<<16)|(4<<11)
T_lbzx_basic="38600042 98610200 38800200 7CA120AE"
TEST_ORDER+=(lbzx_basic)

# --- Record forms ---
# or. r5,r3,r3 with negative value (sets CR0.LT)
# or. = 0x7C000379 | (3<<21)|(5<<16)|(3<<11)
T_or_dot_neg="3860ffff 7C651B79"
TEST_ORDER+=(or_dot_neg)

# and. r5,r3,r4 with zero result (sets CR0.EQ)
# and. = 0x7C000039 | (3<<21)|(5<<16)|(4<<11)
T_and_dot_zero="38600ff0 3880000f 7C651839"
TEST_ORDER+=(and_dot_zero)

# --- mftb (time base) ---
# mftb r5 → should return non-zero
# mftb = 0x7C0002E6 | (5<<21) with TBR=268
T_mftb_basic="7CA602A6"
TEST_ORDER+=(mftb_basic)

# --- cntlzw edge cases ---
# cntlzw r5,r3: li r3,0; cntlzw r5,r3 → r5=32
T_cntlzw_zero="38600000 7C650034"
TEST_ORDER+=(cntlzw_zero)

# cntlzw r5,r3: li r3,-1; cntlzw r5,r3 → r5=0
T_cntlzw_allones="3860ffff 7C650034"
TEST_ORDER+=(cntlzw_allones)

# --- bdnz loop (longer) ---
# li r3,0; li r4,10; mtctr r4; addi r3,r3,1; bdnz -4
T_bdnz_10="38600000 3880000a 7C8903A6 38630001 4200FFFC"
TEST_ORDER+=(bdnz_10)

# --- compare unsigned ---
# cmplwi cr0,r3,100: li r3,200; cmplwi cr0,r3,100
# cmplwi = 0x28030064
T_cmplwi_basic="386000c8 28030064"
TEST_ORDER+=(cmplwi_basic)

# --- stwu/lwzu stack frame ---
# stwu r1,-16(r1); lwzu r3,16(r1)
T_stwu_lwzu="9421FFF0 84610010"
TEST_ORDER+=(stwu_lwzu)

# --- extsh/extsb edge ---
# li r3,0x7FFF; extsh r5,r3 → r5=0x7FFF (positive, no sign ext)
T_extsh_positive="38607fff 7C650734"
TEST_ORDER+=(extsh_positive)

# li r3,0x7F; extsb r5,r3 → r5=0x7F
T_extsb_positive="3860007f 7C650774"
TEST_ORDER+=(extsb_positive)


# --- FP operations ---
# fneg: store 2.0 as double, negate it, check sign


# --- Branch ---
# bl +8; nop; mfspr r5,LR → r5 should equal address of nop
# bl = 0x48000009 (LK=1, +8 bytes)... actually bl offset must be from current insn
# bl +8 = 0x48000009 (branch 8 bytes forward, link)
T_bl_basic="7CA802A6"
TEST_ORDER+=(bl_basic)

# --- srawi ---
# li r3,-128; srawi r5,r3,3 → r5 = -16 = 0xFFFFFFF0
# srawi r5,r3,3: XO=31 XO=824, rS=3 rA=5 SH=3 = 0x7C651E70
T_srawi_basic="3860ff80 7C651E70"
TEST_ORDER+=(srawi_basic)

# --- lha (sign-extending halfword) ---
# li r3,0x8000; sth r3,0x300(r1); lha r5,0x300(r1) → r5 = 0xFFFF8000
T_lha_signext="38608000 B0610300 A8A10300"
TEST_ORDER+=(lha_signext)

# --- lmw/stmw ---
# li r28,0x28; li r29,0x29; li r30,0x30; li r31,0x31; stmw r28,0x400(r1); 
# li r28,0; li r29,0; li r30,0; li r31,0; lmw r28,0x400(r1)
T_lmw_stmw="3B800028 3BA00029 3BC00030 3BE00031 BF810400 3B800000 3BA00000 3BC00000 3BE00000 BB810400"
TEST_ORDER+=(lmw_stmw)

# --- lmw/stmw wide (RA eviction stress) ---
# li r20..r31 with known values (0x14..0x1F); stmw r20,0x400(r1);
# zero r20..r31; lmw r20,0x400(r1). 12 GPRs > RA_NUM_REGS=8, forces
# mid-block eviction of both clean and dirty RA slots.
T_lmw_stmw_wide="3A800014 3AA00015 3AC00016 3AE00017 3B000018 3B200019 3B40001A 3B60001B 3B80001C 3BA0001D 3BC0001E 3BE0001F BE810400 3A800000 3AA00000 3AC00000 3AE00000 3B000000 3B200000 3B400000 3B600000 3B800000 3BA00000 3BC00000 3BE00000 BA810400"
TEST_ORDER+=(lmw_stmw_wide)

# --- RA eviction battery (pure-register, no lmw/stmw): broaden P1a beyond the
# single load/store-multiple case. Each block uses 16 distinct GPRs (r3..r18) >
# RA_NUM_REGS=8 in one straight-line block, forcing ra_evict. Pure li+ALU, so
# there is no memory/chaining/loop confound — a clean JIT-vs-interp REGDUMP diff.
# Encodings capstone-verified. See OPTIMIZATION-PLAN §P1a.
#
# evict_wb16: write r3..r18, then add each EARLY reg to a LATE reg
# (r3+r18, r4+r17, ... r10+r11 = 21 each). r3..r10 are dirty-evicted while
# r11..r18 fill the cache, then reloaded for the adds — exercises spill+reload of
# dirty slots; a bad spill slot / lost dirty bit / stale reload corrupts the sums.
T_evict_wb16="38600003 38800004 38A00005 38C00006 38E00007 39000008 39200009 3940000A 3960000B 3980000C 39A0000D 39C0000E 39E0000F 3A000010 3A200011 3A400012 7C639214 7C848A14 7CA58214 7CC67A14 7CE77214 7D086A14 7D296214 7D4A5A14"
TEST_ORDER+=(evict_wb16)

# evict_rd_eq_ra: rD==rA==rB doubling (add rN,rN,rN) on evicted regs — exercises
# the P1 bringup ordering rule (ra_load source must precede ra_store dest, else a
# read of the just-allocated dest returns uninitialised data) under eviction.
T_evict_rd_eq_ra="38600003 38800004 38A00005 38C00006 38E00007 39000008 39200009 3940000A 3960000B 3980000C 39A0000D 39C0000E 39E0000F 3A000010 3A200011 3A400012 7C631A14 7C842214 7CA52A14 7CC63214 7CE73A14 7D084214 7D294A14 7D4A5214"
TEST_ORDER+=(evict_rd_eq_ra)

# evict_mixed_alu: variety of ALU ops (add/subf/and/or/xor) with different XOs and
# dest-position encodings, spread across the wide live set so eviction churns under
# mixed opcodes rather than a single op shape.
T_evict_mixed_alu="38600003 38800004 38A00005 38C00006 38E00007 39000008 39200009 3940000A 3960000B 3980000C 39A0000D 39C0000E 39E0000F 3A000010 3A200011 3A400012 7C632214 7CA53050 7D074838 7D6A6378 7DCD7A78 7E119214"
TEST_ORDER+=(evict_mixed_alu)

# --- RA eviction at edge surfaces (P1a residual, 2026-06-05) ---
# Two surfaces the pure-register straight-line battery above does NOT cover:
# eviction at a control-flow exit, and eviction × deferred CR0/XER state.
#
# evict_branch_exit: 16-live pressure, r3..r10 dirty-evicted (=0x15), then a
# conditional branch TERMINATES the block — forcing ra_flush_all of the dirty
# slots on the bc edge (a different exit path than the blr terminator the others
# use). The branch condition reads r10 (a dirty/evicted reg), so a bad flush also
# steers the branch wrong. beq is taken (r10==0x15) → skips `li r3,0` → r3 stays
# 0x15; a flush/eviction bug diverges r3 (or the dirty regs in the REGDUMP).
T_evict_branch_exit="38600003 38800004 38A00005 38C00006 38E00007 39000008 39200009 3940000A 3960000B 3980000C 39A0000D 39C0000E 39E0000F 3A000010 3A200011 3A400012 7C639214 7C848A14 7CA58214 7CC67A14 7CE77214 7D086A14 7D296214 7D4A5A14 2C0A0015 41820008 38600000"
TEST_ORDER+=(evict_branch_exit)

# evict_rc1_cr0: same wide eviction but the combines are add. (Rc=1), so
# emit_update_cr0 runs on each op while the RA is evicting. REGDUMP captures CR —
# a bad CR0 input under pressure diverges CR (final CR0 reflects r10+r11=0x15>0).
T_evict_rc1_cr0="38600003 38800004 38A00005 38C00006 38E00007 39000008 39200009 3940000A 3960000B 3980000C 39A0000D 39C0000E 39E0000F 3A000010 3A200011 3A400012 7C639215 7C848A15 7CA58215 7CC67A15 7CE77215 7D086A15 7D296215 7D4A5A15"
TEST_ORDER+=(evict_rc1_cr0)

# evict_adde_carry: XER carry chain under pressure — addic. seeds CA (+CR0), then
# an adde chain propagates carry across evicted regs. REGDUMP captures XER + CR; a
# bad CA handling or operand under eviction diverges XER/GPRs.
T_evict_adde_carry="38600003 38800004 38A00005 38C00006 38E00007 39000008 39200009 3940000A 3960000B 3980000C 39A0000D 39C0000E 39E0000F 3A000010 3A200011 3A400012 3463FFFF 7C842914 7CC63914 7D084914 7D4A5914"
TEST_ORDER+=(evict_adde_carry)

# --- adde/subfe carry-wrap edge cases (backlog A1/A2) ---
# adde carry-out edge: CA=1 and rA+rB=0xFFFFFFFF → result 0, CA_out must be 1
# li r3,-1; addic r0,r3,1 (sets CA=1, r0=0); li r4,0; adde r5,r4,r3
T_adde_carry_wrap="3860FFFF 30030001 38800000 7CA41914"
TEST_ORDER+=(adde_carry_wrap)

# subfe carry-out edge: CA=1 and ~rA+rB=0xFFFFFFFF (rA==rB) → result 0, CA_out must be 1
# li r3,-1; addic r0,r3,1 (sets CA=1); lis r4,0x1234; ori r4,r4,0x5678; subfe r5,r4,r4
T_subfe_carry_wrap="3860FFFF 30030001 3C801234 60845678 7CA42110"
TEST_ORDER+=(subfe_carry_wrap)

# --- mcrf ---
# cmpwi cr0,r3,0 (r3=-1 → LT); mcrf cr1,cr0; then check cr1 has LT
# cmpwi cr0,r3,0 = 0x2C030000; mcrf cr1,cr0 = 0x4C840000
T_mcrf_basic2="3860ffff 2C030000 4C840000"
TEST_ORDER+=(mcrf_basic2)

# --- subfe (simplified) ---
# li r3,5; li r4,10; subfe r5,r3,r4 → r5 = r4 + ~r3 + CA ≈ 4 (simplified as subf)
# subfe = XO=31 XO=136: 0x7C000110 | (5<<21)|(3<<16)|(4<<11) = 0x7CA32110
T_subfe_basic="38600005 3880000a 7CA32110"
TEST_ORDER+=(subfe_basic)

# --- addze (simplified) ---
# li r3,42; addze r5,r3 → r5 = 42 (simplified: ignores CA)
# addze = XO=31 XO=202: 0x7C000194 | (5<<21)|(3<<16) = 0x7CA30194
T_addze_basic="3860002a 7CA30194"
TEST_ORDER+=(addze_basic)

# --- dcbz ---
# stw r3,0x500(r1); li r4,0x500; dcbz r1,r4; lwz r5,0x500(r1) → r5=0
# dcbz = XO=31 XO=1014: 0x7C0007EC | (0<<21)|(1<<16)|(4<<11) = 0x7C0127EC
T_dcbz_basic="3860beef 90610500 38800500 7C0127EC 80A10500"
TEST_ORDER+=(dcbz_basic)

# --- xori ---
# li r3,0xFF; xori r5,r3,0xF0 → r5=0x0F
# xori rA=5,rS=3,UIMM=0xF0: 0x686500F0
T_xori_basic="386000ff 686500F0"
TEST_ORDER+=(xori_basic)

# --- FP compare ---
# Store 1.0 and 2.0, compare: fcmpu cr0,f0,f1 → CR0.LT
T_fcmpu_basic="3C603F80 90610100 38600000 90610104 C0010100 3C604000 90610108 C021010C C8010100 C8210108 FC000000"
TEST_ORDER+=(fcmpu_basic)

# --- FP mul ---

# --- isync (should be NOP) ---
T_isync_basic="4C00012C 60000000"
TEST_ORDER+=(isync_basic)

# --- eieio (should be NOP) ---
# eieio = 0x7C0006AC
T_eieio_basic="7C0006AC 60000000"
TEST_ORDER+=(eieio_basic)

# --- sync (should be NOP) ---
# sync = 0x7C0004AC
T_sync_basic="7C0004AC 60000000"
TEST_ORDER+=(sync_basic)


# ============================================================
# FUZZING VECTORS — edge cases, boundary values, corner cases
# ============================================================

# --- Integer overflow/underflow ---
# add with MAX_INT + 1 → overflow
T_fuzz_add_overflow="3C607FFF 6063FFFF 38800001 7CA32214"
TEST_ORDER+=(fuzz_add_overflow)

# sub producing MIN_INT
T_fuzz_sub_minint="3C608000 38800001 7CA42050"
TEST_ORDER+=(fuzz_sub_minint)

# neg of MIN_INT (0x80000000) → still 0x80000000 (overflow)
T_fuzz_neg_minint="3C608000 7CA300D0"
TEST_ORDER+=(fuzz_neg_minint)

# --- Shift edge cases ---
# slw by 0 (no shift)
T_fuzz_slw_zero="3860FFFF 38800000 7C652030"
TEST_ORDER+=(fuzz_slw_zero)

# slw by 31 (max valid shift)
T_fuzz_slw_31="38600001 3880001F 7C652030"
TEST_ORDER+=(fuzz_slw_31)

# slw by 32 (should produce 0 on PPC)
T_fuzz_slw_32="38600001 38800020 7C652030"
TEST_ORDER+=(fuzz_slw_32)

# srw by 32 (should produce 0)
T_fuzz_srw_32="3860FFFF 38800020 7C652430"
TEST_ORDER+=(fuzz_srw_32)

# sraw by 31 (sign bit fill)
T_fuzz_sraw_31="3C608000 3880001F 7C652630"
TEST_ORDER+=(fuzz_sraw_31)

# sraw of 0 by any amount
T_fuzz_sraw_zero="38600000 38800010 7C652630"
TEST_ORDER+=(fuzz_sraw_zero)

# --- rlwinm edge cases ---
# rotate by 0, full mask
T_fuzz_rlwinm_nop="3860DEAD 5463003E"
TEST_ORDER+=(fuzz_rlwinm_nop)

# rotate by 16, swap halfwords: rlwinm r3,r3,16,0,31
T_fuzz_rlwinm_swap16="3C6012AB 606360CD 5463801E"
TEST_ORDER+=(fuzz_rlwinm_swap16)

# rlwinm with wrapping mask (MB > ME)
# rlwinm r4,r3,0,28,3 → mask = 0xF000000F
T_fuzz_rlwinm_wrapmask="3C60ABCD 6063EF01 5464001E"
TEST_ORDER+=(fuzz_rlwinm_wrapmask)

# --- Multiply edge cases ---
# multiply -1 × -1 = 1
T_fuzz_mul_neg1="3860FFFF 3880FFFF 7CA321D6"
TEST_ORDER+=(fuzz_mul_neg1)

# multiply MAX_INT × 2 → overflow (low word)
T_fuzz_mul_overflow="3C607FFF 6063FFFF 38800002 7CA321D6"
TEST_ORDER+=(fuzz_mul_overflow)

# mulhw: high word of large multiply
T_fuzz_mulhw_big="3C607FFF 6063FFFF 3C807FFF 6084FFFF 7CA32096"
TEST_ORDER+=(fuzz_mulhw_big)

# --- Divide edge cases ---
# divw MIN_INT / -1 → undefined (PPC produces 0)
T_fuzz_divw_minint="3C608000 3880FFFF 7CA323D6"
TEST_ORDER+=(fuzz_divw_minint)

# divw by 0 → undefined
T_fuzz_divw_zero="38600064 38800000 7CA323D6"
TEST_ORDER+=(fuzz_divw_zero)

# divwu large / small
T_fuzz_divwu_large="3C60FFFF 6063FFFF 38800002 7CA32396"
TEST_ORDER+=(fuzz_divwu_large)

# --- Compare edge cases ---
# cmpw: equal values
T_fuzz_cmpw_equal="3860002A 3880002A 7C032000"
TEST_ORDER+=(fuzz_cmpw_equal)

# cmpw: MAX_INT vs MIN_INT
T_fuzz_cmpw_extremes="3C607FFF 6063FFFF 3C808000 7C032000"
TEST_ORDER+=(fuzz_cmpw_extremes)

# cmplwi: 0 vs 0
T_fuzz_cmplwi_zero="38600000 28030000"
TEST_ORDER+=(fuzz_cmplwi_zero)

# --- CR logical edge cases ---
# crxor bit with itself → always 0
T_fuzz_crxor_self="3860FFFF 2C030000 4C000182"
TEST_ORDER+=(fuzz_crxor_self)

# creqv bit with itself → always 1
T_fuzz_creqv_self="3860FFFF 2C030000 4C000242"
TEST_ORDER+=(fuzz_creqv_self)

# --- Load/store with displacement 0 ---
T_fuzz_lwz_disp0="3860BEEF 90610000 80A10000"
TEST_ORDER+=(fuzz_lwz_disp0)

# --- Load/store negative displacement ---
# stwu r1,-32(r1) then lwz from that address
T_fuzz_stwu_neg="9421FFE0 80610000"
TEST_ORDER+=(fuzz_stwu_neg)

# --- Byte operations with 0xFF ---
T_fuzz_stb_ff="386000FF 98610200 88A10200"
TEST_ORDER+=(fuzz_stb_ff)

# --- Halfword sign extension edge ---
# lha of 0x7FFF (positive, no sign ext)
T_fuzz_lha_pos="38607FFF B0610300 A8A10300"
TEST_ORDER+=(fuzz_lha_pos)

# lha of 0xFFFF (-1 sign extended)
T_fuzz_lha_neg1="3860FFFF B0610300 A8A10300"
TEST_ORDER+=(fuzz_lha_neg1)

# --- Record form with zero result ---
# add. 0 + 0 → CR0.EQ should be set
T_fuzz_add_dot_zero="38600000 38800000 7CA32215"
TEST_ORDER+=(fuzz_add_dot_zero)

# --- Carry chain ---
# addic -1,1 → 0 with CA=1; addze r5,r0 → r5 = 0 + CA = 1
T_fuzz_carry_chain="3860FFFF 30630001 7CA00194"
TEST_ORDER+=(fuzz_carry_chain)

# subfic 0,0 → 0 with CA=1; addze r5,r0 → 1
T_fuzz_subfic_carry="20600000 7CA00194"
TEST_ORDER+=(fuzz_subfic_carry)

# --- FP edge cases ---
# fneg of 0.0 → -0.0 (different bit pattern)
T_fuzz_fneg_zero="38600000 90610100 90610104 C8210100 FC2000D0 D8210108"
TEST_ORDER+=(fuzz_fneg_zero)

# fabs of -0.0 → +0.0
T_fuzz_fabs_negzero="3C608000 90610100 38600000 90610104 C8210100 FC200210 D8210108"
TEST_ORDER+=(fuzz_fabs_negzero)

# --- Multi-register operations ---
# lmw/stmw with r31 only (minimum case)
T_fuzz_lmw_r31="3BE0CAFE BFE10400 3BE00000 BBE10400"
TEST_ORDER+=(fuzz_lmw_r31)

# --- bdnz with count=1 (single iteration then fall through) ---
T_fuzz_bdnz_one="38600000 38800001 7C8903A6 38630001 4200FFFC"
TEST_ORDER+=(fuzz_bdnz_one)

# --- cntlzw of powers of 2 ---
T_fuzz_cntlzw_bit0="3C608000 7C650034"
TEST_ORDER+=(fuzz_cntlzw_bit0)

T_fuzz_cntlzw_bit31="38600001 7C650034"
TEST_ORDER+=(fuzz_cntlzw_bit31)

# --- extsb/extsh boundary ---
# extsb of 0x80 → 0xFFFFFF80
T_fuzz_extsb_boundary="38600080 7C650774"
TEST_ORDER+=(fuzz_extsb_boundary)

# extsb of 0x7F → 0x0000007F (no extension)
T_fuzz_extsb_noext="3860007F 7C650774"
TEST_ORDER+=(fuzz_extsb_noext)

# extsh of 0x8000 → 0xFFFF8000
T_fuzz_extsh_boundary="38608000 7C650734"
TEST_ORDER+=(fuzz_extsh_boundary)

# --- All-ones patterns ---
T_fuzz_and_allones="3860FFFF 3880FFFF 7C651838"
TEST_ORDER+=(fuzz_and_allones)

T_fuzz_or_allzero="38600000 38800000 7C651B78"
TEST_ORDER+=(fuzz_or_allzero)

T_fuzz_xor_same="3860ABCD 7C651A78"
TEST_ORDER+=(fuzz_xor_same)


# ============================================================
# FULL COVERAGE VECTORS — every remaining untested opcode class
# ============================================================

# --- FP single precision ---

# --- FP fused multiply-add ---




# --- Indexed load/store ---
# stwx: li r3,0xDEAD; li r4,0; stwx r3,r1,r4; lwzx r5,r1,r4
T_stwx_basic="3860dead 38800000 7C61212E 7CA1202E"
TEST_ORDER+=(stwx_basic)

# stbx/lbzx round-trip
T_stbx_lbzx="38600042 38800100 7C6120AE 7CA120AE"
TEST_ORDER+=(stbx_lbzx)

# sthx/lhzx round-trip  
T_sthx_lhzx="38601234 38800200 7C61232E 7CA1232E"
TEST_ORDER+=(sthx_lhzx)

# lhax (sign-extending indexed)
T_lhax_basic="38608000 B0610300 38800300 7CA122AE"
TEST_ORDER+=(lhax_basic)

# --- Byte-reversed loads ---
# lhbrx: store 0x1234 normally, load byte-reversed → 0x3412
T_lhbrx_basic="38601234 B0610400 38800400 7CA1262C"
TEST_ORDER+=(lhbrx_basic)

# lwbrx
T_lwbrx_basic="3C60DEAD 6063BEEF 90610500 38800500 7CA1242C"
TEST_ORDER+=(lwbrx_basic)

# --- Update forms ---
# lbzu
T_lbzu_basic="38600042 98610200 388101FF 8CA40001"
TEST_ORDER+=(lbzu_basic)

# sthu
T_sthu_basic="38601234 388102FE B0640002"
TEST_ORDER+=(sthu_basic)

# lhau (Load Halfword Algebraic with Update) - opcode 0x2A
# Store a halfword at r1+0x400, then lhau from r1+0x3FE (so lhau 2(r4) loads from 0x400 and updates r4 to 0x400)
T_lhau_basic="38631234 b0610400 388103FE a5640002"
TEST_ORDER+=(lhau_basic)

# lhzu (Load Halfword Zero with Update) - opcode 0x28
# Same test pattern as lhau but with zero-extend instead of sign-extend
T_lhzu_basic="38631234 b0610400 388103FE a1640002"
TEST_ORDER+=(lhzu_basic)

# --- Carry extended ---
# adde: set CA via addic, then adde
T_adde_chain="3860FFFF 30630001 38800005 7CA42114"
TEST_ORDER+=(adde_chain)

# subfe
T_subfe_chain="3860FFFF 30630001 38800005 38600003 7CA32110"
TEST_ORDER+=(subfe_chain)

# addme: rA + CA - 1
T_addme_basic="3860FFFF 30630001 38600005 7CA301D4"
TEST_ORDER+=(addme_basic)

# addze: rA + CA
T_addze_chain="3860FFFF 30630001 38600005 7CA30194"
TEST_ORDER+=(addze_chain)

# subfze: ~rA + CA
T_subfze_basic="3860FFFF 30630001 38600005 7CA30190"
TEST_ORDER+=(subfze_basic)

# subfme: ~rA + CA - 1
T_subfme_basic="3860FFFF 30630001 38600005 7CA301D0"
TEST_ORDER+=(subfme_basic)

# --- mulhwu (unsigned high multiply) ---
T_mulhwu_basic="3C60FFFF 6063FFFF 3C80FFFF 6084FFFF 7CA32016"
TEST_ORDER+=(mulhwu_basic)

# --- mfcr/mtcrf round-trip ---
T_mfcr_mtcrf="3860FFFF 2C030000 7CA00026 7CA0F120"
TEST_ORDER+=(mfcr_mtcrf)

# --- mtcrf partial field (FXM=0x80 — write only CR field 0 from r3) ---
T_mtcrf_partial="38600123 7C680120"
TEST_ORDER+=(mtcrf_partial)

# --- stwbrx: store word byte-reversed indexed (XO=662) ---
# lis/ori r3=0xDEADBEEF; li r4,0x600; stwbrx r3,r1,r4; lwz r5,0x600(r1) → r5=0xEFBEADDE
T_stwbrx_basic="3C60DEAD 6063BEEF 38800600 7C61252C 80A10600"
TEST_ORDER+=(stwbrx_basic)

# --- sthbrx: store halfword byte-reversed indexed (XO=918) ---
# li r3,0x1234; li r4,0x700; sthbrx r3,r1,r4; lhz r5,0x700(r1) → r5=0x3412
T_sthbrx_basic="38601234 38800700 7C61272C A0A10700"
TEST_ORDER+=(sthbrx_basic)

# --- cntlzw mid: r3=0x00FF0000 → 8 leading zeros ---
T_cntlzw_mid="3C6000FF 7C650034"
TEST_ORDER+=(cntlzw_mid)

# --- mcrxr ---
T_mcrxr_basic="3860FFFF 30630001 7C200400"
TEST_ORDER+=(mcrxr_basic)

# --- Conditional bclr ---
# Set CR0.LT via cmpwi, then beqlr (should NOT branch since LT not EQ)
T_bclr_cond="3860FFFF 2C030000 4D820020"
TEST_ORDER+=(bclr_cond)

# --- orc ---
T_orc_basic2="38600000 388000FF 7C652338"
TEST_ORDER+=(orc_basic2)

# --- eqv ---
T_eqv_basic2="386000FF 388000FF 7C652238"
TEST_ORDER+=(eqv_basic2)

# --- andc ---
T_andc_basic2="386000FF 3880000F 7C652078"
TEST_ORDER+=(andc_basic2)

# --- nor ---
T_nor_basic2="38600000 38800000 7C6518F8"
TEST_ORDER+=(nor_basic2)

# --- nand ---
T_nand_basic2="3860FFFF 388000FF 7C6523B8"
TEST_ORDER+=(nand_basic2)

# --- rlwimi ---
T_rlwimi_basic2="3860FF00 38A000FF 5065043E"
TEST_ORDER+=(rlwimi_basic2)

# --- srawi edge ---
T_srawi_neg="3860FF80 7C651E70"
TEST_ORDER+=(srawi_neg)

# --- subfic ---
T_subfic_basic2="3860001E 20A30064"
TEST_ORDER+=(subfic_basic2)

# --- addic carry ---
T_addic_ca="3860FFFF 30A30001"
TEST_ORDER+=(addic_ca)

# --- mfspr/mtspr XER ---
T_mfspr_xer="7CA102A6"
TEST_ORDER+=(mfspr_xer)

# --- cmpw with negative ---
T_cmpw_neg="3860FFFF 38800001 7C032000"
TEST_ORDER+=(cmpw_neg)

# --- cmplw ---
T_cmplw_basic="3860FFFF 38800001 7C032040"
TEST_ORDER+=(cmplw_basic)

# --- lmw with 4 regs ---
T_lmw_4regs="3B800011 3BA00022 3BC00033 3BE00044 BF810400 3B800000 3BA00000 3BC00000 3BE00000 BB810400"
TEST_ORDER+=(lmw_4regs)

# --- divwu ---
T_divwu_basic2="3C60FFFF 6063FFFF 38800002 7CA32396"
TEST_ORDER+=(divwu_basic2)

# --- cntlzw edge: single bit ---
T_cntlzw_bit15="38600001 7C650034"
TEST_ORDER+=(cntlzw_bit15)

# --- neg with 0 ---
T_neg_zero="38600000 7CA300D0"
TEST_ORDER+=(neg_zero)

# --- extsh with 0 ---
T_extsh_zero="38600000 7C650734"
TEST_ORDER+=(extsh_zero)

# --- extsb with 0xFF ---
T_extsb_ff="386000FF 7C650774"
TEST_ORDER+=(extsb_ff)

# --- bdnz with CTR=0: branch IS taken (CTR wraps 0→0xFFFFFFFF, ≠0) ---
# li r3,0; li r4,0; mtctr r4; bdnz +8 (taken→skip next); li r3,0xFF (skipped); li r3,1; mfctr r4
# Expected: r3=1 (branch was taken), r4=0xFFFFFFFF (CTR after wrap)
T_bdnz_zero="38600000 38800000 7C8903A6 42000008 386000FF 38600001 7C8902A6"
TEST_ORDER+=(bdnz_zero)

# --- b (unconditional forward branch) ---
# b +8 skips one instruction
T_b_forward="48000008 38600001 38A00042"
TEST_ORDER+=(b_forward)

# --- record form: subf. ---
T_subf_dot="38600005 38800003 7CA42051"
TEST_ORDER+=(subf_dot)

# --- record form: xor. ---
T_xor_dot="386000FF 388000FF 7C651A79"
TEST_ORDER+=(xor_dot)

# --- record form: neg. ---
T_neg_dot="3860002A 7CA300D1"
TEST_ORDER+=(neg_dot)

# --- isync ---
T_isync_only="4C00012C"
TEST_ORDER+=(isync_only)

# --- sc (system call) ---

# --- twi (trap word immediate) - should NOP ---
T_twi_basic="0C000000"
TEST_ORDER+=(twi_basic)


# ============================================================
# ALTIVEC VECTORS — verify NEON-backed vector operations
# ============================================================














# --- FP load/store coverage ---
# lfs/stfs round-trip: store 2.0 as single, load back
T_fp_lfs_stfs="3C604000 90610100 C0010100 D0010108 80A10108"
TEST_ORDER+=(fp_lfs_stfs)

# lfd/stfd round-trip
T_fp_lfd_stfd="3C604000 90610100 38600000 90610104 C8010100 D8010108 80A10108"
TEST_ORDER+=(fp_lfd_stfd)


# ============================================================
# COMPREHENSIVE FUZZING — edge cases for every opcode class
# ============================================================

# --- Immediate ops edge cases ---
# addi with MAX positive SIMM
T_fuzz_addi_max="38600001 38637FFF"
TEST_ORDER+=(fuzz_addi_max)
# addis overflow
T_fuzz_addis_max="3C607FFF 38638000"
TEST_ORDER+=(fuzz_addis_max)
# mulli overflow: 0x7FFF * 0x7FFF
T_fuzz_mulli_max="38607FFF 1CA37FFF"
TEST_ORDER+=(fuzz_mulli_max)
# ori with 0xFFFF
T_fuzz_ori_ffff="38600000 6063FFFF"
TEST_ORDER+=(fuzz_ori_ffff)
# xori with all-ones
T_fuzz_xori_allones="3860FFFF 6863FFFF"
TEST_ORDER+=(fuzz_xori_allones)
# andi. with 0 (always zero, CR0.EQ)
T_fuzz_andi_zero="3860FFFF 70630000"
TEST_ORDER+=(fuzz_andi_zero)

# --- Compare edge cases ---
# cmplwi unsigned: 0xFFFFFFFF vs 0
T_fuzz_cmplwi_max="3860FFFF 28030000"
TEST_ORDER+=(fuzz_cmplwi_max)
# cmplw: 0 vs 0xFFFFFFFF
T_fuzz_cmplw_max="38600000 3880FFFF 7C032040"
TEST_ORDER+=(fuzz_cmplw_max)

# --- bdnz with CTR=0: branch IS taken, CTR wraps to 0xFFFFFFFF ---
# li r3,0; mtctr r3; bdnz +8 (taken→skip addi); addi r3,r3,1 (skipped); mfctr r3
# Expected: r3=0xFFFFFFFF (mfctr after wrap, addi skipped)
T_fuzz_bdnz_ctr0="38600000 7C0903A6 42000008 38630001 7C6902A6"
TEST_ORDER+=(fuzz_bdnz_ctr0)

# --- Logical op edge cases ---
# andc with all-ones: A & ~B where B=0 → A
T_fuzz_andc_allones="3860FFFF 38800000 7C652078"
TEST_ORDER+=(fuzz_andc_allones)
# orc with all-zeros: A | ~B where B=-1 → A
T_fuzz_orc_allzeros="38600042 3880FFFF 7C652338"
TEST_ORDER+=(fuzz_orc_allzeros)
# eqv all-zeros: ~(0^0) = -1
T_fuzz_eqv_zeros="38600000 38800000 7C652238"
TEST_ORDER+=(fuzz_eqv_zeros)
# nand all-ones: ~(FF&FF) = 0
T_fuzz_nand_allones="3860FFFF 3880FFFF 7C6523B8"
TEST_ORDER+=(fuzz_nand_allones)

# --- Rotate edge cases ---
# rlwimi with SH=0 (no rotation, just mask insert)
T_fuzz_rlwimi_sh0="3860FF00 38A000FF 5065043E"
TEST_ORDER+=(fuzz_rlwimi_sh0)
# rlwimi r27,r29,3,13,28 — exact encoding from DR emulator dispatch loop (ROM 504613e8)
# lis r29,0x1234; ori r29,r29,0x5678; lis r27,0xFFFF; rlwimi r27,r29,3,13,28
# rotl(0x12345678,3)=0x91A2B3C0, mask(13,28)=0x0007FFF8 → r27=0xFFFAB3C0
T_rlwimi_dr_dispatch="3FA01234 63BD5678 3F60FFFF 53BB1B78"
TEST_ORDER+=(rlwimi_dr_dispatch)
# rlwimi r27,r29,3,13,28 — second pattern: r27=0, r29=0xDEADBEEF
# rotl(0xDEADBEEF,3)=0xF56DF77E, mask insert → r27=0x0005F778
T_rlwimi_dr_dispatch2="3FA0DEAD 63BDBEEF 3B600000 53BB1B78"
TEST_ORDER+=(rlwimi_dr_dispatch2)
# rlwnm with count=0
T_fuzz_rlwnm_0="3860DEAD 38800000 5C65203E"
TEST_ORDER+=(fuzz_rlwnm_0)
# rlwinm extract byte: rlwinm r4,r3,0,24,31 (low byte)
T_fuzz_rlwinm_lobyte="3C60DEAD 6063BEEF 5464043E"
TEST_ORDER+=(fuzz_rlwinm_lobyte)

# --- Carry chain stress ---
# Multiple addic in sequence, check CA propagation
T_fuzz_ca_chain3="3860FFFF 30630001 30630001 30630001 7CA30194"
TEST_ORDER+=(fuzz_ca_chain3)

# --- FP edge cases ---
# fsub: 1.0 - 1.0 = 0.0
T_fuzz_fsub_zero="3C603F80 90610100 38600000 90610104 C0010100 C0210100 FC001028 D0010108 80A10108"
TEST_ORDER+=(fuzz_fsub_zero)
# fmul: 0.0 * anything = 0.0
T_fuzz_fmul_zero="38600000 90610100 90610104 C8010100 3C604000 90610108 90610104 C8210108 FC000072 D8010110 80A10110"
TEST_ORDER+=(fuzz_fmul_zero)
# fdiv: 1.0 / 1.0 = 1.0
T_fuzz_fdiv_one="3C603F80 90610100 38600000 90610104 C0010100 C0210100 FC001024 D0010108 80A10108"
TEST_ORDER+=(fuzz_fdiv_one)

# --- Load/store edge cases ---
# lwz from displacement 0
T_fuzz_lwz_d0="3860CAFE 90610000 80A10000"
TEST_ORDER+=(fuzz_lwz_d0)
# stb of 0
T_fuzz_stb_zero="38600000 98610200 88A10200"
TEST_ORDER+=(fuzz_stb_zero)
# sth of 0xFFFF
T_fuzz_sth_ffff="3860FFFF B0610300 A0A10300"
TEST_ORDER+=(fuzz_sth_ffff)

# --- Record form edge cases ---
# add. with zero result → CR0.EQ
T_fuzz_add_dot_eq="38600005 3880FFFB 7CA32215"
TEST_ORDER+=(fuzz_add_dot_eq)
# subf. positive result → CR0.GT
T_fuzz_subf_dot_gt="38600003 38800005 7CA42051"
TEST_ORDER+=(fuzz_subf_dot_gt)

# --- CR edge cases ---
# cror then crand chain
T_fuzz_cr_chain="3860FFFF 2C030000 38800001 2C840000 4C000382 4C000202"
TEST_ORDER+=(fuzz_cr_chain)

# --- AltiVec fuzzing ---
# vadduwm with -1 + 1 = 0 (per word)
T_fuzz_vec_add_wrap="1000FFCC 10230718 10400880 38600600 7C4119CE 80A10600"
TEST_ORDER+=(fuzz_vec_add_wrap)
# vxor v0,v0,v0 clear then check = 0
T_fuzz_vec_clear="10050718 10000E84 38600600 7C0119CE 80A10600"
TEST_ORDER+=(fuzz_vec_clear)
# vcmpequw equal → all 1s
T_fuzz_vec_cmpeq="10050718 10250718 10400886 38600600 7C4119CE 80A10600"
TEST_ORDER+=(fuzz_vec_cmpeq)

# --- Update form edge cases ---
# lwzu then check rA updated
T_fuzz_lwzu_update="3860BEEF 90610400 388103FC 84A40004 7C8402A6"
TEST_ORDER+=(fuzz_lwzu_update)

# --- mfspr/mtspr XER round-trip ---
T_fuzz_xer_roundtrip="3860FFFF 30630001 7C6102A6 7C650026"
TEST_ORDER+=(fuzz_xer_roundtrip)

# --- mulhw with negative * positive ---
T_fuzz_mulhw_neg="3860FFFF 38800002 7CA32096"
TEST_ORDER+=(fuzz_mulhw_neg)

# --- divw: exact division ---
T_fuzz_divw_exact="38600064 3880000A 7CA323D6"
TEST_ORDER+=(fuzz_divw_exact)

# --- cntlzw: value with alternating bits ---
T_fuzz_cntlzw_alt="3C605555 60635555 7C650034"
TEST_ORDER+=(fuzz_cntlzw_alt)

# --- mullwo (OE=1 multiply with overflow detection) ---
# mullwo: 0x10000 × 0x10000 overflows → r5=0, XER OV=1 SO=1
T_mullwo_overflow="3C600001 3C800001 7CA325D6"
TEST_ORDER+=(mullwo_overflow)
# mullwo: 7 × 6 = 42, no overflow → r5=42, XER OV=0
T_mullwo_no_overflow="38600007 38800006 7CA325D6"
TEST_ORDER+=(mullwo_no_overflow)

# ==== AltiVec coverage (2026-06-04) ===========================================
# NOTE: an earlier batch of 14 VX-form AltiVec vectors was REMOVED here. They had
# a DOUBLED XO field (VX-form XO is unshifted, unlike X/A-form), so each decoded
# to an illegal/no-op, left v2 untouched, and "passed" vacuously (r5/r6 read 0 in
# both interp and JIT -> 0==0). Found by adversarial review. Only vsel survived
# (VA-form, correctly encoded); it caught + regression-tests the vsel BSL operand
# swap fix. TODO: a correct VX-form batch needs lvx distinct-lane setup, and a
# correctly-encoded vspltb exposed a hidden interp-vs-JIT divergence to fix first.

# --- vsel v2,v0,v1,v3 (select by mask): v2 = (vB & vC) | (vA & ~vC) ---
T_vsel_mask="1005030C 1023030C 1067030C 104008EA 38600600 7C4119CE 80A10600 80C10604"
TEST_ORDER+=(vsel_mask)

# ==== FP arithmetic coverage (generated by gen-fp-vectors.py) ================
# Each vector loads its FP result back into r4(/r5) because REGDUMP captures
# GPRs, not FPRs (without this the test would be vacuous). See the script header.
# fadd f1,f1,f2: 2.0+3.0=5.0
T_fp_fadd_real="3C604000 90610100 38600000 90610104 C8210100 3C604008 90610110 38600000 90610114 C8410110 FC21102A D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fadd_real)
# fsub f1,f1,f2: 2.0-3.0=-1.0
T_fp_fsub_real="3C604000 90610100 38600000 90610104 C8210100 3C604008 90610110 38600000 90610114 C8410110 FC211028 D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fsub_real)
# fmul f1,f1,f2(C): 2.0*3.0=6.0
T_fp_fmul_real="3C604000 90610100 38600000 90610104 C8210100 3C604008 90610110 38600000 90610114 C8410110 FC2100B2 D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fmul_real)
# fdiv f1,f1,f3: 2.0/4.0=0.5
T_fp_fdiv_real="3C604000 90610100 38600000 90610104 C8210100 3C604010 90610120 38600000 90610124 C8610120 FC211824 D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fdiv_real)
# fmadd f1,f1,f3,f2 = f1*f3+f2: 2.0*4.0+3.0=11.0
T_fp_fmadd_real="3C604000 90610100 38600000 90610104 C8210100 3C604008 90610110 38600000 90610114 C8410110 3C604010 90610120 38600000 90610124 C8610120 FC2110FA D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fmadd_real)
# fmsub f1,f1,f3,f2 = f1*f3-f2: 2.0*4.0-3.0=5.0
T_fp_fmsub_real="3C604000 90610100 38600000 90610104 C8210100 3C604008 90610110 38600000 90610114 C8410110 3C604010 90610120 38600000 90610124 C8610120 FC2110F8 D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fmsub_real)
# fnmadd: -(2.0*4.0+3.0)=-11.0
T_fp_fnmadd_real="3C604000 90610100 38600000 90610104 C8210100 3C604008 90610110 38600000 90610114 C8410110 3C604010 90610120 38600000 90610124 C8610120 FC2110FE D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fnmadd_real)
# fnmsub: -(2.0*4.0-3.0)=-5.0
T_fp_fnmsub_real="3C604000 90610100 38600000 90610104 C8210100 3C604008 90610110 38600000 90610114 C8410110 3C604010 90610120 38600000 90610124 C8610120 FC2110FC D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fnmsub_real)
# frsp f1,f2: round 3.0 to single
T_fp_frsp_real="3C604008 90610110 38600000 90610114 C8410110 FC201018 D8210130 80810130 80A10134"
TEST_ORDER+=(fp_frsp_real)
# fctiwz f1,f2: 3.0 -> int 3
T_fp_fctiwz_real="3C604008 90610110 38600000 90610114 C8410110 FC20101E D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fctiwz_real)
# fctiw 2.5 -> 3 (round half-away per FPSCR)
T_fp_fctiw_round="3C604004 90610110 38600000 90610114 C8410110 FC20101C D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fctiw_round)
# fctiwz 2.5 -> 2 (toward zero)
T_fp_fctiwz_trunc="3C604004 90610110 38600000 90610114 C8410110 FC20101E D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fctiwz_trunc)
# fctiw 2^31 -> 0x7FFFFFFF (saturate)
T_fp_fctiw_ovf="3C6041E0 90610110 38600000 90610114 C8410110 FC20101C D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fctiw_ovf)
# fctiw NaN -> 0x80000000
T_fp_fctiw_nan="3C607FF8 90610110 38600000 90610114 C8410110 FC20101C D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fctiw_nan)
# fctiwz NaN -> 0x80000000
T_fp_fctiwz_nan="3C607FF8 90610110 38600000 90610114 C8410110 FC20101E D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fctiwz_nan)
# fneg f1,f1: -(2.0)
T_fp_fneg_real="3C604000 90610100 38600000 90610104 C8210100 FC200850 D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fneg_real)
# fabs f1,f1: |2.0|=2.0
T_fp_fabs_real="3C604000 90610100 38600000 90610104 C8210100 FC200A10 D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fabs_real)
# fmr f1,f2: copy 3.0
T_fp_fmr_real="3C604008 90610110 38600000 90610114 C8410110 FC201090 D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fmr_real)
# fsel f1,f1(2.0>=0),f3,f2 -> frC=4.0
T_fp_fsel_pos="3C604000 90610100 38600000 90610104 C8210100 3C604008 90610110 38600000 90610114 C8410110 3C604010 90610120 38600000 90610124 C8610120 FC2110EE D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fsel_pos)
# fsel f1,f1(-1.0<0),f3,f2 -> frB=3.0
T_fp_fsel_neg="3C60BFF0 90610100 38600000 90610104 C8210100 3C604008 90610110 38600000 90610114 C8410110 3C604010 90610120 38600000 90610124 C8610120 FC2110EE D8210130 80810130 80A10134"
TEST_ORDER+=(fp_fsel_neg)
# fadds: 2.0f+3.0f=5.0f
T_fp_fadds_real="3C604000 90610100 C0210100 3C604040 90610110 C0410110 EC21102A D0210130 80810130"
TEST_ORDER+=(fp_fadds_real)
# fsubs: 2.0f-3.0f=-1.0f
T_fp_fsubs_real="3C604000 90610100 C0210100 3C604040 90610110 C0410110 EC211028 D0210130 80810130"
TEST_ORDER+=(fp_fsubs_real)
# fmuls: 2.0f*3.0f=6.0f
T_fp_fmuls_real="3C604000 90610100 C0210100 3C604040 90610110 C0410110 EC2100B2 D0210130 80810130"
TEST_ORDER+=(fp_fmuls_real)
# fdivs: 2.0f/4.0f=0.5f
T_fp_fdivs_real="3C604000 90610100 C0210100 3C604080 90610120 C0610120 EC211824 D0210130 80810130"
TEST_ORDER+=(fp_fdivs_real)
# fmadds f1,f1,f3,f2 = f1*f3+f2: 2.0f*4.0f+3.0f=11.0f
T_fp_fmadds_real="3C604000 90610100 C0210100 3C604040 90610110 C0410110 3C604080 90610120 C0610120 EC2110FA D0210130 80810130"
TEST_ORDER+=(fp_fmadds_real)
# 9 FP dests (f0..f8) force FP-RA eviction; evicted f0 must read back 4.0
T_fp_evict_writeback="3C604000 90610100 38600000 90610104 C9410100 FC0A02B2 FC2A02B2 FC4A02B2 FC6A02B2 FC8A02B2 FCAA02B2 FCCA02B2 FCEA02B2 FD0A02B2 D8010130 80810130 80A10134"
TEST_ORDER+=(fp_evict_writeback)
# lfdx f1,r6,r7: load 5.0 double via indexed EA
T_fp_lfdx="3C604014 90610100 38600000 90610104 38C10080 38E00080 7C263CAE D8210130 80810130 80A10134"
TEST_ORDER+=(fp_lfdx)
# lfdux f1,r6,r7: load 5.0 + rA(r6):=EA
T_fp_lfdux="3C604014 90610100 38600000 90610104 38C10080 38E00080 7C263CEE D8210130 80810130 80A10134"
TEST_ORDER+=(fp_lfdux)
# lfsx f1,r6,r7: load 5.0f single via indexed EA
T_fp_lfsx="3C6040A0 90610100 38C10080 38E00080 7C263C2E D8210130 80810130 80A10134"
TEST_ORDER+=(fp_lfsx)
# lfsux f1,r6,r7: load 5.0f + rA(r6):=EA
T_fp_lfsux="3C6040A0 90610100 38C10080 38E00080 7C263C6E D8210130 80810130 80A10134"
TEST_ORDER+=(fp_lfsux)
# stfdx f1,r6,r7: store 5.0 double via indexed EA
T_fp_stfdx="3C604014 90610110 38600000 90610114 C8210110 38C10080 38E00080 7C263DAE 80810100 80A10104"
TEST_ORDER+=(fp_stfdx)
# stfdux f1,r6,r7: store 5.0 + rA(r6):=EA
T_fp_stfdux="3C604014 90610110 38600000 90610114 C8210110 38C10080 38E00080 7C263DEE 80810100 80A10104"
TEST_ORDER+=(fp_stfdux)
# stfsx f1,r6,r7: store 5.0f single via indexed EA
T_fp_stfsx="3C604014 90610110 38600000 90610114 C8210110 38C10080 38E00080 7C263D2E 80810100"
TEST_ORDER+=(fp_stfsx)
# stfsux f1,r6,r7: store 5.0f + rA(r6):=EA
T_fp_stfsux="3C604014 90610110 38600000 90610114 C8210110 38C10080 38E00080 7C263D6E 80810100"
TEST_ORDER+=(fp_stfsux)
# lfdu f1,0x100(r6): load 5.0 double + r6:=EA
T_fp_lfdu="3C604014 90610100 38600000 90610104 38C10000 CC260100 D8210130 80810130 80A10134"
TEST_ORDER+=(fp_lfdu)
# lfsu f1,0x100(r6): load 5.0f single + r6:=EA
T_fp_lfsu="3C6040A0 90610100 38C10000 C4260100 D8210130 80810130 80A10134"
TEST_ORDER+=(fp_lfsu)
# stfdu f1,0x100(r6): store 5.0 double + r6:=EA
T_fp_stfdu="3C604014 90610110 38600000 90610114 C8210110 38C10000 DC260100 80810100 80A10104"
TEST_ORDER+=(fp_stfdu)
# stfsu f1,0x100(r6): store 5.0f single + r6:=EA
T_fp_stfsu="3C604014 90610110 38600000 90610114 C8210110 38C10000 D4260100 80810100"
TEST_ORDER+=(fp_stfsu)
# lfdux while FP-RA full: evicted f0 must read 4.0, f8=5.0, r6=EA
T_fp_evict_across_lfdux="3C604000 90610100 38600000 90610104 C9410100 FC0A02B2 FC2A02B2 FC4A02B2 FC6A02B2 FC8A02B2 FCAA02B2 FCCA02B2 FCEA02B2 3C604014 90610108 38600000 9061010C 38C10008 38E00100 7D063CEE D8010130 80810130 D9010138 80A10138"
TEST_ORDER+=(fp_evict_across_lfdux)

# (probe block)


# --- repro for the confirmed vspltb/vsplth JIT bug (NOT in TEST_ORDER) ---

# ==== AltiVec coverage (generated by gen-altivec-vectors.py) =================
# vspltw v2,v1,0 -> 0x00010203
T_av_vspltw_0="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 10400A8C 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vspltw_0)
# vspltw v2,v1,2 -> 0x08090A0B
T_av_vspltw_2="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 10420A8C 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vspltw_2)
# vspltb v2,v1,0  -> 0x00000000 (ev_mixed remap)
T_av_vspltb_0="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 10400A0C 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vspltb_0)
# vspltb v2,v1,3  -> 0x03030303
T_av_vspltb_3="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 10430A0C 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vspltb_3)
# vspltb v2,v1,15 -> 0x0F0F0F0F
T_av_vspltb_15="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 104F0A0C 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vspltb_15)
# vsplth v2,v1,0  -> 0x00010001
T_av_vsplth_0="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 10400A4C 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsplth_0)
# vsplth v2,v1,3  -> 0x06070607
T_av_vsplth_3="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 10430A4C 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsplth_3)
# vsplth v2,v1,7  -> 0x0E0F0E0F
T_av_vsplth_7="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 10470A4C 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsplth_7)
# vadduwm: 0x05050505+0x03030303=0x08080808
T_av_vadduwm="1005030C 1023030C 10400880 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vadduwm)
# vsubuwm: 0x05..-0x03..=0x02020202
T_av_vsubuwm="1005030C 1023030C 10400C80 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsubuwm)
# vand: 0x05&0x03=0x01010101
T_av_vand="1005030C 1023030C 10400C04 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vand)
# vor: 0x05|0x03=0x07070707
T_av_vor="1005030C 1023030C 10400C84 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vor)
# vxor: 0x05^0x03=0x06060606
T_av_vxor="1005030C 1023030C 10400CC4 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vxor)
# vnor: ~(0x05|0x03)=0xF8F8F8F8
T_av_vnor="1005030C 1023030C 10400D04 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vnor)
# vmaxsw: signed-word max=0x05050505
T_av_vmaxsw="1005030C 1023030C 10400982 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vmaxsw)
# vminsw: signed-word min=0x03030303
T_av_vminsw="1005030C 1023030C 10400B82 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vminsw)
# vcmpequw v2,v0,v0: equal -> 0xFFFFFFFF
T_av_vcmpequw="1005030C 10400086 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vcmpequw)
# vmuloub v2,v1,v3: odd  unsigned byte multiply -> halfword products
T_av_vmuloub="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 3C601011 60631213 90610600 3C601415 60631617 90610604 3C601819 60631A1B 90610608 3C601C1D 60631E1F 9061060C 38600600 7C6118CE 10411808 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vmuloub)
# vmuleub v2,v1,v3: even unsigned byte multiply -> halfword products
T_av_vmuleub="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 3C601011 60631213 90610600 3C601415 60631617 90610604 3C601819 60631A1B 90610608 3C601C1D 60631E1F 9061060C 38600600 7C6118CE 10411A08 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vmuleub)
# vmrghb v2,v1,v3: high-byte merge (ev_mixed-normalized)
T_av_vmrghb="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 3C601011 60631213 90610600 3C601415 60631617 90610604 3C601819 60631A1B 90610608 3C601C1D 60631E1F 9061060C 38600600 7C6118CE 1041180C 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vmrghb)
# vmrglb v2,v1,v3: low-byte merge (ev_mixed-normalized)
T_av_vmrglb="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 3C601011 60631213 90610600 3C601415 60631617 90610604 3C601819 60631A1B 90610608 3C601C1D 60631E1F 9061060C 38600600 7C6118CE 1041190C 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vmrglb)
# vmrghh v2,v1,v3: high-halfword merge (ev_mixed-normalized)
T_av_vmrghh="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 3C601011 60631213 90610600 3C601415 60631617 90610604 3C601819 60631A1B 90610608 3C601C1D 60631E1F 9061060C 38600600 7C6118CE 1041184C 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vmrghh)
# vmrglh v2,v1,v3: low-halfword merge (ev_mixed-normalized)
T_av_vmrglh="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 3C601011 60631213 90610600 3C601415 60631617 90610604 3C601819 60631A1B 90610608 3C601C1D 60631E1F 9061060C 38600600 7C6118CE 1041194C 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vmrglh)
# vmrghw v2,v1,v3 -> high-word merge [A0,B0,A1,B1]
T_av_vmrghw="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 3C601011 60631213 90610600 3C601415 60631617 90610604 3C601819 60631A1B 90610608 3C601C1D 60631E1F 9061060C 38600600 7C6118CE 1041188C 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vmrghw)
# vmrglw v2,v1,v3 -> low-word merge [A2,B2,A3,B3]
T_av_vmrglw="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 3C601011 60631213 90610600 3C601415 60631617 90610604 3C601819 60631A1B 90610608 3C601C1D 60631E1F 9061060C 38600600 7C6118CE 1041198C 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vmrglw)
# vpkuhum v2,v1,v3: halfword->byte pack (ev_mixed-normalized UZP2.16B)
T_av_vpkuhum="3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C2118CE 3C601011 60631213 90610600 3C601415 60631617 90610604 3C601819 60631A1B 90610608 3C601C1D 60631E1F 9061060C 38600600 7C6118CE 1041180E 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vpkuhum)
# vpkshss: halfword->byte signed source, signed-saturate (SQXTN)
T_av_vpkshss="3C600001 60630100 90610600 3C607FFF 60638000 90610604 3C60FF00 606300FF 90610608 3C600080 6063017F 9061060C 38600600 7C2118CE 3C601234 6063FFFF 90610600 3C600010 60638001 90610604 3C607F00 6063007F 90610608 3C60ABCD 60630005 9061060C 38600600 7C6118CE 1041198E 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vpkshss)
# vpkshus: halfword->byte signed source, unsigned-saturate (SQXTUN)
T_av_vpkshus="3C600001 60630100 90610600 3C607FFF 60638000 90610604 3C60FF00 606300FF 90610608 3C600080 6063017F 9061060C 38600600 7C2118CE 3C601234 6063FFFF 90610600 3C600010 60638001 90610604 3C607F00 6063007F 90610608 3C60ABCD 60630005 9061060C 38600600 7C6118CE 1041190E 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vpkshus)
# vpkuhus: halfword->byte unsigned source, unsigned-saturate (UQXTN)
T_av_vpkuhus="3C600001 60630100 90610600 3C607FFF 60638000 90610604 3C60FF00 606300FF 90610608 3C600080 6063017F 9061060C 38600600 7C2118CE 3C601234 6063FFFF 90610600 3C600010 60638001 90610604 3C607F00 6063007F 90610608 3C60ABCD 60630005 9061060C 38600600 7C6118CE 1041188E 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vpkuhus)
# vpkswss: word->halfword signed source, signed-saturate (SQXTN.4H)
T_av_vpkswss="3C600000 60630001 90610600 3C600001 60630000 90610604 3C600000 60638000 90610608 3C608000 60630000 9061060C 38600600 7C2118CE 3C60FFFF 60630000 90610600 3C600000 60637FFF 90610604 3C601234 60635678 90610608 3C600000 6063FFFF 9061060C 38600600 7C6118CE 104119CE 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vpkswss)
# vpkswus: word->halfword signed source, unsigned-saturate (SQXTUN.4H)
T_av_vpkswus="3C600000 60630001 90610600 3C600001 60630000 90610604 3C600000 60638000 90610608 3C608000 60630000 9061060C 38600600 7C2118CE 3C60FFFF 60630000 90610600 3C600000 60637FFF 90610604 3C601234 60635678 90610608 3C600000 6063FFFF 9061060C 38600600 7C6118CE 1041194E 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vpkswus)
# vpkuwus: word->halfword unsigned source, unsigned-saturate (UQXTN.4H)
T_av_vpkuwus="3C600000 60630001 90610600 3C600001 60630000 90610604 3C600000 60638000 90610608 3C608000 60630000 9061060C 38600600 7C2118CE 3C60FFFF 60630000 90610600 3C600000 60637FFF 90610604 3C601234 60635678 90610608 3C600000 6063FFFF 9061060C 38600600 7C6118CE 104118CE 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vpkuwus)
# vpkuwum: word->halfword modulo (low 16 bits, XTN)
T_av_vpkuwum="3C601111 6063AAAA 90610600 3C602222 6063BBBB 90610604 3C603333 6063CCCC 90610608 3C604444 6063DDDD 9061060C 38600600 7C2118CE 3C605555 6063EEEE 90610600 3C606666 60630001 90610604 3C607777 60630002 90610608 3C608888 60630003 9061060C 38600600 7C6118CE 1041184E 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vpkuwum)
# vslb v2,v1,v3: byte shift-left, amount masked mod 8
T_av_vslb="3C608081 60638283 90610600 3C608485 60638687 90610604 3C608889 60638A8B 90610608 3C608C8D 60638E8F 9061060C 38600600 7C2118CE 3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C6118CE 10411904 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vslb)
# vsrb v2,v1,v3: byte LOGICAL shift-right (zero-fill), mask mod 8
T_av_vsrb="3C608081 60638283 90610600 3C608485 60638687 90610604 3C608889 60638A8B 90610608 3C608C8D 60638E8F 9061060C 38600600 7C2118CE 3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C6118CE 10411A04 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsrb)
# vsrab v2,v1,v3: byte ARITHMETIC shift-right (sign-fill), mask mod 8
T_av_vsrab="3C608081 60638283 90610600 3C608485 60638687 90610604 3C608889 60638A8B 90610608 3C608C8D 60638E8F 9061060C 38600600 7C2118CE 3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C6118CE 10411B04 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsrab)
# vslh: halfword shift-left, amount masked mod 16
T_av_vslh="3C608081 60638283 90610600 3C608485 60638687 90610604 3C608889 60638A8B 90610608 3C608C8D 60638E8F 9061060C 38600600 7C2118CE 3C601011 60631213 90610600 3C601415 60631617 90610604 3C601819 60631A1B 90610608 3C601C1D 60631E1F 9061060C 38600600 7C6118CE 10411944 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vslh)
# vsrh: halfword LOGICAL shift-right (zero-fill), mask mod 16
T_av_vsrh="3C608081 60638283 90610600 3C608485 60638687 90610604 3C608889 60638A8B 90610608 3C608C8D 60638E8F 9061060C 38600600 7C2118CE 3C601011 60631213 90610600 3C601415 60631617 90610604 3C601819 60631A1B 90610608 3C601C1D 60631E1F 9061060C 38600600 7C6118CE 10411A44 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsrh)
# vsrah: halfword ARITHMETIC shift-right (sign-fill), mask mod 16
T_av_vsrah="3C608081 60638283 90610600 3C608485 60638687 90610604 3C608889 60638A8B 90610608 3C608C8D 60638E8F 9061060C 38600600 7C2118CE 3C601011 60631213 90610600 3C601415 60631617 90610604 3C601819 60631A1B 90610608 3C601C1D 60631E1F 9061060C 38600600 7C6118CE 10411B44 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsrah)
# vslw: word shift-left, amount masked mod 32
T_av_vslw="3C608081 60638283 90610600 3C608485 60638687 90610604 3C608889 60638A8B 90610608 3C608C8D 60638E8F 9061060C 38600600 7C2118CE 3C602021 60632223 90610600 3C602425 60632627 90610604 3C602829 60632A2B 90610608 3C602C2D 60632E2F 9061060C 38600600 7C6118CE 10411984 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vslw)
# vsrw: word LOGICAL shift-right (zero-fill), mask mod 32
T_av_vsrw="3C608081 60638283 90610600 3C608485 60638687 90610604 3C608889 60638A8B 90610608 3C608C8D 60638E8F 9061060C 38600600 7C2118CE 3C602021 60632223 90610600 3C602425 60632627 90610604 3C602829 60632A2B 90610608 3C602C2D 60632E2F 9061060C 38600600 7C6118CE 10411A84 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsrw)
# vsraw: word ARITHMETIC shift-right (sign-fill), mask mod 32
T_av_vsraw="3C608081 60638283 90610600 3C608485 60638687 90610604 3C608889 60638A8B 90610608 3C608C8D 60638E8F 9061060C 38600600 7C2118CE 3C602021 60632223 90610600 3C602425 60632627 90610604 3C602829 60632A2B 90610608 3C602C2D 60632E2F 9061060C 38600600 7C6118CE 10411B84 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsraw)
# vrlb: rotate-left byte (mod 8), wrap-around
T_av_vrlb="3C608081 60638283 90610600 3C608485 60638687 90610604 3C608889 60638A8B 90610608 3C608C8D 60638E8F 9061060C 38600600 7C2118CE 3C600001 60630203 90610600 3C600405 60630607 90610604 3C600809 60630A0B 90610608 3C600C0D 60630E0F 9061060C 38600600 7C6118CE 10411804 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vrlb)
# vrlh: rotate-left halfword (mod 16), wrap-around
T_av_vrlh="3C608081 60638283 90610600 3C608485 60638687 90610604 3C608889 60638A8B 90610608 3C608C8D 60638E8F 9061060C 38600600 7C2118CE 3C601011 60631213 90610600 3C601415 60631617 90610604 3C601819 60631A1B 90610608 3C601C1D 60631E1F 9061060C 38600600 7C6118CE 10411844 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vrlh)
# vrlw: rotate-left word (mod 32), wrap-around
T_av_vrlw="3C608081 60638283 90610600 3C608485 60638687 90610604 3C608889 60638A8B 90610608 3C608C8D 60638E8F 9061060C 38600600 7C2118CE 3C602021 60632223 90610600 3C602425 60632627 90610604 3C602829 60632A2B 90610608 3C602C2D 60632E2F 9061060C 38600600 7C6118CE 10411884 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vrlw)
# vaddubs: unsigned saturating add, byte
T_av_vaddubs="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411A00 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vaddubs)
# vadduhs: unsigned saturating add, halfword
T_av_vadduhs="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411A40 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vadduhs)
# vadduws: unsigned saturating add, word
T_av_vadduws="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411A80 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vadduws)
# vaddsbs: signed saturating add, byte
T_av_vaddsbs="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411B00 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vaddsbs)
# vaddshs: signed saturating add, halfword
T_av_vaddshs="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411B40 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vaddshs)
# vaddsws: signed saturating add, word
T_av_vaddsws="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411B80 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vaddsws)
# vsububs: unsigned saturating sub, byte
T_av_vsububs="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411E00 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsububs)
# vsubuhs: unsigned saturating sub, halfword
T_av_vsubuhs="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411E40 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsubuhs)
# vsubuws: unsigned saturating sub, word (newly JIT-accelerated)
T_av_vsubuws="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411E80 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsubuws)
# vsubsbs: signed saturating sub, byte
T_av_vsubsbs="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411F00 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsubsbs)
# vsubshs: signed saturating sub, halfword
T_av_vsubshs="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411F40 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsubshs)
# vsubsws: signed saturating sub, word
T_av_vsubsws="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411F80 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vsubsws)
# vavgsb: signed rounding average, byte
T_av_vavgsb="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411D02 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vavgsb)
# vavgsh: signed rounding average, halfword
T_av_vavgsh="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411D42 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vavgsh)
# vavgsw: signed rounding average, word
T_av_vavgsw="3C607F80 606301FF 90610600 3C6040C0 60631090 90610604 3C607E81 606302FE 90610608 3C6041C1 60631191 9061060C 38600600 7C2118CE 3C600180 6063FF02 90610600 3C60C040 60639010 90610604 3C60027F 6063FE03 90610608 3C60C141 60639111 9061060C 38600600 7C6118CE 10411D82 38600600 7C4119CE 80A10600"
TEST_ORDER+=(av_vavgsw)
# --- repro for the confirmed vspltb/vsplth JIT bug (NOT in TEST_ORDER) ---
# ==== QUARANTINE: confirmed JIT divergences awaiting a fix (ROADMAP A2) ========
# Run but do NOT count toward pass/fail/score — KNOWN-FAIL repros of a confirmed
# divergence. When a fix lands they flip xfail→xpass and the harness says "promote".
# Currently EMPTY: the entire AltiVec ev_mixed element-order class (splats, merges,
# pack, even/odd byte multiplies) is fixed + promoted. Still-broken siblings that
# lack a test vector (halfword multiplies vmul*h, word pack vpkuwum) are tracked in
# ROADMAP A2, not here — add a quarantine vector when one is written.
QUARANTINE_ORDER=()

# ---- Execute all tests -------------------------------------------------------
PASS=0
FAIL=0
TOTAL=${#TEST_ORDER[@]}

echo "HARNESS mode=$SS_HARNESS_MODE" >&2

# ---- Harness integrity preflight ---------------------------------------------
# The SheepShaver harness previously had NO self-validation (unlike BasiliskII's).
# Validate the vector table before trusting any result: every TEST_ORDER entry has
# a definition, tokens are well-formed (8 hex chars), and no name/hex is duplicated.
# Vacuousness has two tiers:
#   - SHALLOW (all-NOP body): guarded below. A vector that is nothing but PPC NOPs
#     (60000000 = ori r0,r0,0) exercises only decode/dispatch and asserts nothing
#     under the interp-vs-JIT differential — almost always a gutted/mis-pasted
#     payload. Mirrors BasiliskII/jit-test/run.sh.
#   - DEEP (real opcodes whose result hides in an FPR/VR/memory the REGDUMP can't
#     see, or operands too trivial to distinguish a buggy backend): NOT caught here.
#     That defense lives in the gen-*-vectors.py generators (result built into a GPR
#     with distinct operands); a harness-side deep guard needs a sentinel/mutation
#     redesign (deferred).
_seen_name=""; _seen_hex=""; infra_fail=0
for name in "${TEST_ORDER[@]}"; do
    eval "hex=\"\${T_${name}:-}\""
    if [ -z "$hex" ]; then echo "INFRA: $name is in TEST_ORDER but has no T_$name" >&2; infra_fail=1; continue; fi
    for tok in $hex; do
        case "$tok" in
            [0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]) ;;
            *) echo "INFRA: $name has malformed token '$tok' (need exactly 8 hex chars)" >&2; infra_fail=1 ;;
        esac
    done
    # Shallow vacuousness guard (see two-tier note above): an all-NOP body asserts
    # nothing under the interp-vs-JIT differential. 'nop' is the deliberate
    # decode/dispatch sanity vector and is allow-listed; any other all-NOP body is
    # treated as a gutted/mis-pasted payload. (PPC NOP 60000000 is all-digits, so no
    # case folding is needed; the token loop above already proved $hex is non-empty.)
    case " nop " in
        *" $name "*) ;;   # allow-listed decode/dispatch sanity vector
        *)
            _all_nop=1
            for tok in $hex; do [ "$tok" = "60000000" ] || _all_nop=0; done
            if [ "$_all_nop" = 1 ]; then
                echo "INFRA: vacuous vector '$name': body is all-NOP (60000000), exercises no opcode under test; allow-list it only if it is a deliberate decode/dispatch sanity vector" >&2
                infra_fail=1
            fi
            ;;
    esac
    # Duplicate NAME is a real bug: the 2nd T_<name> shadows the 1st in bash var
    # lookup, so one of the two vectors never runs (lost coverage). Hard fail.
    case " $_seen_name " in *" $name "*) echo "INFRA: duplicate vector name '$name' (shadows an earlier vector — one never runs)" >&2; infra_fail=1 ;; esac
    # Duplicate HEX is redundancy (same test twice under different names) — warn only.
    case "$_seen_hex" in *"|$hex|"*) echo "INFRA-WARN: '$name' duplicates the hex of an earlier vector (redundant)" >&2 ;; esac
    _seen_name="$_seen_name $name"; _seen_hex="$_seen_hex|$hex|"
done
# Quarantine vectors get the same well-formedness checks, and must NOT also be a
# scored TEST_ORDER vector (a vector is either scored or quarantined, never both).
if [ "${#QUARANTINE_ORDER[@]}" -gt 0 ]; then
    for name in "${QUARANTINE_ORDER[@]}"; do
        eval "hex=\"\${T_${name}:-}\""
        if [ -z "$hex" ]; then echo "INFRA: quarantine '$name' has no T_$name" >&2; infra_fail=1; continue; fi
        for tok in $hex; do
            case "$tok" in
                [0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]) ;;
                *) echo "INFRA: quarantine '$name' has malformed token '$tok'" >&2; infra_fail=1 ;;
            esac
        done
        case " $_seen_name " in *" $name "*) echo "INFRA: quarantine '$name' is also in TEST_ORDER" >&2; infra_fail=1 ;; esac
    done
fi
if [ "$infra_fail" = "1" ]; then echo "METRIC infra_fail=1"; echo "ABORT: harness integrity check failed before running any vector" >&2; exit 1; fi
echo "METRIC infra_fail=0"

for name in "${TEST_ORDER[@]}"; do
    eval "hex=\"\${T_${name}}\""
    out1="$RUN_DIR/${name}-run1.txt"
    out2="$RUN_DIR/${name}-run2.txt"

    if [ "$SS_HARNESS_MODE" = "jit" ]; then
        # Equivalence: interpreter REGDUMP (reference) vs JIT REGDUMP.
        run_ppc_test "$name" "$hex" "$out1"           # interpreter (reference)
        run_ppc_test "${name}_jit" "$hex" "$out2" jit # JIT
    else
        # Determinism: run twice through the interpreter (upstream behavior).
        run_ppc_test "$name" "$hex" "$out1"
        run_ppc_test "${name}_r2" "$hex" "$out2"
    fi

    if [ -s "$out1" ] && [ -s "$out2" ]; then
        if diff -q "$out1" "$out2" >/dev/null 2>&1; then
            echo "METRIC opcode_${name}=1"
            PASS=$((PASS+1))
        else
            echo "METRIC opcode_${name}=0"
            echo "  DIFF for $name:" >&2
            diff "$out1" "$out2" >&2 || true
            FAIL=$((FAIL+1))
        fi
    else
        echo "METRIC opcode_${name}=-1"
        FAIL=$((FAIL+1))
        # Show what happened
        if [ ! -s "$out1" ]; then
            echo "  $name: no REGDUMP from reference (interpreter) run" >&2
            tail -5 "$RUN_DIR/test-${name}/emu.log" >&2 2>/dev/null || true
        fi
        if [ "$SS_HARNESS_MODE" = "jit" ] && [ ! -s "$out2" ]; then
            echo "  $name: no REGDUMP from JIT run" >&2
            tail -5 "$RUN_DIR/test-${name}_jit/emu.log" >&2 2>/dev/null || true
        fi
    fi
done

# ---- Quarantine run: KNOWN-FAIL repros, JIT mode only, NOT scored ------------
# Confirmed JIT divergences (ROADMAP A2). xfail = still diverges (expected, the
# known bug). xpass = no longer diverges → the fix likely landed; promote to
# TEST_ORDER. Score is computed over TEST_ORDER only, so these never mask a green.
if [ "$SS_HARNESS_MODE" = "jit" ] && [ "${#QUARANTINE_ORDER[@]}" -gt 0 ]; then
    QXFAIL=0; QXPASS=0
    for name in "${QUARANTINE_ORDER[@]}"; do
        eval "hex=\"\${T_${name}}\""
        qo1="$RUN_DIR/${name}-q1.txt"; qo2="$RUN_DIR/${name}-q2.txt"
        run_ppc_test "$name" "$hex" "$qo1"            # interpreter (reference)
        run_ppc_test "${name}_jit" "$hex" "$qo2" jit  # JIT
        if [ -s "$qo1" ] && [ -s "$qo2" ] && diff -q "$qo1" "$qo2" >/dev/null 2>&1; then
            echo "METRIC quarantine_${name}=xpass"
            echo "  QUARANTINE-XPASS: $name no longer diverges — A2 fix may have landed; promote to TEST_ORDER." >&2
            QXPASS=$((QXPASS+1))
        else
            echo "METRIC quarantine_${name}=xfail"
            QXFAIL=$((QXFAIL+1))
        fi
    done
    echo "METRIC quarantine_total=${#QUARANTINE_ORDER[@]}"
    echo "METRIC quarantine_xfail=$QXFAIL"
    echo "METRIC quarantine_xpass=$QXPASS"
    echo "HARNESS quarantine: $QXFAIL still-diverging (expected), $QXPASS now-passing (promote!)" >&2
fi

SCORE=$(( TOTAL > 0 ? PASS * 100 / TOTAL : 0 ))
echo "METRIC pass=$PASS"
echo "METRIC fail=$FAIL"
echo "METRIC total=$TOTAL"
echo "METRIC score=$SCORE"

rm -rf "$RUN_DIR"
