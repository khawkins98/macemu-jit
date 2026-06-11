# M6a rung 2 — Mixed Mode switch completion: FE01 68k→PPC context switch, FE02 switch-back, NK entry-vector slots, real ongoing entry at table[0]

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the DR Emulator's Mixed Mode switch on the newworld profile so MPLibrary's
PPC TVector (`0x500cef8c`) actually EXECUTES: the $AAFE RoutineDescriptor → FE01 service
allocates a 0x220-byte save record from a **provisioned** pool (Task T promotes
`SS_NW_MM_POOL` to the profile default), the FE01 service's continuation runs through **real
NK entry-vector slots** instead of falling through zeros into the table[0] reset (Tasks U+V:
save the 68k state per the 0x220-record contract, enter PPC at the TVector under the
MixedMode register/MSR convention), the PPC code returns via the **FE02 switch-back** (Task
W: restore the 68k context, free the record, resume past the $AAFE site), and table[0] gets
a **real ongoing entry** (Task X: retire the always-cold-start diagnostic per
M6A-ONGOING-ENTRY-DESIGN R2/R3/R4). Milestone acceptance DIAGNOSTIC (not a gate): the
FE01-retry-spin (68k stub `0x10008fb0` ↔ FE01, ~55M blocks/s) disappears and the 'pwpc'
parcel-init chain proceeds (CodeFragmentMgr lookup at `0xf46e` reached). PASS/FAIL gates are
the regression invariants plus the verifiable sub-contracts in Task Y. Paravirtual
byte-identical throughout (every touched site is newworld/lenient-gated guest-side patching
or profile-gated glue).

**Architecture:** All switch machinery is **guest-side code in the mirror zero run**
(ROM+0x429xxx, the established [NW-TRAMP] idiom) plus glue-side seeds — no new host-side
delivery mechanism and no powerpc_cpu changes. The DR's own FE01 service (allocator at
staged `0x5046e304`, glue blocks `0x5046e1a0..e2ec`) already does the heavy lifting; our
obligation is the surface the NK provides on real hardware: the ECB pool words
(`[ECB+0xE0..0xEC]`), the NK-populated entry-vector slots (base `0x5046e8c0`; the zero
slots `+0x10`, `+0x18..+0x3c`), and a table[0] that distinguishes cold start from ongoing
entry (design doc R2 discriminator + R3 ongoing arm + R4 `[KDP+0x660]` flags seed, with the
`[KDP+0x5f0/0x5f4]` mirror retarget if the stub route is chosen). **Task 0 is a BINDING
pre-implementation recon**: the design doc decides the ongoing-entry shape but deliberately
leaves the FE01-specific contracts open (0x220 record layout, post-allocator control flow,
per-slot semantics, FE07/FE02 mechanics, TVector register/MSR convention) — those get
pinned in writing before Tasks U/V/W freeze. New machinery is env-gated during bring-up
(`SS_NW_MM_SWITCH=1`, default OFF) and flipped to the profile default only by Task Y's
gates.

---

## Authoritative inputs

