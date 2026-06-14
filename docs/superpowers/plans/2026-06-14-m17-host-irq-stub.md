# M17 — Host-Owned NK Interrupt-Handler Stub: Implementation Plan

> **STATUS: DRAFT — pre-red-team (2026-06-14). DO NOT EXECUTE.** A pre-implementation red-team
> round follows this draft; findings fold as rev-2 BINDING amendments. 9.2 NewWorld is now a HARD
> requirement, re-opening the surviving path documented in `M16-FINDINGS-oracle-forge.md` Q7.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking. Tasks 1–3 edit the SAME files
> (`sheepshaver_glue.cpp`, possibly `rom_patches.cpp`); NOTHING after Task 0 parallelizes.

**Goal:** Make the NewWorld 9.x EXT edge — delivered correctly to `0x50314880` but dead-ending in
the empty CGRP table (M16) — dispatch into the 68k world by **synthesizing a host-owned stub** that
manufactures the pending-interrupt state and performs the **sanctioned** PPC→68k cross so the 68k
DR dispatches the level-1 autovector to `0x5000ec50`, advancing the boot to a NEW frontier
(success = advance one wall + capture the next, NOT reach Finder — CGRP is first-of-N per M15),
without re-opening the M10 DR-reentry `0xDEADBEEF` crash class.

**Architecture:** Front-loaded by a BINDING Task-0 recon that can FORK the milestone (the
foundational risk: can a host stub perform a *legitimate* cross from the NK EXT regime, and what
exact state must it manufacture?). The sanctioned cross already exists in-tree
(`HandleInterrupt()` MODE_EMUL_OP proc-template + `Execute68k`, fenced on newworld). If Task 0
returns GO, a single env-gated host path (`SS_M17_STUB=1` + `MachineProfileIsNewWorld()`) at the
Task-0-chosen injection point (spec leans option (b): host-side hook at the EXT delivery site)
manufactures the pinned pending state and crosses. Iterative wall-by-wall acceptance with a
ring-walk progress metric; flip-last / revert-on-red; the service routine / sanctioned cross fail
clean where possible.

**Tech Stack:** SheepShaver aarch64 JIT; slot boot protocol (`ss-slot-boot.sh`); QEMU differential
rig (behavioral oracle only); `tools/ring-walk.py` (repo-root, NOT `SheepShaver/tools/`); capstone
(PPC BE) + `tools/m68k-dis.py` on `/Users/Shared/macemu/dumps/rom901.bin`; diagnostics
(`SS_SEED_MEM`, `SS_NW_PIC`, `SS_NW_IRQ_CONSUME`, `SS_DR_R24_RING`, `SS_JIT_TRACE_RING`,
`SS_JIT_WATCH_ADDR`, `SS_PROBE_PC`, `SS_PROBE_68K`).

**Spec:** `docs/superpowers/specs/2026-06-14-m17-host-irq-stub-design.md`
**Parent strategy:** `docs/planning/MACHINE-LAYER-PLAN.md`.
**Predecessors:** `M16-FINDINGS-oracle-forge.md` Q1–Q7 · `M15-FINDINGS-consumption-recon.md`
"Verdict (Task 4)" · `M14-FINDINGS-cuda-delivery.md` §7.

**Standing rules:** branch `macos-arm64`; never push unprompted; slot boots only (never global
pkill); reap strays with `SheepShaver/tools/ss-reap.sh`; `-F-` heredoc commits (no backticks);
explicit-path staging (never `git add -A`); struct fields appended LAST; budgets are caps,
partial-findings-beat-stalling; one-iteration rule (falsified contract → dated addendum → ONE
re-pin → resume; second falsification of the same contract → stop, re-scope).

---

## Authoritative inputs

