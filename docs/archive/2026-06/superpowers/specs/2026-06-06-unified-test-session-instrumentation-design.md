> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone shipped
>

# Unified test-session instrumentation — structured output + cross-oracle reconciliation

**Status:** DESIGN — not started. Brainstormed in-conversation 2026-06-06; this captures the shape
to build incrementally.
**Track:** A (Correctness & verification). **Lead doc back-reference:** ROADMAP A1.

> One-line: today we have *four good correctness/perf oracles and no shared nervous system* — each
> writes ad-hoc text to a manually-chosen path. Give them one **run-stamped output directory**, one
> **machine-readable record schema**, and one **reconciliation/analysis layer**, so a single command
> runs a session, every tool drops structured results into the same place, and an analyzer produces
> a unified correctness + performance report (and auto-referees disagreements).

---

## 1. Problem

The verification stack is mature but **siloed**:

| Oracle | What it checks | Output today | Trust |
|--------|----------------|--------------|-------|
| `make test-jit` | curated vectors, JIT vs interp REGDUMP | `METRIC pass/fail/total/score` (parseable) + text | high |
| `rom-harness` | random ROM blocks, JIT vs **subset** interp | `Passed/Failed/Score/Span mismatch/...` text | medium (subset interp) |
| `SS_JIT_VERIFY` | every block during a real boot, JIT vs real interp | `[VERIFY]` stderr lines + diag log | high but slow/stalls |
| E2E smoke / bench | boot lifecycle; Speedometer scores | run-stamped artifacts + `history.csv` | system-level |
| Diagnostics (heartbeat, HOT-PC, trace ring) | liveness, hot regions, perf | `[HB]` lines, `jit_diag.<ts>.<pid>.log` | observational |
| `rom-harness --bench` / microbench | ns/insn per kernel | text + optional baseline file | perf |
| `SS_TEST_HEX` | **one** instruction, JIT vs **real** interp | REGDUMP (parseable) | **highest (referee)** |

Three concrete pains:
1. **Ad-hoc text.** Some outputs are semi-structured (`METRIC …`, REGDUMP), most are freeform. No
   common schema → no cross-tool analysis without bespoke parsing.
2. **Manual path wiring.** Each run, you choose where things dump (`SS_JIT_DIAG_LOG`, bench
   baselines, redirects). Good that it's *possible*; tedious that it's *required*.
3. **No reconciliation.** The high-value signal is **cross-oracle disagreement** (e.g. rom-harness
   fails a block the real emulator passes — exactly the manual `SS_TEST_HEX`/`SS_TEST_INIT` referee
   done by hand on 2026-06-06). Nothing automates that.

## 2. Goals / non-goals

**Goals.**
- "Turn it on and it just writes out" — one env var (`SS_RUN_DIR`) every tool honors; default to a
  timestamped dir + a `latest` symlink. No per-tool path wiring.
- A single **session orchestrator** command runs the enabled oracles and drops a manifest.
- A common **JSONL record schema** so all outputs are machine-readable and joinable.
- A **reconciliation analyzer**: correctness (cross-oracle agreement + auto-referee) and performance
  (join "what's hot" with "what's slow" → ranked optimization targets).

**Non-goals.**
- Running all oracles *in one process* — they have incompatible lifetimes (ms → minutes; verify
  stalls the guest). It's one **session** (a driver running them in sequence), not one process.
- Replacing the human text output — JSONL is emitted *alongside*, not instead of.
- A new CI gate (yet). First make the data exist and be analyzable; gating is a later decision.

## 3. Reframe + existing scaffolding to build on

This is **not greenfield** — it's generalizing a pattern that already works for one oracle:
- **Per-instance diag logs** already run-stamp (`jit_diag.<timestamp>.<pid>.log` + `/tmp/jit_diag.log`
  symlink).
- **Benchmark-result-export** (`SheepShaver/e2e/sse2e/bench_export.py`, design
  `2026-06-05-benchmark-result-export-design.md`) already does run-stamped artifact folders +
  `history.csv` trend + median±CV aggregation. **That is a working prototype of "structured,
  run-stamped, analyzable" — for E2E perf.** Generalize it.
