> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# Guest UI Introspection — Plan 2a (Dialog Items / DITL) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement task-by-task. Steps use `- [ ]` checkboxes.

**Goal:** Extend Backend A so each **dialog** window also emits its **DITL items** — buttons, checkboxes, radios, static/edit text, icons — each with a **globalized (VNC-clickable) rect**, its **type**, its **title/text**, and `itemEnable`; plus the dialog's `defaultItem` and `refCon`. This is the unlock that lets the harness **click a named button** ("Save", "Don't Save", "OK") and **assert which dialog is up** by its items.

**Architecture:** Pure additive extension of `ui_introspect.cpp`'s window walk — when a window `isDialog`, dereference the `DialogRecord.items` DITL handle and walk it, globalizing each item rect by the window's content-region origin. The Python `uidump.py` `Window` gains an `items` list + helpers (`find_item`, `click_item`). No new transport, no new C++ machinery — same JSON contract, same idle-hook service, same `guest_ptr_ok` guards. **Backend B remains deferred** (it is the eventual calibrator for these offsets/ParamText).

**Tech Stack:** C++ (SheepShaver), Python 3 + pytest.

**Scope:** Plan 2a is the *first* slice of Plan 2. It ships **item geometry + type + text + enable + default**. Deferred to **Plan 2b**: control *state* (`value`/`hilite` from the `ControlRecord`), **ParamText** `^0`–`^3` resolution, `popupMenu`/`listBox` contents. Deferred to **Plan 2c**: menu items, window-part hot-zones, `screen.depth` via GDevice, the `role:"desktop"` tag. Backend B + `compare()`/`overlay()` calibration is **Plan 3**.

---

## ⚠️ Offset table — VERIFY against a live dialog before trusting

Classic `DialogRecord` = a full `WindowRecord` (0x9C/156 bytes) followed by dialog fields. The
`WindowRecord` size is confirmed (refCon @ +0x98 is its last field → record ends at 0x9C; this matches
the offsets already shipped in Plan 1). Dialog fields:

| Field | Offset | Type | Notes |
|-------|--------|------|-------|
| `items` (DITL) | +0x9C | Handle | the item list |
| `textH` | +0xA0 | TEHandle | current editText TextEdit |
| `editField` | +0xA4 | int16 | index of the editText with focus (−1 = none) |
| `editOpen` | +0xA6 | int16 | internal |
| `aDefItem` | +0xA8 | int16 | **default item** number (1-based) |

