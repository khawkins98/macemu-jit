# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a fork of [macemu](https://github.com/kanjitalk755/macemu) adding AArch64 JIT backends for macOS arm64. It contains two emulators:

- **BasiliskII** — emulates 68K Macs (Mac OS 7.x–8.x), JIT translates 68K→ARM64
- **SheepShaver** — emulates PowerPC Macs (Mac OS 8.x–9.x), JIT translates PPC→ARM64

The active branch `macos-arm64` is the macOS-specific port. The upstream `rcarmo/macemu-jit` targets Linux ARM64 (Orange Pi / Raspberry Pi); macOS is this fork's niche.

---

## Build Commands

### BasiliskII (top-level Makefile delegates to `BasiliskII/src/Unix/`)

```bash
# Configure (first time, or after configure.ac changes)
cd BasiliskII/src/Unix
NO_CONFIGURE=1 ./autogen.sh
./configure --enable-sdl-video --enable-sdl-audio \
            --enable-jit-compiler --enable-aarch64-jit-experimental \
            --disable-vosf --without-gtk --without-x --without-esd

# Build (incremental)
make build          # from repo root — delegates to BasiliskII/src/Unix/

# Full rebuild (regenerates gencomp + compemu.cpp)
make rebuild

# Clean
make clean
```

### SheepShaver

```bash
# Configure (first time)
cd SheepShaver/src/Unix
NO_CONFIGURE=1 ./autogen.sh
./configure --enable-sdl-video --enable-sdl-audio --enable-jit \
            --without-gtk --without-x --without-esd

# Build SheepShaver + ROM harness
cd SheepShaver
make build          # builds SheepShaver binary + rom-harness

# Build only SheepShaver
make build-ss

# Build only ROM harness
make build-rom-harness
```

---

## Test Commands

### BasiliskII JIT opcode harness (301 vectors, score must be 100)

```bash
make test           # from repo root — runs jit-test/run.sh
make test-jit       # same, explicit target
./jit-test/run.sh   # run directly

# Output format: METRIC pass=N fail=N total=N score=N
```

### SheepShaver JIT opcode harness (233 vectors, score must be 100)

```bash
cd SheepShaver
make test-opcodes   # runs SheepShaver/jit-test/run.sh

# Single vector: set SS_TEST_HEX=<hex> SS_TEST_JIT=1 (or 0 for interpreter)
```

### SheepShaver ROM harness (standalone headless JIT exerciser)

```bash
cd SheepShaver
make test-rom       # runs rom-harness against ROM (needs OldWorld PPC ROM)
# Direct:
./rom-harness/rom-harness <rom-file> --count=10000 --min-insns=1 --max-insns=64
# Options: --verbose --stop-on-fail --entry=0xNNNN --seed=N --passes=N
# Requires OldWorld (raw) ROM dump — New World CHRP ROMs don't scan cleanly
```

### BasiliskII headless boot test

```bash
make test-headless  # from repo root — 60s timeout, requires ROM path in Makefile
```

---

## Running

### BasiliskII (tmux + VNC, requires ROM and disk)

```bash
make start          # starts in tmux session 'emu', VNC on port 5900
make stop           # kills session
make status         # check if running
make screenshot     # capture via import(1) to /tmp/

# Environment variables:
# B2_JIT_MAX_OPTLEV=2     Max JIT level (0=interp, 1=dispatch, 2=native codegen)
# B2_JIT_MANAGED_IRQ=1    Deferred IRQ model (recommended)
```

### SheepShaver

```bash
cd SheepShaver
make run-jit        # background, JIT enabled, VNC port 5999
make run            # background, interpreter mode, VNC port 5999
make run-jit-tmux   # in tmux session
make kill           # kill all SheepShaver processes

# Environment variables:
# SS_USE_JIT=0     Force interpreter mode (JIT is ON by default — no env var needed)
# SS_TEST_HEX=...  Run single opcode test vector (harness path)
# SS_TEST_JIT=1    Use JIT for SS_TEST_HEX test (default: interpreter)
#
# NOTE: The `jit` pref in ~/.sheepshaver_prefs controls the legacy kpx_cpu codegen JIT
# only (compiled out on aarch64 via ENABLE_DYNGEN=0 — it has no effect). The aarch64 JIT
# (ppc-jit.cpp, USE_AARCH64_JIT) is independent — it is active whenever SS_USE_JIT != "0",
# regardless of prefs. Running bare ./SheepShaver runs the aarch64 JIT.
# SS_USE_JIT=0 is the only way to force the interpreter.
```

