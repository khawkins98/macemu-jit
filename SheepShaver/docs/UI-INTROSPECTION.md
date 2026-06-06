# Guest UI Introspection — Canonical Reference

> **Status:** Plan 1 + 2a (dialog items) + 2b (control state) + 2c (menu bar, depth, desktop role) shipped (2026-06-06) · **Track:** A5 (E2E harness) / Track C (Silicon Sheep)
> **Canonical reference for the shipped feature.** Design history and the full aspirational schema
> live in `docs/superpowers/specs/2026-06-06-guest-ui-introspection-design.md`.

---

## 1. What & why

Guest UI introspection is a **host-side, read-only** dump of the running classic Mac OS guest's
on-screen window list, emitted as a nonce-stamped JSON file. It is completely **env-gated** —
zero cost unless `SS_UI_DUMP_DIR` is set.

The key property: classic Mac global screen coordinates map 1:1 to the VNC framebuffer. Every
rect this tool emits is directly clickable over VNC — no coordinate translation, no screenshot,
no OCR.

**Three consumers (current + future):**
1. **E2E test harness** — click an element by name; assert which dialog is showing; check
   z-order clickability. Used by `run_uidump_smoke.py` and `sse2e/uidump.py`.
2. **Ad-hoc human debugging** — "dump what's on screen" while diagnosing a live boot.
3. **Silicon Sheep** (future, Track C) — host-side knowledge of guest windows for
   coherence-lite: guest WindowList polling at 0x9D6 is already proven in the E2E code.

