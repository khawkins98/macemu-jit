# M6a rung 2 — Mixed Mode switch completion: FE01 68k→PPC context switch, FE02 switch-back, NK entry-vector slots, real ongoing entry at table[0]

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. **Dispatch ONE task at a time — Tasks T/U/V/W/X edit the same trampoline/glue functions; NOTHING in this plan parallelizes (rev 2 P12).**

**Goal:** Complete the DR Emulator's Mixed Mode switch on the newworld profile so MPLibrary's
PPC TVector (`0x500cef8c`) actually EXECUTES: the $AAFE RoutineDescriptor → FE01 service
allocates a 0x220-byte save record from a **provisioned** pool (Task T promotes
`SS_NW_MM_POOL` to the profile default, RELOCATED off the Hnfo scratch — rev 2 C1), the
FE01 continuation proceeds through the NK surface it expects — **(rev 2 C2: the exact
mechanism is a HYPOTHESIS pending Q-B/Q-C; under pool-on there are already zero reset
transitions, so the dead pre-pool zero-slot fall-through is NOT the live spin mechanism)** —
the 68k state is saved per the 0x220-record contract and PPC entered at the TVector under
the MixedMode register/MSR convention (Tasks U+V), the PPC code returns via the **FE02
switch-back** (Task W), and table[0] gets a **real ongoing entry** (Task X, decided on a
POST-Task-W re-census — rev 2 P1). Milestone acceptance DIAGNOSTIC (not a gate): the
FE01-retry-spin (68k stub `0x10008fb0` ↔ FE01, ~55M blocks/s) disappears and the 'pwpc'
parcel-init chain proceeds (the CodeFragmentMgr lookup — the parcel-by-name caller region
ROM file offset `0xf46e`, observed via the SS_DR_R24_RING 68k-PC ring, NOT SS_PROBE_PC:
68k PCs are PPC-probe-blind — rev 2 P12). PASS/FAIL gates are the regression invariants
plus the verifiable sub-contracts in Task Y. Paravirtual byte-identical throughout.

**Architecture:** All switch machinery is **guest-side code in the mirror zero run**
(ROM+0x429xxx, the established [NW-TRAMP] idiom) plus glue-side seeds — no new host-side
delivery mechanism and no powerpc_cpu changes. The DR's own FE01 service (allocator at
staged `0x5046e304`, glue blocks `0x5046e1a0..e2ec`) already does the heavy lifting; our
obligation is the surface the NK provides on real hardware: the ECB pool words
(`[ECB+0xE0..0xEC]`), the NK entry-vector slots the FE01 path actually consumes (Q-B), and
a table[0] that distinguishes cold start from ongoing entry (design doc R2/R3/R4, with the
`[KDP+0x5f0/0x5f4]` mirror retarget if the stub route is chosen). **Task 0 is a BINDING
pre-implementation recon gating Tasks T..X (rev 2 P2)**: it pins the open contracts in
writing before implementation freezes. New machinery is env-gated during bring-up
(`SS_NW_MM_SWITCH=1`, default OFF) and flipped to the profile default only as Task Y's
LAST step with an explicit rollback rule (rev 2 P6). `SS_NW_MM_SWITCH` implies the pool
(rev 2 C11 — switch-without-pool is not a supported config; the code asserts or implies it).

---

## Authoritative inputs

| Doc | What it fixes |
|---|---|
| `docs/planning/machine/M6A-ONGOING-ENTRY-DESIGN.md` | THE ongoing-entry design: table[0]/slot semantics (§1.2–1.3), NK save/restore protocol (§2), R1–R6 requirement list (§4), rung ladder (§5), PROBE-O1/O2/O4; the R3 ongoing-arm decision and the `[KDP+0x5f0/4]` mirror wrinkle. **Task 0's addendum lands here.** |
| `docs/planning/machine/M6A-WAVE2-SHIM-RECON.md` — "MPLibrary bail NAMED" section (commit 36f048a0) + the main-thread-recon section before it (b3a947e4) | THE FE01 facts: allocator `0x5046e304`, ECB+0xE0..0xEC pool contract, the entry-vector table, the FE01→retry-spin frontier under SS_NW_MM_POOL=1, the 68k retry stub `0x10008fb0` (`dc.w $FE07; bne.b +2; rte`/`jmp 0x500049c4`), the TVector `0x500cef8c`, `[r3+0xd8]` current-record link at `[ECB+0x710]+0xd8`, the SS_M6A_USER_MSR zero-page-slide note, the r24-ring/probe methodology + its corrections (SS_JIT_WATCH_ADDR parses hex, needs SS_JIT_TRACE_RING=1) |
| `docs/planning/machine/M6A-DR-HANDOFF-ANALYSIS.md` | DR dispatch geometry + register map (r24/r29/r30/r31/r1/r25/r27, cr2), UserModeMSR `0x0000D032` history, scheduler/restore anatomy (§2.4), rung-1 results |
| `docs/planning/machine/M3A-ENTRY-TABLE.md` | KDP-shim entry contract, `[KDP+0x65c]/[0x660]` live values, EE deferral semantics, carry-forwards (syscall_entry unresolved — descope active) |
| `docs/planning/HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` §2.8 | Background obstacle map only — NOT a spec for this plan |
| `SheepShaver/src/rom_patches.cpp` (`PatchROM_NW_trampoline` :714–889, SS_NW_MM_POOL block :794–826, SS_M6A_USER_MSR block :829–858, vector stop stubs :862–875) + `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` (`[NW-TRAMP]` ~:1746–2090, KDP/ECB seeds :1973–2069, Hnfo seeding :1882–1898, `deliver_pending_dec_exception` :768ff) | The live scaffolding every task edits; the guest-side-survives-ECB-rebuild idiom |

