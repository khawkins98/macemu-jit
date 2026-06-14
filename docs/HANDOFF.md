# Project Handoff — resume entry point

> **Status: PAUSED 2026-06-14** · M13 COMPLETE (artifact retraction + dead mechanisms reverted) ·
> Next = M14 (model-rejection gate) · Resume: read this doc, then `docs/AGENT-CONTEXT.md`.
> For session logs: `docs/archive/2026-06/LEARNINGS-2026-06.md` + `docs/archive/2026-06/HANDOFF-SESSIONS-M9-M12.md`.

## HEADLINE — the M13 correction (read before anything else)

> **Interrupt delivery WORKS.** The M9→M13 load-bearing thesis — "the 68k interrupt handler
> `0x5000ED08` never runs / interrupts are never delivered to the 68k world" — was a
> **probe-granularity artifact**: `SS_PROBE_68K=0x5000ed08` is exact-match, but the DR's first
> `lhau` advances r24 `ed08→ed0a` before the dispatch hook samples, so the probe was blind to
> handler entry. Re-targeting to `ed0a` matched **8/8 in plain baseline** (HLE OFF), a genuine
> DR-built `$64` level-1 autovector (`[a7+6]=0x0064`, saved `SR=0x2000` = IPL 0 → legitimate, not
> forced; saved PC `0x50034cae`); handler region entered ~207×, full level-dispatch + VBL pass,
> scheduler healthy. **Do NOT re-chase interrupt delivery / CGRP registration / 68k injection** —
> all five milestones' delivery work targeted a non-problem. Both delivery mechanisms
> (`SS_NW_DR_AUTOVEC` Task-C HLE, `SS_M10_CGRP` forged table) were **reverted in this M13
> close-out**. Evidence: `docs/planning/M13-FINDINGS-interrupt-delivery.md` §C-pin.7 / §C-pin.8.

## Resume prompt (M14 — the new frontier)

> Read `docs/HANDOFF.md` (this HEADLINE), then `docs/AGENT-CONTEXT.md` (frontier + constants), then
> `docs/planning/M13-FINDINGS-interrupt-delivery.md` §C-pin.7/8 (why delivery is NOT the blocker).
> **The wall moved downstream.** Native delivery, the 68k handler, and the scheduler are all healthy;
> the boot still parks ~15 s in at the `[ALARM]` **model-rejection / pre-System gate** — a
> Gestalt/machine-ID or System-file boot gate that rejects this machine/ROM combo (saved 68k PC at
> the autovector seam was `0x50034cae`; the `jDR` 68k-block counter FREEZES ~10 s in while the NK
> spins on). This is a **ROM/OS-version** issue, not an interrupt one.
>
> **M14 — characterize the model-rejection gate first, hard.** This wall is in already-RE'd,
> gate-bypass-TOOLED territory — MORE tractable than the opaque NK internals, not less:
> `docs/planning/sheepshaver-research/SYSTEM-BOOT-GATES.md` (Mac OS 9.x `boot` id=3 anatomy, DSAT
> resource format, error catalog, gate-bypass details) and the archived 4-byte System-file
> gate-bypass work in `docs/archive/2026-06/planning/UPGRADE-CARD-PATH.md`.
> ▶ **FIRST ACTION:** characterize the gate (pin the last 68k subroutine before `jDR` freezes; identify
> the device/gate it polls — capture the r24 instruction-boundary trail in the final 1–2 s). Use
> **ONE early cross-version boot as a cheap version-locked check** — a single boot of a different rev
> (the staged 9.2.1 / 9.0.4 / 1.1 ROM at `/Users/Shared/macemu/`, or the QEMU 9.2.1 oracle) to see if
> it hits the *same* `[ALARM]` or a *different* one. **The full ROM/OS-version route-around sweep is
> PROMOTED only if that check shows the gate is version-locked.** Do NOT jump to the sweep before
> characterizing — that repeats the act-before-understand mistake.
> Process: `docs/MILESTONE-WORKFLOW.md`. Run recon via `superpowers:subagent-driven-development`.
> Never push without being asked. Never global pkill — slot boots only via
> `SheepShaver/tools/ss-slot-boot.sh`.

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
  `SS_M10_CGRP` mechanisms reverted. New frontier = the model-rejection / pre-System gate (M14).
  See `docs/planning/M13-FINDINGS-interrupt-delivery.md` (retraction banner + §C-pin.8).
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
