"""Offline unit tests for the generic-workload decision logic + perf history (no emulator)."""
from pathlib import Path

from sse2e import workload
from sse2e.workload import WorkloadResult


# --- launched(): the app has taken the screen once the frame diverges far enough from baseline ---

def test_launched_true_at_or_above_threshold():
    assert workload.launched(12, 12) is True
    assert workload.launched(40, 12) is True


def test_launched_false_below_threshold():
    assert workload.launched(11, 12) is False
    assert workload.launched(0, 12) is False


# --- classify_launch(): menu-bar change = app launched; screen-changed-but-Finder-menu = dialog ---

def test_classify_launch_menubar_change_is_launched():
    # FC menus vs Finder ~30; well over the 14 threshold even though the full screen also diverged
    assert workload.classify_launch(menubar_dist=30, full_dist=26,
                                     menubar_change=14, launch_diverge=12) == "launched"


def test_classify_launch_finder_dialog_is_dialog():
    # a Finder error dialog: menu bar intact (~4) but the screen changed (~18) -> 'dialog' (a failure)
    assert workload.classify_launch(menubar_dist=4, full_dist=18,
                                     menubar_change=14, launch_diverge=12) == "dialog"


def test_classify_launch_menubar_wins_over_dialog():
    # if BOTH cross their thresholds, the menu-bar (real launch) verdict must win
    assert workload.classify_launch(menubar_dist=20, full_dist=40,
                                     menubar_change=14, launch_diverge=12) == "launched"


def test_classify_launch_pending_when_nothing_decisive():
    assert workload.classify_launch(menubar_dist=3, full_dist=5,
                                     menubar_change=14, launch_diverge=12) == "pending"


# --- stable_run(): render settled = last `frames` frame-to-frame distances all <= threshold ---

def test_stable_run_true_when_trailing_frames_small():
    # two trailing transitions both <= 6 -> stable
    assert workload.stable_run([30, 20, 5, 4], threshold=6, frames=2) is True


def test_stable_run_false_when_a_trailing_frame_moves():
    # most recent transition (9) exceeds threshold -> still rendering
    assert workload.stable_run([5, 4, 9], threshold=6, frames=2) is False


def test_stable_run_needs_enough_samples():
    assert workload.stable_run([3], threshold=6, frames=2) is False
    assert workload.stable_run([], threshold=6, frames=1) is False


def test_stable_run_only_inspects_trailing_window():
    # an early big jump doesn't matter once the tail is quiet
    assert workload.stable_run([99, 99, 2, 1, 0], threshold=6, frames=3) is True


# --- perf history: append-only CSV + a render-time trend line ---

def _result(name, render_s, ok=True, phash="abc", stable=True, launch_s=4.0):
    return WorkloadResult(ok=ok, reason="", log="", workload=name, launched=True,
                          launch_s=launch_s, render_s=render_s, stable=stable,
                          result_phash=phash, clean_shutdown=ok)


def test_append_then_read_roundtrip(tmp_path: Path):
    csvp = tmp_path / "h" / "history.csv"
    workload.append_history(csvp, _result("fractal-carbon", 12.5), "2026-06-06T10-00-00")
    rows = workload.read_history(csvp)
    assert len(rows) == 1
    assert rows[0]["workload"] == "fractal-carbon"
    assert rows[0]["render_s"] == "12.50"
    assert rows[0]["ok"] == "1"


def test_read_history_absent_is_empty(tmp_path: Path):
    assert workload.read_history(tmp_path / "nope.csv") == []


def test_format_delta_first_run(tmp_path: Path):
    csvp = tmp_path / "history.csv"
    workload.append_history(csvp, _result("fractal-carbon", 10.0), "2026-06-06T10-00-00")
    s = workload.format_delta(csvp, "fractal-carbon")
    assert "first recorded run" in s and "render=10.00s" in s


def test_format_delta_shows_render_trend(tmp_path: Path):
    csvp = tmp_path / "history.csv"
    workload.append_history(csvp, _result("fractal-carbon", 10.0), "2026-06-06T10-00-00")
    workload.append_history(csvp, _result("fractal-carbon", 12.0), "2026-06-06T11-00-00")
    s = workload.format_delta(csvp, "fractal-carbon")
    assert "+2.00s" in s and "+20.0%" in s


def test_format_delta_isolates_workload(tmp_path: Path):
    csvp = tmp_path / "history.csv"
    workload.append_history(csvp, _result("fractal-carbon", 10.0), "2026-06-06T10-00-00")
    workload.append_history(csvp, _result("povray", 99.0), "2026-06-06T10-30-00")
    # the povray row must not be read as fractal-carbon's "previous"
    s = workload.format_delta(csvp, "fractal-carbon")
    assert "first recorded run" in s
