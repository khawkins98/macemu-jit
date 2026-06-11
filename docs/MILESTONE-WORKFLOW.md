# The Milestone Workflow — multi-agent, multi-stream development for RE-heavy work

> Distilled 2026-06-11 from the Machine Layer sessions that shipped M3a, M3b Wave 1,
> M6a rung 2, and the NK syscall surface — four milestones whose later iterations ran
> with **zero falsified contracts**. Every rule below was earned by a specific incident
> (cited). This is the reusable process; the canonical worked example of the plan
> format is `docs/superpowers/plans/2026-06-11-nk-syscall-surface.md` (body + Rev 2).

## 1. The shape of the problem this fits

Reverse-engineering-heavy emulation work has a specific cost profile: **recon is the
expensive, decisive phase; implementation is small once contracts are pinned** (the
"recon-heavy / implementation-light" pattern held 5-for-5 across the M6 walls — e.g.
the MixedMode switch was two seed words + a world-flip once three recon docs existed).
The workflow optimizes for that: front-load falsifiable recon, make implementation
mechanical, and parallelize everything that doesn't contend for the two scarce
resources (the emulator, and a small set of hot source files).

## 2. The milestone machine (the lifecycle)

Every milestone runs the same ladder. Each step is a fresh agent with curated context
— agents never inherit the coordinator's history; the coordinator writes exactly what
they need into the prompt (file paths, pinned facts, constraints, report contract).

```
frontier captured (the P-M4 artifact, from the previous milestone's acceptance)
  → PLAN draft        (read-only Plan agent, on the house template)
  → RED-TEAM round    (2 parallel reviewers: PROCESS + TECHNICAL/CONTRACTS)
  → rev-2 fold        (coordinator merges findings as BINDING amendments; commit)
  → TASK 0 recon      (BINDING: blocking-answer table, budgets — gates all impl tasks)
  → impl tasks A→…    (sequential per shared-file set; env-gated, default OFF)
  → ACCEPTANCE        (gates env-on first; default flip LAST; revert-on-red)
  → DOCS task         (trackers, CHANGELOG, DIAGNOSTICS, LEARNINGS, stale-claim grep)
  → frontier captured → next milestone
```

Key properties of each step:

- **The plan template** (copy the syscall plan's structure): Goal with honest
  PASS/FAIL-vs-DIAGNOSTIC separation; Authoritative-inputs table (docs cited by
  section); Codebase-facts (carried, "implementers re-verify"); tasks with checkbox
  steps and explicit gates; a stop-rule with operationalized one-iteration mechanics;
  a Self-review record (incl. tensions flagged FOR the red team); an empty Red-team
  record. Plans that pre-pin static facts must mark them "re-verify before use".
- **The red-team round is pre-implementation and adversarial.** Two reviewers, run in
  parallel: one attacks process (gating honesty, budgets, blocking maps, stop-rule
  coverage, falsifiability), one independently re-verifies every technical claim
  against sources/dumps/oracles. This round has positive expected value every single
  time it has run: it found a real memory-layout collision (the MM pool vs Hnfo
  scratch), killed a planned change that would have broken working code (the
  [KDP+0x5f0] retarget), and twice *solved milestones statically* before a boot was
  spent (the FE1F slot-8 finding; the syscall selector/r3 conformance vector).
- **Task 0 is BINDING recon** with: a **blocking-answer table** (which implementation
  task blocks on which question — ALL blocking answers pinned before ANY
  implementation starts; the table is a residue-disposition map, not a start-order
  license); **budgets** (boot caps with co-scheduling, per-question static time-boxes
  with written residue fallbacks — unbounded disassembly killed two agents before
  this rule); evidence **tag discipline** ([RAW-ROM]/[PATCH]/[STATIC]/[PROBE✓]);
  and provenance ritual for /tmp artifacts (md5 anchors recorded in tracked docs).
- **Implementation tasks** are env-gated (default OFF) so the baseline stays
  byte-identical until acceptance; every gate is **falsifiable in advance** (expected-
  register tables, value predicates, enumerated parked-PC sets — never "sane" or
  post-hoc "expected"); gates that are baseline-true must be annotated as validating
  the contract, not the change (the vacuous-pass lesson).
- **Acceptance**: full battery env-on first; the default **flip is the LAST step**;
  any gate failure post-flip ⇒ revert the flip in-task. A **fix budget** exists
  (telemetry freely; ONE small in-scope fix per falsified contract, full gates re-run)
  because acceptance-time unlocks are the norm, not violations (the CV-10 lesson).
- **The stop-rule** names its triggers in advance (incl. "the tempting wrong fix" —
  e.g. host-side HLE where real guest code should run) and operationalizes the
  one-iteration rule: falsified contract → dated addendum entry → ONE bounded re-pin
  → resume; a SECOND falsification of the same contract → stop, re-scope, re-plan.
  Stop-rules firing is the process working (M6a rung 2 fired twice and both
  re-scopes were cheap and correct).
- **The docs task** closes every milestone: knob reference (DIAGNOSTICS), CHANGELOG
  with oracle SHAs and acceptance numbers, tracker rows, LEARNINGS for durable
  lessons, and a **cross-tracker grep** for stale current-state claims (historical
  sections stay; only current-state claims get corrected).

## 3. The parallel layer (streams)

Beyond one milestone's internal ladder, independent **workstreams** run concurrently,
coordinated by file ownership and resource leases — same branch, no worktrees.

**What parallelizes freely:** read-only recon, planning drafts and red-teams, pure new
modules (new files + a pre-wired Makefile target), docs tasks, and (with the slot
protocol) diagnostic boots.

**What serializes:** tasks editing the same file set (declare ownership in every
prompt; ONE task in flight per file set), and the default-config emulator until the
slot protocol's leases apply.

**The de-conflict toolbox** (each proven in live use):
- **Pre-wire shared files**: the coordinator commits the shared touchpoint (Makefile
  target, gitignore entries) BEFORE dispatching parallel implementers, so their
  commits stay disjoint.
- **Pin interfaces**: when two parallel modules must meet, the coordinator pins the
  header/contract in both prompts (or decouples with an injected callback so there is
  no compile-time dependency at all — the dev_cuda/adb_stub pattern).
- **Explicit-path staging**: agents commit only their own files by explicit path —
  never `git add -A` (other agents' untracked work may be in the tree).
- **Always-green fusion**: every code commit carries the canonical gate set, so the
  shared branch is continuously releasable and streams fuse by simply committing —
  no merge phase, no integration debt. This is what makes same-branch parallelism
  cheaper than worktrees here.
- **The slot protocol** (`SheepShaver/tools/ss-slot-boot.sh` + `ss-reap.sh`): leased
  per-slot prefs/logs/diag paths; NEVER global `pkill` — reap only stale leases.

**Convergence is designed, not accidental**: streams are chosen so their outputs feed
each other (tooling multiplies recon throughput; ahead-of-need recon de-risks the
critical path's future milestones; verification streams de-fuse named surprises
before construction streams hit them).

## 4. The rules and the incidents that earned them

| Rule | The incident |
|---|---|
| Verify-first task framing ("confirm the hypothesis before building on it") | "TVector excursions running" looked like boot progress; verification showed a faster reboot loop (M6a rung-2 Task W) |
| Red-team before implementation, always | The 0x5f0/4 retarget recommended by three docs would have destroyed the working switch-back; a red-team killed it pre-implementation |
| Occupancy maps over "free gap" comments | The MM pool was placed on the Hnfo scratch our own earlier commit used; the falsifying writer was in-repo (rung-2 C1) |
| Blocking-answer tables; no improvisation on residues | The syscall plan's P-C1: a task consuming an unblocked answer is how plans go wrong at 2am |
| Boot/static budgets with written fallbacks | Two recon agents died in unbounded disassembly before time-boxes existed |
| Counters for counts, probes for ABI | Logarithmic probe sampling undercounts; the delivered-sc counter exists because probes can't count |
| SIGTERM + SS_TERM_DUMP for capture boots | SIGALRM/SIGKILL skip atexit dumps; evidence was silently lost until this was learned |
| Struct fields appended LAST + explicit header deps | A mid-struct insert + no dep tracking produced garbage counters from stale .o files (twice) |
| Oracle SHA citation + documented divergences | Behavioral extraction is reviewable only if every divergence from the donor is written at the site |
| Instrument subtleties get documented AND fixed | Ring dedup masked round-trip evidence and nearly produced a wrong park-shape verdict |
| The baseline is part of the gate | "Byte-identical" must enumerate its fields (jitter counters excluded) or it's unfalsifiable |

## 5. Roles and report contracts

- **Coordinator** (the main session): curates context into prompts; pre-resolves
  conflicts; runs the review loops (implementer → spec review → quality review →
  fix loop → re-review); folds red-teams; enforces serialization; fuses results;
  keeps the workstream map (the task list) current. The coordinator implements
  directly only for small mechanical folds where dispatch overhead exceeds the work.
- **Planner** (read-only): drafts on the template; reports the plan verbatim plus
  open questions converted to Task-0 items and tensions for the red team.
- **Red-teamers** (read-only, adversarial): findings as Critical/Major/minor with
  file:line/offset evidence and concrete fixes; explicit verdicts on items the
  coordinator names as must-answer.
- **Recon agents**: bounded tools, partial-findings-beat-stalling, addendum +
  blocking-table as the deliverable.
- **Implementers**: report DONE / DONE_WITH_CONCERNS / NEEDS_CONTEXT / BLOCKED;
  per-task commits with gates run and numbers stated; never expand scope silently.
- **Reviewers**: spec compliance first (nothing missing, nothing extra), THEN code
  quality (with mutation spot-checks: "would the suite catch this?"); fix loops
  re-review the diff.

## 6. The canonical gate set (this repo)

`make -C SheepShaver build-ss` · `SS_HARNESS_BATCH=1 make test-jit` AND plain
`make test-jit` (353/353; legacy authoritative) · `make -C SheepShaver/src/machine
test` (ALL suites) · `make e2e-test` · paravirtual `make e2e` PASS + byte-identical.
A task may run a smaller set only with a stated reason. Boot recipes, probe limits,
and instrument caveats live in `SheepShaver/docs/DIAGNOSTICS.md`.

## 7. Why this works (the evidence)

Four milestones in one day, the later two with zero falsified contracts and unspent
fix budgets — because by the time implementation starts, every load-bearing fact has
survived two adversarial passes and the gates were written before the code. The
expensive-looking overhead (red-teams, Task 0s, review loops) is where the speed
comes from: walls that Path A priced as reimplementation keep resolving as seeds,
and the one time a fix would have broken working code, a reviewer caught it on paper.
