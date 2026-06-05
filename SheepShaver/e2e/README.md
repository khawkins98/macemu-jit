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

---

## Prerequisites

1. **A logged-in macOS GUI session.** SDL opens a real window (you can watch the boot on screen),
   which needs an active WindowServer. This will **not** run over a bare SSH session with no
   desktop, and there is no Xvfb equivalent on macOS. (A self-hosted CI Mac must auto-login.)
2. **SheepShaver built** — `SheepShaver/src/Unix/SheepShaver` exists (see build below).
3. **Python 3** — the harness venv (`vncdotool`, `pytest`, `Pillow`, `imagehash`) auto-installs on
   first `make e2e`; no manual setup.
4. **Two assets** (large, not in git), resolved by env var or local-dev default:
   - **ROM** — an OldWorld PPC ROM. `SS_E2E_ROM` (default
     `/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom`).
   - **A bootable ISO** (the default, preferred medium) — a read-only Mac OS 8.x CD that boots to a
     Finder. `SS_E2E_ISO` (default `/Users/Shared/macemu/Mac OS 8.6 Internal Edition.iso`). Because
     a CD is **read-only it can never get dirty** — no disk-repair prompts, no pristine-copy per run,
     fully reproducible. This is why ISO boot is the default.
   - *(Disk medium)* a **writable disk image** — `SS_E2E_DISK` (default
     `/Users/Shared/macemu/macos9_mini.dsk`, a small stripped Mac OS 9.0.4 + Speedometer used for
     benchmarks). Selected with `SS_E2E_MEDIUM=disk`; it boots `config/test.prefs.template` with
     copy-per-run isolation (instant APFS clonefile, so logical image size is irrelevant). The
     default ISO path uses `config/test.prefs.iso.template` and needs no copy.

---

## Quick start

```bash
cd SheepShaver
make e2e          # smoke: build, boot, clean-shutdown, assert exit 0   (PASS / non-zero FAIL)
make e2e-bench    # benchmark: boot Mac OS 9 + Speedometer, drive the suite, capture results
make e2e-test     # offline unit tests (no emulator / assets / GUI)
```

**That's the whole setup.** The Python venv **auto-installs on first run** (cached after, like
`npm install`) — no manual `venv`/`pip`. `make e2e` builds the emulator for you. Other targets:
`make e2e-setup` ((re)create the venv), `make e2e-clean` (remove it).

> First-ever build also needs a one-time `configure` — see the SheepShaver build commands in
> `CLAUDE.md` / the repo build docs. After that, `make e2e` is incremental.

Variants:

```bash
SS_E2E_MEDIUM=disk make e2e     # run the smoke on the writable Mac OS 9 disk instead of the ISO
```

A smoke run is ~1 min (boot ~5 s, shutdown flush ~20–30 s). The SDL window appears on your screen and
**VNC is served on port 5950** if you want to watch live. On failure, `artifacts/fail.log` + any
screenshots are written; `make e2e-bench` also writes `artifacts/benchmark-result.png` (PR/CPU) and
`artifacts/benchmark-emulator.log`.

### Offline unit tests (no boot, no assets)

```bash
make e2e-test          # from SheepShaver/ — 24 tests: observe / runner / disk / config / imagecmp
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
