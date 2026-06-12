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

> Read `docs/HANDOFF.md`, then `docs/AGENT-CONTEXT.md` (the standing context pack —
> its "Current frontier" block is authoritative), then the header of
> `docs/planning/ROADMAP.md`. The development process is BINDING:
> `docs/MILESTONE-WORKFLOW.md` (plan → red-team → rev-2 fold → binding Task-0 recon →
> env-gated implementation → flip-last acceptance → docs close-out), with the
> parallel-workstream layer (file-ownership claims, slot-protocol boots, always-green
> fusion). The named next milestone is **the VIA-IFR surface** — make `dev_via6522`
> present the 60 Hz interrupt source to the 68k level-1 handler so the interrupt
> round trip retires and Ticks becomes guest-claimed. Its Task-0 opens with one
> pinned question: resolve `a4` at 68k PC `0x5000ee9a` — the concrete MMIO address
> of the IFR the handler bit-tests — and which IFR bit `d6` indexes (full seed:
> the M8 plan's Task-B/Task-C results and `docs/planning/machine/
> INTERRUPT-INJECTION-RECON.md`). Run the milestone machine: draft the plan on the
> house template, red-team it, then execute. Never push without being asked.

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

## Operational notes

- Branch `macos-arm64`; the user pushes — **never push unprompted**.
- Parallel boots ONLY via `SheepShaver/tools/ss-slot-boot.sh` (never global pkill);
  cleanup with `ss-reap.sh`. File claims via `docs/superpowers/.claims/` + the
  pre-commit guard.
- Gate tiers via `tools/gates.sh <inner|task|full>`; harness score must stay 353/353.
- Assets in `/Users/Shared/macemu/`; canonical ROM dumps + manifest in
  `/Users/Shared/macemu/dumps/` (verify with `tools/dump-manifest.sh --check` before
  tagging evidence).