- **`jit-analyze.py`** (`diag`/`ring`/`hot` subcommands) is already the "read logs → trends"
  analyzer to extend.

## 4. Core design

### 4.1 `SS_RUN_DIR` convention
- If `SS_RUN_DIR` is set, every tool writes its structured + human artifacts under it.
- If unset, the orchestrator creates `${TMPDIR:-/tmp}/macemu-runs/<UTC-timestamp>-<short-rand>/`
  and updates a `…/macemu-runs/latest` symlink. (Note: scripts can't use `Date.now()` etc. — the
  orchestrator stamps the time; library code just honors the env var.)
- Standalone tools run without the orchestrator still honor `SS_RUN_DIR` if exported; otherwise they
  behave exactly as today (no regression).

### 4.2 The record schema (load-bearing — get it right once)
One JSON object per line, appended to `${SS_RUN_DIR}/<tool>.jsonl`:
```jsonc
{
  "schema": 1,
  "run_id": "2026-06-06T14-30-00Z-ab12",
  "tool":   "rom-harness",          // test-jit | rom-harness | verify | e2e-bench | heartbeat | referee | microbench
  "kind":   "correctness",          // correctness | perf | liveness
  "target": {
    "type": "block",                // block | vector | opcode | suite | kernel | region
    "id":   "ROM+0x009d48",
    "opcodes": ["38c00000","7ce30734","4bffeec9"],   // present when reconstructable (enables auto-referee)
    "seed":  305419896               // input seed/regs digest, when applicable
  },
  "verdict": "fail",                // pass | fail | skip | na
  "oracle":  "subset-interp",       // who it was compared against — DRIVES reconciliation trust:
                                     //   real-interp (high) | subset-interp (med) | self (perf) | none
  "detail": { "diff": { "GPR7": { "a": "b5c5197d", "b": "0000197d" } }, "skip_reason": null },
  "metric": { "ns_per_insn": 12.3 },// perf only
  "timing": { "wall_s": 2.0 }
}
```
The **`oracle` field is the key to reconciliation**: it records *trust*, so the analyzer knows a
`rom-harness` fail (`subset-interp`) is weaker evidence than a `verify` fail (`real-interp`), and
that the way to settle it is a `referee` record (`real-interp`, single opcode).

### 4.3 Per-tool emitters (thin additions, one tool at a time)
- **test-jit** (`run.sh`): already prints `METRIC …`; also append one `vector` record per vector.
- **rom-harness**: append a `block` record per tested block (verdict pass/fail/skip + skip_reason
  `span_mismatch`/`fallback`/`segv`; `target.opcodes` for fails so the referee can replay them).
- **SS_JIT_VERIFY**: append a `block` record per divergence (it already classifies
  SUSPECT/ARTIFACT-PC — map to verdict + a `class` in detail).
- **microbench / E2E bench**: `kernel`/`suite` perf records (`metric.ns_per_insn`, Speedometer
  scores) — largely re-expressing what bench-export already computes.
- **heartbeat / HOT-PC**: periodic `liveness` records (region block counts, HOT-PC samples) — the
  "what's hot" feed for the perf join.

### 4.4 Session orchestrator
A `make test-session` target (thin wrapper script): stamp `SS_RUN_DIR`, run each **enabled** oracle
(flags/env to select), write `manifest.json`:
```jsonc
{ "run_id": "...", "utc": "...", "host": "...", "git_sha": "...", "rom": "...",
  "tools": ["test-jit","rom-harness","microbench"], "env": { "SS_USE_JIT": "1" },
  "summary": { "test-jit": {"score":100}, "rom-harness": {"failed":8,"span_mismatch":711} } }
```
Verify/E2E are **opt-in** members (slow / need a GUI session / verify stalls) — default session =
the fast offline trio (test-jit + rom-harness + microbench).

### 4.5 Reconciliation / analysis layer (extend `jit-analyze.py`)
New subcommand `jit-analyze.py session <SS_RUN_DIR>`:

**Correctness reconciliation.** Group records by `target.id`. Flag any target where (a) oracles
disagree, or (b) a verdict=fail from a non-`real-interp` oracle exists. For each flagged target with
`target.opcodes`, **auto-referee**: reconstruct the sequence, run it through the real emulator both
ways (`SS_TEST_HEX=<opcodes> SS_TEST_INIT=<seed> SS_TEST_JIT={0,1}`), diff REGDUMP, emit a `referee`
record verdict ∈ {`real-jit-bug`, `harness-artifact`, `data-as-code`}. **This is the by-hand
2026-06-06 procedure, automated** — the entire payoff of having the schema.

**Performance join.** Join `perf` records (ns/insn by kernel/opcode) with `liveness` HOT-PC samples
(hot opcodes/regions) → a ranked **"hot × slow = high-value optimization target"** table, instead of
eyeballing two disconnected logs.

Output: a single `report.md` (+ `report.json`) in the run dir: correctness verdict (with referee'd
disagreements resolved), perf hotspots, and deltas vs a prior run dir (regression view).

## 5. Phasing (incremental — each phase is independently useful)
- **Phase 0 — schema + plumbing.** Define the record schema (this doc) + a tiny shared emitter
  helper (C++ side: append-JSONL-to-`$SS_RUN_DIR`; Python side: same). Retrofit **two** tools first
  to validate the schema on real data: `rom-harness` (rich: pass/fail/skip/span/referee-able) and
  `test-jit`. *Deliverable: two `*.jsonl` files you can eyeball.*
- **Phase 1 — orchestrator.** `make test-session` + `manifest.json` + `SS_RUN_DIR`/`latest` symlink.
- **Phase 2 — correctness reconciliation + auto-referee.** The high-value piece; automates the
  manual `SS_TEST_HEX` triage. Wire `SS_JIT_VERIFY` as an opt-in emitter here.
- **Phase 3 — perf join.** Heartbeat/HOT-PC + microbench/bench-export records → hot×slow ranking.

## 6. Open design decisions (resolve as we build, not up front)
1. **Schema home + versioning.** Single `schema:` int + a `docs/` schema note, or a JSON Schema
   file? Start with the int; formalize if external consumers appear.
2. **C++ emitter cost.** rom-harness/verify run *millions* of blocks — per-block JSONL would be huge
   and slow. Likely: emit only **non-pass** records (fails/skips) + a final aggregate record, gated
   by `SS_RUN_DIR` being set (zero cost when unset). Decide sampling vs. full for perf records.
3. **Auto-referee fidelity.** The referee needs the block's opcodes **and** an input seed that
   reproduces the divergence; rom-harness must record the exact seed/regs it used (the 2026-06-06
   manual triage had to reconstruct `SS_TEST_INIT` by hand). Also: relative branches don't replay
   identically out of ROM context — the referee must compare interp-vs-JIT *agreement*, not absolute
   PC (as the manual triage did).
4. **Verify's stall.** `SS_JIT_VERIFY` stalls the guest in early boot (documented timer-starvation),
   so as a session member it only ever contributes early-boot coverage — record that scope honestly
   in its records, don't let the report imply whole-boot verify.
5. **Where `latest` lives + cleanup.** Retention policy for `macemu-runs/` (keep N, prune old).

## 7. Risks / limits
- **Schema churn** if we retrofit all tools before validating — mitigated by the Phase-0 "two tools
  first" rule.
- **Over-engineering a one-shot need.** If the real goal is just "occasionally hunt residual bugs,"
  the auto-referee (Phase 2) is the only must-have; Phases 1/3 are convenience. Build Phase 0+2
  first; stop if that's enough.
- **Perf-record volume** (open decision 2) — must be sampled/aggregated, not per-block.

## 8. Relationship to existing work
- Builds on: benchmark-result-export (run-stamped artifacts + history pattern), per-instance diag
  logs (run-stamp precedent), `jit-analyze.py` (analyzer to extend), the `SS_JIT_VERIFY`
  classifier/`SS_TEST_HEX` referee (the correctness oracles being unified).
- Tracked in: ROADMAP A1 (new bullet). Does **not** block the higher-priority AltiVec/FP vector work
  — this is infrastructure that *amplifies* every oracle, pick up by appetite.
