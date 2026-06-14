# Project Handoff — resume entry point

> **Status: PAUSED 2026-06-14** · M14 RECON COMPLETE (8 smoke tests; all non-timer approaches eliminated — need timer-delayed Cuda SR delivery à la QEMU `cuda_delay_set_sr_int`) ·
> Next = run timer smoke test (§4a); schedule IFR_SR ~20µs after sr_int_pending via EventScheduler; implement only if packets>0 ·
> Resume: read this doc, then `docs/planning/M14-FINDINGS-cuda-delivery.md` §4/§4a, then `docs/AGENT-CONTEXT.md`.
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

## Resume prompt (M14 — timer-delayed Cuda SR delivery)

> Read `docs/HANDOFF.md` (this HEADLINE), then `docs/planning/M14-FINDINGS-cuda-delivery.md`
> (full root cause + 8 smoke tests in §4, revised fix direction in §4a), then
> `docs/AGENT-CONTEXT.md` (frontier + constants).
>
> **M14 recon + 8 smoke tests COMPLETE.** The "model-rejection gate" was WRONG — it's a
> **Cuda device model bug**: the consume-once `sr_int_pending` + deferred `CudaSettle`
> model doesn't match real hardware. Eight smoke tests (§4) systematically eliminated
> every non-timer approach:
> - Eager ORB-edge delivery unblocked NK→68k (jDR 600×↑, IFR 2→15K), proving lazy delivery
>   was blocking — but CV-10 timing violation breaks the protocol (packets=0).
> - SR-read-triggered delivery sets `ifr_latched=0x04` correctly, but `ier=0x00` at that
>   point — the guest enables IER.2 (SR) AFTER clearing IFR.2 via SR read (D+IER trace).
> - Level-triggered (`treq_asserted`): treq is a PULSE (goes 1→0 within ORB sequence),
>   not a sustained level — same result as eager (Smoke E).
> - Non-consuming `sr_int_pending`: SR read consumes before IER enables (Smoke F).
> - Persistent through SR reads: regresses to baseline CV-10 park (Smoke G).
>
> **Next: run the timer smoke test** (§4a). Use EventScheduler to schedule IFR_SR delivery
> ~16 VIA ticks (~20µs) after `sr_int_pending` is set — matching QEMU's
> `cuda_delay_set_sr_int` model. One boot. If `packets > 0`, the timer model is confirmed
> — then build it properly (env-gated). If not, re-recon before building.
>
> Also fix the §5 bug (SS_NW_TRAMPOLINE=0 existence check, machine_profile.cpp:80).
> VIA `irq_fn` wiring: defer until the timer test shows whether IFR polling suffices.
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
- **M14** — RECON COMPLETE (2026-06-14). The "model-rejection gate" was WRONG — it's a
  **Cuda device model bug**: consume-once `sr_int_pending` doesn't match real hardware's
  timer-delayed interrupt. 8 smoke tests (§4) eliminated all non-timer approaches: eager
  delivery unblocks NK→68k (jDR 600×↑, IFR 2→15K) but CV-10 timing violation; SR-read and
  IER-write triggered delivery hits IER ordering (guest enables IER.2 AFTER clearing IFR.2);
  level-triggered treq fails (treq is a pulse not a level); persistent sr_int_pending regresses
  to CV-10 park. Fix = timer-delayed delivery (~20µs after ORB edge, à la QEMU
  `cuda_delay_set_sr_int`) via EventScheduler. **Gated on timer smoke test (§4a) before
  building.** See `docs/planning/M14-FINDINGS-cuda-delivery.md`.
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