The trigger is a **signal-free file poll** serviced at the guest idle hook — not a Unix signal.
(SIGUSR2 is the nanokernel's interrupt and must not be used for this purpose.)

---

## 2. Name map

| Name | Layer | Role |
|------|-------|------|
| `SS_UI_DUMP_DIR` | env var | Feature gate + dump directory; dormant when unset |
| `SheepShaver/src/ui_introspect.h` | C++ | Public API (`guest_ptr_ok`, `ui_introspect_service`) |
| `SheepShaver/src/ui_introspect.cpp` | C++ | Backend A: WindowList walk + JSON + transport |
| `SheepShaver/src/ui_introspect_text.h` | C++ | MacRoman→UTF-8 + JSON-escape (header-only) |
| `SheepShaver/src/ui_introspect_test.cpp` | C++ | Standalone unit test for the text transforms |
| `make -C SheepShaver ui-introspect-test` | build | Build + run the C++ unit test |
| `SheepShaver/e2e/sse2e/uidump.py` | Python | `snapshot()` handshake + `Snapshot`/`Window`/`Rect` |
| `SheepShaver/e2e/run_uidump_smoke.py` | Python | Working end-to-end example (boots + requests + prints) |
| `ss_ui.req` | artifact | Request file written by the Python consumer |
| `ss_ui.A.json` | artifact | Backend-A window-list snapshot |
| `ss_ui.done` | artifact | Nonce-stamped sentinel (written last) |
| `.ss_ui.req.consumed` | artifact | Consumed request (rename-on-read prevents double-service) |

---

## 3. Enable + quick start

**Enable:** set `SS_UI_DUMP_DIR` to any writable directory before launching the emulator (or in
the child env). The emulator's idle hook polls for `$SS_UI_DUMP_DIR/ss_ui.req` on every
`OP_IDLE_TIME` — zero cost when no request is present.

**Important:** keep the feature off during boot. Set `SS_UI_DUMP_DIR` only **after** the
`[READY]` signal — early boot has a VBL spin-wait that can be starved by the extra `access()`
calls if the req file appears too early. See §8 for details.

```bash
# Launch emulator with introspection enabled:
SS_UI_DUMP_DIR=/tmp/ss_ui ./SheepShaver

# Request a snapshot manually (from another terminal):
echo '{"nonce":"abc1","backends":["A"],"screenshot":false}' > /tmp/ss_ui/ss_ui.req
# Wait for /tmp/ss_ui/ss_ui.done (nonce="abc1"), then read:
cat /tmp/ss_ui/ss_ui.A.json
```

**Python quick start** — run the included smoke:

```bash
cd SheepShaver/e2e
.venv/bin/python run_uidump_smoke.py
```

The smoke boots the isolated ISO config with `SS_UI_DUMP_DIR` set, waits for `[READY]`, requests
a snapshot, and prints each window:

```
snapshot: backend=A schema=1 screen={'width': 800, 'height': 600, 'depth': 0} windows=2 modal=False front=0
  [0] 'Mac OS 8.6 Internal Edition' class=document active=True vis=True bounds=(0,20,800,600) clickable=True
  [1] '' class=document active=False vis=False bounds=(0,0,0,0) clickable=False
```

(Exact window list depends on boot media and guest state. A clean ISO boot typically shows the
desktop window at `(0,20,800,600)` as the front document window.)

---

## 4. JSON schema — Plan 1 emitted subset

Only fields **actually emitted** by the shipped `ui_introspect.cpp` are documented here.
`schemaVersion` stays `1` across plans — additions are backward-compatible. Feature-detect new
fields rather than asserting their absence:

```python
if "menuBar" in snap.raw:   # Plan 2+
    ...
```

### Top-level object

| Field | Type | Notes |
|-------|------|-------|
| `schemaVersion` | integer | Always `1` in Plan 1 |
| `backend` | string | Always `"A"` in Plan 1 |
| `nonce` | string | Echoed from the request; truncated to 64 chars |
| `ticks` | integer | `LMGetTicks()` at snapshot time (guest low-mem `0x016A`) |
| `sysVersion` | string | e.g. `"8.6.0"` — decoded from `SysVersion` at `0x015A` |
| `coords` | string | Always `"global"` — rects are screen-global / VNC pixels |
| `text` | string | Always `"utf-8"` — titles are MacRoman-decoded to UTF-8 |
| `screen` | object | `{"width": N, "height": N, "depth": 0}` — from `CrsrPin` at `0x0834` |
| `screen.depth` | integer | Always `0` in Plan 1; GDevice depth deferred to Plan 2 |
| `modalActive` | boolean | True if the front **visible** window has `modality:"modal"` |
| `frontWindowIndex` | integer | Index of the first **visible** window in `windows[]`; `-1` if all hidden |
| `windows` | array | Front-to-back z-order; may be empty (valid, not an error) |

### Per-window object (`windows[]`)

| Field | Type | Notes |
|-------|------|-------|
| `index` | integer | 0 = frontmost; z-order position in `windows[]` |
| `ptr` | string | e.g. `"0x00abe340"` — `WindowRecord` guest address; NOT a stable identity across snapshots |
| `title` | string | MacRoman→UTF-8; empty string for untitled windows (valid) |
| `windowClass` | string | `"dialog"` (`windowKind==2`) or `"document"` (all others) |
| `modality` | string | For dialogs: `"modal"`, `"movableModal"`, or `"modeless"` (variant-derived). For non-dialogs: always `"none"`. |
| `isDialog` | boolean | True if `windowKind == 2` |
| `active` | boolean | True if the `hilited` byte (`+0x6F`) is set |
| `visible` | boolean | True if the `visible` byte (`+0x6E`) is non-zero |
| `collapsed` | boolean | Always `false` in Plan 1 (window-shade state deferred to Plan 2) |
| `contentBounds` | object | `{"left":N,"top":N,"right":N,"bottom":N}` — `contRgn` bounding box, global |
| `structBounds` | object | `{"left":N,"top":N,"right":N,"bottom":N}` — `strucRgn` bounding box, global |
| `suspect` | boolean | Optional — present and `true` when any field read was wild (bad `strucRgn`/`contRgn` handle, or title had an invalid pointer or control bytes). Treat such windows as best-effort. |

**`modality` detail:** the variant is the high byte of `windowDefProc` at `+0x7E`
(`GetWVariant` semantics): variant 1 or 3 → `"modal"`; variant 5 → `"movableModal"`; any
other variant → `"modeless"`. This is **only applied to dialog windows** (`isDialog:true`).
Non-dialogs are always `"none"`. Note: `movableModal` dialogs do **not** set `modalActive` —
only `"modal"` modality triggers it.

**Dialog items (Plan 2a, shipped 2026-06-06):** dialog windows (`isDialog:true`) additionally emit
`refCon` (int32), `defaultItem` (int16, the `aDefItem` number), and an `items` array — one entry per
DITL item with `index` (1-based), `type` (`button`/`checkbox`/`radio`/`control`/`staticText`/
`editText`/`icon`/`picture`/`userItem`), `rect` (**globalized**, VNC-clickable), `enabled` (false when
the DITL `itemDisable` bit is set), `text` (for button/checkbox/radio/static/edit types), and
`default:true` on the default item. Live-verified against the Speedometer choose-disk dialog (8 items,
`OK`/`Cancel` titles, `refCon='sped'`). Consume via `Window.items` / `find_item` / `click_item`.

**Control state (Plan 2b, shipped 2026-06-06):** control-type items (button/checkbox/radio/control)
additionally emit `value` (checkbox/radio 0/1; popup current), `hilite` (0 = active, **255 = dimmed/
disabled**), and `crect` (the `ControlRecord` rect, globalized — a self-check that equals `rect`).
Text items containing `^0`–`^3` get `hasParams:true`. Consume via `Item.value`/`.hilite`/`.checked`
(checkbox/radio on) / `.dimmed` (hilite==255) / `.has_params`. Live-verified (`crect_ok=True`, dimmed
buttons reported `hil=255`).

**Menu bar (Plan 2c, shipped 2026-06-06):** a top-level `menuBar` object: `{height, menus:[{id,
title, enabled, role?, items:[{index, text, enabled, cmdKey?, submenu?}]}]}`. Each menu: signed
`id`, `title`, `enabled` (enableFlags bit 0), `role:"apple"` (apple-logo title). Each item: `text`,
`enabled` (enableFlags bit k), `cmdKey` (the Command-key char, when `cmdChar>0x20`), `submenu` (id,
when the item opens a hierarchical menu). `screen.depth` now reads the real value from the main
GDevice; the Finder desktop window carries `role:"desktop"`. **Live-verified**: 7 Finder menus with
File/Edit/View/Special and File's ⌘N/⌘O/⌘W. Consume via `Snapshot.menu_bar` / `find_menu_item(snap,
"Open").cmd_key`.

**Not yet emitted (Plan 3 / later):** window `parts` (close/zoom/grow rects), `dialogId` (numeric
resource id — not reliably stored in a live `DialogRecord`; use the item set + `refCon` for identity),
`ParamText` `^0`–`^3` *resolution* (only flagged via `hasParams`; resolution is the Backend-B/Plan-3
calibration case), popup choice-lists / list-box rows, submenu *recursion* (flagged via `submenu` id,
not walked).

---

## 5. The file handshake

The transport is a simple, race-free, signal-free file poll:

```
Consumer (Python)                        Emulator (idle hook)
─────────────────                        ────────────────────
1. Delete stale ss_ui.done (if any)
2. Write .ss_ui.req.tmp (atomic)
3. rename → ss_ui.req                ─► 4. access(ss_ui.req) → present
                                         5. rename → .ss_ui.req.consumed
                                         6. Read nonce from consumed file
                                         7. write_atomic(ss_ui.A.json, data)
                                         8. write_atomic(ss_ui.done, {nonce})
