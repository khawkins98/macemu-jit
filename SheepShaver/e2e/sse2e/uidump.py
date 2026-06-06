"""Consumer for the guest UI introspection dump (Backend A, Plan 1).

Drives the file handshake (request -> idle service -> nonce-stamped artifacts) and parses the
window-list JSON into a queryable Snapshot. See
docs/superpowers/specs/2026-06-06-guest-ui-introspection-design.md.
"""
from __future__ import annotations

import json
import os
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


@dataclass
class Rect:
    left: int
    top: int
    right: int
    bottom: int

    @classmethod
    def from_json(cls, d: dict) -> "Rect":
        return cls(d["left"], d["top"], d["right"], d["bottom"])

    @property
    def center(self) -> tuple[int, int]:
        return ((self.left + self.right) // 2, (self.top + self.bottom) // 2)

    def intersects(self, o: "Rect") -> bool:
        return not (o.left >= self.right or o.right <= self.left
                    or o.top >= self.bottom or o.bottom <= self.top)


class Window:
    def __init__(self, d: dict):
        self._d = d
        self.index: int = d["index"]
        self.title: str = d.get("title", "")
        self.window_class: str = d.get("windowClass", "")
        self.is_dialog: bool = d.get("isDialog", False)
        self.active: bool = d.get("active", False)
        self.visible: bool = d.get("visible", False)
        self.collapsed: bool = d.get("collapsed", False)
        self.content_bounds = Rect.from_json(d["contentBounds"])
        self.struct_bounds = Rect.from_json(d["structBounds"])
        self.suspect: bool = d.get("suspect", False)


class Snapshot:
    def __init__(self, data: dict):
        self.raw = data
        self.backend: str = data.get("backend", "")
        self.nonce: str = data.get("nonce", "")
        self.modal_active: bool = data.get("modalActive", False)
        self.front_index: int = data.get("frontWindowIndex", -1)
        self.windows: list[Window] = [Window(w) for w in data.get("windows", [])]

    def front_window(self) -> Optional[Window]:
        if 0 <= self.front_index < len(self.windows):
            return self.windows[self.front_index]
        return None

    def find(self, *, title=None, window_class=None, visible=None) -> list[Window]:
        out = []
        for w in self.windows:
            if title is not None and w.title != title:
                continue
            if window_class is not None and w.window_class != window_class:
                continue
            if visible is not None and w.visible != visible:
                continue
            out.append(w)
        return out

    def clickable(self, win: Window) -> bool:
        """True if `win` can actually receive a click right now: visible, not collapsed, and not
        sitting behind a modal dialog (windows[] is front->back z-order, so anything after the
        front modal is deactivated)."""
        if not win.visible or win.collapsed:
            return False
        if self.modal_active and not win.active:
            return False
        return True


def snapshot(dump_dir, *, timeout: float = 5.0, poll: float = 0.05) -> Snapshot:
    """Request a fresh snapshot and parse it. Writes a nonce'd request, then polls for ss_ui.done
    carrying that nonce. The emulator must be running with SS_UI_DUMP_DIR=<dump_dir>."""
    d = Path(dump_dir)
    d.mkdir(parents=True, exist_ok=True)
    nonce = uuid.uuid4().hex[:8]

    # Clear any stale sentinel so we never read a prior run's result.
    done = d / "ss_ui.done"
    try:
        done.unlink()
    except FileNotFoundError:
        pass

    # Write the request atomically (temp + rename), then wait.
    req = {"nonce": nonce, "backends": ["A"], "screenshot": False}
    tmp = d / ".ss_ui.req.tmp"
    tmp.write_text(json.dumps(req))
    os.replace(tmp, d / "ss_ui.req")

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if done.exists():
            try:
                if json.loads(done.read_text()).get("nonce") == nonce:
                    data = json.loads((d / "ss_ui.A.json").read_text())
                    return Snapshot(data)
            except (json.JSONDecodeError, FileNotFoundError):
                pass        # mid-write; keep polling
        time.sleep(poll)
    raise TimeoutError(f"no UI snapshot with nonce {nonce} within {timeout}s "
                       f"(is the emulator running with SS_UI_DUMP_DIR={dump_dir}?)")
