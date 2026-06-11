#!/usr/bin/env python3
"""m68k-dis.py — m68k recon disassembler with line-trap fixups.

Thin wrapper over capstone CS_ARCH_M68K for scanning ROM dump regions. Raw capstone
mis-decodes A-line ($Axxx) and F-line ($Fxxx) trap words at instruction boundaries
(e.g. $FE1F gets eaten as the start of a multi-word fsmove). This script checks the
word at each instruction boundary FIRST and emits trap words as `dc.w $XXXX` data,
only handing the rest to capstone. Words inside instructions (e.g. the $AA7F
immediate in `move.w #$aa7f,d0`) are untouched — the split happens at boundaries
only. $0FFF words (PPC twi-placeholder style, seen in mixed 68k/PPC regions) get
the same treatment.

This is a recon aid, not a disassembler — sequential scan, no flow analysis.

Usage:
  tools/m68k-dis.py --file /tmp/rom901_inventory.bin --start 0xf240 --end 0xf260
  tools/m68k-dis.py --file DUMP --start 0xOFF --end 0xOFF [--base 0x50000000]

Offsets are file offsets; --base (default 0x50000000 = ROMBase) sets the printed
guest addresses (guest = base + file offset).
"""

import argparse
import sys

try:
    import capstone
except ImportError:
    sys.exit("capstone required: python3 -m pip install capstone")


def trap_label(word):
    """Return a data-word label if `word` must not be fed to capstone, else None."""
    hi = word & 0xF000
    if hi == 0xA000:
        return "A-line trap"
    if hi == 0xF000:
        return "F-line trap"
    if word == 0x0FFF:
        return "PPC twi-placeholder-style word"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--file", required=True, help="dump file to read")
    ap.add_argument("--start", required=True, help="start file offset (hex OK)")
    ap.add_argument("--end", required=True, help="end file offset, exclusive (hex OK)")
    ap.add_argument("--base", default="0x50000000",
                    help="guest base address for the printed column (default ROMBase)")
    args = ap.parse_args()

    start = int(args.start, 0)
    end = int(args.end, 0)
    base = int(args.base, 0)
    if start % 2 or end % 2:
        sys.exit("offsets must be even (m68k instructions are word-aligned)")
    if end <= start:
        sys.exit("--end must be greater than --start")

    with open(args.file, "rb") as f:
        f.seek(start)
        code = f.read(end - start)
    if len(code) < end - start:
        sys.exit(f"file too short: wanted {end - start:#x} bytes at {start:#x}, "
                 f"got {len(code):#x}")

    md = capstone.Cs(capstone.CS_ARCH_M68K,
                     capstone.CS_MODE_M68K_040 | capstone.CS_MODE_BIG_ENDIAN)

    off = 0  # offset within `code`
    while off < len(code) - 1:
        addr = base + start + off
        word = (code[off] << 8) | code[off + 1]

        label = trap_label(word)
        if label is not None:
            print(f"{addr:#010x}  {word >> 8:02x}{word & 0xff:02x}            "
                  f"dc.w ${word:04X}  ; {label}")
            off += 2
            continue

        insn = next(md.disasm(code[off:off + 10], addr, count=1), None)
        if insn is None:
            print(f"{addr:#010x}  {word >> 8:02x}{word & 0xff:02x}            "
                  f"dc.w ${word:04X}  ; (undecodable)")
            off += 2
            continue

        hexbytes = insn.bytes.hex()
        print(f"{addr:#010x}  {hexbytes:<14}  {insn.mnemonic} {insn.op_str}".rstrip())
        off += insn.size


if __name__ == "__main__":
    main()