9. Poll ss_ui.done; match nonce ◄────────
10. Read ss_ui.A.json → Snapshot
```

Key properties:
- **Exactly-once service:** the idle hook renames `ss_ui.req` → `.ss_ui.req.consumed` before
  reading it. A second idle trip sees no `ss_ui.req` and is a no-op.
- **Atomic writes:** both artifacts use `write(tmp) + rename(tmp, dst)`. The consumer only
  reads `ss_ui.A.json` after seeing `ss_ui.done` with the matching nonce.
- **windows:[] is valid-empty**, not an error — the guest may legitimately have no windows
  during early boot or after all windows are closed.
- **If the A.json write fails**, no `ss_ui.done` is written and the consumer's `snapshot()`
  call raises `TimeoutError` after the configured timeout.
- **Latency:** approximately one idle round-trip (~16 ms at the 60 Hz VBL cadence; a few
  hundred ms in a loaded system or during boot).

---

## 6. Python API

Module: `sse2e.uidump` (in `SheepShaver/e2e/sse2e/uidump.py`)

### `snapshot(dump_dir, *, timeout=5.0, poll=0.05) -> Snapshot`

Request a fresh snapshot. Writes the request file and polls for `ss_ui.done` with the matching
nonce. Raises `TimeoutError` on timeout.

```python
from sse2e import uidump

