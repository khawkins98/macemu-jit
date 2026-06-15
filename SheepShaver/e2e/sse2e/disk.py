"""Pristine-disk-per-run + prefs rendering (test isolation)."""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


def appsdisk_line(apps_copy: str | None) -> str:
    """The optional second 'disk' prefs line that attaches an apps volume (empty if none)."""
    return f"disk {apps_copy}" if apps_copy else ""


def render_prefs(template: Path, out_path: Path, **fields) -> Path:
    """Render a checked-in prefs template, substituting `{field}` placeholders.

    Works for both the disk template (rom/disk/vncport) and the ISO template
    (rom/cdrom/vncport). Uses targeted `.replace()` rather than `str.format()` so a
    stray brace anywhere in the template (e.g. in a comment) can never raise.

    Any `{appsdisk_line}` placeholder left unsubstituted by the caller (i.e. not passed
    as a kwarg) is stripped to an empty string so existing single-disk callers are
    unaffected by the new placeholder in test.prefs.template.
    """
    text = template.read_text()
    for key, value in fields.items():
        text = text.replace("{" + key + "}", str(value))
    # Strip any unfilled optional placeholder — keeps single-disk callers clean.
    text = text.replace("{appsdisk_line}", "")
    out_path.write_text(text)
    return out_path


def copy_pristine(master: Path, dest_dir: Path) -> Path:
    """Copy the pristine master disk to a fresh per-run path; return the copy.

    Each run starts from the clean master so a force-killed dirty working copy is
    discarded, never carried into the next boot (breaks the dirty->repair-prompt loop).
    """
    dest_dir.mkdir(parents=True, exist_ok=True)
    run_copy = dest_dir / master.name
    # APFS clonefile (`cp -c`): instant, copy-on-write — the per-run copy is free regardless of
    # the image's logical size. Falls back to a real byte copy off APFS (e.g. Linux/CI).
    try:
        subprocess.run(["cp", "-c", str(master), str(run_copy)], check=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        shutil.copyfile(master, run_copy)
    # Loudly signal the disk-safety guarantee on every run: the emulator boots THIS throwaway copy,
    # never the master, so the master image can never be dirtied/corrupted (even on a force-kill).
    print(f"  \U0001f6e1  disk-safety: booting a pristine per-run copy of {master.name!r} "
          f"(clonefile -> {run_copy}); your master image is never opened for write.", flush=True)
    return run_copy
