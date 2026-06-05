# SheepShaver E2E VNC harness

A scriptable, system-level regression gate that boots SheepShaver from a **repo-tracked isolated
config** (never your `~/.sheepshaver_prefs`) and checks the emulator actually works end-to-end. It
complements `make test-jit` / `SS_JIT_VERIFY` (which test the JIT *per-instruction*) at the layer
they can't reach: "does it still boot to the Finder, run, and shut down cleanly after my change?"

Two scenarios:

| `make e2e` (smoke) | `make e2e-bench` (benchmark) |
|---|---|
| Boot the read-only **ISO** → request a clean shutdown via the **host→guest hook** → assert clean exit. No GUI driving (VNC used only for an optional screenshot). | Boot the **Mac OS 9 + Speedometer disk** → drive the full Speedometer suite over **VNC** → capture the result image → clean shutdown. |
| `PASS: clean lifecycle: booted to Finder, clean shutdown, exit 0` | `PASS: benchmark complete in 39s (gates: splash=6.4s choose=0.2s)` |

## How it works (the signal-driven design)

The harness is **driven by deterministic signals the emulator emits from its idle hook**
(`SheepShaver/src/emul_op.cpp`), not by fixed sleeps or screenshot-scraping:

- **`[BOOT] idle …`** — first Process-Manager idle (boot-ready).
- **`[READY] desktop settled …`** — the Finder has been frontmost + non-modal for a ~2 s dwell (a
  more robust "desktop actually usable" marker than first-idle).
- **`[APP] frontApp='…' modal=N win=0x… title='…'`** — fires on a frontmost-app / modal / window-title
  change. The benchmark **gates** on these: it waits for `Speedometer` to launch and for the
  `All Done!` alert title, and each drive step proceeds the instant the guest reaches the next window
  state (printing `[gate] <step>: Xs`) and **resends a key that didn't take** — so it's faster than
  fixed sleeps and never races. See the signal table in "What it verifies".
- **Shutdown** is a host→guest hook: `SIGUSR1` → the emulator injects the ADB Power key, waits for
  the Shut Down dialog (gated on it being modal), confirms with Return (resending if needed), and the
  guest runs its *real* shutdown → `OP_POWEROFF` → "Shutdown complete." → clean exit.

The `runner` spawns the emulator + captures the log; `observe` parses the signals; `scenario` is the
ordered drive logic; `vnc` is the thin keystroke/screenshot client. Each is unit-testable in
isolation (see "Offline unit tests").

- **Design + full rationale:** `docs/superpowers/specs/2026-06-04-e2e-vnc-harness-design.md`
  (§14 shutdown hook · §15 benchmark · §17–§20 SDL3/signal-gating/robustness · §16 open work)
- **Plan:** `docs/superpowers/plans/2026-06-04-e2e-vnc-harness-p1.md`
- CI integration is **future work** — see ROADMAP A5 for the macOS-runner complications.

---

## What it verifies

The emulator emits deterministic lifecycle signals from its idle hook (`emul_op.cpp`):

| Signal | Meaning | Stream |
|--------|---------|--------|
| `[BOOT] idle frontApp='…' modal=N win=0x… title='…' ticks=N (Xs)` | first Process-Manager idle (boot-ready); the smoke checks `frontApp='Finder'` + `modal=0` | stderr |
| `[READY] desktop settled frontApp='Finder' menubar=N modal=0 ticks=N (Xs)` | the desktop has been Finder-frontmost + non-modal for a ~2 s dwell — a more robust "actually usable" marker than first-idle | stderr |
| `[APP] frontApp='…' modal=N win=0x… title='…' ticks=N` | fires on a frontmost-app / modal / front-window-title change. The benchmark **gates on this**: it waits for `frontApp` containing `Speedometer` (launch) and for `title='All Done!'` (suite finished), and each drive step is signal-gated (look for `[gate] <step>: Xs` in the run output), not fixed sleeps. | stderr |
| `Shutdown complete.` | the guest ran its real shutdown (flush/unmount) | **stdout** |
| `PPC-JIT-A64: session …` + exit 0 | host process exited cleanly (atexit ran) | stderr |

`win=` is the front WindowRecord pointer and `title=` its title (sanitized; a literal `'` becomes a
backtick); background-extension churn frames are suppressed. A force-kill (timeout) or a
`frontApp != Finder` / `modal=1` idle is a **FAIL**, not a pass. A `'?'` no-boot-disk boot usually
means a stray SheepShaver still holds the disk image — the runner's pre-flight kills/reaps strays and
refuses to launch if the image is held; if it still hangs, a host restart clears graphics/VBL
degradation from many prior launches.

### Benchmark history

After a passing `make e2e-bench`, the harness saves Speedometer's **text report** in-guest
(Cmd-T → "Save Text Report", accepting the default name "Power Macintosh Report"), extracts it
host-side with **hfsutils** (no extra boot, reading the unmounted run-copy image directly), and
archives each run to the gitignored `artifacts/benchmark-history/<timestamp>/` (raw `report.txt`,
`scores.csv`, result PNG) plus an append-only `history.csv`. It prints the delta vs the previous
run:

