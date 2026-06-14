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
> **▶ NEXT = OPERATION NEWSHEEP** (research effort, opened 2026-06-14; 9.2 NewWorld is a HARD
> requirement). The reframe: **run/reproduce the PRODUCER of the boot-time init (the Trampoline),
> not forge its outputs.** The Trampoline is an ELF parcel *inside the Mac OS ROM file we already
> hold*; on real HW it sets up the nanokernel's interrupts — exactly the frozen CGRP/IM structures.
> Running/reproducing it clears the whole frozen-struct *class* at once (vs. forging one wall at a
> time). Three-route ladder (run it / patch it offline via `tbxi` / informed host-reproduction),
> chosen by a cheap offline Task-0 (`tbxi dump` + disassemble the Trampoline — startable today on
> 9.0.x). **Charter + scope + risks + asset gaps: `docs/planning/newsheep/README.md`** (log /
> assets / glossary alongside). Baseline tag: `newsheep-baseline`. First milestone (Trampoline RE
> Task-0) runs the normal machine. Does NOT reopen the M15 FORGE verdict — it supersedes the forge
> *approach* with a producer-side one. The M14–M17 RE stays banked.
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
> ROM at `/Users/Shared/macemu/newworld-roms/`). First milestone = the Trampoline RE Task-0
> (`tbxi dump` + disassemble `MacOS.elf`; offline, startable today on the in-hand 9.2-era ROM),
> output = the run/patch/reproduce route decision. Baseline tag: `newsheep-baseline`.
>
> Compatibility-payoff (8.6–9.0.4 usability: CopyBits HLE, idle-skip, perf, app compat, Silicon
> Sheep) remains a valid *secondary* track but is not the current focus.
>
> Process: `docs/MILESTONE-WORKFLOW.md`. Never push without being asked. Never global pkill —
> slot boots only via `SheepShaver/tools/ss-slot-boot.sh`.

---

## Current state (2026-06-14 end-of-session)

- **Live aim:** Operation NewSheep (Trampoline RE Task-0 spec'd + red-teamed rev-2; held
  pre-execution). See the top banner + `docs/planning/newsheep/README.md`.
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
