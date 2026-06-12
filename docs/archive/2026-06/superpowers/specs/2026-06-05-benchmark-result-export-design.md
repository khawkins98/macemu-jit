> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone shipped
>

# Benchmark result export & history — design

> **Status:** ✅ SHIPPED 2026-06-05 (commits `cad1af88`…`286b0aa2`). The design below is
> preserved as written; read **§0 As-built** first for what actually shipped — a few things
> changed during implementation.
> **Topic:** Automate capture of Speedometer's text report from each `make e2e-bench`
> run, extract it host-side, archive it with a timestamp, and report the delta vs the
> previous run — so JIT/boot performance is trackable over time.

---

## 0. As-built (2026-06-05) — what shipped vs. this design

The pipeline works end-to-end and shuts down **unattended** (verified: 3 consecutive PASSes,
`Shutdown complete.` + clean exit). Deltas from the design below:

- **Save uses Speedometer's DEFAULT name, not a typed name.** Typing `e2e-report` into the
  save dialog proved unreliable over VNC (keys dropped/leaked; the file saved under the
  default name anyway). So we press **Cmd-T → Return** (accept the default "Power Macintosh
  Report"), and the host matches it by the `REPORT_MATCH` substring (`"report"`) instead of an
  exact name. (`bench_export._find_report`.)
- **§2 verification gate RESOLVED:** the Cmd-T text report DOES contain `CPU/Graphics/Disk/Math`
  (+ many FPU sub-scores) — but it does **NOT** contain **`PR`** (PowerRating is panel-only).
  So PR is **not** trended yet — see "Open data gaps" below.
- **Shutdown is keyboard-only quit-to-Finder.** New finding: the host→guest Power-key shutdown
  hook only raises the Shut Down dialog **at the Finder**, not over a frontmost app — so the
  benchmark (Speedometer frontmost) hung. Fix: after saving, **Cmd-Q**, then answer
  Speedometer's "Save before quitting?" (Yes/No/Cancel) + any record-save dialog with **Return**
  until it quits to the Finder, where the existing hook shuts down. (`scenario.py`.)
- **VNC clicks — there was never a click bug (misdiagnosis).** The shutdown is keyboard-only
  because that's reliable. The earlier "VNC clicks don't register" was wrong: building **both
  SDL2 and SDL3** and driving an **instant** `mousePress` click works on both (it opens the Apple
  menu and selects a menu item that opens the named "About This Computer" window). The committed
  `SDL_PushEvent` injection path is correct; the "hold the button" change was reverted. No proven
  SDL2-vs-SDL3 click difference. Full trail: `LEARNINGS.md` and memory `e2e-vnc-click-injection`.

**Data decisions & remaining gaps:**
- **`PR` (PowerRating) is deliberately NOT trended.** It's a disk-weighted composite, so it
  inherits the Disk metric's high run-to-run noise and misleads as a "performance" number. It's
  also panel-only (absent from the text report). We trend the component scores instead and
  exclude `pr` from aggregation (`bench_export._AGG_METRICS`). *(Resolved, not a gap.)*
- **Run-to-run noise — addressed via `SS_E2E_RUNS=N`.** A single run is host-load sensitive
  (e.g. CPU 33 vs 64 on a degraded host). Setting `SS_E2E_RUNS=N` runs the benchmark N times
  (each in its **own subprocess** — vncdotool's Twisted reactor can't restart in-process) and
  prints a batch **summary**: the **median** per metric (robust to a one-off spike) and each
  metric's **CV%** (coefficient of variation = run-to-run noise), flagging >5%. The CV% is
  host-state dependent: on a quiet host CPU/Math settle to <1% and **Disk** is the noisy one
  (I/O); under host load CPU contention makes everything noisy (measured ~7% on a busy host). So
  the CV% is the honest "is this batch trustworthy?" signal — for clean numbers, run on an idle
  machine with more runs. (`bench_export.summarize`/`format_summary`/`read_history`.) *(Resolved.)*
- **Drive/gate logic now unit-tested (the FakeRunner gap, §20).** `scenario.py`'s save/quit logic
  was extracted into testable helpers (`_save_text_report`, `_quit_to_finder`, `_await_front_app`),
  and `tests/test_scenario.py` covers them with a `FakeRunner` (scripted log lines) + `FakeVnc`
  (records keys) + a fake clock — 14 offline tests, no boot. *(Resolved.)*
- **`back-to-finder` gate hardened.** It keyed off the **noisy `frontApp` signal** (a single
  spurious `'Finder'` frame), giving false confidence. Replaced with `_await_front_app`, which
  requires `avoid` (Speedometer) to be ABSENT across a window of `settle` consecutive frames and
  `want` (Finder) present — so it only fires once Speedometer has really quit. Unit-tested against
  the exact spurious-frame false positive. *(Resolved.)*
- **Honest benchmark PASS.** A green `e2e-bench` now requires the guest's real clean-shutdown
  signatures (`saw_clean_shutdown`: "Shutdown complete." + the atexit line), not just a 0 exit —
  so PASS means the automation genuinely drove an unattended shutdown, closing the false-PASS gap
  this session exposed. *(Resolved.)*

**Goal:** After the e2e Speedometer benchmark finishes, save its **text report** inside the
guest, extract that file **host-side** (no extra boot), parse the scores, archive each run
under a timestamped folder, and print how this run compares to the last — without ever
dirtying the master disk image.

---

## 1. Background — where this plugs in

`make e2e-bench` (`SheepShaver/e2e/`) already:

- boots a **per-run APFS clonefile copy** of `bench.dsk` (`disk.copy_pristine` → a
  `tempfile.mkdtemp` work dir; the master is never written),
- drives Speedometer 4.02's full suite over VNC, gating on the enriched `[APP]` signals,
- waits for the **"All Done!"** alert, captures `artifacts/benchmark-result.png`, then
  requests a clean shutdown.

What's missing: the **numbers** only exist as pixels in the screenshot. There's no
machine-readable record and no history, so "did this build get faster?" can't be answered.

### Why the master disk never bloats (the user's "don't fill the disk" concern)

The benchmark writes to the **throwaway clonefile copy**, not `bench.dsk`. Whatever
Speedometer saves lands on that copy, we extract from it after shutdown, and the copy's
tempdir is discarded. The master stays pristine **structurally** — no per-run cleanup of
the guest disk is required.

---

## 2. The data source — Speedometer's Text Report (Cmd-T)

Confirmed from the live app (screenshots, 2026-06-05):

- **File → Save Text Report… (Cmd-T)** writes a plain **`TEXT`-type** file (data fork; the
  sample was "Power Macintosh Report", 1,854 bytes). This is the right source.
- **File → Save Machine Record… (Cmd-S)** writes Speedometer's *binary* records database —
  opaque, rejected.
- The text report is flat `Key: Value` lines and embeds the test scores, e.g.
  `CPU: 64.265` (matching the results panel). The results panel also shows
  `Graphics 45.374`, `Disk 9.815`, `Math 12737.795`, and the headline **`PR: 30.103`**
  (PowerRating, the single overall number), plus machine fields (`ROM Version: $077D`,
  `Physical RAM: 262144K`, …).

**Verification gate for implementation (honest unknown):** confirm the Cmd-T text report
actually contains the four sub-scores (`Graphics`/`Disk`/`Math`) and `PR`, not only the
hardware/Gestalt dump. If a report saved from the *hardware* window omits the scores, the
fix is to ensure the **results window is frontmost** before Cmd-T, or fall back to OCR of
`benchmark-result.png` for the missing scores (OCR is already a roadmapped item). The raw
report is archived verbatim regardless, so no data is lost while the parser is tuned.

---

## 3. Pipeline

> ⚠️ **Superseded in part — see §0.** This section's deterministic typed name (`e2e-report`) was
> NOT what shipped: typing proved unreliable, so we accept Speedometer's default name ("Power
> Macintosh Report") and match it by `REPORT_MATCH` substring host-side. The hfsutils extraction
> mechanics below are accurate.

```
guest  (after "All Done!" dismiss, before vnc.close()):
        Cmd-T  →  the name field is pre-selected, so type a deterministic name
        "e2e-report"  →  Return.
        (Deterministic name avoids a "replace existing?" prompt: a fresh clonefile copy
         never already contains "e2e-report", because runs only ever write to copies.)

host   (after clean shutdown, in run_benchmark.py, run-copy still in its tempdir):
        hmount <run-copy.dsk>
        hls -R -F            # locate "e2e-report" wherever the save dialog put it
        hcopy -t <path> <tmp/e2e-report.txt>   # -t = CR→LF text translation
        humount
        → parse Key:Value → write per-run archive + append history.csv → print delta
```

Host-side extraction uses **hfsutils** (`hmount`/`hls`/`hcopy`/`humount`, already installed
at `/opt/homebrew/bin`). It reads the **unmounted** image directly — **no second boot**, no
macOS HFS-mount dependency. Recursive `hls -R` means the save dialog's default *folder*
doesn't matter — we find `e2e-report` by name wherever it landed.

---

## 4. Components

| File | Change |
|------|--------|
| `e2e/sse2e/scenario.py` | In `run_benchmark`, after dismissing "All Done!" and before `vnc.close()`: drive **Cmd-T → type `e2e-report` → Return** to save the text report onto the run-copy. Gate/settle briefly (the save dialog is modal; a short dwell or a settle is enough — no new signal needed). Surface a non-fatal note in `BenchResult` if the save keystrokes couldn't be sent. |
| `e2e/sse2e/bench_export.py` *(new)* | Pure host-side logic, fully unit-testable: `extract_report(disk_path) -> str \| None` (hfsutils), `parse_report(text) -> dict` (Key:Value → known score keys + machine fields), `archive_run(parsed, raw_text, png, history_root) -> Path` (writes the per-run dir + appends `history.csv`), `format_delta(history_csv) -> str` (last vs previous). |
| `e2e/run_benchmark.py` | After `scenario.run_benchmark` returns OK, call `bench_export`: extract from the run-copy disk (it has the path — it created `run_disk`), parse, archive, and **print the delta line**. All wrapped so a failure here **does not flip a PASS to FAIL** (decision: collect+report only). |
| `e2e/tests/test_bench_export.py` *(new)* | Unit tests over `parse_report` (fixture report text → expected scores), `archive_run` (writes files + appends a history row), `format_delta` (two rows → a `+x%` string). No emulator/hfsutils needed — `extract_report` is exercised via a tiny checked-in fixture or mocked. |

`scenario.py` stays focused on **guest driving**; `bench_export.py` owns **host-side**
extraction/parsing/archiving; `run_benchmark.py` orchestrates. Each is testable alone.

---

## 5. Data formats & layout

Everything lives under `e2e/artifacts/` — **gitignored, local-only** (decision). Per run:

```
e2e/artifacts/benchmark-history/
  2026-06-05T16-16-03/
    report.txt        # raw Speedometer text report, verbatim (authoritative fallback)
    scores.csv        # this run's parsed row (header + 1 row, human-openable)
    result.png        # copy of benchmark-result.png (visual record)
  history.csv         # append-only master trend table — ONE row per run
```

`history.csv` columns (extra fields appended over time without breaking older rows):

```
timestamp,pr,cpu,graphics,disk,math,duration_s,rom_version,ram_k,jit_blocks,git_sha
```

- `duration_s` comes from the existing suite-runtime measurement (`BenchResult.duration_s`).
- `jit_blocks` parsed from the SDL title-bar / log JIT line if present (else blank).
- `git_sha` stamped host-side (`git rev-parse --short HEAD`) so a row ties to a build.
- Missing/unparsed fields are left blank — a run still archives raw `report.txt` + PNG.

At ~100 bytes/row, thousands of runs keep `history.csv` under a few hundred KB. No
retention cap needed; we keep everything (per the user — "keep the last run" is satisfied
trivially, and full history is essentially free).

---

## 6. Comparison / reporting

> ⚠️ **Superseded in part — see §0.** The example below leads with a `PR` row, but `PR` is
> **deliberately not trended** (disk-weighted, panel-only). The shipped delta covers
> CPU/Graphics/Disk/Math; `SS_E2E_RUNS=N` adds a median ± CV% batch summary.

`format_delta` reads the last two `history.csv` rows and prints a one-block summary at the
end of `make e2e-bench`, e.g.:

```
benchmark history: 2026-06-05T16-16-03  (vs 2026-06-04T09-02-11)
  PR        30.103   →  31.402   (+4.3%)
  CPU       64.265   →  66.010   (+2.7%)
  Graphics  45.374   →  45.110   (-0.6%)
  Disk       9.815   →   9.790   (-0.3%)
  Math   12737.795   → 12740.10  (+0.0%)
  duration    39s    →    38s
archived: e2e/artifacts/benchmark-history/2026-06-05T16-16-03/
```

First-ever run prints just the captured values (no prior row to diff). **No pass/fail
gate** on the deltas (decision) — this is data collection + visibility. `make e2e-bench`
still passes/fails purely on the lifecycle (boot → benchmark completed → clean shutdown).

---

## 7. Failure handling (must not regress the existing gate)

Because this pass is **collect+report only**, every new step is best-effort:

- Cmd-T keystrokes fail / save dialog doesn't behave → log a note; benchmark still PASSes
  (the PNG is still captured).
- `e2e-report` not found on the disk (hfsutils) → print a clear "report not extracted"
  note; PASS unchanged; PNG retained.
- Parse finds no known scores → still archive raw `report.txt` + PNG; `history.csv` row
  written with blanks for the missing metrics.
- `hfsutils` missing → the doctor (`make e2e-setup`) gains a check for `hmount`; the run
  prints "install hfsutils to enable history export" and continues.

The invariant: **a green benchmark stays green**; export only *adds* artifacts.

---

## 8. Decisions (locked)

1. **Storage:** history is **local-only / gitignored** under `artifacts/`. Nothing
   committed to the repo. (Revisit if cross-machine trend sharing is wanted later.)
2. **Scope:** **collect + report deltas only.** No regression gate this pass — that's a
   clean follow-up once there's baseline data to tune a sane threshold (and avoid
   noise-driven false failures).
