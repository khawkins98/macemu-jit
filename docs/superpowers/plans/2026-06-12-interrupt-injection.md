# M7 host→guest interrupt routing — real delivery through the PIC/EXC_EXTERNAL path: host posts enter the guest's own level-1 chain and Ticks moves

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking. **Dispatch ONE task at a time — Tasks A/B/C
> all edit `sheepshaver_glue.cpp` + `main_unix.cpp` (+ possibly `emul_op.cpp`); NOTHING in
> this plan parallelizes.** This plan is item 3 (M/L) + the item-4 rider of
> INTERRUPT-INJECTION-RECON.md's Q5 list.

**Goal:** Route host-side interrupt posts — timer expiry (`INTFLAG_TIMER`) first; SCC/VIA
later — into the guest's **own** level-1 interrupt chain, with no host-thread injection
and no fake-poke: host post → EXT pending (the W2-3 level-held seam; PIC-source semantics)
→ `ExcEnter(EXC_EXTERNAL)` delivery to the NK-published handler (`external_entry` =
`[KDP+0x374]` = **0x50314880**, wired since W2-3, pre-positioned, **never live-fired**) →
the NK's own dispatch → the NK's 68k-interrupt post (`sth →[[KDP+0x67c]]` + `[KDP+0x674]`
CR-mask OR — static 0x325518-family, exists, never executed) → the DR emulator's level-1
dispatch → the patched **via_int / via_int2 chain (byte-proven PRESENT on 9.0.1**, recon
boot 3) → `M68K_EMUL_OP_IRQ` consumes `InterruptFlags` exactly as on paravirtual →
`TimerInterrupt()` → `Execute68k(tmAddr)` (usable post inj-s-fixes' KDP+0x1074/78
staging) → the 60 Hz task proc 0x5000bbb8 self-re-primes and **`Ticks` (0x16a)
advances** — the item-4 rider, verify-don't-build. The paravirtual `[KDP+0x67c]`/CR
fake-poke stays **fenced permanently** on newworld; the `MODE_EMUL_OP` host-thread
Execute68k injection arm gets a written disposition (Task C). Paravirtual byte-identical
throughout. PASS/FAIL gates are the falsifiable sub-contracts below; "Ticks advancing"
and any boot-frontier advance are recorded diagnostics, not gates.

**Architecture (the recon's answer, made concrete):** real delivery through the exception
path. No new exception machinery — exc_core, the delivery hook's EXT branch, the
level-held `exc_ext_pending_flag` seam (`SheepExcExtSetPending`, glue :231–271), and
`external_entry=0x50314880` all shipped with W2-3 (gated-off, harness-proven H6/H7). What
this milestone adds is (a) a **host interrupt source** for that seam — `InterruptFlags≠0`
as a level, asserted by `SetInterruptFlag` and retired by `ClearInterruptFlag` (OP_IRQ's
consumption), host-owned retirement so no guest IACK/EOI is needed (the NK has none —
W2S-2 pinned) — and (b) whatever minimal NK-side conformance Task 0 pins so the EXT body
reaches the 68k post. **The open architecture fork (PIC-routed EXT vs DEC-tick piggyback)
is confronted in Task 0 Q-I1/Q-I2 with a pre-stated decision rule** — see "The fork"
below; the *full*-fidelity PIC traversal (guest programs CTPR/masks, guest IACKs) is
explicitly NOT this milestone: the guest never touches the PIC at this frontier
([PIC] reads=0 writes=0, CTPR=15 — W2-3), and staging a fake guest PIC init has no donor
(Mac OS's native interrupt dispatcher programs the MPIC later in boot, not the
Trampoline). Device sources (SCC 0x25 / VIA 0x19) stay PIC-routed and gated-off behind
`SS_NW_PIC` awaiting real guest PIC init — an explicitly later milestone. New machinery
is env-gated `SS_NW_HOST_IRQ=1` (default OFF), flipped to the newworld profile default
only as Task C's LAST step with revert-on-red.

**The fork, stated honestly (PIC/EXT-routed vs DEC-tick piggyback):** the recon offers
both. What the evidence already discriminates: (i) full PIC traversal is out — no guest
unmask exists or is honestly stageable at this frontier (above); the EXT *pending seam*
does not require the PIC model in the loop for a host source, and using it keeps
PIC-source semantics (level-held, deassert-retired) so device sources can join the same
rail later. (ii) What the evidence does NOT discriminate: whether an EXC_EXTERNAL
delivery at 0x50314880 — with the NK's registered-handler table NOT installed
(`[[KDP-0x338]+0x20]=1`, base/bound=0 → the first EXT delivery exits via the
`[KDP+0x5b0]` fallback **0x50325f00** [PROBE✓, W2-2 live]) — ever reaches the 68k post,
versus being swallowed by the fallback. The post site is invoked from "service bodies"
(W2S-1), not provably from the EXT body's frontier path. **That is Task 0's first
blocking question (Q-I1).** Decision rule, pre-stated: if the EXT body (directly, via
the fallback, or via the shared dispatch flag-tree) reaches the 68k post for a delivery
that interrupts the emulator context (flags bit 0x00200000), the EXT route stands. If it
provably cannot without the unstaged registered-handler table, the SAME question is asked
of the DEC body 0x50313200's exit tree (deliveries there are live and healthy post-D-7) —
if the post is reachable from the DEC path's bookkeeping leg (the 0x50312cb0
`andi. r8,r7,0x30` CR-mutation family — "the post-68k-interrupt bookkeeping family",
W2-2 link 6), the plan re-pins to **DEC-tick piggyback**: the host level then arms the
68k post at DEC delivery time instead of a separate EXT delivery, same gate, same
acceptance chain from the post onward. If NEITHER body reaches the post without unstaged
NK surfaces, the stop-rule fires (trigger 1) — no improvised host-side transcription of
the post (writing `[[KDP+0x67c]]`/CR from the host IS the fenced fake-poke by another
name; rejected in advance).