| Doc / site | What it fixes |
|---|---|
| `docs/planning/M16-FINDINGS-oracle-forge.md` Q1–Q7 | The CGRP descriptor RE (entry layout `[entry+0]`=SRR0, `[entry+4]`=r2/TOC, SRR1 from r19; `[CGRP+0x3c]` ptr array index source#·4; `[CGRP+0x40]` parallel stack-ptr array indexed differently; gate `[CGRP+0x20]≥2`); the dispatcher `0x503148e0` self-guard; Q7's host-stub re-entry note + the NO-GO reasons (ROM-absent handler PC, scratch live, M10 class). |
| `docs/planning/M15-FINDINGS-consumption-recon.md` "Verdict (Task 4)" | FORGE-verified; structs frozen-zero (obs=1e9); **CGRP is the first of N** (the DoD-1 "advance one wall, not Finder" framing). The waypoint table (EXT delivered `0x50314880`; `0x5000ec50` 0/510). |
| `docs/planning/M14-FINDINGS-cuda-delivery.md` §7 | DEC does NOT signal the DR; the IM-init-downstream-of-Cuda chicken-and-egg; **the M10 `0xDEADBEEF` DR-reentry crash class**; the emulator pair `[KDP+0x1074]/[KDP+0x1078]` "without it any host `Execute68k` is a wild jump". |
| `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` `HandleInterrupt()` (:3435–3560), `execute_68k()` (:1461–1535) | **THE sanctioned PPC→68k cross.** MODE_EMUL_OP arm (:3508ff) builds proc `move.w #0,-(sp); pea @1; move sr,-(sp); move.l $64,a0; jmp (a0)` and runs `Execute68k` — fenced on newworld at :3519 (`if (MachineProfileIsNewWorld() && ExcIrqConsumeEnabled()) break;`). `execute_68k` sets gpr(29)=`[KDP+0x1074]` (opcode table), gpr(30)=`[KDP+0x1078]` (emulator), gpr(24)=entry, gpr(25)=`XLM_68K_R25` (SR MSB), requires `XLM_RUN_MODE==MODE_EMUL_OP`. |
| `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` EXT delivery (:1180, :1254 `ExcEnter(...,EXC_EXTERNAL,&g_exc_entry_table)`, :1269–1317, runaway guard :252/:1313) + `g_exc_entry_table` (:141–177) | The injection site for option (b): where EXT is delivered to `external_entry=0x50314880`. `NW_EXTERNAL_ENTRY_DEFAULT=0x50314880`. The `EXC_EXT_RUNAWAY_N` guard (R6 mitigation). |
| `rom901.bin` 68k handler (verified this plan) | `0x5000ec50 = jmp $5000ef20` (via_int); `0x5000ef20`: `movem; pea $5000ee58; movea.l $1d4.w,a1; moveq #2,d0; movea.w $5000ef40(pc,d0.w),a0; movea.l (a0),a0; jmp (a0)`; `0x5000ec86 movea.l $68ffefd0,a2` (reads the NK PIC descriptor). The autovector $64 → ec50 → via_int chain reads the PIC descriptor + the `$1d4.w` / `$2b6` lowmem vectors. |
| `docs/archive/2026-06/machine/M6A-ONGOING-ENTRY-DESIGN.md` (sub-KDP occupancy map, :683–710) | Authoritative scratch map. `0x68FF5000..0x68FF5800` = ROM machine-detect scratch (LIVE, overwritten each cold cycle — NOT safe). Free gaps: `0x68FF4120..0x68FF4DF0`, `0x68FF4EBC..0x68FF4F00`, **`0x68FF6084..0x68FF7000`**. `0x68FF7000+` = NK pool territory. |
| `docs/AGENT-CONTEXT.md` Constants / Instruments / gate tiers | KDP=0x68ffe000; NK PIC descriptor=0x68ffefd0; XLM_RUN_MODE=0x2810, XLM_IRQ_NEST=0x2818; emulator pair `[KDP+0x1074]=0x50480000`/`[KDP+0x1078]=0x50460000`; DR reg map r24=68k PC; NATMEM_OFFSET 0x400000000000; probe/ring caveats; gate tiers; slot protocol. |

## Codebase facts (carried; implementers re-verify sites before editing)

- **The sanctioned cross is already in-tree and fenced.** `HandleInterrupt` MODE_EMUL_OP
  (glue:3508) is the proven autovector-$64 cross; it is fenced on newworld+consume (glue:3519).
  M17 does NOT un-fence the old paravirtual path — it adds a NEW gated path (`SS_M17_STUB`).
- **`Execute68k` preconditions (glue:1469, :1504–1505):** must be called from `MODE_EMUL_OP`;
  reads emulator pair from `[KDP+0x1074]/[0x1078]`; without them = wild jump (M14). Any M17 cross
  must satisfy these live (resolve, never hardcode).
- **EXT is delivered to `0x50314880` via `ExcEnter(EXC_EXTERNAL)` (glue:1254)** — the natural
  host-side injection point for option (b). The EXT delivery already has a runaway guard (:1313).
- **The 68k L1 handler reads NK state at runtime:** `0x5000ec50→ef20` reads `$68ffefd0` (PIC
  descriptor: `+0x28` pending bits, `+0x14` source-table ptr per AGENT-CONTEXT) and the `$1d4.w` /
  `([$2b6],$31c)` lowmem vectors. The host stub must manufacture whatever of these the chain
  dereferences, OR the chain must already find them populated (Task-0 Q3).
- **Scratch:** `0x68ff5000` is LIVE (do NOT use). If a guest-resident blob/stack is needed, use a
  verified free gap (`0x68FF6084..0x68FF7000`) AFTER extending the occupancy map + a
  watchpoint-quiescence check. Option (b) likely needs no guest blob.
- **Gate-helper idiom:** add `ExcM17StubEnabled()` next to `ExcIrqConsumeEnabled()`
  (getenv("SS_M17_STUB"), default OFF). All new emission newworld-gated.
- **Canonical gate set:** `make -C SheepShaver build-ss`; `SS_HARNESS_BATCH=1 make -C SheepShaver
  test-jit` AND plain `make -C SheepShaver test-jit` (score=100); `make -C SheepShaver/src/machine
  test`; `make -C SheepShaver e2e-test`; paravirtual `make -C SheepShaver e2e` byte-identical (all
  new lines inside `MachineProfileIsNewWorld()`+gate → structural-inertness substitution allowed,
  state which in the commit). NewWorld observe line: `make nw-northstar` → `[NW-PROG verdict]`.
- **Standing rules:** per-task commits; struct fields appended LAST; clean PPC rebuild after
  glue/registers/rom_patches header changes; probe-PC limit 8/run; `SS_JIT_WATCH_ADDR` is hex +
  needs `SS_JIT_TRACE_RING=1`; ring read via `tools/ring-walk.py` only.

---

## Tasks

### Task 0: BINDING recon — the sanctioned cross from the NK EXT regime + the manufactured state (can fork the milestone)

**Files:** Modify (append): create `docs/planning/M17-FINDINGS-host-irq-stub.md` with a Task-0
blocking-answer table. (Q1–Q7 in M16-FINDINGS stay as predecessor evidence.)

Task 0 exists because the premise is unproven: that a host-owned stub can perform a **legitimate**
PPC→68k cross from the NK EXT regime AND manufacture exactly the state the 68k via_int chain needs.
Five answers are BLOCKING. **Primary tools: static RE of `sheepshaver_glue.cpp` + `rom901.bin`,
bounded probe boots; QEMU is a BEHAVIORAL tiebreaker only (never addresses).**

> **Budget honesty:** ≤8 slot boots total (≤60s each, ≤2 per question before residue status is
> decided, co-scheduled — 8 probe PCs/run); ≤3 QEMU boots (behavioral only). Static RE windows are
> bounded (≤2 call levels / ≤12 functions per chain); unbounded disassembly → residue + fallback.
> Evidence tags `[STATIC]/[PROBE✓]/[QEMU-BEHAVIORAL]/[RAW-ROM]/[PATCH]`.

- [ ] **Q0-A (BLOCKING — Task 1): the 68k-autovector manufacture state.** Trace the
  `$64 → 0x5000ec50 → 0x5000ef20 (via_int)` chain (use `tools/m68k-dis.py` on `rom901.bin` +
  `SS_PROBE_68K`): which lowmem / PIC-descriptor fields the chain dereferences before doing useful
  work — `$68ffefd0` (`+0x28` pending bits, `+0x14` source table), the `$1d4.w` vector, the
  `([$2b6],$31c)` counter, lowmem `$64` (autovector). **Deliverable: the exact state a host stub
  must manufacture (address → value/class) so the via_int chain dispatches a level-1 interrupt
  instead of no-op'ing/faulting.** Note which fields our trampoline already asserts
  (`[NW-TRAMP]`: `$68ffefd0=0x68ff4f00`, the level byte `[0x3f3f]=1`) vs. which are still zero
  (M15: hnfo+0x28 pending = 0).
- [ ] **Q0-B (BLOCKING — Task 1): the sanctioned PPC→68k cross from the NK EXT regime.** Pin the
  run-mode/MSR regime at the EXT dispatch (`SS_PROBE_PC=0x50314880` for SRR1/MSR; probe
  `XLM_RUN_MODE`=`[0x2810]`, `XLM_IRQ_NEST`=`[0x2818]`, `[KDP+0x1074]/[0x1078]` live). **Deliverable:
  can the proven `Execute68k`-proc-template cross (glue:3508) be legally entered from this regime
  (it requires `MODE_EMUL_OP` + the emulator pair), and if not, what minimal staging makes it
  legal?** Record the exact sanctioned-cross recipe M17 will use (proc template + `Execute68k`, or
  a PPC-stub-via-EMUL_OP that re-enters this same host path). **Forbid any direct `rfi`-into-68k
  wild jump (R5).**
- [ ] **Q0-C (BLOCKING — Task 1): the injection point.** Decide among spec §3 options (a)
  CGRP-entry-SRR0→host-stub, (b) host-side hook at the EXT delivery site (glue:1254), (c) full CGRP
  replacement. Write the trade-off. **Confirm the chosen point: for (b), that the host hook can
  satisfy/defer the NK EXT EOI/level so the edge does not livelock (R6 — the `EXC_EXT_RUNAWAY_N`
  guard bounds it); for (a)/(c), the scratch + M10 exposure.** Deliverable: chosen option + the
  precise site + the bookkeeping plan.
- [ ] **Q0-D (BLOCKING — Task 1): the safe scratch region (only if the chosen option needs a
  guest-resident blob/stack/table).** Extend the occupancy map; pick from a verified free gap
  (`0x68FF6084..0x68FF7000`); confirm not MMIO-guarded and not NK-touched between trampoline-end
  and the EXT edge (watch across a baseline boot). `0x68ff5000` is LIVE — forbidden. If option (b)
  needs no guest blob, record "N/A — host-side cross uses the existing `Execute68k` stack frame".
- [ ] **Q0-E (BLOCKING — acceptance): the per-wall progress metric.** Define the ring-walk
  progress signal the acceptance gates use: baseline = `tools/ring-walk.py --find-pc 0x5000ec50` = 0
  (the 0/510 M15 result); success = >0 + the NEW park/frozen-struct PC captured. Pin the exact
  `ring-walk.py` invocations + the `[NW-PROG]` markers that classify "advanced one wall".
- [ ] **Q0-F (tiebreaker, behavioral only): QEMU 9.2.1 oracle.** Only if Q0-A/Q0-B leave the
  autovector/IPL handshake or the manufactured-state shape ambiguous: `bash
  SheepShaver/tools/qemu-rig.sh --timeout 60`; read the **behavior/format** of a working
  level-1 dispatch (what lowmem the via_int chain consults; the autovector/IPL shape). **Never
  literal addresses or values** (QEMU MacIO 0x80000000 ≠ ours; Cuda model differs). Tag
  `[QEMU-BEHAVIORAL]`.
- [ ] **Gate (the blocking-answer table) + GO/NO-GO fork.** Append to `M17-FINDINGS` the filled
  table; ALL blocking answers (Q0-A..E) pinned before ANY implementation starts. **Viability fork:**
  cross legitimizable + state manufacturable + injection point chosen → GO (Task 1); the cross
  cannot be made legitimate within budget OR manufacturing the state requires NK surfaces only
  kernel-init builds OR the M10 class is irreducible → write the DoD-3 NO-GO and STOP.
  ```bash
  git add docs/planning/M17-FINDINGS-host-irq-stub.md
  git commit -F- <<'EOF'
  docs(m17): Task-0 recon — sanctioned PPC->68k cross + manufactured-state + injection point
  EOF
  ```

**Acceptance:** filled blocking-answer table (Q0-A manufacture state, Q0-B sanctioned cross recipe,
Q0-C injection point + bookkeeping, Q0-D scratch or N/A, Q0-E progress metric) with an explicit
GO/NO-GO. No stub code written yet.

---

### Task 1: Implement the env-gated host-owned stub + cross (GO path only)

**Files:** Modify: `sheepshaver_glue.cpp` (the `ExcM17StubEnabled()` helper + the gated stub at the
Task-0 injection point); possibly `rom_patches.cpp` (only if Q0-D needs a guest-resident blob);
`docs/planning/M17-FINDINGS-host-irq-stub.md` (impl notes).

- [ ] **Step 1: Add the `SS_M17_STUB` gate + the stub.** Behind `ExcM17StubEnabled() &&
  MachineProfileIsNewWorld()`, at the Task-0-chosen injection point: (1) manufacture the Q0-A
  pending state (resolve all bases live — PIC descriptor from `[0x68ffefd0]`, KDP, emulator pair);
  (2) perform the Q0-B sanctioned cross (the `Execute68k` proc-template path, or its
  EMUL_OP-re-entry equivalent) — NEVER a direct wild jump; (3) for option (b), satisfy/defer the NK
  EXT EOI/level per Q0-C. Record exact addresses/values written.
- [ ] **Step 2: Per-commit gate (inner) + gated-off A/B.**
  ```bash
  cd SheepShaver && make build-ss && SS_HARNESS_BATCH=1 make test-jit   # expect score=100
  make -C src/machine test
  ```
  Show a gated-off (`SS_M17_STUB` unset) byte-identical A/B boot (every new line inside the gate →
  structural-inertness substitution for paravirtual e2e; state it in the commit).
  ```bash
  git add SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp docs/planning/M17-FINDINGS-host-irq-stub.md
  git commit -F- <<'EOF'
  feat(m17): SS_M17_STUB gated host-owned NK interrupt stub + sanctioned 68k cross
  EOF
  ```

**Acceptance:** `make test-jit` score=100; machine suite green; gated-off A/B byte-identical; stub
applies only under `SS_M17_STUB=1` + newworld; all bases resolved live (never hardcoded); the cross
uses the sanctioned `Execute68k`/emulator-pair mechanism (no wild jump).

---

### Task 2: Iterative wall-by-wall acceptance — falsifiable, ring-confirmed

**Files:** Modify: `docs/planning/M17-FINDINGS-host-irq-stub.md` (smoke + frontier evidence).

- [ ] **Step 1: Run the stub smoke boot.**
  ```bash
  SheepShaver/tools/ss-slot-boot.sh --label m17-stub-smoke --timeout 45 \
    --env 'SS_M17_STUB=1 SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_DR_R24_RING=1'
  ```
  (`--env` splits on WHITESPACE, not `;`. Do NOT use `SS_JIT_WATCH_DUMPS=0` — it suppresses the
  ring dump.)
- [ ] **Step 2: Ring-confirm the cross (Q0-E metric).**
  ```bash
  python3 tools/ring-walk.py <RUNDIR> --find-pc 0x5000ec50     # GOAL: now > 0 (was 0/510)
  python3 tools/ring-walk.py <RUNDIR> --r24-flow | tail -80    # the 68k path after the cross
  grep -aE 'EXT delivered|SIGSEGV|SIGTRAP|DEADBEEF|\[ALARM\]|\[NW-PROG' <RUNDIR>/boot.log
  ```
  Gate: `0x5000ec50` reached (cross fired) AND no `0xDEADBEEF`/SIGSEGV (R1) AND a NEW park/frontier
  PC observed past the prior wall. Run `ring-walk.py` from the **repo root** (never
  `SheepShaver/tools/`).
- [ ] **Step 3: Record verdict (DoD 1 vs 2) + capture the new frontier.** DoD-1 (cross fired +
  `0x5000ec50` reached + boot advanced + no crash) → success; **capture the new frozen-struct /
  park as the next milestone's frontier** (ring tail + `[NW-PROG]` + the new park PC). DoD-2 (cross
  fails / handler no-ops / faults) → record the failure mode: wrong manufactured field (Q0-A),
  cross-precondition unmet (Q0-B — e.g. wrong run-mode/MSR/emulator-pair), or `0xDEADBEEF` (R1 — M10
  class re-opened → immediate stop). Isolate via Task 3.
  ```bash
  git add docs/planning/M17-FINDINGS-host-irq-stub.md
  git commit -F- <<'EOF'
  docs(m17): stub smoke boot — cross-to-0x5000ec50 acceptance + new-frontier capture
  EOF
  ```

