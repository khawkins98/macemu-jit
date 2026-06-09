# macemu-jit — SheepShaver for Apple Silicon

## Why does this exist?

I remember being a teenager and thinking SheepShaver was pretty neat. I kept waiting for somebody to crack New World ROM support so I could run Mac OS 9.2 out of the box — do all the really cool things that other emulators over the years have brought to their platforms. But classic macOS was an evolutionary dead end. Things switched to OS X, and the old world moved on. Nobody was ever going to finish the job.

After waiting a quarter of a century, it became obvious that nobody else was going to get New World ROMs running just-in-time on Apple Silicon either. So I thought maybe I would.

I have some software development background, but I don't have the domain expertise, and I frankly don't have the software chops to pull something like this off on my own — not without a four-year grant to go learn it properly. What I do have is curiosity, some stubbornness, and access to an AI coding partner that turned out to be a surprisingly good collaborator on gnarly low-level problems. It's been an interesting learning experience. I think what came out of it is pretty neat.

## A note on AI

Yes, a lot of AI was used to write this code. I wouldn't call it vibe coding — there's been real discipline applied, real creativity, and real decisions made along the way. It's been a genuine partnership with the agent, not just prompting and hoping.

I appreciate that plenty of people won't think much of that, and will consider this project complete trash because of it. They're welcome to that opinion. We don't have to agree. This is my hobby, and this is how I went about it. I'm sharing it back in case anyone finds it useful — to run it, fork it, pick it apart, or just take whatever bits help them. It's open source. Do what you like with it.

---

## macOS Apple Silicon (arm64)

