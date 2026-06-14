# Project Handoff — resume entry point

> ## ▶ RIGHT NOW (read this first)
> - **Aim:** boot Mac OS 9.2 (NewWorld) by **running the Trampoline producer**, not forging its outputs (Operation NewSheep; the M8→M17 forge arc is closed).
> - **State (2026-06-14):** both Task-0s DONE → **Route A GO but MONTHS**. Staged program planned (rev-4): **S1** paged MMU → **S2** loader+OF-CI+DT → **S3** two-supervisor reconciliation → **S4** disk IM-init→CGRP (critical path S1→S3→S4 ≈ quarters). Kickoff recon DONE: Discriminator-A=COARSE, donors extracted, 9.2 ISOs in hand.
> - **▶ NEXT ACTION:** **S1-impl Task A is building** (NewWorld paged MMU). S1-impl plan rev-2
>   (3-reviewer red-teamed): `docs/superpowers/plans/2026-06-14-ss-m18-s1-impl-paged-mmu.md`.
>   **Two decisive red-team findings reshaped it:** **(AD-2 FALSIFIED)** the NK's high-BAT writes
>   (SPR 560–575) are **feature-gated dead code** on every PVR SS presents (7400 `0x000c0000`; gate bit
>   `0x20` never set) → `high_bat[16]` is cheap INSURANCE, not a decision input; the recon-adversary's
>   AD-2 re-band over-weighted a dead premise. **(AD-1 CONFIRMED + deepened)** the live 4 KB PTE engine
>   remaps via **HTAB stores + `tlbie`, NOT `mtspr`** → a window hooking only `mtspr`/`mtsr`/`mtsrin`
>   silently desyncs; **window adequacy is owed to G1.e (a real boot); softmmu is the DEFAULT until
>   then.** **Task A (AUTHORIZED, gated, structurally inert):** append `high_bat[16]` at the struct TAIL
>   (after `msr` — JIT-safe per the static_asserts `ppc-cpu.cpp:861/866/868`; clean PPC recompile) +
>   `case 560…575` capture; build `machine/paged_mmu.cpp` (mechanism-agnostic segment+BAT+HTAB
>   translation) + `machine/test_paged_mmu.cpp` (SPEC-derived oracle; high-BAT rows dropped as dead).
>   Gates: build-ss + `make test-jit` score=100 (authoritative) + machine test. **Task B (the window)
>   PARKED** — needs a tlbie/HTAB-store interception design OR a G1.e PTE-engine-disjointness proof
>   (SURFACE to user). Off-path: S2a-impl cleared (deferred). Do NOT re-run the gating Task-0 /
>   Discriminator-A. Do NOT relitigate Route A.

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
