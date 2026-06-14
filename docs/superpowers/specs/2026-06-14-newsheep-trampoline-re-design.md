# Operation NewSheep — Trampoline RE (Task-0) Design Spec

**Status:** rev 1 · 2026-06-14 · pre-red-team DRAFT
**Effort:** Operation NewSheep (`docs/planning/newsheep/README.md`) · baseline tag `newsheep-baseline`
**Live forks:** `docs/planning/newsheep/DECISIONS.md` (Q0-A…E)
**Predecessors (banked):** M14/M15/M16-FINDINGS (forge-class NO-GO arc); M17 spec/plan CLOSED.

> **What this is:** the first NewSheep milestone — a **reverse-engineering recon** that decides *how*
> we integrate the NewWorld Trampoline into SheepShaver to boot Mac OS 9.2. It is **RE-only: Task-0
> writes no SheepShaver code.** That is a phase boundary, NOT a statement about the effort —
> **SheepShaver integration is the terminal goal**, and Task-0's entire output is the integration
> design input for the next milestone.

---

## §1 — Problem & Definition of Done

### Problem
NewWorld 9.x boot dead-ends because the guest's nanokernel interrupt structures (the "CGRP" group
descriptor, the routing tables) are never built — the **Trampoline**, the OpenFirmware-client ELF
bootloader (a parcel *inside the Mac OS ROM file*) that "copies/modifies the OF device tree and sets
up interrupts for the nanokernel," never runs in SheepShaver. We synthesize a partial substitute
(`SS_NW_TRAMPOLINE`). The M8→M17 arc proved that forging the Trampoline's *outputs* is bankrupt
(M16 DoD-3 NO-GO: the handler PC is runtime-computed/ROM-absent; M17 BLOCKED: the EXT regime is
MODE_68K; the series tripwire fired on wall 1). The producer must be run or faithfully reproduced.

Before we can do that, we must answer **how** — which of three integration routes is viable, gated
by what the Trampoline actually does and needs. That is this milestone.

### What Task-0 decides (the forks — full text in `DECISIONS.md`)
- **Q0-A (GATING): does Route A mean implementing OpenFirmware?** The Trampoline is an OF client.
  Enumerate the OF client-interface calls it makes → **bounded-and-stubbable** (Route A viable) vs
  **open-ended** (Route A collapses toward B/C).
- **Q0-B: are the interrupt-setup writes constants/relocations, or computed from a live OF tree?**
  Decides Route C: if computed, C collapses into A (must reproduce the computation + its OF inputs).
- **Q0-C: can Route B be honest re-binding (relocation), not value-hardcoding?** (Else B is the
  forge in a tbxi hat.)
- **Q0-D: ROM version transfer** — largely resolved (we hold the 9.2-era ROM; verify internal
  versions via `tbxi`); confirm the RE targets the 9.2.x Trampoline.

### Definition of Done
1. **Q0-A/B/C/D answered, cross-instrument** (static `tbxi`-RE **and** QEMU trace **agree**; each
   divergence recorded as its own investigation, never silently averaged).
2. **A committed route decision (A / B / C)** with rationale, INCLUDING an **SS-integration sketch**
   for the chosen route: the concrete integration point (e.g., the `SS_NW_TRAMPOLINE` handoff), the
   env-gate (`SS_M18_*` + `MachineProfileIsNewWorld()`), and what `SS_NW_TRAMPOLINE` becomes
   (replaced by executing the parcel / augmented / fed a tbxi-patched ROM).
3. **Findings doc** `docs/planning/newsheep/FINDINGS-trampoline-re.md` capturing the OF-call
   surface, the write classification, and the agreement/divergence record — framed as *what each
   means for SS integration*, not just "what the Trampoline does."
4. **No code, no SheepShaver change, no boots beyond QEMU tracing.** `make test-jit` is untouched
   (nothing to gate); paravirtual unaffected by construction.

A valid outcome is also **DoD-negative**: if Q0-A proves the OF surface open-ended AND Q0-B proves
the writes are OF-tree-computed, the decision may be "Route A is an OF-implementation effort; choose
B or C," or "park with the integration cost documented." Partial findings beat stalling.

---

## §2 — Scope

**IN:**
- `tbxi` tooling bring-up + ROM identity verification (resolve the "9.0.1 = 9.2.2-era" reframe).
- Static RE of the Trampoline ELF parcel (capstone, PPC BE): OF-call enumeration + write
  classification.
- A QEMU mac99 Trampoline **tracer** (9.2.1): locate + breakpoint the Trampoline; trace its OF
  service calls and guest writes (addr+value). **Required, open budget** (load-bearing for the
  agreement gate).
- Reconciliation under the agreement gate; the route decision + SS-integration sketch.

