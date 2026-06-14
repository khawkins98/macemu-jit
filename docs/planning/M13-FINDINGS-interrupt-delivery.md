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

## Step-0 recon (M13 redraft, 2026-06-14) — wall pinned + eager-delivery FALSIFIED

- **Q-0a — the real wall is the tick-starved idle spin `0x50468ae4`** (≥10000 probe visits), NOT an
  MMU fault at `0x50326050` (single fly-by, visit=1; zero DSI/ISI/fault/slow-path signatures). The
  "MMU wall" framing from the M13 inventory is refuted. [PROBE✓]
- **Q-0b — eager interrupt delivery does NOT advance the boot (lever FALSIFIED).** A gated 60Hz forced
  VIA assert (`SS_M13_EAGER`, +33 lines, harness 353/353) drove EXT 7× harder (host-irq edges 1→2481,
  `irq_fired` 82→592) — but every one landed in the fallback `0x50325f00` and returned; **`0x5000ED08`
  never ran, CGRP registration `0x5031b290` never reached, the kcall funnel `0x5031aca0` fired once,
  the wall/`[ALARM]`/zero-pixels were unchanged.** Critically, the fallback **already fires ≥10000× in
  plain baseline** — EXT is NOT starved at the NK. [PROBE✓]
- **Consequence (BINDING for the redraft):** the gap is purely the **missing NK→DR handoff**, not the
  EXT source. **Task A (eager delivery) is demoted** to a precondition (just ensure EXT reaches the NK
  via the PIC leg — already true under `SS_NW_PIC=1`); the eager-VIA-timer lever is dead. **Task C (HLE
  the handler→DR signal at `0x50325f00`) is the sole lever.** And eager EXT does **not** break the
  registration circularity (registration still 0 hits under 2481 EXTs) → the open keystone test is
  whether Task C's delivery to `0x5000ED08` advances the boot to where registration self-sustains; if
  not, the circularity is deeper than any delivery mechanism and the ROM/OS-version route-around rises.
  (The `SS_M13_EAGER` experiment harness is preserved on branch `worktree-agent-a930ae477fe328489`,
  gated default-OFF, as a negative-result record — not for merge.)

---

## Task B — handler→DR signal spec (QEMU oracle + static cross-check), 2026-06-14

Read-only RE. Pins the contract **Task C gates against**: which DR/PPC context state a healthy
registered handler sets at the `0x50325f00`-equivalent point so the DR vectors `[0x64]→0x5000ED08`.
Tags: [STATIC] = static RE of our ROM (`rom901.bin`, md5 `d1a267a9…`, base `0x50000000`);
[QEMU-BEHAVIORAL] = QEMU mac99 9.0.1-ROM oracle (`tools/qemu-rig.sh`, rundir `/tmp/qemu-rigB`).
**No QEMU MMIO address is cited as a reference value** (QEMU MacIO BAR0 = `0x80000000` ≠ our `0xF3000000`).

### B.1 The signal spec — what a healthy delivery does (the Task C contract)

A healthy registered CGRP handler, invoked from the NK EXT path that today falls through to
`0x50325f00`, performs this sequence. Task C must reproduce the **net effect** (steps 2–4), not the
handler's internals:

1. **Pending-bitmask read / source identify (already happens today in the fallback).** [STATIC]
   The `0x50325f00` body is the NK's hardware interrupt-source scanner, NOT a no-op:
   - `lwz r20,-0x20(r1)` → KDP/per-CPU base; `lwz r22,0xf18(r20)` → the PIC/MMIO base it scans.
   - It reads the controller (`lwbrx r26,r22,r26` at `0x50326068`), masks the source id
     (`clrlwi r26,r26,0x14`), and looks the **source→level** up in a byte table at `0x3f00(r26)`
     (`lbz r28,0x3f00(r26)` → `r28` = the interrupt **level**).
   - It clears that source's pending bit in the per-source word table at `0xf28(r20)`
     (`andc r24,r24,r28; stw r24,…` at `0x50325ff8–0x50326008`) and maintains a pending **count**
     halfword at `0x910(r1)` (`sth r27,0x910(r1)`).
   - The fallback then returns via `b 0x503254e0` (`0x50326104`) — the EXT restore/`rfi` epilogue.
     **It records the interrupt and returns to PPC; it never touches any 68k/DR state.** This is
     exactly stage-2-MISSING from the main diagnosis, now confirmed at the instruction level. [STATIC]
   - The NK PIC descriptor pointer at `*(0x68ffefd0)` is live and valid in the oracle
     (`0x68ffefd0 → 0x5fffef00`, the Hnfo record). [QEMU-BEHAVIORAL]
