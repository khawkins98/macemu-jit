# SheepShaver Runtime Diagnostics

Developer reference for the live diagnostics emitted by SheepShaver on macOS arm64
(`ppc-cpu.cpp` + `jit-heartbeat.hpp`). For the diag-log env vars and analysis tooling,
see the "JIT Diagnostic Logging" section of the repo-root CLAUDE.md and
`SheepShaver/tools/jit-analyze.py`.

## Terminal heartbeat

A liveness + stats line printed to **stderr** (and mirrored into the diag log file) on a
decaying cadence: **every 10s for the first minute of guest execution, then every 60s**.

```
[HB 30s] blocks=812M (28.4M/s) comp=3214 | jNK=89M jDR=655M jRAM=8M j2i=12K | rss=412MB cpu=98%
[HB 12m] blocks=4.1G (29.1M/s) comp=3514 | jNK=...                          | rss=455MB cpu=97%
```

| Field | Meaning |
|---|---|
| `blocks` / `(M/s)` | total guest blocks executed by this loop, and the rate since the last HB |
| `comp` | JIT-compiled block count (JIT mode only) |
| `jNK/jDR/jRAM` (or `iNK/iDR/iRAM`) | blocks executed per region: nanokernel/toolbox ROM, 68k DR emulator, guest RAM |
| `j2i` (or `i2j`) | JIT↔interpreter dispatch transitions |
| `rss` | host process resident memory |
| `cpu` | % of one core used since the last HB (user+sys) |

Interpreter mode (`SS_USE_JIT=0`) prints the `i*` variants so the two modes compare directly.

**Silence is a signal**: a fully-hung dispatch loop produces *no* HB lines (the heartbeat
is driven by block execution). If lines stop appearing, the loop is frozen.

Implementation: `src/kpx_cpu/src/cpu/jit/aarch64/jit-heartbeat.hpp`, called from the two
existing 5s heartbeat sites in `ppc-cpu.cpp`. Test vectors (`SS_TEST_HEX`) exit in
milliseconds and never produce HB lines.

## Warning matrix (anomaly detection)

Each heartbeat evaluates these rules. Findings append to the line as
`[SUSPECT: ...]` (yellow) or `[WARN: ...]` (red); the worst finding colors the whole
stderr line (ANSI, only when stderr is a TTY — redirected output and the log file always
get plain text).

| Signal | 🟡 SUSPECT | 🔴 WARN | Failure signature it catches |
|---|---|---|---|
| **Execution rate** | < 50% of running average | < 0.5M blk/s after 30s | guest stopped making progress |
| **Compile freeze** (JIT) | `comp` unchanged 2 HBs while rate > 1M/s | unchanged 5+ HBs | repeat-loop hang — same compiled blocks cycling forever (the extension-loading-hang signature) |
| **JIT↔interp transitions** | > 100K/s | > 1M/s | dispatch thrash (the pre-DR-JIT 2.4M/s bottleneck signature) |
| **Wild PC** (OTH region) | any growth since last HB | > 1% of all blocks | guest executing outside RAM/ROM — corrupted PC |
| **Memory (RSS)** | +10% between HBs after 60s | 2× initial, or > 2GB | leak / code-cache runaway |
| **CPU utilization** | < 80% after 30s | < 50% | process starved or waiting — e.g. host timer death (VBL) |

### Rules that are deliberately ABSENT

**"Same PC across consecutive heartbeats" is NOT a warning rule and must not be added.**
The heartbeat samples the PC at a point in time; the hottest PC in the system (the
nanokernel exception dispatcher at 0x50313d34) recurs across samples by chance. An entire
wrong "deadlock" theory plus a guest-corrupting fix was built on this misreading — see
LEARNINGS.md "session 5 part 2 — RETRACTION".

### Guards / known limitations

- No rules fire on the **first** heartbeat (no baseline yet)
- Rate and CPU rules need 30s of warmup (boot start is legitimately erratic)
- Rules whose inputs are unavailable (RSS/CPU sampling failure → shown as `rss=? cpu=?`)
  silently skip — missing data never produces a warning
- **Post-boot desktop idle** may yellow-flag the rate rule (an idle guest legitimately
  slows down). Treat SUSPECT as "look", not "broken".
- Thresholds are first-pass guesses encoded from LEARNINGS.md history. If a healthy boot
  shows color, tune the thresholds in `jit-heartbeat.hpp` and update this table — both
  copies of the matrix (this file + the header comment) must stay in sync.

## ROM inspection (rom-inspect)

Standalone tool to check whether a ROM file would be accepted, without launching the
emulator. Shares the decode + type-detection code with the emulator via
`src/include/rom_decode.hpp`.

```bash
cd SheepShaver
make inspect-rom ROM="/path/to/Mac OS ROM"      # build + inspect
# or directly:
./rom-inspect/rom-inspect "/path/to/Mac OS ROM"
```

