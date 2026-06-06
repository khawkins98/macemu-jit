"""Generic real-world-app workload: types, screenshot/pHash decision logic, and a per-workload
perf history (ROADMAP A5 / S4-S5).

Why a separate path from `bench_export` (Speedometer): a Carbon/fullscreen app (e.g. AltiVec Fractal
Carbon) does NOT present a standard `WindowRecord`/`MenuList` while it renders — Backend-A introspection
returns degenerate windows (see LEARNINGS 2026-06-06). So a workload can't be gated on the window list;
it is gated on the **screenshot perceptual hash**: launch = the screen diverges from the Finder desktop,
render-done = the screen stops changing (pHash stabilizes), quit = the screen converges back to the Finder.
The perf signal is the **render time** (launch→stable) plus the result-image pHash (a visual fingerprint).

The pure decision helpers here (`launched`, `stable_run`) take pHash *distances*, so they unit-test with
no emulator. `run_workload` (in scenario.py) supplies the live screenshots.
"""
from __future__ import annotations

import csv
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class WorkloadSpec:
    """Declarative description of one app workload. Drive-steps are screenshot/pHash-gated.

    Per-app tuning: `menubar_change` / `launch_diverge` are pHash hamming *thresholds* and MUST be
    measured against a real launch frame of THIS app — don't inherit Fractal Carbon's blindly. FC is
    the easy case (its menus are sharply unlike the Finder's and it goes straight to a fullscreen
    render). Two cases stress the menu-bar discriminator (see `scenario._await_launch`): a Carbon app
    with a SPLASH screen that diverges the screen *before* its menu bar activates (could read as a
    false launch-failure — raise patience or add a splash step), and an app whose menu bar resembles
    the Finder's (small Δ near the threshold). Non-Carbon apps escape both via `app_signal`/`[APP]`."""
    name: str                       # history key, e.g. "fractal-carbon"
    volume: str                     # Finder type-select volume name, e.g. "E2E"
    app: str                        # Finder type-select app name, e.g. "AltiVec"
    app_signal: str = ""            # optional [APP] front-app substring that also confirms launch
    menubar_change: int = 14        # menu-bar-strip pHash hamming that means an app owns the menu bar
                                    #   (FC menus vs Finder ~30; a Finder error dialog vs Finder ~4)
    launch_diverge: int = 12        # full-frame pHash hamming meaning "the screen changed" (dialog/app)
    launch_timeout: float = 45.0    # max wait for launch (menu-bar change or app_signal)
    poll_interval: float = 3.0      # seconds between render-progress screenshots
    stable_threshold: int = 6       # pHash hamming under which two frames count as "unchanged"
    stable_frames: int = 2          # consecutive unchanged frames that mean "render done"
    max_render_s: float = 60.0      # cap if the render never stabilizes (e.g. it animates)
    quit_converge: int = 12         # pHash hamming back toward baseline that means "back at Finder"
    quit_timeout: float = 20.0      # max wait for quit-to-Finder
    golden_image: str | None = None # optional reference result PNG for regression (pHash compare)
    regression_threshold: int = 10  # hamming above which the result counts as a visual regression


@dataclass
class WorkloadResult:
    ok: bool
    reason: str
    log: str
    workload: str = ""
    launched: bool = False
    launch_s: float | None = None      # boot-settle -> app-on-screen
    render_s: float | None = None      # launch -> pHash-stable (the headline perf number)
    stable: bool = False               # did the render actually stabilize (vs hit max_render_s)
    result_image: str | None = None
    result_phash: str | None = None    # hex pHash of the result frame (visual fingerprint)
    regression_dist: int | None = None # hamming vs golden_image, if one was supplied
    clean_shutdown: bool = False
    launch_error: str | None = None    # the on-screen error dialog text, if the app failed to launch


def launched(distance_from_baseline: int, diverge_threshold: int) -> bool:
    """The app has taken the screen once the live frame diverges far enough from the Finder baseline."""
    return distance_from_baseline >= diverge_threshold


def classify_launch(menubar_dist: int, full_dist: int, menubar_change: int,
                    launch_diverge: int) -> str:
    """Classify a launch-poll frame from its menu-bar-strip and full-frame pHash distances to the
    (volume-open Finder) baseline. Classic Mac OS gives the menu bar to the frontmost app, so:
      'launched' — the menu bar changed: an app is frontmost (it owns the menu bar).
      'dialog'   — the screen changed but the Finder menu bar is intact: a Finder-level dialog is up
                   with NO app frontmost; sustained, this is a launch failure (e.g. a missing-library
                   alert — which services no idle hook, so the screenshot is the only signal for it).
      'pending'  — nothing decisive yet.
    Menu-bar change is checked first so a real launch always wins over the screen-changed heuristic."""
    if menubar_dist >= menubar_change:
        return "launched"
    if full_dist >= launch_diverge:
        return "dialog"
    return "pending"


def stable_run(distances: list[int], threshold: int, frames: int) -> bool:
    """True once the render has settled: the most recent `frames` consecutive frame-to-frame pHash
    distances are all <= `threshold`. `distances[i]` is the hamming between screenshot i and i+1, so
    `frames` unchanged transitions need `frames` trailing small distances."""
    if frames <= 0 or len(distances) < frames:
        return False
    return all(d <= threshold for d in distances[-frames:])


# --- generic per-workload perf history (append-only CSV; best-effort, never fails a run) ---

HISTORY_FIELDS = ["timestamp", "workload", "ok", "launch_s", "render_s", "stable",
                  "result_phash", "regression_dist"]


def _fmt(v) -> str:
    if v is None:
        return ""
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, float):
        return f"{v:.2f}"
    return str(v)


def append_history(history_csv: str | Path, result: WorkloadResult, timestamp: str) -> None:
    """Append one workload run to the history CSV (creating it with a header if absent)."""
    p = Path(history_csv)
    p.parent.mkdir(parents=True, exist_ok=True)
    new = not p.exists()
    with p.open("a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(HISTORY_FIELDS)
        w.writerow([timestamp, result.workload, _fmt(result.ok), _fmt(result.launch_s),
                    _fmt(result.render_s), _fmt(result.stable), _fmt(result.result_phash),
                    _fmt(result.regression_dist)])


def read_history(history_csv: str | Path) -> list[dict]:
    """All rows (oldest first), or [] if the file is absent/empty."""
    p = Path(history_csv)
    if not p.exists():
        return []
    with p.open(newline="") as f:
        return list(csv.DictReader(f))


def format_delta(history_csv: str | Path, workload: str) -> str:
    """One-line render-time trend for `workload`: latest vs the previous recorded run."""
    rows = [r for r in read_history(history_csv) if r.get("workload") == workload]
    if not rows:
        return f"{workload} history: (no runs recorded)"
    cur = rows[-1]
    cur_r = cur.get("render_s") or "?"
    if len(rows) < 2:
        return f"{workload} history: {cur['timestamp']} render={cur_r}s (first recorded run)"
    prev = rows[-2]
    try:
        d = float(cur["render_s"]) - float(prev["render_s"])
        delta = f"{d:+.2f}s ({d / float(prev['render_s']) * 100:+.1f}%)"
    except (ValueError, ZeroDivisionError, KeyError):
        delta = "n/a"
    return (f"{workload} history: {cur['timestamp']} render={cur_r}s "
            f"(vs {prev['timestamp']} {prev.get('render_s', '?')}s; Δ {delta})")
