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

**The shift:** the Trampoline is the *producer* of the boot-time init the frozen structs depend on.
Running or reproducing it clears **a class of walls at once, with real values computed by real guest
code** — converting the unbounded "first of N" forge problem into a bounded "get the producer to do
its job" problem.

> **Refinement (Task-0 red-team, 2026-06-14):** empirically, the Trampoline (`MacOS.elf`) builds the
> **OF device tree + page map** and does NOT itself write the CGRP/interrupt structures — the
> **NanoKernel** (a separate ROM parcel) builds CGRP *from* that device tree. So the "producer" is
> likely **Trampoline + NanoKernel**, and the device tree is the hand-off between them. The effort's
> shape is unchanged (run/reproduce the producer); what the producer *is* is sharpened. Pinning this
> is Task-0's fork **Q0-F** (`DECISIONS.md`).

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

> **Live fork tracker:** `DECISIONS.md` holds the open questions (Q0-A…F) with status. The single
> dominant one is **Q0-A**, below.

## 6. Key risks (carried into the first milestone's Task 0)
- **R1 / Q0-A — "Does Route A mean implementing OpenFirmware?" (the effort's FEASIBILITY GATE, not
  a side risk):** the Trampoline is an OF *client* and calls OF client-interface services; SheepShaver
  has no OF. Task-0's FIRST deliverable is a go/no-go: enumerate the OF calls the Trampoline makes →
  **bounded-and-stubbable** (Route A viable) vs **open-ended** ("Route A" = "write an OpenFirmware",
  ladder collapses toward B/C). **This gates everything else** (`DECISIONS.md` Q0-A).
- **Route C is not automatically distinct from the buried forge:** M16 found the handler PC is
  ROM-absent (computed at runtime). C is viable only if the interrupt-setup writes are
  constants/relocations; if they're computed from a live OF tree, C collapses into A (`DECISIONS.md`
  Q0-B). **Route B must be honest re-binding (relocation), not value-hardcoding** — else it's the
  forge in a tbxi hat; the red-team enforces this in the DoD (Q0-C).
- **R1b — OF-service depth (Route A):** even if bounded, the OF callbacks must be stubbed. Bound the
  set in Task-0 before committing to Route A.
- **R2 — 9.2 asset gap is SYSTEM SOFTWARE, not the ROM:** we hold the full NewWorld ROM-file
  progression incl. the 9.2-era ROMs (`/Users/Shared/macemu/newworld-roms/`; our active "9.0.1" file is
  the Dec-2001/9.2.2-era ROM). So the Trampoline RE runs on the in-hand 9.2-era ROM. The remaining gap
  is genuine **Mac OS 9.2.x system software** for the eventual *boot* (the `macos921.dsk` asset is
  actually 8.6) — NOT on Task-0's critical path. Tracked in `ASSETS-AND-TOOLING.md`.
- **R3 — producer-run reveals a non-IM wall: this is the GOOD outcome.** It means the producer
  approach worked and we're back on the machine-layer plan's mainline (device models already staged),
  not a forge cul-de-sac.
- **R4 — 9.2 is past upstream's NewWorld envelope, and 9.0.x findings may not transfer:** upstream
  SheepShaver targets NewWorld 8.5–9.0.4; 9.1+ moved to a different parcels-based tbxi layout, so
  Task-0 RE on the 9.0.x ROMs may characterize a *structurally different* Trampoline than 9.2's.
  Starting on 9.0.x is fine for tooling/method, but **don't let a clean 9.0.x RE create false
  confidence about 9.2** — prioritize sourcing the genuine **9.2 "Mac OS ROM" file** (a separate
  artifact from the install ISO, possibly sourceable independently) and diff the layouts
  (`DECISIONS.md` Q0-D).

> **Win condition, stated plainly (so success isn't mis-sold):** NewSheep wins by **clearing the
> IM-init frozen-struct class**. The most likely *next* wall is the one M14-FINDINGS pinned: the
> **Cuda device-model IFR/IER bug** (`sr_int_pending` never reaches VIA IFR; the NK polls IER, not
> IFR) — a known, separate downstream frontier (NOT a "model-rejection gate" — M14-FINDINGS retracted
> that framing). Reaching Finder is the effort's north star but NOT this class's DoD.

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

## 9. Status / next steps

**Done**
- [x] Effort opened; baseline tag `newsheep-baseline`; charter + log + assets + glossary written;
  HANDOFF/AGENT-CONTEXT frontier flipped to NewSheep.

**The process (BINDING — `docs/MILESTONE-WORKFLOW.md`).** The first milestone is the **Trampoline RE
Task-0**. Steps 1–4 are DONE (2026-06-14); **▶ the next action is step 5 — execute Task-0** per the plan:
- [x] **1. Brainstorm** — done (method/version/budget locked; `DECISIONS.md` log).
- [x] **2. Spec** — done: `docs/superpowers/specs/2026-06-14-newsheep-trampoline-re-design.md` (rev 2).
- [x] **3. Plan** — done: `docs/superpowers/plans/2026-06-14-newsheep-trampoline-re.md` (rev 2).
- [x] **4. Red-team** — done: 3 reviewers, all GO-WITH-FIXES, folded into rev-2 (producer reframe Q0-F,
  mechanism-level gate, Python gdb-remote client + `-S`). `DECISIONS.md` log + `RESEARCH-LOG.md`.
- [ ] **5. ▶ NEXT — Execute Task-0** (offline-first, no SheepShaver boots; follow the rev-2 plan —
  framed as variance-reduction on the Q0-A feasibility gate, not a general "disassemble and see"):
  - `pip install tbxi`; `tbxi dump -o <dir> <rom>` the in-hand 9.2-era ROM(s); disassemble the
    **Trampoline = top-level `MacOS.elf`** (capstone, PPC BE) — NOT a parcel; the NanoKernel is the
    separate `NanoKernel-vNN` parcel.
  - **Q0-A (GATING): enumerate the OF client-interface calls the Trampoline makes** → bounded-and-
    stubbable vs open-ended. This decides whether Route A is viable or secretly "write an OpenFirmware."
  - **QEMU as a Trampoline TRACER (a step, not a backstop):** single-step the Trampoline under QEMU
    mac99; capture which OF services it calls (→ Q0-A) and which guest addresses it writes with which
    values (→ Q0-B: constants/relocations vs computed-from-OF-tree). Behavioral/structural only —
    never our addresses.
  - Pin (a) what OF state it **reads**, (b) the nanokernel interrupt-setup it **writes** (the frozen
    CGRP/IM structures), (c) the **gap** vs. our synthesized OF + `SS_NW_TRAMPOLINE`.
  - **Output:** close Q0-A/B/C in `DECISIONS.md` → the A/B/C route decision, recorded there +
    `RESEARCH-LOG.md` + a `NEWSHEEP-FINDINGS-*.md`.

**Parallel dependency**
- [ ] Source a genuine **Mac OS 9.2.x install ISO** (`ASSETS-AND-TOOLING.md` R2). Task-0 RE starts on
  9.0.x without it; the actual 9.2 boot and any 9.2-only Trampoline behavior are blocked on it.

**Standing rules for every milestone:** branch `macos-arm64`; never push unprompted; slot boots only
(`ss-slot-boot.sh`, never global pkill); `make test-jit` = 100; all new code behind an env gate +
`MachineProfileIsNewWorld()`, paravirtual byte-identical; `0xDEADBEEF` (M10 DR-reentry) = immediate stop.
