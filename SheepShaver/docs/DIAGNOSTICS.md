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

## ROM patching diagnostics (`[ROMPATCH]`)

Two env vars control ROM-patch logging at startup (`rom_patches.cpp`). Both are permanent
ROM-porting infrastructure -- use them when bringing up a new ROM version.

**`SS_ROM_LENIENT=1`** — force lenient patch mode for any NewWorld ROM. When a
`find_rom_data` pattern is absent, the patch function logs and continues instead of
aborting. Only effective on `ROMTYPE_NEWWORLD` (the check is
`getenv("SS_ROM_LENIENT") && ROMType == ROMTYPE_NEWWORLD`). Auto-enabled by checksum
for the 9.0.4 G4 ROM; opt out of auto-detection with `SS_ROM_NO_904=1`.

**`SS_ROM_PATCH_TRACE=1`** — log every `find_rom_data` call (hit, relocated, or miss).
Independent of lenient mode; useful on any ROM to see where patterns land.

### Log line formats

| Line | Gate | Meaning |
|------|------|---------|
| `[ROMPATCH] find_rom_data [...] -> HIT @offset` | `SS_ROM_PATCH_TRACE` | Pattern found in declared range |
| `[ROMPATCH] find_rom_data [...] -> RELOCATED @offset (904 whole-image fallback)` | `SS_ROM_PATCH_TRACE` + lenient | Pattern absent in declared range, found elsewhere via whole-image scan |
| `[ROMPATCH] find_rom_data [...] -> MISS (absent even whole-image)` | `SS_ROM_PATCH_TRACE` + lenient | Pattern not present anywhere |
| `[ROMPATCH] find_rom_data [...] -> MISS (abort point)` | `SS_ROM_PATCH_TRACE`, no lenient | Pattern not found -- `PatchROM` will abort |
| `[ROMPATCH] SKIP <name> (absent in parcels)` | lenient mode (always, no trace needed) | Patch site skipped; the named shim is not applied |
| `[ROMPATCH] parcels: <detail>` | lenient mode | Structural skip/info for nanokernel-boot patches |

### Workflow: mapping a new ROM

1. Run with both env vars: `SS_ROM_LENIENT=1 SS_ROM_PATCH_TRACE=1 ./SheepShaver --config <new-rom.prefs> 2>/tmp/rompatch.log`
2. Grep the log for `MISS` and `SKIP` lines -- these are the patches that need porting.
3. Cross-reference each name against the shim inventory
   (`docs/planning/PATCH-68K-SHIM-INVENTORY.md`) to find its concept, EMUL_OP, and search range.
4. For `RELOCATED` hits, verify the patch's offset arithmetic is still correct at the new
   site (the pattern matched, but surrounding code may have shifted).

## Other live diagnostics (pre-existing, ppc-cpu.cpp)