| Doc | What it fixes |
|---|---|
| `docs/planning/machine/M6A-ONGOING-ENTRY-DESIGN.md` | THE ongoing-entry design: table[0]/slot semantics (§1.2–1.3), NK save/restore protocol (§2), R1–R6 requirement list (§4), rung ladder (§5), PROBE-O1/O2/O4; the R3 ongoing-arm decision and the `[KDP+0x5f0/4]` mirror wrinkle |
| `docs/planning/machine/M6A-WAVE2-SHIM-RECON.md` — "MPLibrary bail NAMED" section (commit 36f048a0) + the main-thread-recon section before it (b3a947e4) | THE FE01 facts: allocator `0x5046e304`, ECB+0xE0..0xEC pool contract, entry-vector zero slots (`+0x10`, `+0x18..+0x3c`; `+0x40` fallback ends `b table[0]`), the FE01→retry-spin frontier under SS_NW_MM_POOL=1, the 68k retry stub `0x10008fb0` (`dc.w $FE07; bne.b +2; rte`/`jmp 0x500049c4`), the TVector `0x500cef8c`, `[r3+0xd8]` current-record link at `[ECB+0x710]+0xd8`, the SS_M6A_USER_MSR zero-page-slide note, the r24-ring/probe methodology |
| `docs/planning/machine/M6A-DR-HANDOFF-ANALYSIS.md` | DR dispatch geometry + register map (r24/r29/r30/r31/r1/r25/r27, cr2), UserModeMSR `0x0000D032` history, scheduler/restore anatomy (§2.4), rung-1 results |
| `docs/planning/machine/M3A-ENTRY-TABLE.md` | KDP-shim entry contract, `[KDP+0x65c]/[0x660]` live values, EE deferral semantics, carry-forwards (syscall_entry unresolved — descope active) |
| `docs/planning/HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` §2.8 | Background obstacle map only (Phase 1–2 framing; the sc anomaly context) — NOT a spec for this plan |
| `SheepShaver/src/rom_patches.cpp` (`PatchROM_NW_trampoline` :714–889, SS_NW_MM_POOL block :794–826, SS_M6A_USER_MSR block :829–858, vector stop stubs :862–875) + `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` (`[NW-TRAMP]` :1737–2090, KDP/ECB seeds :1973–2069, `deliver_pending_dec_exception` :768ff) | The live scaffolding every task edits; the guest-side-survives-ECB-rebuild idiom |

## Codebase facts (carried; implementers re-verify sites before editing)

- **The reboot loop is dead under the pool; the frontier is the retry spin.** With
  `SS_NW_MM_POOL=1`: allocator success path executes (`0x5046e324`), `[0xEC]` cycles free,
  zero reset transitions; FE01 → NK roundtrip → record allocated → return to the 68k stub
  `0x10008fb0` → FE01 again (~55M blocks/s, comp frozen 3573, exc=0/52/0 DEC deferrals).
  The TVector `0x500cef8c` never executes (probe: 0 visits). `SS_M6A_USER_MSR=1` on top
  reproduces the zero-page-slide crash (pc marches to 0x100000) — EE alone is not the unblock.
- **Pool seeding is guest-side per-entry by necessity**: NK cold-init rebuilds the ECB every
  cycle, so glue-time seeds don't survive (the Hnfo re-assert precedent). The current seed
  zeroes `[ECB+0xEC]` (in-use bitmap) on EVERY table[0] entry — harmless under always-cold,
  **state-destroying once warm re-entry exists**: Task X must keep pool (re)seeding on the
  COLD arm only.
- **Entry-vector table** (`0x5046e8c0`, published at `[KDP+0x648]` — glue seeds the mirror
  value): static branches at `+0x00/04/08/0c/14` (slot semantics per design doc §1.2:
  start/MM-switch/reset/FE0A/FE0F); `+0x10` and `+0x18..+0x3c` observed zero live;
  `+0x40` fallback stub ends `b table[0]` (`0x5046e960: 4bffff60`). The design doc's 8-slot
  table (§1.2, POWERPC_ILLEGAL at 4/6/7) and the recon's 16-slot reading (`+0x3c` is the
  allocator's no-free-bit target) are NOT fully reconciled — Task 0 owns this (see plan-level
  contradiction list in the self-review record).
- **The slot-0 stub exits via `[KDP+0x5f0]` = primary-world `0x366080`** (dormant
  cross-world constant, glue :1986–1987, deliberately left by Wave-1 finding 6). Any rung
  that makes a stub route live must retarget `[KDP+0x5f0]/[0x5f4]` → `0x50466080` (mirror).