snap = uidump.snapshot("/tmp/ss_ui", timeout=10.0)
```

### `Snapshot`

| Attribute | Type | Notes |
|-----------|------|-------|
| `raw` | dict | The full parsed JSON — use for feature-detection (`"menuBar" in snap.raw`) |
| `backend` | str | `"A"` in Plan 1 |
| `nonce` | str | Echo of the request nonce |
| `modal_active` | bool | `modalActive` from JSON |
| `front_index` | int | `frontWindowIndex` from JSON |
| `windows` | list[Window] | All windows, front-to-back |

| Method | Returns | Notes |
|--------|---------|-------|
| `front_window()` | `Window \| None` | Window at `front_index`; None if list is empty |
| `front_dialog()` | `Window \| None` | Front window only if `is_dialog`; else None |
| `find(*, title=None, window_class=None, visible=None)` | `list[Window]` | Exact-match filter on any combination of keyword args |
| `clickable(win: Window)` | bool | True if visible, not collapsed, and not behind a modal |

### `Window`

Attributes: `index`, `title`, `window_class`, `is_dialog`, `active`, `visible`, `collapsed`,
`content_bounds` (`Rect`), `struct_bounds` (`Rect`), `suspect`.

### `Rect`

Attributes: `left`, `top`, `right`, `bottom`.
Properties: `center` → `(x, y)` tuple.
Method: `intersects(other: Rect) -> bool`.

### Copy-paste example

```python
from sse2e import uidump

snap = uidump.snapshot("/tmp/ss_ui", timeout=15.0)

# Is a modal dialog up right now?
if snap.modal_active:
    dlg = snap.front_dialog()
    print(f"Modal dialog: title={dlg.title!r} bounds={dlg.content_bounds}")

# Find the Finder desktop window
desktop_wins = snap.find(window_class="document", visible=True)
for w in desktop_wins:
    cx, cy = w.content_bounds.center
    print(f"  window {w.index!r}: center=({cx},{cy}) clickable={snap.clickable(w)}")

# Feature-detect Plan-2 fields
if "menuBar" in snap.raw:
    print("Plan 2+ snapshot: menu bar available")
