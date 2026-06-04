# SheepShaver macOS arm64 — User Handbook

A guide to running SheepShaver on Apple Silicon Macs.

## Quick Start

1. **Get a ROM**: You need an OldWorld Power Mac ROM (e.g., `Mac OS ROM 1.1`).
   Place it somewhere accessible, e.g., `/Users/Shared/macemu/`.

2. **Get a disk image**: Create one with `dd` or use an existing `.dsk` file
   with Mac OS 8.1–9.x installed.

3. **Configure** `~/.sheepshaver_prefs`:
   ```
   # ROM and disk
   rom /Users/Shared/macemu/Mac OS ROM 1.1.rom
   disk /Users/Shared/macemu/macos86.dsk

   # Hardware
   ramsize 256M
   screen win/1024/768
   nosound false

   # Networking (outbound NAT via slirp)
   ether slirp

   # Boot from hard disk (not CD)
   bootdriver 0
   nocdrom true
   ```

4. **Run**: `./SheepShaver` from `src/Unix/`.

## Prefs Reference

### Syntax

One `key value` per line. Lines starting with `#` or `;` are comments.
Integer values accept `K`, `M`, `G` suffixes and `0x` hex prefix.

### Common Settings

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `rom` | path | — | Path to OldWorld PPC ROM file |
| `disk` | path | — | Path to disk image (repeat for multiple disks) |
| `ramsize` | int | 128M | Guest RAM size (e.g., `256M`) |
| `screen` | string | `win/640/480` | Display: `win/W/H` or `dga/W/H` |
| `nosound` | bool | false | Disable audio |
| `nocdrom` | bool | false | Disable CD-ROM |
| `bootdriver` | int | 0 | Boot device: `0` = hard disk, `-62` = CD-ROM |
| `ether` | string | — | Network: `slirp` for NAT, an interface name, or `vde:<dest>` for a VDE switch (see Networking) |
| `noextfs` | bool | false | Disable Unix filesystem mount |
| `vncserver` | bool | false | Enable VNC server for headless access |
| `jitcachesize` | int | 256M | JIT code cache size (virtual memory, no cost until used) |
| `vncport` | int | 5999 | VNC server port |

## Shutdown and Restart

- **Special > Shut Down**: Cleanly exits the SheepShaver process. All cleanup
  runs (JIT coverage report, SDL teardown). On exit, the JIT prints a session
  summary to stderr with coverage stats and top missed opcodes.
- **Special > Restart**: Not yet supported — has no effect (same as upstream).
  Requires deeper ROM reset state management.
- **Closing the window**: Also triggers a clean exit.
- **Force quit**: `pkill -9 -x SheepShaver` if the process is unresponsive.

## Networking

Add `ether slirp` to your prefs. In the guest Mac OS:

1. Open **TCP/IP** control panel (Apple menu > Control Panels > TCP/IP)
2. Set **Connect via**: Ethernet
3. Set **Configure**: Using DHCP Server
4. Close and save

Slirp provides outbound NAT — web browsing, FTP, etc. work. The guest gets
an IP in the 10.0.2.x range. DNS resolves through the host. No inbound
connections (no port forwarding from host to guest).

### VDE (virtual distributed Ethernet)

If SheepShaver was built with `--with-vdeplug` and the `vdeplug` library is
present (Homebrew: `brew install vde`; header `libvdeplug.h`), you can attach
the guest to a VDE switch instead of slirp. This gives full bidirectional
Ethernet (bridging, inbound connections, TAP devices), unlike slirp's NAT.

Two pref forms:

- `ether vde` — connect to the default VDE socket (legacy form, no destination).
- `ether vde:<dest>` — connect to a specific VDE endpoint, where `<dest>` is a
  vdeplug URL. The destination is stored in the pref, so it persists across runs.

Example — establish a remote TAP device over ssh:

```
sheepshaver --ether 'vde:cmd://ssh root@server vde_plug tap://tap0'
```

VDE support is compiled out (the `vde` / `vde:` ether prefs are ignored) if the
build did not find `libvdeplug`; the `configure` summary prints
`VDE support ...... : yes` when it is enabled.

## Performance

The JIT compiler translates PPC instructions to native ARM64 on the fly.
A register allocator caches PPC GPRs in ARM64 callee-saved registers
(x21-x28) within each compiled block, eliminating redundant memory access.

### Environment Variables

| Variable | Description |
|----------|-------------|
| `SS_USE_JIT=0` | Force interpreter mode (JIT is on by default) |
| `SS_JIT_VERIFY=1` | Differential verify: re-run every JIT block through interpreter and compare. Extremely slow. |
| `SS_JIT_NO_ROM=1` | Keep ROM interpreter-only, JIT only RAM |
| `SS_JIT_NO_CHAIN=1` | Disable block chaining |
| `SS_JIT_TRACE_RING=1` | Enable block-level execution history ring |
| `SS_JIT_CACHE_KB=N` | Override code cache size in KB (default 262144 = 256 MB) |
| `SS_JIT_DIAG_LOG=/path` | Override diagnostic log path |

### Benchmarking

**Quick A/B (no boot needed):**
```bash
cd SheepShaver/rom-harness
make bench BARGS=--save-baseline=/tmp/before.txt   # before a change
# ... make your codegen change, rebuild ...
make bench BARGS=--compare=/tmp/before.txt          # after — shows % deltas
```

**Full benchmark:** Use **Speedometer 4.02** inside the guest.
Key metrics: Benchmark Mix (integer ALU), Dhrystones, CPU score.

Baseline (2026-06-03, Mac OS 8.6 on Apple Silicon):
- Benchmark Mix: 634
- Dhrystones/sec: 1,475K
- CPU score: 64.2

## Building from Source

```bash
cd SheepShaver/src/Unix
NO_CONFIGURE=1 ./autogen.sh
./configure --enable-sdl-video --enable-sdl-audio --enable-jit \
            --without-gtk --without-x --without-esd --with-vdeplug
cd ../..
make build
```

`--with-vdeplug` (default yes) enables VDE networking; it needs the `vdeplug`
library (`brew install vde`). If `configure` reports `VDE support ...... : no`,
point it at the Homebrew prefix:
`CPPFLAGS=-I/opt/homebrew/include LDFLAGS=-L/opt/homebrew/lib`.

See `CLAUDE.md` in the repo root for full build commands, test harness
usage, and debugging workflow.

## Troubleshooting

**Black screen on boot**: Check ROM path in prefs. Only OldWorld ROMs work.
New World (CHRP) ROMs decode but fail downstream patch checks.

**No network**: Verify `ether slirp` in prefs and TCP/IP set to DHCP in guest.

**Slow performance**: Ensure JIT is active (no `SS_USE_JIT=0`). Check the
heartbeat in stderr — `jNK` and `jRAM` should show millions of blocks/s.

**Only one instance at a time**: SheepShaver instances share the prefs file,
disk images, and SDL window. Kill strays with `pkill -9 -x SheepShaver`.
