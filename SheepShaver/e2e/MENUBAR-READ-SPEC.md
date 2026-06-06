# Reading the classic Mac OS Menu Bar from guest memory (host-side, read-only)

Spec for walking `MenuList` → `MenuInfo` → menu items the same way we already walk
`WindowList`/`DialogRecord`/`ControlRecord`. All offsets are **byte offsets**, all
multi-byte fields are **big-endian** (68K/PPC). "Read int16/int32/ptr" means read BE and
(for guest pointers/handles) translate through `NATMEM_OFFSET` and deref the handle's
master pointer exactly as we do for window/control handles.

Primary sources:
- Carbon **`Menus.h`** (HIToolbox) — verbatim structs `MenuBarHeader`, `MenuBarMenu`,
  and the legacy `MenuInfo` (fetched + confirmed; see Step 1/Step 2). These document the
  exact in-memory format and definitively resolve the first-word question.
- *Inside Macintosh: Macintosh Toolbox Essentials*, ch. 3 "Menu Manager" (item/`MENU`
  resource format; enableFlags semantics; cmdChar/hierarchical conventions).
- Low-memory globals confirmed against the classic low-mem map (osdata.com `lowmem` /
  Apple `SysEqu`): MenuList `$0A1C`, MBarEnable `$0A20`, TheMenu `$0A26`, MBarHeight `$0BAA`.

---

## ACTIONABLE SPEC (offsets + iteration)

### Step 0 — globals (all confirmed)

| Addr | Name | Type | Use |
|------|------|------|-----|
| `$0A1C` | **MenuList** | **Handle** | the current menu bar's menu list (this is what we walk) |
| `$0A20` | MBarEnable | int16 | non-0 ⇒ a hierarchical/menu-bar-disabled context (system menu bar); usually 0 for app |
| `$0A26` | TheMenu | int16 | menuID currently highlighted (sanity only) |
| `$0BAA` | MBarHeight | int16 | menu bar height in px (== 20 on a healthy boot; cheap liveness check) |
| `$0A24` | MenuFlash | int16 | flash count (incidental) |

`MenuList` IS a `Handle` (handle to the menu-bar list, a.k.a. the "MenuList" structure).
Deref: `listPtr = *(*(Handle)MenuList)` — i.e. read 4-byte handle at host(`$0A1C`),
translate, read the 4-byte master pointer it points to, translate again → `listPtr`.

### Step 1 — the menu-bar list (MenuList) structure

Confirmed verbatim from Carbon `Menus.h`: `struct MenuBarHeader { UInt16 lastMenu; SInt16
lastRight; SInt16 mbResID; }` followed by an array of `struct MenuBarMenu { MenuRef menu;
SInt16 menuLeft; }`.

At `listPtr` — **MenuBarHeader (6 bytes):**

```
+0x00  uint16  lastMenu     ; "Offset in bytes to last menu in array" = numMenus*6
                            ;   (== total byte length of the MenuBarMenu array)
+0x02  int16   lastRight    ; global x of rightmost menu title's right edge
+0x04  int16   mbResID      ; MBDF (menu-bar definition proc) resource ID; 0 if built by code
+0x06  ...     MenuBarMenu[] ; array, each entry = 6 bytes
```

**Per-entry — MenuBarMenu (6 bytes), starting at `listPtr + 0x06`:**

```
+0x00  MenuRef  menu       ; the MenuHandle (Handle to MenuInfo), 4 bytes
+0x04  int16    menuLeft   ; global x of this menu title's left edge
```

**Iteration (the algorithm):**

```
lastMenu = read_int16(listPtr + 0x00)
numMenus = lastMenu / 6                       // integer divide
for i in 0 .. numMenus-1:
    entry      = listPtr + 6 + i*6
    menuHandle = read_ptr(entry + 0x00)        // a MenuHandle (Handle to MenuInfo)
    menuLeft   = read_int16(entry + 0x04)
    if menuHandle == 0: continue               // defensive: skip nil slots
    menuInfoPtr = deref_handle(menuHandle)     // *menuHandle → MenuInfo master ptr
    ... read MenuInfo (Step 2) ...
```

