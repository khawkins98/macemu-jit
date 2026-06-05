# Benchmark result export & history — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** After `make e2e-bench`, save Speedometer's text report in-guest, extract it host-side
via hfsutils, parse the scores, archive each run under a timestamped dir + append-only
`history.csv`, and print the delta vs the previous run — all gitignored, no regression gate.

**Architecture:** A new pure-host module `sse2e/bench_export.py` owns extraction
(hfsutils) / parsing / archiving / delta-formatting. `scenario.run_benchmark` gains one
best-effort guest step (Cmd-T → type name → Return) to save the report. `run_benchmark.py`
calls the export after a successful run. Every new step is additive — a green benchmark stays
green (collect+report only).

**Tech Stack:** Python 3 (stdlib `csv`/`re`/`subprocess`/`tempfile`), hfsutils
(`/opt/homebrew/bin/hmount|humount|hpwd|hls|hcopy`), vncdotool. Spec:
`docs/superpowers/specs/2026-06-05-benchmark-result-export-design.md`.

---

## Verified facts (from host probing, 2026-06-05)

- `hls -R -F` prints the **root listing first with no header**, then each subfolder as a
  `:Folder:` (or nested `:Folder:Sub:`) header line followed by its entries. Folder *entries*
  end with `:`; apps/aliases get a `*` suffix (the `-F` flag); plain files are bare.
- `hcopy -t "<Volume>:<Folder>:<file>" out.txt` extracts a `TEXT` file with CR→LF translation.
  Absolute paths are `Volume:Folder:file`; `hpwd` after `hmount` returns `Volume:`.
- hfsutils keeps per-process state in `$HOME/.hcwd` — every call runs with `HOME` pointed at a
  throwaway tempdir so it never touches the user's real hfsutils state.
- `artifacts/` is already gitignored (`SheepShaver/e2e/.gitignore:4`).

## File Structure

- **Create** `SheepShaver/e2e/sse2e/bench_export.py` — extraction/parse/archive/delta (pure host).
- **Create** `SheepShaver/e2e/tests/test_bench_export.py` — unit tests (no emulator/hfsutils).
- **Modify** `SheepShaver/e2e/sse2e/vnc.py` — add `type_text`.
- **Modify** `SheepShaver/e2e/sse2e/scenario.py` — Cmd-T save step + `BenchResult.report_saved`.
- **Modify** `SheepShaver/e2e/run_benchmark.py` — call the export after a passing run.
- **Modify** `SheepShaver/e2e/doctor.py` — add an hfsutils check.
- **Modify** docs: `e2e/README.md`, `SheepShaver/docs/USER-HANDBOOK.md`, `CHANGELOG.md`.

All commands below run from `SheepShaver/e2e/` unless noted. Run tests with the venv:
`.venv/bin/python -m pytest -q` (or `cd SheepShaver && make e2e-test`).

---

### Task 1: `bench_export.py` — hfsutils extraction

**Files:**
- Create: `SheepShaver/e2e/sse2e/bench_export.py`
- Test: `SheepShaver/e2e/tests/test_bench_export.py`

- [ ] **Step 1: Write the failing test for the listing parser**

```python
# tests/test_bench_export.py
from pathlib import Path

from sse2e import bench_export


# A real `hls -R -F` shape (root listing first, then ':Folder:' headers).
_LISTING = """\
Apple Extras:
Desktop Folder:
System Folder:

:Desktop Folder:
Machine Records
e2ereport
Speedometer 4.02*

:System Folder:
Finder
Preferences:
"""


def test_find_in_listing_locates_file_in_subfolder():
    assert bench_export._find_in_listing(_LISTING, "e2ereport") == "Desktop Folder"


def test_find_in_listing_root_level_file_returns_empty():
    listing = "ReadMe\nApplications:\n\n:Applications:\nSimpleText*\n"
    assert bench_export._find_in_listing(listing, "ReadMe") == ""


def test_find_in_listing_missing_returns_none():
    assert bench_export._find_in_listing(_LISTING, "nope") is None


def test_find_in_listing_ignores_folder_entries_named_like_target():
    # A *folder* entry "e2ereport:" must not match a file search for "e2ereport".
    listing = ":Desktop Folder:\ne2ereport:\nReal File\n"
    assert bench_export._find_in_listing(listing, "e2ereport") is None
```

