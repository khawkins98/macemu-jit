# M8 — the slot-4 interrupt consumption round trip: a delivered interrupt is CONSUMED — PROGRAM#4 → NK slot-4 service → world-restore tail COMPLETES → 68k via_int chain runs → the armed post is retired on the real path

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking. **Dispatch ONE task at a time — Tasks A/B/C
> share the same files (`sheepshaver_glue.cpp`, `rom_patches.cpp`, possibly
> `main_unix.cpp`); NOTHING in this plan parallelizes.**

**Goal:** M7 shipped delivery: on the env-on test cluster a host edge traverses
EXC_EXTERNAL → NK fallback 0x50325f00 → guest PIC IACK (`src=0x3f vec=0x3f`) → level test
PASS → the NK post writes **0x8001 to 0x68fff070** and ORs the CR arm — and then the boot
LIVELOCKS without the 68k world ever consuming the interrupt (slot5-recon `b3e51b8d`,
three shapes A/B/C). This milestone makes the round trip TERMINATE: after a delivered
interrupt, **(1)** PROGRAM#4 fires (the DR's entry-vector slot-4 "Interrupt" twi at
0x5046e8d0), **(2)** the NK slot-4 service (0x50314660: `mtlr [KDP+0x5b0]; blr`) + the
world-restore tail complete a world switch back to the DR — **no 0x503244e8 /
restart-block livelock ≥10 s**, jDR grows past the delivery with comp unfrozen, **(3)**
the 68k via_int chain runs (probes **0x5000ec52 / 0x5000ef22 / 0x5000bbca** fire), and
**(4)** the armed post (0x8001 at 0x68fff070) is RETIRED on the real consumption path
(a guest writer clears it — zero host writes to per-event state). **RIDER (diagnostic,
the project headline if it lands): Ticks guest-claimed** — watch word **0x16c** (B-2's
corrected LSB word) moving inside a DR-dispatch record window (the `addq.l #1,$16a` at
~0x5000bbc8), NOT the host `HandleInterrupt` keep-set.

