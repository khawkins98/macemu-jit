import csv as _csv
from pathlib import Path

from sse2e import bench_export


# --- _find_in_listing: a real `hls -R -F` shape (root listing first, then ':Folder:' headers) ---

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


def test_find_report_default_matches_report_named_file():
    # No explicit name -> match Speedometer's default 'Power Macintosh Report' by substring.
    listing = ":Desktop Folder:\nMachine Records\nPower Macintosh Report\nSpeedometer 4.02*\n"
    assert bench_export._find_report(listing, None) == ("Desktop Folder", "Power Macintosh Report")


def test_find_report_default_none_when_no_report_file():
    listing = ":Desktop Folder:\nMachine Records\nSpeedometer 4.02*\n"
    assert bench_export._find_report(listing, None) is None


def test_find_report_exact_name_when_given():
    assert bench_export._find_report(_LISTING, "e2ereport") == ("Desktop Folder", "e2ereport")


def test_walk_listing_yields_files_with_folders_skipping_subdir_entries():
    # Shared walk behind both _find_in_listing and list_files: files only, with their folder.
    got = list(bench_export._walk_listing(_LISTING))
    assert ("Desktop Folder", "e2ereport") in got
    assert ("Desktop Folder", "Machine Records") in got
    assert ("System Folder", "Finder") in got
    # "Preferences:" is a subfolder entry, not a file — must be excluded.
    assert all(name != "Preferences" for _, name in got)


# --- parse_report ---

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


# --- archive_run + history + format_delta ---

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
    assert "no runs" in bench_export.format_delta(Path("/nonexistent/history.csv"))


# --- summarize + format_summary (less-noisy multi-run signal) ---

def test_summarize_median_min_max_and_cv():
    r1 = bench_export.parse_report(_REPORT)                                  # CPU 64.265
    r2 = bench_export.parse_report(_REPORT.replace("CPU: 64.265", "CPU: 66.265"))
    r3 = bench_export.parse_report(_REPORT.replace("CPU: 64.265", "CPU: 65.265"))
    s = bench_export.summarize([r1, r2, r3])
    assert s["cpu"]["n"] == 3
    assert s["cpu"]["median"] == 65.265
    assert s["cpu"]["min"] == 64.265 and s["cpu"]["max"] == 66.265
    assert s["cpu"]["cv_pct"] > 0          # the three CPU values differ -> nonzero noise
    assert s["math"]["cv_pct"] == 0.0      # Math identical across all three -> zero noise


def test_summarize_skips_missing_metric_and_accepts_plain_dicts():
    s = bench_export.summarize([{"cpu": 10.0}, {"cpu": "12.0"}])  # plain dicts, one stringy
    assert s["cpu"]["n"] == 2 and s["cpu"]["median"] == 11.0
    assert "disk" not in s                                         # never present -> omitted


def test_format_summary_flags_noisy_metric():
    # Disk swings wildly (high CV), CPU steady (low CV).
    rows = [{"cpu": 64.0, "disk": 5.0}, {"cpu": 64.1, "disk": 15.0}]
    out = bench_export.format_summary(bench_export.summarize(rows))
    assert "Disk" in out and "<- noisy" in out
    assert "CPU" in out and out.count("<- noisy") == 1   # only the Disk *row* is flagged


def test_format_summary_empty():
    assert "no scores" in bench_export.format_summary({})


def test_read_history_roundtrip_and_missing(tmp_path):
    root = tmp_path / "benchmark-history"
    assert bench_export.read_history(root / "history.csv") == []   # absent -> []
    bench_export.archive_run(report=_report(), raw_text=_REPORT, png=None,
                             history_root=root, timestamp="t1")
    bench_export.archive_run(report=_report(), raw_text=_REPORT, png=None,
                             history_root=root, timestamp="t2")
    rows = bench_export.read_history(root / "history.csv")
    assert [r["timestamp"] for r in rows] == ["t1", "t2"]   # oldest-first
    # the "rows appended this batch" slice the multi-run orchestrator relies on:
    assert len(rows[1:]) == 1 and rows[1:][0]["timestamp"] == "t2"
