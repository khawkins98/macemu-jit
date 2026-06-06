#!/usr/bin/env python3
"""One-time CarbonLib 1.6 install, driven over VNC by guest-UI introspection.

Boots the DEDICATED WRITABLE workload master (e2e-macos9-workload-boot.dsk — a copy of macos9_fresh, NOT
any of Ken's masters; the install must persist so this is booted writable, not a per-run copy), with the
apps disk attached (it holds 'CarbonLib 1.6.smi'). Launches the self-mounting installer via Finder
type-select, then runs an ADAPTIVE loop: snapshot the screen, find an installer button by name
(Agree/Continue/Install/OK/Restart/Quit), click its rect center. Screenshots every step. Then clean
shutdown to persist, and the caller verifies host-side that CarbonLib landed in System Folder/Extensions.

Run:  cd SheepShaver/e2e && SS_INPUT_LOCKOUT=1 .venv/bin/python run_carbonlib_install.py
"""
import os
import tempfile
import time
from pathlib import Path

from sse2e import disk, harness, runner, scenario, uidump
from sse2e.vnc import Vnc

HERE = Path(__file__).parent
EMULATOR = HERE.parent / "src" / "Unix" / "SheepShaver"
VNCPORT = 5950
BOOT = "/Users/Shared/macemu/e2e-macos9-workload-boot.dsk"   # writable, dedicated, persisted
APPS = "/Users/Shared/macemu/e2e-apps.dsk"
# Terminal buttons end a panel; ordered by preference (avoid Restart if a Quit/Continue exists).
BUTTONS = ["Continue", "Agree", "Accept", "Install", "OK", "Done", "Quit", "Restart", "Yes"]
TERMINAL = ("quit", "restart", "done")


def _snap(dump_dir, timeout=8.0):
    try:
        return uidump.snapshot(str(dump_dir), timeout=timeout)
    except Exception:
        return None


def _find_button(snap):
    """Return (item, label) for the best clickable installer button on screen, or (None, None)."""
    scan = ([snap.front_dialog()] if snap.front_dialog() else []) + snap.windows
    for label in BUTTONS:                       # preference order
        for w in scan:
            if w is None:
                continue
            for it in getattr(w, "items", []):
                if it.type == "button" and it.enabled and label.lower() in (it.text or "").lower():
                    return it, label
    return None, None


