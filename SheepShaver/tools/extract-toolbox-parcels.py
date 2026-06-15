#!/usr/bin/env python3
"""extract-toolbox-parcels.py - Operation NewSheep (SS_M18 Track A / faithful staging)

Extract the COMPRESSED 'prcl' (toolbox-parcels) container from a NewWorld CHRP
Mac OS ROM file, WITHOUT decompressing it.

WHY (the faithful-staging finding, 2026-06-15):
  The real NewWorld Trampoline's `AAPL,toolbox-parcels` reader UNCONDITIONALLY
  decompresses the parcels image it is handed (each 'rom ' parcel is LZSS, like
  SheepShaver's own decode_parcels in src/include/rom_decode.hpp). SheepShaver
  decompresses the .rom into its ROM aperture at 0x50000000; if BootX-staging
  hands the Trampoline THOSE already-decompressed bytes, the Trampoline
  decompresses them a SECOND time -> computes garbage into the KernelCode gap
  (watchpoint-pinned at guest pc=0x0159c37c writing 0x3882009f, which exists in
  no static source). Staging the COMPRESSED container instead lets the real
  decompressor produce a correct NanoKernel image, and the NK body runs.

WHAT:
  A NewWorld .rom is a <CHRP-BOOT> text container with a Forth boot script that
  declares `<NNNNNN> constant parcels-offset` / `<NNNNNN> constant parcels-size`.
  The bytes at [parcels-offset, parcels-offset+parcels-size) begin with the
  magic 'prcl' and ARE the compressed toolbox-parcels container. This tool slices
  that range out verbatim - the artifact to stage via SS_M18_PARCEL_FILE.

USAGE:
  extract-toolbox-parcels.py <rom-file> [-o out.prcl]
"""
import argparse, re, sys, struct, hashlib

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("-o", "--out", default=None)
    args = ap.parse_args()

    data = open(args.rom, "rb").read()
    if data[:11] != b"<CHRP-BOOT>":
        sys.exit("not a <CHRP-BOOT> container (raw/other ROM format not supported)")

    # The script declares e.g. "00abcd constant parcels-offset". The hex (6 digits)
    # sits 7 bytes before the "constant <name>" token, matching rom_decode.hpp.
    def find_const(name):
        m = re.search((r"([0-9a-fA-F]{6}) constant " + re.escape(name)).encode(), data)
        if not m:
            return None
        return int(m.group(1), 16)

    off = find_const("parcels-offset")
    siz = find_const("parcels-size")
    if off is None or siz is None:
        # Plain-LZSS NewWorld ROMs declare lzss-offset/size instead; not a prcl
        # container -> the faithful artifact would be the raw LZSS payload.
        sys.exit("no parcels-offset/parcels-size constants found "
                 "(this is not a 'prcl'-container ROM)")

    if off + siz > len(data):
        sys.exit(f"parcels range [{off:#x},{off+siz:#x}) exceeds file size {len(data):#x}")

    blob = data[off:off+siz]
    magic = blob[:4]
    if magic != b"prcl":
        sys.exit(f"payload magic is {magic!r}, expected b'prcl' "
                 f"(offset/size parse may be wrong)")

    # Report the 'rom ' parcel inside (the LZSS MacROM the NK lives in).
    rom_parcel = None
    p = 0x14
    while 0 < p < len(blob) - 12:
        nxt = struct.unpack_from(">I", blob, p)[0]
        ptype = blob[p+4:p+8]
        if ptype == b"rom ":
            lz = struct.unpack_from(">I", blob, p+8)[0]
            start = p + lz
            end = nxt if nxt else len(blob)
            rom_parcel = (start, end - start)
        if nxt <= p:
            break
        p = nxt

    out = args.out or (args.rom.rsplit(".", 1)[0] + ".toolbox-parcels.prcl")
    open(out, "wb").write(blob)
    md5 = hashlib.md5(blob).hexdigest()
    print(f"extracted compressed 'prcl' container:")
    print(f"  source ROM        : {args.rom}")
    print(f"  parcels-offset    : {off:#x}")
    print(f"  parcels-size      : {siz:#x} ({siz} bytes)")
    if rom_parcel:
        print(f"  'rom ' parcel LZSS: offset {rom_parcel[0]:#x} size {rom_parcel[1]:#x} "
              f"(decompresses to the 4MB MacROM w/ ConfigInfo+NanoKernel)")
    print(f"  output            : {out}")
    print(f"  output md5        : {md5}")
    print()
    print(f"Stage it with:  SS_M18_PARCEL_FILE={out}")

if __name__ == "__main__":
    main()
