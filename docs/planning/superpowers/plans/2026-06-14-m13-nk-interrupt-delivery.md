# M13 (redraft) — NewWorld 68k interrupt delivery: un-gate eager delivery + HLE the registered-handler→DR signal at the coherent EXT fallback `0x50325f00`

> # ✅ M13 CLOSED / KEYSTONE RETRACTED (2026-06-14) — DO NOT EXECUTE Tasks A/C as written.
> The keystone was a **measurement artifact**: native interrupt delivery **already works**
> (`SS_PROBE_68K=0x5000ed0a` matched 8/8 in plain baseline; a genuine DR-built vector-`$64`
> level-1 autovector frame, saved `SR=0x2000`). The delivery work Tasks A/C describe targeted a
> **non-problem**; the Task C `SS_NW_DR_AUTOVEC` HLE was built then **REVERTED** (`d786eaf8`), and
> `SS_M10_CGRP` with it. The real wall is the downstream **`[ALARM]` model-rejection / pre-System
> gate → M14.** Authoritative now: `docs/HANDOFF.md` HEADLINE + `docs/planning/M13-FINDINGS-interrupt-delivery.md`
> §C-pin.7/8. Everything below is retained as the (retracted) process record only — do not act on it.

> **For agentic workers:** REQUIRED SUB-SKILL — use `superpowers:subagent-driven-development`
> (recommended) or `superpowers:executing-plans` to run this task-by-task. Steps use checkbox
> (`- [ ]`) syntax. **Tasks A and C both edit `sheepshaver_glue.cpp` (and A also `main_unix.cpp`);
> serialize them — ONE in flight per shared-file set. Task B is read-only (QEMU oracle) and runs
> in PARALLEL with A.** Task 0 (step-0) gates ALL of A/B/C and is the opening recon.

**Process:** `docs/MILESTONE-WORKFLOW.md` (this plan follows the house template — Task-0 binding
recon with a blocking-answer table, falsifiable env-gated implementation default-OFF, flip-last
acceptance with revert-on-red). Canonical exemplar:
`docs/archive/2026-06/superpowers/plans/2026-06-11-nk-syscall-surface.md`.

---

> ## Rev 2 — Step-0 fold (2026-06-14, BINDING)
> The parallel step-0 experiment returned and **resolves Task 0** (full addendum in
> `M13-FINDINGS-interrupt-delivery.md` "Step-0 recon"):
> - **Q-0a RESOLVED:** the wall is the **idle spin `0x50468ae4`** (≥10000 visits), NOT the MMU wall
>   `0x50326050` (single fly-by, zero fault signatures). Task A/C gate forms take the idle-spin branch.
> - **Q-0b RESOLVED — eager delivery FALSIFIED as the lever.** Forced 60Hz EXT (`SS_M13_EAGER`) drove
>   EXT 7× harder (edges 1→2481, irq_fired 82→592) but the boot did NOT advance: `0x5000ED08` never
>   ran, registration never reached, wall unchanged. **EXT is NOT starved** — the fallback `0x50325f00`
>   already fires ≥10000× in plain baseline.
> - **BINDING amendments:** (a) **Task A is DEMOTED** from "the lever" to a thin *precondition* — just
>   ensure EXT reaches the NK (the `SS_NW_PIC` leg, already true under `SS_NW_PIC=1`); the eager-VIA/dec
>   timer addition is **dropped** (falsified). (b) **Task C is the sole lever** (HLE the NK→DR handoff
>   at `0x50325f00`). (c) Acceptance rung 1 ("boot advances past the wall") is owned by **Task C's**
>   delivery, not Task A. (d) Stop-rule trigger 1 is **re-scoped**: Q-0b=NO does NOT auto-invoke the
>   route-around (EXT flows fine); the route-around rises only if **Task C** delivery to `0x5000ED08`
>   still fails to advance the boot to registration (the deeper-circularity case). (e) The self-sustain
>   rung is now the milestone's central *uncertain* test — step-0 showed eager EXT does NOT break the
>   registration circularity, so whether running the handler does is the open question Task C answers.

## Goal

Get the NewWorld 9.0.1 diagnostic boot **past its pre-driver-load wall** by making a periodic
hardware-modeled interrupt reach the 68k world the way a healthy boot does, so the NK's own CGRP
registration self-sustains (kcall selector 1 at `0x5031b290` fires naturally) and the 68k handler
`0x5000ED08` runs — on to first pixels (`[FB-DIRTY] non_zero_pixels>0`).

