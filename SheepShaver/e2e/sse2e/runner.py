"""Spawn the emulator, capture stderr, and guarantee teardown."""
from __future__ import annotations

import subprocess
import threading


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