- [ ] **Step 2: Run to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_bench_export.py -q`
Expected: FAIL with `ModuleNotFoundError: No module named 'sse2e.bench_export'`.

- [ ] **Step 3: Create the module with the extraction layer**

```python
# sse2e/bench_export.py
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
    # Isolate hfsutils' $HOME/.hcwd state from the user's real HOME.
    env = dict(os.environ, HOME=home)
    return subprocess.run(args, env=env, capture_output=True, text=True)


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
```

- [ ] **Step 4: Run to verify the parser tests pass**

Run: `.venv/bin/python -m pytest tests/test_bench_export.py -q`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add SheepShaver/e2e/sse2e/bench_export.py SheepShaver/e2e/tests/test_bench_export.py
git commit -F- <<'EOF'
feat(e2e): bench_export — hfsutils extraction of the Speedometer text report

Locate the saved report on the run-copy HFS image (recursive hls -R parse) and
hcopy -t it out, with hfsutils state isolated via a throwaway HOME. Host-side only.
EOF
```

---

### Task 2: `bench_export.py` — report parsing

**Files:**
- Modify: `SheepShaver/e2e/sse2e/bench_export.py`
- Test: `SheepShaver/e2e/tests/test_bench_export.py`

- [ ] **Step 1: Write the failing test**

```python
# append to tests/test_bench_export.py

# Representative Speedometer 4.02 "Save Text Report" content. The five score lines are the
# values from the results panel (CPU/Graphics/Disk/Math/PR); the rest is the machine report.
# NOTE: the exact presence of Graphics/Disk/Math/PR in the text report is confirmed on the
# first real e2e-bench boot (spec §2); the parser keeps the raw text regardless.
_REPORT = """\
Comm. Toolbox: 7.5.0
Script Manager: 8.6.0

CPU: 64.265
Graphics: 45.374
Disk: 9.815
Math: 12737.795
PR: 30.103

Nominal CPU: MC68020
FPU: PowerPC Math
MMU: Integral MMU
Physical RAM: 262144K
Logical RAM: 262144K
ROM Version: $077D
ROM Size: 3072K
"""


def test_parse_report_extracts_scores():
    r = bench_export.parse_report(_REPORT)
    assert r.scores["pr"] == 30.103
    assert r.scores["cpu"] == 64.265
    assert r.scores["graphics"] == 45.374
    assert r.scores["disk"] == 9.815
    assert r.scores["math"] == 12737.795


def test_parse_report_extracts_machine_fields():
    r = bench_export.parse_report(_REPORT)
    assert r.rom_version == "$077D"
    assert r.ram_k == 262144


def test_parse_report_does_not_confuse_nominal_cpu_for_cpu():
    # "Nominal CPU: MC68020" must not overwrite the numeric "CPU: 64.265".
    r = bench_export.parse_report(_REPORT)
    assert r.scores["cpu"] == 64.265


def test_parse_report_tolerates_missing_scores():
    r = bench_export.parse_report("ROM Version: $077D\nFruit: banana\n")
    assert r.scores == {}
    assert r.rom_version == "$077D"


def test_parse_jit_blocks_reads_last_count():
    log = "JIT: 100 blocks, cache 1K/2K\nnoise\nJIT: 46355 blocks, cache 221061K/262144K (84%)\n"
    assert bench_export.parse_jit_blocks(log) == "46355"


def test_parse_jit_blocks_absent_returns_empty():
    assert bench_export.parse_jit_blocks("no jit line here") == ""
```