**OUT (Task-0 only — these are the NEXT milestone, and the effort's goal):**
- Writing any SheepShaver code, env gate, or `SS_NW_TRAMPOLINE` change.
- Implementing the chosen route (running / patching / reproducing the Trampoline).
- Forging any guest structure.
- Booting SheepShaver (QEMU is RE-only here).
- Sourcing 9.2 system software (needed only for the later SS *boot*, not for this RE).

---

## §3 — Method (the two instruments + the gate)

**Locked decisions (brainstorm 2026-06-14):** (1) **both instruments, gated on agreement**;
(2) **both on 9.2** (version-matched: the in-hand 9.2-era ROM + QEMU 9.2.1); (3) **QEMU tracer
required, open budget** — no static-only fallback.

**Leverage banked SS-milestone RE (do not re-derive).** The plan's "Prior-art" section catalogs the
M8→M17 + machine-layer facts the RE cross-references: the KDP/ROMBase/exception-entry constants, the
M16 CGRP descriptor + service-routine layout (`*(KDP-0x338)=0x68ffc1c0`, gate/guard/base/count, `0x503148e0`),
the Execute68k emulator pair, and the **existing `SS_NW_TRAMPOLINE` synthesis** we are deciding to
replace/augment. Critically, **three address spaces stay distinct** — the Trampoline-ELF vaddr (static),
QEMU's guest map (dynamic, NOT our addresses), and SheepShaver's synthesized space (where the M-series
facts live, and the target for the SS-integration sketch). Structures are matched by tag/field-role
across spaces, never by literal address.

### Instrument 1 — static (`tbxi` + capstone)
1. `pip install tbxi`; `tbxi dump` the candidate 9.2-era ROMs (`/Users/Shared/macemu/newworld-roms/`,
   esp. `Mac OS ROM 8.4` ≈ 9.2/9.2.1 and `9.0.1` ≈ 9.2.2 = our active ROM). Read internal
   version/build strings → confirm the canonical 9.2.x ROM (Q0-D).
2. Extract the **Trampoline ELF** parcel; disassemble (capstone, `CS_ARCH_PPC`, `CS_MODE_BIG_ENDIAN`).
3. **Q0-A:** identify the OF client-interface call mechanism (the OF entry/callback pointer) and
   enumerate every call site → the OF-service set; judge bounded-and-stubbable vs open-ended.
4. **Q0-B:** for each nanokernel interrupt-setup write (to the CGRP/IM structures), classify the
   source value: immediate/relocation (cheaply reproducible) vs computed from an OF-tree read.

### Instrument 2 — dynamic (QEMU mac99 tracer)
1. Boot 9.2.1 under `SheepShaver/tools/qemu-rig.sh`; locate where the Trampoline runs (its load
   address / entry); set breakpoints via the monitor.
2. Trace: the OF service calls it makes (→ Q0-A corroboration) and the guest writes it performs
   (addr+value → Q0-B corroboration: see which writes are computed at runtime).
3. **Caveat (standing):** behavioral/structural only — QEMU's MMIO map (MacIO `0x80000000`) and Cuda
   model are NOT our addresses; never cite QEMU addresses as reference values.

### The agreement gate
Q0-A (OF-call set) and Q0-B (const-vs-computed classification) close **only when static and dynamic
corroborate.** A divergence is logged as its own investigation in `DECISIONS.md` and resolved before
the route decision — never silently reconciled.

### Sequence
- **T0.0** tooling + ROM identity (verify the 9.2.x ROM; the reframe).
- **T0.1** static: OF-call enumeration + write classification.
- **T0.2** dynamic: stand up the tracer; trace OF calls + writes. (Required, open budget.)
- **T0.3** reconcile under the agreement gate; resolve/flag divergences.
- **T0.4** decide: close Q0-A/B/C/D; commit the route decision + SS-integration sketch.

---

## §4 — Key risks
- **R1 — QEMU Trampoline-tracer bring-up (top risk).** Locating + breakpointing the Trampoline as it
  runs under mac99 is non-trivial; it is load-bearing for the agreement gate (open budget per the
  decision). Mitigate: use the device-tree/`info` oracle the rig already captures to find the
  Trampoline's load address; lean on the static disasm to predict where to break.
- **R2 — Q0-A may find the OF surface OPEN-ENDED.** That is a legitimate *finding* (pushes the route
  toward B/C), not a Task-0 failure. The DoD admits it explicitly.
- **R3 — instrument divergence.** Static and dynamic may disagree (version skew, an unexecuted static
  path). Each divergence → its own investigation; never averaged.
- **R4 — Route C ≠ distinct from the buried forge** unless Q0-B says the writes are constants/relocs.
  Hold this line in the route decision; the red-team enforces it (Q0-C).
- **R5 — over-running the open budget on R1.** The tracer is required, but record progress honestly;
  if bring-up stalls, surface it as a decision point to the user (don't silently grind).

---

## §5 — SheepShaver integration (the terminal goal — why Task-0's RE matters)
SS integration is the effort's north star; Task-0 is RE-only solely because it must decide the
integration *shape* first. The route decision's SS-integration sketch (DoD §1.2) feeds the **next
milestone**, which DOES write SS code:
- **A** — SS executes the Trampoline parcel at the `SS_NW_TRAMPOLINE` handoff (replacing the synthesis).
- **B** — SS loads the tbxi-patched ROM (artifact swap; minimal SS code).
- **C** — SS host-reproduces the Trampoline's writes, informed by the disasm (not blind).
All behind `SS_M18_*` + `MachineProfileIsNewWorld()`, paravirtual byte-identical, `make test-jit`=100.

---

## §6 — Self-review notes
- Every DoD item maps to a sub-task: Q0-A→T0.1/T0.2, Q0-B→T0.1/T0.2, Q0-C→T0.4 (route decision),
  Q0-D→T0.0; agreement gate→T0.3; route+SS-sketch→T0.4. ✓
- The three brainstorm locks (both/agreement, 9.2-version, tracer-required) are in §3. ✓
- "No SS code" is scoped to Task-0 and explicitly reconciled with the SS-integration goal (§5). ✓
- No placeholders; scope is single-milestone (RE recon); ambiguities (route fork) are the *output*,
  not unresolved spec gaps.