2. **Set the DR's between-instruction take-exception state — the `cr2lt` analogue.** This is the
   step the fallback omits. In register/field terms (our addresses): the DR checks `bgectr cr2` at
   **every opcode-handler tail** (e.g. `0x50468ae8`); a *clear* `cr2lt` continues, a *set* `cr2lt`
   diverts to the unified slow-path `0x5046d114`. Task C must arrange that at the next DR boundary
   `cr2lt` is set **with an interrupt cause** (not a fault/trap cause). See B.2 for the lever. [STATIC]
3. **DR builds the genuine 68k frame & vectors.** `0x5046d114`'s interrupt branch saves the 68k PC
   (`addi r4,r24,-2; stw r4,0x6c(r31)`), builds a 68k exception frame on the 68k SSP (A7=`r1` in the
   DR map) with **vector offset `$64`** in the format/vector word and the **saved SR/IPL**, then
   vectors through `[0x64] = 0x5000ED08`. (`0x5046d114` is in the RAM-resident mirror-emulator region
   `0x5046xxxx`, beyond the 4 MB ROM file — not statically disassemblable from `rom901.bin`; body
   facts carry from the archived probe-dump RE in `2026-06-13-m13-rescope-dr-autovector.md`.) [STATIC]
4. **The 68k handler validates the frame** — independently confirmed by the oracle. The live level-1
   autovector handler reads its exception frame and asserts the vector and SR the DR must have built:
   ```
   cmpi.w  #$64, $6(a7)      ; format/vector word == $64  (autovector level-1 offset)
   bne.b   ...
   movem.l d0-d1/a0-a1,-(a7)
   move.w  $10(a7), d0       ; saved SR (now +0x10 after the movem push)
   andi.w  #$e700, d0        ; mask T1T0/S/IPL
   ```
   [QEMU-BEHAVIORAL — handler at QEMU heap `0x0047d0ba`; OUR equivalent is `0x5000ED08`, behavior-matched only]

   **Net contract for Task C:** at the `0x50325f00`-equivalent point, after the NK records pending,
   set the DR's saved take-exception context to "interrupt, level N (≥1), vector `$64`" so that at the
   next DR between-instruction boundary the DR's own `0x5046d114` path builds a frame with format/vector
   word `$64` and the saved SR, and vectors `[0x64]→0x5000ED08`. Do **not** build the frame in host code
   (falsified 4×) — drive the DR's own builder.

### B.2 The cr2-feed lever (Q-0c) — RESIDUE (register/context state, not a memory latch)

**Verdict: residue, well-characterized — NOT statically pinnable to a byte offset, and this does NOT
block Task C.** [STATIC]

- For **faults/traps** `cr2lt` is set inline, opcode/fault-driven, inside `0x5046d114`
  (`crmove`/`crset cr2lt`) — already established (archived RE).
- For **interrupts** there is **no memory location the DR re-derives `cr2lt` from each check**. The
  archived probe-dump RE settled the make-or-break question: the interrupt `cr2lt` is **PPC
  register/context state** — the `cr2` bit in the DR's **saved CR**, set when the NK manipulates the
  DR's saved PPC context on delivery. The DR's PPC-resume context lives in the `ECB+0x740` family
  (`ECB=0x68fff000`). The candidate lever is therefore **the saved-CR word in the DR's resume context,
  cr2 field**. [STATIC, carried from `2026-06-13-m13-rescope-dr-autovector.md`]
- **Why not pinned to an offset:** the exact byte offset of the saved-CR word within the DR resume
  context, and the precise interrupt-vs-fault cause encoding `0x5046d114` keys on, are in the
  RAM-resident `0x5046xxxx`/`0x5046e1a4` save/resume path — absent from the ROM file, and beyond the
  Q-0c static bound (≤2 call levels / ≤12 funcs around `0x5046d114`/`0x50325f00`) because those
  bodies are not in the static image. QEMU cannot pin it either: it runs the real registered handler
  (heap-resident, not in any ROM), so the oracle shows the *result* (frame + vector `$64` + SR, B.1.4)
  but not the saved-CR write site.
