# Project Handoff — resume entry point

> **Status: PAUSED 2026-06-14** · M14 RECON: timer delivery works at VIA layer; TWO remaining gaps: (1) NK fallback consumes EXT but never forwards to DR/68k (cr2lt not set), (2) interrupt-source struct at *(0x68ffefd0) is all zeros (never populated) ·
> Next = QEMU oracle comparison for struct population + NK fallback forwarding behavior ·
> Resume: read this doc, then `docs/planning/M14-FINDINGS-cuda-delivery.md` §4/§4a (Smoke H), then `docs/AGENT-CONTEXT.md`.
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

## Resume prompt (M14 — Cuda delivery + NK interrupt routing)

> Read `docs/HANDOFF.md` (this HEADLINE), then `docs/planning/M14-FINDINGS-cuda-delivery.md`
> (full root cause + 9 smoke tests in §4, Smoke H timer result in §4a), then
> `docs/AGENT-CONTEXT.md` (frontier + constants).
>
> **M14 recon + 9 smoke tests + watchpoint validation.** Timer-delayed Cuda SR delivery
> is correct at the VIA layer (Smoke H: IFR_SR latched, irq_out 0→1, PIC edge). Two
> remaining gaps identified by uncapped probe + watchpoints:
>
> 1. **NK fallback consumes EXT but doesn't forward to DR.** The fallback handler at
>    `0x50325f00` reads the PIC source, clears the pending bit, and returns to PPC —
>    it never sets `cr2lt` to trigger the DR's exception path. Cuda EXT reaches the NK
>    but NOT the 68k world. (Uncapped `SS_PROBE_68K=0x5000ed0a:64` shows 64/64 baseline
>    = all DEC autovector; Smoke H adds zero ed0a entries.)
>
> 2. **Interrupt-source struct never populated.** `*(0x68ffefd0)` → `0x68ff4f00`.
>    Watchpoint on `+0x00`, `+0x04`, `+0x28`: all zeros for entire 30s boot (1B+ obs).
>    Even if EXT reached ed0a, the 68k handler would find nothing to service.
>
> This is **NOT** the retracted M13 "handler table not installed" thesis — the NK
> fallback handler RUNS and correctly reads the PIC. The gap is behavioral (doesn't
> forward to DR) and data-layer (struct unpopulated). Distinct from registration.
>
> **Next:**
> (a) QEMU oracle: what does `*(0x68ffefd0)` point to and what populates it? Does
>     the NK fallback forward to the DR in QEMU, or does QEMU use a different path?
> (b) Does the NK's non-fallback (registered) handler path forward EXT to DR? If so,
>     what init step installs the registered handler?
> (c) Fix §5 bug (SS_NW_TRAMPOLINE=0 existence check, machine_profile.cpp:80).
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
- **M14** — RECON IN PROGRESS (2026-06-14). 9 smoke tests + watchpoint validation.
  Timer-delayed Cuda SR delivery correct at VIA layer. Two gaps: (1) NK fallback at
  0x50325f00 consumes EXT but never forwards to DR (cr2lt not set — Cuda EXT never
  reaches 68k), (2) interrupt-source struct at `*(0x68ffefd0)` = `0x68ff4f00` is all
  zeros (never populated by anyone). Not the retracted M13 thesis (handler runs, reads
  PIC correctly — gap is behavioral/data-layer). Next: QEMU oracle for struct population
  and NK forwarding. See `docs/planning/M14-FINDINGS-cuda-delivery.md`.
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