```
benchmark history: 2026-06-05T18-57-13  (vs 2026-06-05T18-54-50)
  CPU            63.864 ->      66.035  (+3.4%)
  Graphics       39.185 ->      39.640  (+1.2%)
  Disk            9.052 ->       9.650  (+6.6%)
  Math        11810.757 ->   12614.100  (+6.8%)
archived: artifacts/benchmark-history/2026-06-05T18-57-13/
```

This is **collect + report only** — the numbers never fail the run; they're for tracking
JIT/boot performance over time. The benchmark runs on a throwaway per-run clonefile copy of the
disk, so nothing accumulates on the master image. Needs `brew install hfsutils` (checked by
`make e2e-setup`; without it the run still passes, just skips history).

**How the unattended shutdown works** (non-obvious): the Power-key shutdown hook only raises the
Shut Down dialog at the **Finder**, not over a frontmost app — so after saving, the harness
quits Speedometer **keyboard-only** (Cmd-Q → Return through "Save before quitting?" / record-save
dialogs) back to the Finder, where the hook shuts down. (We use the keyboard because it's reliable.
VNC *clicks* work fine too — verified on both SDL2 and SDL3; an earlier "clicks don't register"
scare was a misdiagnosis, see `LEARNINGS.md`.)

**Less-noisy measurement — `SS_E2E_RUNS=N`.** A single run is host-load sensitive, so for a trend
point run several and let the harness aggregate:

```bash
SS_E2E_RUNS=3 make e2e-bench   # 3 boots, then a batch summary:
#   benchmark summary (3 runs) — median ± run-to-run noise:
#     CPU         64.000  ± 0.2%  [63.900..64.200] n=3
#     Math     12000.000  ± 0.1%  [11990.000..12010.000] n=3
#     Disk         8.000  ±34.4%  [5.000..12.000] n=3  <- noisy, treat with caution
```

It reports the **median** (robust to a one-off spike) and each metric's **CV%** (run-to-run
noise), flagging any metric above 5%. The CV% is measured *per batch* and reflects **host
state**, so use it as a "is this measurement trustworthy?" signal:
- On a **quiet** machine, compute metrics (CPU/Math) settle to <1% and **Disk** is the noisy one
  (I/O, tens of %) — trust CPU/Math for JIT-perf trends, not Disk.
- Under **host load**, CPU contention makes *everything* noisy (we measured CPU/Math ~7% on a
  busy host) — if the whole batch is flagged noisy, the numbers aren't reliable; re-run on an
  idle machine with more runs (`SS_E2E_RUNS=5`).

**`PR` (PowerRating) is deliberately not trended** — it's a disk-weighted composite, so it
inherits Disk's noise and misleads as a "performance" number (and it's panel-only anyway). We
trend the component scores instead.

---

## Prerequisites

1. **A logged-in macOS GUI session.** SDL opens a real window (you can watch the boot on screen),
   which needs an active WindowServer. This will **not** run over a bare SSH session with no
   desktop, and there is no Xvfb equivalent on macOS. (A self-hosted CI Mac must auto-login.)
2. **SheepShaver built** — `SheepShaver/src/Unix/SheepShaver` exists (see build below).
3. **Python 3** — the harness venv (`vncdotool`, `pytest`, `Pillow`, `imagehash`) auto-installs on
   first `make e2e`; no manual setup.
4. **Three assets** (large, not redistributable) — a ROM + boot media. They live in the gitignored
   **`assets/`** directory; drop a **symlink or copy** there and the harness finds them. See
   **[`assets/README.md`](assets/README.md)** for what each is and a one-line symlink setup. Missing
   asset → `make e2e` fails fast with a message pointing back there.
   - **`assets/rom.rom`** — an OldWorld PowerPC Mac ROM (boots the emulated Mac).
   - **`assets/smoke.iso`** — a read-only bootable Mac OS 8.x CD (the smoke medium; read-only ⇒ can't
     get dirty ⇒ reproducible). Used by `make e2e`.
   - **`assets/bench.dsk`** — a writable Mac OS 9 + Speedometer disk (copied per-run). Used by
     `make e2e-bench`, or the smoke with `SS_E2E_MEDIUM=disk`.

   Resolution order per asset: env var (`SS_E2E_ROM` / `SS_E2E_ISO` / `SS_E2E_DISK`) → `assets/` →
   a legacy `/Users/Shared/macemu/` default. The *prefs* are NOT an asset — they're rendered per-run
   from the tracked `config/*.template` files.

---

## Quick start

```bash
cd SheepShaver
make e2e-setup    # FIRST TIME: guided check — venv, build, Homebrew libs, assets (offers to link them)
make e2e          # smoke: build, boot, clean-shutdown, assert exit 0   (PASS / non-zero FAIL)
make e2e-bench    # benchmark: boot Mac OS 9 + Speedometer, drive the suite, capture results
make e2e-test     # offline unit tests (no emulator / assets / GUI)
```