**Current prefs config** (`~/.sheepshaver_prefs`): HD boot — Mac OS 8.6 is installed on
`/Users/Shared/macemu/macos921.dsk`, CD detached (`nocdrom true`, `bootdriver 0`),
VNC server enabled on port 5999 for headless screenshots. The previous CD-installer-boot
config is backed up at `~/.sheepshaver_prefs.cd-boot-backup`.

**Only ONE emulator instance can run at a time** — instances share the prefs file, disk
images, and SDL window. Kill strays with `pkill -9 -x SheepShaver` before starting a run.
Agents must not launch their own emulator instances; the user runs boots, agents read logs.

---

## Debugging & Investigation

### lldb Workflow with Address Arithmetic

Guest addresses must be translated using `NATMEM_OFFSET = 0x400000000000`:
```bash
# To inspect guest memory at 0x5007aed4:
# In lldb: x -s 4 -f x 0x40005007aed4
# Result is little-endian; BSWAP32 each 4-byte word for PPC/68k instructions

# Example: ROM block 504613e0
# Guest: 0x504613e0 → Host: 0x4000504613e0
# lldb> x -s 4 -f x 0x4000504613e0  # shows lhau, addco., rlwimi, etc.
```

### Watchpoint Monitoring

SheepShaver supports guest-address watchpoints via environment variables:
```bash
# Watch addresses (decimal, comma-separated):
SS_JIT_WATCH_ADDR=273866508,268414144
# Suppress REGDUMP output (faster logging):
SS_JIT_WATCH_DUMPS=0

# Example: catch A3 register corruption at 0x103ffffe
# (A3 lives in r19/gpr[19], offset 0x4c from powerpc_registers struct)
SS_JIT_WATCH_ADDR=273866508 SS_JIT_WATCH_DUMPS=0 make run-jit
```

### JIT Diagnostic Logging (sessions 4–6, ppc-cpu.cpp)

The JIT emits structured diagnostic output during execution:

**stderr** (always-on, low noise):
```
[JIT 0.00s] JIT initialized, ROM range [50000000..50500000]
[JIT] diagnostic log: /tmp/jit_diag.20260603-141502.12345.log
[JIT 15.0s] HOT-PC pc=50313d34 sampled 3 consecutive heartbeats (may be sampling artifact)
```

**`/tmp/jit_diag.<timestamp>.<pid>.log`** (heartbeat every ~5s, with per-region block
counters). `/tmp/jit_diag.log` is a symlink to the most recent run's log, so
`tail -f /tmp/jit_diag.log` and `jit-analyze.py` defaults always follow the latest run:
```
[JIT 5.0s]    blocks=326647 pc=5031040c | jNK=... jDR=... jRAM=... jOTH=... | iNK=... iDR=... iRAM=... iOTH=... | j2i=... i2j=...
[INTERP 5.0s] blocks=... pc=... | iNK=... iDR=... iRAM=... iOTH=... | ...   ← pure-interpreter mode (SS_USE_JIT=0)
[JIT 0.03s]   interrupt delivered, pc=5031040c
```

Region key: NK = nanokernel/toolbox ROM (JIT-compiled), DR = 68k DR emulator
(interpreter-only), RAM = guest RAM, OTH = other. `j2i`/`i2j` = JIT↔interpreter
transitions. Heartbeats run in BOTH modes so runs can be compared directly.

**Interpreting the log**:
- Heartbeat lines every 5s — if they stop, the dispatch loop froze
- **HOT-PC lines are a sampling hint, NOT proof of a hang.** The hottest PC in the system
  (the nanokernel exception dispatcher at 0x50313d34) recurs across heartbeats by chance.
  See the session 5 part 2 retraction in `LEARNINGS.md` before acting on these.
- **Block counts are NOT comparable across modes** — DR dispatch blocks are 2–4 instructions,
  toolbox/RAM blocks are larger. Wall-clock time to desktop is the only honest comparison.
- No heartbeats despite process alive = tight native loop bypassing dispatcher (check for new backward-branch regressions)

