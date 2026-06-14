# Project Handoff — resume entry point

> **Status: PAUSED 2026-06-14** · M13 plan ready to execute · Resume: read this doc, then `docs/AGENT-CONTEXT.md`.
> For session logs: `docs/archive/2026-06/LEARNINGS-2026-06.md` + `docs/archive/2026-06/HANDOFF-SESSIONS-M9-M12.md`.

## Resume prompt

> Read `docs/HANDOFF.md`, then `docs/AGENT-CONTEXT.md` (authoritative frontier + constants).
> **M13 STRATEGY DECIDED (2026-06-13), REDRAFTED (2026-06-14).** Read in order:
> `docs/planning/NANOKERNEL-STRATEGY-DECISION.md` (THE decision — "COMPLETE OUR OWN"),
> `docs/planning/M13-FINDINGS-interrupt-delivery.md` (the verified 3-stage diagnosis), then the
> redrafted plan `docs/planning/superpowers/plans/2026-06-14-m13-nk-interrupt-delivery.md`.
> **Decided model:** keep SheepShaver + Apple's NanoKernel; the gap is unwired eager interrupt
> delivery + the unmodeled EXT-fallback→DR-autovector handoff at `0x50325f00` (set DR `cr2lt`).
> Do NOT fork the NK / switch base / borrow device models. **Host-side 68k injection falsified 5×
> AND forging the CGRP table (= M10 crash) — do NOT retry** (warnings in the STUB code).
> `irq_fired` is MISLEADING (NK-level consume, not 68k delivery); NewWorld is paravirtual (not VIA).
> Plan tasks (step-0 RESOLVED 2026-06-14, see plan Rev 2): wall = **idle spin `0x50468ae4`** (not MMU);
> **eager delivery FALSIFIED** (EXT already saturates the fallback ≥10000×, boot doesn't advance) →
> **Task A demoted to a thin EXT precondition; Task C is the sole lever** = HLE the NK→DR handoff at
> `0x50325f00` (`SS_NW_DR_AUTOVEC`, set DR `cr2lt`). B = QEMU oracle for the handler→DR signal.
> Keystone open test: does running `0x5000ED08` advance the boot to where registration self-sustains?
> Flip-last.
> ▶ **FIRST ACTION:** execute the M13 plan. Task 0 is RESOLVED (folded as plan Rev 2) — start with
> **Task B** (QEMU mac99 oracle, read-only) to pin the registered-handler→DR signal spec, which is the
> contract **Task C** (`SS_NW_DR_AUTOVEC` HLE at `0x50325f00`) gates against; **Task A** is just the
> thin EXT precondition (EXT already flows under `SS_NW_PIC=1`). Run it via
> `superpowers:subagent-driven-development`. Baseline: harness 353/353, paravirtual byte-identical.
> Process: `docs/MILESTONE-WORKFLOW.md`. Never push without being asked.
> Never global pkill — slot boots only via `SheepShaver/tools/ss-slot-boot.sh`.

---

## Current state (2026-06-13 end-of-session)

- **M8** — shipped, gated-off-green (`SS_NW_IRQ_CONSUME`).
- **M9** — stall fixed (ROM patch removed); 68k handler delivery blocked by deeper issues (see archived session log + M13-FINDINGS).
- **M10** — COMPLETE (2026-06-13). `SS_PROBE_68K=0x5000ed08` fires. Gate: `SS_M10_CGRP=1`.
  ⚠️ **Reframed by M13:** M10's probe-match was the CGRP-STUB→DR_WARM injection now FALSIFIED
  (→ 0xDEADBEEF crash); retained as a negative result, NOT a working delivery. See M13-FINDINGS.
- **M11a** — COMPLETE (2026-06-13). Frame-PC stability: static RE confirmed r24 is never clobbered by NK (see LEARNINGS 2026-06-13 M11a). 3/3 × 90s acceptance runs: probe match=1/5, no SIGSEGV. No code change.
- **M11** — COMPLETE (2026-06-13). Aperture at 0x81000000 (vm_mac_acquire_fixed 16MB), MMIO_APERTURE non-hull, SDL the_buffer → aperture, OF video node (640×480×32, "cofb"), T-F6 (13/13). [FB-DIRTY]=0 expected (boot exits 0.3s). Harness 353/353. Next: M12.
- **M12** — PARTIAL (2026-06-13). Wave0+Wave1 landed (`ddbd8d79`, `348544cd`); boot stable
  30s+ but `irq_fired=0`, `[FB-DIRTY]=0`. Pixel gate FAIL; frontier captured → M13.

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
