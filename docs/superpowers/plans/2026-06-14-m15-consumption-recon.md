# M15 Consumption-Path Recon Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Determine, with ring/probe evidence, whether the M8 `SS_NW_IRQ_CONSUME` consumption path is one `SC#1=0x0d` divergence short of completing the guest's Interrupt Manager init (→ "real" / option 1) or has deeper gaps (→ "forge" / option 2), and emit a committed forge-vs-real verdict.

**Architecture:** A bounded recon (≤~6 slot boots, read-only / env-gated). Task 0 resolves blocking facts live (notably the dynamically-allocated `hnfo` base — never hardcode `0x68ff4f00`). Three instrumented boots trace the consumption path, characterize the `SC#1=0x0d` divergence, and watch the NK routing structs for any write. A verdict doc applies the binary decision rule and hands off the next milestone.

**Tech Stack:** SheepShaver aarch64 JIT; slot boot protocol (`ss-slot-boot.sh`); diagnostics (`SS_NW_IRQ_CONSUME`, Smoke-H, `SS_DR_R24_RING`, `SS_PROBE_PC`/`SS_PROBE_68K`, `SS_JIT_TRACE_RING`, `SS_JIT_WATCH_ADDR`); `tools/ring-walk.py`, `tools/jit-analyze.py`.

**Spec:** `docs/superpowers/specs/2026-06-14-m15-consumption-recon-design.md`
**Parent strategy:** `docs/planning/MACHINE-LAYER-PLAN.md` (rev 4). **Predecessor:** `docs/planning/M14-FINDINGS-cuda-delivery.md` §4a/§4b/§7.

**Standing rules:** branch `macos-arm64`; never push unprompted; slot boots only (never global pkill); reap strays with `SheepShaver/tools/ss-reap.sh`; `-F-` heredoc commits (no backticks); explicit-path staging (never `git add -A`); budgets are caps, partial-findings-beat-stalling.

---

## File structure

