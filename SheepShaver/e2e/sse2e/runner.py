"""Spawn the emulator, capture stderr, and guarantee teardown."""
from __future__ import annotations

import datetime
import os
import signal
import subprocess
import threading
import time
from pathlib import Path


def binary_build_info(emulator: str) -> str:
    """One-line "when was this binary built" line, printed at the start of a run so you can confirm
    you're testing the binary you just built — not a stale one. Uses the binary's mtime (set by the
    link step) and warns if any emulator source file is newer (you forgot to `make build-ss`)."""
    p = Path(emulator)
    if not p.exists():
        return f"emulator binary not found: {emulator} — run `make build-ss`"
    mtime = p.stat().st_mtime
    built = datetime.datetime.fromtimestamp(mtime).strftime("%Y-%m-%d %H:%M:%S")
    age = max(0.0, time.time() - mtime)
    age_str = (f"{age/60:.0f} min ago" if age < 3600
               else f"{age/3600:.1f} h ago" if age < 86400 else f"{age/86400:.1f} d ago")
    msg = f"emulator: built {built} ({age_str})"
    try:
        if _source_newer_than(mtime, p):
            msg += "  — WARNING: emulator source is NEWER; run `make build-ss` (or `make e2e`)"
    except Exception:
        pass  # staleness check is best-effort; never block a run on it
    return msg


def _source_newer_than(mtime: float, emulator: Path) -> bool:
    """True if any emulator .cpp/.h is newer than the binary. emulator = …/SheepShaver/src/Unix/SheepShaver."""
    unix = emulator.resolve().parent                       # …/SheepShaver/src/Unix
    roots = [unix.parent, unix.parents[2] / "BasiliskII" / "src" / "SDL"]  # SheepShaver/src, BasiliskII/src/SDL
    for root in roots:
        if not root.is_dir():
            continue
        for pattern in ("*.cpp", "*.h"):
            for f in root.rglob(pattern):
                if f.stat().st_mtime > mtime:
                    return True
    return False


def gui_session_ok() -> bool:
    """True if a GUI (Aqua) session is available for the emulator's SDL window.

    CONSERVATIVE — returns True on any uncertainty so it never false-blocks a legitimate run; only
    returns False when macOS clearly reports a non-GUI context (a bare SSH session, a launchd
    daemon). `launchctl managername` reports "Aqua" inside a logged-in desktop session and
    "Background"/"System"/etc. otherwise. Lets a run fail fast with a clear message instead of
    waiting out the 90s boot timeout (the SDL window can't open without an active WindowServer)."""
    try:
        out = subprocess.run(["launchctl", "managername"], capture_output=True, text=True, timeout=3)
        name = out.stdout.strip()
        if name and name != "Aqua":
            return False
    except Exception:
        pass
    return True


def is_configured(emulator: str) -> bool:
    """True if the autoconf build tree has been configured (config.status sits next to the binary).
    `make build-ss` FAILS on an un-configured tree, so the doctor / run can point at the one-time
    `configure` step instead of a cryptic build error."""
    return (Path(emulator).resolve().parent / "config.status").exists()


def _sheepshaver_pids() -> list[int]:
    """PIDs of running SheepShaver instances (empty if none / pgrep absent)."""
    try:
        out = subprocess.run(["pgrep", "-x", "SheepShaver"], capture_output=True, text=True)
        return [int(p) for p in out.stdout.split()]
    except (FileNotFoundError, ValueError):
        return []


def kill_strays(wait: float = 6.0) -> None:
    """Kill any stray SheepShaver AND wait until they are actually gone — only one instance can run
    at a time (they share the disk image, the VNC port, and the SDL window). `pkill` returns before
    the OS has reaped the process and released its file handles, so a bare pkill can leave an instance
    still holding the disk image when the next run launches — the classic cause of a "?" no-boot-disk
    hang. Polling until the PIDs disappear closes that window."""
    try:
        subprocess.run(["pkill", "-9", "-x", "SheepShaver"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except FileNotFoundError:
        return
    deadline = time.monotonic() + wait
    while _sheepshaver_pids() and time.monotonic() < deadline:
        time.sleep(0.2)


def disk_holders(path: str) -> list[int]:
    """PIDs holding `path` open (via `lsof -t`); empty if free or lsof absent."""
    try:
        out = subprocess.run(["lsof", "-t", "--", path], capture_output=True, text=True)
        return [int(p) for p in out.stdout.split()]
    except (FileNotFoundError, ValueError):
        return []


def preflight(*boot_images: str) -> str | None:
    """Run BEFORE launching the emulator. Kill+reap stray instances, then verify none remain and the
    boot image(s) aren't still held open by another process. Returns None if clear, else a
    human-readable reason to abort — so we fail fast with a clear message instead of spinning up a
    doomed session that boots to the "?" no-boot-disk icon (a stray SheepShaver still owns the disk).
    """
    kill_strays()
    stray = _sheepshaver_pids()
    if stray:
        return (f"a SheepShaver instance is still running (pid {stray}) after kill — refusing to "
                f"launch (it would contend for the disk/VNC port and likely '?'-hang). Kill it first.")
    for img in boot_images:
        if not img:
            continue
        holders = disk_holders(img)
        # Exclude our own process just in case; any other holder means the image is in use.
        holders = [p for p in holders if p != os.getpid()]
        if holders:
            return (f"boot image {img!r} is held open by pid(s) {holders} — refusing to launch "
                    f"(SheepShaver couldn't get the disk → would boot to the '?' icon).")
    return None


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
            try:
                os.kill(self._proc.pid, signal.SIGUSR1)
            except ProcessLookupError:
                pass  # the guest exited between poll() and kill() — nothing to signal

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
