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
| `SS_JIT_WATCH_ADDR=dec,dec` | Guest-memory watchpoints (decimal, comma-separated). Emits `[WATCH] pc=PPPPPPPP addr=AAAAAAAA value=VVVVVVVV  (was WWWWWWWW ...)` on each detected change. PC is block-entry granularity for JIT, exact instruction for interpreter. Grep for `[WATCH]` to parse programmatically. |
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
[HB 30s] blocks=812M (28.4M/s) comp=3214 | jNK=... | rss=412MB cpu=98% | exc=2/0/0
```

The three numbers are `delivered/deferred_ee/deferred_depth` for the DEC exception class,
accumulated since boot:

| Subfield | Meaning |
|---|---|
| `delivered` | DEC exceptions delivered to the guest handler (KDP shim + ExcEnter applied) |
| `deferred_ee` | Deliveries skipped because `MSR[EE]=0` at the poll point (latch held; re-raised at EE 0→1 edges) |
| `deferred_depth` | Deliveries skipped because `execute_depth > 1` (inside a nested execute context; re-raised on return) |

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
[EXC] FATAL: sc at pc=PPPPPPPP with unresolved syscall entry (SRR0=SSSSSSSS SRR1=TTTTTTTT msr=MMMMMMMM lr=LLLLLLLL r1=RRRRRRRR) - set SS_EXC_ENTRY or SS_EXC_SC=legacy
```
(Verbatim grep-able string.) Emitted and aborted when `execute_syscall` fires on the
newworld profile and `syscall_entry == 0` (default: no sc entry resolved in Task 0).
`SS_EXC_SC=legacy` falls back to the old no-op behavior without a rebuild.

### M3a exception-delivery env vars

| Env var | Effect |
|---|---|
| `SS_EXC_ENTRY=0xINT[,0xSC]` | Override the interrupt entry address (and optionally the syscall entry) without rebuilding. Hex; comma-separated. Useful for iterating on entry-table values after Task 0 recon. |
| `SS_EXC_SC=abort\|legacy` | Controls what `execute_syscall` does on newworld when `syscall_entry` is unresolved. `abort` (default): SRR-capture + context print then abort. `legacy`: fall back to the old `execute_illegal` + ad-hoc PC-bump behavior (the pre-M3a no-op path). |
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