---

## ENTRY GATE — the in-flight `inj-s-fixes` task must land green FIRST

This plan is **conditioned on** the in-flight task (claim
`docs/superpowers/.claims/inj-s-fixes.claim`: Q5 items 1+2 — Execute68k KDP staging +
the DEFER_NATIVE wake-up re-arm). Task 0 does not start until every row below is
verified against the landed commits' evidence (or re-verified by one boot of our own,
charged to Task 0's budget):

| # | Signature required from inj-s-fixes | Where to verify | Why this plan needs it |
|---|---|---|---|
| E1 | `[KDP+0x1074]=0x50480000` + `[KDP+0x1078]=0x50460000` staged (probe `[0x68fff074]/[0x68fff078]` nonzero on a default boot) | its acceptance boot / one probe boot | Execute68k is the engine of OP_IRQ's `TimerInterrupt()` task calls AND of the recon's crash chain; without it the first OP_IRQ kills the boot (recon Q1) |
| E2 | Default newworld boot survives OP_NAME_REGISTRY: **no SIGSEGV pc=0x100000/lr=0x504ff348**; parks at the boot-2-class frontier (PROGRAM slot=5 srr0=0x50324fec class), not pre-0.2s death | its gated/default A-B boots | every boot in this plan runs past the old crash point |
| E3 | **`delivered_dec>0` live on the riser-on boot** (`SS_NW_EE_RISER=1 SS_NW_DEC_PUBLISHED=1`): the exc= tuple's field 1 nonzero, `deferred_native` no longer terminal-starved at 97-with-zero-delivered (D-7's signature) | its re-arm acceptance boot | the DEFER_NATIVE wake-up is the same edge EXT deliveries will starve on; EXT acceptance is unfalsifiable while delivery itself starves |
| E4 | The **new default-boot frontier signature** published (the inj-s-fixes close-out: term-dump exc= tuple shape, sc-selector census, park signature) | its addendum in INTERRUPT-INJECTION-RECON.md / CHANGELOG | the baseline class every A/B boot in this plan diffs against (Q-I5 starts from it, doesn't re-derive it) |

**If any row is red or absent: HOLD — report to the coordinator; do not start Task 0
against a moving baseline.** If inj-s-fixes shipped item 2 with a different mechanism
than "re-arm the HANDLE spcflag" (recon Q4 option c), read its record and carry the
as-landed mechanism's name into this plan's boots — the signature (E3) is what binds,
not the mechanism.

---

## Authoritative inputs

| Doc | What it fixes |
|---|---|
| `docs/planning/machine/INTERRUPT-INJECTION-RECON.md` (`5accbcf8`) | THE recon this plan implements: Q1 the Execute68k crash anatomy + the seed discriminator (boot 2); Q2 the paravirtual donor chain file:line (PrimeTime→INTFLAG_TIMER→OP_IRQ→TimerInterrupt→Execute68k→Ticks); Q3 the link-by-link newworld gap inventory + **the architecture answer** (exception-path delivery; fake-poke fenced forever; MODE_EMUL_OP injection = paravirtual-only legacy); Q4 the [0x2810] watch (fence semantics CORRECT; run-mode clearing is a NON-fix — do not revisit); Q5 the task shapes; residues R-II1..R-II5 (R-II3 = the boot-2 frontier, Q-I5's target; R-II4 = the KDP+0x1078 consumption residue) |
| `docs/planning/machine/EE-CHAIN-RECON.md` D-6/D-7 + W2-2/W2S/W2L sections | The riser + cadence state: riser as-built (0x318000 stub, EE-only compose); D-6 storm-scale delivery proof (12.4M deliveries, handler clean); D-7 the KDP+0xf2c frequency staging (`f31d475e`) — cadence HEALTHY (mtspr_dec=8, 1.042 ms timeslice); **link-7 consumption OPEN** ("nothing posts Ticks even when delivered"); XLM_IRQ_NEST drift −1/delivery DEFERRED (item 2); W2S-1 the r7-flag tree + **the 68k post** (`r23:=[KDP+0x67c]`, `r28:=level\|0x8000`, bit-0x0a test, `sth →[[KDP+0x67c]]` + `[KDP+0x674]` CR OR; callers = service bodies + init `bl 0x3254a0` ×2); W2S-2 **NO NK IACK/EOI anywhere**; registered-handler table `[KDP-0x338]` NOT installed at frontier → fallback `[KDP+0x5b0]`=**0x50325f00**; W2S-3 no EXT-pending bit (external-ness = entry point + constant code 9); the KDP+0x360 published vector table (0x500→0x50314880, 0x900→0x50313200); W2L-R1 (the [KDP+0x67c] resolved target never live-read — Q-I2 retires it) |
| `docs/superpowers/plans/2026-06-11-wave2-interrupt-chain.md` W2-3 (DONE note + body) | The as-landed EXT machinery this plan turns on: OpenPIC wired gated-off under `SS_NW_PIC` (bus 0xF3040000+0x40000, model `b86449c9` 206 checks); `external_entry`=0x50314880 (the sanctioned U12 flip — NOT the shared interrupt_entry); **level-held latch semantics BINDING (rev 2 C1)** — the hook never clears EXT pending, only the deassert edge does; the runaway (N=16) + U13 starvation (N=64) tripwires; DEC-before-EXT priority with the m11/C1 justification; the live facts: PIC reads=0 writes=0, CTPR=15, flip HELD per stop-rule 3; harness H6/H7 (entry discrimination + dual-pending priority) 9/9 |
| `docs/planning/MACHINE-LAYER-PLAN.md` | The strategy frame (real device models, real exception delivery, no paravirtual fictions on newworld; §2d CPU-core honesty — profile seams on slow paths only); the M3b/W2-4 rows this milestone completes; **§9 stop-rules + re-score conventions** (re-score #3 is due at the next checkpoint — Task Z reminds the coordinator) |
| `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` | The live sites: `exc_ext_pending_flag`/`SheepExcExtSetPending`/`SheepExcExtConfigure` + the F5 atomicity contract (:211–271); the delivery hook's EXT branch + `exc_stat_delivered_ext` (7th exc= field, :1076–1090); the tripwire counters (:233–238); `SS_EXC_ENTRY` third-field external override (:151–177); the exc= tuple layout (:1558–1572) |
| `SheepShaver/src/Unix/main_unix.cpp` (:2792–2800, :2640–2660) + `SheepShaver/src/timer.cpp` (:548/572/596, :607–640) + `SheepShaver/src/emul_op.cpp` (OP_IRQ :811–840) | The host-source donor sites: `SetInterruptFlag`/`ClearInterruptFlag` (the level this plan forwards); the three PRECISE_TIMING-flavor expiry posts; OP_IRQ's per-flag consumption (`ClearInterruptFlag(INTFLAG_VIA/SERIAL/ETHER)` visible; **INTFLAG_TIMER's clear site is a Task 0 verification item**, Q-I4); `TimerInterrupt()` |
| `docs/planning/machine/TRAP-TABLE-RECON.md` "FIX RECORD — instime-fix" | The TM trap-table state on 9.0.1 (entries #0x58/59/5a/93 → 0x2fd2xx, re-verified [PATCH-fresh] recon boot 3) — the host Time Manager (timer.cpp) is the timer model on BOTH profiles; the 60 Hz task proc is 0x5000bbb8 with the `jmp ([$568])` re-prime + `addq.l #1,$16a` |
| `docs/AGENT-CONTEXT.md` + `SheepShaver/tools/README-slots.md` | Boot/probe/gate mechanics: slot protocol, `--expect`/`--absent` boot verdicts, `tools/gates.sh <tier>`, probe limits (8 PCs/run, 68k PCs probe-blind → `SS_PROBE_68K`), watch = hex + needs `SS_JIT_TRACE_RING=1`, ring-walk tooling, evidence tags |

## Codebase facts (carried; implementers re-verify sites before editing)

- **Constants:** KDP=0x68ffe000 ⇒ `[KDP+0x67c]`=0x68ffe67c (a POINTER to the 68k pending
  word — the post is `sth →[[KDP+0x67c]]`, one indirection), `[KDP+0x674]`=0x68ffe674
  (CR mask), `[KDP+0x374]`=0x68ffe374 (=0x50314880), `[KDP-0x338]`/`[KDP+0x5b0]` (handler
  table / fallback=0x50325f00). Ticks word = lowmem 0x16a (probe `[0x168]` covers it,
  alignment). 60 Hz task proc 0x5000bbb8. EXT entry 0x50314880; DEC entry 0x50313200;
  the post family static 0x325518 / `bl 0x3254a0` (primary copy ⇒ live 0x50325518 /
  0x503254a0 — re-verify the live address against the dump before probing). via_int
  patch sites (file offsets, [PATCH-fresh]): via_int @0xef2c, via_int2 fe6b @0xbbc8,
  via_int3 jmp @0x16dd6.
- **The exc= tuple is 7 fields** (delivered_dec / deferred_ee / deferred_depth /
  deferred_native / delivered_sc / delivered_program / delivered_ext — appended LAST,
  glue :1558–1572). The 7th field prints only when `exc_ext_configured` is set — that
  conditional is the byte-identical-baseline mechanism; **the new gate must use the same
  idiom** for any new counter.
- **`SheepExcExtConfigure()` is currently set by SS_NW_PIC bring-up or the
  `SS_TEST_EXT_PENDING` harness knob** (glue :228–230). Task A adds the third
  configurator (`SS_NW_HOST_IRQ`). The F5 atomicity contract (:213–230) governs any new
  asserting thread: `SetInterruptFlag` runs on host timer/pump threads — the existing
  contract already anticipates non-CPU-thread assertion + a TriggerInterrupt-style kick
  on the assert edge; **a missed kick is not safe** (no 60 Hz safety net on newworld) —
  the kick is part of Task A's contract.
- **Level-held semantics are BINDING** (W2-3 rev 2 C1): the hook never clears EXT
  pending. For the host source, the deassert edge = `ClearInterruptFlag` bringing
  `InterruptFlags` to 0 (or the timer flag specifically — Task 0 Q-I4 pins the exact
  level predicate: all-flags vs per-flag). Retirement is HOST-owned; no guest IACK
  exists or is needed (W2S-2).
- **The named nesting hazard (red-team this hard):** EXT pending stays asserted from
  host post until OP_IRQ consumes the flag — an interval spanning the whole NK→68k→
  via_int→OP_IRQ chain. Every EE rise inside that interval re-delivers EXT (level-held +
  the runaway tripwire N=16 fires at 16 deliveries/assert). Whether the NK/68k chain
  masks this naturally (the posted level rides the 68k SR mask; what rides the PPC EE
  between NK exit and OP_IRQ?) is Q-I4's second half. If natural masking does not exist,
  the candidate in-architecture damper is delivering EXT only when not already inside an
  undelivered post window (a host-side once-per-assert latch on the DELIVERY side is a
  semantic change to C1 — needs a written justification, default NO).
- **Riser/published-DEC config:** `SS_NW_EE_RISER` + `SS_NW_DEC_PUBLISHED` are still
  default-OFF gates (their flip was reserved as "W2-4 final acceptance" — this milestone
  IS that acceptance work; Task C owns the cluster-flip decision). All live boots in
  this plan run with both ON unless a step says otherwise.
- **`SS_EXC_ENTRY=0xINT[,0xSC[,0xEXT]]`** third field overrides external_entry — the
  no-rebuild iteration channel for Q-I1 candidate experiments.
- **Fenced/closed things this plan must NOT reopen:** the `[KDP+0x67c]`/CR host
  fake-poke (fenced, recon Q3 item 3 — permanent); run-mode `[0x2810]` clearing and
  fence changes (recon Q4 — falsified non-fixes); `SS_NW_PIC` default flip (held per
  W2-3 stop-rule 3 — stays held; this plan does not need the PIC model in the loop).
- **Gates:** run via `tools/gates.sh <inner|task|full>` and read its `GATES <tier>:
  PASS|FAIL` verdict (AGENT-CONTEXT). Inner per commit; task tier on each task's final
  commit; the risk-based paravirtual `make e2e` applies when any touched line is
  reachable on paravirtual (SetInterruptFlag/ClearInterruptFlag/emul_op are SHARED —
  expect e2e REQUIRED for Task A unless every new line sits inside
  `MachineProfileIsNewWorld()`/env gates, in which case state the structural-inertness
  argument in the commit).
- **Standing rules:** slot-protocol boots only, `--expect/--absent` verdicts; ≤8 probe
  PCs/run; watch addrs hex + `SS_JIT_TRACE_RING=1`; explicit-path staging; `-F-` heredoc
  commits, no backticks; struct fields appended LAST; budgets are caps —
  partial-findings-beat-stalling; one-iteration rule on falsified contracts.

## Tasks

### Task 0: the fork + the post-path recon (BINDING — gates Tasks A..C; static RE + bounded probe boots)

Pin each answer in a written addendum (`INTERRUPT-INJECTION-RECON.md`, new section
"M7 Task 0 — EXT/DEC post-path recon"). Static RE (capstone PPC BE on the manifest
dumps, provenance ritual `tools/dump-manifest.sh --check` FIRST) is the primary tool;
**≤6 bounded boots total, each ≤60 s, ≤2 per question before its residue status is
decided; static windows ≤2 call levels / ≤12 functions per body (the P-C2 idiom)**.
Co-schedule probes (8 PCs/run). Capture-only telemetry commits allowed (inner gates).

- [ ] **Entry gate first:** verify rows E1–E4 against the landed inj-s-fixes evidence
  (≤1 verification boot if its record is ambiguous). Red ⇒ HOLD, report.
- [ ] **(Q-I1 — THE FORK; blocks Task A) Does a delivery reach the 68k post?** Static
  walk of the EXT body 0x50314880 (and its frontier exit = the fallback 0x50325f00)
  for a delivery whose interrupted context carries flags bit 0x00200000 (from-emulator):
  does control reach the post family (the `rlwinm. r8,r7,0,0xa,0xa`-guarded
  `sth →[[KDP+0x67c]]` + `[KDP+0x674]` CR OR at static 0x325518 / via `bl 0x3254a0`)?
  If NO: same walk on the DEC body 0x50313200's exit tree (the 0x50312cb0
  `andi. r8,r7,0x30` bookkeeping leg). **Deliverable: the route verdict — EXT-routed /
  DEC-piggyback / NEITHER (stop-rule trigger 1) — with the instruction-level path
  written down**, plus what the post needs as inputs (who supplies the level in r28?
  is the post a callable service expecting arguments, and from which leg is it invoked
  with level-1?). One candidate live boot allowed (`SS_TEST_EXT_PENDING` or
  `SS_EXC_ENTRY` third field; riser+published on) to observe where a single forced EXT
  delivery actually exits (probe 0x50314880 + 0x50325f00 + the post site).
- [ ] **(Q-I2 — blocks Task A/B) The post's live targets:** probe `[0x68ffe67c]`
  (the pointer), `[[KDP+0x67c]]` resolved target value-before, `[0x68ffe674]` (CR mask)
  in the cold 68k world — retires W2L-R1. Pin: is the resolved target the DR emulator's
  pending word the 68k dispatch polls (and at what 68k-side check site)? **Deliverable:
  the watch address for Task B (the RESOLVED target — the watch instrument cannot follow
  pointers) + the expected written value (`level|0x8000` class).**
- [ ] **(Q-I3 — blocks Task B) The 68k consumption side:** from the resolved pending
  word to OP_IRQ — static on the patched via_int chain: the 68k level-1 entry PC for
  `SS_PROBE_68K`, the via_int head (file 0xef2c → its runtime 68k address), the fe6b
  OP_IRQ site (file 0xbbc8 / proc 0x5000bbb8), and the CR-arm consumption (who reads
  the CR mask the post ORs in). **Deliverable: the 68k probe-PC list + the expected
  visit order for one tick.**
- [ ] **(Q-I4 — blocks Task A) Host-source mapping + retirement + the nesting hazard:**
  (a) the exact level predicate (`InterruptFlags≠0` vs `INTFLAG_TIMER` bit) and the
  deassert site — verify where INTFLAG_TIMER is cleared on the OP_IRQ path
  (emul_op.cpp OP_IRQ; if TimerInterrupt or nothing clears it, name the site Task A
  must use); (b) the assert-edge kick (TriggerInterrupt idiom from the timer thread —
  the F5 contract's "missed kick is not safe"); (c) **the nesting arithmetic**: expected
  deliveries-per-assert across the NK→68k→OP_IRQ window vs the runaway tripwire N=16 —
  what masks re-delivery between the NK's rfi and OP_IRQ's clear (the posted-level/SR
  mask? EE state in the 68k world per the riser's SRR1-image compose?). **Deliverable:
  a written delivery-count expectation per 60 Hz period + the masking mechanism, or the
  named gap.** A C1-touching damper, if needed, requires its own written justification
  here — default is NO change to level-held semantics.
- [ ] **(Q-I5) The baseline:** confirm E4's published frontier signature reproduces on
  one default boot of our own (`--expect` on its named lines); extend the
  characterization of R-II3 (PROGRAM slot=5 srr0=0x50324fec; the sc 0xffffffff growth)
  ONLY as far as one boot + one static window — it is the comparison baseline, not this
  milestone's quarry.
- [ ] **Gate (the blocking-answer table):** addendum committed; every answer tagged
  ([STATIC]/[PROBE✓]/[PATCH-fresh]); **Task A blocks on Q-I1 (route verdict) + Q-I2
  (post targets) + Q-I4 (source mapping + nesting expectation); Task B blocks on Q-I2 +
  Q-I3; Task C blocks on nothing new** (it consumes A+B). A residue on a blocking
  answer invokes the stop-rule — no improvisation. Budget: the 6-boot cap binds over
  per-question allowances.

### Task A: the host EXT source + first live delivery (env-gated `SS_NW_HOST_IRQ`, default OFF) — size S/M, ≤4 boots

Files: `sheepshaver_glue.cpp`, `main_unix.cpp` (+ `timer.cpp`/`emul_op.cpp` only if
Q-I4 named a clear-site gap). Every new line newworld-profile + env-gated; gated off ⇒
byte-identical baseline (E4's class).

- [ ] Land the gate: `SS_NW_HOST_IRQ` (default OFF) — at bring-up, when on:
  `SheepExcExtConfigure()`; `SetInterruptFlag`/`ClearInterruptFlag` newworld arm
  forwards the Q-I4-pinned level to `SheepExcExtSetPending(level)` + the assert-edge
  kick (the F5 contract: single-copy-atomic, release/acquire, kick on assert). If Q-I1
  re-pinned to DEC-piggyback: the same level instead arms the Q-I1-pinned DEC-side post
  condition — the gate name, counters, and acceptance shape are unchanged.
- [ ] Tripwire conformance: the runaway (N=16) and U13 starvation (N=64) counters must
  be live on this path; add NOTHING new unless Q-I4 demanded a damper (then: the written
  justification lands as a comment at the site, C1 cited).
- [ ] **Probe sub-contract (PASS/FAIL), env-on (riser+published+host-irq):**
  (a) `[EXC] EXT pending ASSERTED` edges appear at host-post cadence; (b) **the first
  live EXT delivery ever**: `[EXC] EXT delivered #1: … entry=50314880` (or the Q-I1
  route's equivalent first-delivery line) with the entry probe conforming to the
  Q-I1/Q-I2 expected state (interrupted-context class, flags word); (c) exc= field 7
  (delivered_ext) > 0 at term-dump; (d) tripwires: zero runaway lines, or every firing
  explained by the Q-I4 expectation (a divergence is a falsification → one-iteration
  rule). Boot command shape:
  `SheepShaver/tools/ss-slot-boot.sh --label m7-taskA --timeout 60 --env 'SS_NW_EE_RISER=1 SS_NW_DEC_PUBLISHED=1 SS_NW_HOST_IRQ=1 SS_PROBE_PC=0x50314880;…' --expect 'EXT delivered #1;;EXT pending ASSERTED' --absent 'RUNAWAY'`
- [ ] **Gated-off A/B (PASS/FAIL):** one boot without `SS_NW_HOST_IRQ` reproduces E4's
  baseline class byte-identically (no EXT lines, exc= 7th field absent, same park
  signature class).
- [ ] Gates: task tier (`tools/gates.sh task`) + paravirtual disposition per the
  risk-based rule (SetInterruptFlag is SHARED — paravirtual e2e REQUIRED unless the arm
  is provably inside the profile gate; state which in the commit). Commit.

### Task B: the 68k consumption round trip + the Ticks rider (verify-don't-build) — size S/M, ≤5 boots

Evidence task: at most seed/probe-class source changes (counters allowed, inner gates).
Blocks on Q-I2/Q-I3.

- [ ] **Post observed (PASS/FAIL):** `SS_JIT_TRACE_RING=1 SS_JIT_WATCH_ADDR=<Q-I2
  resolved target, hex no-0x>` — the NK's post writes the pinned value class
  (`level|0x8000`) at delivery cadence; writer PC = the post site, not host code
  (**the fake-poke staying fenced is itself a gate: zero host-side writers**).
- [ ] **via_int → OP_IRQ (PASS/FAIL):** `SS_PROBE_68K=<Q-I3 entry PCs>` fires in the
  pinned order; OP_IRQ executes (EMULOP counter / one-shot counter at the fe6b site);
  `InterruptFlags` consumed — the EXT level DEASSERTS after OP_IRQ (`[EXC] EXT pending
  deasserted` edges paired with asserts; no monotonic pending).
- [ ] **TimerInterrupt → Execute68k (PASS/FAIL):** the task-proc call observed
  (`SS_PROBE_68K=0x5000bbba` per the +2 fetch idiom — the probe that NEVER fired in
  recon boots 1/2) and RETURNS (no recon-Q1 crash class; E1's staging carrying the
  load).
- [ ] **The item-4 rider — Ticks (DIAGNOSTIC headline, recorded not gated):**
  `[0x168]` probe / watch on 0x168: does `Ticks` (0x16a) advance under sustained
  deliveries? If yes — the recon's "falls out for free" verified; record the rate vs
  60 Hz. If no — capture the exact break link (post seen? OP_IRQ ran? TimerInterrupt
  walked an empty tmDescList? — the instime-fix trap-table state is the first suspect)
  and name it; **a broken link here is a finding, not a license to build — the
  one-iteration rule applies if a pinned Q-I3 contract was falsified, else it is the
  next frontier.**
- [ ] **Invariant carry-over (PASS/FAIL):** nest drift `[0x2818]` recorded (the deferred
  XLM_IRQ_NEST item — drift is EXPECTED and tolerated this milestone, but a NEW drift
  rate class is a finding); no reset ring; deferred_native bounded (E3's class); DEC
  cadence stays healthy (mtspr_dec single digits per D-7).
- [ ] Gates: task tier (evidence task — smoke set per the stated-reason rule if zero
  source lines changed). Addendum results section. Commit.

### Task C: acceptance + the flip cluster (flip LAST, revert-on-red) + dispositions — size S, ≤4 boots

- [ ] **PASS/FAIL gates first, env-on:** (a) full gates (`tools/gates.sh full`);
  (b) Task A's delivery sub-contract re-asserted; (c) Task B's post/OP_IRQ/retirement
  gates re-asserted; (d) gated-off A/B byte-identical to E4's class; (e) tripwires
  quiet per Q-I4's expectation.
- [ ] **Fix budget:** telemetry/capture commits free; at most ONE small in-scope fix
  iteration per falsified contract, full gates re-run after any fix. Second
  falsification of the same contract ⇒ stop-rule.
- [ ] **THEN the flip decision (the cluster, explicitly):** flipping `SS_NW_HOST_IRQ`
  to the newworld default REQUIRES `SS_NW_EE_RISER` + `SS_NW_DEC_PUBLISHED` flipped
  with it (the routing is meaningless without delivery) — this is the flip W2-4
  reserved as its final acceptance, now owned here. Flip all three (opt-out `=0` kept
  each, SS_NW_MM_SWITCH polarity), re-run (a)–(e) with NO env vars. **Any red ⇒ revert
  the WHOLE cluster in the same task** (machinery stays env-gated, failure recorded —
  the milestone may ship gated-off green; it does not ship default-on red). `SS_NW_PIC`
  is NOT in the cluster — its flip stays HELD (W2-3 stop-rule 3; device sources await
  real guest PIC init, a later milestone).
- [ ] **Dispositions in writing (the deferred residue, addendum entries):**
  (1) **XLM_IRQ_NEST ownership** (EE-CHAIN item 2, recon-deferred): with deliveries now
  routine, decide retire / re-base / document-as-dead for the −1/delivery drift — a
  decision record, implementation only if S-sized and gate-clean, else a named
  follow-on; (2) **MODE_EMUL_OP injection-arm fencing** (recon Q3 item 5: post-E1 it is
  redundant-not-dangerous): fence it newworld for architectural consistency (S, gated)
  or record why it stays — either way written; (3) **R-II3 frontier**: the post-flip
  default boot's park signature captured in full (HB/exc=/sc-census/ring tail) — the
  next milestone's opening evidence, named honestly.
- [ ] **DIAGNOSTIC outcomes (recorded, not gates):** Ticks rate; any boot-frontier
  movement vs E4; first-ever sustained host→guest interrupt cadence numbers. Commit.

### Task Z: docs close-out — size S

- [ ] `SheepShaver/docs/DIAGNOSTICS.md`: `SS_NW_HOST_IRQ` (+ the cluster flip state,
  opt-outs, interaction with SS_TEST_EXT_PENDING/SS_EXC_ENTRY third field).
- [ ] `CHANGELOG.md`: the first host→guest interrupt delivered through the guest's own
  chain — acceptance numbers, flip state.
- [ ] `docs/planning/MACHINE-LAYER-PLAN.md`: header + the M3b/W2-4 row (the reserved
  riser flip's disposition; link-7 status); **flag re-score #3 due** (§9 convention) to
  the coordinator.
- [ ] `docs/superpowers/plans/2026-06-11-wave2-interrupt-chain.md`: W2-4 checkbox state
  + a pointer here; `docs/planning/machine/EE-CHAIN-RECON.md` + `INTERRUPT-INJECTION-RECON.md`:
  closure notes on items 1b/3/4 + the dispositions. `LEARNINGS.md` per its bar.
- [ ] Cross-tracker grep for stale claims ("never live-fired", "link 7 open",
  "Ticks never moves") — historical sections stay per the rule. ROADMAP cross-check.
  Doc-only commits, no gates.

## Stop-rule (triggers per MACHINE-LAYER-PLAN §9)

1. **Q-I1 NEITHER verdict:** if neither the EXT body nor the DEC exit tree reaches the
   NK's 68k post without unstaged surfaces (the registered-handler table, an NK init
   path the trampoline skips), STOP after the addendum and re-scope — host-side
   transcription of the post is REJECTED in advance (it is the fenced fake-poke).
2. **The consumption wall:** if Task B shows delivery + post green but the 68k side
   never consumes (via_int unreached / OP_IRQ dead / TimerInterrupt's world empty),
   capture the exact link and stop — that is the next milestone's named frontier, not
   tunnel material (the M5/M6 coupling precedent).
3. A residue on a BLOCKING Task-0 answer ⇒ trigger-1 re-scope, not improvisation.
4. The entry gate failing (inj-s-fixes red/absent) ⇒ HOLD before Task 0; coordinator
   decides.

Within tasks — the one-iteration rule: falsified pinned contract → dated addendum
entry → ONE bounded re-pin boot → resume; a second falsification of the same contract
escalates to the stop-rule.

## Self-review record

Spec coverage: implements INTERRUPT-INJECTION-RECON Q5 item 3 + the item-4 rider with
Q3's architecture answer as the spine (exception-path delivery, fake-poke fenced
forever, no host-thread injection); consumes the W2-3 as-landed EXT machinery instead
of rebuilding it (level-held C1 semantics carried BINDING); the riser/cadence state
(D-6/D-7) and the deferred residue (XLM_IRQ_NEST, MODE_EMUL_OP fencing, R-II3) all get
owned dispositions. The fork is confronted head-on: the evidence kills full PIC
traversal at this frontier (no guest unmask, no NK IACK/EOI, uninstalled handler
table) but does NOT pin whether 0x50314880 reaches the 68k post — so the mechanism
(host level on the existing EXT seam) is picked now, the VECTOR (EXT vs DEC-piggyback)
is Task 0 Q-I1's first blocking question with a pre-stated decision rule and a NEITHER
→ stop-rule branch. Entry-gated on inj-s-fixes with exact signatures (E1–E4). Known
tensions flagged for the red team: (1) the nesting/runaway hazard — level-held pending
across the whole NK→68k→OP_IRQ window vs EE re-rise (Q-I4; any C1-touching damper needs
written justification); (2) the flip-cluster scope (this plan claims W2-4's reserved
riser flip — coordinator-visible); (3) the post family's caller set ("service bodies")
may make Q-I1's static walk exceed its bounded window — the budget + residue rule
guards it; (4) the watch instrument cannot follow the [KDP+0x67c] indirection — Q-I2
must deliver the RESOLVED address; (5) Ticks is a rider diagnostic, never a gate —
the temptation to build the consumption side belongs to the next milestone.

## Red-team record

*(empty — a red-team round follows this draft; findings to be folded as rev 2 markers)*
