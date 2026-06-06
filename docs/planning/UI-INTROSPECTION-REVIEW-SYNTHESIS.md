# Guest UI Introspection — Multi-Lens Review Synthesis & Action Plan

> **Date:** 2026-06-06 · **Updated:** 2026-06-06 · **Status:** 🟡 review actions landed; now in S4 (first real-world workload)
>
> **Progress (2026-06-06):** P0/P1 (correctness, docs, DX, S1 gate-wiring) ✅ · **Plan 2 fully shipped** —
> 2a dialog items, 2b control state, 2c menu bar/depth/desktop, **plus control-list items for non-dialog
> windows** (`ac4363a2`, the dogfooding fix below). **S4 underway:** Fractal Carbon installed + CarbonLib 1.6
> resolved by *driving the SMI installer via introspection* (`run_carbonlib_install.py`) and extracted for
> reuse. **Still open:** the generic `scenario.run_workload` + the Fractal Carbon run itself; Plan 3
> (Backend B oracle + ParamText + socket).
> **Scope:** Synthesis of a four-specialist review (technical writer, developer advocate, emulation
> specialist, E2E test architect) of the shipped **Plan 1 (walking skeleton)** of guest UI
> introspection. Source feature: spec `docs/superpowers/specs/2026-06-06-guest-ui-introspection-design.md`,
> plan `docs/superpowers/plans/2026-06-06-guest-ui-introspection-p1-skeleton.md`, code
> `SheepShaver/src/ui_introspect*.{h,cpp}` + `SheepShaver/e2e/sse2e/uidump.py`.

## Verdict

The foundation is **sound and ships**. The WindowRecord/Region/Str255 offsets were validated
*arithmetically* (not from memory) and confirmed **color-QuickDraw-safe** (CGrafPort is byte-identical
to GrafPort from `portRect` onward — both 0x6C). Reading at the idle hook is **genuinely crash- and
timing-safe** (a synchronous `EMUL_OP` trap; no guest instruction executes mid-walk, so the Memory
Manager cannot relocate a handle under us; `WindowRecord`s are non-relocatable `NewPtr` blocks). The
C++↔Python contract matches field-for-field; the nonce handshake is race-free. The live boot smoke
returned correct, globalized bounds.

But three correctness issues and a set of doc inaccuracies must be addressed before building Plan 2 on
top, and the reviews surfaced a high-value, low-effort path to richer E2E.

---

## P0 — Correctness fixes (cheap, unambiguous; do before Plan 2)

From the emulation specialist (all verified against `ui_introspect.cpp`):

1. **`modalActive`/`modality` is a lie.** `windowKind == 2` means "front window is a `DialogRecord`,"
   **not** "modal." A *modeless* dialog (Find, a floating tool window) is `dialogKind` yet non-modal —
   windows behind it stay active/clickable. The code collapses `modality = isDialog ? "modal" : "none"`.
   Real modality comes from the window **variant** (`GetWVariant` semantics; the variant is in the
   `windowDefProc` field at +0x7E): `dBoxProc(1)`/`altDBoxProc(3)` ⇒ modal, `movableDBoxProc(5)` ⇒
   movableModal, else modeless. **Fix:** derive modality from the variant; emit `"unknown"` (not
   `"modal"`) when unrecognized; set `modalActive` only for a true modal front window. **Verify against
   the live Shut Down dialog** (a `dBoxProc` modal) — it must report `modal`. Backend B will later
   calibrate this (a one-shot A-vs-`GetWVariant` canary).

2. **`frontWindowIndex` ignores visibility.** `FrontWindow()` is the first *visible* window, but the
   `WindowList` head can be an invisible (hidden-but-linked) window. The code sets `front_index = 0`
   unconditionally. **Fix:** first window with `visible != 0`.

3. **Junk-title heuristic was dropped + dead code.** The model it was based on
   (`e2e_front_window_title`) rejects `len > 63` **and** non-printable bytes (cooperative-MT
   pseudo-windows momentarily frontmost yield junk titles). `window_title()` kept only a bounds check,
   and its `if (len > 255)` is **dead** (`len` is a `uint8`). **Fix:** restore `len > 63` + printable
   sanity → mark `suspect:true`; delete the dead check.

**Also flagged for Plan 2 (landmines, not P0):**
- **Screen `depth` must come from the GDevice (`gdPMap` pixelSize), never the window port bitmap** —
  within the port, offset +0x06 is a BitMap field in mono GrafPort but inside a PixMapHandle in
  CGrafPort. Add an `is_color_port(port) = (ReadMacInt16(port+6) & 0xC000) == 0xC000` discriminator.
- **Tag the desktop window** (`role:"desktop"`): the `(0,20,800,600)` window is the Finder's real
  full-screen desktop `WindowRecord` (not `GrayRgn`), correct to emit but should be distinguishable
  from document windows.
