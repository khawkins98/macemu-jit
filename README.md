## macOS Apple Silicon (arm64)

This branch (`macos-arm64`) is a macOS Apple Silicon port of [rcarmo/macemu-jit](https://github.com/rcarmo/macemu-jit), adding an AArch64 JIT backend that translates PowerPC instructions to native ARM64 at runtime. **SheepShaver** boots Mac OS 8.x–9.x to the Finder desktop with the full native JIT on M-series Macs.

> **Scope:** SheepShaver (PowerPC) is the working macOS emulator. **BasiliskII** (68K) does *not* currently build on macOS arm64 — see `docs/planning/BasiliskII-MACOS-AARCH64-JIT-PORT.md`. A native macOS launcher, **Silicon Sheep** (Tauri), is in development — see [`SiliconSheep/`](SiliconSheep/).

### Prerequisites

Install build dependencies via Homebrew:

```bash
brew install autoconf automake sdl2 vde
```

You also need:
- An **OldWorld PPC Mac ROM** — Mac OS ROM 1.1 (1.8 MB). Not included; you must source this yourself.
- A **Mac OS 8.6/9.0.4 or similar CD image** or a pre-installed HFS disk image.

### Build

```bash
cd SheepShaver/src/Unix
NO_CONFIGURE=1 ./autogen.sh
./configure --enable-sdl-video --enable-sdl-audio --enable-jit \
            --without-gtk --without-x --without-esd --with-vdeplug \
            CPPFLAGS=-I/opt/homebrew/include LDFLAGS=-L/opt/homebrew/lib
cd ../..
make build
```

> `--with-vdeplug` enables VDE virtual networking (the built binary links `libvdeplug`); the
> Homebrew `CPPFLAGS`/`LDFLAGS` point `configure` at `/opt/homebrew`. `configure` defaults to
> SDL3 where available; pass `--with-sdl2` to pin SDL2.

### Preferences

SheepShaver reads `~/.sheepshaver_prefs` on startup. A minimal working configuration:

```
rom   /path/to/Mac OS ROM 1.1.rom
disk  /path/to/your-disk.dsk        # optional: pre-installed HFS disk image
cdrom /path/to/Mac OS.iso           # bootable installer CD
ramsize 268435456
screen win/800/600
nosound true
bootdriver -62
jit false
```

The `jit` pref only affects the legacy (compiled-out) codegen JIT and has no effect here. The AArch64 JIT is **on by default** — no env var needed; set `SS_USE_JIT=0` to force the interpreter.

### Run

```bash
cd SheepShaver/src/Unix
./SheepShaver                 # AArch64 JIT enabled by default
SS_USE_JIT=0 ./SheepShaver    # force interpreter
```

### Quick test

Verify the JIT opcode harness passes before running the full emulator:

```bash
cd SheepShaver && ./jit-test/run.sh
# Expected: score=100 (fail=0, pass==total).
# The vector count is not fixed — it drifts as vectors are added; query it with: make harness-count
```

### Known limitations

- Mac OS 9.2.1 "Internal Edition" requires a newer ROM (Mac OS ROM 9.0.1+); ROM 1.1 identifies as an older machine model and will not boot 9.2.1.
- Boot from CD ISO is slow (full SCSI scan on each boot); a pre-installed disk image is recommended for day-to-day use.

> The full native JIT now covers the **whole** ROM range including the 68K DR emulator
> (ROM=0x500000) and boots Mac OS 8.6 to the Finder desktop — see `JIT-STATUS.md`. (An earlier
> 0x460000 toolbox-only limitation was resolved 2026-06-03; the root cause was the `subfe`/`adde`
> carry-out fix — `docs/archive/`.)

---

## Provenance & lineage

This repo is the macOS Apple Silicon tip of a four-link fork chain — each link adds a layer:

```
cebix/macemu  →  kanjitalk755/macemu  →  rcarmo/macemu-jit  →  khawkins98/macemu-jit  (this repo)
 original          community upstream      ARM64 JIT (Linux/Pi)   macOS arm64 port
```

| Repo | What it adds | Activity¹ | Last commit¹ |
|------|--------------|-----------|--------------|
| [cebix/macemu](https://github.com/cebix/macemu) | The original macemu (Christian Bauer) — BasiliskII (68K) + SheepShaver (PPC) | 💤 Dormant — 0 commits in the past year | 2025-01-06 |
| [kanjitalk755/macemu](https://github.com/kanjitalk755/macemu) | De-facto community upstream; cross-pollinates with cebix | 🟢 Steady — ~30 commits/yr | 2026-05-20 |
| [rcarmo/macemu-jit](https://github.com/rcarmo/macemu-jit) | **Our direct parent** — the ARM64 JIT backend, originally for Linux ARM64 / Orange Pi / Raspberry Pi | 🔥 Very active — ~480 commits in 3 months (the JIT bring-up) | 2026-05-17 |
| **[khawkins98/macemu-jit](https://github.com/khawkins98/macemu-jit)** *(this repo)* | macOS Apple Silicon port of the JIT + the **Silicon Sheep** launcher | 🔥 Active — branch `macos-arm64` | 2026-06-05 |

¹ My reading of `git log` on each remote's default branch, observed 2026-06-05 — a snapshot, not a guarantee. Full backport analysis: [`docs/UPSTREAM-LINEAGE-SYNC.md`](docs/UPSTREAM-LINEAGE-SYNC.md).

**Other platforms.** For **Linux / Raspberry Pi / Docker** builds — and the broader ARM64-JIT history — use our direct parent **[rcarmo/macemu-jit](https://github.com/rcarmo/macemu-jit)**; those targets, their `.deb`/Docker packaging, and CI live upstream. This fork focuses on macOS arm64.

---

## Supported Platforms

#### BasiliskII
```
macOS     x86_64 JIT / arm64 does-not-build (port deferred)
Linux x86 x86_64 JIT
Linux arm64      JIT (boots Mac OS — upstream)
MinGW x86        JIT
```
#### SheepShaver
```
macOS     x86_64 JIT / arm64 JIT (boots Mac OS 8.6 to Finder)
Linux x86 x86_64 JIT / arm64 JIT (boots Mac OS)
MinGW x86        JIT
```
