> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# Guest UI Introspection — Plan 2c (Menu Bar) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: subagent-driven-development or executing-plans. Steps `- [ ]`.

**Goal:** Emit the live **menu bar** — each menu's id/title/enabled and its **items** with text,
enabled state, and **Command-key equivalent** — so the harness can discover "File ▸ Open ⌘O" and fire
the command by its keystroke. The last major app-automation enabler.

**Architecture:** Pure additive top-level `menuBar` object in `ui_introspect.cpp`, read by a read-only
memory walk of `MenuList` ($0A1C) → menu-bar list → `MenuInfo` → items — the approach both research
agents converged on (memory-walk, **no Toolbox traps** — the idle-hook reentrancy the repo already
avoids). Robustness via **handle-anchoring** (validate each entry derefs to a plausible `MenuInfo`;
stop on failure) rather than trusting the top-level stride. Python `uidump.py` gains `MenuBar`/`Menu`/
`MenuItem` + `find_menu_item`. **Self-validating at boot:** with the Finder frontmost, menu slot 2 must
be `"File"` and slot 3 `"Edit"`, and File's items must carry ⌘N/⌘O/⌘W — one check that validates the
6-byte stride, the title offset, and the item trailer together (the `crect==rect` analogue for menus).

**Source of offsets:** `SheepShaver/e2e/MENUBAR-READ-SPEC.md` (verified against Carbon `Menus.h`).

**Tech Stack:** C++ (SheepShaver), Python 3 + pytest.

**Scope:** Plan 2c ships the menu bar + items + cmd-keys + enabled + `role:"apple"`. **Deferred:**
recursing INTO hierarchical submenus (flag `submenu:<id>` only — `cmdChar==0x1B`; recursion via that id
is a clean follow-up), the `'MENU'`-resource offline oracle (the live File/Edit self-check is the gate),
window-part hot-zones (later 2c slice). ParamText resolution stays Plan 3.

---

## Offsets (from MENUBAR-READ-SPEC.md — big-endian, handles deref twice)

- **`MenuList` = `$0A1C`** (Handle). Deref handle → master ptr = `listPtr`. (`MBarHeight` = `$0BAA`.)
- **MenuBar list:** `+0x00 uint16 lastMenu` = **numMenus × 6**; entries start at `listPtr + 6`, each
  **6 bytes**: `+0x00 MenuHandle`, `+0x04 int16 menuLeft`. Iterate `i in 0 .. lastMenu/6 - 1`.
  **Guards:** `lastMenu % 6 == 0` and `numMenus <= 64`, else bail (emit empty menuBar). Stop at
  `lastMenu` (entries beyond are hierarchical/popup, not bar menus).
- **`MenuInfo`** (MenuHandle → master ptr): `+0x00 int16 menuID` (**signed**), `+0x0A int32 enableFlags`
  (**bit 0 = whole menu enabled; bit k = item k enabled**, k 1-based, cap 31), `+0x0E` Str255 **title**
  (length byte at `+0x0E`, chars at `+0x0F`).
- **Items:** start at `menuInfoPtr + 0x0F + titleLen`. Each item: `len` byte + `len` text bytes, then a
  **4-byte trailer** `{iconOrScript, cmdChar, markChar, style}`. A **zero-length** item ends the list
  (no trailer). Next item = `p + 1 + len + 4`.
  - **cmd-key:** `cmdChar > 0x20` → the key char is `(char)cmdChar` (already uppercase, e.g. `'O'`).
  - **submenu:** `cmdChar == 0x1B` → hierarchical; `markChar` = the submenu's menuID (emit `submenu`,
    do not recurse in 2c).
  - `cmdChar <= 0x20` and `!= 0x1B` → no keyboard equivalent (script codes / specials).
- **Apple menu:** title is the single byte `0x14` (apple-logo glyph) — detect by `titleLen==1 &&
  rawTitle[0]==0x14` and tag `role:"apple"` (do NOT rely on menuID).

---

## Task 1: C++ — emit the `menuBar` object

**Files:** Modify `SheepShaver/src/ui_introspect.cpp`.

