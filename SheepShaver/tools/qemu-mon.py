#!/usr/bin/env python3
"""
qemu-mon.py — QEMU monitor client with ANSI-escape stripping.

Usage:
    python3 qemu-mon.py [--sock PATH] 'COMMAND'
    python3 qemu-mon.py [--sock PATH] --disasm 0xADDR [--count N]
    python3 qemu-mon.py [--sock PATH] --read-bytes 0xADDR --count N

Defaults:
    --sock  /tmp/qemu-rig/mon.sock

Examples:
    # Read level-1 interrupt vector
    python3 qemu-mon.py "x /2wx 0x64"

    # Read 68k low-memory range
    python3 qemu-mon.py "x /8wx 0x0"

    # Disassemble 64 bytes of the level-1 interrupt handler
    python3 qemu-mon.py --disasm 0x47d0ba --count 64

    # Read Ticks (confirm system is running)
    python3 qemu-mon.py "x /2wx 0x168"

    # Screenshot
    python3 qemu-mon.py "screendump /tmp/shot.ppm"

Architecture notes (QEMU mac99, 2026-06-12 rig session):
    Virtual memory is the right access path — the NK enables the MMU.
    `xp` reads physical RAM (mostly zeros after MMU setup); `x` reads
    the current virtual address space and reaches live Mac OS data.

    Key addresses at Mac OS 9.2.1 Finder steady state:
        0x64      Level-1 interrupt vector — read this first with `x /1wx 0x64`,
                  then --disasm the result. The value (e.g. 0x0047d0ba) is heap-
                  allocated by the Mac OS System file and varies per boot. Do NOT
                  hardcode 0x0047d0ba — it is from one specific session.
        0x168     Ticks (32-bit, updating at 60 Hz)
        0x0d94    Flag tested by ROM handler at 0x5000ee98 (= 0 at Finder; see
                  VIA-IFR-RECON.md §3 for the corrected dispatch description)
        0x06e4    $6e4 vector chain pointer (= 0x00493dfe at Finder in one session;
                  heap-allocated, varies per boot)
"""

import socket
import time
import re
import sys
import struct
import argparse

DEFAULT_SOCK = "/tmp/qemu-rig/mon.sock"
WAIT_SECS    = 1.5   # seconds to wait for monitor output after sending a command


def connect(sock_path: str) -> socket.socket:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.settimeout(3)
    time.sleep(0.3)
    try:
        s.recv(65536)   # discard welcome banner
    except OSError:
        pass
    return s


def run_cmd(s: socket.socket, cmd: str, wait: float = WAIT_SECS) -> str:
    """Send one monitor command, return cleaned output (no echo, no prompt)."""
    s.sendall((cmd + "\n").encode())
    # Read until the (qemu) prompt appears, with a generous overall timeout.
    deadline = time.time() + max(wait, 4.0)
    raw = b""
    s.settimeout(0.3)
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
            if chunk:
                raw += chunk
                if b"(qemu)" in raw and len(raw) > 20:
                    break
        except OSError:
            if b"(qemu)" in raw:
                break
    text = raw.decode(errors="replace")
    # Strip ANSI / VT100 escape sequences (terminal echo from monitor)
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text)
    text = re.sub(r"\x1b.", "", text)
    # The monitor echoes each keystroke on a single line before the response.
    # Drop everything up to and including the first \n (which ends the echo line).
    if "\n" in text:
        text = text[text.index("\n") + 1:]
    # Drop QEMU prompt lines
    lines = [l for l in text.splitlines() if l.strip() and "(qemu)" not in l]
    return "\n".join(lines)


def parse_hex_dump(raw: str) -> bytearray:
    """Parse lines like '0047d0ba: 0x0c 0x6f ...' into a bytearray."""
    data = bytearray()
    for line in raw.splitlines():
        m = re.match(r"^[0-9a-f]{4,8}:\s+(.+)", line.strip())
        if m:
            for tok in m.group(1).split():
                tok = tok.strip().rstrip(",")
                if re.match(r"^0x[0-9a-f]{1,2}$", tok):
                    data.append(int(tok, 16))
    return data


def read_virtual_bytes(s: socket.socket, addr: int, count: int) -> bytes:
    """Read `count` bytes from guest virtual address `addr`."""
    raw = run_cmd(s, f"x /{count}bx 0x{addr:x}", wait=2.5)
    data = parse_hex_dump(raw)
    return bytes(data)


def disasm_68k(data: bytes, base: int):
    """Disassemble bytes as 68k, printing to stdout."""
    try:
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_M68K_040)
        for i in md.disasm(data, base):
            print(f"  {i.address:#010x}  {i.bytes.hex():<12}  {i.mnemonic:<12} {i.op_str}")
    except ImportError:
        print("(capstone not installed; raw bytes follow)")
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            hex_s = " ".join(f"{b:02x}" for b in chunk)
            print(f"  {base+i:#010x}  {hex_s}")


def main():
    ap = argparse.ArgumentParser(
        description="QEMU monitor client with ANSI stripping",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Usage:")[0].strip(),
    )
    ap.add_argument("--sock", default=DEFAULT_SOCK, help="Monitor socket path")
    ap.add_argument("--disasm", metavar="ADDR", help="Disassemble bytes at virtual address")
    ap.add_argument("--read-bytes", metavar="ADDR", help="Hex-dump bytes at virtual address")
    ap.add_argument("--count", type=int, default=128, help="Bytes to disassemble/dump (default 128)")
    ap.add_argument("command", nargs="?", help="Raw monitor command")
    args = ap.parse_args()

    try:
        s = connect(args.sock)
    except OSError as e:
        print(f"ERROR: cannot connect to {args.sock}: {e}", file=sys.stderr)
        sys.exit(1)

    try:
        if args.command:
            print(run_cmd(s, args.command))
        elif args.disasm:
            addr = int(args.disasm, 16)
            data = read_virtual_bytes(s, addr, args.count)
            print(f"Disassembly of {args.count} bytes at {addr:#x} (parsed {len(data)} bytes):")
            disasm_68k(data, addr)
        elif args.read_bytes:
            addr = int(args.read_bytes, 16)
            data = read_virtual_bytes(s, addr, args.count)
            print(f"Bytes at {addr:#x} ({len(data)} bytes):")
            for i in range(0, len(data), 16):
                chunk = data[i:i+16]
                hex_s = " ".join(f"{b:02x}" for b in chunk)
                print(f"  {addr+i:#010x}  {hex_s}")
        else:
            ap.print_help()
    finally:
        s.close()


if __name__ == "__main__":
    main()
