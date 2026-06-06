"""P1 lifecycle scenario: boot -> host->guest shutdown hook -> assert clean exit."""
from __future__ import annotations

import os
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from . import observe, uidump
from .runner import Runner
from .vnc import Vnc


@dataclass
class Result:
    ok: bool
    reason: str
    log: str


def run_lifecycle(
    *,
    emulator: str,
    prefs: str,
    vncport: int,
    boot_timeout: float = 90.0,
    shutdown_timeout: float = 30.0,
    artifact_dir: Path | None = None,
) -> Result:
    dump_dir = tempfile.mkdtemp(prefix="ss-ui-")
    os.environ["SS_UI_DUMP_DIR"] = dump_dir

    runner = Runner(argv=[emulator, "--config", prefs])
    runner.start()
    try:
        # 1. Wait for the deterministic boot-ready signal.
        ev = _await_boot_ready(runner, boot_timeout)
        if ev is None:
            return Result(False, "boot timed out (no [BOOT] idle) — if the guest shows the '?' "
                          "no-boot-disk icon, a stray SheepShaver likely holds the disk image "
                          "(check `pgrep SheepShaver`)", runner.log_text())
        if not observe.is_desktop_ready(ev):
            return Result(
                False,
                f"boot blocked on dialog (frontApp={ev.front_app!r} modal={ev.modal})",
                runner.log_text(),
            )

        # 2. Wait for the SETTLED-desktop signal ([READY]) before shutting down — more robust than the
        #    first [BOOT] idle, which can fire while the Finder is still drawing / startup items launch.
        #    Fall back to a short blind settle if [READY] doesn't arrive (e.g. a medium that never
        #    fully settles), so the smoke can't hang waiting on it.
        if not _await_ready(runner, READY_TIMEOUT):
            time.sleep(2.0)

        # Introspection corroboration (non-fatal): log the actual on-screen window list at the
        # settled desktop. Proves the harness can "see" the guest UI; does not gate the result yet.
        try:
            snap = uidump.snapshot(dump_dir, timeout=10.0)
            wins = ", ".join(f"[{w.index}]{w.title!r}({w.window_class})" for w in snap.windows) or "(none)"
            print(f"  [ui] desktop snapshot: {len(snap.windows)} windows, modal={snap.modal_active}: {wins}",
                  flush=True)
            mb = snap.menu_bar
            if mb is not None:
                titles = [m.title for m in mb.menus]
                fm = mb.menu("File")
                fkeys = {it.text: it.cmd_key for it in fm.items if it.cmd_key} if fm else {}
                file_ok = (len(titles) >= 3 and titles[1] == "File" and titles[2] == "Edit")
                print(f"  [ui] menu bar: {len(mb.menus)} menus {titles} | File/Edit_ok={file_ok} "
                      f"File keys={fkeys}", flush=True)
        except Exception as e:
            print(f"  [ui] snapshot unavailable (non-fatal): {e}", flush=True)

        # 3. Request a clean shutdown via the host->guest hook (SIGUSR1 -> the emulator injects
        #    the ADB Power key + Return; the guest runs its real shutdown from its own event loop).
        #    No VNC menu-clicking — the only VNC use is an optional, best-effort boot screenshot.
        if artifact_dir:
            try:
                v = Vnc(port=vncport)
                v.capture(str(artifact_dir / "01-desktop.png"))
            except Exception:
                pass  # screenshot is a debugging artifact, not on the critical path
            finally:
                # ALWAYS stop vncdotool's reactor — even if connect/capture raised. Otherwise the
                # orphaned non-daemon reactor thread blocks interpreter exit (process hangs after the
                # PASS line). api.shutdown() is safe to call whether or not the reactor came up.
                try:
                    from vncdotool import api
                    api.shutdown()
                except Exception:
                    pass
        runner.request_shutdown()

        # 3. Assert clean exit: process exits on its own, log shows both signatures.
        code = runner.wait(timeout=shutdown_timeout)
        if code is None:
            return Result(False, "shutdown timed out — process did not exit (had to kill)", runner.log_text())
        text = runner.log_text()
        if code == 0 and observe.saw_clean_shutdown(text):
            return Result(True, "clean lifecycle: booted to Finder, clean shutdown, exit 0", text)
        return Result(
            False,
            f"unclean exit (code={code}, clean_signatures={observe.saw_clean_shutdown(text)})",
            text,
        )
    finally:
        runner.terminate()