**Acceptance:** the cross fires and `0x5000ec50` is ring-reached with no crash and a captured new
frontier (DoD-1), OR the failing precondition/field isolated with ring/probe evidence (DoD-2).

---

### Task 3: Single-field bisect (only if Task 2 = DoD-2) + close-out

**Files:** Modify: `docs/planning/M17-FINDINGS-host-irq-stub.md`, `docs/HANDOFF.md`,
`docs/AGENT-CONTEXT.md`, `docs/planning/ROADMAP.md`, `docs/planning/MACHINE-LAYER-PLAN.md`.

- [ ] **Step 1: Vary one precondition at a time (bounded).** If the smoke failed, flip one
  manufactured field / cross-precondition at a time (Q0-A pending bits vs the `$1d4`/`$2b6` vectors
  vs autovector `$64`; Q0-B run-mode vs MSR vs emulator-pair staging) to find which the via_int
  chain / cross read wrong. One-iteration rule: a falsified Task-0 value gets ONE re-pin from
  RE/oracle, then stop and document. A `0xDEADBEEF` (M10 class) is NOT bisected — it is an
  immediate DoD-3 stop (R1).
- [ ] **Step 2: Standing gate + revert-on-red.**
  ```bash
  cd SheepShaver && make test-jit    # authoritative, score=100
  make nw-northstar                  # observe line — quote [NW-PROG verdict]
  ```
  If no DoD-1/DoD-2 outcome is reachable within budget, leave the gate OFF-green (code in-tree,
  default-off, inert) — never ship a crashing default. The default flip (if DoD-1) is the LAST step
  and is REVERTED in-task on any post-flip gate failure.
