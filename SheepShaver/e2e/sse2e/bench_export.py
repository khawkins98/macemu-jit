"""Extract, parse, archive and compare Speedometer text reports (e2e benchmark history).

Host-side only. Reads the saved report TEXT file off the run-copy HFS image via hfsutils
(no boot), parses the Key:Value scores, archives each run under a timestamped dir + an
append-only history.csv, and formats the delta vs the previous run. Every entry point is
best-effort: callers treat failures as "no history this run", never as a benchmark failure.
"""
from __future__ import annotations

import csv
import os
import re
import shutil
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

# Deterministic in-guest save name. Pure alphanumeric so typing it over VNC needs no shifted
# or special keys, and it never pre-exists on a pristine clonefile copy (so no "replace?" prompt).
REPORT_NAME = "e2ereport"

_HMOUNT = "/opt/homebrew/bin/hmount"
_HUMOUNT = "/opt/homebrew/bin/humount"
_HPWD = "/opt/homebrew/bin/hpwd"
_HLS = "/opt/homebrew/bin/hls"
_HCOPY = "/opt/homebrew/bin/hcopy"


def hfsutils_available() -> bool:
    return all(Path(p).exists() for p in (_HMOUNT, _HUMOUNT, _HPWD, _HLS, _HCOPY))


def _run(args, home):
    # Isolate hfsutils' $HOME/.hcwd state from the user's real HOME. Decode output as MacRoman:
    # HFS filenames are classic-Mac MacRoman and routinely contain bytes that are invalid UTF-8
    # (e.g. "Apple FM Radio ®") — MacRoman is a total single-byte codec, so it never raises.
    env = dict(os.environ, HOME=home)
    return subprocess.run(args, env=env, capture_output=True, encoding="mac_roman")


def _find_in_listing(listing: str, name: str) -> str | None:
    """Return the colon-path folder containing `name` in `hls -R -F` output, or None.

    Root-level files return "" (empty folder path). Folder *entries* (trailing ':') and
    the ':Folder:' headers themselves are never matched as files.
    """
    folder = ""  # current directory context; "" == volume root
    for line in listing.splitlines():
        if not line.strip():
            continue
        m = re.match(r"^:(.*):$", line)
        if m:
            folder = m.group(1)  # e.g. "Desktop Folder" or "System Folder:Extensions"
            continue
        entry = line.rstrip("*").rstrip()
        if entry.endswith(":"):
            continue  # a subfolder entry within a listing, not a file
        if entry == name:
            return folder
    return None


def extract_report(disk_path: str, name: str = REPORT_NAME) -> str | None:
    """Pull the saved text report off an unmounted HFS image; return its text, or None.

    None means: hfsutils missing, mount failed, the report was not found, or the copy
    failed. Callers treat None as "no history this run" (never a benchmark failure).
    """
    if not hfsutils_available():
        return None
    home = tempfile.mkdtemp(prefix="hfs-home-")
    try:
        if _run([_HMOUNT, disk_path], home).returncode != 0:
            return None
        try:
            vol = _run([_HPWD], home).stdout.strip().rstrip(":")
            listing = _run([_HLS, "-R", "-F"], home).stdout
            folder = _find_in_listing(listing, name)
            if folder is None:
                return None
            src = f"{vol}:{folder}:{name}" if folder else f"{vol}:{name}"
            out = Path(home) / "report.txt"
            if _run([_HCOPY, "-t", src, str(out)], home).returncode != 0 or not out.exists():
                return None
            return out.read_text(errors="replace")
        finally:
            _run([_HUMOUNT], home)
    finally:
        shutil.rmtree(home, ignore_errors=True)


# Speedometer score label -> normalized column. Exact label match so "Nominal CPU" can't be
# captured as "CPU". PR may render as "PR" or "PowerRating"; Math may render as "Math" or "FPU".
_SCORE_KEYS = {
    "cpu": ("CPU",),
    "graphics": ("Graphics",),
    "disk": ("Disk",),
    "math": ("Math", "FPU"),
    "pr": ("PR", "PowerRating", "Power Rating"),
}


@dataclass
class Report:
    scores: dict = field(default_factory=dict)      # {pr,cpu,graphics,disk,math} -> float
    rom_version: str | None = None
    ram_k: int | None = None
    fields: dict = field(default_factory=dict)      # every "Key: Value" pair, raw