This branch (`macos-arm64`) is a macOS Apple Silicon port of [rcarmo/macemu-jit](https://github.com/rcarmo/macemu-jit), adding an AArch64 JIT backend that translates PowerPC instructions to native ARM64 at runtime. **SheepShaver** boots Mac OS 8.x–9.x to the Finder desktop with the full native JIT on M-series Macs.

> **Scope:** SheepShaver (PowerPC) is the working macOS emulator. **BasiliskII** (68K) does *not* currently build on macOS arm64 — see `docs/planning/BasiliskII-MACOS-AARCH64-JIT-PORT.md`. A native macOS launcher, **Silicon Sheep** (Tauri), is in development — see [`SiliconSheep/`](SiliconSheep/).
> **External targets under investigation:** We are also tracking [mihaip/infinite-mac](https://github.com/mihaip/infinite-mac), [dingusdev/dingusppc](https://github.com/dingusdev/dingusppc), and [twvd/snow](https://github.com/twvd/snow) as emulators we target for comparative research. We have not integrated work from these projects yet, but they use similar GPL-family licenses and may contain ideas we can incorporate after targeted evaluation.

### Project direction

SheepShaver was built for a resource-constrained era; an M-series Mac is not. That headroom lets us
**widen what we emulate** — model more of the complete PowerPC Mac stack, more faithfully — rather than
only making the existing slice faster. The work follows a deliberate lifecycle, each stage resting on
the previous: **get it running → make it drivable/testable → make it measurable → broaden what it runs
→ then make it fast.**

1. **Run** ✅ — a native AArch64 JIT (PowerPC → ARM64); SheepShaver boots Mac OS 8.6/9 to Finder on
   Apple Silicon.
2. **Drive & test** ✅ — tools to *control and validate* the guest: a differential opcode harness
   (interp-vs-JIT, score=100), an end-to-end boot/workload harness with clean-shutdown signalling, and
   read-only guest-UI introspection + VNC drive. The correctness safety net.
3. **Measure** ✅ — empirical, regression-tracked performance: Speedometer/MacBench capture, a boot-free
   kernel microbench (`a64/op`), and a per-block + instruction-mix profiler. Makes every later change
   quantifiable.
4. **Widen emulation** 🔜 *(current primary thrust)* — close the structural gaps SheepShaver never could,
   correctness first, measured continuously against stage 3. **First win: preliminary AltiVec (Velocity
   Engine) support** — the JIT translates PowerPC AltiVec → ARM64 NEON, and with the opt-in `altivec`
   pref a real app (AltiVec Fractal Carbon) now detects and runs its vector kernel through the JIT (see
   below). Next: broader OS/software reach (New World ROM → Mac OS 9.1/9.2, fuller device/OS modeling).
   [DingusPPC](https://github.com/dingusdev/dingusppc) is our reference for fuller PPC-Mac-stack modeling.
5. **Optimize** — *then* push performance (per-block overhead, cross-block pinning, a vector register
   allocator, HLE), with the stage-3 benchmarks gating every change against regressions.

Running alongside all of this, **Silicon Sheep** (the Tauri desktop app) improves the day-to-day
usability experience. The full tactical backlog lives in [`docs/planning/ROADMAP.md`](docs/planning/ROADMAP.md).

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

### AltiVec (Velocity Engine) — preliminary support

The AArch64 JIT **does** translate PowerPC AltiVec vector instructions to native ARM64 NEON (it's
JIT-compiled, not interpreted) — extensively hardened against an interpreter oracle (`make test-jit`).
What was historically missing wasn't the codegen but *detection*: under SheepShaver's OldWorld ROM the
guest OS never advertises a vector unit, so apps fell back to scalar code.

Set **`altivec true`** in your prefs to advertise AltiVec to the guest. With it, AltiVec Fractal Carbon
detects the Velocity Engine and runs its vector kernel through the JIT (verified via the profiler:
`SS_JIT_PROFILE` → `[JIT-COMPILED-MIX] AltiVec=N`, with AltiVec blocks in the hot path).

It's **opt-in / preliminary**: Mac OS 8.6/9.0 here don't save/restore vector registers across task
switches, so it's safe for a focused compute app but not yet general-purpose multitasking vector use;
and `VSCR[SAT]` is not modeled. Making it fully safe (model VR context save/restore) is on the roadmap
(§B5). See [`SheepShaver/docs/USER-HANDBOOK.md`](SheepShaver/docs/USER-HANDBOOK.md) for the caveat.

### Known limitations

- Mac OS 9.2.1 "Internal Edition" requires a newer ROM (Mac OS ROM 9.0.1+); ROM 1.1 identifies as an older machine model and will not boot 9.2.1.
- Boot from CD ISO is slow (full SCSI scan on each boot); a pre-installed disk image is recommended for day-to-day use.

> The full native JIT now covers the **whole** ROM range including the 68K DR emulator
> (ROM=0x500000) and boots Mac OS 8.6 to the Finder desktop — see `JIT-STATUS.md`. (An earlier
> 0x460000 toolbox-only limitation was resolved 2026-06-03; the root cause was the `subfe`/`adde`
> carry-out fix — `docs/archive/`.)

---

## Documentation

This is the **tracked map** of the project's docs (for both humans and AI agents). _A local `CLAUDE.md`,
if present, is a fuller working index — but it is gitignored, so the canonical map is here._

**Start here** (orientation for contributors & agents):
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — how to work in this repo: commit style, the documentation lifecycle, attribution.
- [`LEARNINGS.md`](LEARNINGS.md) — non-obvious findings; read these at the start of a session.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — JIT structure (both emulators) + the macOS arm64 constraints.
- [`docs/planning/ROADMAP.md`](docs/planning/ROADMAP.md) — outstanding work, arranged + tracked ("what's next").

**Status & history:** [`JIT-STATUS.md`](JIT-STATUS.md) (pass/fail + boot status) · [`CHANGELOG.md`](CHANGELOG.md) (what changed, by date/component) · [`docs/UPSTREAM-LINEAGE-SYNC.md`](docs/UPSTREAM-LINEAGE-SYNC.md) (fork lineage).

**JIT internals:** [`docs/planning/SheepShaver-AARCH64_JIT_PLAN.md`](docs/planning/SheepShaver-AARCH64_JIT_PLAN.md) (PPC→ARM64) · [`BasiliskII/docs/AARCH64_JIT_BRINGUP.md`](BasiliskII/docs/AARCH64_JIT_BRINGUP.md) (68K→ARM64 + bug history) · [`docs/planning/OPTIMIZATION-PLAN.md`](docs/planning/OPTIMIZATION-PLAN.md).

**E2E testing & guest automation:**
- [`SheepShaver/e2e/README.md`](SheepShaver/e2e/README.md) — the E2E harness: smoke / Speedometer benchmark / real-app workload gates, with the toolkit map.
- [`SheepShaver/e2e/AGENT-API.md`](SheepShaver/e2e/AGENT-API.md) — **drive the classic-Mac guest from code** (the agent surface: `sse2e.uidump` + `vnc`).
- [`SheepShaver/docs/UI-INTROSPECTION.md`](SheepShaver/docs/UI-INTROSPECTION.md) — host-side structured read of the guest UI.

**Using it / the launcher:** [`SheepShaver/docs/USER-HANDBOOK.md`](SheepShaver/docs/USER-HANDBOOK.md) (prefs, networking, env vars, troubleshooting) · [`SiliconSheep/README.md`](SiliconSheep/README.md) (the Tauri launcher, "Silicon Sheep").

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
