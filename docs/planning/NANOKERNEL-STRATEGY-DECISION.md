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

**RESOLVED — CIRCULAR (WHY-trace agent, 2026-06-13, MED-HIGH).** Registration is **NK kernel-call
selector `0x01`** (gateway `0x5031aca0`, selector = `*(r6+0x104)`; dispatcher `0x5031aed0`; handler
`0x5031b250`→`0x5031b290`). Our injection-OFF boot issues **~1 NK kcall total** (the one observed was
raw selector `0x3f`, not 1); the registration handler `0x5031b24c` gets **0 hits** over 45s; no fault.
A *guard* hypothesis would require the client to run → funnel traffic; the funnel is **silent**, so the
kcall-issuing OS bringup phase **is never reached** — the boot is wedged in the early tick-starved 68k
idle spin **upstream** of it. So it is circular, not an upstream-guard. (Sides with the static agent;
reconciles the dynamic agent — QEMU's "early" is wall-clock-early but still causally downstream of the
tick/idle dependency.) Caveat: absence-based (couldn't time a healthy boot's selector-1 via PPC gdb).

**THE HARD CONVERGENCE (all agents together):** (a) it's **circular** — registration needs the boot to
advance past the tick-starved idle spin; (b) the spike proved we **cannot break it by injection** — no
coherent one-shot 68k autovector / `0x5000ee58` / `0x5031b290` call is host-feasible (the whole M13
saga). Therefore the loop can only be broken by the boot **advancing naturally**, which requires the
**real interrupt/timer hardware the NK's early path expects** — i.e. **LLE the machine**, not fake a
tick. NOTE: this means the WHY-trace's suggested "one-shot bootstrap" fix collides with the spike's
"injection infeasible" — reconciled by: the bootstrap must come from a **modeled hardware interrupt
source**, not host injection. Open sub-question (→ borrow-eval): we already model SCC/VIA/Cuda/OpenPIC,
so what is incomplete/unwired such that the early boot never gets its ticks?

**BOOTSTRAP SPIKE RESULT (2026-06-13) — the "one-shot bootstrap" is NOT FEASIBLE; tempers the optimism.**
Both candidates fail on contract analysis:
- **Invoke `0x5031b290` ourselves (A):** the r3==0 "default" path is NOT an "NK-supplies-handler" path.
  The CGRP descriptor table must hold **client-supplied, MMU-validated PPC handler routines** that a
  pre-driver boot has not produced. Issuing the call needs (a) a fabricated NK-call context (r6/KDP/NK
  stack — same incoherence class as the 5 falsified injections) AND (b) a real client handler+table
  that doesn't exist yet. The only host-supplyable table is **forged = exactly M10's falsified path**
  (reproduced as the intermittent ~1/3 SIGTRAP). [contract DISASM✓ + PROBE✓: 0x5031b290 never fires]
- **Drive `0x5000ee58` (B):** DR-recompiler 68k code, coherent only at the DR between-instruction
  boundary — the already-falsified DR-injection class.

**So registration is not a call we can fake** — it intrinsically requires a *client* (driver/OS
component) to supply a real handler. This pushes the WHY toward **circular** (registration is
downstream of driver-load) and means "feed the NK" is NOT "make one call."

**LIVE PRINCIPLED LEAD (from the spike):** the EXT fallback `0x50325f00` runs in a **coherent PPC
supervisor context** (it fires naturally every interrupt). So the real direction is **HLE the
registered handler's DR-signal at that fallback point** — i.e. at 0x50325f00, do what the (missing)
registered handler would: translate the pending interrupt into the DR's autovector trigger (set
`cr2lt` in the DR's resumed context). This is a real effort, NOT a one-shot, and first needs the
still-open RE: **characterize the handler's DR context-signal** (how a registered handler hands the
68k IPL to the DR). Cheap parallel route-around remains the ROM/OS-version sweep (#5).
**NOT needed:** host frame-injection (5× falsified) and forging the CGRP table (= M10, crashes).

**STRATEGIC BOTTOM LINE:** Option 1 (feed the NK) is the indicated path and is **tractable, not an
unbounded rabbit hole.** The decision leans clearly AWAY from abandoning the NanoKernel. Remaining work
is bounded: resolve the one WHY sub-question, then implement the one-shot bootstrap.

## Borrow-vs-rebuild evaluation (2026-06-13, `m13-borrow-eval`)
- **The gap is NOT a missing device model.** We already model SCC/VIA/Cuda/OpenPIC. The blocker is
  (a) OpenPIC→CPU EXT delivery is **gated OFF by default** (only fires under `SS_NW_PIC=1`;
  wiring exists: `main_unix.cpp` OpenPICBindOutput→nw_pic_output_edge→SheepExcExtSetPending),
  (b) **lazy** VIA/Cuda/decrementer assertion, and (c) the **unmodeled** EXT-fallback→DR-autovector
  signal (translate pending IRQ → `cr2lt` at the coherent `0x50325f00`). It's wiring + RE, not silicon.
- **DingusPPC is the WRONG donor:** it boots Mac OS 9 ONLY on the **OldWorld** G3-Beige path (MacIO
  Grand Central/Heathrow, **no OpenPIC**); **NewWorld (our NanoKernel/OpenPIC 9.0.1 path) is not
  supported.** So it doesn't exercise our boot path. (Clean GPLv3 code; our tree is GPLv2-OR-LATER so a
  GPLv3 import is *legal but moot* given the path mismatch. Usable as a behavioral *reference* for
  VIA/Cuda/ESCC timer semantics only — cite SHA, never PR inbound.)
- **QEMU mac99** = the correct NanoKernel-path oracle (real OpenPIC + NewWorld MacIO) — reference-only.
- **Infinite Mac runs 9.x on SheepShaver, not DingusPPC** → SheepShaver is the proven NewWorld-9.x base;
  switching base would regress proven capability.

## DECISION (2026-06-13) — COMPLETE OUR OWN; keep SheepShaver + Apple's NanoKernel

Evidence resolves the fork: **do NOT fork the NK** (Apple's, works), **do NOT switch base** (DingusPPC =
wrong OldWorld path; SheepShaver is the proven NewWorld base), **do NOT borrow device models** (we have
them; the gap is unwired eager delivery + the DR-handoff, not absent silicon). The path is bounded
engineering in our own NK-interaction layer:
1. **Un-gate eager interrupt delivery** (OpenPIC `SS_NW_PIC` toward default-on + eager VIA-T1/T2 +
   decrementer) so a periodic tick reaches `ExcEnter(EXC_EXTERNAL)` before driver-load.
2. **Characterize the registered-handler→DR signal** (how a healthy boot hands the 68k IPL to the DR)
   via the QEMU mac99 oracle.
3. **HLE that signal at the coherent EXT fallback `0x50325f00`** (set the DR's `cr2lt` autovector
   trigger) — the one principled, coherent point, distinct from the 5 falsified injections and from
   M10's crashing forged table.
- **Cheapest validating experiment (no rebuild):** `SS_NW_PIC=1` + eager VIA-timer slot-boot — does
  `0x50325f00` fire repeatedly AND does the idle-spin/`dec_expiries` advance past the wall? Yes →
  the DR-handoff HLE is the bounded finish line. No → circularity is deeper; the ROM/OS-version sweep
  (route-around) rises in priority.

**RECONFIRM BEFORE COMMITTING MILESTONE EFFORT:** the exact stall wall — M13 inventory cites an MMU
wall `~0x50326050`, the 2026-06-13 convergence cites a tick-starved idle spin. Both are "pre-driver-
load," but pin the precise wall first (cheap probe) so the redraft targets the real one.
</content>
