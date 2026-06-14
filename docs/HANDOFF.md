# Project Handoff — resume entry point

> **Status: M15 COMPLETE 2026-06-14 — FORGE verdict verified** · The `SS_NW_IRQ_CONSUME`
> consumption path dead-ends: across **510 consumption boots** the 68k level-1 handler
> `0x5000ec50` is reached **0/510** (EXT edge re-fires into CGRP fallback `0x50325fd0`, not
> NK slot-4 service `0x50314660`); NK routing structs stay **frozen-zero across obs=1e9**
> (zero struct-populating writes); `SC#1=0x0d` is a PIC-off artifact (absent under
> `SS_NW_PIC=1`). Two independent pillars; verdict does NOT rest on a single chain.
> **Misroute-why diagnostic COMPLETE (2026-06-14): STRUCTURAL.** EXT IS delivered correctly to
> the published NK EXT entry `0x50314880`; the misroute is DOWNSTREAM, in the NK dispatcher.
> See `docs/planning/M15-FINDINGS-consumption-recon.md` "Addendum — misroute-why diagnostic".
>
> **M16 COMPLETE (2026-06-14) — DoD-3 NO-GO. NewWorld 9.x interrupt routing banked as
> forge-class.** Task-0 RE pinned the dead-end to the
> empty "CGRP" interrupt-group descriptor (`*(KDP-0x338)=0x68ffc1c0`): gate `[0x68ffc1e0]=1` needs
> ≥2, but the handler table is empty (`+0x38/+0x3c/+0x44`=0) and the service routine `0x503148e0`
> self-guards, so the one-word forge is SAFE but INERT. The milestone was re-scoped to **CGRP
> handler-table synthesis** (plan rev-4 / spec rev-2), then a pre-implementation red-team round
> (SHA `df627fe0`, 3 reviewers) fired the **pre-authorized early NO-GO**: the descriptor FORMAT is
> RE-tractable (`[entry+0]`=SRR0, `[entry+4]`=TOC, SRR1 from r19) but the **synthesis target value
> is ROM-absent** — `0x5000ec50` has **0 word-refs** in the 4 MB ROM (it is 68k code reached only
> via the DR emulator), and the `"CGRP"` tag is ROM-absent (runtime/disk-built by the IM-init that
> never runs). Scratch `0x68ff5000` is provably live (occupancy map); a populated table re-opens the
> M10 DR-reentry `0xDEADBEEF` crash class; CGRP is only the **first of N** frozen structs. The only
> surviving path — a host-owned NK-EXT-handler PPC stub — is materially larger and documented as the
> re-entry point if 9.x becomes a hard requirement. **RE banked (Q1–Q7):
> `docs/planning/M16-FINDINGS-oracle-forge.md`.** Plan/spec CLOSED (do-not-execute banners).
> **M17 CLOSED — red-team BLOCKED (`cc6fc422`).** The host-owned-stub plan's de-risk did NOT survive
> contact: the EXT-edge regime is **MODE_68K** (not the MODE_EMUL_OP the "sanctioned cross" needs),
> and the level-1 handler PC is a second ROM-absent value — the **series tripwire fired on wall 1**,
> proving per-wall forging is structurally bankrupt (every wall is the same disease). Spec/plan
> CLOSED.
>
> **▶ CURRENT = OPERATION NEWSHEEP** (research effort, opened 2026-06-14; 9.2 NewWorld is a HARD
> requirement). The reframe: **run/reproduce the PRODUCER of the boot-time init (the Trampoline),
> not forge its outputs.** The Trampoline is an ELF parcel *inside the Mac OS ROM file we already
> hold*; on real HW it sets up the nanokernel's interrupts — exactly the frozen CGRP/IM structures.
> Running/reproducing it clears the whole frozen-struct *class* at once (vs. forging one wall at a
> time). Does NOT reopen the M15 FORGE verdict — it supersedes the forge *approach*. M14–M17 RE banked.
> **Charter + scope + risks + asset gaps: `docs/planning/newsheep/README.md`.** Baseline: `newsheep-baseline`.
>
> **▶▶ Trampoline RE Task-0 COMPLETE (2026-06-14) → ROUTE A DECIDED.** Two instruments on the same
> `66210b4f…` ROM — static (capstone disasm of the Trampoline = top-level `MacOS.elf`, entry `0x20f078`;
> **177/177 OF call sites resolved, 0 unresolved**) + dynamic (QEMU mac99 gdbstub via a hand-written
> Python RSP client — no PPC gdb on host) — **mechanism-level agreement gate PASS.** Verdicts:
> **Q0-A = BOUNDED** (3 OF gateways: direct / hardcoded `call-method` / hardcoded `interpret`; 21
> services + 14 call-method targets + 4 *fixed* Forth literals; all DT paths/keys = standard Core99);
> **Q0-B = computed(OF-input)** (Trampoline reads `interrupt-map`/`-mask` → explains M16's ROM-absent
> CGRP handler PC; so route C ≡ A); **Q0-F = Trampoline + NanoKernel** (CGRP absent from `MacOS.elf`;
> NanoKernel-v02.27 parcel builds it downstream). **Route B excluded.** Load-bearing find: **OpenBIOS
> loads `MacOS.elf` at its ELF vaddr** (runtime PC=`0x20f078`, `r2=0x1001e8` == static) → the producer is
> hostable at its static addresses. **Findings + SS-integration sketch:
> `docs/planning/newsheep/FINDINGS-trampoline-re.md`**; forks closed in `…/DECISIONS.md`.
>
> **▶▶▶ NEXT = `SS_M18_TRAMPOLINE_LLE`** (code-writing; Route A: an OF client-interface callback +
> Core99 device tree + `call-method` backends + a 3-word `interpret` shim + a Trampoline loader, all
> behind `SS_M18_*` + `MachineProfileIsNewWorld()`, paravirtual byte-identical, `make test-jit`=100).
> **BINDING: open SS_M18 with its OWN gating Task-0 + red-team BEFORE any code** (the pattern that caught
> M16/M17 cheaply) on the two architectural collisions Task-0 under-examined: **(1) emulator-host
> ownership** — does the real NanoKernel REPLACE or FIGHT the M0–M13 supervisor/exception/MixedMode/
> scheduler scaffolding (weeks vs months)? **(2) MMU/V=P** — can `/mmu` claim/translate/map live in SS's
> flat V=P model, or does the NanoKernel force a real paged MMU (deferred in machine-layer M5)? Plus
> **(3)** trace the NanoKernel-v02.27 device-tree→CGRP construction directly under QEMU (Task-0 inferred
> it). Full statement: `FINDINGS-trampoline-re.md` "SS_M18 — gating risks".
> Standing fact: ring tool path is **`tools/ring-walk.py`** (repo-root `tools/`, NOT
> `SheepShaver/tools/`).
> For session logs: `docs/archive/2026-06/LEARNINGS-2026-06.md` + `docs/archive/2026-06/HANDOFF-SESSIONS-M9-M12.md`.

