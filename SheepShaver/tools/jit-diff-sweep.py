#!/usr/bin/env python3
# jit-diff-sweep.py — differential AltiVec/FP op sweep + auto-referee for the SheepShaver JIT.
#
# WHAT / WHY
# ----------
# This is the unified-instrumentation payoff (ROADMAP A1 #11, Phase 0+2): it formalizes the
# throwaway sweep scripts + by-hand SS_TEST_HEX triage that found 27 codegen bugs in 2026-06.
#
# For each registered op it injects a single instruction (with crafted operands) through the
# REAL emulator twice — interpreter (SS_TEST_JIT=0) and JIT (SS_TEST_JIT=1) — and diffs the
# result register. The interpreter IS the trusted oracle, so:
#   interp == JIT  -> the JIT op is correct (for these operands)
#   interp != JIT  -> a real JIT-vs-interp divergence (a bug to triage)
# i.e. this tool is its own "auto-referee": it runs the trusted oracle directly, no second pass.
#
# It writes run-stamped output (JSONL records + a summary) under $SS_RUN_DIR (default a fresh
# /tmp/macemu-runs/<timestamp>-sweep dir, with a `latest` symlink), per the #11 schema. Each
# FAIL record carries the exact SS_TEST_HEX repro so it's directly triageable / committable as a
# bug vector.
#
# LESSONS BAKED IN (see docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md):
#  - operands are lane-distinct & non-saturating where possible, so a lane/byte-order bug can't
#    hide behind a uniform result (the "saturating-operand trap");
#  - ops the interpreter can't reference (fsqrt/fres/frsqrte: unimplemented on 603/604/750, or
#    implementation-defined estimates) are SKIPPED, not reported as bugs.
#
# USAGE
#   tools/jit-diff-sweep.py [--bin PATH] [--families F1,F2,...] [--list] [--timeout N]
#   SS_RUN_DIR=/path tools/jit-diff-sweep.py        # explicit output dir
# Exit code: 0 if no real divergences, 1 if any FAIL.

import os, sys, subprocess, json, argparse, time, glob

# ---- PPC instruction encoders (big-endian 32-bit words) ----
def lis(r, i):   return 0x3C000000 | (r << 21) | (i & 0xFFFF)
def ori(r, i):   return 0x60000000 | (r << 21) | (r << 16) | (i & 0xFFFF)
def li(r, i):    return 0x38000000 | (r << 21) | (i & 0xFFFF)
def stw(r, d, a):return 0x90000000 | (r << 21) | (a << 16) | (d & 0xFFFF)
def lwz(r, d, a):return 0x80000000 | (r << 21) | (a << 16) | (d & 0xFFFF)
def lvx(v, a, b):return 0x7C000000 | (v << 21) | (a << 16) | (b << 11) | (103 << 1)
def stvx(v, a, b):return 0x7C000000 | (v << 21) | (a << 16) | (b << 11) | (231 << 1)
def lfd(f, d, a):return 0xC8000000 | (f << 21) | (a << 16) | (d & 0xFFFF)
def stfd(f, d, a):return 0xD8000000 | (f << 21) | (a << 16) | (d & 0xFFFF)
def vx(vD, fA, fB, xo):    return (4 << 26) | (vD << 21) | (fA << 16) | (fB << 11) | xo
def aform(op, fD, fA, fB, fC, xo): return (op << 26) | (fD << 21) | (fA << 16) | (fB << 11) | (fC << 6) | (xo << 1)
def xform(op, fD, fA, fB, xo):     return (op << 26) | (fD << 21) | (fA << 16) | (fB << 11) | (xo << 1)
def H(w): return "%08X" % (w & 0xFFFFFFFF)

OFF = 0x600
def load_bytes(vT, bs):  # vT.bytes = bs[0..15]
    out = []
    for k in range(0, 16, 4):
        out += [lis(3, (bs[k] << 8) | bs[k+1]), ori(3, (bs[k+2] << 8) | bs[k+3]), stw(3, OFF+k, 1)]
    return out + [li(3, OFF), lvx(vT, 1, 3)]
def load_pattern(vT, base): return load_bytes(vT, [(base + k) & 0xFF for k in range(16)])
def grab_vr():  return [li(3, OFF), stvx(2, 1, 3), lwz(5, OFF, 1)]      # result VR2 (full 128b in REGDUMP)
def setd(fT, hi32, lo32, off):
    return [lis(3, (hi32 >> 16) & 0xFFFF), ori(3, hi32 & 0xFFFF), stw(3, off, 1),
            lis(3, (lo32 >> 16) & 0xFFFF), ori(3, lo32 & 0xFFFF), stw(3, off+4, 1), lfd(fT, off, 1)]
