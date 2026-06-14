# Project Handoff — resume entry point

> **Status: PAUSED 2026-06-14** · M14 RECON COMPLETE (9 smoke tests; timer delivery WORKS through full VIA→PIC→NK→68k pipeline — but packets=0 because 68k Cuda dispatch path doesn't find the Cuda pending bit in the interrupt-source struct) ·
> Next = characterize the 68k-side Cuda interrupt dispatch at 0x68ffefd0 (+0x28) ·
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
> **M14 recon + 9 smoke tests COMPLETE.** Timer-delayed Cuda SR delivery is validated
> through the full pipeline (Smoke H: IFR_SR latched after IER.SR enables, irq_out 0→1,
> PIC edge, NK EXT delivered, **68k autovector handler at ed0a fires 8/8**). But
> `packets=0` — the remaining blocker is the **68k-side Cuda dispatch path**:
> - The 68k handler runs (ed0a fires 8/8 in BOTH baseline+PIC and Smoke-H+PIC).
> - The interrupt reaches the 68k world. The problem is DOWNSTREAM: the Cuda-specific
>   pending bit in the software interrupt-source struct at `0x68ffefd0` (+0x28) is
>   apparently not set, so the 68k dispatcher doesn't know to service the Cuda/VIA
>   interrupt and the byte exchange never starts.
> - **DO NOT** re-investigate NK routing / registered-handler table / CGRP — that is
>   the twice-retracted M13 thesis (probe artifact). The ed0a probe proves delivery works.
>
> **Next:** characterize the 68k-side Cuda interrupt dispatch:
> (a) Probe the interrupt-source struct at `0x68ffefd0` (+0x28) — is the Cuda pending
>     bit ever written? By whom?
> (b) Trace what the 68k autovector handler at `ed0a` does after entry — which memory
>     it reads to decide there's nothing to service.
> (c) Compare with QEMU mac99 for the dispatch mechanism.
>
> Also fix the §5 bug (SS_NW_TRAMPOLINE=0 existence check, machine_profile.cpp:80).
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
- **M14** — RECON COMPLETE (2026-06-14). 9 smoke tests. Timer-delayed Cuda SR delivery
  validated through full VIA→PIC→NK→68k pipeline (Smoke H: IFR_SR latched, irq_out 0→1,
  PIC edge, NK EXT delivered, **68k handler ed0a fires 8/8**). But `packets=0` — remaining
  blocker is the **68k-side Cuda dispatch path**: the interrupt-source struct at 0x68ffefd0
  (+0x28) doesn't have the Cuda pending bit set, so the 68k dispatcher doesn't service
  the Cuda interrupt. DO NOT re-investigate NK routing (twice-retracted M13 thesis).
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