## How we got here — the M8→M17 forge arc (CLOSED, banked NO-GO)

> **CANONICAL lineage = `docs/planning/newsheep/GLOSSARY.md` "The M8→M17 lineage" table.** Read it
> for the per-milestone detail; this banner is NOT restated here (that drift caused the M14-wall
> contradiction). The arc proved forging the guest's interrupt/nanokernel structures is bankrupt —
> root cause = the **Trampoline never runs**. M15 = FORGE verdict (verified); M16 = DoD-3 NO-GO (CGRP
> target ROM-absent); M17 = red-team BLOCKED (EXT regime MODE_68K). **Load-bearing survivors:** M13 —
> NewWorld 68k interrupt *delivery* works (don't re-chase it); M14-FINDINGS VERDICT — the `[ALARM]`
> stall is a **Cuda device-model IFR/IER bug** (the expected post-NewSheep next wall), NOT a
> model-rejection gate. FINDINGS: `M14-FINDINGS-cuda-delivery.md`, `M15-FINDINGS-consumption-recon.md`,
> `M16-FINDINGS-oracle-forge.md` (all CLOSED/banked). Operation NewSheep supersedes the forge
> *approach* (not the M15 verdict) by running/reproducing the producer.

## Resume prompt

> Read `docs/HANDOFF.md` (this banner + "How we got here"), then `docs/AGENT-CONTEXT.md`.
>
> **Current focus: OPERATION NEWSHEEP** — boot Mac OS 9.2 (NewWorld; HARD requirement) by
> **running/reproducing the producer of the boot-time init (the Trampoline), not forging its
> outputs.** The M8→M17 forge arc is closed (banked NO-GO): every wall was the same disease —
> the Trampoline (an ELF parcel inside the Mac OS ROM file we already hold) never runs, so the
> nanokernel interrupt structures it would build stay frozen. **Start here: read the charter
> `docs/planning/newsheep/README.md`** (+ `GLOSSARY.md` for the M8→M17 lineage in one read,
> `ASSETS-AND-TOOLING.md` for the `tbxi` tooling + the 9.2 system-software gap — we hold the 9.2-era
> ROM at `/Users/Shared/macemu/newworld-roms/`). Then the **Trampoline RE Task-0 findings:
> `docs/planning/newsheep/FINDINGS-trampoline-re.md`** + the closed forks in `…/DECISIONS.md`.
> Baseline tag: `newsheep-baseline`.
>
> **Trampoline RE Task-0 is COMPLETE → Route A decided** (run the real Trampoline + NanoKernel against
> a SheepShaver-synthesized OF client interface + Core99 device tree — see the banner). **Your job =
> the NEXT milestone, `SS_M18_TRAMPOLINE_LLE` (code-writing).** Per the milestone machine, **open it
> with its OWN gating Task-0 + red-team BEFORE writing any code**, on the two collisions Task-0
> under-examined (emulator-host ownership + MMU/V=P) and a direct trace of the NanoKernel→CGRP step —
> all in `FINDINGS-trampoline-re.md` "SS_M18 — gating risks". Do NOT relitigate the Route A decision
> (settled, well-evidenced); do the architecture-conflict recon that decides HOW to build it.
>
> Compatibility-payoff (8.6–9.0.4 usability: CopyBits HLE, idle-skip, perf, app compat, Silicon
> Sheep) remains a valid *secondary* track but is not the current focus.
>
> Process: `docs/MILESTONE-WORKFLOW.md`. Never push without being asked. Never global pkill —
> slot boots only via `SheepShaver/tools/ss-slot-boot.sh`.

---

## Current state (2026-06-14 end-of-session)

- **Live aim:** Operation NewSheep. **Trampoline RE Task-0 COMPLETE (2026-06-14) → Route A decided**
  (findings `docs/planning/newsheep/FINDINGS-trampoline-re.md`). **Next = `SS_M18_TRAMPOLINE_LLE`**
  (code-writing; open it with its own gating Task-0 + red-team first). See the top banner +
  `docs/planning/newsheep/README.md` §9.
- **Per-milestone M8→M17 history:** see the **canonical lineage table** in
  `docs/planning/newsheep/GLOSSARY.md` (do not restate it here). M10/M11/M11a COMPLETE; M12 PARTIAL;
  M13 = delivery-works retraction; M14–M17 = the forge arc, CLOSED/banked (FINDINGS docs per the table).

### Session history (relocated)

The full M9 root-cause analysis, the M10/M11 "what shipped" tables, and the session-3 /
session-4 probe campaigns now live in **`docs/archive/2026-06/HANDOFF-SESSIONS-M9-M12.md`**.
The **current, verified** interrupt-delivery diagnosis (which reframes several of those
findings — e.g. the M10 `0x5000ed08` probe-match is now a falsified negative result) is
**`docs/planning/M13-FINDINGS-interrupt-delivery.md`**. Read M13-FINDINGS, not the session log,
for the live picture.

---

## Standing operational notes

- Branch `macos-arm64`; never push unprompted.
- Parallel boots via `SheepShaver/tools/ss-slot-boot.sh` only (never global pkill).
- Cleanup: `SheepShaver/tools/ss-reap.sh`.
- Queued ideas: `docs/planning/BACKLOG.md`.
