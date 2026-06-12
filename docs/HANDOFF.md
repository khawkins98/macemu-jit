# Project Handoff — resume entry point

> **Status: PAUSED 2026-06-12** after the M8 (slot-4 consumption) milestone closed.
> This document is the single entry point for picking the work back up. Hand the
> resume prompt below to a fresh agent session verbatim, or read on for the state
> summary and pointers.
>
> (Not to be confused with `docs/planning/HANDOFF-NEWWORLD-SUPERVISOR-MMU.md`,
> which is the preserved Path-A reference from an earlier, superseded approach.)

## The resume prompt

Paste this to start the next session:

> Read `docs/HANDOFF.md` in full — including the "Recommended resumption order" and
> the queued ideas — then `docs/AGENT-CONTEXT.md` (the standing context pack — its
> "Current frontier" block is authoritative), then the header of
> `docs/planning/ROADMAP.md`. The development process is BINDING:
> `docs/MILESTONE-WORKFLOW.md` (plan → red-team → rev-2 fold → binding Task-0 recon →
> env-gated implementation → flip-last acceptance → docs close-out), with the
> parallel-workstream layer (file-ownership claims, slot-protocol boots, always-green
> fusion). **Follow the recommended resumption order below** unless Ken redirects:
> QEMU rig first (half-day cap), then the two-gear sprint toward the first visible
> boot screen, with the VIA-IFR surface as the first wall. Never push without being
> asked.

## Recommended resumption order (coordinator + Ken, decided at pause time)

