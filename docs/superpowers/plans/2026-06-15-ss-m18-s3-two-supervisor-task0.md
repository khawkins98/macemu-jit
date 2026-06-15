# SS_M18 Stage 3 — two-supervisor reconciliation: DEEP Task-0 recon (BINDING; gates ALL S3 implementation tasks)

> **Status:** rev-2 (2026-06-15) — 4-reviewer pre-implementation red-team folded (PROCESS GO-WITH-FIXES,
> TECHNICAL GO-WITH-FIXES, ADVERSARY OPTIMISTIC-BUT-OK, SPIKE-FAITHFULNESS FAITHFUL-WITH-GAPS). Binding
> amendments captured in "Red-team record (rev-2)" below; key inline edits applied. **This plan is recon +
> design ONLY.** It pins the three S3 blocking questions,
> SCORES the four candidate architectures, and surfaces the infeasibility branch; it writes NO
> `SheepShaver/src/**`. S3 implementation (retire-the-forge restructure in `exc_core.cpp` /
> `sheepshaver_glue.cpp` / `event_sched`/`virt_clock.cpp` / `rom_patches.cpp`, AND the live paged-MMU
> co-land) is the SEPARATE S3-impl milestone, gated on these answers.
>
> **Scope law (carried from the program):** Route A is SETTLED and NOT relitigated; the program shape
> (S1→S3→S4) is NOT relitigated; the Q1 verdict (resident supervisor, **no handoff boundary**) is SETTLED
> (`FINDINGS-trampoline-re.md` Q1) and is NOT re-derived. This Task-0 operationalizes S3's *own* recon —
> it picks the architecture; it does not re-ask whether there is a collision.

## What S3 is (binding one-paragraph frame)

S3 = make SheepShaver STOP forging the NanoKernel's outputs (the `SS_NW_TRAMPOLINE` register-fixup forge
at `rom_patches.cpp:716`, the C-assigned `SDR1`/zeroed-HTAB/forged-`MSR=0x7072` at
`sheepshaver_glue.cpp:2806/2804/2918`, the entry-vector synthesis, the synthetic scheduler) and instead
RUN the real, md5-verified NanoKernel-v02.27 as the **live, permanently-resident, paged, translated-mode
PPC supervisor**. The real NK has **no handoff boundary** (Q1; re-confirmed `FINDINGS-s3-nk-ownership.md`):
it `rfi`s @`0x5031003c` into translated supervisor mode and OWNS the live exception vectors (EXT
`0x50314880` / SC `0x50314ac0` / DEC `0x50313200` / PROGRAM `0x50314700`), the DEC reschedule loop
(`0x50313200`, which re-arms DEC itself via `mtspr 0x16`@`0x50313234`), the 68k-emulator dispatch (via its
OWN ECB/KDP task dispatch — `FINDINGS-s3-nk-ownership.md` proved the parcel has NO static ref to
EmulatorCode `0x50360000`/DR `0x5046/0x5048`, so it loads a runtime pointer), and the MMU. **S3
reconciles the two supervisors, and the live paged MMU is born inside S3** (see
`MMU-NANOKERNEL-INSEPARABILITY.md`, canonical). **S3 Task-0's deliverable is the RECON that pins the
architecture choice + the blocking answers — NOT S3 code.**

## ★ Decisive recon input (folded 2026-06-15): co-residency is statically IMPOSSIBLE

`FINDINGS-s3-nk-ownership.md` (`[STATIC-CAPSTONE]`, md5-verified parcel) establishes the real NK is the
**SOLE, RESIDENT owner** of DEC + the exception vectors + the 68k-dispatch path, and **never `blr`-yields
to a caller**. Consequence for Q-S3.3: the **CO-OWN candidate is scored OUT at recon** (SS and the NK
cannot co-own a supervisor the NK never delegates); the architecture reduces to **"which supervisor is
resident"** — REPLACE (retire SS's host machinery, run the real NK) or the thin-shim variant
(SS-as-hardware / NK-as-OS). "yield-fully" also fails Q1's no-yield-point. This sharpens Q-S3.3 from "pick
among 4" to "REPLACE vs thin-shim, with CO-OWN/yield-fully scored out with evidence."

## Dependency-graph reconciliation (read before the goal)

- **S3's true prerequisite is S1-MECHANISM (DONE), not S1-live.** Per `MMU-NANOKERNEL-INSEPARABILITY.md`:
  `paged_mmu_translate()` + its oracle test (Task A) and the static NK-MMU constants
  (`FINDINGS-s1-mmu-constants.md`) are the satisfied S1→S3 edge. **The S1 LIVE paged MMU is an S3
  sub-deliverable** — no live `(SR/BAT/SDR1)→PA` map exists until the real NK install runs, which is S3.
  "S3 blocked by S1" = blocked by S1-mechanism (DONE), NOT a deadlock; the live-MMU node and S3 are one
  node. The architecture chosen in Q-S3.3 must treat "the NK programs SR/BAT/SDR1 and we host the regime"
  and "a live paged MMU exists" as the **same deliverable**.
