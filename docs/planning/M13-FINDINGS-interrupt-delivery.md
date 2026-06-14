# M13 Findings — 68k interrupt delivery (NewWorld): the verified diagnosis

**Date:** 2026-06-13. **Status:** authoritative findings for the M9→M13 interrupt-delivery blocker.
Durable distillation of a multi-agent investigation; the process trail is archived (see "Provenance").

---

## TL;DR

Getting the NewWorld 9.0.1 diagnostic boot past its ~15s pre-System dead-end (and on to pixels)
requires the **68k interrupt handler `0x5000ED08` to actually run**. It never does. This was the
load-bearing blocker for **M9 through M13** (the whole pixel/QuickDraw push since ~2026-06-12), not a
one-milestone issue. Five independent investigations this session converged on a single conclusion:

> **No coherent host-side injection point for a 68k interrupt exists (5 independent confirmations).** Correct delivery is
> the NanoKernel's own job, and it requires a **registered CGRP interrupt handler that is never
> installed** because the boot wedges before reaching driver/interrupt registration. There is no
> shortcut latch or injection point. The productive direction is the NK code-group registration the
> boot performs — not injecting, patching, or poking around it.

This does **not** touch the shipped Mac OS 8.6 boot (different ROM, doesn't use this path).

---

## The verified diagnosis — delivery is a 3-stage chain; stage 1 works, stages 2–3 never happen

1. **NK EXT (PPC level) — WORKS.** The NK EXT body `0x50314880` is a PPC `save → rfi` return that by
   design never vectors to 68k. With no CGRP handler registered it routes every EXT to the fallback
   `0x50325f00`, which only records the interrupt in the NK's internal pending-bitmask and returns.
   - Diverting gate: `CGRP+0x20 = 1` (a function pointer; the `cmpwi r9,2; blt` guard treats <2 as
     "no handler" — so 1 = uninstalled) and the CGRP dispatch table empty
     (`CGRP+0x38/0x3c/0x44 = 0`). CGRP base `0x68ffc1c0`. [DISASM+PROBE✓]
   - **`irq_fired` is a MISLEADING metric** — it counts EXT consumed *at the NK*
     (g_exc_consume_stats.fired), NOT delivery to the 68k handler. The real delivery signal is
     `0x5000ED08` running (`PROBE68K 0x5000ed08 match`).
2. **NK→DR handoff — MISSING.** Nothing translates the NK pending-bitmask into the DR's interrupt
   state. The translation is the **registered CGRP handler's** job; it is not installed.
3. **DR autovector (68k level) — never fires.** The DR, at a between-instruction boundary, would set
   its "take exception" condition (`cr2lt`), divert to the unified slow-path `0x5046d114`, build a real
   68k exception frame (vector `$64`, saved SR/IPL) and vector through `[0x64]` → `0x5000ED08`. QEMU
   (9.2.1 oracle) confirms this is THE working-delivery contract. The mechanism is **live** (the
   slow-path is reached for traps/faults) — only the interrupt trigger never occurs. [DISASM+QEMU✓]

### Confirmed supporting facts
- **The 68k side is fully READY** (not the blocker): autovectors live `[0x64]=0x5000ed08`,
  `[0x68]=…ed10`, `[0x6c]=…ed18` → the ROM level dispatcher. NewWorld is **paravirtual** — the handler
  reads a *software* interrupt-source struct at `*(0x68ffefd0)`, **NOT VIA IFR/IER hardware** (this
  corrects the M9 VIA-IFR framing). [PROBE+DISASM✓]
- The handler's deferred-task / Time-Manager / VBL pass at `0x5000ee58` runs **unconditionally after
  service** — so even a *pending-less* autovector entry would tick the starved queues. The 68k world
  is spinning the SystemTask/idle loop (HOT-PC at DR interpreter `0x50468ae4`, r24 cycling ROM 68k
  PCs) starving for those ticks. [PROBE✓]
- Injection-OFF baseline is otherwise healthy: scheduler runs (dec_expiries=67393); CGRP-ON is what
  destabilizes it. [PROBE✓]
- The DR is a **recompiler**: steady-state 68k runs in a code cache at `0x17fa0000+`; `0x5046e9d8`
  (DR_WARM) is a cold trampoline (entered ~once), and `0x50468b08` is the movem helper. The 68k
  register file is coherent only at the DR's between-instruction boundary. [DISASM+PROBE✓]

---

## What was falsified (do not retry — code-level warnings are in `rom_patches.cpp` / `sheepshaver_glue.cpp`)

| Approach | Why it's dead |
|----------|---------------|
| CGRP STUB → DR_WARM (M10/M12) | Enters the DR with a clobbered/cold dispatch state → `0xDEADBEEF` SIGTRAP. |
| Host injection, any DR-range PC | EXT is taken in PPC mode; no coherent 68k context exists at that instant. |
| Resume-prologue (`0x5046e1a4`) delivery | `ECB+0x740` holds the DR's *PPC-resume* ctx, not a 68k regfile; injection destabilizes the scheduler. |
| DR state-save vectors (slots 2/3/5) | Volatile-only save (r7-r13), not the non-volatile 68k regfile. |
| DR_WARM ROM-patch IRQ check | DR_WARM is a cold trampoline, not the per-instruction loop; no fixed patch point. |
| "Set a pending-IPL memory latch" re-scope | The DR's interrupt trigger (`cr2lt`) is **register/context state** set by the NK on delivery — no externally-pokable memory latch. |

**Net:** every shortcut substitutes for the NK's registered-handler context-signal, which host code
cannot fabricate coherently. Five independent confirmations.

---

## Forward direction (the re-scoped M13)

Make the NK's **registered CGRP interrupt handler** get installed and signal the DR the way a healthy
boot does. Open questions for the next milestone pass:
1. **What installs the CGRP handler** (sets `CGRP+0x20≥2`, populates `+0x38/+0x3c/+0x44`), and at what
   boot stage? Is it reachable before the tick-starvation wedge, or is it circular (needs ticks to
   reach the code that registers the tick handler)?
2. If circular: what is the **minimal external bootstrap** that lets the boot reach registration once
   (e.g. a one-shot correct delivery), after which natural delivery self-sustains?
3. Faithfully characterize the registered handler's **DR context-signal** (how it sets the DR's
   interrupt `cr2lt`/context on delivery) — the only coherent way to drive the autovector.

Acceptance gate (unchanged, staged): Stage A `PROBE68K 0x5000ed08 match ≥1` with no crash → Stage B
boot passes the ~15s dead-end (blocks compile after 15s, dec keeps climbing, handler fires repeatedly)
→ Stage C `[FB-DIRTY] non_zero_pixels>0`.

---

## Provenance (archived process trail — `docs/archive/2026-06/planning/`)

- `2026-06-13-m13-atrap-bootstrap.md` — original M13 plan (Task A as injection) + Task-0 recon addenda.
- `2026-06-13-m13-taskA-bakeoff.md` — 3-approach bake-off + the full injection map + resume-prologue impl.
- `2026-06-13-m13-diag2-nk-propagation.md` — diag-2 fan-out brief (NK routing / 68k readiness / oracle).
- `2026-06-13-m13-rescope-dr-autovector.md` — the autovector re-scope + the RE verdict.
- `M13-SUMMARY-REPORT.md` — running status report (supersedes a wrong session-1 draft).
- Durable lessons: `LEARNINGS.md` (top entry, 2026-06-13 M13). Code warnings at the STUB sites.
</content>
