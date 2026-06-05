# Design Brief: SheepShaver End-to-End VNC Test Harness

> **Status:** 🟡 Active · **Created:** 2026-06-04 · **Updated:** 2026-06-05
> **Why this doc exists:** Design for an automated boot/run/shutdown E2E harness (ROADMAP A5) —
> the system-level complement to the instruction-level gates, and the unlock for agents to
> self-validate their own JIT changes end-to-end.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo._

**Roadmap home:** Track **A5** (`docs/planning/ROADMAP.md`), complementing A1's instruction-level
gates (`make test-jit`, `SS_JIT_VERIFY`).

---

## 1. Problem & goal

The project has strong *instruction-level* verification (the JIT-equivalence harness, the
differential VERIFY oracle) but **no automated way to test the emulator as a running system** —
"does it actually boot to the Finder, run, and shut down cleanly after a codegen change?" Today
that check is manual: a human boots it and looks at VNC.

Goal: a **scriptable, reproducible end-to-end harness** that spawns SheepShaver, drives it over
its built-in VNC server, observes what happens, and returns a clean pass/fail — usable by an
agent in-session, by the user as a smoke test, and (the bar we're designing for) by an
unattended CI script.

### Why this is feasible

- **The built-in VNC server is bidirectional.** `BasiliskII/src/SDL/vnc_server.cpp` (shared with
  SheepShaver) wires `kbdAddEvent`/`ptrAddEvent` → SDL keycodes → `ADBKeyDown/Up` + ADB mouse.
  The guest can be *driven*, not just viewed.
- **Observability without a guest agent.** Classic Mac OS has no headless automation bridge
  (AppleScript runs *inside* the guest with no host channel). We sidestep that by observing from
  *outside*: the VNC framebuffer, plus the host-side signals the JIT already emits (boot
  heartbeats, `[JIT]` diag log, `atexit` miss report, exit code, serial console).

## 2. Non-goals (YAGNI)

- No guest-side agent / AppleScript bridge / in-guest automation driver.
- No deep *functional* assertions ("the document's bytes are exactly right"). Scope is
  **smoke / lifecycle / visual-checkpoint** regression.
- No New World ROM, no networking-traffic testing, no BasiliskII (SheepShaver only for v1).
- Not a replacement for `make test-jit` / `SS_JIT_VERIFY` — a complement at a different layer.

## 3. Key design decision — observability is hybrid

The deterministic spine is **host-log signals**; framebuffer images are a **tolerant secondary
gate** and human-reviewable artifacts. Exact screenshot hashing is explicitly rejected:

- **Exact MD5 of a full frame is the flakiest possible check** — the menu-bar clock ticks, the
  caret blinks, icon/window positions and the desktop pattern vary, and VNC can deliver a partial
  frame mid-update. It would be red almost every run.
- Instead, for visual checks: (a) restore a **pristine disk image each run** so guest-side state
  doesn't drift; (b) **mask volatile regions** (menu-bar clock, cursor) or compare only a stable
  landmark region; (c) compare with a **perceptual hash + Hamming-distance threshold** (or masked
  pixel-diff with tolerance), not equality.
- **Logs give the exact boolean** (booted / reached steady state / exit 0 / atexit report
  present). Image-hash is used only for what logs can't see ("the app window appeared").

## 4. Architecture

A Python harness (matches existing `*.py` tooling: `jit-analyze.py`, `gen-*-vectors.py`). Four
small, independently-testable units, proposed under `SheepShaver/e2e/`:

| Unit | Responsibility | Depends on |
|------|----------------|------------|
| `runner` | Spawn SheepShaver with an isolated config; capture stderr/diag-log; enforce timeouts; **guarantee teardown** (force-kill on failure path). | subprocess, the test config |
| `vnc` | Thin wrapper over the VNC client: `connect`, `type`, `key`, `click(x,y)`, `capture(region)`. | `vncdotool` |
| `observe` | Log-signal matchers (`wait_for_log(pattern, timeout)`, `assert_clean_exit`) and image checks (masked perceptual-hash vs golden). | `imagehash`, `Pillow` |
| `scenario` | A test = an ordered list of steps. v1 ships one canonical scenario. | the three above |

Each unit answers: *what does it do, how do you call it, what does it depend on* — and is testable
in isolation (e.g., `observe` log matchers against captured log fixtures, no emulator needed).

## 5. The canonical v1 scenario

```
boot  →  (open one app)  →  Special ▸ Shut Down  →  assert clean exit
```

- **Boot:** spawn with the isolated config; `wait_for_log` the steady-state signal; capture a
  "reached Finder" screenshot artifact.
- **Open app (phase 2 — see §8):** drive VNC input to launch a known app from the test disk;
  `wait_for_image` its window region.
- **Shut down (first-class, not `kill`):** drive **Special ▸ Shut Down** over VNC. This doubly
  earns its place — it is the cleanest end-of-test signal *and* a regression test for the
  2026-06-03 clean-shutdown feature (host process exits, runs cleanup, prints the JIT miss
  report).
- **Assert clean exit:** process **exits 0**, the **atexit miss report is present** in stderr, no
  crash. A timeout that has to force-kill is a **FAIL**, never a pass.

## 6. Test isolation & safety (safety-critical)

- **Own config, never the user's.** A dedicated `e2e/config/test.prefs` and a **pristine** small
  disk image copied to a temp path **before each run**. Never touches `~/.sheepshaver_prefs` or
  `/Users/Shared/macemu/*`. (Mac OS writes to the boot volume during boot/shutdown — Desktop DB,
  PRAM — so the pristine image MUST be copy-before-run or a CoW snapshot, or it isn't frozen by
  run 2.)
- **One instance at a time.** Pre-flight `pkill -9 -x SheepShaver` + a lockfile; honors the
  existing "only one emulator instance" invariant.
- **Policy change (deferred edit — see §9):** relax the standing rule from *"agents must not
  launch their own emulator instances"* to *"agents must not launch an emulator instance without
  asking the user first."* Lives in CLAUDE.md (local) and CONTRIBUTING.md "Key Invariants".

## 7. Dependencies

- `vncdotool` (scriptable VNC client — already referenced by the Linux `screenshot` Makefile
  target, but not installed here; `pip install vncdotool`).
- `imagehash` + `Pillow` (perceptual hashing + region masking).
- A small **pristine test disk image** with a known minimal System + one launchable app, stored
  outside the repo (asset, like the existing ROMs/disks under `/Users/Shared/macemu/`).

Rejected alternative: a hand-rolled / `asyncvnc` thin RFB client — more protocol code to maintain
for no v1 benefit over `vncdotool`.

## 8. Phasing

- **P1 — Lifecycle smoke (build first).** One green test: isolated boot → Special ▸ Shut Down →
  assert clean exit, via host-log signals + saved screenshots. No app launch, no golden-diff.
  This is the highest-value regression ("my JIT change broke boot/shutdown") and the
  least-flaky. Scriptable params (ROM/disk/flags) from the start.
- **P2 — Visual checkpoints + app launch.** Add masked perceptual-hash golden comparison and the
  scripted app-launch step, once P1 is proven non-flaky.
- **P3 — Scenario framework.** Generalize steps into a declarative scenario format
  (`boot`/`wait_for_log`/`click`/`type`/`wait_for_image`/`shutdown_clean`) with golden management
  — the full "Playwright-for-VNC" vision. Only if P1/P2 prove the approach earns it.

## 9. Process / coordinated updates (per CONTRIBUTING)

On implementation (deferred now because `ROADMAP.md`/`CONTRIBUTING.md` are being edited by another
agent concurrently — avoid clobbering in-flight work):

- Add a **Track A entry** in `docs/planning/ROADMAP.md` pointing to this brief (A1's
  GUI/lifecycle layer).
- Add this brief to the `docs/planning/README.md` index if it graduates to a `docs/planning/`
  plan; otherwise it stays a spec here.
- Relax the no-boot rule in CONTRIBUTING "Key Invariants" + CLAUDE.md (§6).
- `CHANGELOG.md` entry under **[SheepShaver] Testing & benchmarking** when P1 lands.
- A `make e2e` (or `SheepShaver/e2e/README.md`) entry documenting how to run it.

## 10. Groundwork findings (resolved 2026-06-04, read-only investigation)

1. **Clean-exit signal — SOLVED, deterministic.** A clean guest shutdown prints, in order:
   `"Shutdown complete."` (emul_op.cpp:510) → the `PPC-JIT-A64: session …` atexit block
   (`blocks=… complete=… (100.0%)`, coverage) → process **exit 0**. A crash/kill produces none
   of it. This is the primary pass gate. (Confirmed against a real 43 s boot→shutdown log.)
2. **Boot-complete signal — SOLVED via a small ROM-patch enrichment (see §11).** There is no
   "reached Finder" log line today, and the heuristic alternatives (block-rate collapse / `comp=`
   plateau) **cannot distinguish "idle at the desktop" from "idle on a blocking modal dialog"**
   (disk-repair prompt, etc.) — they look identical. The chosen signal is the **already-trapped
   guest idle path**: `OP_IDLE_TIME` (emul_op.cpp, patched into `SynchIdleTime` ROM trap `0xABF7`,
   gated on the `idlewait` pref) fires exactly when the Process Manager idles. A ~15-line
   enrichment emits a one-shot line carrying guest state to disambiguate. Full design + the
   dialog-false-positive defense in **§11**.
3. **Shutdown trigger — must drive the menu over VNC; no shortcut.** SheepShaver sets
   `signal(SIGINT/SIGTERM, SIG_DFL)` (main_unix.cpp:796) so SIGTERM just *kills* it. Clean exit
   only comes from the guest Special ▸ Shut Down. The VNC framebuffer **is** the Mac framebuffer
   at the same resolution (vnc_server.cpp:205), so at a pinned `screen win/640/480` a coordinate-
   click on the Special menu → Shut Down is deterministic. (No power-key keysym mapping found; the
   keysym table covers Return/arrows/F-keys — usable for dialogs, not for the menu itself.)
4. **Isolation — SOLVED.** `SheepShaver --config <path>` sets `UserPrefsPath` (main_unix.cpp:918),
   so the harness points at its own prefs file; combined with `--nogui`, the spawn never touches
   `~/.sheepshaver_prefs`. The isolated prefs sets `disk` (pristine per-run copy), `rom`,
   `screen win/640/480`, `vncserver true`, a unique `vncport`, `nogui true`.
5. **CI host reality — honest constraint.** `video_sdl3.cpp` calls `SDL_CreateWindow`
   (line 754), which on macOS needs a live **WindowServer (logged-in GUI session)** — there is no
   Xvfb equivalent on macOS. So "classic CI" works on a **logged-in Mac runner**, not a truly
   headless box. (Linux could use Xvfb later; out of scope for v1.) Document this in the README so
   nobody expects it to run on a headless macOS CI agent.
6. **Tooling — available.** Python 3.14, `pip` present; `vncdotool` 1.3.0 installable from PyPI;
   `Pillow` 12.2.0 already installed (`imagehash` for P2 still to add). Pristine-image decision
   for the plan: start by **copy-per-run of a small dedicated test disk** (smaller = faster copy);
   snapshotting `macos86_fresh.dsk` is the fallback if building a minimal disk is too costly.

## 11. Boot-ready signal — the `OP_IDLE_TIME` enrichment + dialog defense

The single emulator-side code change in this feature. Everything else is the external Python
harness; this is the one ~15-line touch to `SheepShaver/src/emul_op.cpp`.

**Why this signal.** `OP_IDLE_TIME` already fires when the guest Process Manager idles
(`SynchIdleTime`, ROM trap `0xABF7`). The first sustained idle ≈ "desktop up, waiting for input."
Crucially, unlike the rate/`comp=` heuristics, the patch sits at a point where it can **read guest
state**, which is what makes the dialog false-positive solvable.

**The false-positive (caught in review).** Any modal blocker idles too — disk-repair prompt,
"no startup disk", extension-conflict alert, system error. A bare "idle = ready" line would fire
while boot is *blocked on a dialog the harness can't answer*. This is **self-inflicted**: our own
force-kill failure path dirties the volume → next boot prompts for repair → false ready → click
into a non-Finder dialog → fail → loop.

**Three-layer defense:**

1. **Prevent (structural).** Pristine-disk-per-run: each run copies a clean master, so a
   force-killed dirty working copy is discarded, never carried forward — the dirty→repair loop is
   broken by construction. **Asset requirement:** the master image must be created via a real
   Special ▸ Shut Down (volume flushed / "properly put away") so it is never flagged dirty.
2. **Disambiguate (the enriched signal).** Emit a one-shot line carrying guest state:
   ```
   [BOOT] idle — frontApp='Finder' modal=0 at 12.3s
   ```
   - `frontApp` = `CurApName` (classic low-mem global `0x910`, `Str31`). At the real desktop this
     is `Finder`; during a pre-Finder blocker it is empty/other.
   - `modal` = whether the front window is a dialog (`WindowRecord.windowKind == dialogKind (2)`,
     via `FrontWindow`/the `WindowList` head `0x9D6`). Catches "Finder up but showing an alert."
   - **Exact globals to be confirmed in implementation** (`CurApName 0x910` is well-known; the
     window-kind read needs a quick verify against a live boot).
   The harness passes only on `frontApp=='Finder' && modal==0`. Any other idle → it reports
   **`boot blocked on dialog`** as an explicit FAILURE (a genuinely useful regression — a change
   that makes boot prompt is exactly what should fail CI), not a hang.
3. **Backstop (watchdog).** Hard timeout: no valid ready-line within N seconds → FAIL and save the
   last VNC frame as an artifact.

**Constraints / notes:**
- Requires `idlewait true` in the isolated test prefs (else the `SynchIdleTime` patch isn't
  installed and `OP_IDLE_TIME` never fires). The harness config sets it.
- The log line lives in the hot idle path → guard with a `static bool` one-shot so the steady-state
  cost is a single bool test.
- **Coordinate with ROADMAP A4** (heartbeat-warning recalibration): it reads the *same* idle
  condition from the other side; the A4 idle-`WARN` must not be treated by the harness as failure.
- This is the v1 boot gate. The general "semantic telemetry for every guest event" (app launch via
  `CheckLoad`, app quit, dialog shown via new trap-table patches) is a **P3 stretch** — larger and
  uneven in difficulty; out of v1 scope.

## 12. Implementation outcome (2026-06-04) — verified live on Mac OS 8.6

P1 is **working end-to-end**: `make e2e` boots an isolated copy, waits for the boot-ready
signal, drives Special ▸ Shut Down, and asserts a clean exit. Verified repeatedly:
`PASS: clean lifecycle: booted to Finder, clean shutdown, exit 0`.

**Boot-ready signal — works as designed (a proper OS-call hook).** `[BOOT] idle frontApp='Finder'
modal=0 ticks=N` fires ~7s in. One ROM-specific fix: this OldWorld ROM patches the **`0x70fe`
variant** of `SynchIdleTime`, so the hook must be in **both** `OP_IDLE_TIME` *and* `OP_IDLE_TIME_2`
(a diagnostic confirmed `installed IDLE_TIME_2`). `CurApName`(0x910) and the modal/window-kind
read(0x9d6) are correct (`frontApp='Finder' modal=0` at the desktop). Blank-disk boot emits **no**
signal → harness times out → FAIL (failure-detection validated; the "?" screen was confirmed).

**Shutdown trigger — Option A (host→guest hook) explored and REVERTED; menu-drive is the working
path.** Two hook variants were tried and neither works cleanly on this ROM/OS:
1. **`Execute68kTrap(0xA895, d0=1)` (ShutDwnPower) from the idle hook** — *flushed volumes*
   (stdout showed the `'flus'` driver calls, so the real shutdown sequence ran) but then
   **SIGSEGV'd at the power-off step** (`pc=0x55590000`). Cause: calling the Shutdown Manager
   *re-entrantly* from inside `SynchIdleTime` corrupts the guest stack at power-off.
2. **ADB power-key injection (`ADBKeyDown(0x7f)`) from the idle hook** — the exact call the SDL
   window-close handler uses, but it had **no effect** on Mac OS 8.6 (no dialog, no shutdown).
Both were reverted to keep the binary clean. The **working** shutdown is driving the Finder's real
**Special ▸ Shut Down** over VNC at the pinned 640×480 (calibrated `SPECIAL_MENU_XY=(175,8)`,
`SHUTDOWN_ITEM_XY=(195,118)`), in a **single VNC session** (open the sticky menu, then select —
disconnecting between closes the menu). This calls the same Shutdown Manager path, but from the
Finder's clean top-level context, so it does the real flush+unmount and reaches
`OP_POWEROFF` → "Shutdown complete." This is "drive the OS's own Shut Down command," not blind
pixel-mashing — deterministic at the pinned resolution.

**Harness finding: `OP_POWEROFF` prints "Shutdown complete." to STDOUT**, while the `[BOOT]`
signals are on stderr. `runner.py` therefore merges stderr into stdout (`stderr=STDOUT`) so
`observe.saw_clean_shutdown` sees both the marker and the atexit `PPC-JIT-A64: session` block.

**If resuming the shutdown hook (future):** the menu path proves the Shutdown Manager works from a
clean top-level context. A working host→guest hook would need to invoke it from a *non-reentrant*
point (not mid-`SynchIdleTime`) — e.g. deferring the `Execute68kTrap` to a top-level dispatch
boundary, or finding why the power-key event isn't consumed. The SIGUSR1 scaffolding (reverted)
is in git history at the pre-revert state if useful.

## 13. Read-only ISO boot — the default test medium (2026-06-04)

The harness now boots a **read-only CD-ROM ISO** by default instead of a writable disk image.
Validated live: the `Mac OS 8.6 Internal Edition.iso` boots to a Finder desktop in ~4.8s
(`[BOOT] idle frontApp='Finder'`), and the full lifecycle passes
(`PASS: clean lifecycle: booted to Finder, clean shutdown, exit 0`).

**Why it's better** (and supersedes the §6 pristine-disk machinery for the default path):
- A read-only volume **can never get dirty** → no disk-repair prompts → the entire dirty-disk →
  modal-dialog false-positive class (§11's three-layer defense) simply cannot occur.
- **No pristine-copy-per-run** (the §6 4 GB copy each run) → faster (~1 min vs ~2) and simpler.
- **Static + reproducible** — the same bytes boot every time, not "somebody's disk image that might
  have been left dirty." Ideal for a regression gate, and much better for CI (smaller to fetch, no
  copy step).

**Mechanism:** CD boot via prefs — `cdrom <iso>`, `nocdrom false`, `bootdriver -62`
(= `CDROMRefNum`, src/include/cdrom.h), no `disk` line. Template: `config/test.prefs.iso.template`.
Asset resolved from `SS_E2E_ISO`. The disk-boot path (`config/test.prefs.template` +
pristine-copy) is retained for scenarios that need a *writable* volume (e.g. P2 app-launch with
saved state); `run_smoke.py` boots the ISO by default.

**Future (your idea):** convert a hard-drive install into a **slim custom bootable ISO** carrying a
minimal System + the benchmark/test tools (Speedometer, MacBench, our harness helpers). That is the
ideal canonical medium — read-only, small, purpose-built. Cost: building a *bootable* classic-Mac
HFS CD image (blessed System Folder + HFS mastering + a CD driver) is a real task, tracked under
ROADMAP A5 as a follow-up.

## 14. Host→guest shutdown hook — SOLVED (2026-06-04)

The shutdown trigger is now a clean **host→guest hook**, not VNC menu-clicking. `SIGUSR1` →
the emulator's idle hook runs a small state machine that injects **ADB Power key (with dwell) →
wait for the Shut Down dialog → Return** (confirms the default "Shut Down" button). The guest then
runs its real shutdown (procs + flush/unmount) from its own event loop → patched `PowerOff()` →
`OP_POWEROFF` → "Shutdown complete." → clean exit. **Verified live on both Mac OS 8.6 and 9.0.4
ISOs** (`PASS: clean lifecycle`).

This supersedes §12's "reverted" status. What unlocked it (two corrections to §12's findings):
1. **Power-key dwell** — injecting key down+up back-to-back was drained in one `ADBInterrupt` pass
   = an instantaneous press the OS ignores. Holding the key across idle-hook cycles fixes that.
