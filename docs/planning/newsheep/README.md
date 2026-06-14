# Operation NewSheep — Charter

> **One line:** Boot Mac OS 9.2 (NewWorld) on SheepShaver/arm64 by **running or faithfully
> reproducing the producer of the guest's boot-time init (the Trampoline), instead of forging
> its outputs** — the approach the M8→M17 arc exhausted and banked as a dead end.

**Status:** ACTIVE research effort, opened 2026-06-14. Baseline tag: `newsheep-baseline`.
**Owner doc set:** this folder (`docs/planning/newsheep/`). **Process:** `docs/MILESTONE-WORKFLOW.md`.
**Hard requirement:** 9.2 NewWorld boot (set by the user 2026-06-14; "pivot to compatibility-payoff"
is OFF the table for this effort).

---

## 1. Why this effort exists — the reframe

Milestones M8 through M17 attacked NewWorld 9.x interrupt routing by **forging the outputs** of
the guest's Interrupt Manager / nanokernel init — the CGRP interrupt-group descriptor, the routing
structs (`hnfo`, `KDP+0x674`), the handler tables. Every attempt dead-ended on the **same root
cause**, rediscovered from a new angle each time:

- **M14**: KDP+0x674 / hnfo fields are zero because IM init runs downstream of Cuda init and never
  completes. All viable fixes "collapse to the forge class."
- **M15**: FORGE verdict verified — NK routing structs frozen-zero across obs=1e9; CGRP is **the
  first of N** frozen structs.
- **M16**: the minimal one-word forge is *safe but inert* (empty CGRP table self-guards); the real
  handler PC is **ROM-absent** (`0x5000ec50` has 0 word-refs; `"CGRP"` tag 0×) → it is built at
  runtime by init that never runs. DoD-3 NO-GO.
- **M17**: even re-using the in-tree "sanctioned cross" fails — the EXT-edge regime is **MODE_68K**,
  not the MODE_EMUL_OP the cross requires. Red-team BLOCKED. The **series tripwire fired on wall 1**:
  per-wall forging is structurally unbounded whack-a-mole, because every wall is the same disease.

**The disease, named:** on a real NewWorld Mac, OpenFirmware loads and runs the **Trampoline** — an
ELF bootloader (a *parcel inside the "Mac OS ROM" file*) that copies/modifies the OF device tree
**and sets up the interrupts for the nanokernel**. SheepShaver does **not** run it; `SS_NW_TRAMPOLINE`
synthesizes a partial hand-built substitute. The frozen CGRP/IM structures are exactly "the
Trampoline never ran." (See `GLOSSARY.md` for the full lineage; sources in §8.)

**The shift:** the Trampoline is the *single producer* of the whole frozen-struct class. Running or
reproducing it clears **a class of walls at once, with real values computed by real guest code** —
converting the unbounded "first of N" forge problem into a bounded "get one finite component to do
its job" problem.

## 2. North star & definition of success

**North star:** Mac OS 9.2 (NewWorld profile) boots far enough that the guest's own IM/nanokernel
init has run and interrupts dispatch through the guest's real path — ideally to Finder, but per the
M-series discipline, **success = the producer-run advances the boot past the entire frozen-struct
class and into a genuinely new, different regime**, captured as the next frontier. Reaching Finder
is the goal; clearing the IM-init wall class is the milestone-defining win.

**Existence proof:** QEMU mac99 boots 9.2 by running the real OF + Trampoline + nanokernel. The
producer-run path is *known to work* for our exact target. Our job is to provide enough of that
environment (or patch/reproduce the producer) for SheepShaver's hybrid model.

## 3. The route ladder (Task-0 RE chooses the rung)

All three share the **same first artifact** — a `tbxi dump` + disassembly of the Trampoline parcel —
and the same north star. Ordered by fidelity/effort:

| Route | What | Trade-off |
|---|---|---|
| **A — Run it (LLE the handoff)** | Replace `SS_NW_TRAMPOLINE`'s ad-hoc writes with a real-enough OF device tree (we have CORE99-MACHINE-DESCRIPTION + the QEMU device-tree oracle), then actually execute the Trampoline ELF. The nanokernel then inits itself. | Highest fidelity; "solid foundations." Risk: may call OF client-interface services we'd have to emulate (OF-depth rabbit hole). |
| **B — Patch the producer offline** | Use `tbxi` + `tbxi-patches` to modify the Trampoline/nanokernel parcels so their setup completes under our *existing* synthesized environment; repack with `tbxi build`. | Middle effort; values still computed by real guest code under adapted preconditions; no runtime forging. New capability surfaced by this effort's research. |
| **C — Informed host-reproduction** | RE the Trampoline's exact writes, then reproduce them host-side — copying the *real algorithm*, not guessing targets (how M10/M16 failed). | Closest to existing machinery; "forge done honestly." Lowest fidelity; still host-side, but no longer blind. |

