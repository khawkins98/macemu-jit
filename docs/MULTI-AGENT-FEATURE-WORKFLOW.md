# The Multi-Agent Feature Workflow — a coordinator-driven playbook

> **What this is.** The concrete, repeatable operating pattern for **substantial feature / bringup work**
> done by a **coordinator** (the main agent loop) driving a **team of spawned subagents** through a fixed
> sequence: deep recon → adversarial red-team → fold → gated serial implementation → frontier capture →
> repeat. It is the *operational instantiation* of `docs/MILESTONE-WORKFLOW.md` (THE process) for
> feature-shaped work — the milestone machine tells you the *gates and laws*; this doc tells you *who does
> what, in what order, and what the coordinator never delegates*.
>
> **Proven 2026-06-15** across one overnight session that shipped two full milestones (SS_M18 S2b-impl
> T1–T5 + S3-impl T1–T4, ~10 gated tasks), opened a third (S1 Task-0 + live probe), and walked a real
> reverse-engineered producer (the NanoKernel Trampoline) forward through four measured walls — at zero
> falsified contracts, zero pushed commits, and full paravirtual byte-identity throughout, including
> recovering cleanly from three transient subagent API deaths.
>
> **When to use it.** Reach for this when the work is a *feature or bringup* with falsifiable gates that
> benefits from (a) independent adversarial review before building, (b) parallel or serialized
> implementer agents, and (c) an empirical measure-the-wall loop. Skip it for trivial mechanical edits.
> When the user says **"use the feature workflow"** / **"spawn a team for this"**, this is the doc.

---

## The roles

| Role | Who | Does |
|---|---|---|
| **Coordinator** | the main agent loop (you) | Owns the plan, dispatches every subagent, runs the heavy/holistic gates personally (real `make e2e`, `make bench`, probe-boots), reviews every diff, **commits serially**, keeps the resume-chain docs current, salvages dead agents. NEVER pushes. |
| **Planner** | one subagent | Drafts the deep Task-0 (binding questions, budgets, stop-rules, PASS taxonomy, self-review tensions) + a red-team voting list. Recon/design only; writes no `src/**`. |
| **Reviewer triad** | three independent perspectives (one 3-in-1 agent or three parallel) | **PROCESS + TECHNICAL + ADVERSARY** red-team the plan; each returns a verdict + a per-item vote on the voting list. |
| **Implementer** | one subagent per serialized task `Tn` | Builds exactly one gated task against the file-ownership LAW; runs `make test-jit` + its own micro-test; reports the diff + what's owed. Does not commit. |
| **RE / Recon** | read-only subagents | Disassemble / trace / locate; bounded budgets; output findings, no edits. |
| **Unblocker** | a lateral/divergent-thinking subagent | Summoned when the coordinator is stuck; re-derives from primary sources, distrusts banked findings. |

The coordinator is also a *team member with non-delegable jobs* (below) — it is not just a dispatcher.

---

## The phases

### Phase 0 — Deep Task-0 (recon + design)
A **Planner** drafts a house-template Task-0 plan: the **binding questions** (each falsifiable), **budgets**
with written residue fallbacks (caps bind; partial-findings-beat-stalling; an unbounded-disasm kill-switch),
named **stop-rules**, a **PASS taxonomy** (GREEN / RESIDUE / NO-GO, each non-gameable), a **self-review**
that flags its own tensions for the red team, and a **red-team voting list** (6–8 must-answer items). It
writes **no `src/**`**. Coordinator writes the draft to `docs/superpowers/plans/…` and commits it.

### Phase 1 — Three-reviewer red-team (the heart)
Dispatch the **reviewer triad** — independent, adversarial, *before any code*:
- **PROCESS** — milestone-machine conformance: are the gates falsifiable-in-advance? Is the PASS taxonomy
  non-gameable? Are budgets/stop-rules bounding? Is the file-ownership serialization coherent?
- **TECHNICAL** — verify every load-bearing claim **against the actual source**: line cites, signatures,
  call-order, what the code contradicts. Re-pin drift.
- **ADVERSARY** — re-derive from primary sources; **distrust banked findings**; take the contrarian side on
  each flagged tension; hunt for false-greens, wrong-build-order, circular dependencies, untestable-until-X.

Each returns a **verdict** (GO / GO-WITH-FIXES / BLOCK) + a **per-item vote**. A single BLOCK-as-written on
a real footgun is a gift, not a setback.

### Phase 2 — Fold to rev-2
Coordinator folds the binding amendments **inline** (mark them, e.g. `★rev-2`), records the red-team in the
plan's "Red-team record", fixes shared cites in *all* affected plans, and commits. The headline amendment
(often an ADVERSARY re-scope) goes in the status header so the next reader sees it first.

### Phase 3 — Gated serial implementation
One **Implementer** per task `Tn`, **serialized by the file-ownership LAW** (sole-in-flight per owned file;
collisions across milestones serialize — the unblocker lands first). Every task obeys the same contract:
- **env-gated default-OFF**; **paravirtual / gate-OFF byte-identical** (structural inertness);
- a **per-task micro-test** that is non-vacuous *without* the parts deferred behind later milestones
  (planted-table / planted-array / placement-unit tests stand in for an un-runnable live path);