2. **The dialog needs confirming** — the power key raises the "Shut Down / Restart / Sleep" dialog;
   it does *not* shut down on its own. Pressing **Return** (Mac key 0x24) activates the default
   button. (The earlier "no effect on 8.6" was actually "dialog shown, waiting for confirmation" —
   a vncdotool screenshot-connect was dismissing the dialog before it could be seen.)

Key property: the idle hook (`OP_IDLE_TIME`/`_2`) **fires even while the modal dialog is up**, so
the whole sequence runs from inside the emulator — no VNC for shutdown. The `Execute68kTrap`
re-entrancy crash (§12) is avoided entirely by posting events rather than re-entering. Code:
`e2e_check_host_shutdown` in `emul_op.cpp`; harness side `runner.request_shutdown()` sends SIGUSR1.
The VNC menu-drive (and its 640×480 coordinate calibration) is retired from the default path; VNC
is now used only for optional, best-effort boot screenshots.

## 15. Benchmark automation (P2) — Speedometer, fully driven (2026-06-05)

`make e2e-bench` (`run_benchmark.py` → `scenario.run_benchmark`) boots the small Mac OS 9 +
Speedometer disk and runs the full Speedometer 4.02 suite end-to-end, unattended. Verified PASS:
results captured (e.g. **PR 29.375, CPU 66.976**), clean shutdown.

