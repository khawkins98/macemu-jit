# M17 — Host-Owned NK Interrupt-Handler Stub: Design Spec

> **STATUS: DRAFT — pre-red-team (2026-06-14).** Not yet reviewed; do NOT execute. This spec
> opens the surviving NewWorld-9.x interrupt path documented in `M16-FINDINGS-oracle-forge.md` Q7
> ("host-owned NK-EXT-handler PPC stub") now that 9.2 NewWorld is a **hard requirement**.
> A pre-implementation red-team round follows this draft; findings fold as rev-2 amendments.

**Status:** rev 1 (DRAFT) · 2026-06-14
**Parent strategy:** `docs/planning/MACHINE-LAYER-PLAN.md`
**Plan:** `docs/superpowers/plans/2026-06-14-m17-host-irq-stub.md`
**Predecessors (the banked RE this builds on):**
- `docs/planning/M16-FINDINGS-oracle-forge.md` Q1–Q7 — the CGRP descriptor RE + the Q7 NO-GO
  verdict that names the host-owned-stub route as the re-entry point.
- `docs/planning/M15-FINDINGS-consumption-recon.md` "Verdict (Task 4)" — FORGE-verified; the NK
  routing structs stay frozen-zero (obs=1e9); **CGRP is the first of N** (load-bearing for the DoD).
- `docs/planning/M14-FINDINGS-cuda-delivery.md` §7 — DEC does NOT signal the DR; the
  IM-init-runs-downstream-of-Cuda chicken-and-egg; the M10 `0xDEADBEEF` DR-reentry crash class.

---

## §1 — Problem & Definition of Done

### Problem

NewWorld 9.x interrupt routing dead-ends because the guest Interrupt Manager (IM) init — which
builds the NK "CGRP" interrupt-group handler table — is sequenced downstream of Cuda init and never
runs (M14/M15 chicken-and-egg). The EXT edge is delivered **correctly** to the published NK EXT
entry `0x50314880` (M15 settled; our delivery side is right). The dispatcher reads the CGRP
descriptor at `*(KDP-0x338) = [0x68ffdcc8] = 0x68ffc1c0` and dead-ends: the gate `[CGRP+0x20]=1`
(needs ≥2), and even past it the service routine `0x503148e0` self-guards on the **empty** handler
table (`[CGRP+0x38/+0x3c/+0x44]=0`), so it `beqlr`s without dispatching. M16 proved the table
cannot be cheaply seeded: the handler PC the entry must hold is **ROM-absent** (`0x5000ec50` has
0 word-refs in the 4 MB ROM; `"CGRP"` tag 0× → runtime/disk-built), so neither static RE of our
ROM nor QEMU-as-literal can produce it.

### The surviving path (this milestone)

Instead of finding the guest's handler PC, **synthesize a HOST-OWNED stub** that (a) manufactures
the pending-interrupt state the 68k level-1 handler expects and (b) performs the **legitimate
PPC→68k cross** so the 68k emulator (DR) dispatches the level-1 autovector to `0x5000ec50`,
wiring that stub in as the interrupt handler. The cross is NOT a wild jump: the codebase already
contains the sanctioned mechanism — `HandleInterrupt()` MODE_EMUL_OP
(`sheepshaver_glue.cpp:3508ff`) builds a 68k proc
(`move.w #0,-(sp); pea @1; move sr,-(sp); move.l $64,a0; jmp (a0)`) and runs it via `Execute68k()`,
which reads the emulator pair `[KDP+0x1074]`/`[KDP+0x1078]` into gpr(29)/gpr(30). That arm is
**fenced on newworld** (`if (MachineProfileIsNewWorld() && ExcIrqConsumeEnabled()) break;`). The
68k handler itself is confirmed by disasm: `0x5000ec50 = jmp $5000ef20` (the via_int dispatcher,
which reads the NK PIC descriptor `$68ffefd0` at `0x5000ec86` and dispatches level d0=2 through the
`$1d4.w` vector at `0x5000ef28..ef3c`). So the sanctioned cross + the manufactured-state target are
both pinned; what is unproven is the *exact state* a host stub must manufacture and the *injection
point* that makes the cross legitimate from inside the NK EXT regime.

