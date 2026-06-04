# Design Brief: SheepShaver End-to-End VNC Test Harness

> **Status:** 🟡 Active · **Created:** 2026-06-04 · **Updated:** 2026-06-04
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