- **S3-impl also depends on S2 landed** (loader + OF-CI + Core99 DT + `/mmu` handoff) — the NK must be
  *reached* before it can be *hosted*. This Task-0 may proceed in parallel with S2b (recon is
  boot-disjoint); S3-impl start additionally requires S2 PASS.

## S3-Task-0 item-0 — the Dolphin SHM-arena feasibility spike (precondition input)

A named, NK-independent platform risk is being built **in parallel**
(`docs/superpowers/specs/2026-06-15-ss-m18-s1-shm-arena-spike.md`): a standalone/synthetic/gated-off proof
that on macOS arm64 we can stand up SHM segment + aliased RW views + 16 KB-host-page granularity + safe
overwrite-under-concurrent-reader. This is the single biggest S1-live platform risk, and S1-live co-lands
in S3. **This Task-0 FOLDS the spike's result as a precondition INPUT.** The spike is now **PROVEN**
(`test_shm_arena_spike`, commit `acd89dce`) → spike-CONSTRUCTION = GREEN; the live-MMU sub-deliverable
inherits a de-risked substrate for the SINGLE-remap primitive. **M4 (rev-2): the spike is DECOUPLED from
the supervisor architecture GREEN-PASS** — the live-MMU substrate is a separately-tracked residue gating
S3-impl START, not a term in the architecture verdict (resolves tension #4/#5). **Critical carried residue
(adversary + spike-faithfulness):** the spike proved CONSTRUCTION (the single-remap primitive), NOT LIVE
OPERATION — the NK's `mtspr`-storm does MULTI-BAT-entry table updates, and multi-entry TRANSACTIONAL
ATOMICITY remains unproven (see "Carried risk re-aim" below + the spike's OWED list O1–O6).

## Goal (PASS-vs-DIAGNOSTIC)

**PASS (the Task-0 deliverable — falsifiable).** S3's three blocking questions closed in writing (each
evidence-tagged), the candidate architectures ENUMERATED + SCORED with the infeasibility test applied, and
the `sc` syscall sub-wall dispositioned — in a committed addendum (`FINDINGS-s3-two-supervisor.md`).
PASSES when all of:

1. **Q-S3.1 answered** — verdict on whether SS's `execute_68k` (`glue:1461`, pair read `:1504-1505`) can
   be DRIVEN BY the NK's path or is dead under newworld (the NK reaches 68k via its own ECB/KDP dispatch +
   ROM EmulatorCode `0x50360000`), with the cited evidence (`FINDINGS-s3-nk-ownership.md` Q-S3.1: the
   parcel has NO static `0x50360000`/`0x5046`/`0x5048` reference → the SS Execute68k pair
   `[KDP+0x1074/0x1078]=0x50480000/0x50460000` is an SS synthetic, not a real-NK structure).
2. **Q-S3.2 answered** — disposition of `exc_core.cpp` ExcEnter/ExcRfi (`:12`/`:104`, PEM masks = LAW,
   which **SURVIVE as the hardware-vectoring mechanism**) and the SS scheduler (`event_sched`/`virt_clock`)
   when the NK owns DEC (`0x50313200`): given the NK-ownership finding, this is **REPLACE/YIELD** (SS
   scheduler goes quiescent; the NK is the live rescheduler), with the falsifiable "SS-scheduler-ticks=0
   while NK live" predicate.
3. **Q-S3.3 answered** — the candidate architectures **enumerated AND scored**: REPLACE / CO-OWN
   (scored OUT, evidence) / yield-fully (scored OUT, Q1 no-yield) / thin-shim; a single PICK or a ≤2
   shortlist (REPLACE vs thin-shim), citing `ppc-cpu.cpp:1991` as the profile-gating precedent. **Includes
   the INFEASIBILITY TEST** + its budget.
4. **The `sc` syscall sub-wall dispositioned** — the Path A double-increment (`sc`→`execute_illegal`→PC+=8;
   `HANDOFF-NEWWORLD-SUPERVISOR-MMU.md:299-313`) mapped to the chosen arch: under REPLACE the NK's real SC
   handler `0x50314ac0` (the `-1/-2/-3` fast-traps, `FINDINGS-s3-nk-ownership.md` Q-S3.2c) becomes live and
   the SS `sc`-as-illegal path retires under the gate; pin whether in-scope for S3 or a named S3-impl
   sub-task.