## Codebase facts (carried; implementers re-verify sites before editing)

- **The reboot loop is dead under the pool; the frontier is the retry spin.** With
  `SS_NW_MM_POOL=1`: allocator success path executes (`0x5046e324`), `[0xEC]` cycles free,
  **zero reset transitions** — so the FE01 success path already returns to the 68k stub
  WITHOUT falling through zero slots into table[0] (rev 2 C2: whether ANY zero slot is
  consumed in the live spin is Q-B/Q-C's question, not a premise). FE01 → NK roundtrip →
  record allocated → return to `0x10008fb0` → FE01 again (~55M blocks/s, comp frozen 3573,
  exc=0/52/0 DEC deferrals). The TVector `0x500cef8c` never executes (probe: 0 visits).
  `SS_M6A_USER_MSR=1` on top reproduces the zero-page-slide crash — EE alone is not the
  unblock.
- **(rev 2 C1 — CONFIRMED COLLISION, Task T must fix):** the staged pool base
  `0x68ff5000` IS the Hnfo scratch (`sheepshaver_glue.cpp:1896–1898` writes
  `[hnfo_rec+0x08] = irp_base+0x1000 = 0x68ff5000`), and the ROM machine-detect copy-out
  (ROM+0xAC20 family) writes scratch bytes `+0x10..+0x17` — inside MM save record 0.
  The "free gap, no other users" comment (rom_patches :805–807) is falsified by the
  repo's own Hnfo seed. Latent today (machine detect precedes FE01 each cold cycle);
  corrupting once records hold live contexts. Task T relocates the pool (e.g.
  `0x68ff5800`) and delivers a **sub-KDP occupancy map** (NKSystemInfo `0x68ff4000+0x120`,
  IRP banks `+0xDF0..0xEBC`, Hnfo `0x68ff4f00`, Hnfo scratch `0x68ff5000+`, pool, the
  Task-X scratch word) as a tracked doc table.
- **(rev 2 C8 — contradiction 1 RESOLVED statically):** the entry-vector table is
  **16 slots**; `patch_68k_emul` (rom_patches :1524–1539) writes branches at slots
  0–3 and 5, `POWERPC_ILLEGAL` at 4 and 6–15, and **`POWERPC_ILLEGAL == 0x00000000`**
  (`include/emul_op.h:26`) — the observed zeros ARE the placeholder. Dump-confirmed at
  table base (file 0x36e8c0): `48001040 4800113c 48001238 48001334 00000000 4800142c` +
  ten zero words. `+0x3c` = slot 15 = the allocator's no-free-record target; slot 4
  (+0x10) is the design doc's "deliberately dead interrupt vector". No collision between
  those two stories. The design doc's "8 slots" was a truncation — Task 0 Q-B records
  this resolution in the addendum and focuses on WHICH slots the FE01 success path
  consumes.
- **Pool seeding is guest-side per-entry by necessity** (NK cold-init rebuilds the ECB
  every cycle; the Hnfo re-assert precedent). **(rev 2 C3: the cold-arm-only restriction
  on zeroing `[ECB+0xEC]` lands IN TASK T**, not X — until X exists, cold-only == every
  entry, so it's free now and removes the V/W warm-re-entry corruption window.)
- **The slot-0 stub exits via `[KDP+0x5f0]` = primary-world `0x366080`** (dormant
  cross-world constant, glue :1986–1987). Any rung that makes a stub route live must
  retarget `[KDP+0x5f0]/[0x5f4]` → `0x50466080` (mirror).
- **NK save/restore is resume-correct with the M3a shim** (design doc §2): shim saves
  r7–r13, NK saves r14–r31/r1/CR/XER/CTR through r6=ECB; scheduler restore resumes at
  `r10=r12=restart`.
- **R1 (r1-independent UserModeMSR load) is ALREADY LANDED** (immediates, rom_patches
  :852–854) — yet MSR-on still crashes; the MSR story is Q-E's, with a TIME-BOX and
  fallback (rev 2 P5). **(rev 2 C10:** Q-E must also re-verify the PR=1 tolerance —
  0xd032 sets PR=1; the design doc's residue 4 says JIT tolerance is "assumed benign by
  precedent, not re-verified post-M3a/M5".)
- **Trampoline budget (rev 2 C6 corrected):** with pool ON + user_msr ON the trampoline
  is 40 words ending `0x429be0` — 8 words below the `0x429c00` stop stubs. New
  handlers/stubs go above `0x429c30`; **every new site gets a real zero check before
  writing** (the existing idiom only checks tp[0]/tp[1]; the stop stubs were written
  unchecked — do not copy that).
- **(rev 2 C4) Table/stub code writes happen at PatchROM time (host-side) ONLY.** The
  JIT compiles ROM-mirror pages and does not invalidate ROM-range blocks on guest stores;
  the ROM mirror is not NK-wiped, so the runtime re-assert idiom is unnecessary there.
  Any runtime code write requires an explicit invalidation story in the task text.
- **(rev 2 C9) `/tmp/rom901.bin` provenance:** the current file contains the PATCHED
  entry table (it postdates the trampoline patches). Task 0 re-establishes provenance
  (fresh SS_DUMP_ROM dump labeled patched; raw-ROM reads need the original .rom or a
  pre-patch dump) before tagging anything [RAW-ROM].
- **Canonical gate set (rev 2 P9 — "full gates" below means exactly this):**
  `make build-ss`; `SS_HARNESS_BATCH=1 make test-jit` AND plain `make test-jit` (both
  353/353, legacy authoritative); `make -C src/machine test` 11/11 ALL PASS;
  `make e2e-test` (122); paravirtual `make e2e` PASS + byte-identical (no [NW-*]/pool/
  switch lines). A task may name a SMALLER set only with a stated reason.
- **Standing rules**: per-task commits; struct fields appended LAST; stale-.o awareness
  (machine-header deps exist since c0e33d2b — but rom_patches/glue are NOT covered by
  those; clean-rebuild after header changes there); boots authorized; one shared checkout.

## Tasks

### Task 0: FE01/FE02 contract recon (BINDING — gates Tasks T..X; static RE + bounded probe boots)
Pin each contract in a written addendum (M6A-ONGOING-ENTRY-DESIGN.md, "Rung 2 contracts"
section). Budget honesty (rev 2 P5): this is N bounded diagnostic boots (probe-PC limit is
8/run), not "one session" — but each boot ≤60s and each question gets at most 2 boots
before its residue status is decided.
- [ ] **(rev 2 C9 first)** Re-establish ROM-dump provenance (patched vs raw) before any
  [RAW-ROM] tag.
- [ ] **(Q-A) The 0x220 save-record layout**: capstone the FE01 service from the allocator
  success path (`0x5046e324` onward, glue blocks `0x5046e1a0..e2ec` and beyond) — what the
  DR itself writes vs what it expects the NK/slot side to write; the `[ECB+0x710]+0xd8`
  current-record link discipline; how many records the boot's nesting needs.
- [ ] **(Q-B) Post-allocator control flow + consumed-slot map**: which entry-vector slots
  (of the 16; resolution in Codebase facts) the FE01 success path actually consumes, in
  what order, with what register state. Deliverable includes the **advance enumeration of
  expected parked-PC stubs** (rev 2 P7) and records the 8-vs-16 resolution.
- [ ] **(Q-C) The retry loop's pivot**: why FE01 currently retries — trace the live spin
  (FE07 semantics: what opcode service is FE07, what does its `bne` test, who sets that
  state). **This names the exact condition Task V must satisfy — and if that condition is
  an NK surface beyond slot population + record plumbing + the pinned conventions, the
  STOP-RULE fires here** (rev 2 P4).
- [ ] **(Q-D) The TVector entry convention**: MixedMode 68k→PPC calling convention at
  `0x500cef8c` — registers/procInfo args, return address, r1/stack — from the FE01 service
  disassembly + the **RoutineDescriptor at `0x10024de8`** (the procDescriptor/TVector
  pointer is `0x10024758` — rev 2 C7). **Deliverable MUST include a register →
  expected-value/class table; Task V's probe gate is exact conformance to it** (rev 2 P7).
- [ ] **(Q-E) MSR across the switch — TIME-BOXED (rev 2 P5)**: what MSR the PPC code runs
  under, where the transition executes (guest-side default; justify if not), the PR=1
  tolerance re-verification (rev 2 C10), and the SS_M6A_USER_MSR zero-page slide. Budget:
  the standard 2 boots + static analysis. **Fallback if not root-caused in budget:** Task V
  implements switch-local MSR handling per the FE01 disassembly only; SS_M6A_USER_MSR stays
  quarantined as a known-broken diagnostic (default off, so documented in Task Z); the
  slide is a named residue and explicitly NOT a Task V blocker.
- [ ] **(Q-F) FE02 switch-back mechanics**: where FE02 is issued, what it reads from the
  record, where the 68k resumes, who frees the pool bit.
- [ ] **Probe pack**: [PROBE-O1] table[0] entry census (pool-on regime — BASELINE only;
  Task X re-censuses post-W, rev 2 P1), [PROBE-O2] who built the restored ctx, [PROBE-O4]
  `[KDP+0x660]` flag consumption.
- [ ] **Gate (rev 2 P3 — the blocking-answer table):** the addendum exists; every answer
  tagged [RAW-ROM]/[PATCH]/[STATIC]/[PROBE✓]; and the BLOCKING answers are pinned (not
  residues): **Task T blocks on Q-A's record-count answer (pool sizing); U blocks on Q-B;
  V blocks on Q-C + Q-D (+ Q-E verdict-or-fallback); W blocks on Q-F; X blocks on O1
  baseline + O4 (O4 residue ⇒ R4 deferred, recorded — non-blocking).** A residue on a
  blocking answer invokes the stop-rule — no improvisation. Capture-only telemetry commits
  allowed (full gates). Commit the addendum.

### Task T: SS_NW_MM_POOL → profile-provisioned default (+ the collision fix + the safety stop)
- [x] **(rev 2 C1) Relocate the pool** off the Hnfo scratch per the occupancy map (e.g.
  base `0x68ff5800`); write the **sub-KDP occupancy map** into the Task-0 addendum doc
  section (tracked, with the Task-X scratch word reserved); fix the falsified "no other
  users" comment at the seed site.
- [x] Promote the pool seed to the newworld profile default (env flips to opt-OUT
  `SS_NW_MM_POOL=0` for A/B; paravirtual/OldWorld untouched). **(rev 2 P11/Task-Z hook:**
  `SS_NW_MM_POOL=1` remains valid explicit-on — document the disposition.)
- [x] **(rev 2 C3) The `[ECB+0xEC]` zeroing moves inside the cold-arm-only guard NOW**
  (cold-only == every entry until Task X lands — free today, removes the V/W warm
  re-entry corruption window). Document the invariant at the seed site.
- [x] **(rev 2 P2) The `+0x3c` (slot 15) allocator-exhaustion LOUD STOP lands HERE** (one
  verify-zero-first branch write to a unique parked-PC stub above 0x429c30) — T's pool
  sizing fallback is then self-contained: if Q-A's nesting answer is unclear, keep 4
  records + the loud stop, never guess bigger.
- [x] Gates: full gates + one newworld diagnostic boot confirming default-on pool = zero
  reset transitions in the r24 ring. Commit.

### Task U: NK entry-vector slot population (the Q-B consumed set)
- [x] Per Q-B's pinned consumed-slot map: real handlers for consumed slots (guest-side
  stubs in the mirror zero run, **written at PatchROM time — rev 2 C4**; verify-zero-first
  on every new site, rev 2 C6). *(Q-B: the only consumed slot is slot 1, which already
  has its real static branch from patch_68k_emul — NO new real handlers needed.)*
- [x] Slots NOT consumed stay as loud stops with unique PCs (never zeros) — the expected
  parked-PC set is **Q-B's advance enumeration; the gate is parked-PCs ⊆ that list**
  (rev 2 P7 — no post-hoc "expected"). *(Zero slots 4, 6-14 → unique `b *` stops at
  0x50429c40..0x50429cd0 per the addendum table; live parked-PC set observed = ∅ ⊆ list.)*
- [x] Each handler comments cite the addendum section. All inside newworld/lenient gating.
- [x] Gates: full gates (ran the FULL canonical set including paravirtual `make e2e`
  PASS + log clean); diagnostic boot: `+0x3c` stop not hit; parked-PC subset check
  (none of the new stops nor slot-15 in the r24 ring). Commit.

### Task V: FE01 switch completion — enter the TVector (+ FE07 ownership)
- [x] Implement the 68k→PPC switch per Q-A/Q-B/Q-D/Q-E: complete the save-record contents
  the DR doesn't write itself, establish the MixedMode register state, perform the MSR
  transition at the pinned site (riding execute_mtmsr's EE-edge re-raise — verified
  contract, ppc-execute.cpp:1321–1347), branch to the TVector. Env-gated
  `SS_NW_MM_SWITCH=1`, default OFF until Task Y. *(Per the pinned Q-C the entire
  implementation reduced to TWO trampoline-resident KDP seeds — `[KDP+0x660]|=0x00200000`
  and MRU pair-0 `[KDP+0x340/0x344]=0x68fff400` — the record contents/register state/
  TVector call are all performed by the DR's FE01 service + the staged NK's own
  switch/save/scheduler + the ROM's native glue 0x500ebc20. No MSR write (Q-E).
  Switch⇒pool misconfig handled: SS_NW_MM_POOL=0 + switch ⇒ loud log + pool forced on.)*
- [x] **(rev 2 P8) FE07 is THIS task's scope**: whatever the FE07 service must return for
  the retry stub's `bne` to fall through, per Q-C — if Q-C named an out-of-scope NK
  surface, the stop-rule already fired in Task 0. *(Q-C pinned: FE07 needs NO
  implementation — the NK switch path writes the nonzero command byte; verified live:
  the retry spin is GONE under switch-on. P8 satisfied with zero new FE07 code.)*
- [x] Resolve the SS_M6A_USER_MSR interaction per Q-E's verdict-or-fallback (subsume / fix
  / quarantine — documented either way). *(QUARANTINED: switch-on disables user_msr with a
  loud [NW-TRAMP] V: line — Q-E verdict (per-context MSR via NK switch) + the 0x429c00
  word-budget overrun (49>48) both forbid combining. Stays default-off known-broken
  diagnostic; residue R-2 unchanged.)*
- [x] Probe sub-contract (PASS/FAIL): `SS_PROBE_PC=0x500cef8c` shows ≥1 visit with the
  register dump conforming to **Q-D's expected-register table** (rev 2 P7 — "sane"
  deleted). What the TVector code DOES afterward is diagnostic. *(PASS, zero
  falsifications: visits 1/10/100+; CTR=0x500cef8c exact, r2=0x10024754 exact,
  r1=0x103ffe00 (64-aligned, <0x103ffe5c, [r1]=0x103ffe5d back-chain tagged |1),
  LR=0x500ecac0 (glue region), r30=0x10024de8 (RD). Diagnostic: FE01 retry spin GONE
  (comp unfroze 3573→3600, jNK collapses 116M→~16K); new frontier = 68k hardware-init
  poll loop (r24 ring in 0x500004e4..0x5000057c), SCC IER/VIA ORB MMIO storm
  (~7.6M reads/10s), CUDA i2c probes to absent devices (addrs 41,4F,B5,91,80,C1,28,71,9D).
  Switch-off baseline boot byte-identical to pre-V behavior (0 TVector visits, comp 3573).)*