- [ ] **Step 1: Add the walker** above `serialize_snapshot`:
```cpp
// Walk the live menu bar (MenuList $0A1C) and append a "menuBar" JSON object. Read-only; every deref
// guarded. Handle-anchoring: a menu whose MenuInfo/title looks wild stops the walk (a wrong stride
// surfaces as a bad deref, not garbage output). cmd-keys are the inline cmdChar trailer byte.
static void serialize_menu_bar(std::string &j) {
    uint16 mbarHeight = ReadMacInt16(0x0BAA);
    j += "\"menuBar\":{";
    char hb[48]; snprintf(hb, sizeof(hb), "\"height\":%u,\"menus\":[", mbarHeight); j += hb;
    uint32 listH = ReadMacInt32(0x0A1C);
    if (!listH || !guest_ptr_ok(listH)) { j += "]}"; return; }
    uint32 lp = ReadMacInt32(listH);
    if (!lp || !guest_ptr_ok(lp)) { j += "]}"; return; }
    uint16 lastMenu = ReadMacInt16(lp);                 // = numMenus * 6
    if (lastMenu == 0 || (lastMenu % 6) != 0) { j += "]}"; return; }
    int numMenus = lastMenu / 6;
    if (numMenus > 64) { j += "]}"; return; }
    int emitted = 0;
    for (int i = 0; i < numMenus; i++) {
        uint32 entry = lp + 6 + i * 6;
        if (!guest_ptr_ok(entry + 6)) break;
        uint32 mh = ReadMacInt32(entry);                // MenuHandle
        if (!mh || !guest_ptr_ok(mh)) break;            // handle-anchoring: bad -> stop
        uint32 mi = ReadMacInt32(mh);                   // -> MenuInfo
        if (!mi || !guest_ptr_ok(mi + 0x0E)) break;
        int16 menuID = (int16)ReadMacInt16(mi);
        int32 enableFlags = (int32)ReadMacInt32(mi + 0x0A);
        uint8 *tp = Mac2HostAddr(mi + 0x0E);
        int titleLen = tp[0];
        if (titleLen > 63 || !guest_ptr_ok(mi + 0x0F + titleLen)) break;   // anchoring sanity
        bool isApple = (titleLen == 1 && tp[1] == 0x14);
        std::string title;
        { std::string raw((const char *)tp + 1, titleLen);
          title = isApple ? std::string("\xef\xa3\xbf") /*U+F8FF*/ : macroman_to_utf8(raw); }
        if (emitted++) j += ",";
        char mb[160];
        snprintf(mb, sizeof(mb), "{\"id\":%d,\"enabled\":%s%s,\"title\":\"",
                 menuID, (enableFlags & 1) ? "true" : "false",
                 isApple ? ",\"role\":\"apple\"" : "");
        j += mb; j += json_escape(title); j += "\",\"items\":[";
        // items
        uint32 p = mi + 0x0F + titleLen;
        int k = 0;
        while (true) {
            if (!guest_ptr_ok(p + 1)) break;
            int ilen = Mac2HostAddr(p)[0];
            if (ilen == 0) break;                       // zero-length item = end of menu
            if (ilen > 63 || !guest_ptr_ok(p + 1 + ilen + 4)) break;
            k++;
            uint8 *ip = Mac2HostAddr(p + 1);
            std::string itext = macroman_to_utf8(std::string((const char *)ip, ilen));
            uint8 *tr = Mac2HostAddr(p + 1 + ilen);     // 4-byte trailer
            uint8 cmdChar = tr[1], markChar = tr[2];
            bool itemEnabled = (k <= 31) ? ((enableFlags & (1 << k)) != 0) : true;
            if (k > 1) j += ",";
            char ib[96];
            snprintf(ib, sizeof(ib), "{\"index\":%d,\"enabled\":%s,\"text\":\"",
                     k, itemEnabled ? "true" : "false");
            j += ib; j += json_escape(itext); j += "\"";
            if (cmdChar > 0x20) { char c[24]; snprintf(c, sizeof(c), ",\"cmdKey\":\"%c\"", (char)cmdChar); j += c; }
            else if (cmdChar == 0x1B) { char c[32]; snprintf(c, sizeof(c), ",\"submenu\":%d", (int)markChar); j += c; }
            j += "}";
            p += 1 + ilen + 4;
            if (k > 255) break;                         // runaway guard
        }
        j += "]}";
    }
    j += "]}";
}
```

- [ ] **Step 2: Emit it after `screen`.** In `serialize_snapshot`'s final assembly, the `screen` object
  is appended then `"modalActive":...,"windows":[`. Insert the menu bar between them. Find where the
  `screen` JSON is added to the output string and, immediately after it (with a leading `,`), do:
```cpp
        j += ",";
        serialize_menu_bar(j);
```
  so the top-level order is `...,"screen":{...},"menuBar":{...},"modalActive":...`. Verify the commas:
  whatever currently sits between `screen` and `"modalActive"` must now route through this (no doubled
  comma). Read the final-assembly block (~lines 285-295) and place the call so the result is valid JSON.

- [ ] **Step 3: Build + gates.** `cd SheepShaver && make build-ss && make ui-introspect-test && make test-jit` → clean, ALL OK, score=100.