- [ ] **Step 3: Close-out — update resume docs.** Update `docs/HANDOFF.md` (headline + resume
  prompt), `docs/AGENT-CONTEXT.md` (current frontier), `docs/planning/ROADMAP.md` (header/status —
  doc-sweep-3 lesson: do NOT skip the ROADMAP header), and the `MACHINE-LAYER-PLAN.md` frontier
  block to M17's outcome (DoD 1/2/3) and the next entry. State the falsification tally line:
  `FALSIFICATIONS: <n> (re-pins <n>); tasks: <which>; tier: <strong|cheap>`. Cross-tracker grep for
  stale current-state claims.
  ```bash
  git add docs/planning/M17-FINDINGS-host-irq-stub.md docs/HANDOFF.md docs/AGENT-CONTEXT.md docs/planning/ROADMAP.md docs/planning/MACHINE-LAYER-PLAN.md
  git commit -F- <<'EOF'
  docs(m17): close-out — host-irq-stub verdict + next-milestone handoff
  EOF
  ```

**Acceptance:** a stated DoD-1/2/3 verdict with cited evidence; `make test-jit` score=100; gate
OFF-green if unproven; HANDOFF + AGENT-CONTEXT + ROADMAP + MACHINE-LAYER-PLAN updated; falsification
tally recorded.

