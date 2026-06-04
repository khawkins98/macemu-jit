"""Parse deterministic lifecycle signals from SheepShaver stderr."""
from __future__ import annotations

import re
from dataclasses import dataclass

_BOOT_RE = re.compile(
    r"\[BOOT\] idle frontApp='(?P<app>[^']*)' modal=(?P<modal>\d+) ticks=\d+ \((?P<secs>[\d.]+)s\)"
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


def saw_clean_shutdown(text: str) -> bool:
    """True if the log shows a clean guest shutdown (both signatures present)."""
    return bool(_SHUTDOWN_RE.search(text)) and bool(_ATEXIT_RE.search(text))