**DITL resource** (deref the `items` handle → master ptr):
- `int16` at +0 = **(itemCount − 1)** — add 1. (Specialist landmine #1.)
- then each item, in order:
  - `+0`: 4 bytes — placeholder/handle (the item's control/resource handle at runtime; 0 in template)
  - `+4`: `Rect` (8 bytes) = top,left,bottom,right — **window-LOCAL** coords
  - `+12`: `int8` **itemType** — **high bit 0x80 = itemDisable**; mask it off, the low 7 bits are the type. (Specialist landmine #2.)
  - `+13`: `uint8` **dataLength**
  - `+14`: `dataLength` bytes of data; **then pad to an even offset** if `dataLength` is odd. (Specialist landmine #3.)

**itemType low-7-bits → our `type` string:**
| value | type | data is |
|-------|------|---------|
| 4 | `button` | Pascal title |
| 5 | `checkbox` | Pascal title |
| 6 | `radio` | Pascal title |
| 7 | `control` | CNTL resource id (popup/scrollbar — tag generically) |
| 8 | `staticText` | Pascal text (may contain `^0`–`^3`; Plan 2b resolves — set `hasParams` if present) |
| 16 | `editText` | Pascal text |
| 32 | `icon` | ICON resource id |
| 64 | `picture` | PICT resource id |
| 0 | `userItem` | (custom-drawn; no data) |

**Globalization (specialist landmine #4):** item rects are window-LOCAL. Global = local **+ the content
origin**. Use the **`contRgn` bbox top-left** (already read per-window in Plan 1 as `contentBounds`) as
the origin: `global.left = local.left + contentBounds.left`, etc. **Caveat:** this assumes the dialog's
GrafPort `portRect` origin is (0,0) (the overwhelmingly common case; `SetOrigin` can violate it). Note
this in a code comment; the Plan 3 screenshot overlay is what will catch any origin drift.

**Verification fixture:** the benchmark's **"choose a disk" dialog** (reached and held by `e2e-bench`)
is a real DITL dialog — use it to confirm the offsets produce sane items (a button with a plausible
global rect + a title). The offsets above are derived, not yet live-verified; the boot test is the gate.

---

## File Structure
- **Modify `SheepShaver/src/ui_introspect.cpp`** — add a `kDlgItems=0x9C`, `kDlgDefItem=0xA8` to the
  offsets; add a `serialize_dialog_items(win, contentBounds, json)` that walks the DITL; call it inside
  the window loop when `isDialog`; emit `refCon`, `defaultItem`, and the `items[]` array.
- **Modify `SheepShaver/e2e/sse2e/uidump.py`** — `Window` parses `items` (list of `Item`), `refCon`,
  `default_item`; add `Item` (index/type/rect/title/text/enabled/is_default); add `Snapshot.find_item`
  and `Snapshot.click_item`.
- **Modify `SheepShaver/e2e/tests/test_uidump.py`** — extend the fixture with a dialog-with-items;
  test item parse + `find_item`/`click_item`.
- **Modify `SheepShaver/e2e/tests/fixtures/ui_two_windows.json`** — add an `items` array to the dialog
  window (or add a new fixture `ui_dialog_items.json`).

---

## Task 1: C++ — emit DITL items for dialog windows

**Files:** Modify `SheepShaver/src/ui_introspect.cpp`.

- [ ] **Step 1: Add offsets.** In the WindowRecord offset enum add `kWinRefCon` is already present
  (0x98); add a new enum for dialog fields:
```cpp
enum { kDlgItems = 0x9C, kDlgDefItem = 0xA8 };   // DialogRecord fields, past the 0x9C WindowRecord
```

- [ ] **Step 2: Add the DITL walker.** Add above `serialize_snapshot`:
```cpp
// Map a DITL itemType (low 7 bits) to our type string.
static const char *ditl_type_name(int t) {
    switch (t) {
        case 4:  return "button";
        case 5:  return "checkbox";
        case 6:  return "radio";
        case 7:  return "control";
        case 8:  return "staticText";
        case 16: return "editText";
        case 32: return "icon";
        case 64: return "picture";
        default: return "userItem";   // 0 and anything unrecognized
    }
}

// Append the dialog's DITL items (globalized rects) to j. `ox`,`oy` = content-region top-left (the
// local->global origin). defItem = DialogRecord.aDefItem. Returns nothing; appends a JSON array body.
static void serialize_dialog_items(uint32 win, int ox, int oy, int defItem, std::string &j) {
    j += "\"items\":[";
    uint32 ditlH = ReadMacInt32(win + kDlgItems);
    if (!ditlH || !guest_ptr_ok(ditlH)) { j += "]"; return; }
    uint32 ditl = ReadMacInt32(ditlH);
    if (!ditl || !guest_ptr_ok(ditl)) { j += "]"; return; }
    int count = (int16)ReadMacInt16(ditl) + 1;            // stored as N-1
    if (count < 0 || count > 255) { j += "]"; return; }   // junk guard
    uint32 p = ditl + 2;
    for (int i = 0; i < count; i++) {
        if (!guest_ptr_ok(p + 14)) break;                 // item header must be in RAM
        int16 top  = (int16)ReadMacInt16(p + 4),  left  = (int16)ReadMacInt16(p + 6);
        int16 bot  = (int16)ReadMacInt16(p + 8),  right = (int16)ReadMacInt16(p + 10);
        uint8 typeByte = Mac2HostAddr(p)[12];
        bool enabled = (typeByte & 0x80) == 0;            // high bit set = itemDisable
        int  type = typeByte & 0x7F;
        uint8 dlen = Mac2HostAddr(p)[13];
        // text/title data (Pascal) for the text/title item types; resource-id types carry a number.
        std::string text;
        bool isTextItem = (type == 4 || type == 5 || type == 6 || type == 8 || type == 16);
        if (isTextItem && guest_ptr_ok(p + 14 + dlen)) {
            uint8 *d = Mac2HostAddr(p + 14);
            std::string raw;
            for (int k = 0; k < dlen; k++) { uint8 c = d[k]; if (c < 32) { raw.clear(); break; } raw.push_back((char)c); }
            text = macroman_to_utf8(raw);
        }
        if (i) j += ",";
        char b[256];
        snprintf(b, sizeof(b),
            "{\"index\":%d,\"type\":\"%s\",\"rect\":{\"left\":%d,\"top\":%d,\"right\":%d,\"bottom\":%d},"
            "\"enabled\":%s%s",
            i + 1, ditl_type_name(type),
            left + ox, top + oy, right + ox, bot + oy,
            enabled ? "true" : "false",
            (i + 1 == defItem) ? ",\"default\":true" : "");
        j += b;
        if (isTextItem) { j += ",\"text\":\""; j += json_escape(text); j += "\""; }
        j += "}";
        // advance: 4 (handle) + 8 (rect) + 1 (type) + 1 (len) + dlen, padded to even
        uint32 adv = 14 + dlen + (dlen & 1);
        p += adv;
    }
    j += "]";
}
```
*(Note: a DITL item's data padding is "pad the whole item to even"; since the 14-byte header is even,
padding `dlen` to even suffices.)*

- [ ] **Step 3: Call it in the window loop.** In `serialize_snapshot`, inside the per-window object,
  for dialog windows emit `refCon`, `defaultItem`, and the items. After the `structBounds` append and
  before the closing `}` of the window object, add:
```cpp
        if (isDialog) {
            int32 refcon = (int32)ReadMacInt32(win + kWinRefCon);
            int16 defItem = (int16)ReadMacInt16(win + kDlgDefItem);
            char d[64];
            snprintf(d, sizeof(d), ",\"refCon\":%d,\"defaultItem\":%d,", refcon, defItem);
            j += d;
            // content-region top-left is the local->global origin (cb computed above for this window)
            int ox = cb.ok ? cb.left : sb.left;
            int oy = cb.ok ? cb.top  : sb.top;
            serialize_dialog_items(win, ox, oy, defItem, j);
        }
```
Ensure `cb`/`sb` (the content/struct `Rect16`) are still in scope at that point (they are — they're
computed earlier in the loop body). Keep all existing fields; this only ADDS to dialog windows.

- [ ] **Step 4: Build + offline gates.**
  Run: `cd SheepShaver && make build-ss && make ui-introspect-test && make test-jit`
  Expected: clean build; `ui_introspect_text: ALL OK`; `score=100`.

- [ ] **Step 5: Commit**
```bash
git add SheepShaver/src/ui_introspect.cpp
git commit -F- <<'EOF'
feat(ss-ui): emit DITL dialog items (globalized rects) for dialog windows

Backend A now walks a DialogRecord's DITL when isDialog: each item gets a
type, a globalized (content-origin + local) clickable rect, enabled
(itemDisable high bit), and title/text; the window gets refCon +
defaultItem (aDefItem). Honors the DITL landmines (count=N-1, type high
bit, even padding). Offsets to be live-verified against the choose-disk
dialog (Plan 2a boot test). score=100.
EOF
```

---

## Task 2: Python — parse items + `find_item`/`click_item`

**Files:** Modify `SheepShaver/e2e/sse2e/uidump.py`, `tests/test_uidump.py`, the fixture.

- [ ] **Step 1: Extend the fixture.** In `tests/fixtures/ui_two_windows.json`, add to the dialog
  window (index 0) a realistic `items` array + `refCon`/`defaultItem`:
```json
      "refCon": 0, "defaultItem": 1,
      "items": [
        {"index": 1, "type": "button", "rect": {"left":620,"top":420,"right":690,"bottom":440}, "enabled": true, "default": true, "text": "Save"},
        {"index": 2, "type": "button", "rect": {"left":420,"top":420,"right":600,"bottom":440}, "enabled": true, "text": "Don't Save"},
        {"index": 3, "type": "staticText", "rect": {"left":330,"top":300,"right":700,"bottom":400}, "enabled": false, "text": "Save changes?"}
      ]
```

- [ ] **Step 2: Write failing tests** (append to `tests/test_uidump.py`):
```python
def test_dialog_items_parse():
    snap = _load()
    dlg = snap.windows[0]
    assert len(dlg.items) == 3
    assert dlg.items[0].type == "button" and dlg.items[0].text == "Save" and dlg.items[0].is_default
    assert dlg.default_item == 1


def test_find_and_click_item():
    snap = _load()
    vnc = _FakeVnc()        # defined earlier in this file
    pt = uidump.click_item(vnc, snap, snap.windows[0], text="Don't Save")
    assert pt == (510, 430) and vnc.clicks == [(510, 430)]   # center of (420,420,600,440)
    import pytest
    with pytest.raises(AssertionError):
        uidump.find_item(snap.windows[0], text="Nonexistent")
```
Run: `cd SheepShaver/e2e && .venv/bin/python -m pytest -q tests/test_uidump.py` → FAIL (no `Item`/`items`).

- [ ] **Step 3: Implement.** In `uidump.py`, add an `Item` class and parse it in `Window`:
```python
class Item:
    def __init__(self, d: dict):
        self.index: int = d["index"]
        self.type: str = d.get("type", "")
        self.rect = Rect.from_json(d["rect"])
        self.text: str = d.get("text", "")
        self.enabled: bool = d.get("enabled", True)
        self.is_default: bool = d.get("default", False)

    def __repr__(self):
        r = self.rect
        return f"Item[{self.index}] {self.type} {self.text!r} ({r.left},{r.top},{r.right},{r.bottom})"
```
In `Window.__init__`, after the existing fields:
```python
        self.items = [Item(it) for it in d.get("items", [])]
        self.ref_con = d.get("refCon")
        self.default_item = d.get("defaultItem")
```
Add module-level helpers (near `assert_window`):
```python
def find_item(win: "Window", *, text=None, text_contains=None, type=None, default=None):
    """Return the single matching dialog item, or raise AssertionError naming the items present."""
    out = []
    for it in win.items:
        if text is not None and it.text != text: continue
        if text_contains is not None and text_contains not in it.text: continue
        if type is not None and it.type != type: continue
        if default is not None and it.is_default != default: continue
        out.append(it)
    if not out:
        raise AssertionError(f"no item text={text!r} text_contains={text_contains!r} type={type!r} "
                             f"in {win.title!r}; items: {[(i.type, i.text) for i in win.items]}")
    return out[0]


def click_item(vnc, snap: "Snapshot", win: "Window", **criteria) -> tuple[int, int]:
    """Click the center of the matching dialog item over VNC. Refuses if the dialog isn't clickable."""
    if not snap.clickable(win):
        raise AssertionError(f"dialog {win.title!r} not clickable right now")
    it = find_item(win, **criteria)
    if not it.enabled:
        raise AssertionError(f"item {it.text!r} is disabled")
    pt = it.rect.center
    vnc.click(*pt)
    return pt
```

- [ ] **Step 4: Run tests** → `cd SheepShaver/e2e && .venv/bin/python -m pytest -q` → all pass.

- [ ] **Step 5: Commit**
```bash
git add SheepShaver/e2e/sse2e/uidump.py SheepShaver/e2e/tests/test_uidump.py SheepShaver/e2e/tests/fixtures/ui_two_windows.json
git commit -F- <<'EOF'
feat(e2e): uidump dialog items — Item, find_item, click_item

Window now parses DITL items (type/rect/text/enabled/default) + refCon +
default_item. click_item(vnc, snap, win, text="Save") clicks a named
button (refusing if the dialog is occluded or the item disabled). Offline
tests on a dialog fixture.
EOF
```

---

## Task 3: Boot-verify against the live choose-disk dialog (also retires S1.3)

**Files:** Modify `SheepShaver/e2e/sse2e/scenario.py` (add a non-fatal item-log at the choose-disk gate).

- [ ] **Step 1: Log the dialog's items at the benchmark choose-disk gate.** In `run_benchmark`, right
  after the `_drive_until(... "choose-disk dialog")` gate succeeds (and `dump_dir` is set from S1.2),
  add a NON-FATAL snapshot that logs the dialog's items — this is the live verification that the DITL
  offsets are correct AND it gives the choose-disk dialog real identity:
```python
        try:
            snap = uidump.snapshot(dump_dir, timeout=8.0)
            dlg = snap.front_dialog()
            if dlg is not None:
                items = ", ".join(f"{it.type}:{it.text!r}" for it in dlg.items) or "(none)"
                print(f"  [ui] choose-disk dialog: {len(dlg.items)} items: {items}", flush=True)
        except Exception as e:
            print(f"  [ui] dialog snapshot unavailable (non-fatal): {e}", flush=True)
```

- [ ] **Step 2: Offline suite** → `cd SheepShaver/e2e && .venv/bin/python -m pytest -q` → all pass.

- [ ] **Step 3: BENCHMARK BOOT VERIFY (authorized, ~4 min).** `pkill -9 -x SheepShaver` if any, then
  `cd SheepShaver && timeout 420 make e2e-bench 2>&1 | tee /tmp/p2a_bench.out`. CONFIRM: the
  `[ui] choose-disk dialog: N items: button:'...', ...` line appears with **plausible item types +
  a button title + (in the full JSON) global rects on-screen**, and the benchmark still PASSes. This
  is the live proof the DITL walk + globalization are correct. If items look wrong (empty, garbage
  text, count absurd), the offsets need adjustment — capture the raw `ss_ui.A.json` from the run's
  dump dir for diagnosis and report. `pkill -9 -x SheepShaver` after.

- [ ] **Step 4: Commit**
```bash
git add SheepShaver/e2e/sse2e/scenario.py
git commit -F- <<'EOF'
feat(e2e): log + verify choose-disk dialog items at the benchmark gate (S1.3 + Plan 2a)

Non-fatal item-log at the choose-disk gate: live verification that the
DITL walk + rect globalization are correct, and gives the dialog de-facto
identity (its item titles) — the robustness S1.3 wanted, now meaningful
with items. Verified on e2e-bench.
EOF
```

---

## Self-Review (plan author)

**Spec coverage:** dialog items with globalized rects + type + text + enable + default (Task 1-2);
de-facto dialog identity via item titles + refCon (Task 2-3); live verification (Task 3). Matches the
synthesis P2 "dialog items/DITL" item and the test-architect's v2 prerequisite for named-button
clicking.

**Deferred (explicit, not gaps):** control `value`/`hilite` (Plan 2b — needs the ControlRecord offset
table the specialist said to validate separately); ParamText `^0`–`^3` resolution (Plan 2b — Backend B
calibrates); `dialogId` numeric resource id (NOT reliably stored in a live DialogRecord — identity is
the item set + refCon, which Task 2-3 provide); menus/parts/depth/desktop-role (Plan 2c); Backend B +
overlay calibration (Plan 3).

**Risk:** the DITL/DialogRecord offsets are derived, not yet live-verified — Task 3's boot test is the
gate, with a defensive `guest_ptr_ok(p + 14)` per-item guard and count/junk bounds so a wrong offset
can't crash (only produce empty/odd items, which the boot test will catch). The local→global origin
assumes `portRect` origin (0,0); flagged in-code; Plan 3's overlay is the definitive check.

**Type consistency:** `Item`(index/type/rect/text/enabled/is_default), `Window.items/.ref_con/.default_item`,
`find_item`/`click_item`, C++ `serialize_dialog_items`/`ditl_type_name`/`kDlgItems`/`kDlgDefItem` —
used consistently across C++ and Python tasks; the fixture matches the emitted keys
(`refCon`/`defaultItem`/`items[].default`).
