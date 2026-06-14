# Project Handoff — resume entry point

> **Status: PAUSED 2026-06-14** · M14 RECON: timer delivery works at VIA layer; **M13 stage-2-MISSING confirmed correct** (ed0a 8/8 retraction was capped-probe artifact — uncapped: all DEC, Cuda EXT adds zero). CGRP handler not registered → NK fallback doesn't forward EXT to DR → hnfo+0x28 empty. Chicken-and-egg: boot stalls at Cuda init before CGRP registration ·
> Next = host-side HLE bypass (set cr2lt + hnfo from host on Cuda EXT), OR poll-driven Cuda, OR CGRP forge ·
> Resume: read this doc, then `docs/planning/M14-FINDINGS-cuda-delivery.md` §4/§4a, then `docs/AGENT-CONTEXT.md`.
> For session logs: `docs/archive/2026-06/LEARNINGS-2026-06.md` + `docs/archive/2026-06/HANDOFF-SESSIONS-M9-M12.md`.

## HEADLINE — the M13 correction + M14 reconciliation

> **DEC delivery to 68k WORKS. EXT (Cuda) delivery to 68k does NOT.**
>
> The M9→M13 thesis — "the 68k handler never runs" — was a probe artifact (ed08 exact-match
> blind to ed0a). But the M13 retraction overcorrected: ed0a "8/8" was a **capped-probe
> artifact** (default cap=8). M14 uncapped probe (`:64`) shows 64/64 in baseline = ALL DEC
> autovector. Smoke H (timer-based Cuda delivery) adds ZERO ed0a entries.
>
> **M13 stage-2-MISSING was correct:** the CGRP handler is not registered, so the NK EXT
> fallback at 0x50325f00 consumes the PIC source and returns without forwarding to the DR.
> Cuda EXT reaches the NK but never the 68k world. Hnfo+0x28 (pending bits) is zero — the
> 68k handler's interrupt-source struct is empty. Both the M13 reverted mechanisms
> (`SS_NW_DR_AUTOVEC`, `SS_M10_CGRP`) targeted the right problem with wrong implementations.
>
> **Standing corrections:**
> - The ed08→ed0a probe-granularity fix IS correct (always probe ed0a, never ed08)
> - DEC delivery to 68k IS healthy (published DEC handler, not CGRP-dependent)
> - The "DO NOT re-chase" blanket prohibition was too broad — the stage-2 gap is real, but
>   the specific failed approaches (host-side register pokes, forged CGRP tables) should not
>   be repeated. Any new approach must follow M13 B.1–B.4 contract analysis.
> - Evidence: M14-FINDINGS §4a (Smoke H + uncapped probe + watchpoints + direct Hnfo probe)

## Resume prompt (M14 — Cuda delivery + NK interrupt routing)

> Read `docs/HANDOFF.md` (this HEADLINE), then `docs/planning/M14-FINDINGS-cuda-delivery.md`
> (full root cause + 9 smoke tests in §4, Smoke H timer result in §4a), then
> `docs/AGENT-CONTEXT.md` (frontier + constants).
>
> **M14 recon + 9 smoke tests + watchpoint + uncapped-probe validation.** Timer-delayed
> Cuda SR delivery is correct at the VIA layer. The M13 **retraction was itself an
> artifact** — ed0a "8/8" was a capped-probe result (default cap=8); uncapped (`:64`)
> shows 64/64 baseline = all DEC autovector, Smoke H adds zero. The M13 stage-2-MISSING
> diagnosis was correct:
>
> 1. **CGRP handler not registered** → NK EXT routes to fallback at 0x50325f00 →
>    fallback reads PIC, clears source, returns — never sets cr2lt for the DR.
>    Cuda EXT reaches NK but NOT the 68k world.
>
> 2. **Hnfo pending bits (hnfo+0x28) empty** — watchpoints: all zeros for entire boot.
>    Direct probe at EXT entry: hnfo+0x14 (source table) = NIL, hnfo+0x28 = 0.
>    Even if EXT reached the 68k handler, it would find nothing to service.
>
> 3. **Chicken-and-egg**: boot stalls at Cuda init (packets=0) BEFORE reaching the
>    Interrupt Manager init that would register the CGRP handler. DEC reaches 68k
>    (published DEC handler, not CGRP-dependent). Cuda init may be poll-driven but
>    the guest polls IER (65,539×) instead of IFR (2×).
>
> **Next (choose one):**
> (a) **Host-side HLE:** on Cuda EXT delivery, set cr2lt + populate hnfo+0x28 from the
>     host. M13 B.1–B.4 has the exact contract. Replaces the reverted SS_NW_DR_AUTOVEC
>     with the correct mechanism.
> (b) **Poll-driven Cuda:** make CudaSettle deliver on IFR reads (it already does) AND
>     investigate why the guest reads IER 65,539× instead of IFR.
> (c) **CGRP forge:** populate the CGRP dispatch table directly. M10 attempted this
>     (crashed). Needs correct struct layout.
> (d) Fix §5 bug (SS_NW_TRAMPOLINE=0 existence check, machine_profile.cpp:80).
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
- **M14** — RECON IN PROGRESS (2026-06-14). 9 smoke tests + watchpoint + uncapped probe.
  Timer delivery correct at VIA layer. **M13 stage-2-MISSING confirmed correct** — the
  M13 retraction was a capped-probe artifact (ed0a "8/8" = DEC only; uncapped 64/64).
  CGRP handler not registered → NK fallback consumes EXT, never forwards to DR → hnfo
  pending bits empty. Chicken-and-egg: boot stalls before CGRP registration. Three fix
  paths: host-side HLE (M13 B.1–B.4 contract), poll-driven Cuda, or CGRP forge.
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
