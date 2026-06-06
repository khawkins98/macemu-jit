# E2E Toolkit — Lay of the Land, Gaps & Proposals (incl. an MCP server)

> **Status:** 🟡 Active · **Created:** 2026-06-07 · **Updated:** 2026-06-07
> **Why this doc exists:** after a burst of E2E-harness work (lifecycle smoke, Speedometer benchmark,
> guest-UI introspection, and the generic screenshot/pHash workload runner), take a lay of the land
> before handing the harness to the other agent: is it a coherent toolkit or a pile of parallel
> approaches? What docs are missing? And scope the forward-looking idea — expose the introspection +
> screenshot + navigation primitives as an **MCP server** so an AI agent can drive the classic-Mac guest.
> _Markers: ✅ done · 🟡 in progress · ⏸ deferred · ☐ todo._

## 1. Lay of the land — what exists

**Architecture (this part is healthy).** Three layers, clean separation:

```
entry scripts (run_*.py)  ->  scenario functions (sse2e/scenario.py)  ->  sse2e library
```

- **Scenarios** (`sse2e/scenario.py`, 753 ln) — three public flows that SHARE real infrastructure
  (`_await_boot_ready`, `_drive_until`, `_await_front_app`, `_quit_to_finder`, the clean-shutdown verdict):
  - `run_lifecycle` — boot → host→guest shutdown → assert clean exit (the smoke gate).
  - `run_benchmark` — boot → drive Speedometer → capture results → shut down (the perf gate).
  - `run_workload` — boot+attach → type-select launch → screenshot/pHash-gated render-timing → quit (S4/S5).
- **Two "sensors"** for reading the guest:
  - **Structured introspection** (`sse2e/uidump.py` + the C++ Backend A): windows/dialogs/controls/menus as
    JSON with VNC-clickable coords. Best for *dialog/standard* apps (find/click by name).
  - **Screenshot + perceptual hash** (`sse2e/imagecmp.py`): menu-bar/render/convergence gating. The fallback
    that works when introspection can't — notably **Carbon/fullscreen apps** (LEARNINGS 2026-06-06).
- **Input**: `sse2e/vnc.py` (click/type/key via vncdotool).
- **Plumbing**: `runner` (process + log signals), `disk` (clonefile safety + prefs render), `config`/`harness`
  (asset resolution, preconditions), `observe` (parse the `[BOOT]`/`[APP]` signals).
- **Reporting**: `bench_export` (Speedometer text report → `history.csv`), `workload` (render-time/pHash →
  per-workload `history.csv`).
- **Entry points**: `run_smoke`, `run_benchmark`, `run_workload` (durable), `run_uidump_smoke` (introspection
  smoke), and `run_fc_discover` + `run_carbonlib_install` (one-off / throwaway).
- **Make targets**: `e2e` (smoke), `e2e-bench`, `e2e-workload`, `e2e-test` (offline), `e2e-setup`, `e2e-clean`,
  `ui-dump`.

**Verdict on coherence (corrected after adversarial review).** The *layering* is sound and the sensor split
is principled — BUT the first draft's "code is coherent, only docs sprawl" was too generous. An adversarial
pass found **real, verbatim duplication** concentrated in a **753-line `scenario.py` god-module** (see
Finding 0). So the honest verdict is: **the toolkit is well-shaped at the seams but has a soft, duplicated
core that should be refactored before it's handed off** — *and* the positioning/docs have sprawl on top.

## 2. Findings — gaps & rough edges (honest)

