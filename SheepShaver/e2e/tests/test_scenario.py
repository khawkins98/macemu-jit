"""Offline unit tests for scenario.py's drive/gate/retry logic via a FakeRunner + FakeVnc.

These exercise the most error-prone part of the harness — the signal-gated drive sequence —
without booting an emulator: a FakeRunner returns scripted `[APP]` log lines, a FakeVnc records
the keys pressed (and can append lines to simulate the guest reacting), and a fake clock makes the
timeouts deterministic and instant.
"""
import pytest

from sse2e import scenario


# --- doubles -------------------------------------------------------------------------------

class FakeRunner:
    """Stand-in for runner.Runner — serves scripted log lines; records shutdown/terminate."""
    def __init__(self, lines=None, wait_code=0):
        self._lines = list(lines or [])
        self.shutdown_requested = False
        self.terminated = False
        self._wait_code = wait_code   # scriptable: an int exit code, or None to model a kill/timeout

    def log_text(self):
        return "\n".join(self._lines)

    def add(self, *lines):           # append lines (e.g. in response to a key press)
        self._lines.extend(lines)

    def request_shutdown(self):
        self.shutdown_requested = True

    def wait(self, timeout=None):
        return self._wait_code

    def terminate(self):
        self.terminated = True


class FakeVnc:
    """Records keys/clicks; optional on_key(name) callback lets a test mutate the FakeRunner so the
    'guest' reacts to input (open a dialog, quit, etc.)."""
    def __init__(self, on_key=None):
        self.keys = []
        self.closed = False
        self._on_key = on_key

    def key(self, name):
        self.keys.append(name)
        if self._on_key:
            self._on_key(name)

    def click(self, x, y):
        self.keys.append(f"click({x},{y})")

    def capture(self, path):
        pass

    def close(self):
        self.closed = True