```

---

## 7. WindowRecord offset table

These are the offsets read by Backend A from each `WindowRecord` in the z-ordered
`WindowList` chain. The `WindowList` global is at low-mem address `0x09D6`. All reads
are big-endian (PPC Mac OS convention).

| Field | Offset | Type | Notes |
|-------|--------|------|-------|
| `windowKind` | +0x6C | int16 | 2 = `dialogKind`; determines `isDialog`/`windowClass` |
| `visible` | +0x6E | byte | 0/1 |
| `hilited` | +0x6F | byte | 0/1 — the real "active" bit |
| `strucRgn` | +0x72 | RgnHandle | Structure region handle; bounding box = `structBounds` |
| `contRgn` | +0x76 | RgnHandle | Content region handle; bounding box = `contentBounds` |
| `windowDefProc` | +0x7E | Handle (high byte) | High byte = `GetWVariant` code; used for `modality` on dialog windows |
| `titleHandle` | +0x86 | StringHandle | Pascal string (Str255); decoded MacRoman→UTF-8 |
| `nextWindow` | +0x90 | WindowPeek | Next window in z-order chain (front→back) |
| `refCon` | +0x98 | int32 | Window refCon — emitted for dialog windows (Plan 2a); part of dialog identity |

**Region bounding-box read pattern:**
A `Region` (pointed to by an `RgnHandle`) is `{int16 rgnSize; Rect rgnBBox; …}`. The
bounding box lives at `masterPtr + 2`:

```
masterPtr+2  = top    (int16, big-endian)
masterPtr+4  = left
masterPtr+6  = bottom
masterPtr+8  = right
```

`guest_ptr_ok()` (defined in `ui_introspect.h`) validates each dereference before reading:
a handle or master pointer outside Mac RAM or with an odd address sets `suspect:true`.

**Low-mem globals used:**

| Symbol | Address | Type | Notes |
|--------|---------|------|-------|
| `WindowList` | `0x09D6` | WindowPeek | Front window (start of z-order chain) |
| `CrsrPin` | `0x0834` | Rect | Screen bounds (`top/left/bottom/right`) — used for `screen.width`/`height` |
| `Ticks` | `0x016A` | uint32 | Tick count at snapshot time |
| `SysVersion` | `0x015A` | uint16 | BCD `$0860` = 8.6.0 |

---

## 8. Limitations & approximations (Plan 1)

**`windowClass` is binary.** Only `"dialog"` (windowKind == 2) vs. `"document"` (all others).
Floating tool windows, desk accessories, and the desktop window are all `"document"` in Plan 1.
The Finder's desktop `WindowRecord` (typically `(0,20,800,600)`) is correct to emit but
indistinguishable from a document window until Plan 2 adds the `role:"desktop"` tag.

**`modality` is variant-derived for dialogs only, not yet calibrated.** The variant is
extracted from the high byte of `windowDefProc` at +0x7E. This correctly classifies standard
alert/dialog procs (`dBoxProc`=1 → `modal`, `movableDBoxProc`=5 → `movableModal`, plain
dialog=0 → `modeless`). Non-standard WDEFs or third-party DAs may yield unexpected variants.
Backend B (Plan 3) will serve as a calibration oracle.

**`collapsed` is hardcoded `false`.** Window-shade (collapse box) state is not read in Plan 1.

**`screen.depth` is always `0`.** Reading pixel depth requires the `GDevice` (`gdPMap` pixelSize).
Within the port, the offset that would give depth is ambiguous between mono `GrafPort` and
color `CGrafPort`; this is deferred to Plan 2.

**Dialog items shipped (Plan 2a); menus + control state not yet.** Dialog `DITL` items (button/text
rects + titles + enable + default) ARE emitted (see §4). Still deferred: `menuBar`, window `parts`,
control `value`/`hilite`, `ParamText` resolution, and a numeric `dialogId` (use the item set + `refCon`
for identity — see §4).

**`ptr` is not a stable identity.** The `WindowRecord` guest address changes between snapshots
if the Memory Manager relocates handles. Do not use `ptr` as a persistent window key.

**Backend B is Plan 3.** Only Backend A (memory walk) is implemented. The Toolbox-trap oracle
(`Execute68kTrap`) for calibration/comparison is future work.

**Keep the feature OFF during boot.** Set `SS_UI_DUMP_DIR` only **after** the `[READY]` signal
from the idle hook. Early boot contains a VBL spin-wait at `0x5031040c` that polls until the
60 Hz timer fires. The per-idle `access(req_path)` syscall is cheap (< 1 µs), but if the req
file is present during this spin, the service call adds latency and can, in extreme cases, delay
the spin-wait's exit — triggering the VBL-timer-starvation trap documented in `LEARNINGS.md`.

---

## 9. Staging roadmap

| Plan | Status | What it adds |
|------|--------|-------------|
| **Plan 1** | Shipped (2026-06-06) | Window list: title, class, modality, bounds, active/visible, suspect. Signal-free file poll. Python `uidump.py`. |
| **Plan 2a** | Shipped (2026-06-06) | Dialog **items**/DITL: type + globalized clickable rect + title/text + `enabled` (itemDisable) + `default`; window `refCon` + `defaultItem`. Python `Item`/`find_item`/`click_item`. Live-verified vs the choose-disk dialog. Also: harness gates use it (`assert_window`/`click_window`, the benchmark quit-to-Finder check). |
| **Plan 2b** | Shipped (2026-06-06) | Control state: `value`/`hilite`/`crect` (deref the item's `ControlHandle` → `ControlRecord`), `checked`/`dimmed` conveniences, `hasParams` flag on ParamText templates. Self-verified via `crect == rect` on the live dialog. |
| **Plan 2c** | Shipped (2026-06-06) | **Menu bar** (menus + items + cmd-keys + enabled + apple role) via a read-only `MenuList` walk (handle-anchoring, no traps); `screen.depth` via the GDevice; `role:"desktop"` tag. Self-verified at boot (Finder File/Edit titles + ⌘N/⌘O/⌘W). Python `MenuBar`/`find_menu_item`. |
| **Plan 2d** | Deferred | Window parts/hot-zones (close/zoom/grow rects), popup choice-lists / list rows, submenu recursion. |
| **Plan 3** | Deferred | Backend B (Toolbox-trap oracle via `Execute68kTrap`) + static recursion guard, `compare(A,B)` divergence report, `overlay(snapshot, png)` pixel spot-check, socket transport, **`ParamText` resolution** (the `DAStrings` layout is uncertain — Backend B's canonical calibration case). |

`schemaVersion` stays `1` across all plans — additions are backward-compatible field additions.
Feature-detect with `"menuBar" in snap.raw` rather than asserting on schema version.

For the full action plan and prioritized queue, see
`docs/planning/UI-INTROSPECTION-REVIEW-SYNTHESIS.md`.

---

## 10. Troubleshooting

**`TimeoutError: no UI snapshot with nonce … within Xs`**
- Is the emulator running? (`pgrep -x SheepShaver`)
- Is `SS_UI_DUMP_DIR` set in the **child** env (the env of the `SheepShaver` process), not just
  the shell? `os.environ["SS_UI_DUMP_DIR"] = ...` before `Popen` (or set in the parent before
  `exec`). Runner.start() inherits the parent env.
- Did you set it before `[READY]`? Set it after `[READY]` to avoid the boot-time starvation trap.
- `ls $SS_UI_DUMP_DIR`:
  - `ss_ui.req` present but no `.ss_ui.req.consumed` — the idle hook never fired. The guest
    is not reaching `OP_IDLE_TIME`. Check the emulator log for `[BOOT]`; if absent, the
    guest hasn't booted yet.
  - `.ss_ui.req.consumed` present but no `ss_ui.A.json` — the idle hook serviced the request
    but the `write_atomic` to `ss_ui.A.json` failed. Check that `SS_UI_DUMP_DIR` is writable
    and has space.
  - `ss_ui.done` present but wrong nonce — a stale result from a prior request. The consumer
    clears `ss_ui.done` before writing the request; if that unlink failed (permissions), a
    stale sentinel blocks the poll. Remove it manually.

**Bounds look wrong / off by a constant**
- Verify `SS_UI_DUMP_DIR` points at the right emulator instance (only one should run at a
  time — `pkill -9 -x SheepShaver` before starting a new run).
- Compare against a VNC screenshot. Rects should visually overlay the drawn windows. A
  systematic offset usually means the `CrsrPin` screen bounds are reading from a stale or
  pre-init low-mem area — only request snapshots after `[READY]`.
- Rect overlay against pixels is Plan 3's `overlay()`. Until then, the visual check is manual.

**`suspect:true` on all windows**
- The guest is in early boot or a crash state — `WindowRecord` pointers are not yet valid.
  Wait for `[READY]` before requesting.

---

## 11. Testing

**Offline unit tests (no emulator, no assets):**
```bash
cd SheepShaver/e2e
make e2e-test          # runs all 84 offline tests, including test_uidump.py
# or directly:
.venv/bin/python -m pytest -q tests/test_uidump.py
```

**C++ transform unit test:**
```bash
make -C SheepShaver ui-introspect-test
# Output: ui_introspect_text: ALL OK
```

**Integration smoke (requires boot, isolated config — ask user before running on agent):**
```bash
cd SheepShaver/e2e
.venv/bin/python run_uidump_smoke.py
```