- [x] Gates: full gates + the probe sub-contract. Commit. *(build-ss OK; batch + plain
  test-jit 353/353 score=100; machine tests 11/11 ALL PASS; e2e-test 122 passed;
  paravirtual make e2e PASS clean lifecycle.)*

### Task W: FE02 switch-back — ⛔ STOP-RULE fired (second falsification); partial landing, re-scope required
- [x] Per Q-F: restore the 68k context from the record, clear the in-use bit, resume the
  68k after the $AAFE site with the convention's result registers. `SS_NW_MM_SWITCH`
  covers the pair. *(PARTIAL — implementation, not verification: the "completion
  already works" hypothesis was FALSE (pre-W, every TVector excursion ended in a 68k
  RESET via the always-cold table[0] — ring `100266f2 → 0 → 1 → 5000002c`). Landed
  (env-gated, default OFF): the table[0] cold/warm discriminator at 0x50429d00 (NEW
  verified-zero region — the trampoline stays 46/48), R2 scratch 0x68ff6080 cold-once,
  cold-arm seed [KDP+0x658]=ECB (the NK switch-back restore target, live-probed
  garbage =1), warm-arm pool re-assert (the NK ctx save OVERLAYS [ECB+0xE0..0xEC] —
  new finding) + b 0x5046f900 (original slot-0 route). The LAST leg self-switches:
  [KDP+0x65c] (save-target) and [KDP+0x658] (restore-source) are both ECB — the warm
  save destroys the parked emulator ctx. The fix is the [KDP+0x65c] current-world
  flip discipline (warm arm := MMCB; slot-1 stub prepend := ECB) — touches the
  working forward stub, beyond the one-iteration budget. Stop-rule trigger 1
  (re-scope, not improvisation). Full chain + corrected contract: addendum
  "Task W results".)*