- [ ] **Step 2: Run to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_bench_export.py -q`
Expected: FAIL — `parse_report` / `parse_jit_blocks` / `Report` not defined.

- [ ] **Step 3: Add the parser**

```python
# add to sse2e/bench_export.py (after extract_report)

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
```

- [ ] **Step 4: Run to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_bench_export.py -q`
Expected: PASS (10 tests).

- [ ] **Step 5: Commit**

```bash
git add SheepShaver/e2e/sse2e/bench_export.py SheepShaver/e2e/tests/test_bench_export.py
git commit -F- <<'EOF'
feat(e2e): bench_export — parse Speedometer scores + machine fields + JIT block count
EOF
```

---

### Task 3: `bench_export.py` — archive + history + delta

**Files:**
- Modify: `SheepShaver/e2e/sse2e/bench_export.py`
- Test: `SheepShaver/e2e/tests/test_bench_export.py`

- [ ] **Step 1: Write the failing test**

```python
# append to tests/test_bench_export.py
import csv as _csv


def _report():
    return bench_export.parse_report(_REPORT)


def test_archive_run_writes_dir_and_appends_history(tmp_path):
    root = tmp_path / "benchmark-history"
    run_dir = bench_export.archive_run(
        report=_report(), raw_text=_REPORT, png=None, history_root=root,
        timestamp="2026-06-05T16-16-03", duration_s=39.2, jit_blocks="46355")
    assert (run_dir / "report.txt").read_text() == _REPORT
    assert (run_dir / "scores.csv").exists()
    rows = list(_csv.DictReader((root / "history.csv").open()))
    assert len(rows) == 1
    assert rows[0]["pr"] == "30.103"
    assert rows[0]["jit_blocks"] == "46355"
    assert rows[0]["duration_s"] == "39.2"


def test_archive_run_appends_second_row(tmp_path):
    root = tmp_path / "benchmark-history"
    bench_export.archive_run(report=_report(), raw_text=_REPORT, png=None,
                             history_root=root, timestamp="2026-06-05T16-16-03")
    bench_export.archive_run(report=_report(), raw_text=_REPORT, png=None,
                             history_root=root, timestamp="2026-06-05T17-00-00")
    rows = list(_csv.DictReader((root / "history.csv").open()))
    assert len(rows) == 2  # header written once, two data rows


def test_format_delta_first_run(tmp_path):
    root = tmp_path / "benchmark-history"
    bench_export.archive_run(report=_report(), raw_text=_REPORT, png=None,
                             history_root=root, timestamp="2026-06-05T16-16-03")
    out = bench_export.format_delta(root / "history.csv")
    assert "first recorded run" in out
    assert "30.103" in out


def test_format_delta_shows_percent_change(tmp_path):
    root = tmp_path / "benchmark-history"
    r1 = bench_export.parse_report(_REPORT)
    r2 = bench_export.parse_report(_REPORT.replace("PR: 30.103", "PR: 31.402"))
    bench_export.archive_run(report=r1, raw_text=_REPORT, png=None,
                             history_root=root, timestamp="t1")
    bench_export.archive_run(report=r2, raw_text=_REPORT, png=None,
                             history_root=root, timestamp="t2")
    out = bench_export.format_delta(root / "history.csv")
    assert "PR" in out and "+4.3%" in out  # (31.402-30.103)/30.103 = +4.31%


def test_format_delta_no_history():
    from pathlib import Path as _P
    assert "no runs" in bench_export.format_delta(_P("/nonexistent/history.csv"))
```

- [ ] **Step 2: Run to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_bench_export.py -q`
Expected: FAIL — `archive_run` / `format_delta` not defined.

- [ ] **Step 3: Add archive + delta**

```python
# add to sse2e/bench_export.py

HISTORY_COLUMNS = ["timestamp", "pr", "cpu", "graphics", "disk", "math",
                   "duration_s", "rom_version", "ram_k", "jit_blocks", "git_sha"]


