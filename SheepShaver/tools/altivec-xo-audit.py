#!/usr/bin/env python3
"""altivec-xo-audit.py — cross-check the AArch64 JIT's AltiVec + scalar-FP XO->op map
against the authoritative decode table, offline.

Why this exists
---------------
The JIT (`src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`) dispatches ops with `switch (xo)`
where each `case N:` is hand-written and labelled by a `/* mnemonic ... */` comment. The
authoritative {mnemonic -> extended-opcode} mapping lives in the interpreter's decode table
(`src/kpx_cpu/src/cpu/ppc/ppc-decode.cpp`). When a case's XO doesn't match the decode table's
XO for the commented mnemonic, the JIT emits a DIFFERENT op's codegen for a real guest
instruction — a silent correctness bug.

Found on 2026-06-07 with this audit: THREE scrambled AltiVec families (sum-across vsum*,
whole-vector shifts vsl/vslo/vsro, FP-round/compare vrfin/vrfiz/vcmpgefp) — all dormant
(AltiVec is gated off) — AND one LIVE scalar-FP bug (mtfsf/mtfsfi code bodies swapped),
which affects every boot. All had zero test coverage.

What it catches  -- and what it DOESN'T
----------------------------------------
CATCHES: case-XO / comment-mnemonic mismatches (wrong op dispatched), and cases at XOs that
no real PPC op decodes to (dead code).

DOES NOT CATCH: **right-XO-wrong-codegen**. A case at the correct XO whose emitted NEON is
semantically wrong (e.g. the sum-across *saturation* bug — correct label, missing clamp; or
vrfin mapped to FRINTN instead of FRINTA — correct XO, wrong rounding) passes this audit
clean. "0 mismatches" is NOT a correctness certificate. Only differential tests
(`make test-jit`) with boundary operands prove codegen correctness. Match WHOLE mnemonics,
never substrings (`vsl` hides behind `vslw/vslh/vslb`).

Usage
-----
  python3 SheepShaver/tools/altivec-xo-audit.py          # AltiVec + FP; exit 1 on any mismatch
  python3 SheepShaver/tools/altivec-xo-audit.py --all     # also list dead-XO cases
  python3 SheepShaver/tools/altivec-xo-audit.py --domain fp   # fp | altivec | both (default both)
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "src"))
DECODE = os.path.join(SRC, "kpx_cpu", "src", "cpu", "ppc", "ppc-decode.cpp")
JIT = os.path.join(SRC, "kpx_cpu", "src", "cpu", "jit", "aarch64", "ppc-jit.cpp")


def decode_map(decode_path, forms, primaries):
    """Entry-bounded parse of ppc-decode.cpp: split into individual { "name", ... } records
    (avoids the cross-entry regex bug that mis-pulls neighbouring ops). Returns
    {(primary, xo): set(mnemonics)} for the requested form/primary filter. `forms` groups by
    a key so callers can separate e.g. A-form (5-bit XO) from X-form (10-bit XO)."""
    dec = open(decode_path).read()
    out = {}
    for blk in re.finditer(r'\{\s*"([a-z][a-z0-9_.]*)"\s*,(.*?)\}\s*,', dec, re.S):
        name, body = blk.group(1).rstrip('.'), blk.group(2)
        fm = re.search(r'\b(VX|VXR|VA|X|XFL|XFX|A)_form\s*,\s*(\d+)\s*,\s*(\d+)', body)
        if not fm:
            continue
        form, prim, xo = fm.group(1), int(fm.group(2)), int(fm.group(3))
        if form not in forms or prim not in primaries:
            continue
        out.setdefault((prim, xo), set()).add(name)
    return out


def jit_cases(jit_path, start_marker, end_marker, sub_switches):
    """(switch_label, xo, mnemonic) for cases in the JIT block [start_marker, end_marker),
    scoped per sub-switch (`switch (<var>)`), filtering to mnemonics matching `mnem_re`."""
    jit = open(jit_path).read()
    i = jit.find(start_marker)
    if i < 0:
        raise SystemExit("marker not found: " + start_marker)
    end = jit.find(end_marker, i + len(start_marker)) if end_marker else len(jit)
    region = jit[i:end if end > i else len(jit)]
    out = []
    for var, mnem_re in sub_switches:
        si = region.find("switch (%s)" % var)
        if si < 0:
            continue
        nxt = region.find("switch (", si + 6)
        body = region[si:nxt if nxt > 0 else len(region)]
        for cm in re.finditer(r'case\s+(\d+):(.*?)(?=case\s+\d+:|default:|\Z)', body, re.S):
            xo = int(cm.group(1))
            c = re.search(r'/\*\s*([a-z][a-z0-9_.]+)', cm.group(2))
            if c and re.match(mnem_re, c.group(1)):
                out.append((var, xo, c.group(1).rstrip('.')))
    return out


def audit(name, cases, table, allow_dead, show_all):
    scrambled, dead = [], []
    for var, xo, label in cases:
        names = table.get((None, xo)) or table.get(xo) or set()
        # callers pass tables keyed by (prim,xo); collapse here
        if names is None:
            names = set()
        if not names:
            dead.append((var, xo, label))
        elif label not in names:
            scrambled.append((var, xo, label, sorted(names)))
    print("== %s: %d cases, %d SCRAMBLED, %d dead-XO ==" % (name, len(cases), len(scrambled), len(dead)))
    for var, xo, label, names in scrambled:
        print("  [%s] case %-5d '%s' -> XO %d is really %s" % (var, xo, label, xo, "/".join(names)))
    if show_all and dead:
        for var, xo, label in dead:
            print("  [dead?] [%s] case %-5d '%s' (no op at THIS XO in this form-table; verify by hand"
                  " — may be a benign A-form op sitting in the X-form switch, or true dead code)"
                  % (var, xo, label))
    return len(scrambled)


def collapse(table_by_prim_xo, primaries):
    """{(prim,xo):names} -> {xo:names} merged across the given primaries (XO is unique per form-space)."""
    out = {}
    for (prim, xo), names in table_by_prim_xo.items():
        if prim in primaries:
            out.setdefault(xo, set()).update(names)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--decode", default=DECODE)
    ap.add_argument("--jit", default=JIT)
    ap.add_argument("--domain", choices=["altivec", "fp", "both"], default="both")
    ap.add_argument("--all", action="store_true", help="also list dead-XO cases")
    args = ap.parse_args()

    bad = 0
    if args.domain in ("altivec", "both"):
        vx = decode_map(args.decode, {"VX", "VXR", "VA"}, {4})
        cases = jit_cases(args.jit, "case 4: /* AltiVec via NEON */", "case 7: /* mulli",
                          [("vxo", r'v[a-z]'), ("vao", r'v[a-z]')])
        bad += audit("AltiVec (primary 4)", cases, collapse(vx, {4}), False, args.all)

    if args.domain in ("fp", "both"):
        # primary 63 (double) + 59 (single). xo10 sub-switch = X/XFL/XFX-form (10-bit),
        # xo5 sub-switch = A-form (5-bit). Keep them separate so a 5-bit XO isn't matched
        # against a 10-bit op table.
        xf = collapse(decode_map(args.decode, {"X", "XFL", "XFX"}, {59, 63}), {59, 63})
        af = collapse(decode_map(args.decode, {"A"}, {59, 63}), {59, 63})
        for prim, marker, endm in ((63, "case 63: /* double-precision FP ops */", "case 59: /* single"),
                                   (59, "case 59: /* single-precision FP ops */", "\n\tdefault:")):
            c10 = jit_cases(args.jit, marker, endm, [("xo10", r'[fm]')])
            c5 = jit_cases(args.jit, marker, endm, [("xo5", r'[fm]')])
            bad += audit("FP primary %d (X-form)" % prim, c10, xf, False, args.all)
            bad += audit("FP primary %d (A-form)" % prim, c5, af, False, args.all)

    print("\nNOTE: catches XO/label mismatches only. Right-XO-wrong-codegen is INVISIBLE here — "
          "prove codegen with `make test-jit` (boundary operands).")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
