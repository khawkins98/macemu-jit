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

## Task C cr2-feed pinning probe — addendum (2026-06-14)

Required FIRST sub-step of Task C (the empirical cr2-feed pin). All boots via the slot
protocol, newworld 9.0.1 diagnostic template + `SS_NW_PIC=1` (the precondition env;
`SS_NW_EAGER_TICK` does not exist). Binary built green at commit `b576c7ba`. Tags: [PROBE✓].

### C-pin.1 The frontier reproduces under `SS_NW_PIC=1` (NOT under the bare template)
- Bare template (PIC=0): boot parks early — `dec_expiries=4`, EXT delivered ×1, the wall is
  barely reached. **`SS_NW_PIC=1` is required** to reach the documented Q-0a frontier.
- With `SS_NW_PIC=1`: `dec_expiries=4181` (healthy), wall `0x50468ae4` ≥1000 visits, EXT
  fallback `0x50325f00` ≥100 visits, **slow-path `0x5046d114` fires (≥1 visit)**, registration
  `0x5031b290` **0 visits** (the keystone gap, as expected). [PROBE✓]

### C-pin.2 The cr2-feed lever is PINNED — but it is the LIVE DR CR, not an ECB save slot
The decisive finding **revises B.2's working theory**. B.2 assumed the DR was *suspended*
at EXT time, so its CR lived in an ECB save slot (`ECB+0x740` family) that Task C would have
to locate by offset. **Empirically the interrupted context AT the host EXT-delivery seam IS
the DR itself:**
- EXT-delivery `restart_pc` (`pc()` at the host hook) observed = `0x50466144`, `0x50498540`,
  `0x50488148` — **3/3 in the DR mirror-emulator range** (`0x5046xxxx`/`0x5048xxxx`/`0x5049xxxx`).
  The wall is the DR idle-spin, so when EXT fires the DR is the live PPC context. [PROBE✓]
- Therefore the DR's CR **is the live `cr()` register** at our EXT hook — no ECB byte-offset
  needed in the common path. The `ECB+0x740` escalation from B.2 is **moot** for this seam
  (and, separately, the absolute-range probe form `[0xADDR:SIZE]` is unsupported by the probe
  parser — only `[rN:SIZE]` ranges and single-word `[0xADDR]` work; `r31=ECB=0x68fff000` is
  confirmed live at `0x5046d114`).

### C-pin.3 cr2 bit math, confirmed both directions [PROBE✓]
- cr2 field = host-CR mask `0x00f00000`; **cr2lt = bit mask `0x00800000`**.
- Slow-path `0x5046d114` entry (fault-driven): `CR=0x80f01820` → cr2 nibble `0xf` → **cr2lt=1**
  (confirms B.1.2: a *set* cr2lt is the divert condition; `bgectr cr2` falls through to
  `0x5046d114` when cr2lt=1).
- Normal DR execution at the wall `0x50468ae4`: `CR ∈ {0x00100083, 0x40100f0f, 0x20101efe,
  0x20100f07}` → cr2 nibble `∈{1,0}` → **cr2lt=0** in every sample (normal continue). So
  OR-ing `0x00800000` into the live CR at the EXT seam is a meaningful, non-redundant divert
  trigger.

### C-pin.4 CAUSE encoding — RESOLVED by live RAM disasm (feasibility GREEN)
Escalation done (coordinator-approved): single bounded lldb read of host `0x40005046d114`
(768 B) + capstone PPC-BE, plus the DR dispatch loop at `0x504689e0`. The interrupt-vs-fault
selection is **NOT** un-fabricatable fault-cause side-state — it is a **memory-backed pending
field plus the cr2lt divert**, both coherently reachable from our seam. Mechanism, pinned
[DISASM✓ live RAM]:

