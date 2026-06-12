# M7 host→guest interrupt routing — real delivery through the PIC/EXC_EXTERNAL path: host posts enter the guest's own level-1 chain and Ticks moves

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking. **Dispatch ONE task at a time — Tasks A/B/C
> all edit `sheepshaver_glue.cpp` + `main_unix.cpp` (+ possibly `emul_op.cpp`); NOTHING in
> this plan parallelizes.** This plan is item 3 (M/L) + the item-4 rider of
> INTERRUPT-INJECTION-RECON.md's Q5 list.

**Goal:** Route host-side interrupt posts — timer expiry (`INTFLAG_TIMER`) first; SCC/VIA
later — into the guest's **own** level-1 interrupt chain, with no host-thread injection
and no fake-poke: host post → EXT pending (the W2-3 seam; **rev 2 A1: the host source is
a deliver-once-per-assert-edge latch, NOT PIC-level semantics — see Rev 2**)
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
asserted by `SetInterruptFlag`, retired by `ClearInterruptFlag` (OP_IRQ's consumption),
host-owned retirement so no guest IACK/EOI is needed (the NK has none — W2S-2 pinned);
**rev 2 A1/A2: forwarded as a once-per-assert-edge latch, not a bare level (livelock,
statically determined), and retirement is `HasMacStarted()`-gated — pre-warm-start the
level never deasserts by ANY route (see Rev 2 + Task B's regime split)** — and (b)
whatever minimal NK-side conformance Task 0 pins so the EXT body
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
name; rejected in advance). **(Rev 2 A1+A2: the decision rule is RE-STATED in the Rev 2
section — the host source is edge-latched on EITHER route (a bare level under the riser
is a statically-determined livelock), and the round-trip acceptance is split by the
`[0xcfc]` warm-start regime. The Rev 2 statement overrides this paragraph where they
differ.)**

---

## ENTRY GATE — the in-flight `inj-s-fixes` task must land green FIRST

**(Rev 2 coordinator update, 2026-06-12: inj-s-fixes is LANDED, review APPROVE —
`34d3d441` (Execute68k KDP staging) / `3cb3b16e` (DEFER_NATIVE wake-up re-arm) /
`bf571e26` (close-out). The E-rows now carry published ACTUALS; Task 0's entry check
verifies against those, not expectations: E1 staged by `34d3d441`; E2 actual = default
boot **44.4 s to the PROGRAM#5 srr0=0x50324fec park, no P-M5 SIGSEGV**; E3 actual =
riser-on **delivered_dec=3, pending=0 drained, mtspr_dec=25**; E4 published in the
`bf571e26` close-out. NOTE the E3 caveat below on `deferred_native` semantics.)**

This plan is **conditioned on** the in-flight task (claim
`docs/superpowers/.claims/inj-s-fixes.claim`: Q5 items 1+2 — Execute68k KDP staging +
the DEFER_NATIVE wake-up re-arm). Task 0 does not start until every row below is
verified against the landed commits' evidence (or re-verified by one boot of our own,
charged to Task 0's budget):

| # | Signature required from inj-s-fixes | Where to verify | Why this plan needs it |
|---|---|---|---|
| E1 | `[KDP+0x1074]=0x50480000` + `[KDP+0x1078]=0x50460000` staged (probe `[0x68fff074]/[0x68fff078]` nonzero on a default boot) | its acceptance boot / one probe boot | Execute68k is the engine of OP_IRQ's `TimerInterrupt()` task calls AND of the recon's crash chain; without it the first OP_IRQ kills the boot (recon Q1) |
| E2 | Default newworld boot survives OP_NAME_REGISTRY: **no SIGSEGV pc=0x100000/lr=0x504ff348**; parks at the boot-2-class frontier (PROGRAM slot=5 srr0=0x50324fec class), not pre-0.2s death | its gated/default A-B boots | every boot in this plan runs past the old crash point |
| E3 | **`delivered_dec>0` live on the riser-on boot** (`SS_NW_EE_RISER=1 SS_NW_DEC_PUBLISHED=1`): the exc= tuple's field 1 nonzero, `deferred_native` no longer terminal-starved at 97-with-zero-delivered (D-7's signature). **ACTUAL (landed): delivered_dec=3, pending=0 drained, mtspr_dec=25.** ⚠️ **deferred_native SEMANTIC CUTOFF at `3cb3b16e`**: the counter now counts EVERY re-poll (one parked window can contribute 65536+), NOT deferral events — never compare its value across that commit as an event count; this applies to EVERY exc=-tuple comparison in this plan (this row, Task B invariants, Q-I5 baselines) | its re-arm acceptance boot | the DEFER_NATIVE wake-up is the same edge EXT deliveries will starve on; EXT acceptance is unfalsifiable while delivery itself starves |
| E4 | The **new default-boot frontier signature** published (the inj-s-fixes close-out: term-dump exc= tuple shape, sc-selector census, park signature) | its addendum in INTERRUPT-INJECTION-RECON.md / CHANGELOG | the baseline class every A/B boot in this plan diffs against (Q-I5 starts from it, doesn't re-derive it) |

**If any row is red or absent: HOLD — report to the coordinator; do not start Task 0
against a moving baseline.** **(Rev 2 B3 — partial-green policy, stated deliberately:
HOLD is TOTAL — no static-only continuation carve-out. Rationale: every Task 0 question
including the static ones is scoped against E4's published frontier class; static work
against an unpinned baseline re-derives it, which is exactly the waste the gate exists
to prevent. The "≤1 verification boot" allowance covers ALL ambiguous rows co-scheduled
in that one boot (probes for E1, signature grep for E2/E3/E4 from the same run); if one
boot cannot cover every ambiguous row ⇒ HOLD, report.)** If inj-s-fixes shipped item 2
with a different mechanism
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
| `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` | The live sites: `exc_ext_pending_flag`/`SheepExcExtSetPending`/`SheepExcExtConfigure` + the F5 atomicity contract (:211–271; SetPending body ~:260–268 — a single 0/1 word, rev 2 A5); the delivery hook's EXT branch + `exc_stat_delivered_ext` (~:1095–1155 at rev 2: tripwire print :1132, `EXT delivered #N` print :1138 — re-verify before editing, the file moves); the tripwire counters (:241–245); `SS_EXC_ENTRY` third-field external override (:151–177); the exc= tuple layout (:1558–1572); the six EE re-raise compose sites that include `SheepExcExtPending()` (glue :952/:1373/:1432/:1462 + `ppc-execute.cpp` :1434/:1733 — rev 2 A1's livelock census) |
| `SheepShaver/src/Unix/main_unix.cpp` (:2792–2800, :2640–2660) + `SheepShaver/src/timer.cpp` (:548/572/596, :607–640) + `SheepShaver/src/emul_op.cpp` (OP_IRQ :811–855) | The host-source donor sites: `SetInterruptFlag`/`ClearInterruptFlag` (the level this plan forwards); the three PRECISE_TIMING-flavor expiry posts; OP_IRQ's per-flag consumption — **(rev 2 A6/A2) INTFLAG_TIMER's clear site is VISIBLE at emul_op.cpp:841 (`ClearInterruptFlag(INTFLAG_TIMER); TimerInterrupt();`) — the open Task 0 item is NOT finding it but the `HasMacStarted()` gate around the WHOLE flag-consuming block (emul_op.cpp:814; `[0xcfc]=='WLSC'` warm-start flag, `SheepShaver/src/include/macos_util.h:373–376`), unset at our frontier — see Q-I4(a)**; the `[KDP+0x67c]` pending-word clear at :812 sits OUTSIDE that gate (the 68k-side handshake IS pre-warm-start-reachable); `TimerInterrupt()` |
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
  glue :1558–1572). **(Rev 2 coordinator update) `deferred_native` is a RE-POLL METER
  since `3cb3b16e`** — it counts every block-boundary re-poll inside a native window
  (one parked window can contribute 65536+), NOT deferral events; any cross-commit
  exc= comparison (entry-gate E3, Q-I5 baselines, Task B invariants) must not read it
  as an event count, and expectations are bounded as ≤ 65536 × kick-episodes (Q-I4(d)).
  The 7th field prints only when `exc_ext_configured` is set — that
  conditional is the byte-identical-baseline mechanism; **the new gate must use the same
  idiom** for any new counter.
- **`SheepExcExtConfigure()` is currently set by SS_NW_PIC bring-up or the
  `SS_TEST_EXT_PENDING` harness knob** (glue :228–230). Task A adds the third
  configurator (`SS_NW_HOST_IRQ`). The F5 atomicity contract (:213–230) governs any new
  asserting thread: `SetInterruptFlag` runs on host timer/pump threads — the existing
  contract already anticipates non-CPU-thread assertion + a TriggerInterrupt-style kick
  on the assert edge; **a missed kick is not safe** (no 60 Hz safety net on newworld) —
  the kick is part of Task A's contract. **(Rev 2 A5: `SheepExcExtSetPending` stores a
  single 0/1 word (`asserted ? 1u : 0u`, glue ~:260–268) — two independent sources
  (host + PIC) forwarding edges into it CLOBBER each other (a PIC deassert would drop a
  still-asserted host level and vice versa). Task A must either OR the sources at the
  forwarding site (per-source state, composed before the store) or land a
  mutual-exclusion assert (SS_NW_HOST_IRQ and SS_NW_PIC's bound output may not both
  forward — acceptable this milestone since SS_NW_PIC stays held, but the assert makes
  the latent clobber loud instead of silent).)**