@pytest.fixture
def fast_clock(monkeypatch):
    """Deterministic, instant time: monotonic reads a counter that `sleep` advances (no real wait)."""
    clock = [0.0]
    monkeypatch.setattr(scenario.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(scenario.time, "sleep", lambda s: clock.__setitem__(0, clock[0] + s))
    return clock


def app_line(app, modal=0, win="0x1", title="", ticks=1):
    return f"[APP] frontApp='{app}' modal={modal} win={win} title='{title}' ticks={ticks}"


# --- _await_since --------------------------------------------------------------------------

def test_await_since_returns_when_predicate_matches(fast_clock):
    r = FakeRunner([app_line("Speedometer 4.02", modal=1, title="All Done!")])
    assert scenario._await_since(r, 0, lambda e: "All Done" in e.title, 1.0, "bench") is not None


def test_await_since_times_out_with_no_match(fast_clock):
    r = FakeRunner([app_line("Speedometer 4.02", title="main")])
    assert scenario._await_since(r, 0, lambda e: "All Done" in e.title, 1.0, "x") is None


def test_await_since_ignores_lines_before_the_cursor(fast_clock):
    # The matching line is BEFORE `since` -> must NOT match (a stale signal from before the action).
    r = FakeRunner([app_line("Speedometer 4.02", modal=1, title="All Done!"),
                    app_line("Speedometer 4.02", title="main")])
    assert scenario._await_since(r, 1, lambda e: "All Done" in e.title, 0.5, "x") is None


# --- _drive_until --------------------------------------------------------------------------

def test_drive_until_sends_key_and_returns_on_match(fast_clock):
    r = FakeRunner([app_line("Speedometer 4.02", modal=1, title="splash")])   # baseline (modal)
    # 'enter' makes the guest reach the non-modal main window.
    vnc = FakeVnc(on_key=lambda n: r.add(app_line("Speedometer 4.02", modal=0, title="main"))
                  if n == "enter" else None)
    out = scenario._drive_until(r, vnc, "enter",
                                lambda e: not e.modal and "Speedometer" in e.app,
                                "splash->main", timeout=1.0, resend=0.5)
    assert out is not None
    assert vnc.keys[0] == "enter"          # sent the key, then matched


def test_drive_until_times_out_and_keeps_resending(fast_clock):
    r = FakeRunner([app_line("Speedometer 4.02", modal=1, title="splash")])   # never goes non-modal
    vnc = FakeVnc()
    out = scenario._drive_until(r, vnc, "enter", lambda e: not e.modal, "x", timeout=1.0, resend=0.3)
    assert out is None
    assert vnc.keys.count("enter") >= 2    # resent as a nudge while waiting


# --- _await_app ----------------------------------------------------------------------------

def test_await_app_matches_substring(fast_clock):
    r = FakeRunner([app_line("Speedometer 4.02", title="splash")])
    assert scenario._await_app(r, "Speedometer", 1.0) is True


def test_await_app_times_out(fast_clock):
    r = FakeRunner([app_line("Finder", title="Desktop")])
    assert scenario._await_app(r, "Speedometer", 0.5) is False


# --- _await_front_app (the hardened, settle-based gate) ------------------------------------

def test_await_front_app_settles_once_avoid_is_gone(fast_clock):
    r = FakeRunner([
        app_line("Speedometer 4.02", modal=1, title="Save before quitting?"),
        app_line("Finder", title="Desktop"),
        app_line("Control Strip Extension", title="?"),
        app_line("Finder", title="Desktop"),
        app_line("Finder", title="Desktop"),
    ])
    assert scenario._await_front_app(r, 0, want="Finder", avoid="Speedometer",
                                     timeout=1.0, settle=4) is not None


def test_await_front_app_not_fooled_by_spurious_finder_while_app_running(fast_clock):
    # The exact false positive this gate exists to prevent: single 'Finder' frames interleaved with
    # Speedometer while it's STILL frontmost -> the recent window still contains Speedometer.
    r = FakeRunner([
        app_line("Speedometer 4.02", title="main"),
        app_line("Finder", title="Desktop"),        # spurious one-off
        app_line("Speedometer 4.02", title="main"),
        app_line("Finder", title="Desktop"),        # spurious one-off
        app_line("Speedometer 4.02", title="main"),
    ])
    assert scenario._await_front_app(r, 0, want="Finder", avoid="Speedometer",
                                     timeout=0.5, settle=4) is None


def test_await_front_app_times_out_if_want_never_appears(fast_clock):
    r = FakeRunner([app_line("Control Strip Extension", title="?")] * 5)
    assert scenario._await_front_app(r, 0, want="Finder", avoid="Speedometer",
                                     timeout=0.5, settle=4) is None


# --- _save_text_report ---------------------------------------------------------------------

def test_save_text_report_opens_and_commits(fast_clock):
    r = FakeRunner([app_line("Speedometer 4.02", modal=0, title="main")])

    def on_key(name):
        if name == "super-t":
            r.add(app_line("Speedometer 4.02", modal=1, win="0x9", title=""))   # save dialog (modal)
        elif name == "enter":
            r.add(app_line("Speedometer 4.02", modal=0, win="0x1", title="main"))  # committed

    vnc = FakeVnc(on_key)
    assert scenario._save_text_report(r, vnc) is True
    assert vnc.keys == ["super-t", "enter"]


def test_save_text_report_false_when_no_dialog(fast_clock):
    r = FakeRunner([app_line("Speedometer 4.02", modal=0, title="main")])
    vnc = FakeVnc()                                  # super-t opens nothing
    assert scenario._save_text_report(r, vnc) is False
    assert vnc.keys == ["super-t"]


# --- _quit_to_finder (the full keyboard quit chain) ----------------------------------------

def test_quit_to_finder_returns_through_save_chain(fast_clock):
    r = FakeRunner([app_line("Speedometer 4.02", modal=0, win="0x1", title="main")])
    state = {"enters": 0}

    def on_key(name):
        if name == "super-q":
            r.add(app_line("Speedometer 4.02", modal=1, win="0x2", title="Save before quitting?"))
        elif name == "enter":
            state["enters"] += 1
            if state["enters"] == 1:                 # Yes -> the record save dialog (another modal)
                r.add(app_line("Speedometer 4.02", modal=1, win="0x3", title=""))
            else:                                    # accept -> Speedometer quits -> Finder
                r.add(app_line("Finder", win="0x10", title="Desktop"),
                      app_line("Control Strip Extension", win="0x11", title="?"),
                      app_line("Finder", win="0x10", title="Desktop"),
                      app_line("Finder", win="0x10", title="Desktop"))

    vnc = FakeVnc(on_key)
    assert scenario._quit_to_finder(r, vnc, timeout=2.0) is True
    assert vnc.keys == ["super-q", "enter", "enter"]   # Cmd-Q + Return through two modals


def test_quit_to_finder_false_when_it_never_leaves_speedometer(fast_clock):
    # Cmd-Q produces no modal and Speedometer stays frontmost -> never settles on the Finder.
    r = FakeRunner([app_line("Speedometer 4.02", modal=0, title="main")])
    vnc = FakeVnc(on_key=lambda n: r.add(app_line("Speedometer 4.02", modal=0, title="main")))
    assert scenario._quit_to_finder(r, vnc, timeout=0.5) is False
    assert vnc.keys[0] == "super-q"


# --- _benchmark_verdict (the "honest PASS" decision) ---------------------------------------

_CLEAN_LOG = "boot...\nShutdown complete.\nPPC-JIT-A64: session 1 ended\n"


def test_benchmark_verdict_passes_on_clean_shutdown():
    ok, _ = scenario._benchmark_verdict(0, _CLEAN_LOG)
    assert ok is True


def test_benchmark_verdict_fails_on_kill_timeout():
    ok, reason = scenario._benchmark_verdict(None, _CLEAN_LOG)   # None == had to kill
    assert ok is False and "timed out" in reason


def test_benchmark_verdict_fails_on_exit0_without_shutdown_signatures():
    # The honest-PASS gate: a 0 exit code alone must NOT pass — the guest's real shutdown
    # signatures must be present. Here the atexit line is missing.
    ok, reason = scenario._benchmark_verdict(0, "Shutdown complete.\n")
    assert ok is False and "not clean" in reason


def test_benchmark_verdict_fails_on_nonzero_exit():
    ok, _ = scenario._benchmark_verdict(3, _CLEAN_LOG)
    assert ok is False


# --- _await_finder_via_ui (the introspection-backed Finder gate) ---------------------------

def test_await_finder_via_ui_detects_finder(monkeypatch, fast_clock):
    from sse2e import scenario
    class _W:
        def __init__(self, title, is_dialog=False): self.title=title; self.is_dialog=is_dialog
    class _Snap:
        def __init__(self, wins, front): self._w=wins; self._f=front
        def find(self, *, title=None, title_contains=None, window_class=None, visible=None):
            return [w for w in self._w if title_contains is None or title_contains in w.title]
        def front_window(self): return self._f
    seq = iter([
        _Snap([_W("Speedometer 4.02")], _W("Speedometer 4.02")),   # still up
        _Snap([_W("Desktop")], _W("Desktop")),                      # Speedometer gone, Finder front
    ])
    monkeypatch.setattr(scenario.uidump, "snapshot", lambda *a, **k: next(seq))
    assert scenario._await_finder_via_ui(None, "/tmp/x", timeout=5.0) is True


def test_await_finder_via_ui_falls_back_when_unavailable(monkeypatch, fast_clock):
    from sse2e import scenario
    def _boom(*a, **k): raise TimeoutError("no snapshot")
    monkeypatch.setattr(scenario.uidump, "snapshot", _boom)
    assert scenario._await_finder_via_ui(None, "/tmp/x", timeout=2.0) is None   # signals fallback
