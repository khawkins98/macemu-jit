# Design Brief: SheepShaver End-to-End VNC Test Harness

**Date:** 2026-06-04
**Status:** Approved design — pending implementation plan (writing-plans)
**Roadmap home:** Track **A1 — Testing infrastructure** (`docs/planning/ROADMAP.md`). This is the
GUI/boot-level complement to the existing instruction-level gates (`make test-jit`, `SS_JIT_VERIFY`).

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

## 10. Open questions (for the plan)

1. **Steady-state log signal:** which exact line means "booted to Finder, idle"? Needs a short
   investigation against a real boot's diag log (the heartbeat block-rate flattening is a
   candidate).
2. **Menu coordinates vs. keyboard:** is Special ▸ Shut Down reachable by a keyboard equivalent,
   or must we click fixed menu coordinates (resolution-dependent)? Affects robustness.
3. **Pristine image source:** build a minimal dedicated test disk, or snapshot the existing
   `macos86_fresh.dsk`? Smaller is faster to copy-per-run.
4. **CI host reality:** does the eventual CI runner have a display/SDL path, or do we run the
   emulator fully headless (offscreen SDL + VNC only)? Determines the spawn flags.