- [ ] Round-trip sub-contract (PASS/FAIL): one full FE01→TVector→FE02 round trip — TVector
  probe visit + **the post-$AAFE 68k PC observed in the SS_DR_R24_RING tail** (rev 2 C5 —
  68k PCs are not probe-able); `[ECB+0xEC]` returns to its pre-call value; guest[0]/[4]
  unmodified (`SS_JIT_WATCH_ADDR` — hex values, with SS_JIT_TRACE_RING=1).
  *(SCOREBOARD: (a) FAIL — e1f4/e408/e1a0 zero visits, the 68k never resumes at the
  derived post-$AAFE PC 0x5000fcf2 (= r25 at TVector entry; `stw r25,0x3c(r31)` at
  0x500ed400 is the completion's resume-PC write). (b) premise FALSIFIED — [ECB+0xEC]
  is clobbered to saved-r12 by the NK ctx save every switch-out (the overlay finding);
  replaced by warm re-assert + residue R-10. (c) PASS — table[0] cold exactly once,
  guest[0]/[4] never rewritten after first entry (probe + watch, boots 3/5). R-4
  RESOLVED: command byte = 0xff, rides in r3 as the table[0]/NK selector.)*
- [x] Gates: as Task V. Commit. *(Full canonical set re-run on the partial landing —
  see commit; switch-off boots byte-identical (branch encoder reproduces 0x4BFBB280;
  W region unwritten when gated off).)*

### Task X: real ongoing entry at table[0] (design doc R2+R3+R4) — COLLAPSED by W/W2 (rev 3); executed as verify-and-leave + re-census + folds
- [x] **(rev 2 P1) PRECONDITION: post-Task-W re-census** — re-run the PROBE-O1 pack with
  the switch on; the R3 decision applies to the RE-CENSUS, not Task 0's baseline (the
  fault-path re-entries the baseline saw are removed by U–W). *(Done post-W2:
  /tmp/taskx_boot1.log — cold exactly once; the ONLY warm-entry class is the
  legitimate completion signature (r3=0xff, native stack r1=0x103ffa2c); slot-15 +
  parked-PC stops zero visits. Addendum "Task X results".)*