- **What would pin it (escalation, NOT required for Task C):** a live probe-dump of OUR engine at the
  DR resume/save boundary — dump the DR's saved-context block at `ECB+0x740` across a real
  trap-driven `0x5046d114` entry (`SS_PROBE_PC=0x5046d114` + `[ECB+0x740 : 0x80]`), diff the saved-CR
  word with/without `cr2lt` set, to read off the exact offset and cause encoding.
- **Why it does not block Task C:** the milestone's Task C HLEs the handler by *setting the DR's
  `cr2lt` autovector trigger* at the `0x50325f00` point. The binding constraint Task C needs from this
  task is the **nature** of the lever — register/context state in the DR's saved CR, not a pokable
  memory latch (so a memory-poke approach is dead, consistent with the falsified-table). Task C
  locates the saved-CR field empirically with the escalation probe above as its first sub-step.

### B.3 Early-bringup confirmation — refutes the strong-circular fear [QEMU-BEHAVIORAL]

The oracle (`/tmp/qemu-rigB/probes.txt`, ladder 10/20/40 s) shows 68k interrupt delivery is an
**early-boot capability**, established progressively and well before Finder (~30 s+):

| t (s) | `[0x64]` level-1 vector | Ticks `*(0x168)` | `$6e4` VBL chain `*(0x6e0)` |
|-------|--------------------------|------------------|------------------------------|
| 10 | `0x00000000` (not installed) | 0 | `0x00493dfe` (already populated) |
| 20 | `0x0047d0ba` (handler INSTALLED) | 0 | `0x00493dfe` |
| 40 | `0x0047d0ba` | `0xb1740000` (BE → `0x000074b1` = 29 873, **Mac OS ticking**) | `0x00493dfe` |

The autovector handler is installed by ~20 s and Ticks advance by ~40 s — interrupt-driven 68k
service comes up **during early bringup, before the system is "up."** EXT→68k delivery is therefore a
bootstrap-time capability, not a Finder-only one; the fear that delivery is strongly circular with a
fully-booted system is **refuted**. (Whether OUR boot can *reach* CGRP registration once Task C lands
a single clean delivery remains the open keystone test from the step-0 recon — that is about
registration reachability, a separate question from delivery being early-capable.)

### B.4 Recorded divergences (oracle vs our DR bodies)

- **PIC topology / addresses.** QEMU mac99 = `openpic` + `macio-newworld` at BAR0 `0x80000000`
  (escc `0x80012000`). These are **QEMU-model addresses only**; our paravirtual machine uses the NK
  software interrupt-source struct at `*(0x68ffefd0)` and (in the fallback scan) the MMIO base in
  `r22 = [KDP+0xf18]`. No reference value crosses over. [QEMU-BEHAVIORAL]
- **Hardware-scan vs paravirtual read — complementary, not contradictory.** Our `0x50325f00` does a
  real controller read (`lwbrx r22`) to *identify the source*, then records into the NK software state;
  the 68k handler `0x5000ED08` reads the *software* struct at `*(0x68ffefd0)`. Two layers (PPC source
  ID → NK record → 68k software consume), not a divergence. [STATIC + prior PROBE]
- **Ticks transient at the final 45 s probe** (`*(0x168)` read back as 0): a probe/shutdown-window
  race overlapping the screenshot, not a boot regression; the 40 s reading (`0x000074b1`) is the
  load-bearing one. [QEMU-BEHAVIORAL]

### B.5 Files / evidence

ROM static: `/Users/Shared/macemu/dumps/rom901.bin` (md5 `d1a267a91993bf2c27fac1ca91ad5974`).
Oracle: `/tmp/qemu-rigB/probes.txt` + `device-tree.txt` (transient; regenerate via
`bash SheepShaver/tools/qemu-rig.sh --ladder 10,20,40`).

---

## Provenance (archived process trail — `docs/archive/2026-06/planning/`)

- `2026-06-13-m13-atrap-bootstrap.md` — original M13 plan (Task A as injection) + Task-0 recon addenda.
- `2026-06-13-m13-taskA-bakeoff.md` — 3-approach bake-off + the full injection map + resume-prologue impl.
- `2026-06-13-m13-diag2-nk-propagation.md` — diag-2 fan-out brief (NK routing / 68k readiness / oracle).
- `2026-06-13-m13-rescope-dr-autovector.md` — the autovector re-scope + the RE verdict.
- `M13-SUMMARY-REPORT.md` — running status report (supersedes a wrong session-1 draft).
- Durable lessons: `LEARNINGS.md` (top entry, 2026-06-13 M13). Code warnings at the STUB sites.
