# Project Handoff — resume entry point

> **Status: PIVOTING 2026-06-14** · M14 COMPLETE: NewWorld 9.x Cuda bootstrap is a
> **precisely-characterized hard wall** (all paths collapse to crash-prone forge class).
> **DEC does NOT signal the DR** — ed0a entries come from HandleInterrupt MODE_EMUL_OP,
> not the NK DEC handler. KDP+0x674=0, hnfo+0x14=NIL, hnfo+0x28=0, all because IM init
> runs downstream of Cuda init. **PARKED** with precise resume experiment in §7.
> Next = **COMPATIBILITY-PAYOFF**: make the already-booting 8.6–9.0.4 genuinely usable.
> Resume NewWorld 9.x: `docs/planning/M14-FINDINGS-cuda-delivery.md` §7 (forge smoke test).
> For session logs: `docs/archive/2026-06/LEARNINGS-2026-06.md` + `docs/archive/2026-06/HANDOFF-SESSIONS-M9-M12.md`.

## HEADLINE — the M13 correction + M14 reconciliation

> **DEC does NOT signal the DR. EXT (Cuda) delivery to 68k does NOT.**
> **ed0a entries are from HandleInterrupt MODE_EMUL_OP, not the NK DEC handler.**
>
> The ed0a entries were **misattributed to DEC autovector.** Full DEC handler RE (§7)
> proves: the NK DEC handler at 0x50313200 services timers, restores CR fully
> (`mtcrf 0xff, r13`), and returns via rfi — it NEVER dispatches to the DR. The 64/64
> ed0a entries come from `HandleInterrupt` `MODE_EMUL_OP` `Execute68k` (the host-side
> VBL path that runs during EMUL_OP callbacks). In `MODE_68K` (where Cuda init runs),
> HandleInterrupt only bumps Ticks — no CR injection, no autovector.
>
> **The chicken-and-egg, precisely located:**
> - `KDP+0x674` (CR mask) = **0** — never initialized (probed live)
> - `hnfo+0x14` (source table) = **NIL** — never populated
> - `hnfo+0x28` (pending bits) = **0** — never written
> - All three are populated by the Interrupt Manager init, which runs DOWNSTREAM of
>   Cuda init. Every viable fix path collapses to the M10-forge class (host-seeding
>   NK data structures). See M14-FINDINGS §7 for the precise resume experiment.
>
> **Standing corrections:**
> - The ed08→ed0a probe-granularity fix IS correct (always probe ed0a, never ed08)
> - DEC does NOT deliver to 68k via the NK handler — ed0a entries are MODE_EMUL_OP only
> - All paths (HLE, CR injection, CGRP forge, Execute68k) collapse to forge-class
> - Evidence: M14-FINDINGS §4a (smoke tests) + §7 (DEC RE + collapse analysis)

## Resume prompt (COMPATIBILITY-PAYOFF — making 8.6–9.0.4 usable)

> Read `docs/HANDOFF.md` (this HEADLINE), then `docs/AGENT-CONTEXT.md`.
>
> **NewWorld 9.x is PARKED.** The chicken-and-egg is precisely characterized (M14 §7)
> and the resume experiment is written down verbatim. Don't re-derive — read §7 if
> picking it up.
>
> **Current focus: COMPATIBILITY-PAYOFF** — make the already-booting Mac OS 8.6–9.0.4
> genuinely usable. The project boots to Finder with full native JIT on arm64. The
> highest-value work is: CopyBits HLE, idle-skip, perf optimization, app compatibility,
> and the Silicon Sheep launcher (Track C).
>
> Process: `docs/MILESTONE-WORKFLOW.md`. Never push without being asked. Never global pkill —
> slot boots only via `SheepShaver/tools/ss-slot-boot.sh`.

---

## Current state (2026-06-14 end-of-session)

- **M8** — shipped, gated-off-green (`SS_NW_IRQ_CONSUME`).
- **M9** — stall fixed (ROM patch removed). ⚠️ Its "68k handler delivery blocked" framing is the
  RETRACTED artifact (see HEADLINE) — delivery was working; the wall was downstream.
- **M10** — COMPLETE (2026-06-13). Gate `SS_M10_CGRP` — **reverted in the M13 close-out (2026-06-14)**:
  the forged CGRP table targeted the non-problem (native delivery already works) and crashed
  (0xDEADBEEF). Retained only as a negative-result record in M13-FINDINGS / LEARNINGS.
- **M13** — COMPLETE (2026-06-14). Produced the **artifact retraction**: native interrupt delivery
  confirmed working (ed0a 8/8 baseline, genuine vector-$64 frame); the dead `SS_NW_DR_AUTOVEC` and
  `SS_M10_CGRP` mechanisms reverted. See `docs/planning/M13-FINDINGS-interrupt-delivery.md`.
- **M14** — COMPLETE, PARKED (2026-06-14). 9 smoke tests + watchpoint + uncapped probe +
  DEC handler RE + ed0a misattribution discovery. Timer delivery correct at VIA layer.
  **DEC does NOT signal the DR** — ed0a entries come from HandleInterrupt MODE_EMUL_OP
  Execute68k, not the NK DEC handler (which restores CR fully and returns without
  dispatching to the DR). KDP+0x674=0, hnfo+0x14=NIL, hnfo+0x28=0 — all because IM init
  runs downstream of Cuda init. All viable paths collapse to the M10-forge class (seed
  uninitialized NK routing infra from host). Parked with precise resume experiment in §7.
  See `docs/planning/M14-FINDINGS-cuda-delivery.md`.
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