**Log analysis tool**: `python3 SheepShaver/tools/jit-analyze.py {diag|ring|hot} [LOG]`

**Diagnostic env vars** (all read by ppc-cpu.cpp / ppc-jit.cpp):
```bash
SS_JIT_DIAG_LOG=/path        # Diagnostic log path override (no symlink touched when set).
                             # Default: per-instance /tmp/jit_diag.<timestamp>.<pid>.log,
                             # with /tmp/jit_diag.log symlinked to the latest run.
SS_JIT_NO_ROM=1              # Keep ROM interpreter-only, JIT only RAM — isolates ROM vs RAM bugs
SS_JIT_ROM_SIZE=0x460000     # Limit JIT-compiled ROM range (binary search tool for isolating bugs)
SS_JIT_NO_CHAIN=1            # Disable block chaining at runtime
SS_JIT_TRACE_RING=1          # Enable block-level execution history ring
SS_JIT_RING_DUMP_TRIGGER=... # Dump ring when a PC is hit
SS_JIT_WATCH_ADDR=...        # Guest-memory watchpoints (see Watchpoint Monitoring)
SS_EMULOP_COUNTS=1           # Count EMUL_OP executions
SS_JIT_DEBUG_PC=...          # Per-PC debug output
```

### Single-Opcode Testing

Test a specific PPC instruction in isolation:
```bash
# From SheepShaver/: run one vector, interpreter mode
SS_TEST_HEX=38601234 SS_TEST_JIT=0 make test-opcodes

# For JIT mode, manually trigger with:
SS_TEST_HEX=38601234 SS_TEST_JIT=1 make test-opcodes
```

---

## Common Issues & Gotchas

### "STUCK"/HOT-PC Detections Are Sampling Artifacts (session 5 retraction)

The heartbeat samples the current PC every 5 seconds. The same PC appearing in consecutive
heartbeats does NOT mean the guest is hung — the hottest PC in the system (the nanokernel
exception dispatcher at 0x50313d34) recurs by chance. An entire wrong "deadlock" theory
plus a guest-memory-corrupting fix (commit a46cda99, reverted in 9f9e617e) was built on
this misreading. Read `LEARNINGS.md` "session 5 part 2 — RETRACTION" before theorizing
about boot hangs.

**Methodology rule**: never infer code behavior from register state alone — disassemble
the actual instructions first (single lldb attach + memory dump + capstone, then detach
immediately).

### Boot-Time Claims Are Config-Dependent

Boot times vary by media: HD boot ~12s (interpreter), CD boot ~318s (interpreter).
JIT boot with full DR emulator compiled (ROM=0x500000, chaining=1) is comparable to
interpreter HD boot. Any speed ratio must specify boot media and prefs config.

### VBL Timer Disruption from lldb

**Problem**: Multiple lldb attach/detach cycles cause the macOS kernel to defer the 60Hz VBL timer. ROM's early-boot spin-wait at 0x5031040c hangs forever.

**Fix**: Attach lldb **at most once per run**. Do minimal investigation and detach immediately. If emulator gets stuck in a 2-block loop with all-zero A0-A7 registers in the trace ring, the VBL timer died — restart.

### Crorc Fix & Block Chaining (Bug #2) — RESOLVED

**Status**: Fully resolved. The original crorc load-immediate bug (commit c100f9ab) was fixed
in session 3. The subsequent DR emulator boot hang (sessions 5–7) was caused by a **subfe/adde
carry-out codegen bug**, not spcflags timing. The JIT's two-step ADDS approach only captured
carry from the partial sum (~rA + rB), missing the +CA contribution. Fixed by computing the
full sum in 64 bits.

**Current verified configuration**:
- ROM range: `0x500000` (full, including DR emulator — JIT-compiled)
- Chaining: `JIT_BLOCK_CHAINING=1` (enabled by default)
- Harness: 235/235, score=100
- Boot: Mac OS 8.6 to Finder desktop (VNC confirmed)

### Opcode Coverage Gaps

