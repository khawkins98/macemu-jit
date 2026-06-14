# SS_M18_TRAMPOLINE_LLE — gating Task-0 recon (BINDING; gates ALL implementation tasks)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:dispatching-parallel-agents
> (Q1/Q2 vs Q3 are boot-disjoint — one recon agent per boot-independent question, fused
> by the coordinator per WORKFLOW §4). Steps use checkbox (`- [ ]`) syntax for tracking.
> **This plan is recon ONLY.** It ends at the GO/NO-GO + weeks-vs-months verdict + the
> pinned device-tree→CGRP handoff contract. It does NOT plan, authorize, or begin any
> implementation task. Implementation (`of_ci_callback()`, the DT model, the Trampoline
> loader, the `call-method` backends) is a SEPARATE later plan gated on these answers.

**Goal (PASS criteria — the Task-0 deliverable):** Answer the three gating questions
carried forward from `FINDINGS-trampoline-re.md` "SS_M18 — gating risks", each with
*falsifiable* evidence and an evidence tag, written into a committed addendum
(`docs/planning/newsheep/FINDINGS-trampoline-re.md`, new section "SS_M18 gating Task-0").
The milestone PASSES Task-0 when ALL of the following exist in writing:
1. **Q1 (emulator-host ownership) answered** with an enumerated REPLACE-vs-FIGHT ledger:
   for each existing SS scaffolding piece the real NanoKernel-v02.27 would touch
   (supervisor/SPR block, exception/`ExcEnter` path, MixedMode/`Execute68k` excursion,
   scheduler/DEC cadence, the entry-vector synthesis), a row stating *replace* (NK
   overwrites/owns it, SS yields cleanly) or *fight* (NK and SS both want to own live
   state, irreconcilable without rework) — grounded in NanoKernel-v02.27 disasm + cited
   SS source file:line.
2. **Q2 (MMU / V=P) answered** with a falsifiable verdict: `/mmu` claim/translate/map is
   *satisfiable within V=P* (identity/window mapping; the NanoKernel never depends on
   real translation happening) — OR it *requires a live paged MMU* (NK installs and reads
   back BAT/SR/page-table state, or branches on translated≠physical), with the disasm
   evidence that decides it.
3. **Q3 (NanoKernel→CGRP contract) answered** with the pinned handoff contract: a traced
   (not inferred) ordered list of what NanoKernel-v02.27 *reads* from the device tree and
   *writes* when building CGRP — the exact contract our synthesized DT must satisfy.
   Tagged [QEMU-BEHAVIORAL]; address-oracle caveat honored (no QEMU MMIO address cited as
   a reference value).
4. **GO / NO-GO verdict** for SS_M18 Route A as a SheepShaver milestone.
5. **Weeks-vs-months scope verdict**, derived explicitly from the Q1 ledger and the Q2
   verdict (Q1=fight OR Q2=requires-paged-MMU ⇒ months; both clean ⇒ weeks).

**DIAGNOSTIC observations (recorded, NOT gates):** how far a *probe* boot of any nascent
loader gets; QEMU boot-progress markers beyond CGRP; the count/identity of NanoKernel DT
reads beyond the CGRP-relevant ones; any newly-named downstream wall (e.g. the expected
Cuda IFR/IER wall). These inform the next plan; they do not pass or fail Task-0.

**Scope guard (LAW for this milestone):** Route A is DECIDED and SETTLED
(`FINDINGS-trampoline-re.md` Q0-E) — do NOT relitigate run-vs-patch-vs-reproduce. This
Task-0 does NOT write any `SheepShaver/src/**` source. Allowed writes: the FINDINGS
addendum + (if a probe boot is run) capture-only telemetry already in-tree. A probe boot
uses EXISTING knobs only (`SS_PROBE_PC`/`SS_DR_R24_RING`/QEMU rig) — no new loader code.

---

## Authoritative inputs

