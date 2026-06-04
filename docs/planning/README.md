# `docs/planning/` — forward-looking plans

Every plan, roadmap, and research backlog for the `macos-arm64` fork lives here, so
planning is no longer sprayed across the repo root and component dirs.

**Start with [`ROADMAP.md`](ROADMAP.md)** — the work tracker that arranges everything below
into four tracks (correctness, performance, desktop integration, platform breadth) with
status and pointers.

## What's here

| File / dir | What |
|---|---|
| [`ROADMAP.md`](ROADMAP.md) | **The map.** Outstanding work, tracked across pickups — start here. |
| [`OPTIMIZATION-PLAN.md`](OPTIMIZATION-PLAN.md) | SheepShaver JIT perf roadmap (done / open / deferred levers). |
| [`IMPROVEMENT-CYCLE-1.md`](IMPROVEMENT-CYCLE-1.md) | Multi-agent improvement-cycle plan. |
| [`DESKTOP_INTEGRATION_PLAN.md`](DESKTOP_INTEGRATION_PLAN.md) | "Silicon Sheep" — Parallels-like macOS app layer (Track C). |
| [`JIT-STYLE-DECISION.md`](JIT-STYLE-DECISION.md) | Engineering approach for new JIT work. |
| [`JIT-FPU-PLAN.md`](JIT-FPU-PLAN.md) | 68K/PPC FPU JIT plan. |
| [`NEW-WORLD-ROM-SUPPORT-PLAN.md`](NEW-WORLD-ROM-SUPPORT-PLAN.md) | New World CHRP ROM support plan. |
| [`CHAINING-VERIFICATION-PLAN.md`](CHAINING-VERIFICATION-PLAN.md) | Block-chaining verification plan. |
| [`AMIBERRY_ARM_JIT_PORT_PLAN.md`](AMIBERRY_ARM_JIT_PORT_PLAN.md) | Notes on porting Amiberry's ARM JIT ideas. |
| [`JIT-APPROACH-RESET.md`](JIT-APPROACH-RESET.md) | JIT policy/direction reset — the "simple by default" engineering stance. |
| [`DR-JIT-SYNTHESIS.md`](DR-JIT-SYNTHESIS.md) | DR-emulator JIT synthesis notes. |
| [`REVIEW-RECOMMENDATIONS-2026-06-03.md`](REVIEW-RECOMMENDATIONS-2026-06-03.md) | Deferred backlog from a 2026-06-03 adversarial review (diagnostics/build hygiene); ~14 open items. |
| [`SheepShaver-AARCH64_JIT_PLAN.md`](SheepShaver-AARCH64_JIT_PLAN.md) | SheepShaver PPC→ARM64 JIT plan + status. |
| [`BasiliskII-MACOS-AARCH64-JIT-PORT.md`](BasiliskII-MACOS-AARCH64-JIT-PORT.md) | BasiliskII macOS build/port pick-up plan (Track D). |
| [`BasiliskII-next-phase-plan.md`](BasiliskII-next-phase-plan.md) | BasiliskII 68K JIT next-phase plan. |
| [`sheepshaver-research/`](sheepshaver-research/) | Research/testing web (Dolphin/RPCS3/MAME leads, [IMPLEMENTATION-BACKLOG](sheepshaver-research/research/IMPLEMENTATION-BACKLOG.md), compatibility-testing plan). Moved as a self-contained unit; internal links preserved. |
| [`autoresearch/`](autoresearch/) | BasiliskII opcode-correctness autoresearch notes + experiment reports. |

Deep technical docs that are *not* plans (architecture, diagnostics, testing strategy,
benchmarks, status, changelog) stay in `docs/`, `SheepShaver/docs/`, `BasiliskII/docs/`,
and the repo root — see the Key Documentation table in `CLAUDE.md`.
