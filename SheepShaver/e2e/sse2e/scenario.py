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