**Architecture:** seeds-not-services (the pattern has held ~7 consecutive times; M7's
entire guest-visible fix set was ~6 staged init words + one latch + one fence narrowing).
The NK side of slot 4 already exists and is 3 instructions; the 68k via_int cluster is
patched-present and load-bearing (M7 Task C disposition: "the next milestone's
consumption rail"). The expected fix class is a small number of staged init words / a
restore-ordering correction, NOT new machinery. **Task 0 is a BINDING recon gating Tasks
A..C**, and it owns the named M3-class fork up front (MACHINE-LAYER-PLAN re-score #3):
if the restore tail cycles because of **NK state we never staged** (a scheduler/run-queue
word; the R-II8 junk-queue family), the fix is another seed — in scope; if it cycles
because the **DR↔NK world-switch protocol needs machinery we don't model**, that is
M3-class and the **STOP-RULE fires at the addendum**, not after improvised machinery.
New code is env-gated **`SS_NW_IRQ_CONSUME`** (default OFF; structurally inert off) and
flipped to the newworld default only as Task C's LAST step, revert-on-red. The M7
cluster (`SS_NW_EE_RISER`/`SS_NW_DEC_PUBLISHED`/`SS_NW_HOST_IRQ`) is already default-ON
and is NOT re-gated; `SS_NW_PIC` stays HELD (its flip has its own written criteria —
NON-GOAL here).

---

## Authoritative inputs

| Doc | What it fixes |
|---|---|
| `docs/planning/machine/INTERRUPT-INJECTION-RECON.md` — "R-II10 / slot-5 park recon" (`b3e51b8d`) | THE recon this plan implements: the Q-S5a–S5e answer table — the park is the NK idle task's nap loop (0x50324f48 body, sc 0x2e, slot-5 twi 0x0fff0005 @0x324fec, nap self-loop 0x50319df8 EE-on); slot-4 service = 0x50314660 `mtlr [KDP+0x5b0]; blr` [STATIC]; the THREE post-delivery livelock shapes — **A**: PROGRAM#4 (srr0=5046e8d0 lr=5046c4f4) → restore-tail block **0x503244e8 livelock, 10⁹ visits** [PROBE✓], r10=0x9040 r12=0x5046c4f4 constant, DEC still delivering ~88/s underneath; **B**: EXT mid-DR (restart=0x50465f28) → NO PROGRAM#4 → 68k busy-wait (jDR +100M blocks/s); **C**: ring-slowed boots reach the nap park with the post armed-unpolled; the boot-4 cold ctx dump (r6=0x68fff100=ECB+0x100, +0x5c/+0x9c/+0xe0/+0xfc fields); the NK nap body's designed coupling (`[KDP+0x670]`(=0x00200000) & live-r13-or-`[KDP-0x440]` suppresses napping while posts pend); the Q-S5e task card whose Task-0 questions this plan's Q-C1..C3 are |
| `docs/superpowers/plans/2026-06-12-interrupt-injection.md` — Task 0/A/B/B-2/C results | The delivery-side facts carried: Q-I2 post target 0x68fff070, CR masks `[KDP+0x674]`=0x00e00000 / `[KDP+0x678]`=0xff9fffff, the deferred-post pair `[KDP-0x43c]/[KDP-0x440]` (drained at world-restore 0x324720); Q-I3 68k chain (level-1 @0xec50 → via_int 0xef2c → table[2] → $192 → 60 Hz proc 0xbbb8 → fe6b @0xbbc8 → `addq.l #1,$16a`; **pre-WLSC OP_IRQ returns d0=1** — Ticks++ pre-warm-start-reachable); Q-I4 **pre-WLSC NO host-side retirement** + deassert=`ClearInterruptFlag` at InterruptFlags==0 + the B-2 staged-word table; R-II7 (level-0 default boots), **R-II8** ([KDP+0x910] junk queue, load-bearing for IACK-leg selection — clean zero → queue-empty leg 0x5032613c never IACKs), **R-II9** (SS_PROBE_LINEAR suspect env-on, 2/2 crashes), R-II10; Task C dispositions: the MODE_EMUL_OP arm "fence it in the slot-4 work" + the per-source tripwire-counter split BINDING CONDITION |
| `docs/planning/MACHINE-LAYER-PLAN.md` §9 re-score #3 (CONFIRMED) | The named next M3-class surprise = **shape A's restore-tail root cause** and its fork (unstaged NK state → seed vs unmodeled world-switch protocol → M3-class stop); gate-retirement explicitly DEFERRED until after this task (no retirement churn here) |
| `docs/planning/machine/EE-CHAIN-RECON.md` D-6/D-7 + the trap_return rows (:381, :419) | The tail/riser mechanics: restore tail 0x3244cc `mtlr r12; mtctr r10; mtcrf 0xff,r13; b 0x318000` [PATCH-DIVERGENT: raw NK = SRR0/SRR1+rfi]; the **0x318000 stub** = nest-- + the EE riser (`mfmsr/rlwimi EE from r11/mtmsr`) + `b 0x3244e4` reload region (reload r0,r7–r13 from ctx; `lwz r6,0x18(r1); lwz r1,4(r1); bctr` to CTR=r10); 0x503244e8 was ALSO the DEC-storm HOT-PC (D-6); the reschedule gate (`r7&0x8000` / `[KDP-0x118]` byte → scheduler vs fast restore); XLM_IRQ_NEST drift documented-dead |
| `docs/planning/machine/M6A-ONGOING-ENTRY-DESIGN.md` §1.2 + §2 | Entry-vector table 0x5046e8c0 slot semantics (slot 4 = "Interrupt", exit ptr `[KDP+0x5b0]`); the NK save/restore protocol (save through r6; scheduler restore via ctx+0xfc); sub-KDP occupancy map (AUTHORITATIVE — extend before placing any word) |
| `docs/AGENT-CONTEXT.md` | Post-flip frontier + instrument caveats (current); constants; gate tiers (`tools/gates.sh`); slot protocol |

## Codebase facts (carried; implementers re-verify sites before editing)

- **Baselines (the entry gate's reproduction targets):**
  - **Default boot (no env), post-flip class** (M7 Task C boot 3): cluster armed, DEC
    #1–4 `(2-SPR)` via 0x50313200, **EXT #1** (entry=50314880, level 0 per R-II7),
    PROGRAM#5 srr0=50324fec park, E4-class sc census (16 distinct, 0xffffffff x233 /
    0xfffffffe x17), 7-wide exc tuple, VCLK park-regime (mtspr_dec≈26, dec_expiries≈4,
    7fffffff/ffffffff nap pair). Block counts are NOT part of the class (host-timing
    coupled — Task C nuance).
  - **Env-on TEST cluster (`SS_NW_PIC=1`; riser/published/host-irq now default)**:
    B-2/battery class — watch 68fff070 trips `value=80010000`, `first-iacks: src=0x3f
    vec=0x3f`, lowmem `[0x3f3f]`=1 survives to edge #1, exactly-once
    (`edges=1 consumed=1 deasserts=0 pending=0`), then ONE of the three livelock shapes
    (timing/instrument-dependent: no-ring → A or B; ring-slowed → C/park).
- **Live consumption acceptance REQUIRES the env-on test cluster**: default boots
  deliver EXT with level 0 (R-II7 — the post writes a no-op value), so the consumption
  chain is unreachable on a default boot BY DESIGN until the SS_NW_PIC flip (held,
  non-goal). This milestone's acceptance gates run `SS_NW_PIC=1` + `SS_NW_IRQ_CONSUME=1`;
  the default-boot post-fix expectation (nap park wakes per DEC/EXT, slot-5 nap
  suppression while posts pend — the NK's designed coupling) is a recorded DIAGNOSTIC,
  not a gate.
- **Shape A anatomy** (what Task 0 must explain): one PROGRAM#4 (counters then froze —
  the cycle is NOT re-trapping through the twi); the spin is pure-NK downstream of one
  delivery; 0x503244e8 = the reload region executing 34–71M blocks/s without completing
  a world switch; probe constants r10=0x9040 (an MSR-image value where the bctr resume
  PC is expected — suspicious on its face), r12=0x5046c4f4. The bctr resume target at
  livelock is THE first open question. The livelock-time ctx was never captured
  (slot5-recon budget cap) — `[r6:0x180]` at the livelock is the designed probe.
- **Shape B anatomy**: EXT delivered at restart=0x50465f28 (mid-DR) → the CR arm never
  reached the LIVE DR context (the post ORs `[KDP+0x674]` into a SAVED r13 image; which
  image, and is it the one the next restore loads?) → guest 68k busy-wait. The
  discriminator question is restore ordering, not delivery.
- **Retirement story today**: NOTHING retires the arm in any shape (`edges=1
  deasserts=0`; the once-per-edge latch holds, no second edge possible). Host-side:
  pre-WLSC there is NO retirement by design (Q-I4); `ClearInterruptFlag` deasserts at
  InterruptFlags==0 and its lost-edge re-check re-runs the assert path (M7 pre-flip
  item 3). Guest-side: the 68k chain must clear the pending halfword and the CR arm
  (the `[KDP+0x678]` and-clear composition is the post's OWN level-0 leg — who runs it,
  or what else clears, on the REAL consumption path is Q-C2's deliverable).
- **The fake-poke fence is PERMANENT**: zero host writes to the pending halfword, CR
  bits, or any per-event state. Watches on 0x68fff070 double as fence audits (M7 Task B
  proved zero host writers — that gate is re-asserted here).
- **Instrument rules (BINDING, from the M7 record + AGENT-CONTEXT):** NO
  `SS_PROBE_LINEAR` on env-on boots (R-II9: 2/2 crashes); hold instrument sets constant
  across A/B boots (the frontier class is timing-sensitive — ring-slowed boots park
  [shape C], no-ring boots spin [A/B]; a shape change under an instrument change is NOT
  evidence); watch addresses are HEX no-0x and need `SS_JIT_TRACE_RING=1`; watches are
  change detectors (0x8001→0 IS visible; 0→0 is not); Ticks LSB = watch word **0x16c**
  (0x168 is the high half — Task B's blindness); `SS_JIT_WATCH_DUMPS=8` + a narrowed
  set for late events; `SS_PROBE_68K` is nested-execute-blind (a host `Execute68k()`
  excursion running via_int would NOT hit it — name this in any "0 matches" claim);
  probes can't count — counters (the exc-tuple idiom) for counts.
- **ROM provenance**: `/Users/Shared/macemu/dumps/` MANIFEST — rom901.bin
  md5 d1a267a9 (PATCHED, post-f808a7fb), rom901_inventory.bin md5 7b1378be (RAW).
  `tools/dump-manifest.sh --check` before tagging [RAW-ROM]/[PATCH]; mirror-region
  (0x5046xxxx) facts are [PROBE✓]-only; capstone-M68K mis-decodes fe1f-class words —
  `tools/m68k-dis.py`.
- **Canonical gates**: `tools/gates.sh <inner|task|full>` — read the `GATES <tier>:
  PASS|FAIL` verdicts. Inner per commit; task tier on each task's final commit; full
  tier (incl. paravirtual e2e or the stated structural-inertness substitution) on
  Task C pre- and post-flip. Boot assertions via `ss-slot-boot.sh --expect/--absent` +
  `BOOT-VERDICT`.
- **Standing rules**: slot protocol only (never pkill); explicit-path staging; struct
  fields appended LAST; clean-rebuild awareness for rom_patches/glue headers; any new
  guest word goes through the sub-KDP occupancy map / the verified-zero trampoline
  region discipline first; `-F-` heredoc commits, no backticks; one-iteration rule.
- **Carried follow-on touching this milestone's files**: the `MODE_EMUL_OP` injection
  arm in HandleInterrupt dispatch (M7 disposition 2: redundant-not-dangerous, fence it
  "in the slot-4 consumption work"). In scope ONLY as a separately-committed,
  single-variable Task B/C step IF this milestone reworks that dispatch anyway;
  otherwise re-record as still-named-deferred. Never entangle it with the flip's A/B
  evidence (the M7 lesson, verbatim).

---

## ENTRY GATE — trivially green (everything landed)

The M7 cluster is default-ON (`81d60cc1`), slot5-recon is committed (`b3e51b8d`),
re-score #3 is confirmed. No in-flight prerequisite. The gate is REPRODUCTION:

- [ ] One default boot (no env) reproduces the post-flip default-boot signature class
  (DEC #1–4 `(2-SPR)`, EXT #1, PROGRAM#5 srr0=50324fec park, E4 sc census) —
  `--expect 'EXT delivered #1;;DEC delivered;;srr0=50324fec' --absent 'TRIPWIRE'`,
  BOOT-VERDICT PASS.
- [ ] One env-on test-cluster boot (`SS_NW_PIC=1`, default instruments) reproduces the
  delivery-green/consumption-livelock class: watch-or-log evidence of the 0x8001 post
  + one of shapes A/B/C identified BY ITS SIGNATURE (A: PROGRAM#4 + 0x3244e8-class
  spin, comp frozen, jDR static; B: no PROGRAM#4, jDR flat-out; C: park reached,
  post armed). Record which shape this machine/instrument set produces — it is the
  baseline shape for all A/B comparisons in this plan.
- [ ] Both boots within the Task-0 budget (they double as Task 0 boots 1–2 if probed
  accordingly — co-scheduling encouraged).

---

## Tasks

### Task 0: the consumption recon (BINDING — gates Tasks A..C; static RE + bounded probe boots)

Pin each contract in a written addendum: **INTERRUPT-INJECTION-RECON.md, new section
"Slot-4 consumption recon (M8 Task 0)"**. Budgets: static RE is the primary tool
(capstone PPC BE on the manifest dumps, raw==patched verified per window;
`tools/m68k-dis.py` for the 68k side); **≤8 bounded diagnostic boots total, each ≤60 s,
≤2 boots per question before its residue status is decided**; static windows bounded
(≤2 call levels / ≤12 functions per question — the P-C2 discipline; unbounded
disassembly has killed agents). Co-scheduling mandated; entry-gate boots count if
probed. Capture-only telemetry commits allowed (inner gates).

- [ ] **(Q-C1, BLOCKING for A — the restore-tail livelock root cause, shape A; owns
  re-score #3's named surprise.)** Why does 0x503244e8 cycle without completing the
  world switch? Method, in order: (a) [STATIC] the full restore path from the slot-4
  service's `blr` through the fallback 0x50325f00 exit legs → the reschedule gate
  (`r7&0x8000` / `[KDP-0x118]`) → 0x3244cc tail → 0x318000 stub → reload region —
  name every ctx field read (which ctx? `[KDP+0x65c]`? `[KDP-0x14]`?) and where r10
  (the bctr target) comes from; explain the observed r10=0x9040 (an MSR image in the
  resume-PC slot is either a mis-read of WHICH block iteration the probe sampled, or
  the smoking gun — decide which); (b) one env-on boot with `[r6:0x180]` +
  r1/r6/r7/r10/r12/r13 probes ON the livelock block (logarithmic sampling reaches
  10⁹-visit regimes fine) — capture the LIVELOCK-TIME ctx slot5-recon never got;
  (c) ring window (`SS_RING_WINDOW`/`ring-walk.py --window`) around livelock onset for
  the actual cycle path (what re-enters 0x3244e8: bctr-to-self-region? per-DEC-delivery
  re-entry? the scheduler leg looping?). **Deliverable: the root-cause statement +
  THE FORK VERDICT in writing — (i) unstaged NK state (name the word(s), the seed
  value, who writes it on a real boot — sanctioned-class justification per the B-2
  staged-word table discipline) ⇒ Task A is a seed; or (ii) the world-switch protocol
  needs unmodeled machinery ⇒ STOP-RULE 1 fires here, after the addendum.** Fallback
  if 2 boots inconclusive: shape A's root cause becomes a residue ⇒ blocking ⇒
  stop-rule 3 (re-scope, not improvisation).
- [ ] **(Q-C2, BLOCKING for B — retirement on the real path.)** What retires the CR arm
  and the post halfword when consumption WORKS? [STATIC] both sides: (a) the NK post
  family's own and-clear leg (`and r13,r13,[KDP+0x678]` — level-0 path today; is it
  also the post-service clear?); (b) the 68k via_int chain's interaction with the
  pending halfword (the DR polls 0x68fff070 — find the poll site and the clear site in
  the DR/emulator code [mirror facts [PROBE✓]-only] and in the patched 68k chain
  (glue :812-class pending-word clear, OP_IRQ d0=1 pre-WLSC semantics — the M7 Q-I4
  "pre-WLSC NO retirement" story applies to the HOST side; pin what the GUEST side
  does pre-WLSC)); (c) where `ClearInterruptFlag`'s deassert meets the guest clear
  (the latch must see InterruptFlags==0 for a second edge ever to fire — name the full
  retire→deassert→re-arm cycle). **Deliverable: the retirement chain as a site table
  (PC → action → word), and the falsifiable Task-B predicate: which word returns to
  what value, observed by which watch.** Fallback: if the guest clear site cannot be
  pinned statically in budget, ONE watch boot (68fff070 + the CR-image word) under a
  forced consumption attempt decides; still inconclusive ⇒ residue ⇒ Task B's
  retirement gate downgrades to "watch-evidenced clear by a non-host writer" with the
  site as a recorded diagnostic — NOT blocking-fatal (the round-trip gate stands).
- [ ] **(Q-C3, BLOCKING for B's shape-B arm — the mid-DR delivery discriminator.)** Why
  did shape B's delivery never arm the LIVE CR? [STATIC] the post's r13 selection
  (live CR r13 from-emulator vs the saved image / `[KDP-0x440]` deferred-post mask —
  the slot-5 nap check reads BOTH; does the POST write both?) vs the restore ordering
  (is the OR applied to a ctx image that the in-progress DR never reloads?).
  **Deliverable: the discriminator + whether the fix is ordering (code), a seed, or
  "shape B resolves itself once shape A's restore completes" (plausible: B is A's
  sibling — delivery interrupted the DR, the restore back into the DR is exactly the
  broken tail).** Fallback: if static-only is inconclusive, classify shape B as a
  post-A re-test item: Task B re-runs the shape-B config after Task A and re-pins.
- [ ] **(Q-C4 — R-II8 on this path.)** Is the junk `[KDP+0x910]` queue load-bearing for
  the CONSUMPTION leg (it already selects the IACK leg)? One probe word `[0x68ffe910]`
  riding any env-on boot + [STATIC] the queue-drain reads on the restore/scheduler
  path (the 0x324720 deferred-post drain neighborhood). **Deliverable: verdict —
  inert / load-bearing-as-is / needs a clean seed (if a seed: stop and bring it through
  the Q-C1 fork's sanctioned-class test).** Non-blocking unless Q-C1's answer names it.
- [ ] **Probe pack** (within budget, co-scheduled): [P-1] livelock-block register+ctx
  probe (Q-C1b); [P-2] ring window at livelock onset (Q-C1c); [P-3] watch set
  68fff070 + 0x16c (`SS_JIT_WATCH_DUMPS=8`) on whichever boot attempts consumption;
  [P-4] `[0x68ffe910]` + nap-coupling words `[0x68ffe670]`/`[0x68ffdbc0]`-class reads
  riding boots 1–2. SS_PROBE_68K probes (0x5000ec52:8 etc.) reserved for Task B —
  don't burn Task-0 boots on a chain we know is upstream-blocked.
- [ ] **Gate (the blocking-answer table):** the addendum exists; every answer tagged
  ([STATIC]/[PROBE✓]/[RAW-ROM]/[PATCH]); the blocking map honored — **Task A blocks on
  Q-C1 (root cause + fork verdict, incl. any seed's sanctioned-class justification);
  Task B blocks on Q-C2's predicate + Q-C3's discriminator (or their named fallback
  downgrades); Task C blocks on nothing new** (it consumes A+B). A residue on a
  blocking answer ⇒ stop-rule 3. Commit the addendum.

### Task 0.5 (conditional, coordinator checkpoint): the fork sign-off

- [ ] If Q-C1's verdict is fork-(i) seed-class: record the seed table (word, value,
  real-boot owner, oracle citation) in the addendum and PROCEED — no separate sign-off
  needed (the B-2 staged-word precedent governs). If fork-(ii) or any seed fails the
  sanctioned-class test (it would be per-event state — the fake-poke fence): **STOP;
  this plan terminates at the addendum** and the coordinator re-scopes (M3-class
  machinery is a milestone of its own, red-teamed on its own).

### Task A: the restore-tail fix (env-gated `SS_NW_IRQ_CONSUME`, default OFF) — size S/M, ≤4 boots

- [ ] Implement EXACTLY Q-C1's pinned fix: expected shape = staged init word(s)
  (PatchROM-time/trampoline, occupancy-map-first, verify-zero-first, per the B-2
  staged-word table discipline: each word justified as "what the real init writes",
  oracle/donor SHAs cited at the site) and/or a bounded ordering correction. All new
  lines gated `MachineProfileIsNewWorld() && SS_NW_IRQ_CONSUME`; gated OFF ⇒
  byte-identical baseline. NO host writes to per-event state (fence). NO new service
  bodies (fork-(i) only reaches this task).
- [ ] **World-switch sub-contract (PASS/FAIL), env-on test cluster +
  `SS_NW_IRQ_CONSUME=1`:** after `EXT delivered #1`: PROGRAM#4 fires (slot-4 counter
  `[KDP+0xe50]`-family or the PROGRAM census), AND **no 0x503244e8/restart-block
  livelock ≥10 s** — jDR grows past the delivery, comp unfrozen, the livelock-block
  probe shows bounded visits (not 10⁹-class). The 68k chain firing is Task B's gate,
  NOT this one — Task A owns only "the world switch completes".
- [ ] **Shape-baseline honesty:** the comparison boot pair (gated-on vs gated-off) holds
  the instrument set constant; the gated-off boot reproduces the entry-gate shape.
- [ ] **Gated-off A/B:** default boot AND env-on-test-cluster-without-IRQ_CONSUME both
  reproduce their baseline classes byte-identically (behavior-line set; block counts
  excluded per the Task-C class definition).
- [ ] Gates: task tier + sub-contracts. Risk tier: structural-inertness substitution
  if every new line is gated (state in the commit); paravirtual e2e if any shared
  line is touched. Commit.

### Task B: the consumption round trip + retirement + the Ticks rider — size M, ≤5 boots

- [ ] **Round-trip sub-contract (PASS/FAIL), env-on test cluster + gate on:** one boot,
  default-class instruments + the Q-C2 watch set + `SS_PROBE_68K` armed: after
  `EXT delivered #1` — (a) Task A's world-switch gate re-asserted; (b) **the via_int
  chain probes fire: `SS_PROBE_68K=0x5000ec52:8` AND 0x5000ef22 AND 0x5000bbca ≥1
  match each** (nested-execute blindness named: if 0 matches but Ticks moves with DR
  attribution, investigate an Execute68k excursion before calling FAIL); (c) **the
  post is RETIRED per Q-C2's predicate** — the watch on 68fff070 shows the guest clear
  (0x8001→0-class change) from a guest writer PC, **zero host writers** (fence gate
  re-asserted).
- [ ] **Shape-B arm (per Q-C3):** one boot in the shape-B-producing config (no-ring
  class) — either shape B no longer forms (A's fix subsumed it — record), or apply
  Q-C3's pinned ordering fix (same gate, separate commit) and re-run. If shape B
  persists with a NEW signature: one-iteration rule (dated addendum entry, ONE re-pin
  boot, resume); second falsification ⇒ stop-rule.
- [ ] **Exactly-once/latch invariants:** `edges/consumed/deasserts` now advance past 1
  (retirement → deassert → a later edge re-arms) — pin the observed cycle count as the
  new invariant class; zero TRIPWIRE; deferred_native within the Q-I4(d) bound; DEC
  cadence healthy (≈2.0 mtspr/delivery class, no zero/tiny storm).
- [ ] **THE RIDER (diagnostic, recorded, NOT a gate): Ticks guest-claimed** — watch word
  **0x16c** +1 with DR-window attribution (ring record window = DR dispatch, not
  HandleInterrupt keep-set; the keep-set's host +1s are the known confounder — count
  both classes separately). If it lands, it is the project headline; if not, record
  where the 60 Hz proc chain stops (the next frontier candidate).
- [ ] **Default-boot diagnostic (recorded):** one default boot (gate on post-flip
  preview is NOT available pre-Task-C — run `SS_NW_IRQ_CONSUME=1` only): does the nap
  park behavior change per the NK's designed coupling (slot-5 nap suppression while a
  post pends; DEC/EXT wakes)? Capture the new default-class signature for Task C's
  baseline.
- [ ] **MODE_EMUL_OP arm (conditional, separate commit):** if this task's work touched
  the HandleInterrupt dispatch, land the M7-named fence as its own single-variable
  commit (gated the same way); otherwise record still-deferred in the addendum.
- [ ] Gates: task tier + sub-contracts. Commit (separate commits per the above).

### Task C: acceptance + default flip (flip LAST, revert-on-red) — size S, ≤4 boots

- [ ] **PASS/FAIL battery first, env-on (`SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1`):** (a) full
  gates (tools/gates.sh full); (b) Task A world-switch sub-contract; (c) Task B
  round-trip + retirement + fence gates; (d) M7 invariant carry-over (delivery
  chronology intact, exactly-once arithmetic in its new multi-edge class, DEC cadence
  class, zero TRIPWIRE); (e) gated-off A/B: default boot + env-on-without-IRQ_CONSUME
  byte-identical to their baseline classes.
- [ ] **Fix budget:** at most ONE small in-scope fix iteration per falsified contract,
  full gates re-run after any fix; second falsification of the same contract ⇒
  stop-rule.
- [ ] **THEN flip** `SS_NW_IRQ_CONSUME` to the newworld default (explicit-"0" opt-out,
  SS_NW_SC_SURFACE polarity; supported configs remain all-ON/all-OFF). Re-run the
  battery with NO gate env: the default boot now carries the fix (still level-0
  deliveries — consumption stays test-cluster-only until the SS_NW_PIC flip, restate
  this in the commit); the opt-out boot reproduces today's default byte-identically.
  **Any post-flip red ⇒ revert the flip in the same task (machinery stays env-gated),
  record the failure — no shipping default-on with red gates.**
- [ ] **DIAGNOSTIC OUTCOMES (recorded, not gates):** the post-fix default-boot frontier
  (does the boot leave the PROGRAM#5 park class? what is the new park/progress
  signature?); the SS_NW_PIC flip-criteria delta (consumption working is one of its
  written criteria inputs — update the held-flip row, do NOT flip it); the Ticks
  headline status; the next named frontier, captured with the full census/HB/ring
  baseline per the house convention.
- [ ] Record results: INTERRUPT-INJECTION-RECON.md (R-II10 disposition + the shape
  table's end-state), M3-class fork outcome noted against re-score #3's prediction.
  Commit.

### Task Z: docs close-out — size S

- [ ] DIAGNOSTICS.md: `SS_NW_IRQ_CONSUME` (default, opt-out, cluster relationship,
  the test-cluster acceptance recipe); the multi-edge latch counter class.
- [ ] AGENT-CONTEXT.md: frontier rewrite (the consumption claim flips from "never
  consumed" to its new honest state; stale-claims list updated), gate census 16→17
  (or 16 stays if the gate retired into the cluster — state which).
- [ ] MACHINE-LAYER-PLAN: header + the M7-follow-on row; **re-score #4 trigger check**
  (the named surprise's outcome — fired/resolved/avoided — is the §9 input; flag if
  a re-score is due, drafting per the convention).
- [ ] ROADMAP: this row closed; next named task row added; the SS_NW_PIC flip-criteria
  row updated with the consumption input; the deferred gate-retirement candidates
  un-deferred (re-score #3's coordinator addition expires with this task — flag, do
  not execute).
- [ ] CHANGELOG + LEARNINGS (at minimum: the fork outcome, the retirement-chain
  anatomy, any instrument lesson); cross-tracker grep for stale "post never
  consumed"/"Ticks never guest-claimed"/"slot-4 livelock" claims (historical sections
  stay). Commit.

---

## Stop-rule (triggers per MACHINE-LAYER-PLAN §9)

1. **The M3-class fork (named in advance, re-score #3):** if Task 0 Q-C1 concludes the
   restore tail cycles because the DR↔NK world-switch protocol needs machinery we do
   not model (not a seedable state hole), STOP after the addendum — Task 0.5 terminates
   the plan for coordinator re-scope. Any "seed" that is per-event state is the
   fake-poke by another name and takes this trigger too.
2. If consumption works but the chain dies on the NEXT surface (the 60 Hz proc's
   downstream, a second service class, WLSC-regime divergence), that is the next
   milestone's frontier — capture and stop; record where pre-WLSC behavior diverges
   from the warm-start regime, build nothing for it (non-goal).
3. A residue on a BLOCKING Task-0 answer ⇒ trigger 1's re-scope, not improvisation
   (Q-C2's named downgrade path is the one pre-authorized exception).
4. Within tasks — the one-iteration rule: falsified pinned contract → dated addendum
   entry → ONE bounded re-pin boot → resume; second falsification of the same
   contract → stop (re-plan, not patch-on-patch).

## Non-goals (binding)

- `SS_NW_PIC` default flip (HELD; its criteria gain an input from this milestone but
  the flip is its own decision).
- M5 framebuffer; any visibility work.
- Warm-start (WLSC) regime work beyond RECORDING where pre-WLSC behavior diverges.
- Gate retirement of the pre-M7 default-ON surfaces (explicitly deferred past this
  task by re-score #3's coordinator addition; Task Z merely un-defers the flag).
- Slot-5/nap service work (exists, NK-internal, conformant — slot5-recon Q-S5b/c).

## Self-review record

Inputs consumed: slot5-recon's full answer table + the Q-S5e card (the three shapes, the
slot-4 service anatomy, the nap coupling, the card's three recon questions mapped to
Q-C1/C2/C3), the M7 plan's Task 0/A/B/B-2/C results (post target + CR masks, the 68k
chain + probe list, pre-WLSC retirement story, R-II7/8/9/10, the dispositions naming
this milestone's carried items), re-score #3 (the fork is Task 0's first deliverable and
stop-rule 1; gate-retirement deferral honored), EE-CHAIN D-6/D-7 (tail/riser/reload
mechanics, 0x3244e8's prior life as the DEC-storm HOT-PC). Constraints carried
verbatim: fake-poke fence, seeds-not-services with the fork as its falsification edge,
separate env gate + flip-last + revert-on-red, the full instrument-rule set (R-II9,
constant instrument sets, watch-word and change-blindness rules, nested-execute
blindness), one-iteration rule, slot protocol. The acceptance subtlety (live consumption
requires the env-on test cluster because default boots post level 0 — R-II7) is stated
as a codebase fact so the gates are falsifiable as written. Known tensions flagged for
the red team: (1) the r10=0x9040 anomaly may be probe-sampling artifact vs smoking gun —
Q-C1 must adjudicate, not assume; (2) Q-C2's retirement predicate has a pre-authorized
downgrade path (site-unpinned) — is that too soft? (3) shape B may be subsumed by A's
fix (claimed plausible, not proven); (4) the rider's DR-attribution method (ring-window
class) vs the keep-set confounder; (5) whether `SS_NW_IRQ_CONSUME` should instead join
the existing M7 cluster rather than be a 17th gate.

## Red-team record

*(empty — a red-team round follows this draft; findings to be folded as rev 2 markers)*