- **NK save/restore is resume-correct with the M3a shim** (design doc §2): shim saves
  r7–r13, NK saves r14–r31/r1/CR/XER/CTR through r6=ECB; scheduler restore resumes at
  `r10=r12=restart`. Table[0] is only the first-dispatch/fault route today.
- **R1 (r1-independent UserModeMSR load) is ALREADY LANDED** in the trampoline
  (immediates, rom_patches :852–854) — yet MSR-on still crashes; the MSR story across the
  MM switch is an OPEN question for Task 0, not a settled input.
- **Trampoline budget**: the 222KB mirror zero run at ROM+0x329b40 has ample room; current
  code ends ≤ ROM+0x429bb8, stop stubs at 0x429c00/10/20 — new handlers/stubs go above
  0x429c30, each site verified zero before writing (the existing skip-if-nonzero idiom).
- **Suite shapes**: machine suite 11/11 binaries; `make test-jit` 353/353 (batch
  `SS_HARNESS_BATCH=1` inner loop, plain legacy run authoritative per task); e2e-test 122;
  paravirtual `make e2e` PASS + byte-identical logs.
- **Standing rules**: per-task commits; §2g locking (no stdio/malloc on bus-lock paths —
  largely moot here, all guest-side); struct fields appended LAST (c0e33d2b precedent);
  clean-rebuild awareness — machine-header deps exist since c0e33d2b, but treat any absurd
  counter as a stale-.o symptom first; boots authorized; no worktrees (one shared checkout —
  the controller pre-resolves conflicts; NOTE: Tasks T/U/V/W/X all edit
  `PatchROM_NW_trampoline` and the glue `[NW-TRAMP]` block — run them SEQUENTIALLY).

## Tasks

### Task 0: FE01/FE02 contract recon (BINDING gate for U/V/W/X — static RE + one probe session, boots authorized)
The design doc pins the ongoing-entry shape but the MM-switch contracts are open. Pin each
in a written addendum (append to M6A-ONGOING-ENTRY-DESIGN.md, "Rung 2 contracts" section)
BEFORE any implementation task freezes:
- [ ] **(Q-A) The 0x220 save-record layout**: capstone the FE01 service from the allocator
  success path (`0x5046e324` onward, glue blocks `0x5046e1a0..e2ec` and beyond) — what the
  DR itself writes into the record vs what it expects the slot handlers to write; the
  `[ECB+0x710]+0xd8` current-record link discipline.
- [ ] **(Q-B) Post-allocator control flow + per-slot semantics**: which entry-vector slots
  the FE01 success path branches to, in what order, with what register state; reconcile the
  8-slot vs 16-slot table reading (is POWERPC_ILLEGAL == 0, explaining the observed zeros?
  re-read `patch_68k_emul` :1426–1448 and the raw-ROM placeholders); name the minimum slot
  set Task U must populate and what each must do.
- [ ] **(Q-C) The retry loop's pivot**: why FE01 currently retries — trace the bail after
  allocation success (zero slot? a flag the stub at `0x10008fb0` polls? FE07 semantics —
  what opcode service is FE07 and what does its `bne` test?). This names the exact
  condition Task V must satisfy to break the spin.
- [ ] **(Q-D) The TVector entry convention**: MixedMode 68k→PPC calling convention at
  `0x500cef8c` (TVector = entry+RTOC: which registers carry procInfo args per
  procInfo=0xE1, where the return address points, what r1/stack the PPC code gets) — from
  the FE01 service disassembly + the RoutineDescriptor at `0x10024758`.
- [ ] **(Q-E) MSR across the switch**: what MSR the PPC code must run under (UserModeMSR
  `0xD032` family? EE state?), where the transition executes, and the interaction with
  `SS_M6A_USER_MSR` (root-cause its zero-page slide far enough to decide whether Task V
  subsumes, fixes, or sidesteps it). Decide guest-side trampoline vs host-side glue for the
  switch site (default expectation: guest-side, per the architecture note — justify if not).