- **Backend B (`Execute68kTrap` from the idle hook) is the top Plan-2 hazard.** The repo's own
  `e2e_check_host_shutdown` deliberately *posts events* rather than calling traps, with the comment
  "non-reentrant: the guest acts from its own modal event loop." Re-entering CPU dispatch mid-`EMUL_OP`
  is the hazard (not allocation). Treat Backend B as opt-in, never-during-boot, behind its own env
  flag, with a static recursion guard. Add the recursion guard to `ui_introspect_service()` now (cheap
  insurance).
- **DITL gotchas (Plan 2):** item count is stored as N−1; each item's type byte high bit (0x80) =
  `itemDisable` (mask before matching); item rects are window-**local** → globalize by adding the
  **`contRgn` top-left** (not `structBounds`, not assumed (0,0); beware `SetOrigin`). `value`/`hilite`
  live in the `ControlRecord` (255 = dimmed), populated only after the dialog is built. Make the
  A-vs-B + screenshot overlay a **gating** calibration test.
- **Keep the feature OFF during boot** — only set `SS_UI_DUMP_DIR` after `[READY]`, so the `access()`
  per-idle never competes with the early-boot VBL spin-wait (the known timer-starvation trap).

---

## P1 — Documentation integration (technical writer)

**Doc inaccuracies to correct in the spec** (it actively misleads future work):
- §4.1/§8 say the trigger is **`SIGUSR2`** — false and *dangerous* (SIGUSR2 is the nanokernel's
  interrupt). The shipped trigger is the signal-free file poll (§3). Remove the SIGUSR2 references.
- §7 labels the full designed schema **"v1 — full-fidelity (this spec)"**; Plan 1 shipped a strict
  **subset**. Relabel to the Plan 1/2/3 staging so no one asserts on `menuBar`/`parts`/`items`/
  `dialogId` that aren't emitted yet.
- §4.2 documents `find(text=/role=/type=/window=)`, `assert_dialog`, `default_key`/`cancel_key`,
  `clickable()`→coords — none shipped. The shipped API is `find(title=/window_class=/visible=)`,
  `front_window`/`front_dialog`/`clickable()`→bool. Mark the rest as Plan 2/3.

**Create a canonical, forward-facing reference:** `SheepShaver/docs/UI-INTROSPECTION.md` (alongside
`DIAGNOSTICS.md`/`USER-HANDBOOK.md`). Sections: what/why · a **name-map** (`SS_UI_DUMP_DIR` ·
`ui_introspect.{h,cpp}` · `uidump.py` · `ss_ui.*`) · enable + quick start · **the Plan-1 emitted JSON
subset** (clearly bounded; note `schemaVersion` stays 1 across plans → feature-detect, e.g.
`"menuBar" in snap.raw`) · the file handshake · the Python API (the *actual* surface) · the
WindowRecord offset table · limitations + the Plan 1→2→3 roadmap · troubleshooting.

**Cross-link it from:** `CLAUDE.md` Key-Documentation table (the top discoverability gap — no row
today), `docs/ARCHITECTURE.md` (one line on the `ui_introspect` module), `SheepShaver/docs/DIAGNOSTICS.md`
env-var table (note it's a feature gate, not a JIT diagnostic), `SheepShaver/e2e/README.md` (point at
the canonical doc), and `docs/planning/ROADMAP.md` A5 (give Plans 2-3 a tracked home).

---

## P1 — Developer experience (developer advocate)

Adoption blockers are ergonomics, not design. Highest-leverage, prioritized:
- **`make ui-dump` + a `python -m sse2e.uidump <dir>` CLI** with a **human-readable ASCII layout**
  printer (the single biggest win — turns "library for spec-readers" into "tool anyone reaches for").
- **`__repr__` on `Window`/`Snapshot`** (REPL is unusable without it).
- **`wait_for_window(title=…, timeout=…)` / `wait_for_dialog()`** — the most-wanted missing affordance
  for any state-changing automation (the spec promised these).
- **`clickable()` should return the click point** (spec says so), and add `find()` substring/regex +
  `find_one()`. **`snapshot()` should default `dump_dir` to `$SS_UI_DUMP_DIR`** and warn on mismatch.