- [x] R2 discriminator: scratch word in the sub-KDP pool **at the occupancy-map-reserved
  address** (mapped+zeroed, data-only — NOT the ROM zero run, SMC/JIT hazard), cold-once
  semantics; the Task-T cold-arm invariant now becomes load-bearing. *(Landed in W;
  Task X confirms the minimal test-and-set protocol is sufficient — census shows
  binary cold/warm is the only consumed semantics.)*
- [x] R3 ongoing arm per the re-census + Q-B: stub route landed in W (`b 0x5046f900`);
  **RATIFIED by the re-census** (no non-completion warm entrant). The
  `[KDP+0x5f0]/[0x5f4]` retarget is STRUCK (rev 3.1 item 4) — **verify-and-leave
  DONE**: live values probed at cold AND warm entry = the NK-rebuilt staged pair
  0x50313bf8/0x503143a0; the glue's primary-world seeds are dead-on-arrival
  (comment-only at the seed site, seeds kept — choice documented). R4: landed in V.
- [x] R5 (nest balance) only if the chosen route requires it — else named residue.
  *(Not required: observed depth 1, R-1 tripwire stands as the named residue.)*
- [x] Sub-contracts (PASS/FAIL): cold path exactly once per boot (scratch-flag probe);
  guest[0]/[4] never rewritten after first entry (watch); jDR/comp still growing across
  re-entries. *(All PASS from fresh evidence — taskx_boot1 + taskx_boot2_legacy HB
  comp=3672/jDR growing.)*
- [x] Gates: as Task V + MSR-state regression per V's verdict. Commit. *(Full canonical
  set green: build-ss; batch + plain test-jit 353/353; machine ALL PASS; e2e-test 122;
  paravirtual make e2e PASS clean lifecycle. MSR regression: no MSR writes on the
  switch path (Q-E verdict unchanged); user_msr stays quarantined. Also folded the W2
  review minor: slot-1 flip gated on the W region having landed.)*