| Doc / source | What it fixes for this Task-0 |
|---|---|
| `docs/MILESTONE-WORKFLOW.md` §2 (Task-0 = blocking-answer table + budgets + tag discipline), §4 (boot-disjoint questions split across parallel agents), §6 gate tiers, §6c (QEMU-oracle-BEFORE-static-RE; the load-bearing address caveat) | The process this plan instantiates; Q3 obeys §6c (QEMU first); Q1/Q2 are static-RE-primary because they ask "does our code collide", not "what does a working boot do at X". |
| `docs/planning/newsheep/FINDINGS-trampoline-re.md` — "SS-integration sketch" (field-by-field `SS_NW_TRAMPOLINE` reconciliation table) + "SS_M18 — gating risks to red-team BEFORE any code" (the 3 questions) | The DECIDED inputs (Q0-A bounded, Q0-B computed(OF-input), Q0-F producer=Trampoline+NanoKernel, Route A). The reconciliation table is the Q1 ledger's starting skeleton; the 3 gating risks are Q1/Q2/Q3 verbatim. |
| `docs/superpowers/plans/2026-06-11-nk-syscall-surface.md` (archived `docs/archive/2026-06/superpowers/plans/`) | The plan-format exemplar (PASS-vs-DIAGNOSTIC split; blocking-answer table; budgets w/ residue fallbacks; one-iteration stop-rule; Self-review tensions FOR the red team; empty Red-team record). |
| `docs/AGENT-CONTEXT.md` — Constants block + Instruments (QEMU rig caveats) | KDP/ECB/mirror constants; the Execute68k pair [KDP+0x1074]=0x50480000 / [KDP+0x1078]=0x50460000; NK exception entries; QEMU caveats (behavioral-only, MacIO at 0x80000000 not 0xF3000000; oracle valid NK-onward). |
| NanoKernel-v02.27 parcel disasm (STATIC input) — Configfile: `NanoKernel-v02.27` @ ROMBase+0x310000, 0x10000 B (per FINDINGS Q0-D); 105280 B parcel in the 9.0.1 dump's `MacROM.src/` | Q1's REPLACE-vs-FIGHT evidence + Q2's MMU-install evidence. Canonical ROM: `/Users/Shared/macemu/newworld-roms/2001-12-19 - Mac OS ROM 9.0.1.rom`, md5 `66210b4f71df8a580eb175f52b9d0f88` (== active project ROM). Full ROM dumps: `/tmp/newsheep/dump-9.0.1`. |
| QEMU rig: `SheepShaver/tools/qemu-rig.sh` + `qemu-mon.py`; gdbstub via `--gdbstub` (-s; add -S to halt at reset); RSP client `/tmp/newsheep/gdbcli.py` | Q3's instrument (`[QEMU-BEHAVIORAL]`). Same rig/ROM-swap as the Trampoline RE T0.2 (OpenBIOS loads the Trampoline at its ELF vaddr; PC=0x20f078, r2=0x1001e8). |

## Codebase facts (carried; recon agents RE-VERIFY each site before use)

> All offsets/addresses below are PRE-PINNED for orientation — **re-verify before use**
> (re-read the cited file:line / re-disassemble the cited parcel offset). They scope the
> collision questions; they are not the answers.

- **The `SS_NW_TRAMPOLINE` output-forge writes (the surfaces Q1 reasons about):**
  - Register-fixup trampoline + entry-vector slot synthesis: `rom_patches.cpp`
    `PatchROM_NW_trampoline` @ :729ff (tp[0..10] seed r31=ECB/r30=mirror 0x50460000/
    r29=0x50480000 LA_DispatchTable, guest[0]/[4] reset SSP/PC); table[0] gate @
    ROM+0x46e8c0; mirror entry-vector table also published `[KDP+0x648]=ROM+0x46e8c0`
    (`sheepshaver_glue.cpp` :3011).
  - `'Hnfo'@0x68ff4f00` + sibling `[[KDP-0x20]+0xf70]=='Hnfo'`: `sheepshaver_glue.cpp`
    :2861–2904. PIC-rail/`[KDP+0xfd0]` family same region.
  - `[KDP+0xf2c]` scheduler TimebaseSpeed (DEC cadence): `sheepshaver_glue.cpp` :3099–3119.
  - **Execute68k emulator pair** `[KDP+0x1074]=ROMBase+0x480000` / `[KDP+0x1078]=ROMBase+
    0x460000`: `sheepshaver_glue.cpp` :3140–3143; ALSO consumed live in
    `sheepshaver_cpu::execute_68k` @ :1504–1505 (`gpr(29)=[KDP+0x1074]`, `gpr(30)=
    [KDP+0x1078]`). **This is the one genuinely SS-specific seam** (FINDINGS reconciliation:
    KEEP / re-inject post-handoff) — Q1 must confirm whether the NanoKernel *rebinds* this
    pair to the ROM EmulatorCode @0x360000 and whether SS can re-inject afterward, or
    whether the NK runs 68k through a path that bypasses `execute_68k` entirely.