| Output | Trigger | Notes |
|---|---|---|
| `[JIT] diagnostic log: <path>` | first heartbeat | per-instance file; `/tmp/jit_diag.log` symlinks to latest run |
| `[JIT Ns] HOT-PC ...` | same sampled PC 3+ heartbeats | **sampling hint only** — see retraction note above |
| `[JIT Ns] STALL: comp=...` | `SS_JIT_RING_DUMP_ON_STALL=<n>` | one-shot trace-ring dump on compile freeze |
| `[JIT Ns] interrupt delivered` | each guest interrupt | diag log file only |
| `[NW-MIRROR] table[0] @ ROM+0x46e8c0: OOOO → PPPP (cold-start redirect)` | `SS_NW_TRAMPOLINE=1` at ROM-patch time | confirms the mirror table[0] cold-start patch applied; if the existing value is unexpected, prints `unexpected value XXXX — skipping patch` instead |
| `[WATCH] pc=... addr=... value=...` | `SS_JIT_WATCH_ADDR` on each detected change | watchpoint hit — see env var entry above for full format |

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
| `SS_JIT_WATCH_ADDR=hex,hex` | Guest-memory watchpoints (**HEX, no `0x` prefix**, comma-separated, up to 4, 4-byte aligned). **Requires `SS_JIT_TRACE_RING=1`** — the check lives in `jit_ring_record()`, so without the ring nothing fires. Emits `[WATCH] pc=PPPPPPPP addr=AAAAAAAA value=VVVVVVVV  (was WWWWWWWW ...)` on each detected change. PC is block-entry granularity for JIT, exact instruction for interpreter. Grep for `[WATCH]` to parse programmatically. (Doc fix 2026-06-11: the parser is `strtoul(tok, NULL, 16)` — earlier docs said decimal.) |
| `SS_JIT_WATCH_DUMPS=<n>` | With `SS_JIT_WATCH_ADDR`: how many of the first detected changes ALSO dump the trace ring (default 3). Set `0` for report-only when watching busy locations (stack slots). |
| `SS_JIT_WATCH_STUB=1` | Software watchpoint on Mixed Mode switch-back stubs (`ppc-cpu.cpp`). |
| `SS_JIT_TRACE_RING=1` | Block-level execution-history ring; dumped to `/tmp/ss_jit_ring.txt` by the SIGSEGV handler (records J/I blocks, inline calls, EMUL_OP entry/return). |
| `SS_JIT_RING_DUMP_TRIGGER=1` | Dump the trace ring when the DR emulator executes stack-region code (`ppc-cpu.cpp`). |
| `SS_JIT_RING_DUMP_ON_STALL=<n>` | One-shot trace-ring dump on a compile freeze (see table above). |
| `SS_BOOT_STALL_SECS=<n>` | Boot-stall watchdog threshold in seconds (default 15; `0` disables). Pre-idle dead-end alarm — see the `[ALARM]`/`[STALL]` section above. |
| `SS_EMULOP_COUNTS=1` | Per-`EMUL_OP` execution counters to stderr every ~5s (`sheepshaver_glue.cpp`). |
| `SS_EMULOP_TRACE=1` | Log SCSI `EMUL_OP` return values to `/tmp/emulop_trace.log` (`sheepshaver_glue.cpp`). |
| `SS_UI_DUMP_DIR=<dir>` | **Feature gate (not a JIT diagnostic)** — enables on-demand guest UI introspection. When set, the idle hook services `ss_ui.req` and writes a Backend-A window-list JSON snapshot (`ss_ui.A.json`) + nonce-stamped `ss_ui.done`. Zero cost when unset. See `SheepShaver/docs/UI-INTROSPECTION.md` for the full reference. |
| `SS_DUMP_ROM=/path` | Dump the full decompressed ROM image at startup (after patching). Essential for NewWorld CHRP ROMs where the `.rom` file is compressed. See CLAUDE.md "Disassembling the Decompressed ROM" for the capstone workflow. |
| `SS_PROBE_PC=0xADDR[:fields][;…]` | No-recompile register/memory dump at block-entry PCs (`ppc-cpu.cpp`). Fields: `rN` (GPR), `[0xADDR]` (guest mem 4-byte read), `[rN:SIZE]` (SIZE bytes from address in gpr(N); SIZE hex, 0x prefix optional, clamped to 4KB — output 8 words/line with `+0xOFFSET:` labels), or omit for full dump. Logarithmic sampling (visit 1, 10, 100, ...). Up to 8 PCs, 16 fields. See CLAUDE.md "PC Probes" for format and examples. |
| `SS_SEED_MEM=…` | No-recompile guest-memory poke (`ppc-cpu.cpp`). Two forms: **immediate** (`0xADDR=0xVAL[;…]`) — applied once at NW-trampoline-end (after nanokernel init, in `init_emul_ppc`); **PC-triggered** (`0xPC:0xADDR=0xVAL[;…]`) — applied at the FIRST JIT block-entry visit of `0xPC`. Each seed is a single 32-bit write. Up to 16 entries, semicolon-separated; may mix both forms. MMIO-range addresses refused (same guard as `SS_PROBE_PC`). Emits `[SEED] addr=0xADDR val=0xVAL` to stderr per applied seed. **Limitations:** the PC-triggered form fires only in JIT execution (boot + the `SS_TEST_JIT` harness) — the hook lives in the JIT block-entry dispatch path, not pure interpreter. The immediate form fires at NW-trampoline-end, which a boot reaches but the `SS_TEST_HEX` single-vector harness does not. Born from the NK spike where a KDP field had to be re-seeded after the nanokernel's own cold-init zeroing clobbered it. |
| `SS_ROM_LENIENT=1` | Force lenient ROM-patch mode for any NewWorld ROM (`rom_patches.cpp`). Pattern misses log `[ROMPATCH] SKIP` and continue instead of aborting. Permanent ROM-porting tool. See the "ROM patching diagnostics" section above. |
| `SS_ROM_PATCH_TRACE=1` | Log every `find_rom_data` pattern search as `[ROMPATCH] ... -> HIT/RELOCATED/MISS` (`rom_patches.cpp`). Independent of lenient mode. See the "ROM patching diagnostics" section above. |
| `SS_ROM_NO_904=1` | Opt out of checksum-based auto-lenient for the 9.0.4 G4 ROM. Does not affect `SS_ROM_LENIENT=1`. |
| `SS_NW_TRAMPOLINE=1` | Enable NewWorld nanokernel trampoline path (`rom_patches.cpp`). Gates the `[NW-MIRROR]` cold-start patch at ROM+0x46e8c0 and other NW-specific trampoline code. Required for New World ROM diagnostic boots. |
| `SS_TERM_DUMP=1` | SIGTERM → `exit(1)` so `timeout(1)`-killed diagnostic boots reach the atexit telemetry dumps (`[MMIO]`/`[VIA]`/`[VCLK]`/`[CUDA]`). See the M3b section below. |
| `SS_JIT_TRACE=/path` | Per-block execution trace to a file (`ppc-cpu.cpp`). Very verbose; the trace ring (`SS_JIT_TRACE_RING=1`) is usually the better tool. |
| `SS_JIT_CHAIN_LOG=1` | Log block-chaining/dispatch events (`ppc-cpu.cpp`). |
| `SS_JIT_CACHE_KB=<n>` | JIT translation-cache size in KB. Set automatically from the `jitcachesize` pref by the glue (pref is in bytes); an explicit env value overrides the pref. |
| `SS_JIT_MAX_INSNS=<n>` | Diagnostic cap on compiled block length (instructions per block). |
| `SS_JIT_INTERP_RANGE=lo-hi` | Force a guest-PC range (hex, `lo-hi`) to the interpreter — range-bisection tool for isolating a miscompiled region. |
| `SS_JIT_SKIP_OPC=n,n` / `SS_JIT_SKIP_XO=n,n` / `SS_JIT_SKIP_XO19=n,n` / `SS_JIT_SKIP_XO63=n,n` | Opcode-bisection: force the listed primary opcodes (or op-31 / op-19 / op-63 extended opcodes) to the interpreter instead of native codegen. |
| `SS_JIT_NO_OE=1` | Force the OE-form (overflow-recording) arithmetic variants (`addco`/`subfco`/… op-31 XO 522/520/778/552/616) to the interpreter — diagnostic only. |
| `SS_JIT_VERIFY_BUDGET=<n>` | With `SS_JIT_VERIFY=1`: divergence report budget before suppression (default 20). |
| `SS_JIT_VERIFY_PC=lo:hi` | With `SS_JIT_VERIFY=1`: restrict verification to a guest-PC range (hex `lo:hi`) — makes whole-boot verify runs tractable. |
| `SS_JIT_PROFILE_PC=hexpc[,…]` | With the profiler: print execution counts for SPECIFIC block PCs (not just the top-40). Companion of `SS_COPYBITS_TRACE`. |
| `SS_JIT_MEMDUMP=1` | Dump guest RAM at exit for interp-vs-JIT differential diffing (`ppc-cpu.cpp`). `SS_JIT_MEMDUMP_AT=0xPC` triggers the dump at a PC instead; `SS_JIT_MEMDUMP_PATH=/path` sets the output file. |
| `SS_JIT_RING_DUMP_AT_PC=0xPC` | One-shot trace-ring dump when execution reaches a PC; `SS_JIT_RING_DUMP_AT_PC_DELAY=<n>` defers it to the n-th visit. |
| `SS_JIT_RING_68K_MONITOR=1` | Watch guest `$28` (the A-line vector) for corruption; on change, dump the ring and exit (use with `SS_JIT_TRACE_RING=1`). Built for the 9.2.1-on-1.1-ROM post-splash stall hunt. |
| `SS_LOG_FIRST_BLOCKS=<n>` | Log the first n JIT block entries (`[FB k] pc=…`) — cheap early-boot trajectory capture. |
| `SS_LOG_ILLEGAL=1` | Log every undecoded opcode reaching the illegal handler (interpreter), incl. the mtmsr/MSR[VEC] AltiVec-detection probe. |
| `SS_LOG_PPCF=1` | Read-only Gestalt registration/dispatch logging (AltiVec-detection force experiment, task #26). No codegen effect. |
| `SS_STUB_TRACE=1` | Per-stub pressure counters (SPR/interpreter-fallback stubs) with an atexit dump; flips boot→steady at first guest idle. See `MMU-NANOKERNEL-MP-PLAN.md`. |
| `SS_COPYBITS_TRACE=1` | Resolve `_CopyBits` (trap 0xA8EC) once the System is up and log its RoutineDescriptor + PPC code entry, so a `SS_JIT_PROFILE`/`SS_JIT_PROFILE_PC` run can read the CopyBits call count from the per-block profiler. No guest patching. |
| `SS_FORCE_ALTIVEC=1` | Dev override: force the `altivec` pref ON even when absent/false (`=0` forces it off). The pref remains the user-facing opt-in. |
| `SS_TEST_DUMP=1` | Opcode-harness REGDUMP output (GPRs/CR/XER + all 32 FPR/VR) for `SS_TEST_HEX` runs — the differential-referee channel. |
| `SS_TEST_INIT=<32 hex words>` | Opcode harness: seed the 32 GPRs before executing the vector. |
| `SS_TEST_HEX_FILE=/path` | Opcode harness batch mode: run every vector in the file in ONE process (the `SS_HARNESS_BATCH=1` mechanism; see `jit-test/README.md`). |

### Machine-profile / machine-layer selection knobs

| Env var | Effect |
|---|---|
| `SS_MACHINE=paravirtual\|newworld` | Machine-profile selection (`machine_profile.cpp`); overrides the `machine` pref. `SS_NW_TRAMPOLINE=1` survives as a deprecated alias for `newworld` (warning printed). |
| `SS_MMIO_BUS=1` | Enable the MMIO bus on the paravirtual profile (the "named third config"); the newworld profile uses the bus unconditionally. |
| `SS_MMIO_STRICT=1` | Strict-fence mode for unclaimed MMIO accesses (resolved once at init — getenv is not Mach-handler-thread safe). |
| `SS_MMIO_THUNK_SELFTEST=1` | Self-test the JIT's MMIO backpatch thunk at JIT init. |
| `SS_NW_NO_SCC=1` | Restore the pre-M1 behavior (SCC base 0 → `check_work` returns -1) — isolates SCC-model regressions. |
| `SS_NW_MODEL=…` | Name-registry `compatible`-string injection experiment (default off; groundwork kept — necessary alongside, not sufficient by itself). |
| `SS_NW_SYNTH_ENTRY=1` | Synthetic-entry research diagnostic (`rom_patches.cpp`); dead-ends at the first Mixed-Mode transition — kept as a research tool only. |
| `SS_NW_FE1F_SURFACE=0` | **Opt OUT** of the FE1F service surface (newworld **default ON** since FE1F Task C, `be0e02cb` 2026-06-11). One gate covers both the raw-`twi` placeholder restore and the 0x700 program-interrupt delivery. Full reference: "Machine Layer M6 — FE1F service surface" below. |

## Machine Layer M2 — virtual clock and event scheduler diagnostics

### `[VCLK]` exit dump

On clean shutdown (or atexit), the virtual clock emits one stats line to stderr:

```
[VCLK] tb_freq=25000000Hz mfspr_dec=N mtspr_dec=N tb_writes=N dec_expiries=N pending=0
```

| Field | Meaning |
|---|---|
| `tb_freq` | TB/DEC tick rate in Hz (reflects the `cpuclock` pref; default 25 MHz) |
| `mfspr_dec` | Count of `mfspr DEC` reads routed through the virtual clock |
| `mtspr_dec` | Count of `mtspr DEC` writes honoured by the virtual clock |
| `tb_writes` | Count of `mttbl`/`mttbu` writes |
| `dec_expiries` | Count of DEC-expiry condition latches (eager from scheduler or lazy from read-side) |
| `pending` | 1 if the DEC condition was still latched at exit (consumed by M3a's delivery hook) |

**Paravirtual profile note:** `SS_SYNTH_DEC=1` on the paravirtual profile keeps the profile inert (no scheduler thread starts, no pump loop runs); the `[VCLK]` exit dump is NOT emitted in that case because the clock module is not initialized. Use the newworld profile to observe `[VCLK]` output.

### `[ESCHED]` pump line

The scheduler pump thread emits one line per cancelled timer when `cancel_all_timers()` is called at shutdown:

```
[ESCHED] Canceling timer id:N ns:TTTTTTTTTT
```

This is a normal shutdown message (the DEC eager-expiry one-shot and the VIA cyclic timers are cancelled). If you see many such lines in non-shutdown context, it indicates unexpected queue-flush (not a normal operating state).

### `SS_SYNTH_DEC` deprecation semantics (post-M2)

`SS_SYNTH_DEC` is now a **deprecated force-override** of the virtual clock:
- `SS_SYNTH_DEC=1` (or any non-zero value) on the newworld profile prints a deprecation warning and forces the pre-M2 synthetic free-running `0 - TB` counter behaviour. The virtual clock module is still initialized but the mfspr DEC path returns the legacy value instead of the real countdown. Use only as a diagnostic/escape hatch.
- `SS_SYNTH_DEC=0` on the newworld profile explicitly disables the force-override (same as not setting it); the virtual clock runs normally.
- **Historical note:** before M2, `SS_SYNTH_DEC=1` was the only way to get a moving DEC value. Any older recipe that sets it can be updated to remove it — the newworld profile now provides a real virtual clock unconditionally.

### Dump the trace ring from a running (or hung) process

With `SS_JIT_TRACE_RING=1`, the ring can be dumped from a **live** process without crashing
it — no SIGSEGV needed:

```bash
lldb -b -p $(pgrep -x SheepShaver) \
     -o "expression -- (void)ppc_jit_dump_trace_ring()" -o detach -o quit
```

Attach **at most once per run** and detach immediately — repeated lldb attach/detach can defer
the 60 Hz VBL timer and hang early boot (see the lldb/VBL caveat in `CLAUDE.md` / `LEARNINGS.md`).

## Machine Layer M3a — exception core + DEC delivery diagnostics

### `[EXC]` heartbeat field

M3a adds a compact exception counter to the periodic `[HB]` heartbeat (the `exc=…` suffix):

```
[HB 30s] blocks=812M (28.4M/s) comp=3214 | jNK=... | rss=412MB cpu=98% | exc=2/0/0/0/5/0
```

The six numbers are
`delivered/deferred_ee/deferred_depth/deferred_native/delivered_sc/delivered_program`,
accumulated since boot. The first four are the DEC exception class (the 4th field landed
with M6a rung-2 Task W2, `43d42b83` — logs older than that show the 3-wide `exc=N/N/N`
form); the 5th is the syscall class (landed with NK-syscall-surface Task A, `bcce26c2` —
older logs show the 4-wide form); the 6th is the program-interrupt (0x700) class (landed
with FE1F-service-surface Task A, `89a28c15`/`669ccf7a` — older logs show the 5-wide form):

| Subfield | Meaning |
|---|---|
| `delivered` | DEC exceptions delivered to the guest handler (KDP shim + ExcEnter applied) |
| `deferred_ee` | Deliveries skipped because `MSR[EE]=0` at the poll point (latch held; re-raised at EE 0→1 edges) |
| `deferred_depth` | Deliveries skipped because `execute_depth > 1` (inside a nested execute context; re-raised on return) |
| `deferred_native` | Deliveries deferred while a MixedMode **native excursion** is in flight — the M6a W2 DEC fence: `deliver_pending_dec_exception` defers while `[XLM_RUN_MODE]` (guest `0x2810`) `!= 0`. The NK maintains that word 1-forward/0-backward across exactly the MM switch pair, so a DEC cannot save into the MMCB mid-excursion. Known window (residue R-14): the word is 0 during the *backward* save — benign by same-values, recorded not fixed. |
| `delivered_sc` | **Delivered `sc` syscalls** (NK-syscall-surface Task A, plan rev 2 P-M4: counters for counts, probes for ABI). Incremented in `SheepExcSyscallShim` on every resolved-entry sc delivery; the first 5 also print `[EXC] SC delivered #N: …` to stderr (see below). With the surface opted out (`SS_NW_SC_SURFACE=0`) this stays 0 — the sc dies at the FATAL capture instead. **Beware the print cap when reading totals:** the long-quoted "5 sc deliveries" park baseline was the cap-5 PRINT artifact — the true parked total is 8 (7 distinct selectors); the FE1F-era default boot delivers 13 (9 distinct). Use this counter (or the `[EXC] sc selectors` per-selector line, below) for counts, never the printed lines. |
| `delivered_program` | **Delivered program interrupts (0x700)** — trap-taken `twi`/`tw` routed to `ExcEnter(EXC_PROGRAM)` (FE1F-service-surface Task A). Incremented in `SheepExcProgramShim`; the first 5 also print `[EXC] PROGRAM delivered #N: …` (see below). With the surface opted out (`SS_NW_FE1F_SURFACE=0`) this stays 0. A healthy FE1F-era default boot shows 2 (selector $31 then $36, both entry-vector slot 8). |

The same six counters are emitted as one `[EXC] delivered_dec=… deferred_ee=…
deferred_depth=… deferred_native=… delivered_sc=… delivered_program=…` line on the
crash-path dump (newworld only — paravirtual crash output stays byte-identical).

On the paravirtual profile the suffix is omitted (`exc=NULL`). Telemetry rides the heartbeat
rather than `atexit` because `SIGALRM` from the `perl alarm` wrapper skips `atexit` dumps.

### `[EXC]` delivery and FATAL lines

```
[EXC] DEC delivered #N: restart=PPPPPPPP srr1=SSSSSSSS msr=MMMMMMMM -> entry=EEEEEEEE
```
Emitted to stderr on the **first 5 deliveries only** (capped; the heartbeat `exc=` field
is the ongoing counter). Fields: `restart` = block-start PC (the not-yet-executed restart
address stored in SRR0); `srr1` = composed SRR1 (msr & 0x0000FFFF); `msr` = new guest MSR
after the exception-entry transform; `entry` = guest handler PC.

```
[EXC] FATAL: DEC delivery with unresolved interrupt entry (restart=PPPPPPPP msr=MMMMMMMM) - check SS_EXC_ENTRY
```
(Verbatim grep-able string.) Emitted and aborted if the entry table's interrupt_entry is
unresolved when a delivery is attempted. Use `SS_EXC_ENTRY` to override without a rebuild.

```
[EXC] SC delivered #N: r0=SSSSSSSS r1=RRRRRRRR lr=LLLLLLLL -> entry=EEEEEEEE
```
Emitted on the **first 5 syscall deliveries only** (NK-syscall-surface Task A; the
heartbeat's 5th `exc=` field is the ongoing counter). `r0` = the NK syscall selector,
`r1`/`lr` = the caller values the 2-SPR shim latched into SPRG1/SPRG2, `entry` = the
resolved NK handler (default `0x50314ac0`).

```
[EXC] FATAL: sc at pc=PPPPPPPP with unresolved syscall entry (SRR0=SSSSSSSS SRR1=TTTTTTTT msr=MMMMMMMM lr=LLLLLLLL r1=RRRRRRRR) - set SS_EXC_ENTRY or SS_EXC_SC=legacy
[EXC] FATAL: sc capture: r0=XXXXXXXX r3=XXXXXXXX r4=XXXXXXXX r5=XXXXXXXX r6=XXXXXXXX r7=XXXXXXXX r8=XXXXXXXX r9=XXXXXXXX r10=XXXXXXXX
```
(Verbatim grep-able strings.) Emitted and aborted when `execute_syscall` fires on the
newworld profile and `syscall_entry == 0`. **Since the NK-syscall-surface milestone
(2026-06-11) the entry is RESOLVED by default — this FATAL pair is now the OPTED-OUT
baseline (`SS_NW_SC_SURFACE=0`), not the default behavior.** The second line (the
extended capture, Task 0 commit `780bbc34`) samples the dying sc's selector (`r0`) and
argument registers (`r3..r10`) — the conformance-vector instrument that pinned the
first guest syscall (selector 0x3f). `SS_EXC_SC=legacy` falls back to the old no-op
behavior without a rebuild (only meaningful on the unresolved-entry path — see the
M6 syscall-surface section below).

```
[EXC] PROGRAM delivered #N: srr0=SSSSSSSS word=WWWWWWWW slot=S r1=RRRRRRRR lr=LLLLLLLL -> entry=EEEEEEEE
```
Emitted on the **first 5 program-interrupt (0x700) deliveries only** (FE1F-service-surface
Task A; the heartbeat's 6th `exc=` field is the ongoing counter). `srr0` = the trap
instruction address (SRR0 points AT the offending instruction per PEM; for the entry-vector
placeholders this is the slot address, e.g. `5046e8e0` = slot 8); `word` = the trap
instruction word (`0x0fff000N` = `twi 31,r31,N`, encoding slot id N — `slot=n/a` for
non-placeholder trap words); `r1`/`lr` = the caller values the 2-SPR shim latched into
SPRG1/SPRG2 (`lr` = the FE1F body's post-`blrl` return on the callout path); `entry` = the
resolved 0x700 handler (default `0x50314700`).

```
[EXC] PROGRAM slot-15 (DR allocator EXHAUSTION) #N: srr0=SSSSSSSS — pool-sizing tripwire (was Task T's parked stop; now delivered to the NK slot-15 exit)
```
Slot-15 exhaustion telemetry: rung-2 Task T's allocator-exhaustion parked stop moved here
when the placeholders were restored (slot 15 IS a raw trap-placeholder; exhaustion now
reaches the NK's own slot-15 exit). Emitted on EVERY slot-15 delivery (not capped) —
grep for it as the MixedMode save-record pool-sizing tripwire.

```
[EXC] sc selectors (arrival order, distinct=D): 0xSS xN 0xSS xN …
```
Per-selector sc delivery counts (FE1F-service-surface Task C fix-budget item, `2949ec32` —
counters-for-counts): the first 16 DISTINCT selectors in arrival order with delivery
counts (`(+N deliveries beyond 16 distinct)` if overflowed). Emitted once at exit
(atexit) AND explicitly on the crash path (atexit does not fire on SIGSEGV). This closed
the cap-5 print artifact: the parked baseline is `0x3f x1 0x19 x2 0x14 x1 0x0f x1
0x27 x1 0x40 x1 0x42 x1` (8 deliveries, 7 distinct); the FE1F-era default boot adds
exactly 5 (`0x0f` +2, `0x42` +1, +`0x50`, +`0x4d`) → 13 deliveries, 9 distinct.

### M3a exception-delivery env vars

| Env var | Effect |
|---|---|
| `SS_EXC_ENTRY=0xINT[,0xSC]` | Override the interrupt entry address (and optionally the syscall entry) without rebuilding. Hex; comma-separated. Takes precedence over the `SS_NW_SC_SURFACE` gate (the designed no-rebuild probe channel). **Fixed trap (NK-syscall-surface Task A):** the no-comma `SS_EXC_ENTRY=0xINT` form now PRESERVES the default syscall entry — it used to zero it, which post-flip would have silently re-broken the resolved syscall surface. Only an explicit `,0xSC` field overrides the syscall entry. |
| `SS_EXC_SC=abort\|legacy` | Controls what `execute_syscall` does on newworld when `syscall_entry` is **unresolved**. `abort` (default): SRR-capture + context print then abort. `legacy`: fall back to the old `execute_illegal` + ad-hoc PC-bump behavior (the pre-M3a no-op path). **Inert on the default config since the syscall surface resolved** — a loud `[EXC] WARNING: SS_EXC_SC=legacy is INERT …` line is printed when set with a resolved entry; see the M6 syscall-surface section for the reproduction recipe. |
| `SS_EXC_BARE=1` | Skip the KDP register-save shim before `ExcEnter` — the bounded direct-entry experiment. Without the shim the handler prologue reads uninitialized context-block fields; use only with a handler known not to dereference r6. |

**Note:** `SS_EXC_FORCE` (deliver once ignoring MSR[EE]) was planned as a debug knob but
was **not implemented** — the heartbeat `exc=0/1/0` deferral telemetry provided equivalent
evidence without it, so the knob was dropped as moot.

### SCC Rx injection (`SS_SCC_RX_INJECT`)

```
SS_SCC_RX_INJECT=DELAY_S:HEXBYTES
```

Inject bytes into the SCC channel-A Rx FIFO after `DELAY_S` seconds of guest execution.
`HEXBYTES` is a hex string (no `0x` prefix, no spaces; e.g. `0D` for one CR, `68656C6C6F0D`
for "hello\r"). Maximum 16 bytes per injection (the FIFO capacity). Newworld profile +
bus-active config only; paravirtual is ungated and silently ignored.

**Demo recipe** (the M3a end-to-end demonstration from commit a2dd1ff8):

```bash
# 9.0.1 diagnostic boot; inject one CR at T+25s to wake the NK Thud console:
SS_ROM_LENIENT=1 SS_ROM_SKIP_JUMP68K=1 SS_SCC_RX_INJECT=25:0D \
  ./SheepShaver --config /tmp/m2accept.prefs 2>&1 | grep -E '\[HB|EXC\]'
```

Expected output (two pre-injection HBs with comp frozen at 781, two post-injection HBs
with comp 791):

```
[HB ...] ... comp=781 | ... | exc=0/1/0
[HB ...] ... comp=781 | ... | exc=0/1/0
[HB ...] ... comp=791 | ... | exc=0/1/0
[HB ...] ... comp=791 | ... | exc=0/1/0
```

The `comp` jump (781→791, 10 new blocks) is the machine-layer composition signal: M2
scheduler → M1 bus/backpatch → SCC Rx → `check_work` → console. The `exc=0/1/0` pattern
(deferred_ee=1) is the cold-MSR-EE fix working correctly — DEC deferred during NK cold-init,
delivered once EE is enabled by the guest.

## Machine Layer M3b — Cuda + ADB stub diagnostics

### `[CUDA]` stats line

On clean shutdown (atexit, newworld-bus configs only — paravirtual never registers it) and
on the crash-path dump, the Cuda protocol model emits one stats line to stderr (suppressed
entirely if the guest never touched the SR/ORB seam):

```
[CUDA] packets=N responses=N syncs=N bytes_in=N bytes_out=N adb=N adb_absent=N
       get_time=N set_time=N autopoll=N pram_rd=N pram_wr=N i2c=N
       i2c_absent=N(addrs=AA,BB,...) acked=N bad_param=N unknown=N(last=T:CC)
       resets=N powerdowns=N overflows=N
```

| Field | Meaning |
|---|---|
| `packets` / `responses` | Complete command packets committed by the host / response packets queued by the model |
| `syncs` | Sync/attention sequences completed (TACK asserted with TIP negated — the startup handshake; a probe-cycling boot re-syncs each cycle) |
| `bytes_in` / `bytes_out` | SR bytes shifted host→Cuda / Cuda→host |
| `adb` / `adb_absent` | ADB packets dispatched to the adb_stub / Talks to absent addresses (framed as timeout status 0x02) |
| `get_time` / `set_time` / `autopoll` | RTC reads/writes and autopoll-control commands |
| `pram_rd` / `pram_wr` | READ_PRAM / WRITE_PRAM (+MCU_MEM) commands served from the in-memory 256-byte PRAM |
| `i2c` / `i2c_absent=N(addrs=…)` | READ_WRITE_I2C (0x22) + COMB_FMT_I2C (0x25) transactions; `addrs` is the capture-only probe map of raw I2C address bytes the boot swept (all answered absent — `CUDA_ERR_I2C` — until a device is modeled) |
| `acked` / `bad_param` | Simple-ack commands (FILE_SERVER_FLAG etc.) / malformed-parameter rejections |
| `unknown=N(last=T:CC)` | Unknown commands, with the most recent one named: `T` = packet type, `CC` = command byte, both hex. A nonzero counter is the "boot demands a command we don't model" signal (this is how 0x22/0x25 were found) |
| `resets` / `powerdowns` | RESET_SYSTEM / POWER_DOWN commands (latched loud — the model acks but does not act) |
| `overflows` | Input-packet buffer overflows (should be 0) |

**Stale-object tripwire:** an absurd counter value (e.g. `powerdowns=6114308096`) means a
stale `.o` compiled against an older `CudaDevice` struct layout, not a logic bug — the Unix
build does not track header dependencies. Force-remove the consuming objects
(`main_unix.o`, `dev_via6522.o`, `sheepshaver_glue.o`) and rebuild.

A one-shot `[CUDA] warning: …` line may precede the stats: warnings are latched (not
printed) on seam paths where stdio is forbidden (§2g, fault-thread reachable) and drained
at exit.

`[CUDA] model bound to via6522 SR/ORB seam (lazy-only; ADB stub kbd@2 mouse@3)` at startup
confirms the bring-up (newworld profile only).

### `SS_CUDA_TRACE=1` — first-packets capture (`[CUDA-TRACE]`)

Latches the first 64 complete Cuda packets (command bytes in + response bytes out,
truncated to 24 bytes each; untruncated lengths recorded) into a static ring. Dumped as
`[CUDA-TRACE NN] in(LEN): xx xx … out(LEN): xx xx …` lines on the crash/term-dump path
only (pair with `SS_TERM_DUMP=1` for timeout-killed boots). The capture side is §2g-safe
(no stdio/malloc/locks — fault-thread reachable). This is the packet-level forensics that
root-caused the M3b probe-cycle loop; zero cost when unset.

### `[VIA] orb:` write-value trace (C3 polarity forensics)

Next to the per-register `[VIA] reads:` histogram, the VIA dumps the ORB **write-value
transition trace** at exit:

```
[VIA] orb: ddrb=30 writes=4 trace=38,28,30
```

| Field | Meaning |
|---|---|
| `ddrb` | Data-direction register B (0x30 = bits 4/5 outputs, bit 3 input — the Cuda-polarity engine: TREQ=3 input, TACK=4, TIP=5, all active-LOW) |
| `writes` | Total ORB writes |
| `trace` | The sequence of *distinct* written byte values (consecutive duplicates collapsed, bounded buffer) |

This exists because the 68k handshake engines run under the DR emulator and are invisible
to `SS_PROBE_PC` (zero PPC block-entry hits) — the written bit pattern is the only direct
evidence of which handshake engine (Cuda vs Egret polarity) is live. `38,28,30` is the
canonical sync choreography (idle → TACK assert → TACK negate).

### `SS_TERM_DUMP=1` — atexit dumps on timeout-killed boots

`timeout(1)`-killed diagnostic boots die by SIGTERM, which skips the atexit telemetry
(`[MMIO]`/`[VIA]`/`[VCLK]`/`[CUDA]` dumps). `SS_TERM_DUMP=1` installs a SIGTERM handler
that calls `exit(1)`, so the dumps run. `exit()` from a signal handler is async-unsafe in
general; acceptable for one-shot teardown on this env-gated diagnostics path (default
behavior unchanged). Standard recipe:

```bash
SS_TERM_DUMP=1 timeout 60 ./SheepShaver --config /tmp/m2accept.prefs 2>/tmp/diag.log
```

### `[M3b]` retirement banners (cuda_init / adb_init)

On the newworld profile, `rom_patches.cpp` no longer applies the `cuda_init_dat` and
`adb_init_dat` patches; each emits one banner reporting whether the retirement changed
anything **on this ROM**:

```
[M3b] cuda_init ROM patch retired (newworld profile, pattern found - patch suppressed): guest Cuda init runs against the dev_cuda model
[M3b] adb_init ROM patch retired (newworld profile, pattern absent - no-op on this ROM): ADBInit wait runs against the dev_cuda model + adb_stub
```

`pattern found - patch suppressed` = the retirement is live behavior change (1.1/OldWorld-
window ROMs). `pattern absent - no-op on this ROM` = the pattern misses its search window
(the 9.0.1 parcels ROM: cuda_init @0x9be2, adb_init @0x2b780 — both outside) so the inits
always ran unpatched there; the banner just records that fact. Paravirtual keeps both
patches forever (same gate idiom as scc_init/via_init).

### PRAM note — two divergent PRAM sources until M4 (deliberate)

Wave 1's Cuda serves READ_PRAM/WRITE_PRAM from an **in-memory, zero-initialized** 256-byte
array (well-formed full-length responses; no persistence), while the `nvram1`–`nvram7`
XPRAM/NVRAM EMUL_OP HLE patches **stay applied** and serve the host-file-backed XPRAM.
These are two divergent PRAM stores — a guest writing through one path will not see it
through the other. This inconsistency window is deliberate and closes at M4 (full
partitioned NVRAM behind the bus, EMUL_OP HLE retired).

## Machine Layer M6a rung 2 — Mixed Mode switch knobs + capture telemetry

The 68k→PPC Mixed Mode switch (FE01 forward switch → TVector execution → world-flip
switch-back) is **complete and is the newworld profile DEFAULT** since rung-2 Task Y
(`296c3661`). Acceptance record: `docs/planning/machine/M6A-ONGOING-ENTRY-DESIGN.md`
(Task T/V/W/W2/X/Y results sections). All knobs are newworld-profile-only; paravirtual
and OldWorld are untouched.

### Switch / pool knobs (`rom_patches.cpp`, `PatchROM_NW_trampoline`)

| Env var | Effect |
|---|---|
| `SS_NW_MM_SWITCH=0` | **Opt OUT** of the Mixed Mode switch completion (newworld **default ON**). Restores the pre-switch baseline byte-identically — FE01↔NK retry spin, 0 TVector visits, no W/W2 region writes, no slot-1 retarget — for A/B comparison. The switch **implies the pool** (switch-without-pool is not a supported config). `=1` remains valid explicit-on. |
| `SS_NW_MM_POOL=0` | **Opt OUT** of the DR Mixed Mode save-record pool seed (newworld **default ON** since Task T, `735f775c`; 4 × 0x220 records at `0x68FF5800` per the sub-KDP occupancy map in M6A-ONGOING-ENTRY-DESIGN). Pool-off under the (default-on) switch is a **MISCONFIGURATION**: a loud `[NW-TRAMP] V: MISCONFIG` line is printed and the pool is forced back on — a pool-off A/B requires `SS_NW_MM_SWITCH=0` as well. With both off, the first FE01 parks loudly at the slot-15 exhaust stop `0x50429cf0` (NOT the old silent ~80 ms reboot loop). `=1` remains valid explicit-on. |
| `SS_M6A_USER_MSR=1` | **Quarantined known-broken diagnostic** — do not use for new work. The Q-E verdict (rung-2 Task 0) is that the architectural MSR transition is per-context, carried by the NK context switch; no trampoline `mtmsr` is needed, and `=1` reproduces an un-root-caused zero-page slide (residue R-2). Under the (default-on) switch it is **ignored loudly** (`[NW-TRAMP] V: SS_M6A_USER_MSR=1 ignored …`). Default OFF. |

The `[NW-TRAMP]` fixup banner at patch time reports the resolved state
(`… user-msr=off mm-pool=ON mm-switch=ON …`) — grep it to confirm what a boot ran with.

### Capture-only telemetry rings (M6a Wave-2 recon tools, `ppc-cpu.cpp`; zero cost unset)

| Env var | Effect |
|---|---|
| `SS_DR_R24_RING=1` | 68k-PC transition ring: records guest **r24** (the DR emulator's 68k PC) at every JIT dispatcher block entry into a 2M-entry ring, deduped against the last 4 recorded values (loop ping-pong suppressed — beware: a literal resume PC can be dedup-masked if it recurs within 4 entries). Dumped as a `[R24RING] N transitions recorded …` block to stderr at exit (pair with `SS_TERM_DUMP=1` for timeout-killed boots). **This is the only way to observe 68k control flow** — 68k PCs are PPC-probe-blind (`SS_PROBE_PC` keys on PPC block-entry PCs). |
| `SS_INTERP_RING=1` (or `=2`, or `=/path.bin`) | Interpreted-insn + JIT-block-entry ring (1M entries, 16 MB binary atexit dump, default `/tmp/ss_interp_ring.bin`; `[IRING]` summary line to stderr). Records `{pc, opcode, ea, val}` for every interpreted PPC instruction (`ea`/`val` = effective address + pre-execution guest word for common loads/stores) plus RAM-range JIT block entries (marker records `op=0xffffffff, ea=r24, val=LR`) — built because the 'pwpc' parcel code runs where `SS_PROBE_PC` can't see. Mode `=2`: record EVERY JIT block entry and **freeze** the ring at the first r24 reset transition (`0x5000002c`) once half-full, so the dump ends exactly at a reboot-loop bail; use with `SS_JIT_NO_CHAIN=1` so chained blocks can't bypass the hook. |

## Machine Layer M6 — NK syscall surface (`SS_NW_SC_SURFACE`)

The NK syscall surface (vector 0xC00) is **complete and is the newworld profile DEFAULT**
since NK-syscall-surface Task C (`7a079079`, 2026-06-11). `g_exc_entry_table.syscall_entry`
defaults to **`0x50314ac0`** — the staged NK's own syscall handler in the PRIMARY copy,
NK-published at `[KDP+0x390]` (deliberate cross-copy asymmetry: `interrupt_entry` stays
the staged-copy `0x50412b1c`; both copies are byte-identical, the syscall entry follows
what the NK itself publishes). Delivery = bare `ExcEnter(EXC_SC)` (SRR0=sc+4, PEM masks)
plus a **2-SPR shim** transcribed from the real 0xC00 vector stub's postconditions:
`SPRG1 := caller r1`, `SPRG2 := caller LR` — nothing else. Acceptance record:
`docs/planning/machine/M3A-ENTRY-TABLE.md` "Syscall entry resolution" + Task B/C results.
All knobs newworld-profile-only; paravirtual and OldWorld untouched.

### The knob

| Env var | Effect |
|---|---|
| `SS_NW_SC_SURFACE=0` | **Opt OUT** of the syscall surface (newworld **default ON**; explicit-`"0"`-only opt-out, polarity mirroring `SS_NW_MM_SWITCH`). Restores the abort-with-capture baseline byte-identically: `syscall_entry=0`, the first sc dies on the `[EXC] FATAL: sc …` pair (incl. the r0/r3..r10 capture line), exit 134/SIGABRT. `=1` remains valid explicit-on. A loud `[NW-SC] syscall surface OFF …` line announces the opt-out. |

The `[NW-SC] syscall surface armed (newworld default; opt-out SS_NW_SC_SURFACE=0):
entry=0x50314ac0 …` line at table-finalization time reports the resolved state — grep it
(or the `[EXC] entry table:` line) to confirm what a boot ran with.

### Override × gate interaction (`SS_EXC_ENTRY` precedence)

Precedence: `SS_EXC_ENTRY` > `SS_NW_SC_SURFACE` default > 0. The pinned 2×2:

| Config | Resulting table `{interrupt, syscall}` |
|---|---|
| gate=0, no override | `{0x50412b1c, 0}` — FATAL sc baseline |
| default, no override | `{0x50412b1c, 0x50314ac0}` — surface armed |
| gate=0, `SS_EXC_ENTRY=I,S` | `{I, S}` — the override is active REGARDLESS of the gate (the designed no-rebuild probe channel) |
| default, `SS_EXC_ENTRY=I` (no comma) | `{I, 0x50314ac0}` — the no-comma form PRESERVES the syscall default (**fixed trap**: it previously zeroed it, which would have silently re-broken the surface post-flip) |

### `SS_EXC_SC=legacy` is inert on the default config

`SS_EXC_SC=legacy` only acts on the UNRESOLVED-entry path inside `execute_syscall`. With
the entry resolved (the default), setting it prints one loud warning instead of silently
doing nothing:

```
[EXC] WARNING: SS_EXC_SC=legacy is INERT — syscall entry resolved (0x50314ac0); legacy no-op applies only to the unresolved path (reproduce the legacy datum with SS_NW_SC_SURFACE=0 SS_EXC_SC=legacy)
```

**Reproduction recipe for the legacy datum** (the 52M/s comp-frozen spin at comp=3672 —
the caller polling the r3 syscall result the no-op never produced; historical evidence,
not a bridge):

```bash
SS_NW_SC_SURFACE=0 SS_EXC_SC=legacy ./SheepShaver --config /tmp/m2accept.prefs
```

### What a healthy default boot shows

5 printed sc deliveries in the first 65s (selectors 0x3f/0x19/0x14/0x19/0xf — `[EXC] SC
delivered #1..#5` lines; the prints cap at 5), every sampled resume r3=0, no `[EXC] FATAL`,
`exc=` 5th field counting. **True totals (per-selector counter, FE1F era): 13 deliveries,
9 distinct on the default config; 8 deliveries, 7 distinct with `SS_NW_FE1F_SURFACE=0`.**
The post-sc regime is heartbeat-SILENT (the boot leaves the dispatch-loop heartbeat path) —
capture term baselines via `SS_TERM_DUMP=1` + SIGTERM kills (`timeout(1)`); SIGALRM
(`perl alarm`) skips ALL atexit dumps.

## Machine Layer M6 — FE1F service surface (`SS_NW_FE1F_SURFACE`)

The FE1F service surface — the DR emulator's generic 68k→native callout opcode `$FE1F`,
serviced through the NK's own program-interrupt dispatch — is **complete and is the
newworld profile DEFAULT** since FE1F-service-surface Task C (`be0e02cb`, 2026-06-11).
One gate covers two pieces:

1. **Placeholder restore** (`rom_patches.cpp`, PatchROM-time): the raw ROM's
   `twi 31,r31,N` entry-vector placeholders (`0x0fff000N`, file 0x36e8c0+4N; slot 14
   duplicates 0x0d — the ROM's own quirk, restored verbatim) are restored over rung-2
   Task T/U's parked stops in mirror slots {4, 6–15}, verify-EXPECTED (current word must
   be the exact stop branch). The placeholders are load-bearing: executing a slot raises
   a program interrupt and the NK's published 0x700 handler decodes the slot id and
   dispatches through the exit-pointer array `[KDP+0x5f0+4·slot]`. The rung-2 "dead
   slots get loud stops" policy is RETIRED (dated note at the rom_patches site).
2. **0x700 delivery surface** (`sheepshaver_glue.cpp` + `exc_core`): trap-taken
   `twi`/`tw` in `execute_illegal` routes to `ExcEnter(EXC_PROGRAM)` →
   `program_entry = 0x50314700` (primary copy, NK-published `[KDP+0x37c]` [PROBE✓]),
   with `SheepExcProgramShim` — the sc-shim's sibling, exactly two SPR writes
   (`SPRG1 := caller r1`, `SPRG2 := caller LR`). SRR0 = the trap instruction verbatim;
   SRR1 = `(msr & 0xFFFF) | 0x00020000` (PEM program-interrupt trap bit, test-pinned in
   `test_exc_core`). Trap-taken with the entry UNRESOLVED (opted out) dies on a loud
   `[EXC] FATAL` capture-abort naming the opt-out.

All knobs newworld-profile-only; paravirtual and OldWorld untouched (all lines inside
`MachineProfileIsNewWorld()` / `PatchROM_NW_trampoline` blocks).

### The knob

| Env var | Effect |
|---|---|
| `SS_NW_FE1F_SURFACE=0` | **Opt OUT** of the FE1F surface (newworld **default ON**; explicit-`"0"`-only opt-out, polarity mirroring `SS_NW_SC_SURFACE` — NOT MachineEnvFlag). Restores the 0x5000f248 park baseline byte-identically: Task-T/U parked stops stay in the slots, `program_entry=0`, ring tail `… 5000f242 5000f246 5000f248` at 839,284 transitions, sc selectors 8/7-distinct, zero PROGRAM deliveries. `=1` remains valid explicit-on. A loud `[NW-FE1F] FE1F surface OFF …` line announces the opt-out. |

Grep `[NW-FE1F]` to confirm what a boot ran with: the armed line
(`[NW-FE1F] FE1F surface armed (newworld default; opt-out SS_NW_FE1F_SURFACE=0):
program_entry=0x50314700 …`) plus the restore line (`[NW-FE1F] raw twi placeholders
RESTORED … slots {4,6-15} (mask=0x…, expected 0xffd0 …)`).

### Dependency matrix (`SS_NW_MM_SWITCH` / `SS_NW_SC_SURFACE`)

The FE1F surface is **MEANINGLESS with `SS_NW_MM_SWITCH=0` or `SS_NW_SC_SURFACE=0`** —
the boot never reaches the FE1F callout without the MixedMode switch + the sc surface
(the CFM-prep routine that issues FE1F sits 5 printed sc deliveries past the MixedMode
round trip). Combined-opt-out behavior (pinned, rev 2 P-m5): the surface still ARMS
(harmless — the twi sites are unreachable on such a boot) but logs the misconfiguration
loudly:

```
[NW-FE1F] MISCONFIG: FE1F surface armed with SS_NW_MM_SWITCH and the sc surface OFF — the boot cannot reach the FE1F callout; surface stays armed but inert (P-m5)
```

A pre-FE1F A/B wants the upstream knob (`SS_NW_MM_SWITCH=0` or `SS_NW_SC_SURFACE=0`),
not this one.

### What a healthy FE1F-era default boot shows

Two `[EXC] PROGRAM delivered` lines (#1 selector $31, #2 selector $36 — both slot 8,
`srr0=5046e8e0 word=0fff0008 … -> entry=50314700`); the selector-0x31 round trip
returns r3=0/r4=handle (e.g. `0x00120001`, NK kernel-object ID `(dir-index<<16)|gen`)
and the 68k stores it into the ExpandMem slot (`[0x100037dc]`); sc selectors 13/9-distinct;
ring total ~4.48M records (vs 839k parked). The boot then dies at the **DSAT
stack-underflow wall** (System Error ID 10; alert machinery underflows RAMBase → host
SIGSEGV `ea=0x…0fffff42` class) — the named frontier as of 2026-06-11, captured in
`docs/planning/machine/M6A-WAVE2-SHIM-RECON.md` "Frontier update (FE1F Task C closeout)".