### Task Y: rung-2 acceptance (boots authorized) — honest gating + rollback (rev 2 P6)
- [x] **PASS/FAIL gates first, flip LAST.** Run with `SS_NW_MM_SWITCH=1` env-on:
  (a) full gates; (b) pool provisioned by default, no `+0x3c` stop hit; (c) **the slots
  Q-B named consumed (if any) executed** (probe counters nonzero; vacuous-pass not
  allowed — if Q-B found none consumed, gate (c) is N/A and recorded as such — rev 2 C2);
  (d) the FE01→TVector→FE02 round trip holds (V/W sub-contracts) on this config;
  (e) cold-start exactly once + guest[0]/[4] stable (X sub-contracts).
  *(ALL PASS pre-flip: (a) canonical set green; (b) pool ON, 0x50429cf0 zero visits;
  (c) slot-1 route 0x50429d80 visit=1 r3=MMCB — nonzero, not vacuous; (d) fresh ring
  round trip incl. literal resume at completion-written [saveblk+0x3c]=0x50033776,
  e1f4 r6=0xff; (e) PASS. Addendum "Task Y results".)*
- [x] **Fix budget (rev 2 P6):** telemetry/capture commits freely; at most ONE small
  in-scope fix iteration per falsified contract (consistent with the one-iteration rule),
  full gates re-run after any fix. *(Budget consumed: ZERO — no contract falsified at
  acceptance, no unlocks needed.)*
- [x] **THEN flip** `SS_NW_MM_SWITCH` to the newworld profile default (env opt-out kept)
  and re-run (a)-(e). **Any gate failure after the flip ⇒ the flip is REVERTED in the
  same task (machinery stays env-gated), the failure recorded — the milestone does not
  ship default-on with red gates.** *(FLIP LANDED: default-ON for newworld, opt-out
  SS_NW_MM_SWITCH=0 (pool polarity mirrored); misconfig logic adjusted (pool-off A/B
  now needs switch-off too, loud message). Post-flip (a)-(e) ALL PASS on the true
  default config (no env var); opt-out boot reproduces the pre-switch baseline
  byte-identically (comp 3573, jNK 116M spin, 0 TVector, no W/W2 writes). NO revert.)*
- [x] **DIAGNOSTIC OUTCOMES (recorded, NOT gates):** the FE01 retry spin gone ✓;
  MPLibrary's parcel init RUNS but does NOT return — the CFM caller region executes
  (ring, file 0xf4xx) and the chain advances through TWO MixedMode round trips to
  named wall (i): **`sc` at pc=0x500d638c, unresolved syscall_entry, SRR0=0x500d6390
  SRR1=0x00007072 lr=0x500cf108 r1=0x103ffb50** — identical pre- and post-flip;
  captured with full HB/CUDA baseline (comp=3672 jNK=4104 exc=0/1/0/0; CUDA 13/9
  quiet). Nothing NEW between the round trip and the sc wall. Stop-rule trigger 2:
  captured and stopped — vector 0xC00 is the NEXT milestone's frontier.
- [x] Record results in M6A-ONGOING-ENTRY-DESIGN (results section) + M6A-WAVE2-SHIM-RECON
  (frontier update). Commit. *(Both updated 2026-06-11.)*

### Task Z: docs
- [ ] DIAGNOSTICS.md: knob changes (`SS_NW_MM_POOL` default flip + the `=1` explicit-on
  disposition — rev 2 P11; `SS_NW_MM_SWITCH`; SS_M6A_USER_MSR disposition per Q-E);
  CHANGELOG (acceptance numbers, the pool-collision fix); MACHINE-LAYER-PLAN M6 row;
  ROADMAP cross-check; LEARNINGS (the pool-collision lesson: "free gap" claims need an
  occupancy map, and the falsifying writer was our own earlier commit); cross-tracker
  grep for stale "always-cold"/"SS_NW_MM_POOL default OFF"/8-slot-table statements.
  Commit.

## Stop-rule (rev 2 P4/P10 — broadened + operationalized)
Triggers, per MACHINE-LAYER-PLAN §9:
1. **(broadened)** If Task 0 shows FE01 completion requires **any NK surface beyond slot
   population + save-record plumbing + the pinned register/MSR conventions** (whether the
   full L-class context-creation Trampoline or an unscoped M-class service behind FE07/
   Q-C), STOP after the addendum and re-scope — the recon itself is the tripwire.
2. If Task Y's diagnostic shows MPLibrary advances past FE01 but dies on the syscall entry
   or another NK service, that is the NEXT milestone's named frontier — capture and stop;
   no vector-0xC00 staging beyond the existing abort-with-capture.
3. **(P3)** A residue on a BLOCKING Task-0 answer (the blocking-answer table) invokes
   trigger 1's re-scope, not improvisation.

Within tasks — the one-iteration rule, operationalized (P10): if a pinned contract is
falsified live, (a) reopen the addendum with a dated falsification entry; (b) ONE
additional bounded probe boot to re-pin; (c) resume the falsified task with the corrected
contract. A SECOND falsification of the same contract escalates to the stop-rule.

## Self-review record
Spec coverage: all six authoritative inputs consumed; coordinator seams T/U/V/W/X mapped;
every open design question carried as Task-0 recon with a blocking-answer table. Honest
gating separates suite/probe sub-contracts (PASS/FAIL, each falsifiable in advance) from
boot advancement (diagnostic). Wave-2-deferred backlog (OpenPIC model+tests, tm_task/
via_int dispositions, io_poll retirement, the shim wave) is OUT — parallel backlog.
Sequencing: strictly one task at a time (shared trampoline/glue sites).