Reports the on-disk format (raw / CHRP-LZSS / CHRP-parcels), decode success, the
nanokernel ID bytes at decoded `0x30d064`, and the detected ROM type. Exit code `0` if
decode + type-detection pass, `1` if rejected, `2` on IO error.

**Scope — important.** The tool models `DecodeROM()` and `PatchROM()`'s **type detection**
only. `PatchROM()` then runs patch-space checks and byte-pattern searches
(`patch_nanokernel_boot`/`_68k_emul`/`_nanokernel`/`_68k`) that this tool does NOT model.
A ROM can pass type-detection here and still be rejected downstream — and the emulator
shows the **same** "Unsupported ROM type" alert for *any* `PatchROM()` failure
(`main.cpp:162`), which is misleading. Example: the parcels-format `Mac OS ROM 9.0.1`
decodes and type-detects as NewWorld, yet the emulator rejects it downstream. So
"type-detection OK" means "not rejected for format/type," not "guaranteed to boot."

## Other live diagnostics (pre-existing, ppc-cpu.cpp)

| Output | Trigger | Notes |
|---|---|---|
| `[JIT] diagnostic log: <path>` | first heartbeat | per-instance file; `/tmp/jit_diag.log` symlinks to latest run |
| `[JIT Ns] HOT-PC ...` | same sampled PC 3+ heartbeats | **sampling hint only** — see retraction note above |
| `[JIT Ns] STALL: comp=...` | `SS_JIT_RING_DUMP_ON_STALL=<n>` | one-shot trace-ring dump on compile freeze |
| `[JIT Ns] interrupt delivered` | each guest interrupt | diag log file only |

## Boot-stall watchdog (`[ALARM]` / `[STALL]`)

Catches **early-boot dead-ends that emit nothing else** — the ROM "This startup disk will
not work on this Macintosh model" alert, an early hang, a sad Mac. The trap these fall into:
every higher-level boot signal (`[BOOT]`/`[APP]` modal, `[SYSV]`, `[READY]`) rides the
`SynchIdleTime` idle hook, which only fires once the guest reaches Process-Manager idle — a
wedged guest never does. So the log shows a ~0.2s burst of `[JIT … first compile …]` lines
and then goes silent, and the operator is left staring at a GUI screen the log can't see
(see `LEARNINGS.md` + memory `gui-outcomes-not-in-log`).

The watchdog (`ss_boot_stall_check`, emul_op.cpp, driven by the host-side heartbeat which
keeps ticking through the wedge) fires on the **shape** of the failure, which is
signal-independent: blocks keep executing fast (~150M/s, a tight already-compiled wait loop)
while **no new blocks compile** and idle is **never reached**.

| Output | Meaning |
|---|---|
| `[ALARM] boot stalled at Ns: no new blocks for Ns, guest NOT idle, still spinning NM/s -> dead-end …` | one-shot when the stall is confirmed (default ~15s). Names the front dialog if the WindowManager is up (later prompts: disk-repair, rebuild-desktop); for a **pre-System DSAlert** (e.g. the model-rejection screen) WindowList reads `0xffffffff` — it says so and points you to a screenshot. |
| `[STALL] still wedged at Ns …` | re-stated every 30s after the alarm, so a `tail` of the log shows the live stalled state instead of silence. |
| `[RECOVERED] boot reached idle at Ns …` | one-shot retraction — printed if an `[ALARM]` fired but the guest *then* reached idle (a slow medium that crossed the threshold yet did boot). Makes a false alarm self-correcting; if you see it, raise `SS_BOOT_STALL_SECS`. |

**Why this is NOT the forbidden "same-PC" rule** (warning-matrix retraction above): that
caution is about POST-boot HOT-PC sampling. This watchdog is scoped strictly **pre-idle** and
self-disarms the instant `[BOOT] idle` fires (healthy boot ~10s « 15s threshold), and re-arms
if compilation resumes — so a healthy boot never trips it. Tunable via `SS_BOOT_STALL_SECS`
(default 15; `0` disables). Empirically verified: `[ALARM]` at 15.0s on the failing 9.2.1
NewWorld boot (2026-06-07).

## Diagnostic environment variables

The canonical reference for the JIT/EMUL_OP debug knobs (read by `ppc-cpu.cpp`,
`ppc-jit.cpp`, `sheepshaver_glue.cpp`). All are zero-cost when unset.