5. **The live-MMU co-land sub-deliverable SPEC'd at recon level** — which DEFERRED S1 plan
   (window vs softmmu) the chosen arch selects, folding the SHM-arena spike state. NO live MMU built here.
   **Minor (c): this is a DEFERRED CONDITIONAL spec — a decision tree keyed on spike-state + NK-table shape
   at first probe — and is BARRED from blocking the supervisor GREEN-PASS** (it cannot be decided at recon
   when the tables only exist once the architecture is built; re: tension #3).
6. **Gate-item-0 (parcel provenance):** NK-v02.27 md5 `61c176e90b6365e84e5c660d703e56af` (105280 B) +
   canonical ROM md5 `66210b4f71df8a580eb175f52b9d0f88` re-verified.

**PASS taxonomy (the floor):**
- **GREEN-PASS** = Q-S3.1/.2 pinned affirmatively AND Q-S3.3 yields a single hostable architecture (or a
  ≤2 shortlist both proven hostable, M3-discriminator named) → S3-impl architecture is unblocked.
  **M4 — the supervisor architecture verdict GREEN-PASSes INDEPENDENTLY of the SHM-arena spike.** The spike
  is now **PROVEN** (`test_shm_arena_spike`, commit `acd89dce`) → record spike-construction = GREEN. The
  **live-MMU substrate is a SEPARATELY-TRACKED residue** that gates S3-impl START but does NOT muddy the
  architecture verdict. (Resolves tension #4: the supervisor pick GREEN-PASSes on its own; the substrate is
  tracked alongside, not folded into, the architecture grade.)
- **RESIDUE-PASS** = ≥1 question closed with a conservative residue → a finding, not a green light: BLOCKS
  S3-impl, triggers re-band.
- **INFEASIBILITY-VERDICT** = Q-S3.3's infeasibility test fires on **positive evidence that no concrete
  seam set exists** for any surviving candidate (NOT from budget-exhaustion — that yields
  INFEASIBILITY-UNKNOWN; M2) → a reportable program-level blocker, recorded with evidence, NOT a license to
  forge.

**DIAGNOSTIC (never a gate):** which SS pieces land REPLACED vs YIELDED; the Execute68k-pair disposition;
counter-design notes; how far a probe NK boot is expected to get. Unexpected live markers ESCALATED.

## Authoritative inputs

| Doc / source | What it fixes for this Task-0 |
|---|---|
| `docs/superpowers/plans/2026-06-14-ss-m18-trampoline-lle-program.md` — "Stage 3" | The S3 charter (Q-S3.1/.2/.3, the 4 candidates, gates G3.a–d, env-gate, file ownership, B4 `ppc-cpu.cpp:1991`, the `sc` sub-wall). External doc. Route A + shape NOT relitigated. |
| `docs/planning/newsheep/MMU-NANOKERNEL-INSEPARABILITY.md` (CANONICAL) | Why the live MMU co-lands with S3; the standing guard; the SHM-arena spike folds as item-0. |
| `docs/planning/newsheep/FINDINGS-s3-nk-ownership.md` (this recon) | DECISIVE: NK = sole resident owner of DEC/exceptions/68k-dispatch, never yields → CO-OWN/yield-fully scored OUT; the SS Execute68k pair is an SS synthetic. |
| `docs/planning/newsheep/FINDINGS-s3-ss-reusemap.md` (this recon) | The SS-side KEEP/TOGGLE/RETIRE/YIELD map + the 3 collision points + the env-gate census (`SS_M18_NK_SUPERVISOR` to create; the existing arm at `ppc-cpu.cpp:1991`). |
| `docs/planning/newsheep/FINDINGS-trampoline-re.md` Q1 | SETTLED: resident supervisor, no handoff. |
| `docs/archive/2026-06/planning/{MMU-NANOKERNEL-MP-PLAN,HANDOFF-NEWWORLD-SUPERVISOR-MMU}.md` | Path A reuse/obstacle map + the `sc` double-increment (`HANDOFF:299-313`). Parked = study. |
| `docs/planning/M13-FINDINGS-interrupt-delivery.md` (+ RETRACTION) | NewWorld 68k interrupt DELIVERY WORKS — do NOT re-chase. |
| `docs/MILESTONE-WORKFLOW.md` §2/§4/§6/§6c | The machine; falsifiable gates; env-on-first/flip-LAST/revert-on-red; kill-switch; QEMU-before-RE + address-oracle LAW. |

## Codebase facts (carried; re-verify at recon AND impl — drift ±1–5)

- **Supervisor surface ALREADY newworld-gated** — `check_spcflags` arm `ppc-cpu.cpp:1991` (B4). S3 toggles
  an existing arm.
- **`exc_core.cpp`** — `ExcEnter:12`/`ExcRfi:104`; **PEM masks = LAW**; they SURVIVE as hardware vectoring.
  `ExcDeliveryDecision:58` survives (pure gate). S3 retires SS's *handler substitution*, not the math.
  **Guard (minor a): retirement happens at the CALL-SITE/gate** (`ppc-cpu.cpp:2027` hook,
  `g_exc_entry_table` bypass) — **NEVER inside `exc_core.cpp` mask math.** Touching a PEM-mask line trips
  the re-plan stop-rule on the FIRST edit.
- **`sheepshaver_glue.cpp`** (= `src/kpx_cpu/sheepshaver_glue.cpp`): `g_exc_entry_table:142` (RETIRE/gate);
  `execute_68k:1461` (Q-S3.1); Execute68k-pair `:1504-1505` (written `:3140-3143`); `interrupt():1002`;
  `deliver_pending_dec_exception():1122` (RETIRE — the synthetic supervisor body); EXT/DEC ExcEnter
  `:1254/:1342`; forge `:2806/2804/2918`; `ppc_cpu` static `:1683` (KEEP); `reset_supervisor_for_test()`
  TEST-ONLY `:560/:566` (no `machine/reset_supervisor*` file — B2).
- **The Execute68k pair** points at SS's OWN JIT emulator (`0x50480000/0x50460000`), NOT the ROM's
  `0x50360000`. The parcel has no ref to either KDP slot or `0x50360000` → the pair is an SS synthetic
  (Q-S3.1 load-bearing).
- **`sc` is a no-op in SS** — `execute_syscall`→`execute_illegal`→PC+=8 (`HANDOFF:299-313`); the NK's real
  SC handler is `0x50314ac0` (`-1/-2/-3` fast-traps). The NK idle `sc` (r0=0x2e) dead-loops in SS today.
- **Scheduler (YIELD candidates):** `virt_clock.cpp:108 VirtClockDECPending` (advisory under NK),
  `:71 VirtClockWriteDEC`, `event_sched.cpp:66 process_timers`. The M0–M13 device models survive beneath.
- **`ppc-cpu.cpp` S3 hook:** `check_spcflags()` is at **`:1972`** (the newworld arm at **`:1991`**);
  `SheepExcDeliverPending():1687` (the hook to retire) lives in **`sheepshaver_glue.cpp`**, NOT
  `ppc-cpu.cpp` — only the call site `:2027` is in `ppc-cpu.cpp`. (Reusemap pinned `:1991`/`:2027`;
  re-verify at impl.)
- **Master gate to CREATE:** `SS_M18_NK_SUPERVISOR` (default OFF) ∧ `MachineProfileIsNewWorld()`.
- **Parcel provenance:** NK md5 `61c176e90b6365e84e5c660d703e56af`; ROM md5 `66210b4f71df8a580eb175f52b9d0f88`.

## Blocking-answer table (which S3-impl task blocks on which Task-0 question)

| S3-impl task (future) | Blocked by | Residue disposition (conservative) |
|---|---|---|
| Retire the Execute68k pair / decide `execute_68k` dead-vs-driven under the gate | **Q-S3.1** | Can't prove dead-or-driven → "68k-dispatch-ownership UNKNOWN → architectural-risk"; RESIDUE-PASS blocks impl. |
| Retire SS handler substitution (`exc_core` KEEP vectoring) + dispose `deliver_pending_dec_exception` | **Q-S3.2** | No clean model → "exception-ownership UNKNOWN"; never edit a PEM-mask LAW line ad-hoc. |
| Quiesce/yield the SS scheduler so NK DEC `0x50313200` is the live rescheduler | **Q-S3.2** | Can't show SS-ticks→0 while NK live → "scheduler-yield UNKNOWN → months". |
| **PICK the architecture** (REPLACE vs thin-shim; CO-OWN/yield-fully scored out) + gate `SS_M18_NK_SUPERVISOR ∧ newworld` | **Q-S3.3** | No hostable arch → **INFEASIBILITY-VERDICT** (program blocker); ≤2 hostable → A/B to impl. |
| Retire the forge (`rom_patches.cpp:716`, `glue:2806/2804/2918`, entry-vector synthesis) under the gate | **Q-S3.3** | Scope unbounded → "restructure-scope UNKNOWN → re-plan". |
| Make NK SC `0x50314ac0` live; retire SS `sc`-as-illegal | **`sc` sub-wall** | Still routed through `execute_illegal` → "sc-delivery-gap → named S3-impl sub-task"; do NOT fix double-increment alone + claim closure. |
| Co-land the live paged MMU (window vs softmmu against NK-installed tables) | **Q-S3.3 + spike (item-0)** | Install can't run under the arch → "live-MMU-UNKNOWN → program-blocking"; spike RED → "substrate UNKNOWN → platform-risk". |
| Whole S3 effort band | **Q-S3.1 ∧ .2 ∧ .3** | Single hostable arch + clean yield ⇒ months; no hostable arch ⇒ INFEASIBILITY. |

## Q-S3.3 — the candidate architectures + the INFEASIBILITY test

> Enumerate AND score each on: (i) **hosts a resident translated-mode PPC supervisor?** (load-bearing),
> (ii) reuses the gated arm (`ppc-cpu.cpp:1991`) vs guts, (iii) live-MMU co-land path, (iv) `sc` path,
> (v) paravirtual-provably-unreachable, (vi) effort band. Cite `ppc-cpu.cpp:1991` for all.

1. **REPLACE** — SS's synthetic supervisor disabled under the gate; the NK owns EXT/SC/DEC/PROGRAM live;
   `execute_68k` dead under newworld (NK reaches 68k via its own ECB + ROM EmulatorCode `0x50360000`);
   ExcEnter/ExcRfi survive as vectoring only. SS = bare PPC core + device models + hardware vectoring. **(The
   NK-ownership finding makes this the front-runner.)**
2. **CO-OWN** — **SCORED OUT** (`FINDINGS-s3-nk-ownership.md`: the NK never delegates; co-residency is
   statically impossible). Recorded scored-out, not silently dropped.
3. **yield-fully** — **SCORED OUT** (Q1: no yield point). Recorded scored-out.
4. **thin-shim** ("SS hosts the NK as a guest supervisor behind a thin shim") — SS provides a thin shim
   adapting the `sc`/ECB/Execute68k seams while the resident NK runs translated; SS keeps the bare core.
   The live shortlist alternative to REPLACE.

> **M3 — REPLACE vs thin-shim has NO falsifiable discriminator at recon via G3.a.** Both satisfy
> SS-scheduler-ticks=0 (G3.a cannot tell them apart). The real discriminator is **static seam-necessity**:
> **thin-shim REQUIRES live `sc`/ECB/Execute68k shim seams** (it interposes at the lone `bctr`@`0x5031a8b8`
> 68k entry); **REPLACE RETIRES those seams**. **A ≤2-shortlist is BARRED from GREEN-PASS unless the static
> discriminator is named AND deferred to a falsifiable S3-impl gate; otherwise the shortlist is a
> RESIDUE-PASS, not a GREEN-PASS.** (re: tension #2.)

**The INFEASIBILITY test (mandatory).** Q-S3.3 MUST test "what if NEITHER REPLACE NOR thin-shim hosts a
resident, translated-mode, paged PPC supervisor within budget":
- **Predicate:** for each surviving candidate, is there a concrete seam set (exception vectoring + DEC +
  68k dispatch + `sc` + live MMU install) under which the NK runs resident WITHOUT SS forging an output? If
  NO candidate has one within budget → **INFEASIBILITY-VERDICT**.
- **M2 (decisive) — the VERDICT needs POSITIVE no-seam evidence, never exhaustion.** INFEASIBILITY-VERDICT
  may be emitted ONLY on positive evidence that **no concrete seam set exists** for any surviving candidate.
  **Budget-EXHAUSTION can ONLY produce INFEASIBILITY-UNKNOWN**, never the VERDICT. **Stop-rule: emitting
  INFEASIBILITY-VERDICT from budget-exhaustion rather than positive no-seam evidence = STOP.** This is what
  makes the ≤90 min / ≤2-boot cap responsible (re: tension #1).
- **Budget:** ≤90 min static + ≤2 QEMU boots (runbook below; INSIDE the ≤4 cap) across the surviving
  candidates. At cap and inconclusive → **INFEASIBILITY-UNKNOWN → program-blocking-risk** (conservative),
  NOT INFEASIBILITY-VERDICT and NOT an optimistic default.
- **Disposition if it fires:** a reportable program-level blocker (Route A may need re-pricing), recorded
  with evidence; NOT a license to forge (Stop-rule #1/#3). "INFEASIBLE/months is the answer" is not a
  trigger to relitigate Route A (Stop-rule #9).

## ★ Carried-risk re-aim (ADVERSARY + SPIKE-FAITHFULNESS convergence — IMPORTANT)

> **The INFEASIBILITY test is aimed at the WELL-PINNED thing and AWAY from the real risk.** Both the
> adversary and spike-faithfulness reviewers converged here: supervisor reconciliation (REPLACE-is-clearly-
> hostable, the program's B4) is *already well-pinned* by the NK-ownership finding — the infeasibility menu
> is largely settled. **S3's biggest CARRIED risk is NOT the supervisor menu; it is the live paged-MMU's
> MULTI-ENTRY TRANSACTIONAL ATOMICITY.** The NK's `mtspr`-storm performs multi-BAT table updates as a set;
> the SHM-arena spike (commit `acd89dce`) proved only the **SINGLE-remap** primitive (construction), NOT a
> map-a-SET/atomic-per-region-set transaction under live CPU-thread access.
>
> **Design directive (binding for the S3-impl window port):** port the window using the **ATOMIC one-step
> `mach_vm_map(VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE)` per-region form** (spike-proven hole-free), **NOT
> Dolphin's unmap-then-map** sequence (which exposes a transient-unmapped fault window). The spike's OWED
> list **O1–O6** (`docs/superpowers/specs/2026-06-15-ss-m18-s1-shm-arena-spike.md` §5 S5-note + appendix)
> is carried by reference as the live-operation residual.
>
> **Follow-on de-risk (next S3 item):** a **multi-entry-transaction spike** — extend `test_shm_arena_spike`
> to the map-a-SET / atomic-per-region-set case + the mixed-perm 4×4KB fork (spike OWED O4, the real
> Discriminator-A decision). Schedule this as the next S3 platform-risk item, before the live window is
> built.

## Probe-boot runbook (QEMU behavioral, address-oracle LAW; ≤4 boots)

1. `bash SheepShaver/tools/qemu-rig.sh --gdbstub --timeout 90`; drive with `SheepShaver/tools/gdbcli.py`.
2. **Q-S3.2 / G3.a:** bp NK DEC `0x50313200`; observe the NK is the live rescheduler in a genuine mac99
   boot (DEC loop iterating).
3. **Q-S3.1 (the POSITIVE probe owed by G3.c):** bp the lone `bctr`@`0x5031a8b8` (`mtctr r9; bctr`,
   r9 runtime-loaded); observe the NK reaching 68k via its own ECB + ROM EmulatorCode `0x50360000` (NOT the
   SS `[KDP+0x1074/0x1078]` pair). This is the *positive* observation that closes Q-S3.1 — the static
   negative-space (no hardcoded `0x50360000` branch) does NOT close it on its own.
4. **`sc` sub-wall:** observe NK SC `0x50314ac0` servicing a real fast-trap.
5. Record `[QEMU-BEHAVIORAL]`. Residue: unclassifiable in ≤4 boots → "INFEASIBILITY-UNKNOWN →
   program-blocking-risk". **Caveat (S1 G1.e exposure):** the rig may stall before NK MMU-install — if so,
   the architecture pick is owed a probe it cannot get; record that limit (red-team tension #3).

## Budgets (caps + WRITTEN residue fallbacks; kill-switch = function/instruction window)

- **Q-S3.1 (static — SS glue + the NK-ownership finding):** ~45 min. Residue: "68k-dispatch-ownership
  UNKNOWN → architectural-risk."
- **Q-S3.2 (static — `exc_core` + scheduler + NK DEC):** ~45 min. Residue: "exception+scheduler-ownership
  UNKNOWN → architectural-risk."
- **Q-S3.3 (static + ≤2-boot infeasibility test):** ~90 min static + ≤2 boots. Residue:
  "INFEASIBILITY-UNKNOWN → program-blocking-risk."
- **`sc` sub-wall (static):** ~20 min. Residue: "sc-delivery-gap → named S3-impl sub-task."
- **Live-MMU co-land SPEC (static + fold spike state):** ~30 min. Residue: "live-MMU-substrate UNKNOWN →
  platform-risk; band drops a notch."
- **QEMU boots: ≤4 TOTAL is the hard cap (M1).** This is NOT additive to 5. A single gdbstub boot serves
  MANY breakpoints (DEC `0x50313200`, the `bctr`/`0x50360000` 68k dispatch, SC `0x50314ac0`) — one boot,
  many bp. The infeasibility test's ≤2 boots AND the one-iteration rule's ≤1 re-pin boot are both INSIDE
  the 4, not on top of it. More than 4 = Stop-rule violation. Do NOT disasm the whole parcel.

## Gates G3.a–d (S3-IMPL gates restated falsifiable-in-advance)

- **G3.a — probe boot: NK reaches DEC `0x50313200` with SS scheduler structurally inert**
  (SS-scheduler-ticks=0 while NK live). Identity/forge-state FAILS by construction.
- **G3.b — NK exception entries are the live handlers** (parked-PC assertion: EXT `0x50314880`/SC
  `0x50314ac0`/DEC `0x50313200`/PROGRAM `0x50314700`); SS's `g_exc_entry_table` yields under the gate.
- **G3.c — Execute68k-pair disposition resolved** (re-bound to ROM EmulatorCode `0x50360000` + SS
  `execute_68k` dead under the gate, OR proven still-driven; the `glue:1504-1505` live read accounted for).
- **G3.d — paravirtual byte-identical via REAL `make e2e` + multi-run soak** (`SS_E2E_RUNS=N` median±CV%)
  — NOT the inertness substitute (LAW/timing-sensitive). **revert-on-red = BRANCH revert.**

## Env-gate (the program's most dangerous flip)

`SS_M18_NK_SUPERVISOR` (default OFF) ∧ `MachineProfileIsNewWorld()`; selected once at boot (reuse
`ppc-cpu.cpp:1991` precedent). **Transfers the supervisor role — paravirtual must be PROVABLY
unreachable.** **Proof obligation (minor b):** "paravirtual provably unreachable" = `MachineProfileIsNewWorld()==false`
audited at the gate SITE + G3.d real-`make e2e` byte-identity. Acceptance: env-on-first, flip-LAST,
**revert-on-red = BRANCH revert** (the restructure is merged regardless of the flag).

## File ownership (LAW, sole-in-flight — S3-IMPL, NOT this Task-0)

- `machine/exc_core.cpp` (retire handler substitution; KEEP ExcEnter/ExcRfi — PEM masks = LAW).
- `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` (interrupt/`deliver_pending_dec_exception`/`execute_68k`
  seams + Execute68k-pair re-bind/retire; forge `:2806/2804/2918`).
- `machine/event_sched.cpp` + `machine/virt_clock.cpp` (scheduler yield).
- `rom_patches.cpp` (retire entry-vector synthesis + reg-fixup forge `:716`).
- `ppc-cpu.cpp` (the `:1991`/`:2027` hook; `check_spcflags` body at `:1972`).
- **Live-MMU co-land:** whichever DEFERRED S1 plan the chosen arch selects (S3 inherits S1's
  memory-translation surface for the co-land).
- **S3 owns the supervisor+exception+scheduler+forge-retirement surface AND (for the co-land) the
  memory-translation surface; no other stage edits these concurrently.** Depends on S1-mechanism (DONE) +
  S2 landed.
- **Fix budget:** a single ad-hoc fix to a **PEM-mask LAW line** trips **re-plan immediately** (not after a
  second falsification).

## Stop-rule (triggers named in advance; one-iteration mechanics)

1. **#1 — Re-forge a frozen output instead of running the real producer.** STOP (M17 tripwire). S3 RUNS the
   real NK.
2. **#3 — Claim a handoff boundary the recon proved fictional.** Q1 + `FINDINGS-s3-nk-ownership.md`: no
   yield point. CO-OWN/yield-fully are SCORED OUT, not silently assumed.
3. **#2 — Fake the MMU with identity-only when translation is load-bearing.** The live MMU co-lands; don't
   arm/validate against today's forge.
4. **PEM-mask LAW: one ad-hoc fix to an `exc_core.cpp` mask line = re-plan** (on the FIRST fix).
5. **#5 — Unbounded disasm.** Past the window ⇒ STOP, conservative residue. Two predecessors died here.
6. **#6 — QEMU-as-address-oracle.** No QEMU MMIO address as a reference value; derive live addresses from
   the handoff.
7. **Start writing S3 code mid-Task-0.** No `src/**`; the architecture is PICKED here, BUILT in S3-impl.
8. **Fix the `sc` double-increment alone and claim the syscall path works.** Named S3-impl sub-task, not a
   closure (`HANDOFF:311-313`).
9. **#8 — "months/INFEASIBLE is the answer," not a trigger to relitigate Route A.** Coordinator re-prices.
10. **Proceed to S3-impl on a RESIDUE-PASS or INFEASIBILITY-UNKNOWN.** Blocks; coordinator re-bands first.
11. **#M2 — Emit INFEASIBILITY-VERDICT from budget-exhaustion** rather than positive evidence that no
    concrete seam set exists. STOP — exhaustion yields INFEASIBILITY-UNKNOWN only.

**One-iteration rule.** Falsified pin → dated entry → ONE bounded re-pin (≤1 window; ≤1 boot from the ≤4
cap) → resume. SECOND falsification of the same answer ⇒ STOP, re-plan.

## Self-review record (tensions FOR the red team)

Spec coverage: all coordinator inputs consumed (program Stage-3; INSEPARABILITY canonical; the two S3
recon findings; Q1; the parked Path A docs incl. the `sc` double-increment; M13 RETRACTION; both
exemplars; the SHM-arena spike as item-0). Recon + design ONLY. Q1 + co-residency-impossible are SETTLED
inputs (not re-derived); Route A + shape NOT relitigated.

**Tensions FOR the red team:**
1. **Infeasibility budget calibration** — is ≤90 min static + ≤2 boots enough to responsibly call
   INFEASIBLE, or does it risk a false-INFEASIBLE that re-prices Route A prematurely?
2. **REPLACE vs thin-shim falsifiability at recon** — can G3.a (SS-scheduler-ticks=0 while NK live)
   discriminate the two at recon, or is the pick owed a probe boot the rig may not give (S1 G1.e exposure)?
3. **Live-MMU co-land circularity** — is "select window vs softmmu against the NK-installed tables"
   decidable at recon when the tables only exist once the architecture is built? (Likely a conditional
   SPEC pushed to S3-impl.)
4. **SHM-arena spike coupling** — should a RED/UNKNOWN spike force a RESIDUE-PASS even when Q-S3.1/.2/.3 are
   GREEN, or should the supervisor pick GREEN-PASS independently with the substrate a separately-tracked
   residue?
5. **Proving the Execute68k-pair is "dead under the gate"** — Q-S3.1 leans on the parcel's *absence* of a
   `0x50360000` store (a NEGATIVE). Is static-absence + a behavioral boot sound, or does G3.c need a
   positive observation of the NK dispatching 68k via `0x50360000`?

## Red-team record (rev-2)

**Round:** 4 reviewers, pre-implementation. **Verdicts:** PROCESS = GO-WITH-FIXES; TECHNICAL =
GO-WITH-FIXES; ADVERSARY = OPTIMISTIC-BUT-OK; SPIKE-FAITHFULNESS = FAITHFUL-WITH-GAPS. All binding
amendments folded into rev-2 (inline edits + this record).

**PROCESS (4 Majors + minors):**
- **M1 — boot budget non-additivity.** ≤4 boots TOTAL is the hard cap; one gdbstub boot serves many
  breakpoints; the ≤2 infeasibility boots and the ≤1 re-pin reserve are INSIDE the 4. Fixed the
  additive-to-5 ambiguity. → folded in "Budgets" + runbook header.
- **M2 (decisive) — INFEASIBILITY-VERDICT needs positive no-seam evidence.** Budget-EXHAUSTION can ONLY
  produce INFEASIBILITY-UNKNOWN. Added stop-rule: VERDICT-from-exhaustion = STOP. → folded in the
  INFEASIBILITY test + PASS taxonomy + Stop-rules.
- **M3 — REPLACE vs thin-shim has no G3.a discriminator.** Discriminator is static seam-necessity
  (thin-shim needs live `sc`/ECB/Execute68k seams at `bctr`@`0x5031a8b8`; REPLACE retires them). ≤2-shortlist
  is BARRED from GREEN-PASS unless the discriminator is named + deferred to a falsifiable S3-impl gate, else
  RESIDUE-PASS. → folded after the thin-shim candidate + GREEN-PASS taxonomy.
- **M4 — DECOUPLE the SHM-spike from the supervisor GREEN-PASS.** Spike now PROVEN
  (`test_shm_arena_spike`, commit `acd89dce`) → spike-construction = GREEN; supervisor architecture
  (Q-S3.1/.2/.3) GREEN-PASSes INDEPENDENTLY; live-MMU substrate = separately-tracked residue gating
  S3-impl START. → folded in item-0 + GREEN-PASS taxonomy. (Resolves tensions #4/#5.)
- **minors:** (a) PEM-mask KEEP/RETIRE happens at the call-site/gate (`ppc-cpu.cpp:2027` hook,
  `g_exc_entry_table` bypass), NEVER inside `exc_core.cpp` mask math → guard added to the `exc_core`
  codebase fact. (b) Env-gate "paravirtual provably unreachable" proof obligation =
  `MachineProfileIsNewWorld()==false` audited at the gate site + G3.d real-e2e byte-identity → folded in
  Env-gate. (c) Live-MMU criterion 5 → a DEFERRED CONDITIONAL spec (decision tree on spike-state + NK-table
  shape at first probe), barred from blocking GREEN-PASS → folded in PASS item 5 (re: tension #3).
  (d) **Gating-honesty:** Q-S3.1/.2 are largely CLOSED by the 2 committed findings
  (`FINDINGS-s3-nk-ownership.md` + the SS-side glue corroboration); Task-0's MARGINAL remaining work =
  the M3 discriminator + the infeasibility test + the `sc` disposition + the conditional live-MMU spec +
  the ≤4-boot positive corroboration. The Task-0 grade rests on those, not on re-deriving the (settled)
  ownership findings.

**TECHNICAL (line-drift fixes, all applied):**
- `check_spcflags` is at `ppc-cpu.cpp:1972` (the newworld arm at `:1991`) — body text corrected (the
  parenthetical already had `:1991`).
- `SheepExcDeliverPending():1687` lives in **`sheepshaver_glue.cpp`**, NOT `ppc-cpu.cpp` — only the call
  site `:2027` is in `ppc-cpu.cpp`. File label corrected.
- Forge HTAB memset `:2805` → `:2804` (all four occurrences of the forge triple).

**ADVERSARY (OPTIMISTIC-BUT-OK) + SPIKE-FAITHFULNESS (FAITHFUL-WITH-GAPS) — convergent strategic re-aim:**
The infeasibility test is aimed at the well-pinned thing (supervisor reconciliation — REPLACE clearly
hostable, B4) and AWAY from the real risk: the live paged-MMU's **MULTI-ENTRY TRANSACTIONAL ATOMICITY**
(the NK's `mtspr`-storm does multi-BAT updates; the spike proved only the SINGLE-remap primitive). Captured
as the prominent "Carried-risk re-aim" section, with the design directive (atomic one-step
`mach_vm_map(FIXED|OVERWRITE)` per-region, NOT Dolphin unmap-then-map), the spike OWED list O1–O6 carried
by reference, and the follow-on multi-entry-transaction spike (O4) scheduled as the next S3 item.
