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


def test_repr_is_readable():
    snap = _load()
    assert "Snapshot(backend=A" in repr(snap)
    assert "Macintosh HD" in repr(snap.windows[1])


def test_render_layout():
    out = _load().render()
    assert "Macintosh HD" in out and "Screen 1024x768" in out
    assert "FRONT" in out


def test_click_point():
    snap = _load()
    front = snap.windows[0]          # modal dialog, active -> clickable
    behind = snap.windows[1]         # behind the modal -> not clickable
    assert snap.click_point(front) == front.content_bounds.center
    assert snap.click_point(behind) is None


def test_find_title_contains():
    snap = _load()
    assert len(snap.find(title_contains="Macintosh")) == 1
    assert len(snap.find(title_contains="HD")) == 1
    assert snap.find(title="HD") == []        # exact still works (no match)


def test_assert_window_found_and_missing():
    snap = _load()
    w = uidump.assert_window(snap, title="Macintosh HD")
    assert w.window_class == "document"
    import pytest
    with pytest.raises(AssertionError):
        uidump.assert_window(snap, title="Nonexistent Window")


class _FakeVnc:
    def __init__(self): self.clicks = []
    def click(self, x, y): self.clicks.append((x, y))


def test_click_window_clickable_and_blocked():
    snap = _load()
    vnc = _FakeVnc()
    front = snap.windows[0]          # modal dialog, active -> clickable
    pt = uidump.click_window(vnc, snap, front)
    assert pt == front.content_bounds.center and vnc.clicks == [pt]
    import pytest
    with pytest.raises(AssertionError):
        uidump.click_window(vnc, snap, snap.windows[1])   # behind modal -> blocked


def test_dialog_items_parse():
    snap = _load()
    dlg = snap.windows[0]
    assert len(dlg.items) == 5
    assert dlg.items[0].type == "button" and dlg.items[0].text == "Save" and dlg.items[0].is_default
    assert dlg.default_item == 1
    assert snap.windows[1].items == []          # non-dialog window has no items


def test_find_and_click_item():
    snap = _load()
    vnc = _FakeVnc()
    pt = uidump.click_item(vnc, snap, snap.windows[0], text="Don't Save")
    assert pt == (510, 430) and vnc.clicks == [(510, 430)]   # center of (420,420,600,440)
    import pytest
    with pytest.raises(AssertionError):
        uidump.find_item(snap.windows[0], text="Nonexistent")
    with pytest.raises(AssertionError):
        uidump.click_item(vnc, snap, snap.windows[0], text="Save changes?")  # disabled item


def test_control_value_and_checked():
    snap = _load()
    dlg = snap.windows[0]
    cb = uidump.find_item(dlg, type="checkbox")
    assert cb.value == 1 and cb.hilite == 0 and cb.checked is True and cb.dimmed is False
    assert cb.crect is not None and cb.crect.left == cb.rect.left   # crect parsed
    btn = uidump.find_item(dlg, text="Don't Save")
    assert btn.value is None and btn.checked is False               # non-control -> no value


def test_has_params_flag():
    snap = _load()
    txt = uidump.find_item(snap.windows[0], text_contains="Save changes to")
    assert txt.has_params is True


def test_screen_depth_and_role():
    snap = _load()
    assert snap.screen_depth == 8
    assert snap.windows[1].role == "desktop"
    assert snap.windows[0].role is None


def test_menu_bar_parse():
    snap = _load()
    mb = snap.menu_bar
    assert mb is not None and len(mb.menus) == 3
    file_menu = mb.menu("File")
    assert file_menu.id == 129 and len(file_menu.items) == 3
    assert mb.menus[0].role == "apple"


def test_find_menu_item_cmdkey():
    snap = _load()
    it = uidump.find_menu_item(snap, "Open")
    assert it.cmd_key == "O" and it.enabled is True
    assert uidump.find_menu_item(snap, "Print").cmd_key is None
    import pytest
    with pytest.raises(AssertionError):
        uidump.find_menu_item(snap, "Nonexistent Command")