> **Disambiguating the first word (REQUIRED robustness step).** The first word is
> `lastMenu = numMenus*6`, NOT a raw count and NOT a byte offset *to* the last entry
> (it is the offset of the entry *after* the last, == array byte length). Validate before
> trusting it:
> 1. `lastMenu >= 0` and `lastMenu % 6 == 0`.
> 2. `numMenus = lastMenu/6` is sane: `1 <= numMenus <= 64` (a real bar is ~3–12).
> 3. The handle in entry[0] is non-nil and points into the guest System heap.
> 4. Cross-check `entry[0].menuLeft` is small/positive (Apple menu sits at left).
>
> If `lastMenu % 6 != 0` or `numMenus` is wild, you are mis-deref'ing the handle (double
> vs single indirection) — re-check the Handle→masterPtr step, not the field offset.
> Reading entries until `i*6 >= lastMenu` is the canonical termination; do **not** scan
> for a nil terminator (there isn't one — the count is authoritative).
>
> **Why stop exactly at `lastMenu` (not nil-scan):** the *same* MenuList handle can hold
> additional `MenuBarMenu` entries *past* `lastMenu` — those are the inserted
> **hierarchical/popup** menus (`InsertMenu(m,-1)`), which are NOT in the visible bar.
> `lastMenu` is precisely the boundary between bar menus and submenus/popups. An
> implementer who keeps reading past it (thinking the count looks short) will pick up
> submenus as if they were top-level — don't.

### Step 2 — MenuInfo (MenuHandle → master ptr), `menuInfoPtr`

Inside Macintosh `MenuInfo` record (offsets are exact, fixed-layout):

```
+0x00  int16   menuID       ; menu's ID (Apple menu conventionally = 1; app menus vary)
+0x02  int16   menuWidth    ; width in px (-1 until calc'd)
+0x04  int16   menuHeight   ; height in px (-1 until calc'd)
+0x06  Handle  menuProc     ; handle to the MDEF (menu definition proc); 4 bytes
+0x0A  int32   enableFlags  ; item-enable bitmask (see below)
+0x0E  Str255  menuData     ; menu TITLE (Pascal string) immediately followed by items
```

- **Title** starts at `menuInfoPtr + 0x0E`. Byte at +0x0E is the **length** (Pascal),
  followed by that many text bytes (Mac Roman). Apple menu's title is a single byte
  `0x14` (the apple-logo char) with length 1.
- **enableFlags** (int32 @ +0x0A): **bit 0 = whole-menu enabled** (1 = enabled, 0 = whole
  menu greyed). **bits 1..31 = item enable**, where **bit N (N≥1) corresponds to item N**
  (items are 1-based). So item *k* is **disabled** iff `(enableFlags & (1 << k)) == 0`.
  Items beyond 31 are always treated as enabled (only 31 item-bits exist). Read as BE int32.

### Step 3 — menu ITEMS (follow the title inside `menuData`)

Items begin **immediately after the title Pascal string**:

```
itemPtr = menuInfoPtr + 0x0E + 1 + titleLen     // skip length byte + title text
```

**Per-item layout (variable length):**

```
+0x00  uint8    textLen            ; Pascal length of item text
+0x01  textLen  bytes              ; item text (Mac Roman)
                                   ; --- then a fixed 4-byte trailer: ---
+(1+textLen)+0   uint8  iconOrScript ; icon number (item shows icon iconNum+0xFF? see note),
                                     ;   0 = no icon; also script code in some encodings
+(1+textLen)+1   uint8  cmdChar      ; KEYBOARD EQUIVALENT (Command-key char), 0 = none
+(1+textLen)+2   uint8  markChar     ; marking char (0=none, 0x12=✓ check, else the char)
+(1+textLen)+3   uint8  style        ; QuickDraw style byte (bold=1, italic=2, underline=4,
                                     ;   outline=8, shadow=16, condense=32, extend=64)
```

So each item = `1 + textLen + 4` bytes. Advance:
`itemPtr += 1 + textLen + 4`.

**Termination:** the item list ends at a **zero-length item text** (`textLen == 0`). That
terminating zero byte is the single byte that closes `menuData`; there is **no** 4-byte
trailer after it. Stop as soon as you read `textLen == 0`.

```
itemIndex = 0
while true:
    textLen = read_u8(itemPtr)
    if textLen == 0: break              // end of menu
    itemIndex += 1                      // items are 1-based
    text   = read_bytes(itemPtr+1, textLen)
    base   = itemPtr + 1 + textLen
    iconN  = read_u8(base + 0)
    cmd    = read_u8(base + 1)          // Command-key equivalent
    mark   = read_u8(base + 2)
    style  = read_u8(base + 3)
    enabled = (enableFlags & (1 << itemIndex)) != 0
    itemPtr = base + 4
    // safety: bound itemIndex (<256) and itemPtr against handle size to avoid runaway
```

**Decoding the Command-key / special bytes (cmdChar @ trailer+1):**

| cmdChar value | meaning |
|---|---|
| `0x00` | no Command-key equivalent |
| `0x1B` (27) | **hierarchical submenu** (`hMenuCmd`) — then `markChar` holds the submenu's menuID (NOT a mark). Item has a submenu; no real cmd-key/mark. *(0x1B confirmed; the markChar=submenuID convention is the standard Menu Manager meaning.)* |
| `0x1C` (28) | **uncertain — verify before relying.** Reduced-icon / script-code interpretation; markChar reused as a code (rare). |
| `0x1D` (29) | **uncertain — verify before relying.** `'SICN'` small-icon selector (rare). |
| `0x1E` (30) | **uncertain — verify before relying.** Reduced-icon id offset (rare). |
| other (printable, > 0x20) | the literal Command-key char, e.g. `'O'` ⇒ **⌘O**. Display uppercases it. |

Practically (and this rule is safe regardless of the uncertain 0x1C–0x1E rows): **a Cmd-key
exists iff `cmdChar > 0x20`** — i.e. a real printable character, excluding 0 and all of the
0x1B–0x1E meta-codes. When `cmdChar == 0x1B`, surface "has submenu (id = markChar)" instead
of a mark, and skip the check-mark interpretation. For `cmdChar` in 0x1C–0x1E, treat as
"no cmd-key" and don't interpret markChar as a mark until the meaning is verified.

**iconOrScript note:** when nonzero and the item is a normal item, the displayed icon
resource ID is `256 + iconNum` for full icons (`ICON`) historically; for our purposes we
only need to record "has icon = (iconN != 0)" unless icon decoding is in scope.

---

## CAVEATS & STABILITY

### System 7 → Mac OS 8 → Mac OS 9
- **MenuInfo and the item trailer layout are unchanged** across 7/8/9 (and back to System
  6). The `enableFlags` 31-item-bit rule, the 4-byte trailer, and zero-length terminator
  are stable. New work (Appearance Manager, theme-savvy MDEFs in OS 8/9) changes *drawing*
  (`menuProc`/MDEF) and adds menu *colors*/command-key glyphs, **not** the MenuInfo byte
  layout we read. `menuWidth`/`menuHeight` may read `-1` (0xFFFF) until the menu is first
  calculated — treat -1 as "not yet measured", not corruption.
- The menu-bar list (`MenuList`) format (`lastMenu`,`lastRight`,`mbResID`, then 6-byte
  entries) is likewise stable. Under Appearance/themes the bar is still a MenuList handle
  at `$0A1C`.
- **Extended Command-key glyphs:** Mac OS 8.5+ allows a glyph code in the high-bit range
  via the `'xmnu'`/`mctb` mechanism for items whose cmdChar is a control code; for a
  read-only "does it have a Cmd-key" view, the `cmdChar` byte is still authoritative for
  the common case. Only deep glyph fidelity needs the aux tables — out of scope for v1.

### Apple / Help / Application menus
- **Apple menu**: title is the single apple-logo char (`0x14`, length-1 Pascal) — **this is
  the reliable identifier, not the menuID.** Its menuID is often 1, but apps frequently use
  128 (or other values); **do not gate detection on menuID == 1** — match the `0x14` title.
  Its items are the contents of the Apple Menu Items folder + DAs; they appear as normal
  items in `menuData`.
- **Help menu** (menuID **-16490**, `kHMHelpMenuID`) and the **Application/Keyboard menus**
  (negative system menuIDs, e.g. `-16491`/`-16489`) are real entries in the same MenuList.
  Expect **negative menuID** values — read `menuID` as a **signed** int16. Don't assume
  menuIDs are positive or contiguous.
- The clock/control-strip style system menus also live here on 8/9; just iterate what
  `lastMenu` gives you and read titles — don't hardcode an expected menu count.

### Reading safety (same discipline as window/control reads)
- Read at an **idle safe point** only (menus aren't mid-rebuild). A menu being torn
  down/built (`InsertMenu`/`DeleteMenu`) momentarily has inconsistent `lastMenu`.
- Bound every loop: `numMenus <= 64`, `itemIndex < 256`, and keep `itemPtr` within the
  handle's allocated size (GetHandleSize-equivalent if available; else a generous byte cap)
  so a corrupt length byte can't walk off into the heap.

---

## SELF-VALIDATION (the `crect==rect` analogue)

Cheap live cross-checks that confirm offsets are right on a real boot (Finder frontmost):

1. **Title plausibility (strongest):** with Finder active the menu titles must read, in
   order, **"\x14" (Apple), "File", "Edit", "View", "Special", "Help"** (System 8.x Finder;
   8.5+/9 add/rename — but "File"/"Edit" are invariant and always slots 2 and 3). If
   slot-2 title decodes to "File" and slot-3 to "Edit", the MenuList stride (6), the
   MenuInfo title offset (+0x0E), and the Pascal length handling are ALL correct
   simultaneously. This is the menu-bar equivalent of `crect==rect`.
2. **Apple menu marker:** menu[0] title length == 1 and byte == `0x14`. Confirms the +0x0E
   title offset and Pascal-length handling. (Do NOT also require menuID == 1 — it's often
   128; reading `menuID` only confirms +0x00 reads *a* small int, not a specific value.)
3. **`lastMenu % 6 == 0`** and `numMenus` in 4..12 for Finder — confirms the first-word
   interpretation (numMenus*6) rather than a raw count (a raw count of e.g. 6 would give a
   wrong reading and usually fail #1).
4. **MBarHeight ($0BAA) == 20** and **lastRight (`listPtr+0x02`) ≈ screen width** — orthogonal
   liveness checks that the bar is real and our globals are translated correctly.
5. **enableFlags bit 0 == 1** for a normally-usable menu (whole menu enabled). If you find
   a menu greyed in the UI, its bit 0 should be 0 — a behavioral cross-check.
6. **Command-key spot check:** Finder's File menu items historically include **⌘N** (New
   Folder), **⌘O** (Open), **⌘W** (Close) — if your cmdChar decode yields `'N'`,`'O'`,`'W'`
   for the right items, the 4-byte trailer offset (+textLen) and the `cmdChar = trailer+1`
   position are confirmed.

Pass criterion (suggested): checks **#1 (File/Edit), #3, #4** all green ⇒ the layout is
verified; #6 confirms the item trailer specifically.
