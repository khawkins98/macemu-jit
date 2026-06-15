# `docs/superpowers/` — design briefs, plans & research notes

> **Status:** 📖 Index · **Created:** 2026-06-04 · **Updated:** 2026-06-04
> **Why this doc exists:** Index for the time-bound design/plan/research artifacts produced
> during agent-driven work sessions, kept separate from the durable `docs/planning/` trackers.

These are **dated, point-in-time** documents tied to specific work sessions — design briefs
written before implementing a feature, phase plans, and read-only analyses. They are not the
living roadmap; once a brief's work ships, its durable record is the code plus
`CHANGELOG.md` / `LEARNINGS.md` / the trackers under [`../planning/`](../planning/).

| Dir | Contents |
|---|---|
| [`specs/`](specs/) | Approved design briefs, one per feature (e.g. heartbeat warnings, per-instance diag log, rom-inspector, build-ss macOS fix, e2e VNC harness). |
| [`plans/`](plans/) | Phase/implementation plans (macOS baseline, W^X + addressing, per-instance diag log). |
| [`research/`](research/) | Read-only analysis notes (68K execution share, dyngen mechanisms). |

Naming convention: `YYYY-MM-DD-<slug>.md`. New session artifacts go in the matching subdir;
anything that becomes ongoing/forward-looking belongs in [`../planning/`](../planning/) instead.
