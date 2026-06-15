"""Shared entry-point scaffolding for the e2e run scripts.

So each entry point (run_smoke.py, run_benchmark.py, and any new scenario) is a thin "check
preconditions, resolve assets, run the drive function" — and nobody re-implements the
reactor-safe exit or the build/GUI/asset preflight (the parts most likely to be copied wrong).
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

from . import config, runner

# …/SheepShaver/e2e/sse2e/harness.py -> parents[2] = …/SheepShaver -> src/Unix/SheepShaver
EMULATOR = Path(__file__).resolve().parents[2] / "src" / "Unix" / "SheepShaver"


def hard_exit(code: int):
    """Flush stdio and `os._exit`. EVERY e2e entry script must exit through this.

    vncdotool starts a NON-daemon Twisted reactor thread on its first VNC connect; if it ever came
    up (even on a *failed* connect, where the usual close()/api.shutdown() was skipped) it blocks a
    normal interpreter exit — the process prints its result line and then HANGS forever needing ^C.
    `os._exit` bypasses interpreter shutdown so that can't happen. (The scenarios also tear the
    reactor down in their `finally`; this is the belt-and-braces for the entry layer.)
    """
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(code)


def check_preconditions(*, need_iso: bool = False, need_disk: bool = False):
    """Print the binary's build info, then verify the build, the GUI session, and the required
    assets — failing FAST with an actionable message instead of a cryptic late failure.

    Returns the resolved `assets` on success, or `None` after printing a `FAIL: …` line (the caller
    should then `return 1`). `need_iso`/`need_disk` say which boot media this scenario requires
    (the ROM is always required).
    """
    print(runner.binary_build_info(str(EMULATOR)))

    if not EMULATOR.exists() and not runner.is_configured(str(EMULATOR)):
        print("FAIL: emulator not built and the build tree isn't configured. First time: run the "
              "one-time configure + build steps — see SheepShaver/e2e/README.md, 'Building the "
              "emulator'.")
        return None

    if not runner.gui_session_ok():
        print("FAIL: no GUI (Aqua) session detected — the emulator opens a real SDL window and needs "
              "an active logged-in desktop, not a bare SSH session (there's no Xvfb on macOS). "
              "See the e2e README, Prerequisite 1.")
        return None

    assets = config.resolve_assets()
    try:
        config.require_asset(assets.rom, "rom")
        if need_iso:
            config.require_asset(assets.iso, "iso")
        if need_disk:
            config.require_asset(assets.disk, "disk")
    except FileNotFoundError as e:
        print(f"FAIL: {e}")
        return None
    return assets
