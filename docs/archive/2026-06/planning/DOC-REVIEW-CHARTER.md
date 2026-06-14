> **ARCHIVED 2026-06-14** — Moved to archive during the Reku pass (2026-06).
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** never dispatched — superseded by manual doc sweep 2026-06-10
>

# Documentation Review & CHANGELOG Reconciliation — Agent Charter

> **Status:** ⏸ Never dispatched — superseded by manual doc sweep 2026-06-10 · **Created:** 2026-06-07 · **Updated:** 2026-06-10
> **Why this doc exists:** a self-contained prompt for a FRESH agent to audit the project's docs for
> staleness/coherence and reconcile the CHANGELOG against git history (tie features to their commits). A
> fresh agent is chosen deliberately — the people who wrote these docs have author bias (it has already
> caused a "code is coherent" overclaim and a missed CHANGELOG entry this week); fresh eyes are the point.

---

## Copy everything below this line into the fresh agent's prompt

You are doing a **documentation review + CHANGELOG reconciliation** of the macemu-jit fork at
`/Users/khawkins/Documents/git/macemu-jit`, branch `macos-arm64`. You have two deliverables: an **audit
findings report** (you REPORT, you don't mass-edit) and a **CHANGELOG reconciliation** (you DO edit +
commit). Read this whole charter before acting.

### What this repo is (context)
A macOS Apple-Silicon (arm64) fork of macemu adding an **AArch64 JIT** that translates PowerPC→ARM64.
**SheepShaver** (PPC, Mac OS 8.x–9.x) works and boots to the Finder with the full native JIT; **BasiliskII**
(68K) does **not** currently build on macOS arm64 (deferred). A Tauri launcher, **Silicon Sheep**, is in
development. The git history has **4622 commits**, but almost all of that is **upstream history back to
1999** — the *fork's* activity is only the recent slice (roughly 2026-05 onward); focus there. Recent
work has come in **bursts** ("eras"): the JIT bring-up, the SheepShaver↔ARM64 codegen fixes, the E2E/VNC
harness, guest-UI introspection, real-app workloads (Fractal Carbon/CarbonLib), the FP register allocator,
and the UDS-RPC/Silicon Sheep IPC. Tying CHANGELOG entries to commits is how a reader recovers those eras.

### Read these first (the orientation hubs)
- `README.md` — the tracked **doc map** (top-level index; "Documentation" section).
- `CONTRIBUTING.md` — **commit style, the documentation lifecycle, and the rule "CHANGELOG entries cite
  their commit"** (load-bearing for Deliverable 2). Note: `CLAUDE.md` is gitignored/local — do not rely on
  it; the tracked README doc-map is canonical.
- `LEARNINGS.md` (non-obvious findings + the "verify, don't infer from state" methodology rule),
  `docs/ARCHITECTURE.md`, `docs/planning/ROADMAP.md` (the live "what's next" tracker).

### The doc surface (≈102 tracked `*.md`, excl. `docs/superpowers/` + `docs/archive/`)
Enumerate with `git ls-files '*.md'`. Concentrations: `docs/planning/` (~25) + `docs/planning/sheepshaver-research/`
(~25), `BasiliskII/docs/` (9), `docs/` (8), `SheepShaver/docs/` (6), `SheepShaver/e2e/` (3 incl. `AGENT-API.md`),
and the root set (`README`, `LEARNINGS`, `CHANGELOG`, `JIT-STATUS`, `CONTRIBUTING`, `PERFORMANCE_AUDIT`).
You will not deep-read all 102 — triage by hub-importance + recency; say what you skipped.