- [ ] **(Q-F) FE02 switch-back mechanics**: where FE02 is issued (the MixedMode glue the
  PPC code returns into?), what it reads from the record, where the 68k resumes, who frees
  the pool bit.
- [ ] **Design-doc probe pack rolled in**: [PROBE-O1] table[0] entry census (now in the
  pool-on regime), [PROBE-O2] who built the restored ctx / is glue's ECB ctx pre-population
  dead, [PROBE-O4] `[KDP+0x660]` flag consumption — these size Task X's R3 choice.
- [ ] Gate: the addendum exists, every Q-A..Q-F answer is tagged [RAW-ROM]/[PATCH]/[STATIC]/
  [PROBE✓] per the house caveat idiom, and open residues are explicitly listed. Capture-only
  telemetry commits allowed (gates: build-ss; batch + legacy test-jit 353/353; machine 11/11;
  paravirtual byte-identical). Commit.

### Task T: SS_NW_MM_POOL → profile-provisioned default
- [ ] Promote the pool seed from env-gated diagnostic to the newworld profile default
  (flag inverts to an opt-OUT override `SS_NW_MM_POOL=0` for A/B; paravirtual/OldWorld
  untouched — gate on `MachineProfileIsNewWorld()` as today).
- [ ] ECB-rebuild-survival story stays: guest-side trampoline seeding, but RESTRUCTURED for
  Task X compatibility — pool base/existence words may re-assert per cold entry; the
  in-use bitmap `[ECB+0xEC]` is zeroed on the COLD arm only (document the invariant at the
  seed site; until Task X lands, cold-only == every entry, unchanged behavior).
- [ ] Pool sizing: re-verify 4 records suffices for the boot's nesting (Task 0 Q-A's
  record-link discipline informs; if unclear, keep 4 + a loud allocator-exhaustion stop via
  Task U's `+0x3c` handler rather than guessing bigger).
- [ ] Gates: build-ss; batch + legacy test-jit 353/353; machine 11/11; e2e-test 122;
  paravirtual `make e2e` PASS + byte-identical (no `[NW-TRAMP]`/pool lines on paravirtual);
  one newworld diagnostic boot confirming default-on pool = no reboot loop (zero reset
  transitions in the r24 ring). Commit.

### Task U: NK entry-vector slot population
- [ ] Per Task 0's pinned slot map, write real handlers for the consumed slots (guest-side
  stubs in the mirror zero run, branches written into the table — same verify-zero-first
  idiom). **Minimum regardless of recon outcome**: the allocator's no-free-record target
  `+0x3c` becomes a loud diagnosable stop (`bra/b *`-style unique parked PC + probe-able),
  never a silent zero-fall-through into table[0].
- [ ] Slots NOT consumed by the FE01 path stay as loud stops (unique PCs), not zeros —
  every future fall-through becomes signal (the slot-4 "treat a trap as signal" note,
  design doc §6.5).
- [ ] Each slot handler comments cite the Task-0 addendum section. All inside
  newworld/lenient gating.
- [ ] Gates: build-ss; batch + legacy test-jit 353/353; machine 11/11; paravirtual
  byte-identical; diagnostic boot shows the `+0x3c` path no longer reached (pool provisioned)
  and no NEW parked PCs (or: parked PCs are the expected pre-Task-V stops, recorded). Commit.

### Task V: FE01 switch completion — enter the TVector
- [ ] Implement the 68k→PPC switch per Task 0 Q-A/Q-B/Q-D/Q-E: complete the save-record
  contents the DR doesn't write itself (if any), establish the MixedMode register state,
  perform the MSR transition at the pinned site (riding execute_mtmsr's EE-edge re-raise if
  EE rises — the M3a deferral machinery contract), branch to the TVector. Env-gated
  `SS_NW_MM_SWITCH=1`, default OFF until Task Y.
