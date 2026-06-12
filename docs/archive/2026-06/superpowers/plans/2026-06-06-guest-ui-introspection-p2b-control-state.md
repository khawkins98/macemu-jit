> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# Guest UI Introspection — Plan 2b (Control State + hasParams) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: subagent-driven-development or executing-plans. Steps use `- [ ]`.

**Goal:** For dialog items that are **controls** (button/checkbox/radio/control), emit the live
control **`value`** (checkbox/radio check state 0/1; popup current item) and **`hilite`** (0 = active,
255 = dimmed/inactive, else a highlighted part). Plus, flag static/edit text that contains ParamText
placeholders (`^0`–`^3`) with **`hasParams`** so consumers don't assert on a template. This lets the
harness **assert "is this checkbox checked?"** and **toggle controls**.

**Architecture:** Extend `serialize_dialog_items` in `ui_introspect.cpp`: for control-type items
(DITL types 4/5/6/7), the DITL item's leading 4-byte field is the live **`ControlHandle`** — deref it
to the `ControlRecord` and read `contrlHilite`/`contrlValue`. Pure additive. Python `Item` gains
`value`/`hilite`/`checked`/`has_params`. **Self-validating:** the `ControlRecord.contrlRect` must equal
the DITL item rect we already boot-verified in Plan 2a — the boot test cross-checks them, proving the
ControlHandle deref + offsets without any new external oracle.

**Tech Stack:** C++ (SheepShaver), Python 3 + pytest.

**Scope:** Plan 2b ships control `value`/`hilite` + `checked` convenience + `hasParams` detection.
**Deferred:** actual ParamText *resolution* (`^0`→string) — that needs the `DAStrings`/ParamText
low-mem globals whose layout is uncertain; it is the canonical Backend-B calibration case (Plan 3).
Popup *choice lists* and list-box *rows* (Plan 2c). Menus/parts/depth (Plan 2c).

---

## ⚠️ ControlRecord offsets — VERIFY via the contrlRect cross-check