### What this milestone tests (one hypothesis)

That a host-owned stub — manufacturing the 68k autovector / pending-source state and performing the
sanctioned PPC→68k cross — wired in as the NK EXT/CGRP interrupt handler, makes the 68k DR dispatch
the level-1 interrupt to `0x5000ec50`, **advancing the boot to a new frontier**, without re-opening
the M10 DR-reentry `0xDEADBEEF` crash class.

### Definition of Done (any ONE is a valid outcome — partial findings beat stalling)

> **RED-TEAM-MANDATED FRAMING (carried from the M16 round): SUCCESS IS NOT "REACH FINDER".**
> Per M15, CGRP is the **first of N** frozen structs. The DoD-1 success criterion is explicitly:
> *the interrupt now dispatches into the 68k world (ring-confirmed reach of `0x5000ec50`) AND the
> boot advances to a NEW frozen struct / new park / new wall, which is captured as the next
> milestone's frontier.* Advancing one wall is the win; reaching Finder is **out of scope** and
> must not be a gate.

1. **Stub dispatches + boot advances (best case).** The host-owned stub crosses to the 68k world;
   ring confirms `0x5000ec50` is reached (previously 0/510) with no SIGSEGV/`0xDEADBEEF`; the boot
   advances past the prior park; the **new** frontier (next frozen struct / wall) is captured.
2. **Stub attempted, cross/state wrong (diagnostic win).** The stub is wired and fires, the cross
   fails or the 68k handler no-ops/faults, and we have ring/probe evidence of *which* manufactured
   field or cross-precondition was wrong (autovector $64 target, SR/IPL, the PIC pending bits the
   via_int chain reads, the MSR/SRR1 regime, or the emulator-pair preconditions) — handed forward.
3. **Cross is not legitimizable within budget (viability fork / NO-GO).** Task 0 proves the host
   stub cannot perform a sanctioned cross from the NK EXT regime within budget (e.g. the regime is
   structurally MODE that `Execute68k` cannot be safely entered from, or manufacturing the pending
   state requires NK surfaces only kernel-init builds), OR the M10 crash class re-opens
   irreducibly. Park with the infeasibility documented; the residual artifact is the pinned
   precondition list.

In all three, the standing gate (`make test-jit` score=100) stays green and paravirtual `make e2e`
is unaffected — **every new line is behind an env gate AND `MachineProfileIsNewWorld()`**.

---

## §2 — Scope

**IN:**
- Task 0 (BINDING recon): pin the five load-bearing unknowns (§3 / the plan's blocking-answer
  table) — the 68k-autovector manufacture state, the sanctioned PPC→68k cross from the NK EXT
  regime, the injection point, the safe scratch region, and the per-wall progress metric — with a
  GO/NO-GO viability fork.
- A single env-gated host-owned stub (`SS_M17_STUB=1` + `MachineProfileIsNewWorld()`): a host
  trampoline reachable from the NK EXT dispatch that manufactures the pending-interrupt state and
  performs the sanctioned cross (the proven `Execute68k`-proc-template mechanism, or its
  PPC-stub-via-EMUL_OP equivalent — Task 0 chooses). All bases (CGRP, KDP, emulator pair, hnfo PIC
  descriptor) **resolved live, never hardcoded**.
- Falsifiable, ring-confirmed acceptance: `0x5000ec50` reached, no SIGSEGV/`0xDEADBEEF`, and a
  captured new-frontier delta. **Iterative wall-by-wall** (§3), not a single all-or-nothing gate.

