# Agent Context Pack — the standing facts every machine-layer agent needs

> Read this ONCE at task start instead of re-deriving from four docs. Maintained by the
> coordinator; facts here are current as of the last commit touching this file. When a
> task prompt conflicts with this pack, the prompt wins (it's newer).

## Boot recipes

- **Slot protocol (DEFAULT — never global `pkill`)**:
  `SheepShaver/tools/ss-slot-boot.sh --timeout 45 --label <task> --env 'SS_PROBE_PC=…'`
  (acquires a lease under /tmp/ss-slots/, per-slot prefs/logs/diag, SIGTERM at deadline
  so atexit dumps fire, prints SLOT/RUNDIR/LOG). Reap strays: `SheepShaver/tools/ss-reap.sh`.
  Full doc: `SheepShaver/tools/README-slots.md`. Concurrent boots are SAFE (proven).
- Default diagnostic config = newworld, 9.0.1 ROM, nogui, no disk (the wrapper's default
  template). Standard env is baked in (SS_TERM_DUMP, SS_NW_TRAMPOLINE, SS_ROM_LENIENT).
- Capture discipline: SIGTERM (the wrapper does this), never SIGKILL/SIGALRM — they skip
  the atexit telemetry dumps. Heartbeat silence in late-boot regimes is normal; the
  term-dump is the capture.

## Instruments (caveats are load-bearing)

- `SS_PROBE_PC=0xPC[:fields][;…]` — PPC block-entry only (max 8 PCs/run, logarithmic
  sampling: visits 1,10,100…). Fields: rN, [0xADDR], [rN:SIZE]. **68k PCs are
  probe-blind for SS_PROBE_PC** — use `SS_PROBE_68K=0x68KPC[:N]` (68k regfile + PPC
  context at the DR dispatch hook, first N matches linear, edge-triggered; r24 word+2:
  to catch word X probe X+2) or `SS_DR_R24_RING=1` (ring is 2M entries, last-4 dedup —
  dedup now COUNTS: dump prints `PC*N` for suppressed repeats; also flushed on SIGSEGV
  via the trace-ring crash dump).
  **Probes cannot count** — land a counter (the `exc=` tuple idiom) for counts; or
  `SS_PROBE_LINEAR=1` (+`SS_PROBE_CAP=N`, default 32) fires probes on EVERY visit up
  to the cap so short sequences become readable.
- Crash-boot ring reach: `SS_RING_WINDOW=0xN` overrides the SS_JIT_TRACE_RING capacity
  (default 0x40000; 0x200000 is the smallest that retains the DSAT ~3.39M window at the
  ~4.48M crash, 108 B/record) and `SS_RING_DUMP_FROM=0xSTART[:0xEND]` clips the dump to
  absolute record numbers (dump file now has a `# records #A..#B …` header line).
  Full retention math: DIAGNOSTICS.md "Instrument batch".
- `SS_JIT_WATCH_ADDR=<HEX,no-0x>` — parser is HEX; requires `SS_JIT_TRACE_RING=1`.
- `SS_SEED_MEM`, `SS_EXC_ENTRY=0xINT[,0xSC]` (no-comma form preserves the syscall
  default), `SS_CUDA_TRACE=1`, `SS_INTERP_RING` — see DIAGNOSTICS.md.
- ROM dumps — canonical location: **`/Users/Shared/macemu/dumps/`** with `MANIFEST.txt`
  (filename + md5 + provenance). `rom901_inventory.bin` = RAW (md5 7b1378be…, 16
  placeholder words at file 0x36e8c0); `rom901.bin` = PATCHED (md5 e432df64…). The /tmp
  copies remain valid but ephemeral. Provenance ritual before tagging [RAW-ROM]/[PATCH]:
  `tools/dump-manifest.sh --check`. Mirror-region facts are [PROBE✓]-only (no dump covers
  the mirror). capstone-M68K mis-decodes `fe1f`-style words — use `tools/m68k-dis.py`
  (boundary-splits A/F-line + 0x0fff words); PPC is BE.

## Constants (probe-ready absolutes)

ECB=0x68fff000 · MMCB=0x68fff400 · KDP=0x68ffe000 ([KDP+X] = 0x68ffeXXX… compute) ·
XLM_RUN_MODE=0x2810 · XLM_IRQ_NEST=0x2818 · ROMBase=0x50000000 · mirror emulator base
0x50460000 · DR dispatch table 0x50480000 (slot = base | opcode<<3) · entry-vector
table 0x5046e8c0 (16 slots) · NK primary 0x5031xxxx / staged +0x100000 · guest→host:
add NATMEM_OFFSET 0x400000000000. DR 68k register map: r8–r15=D0–D7, r16–r22=A0–A6,
r1=A7, r24=68k PC.

## Gate tiers (see MILESTONE-WORKFLOW.md §6 for the policy)

- **Per-commit (inner)**: `make build-ss` + `SS_HARNESS_BATCH=1 make test-jit` (353/353)
  + `make -C src/machine test` (ALL PASS). ~1 minute warm.
- **Per-task (final commit)**: + plain `make test-jit` (authoritative) + `make e2e-test`.
- **Risk-based**: paravirtual `make e2e` — REQUIRED when the change touches code
  reachable on paravirtual (shared functions, non-gated lines); SUBSTITUTABLE by the
  structural-inertness argument + the gated-off byte-identical A/B boot when every new
  line is inside MachineProfileIsNewWorld()/env gates (state which in the commit).
- Doc-only commits: no gates.

## Standing rules (the short list)

Explicit-path staging only (never `git add -A`) · struct fields appended LAST ·
clean-rebuild awareness for rom_patches/glue headers · oracle SHAs cited at the site,
divergences documented where they occur · evidence tags [RAW-ROM]/[PATCH]/[STATIC]/
[PROBE✓] · budgets are caps, partial-findings-beat-stalling · one-iteration rule:
falsified contract → dated addendum entry → ONE re-pin → resume; second falsification
→ stop · `-F-` heredoc commits, no backticks in messages.

## Where things are

Plans: `docs/superpowers/plans/` (canonical exemplar: 2026-06-11-nk-syscall-surface.md).
Evidence/addenda: `docs/planning/machine/` (M6A-ONGOING-ENTRY-DESIGN.md,
M6A-WAVE2-SHIM-RECON.md, M3A-ENTRY-TABLE.md, EE-CHAIN-RECON.md, DISK-PATH-RECON.md).
Knob reference: `SheepShaver/docs/DIAGNOSTICS.md`. Process: `docs/MILESTONE-WORKFLOW.md`.