# Max wait for the settled-desktop [READY] signal before falling back to a blind settle. Generous
# because it waits out startup-items/extension churn (the benchmark disk settled at ~21 s); it
# returns the instant [READY] appears, so it adds no delay in the common case.
READY_TIMEOUT = 45.0


def _await_boot_ready(runner: Runner, timeout: float) -> observe.BootReady | None:
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


# --- Benchmark scenario (Speedometer) -------------------------------------------------

# Speedometer auto-launches from the guest Startup Items; we wait for its [APP] signal (deterministic)
# to detect both the launch AND the "tests are done!" dialog (modal=1) — no fixed benchmark sleep.
SPEEDO_LAUNCH_TIMEOUT = 90.0   # max wait for Speedometer to launch + idle on its splash
BENCH_DONE_TIMEOUT = 240.0     # max wait for the "tests are done!" dialog (suite is ~90s)


@dataclass
class BenchResult:
    ok: bool
    reason: str
    log: str
    result_image: str | None = None
    duration_s: float | None = None  # measured suite runtime (a coarse perf signal)
    report_saved: bool = False       # whether the in-guest Cmd-T text-report save was driven


def run_benchmark(
    *,
    emulator: str,
    prefs: str,
    vncport: int,
    boot_timeout: float = 90.0,
    shutdown_timeout: float = 30.0,
    artifact_dir: Path,
) -> BenchResult:
    """Boot the benchmark disk, drive Speedometer's full suite, capture results, shut down.

    Sequence (Speedometer auto-launches from the guest Startup Items):
      splash -> Return; registration -> Esc; Cmd+A (run all) -> "choose drive" dialog ->
      Return (OK = the main Desktop disk) -> ~90 s benchmark -> screenshot results.
    """
    dump_dir = tempfile.mkdtemp(prefix="ss-ui-")
    os.environ["SS_UI_DUMP_DIR"] = dump_dir

    runner = Runner(argv=[emulator, "--config", prefs])
    runner.start()
    try:
        ev = _await_boot_ready(runner, boot_timeout)
        if ev is None:
            return BenchResult(False, "boot timed out (no [BOOT] idle) — if the guest shows the '?' "
                               "no-boot-disk icon, a stray SheepShaver likely holds the disk image "
                               "(check `pgrep SheepShaver`)", runner.log_text())

        # Wait DETERMINISTICALLY for Speedometer to auto-launch and idle on its splash (the [APP]
        # signal), instead of a fixed sleep — boot+launch time is highly variable.
        if not _await_app(runner, "Speedometer", SPEEDO_LAUNCH_TIMEOUT):
            return BenchResult(False, "Speedometer did not launch (no [APP] frontApp='Speedometer')",
                               runner.log_text())
        vnc = Vnc(port=vncport)

        # Every drive step is SIGNAL-GATED on the actual guest window transition (the enriched [APP]
        # title=/modal= signals) instead of a fixed sleep: we proceed the instant the guest reaches
        # the next state, each gate prints its elapsed time (perf instrumentation), and a key that
        # didn't take is resent (_drive_until). Faster than fixed sleeps when the guest is quick, and
        # robust — never races ahead, self-corrects if a key lands before the window is input-ready.
        timings: dict[str, float] = {}

        # Splash -> main window. Predicate is sound because the splash is the ONLY Speedometer state
        # before the main window, and it is modal — so the first non-modal Speedometer event is the
        # main window. Resend Enter every 3 s until then (the splash drops input for a variable
        # ~10-24 s).
        t = _drive_until(runner, vnc, "enter", lambda e: "Speedometer" in e.app and not e.modal,
                         "splash->main", timeout=40.0, resend=3.0)
        if t is None:
            return BenchResult(False, "splash did not dismiss to Speedometer's main window",
                               runner.log_text())
        timings["splash"] = t

        vnc.key("esc"); time.sleep(0.5)          # dismiss the optional registration prompt (absent on
                                                 # this build, so there's no signal to gate on — brief)

        # Cmd+A = "run all" -> "choose a disk" dialog. Predicate `e.modal` is sound here because the
        # main window (just reached) is non-modal, so the first modal event AFTER Cmd+A is this dialog.
        t = _drive_until(runner, vnc, "super-a", lambda e: e.modal, "choose-disk dialog",
                         timeout=20.0, resend=4.0)
        if t is None:
            return BenchResult(False, "choose-disk dialog did not appear after Cmd+A",
                               runner.log_text())
        timings["choose"] = t

        # [Plan 2a] Live verification + de-facto dialog identity: log the choose-disk dialog's DITL
        # items. Non-fatal — a hiccup here must not fail the benchmark.
        try:
            snap = uidump.snapshot(dump_dir, timeout=8.0)
            dlg = snap.front_dialog()
            if dlg is not None:
                def _fmt(it):
                    s = f"{it.type}:{it.text!r}@({it.rect.left},{it.rect.top})"
                    if it.value is not None:
                        ok = (it.crect is not None
                              and it.crect.left == it.rect.left and it.crect.top == it.rect.top
                              and it.crect.right == it.rect.right and it.crect.bottom == it.rect.bottom)
                        s += f" val={it.value} hil={it.hilite} crect_ok={ok}"
                    return s
                items = ", ".join(_fmt(it) for it in dlg.items) or "(none)"
                print(f"  [ui] choose-disk dialog: refCon={dlg.ref_con} default={dlg.default_item} "
                      f"{len(dlg.items)} items: {items}", flush=True)
            else:
                _fw = snap.front_window()
                _ft = _fw.title if _fw is not None else None
                print(f"  [ui] choose-disk: front is not a dialog (front={_ft!r})", flush=True)
        except Exception as e:
            print(f"  [ui] dialog snapshot unavailable (non-fatal): {e}", flush=True)

        since = _nlines(runner)
        vnc.key("enter")                         # OK = the main Desktop disk -> benchmark runs
        # The benchmark-finished signal is Speedometer's "All Done!" alert (title=) — unambiguous,
        # unlike the older modal=1 count, which also matched the choose-disk dialog.
        duration_s = _await_since(runner, since, lambda e: "All Done" in e.title,
                                  BENCH_DONE_TIMEOUT, "benchmark")
        if duration_s is None:
            return BenchResult(False, "benchmark did not finish (no 'All Done!' alert within timeout)",
                               runner.log_text())

        time.sleep(1.0)                          # let the alert settle before the screenshot
        img = str(artifact_dir / "benchmark-result.png")
        try:
            vnc.capture(img)                     # capture the results (with the "All Done!" alert up)
        except Exception:
            pass                                 # the screenshot is a debugging artifact, not critical
        vnc.key("enter"); time.sleep(1.5)        # dismiss "All Done!" -> guest returns to idle

        # Save the text report onto the (throwaway) run-copy disk so the host can extract it after
        # shutdown, then quit Speedometer back to the Finder (the Power-key shutdown hook only raises
        # the Shut Down dialog at the Finder, not over a frontmost app). Both are extracted, unit-
        # tested helpers. All best-effort: a hiccup here must NOT fail the benchmark — the real gate
        # is the clean shutdown asserted below.
        report_saved = False
        try:
            report_saved = _save_text_report(runner, vnc)
            _quit_to_finder(runner, vnc, dump_dir=dump_dir)
            time.sleep(1.5)                               # let the Finder settle before shutdown
        except Exception:
            pass                                          # leave report_saved False; PASS unaffected
        vnc.close()

        runner.request_shutdown()
        code = runner.wait(timeout=shutdown_timeout)
        log = runner.log_text()
        gates = " ".join(f"{k}={v:.1f}s" for k, v in timings.items())
        ok, reason = _benchmark_verdict(code, log)   # honest PASS: requires a real clean shutdown
        if not ok:
            return BenchResult(False, reason, log, img, duration_s, report_saved=report_saved)
        return BenchResult(True,
                           f"benchmark complete in {duration_s:.0f}s (gates: {gates}); results + log captured",
                           log, img, duration_s, report_saved=report_saved)
    finally:
        # ALWAYS tear down vncdotool's Twisted (non-daemon) reactor. A benchmark that returns early
        # (a drive gate timed out) or raises mid-drive would otherwise leave the reactor thread alive
        # and HANG the process after printing the result line — the exact trap vnc.py / run_smoke.py
        # warn about. api.shutdown() is safe on every path (whether or not a Vnc was created / the
        # reactor came up); the happy path already closed it, so this is the catch-all. (run_lifecycle
        # does the same in its finally — keep them consistent.)
        try:
            from vncdotool import api
            api.shutdown()
        except Exception:
            pass
        # Always save the emulator log (even on early failure) — terminal output for analysis.
        try:
            (artifact_dir / "benchmark-emulator.log").write_text(runner.log_text())
        except Exception:
            pass
        runner.terminate()


