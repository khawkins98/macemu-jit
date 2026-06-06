"""Consumer for the guest UI introspection dump (Backend A, Plan 1).

Drives the file handshake (request -> idle service -> nonce-stamped artifacts) and parses the
window-list JSON into a queryable Snapshot. See
docs/superpowers/specs/2026-06-06-guest-ui-introspection-design.md.
"""
from __future__ import annotations

import json
import os
import sys
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

    def __repr__(self):
        cb = self.content_bounds
        flags = ",".join(f for f, v in [("dialog", self.is_dialog), ("active", self.active),
                                        ("visible", self.visible), ("collapsed", self.collapsed),
                                        ("suspect", self.suspect)] if v)
        return (f"Window[{self.index}] {self.title!r} {self.window_class}/{self._d.get('modality','?')} "
                f"({cb.left},{cb.top},{cb.right},{cb.bottom})" + (f" {flags}" if flags else ""))


class Snapshot:
    def __init__(self, data: dict):
        self.raw = data
        self.backend: str = data.get("backend", "")
        self.nonce: str = data.get("nonce", "")
        self.modal_active: bool = data.get("modalActive", False)
        self.front_index: int = data.get("frontWindowIndex", -1)
        self.windows: list[Window] = [Window(w) for w in data.get("windows", [])]

    def __repr__(self):
        return (f"Snapshot(backend={self.backend} windows={len(self.windows)} "
                f"modal={self.modal_active} front={self.front_index})")

    def front_window(self) -> Optional[Window]:
        if 0 <= self.front_index < len(self.windows):
            return self.windows[self.front_index]
        return None

    def front_dialog(self) -> Optional[Window]:
        """The front window if it is a dialog, else None (the dialog accepting input right now)."""
        fw = self.front_window()
        return fw if (fw is not None and fw.is_dialog) else None

    def find(self, *, title=None, title_contains=None, window_class=None, visible=None) -> list[Window]:
        out = []
        for w in self.windows:
            if title is not None and w.title != title:
                continue
            if title_contains is not None and title_contains not in w.title:
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

    def click_point(self, win: "Window") -> Optional[tuple[int, int]]:
        """The (x, y) point to click `win` (its content-rect center), or None if not clickable."""
        return win.content_bounds.center if self.clickable(win) else None

    def render(self) -> str:
        """A compact human-readable layout of the snapshot (for CLI / debugging)."""
        lines = [f"Screen {self.raw.get('screen', {}).get('width', '?')}x"
                 f"{self.raw.get('screen', {}).get('height', '?')}"
                 f"  backend={self.backend}  modal={self.modal_active}  "
                 f"sys={self.raw.get('sysVersion', '?')}"]
        if not self.windows:
            lines.append("  (no windows)")
        for w in self.windows:
            cb = w.content_bounds
            front = " <- FRONT" if w.index == self.front_index else ""
            flags = " ".join(f for f, v in [("dialog", w.is_dialog), ("active", w.active),
                                            ("hidden", not w.visible), ("collapsed", w.collapsed),
                                            ("suspect", w.suspect)] if v)
            lines.append(f"  [{w.index}] {w.title!r:30} {w.window_class:8} "
                         f"({cb.left:4},{cb.top:4})-({cb.right:4},{cb.bottom:4}) "
                         f"{'CLICKABLE' if self.clickable(w) else 'blocked  '} {flags}{front}")
        return "\n".join(lines)


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


def wait_for_window(dump_dir, *, title=None, title_contains=None, window_class=None,
                    timeout=30.0, poll=0.5) -> "Snapshot":
    """Poll snapshots until a window matching the criteria appears; return that Snapshot.
    Requires the emulator running with SS_UI_DUMP_DIR=<dump_dir>."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            snap = snapshot(dump_dir, timeout=min(5.0, max(0.2, deadline - time.monotonic())))
        except TimeoutError:
            break
        if snap.find(title=title, title_contains=title_contains, window_class=window_class):
            return snap
        time.sleep(poll)
    raise TimeoutError(f"no window matching title={title!r} title_contains={title_contains!r} "
                       f"window_class={window_class!r} within {timeout}s")


def _main(argv=None) -> int:
    import argparse
    p = argparse.ArgumentParser(prog="python -m sse2e.uidump",
                                description="Snapshot and print the live guest window tree.")
    p.add_argument("dump_dir", nargs="?", default=os.environ.get("SS_UI_DUMP_DIR"),
                   help="dump dir (default: $SS_UI_DUMP_DIR). The emulator must be running with "
                        "SS_UI_DUMP_DIR set to this directory.")
    p.add_argument("--timeout", type=float, default=10.0)
    args = p.parse_args(argv)
    if not args.dump_dir:
        print("error: give a dump_dir or set SS_UI_DUMP_DIR", file=sys.stderr)
        return 2
    try:
        snap = snapshot(args.dump_dir, timeout=args.timeout)
    except TimeoutError as e:
        print(str(e), file=sys.stderr)
        return 1
    print(snap.render())
    return 0


if __name__ == "__main__":
    sys.exit(_main())
