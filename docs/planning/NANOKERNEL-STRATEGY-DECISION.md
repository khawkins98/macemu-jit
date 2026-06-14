# Strategic decision — the right foundation for NewWorld 9.x (the NanoKernel question)

> **Terminology (load-bearing):** the "NanoKernel" is **Apple's** PPC supervisor inside the NewWorld
> Mac OS ROM — it owns the 68k emulator, PPC exception/interrupt handling, mixed-mode switching, the
> nano scheduler, and supervisor MMU. SheepShaver *runs* it (as guest code); it is not SheepShaver's
> code. So the realistic options are **feed the NK what it expects** (complete our Machine Layer) or
> **switch/borrow the whole emulator base** — NOT "fork/reimplement the NK," which would mean writing
> a bug-for-bug Mac-OS-9-compatible supervisor from black-box behavior (a multi-year RE effort, and
> unnecessary since Apple's NK already works). "Fork the NK" is essentially a category error.

**Status:** OPEN strategic question, raised 2026-06-13. Needs a deliberate decision (not another
tactical sprint). **Related:** `docs/planning/M13-FINDINGS-interrupt-delivery.md` (the technical wall),
the "hard fork is inevitable" note, and the alternative-base evaluations
(`DINGUSPPC-EVALUATION-PLAN.md`, `INFINITE-MAC-EVALUATION-PLAN.md`, `SNOW-EVALUATION-PLAN.md`,
`COMPATIBILITY-PAYOFF-DINGUSPPC-REVISIT.md`).

## Why this is on the table

NewWorld 9.x support has been gated on **one** mechanism — interrupt delivery through the
SheepShaver/Apple **NanoKernel** to the 68k world — for **five milestones (M9→M13, since ~2026-06-12).**
This session's investigation (5 independent confirmations) established that:
- The NK is an **opaque, deep** subsystem: hand-injection is architecturally impossible; correct
  delivery requires the NK's own registered code-group (CGRP) handler, which the boot never installs;
  the DR emulator's interrupt trigger is register/context state with no external lever.
- Each layer of RE has revealed another layer. The "principled" path (make the NK register its handler)
  is still unscoped and may be circular with the tick-starvation it would cure.

That is a lot of project energy spent fighting a black box. It is worth **deciding on purpose** whether
to keep going down the NK path or change foundation — rather than defaulting into more NK RE by inertia.

## The decision framing

**Option 1 — Keep fighting the NK (principled).** Make the NK install/register its CGRP interrupt
handler the way a healthy boot does. Pivotal unknown (RE in progress): is registration reachable
*before* the wedge (fixable) or gated *behind* it (circular → needs a one-shot bootstrap)?
- Pro: stays on the existing machine-layer investment; "real" emulation.
- Con: deepest rabbit hole; no bound on remaining effort; the lever may not be externally drivable.

**Option 2 — Sidestep the NK interrupt model** (HLE the tick the idle loop waits for; replicate the
registered handler's context-signal; one-shot bootstrap). Tactical bypasses.
- Pro: could unblock the boot to see the *next* wall cheaply.
- Con: against the "solid foundations, no surgical shortcuts" principle; may just move the wall.

**Option 3 — Change foundation / borrow a working model.** Borrow the interrupt/NK handling from a
working PPC emulator (QEMU reaches the handler; DingusPPC; Infinite Mac), or build NewWorld 9.x on a
different base entirely (the deferred "hard fork").
- Pro: a working reference exists; could leapfrog the NK wall.
- Con: large; integration cost; may import a different set of problems.

**Option 4 — Re-target the goal.** Is THIS boot (9.0.1 diagnostic) the right success target, or would a
different ROM/OS version (e.g. 9.2.1, which QEMU boots to the handler) hit a friendlier path?

## Decision inputs

### Ideation triage (2026-06-13, `newworld-ideation` agent — folded in)
The divergent pass converged on: **answer the circular-vs-early-failure fork cheaply before committing
to any fix.** Top candidates, cheapest-first (none relitigate the 5 falsified injections):
1. **QEMU-trace the CGRP installer** *(low effort / very high payoff)* — on the 9.2.1 QEMU oracle,
   watchpoint the write that sets `CGRP+0x20≥2`; capture the call stack + boot stage → names the
   installer routine and whether it's reachable pre-wedge. (Behavioral oracle; confirm routine, not addresses.)
2. **Is the wedge a swallowed fault, not starvation?** *(low / high)* — probe for SIGSEGV/SIGBUS/
   alignment/PR-bit faults in the 15s window; the registration code may run and silently fault → a
   clean MSR/MMU bug rather than an interrupt saga.
3. **Directly invoke the deferred-service routine `0x5000ee58`** at a coherent DR boundary on a host
   timer *(medium / high)* — ticks the starved queues WITHOUT fabricating an interrupt frame; NOT one
   of the 5 falsified injections (verify it isn't a relabel before building).
4. **HLE the registered CGRP handler as a PPC-context shim** *(high / high)* — make the NK fallback
   `0x50325f00` do the handler's bitmask→DR translation once, in the correct PPC context, to bootstrap
   to natural registration. The likely real fix — but needs #1 to characterize the contract first.
5. **ROM/OS matrix sweep** (9.2.1 / 1.1 / 9.0.4 in our engine) *(low / medium-high)* — a different rev
   may reach registration without wedging; cheap route-around.
6. **Machine-Layer real timer** (modeled VIA T2 / SCC) as the un-stick source *(high / high)* —
   strategic pivot (Option 1↔3 hybrid); sequence after #1–#2.

**Recommended sequence:** #1 + #2 in parallel (both decide the fork) → if early-failure-fault, fix the
bug (cheap win); if circular, pursue #3/#4 as the bootstrap, with #5 alongside and #6 as fallback.

### RE verdict — NOT circular; silent early-failure (dynamic agent, 2026-06-13, medium confidence)
- **Our boot:** `CGRP+0x20` (0x68ffc1e0) is written ONCE, early, to placeholder **1** (NK init
  `0x50311a58`); the dispatch table (+0x38/+0x3c/+0x44) is **never** written; **no faults / no SIG\***
  (so #2 "swallowed fault" is largely REFUTED — a fault would leave a fingerprint). The registration
  simply never happens, silently.
- **QEMU oracle (9.2.1):** EXT→68k delivery is an **early-bringup** capability (ticks flow ~12s, VBL
  `$6e4` chain ~8s — well before Finder ~30s), so registration is NOT gated behind deep OS progress →
  refutes the strong circular hypothesis. Our engine reaches that same early window without faulting,
  yet still never registers.
- **The installer:** CGRP registration is done by a **client-driven nanokernel-call family at
  `0x5031b294` / `0x5031b3b4` / `0x5031b2b4`** (selector-dispatched, owner-guarded) — NEVER run
  autonomously by NK init. It must be **invoked by a client NK call during early bringup**, and **our
  boot never issues it** (or never satisfies its precondition).

**Implication: Option 1 is cheaper than feared** — a *silent missing trigger* (a specific NK-call our
bringup doesn't make), not an irreducible bootstrap loop. The "unbounded effort / lever may not be
drivable" fear that motivated abandoning the NK is removed.

**Highest-value next step:** pin the exact client NK-call selector (the `0x5031b294` family) that
registers the CGRP handler, find where healthy early-bringup issues it, and confirm our boot never
reaches/completes it. (Then #4 HLE-the-handler / one-shot-bootstrap is the likely fix; #5 ROM-matrix a
cheap parallel route-around.) Cross-check pending from the static RE agent (`nk-registration-static`).

> Confidence: MEDIUM — our-boot evidence is direct but absence-based; QEMU evidence is behavioral/
> indirect (CGRP store not watched). Hardens to HIGH by directly tracing the `0x5031b294`-family call.

### SYNTHESIS — both agents (dynamic + static), 2026-06-13

**AGREED (HIGH confidence — two independent methods converge):**
- The registration routine is **`0x5031b290`** (the default-interrupt-CGRP path of an NK supervisor
  service): `mfspr r15,0x110`(KDP) → `lwz r14,-0x338(r15)`(CGRP base 0x68ffc1c0) → stores the descriptor
  + `+0x38`(table) / `+0x3c`(TABLE_BASE=tableptr+0x20) / `+0x44`(count, capped 0x40). Same `-0x338(KDP)`
  slot + offsets the EXT guard reads at `0x5031488c`. Unambiguously THE writer.
- It is a **client-driven NK service** (reached via the NK indexed dispatcher ~`0x5031af00`; default
  path taken when caller passes **r3==0**), **NOT autonomous cold-boot code**. Something must *call* it.
- It **never runs in our pre-wedge boot** (static: registration block-entries `0x5031b29c`/`b2ac` silent
  while the control `0x50325f00` fires; dynamic: zero footprint, table never written, `CGRP+0x20=1`).
- **No fault / no SIG\*** → the "entered-but-faulted" early-failure flavor is RULED OUT by both.

**OPEN (agents lean opposite — the one unresolved bit):** WHY the call is never issued —
- *Circular* (static): the call site is downstream of the SystemTask/driver-load phase the starved idle
  loop is waiting to reach (need-ticks→advance→load-drivers→register→ticks).
- *Upstream-guard skip / early* (dynamic): QEMU shows ticks/68k-handler live EARLY (~8–12s, before
  Finder), so registration may be an early op our boot skips a precondition for — BUT the QEMU evidence
  is indirect (couldn't watch the CGRP store; "early" partly reflects the already-healthy 68k side).
- **Discriminator (next step):** directly observe what calls `0x5031b290` in a healthy boot + its
  precondition — via PPC gdb on QEMU (absent on host — install) OR trace our NK dispatcher
  (`~0x5031af00`) selectors during bringup to see if the registration selector is ever requested.

**CONVERGENT FIX DIRECTION (both agents, regardless of which WHY):**
- A **one-shot bootstrap** to break the idle spin / get registration issued once, then self-sustain —
  e.g. invoke the deferred-service routine `0x5000ee58` once, or issue the real NK registration call
  `0x5031b290` ourselves with a proper descriptor (now that the routine + r3==0 convention are known —
  this is DISTINCT from M10's forged table: it lets the NK set up the descriptor + coherent resume).
- **NOT needed:** host injection (5× falsified) and the deep "make the NK self-register from outside"
  rabbit hole. Cheap parallel route-around: the ROM/OS-version sweep (#5).

**STRATEGIC BOTTOM LINE:** Option 1 (feed the NK) is the indicated path and is **tractable, not an
unbounded rabbit hole.** The decision leans clearly AWAY from abandoning the NanoKernel. Remaining work
is bounded: resolve the one WHY sub-question, then implement the one-shot bootstrap.

## Decision
TBD. Do not commit further multi-milestone effort to Option 1 without (a) the registration-circularity
answer and (b) the ideation triage in hand.
</content>