- **Recommend `SS_UI_DUMP_DIR` set by default** in the standard run configs (it's zero-cost when idle),
  so the tool can attach to a session instead of needing a pre-planned boot.

---

## P1/P2 — Make E2E richer (test architect) — the user's primary goal

**Architecture principle:** introspection **layers on** the existing signal gates, it doesn't replace
them. `[APP]`/`[BOOT]`/`[READY]` stay the cheap, event-driven ***when*** (transition detection);
fire **one** snapshot at that moment for the **what/where/which** (exact rect + structural assertion).
Don't convert signal-gated loops into snapshot-polling loops (a snapshot costs an idle round-trip).

**S1 — wire v1 into existing flows (high value / low effort, no C++):** add primitives
`await_window` / `assert_window` / `click_window` / `target(win)=center` to `uidump.py`; replace
brittle patterns in `scenario.py`:
- the `_await_front_app(settle=4)` noisy-'Finder'-frame heuristic (line 330) → a definitive
  "Speedometer absent / Finder is front" structural check — the cleanest heuristic→fact swap;
- the `lambda e: e.modal` / `"All Done" in e.title` ordering assumptions (188, 199) → snapshot-assert
  the *right* dialog is front;
- blind `vnc.key("esc")` / `time.sleep` settles (183, 51, 205–222) → act only if the expected window
  is actually present.

**S2 — v1 visual co-assertion (high/low):** at each `[gate]`, snapshot **and** masked-pHash screenshot
(`imagecmp.py` already exists); assert structure + pixels agree (overlay rects land on real pixels).
Catches garbled-render/video regressions the signal gates are blind to.

**S3 — ship Plan 2 (high value / high effort): the real unlock.** Dialog `items[]` (type/global
rect/title/value/hilite/itemEnable) + `dialogId` + menu items + `cmdChar`. This is what lets you
**click named buttons, assert *which* dialog, toggle controls, drive menus** — the gate for real app
automation. (Most classic dialogs have `title==""`, so v1 can only say "*a* dialog is up"; v2 says
*which* and drives it.)

**S4 — first real-world app benchmark (high/medium):** e.g. GraphicConverter/Photoshop → open a doc →
run one timed operation → record duration to the existing benchmark-export perf history.

**S5 — workload library + Plan 3 reach** (icon-launch, text-body assertions, socket transport).

**Honest division of labor for Photoshop-class tests (the triad):**
- **Signals** = the *when* (app launched, op done, shutdown).
- **Introspection (v1→v2)** = the *what/where/which* (reach the operation, target named controls,
  assert dialog identity/state) — replaces "click coordinate, sleep, hope."
- **Masked pHash** = the *result* in regions introspection is **structurally blind** to — a Photoshop
  **canvas / document body / custom-drawn previews / `userItem`s** are NOT in Toolbox structures.
- **Caveats:** you **cannot launch by clicking a desktop icon** (icons aren't windows until Plan 3) —
  use Startup Items / aliases / keyboard (the Speedometer pattern). `find(title=)` is exact-match —
  titles carry version/state suffixes; add substring/regex. `clickable()` does **not** yet test
  non-modal occlusion (`Rect.intersects` exists but is unused) — extend it with z-order occlusion.

---

## Prioritized queue

| # | Item | Effort | Owner-area | Gate for |
|---|------|--------|-----------|----------|
| **P0** | Backend-A correctness: modality-via-variant, frontWindowIndex-visible, junk-title sanity (+boot verify) | Low | C++ | everything |
| **P0** | Static recursion guard in `ui_introspect_service()` | Trivial | C++ | Backend B |
| **P1** | Spec corrections (SIGUSR2, v1-labeling, find() signature) | Low | docs | accuracy |
| **P1** | Canonical `SheepShaver/docs/UI-INTROSPECTION.md` + cross-links (CLAUDE.md/ARCHITECTURE/DIAGNOSTICS/README/ROADMAP) | Low-Med | docs | discoverability |
| **P1** | DX: `make ui-dump` CLI + ASCII layout + `__repr__` + `wait_for_window` + `clickable→coords` | Med | Python | adoption + S1 |
| **P1** | S1: wire v1 into `scenario.py` gates (structural asserts) | Med | Python | de-flake today |
| **P2** | S2: v1 + masked-pHash co-assertion at each gate | Low | Python | render-regression catch |
| ✅ **P2** | **Plan 2** (DITL items + control state + menus + depth via GDevice + desktop role + **control-list items for non-dialog windows**). *Open:* parts/hot-zones (Plan 2d) + ParamText (Plan 3). | High | C++ + Python | real app automation |
| 🟡 **P3** | **S4 underway**: Fractal Carbon installed + CarbonLib 1.6 via introspection-driven installer. *Next:* generic `scenario.run_workload` + the FC render run; then S5 library + Plan 3. | Med-High | Python | the end goal |

**Recommended next:** P0 correctness + P1 docs integration immediately (cheap, unblock-everything);
then choose between **DX/S1 harness enrichment** (fast payoff on the *current* harness) and **Plan 2**
(the real unlock for app automation) as the next substantial build.

### ✅ Follow-up: offline unit coverage for the memory-walking serializers (tech debt) — DONE (`492607c4`)

Was: the serializers (`serialize_snapshot`/`_dialog_items`/`_menu_bar`/`_window_controls`) read guest
memory via `ReadMacInt*` and were validated **live (boot) only** — a regression in any walk was caught
only by booting, not by `make ui-introspect-test`. **Fixed** by `src/ui_introspect_serialize_test.cpp`:
it compiles the **real** `ui_introspect.cpp` against a flat big-endian **mock RAM** (stub
`sysdeps.h`/`cpu_emulation.h` in `src/uitest/`, selected purely by `-I` order so the real build is
untouched) and asserts the JSON for hand-built Toolbox structures — each offset is now a regression-tested
fact. Fixtures cover the **non-dialog `controlList` → items** branch (titled/active button globalized,
dimmed/untitled control, degenerate-rect skip) and **dialog DITL items** (text/rect/refCon/defaultItem/
modality/default-flag). Wired into `make ui-introspect-test`. *Still extendable:* a `menuBar` fixture
(`MenuList` walk) is the obvious next addition now the scaffold exists.