### ⚠️ Parallel work — do not collide
**Another agent is ACTIVELY committing to this branch** (FP register allocator, UDS-RPC / Silicon Sheep,
profiler, and its own doc reconciles). Therefore:
- For the **audit**: produce a findings REPORT — do **not** mass-edit docs (you'd stomp in-flight edits).
- The **only** files you EDIT are `CHANGELOG.md` (Deliverable 2) and your new findings doc.
- Make **fresh commits only — never `git commit --amend`** (it would clobber the other agent's commits).
- Assume docs may change under you; re-read a file immediately before you touch it.

---

### Deliverable 1 — Audit (REPORT, don't fix): `docs/planning/DOC-REVIEW-FINDINGS-2026-06-07.md`
A prioritized findings list. For **each** finding give: `file:line`, what's wrong, the **evidence you
cross-checked it against** (code/test/git — NOT another doc, which can be co-stale), a suggested fix, and a
severity. Buckets:
- **P0 — wrong facts / contradictions / broken links.** A doc claiming X where the code or tests say Y; a
  feature described as working that isn't (or the reverse); broken relative links; a stale hard number
  (real example just fixed: README said "79 tests", actual 122 — find this *class* across docs: test counts,
  line counts, "N opcodes", score claims, dates-as-facts).
- **P1 — staleness.** Status markers not flipped after work landed; `Updated:` dates that didn't move when
  the doc changed; superseded plans not folded/retired into the live tracker; "next steps" that are already
  done; CLAUDE.md-only references that should be tracked.
- **P2 — coherence / discoverability.** Duplicated or overlapping docs; orphaned docs (linked from no hub);
  planning/spec docs missing the required header block (`Status/Created/Updated/Why` + markers, per
  CONTRIBUTING); cross-doc drift (an item folded in ROADMAP but still "open" in OPTIMIZATION-PLAN — grep the
  item's name across `docs/` and check every hit).

**Method:** VERIFY, don't assume (this repo's own rule — see LEARNINGS). Spot-check a doc's claims by reading
the code/tests it describes. Prefer primary evidence (git, code, `make … test` output) over doc-vs-doc.
Adversarially question the most confident claims. End the report with "what I did NOT cover and why."

### Deliverable 2 — CHANGELOG reconciliation (DO + one fresh commit)
`CHANGELOG.md` has ~62 `###` entries across 4 date sections. CONTRIBUTING requires entries to **cite their
implementing commit (short-SHA)**. Recent entries mostly do; **earlier ones (esp. before ~2026-06-04, the
fork-wide reorg) likely don't.** Reconcile so each **key feature is traceable to its era of activity**:
1. For each entry lacking a SHA, find the implementing commit(s):
   `git log --oneline --grep="<keyword>"`, `git log -S"<symbol>" --oneline`, `git log --oneline -- <file>`,
   then `git show --stat <sha>` to **confirm** the commit actually implements that entry.
2. Cite the **short-SHA** inline, CONTRIBUTING-style: primary + key follow-ups for multi-commit work
   (e.g. `` (`fe378c5d`, triage `980cf4df`) ``).
3. **Prioritize the headline features** — you need not SHA every bullet, but every *major* feature entry
   should be tied to a commit/era. State which you reconciled and which you left.
4. If the entry's written date disagrees with the commit date, note it; the **commit date** is the truer
   "era of activity."
5. **Never fabricate a SHA.** If you cannot confidently map an entry, leave it and list it in the findings
   report as `(commit: unverified — <why>)` rather than guessing.
6. Fix obvious CHANGELOG rot you encounter in the same pass (wrong component tag, a stale claim).
Commit (CONTRIBUTING style, `-F-` heredoc, NO backticks in `-m`, NO `--amend`):
`docs: reconcile CHANGELOG — backfill commit SHAs so features trace to their era`

### Hard constraints
- Branch `macos-arm64`. Do **not** edit `ppc-jit.cpp`, `SiliconSheep/` code, or any file the parallel agent
  is mid-change on. The only edits you make: `CHANGELOG.md` + the new findings doc.
- Fresh commits only; `-F-` heredocs; no `--amend`.
- Verify before you claim. Don't infer behavior from state — read the code/git.

### Report back (to the orchestrator)
The two artifact paths (findings doc + the CHANGELOG commit SHA); a count of findings by severity (P0/P1/P2);
how many CHANGELOG entries you SHA-reconciled vs left unverified; the **top 5 things to fix first**; and an
honest statement of coverage (what you skipped). Your final message is a report to the orchestrator, not the
user — return structured findings, not prose.