def grab_fp(off): return [stfd(1, off, 1), lwz(4, off, 1), lwz(5, off+4, 1)]   # FP result -> r4:r5

# Operand pairs (lane-distinct, sign/saturation-crossing) reused across families.
_A = [0x7F,0x80,0x01,0xFF,0x40,0xC0,0x10,0x90, 0x7E,0x81,0x02,0xFE,0x41,0xC1,0x11,0x91]
_B = [0x01,0x80,0xFF,0x02,0xC0,0x40,0x90,0x10, 0x02,0x7F,0xFE,0x03,0xC1,0x41,0x91,0x11]

def vop(xo, A=_A, B=_B):   # two-source VX op into v2, result observable as VR2
    return load_bytes(1, A) + load_bytes(3, B) + [vx(2, 1, 3, xo)] + grab_vr()
def vshift(xo, dbase, abase):  # data v1, per-lane amount v3
    return load_pattern(1, dbase) + load_pattern(3, abase) + [vx(2, 1, 3, xo)] + grab_vr()

# ---- sweep registry: family -> [(name, words, result_field, note)] ----
# result_field: "VR2" (vector), or "GPR5"/"GPR4" (FP via stfd+lwz).
def REG():
    R = {}
    R["shift"] = [
        ("vslb",  vshift(260, 0x80, 0x00), "VR2", ""), ("vslh", vshift(324, 0x80, 0x10), "VR2", ""),
        ("vslw",  vshift(388, 0x80, 0x20), "VR2", ""), ("vsrb", vshift(516, 0x80, 0x00), "VR2", ""),
        ("vsrh",  vshift(580, 0x80, 0x10), "VR2", ""), ("vsrw", vshift(644, 0x80, 0x20), "VR2", ""),
        ("vsrab", vshift(772, 0x80, 0x00), "VR2", ""), ("vsrah",vshift(836, 0x80, 0x10), "VR2", ""),
        ("vsraw", vshift(900, 0x80, 0x20), "VR2", ""), ("vrlb", vshift(4,   0x80, 0x00), "VR2", ""),
        ("vrlh",  vshift(68,  0x80, 0x10), "VR2", ""), ("vrlw", vshift(132, 0x80, 0x20), "VR2", ""),
    ]
    R["satarith"] = [
        ("vaddubs", vop(512), "VR2", ""), ("vadduhs", vop(576), "VR2", ""), ("vadduws", vop(640), "VR2", ""),
        ("vaddsbs", vop(768), "VR2", ""), ("vaddshs", vop(832), "VR2", ""), ("vaddsws", vop(896), "VR2", ""),
        ("vsububs", vop(1536),"VR2", ""), ("vsubuhs", vop(1600),"VR2", ""), ("vsubuws", vop(1664),"VR2", ""),
        ("vsubsbs", vop(1792),"VR2", ""), ("vsubshs", vop(1856),"VR2", ""), ("vsubsws", vop(1920),"VR2", ""),
        ("vavgsb",  vop(1282),"VR2", ""), ("vavgsh",  vop(1346),"VR2", ""), ("vavgsw",  vop(1410),"VR2", ""),
    ]
    R["compare"] = [
        ("vcmpequb", vop(6),  "VR2", ""), ("vcmpequh", vop(70), "VR2", ""), ("vcmpequw", vop(134),"VR2", ""),
        ("vcmpgtub", vop(518),"VR2", ""), ("vcmpgtsb", vop(774),"VR2", ""), ("vcmpgtsw", vop(902),"VR2", ""),
    ]
    # Known-broken (regression watch): the pack family — confirmed divergent, ev_mixed 2-source
    # rework pending (docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md). Listed so the sweep TRACKS them.
    R["pack"] = [
        ("vpkshss", vop(398), "VR2", "KNOWN-BROKEN: ev_mixed 2-source narrow (A2)"),
        ("vpkuhus", vop(142), "VR2", "KNOWN-BROKEN (A2)"),
        ("vpkswss", vop(462), "VR2", "KNOWN-BROKEN (A2)"),
    ]
    # FP: fctiw/fctiwz edge cases; fsel/fnabs. (fsqrt/fres/frsqrte intentionally absent — the
    # interpreter can't reference them, so a differential test is meaningless.)
    D = {'p2_5': (0x40040000, 0), 'big': (0x41E00000, 0), 'nan': (0x7FF80000, 0),
         'p1': (0x3FF00000, 0), 'm1': (0xBFF00000, 0)}
    def fpx(xo, key):  # X-form unary FP: f1 = op(f2=val)
        return setd(2, *D[key], 0x110) + [xform(63, 1, 0, 2, xo)] + grab_fp(0x130)
    R["fp"] = [
        ("fctiw@2.5",  fpx(14, 'p2_5'), "GPR5", "round-half-away -> 3"),
        ("fctiw@2^31", fpx(14, 'big'),  "GPR5", "saturate -> 0x7FFFFFFF"),
        ("fctiw@NaN",  fpx(14, 'nan'),  "GPR5", "NaN -> 0x80000000"),
        ("fctiwz@2.5", fpx(15, 'p2_5'), "GPR5", "toward zero -> 2"),
        ("fnabs@1.0",  fpx(136,'p1'),   "GPR5", ""),
    ]
    return R