- **Level-held semantics are BINDING for PIC sources** (W2-3 rev 2 C1): the hook never
  clears EXT pending; only the PIC's deassert edge does. **(Rev 2 A1: the HOST source is
  NOT a PIC source — it is pinned as deliver-once-per-assert-edge, its own latch beside
  the PIC seam; a THIRD semantics next to level-held-PIC and one-shot-DEC. C1 stays
  binding, untouched, for real PIC sources.)** For the host source, the deassert edge =
  `ClearInterruptFlag` bringing `InterruptFlags` to 0 (or the timer flag specifically —
  Task 0 Q-I4 pins the exact level predicate: all-flags vs per-flag). Retirement is
  HOST-owned; no guest IACK exists or is needed (W2S-2). **(Rev 2 A2: retirement is
  additionally gated by `HasMacStarted()` — `[0xcfc]=='WLSC'`, unset at our frontier —
  so pre-warm-start NO route deasserts the level; the deliver-once-per-edge latch is
  what makes a pre-warm-start host post safe-but-unconsumed instead of a storm.)**
- **The nesting livelock (rev 2 A1 — was "hazard"; now STATICALLY DETERMINED):** a bare
  level-held EXT source under the riser is a CLOSED LIVELOCK, no boot needed to know it:
  (i) the delivery restart PC is the not-yet-executed block start (the block-entry poll
  stores it before any body code runs — glue, the `restart PC = block-start` contract
  note ~:1054–1060), so the interrupted block makes no progress across a delivery;
  (ii) all SIX EE re-raise compose sites include `SheepExcExtPending()` (glue :952/
  :1373/:1432/:1462 + ppc-execute.cpp :1434/:1733) — every rfi/mtmsr EE rise re-delivers;
  (iii) the hook never clears EXT (C1 binding); (iv) retirement (OP_IRQ's
  `ClearInterruptFlag`) requires exactly the 68k progress the re-delivery storm prevents
  — AND is HasMacStarted-gated besides (A2). Therefore the damper default is INVERTED
  from rev 1: **the host source ships as a deliver-once-per-assert-edge latch (consumed
  at delivery, re-armed only by the next assert edge) — this is Q-I4's DESIGN
  deliverable, not a "find the natural masking" question.** "No damper" is not a
  candidate; DEC-piggyback (which inherits DEC's one-shot clear-on-delivery) is the
  alternative shape if Q-I1 re-pins the route.
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

- [x] **Entry gate first:** verify rows E1–E4 against the landed inj-s-fixes evidence
  (≤1 verification boot if its record is ambiguous). Red ⇒ HOLD, report.
  **GREEN 2026-06-12** (boot 1 [PROBE✓]; see Task 0 results below).
- [x] **(Q-I1 — THE FORK; blocks Task A) Does a delivery reach the 68k post?** Static
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
  delivery actually exits (probe 0x50314880 + 0x50325f00 + the post site). **(Rev 2
  hint, red-team A: the fallback pointer 0x50325f00 lies INSIDE the 0x3258e0–0x326380
  service-body range whose 9 tail-branches reach the 68k post (W2S-1, EE-CHAIN-RECON
  :433–434) — the frontier "fallback swallow" may itself BE a post-reaching service
  body. Start the static walk there.)**
- [x] **(Q-I2 — blocks Task A/B) The post's live targets:** probe `[0x68ffe67c]`
  (the pointer), `[[KDP+0x67c]]` resolved target value-before, `[0x68ffe674]` (CR mask)
  in the cold 68k world — retires W2L-R1. Pin: is the resolved target the DR emulator's
  pending word the 68k dispatch polls (and at what 68k-side check site)? **Deliverable:
  the watch address for Task B (the RESOLVED target — the watch instrument cannot follow
  pointers) + the expected written value (`level|0x8000` class).** *(Rev 2 B4 residue
  fallback: if the cold-world probe boot is inconclusive (pointer null/unmapped at the
  frontier), the [STATIC] fallback is the NK init writers of `[KDP+0x67c]` (the
  `bl 0x3254a0` ×2 init callers, W2S-1) — pin the target class statically, mark the live
  value a residue, and Task B's watch gate downgrades to probe-on-first-post.)*
- [x] **(Q-I3 — blocks Task B) The 68k consumption side:** from the resolved pending
  word to OP_IRQ — static on the patched via_int chain (**68k disassembly via
  `tools/m68k-dis.py`** — capstone-M68K mis-decodes `fe1f`-class A/F-line words,
  AGENT-CONTEXT): the 68k level-1 entry PC for `SS_PROBE_68K`, the via_int head (file
  0xef2c → its runtime 68k address), the fe6b OP_IRQ site (file 0xbbc8 / proc
  0x5000bbb8), and the CR-arm consumption (who reads the CR mask the post ORs in).
  **(Rev 2 A2) Add `[0xcfc]` to this question's probe set** — the warm-start word
  (`'WLSC'` = warm-started) pins WHICH OP_IRQ regime the frontier consumption runs in
  (pre-warm-start: pending-word clear at emul_op.cpp:812 only, d0=1, NO InterruptFlags
  consumption; post: the full :815–851 block). **Deliverable: the 68k probe-PC list +
  the expected visit order for one tick + the `[0xcfc]` regime verdict at the
  frontier.** *(Rev 2 B4 residue fallback: if the static via_int walk exceeds the
  bounded window, the fallback is the patch-site bytes themselves ([PATCH-fresh] file
  offsets above) + one `SS_PROBE_68K` boot on the via_int head — order pinned
  empirically, the full static chain recorded as residue.)*
- [x] **(Q-I4 — blocks Task A) Host-source mapping + retirement regime + the
  once-per-edge damper DESIGN (rev 2 A1/A2/A3 — reshaped):**
  (a) the exact level predicate (`InterruptFlags≠0` vs `INTFLAG_TIMER` bit) and the
  retirement story BY REGIME: the clear site is KNOWN (emul_op.cpp:841, inside the
  `HasMacStarted()` block at :814 — `[0xcfc]=='WLSC'`, macos_util.h:373–376); **probe
  `[0xcfc]` and pin the pre-warm-start retirement story explicitly** — at a
  pre-warm-start frontier NO route consumes `InterruptFlags`, so the deliverable states
  which acceptance outcomes are reachable pre-WLSC (delivery + NK post + pending-word
  handshake) vs deferred-to-post-warm-start (deassert pairing, TimerInterrupt, Ticks);
  (b) the assert-edge kick (TriggerInterrupt idiom from the timer thread — the F5
  contract's "missed kick is not safe");
  (c) **DESIGN the deliver-once-per-assert-edge damper** (the rev 2 A1 inversion — the
  bare level is a statically-determined livelock, see Codebase facts; "find the natural
  masking" is retired as the question): latch placement (its own word beside the PIC
  seam — the third semantics; C1 untouched for PIC sources), consume-at-delivery /
  re-arm-at-assert-edge rules, the A5 source-composition answer (OR vs
  mutual-exclusion), and the F5 atomicity shape for the new word. Expected
  deliveries-per-assert = **exactly 1** — that number, not N<16, is Task A's tripwire
  expectation;
  (d) **(rev 2 A3) the DEFER_NATIVE re-arm interaction**: the wake-up re-arm
  (glue, `EXC_NATIVE_REARM_CAP` 65536, re-polls counted per re-poll) × the pending
  window — with the once-per-edge latch the window is one delivery long, but the
  assert-to-delivery interval still re-polls in every native window it crosses.
  **Deliverable: the expected per-assert `deferred_native` count** (order-of-magnitude,
  from the D-7 window-length data; **post-`3cb3b16e` the counter is a re-poll meter —
  the bound is ≤ 65536 × kick-episodes, never an event count**) — Task B's deferral
  invariant is restated against THIS number, not E3's DEC-era class. A residue here =
  Task B's invariant carries an honest unknown, flagged.
  Any deviation from the once-per-edge default (e.g. a DEC-piggyback re-pin making the
  latch moot) requires its own written justification here.
- [x] **(Q-I6 — blocks Task A; rev 2 coordinator update, R-II6 sharpened) Should
  DEFER_NATIVE apply on the published-handler route at all?** The concrete consequence
  the inj-s-fixes review pinned: a latch landing in a PARKED native window
  (`[0x2810]=1` that never exits — the post-P-M5 frontier's observed regime) is
  **undeliverable by design** — kicks re-poll but cannot deliver; EXC_EXTERNAL routing
  inherits the exact same park unless the fence is narrowed for the published route.
  The fence's ORIGINAL rationale was the legacy KDP shim (unsafe to enter mid-native-
  excursion); the 2-SPR published route RETIRED that shim (M3A-ENTRY-TABLE, W2-4
  step 0). **Deliverable: a written verdict — (i) fence kept as-is on the published
  route (with the reason the 2-SPR handler still can't tolerate native-window entry:
  name the state it would corrupt), (ii) fence NARROWED for the published route
  (delivery permitted in native windows — the design + its riser/SRR1-compose
  implications), or (iii) fence kept + the host source must guarantee its asserts land
  outside parked windows (and HOW, given the timer thread can't see [0x2810]
  transitions).** Static analysis of the 2-SPR handler body vs native-window state is
  the primary tool; the verdict gates Task A because a kept fence + a parked frontier
  = zero deliveries regardless of route — the EXT acceptance would be unfalsifiable.
- [x] **(Q-I5) The baseline:** confirm E4's published frontier signature reproduces on
  one default boot of our own (`--expect` on its named lines); extend the
  characterization of R-II3 (PROGRAM slot=5 srr0=0x50324fec; the sc 0xffffffff growth)
  ONLY as far as one boot + one static window — it is the comparison baseline, not this
  milestone's quarry.
- [x] **Gate (the blocking-answer table):** addendum committed; every answer tagged
  ([STATIC]/[PROBE✓]/[PATCH-fresh]); **Task A blocks on Q-I1 (route verdict) + Q-I2
  (post targets) + Q-I4 (source mapping + nesting expectation) + Q-I6 (the
  DEFER_NATIVE fence verdict on the published route — rev 2 coordinator update); Task B
  blocks on Q-I2 + Q-I3; Task C blocks on nothing new** (it consumes A+B). A residue on a blocking
  answer invokes the stop-rule — no improvisation. Budget: the 6-boot cap binds over
  per-question allowances.

### Task A: the host EXT source + first live delivery (env-gated `SS_NW_HOST_IRQ`, default OFF) — size S/M, ≤4 boots

Files: `sheepshaver_glue.cpp`, `main_unix.cpp` (+ `timer.cpp`/`emul_op.cpp` only if
Q-I4 named a clear-site gap). Every new line newworld-profile + env-gated; gated off ⇒
byte-identical baseline (E4's class).

- [x] Land the gate: `SS_NW_HOST_IRQ` (default OFF) — at bring-up, when on:
  `SheepExcExtConfigure()`; `SetInterruptFlag`/`ClearInterruptFlag` newworld arm
  forwards the Q-I4-pinned level **through the Q-I4-designed once-per-assert-edge
  latch** (rev 2 A1: deliver-once-per-edge is the host source's PINNED semantics, not
  an optional damper) + the assert-edge kick (the F5 contract: single-copy-atomic,
  release/acquire, kick on assert). **(Rev 2 A5)** the forwarding site must compose
  sources, not clobber: OR with the PIC's bound output before any
  `SheepExcExtSetPending` store, or land the mutual-exclusion assert (host-irq and PIC
  may not both forward; one loud line). If Q-I1 re-pinned to DEC-piggyback: the same
  edge-latched source instead arms the Q-I1-pinned DEC-side post condition — the gate
  name, counters, and acceptance shape are unchanged.
- [x] Tripwire conformance: the runaway (N=16) and U13 starvation (N=64) counters must
  be live on this path; the once-per-edge latch lands WITH its written semantics comment
  at the site (C1 cited as the PIC-source contract this latch deliberately sits beside);
  expected tripwire arithmetic = Q-I4(c)'s exactly-1-per-assert.
- [x] **Probe sub-contract (PASS/FAIL), env-on (riser+published+host-irq):**
  (a) `[EXC] EXT pending ASSERTED` edges appear at host-post cadence; (b) **the first
  live EXT delivery ever**: `[EXC] EXT delivered #1: … entry=50314880` (or the Q-I1
  route's equivalent first-delivery line) with the entry probe conforming to the
  Q-I1/Q-I2 expected state (interrupted-context class, flags word); (c) exc= field 7
  (delivered_ext) > 0 at term-dump; (d) tripwires: ZERO tripwire lines — under the
  once-per-edge latch (rev 2 A1) the expectation is exactly 1 delivery per assert, so
  ANY runaway firing is a falsification (one-iteration rule); a starvation firing is
  checked against Q-I4(d)'s deferral expectation. Boot command shape:
  `SheepShaver/tools/ss-slot-boot.sh --label m7-taskA --timeout 60 --env 'SS_NW_EE_RISER=1 SS_NW_DEC_PUBLISHED=1 SS_NW_HOST_IRQ=1 SS_PROBE_PC=0x50314880;…' --expect 'EXT delivered #1;;EXT pending ASSERTED' --absent 'TRIPWIRE'`
  *(rev 2 A4: the absent-pattern is `TRIPWIRE` — the code emits
  `[EXC] TRIPWIRE: EXT re-delivery runaway …` (glue :1132); `RUNAWAY` uppercase appears
  in no output line and would vacuously pass. `TRIPWIRE` also catches the starvation
  tripwire — intended.)*
- [x] **Gated-off A/B (PASS/FAIL):** one boot without `SS_NW_HOST_IRQ` reproduces E4's
  baseline class byte-identically (no EXT lines, exc= 7th field absent, same park
  signature class).
- [x] Gates: task tier (`tools/gates.sh task`) + paravirtual disposition per the
  risk-based rule (SetInterruptFlag is SHARED — paravirtual e2e REQUIRED unless the arm
  is provably inside the profile gate; state which in the commit). Commit.

### Task B: the 68k consumption round trip + the Ticks rider (verify-don't-build) — size S/M, ≤5 boots

Evidence task: at most seed/probe-class source changes (counters allowed, inner gates).
Blocks on Q-I2/Q-I3.

**(Rev 2 A2/B5 — the regime split, BINDING for gate grading):** Q-I3's `[0xcfc]` verdict
partitions this task's gates. **Pre-warm-start-reachable (milestone-RED if they fail):**
the post observed; via_int chain entered; OP_IRQ EXECUTES; the `[KDP+0x67c]` pending-word
clear (emul_op.cpp:812 — OUTSIDE the HasMacStarted gate, so reachable at any frontier).
**Post-warm-start-only (CONDITIONAL gates — PASS/FAIL only if the boot reaches
`[0xcfc]=='WLSC'`; otherwise recorded as NOT-REACHED diagnostics, frontier-recordable
not milestone-RED):** `InterruptFlags` consumption, the assert/deassert pairing,
`TimerInterrupt`→`Execute68k`, and the Ticks rider (already diagnostic). The
deliver-once-per-edge latch (rev 2 A1) is what makes pre-warm-start posts
safe-but-unconsumed: each post delivers exactly once and the un-retired level cannot
storm. The milestone may ship green on the pre-warm-start set with the post-warm-start
set honestly NOT-REACHED — that outcome is acceptable and is recorded as the named
frontier (stop-rule 2's shape).

- [x] **Post observed (PASS/FAIL):** `SS_JIT_TRACE_RING=1 SS_JIT_WATCH_ADDR=<Q-I2
  resolved target, hex no-0x>` — the NK's post writes the pinned value class
  (`level|0x8000`) at delivery cadence; writer PC = the post site, not host code
  (**the fake-poke staying fenced is itself a gate: zero host-side writers**).
- [x] **via_int → OP_IRQ (PASS/FAIL):** `SS_PROBE_68K=<Q-I3 entry PCs>` fires in the
  pinned order; OP_IRQ executes (EMULOP counter / one-shot counter at the fe6b site);
  the pending-word clear observed (the :812 write — pre-warm-start-reachable).
  *Conditional sub-gate (post-warm-start only, per the regime split above):*
  `InterruptFlags` consumed — the host level retires after OP_IRQ (assert edges paired
  with deasserts; no monotonic pending).
- [x] **TimerInterrupt → Execute68k (PASS/FAIL — conditional, post-warm-start only per
  the regime split):** the task-proc call observed
  (`SS_PROBE_68K=0x5000bbba` per the +2 fetch idiom — the probe that NEVER fired in
  recon boots 1/2) and RETURNS (no recon-Q1 crash class; E1's staging carrying the
  load).
- [x] **The item-4 rider — Ticks (DIAGNOSTIC headline, recorded not gated):**
  `[0x168]` probe / watch on 0x168: does `Ticks` (0x16a) advance under sustained
  deliveries? If yes — the recon's "falls out for free" verified; record the rate vs
  60 Hz. If no — capture the exact break link (post seen? OP_IRQ ran? TimerInterrupt
  walked an empty tmDescList? — the instime-fix trap-table state is the first suspect)
  and name it; **a broken link here is a finding, not a license to build — the
  one-iteration rule applies if a pinned Q-I3 contract was falsified, else it is the
  next frontier.**
- [x] **Invariant carry-over (PASS/FAIL):** nest drift `[0x2818]` recorded (the deferred
  XLM_IRQ_NEST item — drift is EXPECTED and tolerated this milestone, but a NEW drift
  rate class is a finding); no reset ring; **deferred_native bounded against Q-I4(d)'s
  per-assert expectation (rev 2 A3 — NOT E3's DEC-era class; the host source crosses
  native windows on a different cadence, and post-`3cb3b16e` the counter is a re-poll
  meter — bound ≤ 65536 × kick-episodes, never an event count; the semantic cutoff
  applies to every exc= comparison here)**; DEC
  cadence stays healthy (mtspr_dec single digits per D-7, riser-on actual 25 per the
  landed E3).
- [x] Gates: task tier (evidence task — smoke set per the stated-reason rule if zero
  source lines changed). Addendum results section. Commit. **(As run: ZERO source lines
  changed — doc-only commits, no gates per the stated-reason rule. Verdicts in "Task B
  results" below; the round trip dies at the NK post's level test — the re-grade's
  staging-decision branch applies, STOPPED for sign-off.)**

### Task C: acceptance + the flip cluster (flip LAST, revert-on-red) + dispositions — size S, ≤4 boots

- [ ] **PASS/FAIL gates first, env-on:** (a) full gates (`tools/gates.sh full`);
  (b) Task A's delivery sub-contract re-asserted; (c) Task B's post/OP_IRQ/retirement
  gates re-asserted; (d) gated-off A/B byte-identical to E4's class; (e) tripwires
  quiet per Q-I4's expectation.
- [ ] **Fix budget:** telemetry/capture commits free; at most ONE small in-scope fix
  iteration per falsified contract, full gates re-run after any fix. Second
  falsification of the same contract ⇒ stop-rule.
- [ ] **(Rev 2 B2) Pre-flip checklist — two wave2 carry-forwards land BEFORE the flip
  commit:** (1) the run-exc.sh / `SS_NW_DEC_PUBLISHED` gate-ON-without-`SS_EXC_ENTRY`
  guard (wave2 W2-4 step-0 review note P2: gate-on aims the 2-SPR shim at the unmapped
  legacy default entry — a guard or loud comment in `SheepShaver/jit-test/run-exc.sh` +
  the gate site, REQUIRED before any default-ON state exists); (2) the stub=1
  6-words-not-4 P3 note (inert today only because fresh-process RAM is zero — re-verify
  or fix before the flip makes the stub path default-reachable). Both verified-or-landed
  ⇒ checklist green; either open ⇒ the flip HOLDS.
  **(Task-A-review carry-forwards, added 2026-06-12 per coordinator relay during
  Task B — two more named checklist items:)** (3) **the latent lost-edge race (P2,
  main_unix.cpp:2850–2851)**: ClearInterruptFlag's zero-check and Deassert are
  non-atomic — the interleaving (Clear reads flags==0 → Set: or+Assert+kick → Clear:
  Deassert) retires a fresh edge; the kick then finds Pending()==0 and one post is
  silently lost until the next post. Unreachable today (OP_IRQ's Clear is
  HasMacStarted-gated) but fix-or-verify before the flip; fix shape: re-check
  `InterruptFlags!=0` after Deassert and re-Assert+kick. (4) **tripwire-counter
  masking note (P3)**: `SheepExcHostIrqAssert` resets the shared episode counters
  `exc_ext_delivs_this_assert`/`exc_dec_delivs_while_ext` — with host+PIC co-armed a
  host edge can mask the PIC runaway tripwire; host-only flip may record it as
  accepted-with-note, any co-armed default state requires the fix.
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
  opt-outs, interaction with SS_TEST_EXT_PENDING/SS_EXC_ENTRY third field; the host
  source's once-per-edge semantics named next to C1's level-held).
- [ ] **(Rev 2 B6) Gated-off-ship disposition (REQUIRED if Task C reverted the cluster
  or never flipped):** for EACH cluster member (`SS_NW_HOST_IRQ`, `SS_NW_EE_RISER`,
  `SS_NW_DEC_PUBLISHED`) record in DIAGNOSTICS: current state, opt-out, and the
  retire-or-retain criterion (what evidence flips it / what evidence deletes it) + ONE
  ROADMAP follow-on row owning the un-flipped cluster. The `SS_NW_*` gate census now
  stands at **16 with this plan** (15 in-tree: DEC_PUBLISHED, DR_R0_INVARIANT, EE_RISER,
  FE1F_SURFACE, MM_POOL, MM_SWITCH, MODEL, NO_SCC, PIC, PIC_FORCE, SC_SURFACE,
  SYNTH_ENTRY, TM_TASK_FORCE, TM_TRAPS, TRAMPOLINE) — flag the census to the coordinator
  as a re-score #3 input (gate-debt is now a strategy-level cost).
- [ ] **(Rev 2 B1) The W2-4 supersession table** (lands in the wave2 plan's W2-4 DONE
  note AND is mirrored in this plan's close-out commit message): one row per unticked
  W2-4 checkbox → absorbed-here (section ref) or named-deferred (ROADMAP row):
  | W2-4 item | Disposition |
  |---|---|
  | tm_task retirement A/B | **MOOTED** by the `2ff7765f` verify-EXPECTED-first guard (the 0x505bb060 slide fix) — record as such, no retirement A/B owed |
  | via_int cluster disposition (rom_patches :3474–3511) | **ABSORBED here** — Q-I3/Task B's evidence of the patched chain consuming the NK post IS the disposition input; the written verdict lands in Task C dispositions |
  | XLM_IRQ_NEST ownership | **ABSORBED here** — Task C disposition (1) |
  | polled-trampoline + SDL_PumpEvents decisions | **NAMED-DEFERRED** — ROADMAP row (not touched by this plan's chain; record the default: leave / keep+decouple) |
  | Ticks acceptance (the W2-4 headline) | **ABSORBED here** — Task B's item-4 rider (diagnostic, regime-split per rev 2 A2) |
  | the reserved riser/published flip | **ABSORBED here** — Task C's cluster flip |
- [ ] **(Rev 2 B1) M3A-ENTRY-TABLE cleanup:** the dual-mode `interrupt_entry` row + the
  legacy-KDP-shim-retirement residue (M3A-ENTRY-TABLE.md "Residue") get their post-flip
  state recorded (flipped ⇒ the legacy row is historical, schedule the retirement
  follow-on; not flipped ⇒ row unchanged, pointer here).
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
   tunnel material (the M5/M6 coupling precedent). **(Rev 2 A2 grading note: the
   HasMacStarted-gated outcomes — flag retirement, TimerInterrupt, Ticks — are NOT this
   wall pre-warm-start; they grade per Task B's regime split.)** (Rev 2 B6) A stop here
   ships gated-off — Task Z's gated-off-ship disposition (per-member state/opt-out/
   retire-or-retain in DIAGNOSTICS + the ROADMAP follow-on row) is then REQUIRED, not
   optional.
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

Two rounds (A technical, B process/scope), both verdicts SOUND-WITH-FIXES, folded below.

## Rev 2 (red-team fold, 2026-06-12) — BINDING amendments; where in conflict with the body, Rev 2 overrides

> **Header note — the fork leans differently than rev 1:** A1 (a bare level under the
> riser is a closed livelock, statically determined) + A2 (NO route retires
> `InterruptFlags` pre-warm-start — OP_IRQ's consuming block is `HasMacStarted()`-gated)
> together change the rev-1 picture. The level-held EXT source as drafted could not have
> worked at this frontier regardless of Q-I1's route verdict; and Task B's "round trip"
> as drafted (delivery → … → OP_IRQ consumes → deassert → TimerInterrupt → Ticks) is
> unreachable before `[0xcfc]=='WLSC'` by construction. **The re-stated Q-I1 decision
> rule:** the host source is a **deliver-once-per-assert-edge latch on EITHER route**
> (its own word beside the PIC seam — a third semantics; C1 stays binding for real PIC
> sources). With the storm objection thus removed from both candidates, the route
> verdict remains evidence-driven exactly as drafted: if the EXT body (directly, via the
> 0x50325f00 fallback — which sits INSIDE the W2S-1 service-body range, see the Q-I1
> hint — or via the shared flag-tree) reaches the 68k post for a from-emulator delivery,
> edge-latched EXT stands; else the same question of the DEC exit tree ⇒ DEC-piggyback
> (which inherits DEC's one-shot semantics and makes the separate latch moot); else
> stop-rule trigger 1. **And the regime question is confronted, not dodged:** this
> milestone does NOT target the post-warm-start regime — it targets
> delivery-through-the-guest's-own-chain at the current frontier, where the
> once-per-edge latch makes pre-warm-start EXT **safe-but-unconsumed**: each post
> delivers exactly once, the NK post + via_int + OP_IRQ-entry + pending-word handshake
> are gateable (milestone-RED on failure), while flag retirement / TimerInterrupt /
> Ticks are conditional gates — PASS/FAIL only if a boot reaches WLSC, otherwise
> NOT-REACHED diagnostics and the named next frontier (Task B regime split; stop-rule 2
> note). Shipping green-pre-warm-start with the consumption half honestly NOT-REACHED is
> an acceptable milestone outcome; shipping a storm or a fake retirement is not.

**Round A (technical) — dispositions:**
- **A1 (BLOCKER — folded, default inverted):** the level-held host source is a CLOSED
  LIVELOCK: restart PC = not-yet-executed block start (glue contract note ~:1054–1060;
  A cited :962–967 — content verified, line drift only), all six EE re-raise compose
  sites include `SheepExcExtPending()` (glue :952/:1373/:1432/:1462 +
  ppc-execute.cpp :1434/:1733 — verified by grep), the hook never clears EXT (C1), and
  retirement needs the 68k progress the storm prevents (plus A2's gate). Folded:
  Codebase-facts livelock entry; Q-I4(c) is now "DESIGN the once-per-edge damper";
  Task A pins the latch as the host source's semantics; tripwire expectation = exactly
  1/assert.
- **A2 (MAJOR — folded):** OP_IRQ's InterruptFlags-consuming block (incl.
  `ClearInterruptFlag(INTFLAG_TIMER)` at emul_op.cpp:841) is gated by `HasMacStarted()`
  (emul_op.cpp:814; `[0xcfc]=='WLSC'`). **Citation correction (mine): the SheepShaver
  definition is `SheepShaver/src/include/macos_util.h:373–376`, not :278–281 — that is
  the BasiliskII copy's line number. Substance verified.** Also verified (load-bearing
  for the regime split): the `[KDP+0x67c]` pending-word clear at emul_op.cpp:812 is
  OUTSIDE the gate — the 68k handshake IS pre-warm-start-reachable. Folded: `[0xcfc]`
  probes in Q-I3 + Q-I4(a); the explicit pre-warm-start retirement story; Task B's
  regime split.
- **A3 (MAJOR — folded):** the DEFER_NATIVE re-arm (EXC_NATIVE_REARM_CAP=65536,
  re-polls counted per re-poll — verified in glue) × a never-clearing level = designed
  busy-poll explosion. Mitigated by the A1 latch (the window is one delivery long) but
  the assert→delivery interval still re-polls; folded as Q-I4(d): expected per-assert
  `deferred_native` count is a deliverable, and Task B's deferral invariant is restated
  against that number, not E3's DEC-era class.
- **A4 (MINOR — folded, verified):** the emitted line is
  `[EXC] TRIPWIRE: EXT re-delivery runaway - …` (glue :1132 at rev 2); `--absent
  'RUNAWAY'` matches nothing and vacuously passes. Task A's boot command now uses
  `--absent 'TRIPWIRE'`.
- **A5 (MINOR — folded, verified):** `SheepExcExtSetPending` stores a single 0/1 word
  (glue ~:260–268; A cited :255–257, drift only) — host + PIC sources clobber. Task A
  ORs sources at the forwarding site or lands the mutual-exclusion assert; Codebase
  facts amended.
- **A6 (MINOR — folded):** INTFLAG_TIMER clear site precision (visible at :841; the
  open item is the gate) — Authoritative-inputs row corrected; glue EXT-branch line
  refs updated to ~:1095–1155 with a re-verify-before-editing note.
- **A supportive note (folded as the Q-I1 hint):** verified — `[KDP+0x5b0]` fallback
  0x50325f00 ∈ 0x3258e0–0x326380, the service-body range whose 9 tails branch to the
  68k post (EE-CHAIN-RECON.md :433–434, W2S-1). The "fallback swallow" may itself be a
  post-reaching body; the static walk starts there.

**Round B (process/scope) — dispositions:**
- **B1 (BLOCKER — folded):** W2-4 supersession table added to Task Z (every unticked
  W2-4 checkbox → absorbed-here ref or named-deferred ROADMAP row; tm_task retirement
  recorded as MOOTED by `2ff7765f` — commit verified in-tree). M3A-ENTRY-TABLE
  dual-mode-row/KDP-shim-retirement cleanup added to Task Z.
- **B2 (BLOCKER — folded, verified):** both wave2 carry-forwards confirmed at
  2026-06-11-wave2-interrupt-chain.md :449–451 (P2 run-exc.sh guard, P3 stub=1 six
  words); now explicit Task C pre-flip checklist items — either open ⇒ the flip HOLDS.
- **B3 (MINOR — folded):** entry-gate partial-green policy stated: HOLD-is-total,
  deliberately (rationale recorded at the gate); the ≤1 verification boot must
  co-schedule ALL ambiguous rows, more ⇒ HOLD.
- **B4 (MINOR — folded):** per-Q residue fallbacks added to Q-I2 (static init-writer
  pin + watch-gate downgrade) and Q-I3 (patch-site bytes + one SS_PROBE_68K boot);
  `tools/m68k-dis.py` cited in Q-I3 (path verified: repo root `tools/`, NOT
  `SheepShaver/tools/`).
- **B5 (MINOR — folded):** Task B reds named: pre-warm-start-reachable set =
  milestone-RED; HasMacStarted-gated set = conditional gates, NOT-REACHED ⇒
  frontier-recordable (the regime-split paragraph).
- **B6 (MAJOR — folded):** gated-off-ship disposition added to Task Z + stop-rule 2
  (per-member state/opt-out/retire-or-retain in DIAGNOSTICS + ROADMAP follow-on row);
  `SS_NW_*` census verified by grep: **15 in-tree + SS_NW_HOST_IRQ = 16 with this
  plan**, flagged as a re-score #3 input.

**Coordinator update (2026-06-12, post-fold — inj-s-fixes LANDED, review APPROVE
`34d3d441`/`3cb3b16e`/`bf571e26`; commits verified in-tree):**
- **deferred_native semantic cutoff at `3cb3b16e` (supports A3):** the counter is now a
  re-poll meter (one parked window can contribute 65536+), not an event count. The
  cutoff commit is now stated at every exc=-tuple comparison site: entry-gate E3, the
  Codebase-facts tuple bullet, Q-I4(d)'s expectation (bound ≤ 65536 × kick-episodes),
  and Task B's deferral invariant.
- **Q-I6 added (BLOCKS Task A; R-II6 sharpened):** should DEFER_NATIVE apply on the
  published-handler route at all? Latches landing in a parked native window
  ([0x2810]=1 never exiting) are undeliverable by design — kicks re-poll but cannot
  deliver — and EXT routing inherits the same park unless the fence is narrowed; the
  fence's original rationale (the legacy KDP shim) is retired on the 2-SPR route.
  Verdict options (keep-with-reason / narrow / keep-plus-assert-placement-guarantee)
  pre-stated; the blocking-answer table updated.
- **Entry gate re-graded LANDED with actuals:** E1 staged (`34d3d441`); E2 = 44.4 s
  default boot to the PROGRAM#5 srr0=0x50324fec park, no P-M5 SIGSEGV; E3 = riser-on
  delivered_dec=3, pending=0 drained, mtspr_dec=25; E4 = the `bf571e26` close-out.
  Task 0's entry check verifies against these actuals.

**Falsified findings:** none — every finding's substance verified against source/docs.
Corrections found while verifying (recorded above): A2's macos_util.h line number is
the BasiliskII file's; A1/A5's glue line numbers drifted (content exact); plus one fact
the reviews missed that STRENGTHENS the plan: the :812 pending-word clear being outside
the HasMacStarted gate is what keeps a pre-warm-start round trip partially gateable.

**Rev-2 self-review:** all six A + six B findings have an in-place edit AND a
disposition row; no body text still instructs the rev-1 behavior (the fork paragraph,
nesting bullet, Q-I4, Task A contract, Task B invariants, Task C flip, Task Z, and
stop-rule 2 all carry rev-2 markers); the boot-command/`--absent` fix is in the
executable command line itself, not only the prose; budgets, gate tiers, and the
stop-rule triggers are unchanged in number and binding. Consistency check: the
once-per-edge latch appears with the same semantics in Codebase facts, Q-I4(c), Task A,
and the Rev 2 header; the regime split appears identically in Q-I4(a), Task B, and
stop-rule 2.

## Task 0 results (2026-06-12, label inj-task0)

Full blocking-answer table + evidence detail: **INTERRUPT-INJECTION-RECON.md "M7 Task 0 —
EXT/DEC post-path recon"**. Boots: 2 of ≤6 (slot0 20260612-021607.5156 default,
20260612-021835.5366 EXT-forced via `SS_NW_PIC=1 SS_NW_PIC_FORCE=1 SS_SCC_RX_INJECT`).
No pinned contract falsified.

- **Entry gate E1–E4: GREEN** ([PROBE✓] E1 staged pair exact; E2/E4 baseline reproduced —
  PROGRAM#5 srr0=50324fec, sc 0xffffffff x233, blocks=7354, no SIGSEGV; E3 per landed record
  + riser-on consistency).
- **Q-I1 ROUTE VERDICT: EXT** — the EXT body via the [KDP+0x5b0] fallback 0x50325f00
  (itself a PIC-IACK-reading service body) reaches the 68k post 0x3254e0 on ALL exit legs;
  live-proven: **the first EXT deliveries ever** (entry=50314880), post body reached with
  r7 bit 0x00200000 SET. DEC-piggyback rejected (DEC body never invokes the post family).
  **NEW BLOCKER FOR TASK B's post-value gate (R-II7)**: the posted level r28=0 at this
  frontier (IACK unmapped → pic i:0; lowmem 0x3f00 vector→level table zero) — the post
  fires but writes 0. Coordinator re-scope options recorded in the addendum.
  **A1 livelock proven in vivo**: 68,657,433 deliveries/40 s under the level-held source,
  runaway tripwire, guest frozen — the once-per-edge latch is the only viable host shape
  (DEC not starved: 3395 interleaved).
- **Q-I2**: resolved target = **0x68fff070** (ECB+0x70); Task B watch
  `SS_JIT_WATCH_ADDR=68fff070`; CR masks [KDP+0x674]=0x00e00000 / [KDP+0x678]=0xff9fffff;
  W2L-R1 retired. Plus the deferred-post staging slot [KDP-0x43c]/[KDP-0x440] (drained at
  world-restore 0x324720) — the NK posts from BOTH context classes.
- **Q-I3**: 68k chain [PATCH-fresh]: level-1 @0xec50 jmp→0xef20 → via_int head 0xef2c
  (moveq #2) → dispatch table[2]→lowmem $192 → 60 Hz proc 0xbbb8 → **fe6b @0xbbc8 →
  `addq.l #1,$16a`** — pre-WLSC OP_IRQ returns d0=1 so **Ticks++ is pre-warm-start-reachable**.
  Probe list (+2): 0x5000ec52, 0x5000ef22, 0x5000bbca. **[0xcfc]=0xffffffff ⇒ PRE-warm-start
  regime** [PROBE✓]. (Correction: via_int3 site is 0xec50, not the recon's 0x16dd6.)
- **Q-I4**: level predicate InterruptFlags≠0; pre-WLSC NO retirement (explicit story in the
  addendum); kick=TriggerInterrupt on assert edge; damper = dedicated `host_irq_latch` word,
  **OR-composed at the poll site** (never writes exc_ext_pending_flag — A5 answered, C1
  untouched); exactly-1-per-assert tripwire arithmetic; deferred_native bound
  ≤65537 × kick-episodes (≈0–10² per assert if Q-I6's narrow lands).
- **Q-I6 VERDICT: (ii) fence NARROWED for the published route** — the KDP-shim rationale is
  retired on 2-SPR; nothing the published round trip touches is corruptible (offset-matched
  save/restore verified); kept fence + the parked frontier ([0x2810]=1, EE=1, pending) =
  undeliverable by design; option (iii) impossible. Shape: route-aware skip of the run_mode
  defer for published entries only; legacy DEC keeps the fence.
- **Q-I5**: baseline reproduced; R-II3 pinned to one window — the park is an NK `sc 0x2e`
  polling loop containing the slot-5 placeholder word 0x0fff0005 at 0x324fec.
- **Flagged to the coordinator**: (1) R-II7 — Task B's "post observed = level|0x8000" gate
  is unsatisfiable at this frontier as written; re-scope (writer-PC/write-event grading, or a
  justified level-source staging decision) BEFORE Task B dispatch. Task A (delivery machinery,
  latch, narrow fence) is NOT blocked by it. (2) The canonical rom901.bin dump is stale
  (pre-f808a7fb, lacks the 68k patches); re-baseline is the coordinator's call — fresh dump
  at /tmp/rom901_fresh_task0.bin, [PATCH-fresh] bytes recorded in the addendum.

**GO for Task A** (Q-I1/Q-I2/Q-I4/Q-I6 pinned; design inputs complete). **Task B holds for
the R-II7 re-scope decision** on the post-value expectation; its Q-I2/Q-I3 inputs are pinned.

### Coordinator re-grade of Task B (2026-06-12, post-Task-0) — the R-II7 decision

Task B's "post observed = level|0x8000" PASS/FAIL gate is **re-graded to write-EVENT
grading**: PASS = a write to the resolved target 0x68fff070 from the pinned post-family
writer (the 0x3254e0 tails), following an EXT delivery, observed via
`SS_JIT_WATCH_ADDR=68fff070` — falsifiable and satisfiable at this frontier. The written
VALUE is recorded as a diagnostic, not gated (R-II7: the level derives from PIC IACK +
the lowmem 0x3f00 table, both guest-init-owned and zero pre-init — a zero post is the
correct behavior of the real chain at this frontier, not a failure).

**Level-source staging is NOT pre-authorized.** It becomes a named follow-on decision
ONLY if Task B's consumption probes prove the 68k chain tests-and-skips on a zero level
(i.e. the round trip dies exactly at the level test). In that case: capture the evidence,
write the staging proposal as a dated addendum (which word, who owns it on a real boot,
why staging is not the fake-poke pattern), and bring it back for sign-off — do not
improvise it in-task. If the 68k chain consumes regardless of level (the OP_IRQ d0=1
pre-WLSC arm), no staging is needed and the gate stands as re-graded.

## Task A results (2026-06-12, label m7-taskA) — DONE, all sub-contracts green

Commits: `b124556f` (implementation) + the acceptance/docs commit. Boots: **2 of ≤4**
(slot0 `20260612-024837.9872` env-on, `20260612-025049.10064` gated-off A/B). Gates:
task tier 5/5 PASS (incl. plain test-jit 353/353 authoritative, e2e-test 122 passed);
exc-vector lane 14/14 with the new H9. No pinned contract falsified.

- **As landed:** the Q-I4(c) damper = dedicated `exc_host_irq_latch`
  (sheepshaver_glue.cpp, after the W2-3 EXT seam): assert = SetInterruptFlag's newworld
  arm → `SheepExcHostIrqAssert()` (atomic exchange-1; returns edge) + TriggerInterrupt
  on exactly the 0→1 edges; consume = atomic exchange-0 in the hook's EXT-delivery
  branch; deassert = `ClearInterruptFlag` at `InterruptFlags==0`. A5: OR-composition at
  the poll site + all six EE re-raise compose sites; neither source writes the other's
  word (C1 untouched). The Q-I6 (ii) fence narrowing = route-aware skip of the
  run_mode re-ask in `deliver_pending_dec_exception` (`route_published` =
  dec_pending ? SS_NW_DEC_PUBLISHED : external_entry≠0); corruption-free argument
  re-verified at the shim sites before landing (2-SPR = SPRG1/SPRG2 only; the legacy
  KDP shim keeps the fence). exc_core untouched.
- **(b) FIRST LIVE HOST-SOURCED EXT DELIVERY:** `[EXC] EXT delivered #1:
  restart=50318018 srr1=00009040 msr=00001040 -> entry=50314880`, immediately after
  `[EXC] EXT pending ASSERTED (host-irq latch, edge #1)`; entry probe [PROBE✓]
  r1=0x68ffe000 (KDP) r6=0x68fff000 (ECB) — the NK EXT context. BOOT-VERDICT PASS
  (`--expect 'EXT delivered #1;;EXT pending ASSERTED' --absent 'TRIPWIRE'`: 2/2, 0
  violations).
- **(d) Exactly-once-per-assert-edge PROVEN:** `[EXC] host-irq: edges=1 consumed=1
  deasserts=0 pending=0` — delivery count == assert-edge count; ZERO tripwire lines.
  (a) cadence note, honest: ONE host post in the 60 s window — pre-WLSC nothing
  re-arms `wakeup_time` after an expiry post (TimerInterrupt is HasMacStarted-gated),
  so the frontier cadence is one INTFLAG post per guest prime. H9 pins the same
  once-per-edge consume in the harness lane per-vector.
- **(c)** exc= 7th field live: `exc=4340/0/156/0/297/4/1` (HB 50 s).
- **DEC chain under the narrowed fence — regime change, not regression:**
  delivered_dec=4340@50 s (~87/s, all `(2-SPR)` published-route), **deferred_native=0**
  (re-poll meter: narrowing removed the parked-window episodes entirely — within
  Q-I4(d)'s ≈0–10² bound), mtspr_dec=10252 ≈ **2.0/delivery** (the NK pair
  7fffffff park + 0x32e10 re-arm @0x503230d4/dc, small/mid value classes, no
  zero/tiny storm). The "mtspr_dec single/low-double digits" expectation was the
  E3-class regime (3 deliveries total); per-delivery ratio is the honest unregressed
  metric. sc/program at baseline class (PROGRAM #1–4 slot=8 identical; sc selectors
  baseline family).
- **Gated-off A/B byte-identical to E4's class:** blocks=7354 (exact), PROGRAM#5
  srr0=50324fec word=0fff0005 slot=5, sc census identical (16 distinct, 0xffffffff
  x233 / 0xfffffffe x17), ZERO host-irq/EXT output, 6-field tuple. BOOT-VERDICT PASS.
- **Recorded diagnostic (not a gate):** the env-on boot no longer reaches the
  PROGRAM#5 park — it runs an NK spin (~82 M blocks/s jNK, comp frozen at 6991, DEC
  cadence live, sc census growing). The riser+published+host-irq+narrowed-fence
  config is a NEW frontier class; named for Task B/C baselining.
- **Task B entry conditions (per the re-grade): SATISFIABLE** — EXT deliveries are
  live and on-demand (one per host edge), the Q-I2 watch target 0x68fff070 +
  write-event grading stand, deliveries interrupt native-window contexts
  (restart=50318018/mirror-region) where the post's r7-bit-0x00200000 precondition
  was proven, and [0xcfc] remains pre-WLSC (regime split applies as written).

## Task B results (2026-06-12, label m7-taskB) — VERIFY-DON'T-BUILD, zero source changes; the round trip dies at the NK post's LEVEL TEST → re-grade staging branch, STOPPED for sign-off

Boots: **4 of ≤5** (slot0 `20260612-025958.16193` watch+log-probes, `20260612-030445.16716`
linear-probe CRASH, `20260612-030701.16951` linear-probe CRASH, `20260612-030924.17181`
the clincher — fallback-counter watch discriminator). All env-on
(`SS_NW_EE_RISER=1 SS_NW_DEC_PUBLISHED=1 SS_NW_HOST_IRQ=1`). Full evidence + the
level-source staging proposal: INTERRUPT-INJECTION-RECON.md "M7 Task B — the consumption
round trip". No pinned contract falsified; one instrument anomaly named (below).

**THE TICKS VERDICT: Ticks did NOT move** (`[0x168]`=0 at delivery, watch on 0x168 silent
after the three boot-init writes, both watch boots, 59 s each). **The break link, pinned
to the instruction: the NK post's level test at r28=0** — at 0x503254ec `beq cr7`
(level==0) skips the `ori r28,r28,0x8000` AND the `[KDP+0x674]` CR-mask load (r31 stays
0), the `sth r28,0(r23)` then stores 0x0000 over 0x0000, and `bgt cr7` not-taken runs
`and r13,r13,[KDP+0x678]` — at level 0 the post actively CLEARS the emulator-CR interrupt
bits instead of setting them [STATIC, fresh dump]. The signal to the DR emulator is
structurally null; everything downstream (via_int, OP_IRQ, pending-word clear,
TimerInterrupt, Ticks) is unreachable at this frontier as the REAL chain's correct
behavior pre-guest-init (R-II7), not a bug.

**Per-gate verdicts (re-graded gate set):**
- **Post observed (write-EVENT grading) — PASS.** Boot 4 sequence, stderr-adjacent:
  `EXT pending ASSERTED (edge #1)` → `EXT delivered #1 … entry=50314880` → entry probe
  `[0x68ffee80]=1` → `[WATCH] pc=50325f38 addr=68ffee80 1→2 (record #3629854, sp=68ffe000)`
  — the [KDP+0x5b0] fallback service body entered immediately after the host-sourced
  delivery, and its entry counter incremented for NOTHING ELSE all boot (init #4529 →
  delivery #3629854: zero increments across thousands of DEC/SC/PROGRAM deliveries —
  uniquely EXT-coupled). All fallback exit legs converge on the post 0x3254e0 (Q-I1
  [STATIC]); the gate-fail skip leg 0x5032562c had ZERO visits in all 4 boots; the
  post-block entry state observed live: r23=0x68fff070, r28=0, r7=0x00a80000 (bit
  0x00200000 SET). **Instrument note, honest:** the graded watch on 68fff070 itself is
  structurally BLIND here — the sth stores 0x0000 over 0x0000 and the watch is a change
  detector; the write event is carried by the fallback-counter discriminator + the
  static all-legs-converge fact + Task 0's live post-body proof. **Zero host-side
  writers of 68fff070 (fake-poke fence): PASS** — no [WATCH] hit on it from any code,
  host or guest, either watch boot.
- **Level diagnostic value (recorded, not gated): 0** — `[0x68fff070]`=0 before and
  after; r28=0 at the post (PIC IACK unmapped + lowmem 0x3f00 table zero, per R-II7
  this is the correct pre-init behavior of the real chain).
- **via_int → OP_IRQ — DID NOT RUN (graded per the re-grade, NOT milestone-RED):**
  `SS_PROBE_68K=0x5000ec52:8` (boot 1) and `0x5000bbca:8` (boot 4): **0 matches in
  59 s each** (armed-line confirmed; nested-execute blindness noted but no host
  Execute68k excursion runs via_int here). The chain "tests-and-skips on a zero
  level" — strictly: the NULL is established one link upstream, at the NK post's
  level test, so the 68k chain's trigger inputs (pending halfword, CR bits) are
  never raised and the via_int chain is never entered. This IS the re-grade's
  "round trip dies exactly at the level test" condition → **the level-source staging
  proposal is written as a dated addendum in INTERRUPT-INJECTION-RECON.md and this
  task STOPPED for sign-off — no staging improvised** (zero source lines changed).
- **TimerInterrupt → Execute68k — NOT-REACHED** (conditional gate; `[0xcfc]`=0xffffffff
  pre-WLSC in every boot [PROBE✓] — the regime split applies as written; additionally
  unreachable pre-level-fix per the break link above).
- **Invariant carry-over — PASS:** ZERO tripwire lines all 4 boots (`--absent
  'TRIPWIRE'`); exactly-once re-proven (`host-irq: edges=1 consumed=1 deasserts=0
  pending=0`, boots 1+4); deferred_native=0 (crash-dump tuples, boots 2/3 — within
  Q-I4(d)'s ≈0–10² bound); nest drift `[0x2818]`=-59/-60 at delivery (expected class,
  no new rate class); DEC cadence healthy in the ring-slowed regime — mtspr_dec=27–31
  per 6–7 expiries, the same 7fffffff@503230e4/ffffffff@503230e8 NK park+re-arm pair
  as Task A's 2.0/delivery class, no zero/tiny storm; sc/program census IDENTICAL to
  the E4 baseline class (16 distinct, 0xffffffff x233 / 0xfffffffe x17, PROGRAM#1–4
  slot=8 + #5 srr0=50324fec slot=5).

**Chain-walk table (the milestone's state after Task B):**
| Link | State | Evidence |
|---|---|---|
| host edge → EXT delivery | **LIVE** | Task A + boots 1/4, exactly-once |
| delivery → fallback service body 0x325f00 | **LIVE** | [KDP+0xe80] 1→2 watch, delivery-adjacent, uniquely EXT-coupled |
| fallback → post body 0x3254e0 | **LIVE** (Task 0 live + all-legs [STATIC]; skip-leg 0 visits) | post entry r23=68fff070 r7-bit SET |
| post level test r28 | **THE BREAK: r28=0** → sth 0x0000 (no-op value) + CR bits CLEARED | [STATIC] + R-II7 + level diag 0 |
| pending word / CR → DR dispatch poll | dead input (null signal) | — |
| via_int 0xef2c → OP_IRQ fe6b | NEVER ENTERED | PROBE68K 0 matches ×2 |
| pending-word clear :812 / OP_IRQ d0=1 | NOT-REACHED | — |
| InterruptFlags retirement / TimerInterrupt / Ticks | NOT-REACHED (pre-WLSC conditional + upstream break) | [0xcfc]=ffffffff |

**Anomaly (named, not a falsification): `SS_PROBE_LINEAR=1` env-on boots crashed 2/2**
(boot 2 SIGTRAP guest pc=0x50460c00 DR-dispatch fetch, pre-assert; boot 3 SIGSEGV host
pc in JIT cache, guest pc=0x500e708c, wild ea guest 0x55590000, after 5 DEC deliveries
restarting the same block) vs 0/2 crashes for the identical config without it.
Same-class precedent: EE-CHAIN W2-2's one-off SIGTRAP@0x5046dc1c "delivery onto the
DR-emulator init loop". Either the linear-probe path interacts with the delivery regime
or the env-on early-boot window is perturbation-fragile — flagged to the coordinator;
SS_PROBE_LINEAR is suspect under env-on until cleared. Not counted against any gate.

**Baselining note for Task C:** the env-on frontier class is timing-sensitive — Task A's
no-ring env-on boot ran the NK spin and never reached the park; BOTH ring-slowed env-on
boots here reached the PROGRAM#5 srr0=50324fec park with the full E4-class census
(blocks=7405 vs baseline 7354). Task C's A/B comparisons must hold the instrument set
constant, not just the env gates.

**What Task C needs (flip-cluster readiness):** Task A's delivery machinery is green and
re-proven here; the consumption half is honestly NOT-REACHED pre-WLSC with the break
link pinned (stop-rule 2's shape — ship-gated-off-green is available per Rev 2 B6 if
the staging decision doesn't land first). Pre-flip checklist now carries FOUR items
(P2 run-exc.sh guard, P3 stub words, + the two Task-A-review carry-forwards added
above). The flip decision is orthogonal to the level-source staging decision: the flip
makes the latch default; the staging decision governs whether deliveries ever carry a
nonzero level pre-guest-init.

### Coordinator sign-off: the level-source staging (2026-06-12, post-Task-B)

**SIGNED OFF — shape (i): the host source joins the PIC rail.** Rationale: it is the
architecture-consistent shape (device sources — SCC/VIA — land on the same rail later;
the QEMU-oracle-tested OpenPIC model finally gets a live consumer; shape (ii)'s
synthesized-vector fiction would need unwinding the moment real devices arrive), and the
proposal's config-vs-event distinction holds: the staged words are init-time platform
constants that Mac OS's native interrupt init writes, after which EVERY interrupt still
traverses PIC-IACK → vector table → NK post level test → 68k chain. Same sanctioned
class as [KDP+0x1074/0x1078] and [KDP+0xf2c].

**Binding constraints on the implementation (Task B-2):**
1. The staged set is the MINIMAL PIC + memory state the IACK path actually needs —
   enumerate it from the fallback's lwbrx walk + the OpenPIC model (expect: the NK-held
   PIC virtual base [[KDP-0x20]+0xf18], the lowmem vector→level table entry
   [0x3f00+vector], the host source's IVPR/IDR unmask, and CTPR lowered from reset-15 —
   each staged word individually justified in the commit as "what the real init writes",
   values cited against the QEMU oracle / dev_openpic.cpp semantics with SHAs).
2. The per-interrupt EVENT path stays fully guest-traversed — zero host writes to the
   pending halfword, CR bits, or any per-delivery state (the fake-poke fence stays).
3. Env-gating: the staging rides the existing cluster (newworld + SS_NW_HOST_IRQ for the
   host-source assert; SS_NW_PIC=1 joins the ENV-ON TEST CLUSTER for acceptance —
   its DEFAULT flip stays HELD per Task C; if the staging is structurally tied to the
   PIC being registered, gate the staging on the same condition and state it).
4. Acceptance: the Task-B chain-walk table re-run — the level test now passes (r28≠0,
   post writes level|0x8000, CR bits SET), via_int 0xef2c / OP_IRQ fe6b probes fire,
   and the Ticks rider gets its real attempt (addq.l #1,$16a). Ticks moving is the
   milestone headline; the gate is the post-value + 68k-entry probes (falsifiable).
   R-II9 caveat: do NOT use SS_PROBE_LINEAR under env-on; hold instrument sets constant.
5. One-iteration rule; ≤5 boots; stop-rule 2 unchanged (a second break link past the
   level test → frontier-record it, ship-gated-off-green remains the fallback).

## Task B-2 results (2026-06-12, label m7-taskB2) — the level-source staging landed per sign-off shape (i): THE LEVEL TEST PASSES; first guest IACK of the live PIC model; the break moves ONE LINK PAST the level test (frontier-record per stop-rule 2)

Boots: **4 of ≤5** (slot0 `20260612-033407.20410` env-on chain walk, `-033746.20813`
env-on OP_IRQ discriminator, `-033954.20998` env-on ring discriminator,
`-034428.21398` gated-off A/B). Env-on cluster = riser+published+host-irq+**SS_NW_PIC=1**
(joins the TEST cluster only; default flip stays HELD). Gates: task tier 5/5 PASS +
exc lane 14/14 (no H-vector extension — latch semantics untouched; the PIC rail is
boot-bring-up wiring). No SS_PROBE_LINEAR (R-II9 honored); instrument set held constant
across the two chain-walk boots.

**THE TICKS VERDICT (honest, with a correction of record): Ticks MOVED — but the writer
is the HOST `HandleInterrupt` newworld keep-set (sheepshaver_glue.cpp:3399, "Always tick
Ticks on every VBL"), NOT `addq.l #1,$16a`.** Ring-pinned: the +1s land inside pure-NK
record windows (no 68k dispatch; via_int/OP_IRQ probes 0 matches ×2 boots). CORRECTION:
the rider's watch word 0x168 covers Ticks' HIGH half only (Ticks long = 0x16a..0x16d;
LSB lives in word 0x16c) — Task B's "Ticks did not move" was watch-word-blind; Ticks has
been ticking via the host keep-set whenever HandleInterrupt runs MODE_68K. The headline
(guest addq) did NOT happen; the rider's premise is retired with the right watch word
recorded for the next attempt (watch 0x16c, expect +1 with NO HandleInterrupt
attribution — i.e. inside a DR-dispatch record window).

**The staged-word table (each = what the real init writes):**
| Word | Value | Justification |
|---|---|---|
| IVPR[0x3F] (model) | prio 8 \| vec 0x3F, unmasked, EDGE | per-source unmask/vector/priority = the MPIC init's job (QEMU write_IRQreg_ivpr openpic.c:503 @ de5d8bfd…); EDGE vs the [DIAG-FORCED] level: composes with the Task-A once-per-edge latch, retirement = guest IACK (openpic_iack :1056), zero host writes per event; vec=input identity (bring-up convention; last in-range vector for the fallback's `<0x40` IACK leg, [STATIC] 0x50326070) |
| IDR[0x3F] (model) | 1 | route to CPU0, the only CPU (write_IRQreg_idr :445, masked to bit 0) |
| CTPR (model) | 0 | lowered from reset-15 (openpic_reset :1254; nothing deliverable until the init lowers it) |
| `[[KDP-0x20]+0xf18]` = 0x68FF4F18 | 0xF3040000 | the NK-held PIC base the fallback reads at 0x50325f48 (`lwz r22,0xf18(r20)` [STATIC] rom901.bin d1a267a9); IACK lwbrx = r22+0x200a0, EOI = r22+0x200b0 = the model's CPU0 bank; value = OPENPIC_CORE99_BASE (MacIO BAR+0x40000, donor Q6) — guest addressing is physical here; occupancy map extended (M6A-ONGOING-ENTRY-DESIGN.md) |
| `[0x3f3f]` (lowmem byte) | 1 | the vector→level table byte the fallback's `lbz r28,0x3f00(r26)` reads (0x503260a4); level 1 = the 68k level-1 autovector chain (Q-I3 via_int); must be ≠0 (the Task-B break) and ≠7 (deferred-slot leg 0x503260a8); one-shot host re-assert at edge #1 guards the lowmem wipe — live: **"survived to edge #1 (trampoline staging intact)"** |

PIC input choice: **0x3F (`OPENPIC_IRQ_HOST`, dev_openpic.h)** — the real platform has NO
PIC input for the decrementer/timer (DEC is CPU-internal; KeyLargo MPIC has zero timer
sources, KEYLARGO_MAX_TMR=0 "Timers don't exist…", QEMU openpic.h:41), so a documented
RESERVED choice: top of the 64-source bank, unassigned in the Q8 device map and QEMU's
NewWorld macio assignments at the pinned SHA. Constant only — zero model-behavior change
(dev_openpic.cpp untouched; 206 oracle checks unchanged).

**Chain-walk table (re-run, env-on):**
| Link | State | Evidence |
|---|---|---|
| host edge → EXT delivery | LIVE | `edges=1 consumed=1 deasserts=0 pending=0`, ×3 boots |
| delivery → fallback 0x325f00 | LIVE | ring #3733960-61: 50314880 → 50325f00, delivery-adjacent |
| fallback IACK lwbrx → live PIC model | **LIVE — FIRST GUEST IACK EVER** | `[PIC] first-iacks: src=0x3f vec=0x3f`, iacks i:1, out_raises=1/out_lowers=1 (the IACK lowers the line — guest-traversed retirement; the A1 livelock shape did not recur); first live lwbrx-over-MMIO (openpic jit_faults=11) |
| vector → staged level table | LIVE | probe `[0x3f3c]=0x00000001`; byte survived to edge #1 (no wipe) |
| **post level test r28** | **PASSES — r28=1 → sth 0x8001 + CR bits SET** | `[WATCH] pc=5032394c addr=68fff070 value=80010000 (was 00000000)` — the formerly-blind watch TRIPS; ring #3734010-12 runs 0x325518→0x325520 (sth+`or r13,r31` same straight line, [STATIC]); `bgt cr7` taken — the and-clear leg skipped |
| pending halfword / CR arm → DR dispatch poll | **THE NEW BREAK (one link PAST the level test)** | the env-on frontier parks in the NK spin/`sc 0x2e` regime; comp frozen, jDR static, the post-delivery ring window is pure NK flow — the DR/68k world never runs again to poll the armed 0x8001 |
| via_int 0xef2c → OP_IRQ fe6b → Ticks(addq) | NOT-REACHED (upstream break) | `SS_PROBE_68K` 0x5000ec52:8 / 0x5000bbca:8 — 0 matches, 59 s each |

**Invariants:** zero TRIPWIRE ×3 env-on boots; exactly-once re-proven; DEC regime healthy
(mtspr_dec=10297 / expiries=5146 ≈ 2.0/delivery, the NK 7fffffff/32e10 park+re-arm pair,
no zero/tiny storm); sc census = the named env-on frontier class (no park; 0xffffffff
x204). A second bounded fallback traversal IACKed empty → SPVE 0xff → OOB leg → guest
EOI (`spurious=1 eoi_empty=1`) — bounded, no storm. **Gated-off A/B: byte-identical to
the E4 class** — blocks=7354 EXACT, PROGRAM#5 srr0=50324fec word=0fff0005 slot=5, sc
census x233/x17 identical, ZERO PIC/host-irq output. e2e risk tier: structural-inertness
substitution (every new line gated under newworld && SS_NW_HOST_IRQ && SS_NW_PIC; stated
in the commit) + the A/B boot.

**Falsification handled per stop-rule 2 (not a milestone failure):** constraint 4's
"via_int/OP_IRQ probes FIRE" expectation did not survive contact — the chain's null is
now established one link DOWNSTREAM of the level test (the parked-regime DR-poll gap),
which is exactly the "break PAST the level test → frontier-record" branch.
Ship-gated-off-green holds: default-off, all gates green, A/B byte-identical. The next
frontier owns "make the DR/68k world run (or schedule) after an EXT post in the parked
regime" — that is a scheduler/park question (R-II3's `sc 0x2e` wait loop), not a
level-source question. Instrument facts for the next attempt: watch 0x16c (not 0x168)
for Ticks; SS_JIT_WATCH_DUMPS default 3 exhausts on early lowmem-init hits — use
SS_JIT_WATCH_DUMPS=8 + a narrowed watch set for ring capture around late events.

**Task C readiness:** delivery + IACK + level + post are all green and guest-traversed;
the staging rides the env-on test cluster (SS_NW_PIC default flip stays HELD). The
flip-cluster decision is unchanged by B-2; the pre-flip checklist carries forward.