- [ ] Resolve the SS_M6A_USER_MSR interaction per Q-E's verdict (subsume/fix/retire the
  flag — documented either way; the zero-page slide must be explained, not papered over).
- [ ] Controlled-probe sub-contract (PASS/FAIL within this task's power): on a diagnostic
  boot with the switch on, `SS_PROBE_PC=0x500cef8c` shows ≥1 visit with a sane register
  dump matching the Q-D convention. (What the TVector code DOES afterward is diagnostic.)
- [ ] Gates: build-ss; batch + legacy test-jit 353/353; machine 11/11; e2e-test 122;
  paravirtual byte-identical; the probe sub-contract above. Commit.

### Task W: FE02 switch-back
- [ ] Per Task 0 Q-F: the return path — restore the 68k context from the record, clear the
  in-use bit, resume the 68k after the $AAFE site with the convention's result registers.
  Same gating as Task V (`SS_NW_MM_SWITCH` covers the pair).
- [ ] Round-trip sub-contract (PASS/FAIL): one full FE01→TVector→FE02 round trip observed
  (probe pair: TVector visit + post-$AAFE 68k PC visit), `[ECB+0xEC]` returns to its
  pre-call value, guest[0]/[4] unmodified across the round trip
  (`SS_JIT_WATCH_ADDR=0,4`).
- [ ] Gates: as Task V. Commit.

### Task X: real ongoing entry at table[0] (design doc R2+R3+R4)
- [ ] R2 discriminator: scratch word in the sub-KDP pool (mapped+zeroed, data-only — NOT
  the ROM zero run, SMC/JIT-invalidation hazard per design doc), cold-once semantics; pool
  in-use-bitmap zeroing moves strictly inside the cold arm (Task T invariant).
- [ ] R3 ongoing arm: target per Task 0's [PROBE-O1] census + Q-B — stub route
  (`b 0x5046f900` + retarget `[KDP+0x5f0]/[0x5f4]` → `0x50466080`, nest-protocol balanced)
  vs direct re-dispatch (`b 0x50466080`); the design doc's decision rule applies (if
  census says nobody legitimately re-enters, keep always-cold + tripwire counter — record
  the choice). R4: seed `[KDP+0x660]` per [PROBE-O4]'s pinned bit.
- [ ] R5 (nest protocol) only if the chosen route requires balance — otherwise note as
  residue (design doc says cosmetic today).
- [ ] Sub-contracts (PASS/FAIL): cold path runs exactly once per boot (scratch-flag probe);
  guest[0]/[4] never rewritten after first entry (watch); jDR/comp still growing across
  any table[0] re-entries.
- [ ] Gates: as Task V, plus `SS_M6A_USER_MSR`/MSR-state regression per Task V's verdict.
  Commit.

### Task Y: rung-2 acceptance (boots authorized) — honest gating
- [ ] Flip `SS_NW_MM_SWITCH` machinery to the newworld profile default (env opt-out kept);
  re-run the full gate set.