This recon creates/modifies:
- **Create:** `docs/planning/M15-FINDINGS-consumption-recon.md` — the verdict doc (Task 0 scaffold → filled per boot → Task 4 verdict).
- **Modify (only if existing instruments can't pin the stall):** at most one env-gated `SS_M15_*` trace hook in the consumption path; paravirtual-byte-identical, gated-off A/B required. Default expectation: **no code change** — existing instruments suffice.
- **Capture (not committed; referenced by path in the findings doc):** per-boot slot rundirs under `/tmp/ss-slots/`, ring dumps, `ring-walk.py` output.

---

## Task 0: BINDING recon — resolve blocking facts before any watch is set

**Files:**
- Create: `docs/planning/M15-FINDINGS-consumption-recon.md` (scaffold + blocking-answer table)

Task 0 exists because two facts are load-bearing and must be resolved live, not trusted from prior sessions:
1. **`hnfo` base is dynamically allocated** — the `0x68ff4f00` from M14 was a *settled-to* observation, not a constant. Watching the wrong base makes the struct-population evidence (the verdict's pivot) worthless.
2. **`SS_NW_IRQ_CONSUME` consumption path waypoints** — the post latch, staging, drain, slot-4 `twi`, and 68k level-1 handler addresses must be pinned from the M8 plan/source before probes are placed.

- [ ] **Step 1: Extract the consumption-path waypoints from the M8 record**

Read the archived M8 plan and the consumption code:
```bash
sed -n '1,200p' docs/archive/2026-06/superpowers/plans/2026-06-12-slot4-consumption.md
grep -rn "SS_NW_IRQ_CONSUME\|0x68fff070\|slot.4\|slot-4\|leg 7\|staging\|drain" SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp SheepShaver/src/machine/*.cpp 2>/dev/null | head -40
```
Record in the findings doc's blocking-answer table: the post-latch address (expected `0x68fff070`), the slot-4 `twi` PC, the 68k level-1 handler entry, and the env vars that arm the path.

- [ ] **Step 2: Resolve the live `hnfo` base (do NOT hardcode 0x68ff4f00)**

Boot once with the NK PIC descriptor probe to read the live `hnfo` pointer at `0x68ffefd0` (KDP+0xFD0):
```bash
SheepShaver/tools/ss-slot-boot.sh --label m15-hnfo-resolve --timeout 30 \
  --env 'SS_NW_IRQ_CONSUME=1;SS_PROBE_PC=0x50314880:[0x68ffefd0],[r3]'
```
Read the resulting `[PROBE]` lines from the printed `LOG`. The word at `0x68ffefd0` is the live `hnfo` base pointer. Confirm it against `0x68ff4f00` — if it differs, the M15 watch addresses use the **live** value.

- [ ] **Step 3: Record blocking answers + acceptance bounds in the findings doc**

Write the Task-0 blocking-answer table into `M15-FINDINGS-consumption-recon.md`:
- live `hnfo` base = `0x________` (+ `+0x00/+0x14/+0x28` watch targets computed from it)
- `KDP+0x674` = `0x68fff674` (static — KDP base is fixed)
- consumption-path waypoint PCs (from Step 1)
- boot budget: ≤~6 boots; stop-when: stall point pinned AND struct-write question answered.

- [ ] **Step 4: Commit the scaffold**

```bash
git add docs/planning/M15-FINDINGS-consumption-recon.md
git commit -F- <<'EOF'
docs(m15): Task-0 recon scaffold — live hnfo base + consumption waypoints

Resolve hnfo base live (not hardcoded 0x68ff4f00) and pin the
SS_NW_IRQ_CONSUME consumption-path waypoints before any watch is set.
EOF
```

**Acceptance:** findings doc has a filled blocking-answer table with a *live-resolved* `hnfo` base and the consumption-path waypoint PCs. No watch addresses are set anywhere yet from the assumed constant.

---

## Task 1: Boot A — consumption baseline trace (where does the SR interrupt stick?)

**Files:**
- Modify: `docs/planning/M15-FINDINGS-consumption-recon.md` (Boot-A evidence section)

- [ ] **Step 1: Run the consumption baseline boot**

> **Task-0 corrections (binding):** Smoke-H is NOT in the tree (it was an uncommitted M14 hack) — do not reference it. Arm the consumption path with **`SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1`** (PIC required; without it EXT delivers at level 0 = no-op). `SS_PROBE_PC` does NOT fire at vector-entry PCs (`0x50314880`, `0x50314ac0`, `0x50325f00` are reached via vector dispatch, bypassing the block-entry hook) — use the always-on `[EXC]` log lines for delivery evidence and the r24 ring for the 68k flow. `--env` splits on `;`, which collides with multi-PC `SS_PROBE_PC` syntax — use at most ONE probe PC per boot.

```bash
SheepShaver/tools/ss-slot-boot.sh --label m15-bootA-consume --timeout 45 \
  --env 'SS_NW_PIC=1;SS_NW_IRQ_CONSUME=1;SS_DR_R24_RING=1'
```
Note the printed `SLOT/RUNDIR/LOG/DIAG`. The consumption-path waypoints to look for (from Task 0): post latch `0x68fff070`, staging stub `0x5032d280`, drain `0x50324720`, slot-4 `twi` `0x5046e8d0`, NK slot-4 service `0x50314660`, 68k level-1 handler `0x5000ec50` (via_int `0xef2c`).

- [ ] **Step 2: Walk the ring for the consumption round-trip**

```bash
python3 tools/ring-walk.py <RUNDIR> --r24-flow | tail -60
python3 SheepShaver/tools/jit-analyze.py diag <RUNDIR>/jit_diag.log | head -40
```
Look for the durable markers: `[DR68K] first instruction`, `EXT delivered #1`, `host-irq: edges=N consumed=M`, and whether the slot-4 `twi` waypoint (Task 0) appears in the r24 flow.

- [ ] **Step 3: Pin the stall point**

In the findings doc, record the *last* consumption-path waypoint reached and the *first* not reached, with record numbers. Classify by durable markers, never by crash presence (the all-on boot is ~50/50 non-deterministic; a bare SIGSEGV is not a regression). If `SS_PROBE_68K`-style "never fires" is involved, it is UNPROVEN until ring-confirmed — cross-check against the r24 ring.

- [ ] **Step 4: Commit the Boot-A evidence**

```bash
git add docs/planning/M15-FINDINGS-consumption-recon.md
git commit -F- <<'EOF'
docs(m15): Boot A — consumption-path stall point (ring-confirmed)
EOF
```

**Acceptance:** the consumption-path stall point is pinned with ring record numbers and durable-marker evidence (not probe-never-fires alone).

---

## Task 2: Boot B — characterize the SC#1=0x0d divergence

**Files:**
- Modify: `docs/planning/M15-FINDINGS-consumption-recon.md` (Boot-B evidence section)

The M8 default flip was refused over a deterministic `SC#1=0x0d` divergence. **Task-1 confound (binding):** the findings doc shows `SC#1=0x0d` only appears under `SS_NW_IRQ_CONSUME=1` **without** `SS_NW_PIC` — but consumption is structurally unreachable without PIC (EXT delivers at level 0). With `SS_NW_PIC=1` (the real consumption regime) the first SC is `r0=0x3f`. So this task's actual question is sharper: **under the real consumption regime (PIC on), is the Task-1 stall (consumed EXT edge routes into CGRP fallback `0x50325fd0` instead of NK slot-4 service `0x50314660`) related to the SC sequence at all, or is `SC#1=0x0d` a PIC-off artifact irrelevant to the verdict?** Adjudicate that, don't just hunt `0x0d`.

- [ ] **Step 1: Locate the SC#1=0x0d evidence basis**

```bash
grep -rn "SC#1\|=0x0d\|0x0d divergence\|selector" docs/archive/2026-06/machine/INTERRUPT-INJECTION-RECON.md docs/archive/2026-06/superpowers/plans/2026-06-12-slot4-consumption.md | head -30
```
Record what `0x0d` is (which syscall selector / what it normally returns) in the findings doc.

- [ ] **Step 2: Boot with syscall + consumption tracing**

```bash
SheepShaver/tools/ss-slot-boot.sh --label m15-bootB-sc0d --timeout 45 \
  --env 'SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_DR_R24_RING=1'
```
**`--env` splits on WHITESPACE, not `;`** (Task-1 correction; a `;`-joined string becomes one garbage token and silently drops the rest). `SS_PROBE_PC` does NOT fire at the syscall vector entry `0x50314ac0` — instead read the always-on `[SC]`/`[EXC]` log lines (the `exc=` tuple records selector + r3) for the `sc` selector sequence, and cross-reference the r24 ring for ordering. Capture the first several `sc` selectors under PIC-on, and determine whether the SC sequence is causally related to the Task-1 stall (EXT→CGRP-fallback `0x50325fd0` instead of NK slot-4 service `0x50314660`) or whether `SC#1=0x0d` is a PIC-off-only artifact irrelevant to the reachable consumption path.

- [ ] **Step 3: Record on-path vs beside-path verdict**

In the findings doc: state whether `SC#1=0x0d` sits on the consumption critical path (a blocker) or beside it (a separate divergence), with ring ordering evidence. If only partially characterized within budget, note the residual as Task 0 of whichever next milestone the verdict selects.

- [ ] **Step 4: Commit the Boot-B evidence**

```bash
git add docs/planning/M15-FINDINGS-consumption-recon.md
git commit -F- <<'EOF'
docs(m15): Boot B — SC#1=0x0d on-path vs beside-path characterization
EOF
```

**Acceptance:** `SC#1=0x0d` is classified on-path or beside-path with ring ordering evidence (or the residual is explicitly handed to the next milestone's Task 0).

---

## Task 3: Boot C — struct-population watch (the verdict's pivot)

**Files:**
- Modify: `docs/planning/M15-FINDINGS-consumption-recon.md` (Boot-C evidence section)

The binary gate: does *anyone* write the NK routing structs when consumption is on? Under native delivery they stayed all-zero for 30s. A single struct-populating write event flips the verdict toward "real."

**This is a WRITE-TRAP, not an end-of-boot sample (binding).** `SS_JIT_WATCH_ADDR` fires `[WATCH]` on any detected change *as it happens* from any JIT/interpreter path, so "frozen-zero when we looked" becomes "provably no struct-populating writer fired from any guest path." Honest residual to document: the watch is blind to value-identical writes (zero-over-zero) and to pure host-accessor (`WriteMacInt`) writes outside the ring's coverage — but the verdict-relevant event (guest IM init writing a *non-zero* pointer/bits/mask into a NIL/zero struct) is exactly what it traps. Pair `[WATCH]` change lines with `[WATCH-SAMPLE]` logarithmic frozen-vs-moving lines.

**Bonus (non-perturbing): capture the misroute "why."** The struct watch needs `SS_JIT_TRACE_RING=1`; the EXT→`0x50325fd0`-fallback-vs-`0x50314660`-slot-4 routing decision is recoverable by POST-HOC `ring-walk` analysis of that SAME ring dump (no extra live probe, no extra watch slot, no incremental perturbation — verified independent). Capture it as a head-start for the forge milestone's design.

- [ ] **Step 1: Build the watch set from the LIVE hnfo base (Task 0)**

Use the live-resolved base from Task 0 — NOT `0x68ff4f00` unless Task 0 confirmed it. Watch addresses are hex, no `0x` prefix, comma-separated; use the span form to cover multi-word longs. Compose: `<hnfo>+0x00`, `<hnfo>+0x14`, `<hnfo>+0x28`, and `KDP+0x674` (`68fff674`).

- [ ] **Step 2: Run the struct-population watch boot**

```bash
# Task-0 live values: hnfo base = 0x68ff4f00 (matched prior session).
# NOTE: --env splits on WHITESPACE not ';' (Task-1 correction).
SheepShaver/tools/ss-slot-boot.sh --label m15-bootC-structwatch --timeout 45 \
  --env 'SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_JIT_TRACE_RING=1 SS_JIT_WATCH_ADDR=68ff4f00,68ff4f14:8,68ff4f28,68fff674 SS_JIT_WATCH_DUMPS=0'
```
`SS_JIT_WATCH_DUMPS=0` = report-only (faster). The watch is a CHANGE detector and is blind to value-identical writes — rely on `[WATCH]` change lines AND `[WATCH-SAMPLE]` logarithmic frozen-vs-moving lines together.

- [ ] **Step 3: Record write-vs-frozen for each struct field**

In the findings doc, for each of the four watched locations: did any `[WATCH]` change event fire, and what do the `[WATCH-SAMPLE]` lines show (frozen at zero vs moving)? This is the verdict pivot — be explicit per field.

- [ ] **Step 4: Commit the Boot-C evidence**

```bash
git add docs/planning/M15-FINDINGS-consumption-recon.md
git commit -F- <<'EOF'
docs(m15): Boot C — NK routing struct population watch (verdict pivot)
EOF
```

**Acceptance:** each of `hnfo+0x00/+0x14/+0x28` and `KDP+0x674` is recorded as written (with the writing PC) or frozen-zero, watched at the **live** hnfo base.

---

## Task 4: Apply the decision rule and emit the verdict

**Files:**
- Modify: `docs/planning/M15-FINDINGS-consumption-recon.md` (Verdict + handoff section)
- Modify: `docs/HANDOFF.md` (headline + resume prompt)
- Modify: `docs/AGENT-CONTEXT.md` (current frontier)

- [ ] **Step 1: Apply the binary decision rule**

In the findings doc Verdict section, apply: consumption reaches the 68k handler (Task 1) AND any struct field begins populating (Task 3) → **REAL (option 1)**; consumption stalls before the handler OR all struct fields stay frozen-zero → **FORGE (option 2)**. State the verdict with a one-paragraph evidence summary citing the Task 1/2/3 record numbers.

**State the FORGE case (if it holds) as TWO independent pillars, explicitly (binding):** (1) the `SC#1=0x0d` divergence that blocked M8's default flip is an *artifact of an unreachable config* (PIC-off, where consumption can't happen); (2) the Task-1 stall is a *routing gap decoupled from the selector stream* (EXT edge routes into CGRP fallback `0x50325fd0`, not slot-4 service `0x50314660`; 68k handler `0x5000ec50` ring-absent). These are separate, load-bearing findings — the verdict must not read as resting on a single chain.

- [ ] **Step 2: Write the next-milestone handoff**

- If **REAL:** the option-1 task list — push the real interrupt through to the 68k handler so IM init populates the structs itself (note whether `SC#1=0x0d` from Task 2 is the first blocker to fix).
- If **FORGE:** the precise oracle-extraction plan — capture correct `KDP+0x674`/`hnfo+0x14`/`hnfo+0x28` values from a working paravirtual or QEMU mac99 boot where IM init runs (this becomes the option-2 milestone's Task 0).

- [ ] **Step 3: Verify standing gates are green (no production change)**

```bash
cd SheepShaver && SS_HARNESS_BATCH=1 make test-jit
```
Expected: `score=100` (353/353). The recon is read-only/env-gated, so paravirtual `make e2e` is structurally unaffected; state this in the close-out. If any `SS_M15_*` trace hook was added, additionally show the gated-off byte-identical A/B per AGENT-CONTEXT "Gate tiers."

- [ ] **Step 4: Update the resume docs and commit the close-out**

Update `docs/HANDOFF.md` headline + resume prompt and `docs/AGENT-CONTEXT.md` current frontier to point at M15's verdict and the selected next milestone.
```bash
git add docs/planning/M15-FINDINGS-consumption-recon.md docs/HANDOFF.md docs/AGENT-CONTEXT.md
git commit -F- <<'EOF'
docs(m15): close-out — consumption recon verdict + next-milestone handoff

Forge-vs-real verdict applied per the binary decision rule; HANDOFF +
AGENT-CONTEXT frontier updated to the selected next milestone.
EOF
```

**Acceptance:** findings doc states REAL or FORGE with cited evidence and a concrete next-milestone task list; `make test-jit` score=100; HANDOFF + AGENT-CONTEXT updated.

---

## Self-review notes (plan vs spec)

- **Spec §1 DoD (1) stall point** → Task 1. **(2) SC#1=0x0d sole-gap** → Task 2. **(3) struct writes** → Task 3. **(4) verdict + handoff** → Task 4. ✓ all four covered.
- **Spec §3 method** boots 1/2/3 → Tasks 1/2/3 one-to-one. ✓
- **Spec §2 OUT (no forge, no fixes, no oracle extraction)** → respected; oracle extraction appears only as the FORGE handoff (next milestone's Task 0), not executed here. ✓
- **Ken's point 4 (resolve hnfo base live)** → Task 0 Step 2 + Task 3 Step 1 (watch set built from live base only). ✓
- **Instrument caveats (exact-match probe blind spot)** → Task 1 Step 3 + Task 3 Step 2 (ring-confirm, watch-blind mitigation). ✓
- **Bound (≤~6 boots)** → Task 0 (1 boot) + Tasks 1/2/3 (1 each) = 4 core boots, headroom for one reflight. ✓