All previously-flagged "uncovered" opcodes ARE native in the JIT (session 4 audit):
- `mcrxr`, `lhbrx`, `lwbrx`, `mtcrf` (all FXM), `sthu`, `lhau`, `lhzu`, `stwbrx`, `sthbrx`, `cntlzw` — all have native emit paths
- Only real gap: `bcl` with CTR+cond+link=1 combined (known incomplete, comment in ppc-jit.cpp)
- SPR fallbacks (SPRG/BAT/SRR) are negligible: max 3 instances in entire ROM, all boot-time supervisor ops

---

## Asset Locations

All test ROMs and disk images are located in `/Users/Shared/macemu/`:

```bash
ROM:  /Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom    # OldWorld, active
ROM:  /Users/Shared/macemu/Mac OS ROM 9.0.1                   # NewWorld, staged for future use
HD:   /Users/Shared/macemu/macos921.dsk                       # 2GB; Mac OS 8.6 INSTALLED (boots from this)
ISO:  /Users/Shared/macemu/Mac OS 8.6 Internal Edition.iso    # installer, currently detached
Prefs: ~/.sheepshaver_prefs            # HD-boot config; CD-boot backup at ~/.sheepshaver_prefs.cd-boot-backup
```

Update the Makefile `ROM_PATH` and `ISO_PATH` variables if assets move.

---

## Development Status

### BasiliskII (68K → ARM64 JIT)
- **Status**: Functional, harness 301/301, score=100
- **LTO disabled**: Intentional on AArch64; clang/gcc strip JIT gate checks as dead code
- **Next**: FPU instruction support (see `JIT-FPU-PLAN.md`)

### SheepShaver (PPC → ARM64 JIT)
- **Status**: Fully functional, harness 235/235, score=100
- **Boot verified**: Mac OS 8.6 boots to Finder desktop with full JIT (VNC screenshot confirmed)
- **Configuration**: ROM=0x500000 (full range including DR emulator), chaining=1 (enabled by default)
- **Key fix**: subfe/adde carry-out computation was wrong — the JIT read carry from a partial ADDS instead of the full three-operand sum. Fixed by computing in 64 bits and extracting bit 32. See LEARNINGS.md session 7.
- **Performance**: DR emulator JIT-compiled eliminates 2.4M transitions/s bottleneck; jDR=37M blocks/s

See `JIT-STATUS.md` for detailed per-opcode pass/fail matrix.

---

## Architecture

### BasiliskII JIT (68K → ARM64)

The JIT is large (~60K lines) and inherits from the x86 UAE JIT via ARM (32-bit) intermediaries:

- **`BasiliskII/src/uae_cpu_2026/compiler/`** — all JIT code:
  - `compemu_support.cpp` — block compilation, register allocator, barriers
  - `compemu_legacy_arm64_compat.cpp` — maps x86-style primitives (sbb, adc, bt) to ARM64
  - `compemu_midfunc_arm64.cpp` / `_arm64_2.cpp` — register allocator API + native codegen
  - `codegen_arm64.cpp` — raw instruction emitter
  - `compemu.cpp` — generated compiled handlers (2827 functions, regenerated by `make rebuild`)
- **`BasiliskII/src/uae_cpu_2026/newcpu.cpp`** — interpreter core, interrupt handling
- **`BasiliskII/src/Unix/basilisk_glue.cpp`** — TriggerInterrupt, managed IRQ model

JIT levels: L1 = interpreter dispatch with spcflags checks; L2 = native ARM64 codegen with register allocator. The register allocator maps m68k D/A registers across instructions within a block; x27=R_MEMSTART (guest RAM base), x28=R_REGSTRUCT. Guest memory uses `LDR [R_MEMSTART + guest_addr]` with REV/REV16 for big-endian conversion.

Key build note: **LTO is intentionally disabled** on AArch64 — it strips JIT gate checks that clang/gcc considers dead code.

### SheepShaver JIT (PPC → ARM64)

The JIT is a hand-written emitter (~4.1K lines) — no dyngen, no x86 legacy:

- **`SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`** — the entire JIT (~4100 lines): block compilation, all emit helpers, block cache, chain patching
- **`SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/jit-target-cache.hpp`** — code cache alloc (MAP_JIT on macOS), write-protect toggling
- **`SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp`** — PPC CPU execute loop; JIT is gated by `SS_USE_JIT` env var (line ~712)
- **`SheepShaver/rom-harness/rom-harness.cpp`** — standalone headless JIT exerciser