- [ ] **PASS/FAIL gates (in the plan's power):** (a) build-ss; batch + legacy test-jit
  353/353; machine 11/11; e2e-test 122; paravirtual `make e2e` PASS + byte-identical;
  (b) pool provisioned by default (allocator success path, no `+0x3c` stop hit);
  (c) consumed entry-vector slots populated and executed (probe counters nonzero);
  (d) the controlled FE01→TVector→FE02 round trip (Task V/W sub-contracts) holds on the
  default-config boot; (e) cold-start exactly once + guest[0]/[4] stable (Task X
  sub-contracts).
- [ ] **DIAGNOSTIC OUTCOMES (recorded, NOT gates):** the FE01 retry spin gone;
  MPLibrary's parcel init returns; the chain reaches the CodeFragmentMgr lookup
  (`0xf46e`); boot advances past the parcel calls. Competing next walls are already named
  and EXPECTED: (i) MPLibrary's real init work is NK syscalls — the unresolved
  `syscall_entry` (vector 0xC00, currently `SS_EXC_SC=abort`) plausibly fires immediately
  (that abort is SIGNAL: capture SRR0/SRR1 + the sc site, per the M3a descope note);
  (ii) EE/DEC delivery during PPC-native execution (the deferral counters say);
  (iii) further unprovisioned NK surfaces. On non-advancement the deliverable is a
  root-caused frontier in the recon-doc idiom (ring tail + probes + capstone of the bail).
- [ ] Record results in M6A-ONGOING-ENTRY-DESIGN (results section) + M6A-WAVE2-SHIM-RECON
  (frontier update). Commit.

### Task Z: docs
- [ ] DIAGNOSTICS.md: new/changed knobs (`SS_NW_MM_POOL` polarity flip, `SS_NW_MM_SWITCH`,
  SS_M6A_USER_MSR disposition); CHANGELOG (acceptance numbers); MACHINE-LAYER-PLAN M6 row
  (rung 2 landed; next = syscall staging / shim wave per Task Y's captured frontier);
  ROADMAP cross-check; LEARNINGS if non-obvious; cross-tracker grep for stale
  "always-cold"/"SS_NW_MM_POOL default OFF" statements. Commit.

## Stop-rule
Two triggers, per MACHINE-LAYER-PLAN §9: (1) if Task 0 shows FE01 completion requires the
NK's full emulator-context-creation surface (the handoff memo's rung-5 synthesized
Trampoline, L-class) rather than slot population + record plumbing, STOP after the addendum
and re-scope — no tunneling. (2) If Task Y's diagnostic shows MPLibrary advances past FE01
but dies on the syscall entry or another NK service, that is the NEXT milestone's named
frontier — capture and stop; do not extend this plan into vector-0xC00 staging beyond the
existing abort-with-capture. Within tasks: one design-iteration maximum per pinned contract
(if a Q-A..Q-F answer proves wrong live, record the falsification and return to Task 0
scope, don't guess twice).

## Self-review record
Spec coverage: all six authoritative inputs consumed; the coordinator's T/U/V/W/X seams map
to Tasks T/U/V/W/X with Task 0 carrying every open design question as recon (Q-A..Q-F +
PROBE-O1/O2/O4). Honest gating separates suite/probe sub-contracts (PASS/FAIL) from boot
advancement (diagnostic). Wave-2-deferred backlog (OpenPIC model+tests, tm_task/via_int
dispositions, io_poll retirement, the shim wave) is explicitly OUT — parallel backlog, not
this plan. **Contradictions flagged for the red team:** (1) design doc §1.2's 8-slot table
with POWERPC_ILLEGAL at slots 4/6/7 vs the recon's 16-slot reading with zeros at
`+0x18..+0x3c` and `+0x10` listed among NK-populated slots — possibly reconciled by
POWERPC_ILLEGAL==0, but slot 4's "deliberately dead interrupt vector" story and the
fail-slot `+0x3c` story must not collide (Task 0 Q-B). (2) The design doc's "table[0]
re-entry is probably rare-to-never" ([PROBE-O1] framing) is superseded by the recon: the
FE01 zero-slot fall-through re-enters table[0] constantly — but that route is the FAULT
path this plan removes, so the census question revives in the post-fix regime. (3) Design
doc R1 is already landed (r1-independent MSR load in rom_patches) yet `SS_M6A_USER_MSR=1`
still dies (zero-page slide) — the doc's "R1 un-blocks Boot B" prediction is falsified;
the MSR story is reopened as Q-E. (4) Naming: this plan's "rung 2" (coordinator scope) is
strictly larger than the design doc's Rung 2 (R2+R3+R4); the mapping is stated in Task X.
Sequencing risk named: T/U/V/W/X all edit the same trampoline/glue functions — sequential
execution mandated.

## Red-team record
_To be filled (red-team round happens after this draft)._