0. **Real duplication in `scenario.py` (the headline fix the first draft denied).** Verified by adversarial
   review + spot-check:
   - **`_quit_to_finder` (`scenario.py:475`) and `_quit_workload` (`scenario.py:610`) share a verbatim
     Cmd-Q + answer-the-modal-chain loop** — only the post-quit convergence check differs (front-app/intro
     vs pHash). `_quit_to_finder` is used by **benchmark only**; workload reimplements it. (So the first
     draft's "scenarios share `_quit_to_finder`" was simply wrong.)
   - **The clean-shutdown verdict is triplicated**: a named `_benchmark_verdict` (`:317`) + inlined
     `code == 0 and observe.saw_clean_shutdown(...)` in lifecycle (`:110`) and workload (`:726`).
   - **`from vncdotool import api; api.shutdown()` reactor-teardown is repeated** in all three scenario
     `finally` blocks (`:99`, `:304`, `:744`) — the code even comments "keep them consistent" (manual
     duplication).
   - `scenario.py` is **753 lines** doing 3 flows + 9 helpers + 2 dataclasses + constants — a god-module.
   This is the real first job (see P0), and it makes any future MCP wrapper thinner.

1. **No single "toolkit map."** The story is scattered across `e2e/README.md`, `UI-INTROSPECTION.md`,
   `HOST-SIDE-MAC-SOFTWARE-INSTALL.md`, `ROADMAP.md` A5, and the introspection synthesis doc. There's no
   one page that says "here are the sensors, the scenarios, the entry points, and *when to use which*."
2. **README entry-point indexing is partial (corrected count).** `run_smoke`/`run_benchmark` ARE reachable
   via their documented `make e2e`/`make e2e-bench` targets, so the genuine gap is the **2 undocumented
   one-offs** (`run_fc_discover`, `run_carbonlib_install`) — not 4 as first claimed. Separately, the README
   carried a **stale test count ("79"; actual 122)** — exactly the doc-rot this review is about, in the file
   P1 edits. (Fixed in passing, 2026-06-07.)
3. **Throwaway scripts undifferentiated from durable ones.** `run_fc_discover.py` (self-labeled "DISCOVERY,
   throwaway") and `run_carbonlib_install.py` (one-time install) sit in the same directory as the real
   scenarios with no signpost. A reader can't tell harness-infra from a spent one-off. (They're worth
   *keeping* — as worked examples of discovery + installer-driving — but they should be labeled/segregated,
   e.g. an `examples/` or `tools/` subdir, or a header tag + a README line.)
4. **Two parallel perf-history systems.** `bench_export` (Speedometer-specific: extract+parse a text report,
   `archive_run`, `format_delta`) and `workload` (generic: render-time/pHash, `append_history`,
   `format_delta`). Genuinely different report shapes — but they're parallel implementations of "append a
   per-run CSV + format a delta." Either reconcile onto a shared history primitive, or explicitly document
   them as two report *types* so the parallelism reads as intentional, not accidental.
5. **The "which sensor when" insight isn't in the docs as guidance.** The hard-won rule — *introspection for
   dialog/standard apps; screenshot+pHash (menu-bar discriminator) for Carbon/fullscreen* — lives in
   LEARNINGS as a war story, not in a "how to add a gate / a workload" guide where the next author needs it.
6. **No captured statement of the agent-driving opportunity** (the MCP idea below) — the introspection design
   doc gestured at "socket transport / Silicon Sheep" but nothing names the AI-agent-as-consumer surface.

## 3. Proposals

*(Reprioritized after review: P0 first, P4 parked. Effort/risk noted per item.)*

### P0 ☐ Extract the duplicated drive helpers + split `scenario.py` (the real first job)
The highest-value, lowest-risk fix and the one the first draft missed. Pull the verbatim duplication into one
place: a shared `quit_app(runner, vnc, confirm_fn)` (the Cmd-Q + modal-chain loop, parameterized on the
post-quit check), a single `clean_shutdown_verdict(code, log)`, and a `reactor_shutdown()` teardown helper.
Then split `scenario.py` (753 ln) — e.g. `drive.py` (the `_await_*`/`_drive_until`/quit/verdict primitives)
vs the three scenario flows. Net: less code, no behaviour change (it's covered by `test_scenario.py`), and a
thinner base for any future MCP/agent wrapper. **Effort: ~half a day. Risk: low (offline tests gate it).**

### P1 ☐ A one-page "E2E toolkit map" (cheap, high value)
A single `e2e/README.md` top section (or a short `docs/E2E-TOOLKIT.md` linked from it) that lays out: the two
sensors, the three scenarios, the entry-point table (script → make target → what it's for → durable/example),
and a **decision line** ("dialog/standard app → introspection `find_item`; Carbon/fullscreen → screenshot
`run_workload`"). This is the "lay of the land" the other agent needs to *use* the harness without spelunking.

### P2 ☐ Tag the one-off scripts in place (do NOT move them)
`run_fc_discover.py` + `run_carbonlib_install.py` use bare `from sse2e import ...` with no path shim, so
moving them into `e2e/examples/` **breaks their imports** unless invoked via `python -m` / a `sys.path` shim.
So: lead with a **header tag + a README line** ("worked examples of the discovery + installer-driving
patterns, not standing gates"). Only do a physical move if paired with a `python -m examples.x` invocation.

### P3 ☐ One-line note on the two history systems — do NOT build a shared primitive
The review is right that `bench_export` (host-side hfsutils report extractor with per-metric columns + CV%
aggregation) and `workload` (in-process render/pHash appender) overlap on only ~4 lines ("append a CSV +
format a delta") and even those signatures diverge. Factoring a shared `history` primitive would couple two
things with different lifecycles for no real gain. Just add a README/comment note positioning them as two
report *types*. **Decision: no shared primitive.**

### P4 ⏸ **MCP server: expose the guest-driving primitives to an AI agent** (PARKED — answer the gate first)
The forward-looking bet, and the most appealing — but the review parked it for two solid reasons that must be
answered *before* any build:

- **Gate question the first draft never posed: is there a consumer that can't already `import sse2e`?**
  `uidump.py` ALREADY exposes a clean agent API — `find_item`, `click_item`, `click_window`, `wait_for_window`,
  `find_menu_item`, `assert_window` (`uidump.py:239-316`) — plus `vnc` for input. An agent that can run repo
  Python just does `from sse2e import uidump, vnc`. **MCP only earns its keep for an agent that CANNOT execute
  repo Python** (a remote/sandboxed model). So the cheaper, higher-value step is **P4a: document the existing
  Python API as the agent surface** (one page). Reach for the MCP server (P4b) only when a non-Python-executing
  consumer is real.
- **Load-bearing technical blocker (not "transport"): introspection is a LAUNCH-TIME contract.**
  `SS_UI_DUMP_DIR` must be set in the parent env *before* the emulator is spawned (`scenario.py:33/183/646`
  → `Runner.start()`); the idle hook only services snapshot requests when it was present at launch. Consequence:
  **an MCP server that *attaches* to an already-running emulator cannot use introspection at all** (VNC
  screenshot/click/type still work — separate socket — but `ui_snapshot`/`find_item`/`click_menu`/
  `wait_for_window` do not). So "attach mode" gives only the pixel sensor; "boot mode" is the harness with
  extra steps (and the one-emulator-at-a-time rule means it can't coexist with a user session). State this
  plainly in any MCP design; it bounds the whole thing.

Proposed tool surface (read = observe, act = input) — applies to P4b, and doubles as the P4a Python-API map:

| MCP tool | Wraps | Notes |
|---|---|---|
| `screenshot()` → PNG | `vnc.capture` | the pixel sensor |
| `ui_snapshot()` → JSON | `uidump.snapshot` | windows/dialogs/controls/menus + global coords |
| `find_item(text)` / `click_item(text)` | `uidump` + `vnc.click` | click a named control |
| `menu_bar()` / `click_menu(menu,item)` | `uidump` + `vnc` | drive menus by name/⌘-key |
| `type_text` / `key` / `click(x,y)` | `vnc` | raw input |
| `launch_app(volume, app)` | the type-select launch | the proven Finder launch |
| `wait_for_window(title)` / `front_app()` | `uidump` / `observe` | gate on guest state |

**Why it's well-aligned:** introspection was always partly aimed at agent-driving; this is the named consumer.
It also dovetails with Silicon Sheep (Track C) — same primitives, a different (Tauri) front-end.

**Remaining design questions (only relevant once the gate above says "build P4b"):**
- **Read vs act boundary.** `ui_snapshot`/`screenshot` are read-only; input tools mutate. A read-only mode +
  `SS_INPUT_LOCKOUT`-style guarantee lets an agent observe a live session without fighting for the cursor.
- **Scope creep vs Silicon Sheep (Track C).** Keep it a thin stdio wrapper over existing primitives; do NOT
  grow session management (that's Track C's job) — otherwise P4b duplicates Silicon Sheep.
- **Determinism.** Expose the `wait_for_*` gates, not just raw click/type — blind sleeps are what the harness
  exists to avoid.

**If P4b is ever built, the only viable v1 is BOOT-mode** (the server boots one isolated-config instance, so
`SS_UI_DUMP_DIR` is set and the full tool surface works) — which, per the blocker above, is "the harness as a
service." That's fine as a goal, but name it honestly: it's not "attach to whatever's running."

## 4. Recommended order (post-review) & handoff note

**Do in this order — NOT MCP-first:**
1. **P0** — extract the duplicated quit/verdict/teardown helpers + split `scenario.py`. The real coherence
   fix; offline tests gate it; makes everything after thinner.
2. **P1** — the one-page toolkit map (fold the stale-count class of fix in; the `79→122` instance is already
   fixed).
3. **P2** — tag the two one-offs in place (do not move — it breaks their imports).
4. **P3** — a one-line note on the two history systems. No shared primitive.
5. **P4a** — document the existing `sse2e` Python API as the agent surface. Then **stop** unless a consumer
   that *can't* `import sse2e` is real; only then scope **P4b** (boot-mode MCP server).

**Trivial defects found by the audit, fixed in passing (2026-06-07):** README test count `79→122`; unused
`field` import in `workload.py`.

**Handoff to the other agent:** the harness is ready to *use* as gates today — `make e2e` (smoke),
`make e2e-bench` (Speedometer perf), `make e2e-workload` (real-app render, Fractal Carbon); offline logic
unit-tested (`make e2e-test`, 122). None of P0–P4 blocks using the existing gates; they're toolkit-quality
and capability work, not prerequisites.