**This is the DECIDED model**, not a re-litigation. The strategic fork is closed:
`docs/planning/NANOKERNEL-STRATEGY-DECISION.md` → "DECISION (2026-06-13) — COMPLETE OUR OWN".
We keep SheepShaver + Apple's NanoKernel; we do **not** fork the NK, switch base (DingusPPC is the
wrong OldWorld path), or borrow device models (we already model SCC/VIA/Cuda/OpenPIC). The gap is
**unwired eager interrupt delivery** + the **unmodeled EXT-fallback→DR-autovector handoff** — wiring
+ RE, not silicon.

**Honest PASS/FAIL vs DIAGNOSTIC separation:**
- **PASS/FAIL gates** (falsifiable, this milestone owns): per-task contracts in Tasks A/C + the
  regression invariants (harness 353/353, machine suite ALL PASS, e2e-test, paravirtual byte-identical
  gated-off). The acceptance ladder's rungs are individually falsifiable.
- **DIAGNOSTIC** (recorded, captured as the next frontier, NOT a gate): exactly *how far* past the wall
  the boot advances (driver-load reached? Process Mgr? Finder?), what the next wall is, and the
  non-deterministic boot caveat (a bare post-EXT SIGSEGV is NOT a regression — classify by durable
  markers, LEARNINGS 2026-06-13 "NW frontier boot is non-deterministic").

