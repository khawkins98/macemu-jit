"""Pristine-disk-per-run + prefs rendering (test isolation)."""
from __future__ import annotations

import shutil
from pathlib import Path


def render_prefs(template: Path, out_path: Path, *, rom: str, disk: str, vncport: int) -> Path:
    """Render the checked-in prefs template, substituting the system-specific paths."""
    text = template.read_text()
    # Only format placeholder lines; leave '#' comment lines (which contain no braces) intact.
    text = text.format(rom=rom, disk=disk, vncport=vncport)
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
