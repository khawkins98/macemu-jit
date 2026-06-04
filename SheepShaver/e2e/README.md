# SheepShaver E2E VNC harness (P1)

Boots SheepShaver in a **repo-tracked isolated config** (`config/test.prefs.template` —
*not* your `~/.sheepshaver_prefs`), waits for the deterministic
`[BOOT] idle frontApp='Finder' modal=0` signal, drives **Special ▸ Shut Down** over VNC,
and asserts a clean exit (`Shutdown complete.` + the `PPC-JIT-A64: session` atexit block +
exit 0).

Design + rationale: `docs/superpowers/specs/2026-06-04-e2e-vnc-harness-design.md`.
Plan: `docs/superpowers/plans/2026-06-04-e2e-vnc-harness-p1.md`.

## Requirements

- A **logged-in macOS GUI session** — SDL needs a live WindowServer. This does **not** run on
  a headless macOS CI agent (there is no Xvfb equivalent on macOS); use a logged-in Mac runner.
- `python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`
- Assets (large, not in git): a ROM and a **pristine master disk** created via a clean Shut Down
  (so the volume is "properly put away" and never triggers a repair prompt). Resolve their paths
  via env vars or local-dev defaults:
  - `SS_E2E_ROM`  (default: `/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom`)
  - `SS_E2E_DISK` (default: `/Users/Shared/macemu/e2e_master.dsk`)

## Run

    make e2e          # from SheepShaver/
    # or: cd e2e && .venv/bin/python run_smoke.py

Offline unit tests (no boot, no assets needed): `cd e2e && .venv/bin/pytest -q`

## What's checked in vs. system-specific

- **In the repo:** the prefs config template, all harness code, the unit tests.
- **System-specific (env-resolved):** ROM + disk paths. CI sets `SS_E2E_ROM`/`SS_E2E_DISK` to
  point at its own assets without editing any tracked file.

## Status

P1 lifecycle smoke. Menu coordinates in `sse2e/scenario.py` (`SPECIAL_MENU_XY`,
`SHUTDOWN_ITEM_XY`) are calibrated against a live 640×480 desktop — confirm them before trusting
a green run. Golden-image diff and app-launch are P2; the scenario DSL is P3.
