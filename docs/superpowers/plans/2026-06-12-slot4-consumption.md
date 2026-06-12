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
corrected LSB word) moving with GUEST attribution per the rev-2 respecified evidence
triplet (r24≈0xbbca-class watch record + the 0x5000bbca probe coincidence + the host
keep-set counter census — see Task B's rider step), NOT the host `HandleInterrupt`
keep-set.

**Architecture:** seeds-not-services (the pattern has held ~7 consecutive times; M7's
entire guest-visible fix set was ~6 staged init words + one latch + one fence narrowing).
The NK side of slot 4 already exists and is 3 instructions; the 68k via_int cluster is
patched-present and load-bearing (M7 Task C disposition: "the next milestone's
consumption rail"). **[rev 2 / A1]** The expected fix class is NOT only seeds: the
leading shape-A hypothesis (statically confirmed at fold time, see Rev 2 §A1) is that
the livelock is **patch-created non-atomicity in OUR riser stub** — the raw NK exits via
`rfi` (MSR+PC raised ATOMICALLY); our patched tail raises EE via `mtmsr` BEFORE the ctx
reloads and the `bctr`, and `execute_mtmsr` re-raises on the EE 0→1 edge whenever a
source pends (DEC ~88/s), delivering a TORN context (srr0 in the reload region,
r10=composed-MSR scratch 0x9040, r12=LR image 0x5046c4f4) whose restore re-enters the
same window. The expected fix class is therefore a **bounded rfi-atomicity correction in
the riser-cluster code** (defer the EE-edge re-raise past the bctr / move the rise to
the bctr boundary) and/or staged init words — still NO new service bodies, NO new
machinery. **Task 0 is a BINDING recon gating Tasks A..C**, and it owns the named
M3-class fork up front (MACHINE-LAYER-PLAN re-score #3), now THREE-WAY: (i) **unstaged
NK state** (a scheduler/run-queue word; the R-II8 junk-queue family) ⇒ the fix is
another seed — in scope; (iii) **patched-tail non-atomicity** ⇒ an in-scope
riser-cluster code fix (rfi-atomicity emulation); only (ii) the **DR↔NK world-switch
protocol needs machinery we don't model** is M3-class and the **STOP-RULE fires at the
addendum**, not after improvised machinery.
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
  a world switch; probe constants r10=0x9040, r12=0x5046c4f4. **[rev 2 / A3 — corrected
  framing]**: r10 at 0x3244e8 is NEVER the resume PC by construction — the patched tail
  moved it to CTR (`mtctr r10` at 0x3244d8) and the 0x318000 stub then clobbers r10
  twice (`lwz r10,XLM_IRQ_NEST`, then the riser's `mfmsr r10; rlwimi r10,r11; mtmsr
  r10`). r10=0x9040 is exactly the riser's composed-MSR scratch (EE=0x8000 | 0x1040) —
  statically explained, NOT an anomaly to adjudicate. CTR is unprobeable: pin the bctr
  target via the **ctx SRR0-image slot (+0xa4, verify the offset in Q-C1a) inside the
  already-planned `[r6:0x180]` probe**. The livelock-time ctx was never captured
  (slot5-recon budget cap) — `[r6:0x180]` at the livelock is the designed probe; its
  job is now CONFIRMING the torn-ctx mechanism live (srr0-image in the reload region),
  not open-ended root-causing.
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
  **[rev 2 / A2 — VERIFIED at fold time]: rom901.bin PREDATES the 81d60cc1 cluster
  flip — its 0x318000 stub is the 4-word riser-LESS shape (`81402818 394affff 91402818
  4800c4d8`, no mfmsr/rlwimi/mtmsr). Any Q-C1 static walk of the tail/stub MUST use
  `rom_patches.cpp:2522ff` as the stub authority (the 7-word default-ON shape), or
  re-dump from a current default boot and re-baseline the MANIFEST. `/tmp/rom901.bin`
  is the retired pre-f808a7fb image (md5 e432df64) — never use it.**
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

- [x] One default boot (no env) reproduces the post-flip default-boot signature class
  (DEC #1–4 `(2-SPR)`, EXT #1, PROGRAM#5 srr0=50324fec park, E4 sc census) —
  `--expect 'EXT delivered #1;;DEC delivered;;srr0=50324fec' --absent 'TRIPWIRE'`,
  BOOT-VERDICT PASS.
- [x] One env-on test-cluster boot (`SS_NW_PIC=1`, default instruments) reproduces the
  delivery-green/consumption-livelock class: watch-or-log evidence of the 0x8001 post
  + one of shapes A/B/C identified BY ITS SIGNATURE (A: PROGRAM#4 + 0x3244e8-class
  spin, comp frozen, jDR static; B: no PROGRAM#4, jDR flat-out; C: park reached,
  post armed). **[rev 2 / B6]** Pin the verdict as grep targets, not log reading:
  `--expect 'first-iacks: src=0x3f vec=0x3f;;EXT delivered #1' --absent 'TRIPWIRE'`
  + BOOT-VERDICT PASS, with the shape classifier recorded as the grep used
  (A: `PROGRAM delivered #4`-class line present + the livelock-block probe/HB
  signature; B: absent; C: `srr0=50324fec`-class park line). Record which shape this
  machine/instrument set produces — it is the baseline shape for all A/B comparisons
  in this plan. **[rev 2 / B3]** If the entry baseline shape is C (ring-slowed class),
  Task B's shape-B arm needs a config-discovery boot to find a B-producing config —
  that boot comes out of Task B's ≤5-boot cap; state it in Task B's accounting.
- [x] Both boots within the Task-0 budget (they double as Task 0 boots 1–2 if probed
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

- [x] **(Q-C1, BLOCKING for A — the restore-tail livelock root cause, shape A; owns
  re-score #3's named surprise.)** Why does 0x503244e8 cycle without completing the
  world switch? **[rev 2 / A1] The leading hypothesis is PINNED and statically
  confirmed (Rev 2 §A1): the riser's mtmsr fires the EE-edge re-raise MID-TAIL** —
  `execute_mtmsr` (ppc-execute.cpp:1432-1439) triggers on the EE 0→1 edge with any
  pending source; the stub's mtmsr precedes the ctx reloads (r10/r11 at
  0x3244fc/0x324500 from r6+0x154/+0x15c) and the bctr, so with DEC pending ~88/s
  each restore pass is interrupted mid-reload, saving a TORN ctx (srr0 in the reload
  region, r10=composed-MSR scratch 0x9040, r12=LR image 0x5046c4f4) whose restore
  re-enters the same window — the 0x3244e4↔0x3244e8 self-loop. Raw rfi is atomic
  (MSR+PC together); this window is patch-created. Task 0's job is the CHEAP LIVE
  CONFIRMATION, not open-ended discovery. Method, in order: (a) [STATIC] the full
  restore path from the slot-4 service's `blr` through the fallback 0x50325f00 exit
  legs → the reschedule gate (`r7&0x8000` / `[KDP-0x118]`) → 0x3244cc tail → 0x318000
  stub → reload region — **stub authority = `rom_patches.cpp:2522ff` (the 7-word
  default-ON shape), NOT rom901.bin (riser-less, A2)**; name every ctx field read
  (which ctx? `[KDP+0x65c]`? `[KDP-0x14]`?), pin the ctx SRR0-image offset (+0xa4
  expected) and where CTR (the bctr target) comes from — r10 at the livelock block is
  the riser scratch by construction (A3), not the resume PC; (b) one env-on boot with
  `[r6:0x180]` + r1/r6/r7/r10/r12/r13 probes ON the livelock block (logarithmic
  sampling reaches 10⁹-visit regimes fine) — capture the LIVELOCK-TIME ctx
  slot5-recon never got; **CONFIRMATION predicate: the ctx srr0-image slot holds a
  reload-region/tail PC** (torn ctx) — if instead it holds a DR/foreign PC, the
  torn-ctx hypothesis is falsified and the fork re-opens; (c) **[rev 2 / A4+B3:
  DEMOTED to optional/confirmatory]** — the ring changes the shape (ring ⇒ C, the
  instrument rule's own statement), so a ring window at livelock onset is
  self-defeating as primary evidence; the cycle path is statically derivable. If run
  at all: a pre-authorized `SS_RING_WINDOW`-minimized NAMED config, its shape RECORDED
  (not A/B-compared against no-ring boots). **Deliverable: the root-cause statement +
  THE FORK VERDICT in writing — (i) unstaged NK state (name the word(s), the seed
  value, who writes it on a real boot — sanctioned-class justification per the B-2
  staged-word table discipline) ⇒ Task A is a seed; (iii) patched-tail non-atomicity
  confirmed ⇒ Task A is the rfi-atomicity fix in the riser-cluster code (defer the
  EE-edge re-raise past the bctr / move the rise to the bctr boundary — bounded code,
  no new service bodies); or (ii) the world-switch protocol needs unmodeled machinery
  ⇒ STOP-RULE 1 fires here, after the addendum.** Fallback if 2 boots inconclusive:
  shape A's root cause becomes a residue ⇒ blocking ⇒ stop-rule 3 (re-scope, not
  improvisation).
- [x] **(Q-C2, BLOCKING for B — retirement on the real path.)** What retires the CR arm
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
  **[rev 2 / B1] The downgrade is HARDENED — it requires all three:** (a) the
  watch-attributed writer-PC falls in a PINNED family (the via_int 0x5000exxx/
  0x5000bbxx chain, or the NK post family's and-clear leg — families ENUMERATED in the
  Task-0 addendum, not post-hoc); (b) temporal ordering: the clear lands AFTER the
  via_int probes fire (by ring record number / probe visit order); (c) the multi-edge
  invariant (Task B's `edges/consumed/deasserts` advancing past 1) is bound to the
  downgrade explicitly — a "clear" that does not unlock a second edge does not count.
- [x] **(Q-C3, BLOCKING for B's shape-B arm — the mid-DR delivery discriminator.)** Why
  did shape B's delivery never arm the LIVE CR? [STATIC] the post's r13 selection
  (live CR r13 from-emulator vs the saved image / `[KDP-0x440]` deferred-post mask —
  the slot-5 nap check reads BOTH; does the POST write both?) vs the restore ordering
  (is the OR applied to a ctx image that the in-progress DR never reloads?).
  **Deliverable: the discriminator + whether the fix is ordering (code), a seed, or
  "shape B resolves itself once shape A's restore completes".** **[rev 2 / A6] The
  "B is subsumed by A" framing is WEAKENED, not load-bearing: in shape B the tail
  COMPLETED (no PROGRAM#4; jDR flat-out at +100M blocks/s — the restore back into the
  DR worked). B's defect is the CR-arm landing in a NEVER-RELOADED r13 image. So the
  static r13-image selection analysis here is the PRIMARY deliverable, not a
  contingency.** Fallback: if static-only is inconclusive, classify shape B as a
  post-A re-test item: Task B re-runs the shape-B config after Task A and re-pins —
  but expect "B no longer forms" to FAIL (see Task B's shape-B arm).
- [x] **(Q-C4 — R-II8 on this path.)** Is the junk `[KDP+0x910]` queue load-bearing for
  the CONSUMPTION leg (it already selects the IACK leg)? One probe word `[0x68ffe910]`
  riding any env-on boot + [STATIC] the queue-drain reads on the restore/scheduler
  path (the 0x324720 deferred-post drain neighborhood). **Deliverable: verdict —
  inert / load-bearing-as-is / needs a clean seed (if a seed: stop and bring it through
  the Q-C1 fork's sanctioned-class test).** Non-blocking unless Q-C1's answer names it.
- [x] **Probe pack** (within budget, co-scheduled): [P-1] livelock-block register+ctx
  probe (Q-C1b — includes the ctx srr0-image slot, the torn-ctx confirmation field);
  [P-2] ring window at livelock onset (Q-C1c — OPTIONAL/confirmatory per rev 2 A4,
  named-config only); [P-3] watch set
  68fff070 + 0x16c (`SS_JIT_WATCH_DUMPS=8`) on whichever boot attempts consumption;
  [P-4] `[0x68ffe910]` + nap-coupling words `[0x68ffe670]`/`[0x68ffdbc0]`-class reads
  riding boots 1–2. SS_PROBE_68K probes (0x5000ec52:8 etc.) reserved for Task B —
  don't burn Task-0 boots on a chain we know is upstream-blocked.
- [x] **Gate (the blocking-answer table):** the addendum exists; every answer tagged
  ([STATIC]/[PROBE✓]/[RAW-ROM]/[PATCH]); the blocking map honored — **Task A blocks on
  Q-C1 (root cause + fork verdict, incl. any seed's sanctioned-class justification);
  Task B blocks on Q-C2's predicate + Q-C3's discriminator (or their named fallback
  downgrades); Task C blocks on nothing new** (it consumes A+B). A residue on a
  blocking answer ⇒ stop-rule 3. Commit the addendum.

### Task 0.5 (conditional, coordinator checkpoint): the fork sign-off

- [ ] **[rev 2 / B2]** If Q-C1's verdict is fork-(i) seed-class or fork-(iii)
  atomicity-fix-class: record the seed table (word, value, real-boot owner, oracle
  citation) / the fix-shape table (mechanism, code site, why it restores rfi
  atomicity, why no new machinery) in the addendum, and **PROCEED requires a one-line
  coordinator ACK of that table** (async, cheap — post the table, get the ACK, no
  meeting). Only fork-(ii) STOPs: **this plan terminates at the addendum** and the
  coordinator re-scopes (M3-class machinery is a milestone of its own, red-teamed on
  its own). Any "seed" that is per-event state fails the sanctioned-class test (the
  fake-poke fence) and takes the fork-(ii) STOP too.
- [ ] **[rev 2 / B2] Middle case (escalation, per-fork):** if a fork-(i) seed (or a
  fork-(iii) fix) fails Task A's world-switch sub-contract and ONE re-pin (the
  one-iteration rule) also fails, that escalates to stop-rule 1 — per FORK-BRANCH,
  not per individual seed/word (no seed-shopping).
- [ ] **[rev 2 / B2] Termination clause:** a Task-0.5 STOP includes a mini-Z — the
  addendum committed, the ROADMAP row updated to the stopped state, the re-score #4
  trigger flagged, and an AGENT-CONTEXT frontier note (no silent abandonment).

### Task A: the restore-tail fix (env-gated `SS_NW_IRQ_CONSUME`, default OFF) — size S/M, ≤4 boots

- [x] Implement EXACTLY Q-C1's pinned fix. **[rev 2 / A1 — corrected expected shape]:**
  per the fork verdict, EITHER fork-(iii) the **rfi-atomicity correction in the
  riser-cluster code** (the LIKELY shape: defer the EE-edge re-raise past the bctr /
  move the rise to the bctr boundary, so MSR.EE and the resume PC become effectively
  atomic as the raw rfi was — bounded edit to the stub/`execute_mtmsr` re-raise
  predicate, oracle = the raw NK rfi semantics cited at the site), OR fork-(i) staged
  init word(s) (PatchROM-time/trampoline, occupancy-map-first, verify-zero-first, per
  the B-2 staged-word table discipline: each word justified as "what the real init
  writes", oracle/donor SHAs cited at the site), and/or a bounded ordering correction.
  All new lines gated `MachineProfileIsNewWorld() && SS_NW_IRQ_CONSUME`; gated OFF ⇒
  byte-identical baseline. NO host writes to per-event state (fence). **NO new service
  bodies stands** (only fork-(i)/(iii) reach this task). **[rev 2 / A7] Gate-coherence
  note:** if the fix edits the riser stub or the mtmsr re-raise path, the combination
  `SS_NW_IRQ_CONSUME=1` + `SS_NW_EE_RISER=0` is UNDEFINED (state consumption requires
  the riser) — implement it as riser-conditional (consume code inert when the riser is
  opted out), record that, and carry the cluster-join question to Task C's deliberate
  decision.
- [x] **World-switch sub-contract (PASS/FAIL), env-on test cluster +
  `SS_NW_IRQ_CONSUME=1`:** after `EXT delivered #1`: PROGRAM#4 fires (slot-4 counter
  `[KDP+0xe50]`-family or the PROGRAM census), AND **no 0x503244e8/restart-block
  livelock ≥10 s** — jDR grows past the delivery, comp unfrozen, the livelock-block
  probe shows bounded visits (not 10⁹-class). The 68k chain firing is Task B's gate,
  NOT this one — Task A owns only "the world switch completes".
- [x] **Shape-baseline honesty:** the comparison boot pair (gated-on vs gated-off) holds
  the instrument set constant; the gated-off boot reproduces the entry-gate shape.
- [x] **Gated-off A/B:** default boot AND env-on-test-cluster-without-IRQ_CONSUME both
  reproduce their baseline classes byte-identically (behavior-line set; block counts
  excluded per the Task-C class definition).
- [x] Gates: task tier + sub-contracts. Risk tier: structural-inertness substitution
  if every new line is gated (state in the commit); paravirtual e2e if any shared
  line is touched. Commit.

#### Task A addendum (2026-06-12, one-iteration rule): the HANDLE re-arm hold FALSIFIED — re-pinned to the passive latch

The first implementation (commit `42ce3e0e`: latch at mtmsr + trigger_interrupt + a
DEFER_NATIVE-idiom HANDLE re-arm hold in check_spcflags) **starved the guest**: the JIT
exits on non-empty spcflags at block entry BEFORE executing, so a permanently re-armed
HANDLE is a pure dispatcher spin. Boot s4ta-b1r (rundir 20260612-054206.53665):
`[IRQ-CONSUME] deferred=1 held=1122321012 fired=0 latch=1`, HOT-PC frozen at 0x50318018
for 8 consecutive heartbeats with IDENTICAL registers (r10=r11=0x9040 = the defer pass's
pre-reload values — DEC#4's SRR1 image, proving NO reload ever executed post-defer), comp
frozen, jDR static. The "fire at an out-of-window boundary" predicate can never observe an
out-of-window PC if the hold itself prevents the guest from reaching one.

**ONE re-pin (commit with this addendum): the passive latch.** Defer = latch WITHOUT
trigger_interrupt (no spcflags set → the guest runs the reload+bctr unmolested → no torn
boundary exists at all); in check_spcflags' HANDLE arm, gate-on, ANY in-window poll is
suppressed (latch set, NO flags re-armed — the guest must run); the latched edge fires at
the next natural kick's poll (DEC cadence / host edge — TriggerInterrupt is "the
DEC-expiry idiom", main_unix.cpp:1447, so a kick is guaranteed) once the entry PC is
outside the windows. Deferred-delivery latency is bounded by the next natural kick;
pending sources are level-held (EXT) or latched (DEC), so a suppressed kick loses nothing
but latency. Re-pin boot s4ta-b4r (20260612-055910.70595): GREEN — see results below.
Second falsification would have stopped the task; none occurred.

#### Task A results (2026-06-12) — commits `42ce3e0e` (impl) + re-pin; boots 4 counted of ≤4 (+3 crash boots disclosed)

| Gate | Verdict | Evidence (boot s4ta-b4r unless noted) |
|---|---|---|
| World switch completes / no livelock ≥10s | **PASS** | EXT delivered #1 at restart=**500ed8ec** (a REAL out-of-window PC) → edge deasserted (`EXT pending deasserted (edge #2)`) → boot progresses to **PROGRAM#5 srr0=50324fec nap park** + the healthy park VCLK regime (mtspr_dec=26, dec_expiries=4, 7fffffff/ffffffff nap pair). No 0x3244e4-region livelock: the livelock-block probe shows visit=1 ONLY (bounded, vs 10⁹ pre-fix), no ALARM/STALL/HOT-PC. |
| PROGRAM#4 (slot-4 twi) fires | **NOT YET — moved to Task B by chronology** | Post-fix the EXT delivery lands at a clean PC outside the DR (500ed8ec ROM code; gated-off run-variant boots park too), so the slot-4 twi never arms a LIVE DR — exactly Q-C3's image-selection defect (the from-emulator post ORs a volatile r13; ctx-reloading exits discard it) + the shape-C armed-unpolled park. The shape-A chronology (PROGRAM#4 then livelock) was ITSELF a torn-boundary artifact; with the tear fixed the boot lands in the park class. Slot-4 consumption = Task B's round trip (the coordinator ACK note 2 keeps Q-C3 in Task B). |
| Torn-ctx fingerprint GONE | **PASS** | EXT restart is NOT a stub-window PC (pre-fix b4: restart=50318018); `[IRQ-CONSUME] EE edge deferred at pc=50318014` shows the exact tear moment latched instead; livelock-block probe r10=0xf072 (clean, not 0x9040-scratch class); no constant-register spin. |
| Delivery healthy / exactly-once | **PASS** | DEC #1–4 normal varied restarts, (2-SPR) route; zero TRIPWIRE; `host-irq: edges=1 consumed=1 pending=0`; `[IRQ-CONSUME] deferred=1 held=1 fired=2 latch=0` (every latch episode ends in exactly one fire; held=1 — one suppressed in-window poll all boot); sc census E4-class (distinct=16, 0xffffffff/0xfffffffe tails), PROGRAM#1–4 slot=8 + #5 slot=5 = baseline census class. |
| Gated-off A/B | **PASS** | s4ta-b5 (SS_NW_PIC=1, no IRQ_CONSUME, same probe set): BOOT-VERDICT PASS, `--absent 'TRIPWIRE;;IRQ-CONSUME;;riser windows'` clean (ZERO new behavior lines gated off — the window-export log line is consume-gated), class member (park/shape-C this run; shape is run-variant per Task-0's own record). Default boot: structural-inertness substitution — every behavioral line sits behind MachineProfileIsNewWorld() && ExcIrqConsumeEnabled() (default OFF) && riser-armed; the only unconditional addition is the data-only g_exc_riser_window fill (no output, no guest-visible effect). |

**Boot accounting:** counted — b1r (livelock capture/falsification), b3-livelock-ctx
(loop-membership probes; crashed post-capture), b4r (re-pin GREEN), b5 (A/B) = 4 of ≤4.
Disclosed, not counted (the Task-0 crash-rerun precedent): b1 SIGTRAP, b2-loopmap SIGSEGV,
b4 SIGSEGV — ALL pre-engagement (zero [IRQ-CONSUME] activity, latch never set), all in the
same class: DEC #1 delivered into 0x500eXXXX ROM boot-path code (b1: 5x restart=500e7310
then r9 marched to 0x1ffffffc in a bdnz loop; b4: restart=500e1c7c, SIGSEGV at 500e1c98).
**RESIDUE FLAG for Task B/C:** 3/7 env-on boots crashed this way vs Task-0's 1/5 — a
pre-existing delivery-into-early-ROM-code fragility (possibly CTR/loop-state interaction
with the 2-SPR route), timing-sensitive, NOT caused by the consume machinery (engages
later) but possibly timing-shifted by it. Deserves its own recon question if the rate
holds.

#### Task A sub-contract re-grade (2026-06-12, post-review fold — coordinator P1)

The rev-2 world-switch sub-contract as written required "PROGRAM#4 fires AND no
livelock". The landed Task A passed the livelock/fingerprint arms but **PROGRAM#4 was
re-scoped out of Task A's gate and into Task B's round-trip gate** — legitimately: Task
0's Q-C3 verdict (the from-emulator post ORs only the volatile working r13, so a clean
post-fix delivery at an out-of-DR PC never arms a live DR) plus the finding that the
shape-A chronology (PROGRAM#4 → livelock) was itself a torn-boundary artifact. The gate
wording was not amended at the time; this note is the dated re-grade. PROGRAM#4 (slot-4
twi) now belongs to Task B's round-trip sub-contract (a) — or to the direct CR-arm
consumption path if that is what the real chain does, per Q-C2.

**Design notes as landed:** windows single-source = `g_exc_riser_window` filled at the
trap_return patch site (rom_patches.cpp) from the emitted values — derived stub window is
[0x50318000, 0x5031801c) (7 words: the plan's 0x318020 quote was the loose 8-word bound;
the derivation is authoritative), reload [0x503244e4, 0x50324528). Latch/fire predicates
are pure (exc_core.cpp, test_exc_chain U14, 24 checks). Riser-conditional per A7:
armed recorded at the rom_patches riser gate's one eval site; consume-on+riser-off inert
by construction (empty windows). The fix edits the mtmsr re-raise path ⇒ **B4's
fold-into-cluster arm is the live one for Task C's 17th-gate decision.**

### Task B: the consumption round trip + retirement + the Ticks rider — size M, ≤5 boots

> **DONE 2026-06-12 (stop-rule 2 capture)** — commits `02a0b74e` (Q-C3 fix) /
> `17e0d071` (ticks_keepset) / `377edbf6` (MODE_EMUL_OP fence) / `e6824327` (re-pin
> backstop). Full record: INTERRUPT-INJECTION-RECON.md § "Slot-4 consumption Task B".
> Chain GREEN through five legs (post → Q-C3 staging → drain → slot-4 twi [the
> re-graded PROGRAM#4 gate] → 68k level-1 handler at 60 Hz); RED at leg 8 — the
> via6522 model presents no VIA IFR source, the handler rte's source-less
> (0x5000eecc), OP_IRQ retire never runs, post re-traps slot-4 ~1.2k/s. That is a
> device-model frontier, NOT consumption machinery — captured and stopped per
> stop-rule 2. Multi-edge invariant: structurally unreachable pre-WLSC (deassert is
> HasMacStarted-gated — gate flaw recorded). Ticks: NOT guest-claimed; stop site
> recorded per the rider's fallback. Boots 5/5 counted + 1 disclosed edge-miss rerun.

- [x] **Round-trip sub-contract (PASS/FAIL), env-on test cluster + gate on:** one boot,
  default-class instruments + the Q-C2 watch set + `SS_PROBE_68K` armed: after
  `EXT delivered #1` — (a) Task A's world-switch gate re-asserted; (b) **the via_int
  chain probes fire: `SS_PROBE_68K=0x5000ec52:8` AND 0x5000ef22 AND 0x5000bbca ≥1
  match each** (nested-execute blindness named: if 0 matches but Ticks moves with DR
  attribution, investigate an Execute68k excursion before calling FAIL); (c) **the
  post is RETIRED per Q-C2's predicate** — the watch on 68fff070 shows the guest clear
  (0x8001→0-class change) from a guest writer PC, **zero host writers** (fence gate
  re-asserted). **[rev 2 / A4] Watch-needs-ring tension named:** the watch requires
  `SS_JIT_TRACE_RING=1`, and the ring historically forces shape C — this boot is
  acceptable ONLY POST-FIX (after Task A the livelock shapes should no longer form, so
  the ring no longer selects the shape; if the ring boot still parks pre-consumption,
  that is a Task-A falsification signal, not a watch problem).
- [x] *(adjudicated — moot; see DONE banner)* **Shape-B arm (per Q-C3):** one boot in the shape-B-producing config (no-ring
  class; **[rev 2 / B3]** if the entry baseline shape was C, the config-discovery boot
  to find a B-producing config comes out of THIS task's ≤5-boot cap — account for it).
  **[rev 2 / A6] EXPECT the "B no longer forms" branch to FAIL** — B's tail completed
  (its defect is the CR-arm in a never-reloaded r13 image, not the torn tail), so the
  default path is applying Q-C3's pinned ordering/image-selection fix (same gate,
  separate commit) and re-running; "A's fix subsumed it" is the surprise outcome,
  recorded if observed. If shape B persists with a NEW signature: one-iteration rule
  (dated addendum entry, ONE re-pin boot, resume); second falsification ⇒ stop-rule.
- [x] *(adjudicated — zero TRIPWIRE, DEC healthy post-backstop; edges=1 is the pre-WLSC structural ceiling, gate flaw recorded)* **Exactly-once/latch invariants:** `edges/consumed/deasserts` now advance past 1
  (retirement → deassert → a later edge re-arms) — pin the observed cycle count as the
  new invariant class; zero TRIPWIRE; deferred_native within the Q-I4(d) bound; DEC
  cadence healthy (≈2.0 mtspr/delivery class, no zero/tiny storm).
- [x] *(recorded: NOT guest-claimed — stop site = the via6522 IFR dispatch, per the fallback clause)* **THE RIDER (diagnostic, recorded, NOT a gate): Ticks guest-claimed** — watch word
  **0x16c** +1 attributed to the GUEST. **[rev 2 / A5 — attribution respecified]:**
  "ring record window = DR dispatch" is UNSOUND alone — the watch attributes a change
  to the NEXT ring record's block pc, and host keep-set writes also land inside
  DR-window records (both classes present identically). The guest-claim evidence is
  the conjunction of: (a) the attributing watch record carrying an
  **r24≈0xbbca-class value** (the 68k PC at the `addq.l #1,$16a` site); (b) the
  **`SS_PROBE_68K=0x5000bbca` match coinciding** with the watch trip (visit/record
  ordering); (c) a **host keep-set counter** (add/read one if absent) whose delta over
  the window accounts for the confounder census — guest claims = total 0x16c
  increments minus keep-set increments, and the claim requires that difference > 0.
  If it lands, it is the project headline; if not, record where the 60 Hz proc chain
  stops (the next frontier candidate).
- [ ] *(NOT RUN — boot budget exhausted at the round-trip evidence; carried to Task C's battery)* **Default-boot diagnostic (recorded):** one default boot (gate on post-flip
  preview is NOT available pre-Task-C — run `SS_NW_IRQ_CONSUME=1` only): does the nap
  park behavior change per the NK's designed coupling (slot-5 nap suppression while a
  post pends; DEC/EXT wakes)? Capture the new default-class signature for Task C's
  baseline.
- [x] *(landed 377edbf6, single-variable commit, same gate)* **MODE_EMUL_OP arm (conditional, separate commit):** if this task's work touched
  the HandleInterrupt dispatch, land the M7-named fence as its own single-variable
  commit (gated the same way); otherwise record still-deferred in the addendum.
- [x] Gates: task tier PASS (5/5 incl. plain test-jit 353/353, e2e-test 122) on the final commit; inner per commit. Risk tier: structural-inertness substitution (every behavior line behind MachineProfileIsNewWorld() && ExcIrqConsumeEnabled(); unconditional adds are data-only — ticks_keepset counter, zero-global read in tick_func).

#### Task B addendum (2026-06-12, one-iteration rule): the "next natural kick" premise FALSIFIED in the slot-4 cycle — re-pinned with the starvation backstop kick

Boot s4tb-b4 (rundir 20260612-063939.81376) carried the round trip through THREE new
legs live: EXT delivered #1 → post armed (watch 68fff070=0x8001 @50325520, block
50325520→502fd280 = the Q-C3 detour) → **the Q-C3 stub staged the deferred pair**
([PROBE 0x502fd280 visit=1] r28=0x8001 r31=0x00e00000; watch [KDP-0x440]=0x00e00000 +
[KDP-0x43c]=0x8001 at record #3568016) → **the scheduler-restore drain consumed it**
(watch: @0x503246b0 mask→0, @0x50324734 halfword re-posted + sentinel 0xffff reset,
record #3568082-84) → **the slot-4 twi FIRED** (`PROGRAM delivered #5: srr0=5046e8d0
word=0fff0004 slot=4 lr=5046c4f4` — the re-graded PROGRAM#4 gate, GREEN). Then the NEW
terminal shape: the DR↔NK twi cycle with `[IRQ-CONSUME] deferred=1434126 fired=0
latch=1` — every tail mtmsr EE-edge latches in-window and the latch NEVER fires,
because Task A's fire path relies on "the next natural kick" and both kicks are dead in
this regime: **the latched edge IS the DEC's own delivery** (VCLK pending=1, DEC
nap-parked, no future mtspr kick until the delivery the latch is holding) and the host
EXT edge is one-shot pre-WLSC (InterruptFlags never clears — Q-I4). This is exactly the
coordinator's Task-C flip-criteria item (a) "EXT-only strand," observed live one task
early.

**ONE re-pin (committed with this addendum): the starvation backstop kick.** The 60 Hz
tick thread re-kicks the CPU thread (`TriggerInterrupt()` — the existing DEC-expiry
idiom; a poll kick, not guest state — the fake-poke fence is untouched) while
`g_exc_deferred_ee_edge` is set. One poll per 16.7 ms: an in-window poll suppresses
again (no flag re-arm — the b1r dispatcher-spin shape cannot form), an out-of-window
poll fires the latch and delivers. Gated by construction (the latch is set only inside
consume+riser-armed code). Second falsification of the round-trip contract stops the
task.

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
- [ ] **[rev 2 / B4+A7] The 17th-gate decision — OWNER: Task C's implementer, DEADLINE:
  this flip step (decided, recorded, not drifted into):** EITHER (1) **fold
  `SS_NW_IRQ_CONSUME` into the M7 cluster** (atomic opt-out with
  EE_RISER/DEC_PUBLISHED/HOST_IRQ; mandatory if Task A's fix edits the riser stub —
  consume-on+riser-off is undefined per A7 — gate census stays 16), OR (2) keep it a
  **standalone 17th gate** + flag it a retirement candidate in Task Z, with the rule
  that IRQ_CONSUME-off-alone is diagnostic-only after the one-time A/B below (only
  defensible if Task A turned out pure-staging, where the ungated-seed precedent
  f31d475e/34d3d441 governs unless deliberately deviated — state which). **Named
  sanctioned exception:** this task's own opt-out A/B boot IS a partial-opt-out-class
  boot — it is the one-time validation run that licenses the opt-out's existence, not
  a supported config.
- [ ] **Flip-criteria additions (2026-06-12, Task-A review fold — coordinator P1,
  re-examine BEFORE the default flip):** (a) **the EXT-only strand** — the deferred-edge
  consume relies on "the next natural kick" to fire a latched edge, but the DEC kick is
  a per-mtspr one-shot and the host EXT kick fires only on assert edges; an EXT-only
  suppressed in-window poll with a quiescent DEC has NO guaranteed re-kick (today the
  ~88/s DEC metronome bounds the latency — that bound must be argued or fixed before
  default-on). (b) **the 0x500eXXXX pre-engagement crash class** — Task A's 3/7 env-on
  crash residue (DEC#1 delivered into 0x500eXXXX early-ROM code, r9 marching out of
  RAM) matches R-II9's ORIGINAL SIGSEGV (pc=0x500e708c, 5 same-block DEC restarts =
  the same class as Task A's 5x restart=500e7310), which tensions the R-II9 downgrade;
  carry it as a named open class into the flip decision.
- [ ] **P2 list (recorded for Task C/Z, not fixed now — Task-A review fold):**
  exc_core.h:243's "8 words → +0x20" comment is stale (7 words, end 0x5031801c); the
  U14 suite is 17 checks, not the 24 claimed in the commit message/plan; U14's
  se=0x50318020 loose-bound labels; the Task-A addendum's main_unix.cpp:1447 citation
  should point at the DEC scheduler kick (~:1350); telemetry note — `fired` can exceed
  `deferred` (the suppressed-poll-sets-latch path; harmless).
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
  (or 16 stays if the gate folded into the cluster — state which, per Task C's
  recorded decision). **[rev 2 / B5]** Add the "named next task" pointer line AND
  extend the cross-tracker stale-claim grep to cover it.
- [ ] MACHINE-LAYER-PLAN: header + the M7-follow-on row; **re-score #4 trigger check**
  (the named surprise's outcome — fired/resolved/avoided — is the §9 input; flag if
  a re-score is due, drafting per the convention).
- [ ] ROADMAP: **[rev 2 / B5]** this row closed BY NAME (header/status line named, not
  implied); next named task row added; the SS_NW_PIC flip-criteria row updated with
  the consumption input; the deferred gate-retirement candidates un-deferred
  (re-score #3's coordinator addition expires with this task — flag, do not execute).
- [ ] **[rev 2 / B5] Tabulated residue disposition** (one row each, in the addendum or
  Task Z commit): R-II7 stays-open-until-PIC-flip; R-II8 per Q-C4's verdict; R-II9
  still-open instrument suspect; R-II10 closed-by-this-milestone or its named
  remainder.
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
   fake-poke by another name and takes this trigger too. **[rev 2 / A1] EXCEPTION —
   fork branch (iii) does NOT trigger this rule:** patched-tail non-atomicity is a
   defect in OUR riser-cluster code, not unmodeled NK protocol; its fix (rfi-atomicity
   emulation) is in-scope Task-A work, gated by the Task-0.5 ACK like a seed.
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
the red team *(rev 2: all five were adjudicated — see the Rev 2 table; statuses noted
inline)*: (1) the r10=0x9040 anomaly may be probe-sampling artifact vs smoking gun —
*(rev 2: NEITHER — it is the riser's composed-MSR scratch by construction, A1/A3)*;
(2) Q-C2's retirement predicate has a pre-authorized downgrade path (site-unpinned) —
is that too soft? *(rev 2: yes — hardened, B1)*; (3) shape B may be subsumed by A's
fix (claimed plausible, not proven) *(rev 2: expectation inverted — expect NOT
subsumed, A6)*; (4) the rider's DR-attribution method (ring-window class) vs the
keep-set confounder *(rev 2: respecified, A5)*; (5) whether `SS_NW_IRQ_CONSUME` should
instead join the existing M7 cluster rather than be a 17th gate *(rev 2: a deliberate
Task-C decision with owner+deadline; cluster-join mandatory if the fix edits the
riser, A7/B4)*.

## Red-team record

Two red-team reviews received 2026-06-12: A (technical, verdict SOUND-WITH-FIXES, 7
findings) and B (process, verdict SOUND-WITH-FIXES, 6 findings). Folded below as rev 2.

## Rev 2 (red-team fold, 2026-06-12)

Every finding was independently re-verified against the evidence before folding (the
fold agent re-walked the code/dumps; verification notes inline). Markers `[rev 2 / Xn]`
in the body locate each in-place edit.

| # | Finding | Verification | Disposition |
|---|---|---|---|
| A1 | BLOCKER — Q-C1 fork missing branch (iii): the livelock is likely OUR riser's mtmsr firing the EE-edge re-raise MID-TAIL (between mtmsr and bctr), delivering a torn ctx whose restore self-loops 0x3244e4↔0x3244e8; raw rfi is atomic, the window is patch-created | **CONFIRMED by static re-walk.** (1) rom_patches.cpp:2546-2556: the default-ON 7-word stub emits `mfmsr r10; rlwimi r10,r11,0,16,16; mtmsr r10` BEFORE `b reload-region`; the ctx reloads (r10←r6+0x154 @0x3244fc, r11←r6+0x15c @0x324500, per the patch comment + EE-CHAIN D-1) and the bctr all execute AFTER the EE rise. (2) ppc-execute.cpp:1432-1439: `execute_mtmsr` calls `trigger_interrupt()` iff old EE=0 ∧ new EE=1 ∧ (DEC ∨ EXT ∨ host-latch pending) — with DEC delivering ~88/s underneath shape A, the window is hit on every restore pass. (3) The fingerprint is by-construction: r10=0x9040 (=EE 0x8000 \| 0x1040) is EXACTLY the riser's composed-MSR scratch — no other mechanism places an MSR image in r10 at reload-region entry; r12=0x5046c4f4 is the tail's `mtlr r12` LR image, constant because the same torn ctx is saved/restored each cycle. Raw NK tail = `mtspr SRR0,r10; mtspr SRR1,r11; rfi` (EE-CHAIN :381) — atomic by design. | FOLDED: fork branch (iii) added (Architecture, Q-C1, Task 0.5, Task A, stop-rule 1 exception); Task A's expected shape corrected to "rfi-atomicity fix OR seed"; "NO new service bodies" stands; Q-C1 reframed from open-ended discovery to cheap live confirmation (srr0-image predicate) |
| A2 | BLOCKER — rom901.bin (d1a267a9) predates the 81d60cc1 flip; its 0x318000 stub is riser-less; /tmp/rom901.bin is the retired image | **CONFIRMED by byte inspection**: rom901.bin @0x318000 = `81402818 394affff 91402818 4800c4d8` (4 words, no mfmsr/rlwimi/mtmsr); MANIFEST provenance "inj-task0 boot" = pre-Task-C-flip; /tmp/rom901.bin md5 = e432df64 (the retired pre-f808a7fb image) | FOLDED: ROM-provenance fact + Q-C1(a) — stub authority is rom_patches.cpp:2522ff or a re-dumped re-baselined image; /tmp copy banned |
| A3 | MAJOR — r10 at 0x3244e8 is never the resume PC (mtctr r10 @0x3244d8 moved it; the stub clobbers r10 twice); CTR unprobeable — pin the bctr target via the ctx SRR0-image slot (+0xa4) in the planned [r6:0x180] probe | CONFIRMED (same code walk as A1; the patched tail is `mtctr r10; mtcrf; b 0x318000`, then `lwz r10,XLM_IRQ_NEST` then `mfmsr r10`) | FOLDED: Shape-A anatomy fact rewritten (the rev-1 "MSR image in the resume-PC slot — suspicious" framing was WRONG and would have misled the implementer); +0xa4 offset carried as expected-verify-in-Q-C1a |
| A4 | MAJOR — Q-C1c (ring at livelock onset) is self-defeating: ring ⇒ shape C | CONFIRMED by the plan's own instrument rule ("ring-slowed boots park, no-ring boots spin") | FOLDED: Q-C1c demoted to optional/confirmatory, named SS_RING_WINDOW-minimized config, shape recorded not A/B-compared; Task B's watch-needs-ring tension named acceptable-only-post-fix |
| A5 | MAJOR — the Ticks rider's "ring record window = DR dispatch" attribution is unsound (host keep-set writes also attribute to the next ring record's block pc) | CONFIRMED conceptually (the watch check lives in the ring recorder; host writes generate no records of their own) | FOLDED: rider respecified — r24≈0xbbca-class watch-record value + SS_PROBE_68K=0x5000bbca coincidence + host keep-set counter delta census |
| A6 | MINOR — shape B's tail COMPLETED (jDR flat-out); its defect is the CR-arm in a never-reloaded r13 image; "B no longer forms" should be expected to FAIL; Q-C3's static r13-image analysis is load-bearing | CONFIRMED against the recorded shape-B signature (no PROGRAM#4, jDR +100M blocks/s = the restore worked) | FOLDED: Q-C3 deliverable re-weighted; Task B shape-B arm expectation inverted |
| A7 | MINOR — if the fix is a riser-stub edit, consume-on+riser-off is undefined; decide cluster-join vs 17th gate deliberately | CONFIRMED (follows from A1's fix shape) | FOLDED: Task A gate-coherence note (riser-conditional implementation) + merged into B4's Task-C decision step |
| B1 | MAJOR — harden the Q-C2 retirement downgrade | Sound (the rev-1 downgrade accepted any non-host writer) | FOLDED: three-condition hardening (pinned writer-PC family, temporal ordering, multi-edge invariant bound) |
| B2 | MAJOR — Task 0.5: fork-(i)/(iii) PROCEED needs a one-line coordinator ACK; only (ii) STOPs; per-fork escalation after ONE failed re-pin; STOP includes a mini-Z | Sound | FOLDED: Task 0.5 rewritten with all three clauses |
| B3 | MAJOR — same ring contradiction as A4 (folded once) + shape-B config-discovery boot accounting | Sound | FOLDED: entry gate + Task B shape-B arm carry the boot-cap accounting; ring item folded under A4 |
| B4 | MAJOR — 17th-gate decision gets owner+deadline; name Task C's own opt-out boot as the sanctioned partial-opt-out exception | Sound | FOLDED: new Task-C decision step (owner = Task C implementer, deadline = the flip step; fold-into-cluster mandatory if the fix edits the riser; standalone+retirement-flag path conditioned on pure-staging with the f31d475e/34d3d441 precedent) |
| B5 | MINOR — Task Z: ROADMAP row closed by name; AGENT-CONTEXT named-next-task pointer in the stale-claim grep; tabulated residue disposition (R-II7/8/9/10) | Sound | FOLDED into Task Z |
| B6 | MINOR — pin the env-on entry boot's --expect patterns as grep-verdicts | Sound | FOLDED into the entry gate |

### Re-stated critical path (post-fold)

A1 is CONFIRMED at the static level, so the likely critical path is now:

1. **Task 0** — confirm the torn-ctx fingerprint LIVE (cheap, ≤2 boots): the
   livelock-block `[r6:0x180]` probe's ctx srr0-image slot holds a reload-region/tail
   PC. Q-C2/Q-C3/Q-C4 proceed in parallel within budget (static-first).
2. **Task 0.5** — fork-(iii) verdict table → one-line coordinator ACK.
3. **Task A** — the rfi-atomicity fix in the riser-cluster code (env-gated
   `SS_NW_IRQ_CONSUME`, riser-conditional), world-switch sub-contract.
4. **Task B** — the consumption round trip + retirement (hardened predicate) + the
   shape-B r13-image fix (expected NOT subsumed) + the respecified Ticks rider.
5. **Task C/Z** — battery, the deliberate cluster-join-vs-17th-gate decision, flip
   last, docs.

If Task 0's live probe FALSIFIES the torn-ctx mechanism (srr0-image holds a DR/foreign
PC), the fork re-opens to (i)/(ii) and the original seed-vs-machinery path governs.

### Errors found in the reviews themselves

None falsified. One precision note: A1's phrase "between mtmsr and bctr" spans the
whole reload region (mtmsr is block-ending; delivery lands at the next block boundary,
i.e. at reload-region entry or mid-reload, not literally mid-instruction) — the folded
text states the window as "before the ctx reloads + bctr complete", which is the
verified form. A3's +0xa4 ctx SRR0-image offset was NOT independently re-verified at
fold time — carried as expected-verify-in-Q-C1a, not as fact.

## Task-0 results (2026-06-12, s4t0 recon) — fork-(iii) CONFIRMED LIVE; Tasks A/B unblocked pending the Task-0.5 ACK

Full addendum (the blocking-answer table, per-gate evidence, the fix table):
**INTERRUPT-INJECTION-RECON.md § "Slot-4 consumption recon (M8 Task 0)"**. Summary:

- **Entry gate: PASS both boots** (grep-verdict BOOT-VERDICT PASS each; b1 default 3/3
  expects, b2 env-on 2/2). **Baseline shape = A** (slot-4 PROGRAM srr0=5046e8d0
  lr=5046c4f4 present; jNK ~82M/s with jDR static + comp frozen), reproduced 3/3 env-on
  boots; livelock block run-variant within the reload region (0x3244f8 on b2, 0x3244e8
  on b4). Shape-B config not yet observed — Task B's config-discovery boot stands (B3).
- **Q-C1 = fork-(iii), CONFIRMED LIVE** [PROBE✓+STATIC]: b4's `EXT delivered #1:
  restart=50318018` is the mid-stub (post-mtmsr) delivery A1 predicted, and the
  livelock-time `[r6:0x180]` ctx (visits 10⁰..10⁹, constant) carries the torn images
  **ctx+0x154=ctx+0x15c=0x9040** (the riser's composed-MSR scratch saved as the r10/r11
  GPR images) with the armed post 0x8001 visible in-frame (+0x70) and the original DR
  resume PC intact-but-unreached at the **real SRR0-image slot ctx+0xfc**
  (+0xa4 = SRR1 image — the rev-2 verify-first offset is now pinned). Predicate
  refinement recorded (resume-PC tear travels the register/CTR path, not the ctx slot;
  mechanism confirmed by the stronger direct evidence; fork NOT re-opened).
- **Q-C2**: retirement site table pinned — guest halfword retire = emul_op.cpp:812
  (OP_IRQ head via fe6b@0xbbc8); NK level-0 retire leg = 0x325520/0x32552c
  (sth 0 + CR and-clear); deassert = main_unix.cpp:2930 ClearInterruptFlag at
  InterruptFlags==0; DR cr2-clear site mirror/[PROBE✓]-only (open half, covered by the
  hardened-B1 downgrade conditions, all three stated in the addendum).
- **Q-C3**: image-selection defect — the from-emulator post leg ORs the VOLATILE working
  r13 only (no ctx+0xdc write-through, no deferred-pair staging); ctx-reloading exit
  paths (scheduler restore `lwz r13,0xdc(r6)`) discard the arm; only the deferred pair
  `[KDP-0x440]/[KDP-0x43c]` survives via the 0x324720 drain. Fix = bounded ordering
  (stage the deferred pair from the from-emulator leg too); **NOT subsumed by A's fix**
  (A6 confirmed).
- **Q-C4**: `[KDP+0x910]` inert for the consumption/restore leg (zero readers there
  [STATIC]; live junk value 0x503224e8 [PROBE✓ b1]) — no seed; R-II8 unchanged.
- **Boots: 4 counted of ≤8** (+1 crash re-run disclosed, rebuild-race rule; Q-C1 took 3
  boots due to a disclosed `--env` whitespace-split instrument mishap — b2/b3r ran
  probe-less). Q-C1c ring window not run (demoted per A4).
- **Task 0.5**: the fix table is in the addendum — rfi-atomicity emulation: defer the
  EE-edge re-raise past the bctr (latch in execute_mtmsr when the edge fires inside the
  stub window 0x318000-0x318020; fire at the first block boundary outside the
  tail/reload window), files ppc-execute.cpp + ppc-cpu.cpp/exc_core (latch),
  rom_patches.cpp untouched, gate `SS_NW_IRQ_CONSUME` default OFF, riser-conditional
  per A7. **PROCEED awaits the one-line coordinator ACK.**

### Task 0.5 — coordinator ACK (2026-06-12)

**ACKed as proposed** (fork-(iii), the Task-0 fix table): rfi-atomicity emulation via a
deferred EE-edge latch — when execute_mtmsr's EE 0→1 edge fires with the guest PC inside
the riser-stub window [0x50318000,0x50318020), latch instead of trigger_interrupt; fire
at the first block boundary outside the tail/reload window (past the 0x324524 bctr).
One latch + one predicate; ppc-execute.cpp/ppc-cpu.cpp/exc_core only; rom_patches
untouched; SS_NW_IRQ_CONSUME default OFF, riser-conditional, flip-last at Task C.
Coordinator notes: (1) the window constants must reference/derive from the same stub
constants rom_patches.cpp:2522ff emits (one source of truth, or a cross-checked static
assert — no second hand-copied magic range); (2) the Q-C3 shape-B ordering fix stays
Task B scope per the plan; do not fold it into Task A.
