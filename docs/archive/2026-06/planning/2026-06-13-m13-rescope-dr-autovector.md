# M13 RE-SCOPED — drive the DR emulator's 68k autovector (verified diagnosis)

**Date:** 2026-06-13. Supersedes the "inject a 68k interrupt" framing of
`2026-06-13-m13-atrap-bootstrap.md` (Task A as injection is falsified 4×).
**Inputs:** the diag-2 fan-out (`2026-06-13-m13-diag2-nk-propagation.md`) — 3 threads, all complete.

## The complete verified diagnosis (T1+T2+T3)

The pixel/QuickDraw dead-end (~15s, M9→M13) is a **starved 68k idle loop**, and delivery is a
**three-stage** chain. We satisfy stage 1, and stages 2–3 never happen:

1. **NK EXT (PPC level) — WORKS.** The NK EXT body `0x50314880` is a PPC save→`rfi` return; by design
   it never vectors to 68k. With no CGRP handler registered it routes every EXT to the fallback
   `0x50325f00`, which records the interrupt in the NK's **internal pending-bitmask** and returns.
   `irq_fired` counts exactly these consume edges — NOT 68k delivery. [T1, T3: DISASM+PROBE✓]
   - Gate that diverts: `CGRP+0x20=1` (<2) and the CGRP dispatch table empty (`+0x38/+0x3c/+0x44`=0).
     The table is installed only once the boot reaches driver/interrupt registration — which it never
     does (chicken-and-egg). PR-bit hypothesis REFUTED (only EE is checked).
2. **NK→DR IPL handoff — MISSING.** Nothing translates the NK's pending-bitmask entry into a **pending
   68k IPL** that the DR emulator consults. This is the broken link.
3. **DR autovector (68k level) — never fires.** The DR, at a between-instruction boundary, would see a
   pending 68k IPL above the SR mask, build a real 68k exception frame (SR/PC/vector `$64`) on the 68k
   SSP, and vector through `[0x64]`. QEMU confirms this is THE working-delivery contract. [T3: QEMU✓]

**The 68k side is fully READY** (so it is NOT the blocker): autovectors installed and live —
`[0x64]=0x5000ed08`, `[0x68]=0x5000ed10`, `[0x6c]=0x5000ed18` → the ROM level dispatcher. NewWorld is
**paravirtual** — the dispatcher reads a *software* interrupt-source struct at `*(0x68ffefd0)` (NOT
VIA IFR/IER hardware). The dispatcher contains the deferred-task / Time-Manager / VBL pass the idle
loop is starving for. [T2: PROBE+DISASM✓]

**KEY ACTIONABLE NUANCE (T2):** the dispatcher's deferred/VBL/Time-Manager pass at `0x5000ee58` runs
**unconditionally after service** — so even a *pending-less* autovector entry to `0x5000ED08` still
ticks the time-driven queues and should advance the boot. We do not necessarily need a matching
pending bit in the software struct to make progress.

## The re-scoped target

**Drive the DR emulator's own between-instruction autovector — do NOT build the 68k frame ourselves
and do NOT route through the NK EXT/CGRP path.** Concretely: present a **pending 68k IPL (level 1)**
to the latch the DR's resume/dispatch loop checks, with the 68k SR mask permitting level 1, so the DR
itself synthesizes the genuine frame and vectors to `0x5000ED08`. This is the one mechanism that:
- produces the exact frame the handler expects (QEMU-confirmed contract), which host injection cannot
  fabricate from PPC context (falsified 4×), and