1. **The QEMU differential rig FIRST (≤ half a day).** Build idea 2 below on Spike
   S1's working mac99 boot of our 9.0.1 ROM (`docs/planning/spikes/
   SPIKE-S1-QEMU-GATE-CHECK.md`). Its first customer is the VIA-IFR Task-0 itself:
   observe on the reference boot what the VIA IFR presents at tick time, what the
   68k level-1 handler reads (the a4@0x5000ee9a address, the d6 bit), and what the
   $6e4 vector chain expects on dismissal — answers by observation instead of recon
   boots. If the rig stalls past the half-day cap, fall back to the standard
   Task-0-by-probes and finish the rig later.
2. **Then the two-gear sprint toward pixels** (idea 1 below): VIA-IFR first
   (pre-answered by the rig), the M5 framebuffer early (recon complete — a visible
   screen converts later debugging from ring-forensics to looking at it), then
   frontier-chase. Light gear for seed-class walls (evidence-tagged root cause →
   gated fix → inner gates → one-line log); the FULL milestone machine for anything
   touching delivery/world-switch semantics or paravirtual-reachable code.
   Non-negotiables in either gear: slot protocol, falsifiable evidence before fixes,
   env gates. One consolidated review + docs pass at sprint end (scheduled review
   debt, not skipped review).
3. **Day one, inside the sprint:** sketch the M9+ milestone map (item 6, rough is
   fine) and kick off the QEMU wall census (item 7) as a background task once the
   rig works.
4. **Defer doc restructuring** (item 4's residual) to the next doc-sweep trigger —
   it is not the bottleneck.

## Where the project stands (2026-06-12)

One-line: **SheepShaver boots Mac OS 8.6 to Finder with the full native JIT
(paravirtual profile, shipped, stable). The Machine Layer work toward Mac OS 9.2.x
on the NewWorld fidelity profile has a complete, default-on interrupt-delivery
architecture; consumption is green to within one device surface of the first
guest-claimed tick.**

Shipped in the final two days before the pause (details: `CHANGELOG.md` 2026-06-11/12):

- The tm_task misalignment guard (slide wall), the HLE Time Manager trap population
  (SysError-12 wall), the EE riser + the DEC-cadence fix (first DEC deliveries ever).
- **M7 interrupt injection** (plan: `docs/superpowers/plans/2026-06-12-interrupt-injection.md`):
  host edge → EXC_EXTERNAL → NK-published handler → first guest PIC IACK → vector/level
  table → NK post — all guest-traversed, cluster default-ON (`81d60cc1`).
- **M8 slot-4 consumption** (plan: `docs/superpowers/plans/2026-06-12-slot4-consumption.md`):
  the torn-context livelock root-caused (our riser's non-atomic rfi emulation — found
  statically by the red-team) and fixed; **PROGRAM#4/slot-4 fired for the first time**;
  the 68k level-1 interrupt handler runs at 60 Hz. Shipped **gated-off-green**
  (`SS_NW_IRQ_CONSUME` default OFF; the flip prerequisites are recorded in the plan's
  Task-C results).
- Instrument hardening: watch spans + `[WATCH-SAMPLE]`, SS_PROBE_LINEAR cleared,
  r24-ring SIGSEGV flush.

The honest red: **Ticks is still host-attributed.** The 68k handler `rte`s source-less
because the VIA 6522 IFR presents no interrupt source — that is exactly the named next
milestone.

## Reading order for a fresh session

| Read | Why |
|---|---|
| `docs/AGENT-CONTEXT.md` | The standing facts pack — current frontier, constants, instrument caveats, gate states, boot recipes. One read replaces four docs. |
| `docs/planning/ROADMAP.md` (header) | What's next, arranged. |
| `docs/MILESTONE-WORKFLOW.md` | THE binding process. |
| `CHANGELOG.md` (top) | What just happened, by commit. |
| `docs/superpowers/plans/2026-06-12-slot4-consumption.md` | The last milestone — its Task-C flip criteria and residues seed the next one. |
| `docs/planning/machine/INTERRUPT-INJECTION-RECON.md` | The evidence base + the consolidated residue table (R-II7..R-II10, SC#1=0x0d). |
| `LEARNINGS.md` | Non-obvious lessons; read before theorizing. |

## Ideas queued at the pause (2026-06-12 discussion — candidates, not commitments)

Three directions discussed with Ken at pause time, recorded here so resumption can
weigh them against the default next milestone (VIA-IFR):

1. **A two-gear sprint toward pixels.** The seeds-not-services pattern held ~7
   consecutive times — most walls are one staged word found in 2–4 boots, and the
   per-wall ceremony (plan/red-team/dual-review) now costs more than the walls.
   Proposal: a timeboxed sprint whose goal is *the ?-disk icon on screen*, running
   seed-class walls in a LIGHT gear (evidence-tagged root cause → gated fix → inner
   gates → one-line log; no plan/red-team per wall) while keeping the full machine
   ONLY for delivery/world-switch semantics or paravirtual-reachable changes.
   Non-negotiables even in sprint gear: slot protocol, falsifiable evidence before
   fixes, env gates. One consolidated review + docs pass at sprint end (the sprint
   accumulates review debt deliberately — schedule the hardening pass). Suggested
   day-one items: the M5 framebuffer (recon complete — a visible screen is itself
   an instrument) and the QEMU differential rig below.
2. **QEMU mac99 as a differential boot oracle.** Spike S1
   (`docs/planning/spikes/SPIKE-S1-QEMU-GATE-CHECK.md`, 2026-06-10) already boots
   our exact 9.0.1 ROM to Finder under QEMU mac99 — the rig is half-built (working
   invocation, QEMU 11.0.1, hfsutils). Upgrade it from a one-off gate check to a
   routine instrument: `-d` traces / gdbstub captures diffed against our boot at
   the same PC regions, so "what does the NK expect here?" becomes observation
   instead of RE. Caveat (already noted in MACHINE-LAYER-PLAN §oracles): QEMU boots
   via OpenBIOS, not Apple OF — the oracle is valid from NK entry onward, which is
   where our walls live. First customers: the VIA-IFR question (what does the IFR
   present at tick time on a working boot?), the SC#1=0x0d divergence, the
   0x500eXXXX crash class.
3. **DingusPPC as the fidelity second-opinion.** For NewWorld/Core99 behaviors it
   is the most faithful modern reference (real Apple-OF-path focus). Use when QEMU
   and our RE disagree (Cuda/KeyLargo/VIA). Standing rules apply: import GPL code
   with citation per backport hygiene; never contribute upstream to them. Existing
   notes: `docs/planning/COMPATIBILITY-PAYOFF-DINGUSPPC-REVISIT.md`.

Four more from an external review of the plan (2026-06-12, accepted at pause time —
the first item was acted on immediately, the rest are queued):

4. **Header discipline — DONE at the pause**: ROADMAP/MACHINE-LAYER-PLAN headers
   trimmed to a 5-line current-state budget; the old narratives relocated verbatim to
   "Archived status narratives" sections in each doc; the budget is now an enforced
   rule in CONTRIBUTING's sweep checklist (Task Zs update in place, never append).
   Residual candidate for a future doc-sweep: split other long trackers into
   status-front + dated-annex on the same pattern (relocate, never delete).
5. **Gate retirement needs a trigger, not a vibe.** 17 SS_NW_* gates in-tree, 6
   named retirement candidates, but "after a quiet release cycle" is not a schedule.
   Proposed trigger: the FIRST milestone after resumption includes a gate-retirement
   task (hard-wire the six pre-M7 default-ON surfaces), and the all-ON/all-OFF-only
   support claim becomes an enforced contract (a gates.sh check or a documented
   refusal), not a doc sentence.
6. **The consumption path needs M9+ milestone framing.** The plan structurally ends
   at M8 + "VIA-IFR next". The remaining path to 9.2.x-to-Finder is ~3-4 milestone-class
   efforts: M9 consumption (VIA-IFR → retirement → Ticks guest-claimed), M10 visible
   framebuffer (M5 work, recon done), M11+ CFM / Process Manager / drivers (unscoped).
   First planning act on resumption: write the milestone map, even rough.
7. **Measure the wall count instead of re-flagging it.** Every re-score names the
   CFM/Process-Mgr "unmeasured wall tail" as the residual drag; nobody has measured
   it. The QEMU rig (idea 2) pointed FORWARD — a traced reference boot enumerating
   the syscall/trap/device surfaces between the current frontier and Finder — turns
   the unknown into a checklist. Honest caveat from the review: seeds-not-services
   has held because NK state is inspectable; it may NOT hold in Process Manager
   territory. A short M5/framebuffer scoping spike (the historical home of M3-class
   surprises in other emulators) de-risks the same estimate from the other side.

## Operational notes

- Branch `macos-arm64`; the user pushes — **never push unprompted**.
- Parallel boots ONLY via `SheepShaver/tools/ss-slot-boot.sh` (never global pkill);
  cleanup with `ss-reap.sh`. File claims via `docs/superpowers/.claims/` + the
  pre-commit guard.
- Gate tiers via `tools/gates.sh <inner|task|full>`; harness score must stay 353/353.
- Assets in `/Users/Shared/macemu/`; canonical ROM dumps + manifest in
  `/Users/Shared/macemu/dumps/` (verify with `tools/dump-manifest.sh --check` before
  tagging evidence).