## Red-team record

**Round 1 (pre-implementation, two reviewers, 2026-06-11) — verdict: restructure-then-GO;
all findings folded as rev 2 markers in-line.**

*Process reviewer* (2 Critical, 6 Major, 4 minor): **P1** Task X's R3 decision used a
census Tasks U–W invalidate → post-W re-census precondition. **P2** "gates U/V/W/X" was
dishonest (T depends on Q-A; T's fallback cited U's stub) → Task 0 gates T..X; the +0x3c
loud stop moved into T. **P3** no must-pin vs may-be-residue distinction → the
blocking-answer table in Task 0's gate. **P4** stop trigger 1 too narrow (the FE07
M-class-service middle case) → broadened, recon as tripwire. **P5** Q-E unbounded → time-box
+ written fallback + honest boot budget. **P6** Task Y had no flip rollback or fix budget
(the CV-10 lesson) → flip-last + revert-on-red + one-fix-per-falsified-contract budget.
**P7** unfalsifiable gates ("sane registers"; post-hoc "expected" parked PCs) → Q-D
expected-register table; parked-PCs ⊆ Q-B's advance enumeration. **P8** FE07 had no
implementation owner → Task V. **P9** inconsistent gate sets → canonical set + named
deltas. **P10** return-to-Task-0 operationalized. **P11** SS_NW_MM_POOL polarity
disposition documented. **P12** reviewability: 0xf46e observation method, nest-protocol
clause, dispatch-one-task-at-a-time line.

*Contracts reviewer* (1 Critical, 4 Major, 7 minor; all load-bearing recon numbers
byte-verified — allocator/fallback/table contents dumped and matched): **C1 CRITICAL —
the 0x68ff5000 pool/Hnfo-scratch collision is REAL** (glue :1896–1898; machine-detect
writes scratch +0x10..0x17 inside record 0) → Task T relocates the pool + occupancy map.
**C2** the Goal's spin mechanism was the dead pre-pool fault path → reworded as
hypothesis; gate (c) made conditional. **C3** V/W-before-X warm-re-entry window with the
every-entry [0xEC] wipe → cold-arm guard pulled into Task T. **C4** runtime code writes
into JIT-compiled ROM pages → PatchROM-time-only constraint. **C5** Task W's second
probe leg was a 68k PC (not probe-able) → r24-ring observation. **C6** trampoline budget
corrected (40 words → 0x429be0 with both flags); real zero checks mandated. **C7**
RoutineDescriptor address corrected (0x10024de8). **C8** the 8-vs-16-slot contradiction
RESOLVED statically (16 slots; POWERPC_ILLEGAL==0; no slot-4/slot-15 collision). **C9**
rom901.bin provenance drift → Task 0 re-establishes. **C10** PR=1 tolerance added to Q-E.
**C11** switch⇒pool dependency stated. Verified clean: FE01 facts faithful, mtmsr EE-edge
contract real (ppc-execute :1321–1347), paravirtual gating complete, design-doc
representation accurate.

## Rev 3 re-scope (2026-06-11, post-Task-W stop-rule)

Task W fired stop-rule trigger 1 on the second falsification of the switch-back
contract — correctly. The full corrected contract is PINNED with live evidence in the
addendum ("Task W results", commit 3dd1550b): the architectural switch-back works
through table[0]/slot-0 with r3=0xff as the NK selector (R-4 resolved), and the ONE
missing surface is the **`[KDP+0x65c]` current-world flip discipline** (R-12): the
word must hold ECB while the 68k world runs and the MMCB during native excursions; no
NK code maintains it — it is a Trampoline-init surface (on real HW the real slot-stub
bodies plausibly encode the flip).

### Task W2: the world-flip discipline (re-scoped from W; supersedes W's remaining scope)
- [x] Warm arm (0x50429d00 region): set `[KDP+0x65c] := MMCB (0x68fff400)` before
  `b 0x5046f900`. Slot-1 stub body (ours, patch_68k_emul): prepend
  `[KDP+0x65c] := ECB (0x68fff000)` (idempotent on the first call). Both env-gated
  with `SS_NW_MM_SWITCH` (the slot-1 prepend must be structurally inert when the
  switch is off — same table/stub write discipline as T/U: verify-before-write,
  PatchROM-time only; if the prepend cannot be gated at patch time without changing
  the switch-off stub bytes, gate it at RUNTIME inside the stub via a guarded load —
  document the choice). *(Implemented per rev 3.1 BINDING corrections: slot-1 side =
  the RETARGETED 7-word region at verified-zero 0x50429d80 (NOT a prepend, NOT
  patch_68k_emul) + table-word retarget at 0x46e8c4 with verify-EXPECTED ==
  0x4800113c; warm-arm flip = 5 words inserted before mfctr/b — the W region is now
  30 words, 0x429d00..0x429d74 end-exclusive 0x429d78. Switch-off: zero writes.
  + item-6 hardening: cold-arm [KDP-0x14]:=ECB, one word.)*