**Does NOT touch the shipped Mac OS 8.6 boot** (different ROM, doesn't use this path). All new
machinery env-gated default-OFF until acceptance flip-last; gated OFF the baseline is byte-identical.

---

## Authoritative inputs

| Doc | What it fixes for this plan |
|---|---|
| `docs/planning/NANOKERNEL-STRATEGY-DECISION.md` — "DECISION (2026-06-13)" + "Borrow-vs-rebuild evaluation" | THE settled model + the three-step shape this plan implements (un-gate eager delivery → characterize handler→DR signal via QEMU → HLE at `0x50325f00`). The "cheapest validating experiment" = step-0's eager-delivery probe. **Do NOT edit this doc (current input).** |
| `docs/planning/M13-FINDINGS-interrupt-delivery.md` | The verified diagnosis: 3-stage delivery chain (NK EXT consume WORKS → NK→DR handoff MISSING → DR autovector NEVER fires); CGRP base `0x68ffc1c0`, `CGRP+0x20=1` (uninstalled guard `<2`), table `+0x38/+0x3c/+0x44=0`; the 68k side is READY (`[0x64]=0x5000ed08`); paravirtual interrupt-source struct at `*(0x68ffefd0)`; `0x5000ee58` deferred-task pass runs unconditionally after service; the falsified-approaches table. **Do NOT edit (current input).** |
| `docs/MILESTONE-WORKFLOW.md` §2/§6/§6b/§6c | The machine + gate tiers + dispatch economics + **QEMU-oracle-before-static-RE** (Task B is exactly that) + the falsification-tally close-out line. |
| `docs/AGENT-CONTEXT.md` — Constants + Env-gate state + Instruments | Probe-ready absolutes (ECB/MMCB/KDP, NK PIC descriptor `0x68ffefd0`, NK-published exception entries incl. EXT `0x50314880`/`[KDP+0x374]`); the SS_NW_* default map (M7 cluster default-ON; `SS_NW_PIC` default-OFF/HELD; supported configs all-ON or all-OFF); instrument caveats (SS_PROBE_PC PPC-only, SS_PROBE_68K nested-execute-blind, ring read via `tools/ring-walk.py`, `[NW-PROG]`/`nw-northstar` non-determinism). |
| `SheepShaver/src/Unix/main_unix.cpp` — OpenPIC bus wiring (`nw_pic_force` :1382, `SheepExcExtSetPending` :1389, `nw_pic_output_edge` :1461, `SS_NW_PIC` gate :2092ff incl. `OpenPICBindOutput` :2128 + `SS_NW_PIC_FORCE` :2146, `SS_NW_HOST_IRQ` source arm :2318ff, level-edge forward :3042) | The eager-delivery seam Task A un-gates: OpenPIC output edge → `SheepExcExtSetPending` → glue EXT latch. The M7 cluster (`SS_NW_EE_RISER`+`SS_NW_DEC_PUBLISHED`+`SS_NW_HOST_IRQ`) is already newworld-default; `SS_NW_PIC` is the held one. |
| `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` — the EXT fallback context note (:1265, "0x50325f00, registered-handler table NOT installed — W2L-1"); `g_exc_entry_table`/`NW_INTERRUPT_ENTRY_DEFAULT`; the M10 CGRP STUB warning sites; the M7 EE-riser/DEC-published latch (:115ff) | Task C's HLE site (the coherent PPC-supervisor point that fires every interrupt) lives in this file; the falsified STUB warnings here are the "do not retry" guardrails. |
| `SheepShaver/src/rom_patches.cpp` — `patch_68k()`, the M10 CGRP guest-RAM table/STUB (gated `SS_M10_CGRP`, FALSIFIED), the NW-MIRROR cold-start patch (`SS_NW_TRAMPOLINE`) | Where the falsified forged-table path lives (do not reuse); reference only. |
| `docs/archive/2026-06/planning/2026-06-13-m13-rescope-dr-autovector.md` | The DR autovector mechanics: unified slow-path `0x5046d114`, `cr2lt` is the between-instruction take-exception gate (register/context state, NOT a memory latch), vector `$64` frame build → `[0x64]`. **Open RE this plan's Task B/C must pin: where the DR initially sets `cr2lt` from a PPC-side flag (the lever that feeds cr2).** Archived process trail — facts carry, status superseded. |
| `SheepShaver/tools/qemu-rig.sh` + `qemu-mon.py` | Task B oracle (mac99 = the correct NanoKernel/OpenPIC NewWorld-path oracle). **Load-bearing caveat: behavioral/topology oracle only, NEVER an address oracle** (QEMU MacIO BAR0 `0x80000000` ≠ our `0xF3000000`); tag findings `[QEMU-BEHAVIORAL]`. |

## Codebase facts (carried; implementers re-verify sites before editing)

- **`SS_NW_PIC` is default-OFF/HELD** today; its full wiring already exists (main_unix.cpp:2092ff:
  `OpenPICBindOutput(&openpic, nw_pic_output_edge, NULL)` → `SheepExcExtSetPending`). The M7 cluster
  (`SS_NW_EE_RISER`+`SS_NW_DEC_PUBLISHED`+`SS_NW_HOST_IRQ`) is **already newworld-default**. So Task A
  is "un-gate the held PIC leg + add eager VIA/decrementer assertion", not "build delivery from zero".
- **EXT chain stage 1 WORKS:** NK EXT body `0x50314880` (`[KDP+0x374]`) is a PPC `save→rfi`; with no
  CGRP handler it routes EXT to the fallback `0x50325f00`, which records the source in the NK pending-
  bitmask and returns. `irq_fired`/`g_exc_consume_stats.fired` count THIS (NK-level consume), **NOT**
  68k delivery — the real delivery signal is `0x5000ED08` running (`SS_PROBE_68K=0x5000ed08 match`).
- **CGRP diverting gate:** `CGRP+0x20` (a fn-ptr; guard `cmpwi r9,2; blt` treats `<2`=uninstalled;
  ours=1) + table `+0x38/+0x3c/+0x44=0`. CGRP base `0x68ffc1c0`. Registration is **NK kcall selector
  0x01** (gateway `0x5031aca0`, dispatcher `0x5031aed0`, handler `0x5031b250`→`0x5031b290`), a
  **client-driven** service never run by NK cold-init — it requires a real client handler+table; a
  forged table = M10's falsified crash path.
- **The 68k side is READY:** autovectors `[0x64]=0x5000ed08`/`[0x68]=…ed10`/`[0x6c]=…ed18`. Handler
  reads the **software** interrupt-source struct at `*(0x68ffefd0)` (paravirtual — NOT VIA IFR/IER
  hardware). `0x5000ee58` (deferred-task / Time-Manager / VBL pass) runs **unconditionally after
  service** — even a pending-less autovector entry ticks the starved queues.
- **The DR is a recompiler:** steady-state 68k in a code cache `0x17fa0000+`; `cr2lt` is the
  between-instruction interrupt/exception gate, set as **DR saved register/context state by the NK on
  delivery — no externally-pokable memory latch.** Coherent 68k regfile only at the DR
  between-instruction boundary. DR 68k reg map (AGENT-CONTEXT): r8–r15=D0–D7, r16–r22=A0–A6, r1=A7,
  r24=68k PC.
- **The HLE point** `0x50325f00` runs in a **coherent PPC supervisor context** (fires naturally every
  interrupt). This is the one principled place to do what the missing registered handler would:
  translate the pending NK interrupt into the DR's autovector trigger. Distinct from the 5 falsified
  host injections and from M10's crashing forged CGRP table.
- **Constants** (probe-ready): ECB=0x68fff000, MMCB=0x68fff400, KDP=0x68ffe000, NK PIC descriptor
  =`0x68ffefd0` (`+0x28`=pending-bits word, `+0x14`=ptr to level-indexed source table), CGRP base
  =0x68ffc1c0, ROMBase=0x50000000, NK primary 0x5031xxxx, mirror emulator base 0x50460000, DR dispatch
  table 0x50480000; guest→host add NATMEM_OFFSET 0x400000000000.
- **Canonical gate set** ("full gates" below = exactly this, via `tools/gates.sh`):
  `make -C SheepShaver build-ss`; `SS_HARNESS_BATCH=1 make -C SheepShaver test-jit` AND plain
  `make -C SheepShaver test-jit` (both 353/353); `make -C SheepShaver/src/machine test` (ALL PASS);
  `make -C SheepShaver e2e-test`; paravirtual `make -C SheepShaver e2e` PASS + byte-identical
  (no new [NW-*]/[EXC] lines on paravirtual). Plus the observe line `make -C SheepShaver nw-northstar`
  (report-only verdict, NOT a failing gate).
- **Standing rules** (AGENT-CONTEXT): explicit-path staging (never `git add -A`); struct fields
  appended LAST; clean-rebuild awareness for rom_patches/glue headers; evidence tags
  [RAW-ROM]/[PATCH]/[STATIC]/[PROBE✓]/[QEMU-BEHAVIORAL]; SS_PROBE_PC limit 8/run; `SS_JIT_WATCH_ADDR`
  is hex + needs `SS_JIT_TRACE_RING=1`; one-iteration falsification rule; never global pkill (slot
  boots only); `-F-` heredoc commits, no backticks.

---

## Task 0 / step-0: pin the wall + the eager-delivery experiment (BINDING — gates Tasks A/B/C)

> **A PARALLEL agent is running this experiment NOW.** This plan is written to **consume** step-0's
> result, not to assume it. The plan holds whether the wall turns out to be the MMU fault
> (`~0x50326050`) OR the tick-starved idle spin (DR interpreter `0x50468ae4`). Where the two outcomes
> branch, the branch is called out explicitly (Task A's gate, the stop-rule). If the parallel agent
> has already delivered, the coordinator folds its addendum here verbatim and marks Q-0a/Q-0b
> RESOLVED before dispatching A/B/C.

Pin each answer in a written addendum (new section in
`docs/planning/M13-FINDINGS-interrupt-delivery.md` **"Step-0 recon (M13 redraft)"** — additive, does
not edit the existing diagnosis). Budget: static RE + QEMU oracle primary; **≤6 bounded diagnostic
boots total, each ≤60s via `ss-slot-boot.sh`, ≤2 boots per question before its residue status is
decided.** Co-schedule probes (8 PCs/run). Capture-only telemetry commits allowed (full gates).

- [ ] **(Q-0a) PIN THE EXACT STALL WALL — the recon question to settle (DO NOT assume which).**
  The M13 inventory cites an MMU wall `~0x50326050` (`lwbrx` of hardcoded phys `0x200a0`); the
  2026-06-13 convergence cites a tick-starved idle spin at DR interpreter `0x50468ae4` (r24 cycling
  ROM 68k PCs). Both are "pre-driver-load." Method: a default newworld slot-boot with
  `SS_PROBE_PC=0x50326050;0x50325f00` + `SS_PROBE_68K=0x50468ae4` (or r24-flow via
  `SS_DR_R24_RING=1` → `tools/ring-walk.py --r24-flow`) + the `[NW-PROG]` atexit readout. **Deliverable:
  the precise wall PC + which world it is in (PPC MMU fault vs 68k idle spin) + the `[NW-PROG]`
  signature (program_max / dr68k / dec_expiries / irq_fired) — tagged [PROBE✓].**
- [ ] **(Q-0b) THE EAGER-DELIVERY EXPERIMENT (the strategy doc's "cheapest validating experiment").**
  No rebuild: `SS_NW_PIC=1` (+ the already-default M7 cluster) eager VIA-timer slot-boot. **Does
  `0x50325f00` fire repeatedly AND does the idle-spin / `dec_expiries` advance past the Q-0a wall?**
  Probe `SS_PROBE_PC=0x50325f00` (visit count climbs) + re-read the Q-0a wall signature under PIC-on.
  **Deliverable verdict (decides Task A's shape):**
  - **YES (ticks flow, fallback fires repeatedly, boot advances toward the wall):** the eager-delivery
    wiring is sufficient to feed `ExcEnter(EXC_EXTERNAL)`; Task A is the bounded "make it default +
    eager VIA/T2/dec" finish, and Task C (the DR-handoff HLE) is the principled finish line.
  - **NO (no repeated fallback, or boot does not advance):** circularity is deeper — escalate per the
    stop-rule; the ROM/OS-version route-around (sweep) rises in priority (recorded as a residue,
    NOT auto-started).
- [ ] **(Q-0c) The handler→DR signal contract — open RE seed for Task B/C (bound here, full pin in B).**
  From the archived autovector recon: where does the DR initially set `cr2lt` from a PPC-side flag?
  Static-RE bound ≤2 call levels / ≤12 functions around `0x5046d114` (unified slow-path) and the
  `0x50325f00` fallback body; identify the field/flag the DR consumes between instructions (the lever
  that feeds cr2). **Deliverable: the candidate PPC-side signal + its address/field — or a residue
  marking it as Task B's QEMU-oracle question.** [STATIC]/[QEMU-BEHAVIORAL]
- [ ] **Gate (the blocking-answer table) — ALL blocking answers pinned before ANY of A/B/C starts:**

  | Impl task | Blocks on |
  |---|---|
  | **Task A** (un-gate eager delivery) | **Q-0a** (the wall it must advance past) + **Q-0b** (YES verdict — eager delivery feeds EXT; a NO verdict invokes the stop-rule, not Task A) |
  | **Task B** (characterize handler→DR signal) | **Q-0c** (the static seed it deepens with the oracle) |
  | **Task C** (HLE at `0x50325f00`) | **Q-0b YES** + Task B's pinned signal contract |

  A residue on a BLOCKING answer invokes the stop-rule — no improvisation. Commit the addendum.

---

## Task A: un-gate eager interrupt delivery (env-gated `SS_NW_EAGER_TICK`, default OFF)

> Owns `SheepShaver/src/Unix/main_unix.cpp` (+ `sheepshaver_glue.cpp` if the eager assert lives in the
> latch). **Blocks on Q-0a + Q-0b=YES.** Serialize with Task C.

Goal: a periodic tick reaches `ExcEnter(EXC_EXTERNAL)` *before* driver-load, so EXT fires repeatedly
and the boot advances to/past the Q-0a wall — without yet doing the 68k handoff (that's Task C).

- [ ] Land `SS_NW_EAGER_TICK` (default OFF during bring-up; gated off ⇒ baseline byte-identical):
  (a) move `SS_NW_PIC` toward default-on for the newworld profile **under this gate** (do NOT flip
  `SS_NW_PIC`'s own default yet — compose: `SS_NW_EAGER_TICK=1` implies the PIC leg active), reusing
  the existing `OpenPICBindOutput → nw_pic_output_edge → SheepExcExtSetPending` wiring (main_unix.cpp
  :2092–2128); (b) add **eager VIA-T1/T2 + decrementer assertion** so a periodic source actually
  toggles the OpenPIC input edge at a boot-realistic cadence (re-use the M7 `SS_NW_DEC_PUBLISHED`
  cadence facts; `[KDP+0xf2c]` TimebaseSpeed is the DEC-cadence anchor). Honest upgrade documented at
  the site.
- [ ] One loud `[NW-EAGER]` line per arm (cadence, source) under the gate; gated off → no emission.
- [ ] **Probe sub-contract (PASS/FAIL), env-on `SS_NW_EAGER_TICK=1`:**
  (a) `SS_PROBE_PC=0x50325f00` shows the EXT fallback firing **repeatedly** (visit count climbs
  monotonically across the boot — not a one-shot); (b) the NK pending-bitmask at `*(0x68ffefd0)+0x28`
  is observed non-zero after an assert (probe `[0x68ffeff8]`); (c) the Q-0a wall signature **advances**
  (the falsifiable form depends on Q-0a's outcome — pinned at dispatch):
    - if Q-0a = **idle spin** `0x50468ae4`: `dec_expiries` climbs past the stall value and the spin's
      r24 set widens (more ROM 68k PCs visited) — i.e. the starved loop gets ticks.
    - if Q-0a = **MMU wall** `0x50326050`: EXT fallback fires repeatedly *without* re-faulting at the
      wall PC (no new SIGSEGV at `0x50326050`-class `ea`).
  What happens AFTER the wall is DIAGNOSTIC, recorded.
- [ ] **Gated-off A/B (PASS/FAIL):** one boot without the env var reproduces the captured baseline
  byte-identically (same `[NW-PROG]` signature class, same wall, no new `[NW-EAGER]`/[EXC] lines).
- [ ] Gates: full gates + both sub-contracts (+ `nw-northstar` observe line quoted). Commit.

---

## Task B: characterize the registered-handler→DR signal via the QEMU mac99 oracle (read-only)

> Read-only; no owned source files. **Runs in PARALLEL with Task A.** Blocks on Q-0c.

QEMU-oracle-before-static-RE (MILESTONE-WORKFLOW §6c). Pin how a **healthy** boot hands the 68k IPL to
the DR — the contract Task C must HLE. Tag every finding `[QEMU-BEHAVIORAL]`; **NEVER cite a QEMU MMIO
address as a reference value** (BAR0 `0x80000000` ≠ our `0xF3000000`).

- [ ] Boot the 9.0.1 (or 9.2.1, per the strategy doc's oracle note) ROM on `tools/qemu-rig.sh`; capture
  `info qtree`/`info mtree` (device-tree.txt) + the EXT→68k delivery window. From the oracle's known
  timing (ticks ~12s, VBL `$6e4` chain ~8s — well before Finder ~30s), confirm EXT→68k delivery is an
  **early-bringup** capability (refutes the strong-circular fear) and observe the registered handler's
  action: how it sets the DR's between-instruction take-exception state (the `cr2lt` analogue) and what
  it reads from the NK pending-bitmask.
- [ ] **Deliverable (the contract Task C gates against): a written signal spec** — the sequence
  (pending-bitmask read → DR context field(s) set → autovector vector `$64` → `[0x64]=0x5000ed08`),
  expressed as *which DR/PPC context state must be set* at the `0x50325f00`-equivalent point, in
  register/field terms (behavioral, with our addresses from static RE, NOT QEMU addresses).
- [ ] Static cross-check against Q-0c's seed (our `0x5046d114`/`0x50325f00` bodies): reconcile the
  oracle's behavior with our DR's cr2-feed lever. Record divergences at the contract.
- [ ] Gate: the signal spec exists in the M13-FINDINGS addendum, every claim tagged, the cr2-feed lever
  pinned (or escalated as a residue). Commit (docs).

---

## Task C: HLE the handler→DR signal at the coherent EXT fallback `0x50325f00` (env-gated `SS_NW_DR_AUTOVEC`, default OFF)

> Owns `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp`. **Blocks on Q-0b=YES + Task B's signal spec.**
> Serialize with Task A.

At `0x50325f00` (coherent PPC supervisor context, fires every interrupt), do what the missing
registered CGRP handler would: translate the pending NK interrupt into the DR's autovector trigger
(set the DR `cr2lt` autovector state per Task B's spec). **This is distinct from the 5 falsified host
injections and from M10's forged CGRP table** — see the ruled-out section; the STUB warnings in
`sheepshaver_glue.cpp`/`rom_patches.cpp` are the guardrails.

- [ ] Land the HLE under `SS_NW_DR_AUTOVEC` (default OFF; gated off ⇒ `0x50325f00` path byte-identical
  to today — records-and-returns). Hook the coherent fallback point; when the NK pending-bitmask shows
  a deliverable source AND the DR is at a between-instruction boundary, set the DR take-exception
  context (`cr2lt`-equivalent) per Task B's pinned spec so the DR vectors through `[0x64]→0x5000ED08`.
  No forged CGRP table; no host 68k-frame fabrication.
- [ ] One loud `[NW-AUTOVEC]` line per delivery under the gate (pending source, DR PC). Gated off → no
  emission. Misuse hardening: if the boundary precondition is not met, do NOT deliver (the M10 lesson —
  delivering into a cold/clobbered DR state → 0xDEADBEEF SIGTRAP).
- [ ] **Delivery sub-contract (PASS/FAIL), env-on (`SS_NW_EAGER_TICK=1 SS_NW_DR_AUTOVEC=1`):**
  `SS_PROBE_68K=0x5000ed08` shows **≥1 match with no SIGTRAP/0xDEADBEEF** and the run does not regress
  below the Q-0a frontier. (`0x5000ee58` deferred-pass observed running after service is supporting
  diagnostic.)
- [ ] **Self-sustain sub-contract (PASS/FAIL — the milestone's keystone):** after delivery begins,
  NK kcall **selector 1** (registration handler `0x5031b250`→`0x5031b290`) **fires naturally** —
  probe `SS_PROBE_PC=0x5031b290` ≥1 visit AND `CGRP+0x20` (`[0x68ffc1e0]`) transitions to ≥2 with the
  table `+0x38/+0x3c/+0x44` populated (non-zero). This is the falsifiable signal that the boot reached
  driver/interrupt registration on its own — the loop is broken from the inside, not forged.
- [ ] **Gated-off A/B (PASS/FAIL):** boot without the env vars reproduces the Q-0a baseline
  byte-identically.
- [ ] Gates: full gates + the sub-contracts (+ `nw-northstar` observe line). Commit.

---

## Acceptance ladder (flip-last, revert-on-red)

Each rung is individually falsifiable; all gated default-OFF until the final flip.

1. **Boot advances past the Q-0a wall** — Task A probe sub-contract PASS (EXT fallback fires
   repeatedly; wall signature advances per Q-0a's pinned form).
2. **Driver-load reached** — the boot progresses to the driver/interrupt-registration stage
   (`[NW-PROG] program_max` advances past the stall row; DIAGNOSTIC frontier captured if it stops here).
3. **Registration self-sustains** — `0x5031b290` (kcall selector 1) fires naturally; `CGRP+0x20≥2` +
   table populated (Task C self-sustain sub-contract).
4. **Ticks** — `dec_expiries` climbs to the healthy-baseline class; `0x5000ee58` deferred-pass runs
   repeatedly.
5. **`0x5000ED08` runs** — `SS_PROBE_68K=0x5000ed08 match ≥1`, no SIGTRAP (Task C delivery sub-contract).
6. **Pixels** — `[FB-DIRTY] non_zero_pixels>0` (the milestone's north-star DIAGNOSTIC outcome).

**Acceptance procedure:** run the full battery env-on first (`SS_NW_EAGER_TICK=1 SS_NW_DR_AUTOVEC=1`)
+ all per-task sub-contracts + regression invariants (harness 353/353, machine ALL PASS, e2e-test,
paravirtual byte-identical gated-off). **Fix budget:** telemetry freely; ONE small in-scope fix per
falsified contract, full gates re-run. **THEN flip** `SS_NW_EAGER_TICK` + `SS_NW_DR_AUTOVEC` (and
`SS_NW_PIC` if Task A composed it) to newworld-profile default (explicit-"0" opt-out, mirroring the
M7-cluster polarity) as the LAST step; re-run (1)–(6) + invariants with NO env vars. **Any gate
failure post-flip ⇒ revert the flip in-task** (machinery stays env-gated), failure recorded — the
milestone does not ship default-on with red gates.

Honest scoping: rungs 1–5 are this milestone's owned, falsifiable contracts. Rung 6 (pixels) is the
DIAGNOSTIC north-star — if the boot advances but stops short of pixels at a NEW wall, that wall is the
next milestone's named frontier (capture + stop per the stop-rule), and M13 ships on rungs 1–5 with
the frontier captured.

---

## Ruled out — do NOT retry (folded from M13-FINDINGS + the strategy decision)

**Host-side 68k interrupt injection — falsified 5× (do NOT retry; warnings at the STUB sites):**

| Approach | Why it's dead |
|---|---|
| CGRP STUB → DR_WARM (M10/M12) | Enters the DR with clobbered/cold dispatch state → `0xDEADBEEF` SIGTRAP. |
| Host injection at any DR-range PC | EXT is taken in PPC mode; no coherent 68k context exists at that instant. |
| Resume-prologue (`0x5046e1a4`) delivery | `ECB+0x740` holds the DR's PPC-resume ctx, not a 68k regfile; destabilizes the scheduler. |
| DR state-save vectors (slots 2/3/5) | Volatile-only save (r7–r13), not the non-volatile 68k regfile. |
| DR_WARM ROM-patch IRQ check | DR_WARM is a cold trampoline, not the per-instruction loop; no fixed patch point. |
| "Set a pending-IPL memory latch" | The DR's `cr2lt` trigger is register/context state — no externally-pokable memory latch. |
| **Forging the CGRP table / faking the registration kcall** | = M10's path; the table needs client-supplied MMU-validated PPC handlers that don't exist pre-driver → reproduces the intermittent SIGTRAP. Registration must come from the boot advancing naturally (Task C self-sustain rung), not a fabricated call. |

**Strategic options ruled out (NANOKERNEL-STRATEGY-DECISION 2026-06-13):**
- **Fork/reimplement the NanoKernel** — category error (Apple's NK works; reimplementing = multi-year
  bug-for-bug RE).
- **Switch base to DingusPPC** — wrong path: DingusPPC boots Mac OS 9 only on the **OldWorld** G3-Beige
  path (no OpenPIC); NewWorld (our NanoKernel/OpenPIC) is unsupported. Behavioral reference only (cite
  SHA, never PR inbound).
- **Borrow device models** — we already model SCC/VIA/Cuda/OpenPIC; the gap is unwired eager delivery +
  the DR handoff, not absent silicon.
- **Switch off SheepShaver** — Infinite Mac runs 9.x on SheepShaver; switching base regresses the
  proven NewWorld-9.x foundation.

**Route-around held in reserve (NOT this plan's path):** the ROM/OS-version sweep (9.2.1 / 1.1 / 9.0.4)
— promoted only if step-0 Q-0b returns NO (circularity deeper than eager delivery can cure).

---

## Stop-rule (triggers per MILESTONE-WORKFLOW §2 + MACHINE-LAYER-PLAN §9)

1. **Step-0 Q-0b = NO** (eager delivery does not make `0x50325f00` fire repeatedly / the boot does not
   advance): STOP after the addendum; the eager-delivery model is insufficient — re-scope toward the
   ROM/OS-version route-around (do not improvise injection — the 5× falsified class is closed).
2. **A residue on a BLOCKING Task-0 answer** (the blocking-answer table) invokes trigger 1's re-scope,
   not improvisation.
3. **Task C cannot set the DR take-exception state coherently** without fabricating context of the 5×
   falsified class (i.e. the only reachable lever is host frame-injection / forged table): STOP — the
   `0x50325f00` HLE is not, after all, a coherent point for this boot; capture and re-scope (this is
   the tempting-wrong-fix tripwire, named in advance).
4. **Acceptance shows the boot advances but dies at a NEW wall short of pixels:** that is the NEXT
   milestone's named frontier — capture (the P-M4 artifact: `[NW-PROG]` readout + ring tail + term-dump
   baseline) and STOP; ship M13 on rungs 1–5. No staging beyond capture for the new class.

Within tasks — the one-iteration rule: a falsified pinned contract → dated addendum entry → ONE bounded
re-pin boot → resume; a SECOND falsification of the same contract escalates to re-scope.

---

## Self-review record

Spec coverage: built on the DECIDED model (NANOKERNEL-STRATEGY-DECISION "COMPLETE OUR OWN") and the
verified diagnosis (M13-FINDINGS 3-stage chain). The three implementation tasks mirror the strategy
doc's three steps exactly (un-gate eager delivery → characterize handler→DR via QEMU → HLE at
`0x50325f00`). Task 0 frames the wall (MMU vs idle-spin) as the recon question and is written to
consume the parallel step-0 experiment without assuming its outcome — Task A's gate branches on Q-0a,
and a Q-0b=NO verdict routes to the stop-rule rather than forcing the plan forward. Falsifiable gates
throughout (EXT-fallback visit-climb, `CGRP+0x20≥2` + table-populated for self-sustain,
`0x5000ed08` match, byte-identical gated-off A/B); the self-sustain rung is the keystone falsifiable
signal that the loop broke from the inside (not forged — distinguishing this from M10). Env-gated
default-OFF (`SS_NW_EAGER_TICK`, `SS_NW_DR_AUTOVEC`) with flip-last + revert-on-red. The ruled-out
section folds the 5 falsified injections + the strategic options so no agent relitigates them.

**Known tensions flagged FOR the red team:**
1. **The wall identity is unresolved by design** — the plan must hold for both Q-0a outcomes; if the
   parallel step-0 finds a THIRD wall (neither MMU nor idle-spin), Task A's gate form needs re-pinning
   (is the branch-on-Q-0a coverage complete?).
2. **`SS_NW_PIC` composition vs its own held default** — Task A composes PIC-on under
   `SS_NW_EAGER_TICK` rather than flipping `SS_NW_PIC`'s default; is this the right layering, or should
   the milestone flip `SS_NW_PIC` itself (its flip criteria live in DIAGNOSTICS M7 — does this
   milestone satisfy them)?
3. **The cr2-feed lever (Q-0c/Task B) may be a residue** — if neither static RE nor the QEMU oracle
   pins where the DR sets `cr2lt` from a PPC-side flag, Task C is blocked; is the QEMU oracle actually
   able to observe DR-internal CR state, or only the 68k-visible outcome (behavioral-only caveat)?
4. **"Eager VIA-T1/T2 + decrementer" cadence** — boot-realistic cadence is asserted, not pinned; a
   wrong cadence could either starve (too slow) or livelock the NK (too fast). Should Task 0 pin the
   cadence from the QEMU oracle's observed tick rate?
5. **Self-sustain assumes registration is reachable once delivery starts** — the strategy doc's
   "circular, broken by natural advance" thesis; if delivery starts but registration STILL never fires
   (rung 3 fails while rung 5 passes), is that a Task C falsification or evidence the circular thesis
   is wrong (→ stop-rule)?

## Red-team record

*(empty — a red-team round follows this draft; findings fold as rev 2 BINDING amendments)*