| Env var | Effect |
|---|---|
| `SS_USE_JIT=0` | Force interpreter mode (the aarch64 JIT is ON by default). |
| `SS_JIT_DIAG_LOG=/path` | Diagnostic-log path override (no `/tmp/jit_diag.log` symlink touched when set). |
| `SS_JIT_NO_ROM=1` | Keep ROM interpreter-only, JIT only RAM — isolates ROM vs RAM bugs. |
| `SS_JIT_ROM_SIZE=0xNNNNNN` | Limit the JIT-compiled ROM range — binary-search tool for isolating a bad ROM region. |
| `SS_JIT_NO_CHAIN=1` | Disable block chaining at runtime (bisect chaining bugs without a rebuild). |
| `SS_JIT_VERIFY=1` | Differential verify: re-run every JIT block through the interpreter and compare register state. **EXTREMELY SLOW**; reports the first divergences with block PC + opcodes. |
| `SS_JIT_PROFILE=1` (or `=/path`) | Execution-weighted hot-block profiler (OPTIMIZATION-PLAN §P0): each block counts its executions + an instruction-mix tag; dumps the top-40 hottest blocks (pc/exec/%/mix/insns/region) at exit to stderr (or to `/path`). Also emits a `[JIT-RUN-PROFILE]` line: empirical **guest-MIPS** (wall-clock throughput, "operations per second" for the whole run — boot/app/benchmark), and a **deterministic** execution-weighted `a64/guest-op` codegen-density A/B metric (zero host-noise; emitted whole-block, an inflated upper bound — use for deltas, not as a literal executed count). Capture per workload via the e2e harness (`SS_JIT_PROFILE=/path make e2e` / `e2e-bench`). Zero cost when unset. |
| `SS_JIT_PROFILE_DISASM=1` | With `SS_JIT_PROFILE`, also dump each top block's first 16 PPC instruction words (big-endian encodings), captured at compile time. Disassemble offline with capstone (`CS_ARCH_PPC`, `CS_MODE_BIG_ENDIAN`, `struct.pack('>I', word)`). Used to identify hot blocks — e.g. the boot atomic-primitive cluster in §P0. (Compile-time capture, not exit-time reads: the NATMEM reservation has PROT_NONE holes that fault on read.) |
| `SS_JIT_DEBUG_PC=0xNNNNNNNN` | Per-PC debug output. |
| `SS_JIT_WATCH_ADDR=dec,dec` | Guest-memory watchpoints (decimal, comma-separated). |
| `SS_JIT_WATCH_STUB=1` | Software watchpoint on Mixed Mode switch-back stubs (`ppc-cpu.cpp`). |
| `SS_JIT_TRACE_RING=1` | Block-level execution-history ring; dumped to `/tmp/ss_jit_ring.txt` by the SIGSEGV handler (records J/I blocks, inline calls, EMUL_OP entry/return). |
| `SS_JIT_RING_DUMP_TRIGGER=1` | Dump the trace ring when the DR emulator executes stack-region code (`ppc-cpu.cpp`). |
| `SS_JIT_RING_DUMP_ON_STALL=<n>` | One-shot trace-ring dump on a compile freeze (see table above). |
| `SS_BOOT_STALL_SECS=<n>` | Boot-stall watchdog threshold in seconds (default 15; `0` disables). Pre-idle dead-end alarm — see the `[ALARM]`/`[STALL]` section above. |
| `SS_EMULOP_COUNTS=1` | Per-`EMUL_OP` execution counters to stderr every ~5s (`sheepshaver_glue.cpp`). |
| `SS_EMULOP_TRACE=1` | Log SCSI `EMUL_OP` return values to `/tmp/emulop_trace.log` (`sheepshaver_glue.cpp`). |
| `SS_UI_DUMP_DIR=<dir>` | **Feature gate (not a JIT diagnostic)** — enables on-demand guest UI introspection. When set, the idle hook services `ss_ui.req` and writes a Backend-A window-list JSON snapshot (`ss_ui.A.json`) + nonce-stamped `ss_ui.done`. Zero cost when unset. See `SheepShaver/docs/UI-INTROSPECTION.md` for the full reference. |
| `SS_DUMP_ROM=/path` | Dump the full decompressed ROM image at startup (after patching). Essential for NewWorld CHRP ROMs where the `.rom` file is compressed. See CLAUDE.md "Disassembling the Decompressed ROM" for the capstone workflow. |
| `SS_PROBE_PC=0xADDR[:fields][;…]` | No-recompile register/memory dump at block-entry PCs (`ppc-cpu.cpp`). Fields: `rN` (GPR), `[0xADDR]` (guest mem 4-byte read), or omit for full dump. Logarithmic sampling (visit 1, 10, 100, ...). Up to 8 PCs, 16 fields. See CLAUDE.md "PC Probes" for format and examples. |

### Dump the trace ring from a running (or hung) process

With `SS_JIT_TRACE_RING=1`, the ring can be dumped from a **live** process without crashing
it — no SIGSEGV needed:

```bash
lldb -b -p $(pgrep -x SheepShaver) \
     -o "expression -- (void)ppc_jit_dump_trace_ring()" -o detach -o quit
```

Attach **at most once per run** and detach immediately — repeated lldb attach/detach can defer
the 60 Hz VBL timer and hang early boot (see the lldb/VBL caveat in `CLAUDE.md` / `LEARNINGS.md`).
