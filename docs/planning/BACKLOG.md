# Backlog — Queued Ideas (not commitments)

Ideas discussed at the 2026-06-12 pause. None are scheduled — weigh against the default
next milestone (VIA-IFR → framebuffer → CFM/Process Manager).

## Sprint mode: two-gear toward pixels

The seeds-not-services pattern held ~7 times — most walls are one staged word found in 2–4
boots; the per-wall ceremony (plan/red-team/dual-review) now costs more than the walls.
Proposal: a timeboxed sprint whose goal is *the ?-disk icon on screen*, running seed-class
walls in LIGHT gear (evidence-tagged root cause → gated fix → inner gates → one-line log;
no plan/red-team per wall) with the full machine reserved for delivery/world-switch semantics.
Non-negotiables even in sprint gear: slot protocol, falsifiable evidence before fixes, env gates.
One consolidated review + docs pass at sprint end.

Day-one items: M9 VIA-IFR (in progress) → M10 framebuffer (recon done).

## Gate retirement needs a trigger

17 SS_NW_* gates in-tree; 6 named retirement candidates. Proposed trigger: the FIRST
milestone after resumption includes a gate-retirement task (hard-wire the six pre-M7
default-ON surfaces). Retirement criterion for `SS_NW_IRQ_CONSUME`: VIA-IFR surface +
SC#1=0x0d divergence explained or fixed.

## Measure the wall count

Every re-score names the CFM/Process-Mgr "unmeasured wall tail" as the residual drag;
nobody has measured it. The QEMU rig (trace reference boot enumerating syscall/trap/device
surfaces between the current frontier and Finder) turns the unknown into a checklist.
Run this before scoping M11+.

## ROM-architecture reference doc

A curation pass (not new RE) — extract ROM FACTS from the recon docs into a standalone
"NewWorld ROM boot architecture notes." Everything is already pinned and evidence-tagged;
the separation is mechanical. Mandatory: one DB per ROM version (three in hand; md5s in
AGENT-CONTEXT.md Constants section), version-agnostic architecture with per-version
address appendices.

Sub-items:
- **NK completion pass**: NK is small (~tens of KB reachable), ~70% RE'd by accretion.
  Finishing it systematically is bounded and everything routes through it.
- **Annotated-disassembly consolidation**: pour hundreds of pinned addresses into Ghidra
  DBs over the decompressed images so future RE compounds instead of re-derives.
  Pairs with the QEMU rig (ground truth to annotate against).

## ROM portability strategy

Today: a different NewWorld ROM boots some distance, GUARDED-SKIP loudly, stalls.
Mature shape: checksum-keyed per-version offset profiles (from Ghidra DBs above) +
signature-search for staging constants that drift + only-if-needed dynamic discovery.
Scope: ~3 ROM versions (9.0.1 now, 9.2.x-era, maybe one more). Three curated profiles
beat a general mechanism.

## DingusPPC as fidelity second-opinion

For NewWorld/Core99 behaviors it is the most faithful modern reference (Cuda/KeyLargo/VIA).
Use when QEMU and our RE disagree. Standing rules: import GPL code with citation per
backport hygiene; never contribute upstream.
Notes: `docs/planning/COMPATIBILITY-PAYOFF-DINGUSPPC-REVISIT.md`.