- **clean recompile**; **`make test-jit` = 100**;
- **`env-on-first → flip-LAST → revert-on-red = BRANCH revert`** — the gate flips only after the whole
  chain is merged-green; reverting the flip (not the merged restructure) is what makes the dangerous flip
  safe.

**The coordinator then does what it does not delegate** (Phase-3 verify): run the **real `make e2e` A/B**
(GUI/boot authority the implementer lacks) + `make bench` where the hot path is touched, **review the diff**
(especially shared-file and LAW-adjacent edits), and **commit** with a gate-result-bearing message. One task
in, the next dispatched.

### Phase 4 — Frontier capture + the measure-the-wall loop
For bringup (running real RE'd code), each cleared task exposes the next wall. The loop:
**dispatch RE → run a probe (slot-protocol boot) → pin the wall empirically → apply the minimal gated fix →
measure forward progress → commit → next wall.** Probe-don't-theorize: never infer behavior from register
state alone — disassemble / instrument / measure first. Each wall is a small, well-understood commit with a
passing-gates message and a precise "new wall" note.

Then **capture the frontier**: update `docs/HANDOFF.md`'s RIGHT-NOW box and the relevant findings/plan docs
so the resume chain is coherent for the next session (in-session task lists do not persist; the docs do).

---

## What the coordinator never delegates

1. **The holistic gates.** Real `make e2e` A/B + soak, `make bench`, and probe-boots — the implementer runs
   `test-jit` + its unit test and *reports what's owed*; the coordinator runs the boot-level / paravirtual /
   performance gates and is the one who can say "byte-identical, committed".
2. **Diff review + the commit.** Read the actual hunks (gating correctness, LAW lines untouched, gate-OFF
   inertness) before committing. **Commit serially; never push unprompted.**
3. **Agent-death salvage.** Subagents die on transient API errors mid-task. Inspect the partial tree, decide
   salvage-vs-restart, **complete + verify yourself**, and commit only when green — never let half-done work
   enter the tree, never re-run a dead agent's prefix blindly.
4. **The resume chain.** HANDOFF + findings + plan rev-N must stay current; a future reader (or a compacted
   context) resumes from the docs, not from memory.

---

## The standing laws (carried from the milestone machine; enforced every task)

- **File-ownership LAW** — sole-in-flight per owned file; cross-milestone collisions serialize (the
  unblocker lands first; the later milestone rebases as an *additive clause*, not a rewrite — pin the
  combined predicate as a coordinator-reviewed interface).
- **Gated-OFF + byte-identical** at every task; paravirtual provably unreachable (audit the gate site).
- **Falsifiable-in-advance gates**; **PEM/LAW lines never touched** (retire at the call-site/gate; one
  ad-hoc fix to a LAW line ⇒ re-plan).
- **Honest NON-ACCEPTANCE labeling** — a bringup approximation (a stub, an identity map, a provisional ABI
  value) is logged + commented as NON-ACCEPTANCE / owed-to-X, never claimed as real; reaching the next wall
  is the win, not faking a green.
- **Probe-don't-theorize**; **stop-and-surface** on a falsified pin (one bounded re-pin, then re-plan);
  **no fabricated boots / addresses / sentinels** (`0xDEADBEEF` where a real value is expected ⇒ STOP).

---

## The exemplars (point future work at these)

- **Plans** — `docs/superpowers/plans/2026-06-15-ss-m18-s2b-impl-loader-launch.md` (rev-2; the gated serial
  T1–T5 task breakdown + the red-team fold), `…-s3-impl-replace.md` (rev-2; the ADVERSARY re-scope headline),
  `…-s1-task0-live-mmu.md` (a Task-0 with a coordinator-run probe-boot design + the measure-the-wall
  findings appended).
- **The measure-the-wall loop in action** — that S1 plan's `§FINDING` / `§Relocation-loop RE`: probe boot →
  pin the wall → minimal gated fix → re-measure → next wall, commit by commit.
- **The machine + laws** — `docs/MILESTONE-WORKFLOW.md` (§2/§4/§6) is the parent; this doc is the
  who-does-what overlay.

---

## One-paragraph version (for a prompt)

> Coordinate it as a team: a Planner drafts a falsifiable Task-0 + a red-team voting list; a
> PROCESS+TECHNICAL+ADVERSARY triad red-teams it *before any code* (verify cites against source, distrust
> banked findings, hunt false-greens); fold the binding amendments to rev-2 and commit. Then implement in
> serialized gated tasks — one implementer per owned file-set, each env-gated default-OFF + paravirtual
> byte-identical + a non-vacuous micro-test + `test-jit`=100; the coordinator personally runs the real
> `make e2e` A/B + any probe-boots, reviews the diff, and commits serially (never pushes). For bringup, run
> the measure-the-wall loop: RE → probe → pin the wall → minimal gated fix → measure progress → commit →
> next wall. Label every approximation NON-ACCEPTANCE honestly, keep `HANDOFF.md` current, and salvage any
> agent that dies mid-task by finishing + verifying it yourself before committing.
