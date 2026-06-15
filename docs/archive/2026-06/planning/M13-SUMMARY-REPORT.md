# M13 "Pixel Attempt 2" — Status Report

**Date:** 2026-06-13 (session 2 — supersedes the session-1 draft of this file)
**Status:** Task A (host-side interrupt injection) FALSIFIED 4 independent ways; re-scope required (NK-native EXT→68k propagation, not injection). Recon COMPLETE; implemented+tested, baseline green.
**Branch:** `macos-arm64`
**Source of truth:** `docs/planning/superpowers/plans/2026-06-13-m13-atrap-bootstrap.md`
(Task-0 addenda + "Task A — implementation attempt + ARCHITECTURAL WALL"). This file is a summary.

> ## SCOPE OF THE BLOCKER (read this first)
> **This is not just an M13 problem. Reliable interrupt delivery to the 68k world has been the
> load-bearing blocker for the last four milestones — M9 through M13 — i.e. the entire pixel /
> QuickDraw push since ~2026-06-12.** M9 deferred the `0x5000ed08`-fires criterion to M10 (user-mode
> structural blocker); M10 achieved first delivery but on a path that crashes ~1/3; M11's framebuffer
> and M12's pixel gate are both stalled downstream of it (pixels need QuickDraw needs delivered VBL
> interrupts); M13 is still on it. So a real fix here is the **keystone** that unblocks the
> accumulated M11/M12/M13 pixel goal — not a one-milestone fix. (It does NOT reach back before M9:
> the earlier Path-A parcels port and Upgrade-Card dead-ends hit different walls, and none of this
> affects the shipped Mac OS 8.6 boot, which doesn't use this path.) **Carry this framing into the
> M13 close-out / milestone report when the fix lands.**
>
> **Diagnostic #1 (injection-OFF, 2026-06-13) CONFIRMS the blocker and corrects a metric:** with
> CGRP off the scheduler is healthy (dec=67393) but the 68k handler `0x5000ed08` NEVER fires and the
> 68k world spins in a polling/wait loop (HOT-PC at DR interpreter 0x50468ae4) → pre-System dead-end
> at ~15s. **`irq_fired` is MISLEADING** — it counts EXT consumed *at the NK*, not delivery to the
> 68k handler; the real signal is `0x5000ed08` running (PROBE68K match). The blocker is specifically
> NK→68k-handler delivery; the forward path is NK-native EXT→68k propagation, not hand-injection
> (falsified 4×). Re-scope = diagnostic #2 (why the NK consumes EXT but never routes it to 0x5000ed08).

> **Correction notice.** The session-1 draft of this file concluded the dec_expiries=1 stall
> "predates our changes," the STUB "never fires," and `r24=0` was a pre-existing bug. **All three
> were wrong** — they described a self-inflicted regression from a divergent rewrite (STUB moved
> to ROM 0x50429f00 + descriptor rewiring), not the baseline. Re-baselining (source reverted to
> HEAD) refuted them. The divergent rewrite is parked in `git stash@{0}`.

---

## Verified baseline (HEAD, harness 353/353)

With `SS_M10_CGRP=1 SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M11_FB=1`:
- Delivery to the 68k handler **works**: `[PROBE68K 0x5000ed08 match]` fires every boot.
- `dec_expiries=5` (PARK) typically; one boot ran free to 39100 when no delivery was attempted.
- Crash is **intermittent (~1/3 boots)**: SIGTRAP on DR-cache dead-fill (0xDEADBEEF @ guest 0x100259dc).

## Root cause (PROBE-confirmed)

DR_WARM (`0x5046e9d8`) is the 68k **opcode fetch-decode-dispatch loop**:
`lha r27,0(r24)` / `rlwimi r29,r27,3,13,28` (handler = r29 dispatch-base | opcode<<3) / `mtctr r29` / `bctr`.
The CGRP STUB enters DR_WARM after the NK exception path has **clobbered r29/r30 and the
D0-D7/A0-A6 gprs**; the baseline STUB restores only r24/r1, so the dispatch jumps to garbage
(dead-fill) → SIGTRAP. (Normal r29 at DR_WARM entry = 0x17ffeb20, stable across boots; the crash
address 0x100259dc is outside that range — i.e. r29 was wrong at the delivery entry.)

## The wall (why Task A is hard)

Safe 68k interrupt injection requires hitting the DR's **between-instruction boundary** (DR_WARM) —
the only point where the 68k register file is coherent in PPC gprs. **JIT block-chaining makes that
boundary unreachable from the interrupt poll** (`check_spcflags` runs only at *unchained* block
entries; DR_WARM is entered once then chained). Three direct-injection gate variants were tried and
all fail (any-DR-range PC → never fires / incoherent regs → SIGSEGV; exact DR_WARM → never polled).
Detail + reproduction in the plan's "Task A — implementation attempt" section. The attempt was
reverted (gated-off, non-working); baseline is intact.

## Forward options (architectural decision required)

- **(a) NK CGRP path + context restore** — deliver via the NK (safe by design) and set up the CGRP
  context save area so DR state is preserved/restored. Most correct, most expensive (the original
  deep-RE wall).
- **(b) DR_WARM ROM-patch IRQ check** — patch a poll/inject at 0x5046e9d8 so every
  between-instruction boundary checks pending EXT (mirrors real 68k instruction-boundary IRQ
  checking). Clean but invasive; must coexist with / suppress chaining for that block.
- **(c) DR state-save entry vectors (cheapest to evaluate)** — dispatch-table slots 2/3/5
  (`0x5046fb00/fc00/fd00`) are register-save prologues that snapshot the regfile into the DR context
  block at `*(0x2804)`. Vectoring to one of these instead of DR_WARM may make injection safe from any
  PC. Needs RE of those entries + the `*(0x2804)` block to confirm.

**Companion constraint (readiness gate):** don't deliver interrupts to the 68k world until the boot
reaches a stage where the world is idle/coherent AND can handle them (ties into Task B — the
0xAAF3/0xAAF4 OS-trap availability). This is a necessary design constraint for any option, but does
not by itself defeat the chaining/safe-point wall.

## Recon facts banked (in the plan)

- Q-A1 closed (root cause above). Q-A2: no interrupt entry-vector; 0x5046e8c0 is the opcode-dispatch
  trampoline; DR_WARM is the dispatch loop (Candidate A-1 as originally framed is void). Q-A6: A7=r1
  in DR context. Two stale plan recipes corrected (ROM-dump offsets out-of-bounds; DR region is
  RAM-resident).
</content>