def main() -> int:
    assets = harness.check_preconditions()      # ROM check; disk/apps are explicit paths below
    if assets is None:
        return 1
    work = Path(tempfile.mkdtemp(prefix="ss-clinst-"))
    dump_dir = work / "ui"; dump_dir.mkdir()
    os.environ["SS_UI_DUMP_DIR"] = str(dump_dir)
    art = HERE / "artifacts"; art.mkdir(exist_ok=True)

    prefs = disk.render_prefs(
        HERE / "config" / "test.prefs.template", work / "test.prefs",
        rom=assets.rom, disk=BOOT, appsdisk_line=disk.appsdisk_line(APPS), vncport=VNCPORT,
    )
    print(f"  [install] booting WRITABLE {Path(BOOT).name} (persisted) + {Path(APPS).name} attached", flush=True)
    problem = runner.preflight(BOOT, APPS)
    if problem:
        print(f"FAIL: preflight — {problem}")
        return 1

    emu = runner.Runner(argv=[str(EMULATOR), "--config", str(prefs)])
    emu.start()
    try:
        if scenario._await_boot_ready(emu, 120.0) is None:
            print("FAIL: boot timed out")
            print(emu.log_text()[-1500:])
            return 1
        scenario._await_ready(emu, 60.0)
        time.sleep(3.0)

        vnc = Vnc(port=VNCPORT)
        snap = _snap(dump_dir)
        scr = (snap.raw.get("screen") if snap else None) or {"width": 800, "height": 600}
        ex, ey = scr["width"] // 2, scr["height"] - 30

        def wait_and_click(labels, timeout=45.0, settle=2.5, shot=None):
            """POLL up to `timeout`s for an enabled button whose text matches one of `labels`
            (preference order), click its rect center, and return the clicked text (or None on
            timeout). Robust to slow self-mount/installer panels — it waits for the button to appear."""
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                s = _snap(dump_dir, timeout=4.0)
                if shot:
                    try: vnc.capture(str(art / shot))
                    except Exception: pass
                if s is not None:
                    scan = ([s.front_dialog()] if s.front_dialog() else []) + s.windows
                    for lab in labels:
                        for w in scan:
                            for it in getattr(w, "items", []):
                                if it.type == "button" and it.enabled and lab.lower() in (it.text or "").lower():
                                    print(f"    click {it.text!r} @{it.rect.center}", flush=True)
                                    vnc.click(*it.rect.center); time.sleep(settle)
                                    return it.text
                time.sleep(2.0)
            print(f"    (no button matching {labels} within {timeout}s)", flush=True)
            return None

        print(">>> open E2E Apps; launch 'CarbonLib 1.6.smi'", flush=True)
        vnc.click(ex, ey); time.sleep(1.0)
        vnc.type_text("E2E"); time.sleep(1.2); vnc.key("super-o"); time.sleep(4.0)
        vnc.type_text("CarbonLib"); time.sleep(1.2); vnc.key("super-o"); time.sleep(8.0)

        print(">>> agree to the SMI license (polling)", flush=True)
        wait_and_click(["Agree", "Accept"], timeout=50.0, shot="cl_license.png")
        time.sleep(6.0)   # let the SMI mount its disk image

        print(">>> open the mounted CarbonLib volume + launch its Installer", flush=True)
        vnc.click(ex, ey); time.sleep(1.0)                          # focus the desktop
        vnc.type_text("CarbonLib"); time.sleep(1.2); vnc.key("super-o"); time.sleep(4.0)
        try: vnc.capture(str(art / "cl_volume.png"))                # SEE the volume / installer layout
        except Exception: pass
        # the installer is the standard "Apple SW Install" — type-select "Apple" + Open
        vnc.type_text("Apple"); time.sleep(1.2); vnc.key("super-o"); time.sleep(8.0)

        # The Apple Installer's panels (Continue/Install) live in a movable-modal/document window whose
        # controls our dialog-only introspection can't see — but they're DEFAULT buttons, so Return
        # fires them. Modal ALERTS (license Agree, final success Restart/Quit) ARE dialogKind -> we
        # click those by name via introspection. So: click a dialog button if one is visible, else Enter.
        print(">>> drive the installer (Enter = default panel button; introspection clicks alerts)", flush=True)
        for step in range(18):
            try: vnc.capture(str(art / f"cl_inst{step:02d}.png"))
            except Exception: pass
            snap = _snap(dump_dir, timeout=4.0)
            t = None
            if snap is not None:
                scan = ([snap.front_dialog()] if snap.front_dialog() else []) + snap.windows
                for lab in ["Restart", "Quit", "Done", "Agree", "Yes", "OK", "Install", "Continue"]:
                    for w in scan:
                        for it in getattr(w, "items", []):
                            if it.type == "button" and it.enabled and lab.lower() in (it.text or "").lower():
                                t = it.text; print(f"  [inst {step}] dialog button -> click {t!r}", flush=True)
                                vnc.click(*it.rect.center); break
                        if t: break
                    if t: break
            if t is not None:
                if any(k in t.lower() for k in TERMINAL):
                    print(f"  [inst {step}] terminal {t!r} — done", flush=True); time.sleep(5.0); break
                time.sleep(3.0); continue
            # no visible dialog button -> press the panel's default action (Continue / Install / OK)
            print(f"  [inst {step}] no dialog button; Return (default panel action)", flush=True)
            vnc.key("enter"); time.sleep(3.5)
        try:
            vnc.capture(str(art / "cl_final.png"))
        except Exception:
            pass
        vnc.close()

        # Let the machine settle (a forced Restart reboots; a Quit returns to Finder), then shut down
        # cleanly to flush the install to the writable master.
        time.sleep(20.0)
        emu.request_shutdown()
        code = emu.wait(45.0)
        print(f"  [install] shutdown exit code: {code}", flush=True)
        return 0
    finally:
        emu.terminate()


if __name__ == "__main__":
    harness.hard_exit(main())
