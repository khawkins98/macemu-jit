"""Emulator-agnostic boot/drive/gate primitives shared by the scenario flows."""
from __future__ import annotations

import time

from . import observe
from .runner import Runner
from .vnc import Vnc


# Max wait for the settled-desktop [READY] signal before falling back to a blind settle. Generous
# because it waits out startup-items/extension churn (the benchmark disk settled at ~21 s); it
# returns the instant [READY] appears, so it adds no delay in the common case.
READY_TIMEOUT = 45.0


def _nlines(runner: Runner) -> int:
    """Current emulator-log line count — a cursor so a gate only matches signals AFTER an action."""
    return len(runner.log_text().splitlines())


def _await_boot_ready(runner: Runner, timeout: float) -> "observe.BootReady | None":
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for line in runner.log_text().splitlines():
            ev = observe.parse_boot_ready(line)
            if ev is not None:
                return ev
        time.sleep(0.5)
    return None


def _await_ready(runner: Runner, timeout: float) -> bool:
    """Wait for the emulator's settled-desktop `[READY]` signal (Finder frontmost + non-modal for a
    dwell). Returns True if it arrived, False on timeout (caller falls back to a blind settle)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if observe.saw_desktop_ready(runner.log_text()):
            return True
        time.sleep(0.5)
    return False


def _await_since(runner: Runner, since: int, predicate, timeout: float, desc: str):
    """Wait until an `[APP]` event appearing AFTER log line `since` satisfies `predicate(ev)`.

    Returns the elapsed seconds (perf instrumentation — also printed as `[gate] <desc>: Xs`), or
    None on timeout. Gating on the actual window transition (parsed via observe.parse_app) rather
    than a fixed sleep makes each step faster (proceed the instant the state is reached) AND robust
    (never races ahead of the guest). `since` prevents matching a stale signal from before the action.
    """
    t0 = time.monotonic()
    deadline = t0 + timeout
    while time.monotonic() < deadline:
        lines = runner.log_text().splitlines()
        for ln in lines[since:]:
            ev = observe.parse_app(ln)
            if ev is not None and predicate(ev):
                elapsed = time.monotonic() - t0
                print(f"  [gate] {desc}: {elapsed:.1f}s", flush=True)
                return elapsed
        time.sleep(0.2)
    print(f"  [gate] {desc}: TIMEOUT after {timeout:.0f}s", flush=True)
    return None


def _drive_until(runner: Runner, vnc: Vnc, key: str, predicate, desc: str,
                 timeout: float = 40.0, resend: float = 3.0):
    """Drive a key-triggered window transition robustly.

    ONE continuous watch (from a single log cursor `since`) for an `[APP]` event satisfying
    `predicate`, while RESENDING `key` every `resend` seconds as a nudge until it happens. The resend
    is needed because the target window can silently drop input until it is ready (Speedometer's
    splash takes a variable ~10-24 s). Watching continuously — rather than re-cursoring per retry —
    means a transition is never missed in the gap between retries, and the resend stops the instant
    the transition is observed (at most one extra key after it). Returns elapsed seconds, or None on
    timeout. Use only for idempotent keys (resending is harmless once the window is past).
    """
    since = _nlines(runner)
    t0 = time.monotonic()
    last_send = t0 - resend          # send once immediately
    while time.monotonic() - t0 < timeout:
        now = time.monotonic()
        if now - last_send >= resend:
            vnc.key(key)
            last_send = now
        for ln in runner.log_text().splitlines()[since:]:
            ev = observe.parse_app(ln)
            if ev is not None and predicate(ev):
                elapsed = time.monotonic() - t0
                print(f"  [gate] {desc}: {elapsed:.1f}s", flush=True)
                return elapsed
        time.sleep(0.2)
    print(f"  [gate] {desc}: TIMEOUT after {timeout:.0f}s", flush=True)
    return None


def _await_front_app(runner: Runner, since: int, want: str, avoid: str,
                     timeout: float, settle: int = 4):
    """Wait until the front-app stream SETTLES on `want` and away from `avoid`: the most recent
    `settle` `[APP]` frames (after `since`) all lack `avoid` and at least one is `want`.

    This is reliable where a single-frame check is NOT. The idle hook emits spurious one-off
    `frontApp='Finder'` frames even while another app is genuinely frontmost, so gating on a single
    'Finder' frame gives false positives (it "passes" while Speedometer is still up). Requiring
    `avoid` to be ABSENT across a window of consecutive frames only succeeds once the app has really
    quit — those spurious frames are interleaved with `avoid` frames, so any `settle`-frame window
    that still contains the app fails the check. Returns elapsed seconds, or None on timeout.

    Calibration: `settle=4` assumes the spurious non-`avoid` runs are <4 consecutive frames (true of
    the one-off 'Finder' frames observed). If the front-app signal ever emits >=settle consecutive
    non-`avoid` frames while `avoid` is still frontmost, raise `settle`.
    """
    t0 = time.monotonic()
    deadline = t0 + timeout
    while time.monotonic() < deadline:
        events = [e for e in (observe.parse_app(ln)
                              for ln in runner.log_text().splitlines()[since:]) if e is not None]
        recent = events[-settle:]
        if len(recent) >= settle and not any(avoid in e.app for e in recent) \
                and any(want in e.app for e in recent):
            elapsed = time.monotonic() - t0
            print(f"  [gate] settle:{want} (no {avoid}): {elapsed:.1f}s", flush=True)
            return elapsed
        time.sleep(0.2)
    print(f"  [gate] settle:{want} (no {avoid}): TIMEOUT after {timeout:.0f}s", flush=True)
    return None


def _await_app(runner: Runner, app: str, timeout: float) -> bool:
    """Wait until a frontmost-app line CONTAINS `app` (substring, e.g. 'Speedometer' matches
    'Speedometer 4.02'). CurApName carries the full app name + version."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for line in runner.log_text().splitlines():
            fa = observe.front_app(line)
            if fa is not None and app in fa:
                return True
        time.sleep(0.5)
    return False


# ---------------------------------------------------------------------------
# Shared helpers that deduplicate verbatim patterns across scenario flows
# ---------------------------------------------------------------------------

def quit_app(runner: Runner, vnc: Vnc) -> int:
    """Cmd-Q + answer-modal-chain loop: the shared quit primitive for all scenario flows.

    Sends Cmd-Q, then answers up to 4 consecutive save/confirm modals with Return, advancing the
    log cursor after each modal so each `_await_since` only matches a NEW signal. Returns the final
    `since` cursor (the log-line count after the last answered modal, or after the initial Cmd-Q if
    no modal appeared), so callers can continue gate checks from that position.
    """
    since = _nlines(runner)
    vnc.key("super-q")
    for _ in range(4):
        if _await_since(runner, since, lambda e: e.modal, 3.0, "quit-modal") is None:
            break
        since = _nlines(runner)
        vnc.key("enter")
    return since


def clean_shutdown(code: "int | None", log: str) -> bool:
    """True iff the emulator exited cleanly: exit code 0 AND the guest's real shutdown signatures."""
    return code == 0 and observe.saw_clean_shutdown(log)


def reactor_shutdown() -> None:
    """Tear down vncdotool's Twisted (non-daemon) reactor. Safe to call on every path."""
    try:
        from vncdotool import api
        api.shutdown()
    except Exception:
        pass
