#!/usr/bin/env python3
"""Manual smoke: boot the isolated config, request a Backend-A UI snapshot, print the window list.

End-to-end proof of guest UI introspection (Plan 1). Boots the same read-only ISO isolated config
as run_smoke.py, but sets SS_UI_DUMP_DIR so the emulator services UI-snapshot requests, then asks for
a snapshot once the desktop is ready and finally drives the normal clean shutdown.

Run from SheepShaver/e2e/:  .venv/bin/python run_uidump_smoke.py
Requires a logged-in macOS GUI session (SDL needs a live WindowServer) + ROM/ISO assets.
"""
import os
import tempfile
import time
from pathlib import Path

from sse2e import disk, harness, runner, scenario, uidump

HERE = Path(__file__).parent
EMULATOR = HERE.parent / "src" / "Unix" / "SheepShaver"
VNCPORT = 5950


def main() -> int:
    assets = harness.check_preconditions(need_iso=True)
    if assets is None:
        return 1

    work = Path(tempfile.mkdtemp(prefix="ss-uidump-"))
    dump_dir = work / "uidump"
    dump_dir.mkdir()
    prefs = disk.render_prefs(
        HERE / "config" / "test.prefs.iso.template",
        work / "test.prefs",
        rom=assets.rom,
        cdrom=assets.iso,
        vncport=VNCPORT,
    )

    problem = runner.preflight(assets.iso)
    if problem:
        print(f"FAIL: preflight — {problem}")
        return 1

    # Runner.start() launches via Popen WITHOUT an explicit env=, so the child inherits our
    # environment. Setting SS_UI_DUMP_DIR here turns on the introspection service in the emulator.
    os.environ["SS_UI_DUMP_DIR"] = str(dump_dir)

    emu = runner.Runner(argv=[str(EMULATOR), "--config", str(prefs)])
    emu.start()
    try:
        ev = scenario._await_boot_ready(emu, 90.0)
        if ev is None:
            print("FAIL: boot timed out (no [BOOT] idle)")
            print(emu.log_text()[-2000:])
            return 1
        scenario._await_ready(emu, 45.0)  # settle; proceed even if [READY] doesn't arrive

        # --- the test: request a Backend-A snapshot of the live desktop ---
        snap = uidump.snapshot(str(dump_dir), timeout=15.0)
        screen = snap.raw.get("screen") or {}
        print(f"snapshot: backend={snap.backend} schema={snap.raw.get('schemaVersion')} "
              f"screen={screen} windows={len(snap.windows)} modal={snap.modal_active} "
              f"front={snap.front_index}")
        for w in snap.windows:
            cb = w.content_bounds
            print(f"  [{w.index}] {w.title!r} class={w.window_class} active={w.active} "
                  f"vis={w.visible} bounds=({cb.left},{cb.top},{cb.right},{cb.bottom}) "
                  f"clickable={snap.clickable(w)}")

        # Pipeline assertions: do NOT require an open window (a clean Finder desktop may have none).
        ok = (snap.backend == "A"
              and snap.raw.get("schemaVersion") == 1
              and screen.get("width", 0) > 0
              and screen.get("height", 0) > 0)

        # Bonus (best-effort, not required): raise the Shut Down dialog via the host->guest hook and
        # try to catch it as a modal dialog window — exercises dialog/modal detection.
        emu.request_shutdown()
        for _ in range(20):
            if emu.wait(0.0) is not None:
                break
            try:
                s2 = uidump.snapshot(str(dump_dir), timeout=2.0)
            except TimeoutError:
                break
            d = s2.front_dialog()
            if s2.modal_active and d is not None:
                print(f"  [dialog caught] modal dialog: title={d.title!r} "
                      f"bounds={d.content_bounds}")
                break
            time.sleep(0.25)

        code = emu.wait(timeout=30.0)
        print(f"emulator exit code: {code}")
        print("PASS" if ok else "FAIL: snapshot pipeline check failed")
        return 0 if ok else 1
    finally:
        emu.terminate()


if __name__ == "__main__":
    harness.hard_exit(main())