For a DITL **control** item (type 4=button, 5=checkbox, 6=radio, 7=control), the item's leading 4-byte
field (offset +0 within the item, which Plan 2a's walker currently skips) holds the **`ControlHandle`**
at runtime. Deref `ControlHandle` → master pointer = `ControlRecord`:

| Field | Offset | Type | Use |
|-------|--------|------|-----|
| `contrlRect` | +0x08 | Rect (8) | **cross-check** vs the DITL item rect (must match → proves the deref) |
| `contrlVis` | +0x10 | byte | visible |
| `contrlHilite` | +0x11 | byte | 0=active, 255=inactive/dimmed, 1–253=part highlighted |
| `contrlValue` | +0x12 | int16 | checkbox/radio: 0/1; popup/scroll: current value |
| `contrlMin` | +0x14 | int16 | (not emitted in 2b) |
| `contrlMax` | +0x16 | int16 | (not emitted in 2b) |

These are the classic Inside Macintosh `ControlRecord` offsets. The boot test (Task 3) logs both the
emitted `contrlRect` (call it `crect`) and the item `rect`; **if they match for the choose-disk OK/Cancel
buttons, the offsets are correct.** All reads stay behind `guest_ptr_ok`.

ParamText: detect any of the byte pairs `^0`,`^1`,`^2`,`^3` in a text item's decoded string → set
`hasParams:true`. Do NOT attempt resolution in 2b.

---

## Task 1: C++ — emit control value/hilite + hasParams

**Files:** Modify `SheepShaver/src/ui_introspect.cpp`.

- [ ] **Step 1:** In `serialize_dialog_items`, the per-item loop currently reads the rect/type/text. The
  item's leading 4-byte field is at `p + 0`. For control-type items, deref it as a ControlHandle and
  read the ControlRecord. Add, inside the loop after `type`/`enabled`/`text` are computed and BEFORE
  the per-item `snprintf`:
```cpp
        // Control state: for control-type items (button/checkbox/radio/control) the item's leading
        // 4-byte field is the live ControlHandle. Deref -> ControlRecord; read hilite/value. We also
        // read contrlRect for a self-check (it must equal the globalized item rect). All guarded.
        bool isCtrl = (type == 4 || type == 5 || type == 6 || type == 7);
        bool haveCtrl = false; int cval = 0, chil = 0; int crl = 0, crt = 0, crr = 0, crb = 0;
        if (isCtrl) {
            uint32 ch = ReadMacInt32(p);                  // ControlHandle (item's leading field)
            if (ch && guest_ptr_ok(ch)) {
                uint32 cr = ReadMacInt32(ch);             // -> ControlRecord
                if (cr && guest_ptr_ok(cr) && guest_ptr_ok(cr + 0x18)) {
                    crt = (int16)ReadMacInt16(cr + 0x08); crl = (int16)ReadMacInt16(cr + 0x0A);
                    crb = (int16)ReadMacInt16(cr + 0x0C); crr = (int16)ReadMacInt16(cr + 0x0E);
                    chil = Mac2HostAddr(cr)[0x11];        // contrlHilite (byte)
                    cval = (int16)ReadMacInt16(cr + 0x12);// contrlValue
                    haveCtrl = true;
                }
            }
        }
        // ParamText placeholder detection (no resolution in 2b).
        bool hasParams = (text.find("^0") != std::string::npos || text.find("^1") != std::string::npos
                       || text.find("^2") != std::string::npos || text.find("^3") != std::string::npos);
```
Then in the per-item JSON, after the existing `enabled`/`default` and the optional `text`, append the
new optional fields. Right before the item-closing `j += "}";`, add:
```cpp
        if (haveCtrl) {
            char c[160];
            snprintf(c, sizeof(c),
                ",\"value\":%d,\"hilite\":%d,\"crect\":{\"left\":%d,\"top\":%d,\"right\":%d,\"bottom\":%d}",
                cval, chil, crl + ox, crt + oy, crr + ox, crb + oy);   // crect globalized like rect
            j += c;
        }
        if (hasParams) j += ",\"hasParams\":true";
```
(Note: `ox`/`oy` are the content-origin params already passed to `serialize_dialog_items`; reuse them
to globalize `crect` so it's directly comparable to the item `rect`. Keep the `text` emission exactly
where it is — these new fields go after it, before `}`.)

- [ ] **Step 2: Build + gates.** `cd SheepShaver && make build-ss && make ui-introspect-test && make test-jit` → clean, ALL OK, score=100.

- [ ] **Step 3: JSON sanity.** Re-read the per-item assembly: confirm comma placement stays valid when
  `text`, `value`/`hilite`/`crect`, and `hasParams` are present in any combination (each new chunk
  starts with a leading `,` and the item object opened with no trailing comma after `enabled`/`default`).

- [ ] **Step 4: Commit**
```bash
git add SheepShaver/src/ui_introspect.cpp
git commit -F- <<'EOF'
feat(ss-ui): emit control value/hilite (+ crect cross-check) + hasParams

Control-type DITL items now carry the live ControlRecord value and hilite
(deref the item's ControlHandle), plus a globalized crect for a self-check
against the item rect (proves the deref/offsets at boot). staticText/editText
containing ^0-^3 get hasParams (resolution deferred to Backend B). score=100.
EOF
```

---

## Task 2: Python — parse value/hilite/checked/has_params

**Files:** Modify `SheepShaver/e2e/sse2e/uidump.py`, `tests/test_uidump.py`, the fixture.

- [ ] **Step 1: Fixture.** In `tests/fixtures/ui_two_windows.json`, add a checkbox item to the dialog's
  `items` (and value/hilite to a button) so tests have real values:
```json
        {"index": 4, "type": "checkbox", "rect": {"left":330,"top":250,"right":500,"bottom":270}, "enabled": true, "text": "Apply to all", "value": 1, "hilite": 0},
        {"index": 5, "type": "staticText", "rect": {"left":330,"top":210,"right":700,"bottom":240}, "enabled": false, "text": "Save changes to ^0?", "hasParams": true}
```
(Append inside the existing `items` array — keep valid JSON. Also add `"value":0,"hilite":0` to the
existing button index 1 if you like, but the checkbox is the key test.)

- [ ] **Step 2: Failing tests** (append to `tests/test_uidump.py`):
```python
def test_control_value_and_checked():
    snap = _load()
    dlg = snap.windows[0]
    cb = uidump.find_item(dlg, type="checkbox")
    assert cb.value == 1 and cb.hilite == 0 and cb.checked is True
    # a button with no value field -> value None, checked False
    btn = uidump.find_item(dlg, text="Don't Save")
    assert btn.value is None and btn.checked is False


def test_has_params_flag():
    snap = _load()
    txt = uidump.find_item(snap.windows[0], text_contains="Save changes")
    assert txt.has_params is True
```
Run → FAIL (no `value`/`checked`/`has_params`).

- [ ] **Step 3: Implement** in `uidump.py` `Item.__init__` (additive):
```python
        self.value = d.get("value")          # None unless a control
        self.hilite = d.get("hilite")        # None unless a control; 255 = dimmed
        self.has_params = d.get("hasParams", False)
        self.crect = Rect.from_json(d["crect"]) if "crect" in d else None   # ControlRecord rect (self-check)

    @property
    def checked(self) -> bool:
        """True if this is a checkbox/radio that is on (value != 0)."""
        return self.type in ("checkbox", "radio") and bool(self.value)

    @property
    def dimmed(self) -> bool:
        """True if the control is drawn inactive/dimmed (contrlHilite == 255)."""
        return self.hilite == 255
```
(Put the two `@property` methods on the `Item` class; keep `__repr__`.)

- [ ] **Step 4: Run tests** → all pass.

- [ ] **Step 5: Commit**
```bash
git add SheepShaver/e2e/sse2e/uidump.py SheepShaver/e2e/tests/test_uidump.py SheepShaver/e2e/tests/fixtures/ui_two_windows.json
git commit -F- <<'EOF'
feat(e2e): uidump control state — Item.value/hilite/checked/dimmed/has_params

Parses control value + hilite from the dialog-item JSON; `checked`
(checkbox/radio on) and `dimmed` (hilite==255) conveniences; `has_params`
flags ParamText templates. Offline tests on a checkbox fixture.
EOF
```

---

## Task 3: Boot-verify control offsets via the crect cross-check

**Files:** Modify `SheepShaver/e2e/sse2e/scenario.py` (extend the existing Plan-2a `[ui] choose-disk` log).

- [ ] **Step 1:** `Item.crect` is parsed in Task 2 (above). Extend the choose-disk `[ui]` log block
  (added in Plan 2a) so each control item also prints `val=`/`hil=` and `crect_ok` (does the
  ControlRecord rect equal the item rect — the offset proof). Replace the inline item-format expression
  in that log block with a small local helper:
```python
                def _fmt(it):
                    s = f"{it.type}:{it.text!r}@({it.rect.left},{it.rect.top})"
                    if it.value is not None:
                        ok = (it.crect is not None
                              and it.crect.left == it.rect.left and it.crect.top == it.rect.top
                              and it.crect.right == it.rect.right and it.crect.bottom == it.rect.bottom)
                        s += f" val={it.value} hil={it.hilite} crect_ok={ok}"
                    return s
                items = ", ".join(_fmt(it) for it in dlg.items) or "(none)"
```

- [ ] **Step 2: Offline suite** → all pass.

- [ ] **Step 3: BENCHMARK BOOT VERIFY (authorized, ~4 min).** `pkill -9 -x SheepShaver` if any, then
  `cd SheepShaver && timeout 460 make e2e-bench 2>&1 | tee /tmp/p2b_bench.out`. In the
  `[ui] choose-disk dialog:` line, CONFIRM for the OK/Cancel buttons: a `val=` and `hil=` appear, and
  **`crect_ok=True`** (the ControlRecord rect equals the item rect → the ControlHandle deref + offsets
  are correct). Typical: `hil=0` (active) on enabled buttons, `val=0`. If `crect_ok=False` or
  values are absurd, the ControlRecord offsets are wrong — capture the raw `ss_ui.A.json` dialog object
  and report (do NOT guess-fix). Benchmark must still PASS. `pkill -9 -x SheepShaver` after.

- [ ] **Step 4: Commit**
```bash
git add SheepShaver/e2e/sse2e/scenario.py
git commit -F- <<'EOF'
feat(e2e): boot-verify control offsets via crect cross-check (Plan 2b)

The choose-disk [ui] log now prints control val=/hil= and crect_ok (does
the ControlRecord rect match the item rect) — the self-validating proof
that the ControlHandle deref + ControlRecord offsets are correct.
EOF
```

---

## Self-Review (plan author)

**Spec coverage:** control `value`/`hilite` + `checked`/`dimmed` conveniences (Task 1-2); `hasParams`
detection (Task 1-2); self-validating boot proof via `crect == rect` (Task 3). Matches synthesis Plan-2b
"control state + ParamText flag."

**Deferred (explicit):** ParamText *resolution* (Backend B / Plan 3 — `DAStrings` layout uncertain);
popup choice lists / list rows (Plan 2c); menus/parts/depth (Plan 2c).

**Risk:** ControlRecord offsets are the classic IM layout but new to this codebase. Mitigated by (a)
the `crect == rect` cross-check that boot-proves them, (b) full `guest_ptr_ok` guards incl.
`guest_ptr_ok(cr + 0x18)` so a wrong handle can't crash (only yields no/garbage control fields, which
the cross-check flags), (c) reading control fields ONLY for control-type items.

**Type consistency:** `Item.value/.hilite/.checked/.dimmed/.has_params/.crect`; C++ `crect`/`value`/
`hilite`/`hasParams` JSON keys; the fixture matches. `find_item(type="checkbox")` reuses Plan-2a's
helper.
