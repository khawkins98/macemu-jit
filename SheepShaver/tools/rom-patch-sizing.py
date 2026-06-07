#!/usr/bin/env python3
"""rom-patch-sizing.py — size the PatchROM() effort for a new NewWorld ROM, offline.

Background
----------
SheepShaver's `PatchROM()` (src/rom_patches.cpp) applies ~80 byte-pattern patches via
`find_rom_data(start, end, PATTERN, len)`. Each pattern is calibrated to a SPECIFIC ROM
version. A different ROM (e.g. a newer "Mac OS ROM" file) decodes and type-detects fine but
PatchROM rejects it the moment the first pattern isn't found in its expected range — the
emulator then shows the misleading "Unsupported ROM type" alert (see
docs/planning/NEW-WORLD-ROM-SUPPORT-PLAN.md).

This tool answers "how much work is a new ROM?" WITHOUT booting: it extracts every
literal-range `find_rom_data` pattern from rom_patches.cpp and checks each against the
DECODED image, bucketing them:
  - in_range  : found within its expected [start,end) — works as-is
  - relocated : exists elsewhere in the image — range-fixable IF the patch's writes are
                base-relative (verify per-site; some write absolute offsets)
  - absent    : byte sequence not present at all — the code was rewritten between versions;
                needs real reverse-engineering to re-locate the equivalent site

Pass a known-good ROM's decoded image as the control (--control) to see which patterns the
target genuinely needs (patterns that are also absent in the working ROM belong to other
ROMType paths and don't apply).

Usage
-----
  # 1. decode the image(s) with rom-inspect (shares the emulator's exact decoder):
  cd SheepShaver/rom-inspect && make
  ./rom-inspect "/path/to/Mac OS ROM 9.0.1.rom" --dump /tmp/target.bin
  ./rom-inspect "/path/to/Mac OS ROM 1.1.rom"   --dump /tmp/control.bin   # known-good

  # 2. size it:
  python3 SheepShaver/tools/rom-patch-sizing.py /tmp/target.bin \
      --control /tmp/control.bin

A target whose patterns are mostly in_range/relocated (and 0 absent) is close to bootable;
many `absent` patterns mean deep, version-specific RE (NEW-WORLD-ROM-SUPPORT-PLAN.md Phase 2).
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SRC = os.path.normpath(os.path.join(HERE, "..", "src", "rom_patches.cpp"))


def parse_patterns(src_path):
    src = open(src_path).read()
    arrays = {}
    for m in re.finditer(r'static\s+const\s+uint8\s+(\w+)\s*\[\]\s*=\s*\{([^}]*)\}', src):
        bts = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]{2}', m.group(2))]
        if bts:
            arrays[m.group(1)] = bytes(bts)
    calls = []
    for m in re.finditer(
            r'find_rom_data\(\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(\w+)\s*,', src):
        calls.append((m.group(3), int(m.group(1), 16), int(m.group(2), 16)))
    return arrays, calls


def find_all(img, pat):
    offs, i = [], 0
    while True:
        j = img.find(pat, i)
        if j < 0:
            break
        offs.append(j)
        i = j + 1
    return offs


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("decoded", help="decoded 4MB target ROM image (rom-inspect --dump)")
    ap.add_argument("--control", help="decoded image of a known-good ROM (e.g. 1.1)")
    ap.add_argument("--src", default=DEFAULT_SRC, help="path to rom_patches.cpp")
    args = ap.parse_args()

    arrays, calls = parse_patterns(args.src)
    img = open(args.decoded, "rb").read()
    ctl = open(args.control, "rb").read() if args.control else None

    cats = {"in_range": 0, "relocated": 0, "absent": 0, "no_array": 0}
    rows = []
    for name, start, end in calls:
        pat = arrays.get(name)
        if not pat:
            cats["no_array"] += 1
            continue
        o = find_all(img, pat)
        if any(start <= x < end for x in o):
            cat = "in_range"
        elif o:
            cat = "relocated"
        else:
            cat = "absent"
        cats[cat] += 1
        ctlnote = ""
        if ctl is not None:
            oc = find_all(ctl, pat)
            ctlnote = ("ctl:ok" if any(start <= x < end for x in oc)
                       else "ctl:reloc" if oc else "ctl:absent")
        rows.append((cat, name, start, end, o[:3], ctlnote))

    print(f"{len(arrays)} pattern arrays, {len(calls)} literal-range find_rom_data calls")
    print(f"target: {args.decoded}" + (f"   control: {args.control}" if ctl else ""))
    print("\nSUMMARY:")
    for k in ("in_range", "relocated", "absent", "no_array"):
        print(f"  {k:10}: {cats[k]}")
    print("\nNON-in-range (the work), [control] flags non-applicable (ctl:absent) patterns:")
    for cat, name, start, end, o, ctlnote in rows:
        if cat == "in_range":
            continue
        os_ = ",".join(f"0x{x:x}" for x in o) or "none"
        print(f"  [{cat:9}] {name:24} [0x{start:x},0x{end:x}) @{os_}  {ctlnote}")
    # exit non-zero if any applicable pattern is absent (rough "needs RE" signal)
    applicable_absent = sum(1 for c, n, s, e, o, cn in rows
                            if c == "absent" and cn != "ctl:absent")
    print(f"\napplicable-absent (needs RE, excl. other-ROMType patterns): {applicable_absent}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