**OUT:**
- Reaching Finder (CGRP is first-of-N — see DoD framing).
- Any non-gated change to the delivery path (M15 proved delivery is already correct).
- Pursuing the IM-init root cause (why it runs downstream of Cuda init) — separate, larger effort.
- Hardcoding the CGRP/KDP/emulator-pair/PIC-descriptor base or any literal QEMU address.
- Un-fencing the paravirtual `HandleInterrupt` MODE_68K CR-injection (M3a F3/M2 — corrupts live
  guest CR). The newworld fence stays; M17 adds a NEW gated path, it does not re-open the old one.
- Performance work; anything touching the 8.6–9.0.4 booting configs.

---

## §3 — Method (the work, in order)

1. **Cross-mechanism RE — Task 0 (primary, static + targeted boots).** Pin, from
   `sheepshaver_glue.cpp` + `rom901.bin` + bounded probe boots:
   (a) the exact register/lowmem state the 68k autovector $64 → `0x5000ec50` → via_int chain reads
   to do useful work (the PIC descriptor `$68ffefd0` fields `+0x28` pending / `+0x14` source table,
   the `$1d4.w` vector, SR/IPL, lowmem `$64`); (b) the precise mode/MSR/run-mode regime at the NK
   EXT dispatch (`XLM_RUN_MODE`, `XLM_IRQ_NEST`, SRR1/MSR at `0x50314880`) and whether the
   sanctioned `Execute68k` cross can be legally entered from it; (c) the injection point trade-offs
   (§ below); (d) a safe scratch region; (e) the per-wall ring-walk progress metric.
2. **QEMU behavioral oracle (format/semantics ONLY, tiebreaker) — Task 0.** When a sub-question is
   "what does a working interrupt dispatch *do* here?" run the QEMU 9.2.1/mac99 rig
   (`tools/qemu-rig.sh`) and read the **behavior/format** of a populated dispatch (e.g. the
   autovector/IPL handshake shape, which lowmem the via_int chain consults). **Never** cite QEMU
   MMIO addresses or literal values (caveat: QEMU MacIO `0x80000000` ≠ ours `0xF3000000`; Cuda
   model differs). Tag `[QEMU-BEHAVIORAL]`.
3. **Stub synthesis + cross — Task 1/2 (GO path).** Behind `SS_M17_STUB=1`, wire the host stub at
   the Task-0 injection point; manufacture the pinned pending state; perform the sanctioned cross.
4. **Iterative wall-by-wall acceptance — Task 2/3.** Ring-confirm `0x5000ec50` reach + crash-
   absence; **capture the new frontier**; if a manufactured field is wrong, single-field bisect
   (one-iteration rule). Each advanced wall is recorded with the ring-walk progress metric; the
   milestone closes when the first wall is crossed (DoD-1) OR the precondition is isolated (DoD-2)
   OR proven non-legitimizable (DoD-3).

### Injection-point options (Task 0 must choose, with the trade-off written)

| Option | Mechanism | Trade-off |
|---|---|---|
| **(a) CGRP entry SRR0 → host stub** | Synthesize a minimal CGRP table whose `[entry+0]` SRR0 points at a host-owned PPC stub (in scratch), reusing the existing `0x503148e0` dispatcher + its self-guard. Set `[CGRP+0x20]≥2`, guard/base/count. | Minimal-surface; rides the NK's own self-guarding dispatcher (M16 Q6: empty-table case is crash-safe). BUT still writes NK structures (R: scratch ownership, M10 class on a *populated* table) and the stub must be valid PPC reachable by `rfi`. |
| **(b) Hook EXT entry `0x50314880` earlier (host-side)** | In our EXT delivery path (`sheepshaver_glue.cpp:1254` `ExcEnter(...,EXC_EXTERNAL,...)`), when newworld + gated, route to a host trampoline that does the cross instead of (or before) entering the NK dispatcher. | Most robust — no guest-memory forge, no NK-struct write, no CGRP dependency, smallest M10 exposure. BUT bypasses the NK's own dispatch (less "real"); must ensure the NK EXT bookkeeping (EOI/level) is still satisfied or the edge re-fires. |
| **(c) Replace CGRP descriptor wholesale** | Forge a complete host-owned CGRP descriptor + handler table + per-source stacks. | Highest fidelity to the guest model; highest crash exposure (full M10 class) + heaviest scratch budget. Likely OUT unless (a)/(b) fail. |

