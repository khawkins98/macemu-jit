# SheepShaver macOS arm64 — User Handbook

A guide to running SheepShaver on Apple Silicon Macs.

> **Prefer a GUI?** SiliconSheep (`SiliconSheep/` in the repo) is a Tauri-based
> launcher that wraps SheepShaver with a first-run wizard, VM library, settings
> editor, and a full **Inspector** (live JIT stats, register/memory viewer, guest
> window state, session recording/replay — like Chrome DevTools for the emulator).
> See `SiliconSheep/README.md` to get started. Everything below still applies if
> you prefer the command line or need to understand what SiliconSheep is doing
> under the hood.

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
| `altivec` | bool | false | **Advertise AltiVec (Velocity Engine) to the guest** so apps detect and use the vector unit. The JIT always compiles AltiVec→ARM64 NEON; this only registers the `'ppcf'` gestalt the guest OS otherwise lacks under the OldWorld ROM. **Opt-in** — see caveat below. |

> **`altivec` caveat.** The AArch64 JIT translates PowerPC AltiVec to ARM NEON regardless of this
> pref; `altivec true` only makes Mac OS *report* a vector unit so apps take their AltiVec path
> (verified: AltiVec Fractal Carbon then runs its vector kernel through the JIT). It's **off by
> default** because Mac OS 8.6/9.0 in SheepShaver's OldWorld environment don't save/restore the
> vector registers across task switches — safe for a single compute app, but advertising AltiVec
> system-wide risks vector-state corruption under heavy multitasked vector use. Enable it when you
> want a specific AltiVec app to use the vector unit. (Dev override: `SS_FORCE_ALTIVEC=1`/`=0`.)

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
| `SS_JIT_PROFILE=/path` | Write an execution profile on clean shutdown (hot blocks + `[JIT-COMPILED-MIX]` per-class op counts incl. `AltiVec=N`) |
| `SS_FORCE_ALTIVEC=1`/`=0` | Dev override for the `altivec` pref (force AltiVec advertisement on/off regardless of prefs) |
| `SS_DUMP_ROM=/path` | Dump the full decompressed ROM image to the given file at startup (after patching), for offline disassembly. Useful for NewWorld CHRP ROMs where the .rom file is compressed and does not match guest memory. |
| `SS_PROBE_PC=0xADDR[:fields][;…]` | No-recompile register/memory dump at block-entry PCs. Fields: `rN` (GPR), `[0xADDR]` (guest mem read), or omit for full dump. Logarithmic sampling. Up to 8 PCs, 16 fields each. See CLAUDE.md "PC Probes" for examples. |
| `SS_ROM_LENIENT=1` | **ROM porting tool.** Force lenient patch mode for any NewWorld ROM. Pattern misses in `PatchROM()` log `[ROMPATCH] SKIP <name>` to stderr and continue instead of hard-aborting. Essential for incremental bring-up of new ROM versions (parcels-format 9.x ROMs have drifted byte patterns). Auto-enabled for the 9.0.4 G4 ROM by checksum; this env var forces it for any NewWorld ROM. Only effective on `ROMTYPE_NEWWORLD`; no-op on OldWorld. See `DIAGNOSTICS.md` for the full log format. |
| `SS_ROM_PATCH_TRACE=1` | **ROM porting tool.** Log every `find_rom_data` pattern search to stderr as `[ROMPATCH] find_rom_data [...] -> HIT/RELOCATED/MISS`. Shows the full patch-search trace: which patterns matched in-range, which relocated via whole-image fallback (lenient mode), and which are absent. Zero cost when unset. Use with `SS_ROM_LENIENT=1` to map a new ROM's patch compatibility. |
| `SS_ROM_NO_904=1` | Opt out of the auto-detected 9.0.4 lenient mode (checksum-based). Does not affect `SS_ROM_LENIENT=1` (the manual override). |

### RPC Guest Memory Inspection

A running SheepShaver exposes a C2.0 RPC server at `/tmp/sheepshaver-<pid>` (see `rpc.h`).
Two methods allow live guest memory inspection without lldb:

| Method (id) | Args | Reply |
|---|---|---|
| `mem_search` (23) | `(uint32 value, uint32 start, uint32 end)` — start/end=0 defaults to full RAM | `{"matches":["0x..."],"count":N,"truncated":bool}` (max 1000 hits) |
| `mem_read_json` (24) | `(uint32 addr, uint32 count)` — count=0 defaults to 64, capped at 4096 | `{"addr":"0x...","hex":"word0 word1 ..."}` (4-byte aligned, big-endian) |

Both methods check address bounds (RAM, ROM, kernel data pages) before reading.
Unmapped addresses in `mem_read_json` return `0xDEADC0DE`. These are consumed
programmatically over the Unix socket (SiliconSheep Inspector, custom scripts);
there is no standalone CLI wrapper.

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

**Automated end-to-end testing (the E2E harness).** `make e2e` (boot → clean shutdown smoke) and
`make e2e-bench` (boots Mac OS 9 + Speedometer, drives the full suite over VNC, captures the result
image) run the emulator as a *system* and return a clean pass/fail — the regression gate for "does my
change still boot/run/shut-down cleanly?" Full guide, signals, knobs, and troubleshooting:
**`SheepShaver/e2e/README.md`**.

`make e2e-bench` also archives **benchmark history**: it saves Speedometer's text report in-guest,
extracts it host-side via `hfsutils` (`brew install hfsutils`), and writes each run to the gitignored
`SheepShaver/e2e/artifacts/benchmark-history/` (per-run `report.txt`/`scores.csv` + an append-only
`history.csv` trend table), printing the PR/CPU/… delta vs the previous run. It's collect-and-report
only — the numbers never fail the run.

### Diagnosing a "?" boot disk

A flashing **`?` floppy** means classic Mac OS found no bootable System — almost always a **stray
SheepShaver instance still holding the disk image** (only one instance can use it at a time):

```bash
pgrep -x SheepShaver        # is one already running?
pkill -9 -x SheepShaver     # clear it, then relaunch
```

If `pgrep` shows nothing but boots still `?`-hang after many launches in one session, the macOS
graphics/VBL timer has degraded — **log out/in or restart** to clear it. (The E2E harness now
pre-flights this: it kills + *reaps* strays and refuses to launch if the disk image is held.)

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

### Video backend (SDL3 default, SDL2 opt-out)

This fork **defaults to SDL3** for video. `configure` (with no `--with-sdlN` flag)
selects SDL 3.x, so the command above builds against SDL3 — no extra flag needed.

- **Dependency:** the `sdl3` pkg-config module. On macOS: `brew install sdl3`
  (tested with 3.4.10). SDL2 (`brew install sdl2`) is still required if you opt out.
- **Opt back to SDL2:** add `--with-sdl2` to the `configure` line:

  ```bash
  ./configure --enable-sdl-video --enable-sdl-audio --enable-jit \
              --without-gtk --without-x --without-esd --with-sdl2
  ```

> **SDL3 is boot-verified.** The SDL3 backend links against `libSDL3.0.dylib`, and
> Mac OS 8.6 boots to the Finder desktop on it (verified 2026-06-04). If you see display
> problems after a fresh build, fall back to SDL2 with `--with-sdl2` and please report the
> SDL3 behavior.

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