def _benchmark_verdict(code: int | None, log: str) -> tuple[bool, str]:
    """Decide PASS/FAIL from the emulator's exit code + log. HONEST PASS: a clean exit *code* alone
    isn't enough — require the guest's real clean-shutdown signatures (`observe.saw_clean_shutdown`:
    "Shutdown complete." + the atexit session line), so a green benchmark means the harness genuinely
    drove an unattended shutdown, not that the process merely exited. Returns (ok, reason)."""
    if code is None:
        return False, "shutdown timed out after benchmark (had to kill)"
    clean = observe.saw_clean_shutdown(log)
    if code != 0 or not clean:
        return False, (f"benchmark ran but the shutdown was not clean "
                       f"(exit={code}, clean_signatures={clean})")
    return True, "shutdown clean"


def _nlines(runner: Runner) -> int:
    """Current emulator-log line count — a cursor so a gate only matches signals AFTER an action."""
    return len(runner.log_text().splitlines())


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


def _save_text_report(runner: Runner, vnc: Vnc) -> bool:
    """Drive Cmd-T "Save Text Report" and accept the default name ("Power Macintosh Report").

    Typing a custom name proved unreliable (keys dropped/leaked and it saved under the default name
    anyway), so we don't type — the host matches the default name. Returns True if the modal save
    dialog opened and committed; best-effort — the caller must not fail the benchmark on a False.
    """
    since = _nlines(runner)
    vnc.key("super-t")                                # File > Save Text Report...
    if _await_since(runner, since, lambda e: e.modal, 8.0, "save-dialog") is None:
        return False                                  # no save dialog appeared
    since = _nlines(runner)
    vnc.key("enter")                                  # Return = Save (accept the default name)
    if _await_since(runner, since, lambda e: not e.modal, 8.0, "save-commit") is not None:
        return True
    since = _nlines(runner)                            # dialog stuck — Escape it so it can't block quit
    vnc.key("esc")
    _await_since(runner, since, lambda e: not e.modal, 5.0, "save-cancel")
    return False