**Flow:** boot → wait for the **`[APP] frontApp='Speedometer 4.02'`** signal → drive over VNC:
Enter (dismiss splash) → Esc (dismiss registration) → Cmd+A (`super-a`; run all tests) → Enter
(the "choose drive to test" dialog → OK = the Desktop disk) → ~90 s suite → capture
`benchmark-result.png` → Enter (dismiss "The tests are done!") → SIGUSR1 shutdown hook → clean exit.

**Two robustness lessons (both now fixed):**
1. **Boot+launch time is highly variable** (observed the desktop taking ~25 s to draw on a cold
   run). Fixed `sleep`s raced Speedometer's launch and mis-fired keys onto the registration prompt.
   Fix: the emulator emits **`[APP] frontApp='X'` on every `CurApName` change** (emul_op.cpp
   `e2e_emit_idle_signals`), so the harness waits *deterministically* for Speedometer to be up +
   idle. `CurApName` carries the version, so the harness matches the substring (`Speedometer`).
   (Note: `CurApName` oscillates among background extensions under cooperative multitasking, so the
   `[APP]` stream is noisy — fine for detection, just log spam.)
2. **vncdotool didn't release** — `api.connect()` starts a Twisted reactor in a non-daemon thread;
   without `api.shutdown()` after `disconnect()` the process hangs (every capture/drive command
   took ~2 min via timeout). `Vnc.close()` now calls `api.shutdown()`.

