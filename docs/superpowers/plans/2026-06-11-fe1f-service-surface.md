# M6 next milestone — the FE1F service surface: provision the DR's native-callout service (opcode $FE1F, selector 0x31) so the 68k CFM-prep routine fills its ExpandMem slot and the newworld boot advances past 0x5000f248

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. **Dispatch ONE task at a time — Tasks A/B/C all touch the same two files (`sheepshaver_glue.cpp`, `rom_patches.cpp`); NOTHING in this plan parallelizes.**

**Goal:** On the default newworld profile (MixedMode switch + syscall surface both
default-on), the boot runs 5 NK syscalls, MPLibrary's excursion returns to the 68k
world, and the 68k advances through the A-line dispatcher (0x5000dfa2) and a CFM-prep
routine (0x5000e3e0..e43c: `_GetToolTrapAddress($AA7F)` availability check + an
ExpandMem-anchored pointer-array `([$2b6],$310)` slot fill) until **0x5000f240 issues
F-line trap `$FE1F` with selector d0=0x31 and parks at 0x5000f248** — ring tail
`… 5000e43e → 5000f242 → 5000f246 → 5000f248`, 839,284 transitions, byte-identical
across three boots, heartbeat silent afterward while wall-clock continues (full
capture: M6A-WAVE2-SHIM-RECON.md "Frontier update (2026-06-11, NK-syscall-surface
Task C closeout)"; M3A-ENTRY-TABLE.md "Task C results"). This milestone resolves that
park the machine-layer way: **the DR emulator's own FE1F service runs** — FE1F is a
real raw-ROM DR opcode (pre-verified static, Task 0 re-verifies), whose dispatch slot
loads a **continuation pointer from `[ECB+0x9dc]`** and `blrl`s to it (the FE01
`[ECB+0x9e0]` sibling pattern, rung 2). Our obligation is rung-2-style provisioning:
pin what `[ECB+0x9dc]` must hold and what the selector-0x31 service needs, seed it,
and let real ROM/NK code do the work. Milestone acceptance DIAGNOSTIC (not a gate):
the 68k advances past 0x5000f248 — the ExpandMem slot fills, the e3e0 routine
completes, and the boot reaches its NEXT wall (captured, named, stopped per the
stop-rule). PASS/FAIL gates are the regression invariants plus the falsifiable
sub-contracts below. Honest framing: the e3e0 routine's id→index table has 5 entries
and FE1F is a GENERIC callout opcode — this milestone owns the FE1F *surface*
(continuation provisioned + selector 0x31 conformant), not every selector's semantics;
subsequent selectors/walls are captured-and-stopped. Paravirtual byte-identical
throughout. Wave-2 backlog (OpenPIC wiring, EE chain) stays parallel and untouched.

**Architecture:** No host-side execution seam and no powerpc_cpu changes — the FE1F
dispatch and service body are guest-side DR code that already executes under the
Wave-1 dispatch-contract repair (`handler = (r29 & 0xFFF80007) | (opcode<<3)`, mirror
table 0x50480000). The expected implementation is SMALL and rung-2-shaped: pin the
continuation contract (Task 0), then seed `[ECB+0x9dc]` (glue ECB pre-population
site, the `[KDP+0x65c]`/ctx-seed precedent at sheepshaver_glue.cpp ~:2063–2144 —
or PatchROM-time trampoline region if the writer analysis demands a post-NK-init
assert) plus whatever seed-class state the selector-0x31 service consumes. **If the
native service body for selector 0x31 is genuinely absent from the staged image, the
stop-rule fires — a host-side HLE NativeOp is the paravirtual idiom and is explicitly
NOT this milestone's move** (re-scope decision recorded instead). **Task 0 is a
BINDING pre-implementation recon gating Tasks A..C.** New machinery is env-gated
during bring-up (`SS_NW_FE1F_SURFACE=1`, default OFF) and flipped to the newworld
profile default only as Task C's LAST step with explicit revert-on-red. Gated OFF,
the 0x5000f248 park baseline is byte-identical.

---

## Authoritative inputs

| Doc | What it fixes |
|---|---|
| `docs/planning/machine/M6A-WAVE2-SHIM-RECON.md` — "Frontier update (2026-06-11, NK-syscall-surface Task C closeout)" | THE opening evidence verbatim: the ring tail, the 0xdfa2/0xe3e0/0xf240 capstone-M68K naming, the term-dump baseline class (blocks=3836 complete=3836, MMIO S:65540/V:70183, CUDA 13 pkts quiet, VCLK pending=1, heartbeat silence; capture via SIGTERM + SS_TERM_DUMP — SIGALRM skips atexit); the probe methodology + corrections (SS_JIT_WATCH_ADDR hex + SS_JIT_TRACE_RING=1; probe-PC limit 8/run) |
| `docs/planning/machine/M3A-ENTRY-TABLE.md` — "Task C results" | The same frontier from the syscall plan's side: the A-line dispatcher decode, the e3e0 routine decode (id table 0x5000e3a0, ExpandMem array `([$2b6],$310)`, error 0xffff8d8e arm), 0x5000f240's instruction sequence, the park PC; the two-anchor probe methodology, [RAW-ROM]/[PATCH]/[STATIC]/[PROBE✓] tag discipline |
| `docs/planning/machine/M6A-DR-HANDOFF-ANALYSIS.md` | The dispatch geometry LAW: `handler = (r29 & 0xFFF80007) | (opcode<<3)`; r29=0x50480000 (LA_DispatchTable, mirror); the opcode table fully populated at static 0x380000–0x400000, mirror 0x480000–0x500000 branching into the 0x46xxxx mirror emulator; the three-constant Wave-1 repair |
| `docs/planning/machine/M6A-ONGOING-ENTRY-DESIGN.md` (incl. ALL rung-2/W2 results sections) | The FE-opcode family facts this plan extends: FE01's slot `lwz r5,0x9e0(r31)` with **[ECB+0x9e0]=0x5046de1c [PROBE✓]** (RoutineDescriptor parser); FE02/FE07 semantics (FE07 = frame validator polling a command byte — the named alternative park-shape); the entry-vector table slots (FE0A/FE0F have dedicated slots; FE1F does NOT — it is a pure opcode-table service); rung-2 invariants (cold-once, guest[0]/[4], slot-15 stop); the sub-KDP occupancy map (AUTHORITATIVE); `[KDP+0x65c]` world-flip + `[0x2810]` run-mode discipline; the seed idiom + regions |
| `docs/planning/PATCH-68K-SHIM-INVENTORY.md` | Negative evidence: NO patch_68k pattern installs or services $FE1F (greps clean across rom_patches.cpp/emul_op.cpp/sheepshaver_glue.cpp — SheepShaver EMUL_OPs are the 0x71xx family); the only FE-territory patch is `shutdown_dat` ("Don't call FE0A in Shutdown Manager"). FE1F is NOT one of our patch-introduced traps — branch (i) of the milestone's shape question is CLOSED pending Task 0's provenance re-verify |
| `docs/planning/machine/EE-CHAIN-RECON.md` (89fd0642) §A/§B | Contingency map ONLY: if Q-F5 shows selector 0x31's service blocks on tick/interrupt state (XLM_IRQ_NEST, the polled trampolines), the real dependency is the EE-chain milestone — stop-rule trigger 1 names it; NOT a spec for this plan |
| `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` (ECB pre-population ~:2063–2144; `[NW-TRAMP]` announce :2128; DEC-shim ECB discipline :804–866, :1152) | The host-side seed site precedent: glue already pre-populates ECB+0xfc/+0x104..+0x7fc at init; a `[ECB+0x9dc]` seed (if Task 0 verdicts it) lands HERE, env-gated, next to its siblings |
| `SheepShaver/src/rom_patches.cpp` (`PatchROM_NW_trampoline` :714ff; W region :1033–1078; W2 slot-1 region) | The guest-side seed idiom: PatchROM-time only, verify-zero-first (verify-EXPECTED for nonzero sites); current high-water mark = W2 slot-1 region 0x429d80..0x429d9c (end-exclusive) — **new regions start at/above 0x429d9c** |
| `docs/superpowers/plans/2026-06-11-nk-syscall-surface.md` | The process template (BINDING): blocking-answer table, budgets, flip-last + revert-on-red, the canonical gate set, the one-iteration mechanics, the P-M4 frontier-capture artifact |
| `docs/planning/MACHINE-LAYER-PLAN.md` §2d + M6 rows; §9 stop-rule | Placement + the CPU-core honesty rule (no new host seams for guest-executable services) |

