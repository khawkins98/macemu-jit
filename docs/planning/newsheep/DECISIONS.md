# Operation NewSheep — Decisions & Open Questions (live tracker)

> The charter (`README.md`) holds the stable vision/scope/DoD. **This doc tracks what's still
> undecided and why** — the load-bearing forks, their status, and how each will be resolved. Update
> the status column as questions close; move resolved items to the Decision Log at the bottom.

> **ALL Q0 forks CLOSED 2026-06-14 by Task-0 execution.** Findings:
> `docs/planning/newsheep/FINDINGS-trampoline-re.md`. **Route A decided** (run the real Trampoline +
> NanoKernel against a synthesized OF-CI + Core99 device tree). The table below is kept for the record;
> the resolutions are in the Decision Log.

## Open questions (the forks that gate the effort) — ALL CLOSED

| ID | Question | Why it's load-bearing | Resolved by | Status |
|----|----------|----------------------|-------------|--------|
| **Q0-A** | **Does Route A mean "implement OpenFirmware"?** The Trampoline is an OF *client* — it calls OF client-interface services. SheepShaver has no OF. | **This is the feasibility gate for the WHOLE effort, not a side risk** (sharpens charter R1). If the OF-call set is bounded/stubbable → Route A is viable. If open-ended → "Route A" is secretly "write an OpenFirmware," and the ladder collapses toward B/C. | Task-0: enumerate the OF client-interface calls the Trampoline makes (static disasm + QEMU trace) → bounded-and-stubbable vs open-ended. **GATES everything else.** | ✅ **CLOSED — BOUNDED.** 3 OF gateways, 21 direct services + 14 call-method targets + 4 fixed-Forth `interpret`; all finddevice paths/getprop keys are standard Core99 DT. Static (177/177 resolved) + QEMU trace agree at mechanism level. |
| **Q0-B** | **Are the Trampoline's interrupt-setup writes constants/relocations, or computed from a live OF device tree?** | Decides Route **C**'s viability. M16 already found the handler PC is **ROM-absent (computed at runtime)**. If the writes are computed from an OF tree, C collapses into A (you must reproduce the computation AND its OF inputs) → C is "M16 with more steps." | Task-0: classify each interrupt-setup write (constant/reloc vs computed-from-OF-input), via QEMU value-trace + disasm. | ✅ **CLOSED — computed(OF-input).** Trampoline reads `interrupt-map`/`-mask` (QEMU: ×6 each); routing data derives from the OF tree, not ROM constants. So **C ≡ A**. (Explains M16's ROM-absent handler PC.) |
| **Q0-C** | **Can Route B be done as honest re-binding, not hardcoding?** | Route B risks begging the question. Honest B = "re-bind the Trampoline's reads to where our env puts things" (relocation fix). Dishonest B = "delete the compute, hardcode the result" = forging in a tbxi hat. | Red-team must force this line in the DoD: B is GO only if it's a relocation/env-adaptation, not a value-hardcode. | ✅ **CLOSED — moot, B excluded.** The Trampoline's blocker is the absence of an OF env, not relocation (OpenBIOS already runs it at its ELF vaddr). `tbxi build` can't supply an OF env; any value-hardcode B = the banked forge. |
| **Q0-D** | **Do RE findings transfer across ROM versions to 9.2?** | 9.1+ moved to a different parcels-based tbxi layout. Risk: RE on the wrong version characterizes a structurally different Trampoline. | **LARGELY RESOLVED (2026-06-14):** we now hold the **full ROM-file progression 1998→2003** (`~/Downloads/New_World_Mac_Roms`), incl. 9.2-era ROMs (8.4=9.2/9.2.1, **9.0.1=9.2.2 == our active ROM**, byte-identical). RE runs directly on the 9.2-era ROM; the cross-version set answers the layout-diff question by direct comparison. `tbxi`-verify internal OS versions in Task-0. | 🟢 mostly closed — ROM in hand |
| **Q0-F** | **Who builds CGRP — the Trampoline, or the NanoKernel from the device tree?** | Red-team RE found `CGRP` absent from the Trampoline (`MacOS.elf`); it builds the OF device tree + page map, the NanoKernel builds CGRP downstream. Decides whether the producer is Trampoline-alone or **Trampoline + NanoKernel** — changes Q0-E + the SS sketch. | Task-0 T0.1/T0.2: test "Trampoline writes NK structs directly" vs "only produces the device tree the NK consumes" (with a real null). | ✅ **CLOSED — Trampoline + NanoKernel.** `CGRP` absent from `MacOS.elf`; Trampoline reads interrupt-map + claims memory (no NK-struct writes, static + QEMU). The NanoKernel-v02.27 parcel builds CGRP from the DT downstream. |
| **Q0-E** | **Which route?** (A run / B patch / C reproduce) | The effort's central fork. | Falls out of Q0-A + Q0-B + Q0-C + Q0-F once those close. | ✅ **CLOSED — Route A** (run the real Trampoline+NanoKernel against a synthesized OF-CI + Core99 DT). Passes the S3 mechanical-feasibility check. SS sketch in FINDINGS. |

## Method commitments (from the round-1 charter feedback, 2026-06-14)

- **QEMU as a Trampoline TRACER, not just an existence proof.** Single-step/trace the Trampoline
  under QEMU mac99: capture which OF services it calls (→ Q0-A) and which guest addresses it writes
  with which values (→ Q0-B). This is a **Task-0 step**, not a backstop — it answers both feasibility
  forks behaviorally before committing a route. *(Standing caveat: behavioral/structural only —
  QEMU's MMIO map + Cuda model are NOT our addresses.)*
- **Task-0 is variance-reduction on the Q0-A fork.** Point the cheap offline RE straight at the
  highest-variance unknown (OF-depth), not a general "disassemble and see."
- **Win condition stated plainly:** NewSheep's win = **clearing the IM-init frozen-struct class**.
  The expected *next* wall is the one M14-FINDINGS pinned: the **Cuda device-model IFR/IER bug**
  (`sr_int_pending` never reaches VIA IFR; NK polls IER) — NOT a "model-rejection gate" (retracted). (a known,
  separate downstream frontier), NOT Finder. Don't mis-sell success as "9.2 boots."
- **R3 (producer-run reveals a non-IM wall) is the GOOD outcome** — it means the producer approach
  worked and we're back on the machine-layer mainline with device models already staged.

## Decision Log (resolved — newest first)

- **2026-06-14 — Doc-sync sweep (tech-writer + dev-advocate).** Cross-doc sync pass after the heavy
  session; folded both reviews. Caught + fixed stale resume-chain next-actions (HANDOFF resume-prompt /
  current-state + AGENT-CONTEXT "where things are" still said "open SS_M18's gating Task-0" — done),
  added a RIGHT NOW box (HANDOFF) + a per-stage STATUS table (program plan), and committed `gdbcli.py`
  into the repo (was /tmp-only). Resume chain now coherent for a cold start.
- **2026-06-14 — Kickoff recon DONE (3 zero-dependency workstreams).** Discriminator-A = **COARSE**
  (`FINDINGS-discriminator-a.md`) → Stage-1 path a (Dolphin Dynamic-BAT shadow-arena) indicated, JIT fast
  path preserved (residual fine-PTE falsifier routed to S1's MMU-oracle test); donors extracted
  (`DONOR-NOTES.md` — full SHAs; `UpdateDBATMappings` is in Dolphin `HW/Memmap.cpp`); **9.2.x ISOs found**
  (9.2.1 `3f129e03…`, 9.2.2 `2cfb856b…`; S4 asset block CLEARED, `ASSETS-AND-TOOLING.md` R2).
- **2026-06-14 — SS_M18 staged program planned (user-approved "plan the full months program now").**
  rev-4 plan `docs/superpowers/plans/2026-06-14-ss-m18-trampoline-lle-program.md`: S1 paged MMU → S2
  loader+OF-CI+DT → S3 two-supervisor reconciliation → S4 disk IM-init→CGRP; critical path S1→S3→S4 ≈
  multiple quarters. Red-teamed (2 reviewers) + tech-writer/dev-advocate reviewed. Each stage opens with
  its own Task-0 + red-team. NEXT = Stage 1's deep Task-0.
- **2026-06-14 — SS_M18 gating Task-0 EXECUTED → Route A GO but MONTHS (weeks-sketch falsified).** Plan
  → 2 red-teamers → 2 boot-disjoint recon agents. Three independent month-forcing findings: **Q1** the
  real NanoKernel-v02.27 is a permanently-resident paged supervisor with NO handoff boundary (all 5
  ledger surfaces FIGHT; Execute68k pair is an SS synthetic); **Q2** it requires a real paged MMU
  (un-defer machine-layer M5); **Q3** [QEMU-BEHAVIORAL] **CGRP is built by disk/CFM IM-init, NOT the NK
  parcel** — this CORRECTS Q0-F's inference (below) and confirms M16. Findings: `FINDINGS-trampoline-re.md`
  "SS_M18 gating Task-0".
- **2026-06-14 — Q0-F CORRECTION (supersedes the row above / the earlier "Trampoline+NanoKernel builds
  CGRP" entry).** Direct QEMU observation in the SS_M18 gating Task-0 (Q3) found CGRP is materialized by
  **disk/CFM-loaded IM-init in low RAM (0x0045xxxx–0x0046xxxx, shared CFM TOC)**, NOT by the
  NanoKernel-v02.27 parcel. The earlier Q0-F *inference* ("NanoKernel builds CGRP from the DT downstream")
  is **superseded** — M16's RE ("CGRP" tag 0× in ROM; no inline builder) was right. Route A therefore
  must run the disk System/Enabler IM-init in the loop (program Stage 4).
- **2026-06-14 — Task-0 EXECUTED → Route A decided; Q0-A/B/C/E/F all CLOSED.** Two instruments
  (static capstone disasm of `MacOS.elf` + dynamic QEMU mac99 gdbstub trace via a hand-written Python
  RSP client) on the same `66210b4f…` ROM. **Q0-A = BOUNDED** (3 OF gateways; 21 direct services +
  14 `call-method` targets + 4 fixed-Forth `interpret`; 177/177 static call sites resolved; all
  finddevice paths/getprop keys = standard Core99 DT). **Q0-B = computed(OF-input)** (Trampoline reads
  `interrupt-map`/`-mask`; QEMU confirmed ×6 each). **Q0-F = Trampoline + NanoKernel** (CGRP absent from
  `MacOS.elf`; NanoKernel-v02.27 builds it downstream). **Mechanism-level agreement gate: PASS**
  (OpenBIOS≠AppleOF value/count/path divergences logged, not blocking). Load-bearing find: **OpenBIOS
  loads the Trampoline at its ELF vaddr** (PC=`0x20f078`, `r2=0x1001e8` at runtime == static), so the
  3 wrappers are at their static addresses. **DECISION = Route A** (run the real Trampoline+NanoKernel
  against a SheepShaver-synthesized OF client interface + Core99 device tree; C≡A since writes are
  computed; B excluded — can't supply an OF env by patching). Passes the S3 feasibility check
  (`MacOS.elf` relocatable, single OF-CI entry dependency). FINDINGS + SS-integration sketch:
  `FINDINGS-trampoline-re.md`. Next milestone = `SS_M18_TRAMPOLINE_LLE` (code-writing; held for user).

- **2026-06-14 — Task-0 spec/plan red-team folded (rev 2):** 3 reviewers, all GO-WITH-FIXES (one
  empirically installed tbxi + dumped our ROM). Producer reframe (Q0-F: Trampoline=`MacOS.elf` builds
  the device tree; NanoKernel builds CGRP → producer is likely Trampoline+NanoKernel). Agreement gate
  redefined mechanism-level (same ROM binary verified, but OpenBIOS≠AppleOF → value divergence expected).
  Route A "bounded"≠"cheap" (must cost the getprop key set). Tracer: Python gdb-remote client + `-S`
  (no PPC gdb on host). Added per-route mechanical-feasibility check; DoD-negative is a costed first-class
  outcome. tbxi facts pinned (`dump -o`, `MacOS.elf`). See plan/spec rev-2.
- **2026-06-14 — Task-0 method/version/budget locked (brainstorm):** (1) **both instruments
  (static `tbxi`-RE + QEMU trace), gated on agreement** — divergence = its own investigation;
  (2) **both on 9.2** (version-matched so the agreement gate is honest); (3) **QEMU tracer is
  required, open budget** (load-bearing for the gate; no silent static-only fallback).
- **2026-06-14 — ROM collection found** (`~/Downloads/New_World_Mac_Roms`): full 1998→2003 ROM-file
  progression; the 9.2-era ROM is in hand (our active "9.0.1" ROM is the Dec-2001 / 9.2.2-era file).
  "Source 9.2 ROM first" is effectively solved; reframe-to-verify = our "9.0.1" label is the ROM
  *file* version, the ROM is 9.2.2-era. Remaining 9.2 gap = system software (not on Task-0 path).
- **2026-06-14 — Approach: run/reproduce the producer (Trampoline), not forge outputs.** Supersedes
  the M8→M17 forge approach (banked NO-GO). Route ladder A/B/C, chosen by Task-0. Rationale: charter
  §1–§3; round-1 feedback confirmed the framing sound.
- **2026-06-14 — 9.2 NewWorld is a HARD requirement** (Ken). Compatibility-payoff demoted to
  secondary track.
