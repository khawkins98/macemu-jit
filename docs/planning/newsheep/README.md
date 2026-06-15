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
- Integrating a genuine Mac OS 9.2.x install ISO (✅ **found 2026-06-14** — 9.2.1 + 9.2.2 in hand; see
  `ASSETS-AND-TOOLING.md` R2). Sourcing is done; copy into the asset area when Stage 4 opens.

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
  the Dec-2001/9.2.2-era ROM). So the Trampoline RE runs on the in-hand 9.2-era ROM. The system-software
  gap (genuine **Mac OS 9.2.x** install media for the eventual *boot* — `macos921.dsk` is actually 8.6)
  is now **RESOLVED**: genuine 9.2.1 + 9.2.2 ISOs found 2026-06-14 (`ASSETS-AND-TOOLING.md` R2).
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

**The process (BINDING — `docs/MILESTONE-WORKFLOW.md`).** Two Task-0s COMPLETE; the SS_M18
implementation has been planned as a staged program; kickoff recon is done. State as of 2026-06-14:
- [x] **Trampoline RE Task-0** — DONE. Static (`MacOS.elf` disasm; 177/177 OF calls) + dynamic (QEMU
  gdbstub) on the `66210b4f…` ROM → **Q0-A BOUNDED, Q0-B computed(OF-input), Q0-F Trampoline+NanoKernel
  → ROUTE A DECIDED.** `FINDINGS-trampoline-re.md`; forks in `DECISIONS.md`.
- [x] **SS_M18 gating Task-0** (plan→red-team→2 recon agents) — DONE. **Route A GO but MONTHS**; the
  weeks-sketch falsified by three findings: Q1 NK is a permanently-resident supervisor with no handoff
  boundary; Q2 it requires a real paged MMU; Q3 CGRP is built by disk/CFM IM-init (NOT the NK parcel →
  corrects Q0-F's inference, confirms M16). `FINDINGS-trampoline-re.md` "SS_M18 gating Task-0".
- [x] **SS_M18 staged program planned** (rev-4, red-teamed + tech-writer/dev-advocate reviewed):
  `docs/superpowers/plans/2026-06-14-ss-m18-trampoline-lle-program.md`. Four stages —
  **S1** NewWorld paged MMU (PREREQUISITE) → **S2** Trampoline loader + OF-CI + Core99 DT (S2a parallel)
  → **S3** two-supervisor reconciliation → **S4** disk IM-init→CGRP. Critical path S1→S3→S4 strictly
  sequential ≈ **multiple quarters**. Each stage opens with its OWN Task-0 + red-team before its code.
- [x] **Kickoff recon (zero-dependency workstreams)** — DONE 2026-06-14: **Discriminator-A = COARSE** →
  S1 path a (Dolphin Dynamic-BAT shadow-arena) indicated, JIT fast path preserved
  (`FINDINGS-discriminator-a.md`); **donors extracted** (`DONOR-NOTES.md`); **9.2.x ISOs in hand**
  (`ASSETS-AND-TOOLING.md` R2 — S4 asset block cleared).

**▶ NEXT ACTION (re-banded 2026-06-15):** **S1's mechanism is DONE** — Task A
(`paged_mmu_translate()` + oracle test) is committed and the static NK-MMU constants are derived
(`FINDINGS-s1-mmu-constants.md`). **S1's LIVE paged MMU (the window + the softmmu) is DEFERRED and is
INSEPARABLE FROM S3** — there is no live `(SR/BAT/SDR1)` map until the real NanoKernel install runs,
which only S3 (two-supervisor reconciliation) provides. **S1 live MMU ⟺ S3; do NOT attempt a standalone
S1 live MMU — see `MMU-NANOKERNEL-INSEPARABILITY.md`** (the canonical finding + kill-switch). The only
truly-S1 work remaining pre-S3 is a cheap `SS_PROBE_PC` forge-state instrumentation pass; the live MMU is
folded into S3. **The next real work is S3 (months-scale, a user decision).** S2a-impl is DONE (inert).
Live ▶ box: `docs/HANDOFF.md`. Do NOT relitigate Route A.

**Standing rules for every milestone:** branch `macos-arm64`; never push unprompted; slot boots only
(`ss-slot-boot.sh`, never global pkill); `make test-jit` = 100; all new code behind an env gate +
`MachineProfileIsNewWorld()`, paravirtual byte-identical; `0xDEADBEEF` (M10 DR-reentry) = immediate stop.