---

## Stop-rule (triggers)

1. **Task 0 viability fork:** if the sanctioned cross cannot be legitimized from the NK EXT regime
   within budget, OR manufacturing the Q0-A state needs NK surfaces only kernel-init builds, OR the
   M10 crash class is irreducible → STOP after the addendum (DoD-3), the pinned precondition list is
   the residual artifact. (The recon is the tripwire — the M16 lesson.)
2. **The tempting wrong fix:** a direct `rfi`-into-68k wild jump (R5), un-fencing the old
   paravirtual MODE_68K CR-injection (spec OUT), or chasing wall N+1 past the first crossed wall
   (R2) — all forbidden; if reached, STOP and re-scope.
3. **M10 `0xDEADBEEF`:** any DR-reentry crash is an immediate DoD-2/3 stop, never a retry (R1).
4. **One-iteration rule:** a falsified contract → dated `M17-FINDINGS` addendum → ONE bounded
   re-pin → resume; a SECOND falsification of the same contract → stop, re-scope, re-plan.
5. **Next surface:** if the stub advances the wall but the boot dies on the NEXT frozen struct,
   that is the next milestone's named frontier — capture and stop (CGRP is first-of-N).

## Self-review record

Spec coverage: all spec §1–§4 mapped — DoD-1/2/3 → Task 2 (1/2) + Task 0 (3 viability fork) +
Task 3 (close-out states outcome); §3 method (cross RE → behavioral oracle → synthesis-smoke →
wall-by-wall bisect) → Task 0 Q0-A..F / Task 2 / Task 3 one-to-one; §4 risks R1(M10)→gating +
sanctioned-cross + immediate-stop, R2(first-of-N)→DoD framing + stop-rule 5, R3(scratch)→Q0-D +
occupancy map, R4(MSR/SRR1)→Q0-B, R5(wild-jump)→Q0-B forbid + stop-rule 2, R6(EOI)→Q0-C +
runaway guard. §2 OUT (no non-gated delivery change; no IM-init root cause; no hardcoded bases; no
un-fencing; not Finder) respected — all behind `SS_M17_STUB` + `MachineProfileIsNewWorld()`.