Block cache: 8192-bucket hash of linked lists in `jit_bc_heads[]`/`jit_bc_pool[]` (sentinel = -1). Chain sites: a pool of patch locations back-filled by `patch_chain_sites()` when a target block becomes available.

XER is a struct `{uint8 so, ov, ca, byte_count}` — **NOT** a packed uint32. All JIT access uses LDRB/STRB at individual byte offsets (so=900, ca=902).

### macOS arm64 Constraints

These apply on the `macos-arm64` branch and are not present in the upstream Linux build:

1. **DIRECT_ADDRESSING** — guest addresses are translated as `host = NATMEM_OFFSET + (guest & 0xFFFFFFFF)` where `NATMEM_OFFSET = 0x400000000000`. The classic low-4GB REAL_ADDRESSING is impossible on macOS arm64 (4GB `__PAGEZERO` is mandatory; no `personality()` to disable ASLR).

2. **MAP_JIT** — anonymous `mmap(PROT_EXEC)` returns EPERM on Apple Silicon without `MAP_JIT`. Code cache allocation uses `MAP_JIT`; emission is bracketed by `pthread_jit_write_protect_np(false/true)` + `sys_icache_invalidate()`. Ad-hoc/linker-signed dev builds don't need the `com.apple.security.cs.allow-jit` entitlement.

3. **configure.ac host detection** — on macOS, `config.guess` reports `arm-apple-darwin*` (not `aarch64-*`). The AArch64 JIT path is guarded by a `case $host_cpu` that matches both `aarch64` and `arm` on Darwin.

4. **K&R / C23 slirp** — Homebrew autoconf bakes `-std=gnu23` into `$(CC)`. Slirp's vendored `.c` files use K&R syntax (removed in C23); `configure.ac` detects this and appends `-std=gnu89` to `SLIRP_CFLAGS` only for those files.

5. **CD-ROM open** — `sys_unix.cpp` uses `O_EXLOCK|O_NONBLOCK` on macOS; read-only images retry without lock on EAGAIN.

---

## JIT Style Decision

**Prefer SheepShaver-style JIT for new work:** immediate architectural writeback, explicit state ownership, no lazy-state assumptions across block boundaries. See `JIT-STYLE-DECISION.md` for the full rationale.

For BasiliskII: keep the existing machinery but push toward clearer boundaries, explicit ownership, and exact helper barriers over speculative native continuation for uncertain semantics.

Rule: *simple by default, complexity by proof, performance by earned sophistication.*

---

## Key Documentation

| File | Contents |
|------|----------|
| `LEARNINGS.md` | Session log of non-obvious findings — **read at the start of each session** |
| `docs/HANDOFF-2026-06-02-SESSION6.md` | **Current** investigation plan (boot-time profiling) + known traps |
| `docs/HANDOFF-2026-06-02-SESSION5.md` | ⚠️ RETRACTED — kept for history, do not implement anything from it |
| `JIT-STATUS.md` | Current pass/fail status for both emulators |
| `JIT-STYLE-DECISION.md` | Engineering approach for new JIT work |
| `BasiliskII/docs/AARCH64_JIT_BRINGUP.md` | Full 68K JIT bringup history + all bug fixes |
| `SheepShaver/AARCH64_JIT_PLAN.md` | SheepShaver PPC JIT plan and status |
| `BasiliskII/docs/AARCH64_JIT_RUNTIME_CONTRACT.md` | Register and state ownership contract |
| `jit-test/README.md` | Harness invariants and metric contract |
| `SheepShaver/rom-harness/README.md` | ROM harness usage and options |
| `SheepShaver/tools/jit-analyze.py` | Diagnostic log analysis (diag/ring/hot subcommands) |
| `SheepShaver/docs/EMULATOR-RESEARCH-LEADS.md` | Leads from Dolphin/other PPC JITs worth investigating |

---

## Harness Integrity

`jit-test/run.sh` validates its own test vector table before running: no duplicate names, every ordered test has both bytecode and sentinel, strict 4-hex-word token format, no `2C7C` reserved token, unique sentinels. Any violation aborts with `infra_fail=1` before executing any vector.

The JIT harness runs each vector **twice in interpreter mode** and diffs the two REGDUMPs. It does not currently run JIT mode (set `SS_TEST_JIT=1` manually to drive JIT path in opcode tests).