- enters at a coherent 68k boundary by construction (the DR's own check), sidestepping the
  recompiler-coherence wall and the cold/warm dispatch-table crash.

### Why this differs from every dead approach
- **Injection (4× falsified):** we tried to BE the frame-builder at a PPC poll. Here the DR builds it.
- **M10 CGRP forge:** forces `0x5000ED08` to fire via a STUB that enters the DR incoherently → crash.
  Here we set the IPL and let the DR's normal autovector run — no STUB, no DR_WARM.
- **NK EXT routing:** architecturally a PPC `rfi`; never the 68k vector. Out of scope.

## RE VERDICT (2026-06-13): the lever is REGISTER/CONTEXT state, NOT a memory latch — re-scope falsified

The make-or-break question (memory-backed vs pure-CR) is answered: **pure register/context state.**
- `0x50468b08` is the **movem register-load helper** (cr4-cr7 = register-select mask, `lwzu rN,4(r3)`,
  `blr` at 0x50468b54) — NOT an interrupt poll. [DISASM✓]
- `bgectr cr2` is therefore a **per-operation fault/exception gate**; `0x5046d114` is a UNIFIED
  slow-path (faults + traps + interrupts). `cr2lt` is a multi-purpose "take exception" condition,
  set INLINE for faults/traps (opcode/fault-driven). [DISASM✓; divert path reached for traps PROBE✓]
- For interrupts, `cr2lt` is set as **the DR's saved register/context state by the NK on delivery** —
  there is **no externally-pokable memory latch** that the DR re-derives each check.

**Implication:** the optimistic re-scope ("set a pending-IPL latch → DR autovectors") is NOT viable.
The DR autovector is real and live, but its interrupt trigger requires the NK to manipulate the DR's
context on delivery — which only the **registered CGRP handler does, and it is uninstalled** (T1:
CGRP+0x20=1, table empty; nothing registers a handler before the boot wedges). There is no shortcut
latch. **Productive direction = the NK code-group registration the boot performs** (understand why the
boot never reaches it / whether it is circular with the tick-starvation, or faithfully replicate the
registered handler's context-signal — itself deep NK RE). This corroborates T1 + the bake-off: let the
boot install the handler; do not inject or poke around it.

---

## (historical) RE PROGRESS (2026-06-13): the DR interrupt gate is LOCATED — but the lever may be register-state

Disassembled the DR interpreter dispatch (RAM-resident, probe-dumped):
- **Per-instruction dispatch core `0x50468b08`:** `rlwimi r29,r27,3,13,28` / `mtctr r29` / `lhau r27,2(r24)`
  (same shape as DR_WARM) — computes the next opcode handler into CTR.
- **The gate (repeated at every opcode-handler tail, e.g. 0x50468ae8):**
  `bgectr cr2` → execute next opcode (normal) ; else `b 0x5046d114` (DIVERT).
  **`cr2lt` is the between-instruction interrupt/exception gate.** [DISASM✓]
- **Divert target `0x5046d114`** = the DR slow-path exception/trap/interrupt dispatcher: saves faulting
  68k PC (`addi r4,r24,-2; stw r4,0x6c(r31)` → ECB+0x6c), manipulates cr2lt (`crmove cr2lt,cr5gt`;
  `crset cr2lt`), and decodes cause via opcode masks (0x3c / 0x46c0 / 0x6000 → A-line/F-line/trap).
  The interrupt case is one branch of this dispatcher (it then builds the 68k frame + vectors [0x64]). [DISASM✓]

**COMPLICATION (honest):** `cr2` is a PPC condition-register bit — recompiler-internal state, NOT a
memory flag we can poke. The lever that *feeds* cr2 (where the DR initially sets cr2lt from a
pending-IPL vs SR-mask compare) is one layer deeper; even once found, driving it externally means
manipulating the DR's cached CR in its saved context — the same recompiler-coherence class that sank
injection. So "set a memory latch and the DR autovectors" is only viable IF the DR's interrupt-pending
state is memory-backed (re-derived each check) rather than purely cached in CR. **This is the make-or-
break unknown for the whole re-scope and must be settled next.**

## Open RE before implementation (the next, smaller step)
0. **(NEW, decisive) Is the DR's interrupt-pending state memory-backed or pure CR?** Find where cr2lt is
   *initially set* — the DR's periodic interrupt-pending recompute. If it reads a memory location each
   check (a pending-IPL field / the software struct at *(0x68ffefd0)), that location IS the latch and
   the re-scope is viable. If cr2 is only ever set/cleared in-register (e.g. by the NK writing the DR's
   saved CR on a context switch), external driving is as hard as injection → the re-scope needs
   rethinking (likely back to "let the boot install the real CGRP handler").

1. **Locate the DR's between-instruction interrupt check + the 68k-IPL latch it reads.** T3 points at
   the DR resume/dispatch family (`0x5046e1a4`/`0x5046cdd8`); find where it tests a pending IPL vs the
   SR mask. This is the address we must drive.
2. **Determine how to set that latch from the host/PIC side** (the NK pending-bitmask at the
   `0x50325f00` path is the source; find the field the DR consumes, or the PPC-side flag).
3. **Confirm the SR mask permits level 1** at the wedge (T2: the idle loop runs with interrupts
   enabled — likely yes, but verify the DR's cached SR).

## Acceptance gate (falsifiable, staged)
- **Stage A:** `[PROBE68K 0x5000ed08 match]` fires ≥1 with NO SIGSEGV/SIGTRAP (the DR autovectored
  cleanly — the thing that has never happened without a crash).
- **Stage B:** boot progresses past the ~15s dead-end ([ALARM]/[STALL] gone; new blocks compile after
  15s; `dec_expiries` keeps climbing AND the handler fires repeatedly).
- **Stage C:** `[FB-DIRTY] non_zero_pixels>0` (the original M13 goal).
- Inner gates throughout: `make build-ss`, harness 353/353, baseline byte-identical (gated).

## Standing constraints
All work gated (default OFF); slot-protocol boots only; never pkill; re-verify addresses (RAM-resident
DR region, 9.0.1 NW diagnostic config); RE before code; report BLOCKED with evidence rather than force.
</content>