def _await_finder_via_ui(runner: Runner, dump_dir: str, timeout: float):
    """Definitive 'back at the Finder' check via UI introspection: Speedometer has no window left
    and the front window is a non-dialog (the Desktop / a Finder window). Returns True if reached,
    False if it timed out with Speedometer still present, or None if introspection produced no
    snapshot at all (so the caller can fall back to the front-app settle heuristic)."""
    deadline = time.monotonic() + timeout
    first = True
    while time.monotonic() < deadline:
        try:
            snap = uidump.snapshot(dump_dir, timeout=min(3.0, max(0.5, deadline - time.monotonic())))
        except TimeoutError:
            if first:
                return None            # no snapshots at all -> let the caller fall back
            time.sleep(0.3); continue
        first = False
        if not snap.find(title_contains="Speedometer"):
            fw = snap.front_window()
            if fw is not None and not fw.is_dialog:
                print(f"  [gate] ui:Finder (Speedometer gone, front={fw.title!r})", flush=True)
                return True
        time.sleep(0.3)
    return False


def _quit_to_finder(runner: Runner, vnc: Vnc, timeout: float = 12.0, dump_dir: str | None = None) -> bool:
    """Quit Speedometer back to the Finder, KEYBOARD-ONLY. Cmd-Q, then answer each modal that follows
    ("Save before quitting?" -> Yes -> the record save dialog -> accept -> any replace prompt) with
    Return, until Speedometer has reliably VANISHED from the front-app stream. When `dump_dir` is
    supplied, uses UI introspection (`_await_finder_via_ui`) as a definitive check; falls back to the
    noisy-'Finder'-frame settle heuristic (`_await_front_app`) when introspection is unavailable.
    Returns True if the Finder was reached. The Power-key shutdown hook only raises the Shut Down
    dialog at the Finder, not over a frontmost app.
    """
    since = _nlines(runner)
    vnc.key("super-q")                                # Cmd-Q = Quit
    for _ in range(4):                                # answer the save-changes chain with Return
        if _await_since(runner, since, lambda e: e.modal, 3.0, "quit-modal") is None:
            break
        since = _nlines(runner)
        vnc.key("enter")                              # default button (Yes / Save / Replace)
    # Prefer a definitive introspection check; fall back to the front-app settle heuristic if
    # introspection is unavailable (no SS_UI_DUMP_DIR / no snapshot).
    if dump_dir is not None:
        ok = _await_finder_via_ui(runner, dump_dir, timeout)
        if ok is not None:
            return ok
    return _await_front_app(runner, since, want="Finder", avoid="Speedometer",
                            timeout=timeout) is not None


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