3. **Source:** Cmd-T **text report**, extracted **host-side via hfsutils**, not Cmd-S
   binary and not OCR.

---

## 9. Deferred / open

- **Regression gate** — fail `make e2e-bench` if `PR` drops > threshold vs a stored
  baseline. Needs baseline + tuned threshold; deferred per decision 2.
- **OCR fallback** — if the text report turns out not to carry the sub-scores, OCR the
  results PNG for `CPU/Graphics/Disk/Math/PR` (already a roadmap item).
- **Score-format conversion** — `history.csv` is the durable format; a tiny `plot`/`view`
  helper (e.g. print an ASCII trend or emit a chart) is a nice-to-have, not in scope.
- **Committing trend data** — if perf-over-time should be shared across clones/branches,
  promote `history.csv` to a tracked file later (decision 1 can be reversed cheaply).

---

## 10. Testing

- **Unit (offline, in `make e2e-test`):** `test_bench_export.py` — `parse_report` over a
  checked-in fixture report, `archive_run` (creates the per-run dir + appends a row),
  `format_delta` (two rows → expected `±x%` text). Keeps the new logic fully covered
  without an emulator, assets, or GUI.
- **Manual/integration:** one real `make e2e-bench` (user-run boot) to confirm the Cmd-T
  drive saves `e2e-report`, hfsutils extracts it, and the parser finds the scores —
  resolving the §2 verification gate.