- [ ] **Step 4: JSON sanity.** Trace the menuBar object: `{"height":N,"menus":[{"id":N,"enabled":b,
  ["role":"apple"],"title":"...","items":[{"index":N,"enabled":b,"text":"...",["cmdKey":"O"|"submenu":N]}]}]}`
  — comma-join of menus (`if (emitted++)`), comma-join of items (`if (k>1)`), empty arrays valid.

- [ ] **Step 5: Commit**
```bash
git add SheepShaver/src/ui_introspect.cpp
git commit -F- <<'EOF'
feat(ss-ui): emit live menu bar (menus + items + cmd-keys + enabled)

Read-only walk of MenuList ($0A1C) -> menu-bar list -> MenuInfo -> items
(offsets per MENUBAR-READ-SPEC.md, Carbon Menus.h). Each menu: id/title/
enabled + role:"apple"; each item: text/enabled/cmdKey (cmdChar>0x20) or
submenu (cmdChar==0x1B). Handle-anchoring stops the walk on a wild deref;
no Toolbox traps. Self-verified at boot (Finder File/Edit titles). score=100.
EOF
```

---

## Task 2: Python — parse the menu bar + `find_menu_item`

**Files:** Modify `SheepShaver/e2e/sse2e/uidump.py`, `tests/test_uidump.py`, the fixture.

- [ ] **Step 1: Fixture.** In `tests/fixtures/ui_two_windows.json`, add a top-level `menuBar`:
```json
  "menuBar": {
    "height": 20,
    "menus": [
      {"id": 128, "enabled": true, "role": "apple", "title": "", "items": [
        {"index": 1, "enabled": true, "text": "About This Computer"}
      ]},
      {"id": 129, "enabled": true, "title": "File", "items": [
        {"index": 1, "enabled": true, "text": "New Folder", "cmdKey": "N"},
        {"index": 2, "enabled": true, "text": "Open", "cmdKey": "O"},
        {"index": 3, "enabled": false, "text": "Print"}
      ]},
      {"id": 130, "enabled": true, "title": "Edit", "items": []}
    ]
  },
```
(Insert as a top-level key alongside `screen`/`windows` — keep valid JSON.)

- [ ] **Step 2: Failing tests** (append to `tests/test_uidump.py`):
```python
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
    assert uidump.find_menu_item(snap, "Print").cmd_key is None   # no key, disabled
    import pytest
    with pytest.raises(AssertionError):
        uidump.find_menu_item(snap, "Nonexistent Command")
```
Run → FAIL.

- [ ] **Step 3: Implement** in `uidump.py`:
```python
class MenuItem:
    def __init__(self, d: dict):
        self.index: int = d["index"]
        self.text: str = d.get("text", "")
        self.enabled: bool = d.get("enabled", True)
        self.cmd_key = d.get("cmdKey")        # e.g. "O" for Cmd-O; None if no key
        self.submenu = d.get("submenu")       # submenu menu id, or None
    def __repr__(self):
        k = f" Cmd-{self.cmd_key}" if self.cmd_key else ""
        return f"MenuItem[{self.index}] {self.text!r}{k}{'' if self.enabled else ' (disabled)'}"


class Menu:
    def __init__(self, d: dict):
        self.id: int = d.get("id")
        self.title: str = d.get("title", "")
        self.enabled: bool = d.get("enabled", True)
        self.role = d.get("role")
        self.items = [MenuItem(it) for it in d.get("items", [])]


class MenuBar:
    def __init__(self, d: dict):
        self.height = d.get("height")
        self.menus = [Menu(m) for m in d.get("menus", [])]
    def menu(self, title: str):
        for m in self.menus:
            if m.title == title:
                return m
        return None
```
In `Snapshot.__init__`, add:
```python
        self.menu_bar = MenuBar(data["menuBar"]) if "menuBar" in data else None
```
Module-level helper:
```python
def find_menu_item(snap: "Snapshot", text: str, *, menu=None) -> "MenuItem":
    """Find a menu item by exact text (optionally within a named menu). Raises if not found.
    Use `it.cmd_key` to fire it: vnc.key_combo('super', it.cmd_key.lower())."""
    if snap.menu_bar is None:
        raise AssertionError("no menu bar in snapshot")
    for m in snap.menu_bar.menus:
        if menu is not None and m.title != menu:
            continue
        for it in m.items:
            if it.text == text:
                return it
    raise AssertionError(f"no menu item {text!r}"
                         + (f" in menu {menu!r}" if menu else "")
                         + f"; menus: {[m.title for m in snap.menu_bar.menus]}")
```

- [ ] **Step 4: Run tests** → all pass.