**These are a ladder, not competing milestones.** Task-0's RE tells us which rung is reachable;
we take the cheapest that works.

## 4. Scope

**IN:**
- Offline RE of the Trampoline (and the OF→Trampoline→nanokernel handoff) via `tbxi` on ROMs we
  already hold (9.0.1/9.0.4), starting today, no boots.
- Pinning the *gap* between what the Trampoline reads/expects and what our synthesized OF +
  `SS_NW_TRAMPOLINE` currently provide — the mechanism-level "why IM-init never runs."
- The A/B/C route decision and its first implementation, all behind env gates +
  `MachineProfileIsNewWorld()`; paravirtual byte-identical; `make test-jit` stays 100.
- Sourcing + integrating a genuine Mac OS 9.2.x install ISO (see `ASSETS-AND-TOOLING.md`).

**OUT:**
- Resuming any per-wall output-forge (M8→M17 proved it bankrupt).
- Touching the 8.6–9.0.4 booting configs / paravirtual path semantics.
- Performance work (separate compatibility-payoff track).
- Pushing tags/branches without explicit ask.

## 5. Success criteria / DoD per the route chosen
- **A**: the real Trampoline executes; ring/probe confirms the guest itself populates the CGRP/IM
  structures (previously frozen-zero); the boot advances into a new regime; no M10-class crash.
- **B**: a patched ROM repacks cleanly and boots further with the structures guest-populated under
  our environment; the patch is documented and minimal.
- **C**: host-reproduced writes (derived from the Trampoline's real code) make the boot advance,
  with the derivation cited from the disassembly (not guessed).
- **All**: `make test-jit` = 100; gated-off byte-identical A/B; new frontier captured in `RESEARCH-LOG.md`.

## 6. Key risks (carried into the first milestone's Task 0)
- **R1 — OF-service depth (Route A):** running the Trampoline may pull in OF client-interface
  callbacks. Bound this in Task-0 RE before committing to Route A.
- **R2 — 9.2 asset gap:** we have 9.0.1/9.0.4 ROMs; the `macos921.dsk` asset is actually 8.6. The
  Trampoline RE can *start* on 9.0.x, but the real 9.2 boot needs genuine 9.2.x media (an ISO, not a
  hardware ROM). Tracked in `ASSETS-AND-TOOLING.md`.
- **R3 — producer-run reveals a non-IM wall:** possible, but that returns us to the machine-layer
  plan's mainline (device models already staged), not a forge cul-de-sac.
- **R4 — 9.2 is past upstream's NewWorld envelope:** upstream SheepShaver targets NewWorld 8.5–9.0.4;
  9.1+ offloads *more* to the Trampoline (parcels-based tbxi). So 9.2 is the hardest case for the
  exact thing we lack — and nobody's SheepShaver runs it. Plan for genuinely new ground.

## 7. How this fits the existing plan
This is the `MACHINE-LAYER-PLAN.md` §2.7 principle ("LLE for boot, HLE for runtime") finally pointed
at the right component. The M-series kept concluding "the staged nanokernel is more complete than
assumed; our obligation reduces to providing the Trampoline-init surface." We mis-read that as "a few
more seeds." The honest reading: **the Trampoline-init surface *is the Trampoline* — provide it by
running/reproducing the real thing.** M8→M17 weren't wasted: they proved the seed-the-outputs
interpretation bankrupt and handed us the exact producer to target.

## 8. Lineage & sources
- Banked forge-era RE: `docs/planning/M14-FINDINGS-cuda-delivery.md` §7, `…/M15-FINDINGS-consumption-recon.md`,
  `…/M16-FINDINGS-oracle-forge.md` Q1–Q7, the M17 spec/plan (CLOSED).
- Strategy parent: `docs/planning/MACHINE-LAYER-PLAN.md`.
- External (see `GLOSSARY.md` for what each is): elliotnunn/tbxi, elliotnunn/tbxi-patches,
  elliotnunn/newworld-rom, the 68kMLA "Picking apart the NewWorld ROM" thread, the E-Maculation
  "Using QEMU to explore the Mac OS Nanokernel" thread, Apple Wiki "New World ROM".

## 9. Status / next
- [x] Effort opened, baseline tag `newsheep-baseline`, charter written.
- [ ] **First milestone (run the machine): Trampoline RE Task-0** — `tbxi dump` the 9.0.1/9.0.4 ROMs,
  disassemble the Trampoline parcel, pin its OF-tree reads + nanokernel interrupt-setup writes, and
  the gap vs. our synthesized environment. Output = the A/B/C route decision. Brainstorming → spec →
  red-team → execute.
- [ ] Source a genuine 9.2.x install ISO (`ASSETS-AND-TOOLING.md` R2).