> **Spec leaning (for the red team to challenge):** option **(b)** is the lowest-risk first cut —
> it performs the sanctioned cross host-side with no NK-struct forge, directly testing the central
> hypothesis (does crossing to `0x5000ec50` advance the boot?) before committing to the heavier
> guest-memory synthesis of (a)/(c). If (b) advances the wall, (a) can follow as the "more real"
> implementation. Task 0 must confirm (b) can satisfy NK EXT EOI/level bookkeeping so the edge
> does not livelock.

**Budget (caps, not targets):** ≤~8 slot boots + ≤~3 QEMU boots (behavioral tiebreaker only).
Stop-when: first wall crossed (DoD-1), OR the wrong precondition isolated (DoD-2), OR the cross is
proven non-legitimizable within budget (DoD-3).

---

## §4 — Key risks (carried into the plan's Task 0)

- **R1 (foundational): the M10 DR-reentry `0xDEADBEEF` crash class re-opens.** A populated handler
  table / an unsanctioned cross hit ~1/3 of boots in M10 (the class M13 reverted M10 to escape).
  Mitigate: prefer the host-side hook (option b) that uses the *proven* `Execute68k` cross with a
  valid stack and the emulator pair; gate everything; revert-on-red; treat any `0xDEADBEEF` as an
  immediate DoD-2/3 stop, not a retry.
- **R2: first-of-N iteration uncertainty.** Per M15, crossing CGRP buys "advance to the next wall,"
  not Finder. The DoD already encodes this; the risk is *scope creep* into chasing wall N+1.
  Mitigate: the milestone closes at the FIRST crossed wall with the new frontier captured; wall N+1
  is the next milestone.
- **R3: scratch ownership.** Any host-owned PPC stub / table / stack must live in NK-owned memory
  the NK will not reclaim or overwrite. The M16 candidate `0x68ff5000` is **provably live**
  (occupancy map `M6A-ONGOING-ENTRY-DESIGN.md:690` — ROM machine-detect overwrites it each cold
  cycle; the "free gap" claim was falsified once). Safe gaps exist (`0x68FF6084..0x68FF7000`) but
  require an occupancy-map extension + a watchpoint-quiescence gate. Option (b) sidesteps most of
  this (host-side, minimal/no guest blob). Task 0 must pin the region with the occupancy map.
- **R4: MSR/SRR1 regime for the cross.** The cross must enter the 68k world with a sane MSR and a
  valid 68k stack; `Execute68k` requires EMUL_OP mode and the emulator pair `[KDP+0x1074/0x1078]`
  (M14: a host `Execute68k` without that pair is a wild jump). Task 0 must pin the run-mode/MSR at
  the EXT dispatch and confirm the cross is enterable (or what to stage so it is).
- **R5: wild-jump risk.** Any cross that is not the sanctioned `Execute68k`/emulator-pair path is a
  wild jump (M14). The stub MUST use the sanctioned mechanism; a PPC-stub-via-`rfi` that jumps
  directly into 68k code is forbidden. Task 0 records the exact sanctioned-cross recipe used.
- **R6: NK EXT bookkeeping (option-b specific).** Bypassing the NK dispatcher may leave the EXT
  level/EOI unsatisfied → the edge re-fires (runaway). The existing runaway guard
  (`EXC_EXT_RUNAWAY_N`, glue:252/1313) bounds it; Task 0 must confirm the host hook satisfies or
  defers the EOI so the boot does not livelock.
