"""Offline tests for the uidump consumer (no emulator/boot)."""
import json
from pathlib import Path

from sse2e import uidump

FIX = Path(__file__).parent / "fixtures" / "ui_two_windows.json"


def _load():
    return uidump.Snapshot(json.loads(FIX.read_text()))


def test_parse_windows_and_front():
    snap = _load()
    assert len(snap.windows) == 2
    assert snap.modal_active is True
    assert snap.front_window().title == ""           # the modal dialog is front
    assert snap.front_window().is_dialog is True


def test_find_by_title():
    snap = _load()
    hd = snap.find(title="Macintosh HD")
    assert len(hd) == 1
    assert hd[0].window_class == "document"


def test_clickable_respects_modal():
    snap = _load()
    front = snap.windows[0]          # modal dialog, active
    behind = snap.windows[1]         # document behind it, inactive
    assert snap.clickable(front) is True
    assert snap.clickable(behind) is False   # behind a modal -> not clickable


def test_clickable_invisible_or_collapsed():
    snap = _load()
    w = snap.windows[1]
    w.visible = False
    assert snap.clickable(w) is False
    w.visible = True
    w.collapsed = True
    assert snap.clickable(w) is False


def test_front_dialog():
    snap = _load()
    dlg = snap.front_dialog()
    assert dlg is not None and dlg.is_dialog
    assert dlg.index == 0