## Codebase facts (carried; implementers re-verify sites before editing)

**Free static wins (pre-pinned 2026-06-11 from /tmp/rom901_inventory.bin (raw) vs
/tmp/rom901.bin (patched), md5/provenance to be RE-ESTABLISHED by Task 0 before any
[RAW-ROM]/[PATCH] tag is reused — /tmp files evaporate):**

- **THE PROVENANCE ANSWER (the milestone's shape question): the `$FE1F` at file
  0xf240 is RAW ROM** — bytes at file 0xf230..0xf260 byte-identical raw vs patched:
  `0xf240: 4e56 0000` (link a6,#0) / `0xf242: 7031` (moveq #$31,d0) / `0xf246: fe1f`
  / `0xf248: 2d40 000c` (move.l d0,$c(a6)) / `202e 0008 / 6704` (tst.l + beq.s —
  a RESULT TEST: d0==0 skips the store-through) / `2240 2288` (movea.l d0,a1;
  move.l a0,(a1)) / unlk-return tail. This is branch (ii): a real DR service, NOT a
  patch_68k EMUL_OP. (Branch (i) — service-or-retire a paravirtual-fenced EMUL_OP —
  is dead unless Task 0's provenance re-verify falsifies the raw-dump identity.)
- **The FE1F dispatch slot** (static 0x380000 + (0xFE1F<<3) = **0x3ff0f8**; live
  mirror handler = 0x50480000 | 0x7F0F8 = **0x504F70F8**): `80bf 09dc 4bf6 ea48` =
  `lwz r5, 0x9dc(r31); b -0x915b8` → static body **0x36db44** (mirror **0x5046db44**).
  Identical raw vs patched. Same family shape as FE01's `lwz r5,0x9e0(r31)`; the
  FE10..FE1F slot run loads sequential ECB offsets 0x9a0..0x9dc — a 16-entry
  continuation-pointer block at ECB+0x9a0.
- **The FE1F service body** (static 0x36db44, raw=patched byte-identical, capstone
  PPC-BE): `mtlr r5` (continuation = [ECB+0x9dc]) · marshal the DR 68k register file
  into PPC ABI — `mr r0,r8 / mr r3,r16 / mr r4,r9 / mr r5,r17 / mr r6,r10 /
  mr r7,r18 / mr r8,r11 / mr r9,r19` (per the DR convention r8..r15=D0..D7,
  r16..r22=A0..A6: **r0=D0=selector 0x31, r3=A0, r4=D1, r5=A1, r6=D2, r7=A2,
  r8=D3, r9=A3**) · `blrl` (the native callout) · unmarshal `mr r8,r3 / mr r16,r4 /
  mr r9,r5 / mr r17,r6` (**r3 → D0 = the result the 68k stores through the slot
  pointer**) · `li r0,0` · `rlwimi r29,r27,3,0xd,0x1c / mtlr r29 / lhau r27,2(r24) /
  bgelr cr2 / b 0x36d114` — resume 68k dispatch. **FE1F is the DR's generic
  68k→native callout opcode; everything selector-specific lives in the routine at
  `[ECB+0x9dc]`.** Register-map caveat: the D/A→r assignment above is inferred from
  the documented DR register-file convention, NOT yet probe-verified — Q-F3 confirms.
- **No EMUL_OP analog exists**: `grep -ri fe1f` over SheepShaver/src is clean;
  EMUL_OPs are 0x71xx (kpx EMUL_OP_field); patch_68k's only FE-trap touch is
  avoiding FE0A (`shutdown_dat`).

**Carried live/process facts:**

- **The captured baseline (the milestone's opening evidence; re-confirm once before
  Task A):** default newworld config, no FATAL; 5 sc deliveries (selectors
  0x3f/0x19/0x14/0x19/0xf, every sampled resume r3=0); ring tail
  `… 5000e43e → 5000f242 → 5000f246 → 5000f248`, 839,284 transitions; term-dump
  blocks=3836 complete=3836, MMIO S:65540/V:70183, CUDA 13 pkts quiet, VCLK
  pending=1, delivered-DEC=0; heartbeat silent post-sc. Boots die via SIGTERM +
  SS_TERM_DUMP.
- **The park-shape question is OPEN (two named hypotheses — Q-F4 discriminates;
  gates must not presuppose either):** (H1, one-shot-never-returns) the `blrl`
  target is 0/garbage/unstaged and the callout never returns — consistent with the
  f24x PCs appearing ONLY at the ring tail and the body having no poll structure;
  (H2, FE07-style poll) the callout returns d0=0, the 68k's `beq.s` skips the
  store, the caller re-loops — would require the f24x family to RECUR in the ring;
  re-read the full ring (and its wrap/capacity arithmetic — "never wrapped" must be
  re-verified against ring size) before pinning. The d0==0-skip arm in the 68k tail
  is real either way and is the conformance predicate's negative space.
- **[ECB+0x9e0] (FE01's sibling) was live-verified nonzero** (=0x5046de1c [PROBE✓],
  rung 2) but its WRITER was never identified — glue's ECB pre-population
  (~:2063–2144) does not write the 0x9a0..0x9dc block (grep clean). Q-F2's writer
  analysis decides the seed site; the asymmetry (0x9e0 filled, 0x9dc =? ) is the
  likely heart of the park.
- **No entry-vector-table slot involvement**: FE1F has no slot (unlike FE0A/FE0F) —
  the service runs inline on the DR's dispatch path in the emulator world; no
  world-flip is architecturally implied unless the continuation itself switches
  (Q-F4 records [KDP+0x65c]/[0x2810]/XLM_IRQ_NEST interactions as residues unless
  load-bearing).
- **Guest-side seed budget** (if Task A needs trampoline-resident seeds): new regions
  start at/above **0x429d9c**, verify-zero-first, PatchROM-time only, env-gated
  structurally inert when off. **Sub-KDP occupancy map is AUTHORITATIVE** (free
  ranges 0x68FF6084..0x68FF7000) — any new data word goes through the map first.
- **ROM-dump provenance**: `/tmp/rom901_inventory.bin` = the only RAW image;
  `/tmp/rom901.bin`/`/tmp/rom901_patched_t0.bin` = PATCHED. Re-verify existence +
  md5 before tagging; re-dump via SS_DUMP_ROM if evaporated. The 4 MB dump covers
  the primary copy only; **mirror facts (0x504F70F8 slot, 0x5046db44 body,
  live [ECB+0x9dc]) are [PATCH-source] or [PROBE✓] only** (T-M1 carried:
  per-target re-confirmation required).
- **Canonical gate set ("full gates" below means exactly this):** `make -C SheepShaver
  build-ss`; `SS_HARNESS_BATCH=1 make -C SheepShaver test-jit` AND plain
  `make -C SheepShaver test-jit` (both 353/353, legacy authoritative);
  `make -C SheepShaver/src/machine test` ALL PASS (12/12 suites);
  `make -C SheepShaver e2e-test` (122); paravirtual `make -C SheepShaver e2e` PASS +
  byte-identical (no new [NW-*]/[EXC]-shape lines on paravirtual). A task may name a
  SMALLER set only with a stated reason.
- **"Byte-identical" baselines defined** (P-m2 carried): ring-tail PCs exact;
  comp/blocks exact-class (3836); selector list exact (0x3f/0x19/0x14/0x19/0xf);
  HB/exc signature class; raw MMIO counters EXCLUDED (jitter).
- **Standing rules**: per-task commits; struct fields appended LAST (JIT hardcodes
  offsets); stale-.o awareness (rom_patches/glue not covered by machine-header deps —
  clean-rebuild after header changes there); boots authorized; one shared checkout;
  probe-PC limit 8/run; SS_JIT_WATCH_ADDR is hex + needs SS_JIT_TRACE_RING=1;
  68k PCs are PPC-probe-blind — 68k-side evidence rides SS_DR_R24_RING.
- **Tracker concurrency**: Task Z elsewhere is editing tracker docs — this plan cites
  trackers BY SECTION; do not depend on their latest lines; cross-tracker greps in
  this plan's Task Z run at its own execution time.

## Tasks

### Task 0: FE1F callout recon (BINDING — gates Tasks A..C; static RE + bounded probe boots)

Pin each contract in a written addendum (`M6A-ONGOING-ENTRY-DESIGN.md`, new section
"FE1F native callout (selector 0x31) — fe1f-service-surface plan Task 0"). Budget
honesty: static RE (capstone PPC-BE + capstone-M68K, raw + patched dumps with
provenance re-established) is the primary tool; **≤8 bounded diagnostic boots total,
each ≤60s, at most 2 boots per question before its residue status is decided**;
co-scheduling mandated (8 probe PCs/run; one boot may serve multiple questions);
canonical-gate boots (paravirtual e2e) are OUTSIDE this budget. Each Q gets a
static time-box (≤90 min) with a written residue fallback. Capture-only telemetry
commits allowed (full gates).

- [ ] **Provenance first**: confirm `/tmp/rom901_inventory.bin` (raw: 16 `twi`
  placeholders at 0x36e8c0) + `/tmp/rom901.bin` (patched) exist and md5-match the
  recorded values (or re-dump via SS_DUMP_ROM); re-confirm the three free static
  wins above against the re-established dumps (file 0xf240 region, slot 0x3ff0f8,
  body 0x36db44 — raw=patched on all three). **If the raw-dump identity is
  falsified (the $FE1F turns out patch-introduced), STOP: the milestone re-shapes
  to branch (i) (identify the patch + its EMUL_OP semantics; service-or-retire on
  newworld) — re-plan, do not improvise.** Tag discipline carried unchanged.
- [ ] **(Q-F1) The dispatch route, mirror-confirmed**: verify the live route from the
  68k F-line word to the service body — [PROBE-F1] probe `0x5046db44` (mirror body)
  for ≥1 visit at the park boot, and read the live mirror slot words at
  `0x504F70F8` (expect the `lwz r5,0x9dc(r31); b …` pair relocated). Deliverable:
  the route pinned [PROBE✓] (table slot → body → blrl), confirming FE1F dispatches
  through the opcode table under the Wave-1 geometry (NOT the 68k F-line vector
  0x2C stop-stub — if the 0x2C stub is what actually catches it, that is a
  falsification of the whole shape: record + stop-rule trigger 3).
- [ ] **(Q-F2) The continuation pointer `[ECB+0x9dc]` — value and writer** (THE
  blocking question): (a) [PROBE-F2] one probe of live `[ECB+0x9dc]` (ECB from
  `[KDP+0x65c]` discipline — mind the world-flip: read it in the emulator regime,
  i.e., at/near the park, riding PROBE-F1's boot) + the neighboring block
  0x9a0..0x9e0 (one 0x40-byte window — classify all 16 continuation slots, P-M3
  idiom); (b) static RE: find the writer of the 0x9a0-block (who filled
  [ECB+0x9e0]=0x5046de1c on the live boot — search the staged init/parcel code for
  the store family; bounded: ≤2 call levels / ≤12 functions from the candidate
  init roots (DR cold start 0x36e964, MixedMode parcel init, NK cold-init
  publication cluster)); (c) classify the verdict: **(V1) staged-and-correct**
  (nonzero, points at staged code — the park is inside the callout, go deeper via
  Q-F4/Q-F5); **(V2) unseeded** (zero/garbage because the filling init never runs
  on our boot path — the fix is a rung-2-style seed; deliverable = the EXACT value
  the real writer would store, transcribed, + the seed site verdict
  (glue ECB pre-population vs PatchROM trampoline, recommend glue per the ECB
  precedent; justify if not)); **(V3) points-at-unstaged-code** (the native body
  only exists when the real Trampoline/parcel chain builds it — STOP-RULE).
- [ ] **(Q-F3) Selector 0x31 semantics + the conformance vector**: (a) [PROBE-F3]
  in-trap register file at the callout — probe the body 0x5046db44 (or the
  continuation entry if V1) with full register dump: confirm the marshaling map
  (r0=0x31? r3=A0=the &slot pointer into the ExpandMem array?) — the register-map
  caveat above is a falsification target; (b) static RE of the selector-0x31
  service (≤2 call levels / ≤12 functions from the continuation entry): name the
  service (the e3e0 caller context says CFM-accelerator/native-glue pointer fill;
  the 5-entry id table at 0x5000e3a0 bounds the selector family) and pin the
  return protocol. **Deliverable: a register → expected-value/class table at the
  callout entry AND the return predicate in register→value form — expected:
  r3 (→D0) ≠ 0, a pointer-class value the 68k stores through the slot pointer
  (`movea.l d0,a1; move.l a0,(a1)` tail — note: pin WHICH of d0/a0 lands in the
  slot from the 68k tail's exact operand reading); the d0==0 arm = the failure/
  not-ready negative space.** Task A's probe gate is exact conformance to this
  table.
- [ ] **(Q-F4) The park mechanism, named**: discriminate H1 (one-shot-never-returns)
  vs H2 (poll-returns-unsatisfied): re-read the full SS_DR_R24_RING (wrap/capacity
  arithmetic re-verified in writing) — do the f24x PCs recur or appear once?; pair
  with PROBE-F1's body visit COUNT (1 visit = H1-shaped; many = H2) and one probe
  at the post-return body PC 0x5046db6c (the unmarshal first instruction — visits
  here = the callout RETURNS). Name where execution sits during the silence
  (blrl-to-0? a spin inside the continuation? — one bounded look). Record as
  residues unless load-bearing: [KDP+0x65c]/[0x2810]/XLM_IRQ_NEST during the
  callout, and any MSR/world-flip the continuation performs. Deliverable: the
  park named + the exit-path confirmation (the body's unmarshal+dispatch-resume
  is the only return route).
- [ ] **(Q-F5) Staged-surface audit of the selector-0x31 service** (the stop-rule
  tripwire): from the (possibly newly-seeded-on-paper) continuation entry,
  enumerate every structure the service reads/writes (KDP fields, ECB fields,
  ExpandMem, kernel pool, parcel data) — bounded ≤2 call levels / ≤12 functions;
  structures not classifiable within the bound → the verdict is a residue →
  trigger 3. **Verdict required in writing: the service runs on staged state
  (+ at most seed-class fixes, LISTED — Task A consumes the list) — or it requires
  unstaged Trampoline/parcel surfaces, in which case the STOP-RULE fires here**,
  after the addendum. Also record (bounded, diagnostic): what the OTHER 4 id-table
  entries / nearby selectors imply for the next milestone, and whether anything
  EE/tick-shaped appears (→ EE-CHAIN-RECON cross-ref, contingency only).
- [ ] **Paravirtual donor datum (1 boot max, OUTSIDE the 8-boot newworld budget per
  the donor-study precedent, or static-only if the boot budget is contended)**:
  what does paravirtual hold in `[ECB+0x9dc]` / does its boot path ever issue
  FE1F? A filled paravirtual slot is a transcription donor for V2; a never-reached
  FE1F on paravirtual is also signal (the init that fills it may be OldWorld-only).
  Residue-class: skippable if Q-F2(b) pins the writer statically.
- [ ] **Probe pack mapping** (within the budget): PROBE-F1→Q-F1/Q-F4 (shared boot),
  PROBE-F2→Q-F2 (rides the same boot), PROBE-F3→Q-F3; a question with no boots
  left gets its residue status from static evidence only.
- [ ] **Gate (the blocking-answer table): ALL blocking answers pinned before ANY
  implementation task starts.** Task A blocks on **Q-F2 (value+writer+seed-site
  verdict) + Q-F3 (callout-entry register table) + Q-F5's verdict (go/no-go + the
  seed-class fix list)**; Task B blocks on **Q-F3 (return predicate) + Q-F4 (park
  mechanism + exit path)**; Task C blocks on **Q-F5's full enumeration**. A residue
  on a blocking answer invokes the stop-rule — no improvisation. Commit the
  addendum.

### Task A: provision the continuation + the seed-class fixes (env-gated `SS_NW_FE1F_SURFACE`, default OFF)

- [x] Land the `[ECB+0x9dc]` provisioning per Q-F2's verdict — **active only under
  `SS_NW_FE1F_SURFACE=1`** during bring-up (gated off ⇒ the park baseline
  byte-identical): host-side at glue's ECB pre-population site (next to the
  ECB+0xfc/+0x7fc siblings, with the transcription discipline: exact value the real
  writer stores, source cited) — or PatchROM-time guest-side ONLY if Q-F2 demands a
  post-NK-init assert (then: occupancy map first, verified-zero region at/above
  0x429d9c). If Q-F2's verdict is V1 (already correct — the fix is elsewhere per
  Q-F5's list), record that explicitly and land only the Q-F5 seed-class fixes.
- [x] Land the Q-F5 seed-class fix list (same gate, same discipline; every seed site
  logs one loud `[NW-FE1F]` line under the gate). Misuse hardening: gated-off path
  untouched; no new abort classes (the park IS the captured baseline).
- [x] Env-flag matrix as BEHAVIOR (P-M1 carried): pin the gate's interaction with
  SS_NW_MM_SWITCH/SS_NW_SC_SURFACE (this surface is meaningless with either OFF —
  document the dependency, decide+comment the combined-opt-out behavior), polarity
  mirroring SS_NW_SC_SURFACE (explicit-"0"-only opt-out at flip time).
- [x] **Probe sub-contract (PASS/FAIL):** with `SS_NW_FE1F_SURFACE=1`, the
  callout-entry probe shows ≥1 visit with the register dump conforming to **Q-F3's
  expected-register table** (exact/class per row — no "sane registers"; r0=0x31 and
  the &slot-pointer row mandatory). What the callout does AFTERWARD is diagnostic,
  recorded.
- [x] **Gated-off A/B:** one boot without the env var reproduces the 0x5000f248 park
  baseline byte-identically (ring tail exact, term-dump class exact, selector list
  exact).
- [x] Gates: full gates + both sub-contracts. Commit.

### Task B: the selector-0x31 round trip (conformance per Q-F3/Q-F4)

> **DONE 2026-06-11 — all sub-contracts PASS; the return predicate REFINED
> (judged refinement, not falsification).** Results:
> `M6A-ONGOING-ENTRY-DESIGN.md` "Task B results". Two Q-F3 mis-pins corrected
> from evidence: (1) the slot recipe is **base + 4·index** (the e3e0 lea carries
> a +4 displacement) → the live id=2/index-7 slot is **0x100037dc**, not
> 0x100037d8 (rev-3 item 4's watch address is superseded); (2) r4(→A0) is the
> **NK kernel-object ID handle** ((dir-index<<16)|generation; live 0x00120001),
> NOT the EVNT pointer (which stays kernel-internal) — the sc ID-directory
> precedent. 3 slot-protocol boots used. New frontier captured + named: the
> DSAT stack-underflow wall (SysError ID 10 post-$36-callout, saved PC
> 0x5000e448; alert machinery underflows RAMBase from a 0x100000a0 stack).

- [x] **Round-trip sub-contract (PASS/FAIL), env-on:** (a) the callout RETURNS —
  post-return body PC 0x5046db6c (unmarshal) ≥1 visit; (b) callout-entry probe
  conforms (Task A's gate re-asserted); (c) **result conformance**: the Q-F3-pinned
  return rows hold at the return (r3→D0 of the pinned pointer-class; the d0==0
  skip-arm NOT taken — observable: the 68k tail's store-through executes)
  *(executed against the CORRECTED predicate above; the skip-arm row was already
  deleted by Rev-2 T-C2)*; (d)
  **the 68k advances**: SS_DR_R24_RING shows 68k PCs BEYOND the f24x family after
  the trap (the e3e0 routine's slot-filled arm; ring tail ≠ the captured baseline
  tail) *(r24-ring atexit doesn't fire on the crash path — the trace ring's r24
  column substituted, recorded)*; (e) **the ExpandMem slot fills**: one guest-memory read of the
  `([$2b6],$310)` array slot (index per the id-table entry) shows the stored value
  of the pinned class *(WATCH on 0x100037dc: := 0x00120001 by the f240 tail,
  stable until the crash; Task A's churn reading retired — wrong word)*.
- [x] **Invariant carry-over (PASS/FAIL):** rung-2 + syscall-surface invariants hold —
  cold-once (WATCH pair), guest[0]/[4] stable, slot-15 unvisited, the 5 sc
  deliveries with the same selector list and resume r3=0, delivered-DEC=0 (exc=
  first field; a DEC delivered inside the callout while EE=0 + fence closed is a
  falsification — conditioned per the EE-legality rule: only while the callout
  keeps EE=0). *(delivered_sc now 13: #1-5 unchanged, #10=0x50 probe-sampled;
  #6-9/#11-13 unenumerated — SC print caps at 5, counter bump deferred.)*
- [x] **Diagnostic (recorded, not gates; ≤2 mapping boots):** how many FE1F callouts
  fire and with which selectors (bounded callout-entry probe with r0 dumps); which
  other FE opcodes appear; where the boot stands afterward — the honest map of
  "more selectors or the next surface's walls". *(2 callouts: $31 then $36, both
  slot 8; $36 service staged at 0x5031d6b4 [STATIC]; $34 sibling stub at 0xf280;
  the wall is the DSAT stack-underflow — addendum.)*
- [x] If a pinned contract is falsified live: the one-iteration mechanics (stop-rule
  section) — dated addendum falsification entry, ONE bounded re-pin boot, resume;
  second falsification of the same contract escalates. *(Dated refinement entry
  recorded; corrections pinned statically + confirmed in the same evidence boot —
  no re-pin boot consumed, no escalation.)*
- [x] Gates: full gates + the sub-contracts (evidence-only steps may name the smoke
  set with stated reason). Commit. *(Doc-only commit — no source edits this task;
  stated reason: parallel Task-A review on the same tree.)*

### Task C: acceptance + default flip (flip LAST, revert-on-red)

> **DONE 2026-06-11 — FLIP LANDED, battery green pre/post-flip, revert-on-red
> not invoked.** Results: `M6A-ONGOING-ENTRY-DESIGN.md` "Task C results";
> frontier: M6A-WAVE2-SHIM-RECON.md "FE1F-service-surface Task C closeout".
> Commits: 2949ec32 (per-selector sc counter), be0e02cb (the flip).

- [x] **PASS/FAIL gates first, env-on (`SS_NW_FE1F_SURFACE=1`):** (a) full gates;
  (b) Task A callout-entry conformance; (c) Task B round-trip + result + advance
  gates; (d) rung-2 + syscall-surface invariant carry-over (incl. the MixedMode
  round trip itself: TVector visit + completion resume per the rung-2 Task Y
  recipe — see M6A-ONGOING-ENTRY-DESIGN.md "Task W2 results" + "Task Y results");
  (e) gated-off boot reproduces the 0x5000f248 park baseline byte-identically.
  *(ALL PASS; paravirtual e2e substituted per the risk-based rule — structural
  inertness + gated-off A/B, stated in the flip commit.)*
- [x] **Fix budget:** telemetry/capture commits freely; at most ONE small in-scope
  fix iteration per falsified contract, full gates re-run after any fix.
  *(Used for the Task-B-recorded counter gap: per-selector sc counter landed
  (2949ec32, inner gates). Finding: the baseline "5 sc deliveries" was the
  cap-5 PRINT artifact — true parked total is 8/7-distinct; FE1F adds 5 → 13/9:
  0x3f,0x19×2,0x14,0x0f×3,0x27,0x40,0x42×2,0x50,0x4d.)*
- [x] **THEN flip** `SS_NW_FE1F_SURFACE` to the newworld profile default (opt-out
  `=0` kept, explicit-"0"-only, polarity mirroring SS_NW_SC_SURFACE) and re-run
  (a)–(e) with NO env vars. **Any gate failure after the flip ⇒ the flip is
  REVERTED in the same task (machinery stays env-gated), the failure recorded —
  the milestone does not ship default-on with red gates.**
  *(Flipped (be0e02cb); (a)-(e) re-run with no env vars, ALL PASS — slot fill
  WATCH at record #3391079 and ring total 4,480,459 Task-B-identical.)*
- [x] **DIAGNOSTIC OUTCOMES (recorded, NOT gates) — the P-M4 frontier artifact:**
  does the e3e0 routine COMPLETE (all needed id-table slots filled) and the A-line
  dispatcher chain proceed? Where does the boot park/wall next? Capture with the
  full named artifact: callout-entry probe with r0 dump + SS_DR_R24_RING tail +
  the term-dump HB/MMIO/CUDA/VCLK baseline (SIGTERM + SS_TERM_DUMP) — named
  honestly as the NEXT milestone's opening evidence (likely: more FE1F selectors,
  another FE opcode, the CFM chain's next surface, or the EE/tick chain). Stop-rule
  trigger 2 applies: capture and stop; no staging beyond capture for new classes.
  *(The DSAT stack-underflow wall re-confirmed record-for-record on the default
  config ([$C70]:=0x5000e448 #3391708 identical; SIGSEGV ea=0x0fffff42 class);
  NEW: [$AF0] pre-raise content pinned 0xffff — Task B's "d6=0x40 from $AF0"
  corrected. r24-ring atexit doesn't fire on SIGSEGV; trace-ring substitute
  carried.)*
- [x] Record results: M6A-ONGOING-ENTRY-DESIGN.md (the Task-0 addendum's results
  sections), M6A-WAVE2-SHIM-RECON.md (frontier update). Commit.

### Task Z: docs

- [ ] DIAGNOSTICS.md: `SS_NW_FE1F_SURFACE` (default + opt-out + the
  SS_NW_MM_SWITCH/SS_NW_SC_SURFACE dependency matrix); CHANGELOG (the first
  serviced DR native callout — acceptance numbers); MACHINE-LAYER-PLAN header +
  M6 row; ROADMAP cross-check; LEARNINGS (at minimum: the raw-ROM-provenance
  method as the first recon move, the [ECB+0x9dc] writer finding, the park-shape
  verdict); cross-tracker grep for stale "FE1F frontier"/"park at 0x5000f248"
  claims (historical sections stay per the rule; mind concurrent tracker edits —
  re-grep at execution time). Commit.

## Stop-rule (triggers per MACHINE-LAYER-PLAN §9)

1. If Task 0 shows the FE1F path requires **any surface beyond continuation
   provisioning + the pinned seed-class fixes** (Q-F2 verdict V3: the selector-0x31
   native body is unstaged Trampoline/parcel product; or Q-F5 enumerates structures
   only the real init chain builds; or the service blocks on the EE/tick chain),
   STOP after the addendum and re-scope — the recon itself is the tripwire.
   **A host-side HLE NativeOp implementation of selector 0x31 is named in advance
   as the WRONG fix** (paravirtual idiom; violates §2d) — it may be proposed only
   as an explicit re-scope decision, never improvised inside a task.
2. If Task C's diagnostic shows the boot advances past the e3e0 routine but dies on
   the NEXT surface (another selector, another FE opcode, the CFM chain, the EE
   chain), that is the NEXT milestone's named frontier — capture (the P-M4 artifact)
   and stop; no staging beyond capture for new classes.
3. A residue on a BLOCKING Task-0 answer (the blocking-answer table) invokes
   trigger 1's re-scope, not improvisation. This includes the Q-F1 route
   falsification (0x2C-stub catch) and the provenance falsification (branch (i)
   revival).

Within tasks — the one-iteration rule, operationalized: if a pinned contract is
falsified live, (a) reopen the addendum with a dated falsification entry; (b) ONE
additional bounded probe boot to re-pin; (c) resume the falsified task with the
corrected contract. A SECOND falsification of the same contract escalates to the
stop-rule (re-plan, not patch-on-patch).

## Self-review record

Spec coverage: all coordinator inputs consumed (the Task-C frontier capture +
M3A/WAVE2 sections by name, the FE-opcode family geometry from the DR-handoff +
ongoing-entry docs, the patch_68k inventory negative check, the EE-chain contingency,
the binding process template incl. its rev-2 discipline). The shape question the
coordinator front-loaded (patch-introduced EMUL_OP vs raw-ROM DR service) was settled
by direct byte-compare during planning: raw-ROM, branch (ii) — recorded as a free
static win with mandatory Task-0 re-verification and an explicit branch-(i) revival
clause if provenance falsifies. The architectural shape follows the machine-layer
philosophy: the DR's own FE1F body and the real native service run; our obligation is
the continuation pointer + seed-class state (the rung-2 idiom one opcode over), with
the HLE temptation named in advance as a stop-rule re-scope, never an in-task move.
Every open question is a Task-0 item with a blocking-answer table and
all-pinned-before-implementation; gates are falsifiable in advance (the callout-entry
register table, the return predicate with its d0==0 negative space, the
68k-advance/slot-fill observables, byte-identical off-boots) and separated from
boot-advancement diagnostics. Env-gated bring-up with flip-last + revert-on-red
mirrors the syscall milestone. Known tensions flagged for the red team: (1) the
park-shape ambiguity — the coordinator's FE07-poll reading vs the ring-tail/body
evidence for one-shot-never-returns (Q-F4 discriminates; the ring's wrap arithmetic
must be re-verified before either is trusted); (2) the marshaling register map is
convention-inferred, not probe-verified; (3) the [ECB+0x9e0]-filled/[0x9dc]-? writer
asymmetry is unexplained — rung 2 never identified the writer; (4) mirror-vs-static
copy discipline (all body/slot facts are primary-dump; live facts need per-target
[PROBE✓]); (5) the 68k tail's store-through operand reading (which of d0/a0 lands in
the slot) needs the exact pin; (6) the paravirtual donor boot sits outside the
newworld boot budget by precedent — challengeable; (7) Task Z tracker concurrency.
Sequencing: strictly one task at a time (shared glue/rom_patches sites). Wave-2
backlog stays parallel.

## Red-team record

*(empty — a red-team round follows this draft; findings to be folded as rev 2 markers)*

## Rev 2 (2026-06-11) — both red-team rounds folded (BINDING; overrides the body where in conflict)

**The contracts review settled the milestone's two load-bearing contracts STATICALLY —
the body's Q-F2/Q-F3 theories are corrected as follows:**

- **(T-C1) THE SEED THEORY IS WRONG — `[ECB+0x9dc]` is already filled; the park is an
  UNPOPULATED ENTRY-VECTOR SLOT.** Glue's 0x97-entry loop (`sheepshaver_glue.cpp:2163-2166`)
  writes ECB+0x7fc..0xa54 — covering the whole 0x9a0..0x9e0 continuation block — from
  the ROM halfword table at file **0x36dc42** (raw==patched). Index 0x79 (→0x9e0) =
  0xde1c → mirror-rebased **0x5046de1c = the rung-2 FE01 [PROBE✓] value** (the
  "writer never identified" mystery closed). Index 0x78 (→0x9dc, FE1F) = 0xe8e0 →
  **0x5046e8e0 = entry-vector table slot 8** — a raw `0fff0008` placeholder, ZERO in
  the patched primary. The park = `blrl` into an unpopulated vector slot. **Task A
  re-aims: populate mirror vector slot 8 (the table-redirect class move — the Task-T/U
  idiom: verify-EXPECTED/zero, PatchROM-time) with whatever the real boot installs
  there — Q-F2 pivots to "what does the real init install in slot 8" (THE stop-rule
  question: if the slot-8 service body is unstaged, trigger 1).** PROBE-F2 becomes a
  CONFIRMATION: `[ECB+0x9dc]==0x5046e8e0` + the live mirror bytes at 0x5046e8e0
  ([PROBE✓] required — mirror is in no dump).
- **(T-C2) THE SUCCESS PREDICATE WAS INVERTED + the tail misread.** The 0xf240 stub is
  one Pascal-convention wrapper: arg at 8(a6), result space at 0xc(a6). `202e 0008` =
  `move.l $8(a6),d0` (the caller's POINTER ARG reload, never zero at the e3e0 site) —
  the beq.s is a null-arg guard, NOT a result test; the store-through always executes.
  **A0 (← r4) lands in the ExpandMem slot; D0 (← r3) is the status at 0xc(a6) — and
  0 = noErr (the error arms store nonzero: 0xffff8d8e, paramErr −50).** Q-F3's
  conformance table: success = r4(→A0) pointer-class slot value AND r3(→D0) = 0-class
  status. Task B gate (c)'s "d0==0 skip-arm not taken" is DELETED (vacuous); the
  conformance observable = the slot-fill (gate (e)) + the e3e0 re-entry arm. The same
  misreading exists in M3A-ENTRY-TABLE.md "Task C results" — Task Z corrects it.
- **(T-M1)** "No entry-vector-slot involvement" is FALSE (slot 8 IS the continuation);
  the no-world-flip conclusion survives (slot 8 = mirror emulator-world code).
- **(T-M2) The glue table loop is dead-on-arrival AND latently buggy** (`hw | page_base`
  OR-corrupts page bits; primary-world base) — a real builder (DR cold start, the
  0x36e964 cluster) overwrites with mirror-base values. Glue-time ECB seeds in this
  block are the dead-seed class; PatchROM-time mirror vector-slot patching sidesteps
  the ECB entirely. The latent OR-vs-ADD/wrong-world glue bug gets its own ticket
  (Task Z registers it; fixing it is OUT of this milestone).
- **(T-M3) Family shape corrected:** FE10..FE1E (except FE11) share a
  raise-68k-exception body (0x36d760, vectors 0x2c/0x28); FE11 is `b 0x36da1c`
  (table word 0xe8dc = vector slot 7); **FE1F is the ONLY marshaled-callout slot, and
  it has NO null check before `blrl`** — H1 (one-shot-never-returns) structurally
  corroborated with the park target PREDICTED: blrl → 0x5046e8e0 (zero/placeholder).
  Q-F4 tests that exact prediction.
- **(T-minors):** sibling FE1F stub at file 0xf260, selector **$36** (arg in/out via
  $c/$10(a6)) — the diagnostic map's concrete second selector; the seed-precedent
  range corrected (glue writes ctx+0xfc/+0x1c4/+0x1ec where ctx=ECB+0x100, + the
  table at +0x7fc..0xa54); M3A "mismatch⇒error" polarity nit (error fires on EQUALS
  the $9F unimplemented address); raw slot-id quirk (slot 14 duplicates 0x0d);
  ctx-save clobber risk on 0x9dc is LOW (the builder, not ctx save, is the writer
  to respect); capstone-M68K eats `fe1f` as `fsmove` — hand-split required.

**Process findings folded:**

- **(P-C1) Q-F1 joins the blocking set** (Task A blocks on Q-F1+Q-F2+Q-F3+Q-F5-verdict;
  transitively B/C) and is **probe-mandatory** — no static residue path exists (mirror
  facts are [PROBE✓]-only); losing its boots IS a trigger-3 residue. The per-task table
  is a residue-disposition map, not a start-order license (all blocking answers pinned
  before ANY implementation task).
- **(P-C2) Task A's gate gains a seed-observing row**: probe memory field at absolute
  `[0x68fff9dc]` (ECB=0x68fff000, MMCB=0x68fff400 — constants now stated) confirming
  the continuation, PLUS the live mirror slot-8 words showing the LANDED population
  (the change itself, not the baseline-true entry conformance — which is annotated
  baseline-true, validating Q-F3's table only). **The V1-with-empty-fix-list middle
  case routes to trigger 1** ("recon found no provisioning obligation explaining the
  park ⇒ re-scope, Task A does not start") — though T-C1 has likely mooted it (the
  obligation is slot 8).
- **(P-M1) Task B gate (c) replaced** per T-C2 (the store-through observable is
  derived from gate (e), not independently instrumented).
- **(P-M2) Gate (e) gets its tool + address recipe**: Q-F3(b) delivers the id→index
  map + THE index for the f240 site + the slot-address recipe (probe field `[0x2b6]`
  → ExpandMem+$310+4·index); instrument = `SS_JIT_WATCH_ADDR` on the resolved slot
  address (hex, SS_JIT_TRACE_RING=1; the WATCH line beats a point read) with a
  probe-field fallback.
- **(P-M3) Q-F4's PRIMARY discriminator = probe visit counts** at 0x5046db44/0x5046db6c;
  the ring reading is corroboration only, with dedup semantics + capacity re-verified
  in writing (cite DIAGNOSTICS.md "Machine Layer M6a rung 2" ring section).
- **(P-M4) World-flip/MSR routing added to Q-F4**: any continuation world-flip/MSR
  write is BLOCKING for Task B (pin the DEC-fence/EE-conditioning interaction);
  EE-required-for-progress ⇒ trigger 1.
- **(P-M5) Task B gate (a) annotated**: meaningful under H1 only; under H2 it is
  baseline-true → recorded N/A, evidence rests on (d)/(e). (Post-T-M3, H1 is the
  strong expectation.)
- **(P-M6) Provenance anchors stated**: raw md5 `7b1378be…` (+ the 16-placeholder
  fingerprint at 0x36e8c0), patched md5 `e432df64…` (recorded in
  M6A-ONGOING-ENTRY-DESIGN.md); re-dump re-baselines the PATCHED md5 only — the raw
  image re-establishes only from the original .rom.
- **(P-M7) Budget restated honestly**: total recon boots ≤9 = ≤8 newworld + ≤1
  paravirtual donor (the donor boot is recon, charged against the stated cap; the
  precedent cite dropped); **(P-m1)** the SS_DUMP_ROM re-dump and the baseline
  re-confirmation each charge the newworld budget; **(P-m2)** Q-F2(b)'s bound =
  ≤12 functions TOTAL across all roots (largely mooted by T-C1's static answer);
  **(P-m3)** absolute-address recipes stated (probe 0x5046db44 with field
  `[0x504F70F8:0x8]` for the slot words); **(P-m5)** env-matrix shape pre-stated
  (FE1F surface with MM_SWITCH=0 or SC_SURFACE=0 ⇒ inert + one loud `[NW-FE1F]`
  misconfig line; Task A confirms, not invents); **(P-m6)** an FE1F-callout counter
  (the exc=/HB idiom) is the COUNT instrument; probes are the ABI instrument;
  **(P-m7)** the transition COUNT is jitter-class (excluded from byte-identical),
  ring-tail PCs are gated.

**Net rev-2 shape change:** Task A = populate mirror entry-vector slot 8 per Q-F2's
pivoted recon (what the real init installs), PatchROM-time, env-gated; Task 0's probe
pack confirms the static chain ([ECB+0x9dc]=0x5046e8e0, slot 8 zero, body route) and
pins the slot-8 service contract. Everything else (gates, flip-last, stop-rule)
carries with the corrections above.

## Rev 3 (2026-06-11) — Task-0 re-scope RATIFIED: the trap-placeholder mechanism (BINDING; supersedes the rev-2 population framing)

Task 0 (addendum `74fdc067`) falsified the population framing entirely and pinned the
real mechanism: **nobody writes slot 8 — the raw ROM's `twi 31,r31,8` placeholder IS
the design.** On real HW the slot traps vector 0x700 (program) → NK handler
`[KDP+0x37c]`=0x50314700 [PROBE✓] → exit-pointer dispatch `[KDP+0x610]`=0x5031aca0
[PROBE✓, NK-published, already runs on our boot] = the same NK selector-service
gateway the sc surface traverses 5×/boot. The 16 placeholder slots are
trap-to-NK-dispatch trampolines (each `twi` encodes its slot id); this also closes
rung 2's `[ECB+0x9e0]`-writer mystery (the exit-pointer publication cluster at file
0x3117xx is the real builder). Q-F5: the selector-0x31 service (0x5031d204, 'EVNT'
kernel-pool allocator) is fully staged; seed-class fix list EMPTY — **the fix is
route-class.**

**Ratified Task A re-shape (the coordinator/plan-owner decision, trigger-1
discipline observed):**
1. **Restore the raw `twi` words** in the mirror entry-vector slots that rung-2
   Task U overwrote with parked stops (slots 4, 6–14; slot 15 keeps Task T's
   allocator-exhaustion stop only if Q-F2's evidence says slot 15 is NOT a
   trap-placeholder on real HW — re-verify against the raw image; if it is one,
   restore it too and the exhaustion diagnostics move to telemetry). The
   verify-EXPECTED discipline applies (current bytes == the Task-U stop branches).
   Rung-2's "dead slots get loud stops" policy is RETIRED with a dated note in the
   design doc — the slots were never dead; the raw placeholders are load-bearing.
2. **The 0x700 delivery surface** (the sc-surface idiom one vector over):
   `EXC_PROGRAM` in exc_core (entry table gains `program_entry`, appended last;
   masks per PEM — same LAW discipline), `NW_PROGRAM_ENTRY_DEFAULT = 0x50314700`
   ([KDP+0x37c]'s published value, primary copy per the publication precedent), and
   the powerpc_cpu seam: the `twi`/trap-instruction execute path gains a
   profile-gated newworld arm (the `execute_syscall` precedent EXACTLY — §2d
   honesty: a slow-path seam, no JIT changes; verify where twi currently lands —
   likely the illegal/program default — and route trap-taken to
   `ExcEnter(EXC_PROGRAM)`). SRR1 trap-bit semantics per PEM (program-exception
   SRR1 flags: trap bit 0x00020000-class — pin from the PEM/oracle during
   implementation, test-pinned in exc_core).
   Env-gated `SS_NW_FE1F_SURFACE=1` covers the pair (restore + delivery).
3. Task A's probe gate (rev-2 P-C2 carried, re-aimed): the callout-entry conformance
   (baseline-true, annotated) PLUS the CHANGE observables — the slot-8 word ==
   0x0fff0008 (probe field), and ≥1 `[EXC] PROGRAM delivered` with
   SRR0=the twi address, entry=0x50314700.
4. Task B unchanged in spirit (the round trip through the REAL gateway; the return
   predicate r4=EVNT-pointer/r3=0; the ExpandMem slot 0x100037d8 fill — the pinned
   watch address), plus the H1-park-gone observable (db6c unmarshal visits ≥1).
5. The body's "no powerpc_cpu changes" line is SUPERSEDED by this rev (the seam is
   the precedented sc-class change); the HLE prohibition stands unchanged.

Mirror-slot typo corrected: the FE1F dispatch slot's live mirror address is
**0x504FF0F8** (0x50480000 | 0xFE1F<<3); rev-2's 0x504F70F8 was a transcription slip
(the static file offset 0x3ff0f8 and body 0x36db44 were always correct).