The architecture deliberately reuses the **proven** in-tree cross (`HandleInterrupt` MODE_EMUL_OP +
`Execute68k`) rather than inventing a new one — the central de-risking move vs M10's blind forge.
The milestone is designed ITERATIVE wall-by-wall with a ring-walk progress metric (Q0-E), NOT a
single all-or-nothing gate to Finder, per M15's first-of-N reality.

**Known tensions flagged FOR the red team:**
1. **Injection point (a) vs (b):** the spec leans option (b) (host-side hook, no NK-struct forge,
   lowest M10 exposure) as the first cut, but (b) bypasses the NK's own dispatch — is that "real
   enough", and does it satisfy the NK EXT EOI/level so the edge doesn't livelock (R6)? Red team to
   adjudicate (b)-first vs (a)-first.
2. **Is the host-side cross "host-side HLE where real guest code should run"?** The stop-rule's
   "tempting wrong fix" trigger names host-side HLE. The defence: the *68k handler* (`0x5000ec50`
   via_int chain) IS real guest code; M17 only manufactures the *cross* the missing IM-init would
   have wired — and uses the already-sanctioned `Execute68k` path the host owns by design. Red team
   to confirm this is not the proscribed HLE.
3. **First-of-N value:** even a perfect DoD-1 buys one wall (M15). Is M17 worth it before a survey
   of how many of the N walls are similarly forge-class? (Counter: 9.2 NW is now a hard requirement;
   the first wall is the only way to learn the second.)
4. **Q0-A manufacturability:** M15 shows hnfo+0x28 (pending bits) is frozen-zero; if the via_int
   chain hard-derefs a source table the host can't legitimately populate, DoD-3 fires at Task 0 —
   the red team should pre-judge whether Q0-A is answerable from static RE + QEMU-behavioral alone.
5. **Run-mode at the EXT dispatch:** `Execute68k` requires `MODE_EMUL_OP`; the NK EXT regime may be
   MODE_68K or MODE_NATIVE. If the cross can't be entered without forcing run-mode (which risks
   desync), Q0-B may return NO-GO. Flagged as the highest-probability Task-0 killer.

## Red-team record

*(empty — a red-team round follows this draft; findings fold as rev-2 markers)*