def _git_sha() -> str:
    try:
        r = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                           capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else ""
    except Exception:
        return ""


def _row(report: "Report", timestamp: str, duration_s, jit_blocks: str) -> dict:
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


def archive_run(*, report: "Report", raw_text: str, png, history_root: Path,
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
```

- [ ] **Step 4: Run to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_bench_export.py -q`
Expected: PASS (15 tests).

- [ ] **Step 5: Commit**

```bash
git add SheepShaver/e2e/sse2e/bench_export.py SheepShaver/e2e/tests/test_bench_export.py
git commit -F- <<'EOF'
feat(e2e): bench_export — archive runs + append-only history.csv + delta formatter
EOF
```

---

### Task 4: Guest step — save the text report (Cmd-T)

**Files:**
- Modify: `SheepShaver/e2e/sse2e/vnc.py`
- Modify: `SheepShaver/e2e/sse2e/scenario.py:124-218`

- [ ] **Step 1: Add `type_text` to the VNC client**

In `sse2e/vnc.py`, add this method to `Vnc` (after `key`):

```python
    def type_text(self, text: str) -> None:
        """Send each character as a discrete key press (alphanumeric names map 1:1)."""
        for ch in text:
            self._client.keyPress(ch)
```

- [ ] **Step 2: Add the `report_saved` flag to `BenchResult`**

In `sse2e/scenario.py`, in the `BenchResult` dataclass (around line 124-130), add a field:

```python
    report_saved: bool = False  # whether the in-guest Cmd-T text-report save was driven
```

- [ ] **Step 3: Import the deterministic name**

At the top of `sse2e/scenario.py`, with the other `from .` imports, add:

```python
from .bench_export import REPORT_NAME
```

- [ ] **Step 4: Drive the save after dismissing "All Done!"**

In `run_benchmark`, replace this block (around line 207-208):

```python
        vnc.key("enter"); time.sleep(1.5)        # dismiss "All Done!" -> guest returns to idle
        vnc.close()
```

with:

```python
        vnc.key("enter"); time.sleep(1.5)        # dismiss "All Done!" -> guest returns to idle

        # Save Speedometer's text report onto the (throwaway) run-copy disk so the host can
        # extract the numbers after shutdown. Cmd-T = "Save Text Report"; the dialog opens with
        # the name field selected, so typing replaces the default with our deterministic name
        # (no "replace existing?" prompt on a pristine copy). Best-effort: any failure here must
        # NOT fail the benchmark — the report export is purely additive (collect+report only).
        report_saved = False
        try:
            vnc.key("super-t"); time.sleep(1.0)           # File > Save Text Report...
            vnc.type_text(REPORT_NAME); time.sleep(0.3)   # replace the selected default name
            vnc.key("enter"); time.sleep(1.5)             # Return = Save (default button)
            report_saved = True
        except Exception:
            pass                                          # leave report_saved False; PASS unaffected
        vnc.close()
```

- [ ] **Step 5: Thread `report_saved` into the successful result**

In the same function, change the final success return (around line 216-218) from:

```python
        return BenchResult(True,
                           f"benchmark complete in {duration_s:.0f}s (gates: {gates}); results + log captured",
                           log, img, duration_s)
```

to:

```python
        return BenchResult(True,
                           f"benchmark complete in {duration_s:.0f}s (gates: {gates}); results + log captured",
                           log, img, duration_s, report_saved=report_saved)
```

- [ ] **Step 6: Verify the suite still imports/passes (no new unit test — VNC path needs a live guest)**

Run: `.venv/bin/python -m pytest -q`
Expected: PASS (all existing + 15 new bench_export tests; nothing broken by the import/flag).

- [ ] **Step 7: Commit**

```bash
git add SheepShaver/e2e/sse2e/vnc.py SheepShaver/e2e/sse2e/scenario.py
git commit -F- <<'EOF'
feat(e2e): drive Cmd-T to save Speedometer's text report (best-effort, additive)

After the benchmark, type the deterministic report name into the Save Text Report
dialog so the host can extract scores post-shutdown. A save failure never fails the run.
EOF
```

---

### Task 5: Wire the export into `run_benchmark.py`

**Files:**
- Modify: `SheepShaver/e2e/run_benchmark.py`

- [ ] **Step 1: Add `time` + `bench_export` to the imports**

In `run_benchmark.py`, the import block currently is:

```python
import sys
import os
import tempfile
from pathlib import Path

from sse2e import config, disk, runner, scenario
```

Change the last line to include `bench_export` and add `import time`:

```python
import sys
import os
import time
import tempfile
from pathlib import Path

from sse2e import bench_export, config, disk, runner, scenario
```

- [ ] **Step 2: Call the export after a passing run**

In `main()`, replace the result-printing tail (around line 54-58):

```python
    print(f"{'PASS' if res.ok else 'FAIL'}: {res.reason}")
    if res.result_image:
        print(f"  results image: {res.result_image}")
    print(f"  emulator log:  {artifacts / 'benchmark-emulator.log'}")
    return 0 if res.ok else 1
```

with:

```python
    print(f"{'PASS' if res.ok else 'FAIL'}: {res.reason}")
    if res.result_image:
        print(f"  results image: {res.result_image}")
    print(f"  emulator log:  {artifacts / 'benchmark-emulator.log'}")

    # Best-effort benchmark-history export. Collect+report only: nothing here can flip a
    # PASS to FAIL. The run-copy disk (run_disk) still exists in its tempdir at this point.
    if res.ok:
        try:
            ts = time.strftime("%Y-%m-%dT%H-%M-%S")
            raw = bench_export.extract_report(str(run_disk))
            if raw is None:
                why = ("hfsutils missing — `make e2e-setup`"
                       if not bench_export.hfsutils_available()
                       else f"no '{bench_export.REPORT_NAME}' on the disk (Cmd-T save may not have taken)")
                print(f"  history: report not extracted ({why})")
            else:
                rep = bench_export.parse_report(raw)
                hist_root = artifacts / "benchmark-history"
                bench_export.archive_run(
                    report=rep, raw_text=raw,
                    png=Path(res.result_image) if res.result_image else None,
                    history_root=hist_root, timestamp=ts,
                    duration_s=res.duration_s,
                    jit_blocks=bench_export.parse_jit_blocks(res.log))
                print(bench_export.format_delta(hist_root / "history.csv"))
                print(f"  archived: {hist_root / ts}")
        except Exception as e:
            print(f"  history: export skipped ({e})")
    return 0 if res.ok else 1
```

- [ ] **Step 3: Sanity-check it imports and unit tests still pass**

Run: `.venv/bin/python -c "import run_benchmark"` then `.venv/bin/python -m pytest -q`
Expected: import OK; all tests PASS.

- [ ] **Step 4: Commit**

```bash
git add SheepShaver/e2e/run_benchmark.py
git commit -F- <<'EOF'
feat(e2e): export + archive benchmark history after a passing run, print the delta

Extract the Speedometer text report from the run-copy disk (hfsutils), parse, archive
to gitignored artifacts/benchmark-history/, and print PR/CPU/... vs the previous run.
EOF
```

---

### Task 6: Doctor check + docs

**Files:**
- Modify: `SheepShaver/e2e/doctor.py`
- Modify: `SheepShaver/e2e/README.md`
- Modify: `SheepShaver/docs/USER-HANDBOOK.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Add an hfsutils check to the doctor**

In `doctor.py`, find where the Homebrew-lib checks run (the `otool`/brew section) and add a
check that `bench_export.hfsutils_available()` is true; print a ✓/✗ line. ✗ is a **warning,
not a hard fail** — the smoke/benchmark still run without history export. Example (adapt to
the file's existing check helper/printing style):

```python
from sse2e import bench_export
...
if bench_export.hfsutils_available():
    ok("hfsutils present (benchmark history export enabled)")
else:
    warn("hfsutils missing — benchmark history export disabled. Install: brew install hfsutils")
```

- [ ] **Step 2: Document the feature in the e2e README**

In `e2e/README.md`, under the benchmark description / "What it verifies", add a short
subsection:

```markdown
### Benchmark history

After a passing `make e2e-bench`, the harness saves Speedometer's **text report** in-guest
(Cmd-T), extracts it host-side with **hfsutils** (no extra boot), and archives each run to
the gitignored `artifacts/benchmark-history/<timestamp>/` (raw `report.txt`, `scores.csv`,
result PNG) plus an append-only `history.csv`. It prints the delta vs the previous run:

    benchmark history: 2026-06-05T16-16-03  (vs 2026-06-04T09-02-11)
      PR        30.103 ->  31.402  (+4.3%)
      ...

This is **collect + report only** — the numbers never fail the run; they're for tracking
JIT/boot performance over time. Needs `brew install hfsutils` (checked by `make e2e-setup`).
```

- [ ] **Step 3: Note it in the user handbook**

In `SheepShaver/docs/USER-HANDBOOK.md`, in the e2e/benchmark section, add one line pointing
at `artifacts/benchmark-history/` and the `history.csv` trend table + the `hfsutils`
requirement. (Match the surrounding doc's heading style.)

- [ ] **Step 4: Changelog entry**

In `CHANGELOG.md`, under today's `## 2026-06-05`, add an entry:

```markdown
### [SheepShaver] E2E benchmark history export

`make e2e-bench` now saves Speedometer's text report in-guest (Cmd-T), extracts it
host-side via hfsutils (no extra boot), and archives each run under gitignored
`artifacts/benchmark-history/<timestamp>/` plus an append-only `history.csv`, printing the
PR/CPU/Graphics/Disk/Math delta vs the previous run. Collect+report only (no regression
gate). Design: `docs/superpowers/specs/2026-06-05-benchmark-result-export-design.md`.
```

- [ ] **Step 5: Run the full unit suite + doctor**

Run: `cd SheepShaver && make e2e-test` then `make e2e-setup`
Expected: all unit tests PASS; doctor prints the new hfsutils ✓ (or a non-fatal ✗ with the
install hint).

- [ ] **Step 6: Commit**

```bash
git add SheepShaver/e2e/doctor.py SheepShaver/e2e/README.md SheepShaver/docs/USER-HANDBOOK.md CHANGELOG.md
git commit -F- <<'EOF'
docs(e2e): document benchmark history export + doctor hfsutils check
EOF
```

---

## Verification gate (first real boot — spec §2)

After implementation, the user runs one `make e2e-bench` on a real boot to confirm:
1. The Cmd-T save dialog accepts the typed name and Return saves `e2ereport`.
2. hfsutils extracts it and `parse_report` finds `PR/CPU/Graphics/Disk/Math`.

If the text report turns out to contain only the hardware report (no sub-scores), adjust the
guest step to bring the **results window** frontmost before Cmd-T, or wire the OCR-the-PNG
fallback (already a roadmap item). The raw `report.txt` is archived regardless, so the first
run is never wasted — we can refine the parser against a real captured report.

## Self-review notes

- **Spec coverage:** extraction (T1), parsing (T2), archive+history+delta (T3), guest save
  (T4), wiring+print (T5), doctor+docs (T6) — all spec sections covered.
- **Type consistency:** `Report` (scores/rom_version/ram_k/fields), `REPORT_NAME`,
  `HISTORY_COLUMNS`, `BenchResult.report_saved` used consistently across tasks.
- **Additive invariant:** every guest/host export step is wrapped so a green benchmark stays
  green (T4 try/except, T5 `if res.ok` + try/except, T6 doctor warn-not-fail).