- [x] **RED-TEAM-FIRST risk (blocking)**: the M3a DEC shim also consumes
  `[KDP+0x65c]` (the KDP register-save shim saves through r6=[KDP+0x65c]) — the flip
  is architecturally right for it (the interrupted world's ctx is exactly what the
  shim should save into) but UNVERIFIED: a DEC delivery during a native excursion
  would now save into the MMCB instead of the ECB. Verify the shim's full consumption
  chain (and the scheduler-restore side) tolerates MMCB-parked state, or fence DEC
  delivery during native excursions (EE is per-context anyway per Q-E — check what
  MSR the native ctx runs with), BEFORE implementing. *(Resolved per rev 3.1 item 3:
  the DEC fence landed — deliver_pending_dec_exception defers while [XLM_RUN_MODE]
  (0x2810) != 0; new deferred_native counter (exc= 4th field). Inert live:
  exc=0/1/0/0. Residues R-14 (backward-half window) + long-term shim-save-target →
  [KDP-0x14] recorded.)*
- [x] Round-trip sub-contract (carried from W, now expected to PASS): TVector visit +
  68k resume at 0x5000fcf2 in the r24 ring; e1f4/e408 visits nonzero; cold-once +
  guest[0]/[4] stability maintained. *(PASS — addendum "Task W2 results": TVector
  visit=1 w/ Q-D-exact registers; ring shows the 0x5000fcf2 → 10024dea/de8 $AAFE
  chain then CONTINUED execution (no reset signature; literal post-excursion
  re-record dedup-masked), and the second excursion's resume IS literal in the ring
  (… 100266f2 → 0 → 1 → 50033776 …) matching the probed [saveblk+0x3c]=0x50033776;
  e1a0 + e1f4 execute (e1f4 r6=0xff command byte live; e408 0 block-entry visits —
  chained, recorded); cold-once + stability PASS. Diagnostic frontier: sc at
  0x500d638c, unresolved syscall_entry — stop-rule trigger 2's named wall, captured.)*
- [x] Gates: canonical set + switch-off byte-identical boot. *(build-ss OK; batch +
  plain test-jit 353/353 score=100; machine 11/11 ALL PASS (4794 checks); e2e-test
  122 passed; paravirtual make e2e PASS; switch-off boot = baseline signature
  (0 TVector, comp 3573, FE01↔NK spin, no W/W2 writes).)*
- [x] One-iteration rule re-arms for THIS contract (the W falsifications are spent;
  a NEW falsification of the flip contract = stop-rule, full re-plan). *(Zero
  falsifications — no re-pin boot consumed.)*

Task X's remaining scope after W's partial landing: R3 choice is MADE (warm arm →
`b 0x5046f900` stub route landed in W; ~~the [KDP+0x5f0/4] retarget question must be
re-checked — W's warm arm uses the displaced original slot-0 stub which exits via
[KDP+0x5f0]~~ **STRUCK per rev 3.1 item 4 — verify-and-leave**: the live
[KDP+0x5f0/4] values are NK-rebuilt staged addresses (0x50313bf8/0x503143a0, Q-C
probe authoritative), the warm path traverses them correctly, and retargeting would
destroy both switch directions); R4 ([KDP+0x660] flags) landed in V; R2
(discriminator) landed in W (Task X refines the scratch-word protocol if needed).
Task X collapses into: the [KDP+0x5f0/4] verify-and-leave check + the post-W2
re-census + the X sub-contracts. Then Y (acceptance + flip-last) and Z (docs) as
planned.

### Rev 3.1 — W2 red-team verdict folded (GO-WITH-CHANGES; the corrected design is BINDING)
1. Slot-1 side = a retargeted **switch-on-only 7-word region** in PatchROM_NW_trampoline
   (mirror-only; retarget mirror table word 0x5046e8c4 with **verify-EXPECTED ==
   0x4800113c** before writing — the verify-zero idiom's sibling for nonzero sites;
   stub: mtctr r1 / lwz r1,0x2804(0) / lis+ori r0=ECB / stw r0,0x65c(r1) / mfctr r1 /
   b 0x5046fa00; clobbers r0/CTR only). NOT an in-place prepend, NOT patch_68k_emul.
2. Warm-arm flip: 5 words ([KDP+0x65c]:=MMCB via explicit r28=KDP re-derive) inserted
   after the pool re-asserts, before mfctr r28/b 0x5046f900; verify-zero-first over the
   enlarged 0x429d00 region (~28+5 words).
3. **DEC fence lands NOW** (cheap, inert while delivered=0): deliver_pending_dec_exception
   defers while ReadMacInt32(0x2810) != 0 (XLM_RUN_MODE — NK-maintained 1-forward/
   0-backward on exactly this switch pair). Long-term residue: shim save target →
   [KDP-0x14] on newworld.
4. **STRIKE the [KDP+0x5f0/4] retarget from Task X** — live values are NK-rebuilt staged
   addresses (0x50313bf8/0x503143a0, Q-C probe authoritative); the warm path traverses
   them correctly; retargeting would destroy both switch directions. Task X verifies-and-
   leaves; design doc §1.3's wrinkle paragraph gets corrected.
5. Correct the falsified "no NK writer of 0x658" claims (addendum + rom_patches
   ~:1031-1033): one cold-init writer at static 0x310834 (the live-garbage source),
   pre-table[0], cold-arm seed wins; register the reset-re-init residue.
6. Optional one-word hardening: cold-arm [KDP-0x14]:=ECB.
7. New residues: stale-MMCB window vs slots 2/3/5 if ever consumed; backward-half
   mid-switch DEC once EE is live ([0x2810]=0 during the backward save — benign by
   same-values, recorded).
[KDP+0x660] stays a persistent OR (both directions need cr2eq). [KDP+0x658] stays
cold-seed-only (constant emulator-ctx word). MSR: no rfi on the switch path — nothing
to flip (Q-E re-confirmed).
