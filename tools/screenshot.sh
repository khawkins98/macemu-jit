#!/bin/bash
# screenshot.sh — capture the SheepShaver guest display to a PNG without
# macOS Screen Recording permission.
#
# How it works: SheepShaver maps the guest framebuffer at a fixed host address
# (NATMEM_OFFSET + guest frame base). We attach lldb to the running process,
# dump the raw framebuffer bytes, and convert them to PNG.
#
# Usage:  ./tools/screenshot.sh [output.png]
#         ./tools/screenshot.sh                  → /tmp/sheepshaver_screen.png
#
# Requirements: lldb (Xcode CLT), python3. No special permissions needed
# beyond the ability to attach a debugger to your own processes.
#
# Frame geometry: read from the boot log when available, else defaults to
# 800x600x32bpp at guest 0x50590000 (the standard SheepShaver setup here).

set -euo pipefail

OUT="${1:-/tmp/sheepshaver_screen.png}"
WIDTH="${SS_FB_WIDTH:-800}"
HEIGHT="${SS_FB_HEIGHT:-600}"
# Guest frame base 0x50590000 + NATMEM_OFFSET 0x400000000000
FB_ADDR="${SS_FB_ADDR:-0x400050590000}"
BYTES=$((WIDTH * HEIGHT * 4))

PID=$(pgrep -x SheepShaver | head -1 || true)
if [ -z "$PID" ]; then
    echo "error: no running SheepShaver process found" >&2
    exit 1
fi

RAW=$(mktemp /tmp/ss_fb_XXXXXX.raw)
trap 'rm -f "$RAW"' EXIT

# Dump framebuffer via lldb (binary memory read). The attach briefly pauses
# the emulator (~100ms); it resumes on detach.
lldb -p "$PID" --batch \
    -o "memory read --binary --outfile $RAW --force $FB_ADDR ${FB_ADDR}+${BYTES}" \
    -o "detach" -o "quit" > /dev/null 2>&1

if [ ! -s "$RAW" ]; then
    echo "error: framebuffer dump failed (file empty)" >&2
    exit 1
fi

# Convert raw big-endian ARGB to PNG.
python3 - "$RAW" "$OUT" "$WIDTH" "$HEIGHT" << 'PYEOF'
import sys, struct, zlib

raw_path, out_path, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
data = open(raw_path, 'rb').read()
expected = w * h * 4
if len(data) < expected:
    sys.exit(f"error: dump too small ({len(data)} < {expected})")

# Mac framebuffer is big-endian ARGB (A,R,G,B byte order in memory).
# Build RGB rows for PNG.
def write_png(path, width, height, rgb_rows):
    def chunk(tag, payload):
        c = tag + payload
        return struct.pack('>I', len(payload)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    raw = b''.join(b'\x00' + row for row in rgb_rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', ihdr))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))

rows = []
for y in range(h):
    row = bytearray()
    base = y * w * 4
    for x in range(w):
        o = base + x * 4
        # big-endian ARGB: [A][R][G][B]
        row += data[o+1:o+4]
    rows.append(bytes(row))

write_png(out_path, w, h, rows)
print(f"wrote {out_path} ({w}x{h})")
PYEOF

echo "Screenshot saved: $OUT"