- [ ] **Step 5: Commit**
```bash
git add SheepShaver/e2e/sse2e/uidump.py SheepShaver/e2e/tests/test_uidump.py SheepShaver/e2e/tests/fixtures/ui_two_windows.json
git commit -F- <<'EOF'
feat(e2e): uidump menu bar — MenuBar/Menu/MenuItem + find_menu_item

Parses the menuBar (menus + items + cmd_key + enabled + role); find_menu_item
locates a command and exposes its Cmd-key so the harness can fire it by
keystroke. Offline tests on a File/Edit menu fixture.
EOF
```

---

## Task 3: Boot-verify the menu offsets against the live Finder menu bar (the self-check)

**Files:** Modify `SheepShaver/e2e/sse2e/scenario.py` (extend the `run_lifecycle` `[ui]` corroboration).

- [ ] **Step 1: Log + assert the menu bar in the lifecycle smoke.** In `run_lifecycle`, the S1.1 block
  snapshots the settled desktop and logs windows. Extend it (still NON-FATAL) to also log the menu bar
  and the File/Edit self-check:
```python
            mb = snap.menu_bar
            if mb is not None:
                titles = [m.title for m in mb.menus]
                fm = mb.menu("File")
                fkeys = {it.text: it.cmd_key for it in fm.items if it.cmd_key} if fm else {}
                file_ok = titles[1:3] == ["File", "Edit"] if len(titles) >= 3 else False
                print(f"  [ui] menu bar: {len(mb.menus)} menus {titles} | File/Edit_ok={file_ok} "
                      f"File keys={fkeys}", flush=True)
```
  (Place inside the existing `try:` so any hiccup stays non-fatal. With the Finder frontmost at the
  settled desktop, slot 1 is Apple (``), slot 2 `File`, slot 3 `Edit`.)

- [ ] **Step 2: Offline suite** → all pass.

- [ ] **Step 3: LIGHT BOOT VERIFY (authorized, ~15-30s — `make e2e`, NOT the benchmark).**
  `pkill -9 -x SheepShaver` if any, then `cd SheepShaver && timeout 240 make e2e 2>&1 | tee /tmp/p2c_menu.out`.
  CONFIRM the `[ui] menu bar:` line shows: a sane menu count (~6-8 on the Finder), titles including
  `File`/`Edit`/`View`/`Special`, **`File/Edit_ok=True`**, and File `keys` containing recognizable
  entries (e.g. an Open→`O`, New→`N`/`W`). `File/Edit_ok=True` is THE proof the menu offsets (stride +
  title +0x0E + item trailer) are correct. The smoke must still reach `PASS: clean lifecycle`. If
  `File/Edit_ok=False` or titles are garbage, the offsets need adjustment — capture the menu bar from
  the run's `ss_ui.A.json` (find the dump dir via `ls -dt $TMPDIR/ss-ui-* /tmp/ss-ui-* 2>/dev/null | head`)
  and report; do NOT guess-fix. `pkill -9 -x SheepShaver` after.

- [ ] **Step 4: Commit**
```bash
git add SheepShaver/e2e/sse2e/scenario.py
git commit -F- <<'EOF'
feat(e2e): boot-verify menu offsets via Finder File/Edit self-check (Plan 2c)

run_lifecycle's [ui] corroboration now logs the live menu bar and asserts
slot-2/3 titles are File/Edit (+ File cmd-keys) — the self-validating proof
that the MenuList stride + MenuInfo title offset + item trailer are correct.
EOF
```

---

## Self-Review (plan author)

**Spec coverage:** menu bar with menus + items + cmd-keys + enabled + apple role (Task 1-2); the live
File/Edit self-check that validates the offsets (Task 3). Matches synthesis Plan-2c "menu items +
cmdChar," built on the two research agents' converged recommendation (memory-walk + handle-anchoring,
no traps).

**Deferred (explicit):** submenu *recursion* (flagged via `submenu:<id>`, not walked); the
`'MENU'`-resource offline oracle (live File/Edit check is the gate); window parts; ParamText resolution
(Plan 3).

**Risk:** the top-level `MenuList` stride is the one format ambiguity — mitigated by (a) `lastMenu%6==0`
+ `numMenus<=64` guards, (b) **handle-anchoring** (each entry must deref to a MenuInfo with a sane title
or the walk stops), (c) the File/Edit boot self-check that fails loudly if the stride/offsets are wrong,
(d) full `guest_ptr_ok` guards so a wrong offset yields empty/short output, never a crash. cmd-key rule
`cmdChar>0x20` is robust to the uncertain 0x1C-0x1F meta-codes.

**Type consistency:** C++ keys `menuBar.height/menus[].id/enabled/role/title/items[].index/enabled/text/
cmdKey/submenu`; Python `MenuBar/Menu/MenuItem`, `Snapshot.menu_bar`, `find_menu_item`, `MenuItem.cmd_key`
— used consistently; the fixture matches the emitted keys.