def _num(s: str):
    m = re.search(r"-?\d+(?:\.\d+)?", s)
    return float(m.group()) if m else None


def parse_report(text: str) -> Report:
    fields: dict[str, str] = {}
    for line in text.splitlines():
        m = re.match(r"^\s*(.+?):\s+(.+?)\s*$", line)
        if m:
            fields.setdefault(m.group(1).strip(), m.group(2).strip())  # first wins
    scores: dict[str, float] = {}
    for col, labels in _SCORE_KEYS.items():
        for label in labels:
            if label in fields and _num(fields[label]) is not None:
                scores[col] = _num(fields[label])
                break
    ram_k = None
    for label in ("Physical RAM", "Logical RAM"):
        if label in fields:
            m = re.search(r"(\d+)", fields[label])
            if m:
                ram_k = int(m.group(1))
                break
    return Report(scores=scores, rom_version=fields.get("ROM Version"),
                  ram_k=ram_k, fields=fields)


def parse_jit_blocks(log_text: str) -> str:
    """The last 'JIT: N blocks' count from the emulator log/title-bar line (or '')."""
    matches = re.findall(r"JIT:\s*(\d+)\s+blocks", log_text)
    return matches[-1] if matches else ""


HISTORY_COLUMNS = ["timestamp", "pr", "cpu", "graphics", "disk", "math",
                   "duration_s", "rom_version", "ram_k", "jit_blocks", "git_sha"]


def _git_sha() -> str:
    try:
        r = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                           capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else ""
    except Exception:
        return ""


def _row(report: Report, timestamp: str, duration_s, jit_blocks: str) -> dict:
    return {
        "timestamp": timestamp,
        "pr": report.scores.get("pr", ""),
        "cpu": report.scores.get("cpu", ""),
        "graphics": report.scores.get("graphics", ""),
        "disk": report.scores.get("disk", ""),
        "math": report.scores.get("math", ""),
        "duration_s": "" if duration_s is None else round(duration_s, 1),
        "rom_version": report.rom_version or "",
        "ram_k": "" if report.ram_k is None else report.ram_k,
        "jit_blocks": jit_blocks,
        "git_sha": _git_sha(),
    }


def archive_run(*, report: Report, raw_text: str, png, history_root: Path,
                timestamp: str, duration_s=None, jit_blocks: str = "") -> Path:
    """Write the per-run dir (report.txt + scores.csv [+ result.png]) and append history.csv."""
    run_dir = history_root / timestamp
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / "report.txt").write_text(raw_text)
    row = _row(report, timestamp, duration_s, jit_blocks)
    with (run_dir / "scores.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=HISTORY_COLUMNS)
        w.writeheader()
        w.writerow(row)
    hist = history_root / "history.csv"
    first = not hist.exists()
    with hist.open("a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=HISTORY_COLUMNS)
        if first:
            w.writeheader()
        w.writerow(row)
    if png is not None and Path(png).exists():
        shutil.copyfile(png, run_dir / "result.png")
    return run_dir


def format_delta(history_csv: Path) -> str:
    if not Path(history_csv).exists():
        return "benchmark history: (no runs recorded)"
    with Path(history_csv).open(newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return "benchmark history: (no runs recorded)"
    metrics = [("PR", "pr"), ("CPU", "cpu"), ("Graphics", "graphics"),
               ("Disk", "disk"), ("Math", "math")]
    cur = rows[-1]
    lines: list[str] = []
    if len(rows) == 1:
        lines.append(f"benchmark history: {cur['timestamp']}  (first recorded run)")
        for label, key in metrics:
            if cur.get(key):
                lines.append(f"  {label:<9} {cur[key]}")
    else:
        prev = rows[-2]
        lines.append(f"benchmark history: {cur['timestamp']}  (vs {prev['timestamp']})")
        for label, key in metrics:
            c, p = cur.get(key, ""), prev.get(key, "")
            if c and p:
                try:
                    cv, pv = float(c), float(p)
                    pct = (cv - pv) / pv * 100 if pv else 0.0
                    lines.append(f"  {label:<9} {pv:>11} -> {cv:>11}  ({pct:+.1f}%)")
                except ValueError:
                    lines.append(f"  {label:<9} {p} -> {c}")
            elif c:
                lines.append(f"  {label:<9} {c}")
    return "\n".join(lines)
