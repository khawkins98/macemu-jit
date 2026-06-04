"""Pristine-disk-per-run + prefs rendering (test isolation)."""
from __future__ import annotations

import shutil
from pathlib import Path


def render_prefs(template: Path, out_path: Path, **fields) -> Path:
    """Render a checked-in prefs template, substituting `{field}` placeholders.

    Works for both the disk template (rom/disk/vncport) and the ISO template
    (rom/cdrom/vncport). Uses targeted `.replace()` rather than `str.format()` so a
    stray brace anywhere in the template (e.g. in a comment) can never raise.
    """
    text = template.read_text()
    for key, value in fields.items():
        text = text.replace("{" + key + "}", str(value))
    out_path.write_text(text)
    return out_path


def copy_pristine(master: Path, dest_dir: Path) -> Path:
    """Copy the pristine master disk to a fresh per-run path; return the copy.

    Each run starts from the clean master so a force-killed dirty working copy is
    discarded, never carried into the next boot (breaks the dirty->repair-prompt loop).
    """
    dest_dir.mkdir(parents=True, exist_ok=True)
    run_copy = dest_dir / master.name
    shutil.copyfile(master, run_copy)
    return run_copy
