# Agent Context Pack — the standing facts every machine-layer agent needs

> Read this ONCE at task start instead of re-deriving from four docs. Maintained by the
> coordinator; facts here are current as of the last commit touching this file. When a
> task prompt conflicts with this pack, the prompt wins (it's newer).

## Current frontier (2026-06-12, post-M8-slot-4-consumption)

**The M8 slot-4 consumption milestone SHIPPED GATED-OFF-GREEN** (`42ce3e0e`…`52b69cd7`;
`SS_NW_IRQ_CONSUME` stays default OFF — a standalone 17th gate, acceptance recipe
`SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1`). On the env-on test cluster the **consumption round
trip is GREEN through leg 7**: EXT delivery → NK from-emulator post (0x8001@0x68fff070)
→ the Q-C3 staging detour (deferred pair `[KDP-0x440]/[KDP-0x43c]` + task-flag) → the
scheduler-restore drain (0x324720) → **the slot-4 twi fires** (PROGRAM srr0=5046e8d0
word=0fff0004 slot=4) → world switch completes (the R-II10 livelock shapes are FIXED —
torn-ctx root cause fork-(iii), the riser's mtmsr re-raise mid-tail, cured by the
deferred EE-edge latch + 60 Hz backstop) → **the 68k level-1 handler runs at 60 Hz**.
**RED at leg 8, the via6522 IFR surface**: the handler's source dispatch finds NO VIA IFR
source bit in the via6522 model and **rte's source-less** (0x5000eecc) — OP_IRQ retire
never runs, the un-retired post re-traps slot-4 ~1.2k/s, Ticks is NOT guest-claimed (the
60 Hz 0x16c movement is the host keep-set, census-proven via `ticks_keepset`).
**Correction (2026-06-12, QEMU rig + static disassembly):** The prior description
`btst d6,(a4) @0x5000ee9a` was WRONG — `0x5000ee9a` is mid-word of a 4-byte
`tst.l $d94.w` starting at `0x5000ee98`. The actual ROM stub source-dispatch is:
`tst.l $d94.w; beq $5000eea4; movea.l $6e4.w,a0; jsr (a0)` — it tests the 68k
low-memory flag `$0d94` (NOT a VIA MMIO register). No `btst d6,(a4)` exists in the ROM
near that address; no `movea.l #$F3016xxx, a4` exists anywhere in the ROM.
The real question is what sets `$0d94` at tick time (and whether it is even the active
handler path — at Finder, Mac OS 9.2.1 installs its own handler in RAM, replacing the ROM
stub). See `docs/archive/2026-06/machine/VIA-IFR-RECON.md` for the full corrected picture.
**Named next task: the VIA-IFR surface** (M-class device-model work — `dev_via6522`
exists). **Task A COMPLETE (2026-06-12 session 2 probe campaign).**

**Corrected addresses (the AGENT-CONTEXT and RECON doc both had this wrong):**
`[KDP+0xfd0]` (= address `0x68ffefd0`) is the POINTER FIELD holding the Hnfo record
address; the record itself is at `hnfo_rec = 0x68ff4f00` (= irp_base + 0xf00).
Fields: source-table-ptr = `hnfo_rec+0x14` = **`0x68ff4f14`**;
pending-bits = `hnfo_rec+0x28` = **`0x68ff4f28`**. The previously cited
`0x68ffeff8`/`0x68ffefe4` were wrong.

**Probe results at first EXT interrupt (SS_PROBE_PC=0x50314880 visit=1):**
- `*(0x64)` = **`0x5000ed08`** ← the active handler in OUR boot is the SECONDARY dispatch
  table (NOT the QEMU-observed `0x5000ec50`; these are two different 9.0.1 ROM paths —
  QEMU boots 9.2.1 Mac OS which may install the primary path early; our trampoline or
  rom_patches writes something different to 0x64)
- `hnfo_rec+0x14` = **`0x00000000`** — source table pointer NIL (never initialized)
- `hnfo_rec+0x28` = **`0x80000000`** — pending bit set by NK/init before first interrupt

**Root cause:** With source-table-ptr NIL, the secondary dispatch (0x5000ed36) does
`and.l (0, 1*4), d0` = read from 68k address 4 (= Initial PC = 0x50010000); bit 31 = 0
→ AND = 0 → both primary and secondary checks fail → no source found → hardware-reset
path at 0x5000980a (which in our emulation likely loops or soft-falls-through without
visible reset) → epilogue at 0x5000ee58 → `tst.l $d94.w` = 0 → rte source-less.

**Fix target (identified):** Patch ROM offset 0xed08 (= 0x5000ed08) with
`M68K_EMUL_OP_IRQ; rte; nop; nop`. The via_int3_dat pattern is confirmed present there;
the existing via_int3 search range (0x15000–0x19000) misses it, and the via_int
pre-condition also blocks. A new 9.0.1-specific patch with range 0xed00–0xee00 lands it.
**Prerequisite:** `KernelDataAddr+0x67c` (`0x68ffe67c`) must not be zero before OP_IRQ
runs — set it to a scratch address in the NewWorld trampoline init to avoid a write to
68k address 0 (Initial SSP corruption). Gate: `SS_NW_VIA_IFR`.
Full implementation plan: `docs/archive/2026-06/machine/VIA-IFR-RECON.md` §5e–5f.

**Tooling lesson (load-bearing):** Use `SS_PROBE_68K=0xPC:N` for 68k code paths, NOT
`SS_PROBE_PC`. SS_PROBE_PC is PPC block-entry only and is probe-blind to 68k addresses.
A single `SS_PROBE_68K=0x5000ed08:5` would have shown a2=hnfo_rec and *(a2+0x14)=0
directly, without the address confusion or QEMU comparison step.
Default boots are unchanged (PROGRAM#5 srr0=0x50324fec park + the M7 delivery
chronology; level-0 posts per R-II7 keep consumption unreachable by design until the
SS_NW_PIC flip). **NEW named residue: SC#1 r0=0x0d** (r1=1017ffde lr=5046c5ac) —
deterministic 2/2 on default+consume-on boots vs 0/19 without; candidate mechanism =
the Q-C3 stub's unconditional level-0 staging re-arming at the drain; a flip
prerequisite and a VIA-IFR Task-0 recon question. Do not treat "the post is never
polled" / "PROGRAM#4 never fires" / "the slot-4/0x3244e8 livelock" / "Ticks never
moves" / "EE has never risen" / "delivered_ext stays 0 on live boots" / "SysError 12"
as current claims — they are historical.

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
  dedup now COUNTS: dump prints `PC*N` for suppressed repeats; flushed on SIGSEGV
  directly+early in the crash handler since 2026-06-12 — mid-run watch/stall trace dumps
  no longer eat the crash once-shot; SIGTRAP deaths still produce NO dump).
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

**Next task: M9 VIA-IFR surface.** Probe recipe and open questions: `docs/HANDOFF.md`.
Active planning: `docs/archive/2026-06/machine/VIA-IFR-RECON.md` (boot stall detail, §7–§8; archived — probe recipe is in HANDOFF.md).
Keep-active machine docs: `CORE99-MACHINE-DESCRIPTION.md`, `M1-DEVICE-CONFORMANCE.md`,
`ROM-PATCH-AUDIT.md`, `FRAMEBUFFER-RECON.md` (HOLD).
Archived milestone recon: `docs/archive/2026-06/machine/`.
Archived plans: `docs/archive/2026-06/superpowers/plans/`.
Knob reference: `SheepShaver/docs/DIAGNOSTICS.md`. Process: `docs/MILESTONE-WORKFLOW.md`.
