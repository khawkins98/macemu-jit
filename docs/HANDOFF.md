# Project Handoff — resume entry point

> ## ▶ RIGHT NOW (read this first)
> - **Aim:** boot Mac OS 9.2 (NewWorld) by **running the Trampoline producer**, not forging its outputs (Operation NewSheep; the M8→M17 forge arc is closed).
> - **State (2026-06-14):** both Task-0s DONE → **Route A GO but MONTHS**. Staged program planned (rev-4): **S1** paged MMU → **S2** loader+OF-CI+DT → **S3** two-supervisor reconciliation → **S4** disk IM-init→CGRP (critical path S1→S3→S4 ≈ quarters). Kickoff recon DONE: Discriminator-A=COARSE, donors extracted, 9.2 ISOs in hand.
> - **▶ NEXT ACTION:** **S1-impl gating first step** (NewWorld paged MMU). S1 Task-0 recon DONE + 3-reviewer red-teamed → **RESIDUE-PASS** (`FINDINGS-s1-paged-mmu.md` rev-2): window (mach `vm_remap` on the separate NATMEM RAM/ROM reservations — NOT MEM_BULK) is the *preferred* mechanism, BUT the adversary FALSIFIED two rev-1 claims from primary parcel bytes: (AD-1) the NK runs a **live 4 KB PTE engine** (HTAB not "residual"); (AD-2) the NK programs **high BATs SPR 560–575** that SheepShaver drops (`ppc-execute.cpp:1651`) and the oracle `(SR,BAT[16],SDR1,EA)` contract can't express. → **walker/softmmu is the DEFAULT**, and S1-impl does NOT start by betting on the window. **Gating first step:** append `high_bat[16]` to the supervisor block (BEFORE the MUST-stay-LAST block — struct-offset hazard, needs clean PPC recompile), capture SPR 560–575 in the `mtspr` handler, extend the oracle I/O contract to `(SR[16],BAT[16],HIGH_BAT[16],SDR1,EA)`, then build the G1.a-pure oracle test + FINE/high-BAT battery → that measurement earns the window (or confirms softmmu). Env-gated `SS_M18_PAGED_MMU ∧ MachineProfileIsNewWorld()`, paravirtual byte-identical, `make test-jit`=100. Plan `…/2026-06-14-ss-m18-s1-paged-mmu-task0.md`. Off-path: S2a Task-0 recon (plan rev-2 red-teamed). Do NOT re-run the gating Task-0 / Discriminator-A. Do NOT relitigate Route A.

## Resume chain (read in this order; see CONTRIBUTING §0b for the discipline)

1. **This file** — the RIGHT NOW box above is the live state; the rest is pointers + standing rules.
2. **`docs/AGENT-CONTEXT.md`** — standing facts, constants, instrument caveats, gate tiers.
3. **`docs/planning/newsheep/README.md`** — the Operation NewSheep charter (§9 = status/next).
4. **The program plan's STATUS table** (`…/plans/2026-06-14-ss-m18-trampoline-lle-program.md`) — the
   per-stage done-vs-todo board; **this is the canonical "what's left" record** (the in-session task
   list does NOT persist).
5. **Findings the next action needs:** `FINDINGS-trampoline-re.md` (incl. "SS_M18 gating Task-0"),
   `FINDINGS-discriminator-a.md`, `DONOR-NOTES.md`.

Process: `docs/MILESTONE-WORKFLOW.md`. **Never push without being asked. Never global pkill — slot boots
only via `SheepShaver/tools/ss-slot-boot.sh`.** Baseline tag: `newsheep-baseline`.

## Frontier detail (the RIGHT NOW box, expanded)

- **Operation NewSheep** — run/reproduce the **producer** of the boot-time init (the Trampoline, an ELF
  parcel inside the Mac OS ROM file we hold + the NanoKernel) instead of forging the frozen CGRP/IM
  structures. Supersedes the forge *approach* (not the M15 verdict). Charter + scope + risks:
  `docs/planning/newsheep/README.md`.
- **Both Task-0s complete →** Trampoline RE: **Route A decided** (run the real Trampoline + NanoKernel vs
  a synthesized OF-CI + Core99 device tree). SS_M18 gating Task-0: **GO but MONTHS** — three independent
  month-forcing findings: **Q1** the real NanoKernel is a permanently-resident supervisor with no handoff
  boundary; **Q2** it requires a real paged MMU; **Q3** CGRP is built by disk/CFM IM-init, NOT the NK
  parcel (corrects Q0-F, confirms M16). Detail: `FINDINGS-trampoline-re.md`.
- **Staged program (rev-4)** + kickoff recon done: Discriminator-A=COARSE (→ S1 Dolphin shadow-arena),
  donors extracted (`DONOR-NOTES.md`), 9.2.1/9.2.2 ISOs in hand (`ASSETS-AND-TOOLING.md` R2). Per-stage
  status: the program plan's STATUS table.
- **Standing fact:** the ring-walk tool is **`tools/ring-walk.py`** (repo-root `tools/`, NOT
  `SheepShaver/tools/`).

## How we got here — the M8→M17 forge arc (CLOSED, banked NO-GO)

> **CANONICAL lineage = `docs/planning/newsheep/GLOSSARY.md` "The M8→M17 lineage" table** — read it for
> the per-milestone detail; it is NOT restated here (restating it caused the M14-wall contradiction). The
> arc proved forging the guest's interrupt/nanokernel structures is bankrupt; root cause = the Trampoline
> never runs. **Load-bearing survivors:** M13 — NewWorld 68k interrupt *delivery* works (don't re-chase
> it); M14-FINDINGS — the `[ALARM]` stall is a **Cuda IFR/IER device-model bug** (the expected
> post-NewSheep next wall), NOT a model-rejection gate. Banked FINDINGS:
> `M14-FINDINGS-cuda-delivery.md`, `M15-FINDINGS-consumption-recon.md`, `M16-FINDINGS-oracle-forge.md`.
> M10/M11/M11a COMPLETE; M12 PARTIAL; M13 retraction; M14–M17 = the forge arc (CLOSED).

## Standing operational notes

- Branch `macos-arm64`; never push unprompted.
- Parallel boots via `SheepShaver/tools/ss-slot-boot.sh` only (never global pkill); cleanup
  `SheepShaver/tools/ss-reap.sh`.
- All new code behind an env gate + `MachineProfileIsNewWorld()`; paravirtual byte-identical;
  `make test-jit` = 100. `0xDEADBEEF` (M10 DR-reentry) = immediate stop.
- Queued ideas: `docs/planning/BACKLOG.md`.
- Session logs / older history: `docs/archive/2026-06/LEARNINGS-2026-06.md`,
  `docs/archive/2026-06/HANDOFF-SESSIONS-M9-M12.md`; current interrupt-delivery diagnosis (reframes the
  old session log) = `docs/planning/M13-FINDINGS-interrupt-delivery.md`.
