"""P1 lifecycle scenario: boot -> host->guest shutdown hook -> assert clean exit."""
from __future__ import annotations

import time
from dataclasses import dataclass
from pathlib import Path

from . import observe
from .bench_export import REPORT_NAME
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
        vnc.capture(img)                         # capture the results (with the "All Done!" alert up)
        vnc.key("enter"); time.sleep(1.5)        # dismiss "All Done!" -> guest returns to idle

        # Save Speedometer's text report onto the (throwaway) run-copy disk so the host can
        # extract the numbers after shutdown. Cmd-T = "Save Text Report"; the dialog opens with
        # the name field selected, so typing replaces the default with our deterministic name
        # (no "replace existing?" prompt on a pristine copy). Best-effort: any failure here must
        # NOT fail the benchmark — the report export is purely additive (collect+report only).
        report_saved = False
        try:
            vnc.key("super-t"); time.sleep(1.0)           # File > Save Text Report...
            vnc.type_text(REPORT_NAME); time.sleep(0.3)   # replace the selected default name
            vnc.key("enter"); time.sleep(1.5)             # Return = Save (default button)
            report_saved = True
        except Exception:
            pass                                          # leave report_saved False; PASS unaffected
        vnc.close()

        runner.request_shutdown()
        code = runner.wait(timeout=shutdown_timeout)
        log = runner.log_text()
        gates = " ".join(f"{k}={v:.1f}s" for k, v in timings.items())
        if code is None:
            return BenchResult(False, "shutdown timed out after benchmark (had to kill)", log, img, duration_s)
        return BenchResult(True,
                           f"benchmark complete in {duration_s:.0f}s (gates: {gates}); results + log captured",
                           log, img, duration_s, report_saved=report_saved)
    finally:
        # Always save the emulator log (even on early failure) — terminal output for analysis.
        try:
            (artifact_dir / "benchmark-emulator.log").write_text(runner.log_text())
        except Exception:
            pass
        runner.terminate()


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
