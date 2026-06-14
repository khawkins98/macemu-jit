# Agent Context Pack — the standing facts every machine-layer agent needs

> Read this ONCE at task start instead of re-deriving from four docs. Maintained by the
> coordinator; facts here are current as of the last commit touching this file. When a
> task prompt conflicts with this pack, the prompt wins (it's newer).

## Current frontier (2026-06-14)

**M13 — COMPLETE (2026-06-14). The load-bearing fact: NewWorld 68k interrupt delivery WORKS** — the
M9→M13 keystone ("`0x5000ED08` never runs / interrupts never delivered") was a **probe-granularity
artifact** (see "load-bearing negatives" below). M13's deliverable is that retraction plus the revert
of the two dead delivery mechanisms (`SS_NW_DR_AUTOVEC`, `SS_M10_CGRP`). Canonical:
`docs/planning/M13-FINDINGS-interrupt-delivery.md` (retraction banner + §C-pin.7/8).

**M10 / M11a / M11 — COMPLETE (2026-06-13)** (detail: ROADMAP + CHANGELOG). M11 = 16 MB aperture at
0x81000000 + OF display node ("cofb") (gate `SS_M11_FB=1`); M11a = r24 never NK-clobbered. M10's
`SS_M10_CGRP` forged table is **reverted (2026-06-14)** — it targeted the non-problem and crashed.

**M12 PARTIAL — Wave0+Wave1 landed.** Wave0 (`ddbd8d79`): NW lowmem 1MB→32MB. Wave1 (`348544cd`):
anon-zero 0xFF000000–0xFFFFFFFF (sign-extended 68k EAs). Boot stable 30s+. Its "pixel gate FAIL =
`irq_fired=0`" framing is subsumed by the M13 retraction (delivery was never the blocker).

**NEW FRONTIER (M14): the model-rejection / pre-System gate.** Native delivery, the 68k handler, and the
scheduler are all healthy; the boot parks ~15 s in at the `[ALARM]` model-rejection / pre-System gate
(Gestalt/machine-ID or System-file boot gate rejecting this machine/ROM combo; `jDR` 68k-block counter
FREEZES ~10 s in while the NK spins). This is a **ROM/OS-version** issue, not an interrupt one.
- **Leverage (already-RE'd, gate-bypass-TOOLED territory):** `docs/planning/sheepshaver-research/SYSTEM-BOOT-GATES.md`
  (`boot` id=3 anatomy, DSAT format, gate-bypass) + the archived 4-byte System-file bypass in
  `docs/archive/2026-06/planning/UPGRADE-CARD-PATH.md`.
- **Approach (per lead):** characterize the gate FIRST (pin the last 68k subroutine before `jDR` freezes;
  identify the device/gate it polls). Use ONE early cross-version boot as a cheap version-locked check
  (a single boot of a different rev — staged 9.2.1/9.0.4/1.1 ROM at `/Users/Shared/macemu/`, or QEMU
  9.2.1 oracle — same `[ALARM]` or different?). The full ROM/OS-version route-around sweep is PROMOTED
  only if that check shows the gate is version-locked. Do NOT jump to the sweep before characterizing.

**Tooling (2026-06-13):** `make nw-northstar` — repeatable NewWorld boot-progress snapshot (all-on
cluster → `[NW-PROG verdict]`, report-only). Its load-bearing non-determinism caveat (post-EXT SIGSEGV
≠ regression; classify by durable markers) is in the Instruments section below.

## Boot recipes

- **Slot protocol (DEFAULT — never global `pkill`)**:
  `SheepShaver/tools/ss-slot-boot.sh --timeout 45 --label <task> --env 'SS_PROBE_PC=…'`
  (acquires a lease under /tmp/ss-slots/, per-slot prefs/logs/diag, SIGTERM at deadline
  so atexit dumps fire, prints SLOT/RUNDIR/LOG). Reap strays: `SheepShaver/tools/ss-reap.sh`.
  Full doc: `SheepShaver/tools/README-slots.md`. Concurrent boots are SAFE (proven).
- Default diagnostic config = newworld, 9.0.1 ROM, nogui, no disk; standard env baked in
  (SS_TERM_DUMP, SS_NW_TRAMPOLINE, SS_ROM_LENIENT).
- Capture discipline: SIGTERM (the wrapper does this), never SIGKILL/SIGALRM — they skip the atexit
  telemetry dumps. Heartbeat silence in late-boot regimes is normal; the term-dump is the capture.

## Instruments (caveats are load-bearing)

- `SS_PROBE_PC=0xPC[:fields][;…]` — PPC block-entry only (max 8 PCs/run, logarithmic
  sampling: visits 1,10,100…). Fields: rN, [0xADDR], [rN:SIZE]. **68k PCs are
  probe-blind for SS_PROBE_PC** — use `SS_PROBE_68K=0x68KPC[:N]` (68k regfile + PPC
  context at the DR dispatch hook, first N matches linear, edge-triggered; r24 word+2:
  to catch word X probe X+2 — **this is load-bearing: the M9→M13 "`0x5000ED08` never runs"
  keystone was a FALSE NEGATIVE from probing `ed08` instead of the post-`lhau` `ed0a`;
  five milestones built on it. A "never fires" from this exact-match probe is UNPROVEN until
  ring-confirmed — see LEARNINGS 2026-06-14 + M13-FINDINGS §C-pin.8**) or
  `SS_DR_R24_RING=1` (ring is 2M entries, last-4 dedup —
  dedup now COUNTS: dump prints `PC*N` for suppressed repeats; flushed on SIGSEGV
  directly+early in the crash handler since 2026-06-12 — mid-run watch/stall trace dumps
  no longer eat the crash once-shot; SIGTRAP deaths still produce NO dump).
  **Probes cannot count** — land a counter (the `exc=` tuple idiom) for counts; or
  `SS_PROBE_LINEAR=1` (+`SS_PROBE_CAP=N`, default 32) fires probes on EVERY visit up
  to the cap so short sequences become readable.
- **`[NW-PROG …]` boot-progress readout** (atexit; was `[PROGRESS]`, renamed 2026-06-13) —
  discrete per-signal lines (`config`/`nk-stage`/`dr68k`/`sched`/`irq`), each greppable
  with a score word + inline threshold context (spec: DIAGNOSTICS.md "[NW-PROG] readout").
  The standing signal `make nw-northstar` wraps it (all-on boot → `[NW-PROG verdict]`,
  report-only). **CAVEAT (load-bearing): the all-on boot is ~50/50 non-deterministic
  between a clean park and a post-EXT frontier crash (variable `ea`, often before atexit)
  — a bare SIGSEGV is NOT a regression.** Classify by durable markers (`[DR68K] first
  instruction`, `EXT delivered #1`), never by crash presence. See LEARNINGS 2026-06-13.
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
- `SS_JIT_WATCH_ADDR=<HEX,no-0x>[:LEN]` — parser is HEX; requires `SS_JIT_TRACE_RING=1`.
  **It is a CHANGE detector — blind to value-identical writes** (a zero-over-zero store
  never trips the `[WATCH]` line; use a counter/adjacent discriminator). Two blindness
  mitigations landed 2026-06-12: **span form** `ADDR:LEN` (bytes, hex, cap 0x10) watches
  every word of a multi-word lowmem long (`168:8` covers Ticks' LSB word 0x16c — Task B's
  "Ticks did not move" was watch-word-blind), and **`[WATCH-SAMPLE]`** lines at
  logarithmic record counts (1,10,100,…) prove frozen-vs-moving without a change edge.
  Slot cap is 8 words (spans consume one slot per word). `SS_JIT_WATCH_DUMPS` default 3
  exhausts on early lowmem-init hits — use =8 + a narrowed set for late events.
- **R-II9 DOWNGRADED (2026-06-12, instr-hardening re-test): `SS_PROBE_LINEAR=1` no
  longer crashes under the delivery regime** — 0/2 at HEAD (full env-on + 8 probes +
  ring + r24 ring) vs the original 2/2 on pre-`2a452166` code (the ClearInterruptFlag
  lost-edge race fix is the plausible retirer; the probe path is read-only, so LINEAR
  could only perturb timing). OK to combine again. Standing rule unchanged: hold
  instrument sets constant across A/B boots (the frontier class is timing-sensitive —
  ring-slowed boots park, no-ring boots spin in the NK); if a LINEAR-correlated crash
  recurs, re-raise R-II9 with the new tuple (note in INTERRUPT-INJECTION-RECON.md).
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
- **QEMU differential rig** (`SheepShaver/tools/qemu-rig.sh` + `qemu-mon.py`) —
  behavioral reference oracle for NewWorld/mac99 boot (shipped 2026-06-12). Use when a
  Task-0 question is "what does the guest do on a working boot?" — answers by observation
  instead of static RE. Invoke: `bash SheepShaver/tools/qemu-rig.sh --timeout 50` →
  `python3 SheepShaver/tools/qemu-mon.py --sock /tmp/qemu-rig-*/mon.sock '<cmd>'`.
  **Two load-bearing caveats:**
  (1) **Behavioral oracle only, never address oracle** — QEMU mac99 MacIO is at PCI
  BAR0 `0x80000000` (not `0xF3000000`); VIA=`0x80016000`, SCC=`0x80012000`. Never cite
  QEMU MMIO addresses as reference values for our machine layer.
  (2) **QEMU's Cuda model handles 60 Hz ticks internally** — hardware watchpoints on
  the VIA MMIO range never fire during interrupt handling in QEMU. On real hardware,
  Cuda asserts VIA IFR bit 3; Mac OS's Cuda driver may read it. "MMIO never read" is a
  property of QEMU's model, not of Mac OS in general — `dev_via6522` may still need to
  present the correct IFR bit via the Cuda protocol.
  Oracle scope: valid from NK entry onward (OpenBIOS ≠ Apple OF pre-NK).
  **Device-tree oracle (2026-06-13): the rig now auto-captures `info qtree` + `info mtree`
  to `<rundir>/device-tree.txt`** — every device + gpio-in/out wiring + MMIO size, and the
  live memory map incl. the escc-legacy alias table + NVRAM placement. Use it for the
  topology/wiring/NVRAM questions the milestones otherwise re-derive from QEMU C source.
  Same caveat (1): the addresses are QEMU-assigned (MacIO BAR0 0x80000000) — wiring oracle,
  NEVER our reference addresses.
  Pitfalls: `LEARNINGS.md` "2026-06-12". First-session findings: `docs/archive/2026-06/machine/VIA-IFR-RECON.md`.
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
**NK PIC descriptor=0x68ffefd0** (KDP+0xFD0, 0x30 bytes before ECB — ROM interrupt dispatch
reads this to identify sources: `+0x28`=pending-bits word, `+0x14`=ptr to level-indexed
source table; if `pending & table[level]` == 0, handler rte's source-less — this is the
VIA-IFR fix target, not VIA MMIO directly; confirmed by QEMU rig 5s probe, 2026-06-12) ·
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

17 `SS_NW_*` gates live in-tree (census 2026-06-12 post-M8: the 16 of re-score #3 +
the standalone `SS_NW_IRQ_CONSUME`; full disposition in MACHINE-LAYER-PLAN re-score #3
+ the M8 trigger-check note). The ones whose default you must know:

- **Default-ON (newworld)**: `SS_NW_TM_TRAPS` (HLE TM trap population — flipping it OFF
  re-introduces SysError-12), `SS_NW_MM_SWITCH`/`SS_NW_MM_POOL`, `SS_NW_SC_SURFACE`,
  `SS_NW_FE1F_SURFACE`, `SS_NW_DR_R0_INVARIANT`, **and the M7 cluster (`81d60cc1`):
  `SS_NW_EE_RISER` + `SS_NW_DEC_PUBLISHED` + `SS_NW_HOST_IRQ`** (the host
  once-per-assert-edge EXT latch). All explicit-"0" opt-out. **Supported configs are
  all-ON (default) and all-OFF; partial cluster opt-outs are diagnostic-only and
  unvalidated** (riser-off-alone / published-off-alone are known-broken intermediates).
- **Default-OFF/HELD**: `SS_NW_PIC` — joins the **env-on test cluster** (`SS_NW_PIC=1`
  adds the B-2 level staging: IACK/vector/level, so the NK post writes level|0x8000).
  Its default flip awaits real guest MPIC init + the tripwire per-source split
  (criteria in DIAGNOSTICS M7 section / ROADMAP follow-on row).
- **Default-OFF, standalone 17th gate**: `SS_NW_IRQ_CONSUME` (M8, ships gated-off-green;
  explicit-"1" opt-in, SS_NW_PIC polarity) — the deferred EE-edge latch + Q-C3 staging
  detour + 60 Hz backstop + MODE_EMUL_OP fence. Riser-conditional (consume-on +
  riser-off is inert by construction). **Retirement criterion: fold-into-cluster is
  MANDATORY at its future flip**; prerequisites = the VIA-IFR surface + the SC#1=0x0d
  divergence explained-or-fixed (DIAGNOSTICS M8 section).

## Gate tiers (see MILESTONE-WORKFLOW.md §6/§6b for the policy)

- **Run tiers via `tools/gates.sh <inner|task|full> [--reason "…"]`** — read the
  `GATE …: PASS|FAIL` summary lines + final `GATES <tier>: PASS|FAIL` verdict, not raw output.
  Boot assertions: `ss-slot-boot.sh --expect 'PAT;;…' [--absent 'PAT;;…']` →
  `EXPECT: n/m present` + `BOOT-VERDICT: PASS|FAIL` (exit 0/3) — grep targets, not log reading.
- **Per-commit (inner)**: `make build-ss` + `SS_HARNESS_BATCH=1 make test-jit` (353/353)
  + `make -C src/machine test`. **Per-task**: + plain `make test-jit` (authoritative) + `make e2e-test`.
- **NewWorld observe line (report-only)**: `make nw-northstar` → `[NW-PROG verdict]` at task close on
  any newworld-path change. NOT a failing gate (non-deterministic; classify by durable markers — see
  the load-bearing caveat under Instruments). A `REGRESSED(...)` = real below-frontier break.
- **Risk-based**: paravirtual `make e2e` — REQUIRED when the change touches code reachable on
  paravirtual; SUBSTITUTABLE by the structural-inertness argument + a gated-off byte-identical A/B
  boot when every new line is inside `MachineProfileIsNewWorld()`/env gates (state which in the commit).
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

**Next task: M14 — characterize the model-rejection / pre-System gate** (see Current frontier above).
Leverage: `docs/planning/sheepshaver-research/SYSTEM-BOOT-GATES.md`,
`docs/archive/2026-06/planning/UPGRADE-CARD-PATH.md`. M13 close-out / retraction:
`docs/planning/M13-FINDINGS-interrupt-delivery.md` (§C-pin.7/8). The M13 strategy/plan docs
(`NANOKERNEL-STRATEGY-DECISION.md`, the m13 plan) are now historical — they planned the non-problem.
Keep-active machine docs (`docs/planning/machine/`): `CORE99-MACHINE-DESCRIPTION.md`,
`M1-DEVICE-CONFORMANCE.md`, `ROM-PATCH-AUDIT.md`, `FRAMEBUFFER-RECON.md` (prior recon, complete).
Archived: `docs/archive/2026-06/{machine,superpowers/plans}/`; session log
`docs/archive/2026-06/HANDOFF-SESSIONS-M9-M12.md`.
Knob reference: `SheepShaver/docs/DIAGNOSTICS.md`. Process: `docs/MILESTONE-WORKFLOW.md`.