**That's the whole setup.** `make e2e-setup` is the friendly front door: it verifies the build, the
Homebrew libs (`sdl3`/`vde`/`libvncserver`), and the three assets, prints a ✓/✗ report with the exact
fix for any gap, and — in a terminal — **offers to symlink ROM/ISO/disk files it finds** (e.g. under
`/Users/Shared/macemu` or `~/Downloads`) into `assets/`. The Python venv **auto-installs on first
run** (cached, like `npm install`) — no manual `venv`/`pip`; `make e2e` builds the emulator for you.
`make e2e-clean` removes the venv.

> First-ever build also needs a one-time `configure` — see the SheepShaver build commands in
> `CLAUDE.md` / the repo build docs. After that, `make e2e` is incremental.

Variants:

```bash
SS_E2E_MEDIUM=disk make e2e     # run the smoke on the writable Mac OS 9 disk instead of the ISO
SS_E2E_RUNS=5 make e2e-bench    # 5 benchmark runs -> a median ± CV% summary (less-noisy trend)
```

### Environment variables

| Variable | Default | Effect | Used by |
|----------|---------|--------|---------|
| `SS_E2E_ROM` / `SS_E2E_ISO` / `SS_E2E_DISK` | resolve via `assets/` then a legacy path | Point at a specific ROM / smoke ISO / benchmark disk without editing tracked files | both |
| `SS_E2E_MEDIUM` | `iso` | `iso` (read-only, default) or `disk` for the **smoke** boot medium | `make e2e` only (the benchmark is always the disk) |
| `SS_E2E_RUNS` | `1` | Run the benchmark N times and print a median ± per-metric CV% batch summary | `make e2e-bench` |
| `SS_E2E_SHUTDOWN_TIMEOUT` | `60` | Seconds to wait for the post-benchmark shutdown before force-killing (raise it to watch a stuck shutdown on screen) | `make e2e-bench` |

A smoke run is ~1 min (boot ~5 s, shutdown flush ~20–30 s). The SDL window appears on your screen and
**VNC is served on port 5950** if you want to watch live. On failure, `artifacts/fail.log` + any
screenshots are written; `make e2e-bench` also writes `artifacts/benchmark-result.png` (PR/CPU) and
`artifacts/benchmark-emulator.log`.

### Offline unit tests (no boot, no assets)

```bash
make e2e-test          # from SheepShaver/ — 67 tests: observe / runner / disk / config / imagecmp / bench_export / scenario
```

These run anywhere (CI included) — they exercise the signal parsing, teardown, and prefs logic with
fixtures, no emulator required.

---

## What's checked in vs. system-specific

- **In the repo:** the prefs config template (`config/test.prefs.template`), all harness code, the
  unit tests. The config pins `screen win/640/480`, `idlewait true` (required — it installs the
  `SynchIdleTime` patch that fires the boot-ready signal), VNC on, and headless GUI.
- **System-specific (env-resolved):** ROM + disk paths only. Set `SS_E2E_ROM` / `SS_E2E_DISK` to
  point at your assets without editing any tracked file.

---

## Troubleshooting

- **`boot timed out (no [BOOT] idle …)`** — the guest never reached the idle Finder desktop. Check
  `artifacts/fail.log`; the disk may be non-bootable (a blank disk gives the "?" icon and this
  message — that's the correct failure), or `idlewait` got turned off in the config.
- **`boot blocked on dialog (frontApp=… modal=…)`** — the guest idled on a modal dialog instead of
  the desktop. The default **read-only ISO can't trigger this** (no disk-repair prompt — a CD is
  never dirty). It can only occur on the disk-boot path with a dirty master; re-create that master
  from a clean, cleanly-shut-down install.
- **`shutdown timed out`** — the guest didn't shut down after the `SIGUSR1` hook fired. Check the
  log for the `[BOOT] host shutdown requested … Power key … Return` lines (the in-emulator
  sequence) and `Shutdown complete.`. The hook needs an OS whose power key raises the Shut Down
  dialog (verified on 8.6 + 9.0.4); a very different System might need the dialog-confirm key
  adjusted in `emul_op.cpp` (`e2e_check_host_shutdown`).
- **Two instances / port in use** — kill strays first: `pkill -9 -x SheepShaver`.
- **Nothing happens / SDL error** — you're likely not in a logged-in GUI session (see Prerequisite 1).

---

## Status & roadmap

P1 lifecycle smoke (`make e2e`) and P2 benchmark automation (`make e2e-bench`) both **complete**.
Boot detection and shutdown are host→guest hooks; the benchmark drives Speedometer over VNC. Open
(ROADMAP A5):
- **Benchmark score parsing** — OCR the PR/CPU from `benchmark-result.png` (or read Speedometer's
  "Machine Records" file) to turn the captured image into an automated perf-regression number.
- **P2 (visual)** — golden-image screenshot diff (masked perceptual hash) for the smoke.
- **P3** — declarative scenario DSL.
- **CI integration** — needs a self-hosted macOS runner with a GUI session + asset provisioning;
  see ROADMAP A5 for the full complication list.