**Honest limits / next refinements:**
- The PASS criterion is "drove the sequence + captured + clean shutdown" — it does **not yet parse
  the score**. The PR/CPU numbers are in `benchmark-result.png` (human/OCR readable). True
  perf-regression gating needs OCR of the result (or reading Speedometer's "Machine Records" file).
- The drive still uses a few fixed `sleep`s for the within-Speedometer dialog transitions (splash→
  registration→disk-dialog), which are fast and consistent once Speedometer is up; only the
  variable boot/launch is signal-gated.
- The benchmark disk is a writable copy-per-run (instant clonefile); the `extfs` host-FS mount is
  disabled in the prefs so only the one Mac disk is present (unambiguous disk-select, no host
  exposure).

## 16. Remaining work + the score-parsing investigation (2026-06-05)

The harness is functionally complete (smoke + benchmark run end-to-end). This section records what
was finished in the "do all the remaining items" pass and the honest state of the hard one.

**Done in this pass:**
- **Pre-flight stray-kill** — `runner.kill_strays()` (pkill) runs at the start of `run_smoke` /
  `run_benchmark`, so a re-run never collides with a stray instance.
- **Golden-image diff machinery** (`sse2e/imagecmp.py`, smoke P2) — masked perceptual hash
  (`phash` + a Hamming threshold) with the menu-bar clock region blacked out, so visual/video
  regressions are catchable without the flakiness of exact pixel diffs. Tested (synthetic images).
  *To enable:* capture a known-good "golden" desktop once (a boot) and compare the smoke screenshot
  against it; the function is ready, the golden image is a one-time boot-gated asset.
- **CI workflow** — `.github/workflows/e2e.yml`: an offline `e2e-units` job (runs anywhere) + a
  manual self-hosted-macOS `e2e-run` job, with the asset-fetch + logged-in-session requirements
  documented inline.

**Score parsing — investigated, NOT cleanly finishable without OCR (which is excluded).** Goal: an
automated perf-regression *number* from the Speedometer run. Findings:
- Speedometer writes results to a **"Machine Records" file** (type `MchT`, creator `sPd3`) on the
  disk. The data is in the **resource fork** (~1947 bytes; data fork empty). Extractable on the host
  with `hfsutils` (`hmount` + `hcopy -m` → MacBinary), which works.
- But the **headline PR/CPU values are NOT stored as plain big-endian floats** at stable offsets.
  A scan found a benchmark float cluster around resource-fork offset ~1654–1850, but the displayed
  PR (e.g. 29.375) and CPU (66.976) don't match any single/double there — likely SANE 80-bit
  extended or a scaled encoding. Parsing it reliably is real reverse-engineering of Speedometer's
  proprietary resource format, and the PR also varies run-to-run (timing-based), so a single value
  is a noisy gate.
- **Recommended paths (any one):** (a) RE the `MchT`/`sPd3` resource format from a Speedometer build
  with known inputs; (b) check whether Speedometer can **export results as text** (a File/Analysis
  menu item) and drive that → read the text file off the disk; (c) a **duration proxy** — emit an
  `[APP]`-style signal when the "tests are done!" modal appears (extend the idle hook to fire on
  *modal* change, not just app change) and measure wall-clock from Cmd+A to done; coarse but
  non-OCR. For now the PR/CPU live in `benchmark-result.png` (human-readable).

**Deferred (with reason):**
- **P3 scenario DSL** — generalizing `run_lifecycle`/`run_benchmark` into declarative steps. The two
  scenarios work; only worth it when adding more.
- **Spec relocation** (`docs/superpowers/specs/` → `docs/planning/specs/`) — cross-file ref churn
  while other agents edit docs; deferred to avoid conflicts.

## 17. Update (2026-06-05 pt 2) — benchmark-finished hook landed; boot fix; SDL2/SDL3 finding; PARKED

This pass closed two of the §16 deferrals and fixed a boot-breaking regression, then parked the
feature (work moved to Silicon Sheep). State at park:

**Done (commits `2dcddbb1`, `5d87d713`):**
- **Benchmark-finished hook + `[APP]` debounce (was §16 path (c) + the deferred debounce).** The
  idle hook's `[APP]` signal now also fires on a front-window **modal change**, so Speedometer's
  "tests are done!" dialog (modal 0→1) is a deterministic finish signal. `run_benchmark` waits for
  that dialog instead of a fixed 105 s sleep and reports the measured suite duration (a coarse,
  non-OCR perf proxy — e.g. **39 s** on the verifying run). App-change emits are debounced to ~0.5 s.
  (`emul_op.cpp`, `observe.is_app_dialog()`, `scenario.run_benchmark` + unit tests.) Verified
  end-to-end: captured the real "tests are done!" dialog (PR 28.644) — see `benchmark-result.png`.
- **Boot-breaking regression fixed.** The live-JIT-stats window-title feature called
  `SDL_SetWindowTitle` from `do_video_refresh()` (the **Redraw Thread**); on macOS that Cocoa call
  is main-thread-only — it aborts (SDL2) or stalls the redraw thread so the 60 Hz VBL stops and the
  guest hangs in early disk/SCSI boot. Removed the periodic update from both backends; getter +
  `status_suffix` retained for a main-thread reimpl. (See LEARNINGS 2026-06-05.)

**Finding — the harness has been validating SDL2, not SDL3.** `otool -L` shows the `make build-ss`
binary links `libSDL2-2.0.0.dylib`, although `configure.ac` (and the docs) default to **SDL3**. Root
cause: the *generated* `configure` is stale (generated Jun 4 09:28; `configure.ac` flipped the
default to SDL3 at Jun 4 11:34) and still defaults to SDL2, so it never runs the sdl3 check.
sdl3 pkg-config **is** available (3.4.10). One-command fix when desired:
`cd SheepShaver/src/Unix && NO_CONFIGURE=1 ./autogen.sh && ./configure …` (no `--with-sdl2`), then
`make build-ss` — re-verify `make e2e` boots on SDL3 (the title fix covers both backends).

**Parked next step (the one real open item): score parsing via Speedometer text export (§16 path
(b)).** Boot now works, so this is unblocked: drive Speedometer's File/Analysis menus over VNC to
find a "Save as text"/export action, write it to the disk, read + parse the file on the host for an
exact PR/CPU number. Until then the duration proxy + `benchmark-result.png` cover the perf signal.

## 18. SDL3 shutdown crash — RESOLVED (2026-06-05, commit 3daa9c98)

Switching the build to the intended **SDL3** backend (see §17; `NO_CONFIGURE=1 ./autogen.sh` fixed
the stale `configure` that had silently built SDL2) surfaced a real bug: SDL3 booted to Finder fine
but the E2E shutdown failed (`code=-9`). SDL2 ran the identical sequence to a clean `exit 0`.

**Root cause — a double-`SDL_DestroyMutex` (idempotency bug), pinned by a user-captured crash
backtrace:**
```
BUG IN CLIENT OF LIBPLATFORM: os_unfair_lock is corrupt, or owner thread exited without unlocking
  pthread_mutex_destroy → SDL_DestroyMutex → VideoExit() (video_sdl3.cpp:1682)
  → ExitAll() (main.cpp:312) → Quit() (main_unix.cpp:1344)
```
`Quit()` calls `VideoExit()` **twice** — directly (`main_unix.cpp:1301`) and again via `ExitAll()`
(`main_unix.cpp:1344` → `main.cpp:312`). `VideoExit()` destroyed `frame_buffer_lock`/
`sdl_palette_lock`/`sdl_events_lock` but never NULLed them, so the second pass double-destroyed
already-freed mutexes. SDL2 tolerated this; SDL3's mutexes are os_unfair_lock-backed and abort.

**Fix:** NULL each mutex pointer after `SDL_DestroyMutex` in `VideoExit()` (`video_sdl3.cpp`), so the
second pass is a no-op — completing the idempotency the existing `if (lock)` guards already intended.

**Process-history note (so the next reader isn't misled):** while the host was VBL-degraded (dozens
of launches in a ~13 h session → boots hanging in SCSI init), this was mis-theorized as a Metal
window/renderer teardown deadlock in `SDL_Quit`. That was WRONG — the host degradation masked the
real crash. After a **host restart** restored reliable boots, the user's crash report localized it
immediately. Lessons: (a) `printf("Shutdown complete.")` →stdout is block-buffered and lost on a
`kill -9`/crash, so its absence proves nothing; (b) don't trust shutdown diagnostics while boots are
degraded — restart first. Keyboard delivery was correctly ruled out throughout (ADB injection never
traverses SDL).

**Verified:** `make e2e` (smoke) PASS ×2 on SDL3 (boot → Finder → clean shutdown, exit 0);
`make test-jit` 264/264 score=100.

## 19. SDL3 has NO VNC server — `make e2e-bench` + screenshots require SDL2 (2026-06-05) — RESOLVED, see §20

Running the benchmark on the (now genuinely) SDL3 build fails: it boots to Finder and Speedometer
launches, but the harness's VNC client gets `ConnectionRefused` on :5950 — **the VNC server never
starts on SDL3.** Root cause: `vnc_server.cpp:14` gates the entire libvncserver implementation to
`#if SDL_VERSION_ATLEAST(2,0,0) && !SDL_VERSION_ATLEAST(3,0,0)`; the SDL3 `#else` branch is **empty
stubs** (`VNCServerInitFromPrefs`/`Shutdown`/`Update` are no-ops, `vnc_server.cpp:630-650`).
`video_sdl3.cpp` correspondingly has **zero** VNC calls, vs `video_sdl2.cpp`'s 4 (`#include
"vnc_server.h"`; `VNCServerInitFromPrefs()` @1745; `VNCServerUpdate(host_surface, rect)` @1192;
`VNCServerShutdown()` @1966; input is via libvncserver's own callback thread). The benchmark never
caught this before because every prior build silently linked SDL2 (§17).

**Impact:** on SDL3, only the **smoke** (boot + SIGUSR1 shutdown — no VNC) works. The **benchmark**
and the golden-image **screenshot** diff need VNC → **SDL2 only** for now. This is a real cost of the
SDL3 default that the §17 backend review didn't know about.

**Options (user decision):**
1. **Keep SDL2 as the harness default** (`./configure … --with-sdl2`): full harness — smoke +
   benchmark + screenshots — all work today. Treat SDL3 as opt-in until VNC is ported.
2. **Port the VNC server to SDL3** (tracked work, ~half a day, needs a *stable* host to test): remove
   the SDL2-only guard in `vnc_server.cpp`, adapt its ~480-line libvncserver integration to SDL3's
   changed `SDL_Surface` API (format enum, lock/pitch), and wire the 4 call sites into
   `video_sdl3.cpp` mirroring SDL2. Then SDL3 reaches feature parity.
3. **SDL3 default, benchmark/screenshots documented as SDL2-only** until (2) lands.

## 20. VNC ported to SDL3 + signal-gated/instrumented benchmark + robustness (2026-06-05, RESOLVED)

Chose §19 Option 2 — **the VNC server is now ported to SDL3** (commit `01bc52fe`), so SDL3 has full
harness parity. `vnc_server.cpp`'s guard widened from `#if SDL2 && !SDL3` to `#if
SDL_VERSION_ATLEAST(2,0,0)` (the `#else` stub now only covers SDL<2); SDL3 adaptations: `#undef`/
`#define` shims for the renamed keymod + condition-variable symbols (SDL3 ships them as migration
error-tokens), `SDL_EVENT_KEY_*`/float mouse-coords event injection, and `SDL_GetPixelFormatDetails`
for the pixel format. Four call sites wired into `video_sdl3.cpp` mirroring `video_sdl2.cpp` (include;
`VNCServerInitFromPrefs()` in video init; `VNCServerUpdate(host_surface, sdl_update_video_rect)` in
`present_sdl_video`; `VNCServerShutdown()` in `VideoExit`; plus a `video_mode_changing` definition for
this backend). Verified: `make e2e-bench` PASS on SDL3 — `VNC server enabled on port 5950`, VNC keys
drove Speedometer, pixel-correct result capture, clean shutdown.

**Signal-gating + instrumentation.** The benchmark drive steps no longer use fixed sleeps. The idle
hook's `[APP]`/`[BOOT]` signals now carry the front-window pointer (`win=`) and **title** (`title=`),
with garbage background-window frames suppressed (the cooperative-MT churn). Each drive step gates on
the actual window transition (`_await_since`), prints `[gate] <step>: Xs`, and self-corrects
(`_drive_until` resends a dropped key on one continuous watch — no cursor race). Done-detection gates
on the `All Done!` alert title. The instrumentation revealed Speedometer's splash takes a variable
~10-24 s to become input-ready (the real bottleneck; transitions are then 0.2 s).

**Correctness + robustness (expert-review pass).**
- Guest reads in `emul_op.cpp` are sanitized (a literal `'` → backtick, so it can't break the
  `frontApp='…'`/`title='…'` regexes) and **bounds-checked** (a wild `titleHandle` no longer
  SIGSEGVs the host).
- New **`[READY]`** signal: emitted once the Finder is frontmost + non-modal for a ~2 s dwell — a more
  robust "desktop actually usable" marker than the first `[BOOT] idle`; carries `MBarHeight`.
- **`runner.preflight()`**: kill *and reap* stray SheepShaver + verify the boot image isn't held
  open, refusing to launch into a "?" no-boot-disk hang. (A stray holding the disk is one "?" cause;
  host graphics/VBL degradation after many launches is another that no pre-flight can detect — a host
  restart clears it.)
- Harness no longer hangs after PASS (entry points `os._exit` past vncdotool's non-daemon reactor).

**Still open:** the C++ shutdown-confirm step (`e2e_check_host_shutdown`) is the one blind, un-gated
key-press — should gate on the dialog being modal + resend, mirroring `_drive_until`. Unit tests for
the gate/retry logic (feed canned log text to a `FakeRunner`). `Runner` should serve incremental log
lines (the gates re-read+re-parse the whole buffer every 0.2 s). Score parsing (Speedometer text
export) remains the headline open feature (§16).
