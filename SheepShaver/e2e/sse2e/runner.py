"""Spawn the emulator, capture stderr, and guarantee teardown."""
from __future__ import annotations

import os
import signal
import subprocess
import threading


def kill_strays() -> None:
    """Kill any stray SheepShaver before a run — only one instance can run at a time (shared
    prefs/disk/SDL window). Safe no-op if none are running or `pkill` is absent."""
    try:
        subprocess.run(["pkill", "-9", "-x", "SheepShaver"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except FileNotFoundError:
        pass


class Runner:
    def __init__(self, argv: list[str]):
        self.argv = argv
        self._proc: subprocess.Popen | None = None
        self._buf: list[str] = []
        self._lock = threading.Lock()
        self._reader: threading.Thread | None = None
        self.was_killed = False

    def start(self) -> None:
        # Capture BOTH streams merged: the boot-ready signal is on stderr, but the
        # "Shutdown complete." clean-exit marker (OP_POWEROFF) is on stdout. observe.py
        # needs both, so merge stderr into stdout and drain the one pipe.
        self._proc = subprocess.Popen(
            self.argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self._reader = threading.Thread(target=self._drain, daemon=True)
        self._reader.start()

    def _drain(self) -> None:
        assert self._proc and self._proc.stdout
        for line in self._proc.stdout:
            with self._lock:
                self._buf.append(line)

    def log_text(self) -> str:
        with self._lock:
            return "".join(self._buf)

    def request_shutdown(self) -> None:
        """Ask the guest to cleanly shut down via the host->guest hook (ROADMAP A5).

        Sends SIGUSR1; the emulator's idle hook injects the ADB Power key + Return, so the
        guest runs its real shutdown (flush/unmount) from its own event loop. No VNC needed.
        """
        if self._proc and self._proc.poll() is None:
            os.kill(self._proc.pid, signal.SIGUSR1)

    def wait(self, timeout: float) -> int | None:
        """Return exit code, or None if it did not exit within `timeout`."""
        assert self._proc
        try:
            return self._proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def terminate(self) -> None:
        """Force teardown. Sets was_killed if the process had to be killed."""
        if not self._proc:
            return
        if self._proc.poll() is None:
            self.was_killed = True
            self._proc.kill()
            self._proc.wait(timeout=5)
