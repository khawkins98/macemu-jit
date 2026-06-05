"""Parse deterministic lifecycle signals from SheepShaver stderr."""
from __future__ import annotations

import re
from dataclasses import dataclass

_BOOT_RE = re.compile(
    # The win=/title= fields (added 2026-06-05) sit between modal= and ticks=; tolerate them.
    r"\[BOOT\] idle frontApp='(?P<app>[^']*)' modal=(?P<modal>\d+).*? ticks=\d+ \((?P<secs>[\d.]+)s\)"
)
# The clean-exit signature: "Shutdown complete." plus the atexit session block.
_SHUTDOWN_RE = re.compile(r"Shutdown complete\.")
_ATEXIT_RE = re.compile(r"PPC-JIT-A64: session ")


@dataclass(frozen=True)
class BootReady:
    front_app: str
    modal: bool
    secs: float


def parse_boot_ready(line: str) -> BootReady | None:
    """Return a BootReady if the line is the [BOOT] idle signal, else None."""
    m = _BOOT_RE.search(line)
    if not m:
        return None
    return BootReady(
        front_app=m.group("app"),
        modal=m.group("modal") != "0",
        secs=float(m.group("secs")),
    )


def is_desktop_ready(ev: BootReady) -> bool:
    """True only when idle at the Finder desktop (not blocked on a modal dialog)."""
    return ev.front_app == "Finder" and not ev.modal


_FRONTAPP_RE = re.compile(r"frontApp='(?P<app>[^']*)'")


def front_app(line: str) -> str | None:
    """The frontmost-app name from a [BOOT]/[APP] line, else None.

    The emulator emits `[APP] frontApp='X'` whenever the frontmost app changes, so the harness can
    wait for a specific app (e.g. Speedometer auto-launching) deterministically, not by timing.
    """
    m = _FRONTAPP_RE.search(line)
    return m.group("app") if m else None


_APP_RE = re.compile(
    r"\[APP\] frontApp='(?P<app>[^']*)' modal=(?P<modal>\d+)"
    r"(?: win=(?P<win>0x[0-9a-fA-F]+))?(?: title='(?P<title>[^']*)')? ticks=(?P<ticks>\d+)"
)


@dataclass(frozen=True)
class AppEvent:
    app: str
    modal: bool
    win: str      # front-window pointer, e.g. "0x12ab34" ("" if none)
    title: str    # front-window title ("" if untitled / unavailable)
    ticks: int    # guest Ticks (60/s) — for timing instrumentation


def parse_app(line: str) -> AppEvent | None:
    """Parse an `[APP]` signal line into its fields (app/modal/win/title/ticks), else None.

    `win` (the front-window address) changes whenever one dialog replaces another even if the app
    and modal flag are unchanged — the primitive for gating each benchmark drive-step on the actual
    window transition instead of a fixed sleep.
    """
    m = _APP_RE.search(line)
    if not m:
        return None
    return AppEvent(
        app=m.group("app"),
        modal=m.group("modal") != "0",
        win=m.group("win") or "",
        title=m.group("title") or "",
        ticks=int(m.group("ticks")),
    )


def is_app_dialog(line: str) -> bool:
    """True if this `[APP]` line reports a modal dialog is up (`modal=1`).

    Used to detect dialogs appearing (e.g. Speedometer's "tests are done!" — the
    benchmark-finished hook) without screenshots/OCR.
    """
    return line.lstrip().startswith("[APP]") and "modal=1" in line


def saw_clean_shutdown(text: str) -> bool:
    """True if the log shows a clean guest shutdown (both signatures present)."""
    return bool(_SHUTDOWN_RE.search(text)) and bool(_ATEXIT_RE.search(text))
