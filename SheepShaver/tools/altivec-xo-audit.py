#!/usr/bin/env python3
"""altivec-xo-audit.py — cross-check the AArch64 JIT's AltiVec/FP XO->op map against the
authoritative decode table, offline.

Why this exists
---------------
The JIT (`src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`) dispatches AltiVec ops with
`switch (vxo)` / `switch (vao)` where each `case N:` is hand-written and labelled by a
`/* mnemonic ... */` comment. The authoritative {mnemonic -> extended-opcode} mapping lives
in the interpreter's decode table (`src/kpx_cpu/src/cpu/ppc/ppc-decode.cpp`). When a case's
XO doesn't match the decode table's XO for the commented mnemonic, the JIT emits a DIFFERENT
op's codegen for a real guest instruction — a silent correctness bug.

Two such scrambled families were found and fixed on 2026-06-07 (sum-across vsum*, and the
whole-vector shifts vsl/vslo/vsro + the FP round/compare ops vrfin/vrfiz/vcmpgefp), each of
which had ZERO test coverage. This tool makes that audit repeatable.

What it catches  -- and what it DOESN'T
----------------------------------------
CATCHES: case-XO / comment-mnemonic mismatches (wrong op dispatched for an XO), and cases at
XOs that no real PPC op decodes to (dead code).

DOES NOT CATCH: **right-XO-wrong-codegen**. A case at the correct XO whose emitted NEON is
semantically wrong (e.g. the sum-across *saturation* bug — correct label, missing clamp; or a
swapped-operand multiply) passes this audit clean. "0 mismatches" is NOT a correctness
certificate. Only differential tests (`make test-jit`) with boundary operands prove codegen
correctness. Match on WHOLE mnemonics, never substrings (`vsl` hides behind `vslw/vslh/vslb`).

Usage
-----
  python3 SheepShaver/tools/altivec-xo-audit.py
  # exit 0 if no mismatches, 1 if any (CI-friendly). --all also lists dead-XO cases.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "src"))
DECODE = os.path.join(SRC, "kpx_cpu", "src", "cpu", "ppc", "ppc-decode.cpp")
JIT = os.path.join(SRC, "kpx_cpu", "src", "cpu", "jit", "aarch64", "ppc-jit.cpp")


def authoritative_map(decode_path):
    """mnemonic<->XO for VX/VXR/VA-form ops under primary opcode 4 (AltiVec)."""
    dec = open(decode_path).read()
    xo2names, name2xo = {}, {}
    for m in re.finditer(
            r'\{\s*"([a-z][a-z0-9_.]*)"\s*,.*?\b(VX|VXR|VA)_form\s*,\s*4\s*,\s*(\d+)\s*,',
            dec, re.S):
        nm, xo = m.group(1), int(m.group(3))
        xo2names.setdefault(xo, set()).add(nm)
        name2xo.setdefault(nm, set()).add(xo)
    return xo2names, name2xo


def jit_altivec_cases(jit_path):
    """(xo, label) for every `case N:` in the JIT 'case 4: /* AltiVec' block whose comment
    names a vector mnemonic. Covers both the vxo and vao sub-switches (vao XOs are the low 6
    bits and live in the same VA-form numbering as decode's VA_form XOs)."""
    jit = open(jit_path).read()
    i = jit.find("case 4: /* AltiVec via NEON */")
    if i < 0:
        raise SystemExit("could not find AltiVec block in " + jit_path)
    region = jit[i:]
    j = region.find("case 7: /* mulli")          # AltiVec block ends before mulli
    if j > 0:
        region = region[:j]
    out = []
    for cm in re.finditer(r'case\s+(\d+):(.*?)(?=case\s+\d+:|\Z)', region, re.S):
        xo, body = int(cm.group(1)), cm.group(2)
        c = re.search(r'/\*\s*([a-z][a-z0-9_.]+)', body)
        if not c:
            continue
        label = c.group(1).rstrip('.')
        if re.match(r'v[a-z]', label):           # vector mnemonic only
            out.append((xo, label))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--decode", default=DECODE)
    ap.add_argument("--jit", default=JIT)
    ap.add_argument("--all", action="store_true", help="also list dead-XO cases")
    args = ap.parse_args()

    xo2names, name2xo = authoritative_map(args.decode)
    cases = jit_altivec_cases(args.jit)

    scrambled, dead = [], []
    for xo, label in cases:
        names = {n.rstrip('.') for n in xo2names.get(xo, set())}
        if not names:
            dead.append((xo, label))
        elif label not in names:
            real_xo = sorted(name2xo.get(label, []))
            scrambled.append((xo, label, sorted(xo2names[xo]), real_xo))

    print("checked %d JIT vector cases against %s" % (len(cases), os.path.basename(args.decode)))
    print("SCRAMBLED (wrong op dispatched for this XO): %d" % len(scrambled))
    for xo, label, atxo, realxo in scrambled:
        print("  case %-5d labeled '%s' -> XO %d is really %s (real '%s' is XO %s)"
              % (xo, label, xo, "/".join(atxo), label, realxo))
    if args.all:
        print("DEAD-XO cases (no real PPC op; real op falls back to interp): %d" % len(dead))
        for xo, label in dead:
            print("  case %-5d labeled '%s' (real '%s' XO %s)"
                  % (xo, label, label, sorted(name2xo.get(label, []))))
    print("\nNOTE: this audit catches XO/label mismatches only. Right-XO-wrong-codegen is "
          "INVISIBLE here — prove codegen with `make test-jit` (boundary operands).")
    return 1 if scrambled else 0


if __name__ == "__main__":
    sys.exit(main())
