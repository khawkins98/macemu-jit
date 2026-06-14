# Driving the classic-Mac guest from code — the `sse2e` agent API

> **Status:** 📖 Reference · **Created:** 2026-06-07 · **Updated:** 2026-06-07
> **Why this doc exists:** an AI agent (or human) that can run repo Python already has everything it needs to
> *observe and drive* the SheepShaver guest — `from sse2e import uidump, vnc`. No MCP server required. This
> is the "agent surface" (proposal P4a in `docs/archive/2026-06/planning/E2E-TOOLKIT-REVIEW-AND-MCP-PROPOSAL.md`). An MCP
> wrapper only earns its keep for an agent that *cannot* execute repo Python (a remote/sandboxed model).

## The two sensors + the actuator

| | Module | Use it for |
|---|---|---|
| **Structured introspection** | `sse2e.uidump` | windows / dialogs / controls / menus as objects with **global (VNC-clickable) coords** — find & click by *name*. Best for dialog/standard apps. |
| **Screenshot + perceptual hash** | `sse2e.vnc.capture` + `sse2e.imagecmp` | "what's on screen / has it changed / who owns the menu bar" — the fallback when introspection can't see an app (Carbon/fullscreen — see Gotchas). |
| **Input** | `sse2e.vnc` | `click(x,y)`, `key(name)`, `type_text(s)`, `capture(path)`. |

## Read API (`sse2e.uidump`)

```python
from sse2e import uidump

snap = uidump.snapshot(dump_dir, timeout=5.0)   # one Backend-A snapshot (see "how it connects" below)
snap.front_window()            # -> Window | None
snap.front_dialog()            # -> the frontmost dialogKind Window | None
snap.find(title_contains="Speedometer")          # -> [Window]
snap.menu_bar                  # -> MenuBar (.menus, .menu("File"))
snap.screen_depth()            # -> int (1/2/4/8/16/32)
print(snap.render())           # ASCII dump of the whole UI (great for an agent to "look")

# Objects: Window(.title, .window_class, .is_dialog, .visible, .active, .items, .content_bounds ...),
#          Item(.type, .text, .rect (.center), .enabled, .value, .hilite, .checked, .dimmed, .default),
#          MenuBar/Menu/MenuItem(.text, .cmd_key, .enabled).

# Convenience finders / waiters:
win  = uidump.wait_for_window(dump_dir, title_contains="Save", timeout=10)   # poll until it appears
win  = uidump.assert_window(snap, title_contains="Speedometer")              # raise if absent
item = uidump.find_item(win, text="OK")                                      # a control by name
mi   = uidump.find_menu_item(snap, "Open", menu="File")                      # a menu item (+ .cmd_key)
```

## Act API (`sse2e.vnc` + the click helpers)

```python
from sse2e.vnc import Vnc
v = Vnc(port=5950)
v.click(*item.rect.center)     # click a control found above
v.type_text("AltiVec"); v.key("super-o")   # Finder type-select launch (type name -> Cmd-O)
v.key("enter"); v.capture("/tmp/shot.png")

# Or let uidump find + click in one call:
uidump.click_item(v, snap, win, text="Continue")   # find the named control, click its center
uidump.click_window(v, snap, win)                  # click a window's clickable point
```

## A worked example (launch an app, drive a dialog, read a menu)

```python
from sse2e import uidump
from sse2e.vnc import Vnc
v = Vnc(port=5950)
# launch by Finder type-select (volume window already open):
v.type_text("AltiVec"); v.key("super-o")
# wait for / assert a window, click a named button:
snap = uidump.snapshot(dump_dir)
if snap.front_dialog():
    uidump.click_item(v, snap, snap.front_dialog(), text_contains="OK")
# read the menu bar + a command key:
print(uidump.find_menu_item(snap, "Open", menu="File").cmd_key)   # 'O'
```

For the **gated, end-to-end** pattern (boot → launch → time → quit → clean shutdown), read
`run_workload.py` + `scenario.run_workload` rather than hand-rolling sleeps — the harness exists to replace
blind sleeps with signal/pHash gates.

## How it connects (and the load-bearing gotcha)

- **`SS_UI_DUMP_DIR` is a LAUNCH-TIME contract.** Introspection works only if that env var was set *before*
  the emulator was spawned (the idle hook services snapshot requests via a file handshake in that dir). You
  **cannot** bolt introspection onto an emulator that was already booted without it. VNC (a separate socket)
  works on any running instance, so `capture`/`click`/`type` survive "attach"; `snapshot`/`find_item` do not.
- **One emulator at a time.** Instances share prefs/disks/window — `pkill -9 -x SheepShaver` before booting.
  Agents must not boot the user's instance; use the harness's isolated-config path (see `e2e/README.md`).
- **Carbon / fullscreen apps are invisible to introspection.** A Carbon app rendering fullscreen returns
  degenerate windows and its error alert emits no signal — use the **screenshot + menu-bar pHash** path
  instead (`imagecmp.region_phash` / `workload.classify_launch`; see `LEARNINGS.md` 2026-06-06).
- **Coordinates are global** — an `item.rect.center` is a VNC click point 1:1.

## CLI

`make ui-dump` (or `python -m sse2e.uidump <dump_dir>`) prints a live snapshot — the quickest way for an
agent or human to "look at" the guest UI once an emulator is running with `SS_UI_DUMP_DIR` set.
