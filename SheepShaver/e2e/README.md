# SheepShaver E2E VNC harness (P1)

A scriptable end-to-end smoke test: it boots SheepShaver from a **repo-tracked isolated config**
(`config/test.prefs.iso.template` — *not* your `~/.sheepshaver_prefs`), waits for a deterministic
boot-ready signal, requests a clean shutdown via a **host→guest signal hook** (`SIGUSR1`), and
asserts a clean exit. Both the boot-ready signal and the shutdown are proper hooks — VNC is used
only for optional screenshots, not to drive the GUI.

A green run prints:

```
PASS: clean lifecycle: booted to Finder, clean shutdown, exit 0
```

It is the first *system-level* regression gate — complements `make test-jit` / `SS_JIT_VERIFY`
(which test the JIT per-instruction) by checking the emulator actually boots and shuts down.

- Design + rationale: `docs/superpowers/specs/2026-06-04-e2e-vnc-harness-design.md`
  (§12 = implementation outcome, including the shutdown-hook exploration)
- Plan: `docs/superpowers/plans/2026-06-04-e2e-vnc-harness-p1.md`
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
3. **Python 3** with a venv + the harness deps (`vncdotool`, `pytest`, `Pillow`).
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

## One-time setup

```bash
# 1. Build SheepShaver (first time also needs configure — see CLAUDE.md / SheepShaver build docs)
cd SheepShaver/src/Unix
NO_CONFIGURE=1 ./autogen.sh
./configure --enable-sdl-video --enable-sdl-audio --enable-jit \
            --without-gtk --without-x --without-esd \
            CPPFLAGS=-I/opt/homebrew/include LDFLAGS=-L/opt/homebrew/lib
cd ../.. && make build-ss

# 2. Create the Python venv + install harness deps
cd e2e
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt

# 3. Provide the assets: a ROM at SS_E2E_ROM and a bootable ISO at SS_E2E_ISO (defaults above).
#    The default ISO boot needs NO disk setup — the read-only CD is booted directly each run.
#    (Only if you use the disk path instead: provide a cleanly-shut-down master at SS_E2E_DISK.)
```

---

## Run it

```bash
cd SheepShaver
make e2e                       # build-ss + run the smoke; exit 0 = PASS, non-zero = FAIL

# read-only ISO (default) vs the writable Mac OS 9 + Speedometer disk:
cd e2e
.venv/bin/python run_smoke.py                      # ISO medium (default)
SS_E2E_MEDIUM=disk .venv/bin/python run_smoke.py   # disk medium (macos9_mini.dsk)

# Benchmark: boot the Mac OS 9 + Speedometer disk, run the full suite, capture results:
make e2e-bench                                     # from SheepShaver/
# or: cd e2e && .venv/bin/python run_benchmark.py
# Artifacts: e2e/artifacts/benchmark-result.png (PR/CPU numbers) + benchmark-emulator.log
```

A run takes ~1 min on the default ISO path (boot ~5 s, shutdown flush ~20–30 s; no per-run disk
copy — the ISO is read-only). The SDL window appears on your screen and VNC is served on port
**5950** if you want to watch live. On failure, `e2e/artifacts/fail.log` (full merged log) and any
screenshots are written for inspection.

### Offline unit tests (no boot, no assets)

```bash
cd e2e && .venv/bin/pytest -q          # 12 tests: observe / runner / disk / config
```

These run anywhere (CI included) — they exercise the log parsing, teardown, and prefs logic with
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