- **Opcode tail** (e.g. `0x50468ae8`): `bgectr cr2` → next opcode when cr2lt **clear**; falls
  through to `b 0x5046d114` (DIVERT) when cr2lt **set**. The dispatch helper `0x50468b08`
  (`rlwimi r29,r27,3,..; mtctr r29; lhau r27,2(r24)` + the movem cr4–7 loads) does **NOT touch
  cr2** — so cr2lt is preserved across the loop and only *tested* at the tail. **Poking memory
  alone will not divert; cr2lt must be set** (corroborates B.2's "register/context state").
- **Slow-path `0x5046d114`** calls `0x5046d3e4`, which is the interrupt selector:
  `lwz r3,0x1d0(r31)` (pending level = **`[ECB+0x1d0]`**), `clrlwi r6,r25,0x1d` (IPL mask =
  **`r25 & 7`**), `cmplw r3,r6; blelr` → returns (no interrupt) if `pending <= mask`. When
  `pending > mask` it sets cr2eq, so `0x5046d140 beql cr2` calls the **interrupt builder
  `0x5046d248`**, which builds the frame: `li r4,0xc0; rlwimi r4,r7,2,..` → vector offset, and
  `xori r6,r4,0xa0` → for level 1 yields **`r6 = 0x64`**, written to frame+6 (`sth r6,6(r1)`) —
  exactly the `cmpi.w #$64,$6(a7)` the handler asserts (B.1.4). The faulting 68k PC goes to
  frame+2 (`stw r24,2(r1)`). The opcode-mask fault/trap decode (`0x3c/0x46c0/0x6000` at
  `0x5046d160+`) is a **different branch**, reached only when the interrupt selector declines.
- **Steady-state values [PROBE✓]:** `[ECB+0x1d0]=0` always (the pending latch nothing sets —
  the smoking gun for "no registered handler"); `[ECB+0x71]=1`; `r25&7 = 0` at the fault-driven
  slow-path entry (`r25=0x20`) but `= 7` at the idle wall `0x50468ae4` (`r25=0x27`).

**Task C lever (pinned, no fabrication):** at the EXT seam (DR live), write `[ECB+0x1d0] =
level` (via `gpr(31)`=ECB) **and** OR `0x00800000` (cr2lt) into the live `cr()`; the DR resumes,
self-diverts at its next tail, and its own `0x5046d248` builds the genuine `$64` frame. No
forged CGRP table, no host frame fabrication. Refines B.2: the interrupt selector IS a pokable
memory latch (`[ECB+0x1d0]`); B.2 missed it only because the body was not statically disassembled.

**Residual (delivery-success variable, not a feasibility blocker):** the IPL-mask gate
(`r25 & 7`) must be `< level` at the divert. Mask is 7 at the idle wall (level-1 would be
*masked there* — correct 68k semantics, NOT to be forced). Delivery is therefore gated on
`(gpr(25) & 7) < level` (misuse hardening — never force a masked interrupt, the M10 lesson). If
the DR is never at a deliverable IPL at the EXT seam, that is a captured frontier, not a crash;
the `SS_PROBE_68K=0x5000ed08` sub-contract answers it falsifiably.

### C-pin.6 IMPLEMENTED + delivery FALSIFIED — the IPL-7 frontier (stop-rule 1) [PROBE✓]
The HLE landed gated-OFF (`SS_NW_DR_AUTOVEC`, `sheepshaver_glue.cpp`, in the DELIVER path of
`deliver_pending_exception`, inside `MachineProfileIsNewWorld()`): on a DEC/EXT DELIVER, when
the interrupted PPC context is the DR (restart_pc in `[0x50460000,0x504a0000)`) and the IPL
permits, it writes `[ECB+0x1d0]=1` and ORs cr2lt into the live CR, then falls through to normal
NK delivery. All gates green (build, harness 353/353, machine ALL PASS, e2e-test 122, paravirtual
e2e PASS, gated-off A/B byte-identical: 0 `[NW-AUTOVEC]`).

**Env-on (`SS_NW_PIC=1 SS_NW_DR_AUTOVEC=1`): delivery sub-contract FALSIFIED, but cleanly.**
- **0 deliveries / `0x5000ed08` never ran / no crash / scheduler healthy** (dec_expiries 29k).
- Cause, decisive [PROBE✓]: **at EVERY delivery-seam sample the DR is at IPL 7** (`gpr(25)&7 == 7`,
  `SR=0x2700`) — the pre-System 68k spins *masked*. 512+ consecutive `[NW-AUTOVEC] skip (IPL
  masked)` at dr_pc cycling `0x50460100`/`0x50465ef8`, zero `ipl_ok`. The misuse-hardening
  correctly **refuses to force a masked interrupt** (the M10 lesson), so it never fires.
- Cross-fact: the DR *does* run at IPL 0 elsewhere (the fault-driven `0x5046d114` entry has
  `r25=0x20`), so IPL-7 is not a constant — but **the coherent host delivery seam (the NK DEC/EXT
  hook) only fires while the 68k is masked-and-idling (ceding to the NK)**; it never coincides
  with a DR-at-IPL<7 moment. The two phases are disjoint at this seam.

**Verdict — STOP-RULE 1 (deeper circularity), captured as frontier; NOT improvised around.**
The cause-encoding lever is correct and proven safe, but a level-1 autovector is *correctly*
undeliverable: the boot is wedged in 68k early-init at `SR=0x2700` (IPL 7), upstream of the point
where the OS lowers its interrupt mask to accept ticks. Delivering ticks cannot advance it — the
blocker is "why the 68k never lowers IPL below 7," not interrupt delivery. Forcing it (clearing
the mask, or level 7 — the DR selector's `cmplw pending,mask; blelr` blocks `7<=7` too) would be
exactly the falsified "force a masked interrupt" class. **Next:** characterize what the pre-System
68k is waiting on at IPL 7 (r24=`0x5000010a` low-ROM at the wall) — likely a memory flag the NK
should set, i.e. the route-around rises (ROM/OS-version), per the step-0 keystone caveat.

### C-pin.5 Evidence (slot rundirs, transient)
`/tmp/ss-slots/slot0/runs/`: `…085423` (frontier+PIC, 4-PC probe), `…085551` (full reg dump
@ `0x5046d114`), `…085849` (`r31`/ECB confirm), `…090032` (wall CR samples), `…090908`
(`[ECB+0x1d0]`/`r25` fields). Live RAM disasm: `/tmp/dr_5046d114.bin` (768 B @ host
`0x40005046d114`), `/tmp/dr_dispatch.bin` (512 B @ `0x4000504689e0`) — capstone PPC-BE.
Reproduce: `ss-slot-boot.sh --timeout 45 --env 'SS_NW_PIC=1 SS_PROBE_PC=0x5046d114'`.

---

## §C-pin.7 — the IPL-7 "spin": what it waits on (2026-06-14, one bounded recon pass)

Strict read-only recon (disasm + bounded probing; no inject/force/poke). Goal: characterize what the
C-pin.6 "IPL-7 spin at r24=0x5000010a" is waiting on. **The pass overturned the framing of the
question and surfaced a likely keystone false-negative.** Tags: [STATIC] = disasm of `rom901.bin`
(md5 `d1a267a9…`, base `0x50000000`); [PROBE✓] = live boot of our engine (`SS_NW_PIC=1`, slot
protocol). Evidence: slot rundir `/tmp/ss-slots/slot0/runs/20260614-093644.57802` (r24-ring + 68k
probe) and `…-093817.58053` (lldb mem read); lldb script `/tmp/lldb_m13.txt`.

### C-pin.7.1 r24=0x5000010a is NOT a spin — it is a mid-instruction fetch-pointer artifact
- **Live low-ROM == static ROM, byte-identical.** A single bounded lldb read of host
  `0x4000_5000_00e0…0120` and `…aad0…ab40` matches `rom901.bin` exactly — **low-ROM is not overlaid
  or patched at runtime.** So the static decode is authoritative here. [PROBE✓ + STATIC]
- In that authoritative decode, **`0x5000010a` is mid-instruction**: it is the displacement word
  `0008` of `lea $50000112(pc),a6` at `0x50000108` (`4dfa 0008`). The DR advances r24 by 2 per
  extension word (`lhau r27,2(r24)`, C-pin.4), so r24 transiently points *inside* a multi-word opcode.
  `r24=0x5000010a` is simply where the fetch pointer sat when the PPC EXT seam sampled it. [STATIC]
- **It is not visited as a loop.** `SS_PROBE_68K=0x5000010a:6` fired exactly **once** (match=1/6);
  the r24-ring shows `0x5000010a` at a single record (`@14384`, 1 hit). [PROBE✓]
- The actual code at `0x500000e0–0x50000130` is **straight-line early-boot init**: a run of subroutine
  trampolines (`bra.l 0x5000ab4e`, `…ae82`, `…aad8`; `bsr.l 0x500081f8`; `bsr.w 0x50000624/03f2/03f6`;
  `bsr.l 0x50007a20`; then an `FE4A` F-line) executed **once**, not a wait loop. The `0x5000aad8`
  callee is device/IO register init (byte writes `move.b (a3)+,$1e00/$600/$400/$1800/$1600(a2)`,
  `move.b #$7f,$1c00(a2)`). [STATIC]

**⇒ Q1/Q2/Q3 as posed are vacated:** there is no polled flag at `0x5000010a`, no spin loop there, and
no `move.w …,SR` exit-gate around it. The "IPL-7 spin at 0x5000010a" in C-pin.6 was the transient
r24 at the autovector seam, mis-read as a wait location.

### C-pin.7.2 The keystone may be a probe false-negative — the 68k handler region **DOES execute in baseline**
This is the load-bearing new finding, and it cuts against the M9–M13 premise. In **plain baseline**
(`SS_NW_PIC=1`, **autovector HLE OFF**), the r24-ring shows the 68k interrupt-handler region running:
- `0x5000ed0a` is entered **207 times** as a far-`BRANCH` target (a vector/redirect into the handler),
  and the full handler sweep executes linearly: `ed0a→ed10→ed38…ed60` (level dispatch + service) and
  the deferred-task/Time-Manager/VBL pass `ee5a→eece` — the exact `0x5000ee58` pass the diagnosis says
  "runs unconditionally after service." [PROBE✓]
- **Why "0x5000ed08 never ran" was likely false:** the DR vectors to `0x5000ed08`, then the first
  `lhau` advances r24 to `0x5000ed0a` **before** the dispatch-hook sample. The ring records `ed0a`/
  `ed0e` and **never the even vector word `ed08`/`ed0c`**. `SS_PROBE_68K=0x5000ed08` (exact-match,
  edge-triggered) is therefore **blind to the handler entry** — the same fetch-pointer granularity
  artifact as C-pin.7.1. The "`0x5000ed08` never runs" keystone (TL;DR, stage-3, C-pin.6) is **very
  likely a measurement artifact**: the handler entry lands on `0x5000ed0a`. [PROBE✓ — strong, see residue]

### C-pin.7.3 The real wedge: the **68k DR halts** at ~10 s while the NK busy-spins
Independent of delivery, the heartbeat pins where progress actually stops [PROBE✓]:
- **`jDR` (68k blocks executed) FREEZES** — `2474517` flat across HB 10/20/30 s (run B: `2897186` flat
  across 23/33/43 s) — while **`jNK` climbs unboundedly** (740M→2196M) at ~72M blocks/s. The 68k stops
  advancing ~10 s in; only the NanoKernel spins. The 207 handler entries all occur **before** the freeze.
- `[ALARM] boot stalled at 15s … guest NOT idle, still spinning … model-rejection alert / pre-System
  screen, e.g. "won't work on this model"`. `dec_expiries=4054` (scheduler healthy), `comp` frozen at
  6708. So: ticks are delivered, the VBL/deferred pass runs, the scheduler lives — **and the 68k still
  parks forever.** [PROBE✓]

### C-pin.7.4 Verdict — **(b) deeper circularity / (c) ROM-OS-version**, and interrupt delivery is *not* the lever for a new reason
- **(a) Cheap-unstick on a missing NK memory handshake: NO.** There is no polled flag at `0x5000010a`;
  the framing that pointed at one is an artifact.
- **(b) Deeper than delivery: YES, and more strongly than C-pin.6 stated.** C-pin.6 concluded "ticks
  cannot advance it because the 68k is masked at IPL 7." This pass shows the stronger fact: in baseline
  the **handler already runs (≥207×) and the VBL pass already fires, yet the 68k DR still halts.** The
  blocker is downstream of interrupt delivery entirely — the 68k early-boot/System is waiting on
  *something that is not a tick* (a driver/device that never responds, or a model/gestalt gate), then
  ceding to the NK permanently. The `[ALARM]` text's own hypothesis — a **model-rejection / "won't run
  on this model"** pre-System gate — is the most economical fit and is a **(c) ROM/OS-version** issue,
  not an interrupt one. This *re-validates* the C-pin.6 STOP-RULE-1 decision (delivery is not the lever)
  while removing its stated cause (it is not "the mask is never lowered for ticks").
- **(c) Version-specificity:** the wedge looks **OS/ROM-version-specific**, not universal NK behavior —
  it is a pre-System (System-file/Gestalt era) gate on the 9.0.1 path, consistent with the QEMU oracle
  booting the *same ROM* fine to ticking (B.3) where our paravirtual machine layer differs. This is
  exactly the signal that a **route-around (Option 3: ROM/OS-version sweep, or closing the machine-layer
  gap the System rejects)** is now *indicated* over further interrupt-delivery work.

### C-pin.7.5 Residue (honest, one pass is all this was)
1. **Mechanism of the 207 `0x5000ed0a` entries not 100% pinned.** They are far-`BRANCH` targets into
   the autovector handler (consistent with real exception entry), but this pass did not prove they are
   autovector-driven vs. a direct ROM call of the level dispatcher. **What would pin it:** widen
   `SS_PROBE_68K` to `0x5000ed0a` (the post-fetch boundary, not `ed08`) and/or dump the 68k exception
   frame (`$6(a7)==$64`) at entry; cross-check the source PC `≈0x5007b2fe` of the redirect. *This single
   fix to the probe target retroactively tests the whole "handler never runs" keystone* — high-value,
   cheap, deferred only by the one-pass budget.
2. **What the 68k is actually waiting on at the halt** is not pinned (only that it is not a tick).
   Pinning it means probing the *last* 68k subroutine the DR runs before `jDR` freezes (capture the r24
   instruction-boundary trail in `[0x5000xxxx]` in the final 1–2 s) and identifying the device/gate it
   polls — likely the model-rejection path the `[ALARM]` names. That is the next milestone's recon, not
   this pass.

---

## §C-pin.8 — keystone retest: VERDICT = **KEYSTONE-ARTIFACT** (delivery was happening all along)

Close-out due-diligence: one bounded baseline boot (`SS_NW_PIC=1`, **autovector HLE OFF**) re-targeting
the keystone probe to the post-`lhau` PC, plus one lldb read of the DR-built 68k exception frame.
Evidence: slot rundir `/tmp/ss-slots/slot0/runs/20260614-094617.59580`; lldb script `/tmp/lldb_ks.txt`.
Tags: [PROBE✓] = live boot of our engine; [STATIC] = `rom901.bin` disasm.

### C-pin.8.1 The probe matches at the corrected target — the dispatch hook DOES see the entry
`SS_PROBE_68K=0x5000ed0a:8` hit its cap (**8/8 matches**) in plain baseline, with a stable
`a7=0x17ffeaee` across deliveries. The earlier `SS_PROBE_68K=0x5000ed08` "never matched" purely because
the DR's first `lhau` advances r24 `0x5000ed08→0x5000ed0a` **before** the dispatch-hook samples — an
exact-match/edge-trigger granularity blind spot, not an absence of delivery. [PROBE✓]

### C-pin.8.2 It is a genuine `$64` level-1 autovector — frame proven
At entry `a7=0x17ffeaee` is the exception-frame SSP. A single lldb read of host `0x4000_17ff_eaee`
(lldb `--size 2` byte-swaps the guest big-endian words; values below are the corrected guest BE words):
- `[a7+0]` (word) = **`0x2000`** = saved **SR**: S=1 (supervisor), **IPL = 0** (bits 10–8 = 0) — the
  interrupted 68k was **NOT masked**; this was a legitimate, deliverable autovector, not a forced one.
- `[a7+2]` (long) = **`0x50034cae`** = saved 68k PC (ROM).
- `[a7+6]` (word) = **`0x0064`** = format/vector word = **`$64`** = the level-1 autovector offset. ✓
[PROBE✓]

Cross-check against the ROM dispatcher (low-ROM == static, C-pin.7.1): `0x5000ed08` is the level-1
vector entry — `movem.l d0-d3/a0-a3,-(a7); moveq #1,d3` (d3 = level) `→ 0x5000ed36`, which increments an
interrupt counter, loads the paravirtual source struct `movea.l 0x68ffefd0,a2`, and reads the pending
bits `move.l 0x28(a2),d0` / source table `movea.l 0x14(a2),a0` — the documented `*(0x68ffefd0)+0x28/
+0x14` paravirtual mechanism. (Our ROM dispatcher does **not** do the QEMU heap-handler's
`cmpi.w #$64,$6(a7)` validation — that handler was behavior-matched only, B.1.4; the `$64` proof here is
the DR-built frame word, which the native delivery path produced unaided.) [STATIC + PROBE✓]

### C-pin.8.3 Reconciliation with C-pin.6's "IPL 7" reading
No contradiction: C-pin.6 measured IPL 7 **at the NK DEC/EXT hook** (`gpr(25)&7==7`) — the seam where
the DR has *ceded to the NK and is masked*. The native autovector deliveries (this section) happen at a
**different point** — the DR's own between-instruction boundary, where the saved SR shows **IPL 0**. The
`SS_NW_DR_AUTOVEC` HLE only fired at the masked NK seam and so correctly skipped; meanwhile the **native
NK→DR path was delivering all along at IPL-0 boundaries**. The HLE was solving a non-problem at the wrong
seam.

### C-pin.8.4 VERDICT — KEYSTONE-ARTIFACT; M13's delivery-falsified narrative needs reframing
**The M9–M13 "the 68k interrupt handler `0x5000ED08` never runs / interrupts are never delivered"
keystone was a measurement artifact.** In plain baseline, genuine `$64` level-1 autovectors are
delivered (≥8 probe matches; ring showed ~207 entries in C-pin.7), the handler runs the full
level-dispatch + deferred-task/VBL pass, the saved SR proves the delivery was legitimate (IPL 0), and the
scheduler is healthy (`dec_expiries=3406`). **Interrupt delivery is NOT the blocker** — it works natively.
The real wedge is downstream: the 68k DR halts (`jDR` frozen, C-pin.7.3) into the `[ALARM]` **model-
rejection / pre-System gate** (Gestalt/machine-type rejection), which is a **ROM/OS-version** issue, not
an interrupt one.

**Close-out consequence:** every M9→M13 artifact framed around "deliver the 68k interrupt" (Task A
injection, Task C `SS_NW_DR_AUTOVEC`, the CGRP-handler-registration thesis) was aimed at a problem the
native path already solves. The forward direction is the **route-around** (the next milestone): pin and
clear the pre-System model-rejection gate the `[ALARM]` names — **not** any further interrupt-delivery
mechanism. (Per the lead's scope, that gate is the next milestone's recon, not investigated here.)

### C-pin.8.5 Residue
The saved PC `0x50034cae` (what the 68k was doing when the autovector preempted it) and the precise
model/Gestalt gate are not pinned — explicitly deferred to the next milestone. The `$64`/SR/handler
evidence is direct; the "207×" count is from the C-pin.7 ring (this boot capped the probe at 8).

---

## Provenance (archived process trail — `docs/archive/2026-06/planning/`)

- `2026-06-13-m13-atrap-bootstrap.md` — original M13 plan (Task A as injection) + Task-0 recon addenda.
- `2026-06-13-m13-taskA-bakeoff.md` — 3-approach bake-off + the full injection map + resume-prologue impl.
- `2026-06-13-m13-diag2-nk-propagation.md` — diag-2 fan-out brief (NK routing / 68k readiness / oracle).
- `2026-06-13-m13-rescope-dr-autovector.md` — the autovector re-scope + the RE verdict.
- `M13-SUMMARY-REPORT.md` — running status report (supersedes a wrong session-1 draft).
- Durable lessons: `LEARNINGS.md` (top entry, 2026-06-13 M13). Code warnings at the STUB sites.
