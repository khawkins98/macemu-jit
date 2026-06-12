# Agent Context Pack — the standing facts every machine-layer agent needs

> Read this ONCE at task start instead of re-deriving from four docs. Maintained by the
> coordinator; facts here are current as of the last commit touching this file. When a
> task prompt conflicts with this pack, the prompt wins (it's newer).

## Current frontier (2026-06-12)

Default newworld diagnostic boot runs **44.4 s to the PROGRAM#5 srr0=0x50324fec park** —
SysError-12 is CLEARED (SS_NW_TM_TRAPS default-ON, f808a7fb/adea99bc) and the P-M5
Execute68k SIGSEGV is CLEARED ([KDP+0x1074/0x1078] staged, 34d3d441). On a riser-on boot
the published DEC route delivers live (`delivered_dec=3` — the first live published-route
deliveries). Active milestone: **interrupt injection**
(`docs/superpowers/plans/2026-06-12-interrupt-injection.md`, rev 2; W2-4's remainder is
superseded into it). Do not treat "EE has never risen" / "delivered_dec=0" /
"SysError 12" as current claims — they are historical.

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
- **Ring dumps are read with `tools/ring-walk.py`, not by eye** — point it at a boot
  log, a ring dump, or a slot rundir: `--window START:END` (absolute record numbers),
  `--r24-flow` (68k-PC transitions; odd-PC/odd-delta flagged DESYNC-CANDIDATE),
  `--find-pc 0xPC`, `--regs-at REC`. It follows the log's "dumped to" pointer and
  warns when /tmp/ss_jit_ring.txt is stale (shared across runs); `--ring` overrides.
- `SS_JIT_WATCH_ADDR=<HEX,no-0x>` — parser is HEX; requires `SS_JIT_TRACE_RING=1`.
- `SS_SEED_MEM`, `SS_EXC_ENTRY=0xINT[,0xSC]` (no-comma form preserves the syscall
  default), `SS_CUDA_TRACE=1`, `SS_INTERP_RING` — see DIAGNOSTICS.md.
- **`deferred_native` is a RE-POLL count post-`3cb3b16e`** (wake-up re-arm: one deferral
  episode can contribute up to 65536 counts) — never compare it across that commit.
- **`SS_PROBE_68K` is nested-execute-blind**: it hooks the DR dispatch loop, so 68k code
  run via host-side `Execute68k()` excursions never hits the probe (it "never fires"
  ≠ "never runs").
- **mtspr-DEC capture is default-on** in the `[VCLK]` exit dump (`21704614`): last-8
  (value@PC) ring + zero/tiny/small/mid/msb value-class buckets — the DEC-cadence
  evidence channel.
- ROM dumps — canonical location: **`/Users/Shared/macemu/dumps/`** with `MANIFEST.txt`
  (filename + md5 + provenance). `rom901_inventory.bin` = RAW (md5 7b1378be…, 16
  placeholder words at file 0x36e8c0); `rom901.bin` = PATCHED (md5 d1a267a9…,
  re-baselined 2026-06-12 post-f808a7fb — the EMUL_OP tail now applied; the previous
  image preserved as `rom901_pre-f808a7fb.bin` md5 e432df64… for older [PATCH]
  evidence). The /tmp
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

NK-published exception entries (the `[KDP+0x360]` vector table): DEC 0x50313200
(`[KDP+0x384]`, SS_NW_DEC_PUBLISHED route) · EXT 0x50314880 (`[KDP+0x374]`) ·
SC 0x50314ac0 (`[KDP+0x390]`) · PROGRAM
0x50314700 (`[KDP+0x37c]`). Trampoline-staged globals: `[KDP+0xf2c]` = NK scheduler
TimebaseSpeed (tb-freq; the DEC-cadence fix, `f31d475e`) · `[KDP+0x1074]`=0x50480000 /
`[KDP+0x1078]`=0x50460000 = the Execute68k emulator pair (dispatch-table/emulator-code;
`34d3d441` — without them any host `Execute68k()` is a wild jump) · TM patch space
`TIME_MANAGER_PATCH_SPACE=0x2fd240` (ROM-offset; HLE TM trap population, `f808a7fb`).

## Env-gate state (SS_NW_*, the ones that bite)

15 `SS_NW_*` gates live in-tree (census 2026-06-12; flagged for re-score #3 disposition).
The ones whose default you must know:

- **Default-ON (newworld)**: `SS_NW_TM_TRAPS` (HLE TM trap population — flipping it OFF
  re-introduces SysError-12), `SS_NW_MM_SWITCH`/`SS_NW_MM_POOL`, `SS_NW_SC_SURFACE`,
  `SS_NW_FE1F_SURFACE`, `SS_NW_DR_R0_INVARIANT`.
- **Default-OFF, flip RESERVED** for the interrupt-injection milestone acceptance:
  `SS_NW_DEC_PUBLISHED` (DEC → NK-published 0x50313200 + 2-SPR shim, `181efc02`) and
  `SS_NW_EE_RISER` (the 0x318000-stub EE riser, `10b1b3e8`). Riser-on boots deliver
  (storm-scale 12.4M proven; `delivered_dec=3` live on the published route).

## Gate tiers (see MILESTONE-WORKFLOW.md §6/§6b for the policy)

- **Run tiers via `tools/gates.sh <inner|task|full> [--reason "…"]`** — read its
  `GATE …: PASS|FAIL` summary lines and the final `GATES <tier>: PASS|FAIL` verdict,
  NOT the raw gate output (on FAIL it prints the failing gate's last 20 lines; full
  per-gate logs stay in the printed tmpdir for audit). Boot assertions likewise:
  `ss-slot-boot.sh --expect 'PAT;;…' [--absent 'PAT;;…']` prints
  `EXPECT: n/m present, k absent-violations` + `BOOT-VERDICT: PASS|FAIL` (exit
  0/3) — grep targets, not log reading.
- **Per-commit (inner)**: `make build-ss` + `SS_HARNESS_BATCH=1 make test-jit` (353/353)
  + `make -C src/machine test` (ALL PASS). ~1 minute warm.
- **Per-task (final commit)**: + plain `make test-jit` (authoritative) + `make e2e-test`.
- **Risk-based**: paravirtual `make e2e` — REQUIRED when the change touches code
  reachable on paravirtual (shared functions, non-gated lines); SUBSTITUTABLE by the
  structural-inertness argument + the gated-off byte-identical A/B boot when every new
  line is inside MachineProfileIsNewWorld()/env gates (state which in the commit).
- Doc-only commits: no gates.

## Standing rules (the short list)

Explicit-path staging only (never `git add -A`; the claims guard now WARNS when you
stage a file claimed by nobody — the diff may include another agent's uncommitted
work, verify it is all yours) · struct fields appended LAST ·
clean-rebuild awareness for rom_patches/glue headers · oracle SHAs cited at the site,
divergences documented where they occur · evidence tags [RAW-ROM]/[PATCH]/[STATIC]/
[PROBE✓] · budgets are caps, partial-findings-beat-stalling · one-iteration rule:
falsified contract → dated addendum entry → ONE re-pin → resume; second falsification
→ stop · `-F-` heredoc commits, no backticks in messages.

## Where things are

Plans: `docs/superpowers/plans/` (canonical exemplar: 2026-06-11-nk-syscall-surface.md;
ACTIVE: 2026-06-12-interrupt-injection.md). Evidence/addenda: `docs/planning/machine/`
(M6A-ONGOING-ENTRY-DESIGN.md, M6A-WAVE2-SHIM-RECON.md, M3A-ENTRY-TABLE.md,
EE-CHAIN-RECON.md, TRAP-TABLE-RECON.md [the InsTime/SysError-12 wall + fix record],
INTERRUPT-INJECTION-RECON.md [P-M5 anatomy, the paravirtual donor chain, the
exception-path architecture answer], SLIDE-WALL-RECON.md, DSAT-WALL-RECON.md,
DISK-PATH-RECON.md).
Knob reference: `SheepShaver/docs/DIAGNOSTICS.md`. Process: `docs/MILESTONE-WORKFLOW.md`.