- **The SS exception/MixedMode/scheduler scaffolding Q1 reasons about:**
  - Exception transition core: `machine/exc_core.cpp` `ExcEnter` :12 / `ExcRfi` :104
    (EXC_EXTERNAL/EXC_DECREMENTER/EXC_SC); the entry table `g_exc_entry_table`. PEM masks
    are LAW.
  - Interrupt/DEC delivery + MixedMode native-excursion fence: `sheepshaver_glue.cpp`
    `interrupt()` :1002, `deliver_pending_dec_exception()` :1122ff (the `[XLM_RUN_MODE]`
    0x2810 fence @ :1139), EXT `ExcEnter` :1254, DEC `ExcEnter` :1342, `execute_68k` :1461.
  - Supervisor SPR/BAT/SDR1 block: `sheepshaver_cpu::reset_supervisor_for_test` :555–574
    (the only place SS touches BAT/SDR1 — currently a TEST helper, not a live MMU).
  - Scheduler/event cadence: `machine/event_sched.cpp`, `machine/virt_clock.cpp`.
- **V=P / flat-model definition (Q2's collision floor):** `Unix/sysdeps.h` :91–99 —
  `DIRECT_ADDRESSING 1` when `NATMEM_OFFSET` set (macOS arm64). Host = `NATMEM_OFFSET +
  guest` (`sheepshaver_glue.cpp` :2388, NATMEM_OFFSET = 0x400000000000 per AGENT-CONTEXT).
  No live paged MMU: BAT/SDR1 are touched only in the test-reset helper above; the paged
  MMU was deliberately deferred in machine-layer M5. There is no guest page-table walker.
- **Constants (AGENT-CONTEXT):** KDP=0x68ffe000, ECB=0x68fff000, mirror emulator base
  0x50460000, DR dispatch table 0x50480000, ROMBase=0x50000000, NK primary 0x5031xxxx,
  CGRP family `*(KDP-0x338)`, `[CGRP+0x38/0x3c/0x40/0x44]`, service `0x503148e0`. NK
  exception entries: EXT 0x50314880 (`[KDP+0x374]`), SC 0x50314ac0, DEC 0x50313200,
  PROGRAM 0x50314700. The ROM EmulatorCode parcel @ ROMBase+0x360000 (Configfile).
- **ROM/parcel provenance ritual (before any [STATIC] tag):** confirm the canonical ROM
  md5 `66210b4f…` and that `/tmp/newsheep/dump-9.0.1` + the NanoKernel-v02.27 parcel still
  exist and match recorded md5s; if `/tmp` evaporated, re-extract via `tbxi` (venv
  `/tmp/newsheep/venv`) from the canonical ROM. /tmp artifacts get an md5 anchor recorded
  in the FINDINGS addendum (the standing /tmp provenance rule).

## Blocking-answer table (which downstream SS_M18 implementation concern blocks on which gating question)

> This is a residue-disposition map, NOT a start-order license. ALL blocking answers must
> be pinned (or their residue dispositions recorded) before the SEPARATE implementation
> plan may be drafted. Implementation concerns named here are for traceability only — none
> are started in this milestone.

| Downstream SS_M18 implementation concern (future plan) | Blocked by | Residue disposition if only partially answered (partial-findings-beat-stalling) |
|---|---|---|
| Whether the Trampoline loader + handoff can re-use the existing `ExcEnter`/`interrupt()`/`execute_68k` scaffolding, or must replace it | **Q1** | If the REPLACE-vs-FIGHT ledger is complete for ≥ the 3 load-bearing rows (Execute68k pair, exception path, scheduler) but incomplete on minor rows → record the covered rows as pinned, the rest as residues feeding the implementation plan's own Task-0; do NOT guess "probably replaces". A FIGHT on any load-bearing row → weeks-vs-months flips to months; that is the verdict, not a blocker to keep digging. |
| Whether `/mmu` claim/translate/map is a thin call-method backend over V=P, or forces a real paged MMU (machine-layer M5 un-deferral) | **Q2** | If disasm shows claim/translate but the *consumption* of translated addresses can't be classified within the time-box → residue verdict = "MMU dependence UNKNOWN; treat as months-risk until traced", recorded as the implementation plan's first gating item. Never default to "identity mapping will be fine". |
| Exact device-tree properties + write order our synthesized DT/CGRP-builder must satisfy (the OF-CI callback + DT model contract) | **Q3** | If QEMU breakpoint-into-NK lands but the full read/write order can't be captured within the boot cap → pin whatever ordered reads/writes WERE captured + the byte ranges still open; the implementation plan supplies the rest from static NK disasm. A captured-partial contract still beats the current INFERRED one. |
| Whether the Execute68k pair must be re-injected post-handoff (the one SS-specific seam) | **Q1** (sub-row) | If Q1 confirms NK rebinds [KDP+0x1074/0x1078] to ROM EmulatorCode but the SS re-injection feasibility is unclear → record as a pinned *fact* (NK rebinds) + a residue (*re-inject feasibility*) for the implementation plan. |
| GO/NO-GO + weeks-vs-months | **Q1 ∧ Q2** (verdict), informed by **Q3** | The verdict is REQUIRED output even on partial answers: state it conditionally ("GO, weeks IF Q2-residue resolves clean; else months") — a conditional verdict is the deliverable, stalling is not. |

## Budgets (caps with WRITTEN residue fallbacks — unbounded disasm killed predecessor agents)

> Caps bind; partial-findings-beat-stalling. Static time-boxes are wall-clock guidance for
> the recon agent; the hard cap is the *function/instruction window*, the proven kill-switch.

- **Q1 (static, NanoKernel-v02.27 disasm + SS source):** time-box ~90 min. Hard window:
  disassemble the NanoKernel **entry + emulator-binding + exception-publish clusters
  only** — follow each of the 5 ledger surfaces ≤2 call levels / ≤12 functions from its
  entry point. Anchor on the Execute68k-pair writers (search the parcel for stores of the
  ROM EmulatorCode base 0x360000 / dispatch-table base, and KDP-relative stores to +0x1074/
  +0x1078). **Residue fallback:** a ledger row not classifiable within the window is
  recorded as `FIGHT?-residue` (NOT silently "replace") and feeds the months-risk side of
  the verdict. Do NOT disassemble the whole 105280 B parcel.
- **Q2 (static, same parcel):** time-box ~60 min, shares the Q1 disasm artifact. Hard
  window: locate the `/mmu` handoff consumption — the NK code path that runs after the
  Trampoline's claim/translate/map, looking specifically for (a) `mtsr`/`mtsrin`/`mtspr`
  SDR1/BAT writes, (b) reads of translated vs physical addresses, (c) any branch on a
  translation result. **Residue fallback:** if the MMU-install evidence is ambiguous,
  verdict = "requires-paged-MMU UNKNOWN → months-risk" (the conservative residue), recorded
  as the implementation plan's gating item — never the optimistic default.
- **Q3 (QEMU rig — §6c: QEMU BEFORE static RE):** boot cap **≤4 QEMU rig boots**, each
  ≤90 s (gdbstub `-S` halt-at-reset adds setup; the RSP step-over pattern from
  `gdbcli.py`: remove-bp → single-step → re-insert → continue). At most 2 boots before the
  residue status of "did we land inside the NK CGRP-builder?" is decided. **Residue
  fallback:** if a breakpoint in the NanoKernel-v02.27 region (CGRP-builder, around the
  `0x503148e0` service / `[KDP-0x338]` build) doesn't land within 2 boots, fall back to
  static NK disasm of the CGRP-build cluster for the contract and tag it [STATIC-inferred]
  (the gap stays explicitly inferred, as it is today — but now bounded and documented).
- **Boot arithmetic:** Q3's QEMU boots are the ONLY boots in this Task-0. Q1/Q2 are
  static. An optional ≤2 SS diagnostic boots (`ss-slot-boot.sh`, existing knobs) are
  permitted ONLY to confirm a current-scaffolding fact a recon agent needs (e.g. that
  `execute_68k` reads [KDP+0x1074] live) — DIAGNOSTIC, never to test new loader code (none
  exists). Total SS boots ≤2; QEMU boots ≤4; both outside any gate battery.

## Parallelization (WORKFLOW §4 — one agent per boot-independent question)

- **Agent P1 (static): Q1 + Q2.** Both consume the same NanoKernel-v02.27 disasm artifact +
  SS source; no boot needed. One agent, one disasm pass, two verdicts. Strong tier (subtle
  RE — disassembly judgement is exactly where the cheap tier degrades per §6c).
- **Agent P2 (QEMU): Q3.** Boot-disjoint from P1 (different instrument: QEMU rig vs static
  disasm). Strong tier. Runs concurrently.
- **Coordinator fuses** P1 + P2 into the FINDINGS addendum + the GO/NO-GO + weeks-vs-months
  verdict (the verdict needs Q1 ∧ Q2 from P1, informed by Q3 from P2). The fusion step is
  where the conditional-verdict logic in the blocking-answer table is resolved.

## Tag discipline & provenance

- Evidence tags: **[STATIC]** (NanoKernel-v02.27 disasm / SS source file:line),
  **[QEMU-BEHAVIORAL]** (Q3 rig observations — never an address oracle), **[PROBE✓]**
  (a confirming SS diagnostic boot, if used). A claim with no tag is not pinned.
- **The QEMU address caveat is LAW for Q3 (WORKFLOW §6c, AGENT-CONTEXT):** QEMU mac99 MacIO
  is at 0x80000000 (ours at 0xF3000000); VIA=0x80016000. Q3 may cite the *order and identity*
  of DT reads/writes and the *structure* of the CGRP build, NEVER a QEMU MMIO address as a
  reference value. Oracle scope is valid NK-entry-onward (OpenBIOS ≠ Apple OF pre-NK).
- **/tmp provenance ritual:** before tagging anything [STATIC] from `/tmp/newsheep/dump-9.0.1`
  or the parcel, confirm existence + md5 against the recorded anchors; record the md5 in the
  FINDINGS addendum. Re-extract from the canonical `66210b4f…` ROM if /tmp evaporated.

## Tasks

### Task 0 (this plan IS Task 0 — there are no follow-on tasks here; the deliverable is the addendum + verdict)

- [ ] **Provenance first.** Re-verify the canonical ROM md5 `66210b4f…`; confirm
  `/tmp/newsheep/dump-9.0.1` + the NanoKernel-v02.27 parcel exist and match; re-extract if
  needed. Record md5 anchors in the addendum. (Coordinator or either agent's first step.)
- [ ] **(Q1) Emulator-host REPLACE-vs-FIGHT ledger** [STATIC] — Agent P1. For each of the 5
  surfaces (Execute68k pair / exception `ExcEnter` path / MixedMode `execute_68k` excursion
  / scheduler-DEC cadence / entry-vector synthesis), disassemble the NanoKernel-v02.27
  cluster that owns it (≤2 levels / ≤12 functions each) and produce a row: does the real NK
  **overwrite/own** that state (SS yields cleanly = replace) or does it **install live state
  SS also drives** (= fight)? Anchor the Execute68k row on the NK's writer of [KDP+0x1074/
  0x1078] and confirm whether it points them at the ROM EmulatorCode @0x360000 (FINDINGS
  reconciliation row). **Deliverable: the ledger table + a one-line per-row replace/fight
  verdict + a roll-up (any load-bearing FIGHT ⇒ months).**
- [ ] **(Q2) MMU / V=P verdict** [STATIC] — Agent P1 (same disasm artifact). Locate the NK
  consumption of the Trampoline's `/mmu` claim/translate/map. Falsifiable verdict:
  *satisfiable within V=P (identity/window; NK never depends on real translation)* — OR
  *requires a live paged MMU (NK installs+reads BAT/SR/SDR1 or branches on translated≠
  physical)*. **Deliverable: the verdict + the disasm evidence (instruction addresses) that
  decides it; if ambiguous, the conservative residue verdict.**
- [ ] **(Q3) Direct NanoKernel→CGRP trace** [QEMU-BEHAVIORAL] — Agent P2. §6c: QEMU first.
  Boot the rig with `-S` halt-at-reset; using `gdbcli.py`, breakpoint into the
  NanoKernel-v02.27 region (the CGRP-builder around `0x503148e0` / `[KDP-0x338]`); trace the
  device-tree→CGRP construction: which DT properties it reads (`interrupt-map`/`-mask` on
  `/pci/mac-io/interrupt-controller` is the expected load-bearing input per Q0-B), what it
  writes into the CGRP family (`[CGRP+0x38/0x3c/0x40/0x44]`), and **in what order**.
  **Deliverable: the pinned, ordered read/write contract our synthesized DT must satisfy
  — replacing the current INFERRED gap. Tag [QEMU-BEHAVIORAL]; no QEMU address cited as a
  reference value.** Residue fallback (static NK disasm, tagged [STATIC-inferred]) if the
  breakpoint doesn't land within 2 boots.
- [ ] **Fuse + verdict** (coordinator). Write the FINDINGS addendum with all three answers,
  tags, and provenance. State the **GO/NO-GO** and the **weeks-vs-months** verdict, derived
  per the blocking-answer table (Q1 fight OR Q2 paged-MMU ⇒ months; both clean ⇒ weeks;
  conditional verdict if a residue is open). **Gate:** the addendum exists; every answer is
  tagged; every blocking answer is pinned OR has a recorded residue disposition; the verdict
  is stated. Commit (docs-only; no gates per §6).

## Stop-rule (triggers named in advance; one-iteration mechanics operationalized)

1. **The tempting wrong fix — "forge CGRP again."** If Q3's residue tempts anyone to
   hand-write the CGRP table (the banked M16 forge), STOP. The M8→M17 arc proved per-wall
   forging is structurally bankrupt (AGENT-CONTEXT M17: series tripwire). The deliverable is
   the *contract a real producer satisfies*, never a forged output. Route A is decided.
2. **The tempting wrong fix — "start writing the loader / OF-CI callback mid-recon."** This
   Task-0 writes NO `SheepShaver/src/**`. If a recon agent begins drafting `of_ci_callback()`
   or the DT model, STOP — that is the SEPARATE implementation plan, gated on these answers.
3. **Unbounded disasm.** If Q1 or Q2 exceeds its function/instruction window without a
   verdict, STOP that thread and record the conservative residue (FIGHT?-residue /
   requires-paged-MMU-UNKNOWN) — do NOT keep disassembling. (Two predecessor agents died
   this way; the window is the kill-switch.)
4. **QEMU-as-address-oracle.** If Q3 finds itself about to cite a QEMU MMIO address as a
   reference value for our machine layer, STOP and re-tag as behavioral/topology only.
5. **The verdict itself is months.** If Q1=fight on a load-bearing row OR Q2=requires-paged-
   MMU, that is the ANSWER (weeks→months), NOT a trigger to re-scope Route A. Record it;
   the implementation plan inherits the larger scope. Route A is not relitigated.

**One-iteration rule (operationalized):** if a pinned answer is falsified by a later step
within this Task-0 (e.g. a Q1 "replace" row contradicted by a Q3 observation), (a) reopen
the addendum with a dated falsification entry; (b) ONE bounded re-pin (≤1 disasm window or
≤1 QEMU boot from the remaining cap); (c) resume. A SECOND falsification of the same answer
⇒ STOP, escalate to a re-plan of this Task-0 (re-scope, not patch-on-patch).

## Self-review record

Spec coverage: all coordinator inputs consumed — WORKFLOW §2/§4/§6/§6c (Task-0 shape,
parallel boot-disjoint split, gate tiers, QEMU-before-static-RE), FINDINGS "SS-integration
sketch" (the reconciliation table seeds the Q1 ledger) + "SS_M18 gating risks" (Q1/Q2/Q3
verbatim), the syscall-plan format exemplar (PASS-vs-DIAGNOSTIC split, blocking-answer
table as residue-map, budgets with written fallbacks, named stop-rule incl. tempting-wrong-
fix, one-iteration mechanics, tensions-for-red-team, empty Red-team record), AGENT-CONTEXT
constants + QEMU caveats. The plan is recon-only and writes no source — it ends at GO/NO-GO
+ weeks-vs-months + the pinned DT→CGRP contract, with implementation explicitly deferred to
a separate gated plan. Q1/Q2 are static-primary (they ask "does our code collide", not
"what does a working boot do at X" — so §6c's QEMU-first does not bind them); Q3 is QEMU-
first per §6c. Parallelization follows §4: P1=Q1+Q2 (static, one disasm artifact),
P2=Q3 (QEMU), boot-disjoint, fused by the coordinator.

**Tensions flagged FOR the red team:**
1. **Q1 is the months-maker and I cannot derisk it on paper here.** The Execute68k seam is
   the crux: FINDINGS calls the [KDP+0x1074/0x1078] re-bind "the one genuinely SS-specific
   seam (KEEP / re-inject)". But `execute_68k` @ glue:1504–1505 reads that pair LIVE every
   excursion, and SS *is* the JIT. If the real NK doesn't merely rebind a pair but takes
   ownership of the whole 68k-dispatch + exception + scheduler regime that M0–M13 built,
   "re-inject post-handoff" may be a fiction — there may be no coherent "after the NK runs"
   moment where SS's `Execute68k()` path is still the live emulator. RED TEAM: is the Q1
   ledger's replace/fight dichotomy even the right frame, or is there a third "co-own /
   time-share" outcome the ledger format hides? If so the weeks-vs-months heuristic is
   wrong.
2. **Q2's "satisfiable within V=P" optimism vs. M5 deferral.** The whole machine layer is
   V=P by construction (`sysdeps.h`:96; no paged MMU since M5). If the NK genuinely installs
   and reads back page-table/BAT state, Q2=months and it collides with a deliberately
   deferred subsystem. I've set the residue default to the conservative (months) side, but
   the disasm window may be too small to see a *lazy* MMU dependence (NK installs state now,
   depends on it only much later, past the window). RED TEAM: is the ≤60-min / consumption-
   cluster window large enough to falsify "NK never depends on real translation", or does a
   clean Q2 verdict require tracing further than the budget allows (making "satisfiable
   within V=P" an unprovable-within-budget claim that should default to months)?
3. **Q3 and the OpenBIOS≠AppleOF / address caveat could mislead the CGRP contract.** Q3
   traces the NK building CGRP under the QEMU rig — but the rig's device tree is OpenBIOS's,
   not Apple OF's (the Trampoline RE already logged "expected divergences": more getprop
   keys, concrete QEMU device paths). If the NK's CGRP build reads a DT property whose
   *value/shape* differs between OpenBIOS and Apple OF, the "pinned contract" could encode an
   OpenBIOS-ism. The §6c caveat protects addresses but NOT property *shapes/values*. RED
   TEAM: is the DT→CGRP contract safe to pin from the QEMU oracle at all, or must Q3 pin only
   the *mechanism* (which properties, what write order) and explicitly defer the property
   *values* to the synthesized Core99 DT (CORE99-MACHINE-DESCRIPTION.md), exactly as the
   Trampoline RE gated on mechanism-not-values?
4. **The weeks-vs-months heuristic is binary on inputs that may be graded.** I derive the
   verdict mechanically (any load-bearing FIGHT or paged-MMU ⇒ months). Reality may be "weeks
   for the loader, months for the MMU un-deferral" — a split the single verdict flattens.
   RED TEAM: should the deliverable require a *per-surface* effort estimate rather than one
   weeks-vs-months roll-up?

## Red-team record (2026-06-14 — 2 parallel reviewers, both GO-WITH-FIXES)

PROCESS reviewer: 1 Critical (C1), 4 Major (M1–M4), 4 minor; explicit closures on all 4
self-review tensions. TECHNICAL/CONTRACTS reviewer: 1 Critical (F1), 1 Major (F2), minors
(F3); re-verified provenance + every source anchor against the live tree. The two Criticals
**converge** on Q3 (the breakpoint target is wrong AND it's an inverted address-oracle).

**Verified source corrections (coordinator re-confirmed before folding):**
- Glue path is **`SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp`** (the `src/Unix/` prefix
  used loosely above is WRONG — that file does not exist; every `[STATIC]` glue cite must
  use the kpx_cpu path). Execute68k pair read live at **:1504–1505** (CONFIRMED, every
  excursion). `reset_supervisor_for_test` body **:560–569** (not :555).
- **No page-table walker exists** (grep `tlbie|get_physical_address|htab|page_table` over
  the PPC cpu sources → none) — V=P is structural. BUT BAT/SDR1/SR are writable+readable
  **live** via interpreter `mtspr/mfspr` (the aarch64 JIT falls back for unknown SPRs), so
  a "write-BAT-then-read-BAT" check passes **inertly** and is a **false-clean** for Q2.

## Rev-2 BINDING amendments (these OVERRIDE the draft body above where they conflict)

**A1 [C1+F1, Critical — Q3 re-targeted].** `0x503148e0` is the CGRP **service/dispatch**
routine (M16-FINDINGS:71,77 — indexes by source#, self-guards `beqlr`, `rfi`s), NOT the
builder. M16 RE further found the **`"CGRP"` tag (`0x43475250`) appears 0× in the ROM** and
**no inline ROM builder stores to `[base+0x38/+0x3c/+0x44]` off `*(KDP-0x338)`** (M16:107–109)
— i.e. the descriptor is materialized at runtime by **disk/CFM-loaded IM-init**, contradicting
Q0-F's *inference* that the NK parcel builds it. **Q3 is re-chartered to RESOLVE this
contradiction directly.** Concretely:
  1. **Q3.0 precondition (also closes the inverted-address-oracle hole):** SheepShaver's
     `0x5031xxxx` / `[KDP-0x338]` / `0x503148e0` are **SheepShaver-space ORIENTATION ONLY** —
     do NOT set a QEMU breakpoint on them. First derive the **live NK run base** under QEMU
     from the Trampoline's `NanoKernelEntry` handoff (read where it jumps), and the **live
     CGRP descriptor base** from a register/memory snapshot.
  2. **Instrument = memory watchpoint, not PC breakpoint:** once the live CGRP base is known,
     set a gdbstub **watchpoint** (`Z2`/`Z4`) on the descriptor fields (`+0x38/+0x3c/+0x44`)
     to catch **the writer's PC wherever it lives**, then disassemble around that PC.
     `gdbcli.py` currently has only the bp step-over path — **add a watchpoint path** (note
     it as a tooling sub-task; tag the artifact md5).
  3. **Pinned Q3 deliverable now includes "builder identity + location":** NK-v02.27 parcel
     vs disk/CFM IM-init (outside the parcel). This feeds weeks-vs-months directly — if the
     builder is disk-resident IM-init, Route A needs the disk System/Enabler IM-init in the
     loop to produce CGRP at all (surface as a finding; do NOT relitigate Route A).

**A2 [M1, Major — Q3 PASS reconciled].** Goal-3 PASS = a **pinned ordered read/write
contract tagged EITHER [QEMU-BEHAVIORAL] (traced) OR [STATIC-inferred] (residue fallback)** —
both satisfy Task-0. A [STATIC-inferred] result is an explicitly-flagged OPEN item the
implementation plan's own Task-0 must close by tracing. (Removes the traced-vs-inferred
contradiction.)

**A3 [M2+F2, Major — Q2 asymmetric evidence].** Replace the Codebase-fact "BAT/SDR1 touched
only in the test helper" with: *"BAT/SDR1/SR are writable+readable live via interpreter
mtspr/mfspr (JIT falls back: ppc-jit.cpp aarch64 ~:1987/:2022 → ppc-execute.cpp ~:1607/1626/
1539/1561), stored **inertly** — no walker/htab/tlbie consumer exists; V=P is structural."*
Q2's verdict rule becomes asymmetric: within budget Q2 may return ONLY **(a) requires-paged-
MMU** — POSITIVE evidence (cited instruction addresses of a **branch on translated≠physical**
or a genuine dependence on translation occurring; readback-consistency is a **false-clean**
and is NOT evidence) — or **(b) requires-paged-MMU-UNKNOWN → months-risk** (conservative
residue). A bare "satisfiable within V=P" is **forbidden** unless the agent also pins the
bound where translated addresses are consumed and shows it lies inside the traced window.

**A4 [M4+Tension-1, Major — Q1 third verdict + handoff-boundary precondition].** Q1 answers
a **precondition row FIRST:** *"Does a coherent post-NK-init handoff boundary exist — a point
where NK init completes and yields to a steady state in which SS's `execute_68k`/`ExcEnter`/
scheduler path is the live emulator? Cite NK disasm."* Add a **third ledger verdict
CO-OWN/TIME-SHARE** (NK and SS alternate ownership of live state with no clean yield point).
Roll-up: **any load-bearing FIGHT *or* CO-OWN ⇒ months; a MISSING handoff boundary ⇒ months
and is itself the headline finding** (it means "re-inject post-handoff" is fiction).

**A5 [M3, Major — provenance race].** Provenance is a **coordinator-serialized pre-dispatch
gate**: re-verify/re-extract md5 anchors and record them in the addendum BEFORE dispatching
P1/P2. Recon agents treat all `/tmp/newsheep/*` artifacts as **read-only** and never
re-extract.

**A6 [Tension-3, Major — Q3 pins mechanism, defers shape].** Extend the Q3 caveat beyond
addresses: *"Q3 pins only the MECHANISM — which DT properties are read, what is written into
the CGRP family, and in what ORDER. Property VALUES and SHAPES (tuple layout, lengths,
encodings) are OpenBIOS-specific and are explicitly DEFERRED to the synthesized Core99 DT
(`docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md`), exactly as the Trampoline RE gated
on mechanism-not-values. Pinning a property shape/value from the QEMU oracle = stop-rule
violation."*

**A7 [Tension-4, Major — per-surface effort bands].** The deliverable requires a
**per-surface effort band** {Trampoline loader+CHRP entry; OF-CI callback dispatch; Core99 DT
model; `/mmu` backend-vs-paged-MMU; Execute68k re-bind / 68k-regime} each tagged weeks /
months / UNKNOWN-months, with the single roll-up = max() **derived from** (not replacing) the
per-surface breakdown.

**A8 [new stop-rules].** #6 — do NOT break on a SheepShaver NK address under QEMU; derive the
run address from the live handoff first (inverted address-oracle). #7 — never assert
"satisfiable within V=P" from absence of install code within the window; default to
UNKNOWN→months.

**A9 [minors].** NO-GO is narrowed to "Route A not viable *as a SheepShaver milestone on this
host*" — distinct from GO-but-months (which is the expected non-optimistic outcome). The
**non-MMU `call-method` backends** (`read-blocks`/display/`instantiate-rtas`) are NOT
recon-blocked (Q0-A bounded them) — they are implementation-plan items, stated here so their
absence from the blocking table is not read as a gap. Reserve **1 boot of the ≤4 QEMU cap**
for a one-iteration re-pin; a partial-within-boot ordered prefix is an acceptable
[QEMU-BEHAVIORAL] deliverable.

**Disposition:** GO-WITH-FIXES → all amendments folded BINDING. Recon dispatches (P1 static
Q1+Q2; P2 QEMU Q3) carry A1–A9 in their cards. The Q0-F-inference vs M16-RE contradiction
(A1) is the headline thing Q3 exists to settle.
