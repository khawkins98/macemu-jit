> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone shipped
>

# Guest UI Introspection — Design

> **Status:** Design approved 2026-06-06 · **Track:** A5 (E2E harness) adjacent / Track C (Silicon Sheep) enabler
> **Spec for:** a host-side tool that emits a structured JSON tree of the running classic Mac OS guest's
> on-screen UI (windows, dialog items, menus) with exact, VNC-clickable coordinates — *without* a screenshot.

> **⚠️ Implementation status (2026-06-06):** This is the DESIGN. Plan 1 (the walking skeleton)
> shipped a SUBSET — see the canonical reference `SheepShaver/docs/UI-INTROSPECTION.md` for what
> actually exists. In particular: (1) the trigger is the **signal-free file poll** (§3), NOT SIGUSR2
> (any §4.1/§8 SIGUSR2 mention is obsolete — SIGUSR2 is the nanokernel's interrupt); (2) §7's
> "v1 full-fidelity" describes the full DESIGN, not Plan 1 (dialog items/parts/menus/dialogId are
> Plan 2); (3) the shipped Python API is `find(title=/window_class=/visible=)` + `clickable()->bool`,
> not §4.2's `find(text=/role=/type=)`/`assert_dialog`/`default_key` (those are Plan 2/3).

---

## 0. Problem & motivation

Today the harness can click fixed coordinates, read files off the disk, and learn the frontmost app
name + modal state from the idle hook (`emul_op.cpp`'s `[APP]`/`[BOOT]`/`[READY]` signals). What it
**cannot** do is answer *"what is on screen right now, and where is each element?"* — it can't target a
button by name, assert which dialog is up, or know whether a control is checked. The only current path
is a screenshot processed expensively (OCR/vision), which is slow, fragile, and structurally blind
(it can't tell a checkbox's value or a button's enabled state).

Classic Mac OS keeps the **entire** window/control/dialog/menu structure in well-documented Toolbox
data structures (Window Manager `WindowList`, Dialog Manager `DITL`/`DialogRecord`, Control Manager
control lists, Menu Manager `MenuList`). We already read some of these from the host safely
(`emul_op.cpp`: `e2e_guest_ptr_ok`, `ReadMacInt16/32`, `Mac2HostAddr`, Pascal-string decode, walking
`WindowList` at `$9D6`, reading `windowKind` at `+$6C` and `titleHandle` at `+$86`). This design
extends that proven, read-only machinery into a full **UI introspection** capability: a JSON tree with
exact global-coordinate rectangles for every element.

**Key property:** classic Mac **global screen coordinates map 1:1 to the VNC framebuffer** (the screen
is the GrafPort). So every rect this tool emits is directly clickable over VNC — no coordinate
translation.

### Consumers (general capability, not harness-only)
1. **E2E test harness** (primary, now) — click an element by name/role; assert which dialog is showing
   and in what state.
2. **Ad-hoc human debugging** — "dump what's on screen" while diagnosing.
3. **Silicon Sheep** (future, Track C) — host-side knowledge of guest windows for coherence-lite.

The output contract is therefore designed to be **stable and consumer-agnostic**, not shaped to the
harness.

---

## 1. Architecture

A single **UI Introspection Controller** in the emulator with two interchangeable backends that emit
the **same JSON schema**, so they can be diffed positionally. The C++ side is a *read-only,
comparison-free producer* — it does the serialization transforms it must (rect globalization, part
geometry, ParamText resolution) but holds **no** querying, diffing, or calibration logic; all of that
lives in Python.

```
                    ┌─────────────────────────────────────────────┐
   request file     │  emul_op.cpp — serviced at next guest idle    │
   (polled at idle) │  (heap quiescent; Execute68kTrap is safe)     │
   {backends,nonce, │                                               │
    screenshot}     │   UISnapshotController                        │
                    │   ├── Backend A: memory-walk  ──┐             │
                    │   │   (reuse e2e_guest_ptr_ok,   │  one        │
                    │   │    ReadMacIntN, Mac2HostAddr)│  JSON       │
                    │   ├── Backend B: trap-call ──────┤  schema     │
                    │   │   (Execute68kTrap: FrontWindow,│           │
                    │   │    GetWTitle, GetDItem, …)     │           │
                    │   └── screenshot: trigger only ───┘            │
                    └───────────────┬───────────────────────────────┘
                                    ▼
        /tmp/ss_ui.A.json   /tmp/ss_ui.B.json   /tmp/ss_ui.png   /tmp/ss_ui.done
        (all stamped with the request nonce; .done written last)
                                    ▼
                    ┌─────────────────────────────────────────────┐
   Python           │  sse2e/uidump.py — the consumer/oracle        │
                    │   • snapshot(): request → wait nonce → parse  │
                    │   • query: front_dialog/find/assert/clickable │
                    │   • compare(A,B) → divergences (+ allowlist)  │
                    │   • overlay(snapshot, .png) → pixel spot-check │
                    └─────────────────────────────────────────────┘
```

### Why this split
- **C++ stays read-only and comparison-free** — it produces labeled data (A's tree, B's tree, a PNG)
  via the necessary serialization transforms, but no querying/diffing. It cannot crash or perturb the
  guest (Backend A is pure reads; Backend B calls only read-only, non-allocating Toolbox accessors).
- **Python owns the intelligence** — querying, the A-vs-B-vs-pixels comparison, the divergence report
  with an allowlist. This mirrors the repo's existing `SS_JIT_VERIFY` differential-oracle culture
  (interp-vs-JIT REGDUMP diff). It doubles as a standing regression test for Backend A's offset/
  geometry correctness.
- **Both backends service at the idle hook** (the `OP_IDLE_TIME` / `SynchIdleTime` patch), where the
  Memory Manager is quiescent and `Execute68kTrap` is safe to call from host context.

### The three oracles (each catches a different failure class)
| Oracle | What it is | Calibrates |
|--------|-----------|-----------|
| **A — memory-walk** | Fast default; reads struct offsets directly | *the thing under test* |
| **B — trap-call** | Slower; calls the OS's own accessors | A's offsets + geometry + ParamText |
| **Screenshot** | Pixel ground truth | that rects correspond to drawn pixels |

Backend A is framed as *fast-but-fallible*; Backend B as the *structural oracle*. The schema never
implies A is ground truth.

### Feasibility (confirmed against the codebase)
- **Backend A** extends existing, shipping reads in `emul_op.cpp` (`WindowList` walk, `windowKind`,
  `titleHandle`, safe-deref gate, Pascal-string decode).
- **Backend B** uses `Execute68kTrap(trap, &r)` / `Execute68k()` — used throughout the tree
  (`audio.cpp`, `adb.cpp`, `ether.cpp`'s `CallMacOS*`) to call guest Toolbox routines from host
  context. The only constraint is running from the emulator's CPU thread at a controlled point — which
  is exactly the idle hook.

---

## 2. The JSON schema (the contract)

One schema, emitted identically by both backends so a positional diff is trivial. **All rects are in
global screen coordinates = VNC framebuffer pixels** (declared by `"coords":"global"`). **All text is
UTF-8** (declared by `"text":"utf-8"`). `windows[]` is in **true front→back z-order**.

### 2.1 Top level
```json
{
  "schemaVersion": 1,
  "backend": "A",                       // "A" (memory-walk) | "B" (trap-call)
  "nonce": "7f3a",                       // echoes the request; stamped in every artifact + .done
  "ticks": 1234567,                      // guest Ticks ($16A) at capture
  "sysVersion": "8.6.0",                 // from SysVersion ($015A), BCD-decoded
  "coords": "global",                    // contract: every rect is global-screen = VNC pixels
  "text": "utf-8",                       // contract: every string is UTF-8 (see Apple-glyph policy)
  "screen": { "width": 1024, "height": 768, "depth": 8 },
  "menuBar": { ... },                    // §2.2
  "modalActive": true,                   // a modal dialog is up → windows behind it are deactivated
  "frontWindowIndex": 1,                 // index into windows[] of the front window (-1 if none)
  "windows": [ ... ]                     // §2.3, true front→back z-order
}
```

### 2.2 Menu bar
```json
"menuBar": {
  "height": 20,                          // MBarHeight ($0BAA); 0 before the Finder draws it
  "active": false,                       // false when a modal dialog suppresses the menu bar
  "menus": [
    { "id": 1,  "title": "", "enabled": true, "role": "apple" },
    { "id": 2,  "title": "File",   "enabled": true },
    { "id": 3,  "title": "Edit",   "enabled": true },
    { "id": 32, "title": "Help",   "enabled": true, "role": "help" },
    { "id": 33, "title": "",       "enabled": true, "role": "application" }
  ]
}
```
- `role` ∈ `apple | help | application` for the menus that sit **outside** the left-to-right title
  flow (right-aligned on Mac OS 8/9, icon-only). Absent for ordinary menus.
- Menu **items** (with `cmdChar`) are **v2** — v1 emits menu titles + state only.

### 2.3 Window
```json
{
  "index": 0,
  "ptr": "0x00abc120",                   // guest WindowRecord ptr — correlation/debug ONLY, not identity
  "title": "Macintosh HD",               // GetWTitle; "" is valid (most dialogs/alerts)
  "windowClass": "document",             // WDEF/procID-derived: document|dBox|movableDBox|floating|rounded|...
  "modality": "none",                    // none | modal | movableModal  (orthogonal to windowClass)
  "isDialog": false,                     // true ⇒ this is a DialogRecord; "items" present
  "active": false,                       // is this the active (front, non-deactivated) window?
  "visible": true,                       // raw WindowRecord visible bit (NOT "user can see/click")
  "collapsed": false,                    // windowshade-collapsed: title bar visible, content dead
  "contentBounds": { "left":100,"top":100,"right":500,"bottom":400 },  // global content rect
  "structBounds":  { "left":100,"top":80, "right":500,"bottom":400 },  // global structure rect (incl. title bar)
  "parts": {                             // hot-zones for FindWindow-style actions; rect null when absent
    "titleBar":    { "left":100,"top":80,"right":500,"bottom":99 },
    "closeBox":    { "left":108,"top":84,"right":120,"bottom":96 },
    "zoomBox":     { "left":480,"top":84,"right":492,"bottom":96 },
    "collapseBox": { "left":465,"top":84,"right":477,"bottom":96 },
    "growBox":     { "left":488,"top":388,"right":500,"bottom":400 }
  },
  "suspect": false                       // optional; present only when a deref looked like junk
  // non-dialog windows OMIT "items" entirely (no empty-array lie)
}
```

For **dialog** windows (`isDialog:true`), add:
```json
  "dialogId": 128,                       // DLOG/ALRT resource id — the DURABLE assert handle
  "refCon": 0,                           // window refCon (secondary identity)
  "defaultItem": 1,                      // DialogPeek aDefItem (NOT "item 1 by convention")
  "cancelItem": 2,                       // GetDialogCancelItem / convention (Esc/Cmd-.)
  "items": [ ... ]                       // §2.4
```

### 2.4 Dialog item (DITL)
```json
{
  "index": 1,                            // 1-based DITL item number
  "type": "button",                      // see enum below
  "rect": { "left":620,"top":420,"right":690,"bottom":440 },   // GLOBAL (window origin added)
  "title": "Save",                       // for buttons/checkboxes/radios/controls
  "text": "Save changes to “Untitled”?",             // for static/edit text (ParamText-resolved)
  "hasParams": false,                    // true if ^0..^3 were present (and, if unresolved, still are)
  "itemEnable": true,                    // DITL enable bit: does ModalDialog REPORT clicks here?
  "value": 0,                            // control value (checkbox/radio: 0/1; popup: current choice)
  "hilite": 0,                           // control hilite state (255 = dimmed/inactive)
  "default": true,                       // this is the default item
  "cancel": false,                       // this is the cancel item
  "focused": false,                      // editText with the keyboard focus + selection
  "note": null                           // e.g. "custom-drawn; structure opaque" for userItem
}
```
Only the fields relevant to a `type` are emitted (a `button` has no `value`/`focused`; a `staticText`
has no `itemEnable` semantics worth asserting, etc.).

**`type` enum:** `button | checkbox | radio | control | popupMenu | listBox | staticText | editText |
icon | picture | userItem`.
- `popupMenu` (CDEF popup) and `listBox` (LDEF) are tagged distinctly even though each is one DITL
  item containing many choices/rows; their **contents** (choice list / rows) are **v2**. v1 emits the
  current `value`.
- `userItem` is custom-drawn and structurally opaque — it is **marked** (so a consumer knows "clickable
  hot-zone I can't describe") rather than silently omitted.

### 2.5 Conventions & policies
- **`enabled` is split** into `itemEnable` (DITL: does `ModalDialog` report the click?) and control
  `hilite`/`value` (is it dimmed / checked?). These are genuinely different and conflating them lies.
- **`ptr` is NOT identity** — the system reuses `DialogRecord`s (StandardFile, alert pool). Assert on
  `dialogId`/`refCon`, never `ptr`. `ptr` is for debugging/correlation only.
- **ParamText:** static text in a DITL holds `^0`–`^3` placeholders substituted at display time from
  the ParamText globals (`DAStrings`). v1 **resolves** them host-side (full-fidelity); `hasParams`
  records that substitution occurred. This is a case where Backend B (calling the drawn-text accessor)
  is inherently truthful and calibrates A's manual substitution.
- **MacRoman → UTF-8:** all titles/text are MacRoman `Str255`; transcode to UTF-8 for valid JSON. The
  **Apple-logo glyph 0xF0** has no standard Unicode mapping → emit **U+F8FF** (the private-use
  codepoint Apple itself uses).
- **`suspect:true`** is set on any node where a deref looked like junk (reusing A's existing
  `title_valid` junk-detection). The Python oracle treats `suspect` divergences as expected-noise, not
  failures.
- **Coordinate space is declared once** (`"coords":"global"`) and is uniform — no per-field ambiguity.
- **Out of scope for v1 (explicitly):** the desktop itself and disk/Trash/Finder icons (not windows;
  live in the Finder's icon views / Desktop database). Acknowledged, deferred (see §7).

---

## 3. Trigger & transport

**v1 transport = on-demand JSON file via a polled request file, designed so a socket/stream can wrap
the same producer later** (no schema change) for Silicon Sheep.

**Why not a signal?** SheepShaver already uses both POSIX user signals: **`SIGUSR1`** is the E2E
clean-shutdown hook (`main_unix.cpp`), and **`SIGUSR2` is the nanokernel's interrupt mechanism**
(`main_unix.cpp:273` — *not* free). So the trigger is **signal-free**: the idle hook polls for a
request file. This is also portable (no Darwin-only `SIGINFO`) and matches the repo's env-gated
diagnostics pattern (`SS_JIT_DIAG_LOG`).

### Handshake (file-poll, nonce-based, race-free)
1. The feature is **off unless `SS_UI_DUMP_DIR` is set** (an absolute dir). When unset the idle hook
   does nothing — zero added cost. When set, all artifacts live under that dir.
2. Consumer writes a **request file** `$SS_UI_DUMP_DIR/ss_ui.req` **atomically** (write a temp file in
   the same dir, then `rename()` into place), JSON:
   ```json
   { "nonce": "7f3a", "backends": ["A"], "screenshot": false }
   ```
   Defaults if a field is absent: `backends:["A"]`, `screenshot:false`, `nonce:""`.
3. The **idle hook**, when `SS_UI_DUMP_DIR` is set, `stat()`s for `ss_ui.req` each idle (a cheap host
   syscall). On finding it, it **renames it away** (consumes it, so it services exactly once) and
   produces the snapshot at that same quiescent, `Execute68kTrap`-safe point. Latency ≈ one idle
   round-trip (typically < 100 ms). Servicing completes within the single idle call (Backend A is
   synchronous reads; Backend B's accessor calls are synchronous too).
4. The controller writes the requested artifacts, **each stamped with the request `nonce`**:
   `ss_ui.A.json`, `ss_ui.B.json` (if `"B"` requested), `ss_ui.png` (if `screenshot`). Each is written
   to a temp name and **atomically renamed** into place.
5. The controller writes the sentinel **`ss_ui.done`** *last*, containing the same `nonce`.
6. Consumer polls for `ss_ui.done` **carrying the matching nonce**, then reads the artifacts. A stale
   `.done` from a prior request (wrong nonce) is ignored — no race, and `.json`/`.png` are guaranteed
   to come from the *same* idle frame.

### States
- **File present** = the tool ran. `windows:[]` is a valid "nothing on screen," not an error.
- **`.done` never appears within the timeout** = the tool failed (or the guest never idled). Consumer
  reports a timeout; it does not hammer (each poll costs one idle round-trip — documented for harness
  authors so they set sane timeouts).

### Paths
All artifacts live under `$SS_UI_DUMP_DIR` (the same env that enables the feature), so the isolated
E2E config keeps its artifacts under its own run dir. (Mirrors the existing `SS_JIT_DIAG_LOG`
env-gating pattern.) No hardcoded `/tmp` path; the harness sets the dir per run.

---

## 4. The Python oracle layer (`sse2e/uidump.py`)

This is where querying and calibration live; the C++ side only produces data.

### 4.1 Snapshot
```
snapshot(backends=["A"], screenshot=False, timeout=…) -> Snapshot
```
Generates a fresh nonce, writes the request, sends `SIGUSR2`, waits for `ss_ui.done` with that nonce,
parses the artifact(s). Always returns (empty tree is valid). Raises on timeout only.

### 4.2 Query helpers (on a parsed Snapshot)
- `front_dialog()` → the front dialog window, or None.
- `find(text=…, role=…, type=…, window=…)` → matching node(s).
- `assert_dialog(id=128)` → raise unless a dialog with that `dialogId` is front.
- `clickable(node)` → bool, computed from z-order + bounds + `active`/`modalActive` (a visible button
  behind a modal, or in a deactivated window, is **not** clickable). Returns the click point (rect
  center) when true.
- Prefer-keystroke helpers: `default_key()`/`cancel_key()` → "Return"/"Escape" for the front dialog
  (sending keys is more reliable than clicking; the schema surfaces `default`/`cancel` to enable this).

### 4.3 Compare / calibrate
```
compare(a: Snapshot, b: Snapshot) -> [Divergence]    # ranked, with an allowlist
```
Positional diff of two same-schema trees (A vs B). An **allowlist** absorbs known-expected divergences
(any `suspect` node, reused-`ptr`, and any field we knowingly defer in one backend). Mirrors the
`SS_JIT_VERIFY` false-positive allowlist. This is both a calibration tool and a standing regression
test for Backend A's offset/geometry correctness.

### 4.4 Overlay (pixel ground truth)
```
overlay(snapshot: Snapshot, png_path) -> annotated_png
```
Draws each element rect (labeled) onto the screenshot — the pixel-truth spot-check, and a high-value
human debugging artifact (immediately shows a globalization bug as a misaligned box).

---

## 5. Testing

- **C++ pure transforms (unit-testable):** factor Backend A's risky logic into pure functions —
  `local_to_global_rect`, `macroman_to_utf8` (incl. the 0xF0 policy), `windowkind_to_class`, WDEF
  `part_rects(windowClass, structBounds, flags)`, `paramtext_resolve`. These are the bug-prone bits;
  test them in isolation against hand-computed expectations.
- **Python offline tests (no boot):** parser, query helpers, `compare` + allowlist, `clickable`
  occlusion math — fixture JSON in, assertions out. Joins the existing ~79-test offline suite
  (`make e2e-test`).
- **Integration calibration test (under the harness):** raise a known dialog (the Shut Down confirm,
  or Speedometer's "done" dialog), capture **A + B + screenshot**, assert `compare(A,B)` is clean
  modulo allowlist, and assert the overlay rects land on real pixels. This is the end-to-end proof
  that A's geometry is right, and the regression that catches an offset drift across OS versions.

---

## 6. Risks & mitigations

| Risk | Mitigation |
|------|-----------|
| **Struct offsets are version-sensitive** (WindowRecord/ControlRecord/DialogRecord/MenuInfo) | Offsets are fixed + documented across System 7–9; Backend B (OS accessors) calibrates them; the integration test catches drift. |
| **Item rects emitted window-local** (the classic off-by-title-bar bug) | C++ globalizes all item rects (add window origin); `"coords":"global"` declared; overlay test makes any miss visible immediately. |
| **Handle relocation during a read** (A walks compactible handles) | Service only at the idle safe point; `e2e_guest_ptr_ok` bounds every deref; `suspect:true` flags dodgy reads; A-vs-B disagreement is *expected* and allow-listed. |
| **`Execute68kTrap` reentrancy (Backend B)** | Call only read-only, non-allocating accessors, only from the idle hook (the emulator's own CPU thread at a controlled point), exactly as existing audio/adb code does. |
| **Backend B perturbs guest state** | Restrict B to non-mutating traps (FrontWindow, GetWTitle, GetDItem, CountMItems, GetMenuItemText, GetDialogItemText). No allocation, no UI mutation. |
| **Stale `.done` / mismatched `.json`/`.png`** | Request nonce stamped into every artifact + the sentinel; atomic rename; sentinel written last. |
| **`visible` ≠ "user can see/click"** | Separate `visible` (raw bit) from `active`/`collapsed`/`modalActive` + z-order; `clickable()` computes the real answer. |

---

## 7. Staging

**v1 — full-fidelity (this spec):**
- Backend A: full window list in true z-order with `active`/`visible`/`collapsed`/`modalActive`,
  `windowClass`+`modality`, `contentBounds`+`structBounds`, **window-part hot-zone rects**, dialog
  identity (`dialogId`/`refCon`), dialog items with global rects + `itemEnable`/`value`/`hilite`/
  `default`/`cancel`/`focused`, **ParamText resolution**, menu-bar titles + roles + state, MacRoman→
  UTF-8, `suspect`.
- Backend B: covers the **high-value calibration fields** — window list, dialog identity, titles, item
  text (FrontWindow/GetWTitle/GetDItem/GetDialogItemText/CountMItems). Full per-field parity with A is
  **not** required; B is the oracle for the fields most likely to be wrong in A.
- Transport: on-demand JSON file + nonce handshake; optional screenshot.
- Python: `uidump.py` (snapshot, query, `compare`, `overlay`) + offline tests + the integration
  calibration test.

**v2 — enrichment:**
- Standalone **window control lists** (a document window's scrollbars/buttons: value, thumb position).
- Menu **items** + `cmdChar` (so menu-driven automation can pick a command).
- Popup-menu **choice lists** + list-box **rows**.
- Radio-button **group inference** (DITL radios have no inherent grouping).
- Occlusion-geometry helper (compute visible regions from z-order + bounds).

**v3 — reach & integration:**
- TextEdit / static-text **body text** content.
- **Socket / streaming transport** for Silicon Sheep (wraps the v1 producer).
- Pulled-down / contextual menu capture (transient `MenuHandle`s).
- Desktop / disk / Trash **icon model** (Finder icon views).

---

## 8. File-level structure (for the implementation plan)

- `SheepShaver/src/ui_introspect.cpp` / `.h` (new) — the `UISnapshotController`, both backends, JSON
  serializer, pure transforms. Keeps `emul_op.cpp` thin (it just calls `ui_introspect_service()` from
  the idle hook + sets the request flag from the `SIGUSR2` handler).
- `SheepShaver/src/emul_op.cpp` (modify) — the idle hook calls `ui_introspect_service()` (which polls
  for the request file when `SS_UI_DUMP_DIR` is set); reuse existing `e2e_guest_ptr_ok` / Pascal-string
  helpers (factor them out into the new module so both files share them).
- `SheepShaver/e2e/sse2e/uidump.py` (new) — snapshot/query/compare/overlay.
- `SheepShaver/e2e/tests/test_uidump.py` (new) — offline parser/query/compare/occlusion tests.
- `SheepShaver/e2e/tests/test_ui_transforms.*` — unit tests for the C++ pure transforms (mechanism per
  the plan; a small fixture-driven self-test, e.g. an `SS_UI_SELFTEST` path, or extracted into a
  testable form).
- Docs: a reference doc under `SheepShaver/docs/` (schema + handshake + Toolbox offset table) once v1
  lands; link from the README env-var table and CLAUDE.md key-docs.

---

## 9. Open questions (resolved during design)

- *Host memory-walk vs guest trap injection?* → **Both** (A default, B oracle); the differential is the
  point.
- *Transport?* → On-demand file now, socket later (same producer).
- *Primary driver?* → General capability; harness first, debugging + Silicon Sheep designed-for.
- *v1 depth?* → **Full-fidelity** (user decision 2026-06-06), including window-part rects + ParamText
  resolution.
- *Coordinate space?* → Global = VNC; declared in-schema; all item rects globalized in C++.
