# Operation NewSheep — Decisions & Open Questions (live tracker)

> The charter (`README.md`) holds the stable vision/scope/DoD. **This doc tracks what's still
> undecided and why** — the load-bearing forks, their status, and how each will be resolved. Update
> the status column as questions close; move resolved items to the Decision Log at the bottom.

## Open questions (the forks that gate the effort)

| ID | Question | Why it's load-bearing | Resolved by | Status |
|----|----------|----------------------|-------------|--------|
| **Q0-A** | **Does Route A mean "implement OpenFirmware"?** The Trampoline is an OF *client* — it calls OF client-interface services. SheepShaver has no OF. | **This is the feasibility gate for the WHOLE effort, not a side risk** (sharpens charter R1). If the OF-call set is bounded/stubbable → Route A is viable. If open-ended → "Route A" is secretly "write an OpenFirmware," and the ladder collapses toward B/C. | Task-0: enumerate the OF client-interface calls the Trampoline makes (static disasm + QEMU trace) → bounded-and-stubbable vs open-ended. **GATES everything else.** | 🔴 OPEN — Task-0 step 1 |
| **Q0-B** | **Are the Trampoline's interrupt-setup writes constants/relocations, or computed from a live OF device tree?** | Decides Route **C**'s viability. M16 already found the handler PC is **ROM-absent (computed at runtime)**. If the writes are computed from an OF tree, C collapses into A (you must reproduce the computation AND its OF inputs) → C is "M16 with more steps." | Task-0: classify each interrupt-setup write (constant/reloc vs computed-from-OF-input), via QEMU value-trace + disasm. | 🔴 OPEN |
| **Q0-C** | **Can Route B be done as honest re-binding, not hardcoding?** | Route B risks begging the question. Honest B = "re-bind the Trampoline's reads to where our env puts things" (relocation fix). Dishonest B = "delete the compute, hardcode the result" = forging in a tbxi hat. | Red-team must force this line in the DoD: B is GO only if it's a relocation/env-adaptation, not a value-hardcode. | 🔴 OPEN — red-team to enforce |
| **Q0-D** | **Do 9.0.x RE findings transfer to 9.2?** | 9.1+ moved to a different parcels-based tbxi layout (charter R4). Task-0 on 9.0.1/9.0.4 may characterize a *structurally different* Trampoline than 9.2's. Risk: a clean 9.0.x RE creates false confidence about 9.2. | Get the genuine **9.2 "Mac OS ROM" file** early (separate artifact from the install ISO; may be sourceable independently) and diff its Trampoline layout against 9.0.x. | 🟡 OPEN — mitigate by sourcing 9.2 ROM early |
| **Q0-E** | **Which route?** (A run / B patch / C reproduce) | The effort's central fork. | Falls out of Q0-A + Q0-B + Q0-C once those close. | ⚪ BLOCKED on Q0-A/B/C |

## Method commitments (from the round-1 charter feedback, 2026-06-14)

- **QEMU as a Trampoline TRACER, not just an existence proof.** Single-step/trace the Trampoline
  under QEMU mac99: capture which OF services it calls (→ Q0-A) and which guest addresses it writes
  with which values (→ Q0-B). This is a **Task-0 step**, not a backstop — it answers both feasibility
  forks behaviorally before committing a route. *(Standing caveat: behavioral/structural only —
  QEMU's MMIO map + Cuda model are NOT our addresses.)*
- **Task-0 is variance-reduction on the Q0-A fork.** Point the cheap offline RE straight at the
  highest-variance unknown (OF-depth), not a general "disassemble and see."
- **Win condition stated plainly:** NewSheep's win = **clearing the IM-init frozen-struct class**.
  The expected *next* wall is the **M14 `[ALARM]` model-rejection / pre-System boot gate** (a known,
  separate downstream frontier), NOT Finder. Don't mis-sell success as "9.2 boots."
- **R3 (producer-run reveals a non-IM wall) is the GOOD outcome** — it means the producer approach
  worked and we're back on the machine-layer mainline with device models already staged.

## Decision Log (resolved — newest first)

- **2026-06-14 — Approach: run/reproduce the producer (Trampoline), not forge outputs.** Supersedes
  the M8→M17 forge approach (banked NO-GO). Route ladder A/B/C, chosen by Task-0. Rationale: charter
  §1–§3; round-1 feedback confirmed the framing sound.
- **2026-06-14 — 9.2 NewWorld is a HARD requirement** (Ken). Compatibility-payoff demoted to
  secondary track.