def regdump_field(out, field):
    for tok in out.split():
        if tok.startswith(field + "="):
            return tok.split("=", 1)[1]
    return None

def run_once(binp, hexwords, jit, timeout):
    env = dict(os.environ, SS_TEST_HEX=hexwords, SS_TEST_DUMP="1", SS_TEST_JIT=("1" if jit else "0"))
    try:
        # macOS has no `timeout(1)`; bound with perl alarm.
        p = subprocess.run(["perl", "-e", "alarm shift; exec @ARGV", str(timeout), binp],
                           env=env, capture_output=True, text=True, timeout=timeout + 5)
        return p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return ""

def main():
    ap = argparse.ArgumentParser(description="Differential AltiVec/FP op sweep vs the real interpreter.")
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--bin", default=os.path.join(here, "..", "src", "Unix", "SheepShaver"))
    ap.add_argument("--families", default="", help="comma list (default: all). e.g. shift,fp")
    ap.add_argument("--list", action="store_true", help="list families and exit")
    ap.add_argument("--timeout", type=int, default=20)
    args = ap.parse_args()

    reg = REG()
    if args.list:
        for fam, ops in reg.items():
            print(f"{fam:10} ({len(ops)} ops)")
        return 0
    binp = os.path.abspath(args.bin)
    if not os.path.exists(binp):
        print(f"ERROR: emulator not found at {binp} (build with `make build-ss`)", file=sys.stderr)
        return 2
    fams = args.families.split(",") if args.families else list(reg.keys())

    run_dir = os.environ.get("SS_RUN_DIR")
    if not run_dir:
        # caller didn't set one — stamp a fresh dir (scripts can't, so the orchestrator/CLI does)
        ts = time.strftime("%Y-%m-%dT%H-%M-%SZ", time.gmtime())
        base = os.path.join(os.environ.get("TMPDIR", "/tmp"), "macemu-runs")
        run_dir = os.path.join(base, f"{ts}-sweep")
        os.makedirs(run_dir, exist_ok=True)
        link = os.path.join(base, "latest")
        try:
            if os.path.islink(link) or os.path.exists(link): os.remove(link)
            os.symlink(run_dir, link)
        except OSError:
            pass
    os.makedirs(run_dir, exist_ok=True)
    jsonl = open(os.path.join(run_dir, "diff-sweep.jsonl"), "w")

    n_pass = n_fail = n_known = 0
    fails = []
    print(f"jit-diff-sweep: bin={binp}\n  run_dir={run_dir}")
    for fam in fams:
        if fam not in reg:
            print(f"  (no such family: {fam})"); continue
        for name, words, field, note in reg[fam]:
            hx = " ".join(H(w) for w in words)
            i = regdump_field(run_once(binp, hx, False, args.timeout), field)
            j = regdump_field(run_once(binp, hx, True,  args.timeout), field)
            known = "KNOWN-BROKEN" in note
            if i is None or j is None:
                verdict = "error"
            elif i == j:
                verdict = "pass"
            else:
                verdict = "known-broken" if known else "FAIL"
            rec = {"schema": 1, "tool": "diff-sweep", "kind": "correctness", "family": fam,
                   "op": name, "verdict": verdict, "oracle": "real-interp",
                   "interp": i, "jit": j, "note": note, "hex": hx}
            jsonl.write(json.dumps(rec) + "\n")
            if verdict == "pass": n_pass += 1
            elif verdict == "known-broken":
                n_known += 1; print(f"  known-broken {fam}/{name}  ({note})")
            else:
                n_fail += 1; fails.append(rec)
                print(f"  FAIL {fam}/{name}: interp={i} jit={j}\n       repro: SS_TEST_HEX=\"{hx}\" SS_TEST_DUMP=1 SS_TEST_JIT={{0,1}} {binp}")
    jsonl.close()
    summary = {"pass": n_pass, "fail": n_fail, "known_broken": n_known, "run_dir": run_dir}
    with open(os.path.join(run_dir, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2)
    print(f"\n  pass={n_pass} FAIL={n_fail} known-broken={n_known}   ({run_dir}/summary.json)")
    return 1 if n_fail else 0

if __name__ == "__main__":
    sys.exit(main())
