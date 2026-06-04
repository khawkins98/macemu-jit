"""P1 lifecycle scenario: boot -> host->guest shutdown hook -> assert clean exit."""
from __future__ import annotations

import time
from dataclasses import dataclass
from pathlib import Path

from . import observe
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
            return Result(False, "boot timed out (no [BOOT] idle within timeout)", runner.log_text())
        if not observe.is_desktop_ready(ev):
            return Result(
                False,
                f"boot blocked on dialog (frontApp={ev.front_app!r} modal={ev.modal})",
                runner.log_text(),
            )

        # 2. Request a clean shutdown via the host->guest hook (SIGUSR1 -> the emulator injects
        #    the ADB Power key + Return; the guest runs its real shutdown from its own event loop).
        #    No VNC menu-clicking — the only VNC use is an optional, best-effort boot screenshot.
        time.sleep(2.0)  # small settle margin after idle
        if artifact_dir:
            try:
                v = Vnc(port=vncport)
                v.capture(str(artifact_dir / "01-desktop.png"))
                v.close()
            except Exception:
                pass  # screenshot is a debugging artifact, not on the critical path
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


def _await_boot_ready(runner: Runner, timeout: float) -> observe.BootReady | None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for line in runner.log_text().splitlines():
            ev = observe.parse_boot_ready(line)
            if ev is not None:
                return ev
        time.sleep(0.5)
    return None


# --- Benchmark scenario (Speedometer) -------------------------------------------------

# Speedometer auto-launches from the guest Startup Items; we wait for its [APP] signal (deterministic)
# rather than guessing the (highly variable) launch time. BENCH_RUN_WAIT is the only fixed wall-clock.
SPEEDO_LAUNCH_TIMEOUT = 90.0   # max wait for Speedometer to launch + idle on its splash
BENCH_RUN_WAIT = 105.0         # OK-the-disk -> full Speedometer suite done (~90 s + margin)


@dataclass
class BenchResult:
    ok: bool
    reason: str
    log: str
    result_image: str | None = None


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
            return BenchResult(False, "boot timed out (no [BOOT] idle)", runner.log_text())

        # Wait DETERMINISTICALLY for Speedometer to auto-launch and idle on its splash (the [APP]
        # signal), instead of a fixed sleep — boot+launch time is highly variable.
        if not _await_app(runner, "Speedometer", SPEEDO_LAUNCH_TIMEOUT):
            return BenchResult(False, "Speedometer did not launch (no [APP] frontApp='Speedometer')",
                               runner.log_text())
        time.sleep(2.0)  # small settle after the splash-idle signal
        vnc = Vnc(port=vncport)
        vnc.key("enter"); time.sleep(2.5)    # dismiss splash (stable, waits for Enter)
        vnc.key("esc");   time.sleep(2.5)    # dismiss registration prompt
        vnc.key("super-a"); time.sleep(3.0)  # Cmd+A -> run all -> "choose drive to test" dialog
        vnc.key("enter"); time.sleep(2.5)    # OK = main (Desktop) disk -> benchmark auto-starts
        time.sleep(BENCH_RUN_WAIT)           # full suite runs -> "The tests are done!" dialog
        img = str(artifact_dir / "benchmark-result.png")
        vnc.capture(img)                     # capture the results (with the done-dialog)
        vnc.key("enter"); time.sleep(2.0)    # dismiss "The tests are done!" -> guest returns to idle
        vnc.close()

        runner.request_shutdown()
        code = runner.wait(timeout=shutdown_timeout)
        log = runner.log_text()
        if code is None:
            return BenchResult(False, "shutdown timed out after benchmark (had to kill)", log, img)
        return BenchResult(True, "benchmark complete; results + log captured", log, img)
    finally:
        # Always save the emulator log (even on early failure) — terminal output for analysis.
        try:
            (artifact_dir / "benchmark-emulator.log").write_text(runner.log_text())
        except Exception:
            pass
        runner.terminate()


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
