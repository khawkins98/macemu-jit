> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# SheepShaver E2E VNC Harness — P1 (Lifecycle Smoke) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Status:** 🟡 Active · **Created:** 2026-06-04 · **Updated:** 2026-06-04
> **Why this doc exists:** Step-by-step build of P1 of the E2E harness (ROADMAP A5). Spec:
> `docs/superpowers/specs/2026-06-04-e2e-vnc-harness-design.md`.

**Goal:** Build the P1 "lifecycle smoke" harness — spawn SheepShaver in an isolated config, wait
for a deterministic boot-ready signal, drive Special ▸ Shut Down over VNC, and assert a clean exit.

**Architecture:** One small emulator-side change (a one-shot `[BOOT] idle …` log line enriched
with guest frontmost-app/modal state, emitted from the already-trapped `OP_IDLE_TIME` idle path),
plus an external Python harness (`SheepShaver/e2e/`) with four focused modules — `observe` (parse
host-log signals), `runner` (spawn/capture/teardown), `vnc` (drive input over VNC), `scenario`
(orchestrate). The deterministic spine is host-log signals; the VNC framebuffer is artifacts only
in P1.

**Tech Stack:** C++ (emulator), Python 3.14 + `pytest` + `vncdotool` (harness; `Pillow` already
present, `imagehash` deferred to P2).

---

## Scope

**In P1:** boot → wait `[BOOT] idle frontApp='Finder' modal=0` → click Special ▸ Shut Down → assert
`"Shutdown complete."` + `PPC-JIT-A64: session` atexit block + exit 0. Isolated prefs +
pristine-disk-per-run. **Out of P1 (future plans):** golden-image diff, app-launch, scenario DSL
(P2/P3). The general guest-event telemetry channel is P3.

**Boot-gated steps** (need a real boot, which the user runs — agents must ask first per the
relaxed CLAUDE.md rule) are marked **[BOOT-GATED]**. Everything else is verifiable offline
(build + pytest against fixtures).

## File structure

```
SheepShaver/src/emul_op.cpp                 (MODIFY) — one-shot boot-ready log line
SheepShaver/e2e/
  ├── README.md                             how to run + the macOS logged-in-session caveat
  ├── requirements.txt                      vncdotool, pytest  (+ pillow already present)
  ├── pyproject.toml                        pytest config (testpaths)
  ├── config/test.prefs.template            isolated prefs (idlewait true, pinned 640x480)
  ├── sse2e/
  │   ├── __init__.py
  │   ├── observe.py                        parse [BOOT] idle / Shutdown complete / atexit / blocked
  │   ├── runner.py                         spawn isolated emulator, capture stderr, teardown
  │   ├── vnc.py                            vncdotool wrapper: connect/click/key/capture
  │   ├── disk.py                           pristine-copy-per-run + prefs render
  │   └── scenario.py                       boot→shutdown→assert orchestration
  └── tests/
      ├── fixtures/clean_boot_shutdown.log  real captured log (clean run)
      ├── fixtures/boot_ready.log           a [BOOT] idle line sample
      ├── test_observe.py
      ├── test_runner.py
      └── test_disk.py
SheepShaver/Makefile                        (MODIFY) — add `e2e` target
```

---

## Task 1: Emulator boot-ready signal (`OP_IDLE_TIME` enrichment)

**Files:**
- Modify: `SheepShaver/src/emul_op.cpp` (the `case OP_IDLE_TIME:` handler, ~line 487)

Emit a one-shot, guest-state-enriched line the first time the guest reaches Process-Manager idle.
`CurApName` is the classic low-mem global at `0x910` (a Pascal `Str31`: length byte + up to 31
chars). The front-window modal check reads the `WindowList` head (`0x9D6`) and the window's
`windowKind` field (offset `0x6C` in a `WindowRecord`; `dialogKind == 2`). Timestamp uses guest
`Ticks` (`0x16A`, 60/s).

- [ ] **Step 1: Add a self-contained boot-ready helper above the EmulOp dispatcher**

In `emul_op.cpp`, just above the function containing the `switch` (search for `case OP_IDLE_TIME`),
add:

```cpp
// E2E harness boot-ready signal (ROADMAP A5). Emit ONE line the first time the guest reaches
// Process-Manager idle (OP_IDLE_TIME = SynchIdleTime patch), enriched with frontmost-app + modal
// state so an automated harness can tell "idle at the Finder desktop" from "idle blocked on a
// modal dialog" (disk-repair prompt etc.). See docs/superpowers/specs/2026-06-04-e2e-vnc-harness-design.md §11.
static void e2e_emit_boot_ready_once(void)
{
	static bool emitted = false;
	if (emitted) return;
	emitted = true;

	// CurApName: low-mem 0x910, Pascal Str31 (length byte + chars).
	char app[32];
	uint8 *namep = Mac2HostAddr(0x910);
	int len = namep[0];
	if (len > 31) len = 31;
	for (int i = 0; i < len; i++) app[i] = (char)namep[1 + i];
	app[len] = '\0';

	// Modal check: is the front window a dialog? WindowList head = 0x9D6; windowKind at +0x6C.
	int modal = 0;
	uint32 front = ReadMacInt32(0x9d6);
	if (front) {
		int16 kind = (int16)ReadMacInt16(front + 0x6c);
		if (kind == 2) modal = 1;   // dialogKind
	}

	double secs = ReadMacInt32(0x16a) / 60.0;   // Ticks since boot
	fprintf(stderr, "[BOOT] idle frontApp='%s' modal=%d ticks=%u (%.1fs)\n",
	        app, modal, ReadMacInt32(0x16a), secs);
	fflush(stderr);
}
```

- [ ] **Step 2: Call it from the idle handler**

Change the `case OP_IDLE_TIME:` handler (≈ line 487) from:

```cpp
		case OP_IDLE_TIME:
			// Sleep if no events pending
			if (ReadMacInt32(0x14c) == 0)
				idle_wait();
			r->a[0] = ReadMacInt32(0x2b6);
			break;
```

to (add the one call as the first statement):

```cpp
		case OP_IDLE_TIME:
			e2e_emit_boot_ready_once();
			// Sleep if no events pending
			if (ReadMacInt32(0x14c) == 0)
				idle_wait();
			r->a[0] = ReadMacInt32(0x2b6);
			break;
```

- [ ] **Step 3: Build the emulator**

Run: `cd SheepShaver && make build-ss`
Expected: builds to `src/Unix/SheepShaver`, no errors. (If `ReadMacInt16` is undeclared, confirm
its declaration in `cpu_emulation.h`/`main.h` — it is used elsewhere in this file; include is
already present.)

- [ ] **Step 4: Verify the codegen gate still passes (no behavior change to the JIT)**

Run: `cd SheepShaver && make test-jit`
Expected: `score=100` (this change does not touch codegen; this is a regression guard).

- [ ] **Step 5: Commit**

```bash
git add SheepShaver/src/emul_op.cpp
git commit -m "feat(ss): emit one-shot [BOOT] idle signal from OP_IDLE_TIME (A5 harness)

Enriched with CurApName (0x910) + front-window modal check (0x9d6/windowKind)
so the E2E harness can distinguish 'idle at desktop' from 'idle on a modal
dialog'. One-shot static guard keeps the hot idle path free. test-jit=100."
```

- [ ] **Step 6: [BOOT-GATED] Confirm the line on a real boot**

Ask the user to boot once (or run yourself if authorized) and capture stderr:
`./src/Unix/SheepShaver --config <test.prefs> 2>/tmp/e2e-boot.log`
Expected: a line like `[BOOT] idle frontApp='Finder' modal=0 ticks=… (…s)` appears once, ~10–20s
in. Record the actual `frontApp` string (confirm it is exactly `Finder`) and whether `modal` reads
0 at the desktop — these calibrate `observe.py`. If `frontApp` is empty or wrong, re-verify the
`0x910`/`0x9d6`/`0x6c` offsets against a live memory dump before proceeding.

---

## Task 2: Harness scaffold + dependencies

**Files:**
- Create: `SheepShaver/e2e/requirements.txt`, `SheepShaver/e2e/pyproject.toml`,
  `SheepShaver/e2e/sse2e/__init__.py`, `SheepShaver/e2e/.gitignore`

- [ ] **Step 1: Create the package layout and dependency files**

`SheepShaver/e2e/requirements.txt`:
```
vncdotool==1.3.0
pytest>=8.0
Pillow>=10.0
```

`SheepShaver/e2e/pyproject.toml`:
```toml
[tool.pytest.ini_options]
testpaths = ["tests"]
```

`SheepShaver/e2e/sse2e/__init__.py`:
```python
"""SheepShaver end-to-end VNC test harness (ROADMAP A5, P1)."""
```

`SheepShaver/e2e/.gitignore`:
```
.venv/
__pycache__/
*.pyc
artifacts/
```

- [ ] **Step 2: Create a venv and install deps**

Run:
```bash
cd SheepShaver/e2e
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```
Expected: installs `vncdotool`, `pytest`, `Pillow` with no error. (Python 3.14: if a wheel is
missing, `pip` builds from sdist — allow it.)

- [ ] **Step 3: Verify pytest runs (collects zero tests)**

Run: `cd SheepShaver/e2e && .venv/bin/pytest -q`
Expected: `no tests ran` (exit 5) — confirms the harness is set up.

- [ ] **Step 4: Commit**

```bash
git add SheepShaver/e2e/requirements.txt SheepShaver/e2e/pyproject.toml \
        SheepShaver/e2e/sse2e/__init__.py SheepShaver/e2e/.gitignore
git commit -m "feat(e2e): scaffold harness package + deps (vncdotool, pytest)"
```

---

## Task 3: `observe` module — parse host-log signals (TDD)

**Files:**
- Create: `SheepShaver/e2e/sse2e/observe.py`
- Test: `SheepShaver/e2e/tests/test_observe.py`,
  `SheepShaver/e2e/tests/fixtures/clean_boot_shutdown.log`,
  `SheepShaver/e2e/tests/fixtures/boot_ready.log`

The pure, fully-offline-testable heart. Parses three things from emulator stderr text: the
boot-ready signal (and its frontApp/modal), the clean-exit signal, and a "boot blocked on dialog"
condition.

- [ ] **Step 1: Create fixtures**

`SheepShaver/e2e/tests/fixtures/boot_ready.log`:
```
[JIT 9.80s]   blocks=600M pc=5031040c
[BOOT] idle frontApp='Finder' modal=0 ticks=812 (13.5s)
[HB 14s] blocks=620M (1.2M/s) comp=210000
```

`SheepShaver/e2e/tests/fixtures/clean_boot_shutdown.log`:
```
[BOOT] idle frontApp='Finder' modal=0 ticks=812 (13.5s)

    Shutdown complete.

PPC-JIT-A64: session 43s (0m42.6s)
PPC-JIT-A64: blocks=254318 complete=254316 (100.0%)
PPC-JIT-A64: hit=1768692 miss=21347 (98.8% coverage)
```

- [ ] **Step 2: Write the failing test**

`SheepShaver/e2e/tests/test_observe.py`:
```python
from sse2e import observe


def test_parse_boot_ready_finder():
    line = "[BOOT] idle frontApp='Finder' modal=0 ticks=812 (13.5s)"
    ev = observe.parse_boot_ready(line)
    assert ev == observe.BootReady(front_app="Finder", modal=False, secs=13.5)


def test_parse_boot_ready_ignores_non_matching():
    assert observe.parse_boot_ready("[HB 14s] blocks=620M") is None


def test_boot_ready_blocked_on_dialog():
    line = "[BOOT] idle frontApp='' modal=1 ticks=300 (5.0s)"
    ev = observe.parse_boot_ready(line)
    assert ev is not None
    assert observe.is_desktop_ready(ev) is False


def test_boot_ready_desktop_ok():
    ev = observe.BootReady(front_app="Finder", modal=False, secs=13.5)
    assert observe.is_desktop_ready(ev) is True


def test_clean_exit_detected_in_log():
    text = open("tests/fixtures/clean_boot_shutdown.log").read()
    assert observe.saw_clean_shutdown(text) is True


def test_clean_exit_absent_when_only_boot():
    text = open("tests/fixtures/boot_ready.log").read()
    assert observe.saw_clean_shutdown(text) is False
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cd SheepShaver/e2e && .venv/bin/pytest tests/test_observe.py -q`
Expected: FAIL (`ModuleNotFoundError` or `AttributeError: parse_boot_ready`).

- [ ] **Step 4: Implement `observe.py`**

`SheepShaver/e2e/sse2e/observe.py`:
```python
"""Parse deterministic lifecycle signals from SheepShaver stderr."""
from __future__ import annotations

import re
from dataclasses import dataclass

_BOOT_RE = re.compile(
    r"\[BOOT\] idle frontApp='(?P<app>[^']*)' modal=(?P<modal>\d+) ticks=\d+ \((?P<secs>[\d.]+)s\)"
)
# The clean-exit signature: "Shutdown complete." plus the atexit session block.
_SHUTDOWN_RE = re.compile(r"Shutdown complete\.")
_ATEXIT_RE = re.compile(r"PPC-JIT-A64: session ")


@dataclass(frozen=True)
class BootReady:
    front_app: str
    modal: bool
    secs: float


def parse_boot_ready(line: str) -> BootReady | None:
    """Return a BootReady if the line is the [BOOT] idle signal, else None."""
    m = _BOOT_RE.search(line)
    if not m:
        return None
    return BootReady(
        front_app=m.group("app"),
        modal=m.group("modal") != "0",
        secs=float(m.group("secs")),
    )


def is_desktop_ready(ev: BootReady) -> bool:
    """True only when idle at the Finder desktop (not blocked on a modal dialog)."""
    return ev.front_app == "Finder" and not ev.modal


def saw_clean_shutdown(text: str) -> bool:
    """True if the log shows a clean guest shutdown (both signatures present)."""
    return bool(_SHUTDOWN_RE.search(text)) and bool(_ATEXIT_RE.search(text))
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd SheepShaver/e2e && .venv/bin/pytest tests/test_observe.py -q`
Expected: PASS (6 passed).

- [ ] **Step 6: Commit**

```bash
git add SheepShaver/e2e/sse2e/observe.py SheepShaver/e2e/tests/test_observe.py \
        SheepShaver/e2e/tests/fixtures/
git commit -m "feat(e2e): observe module — parse boot-ready/clean-exit/blocked signals (TDD)"
```

---

## Task 4: `runner` module — spawn, capture, teardown

**Files:**
- Create: `SheepShaver/e2e/sse2e/runner.py`
- Test: `SheepShaver/e2e/tests/test_runner.py`

Spawns the emulator with `--config <prefs>`, streams stderr into an in-memory buffer the scenario
can poll, and guarantees teardown. A clean shutdown makes the process exit on its own; a hung run
is force-killed and reported as failure.

- [ ] **Step 1: Write the failing test (teardown logic, no emulator needed)**

`SheepShaver/e2e/tests/test_runner.py`:
```python
import sys
import time

from sse2e.runner import Runner


def test_runner_captures_stderr_and_exit_code():
    # Use a tiny python program as a stand-in for the emulator.
    prog = "import sys; sys.stderr.write('hello\\n'); sys.stderr.flush(); sys.exit(0)"
    r = Runner(argv=[sys.executable, "-c", prog])
    r.start()
    code = r.wait(timeout=10)
    assert code == 0
    assert "hello" in r.log_text()


def test_runner_force_kill_on_timeout_is_failure():
    prog = "import time; time.sleep(30)"
    r = Runner(argv=[sys.executable, "-c", prog])
    r.start()
    code = r.wait(timeout=1)        # should time out
    assert code is None            # None == timed out / had to be killed
    r.terminate()
    assert r.was_killed is True
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd SheepShaver/e2e && .venv/bin/pytest tests/test_runner.py -q`
Expected: FAIL (`ModuleNotFoundError: sse2e.runner`).

- [ ] **Step 3: Implement `runner.py`**

`SheepShaver/e2e/sse2e/runner.py`:
```python
"""Spawn the emulator, capture stderr, and guarantee teardown."""
from __future__ import annotations

import subprocess
import threading


class Runner:
    def __init__(self, argv: list[str]):
        self.argv = argv
        self._proc: subprocess.Popen | None = None
        self._buf: list[str] = []
        self._lock = threading.Lock()
        self._reader: threading.Thread | None = None
        self.was_killed = False

    def start(self) -> None:
        self._proc = subprocess.Popen(
            self.argv,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self._reader = threading.Thread(target=self._drain, daemon=True)
        self._reader.start()

    def _drain(self) -> None:
        assert self._proc and self._proc.stderr
        for line in self._proc.stderr:
            with self._lock:
                self._buf.append(line)

    def log_text(self) -> str:
        with self._lock:
            return "".join(self._buf)

    def wait(self, timeout: float) -> int | None:
        """Return exit code, or None if it did not exit within `timeout`."""
        assert self._proc
        try:
            return self._proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def terminate(self) -> None:
        """Force teardown. Sets was_killed if the process had to be killed."""
        if not self._proc:
            return
        if self._proc.poll() is None:
            self.was_killed = True
            self._proc.kill()
            self._proc.wait(timeout=5)
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd SheepShaver/e2e && .venv/bin/pytest tests/test_runner.py -q`
Expected: PASS (2 passed).

- [ ] **Step 5: Commit**

```bash
git add SheepShaver/e2e/sse2e/runner.py SheepShaver/e2e/tests/test_runner.py
git commit -m "feat(e2e): runner module — spawn/capture-stderr/teardown (TDD)"
```

---

## Task 5: `disk` module — pristine copy + prefs render

**Files:**
- Create: `SheepShaver/e2e/sse2e/disk.py`, `SheepShaver/e2e/config/test.prefs.template`
- Test: `SheepShaver/e2e/tests/test_disk.py`

Each run copies the pristine master disk to a fresh temp path and renders a prefs file pointing at
it — never touching the user's config. `idlewait true` is required so the `OP_IDLE_TIME` signal
fires (Task 1).

- [ ] **Step 1: Create the prefs template**

`SheepShaver/e2e/config/test.prefs.template`:
```
rom {rom}
disk {disk}
ramsize 134217728
screen win/640/480
displaycolordepth 8
nosound true
nocdrom true
nogui true
idlewait true
ignoreillegal true
vncserver true
vncport {vncport}
cpuclock 500
```

- [ ] **Step 2: Write the failing test**

`SheepShaver/e2e/tests/test_disk.py`:
```python
from pathlib import Path

from sse2e import disk


def test_render_prefs_substitutes_fields(tmp_path):
    out = tmp_path / "test.prefs"
    disk.render_prefs(
        template=Path("config/test.prefs.template"),
        out_path=out,
        rom="/roms/x.rom",
        disk="/tmp/run.dsk",
        vncport=5950,
    )
    text = out.read_text()
    assert "rom /roms/x.rom" in text
    assert "disk /tmp/run.dsk" in text
    assert "vncport 5950" in text
    assert "idlewait true" in text


def test_pristine_copy_makes_independent_file(tmp_path):
    master = tmp_path / "master.dsk"
    master.write_bytes(b"PRISTINE")
    run_copy = disk.copy_pristine(master, tmp_path / "work")
    assert run_copy.read_bytes() == b"PRISTINE"
    run_copy.write_bytes(b"DIRTIED")
    assert master.read_bytes() == b"PRISTINE"   # master untouched
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cd SheepShaver/e2e && .venv/bin/pytest tests/test_disk.py -q`
Expected: FAIL (`ModuleNotFoundError: sse2e.disk`).

- [ ] **Step 4: Implement `disk.py`**

`SheepShaver/e2e/sse2e/disk.py`:
```python
"""Pristine-disk-per-run + prefs rendering (test isolation)."""
from __future__ import annotations

import shutil
from pathlib import Path


def render_prefs(template: Path, out_path: Path, *, rom: str, disk: str, vncport: int) -> Path:
    text = template.read_text().format(rom=rom, disk=disk, vncport=vncport)
    out_path.write_text(text)
    return out_path


def copy_pristine(master: Path, dest_dir: Path) -> Path:
    """Copy the pristine master disk to a fresh per-run path; return the copy."""
    dest_dir.mkdir(parents=True, exist_ok=True)
    run_copy = dest_dir / master.name
    shutil.copyfile(master, run_copy)
    return run_copy
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd SheepShaver/e2e && .venv/bin/pytest tests/test_disk.py -q`
Expected: PASS (2 passed).

- [ ] **Step 6: Commit**

```bash
git add SheepShaver/e2e/sse2e/disk.py SheepShaver/e2e/config/test.prefs.template \
        SheepShaver/e2e/tests/test_disk.py
git commit -m "feat(e2e): disk module — pristine-copy-per-run + prefs render (TDD)"
```

---

## Task 6: `vnc` module — drive input over VNC

**Files:**
- Create: `SheepShaver/e2e/sse2e/vnc.py`

A thin wrapper over `vncdotool`'s synchronous API so the scenario reads cleanly and the dependency
is isolated to one file. Not unit-tested (it needs a live VNC server); validated in Task 8.

- [ ] **Step 1: Implement `vnc.py`**

`SheepShaver/e2e/sse2e/vnc.py`:
```python
"""Thin wrapper over vncdotool's synchronous API."""
from __future__ import annotations

from vncdotool import api


class Vnc:
    def __init__(self, host: str = "127.0.0.1", port: int = 5950):
        # vncdotool address form is "host::port" (note the double colon).
        self._client = api.connect(f"{host}::{port}")

    def click(self, x: int, y: int) -> None:
        self._client.mouseMove(x, y)
        self._client.mousePress(1)

    def key(self, name: str) -> None:
        self._client.keyPress(name)

    def capture(self, path: str) -> None:
        self._client.captureScreen(path)

    def close(self) -> None:
        self._client.disconnect()
```

- [ ] **Step 2: Verify it imports (offline)**

Run: `cd SheepShaver/e2e && .venv/bin/python -c "from sse2e import vnc; print('ok')"`
Expected: `ok` (import resolves `vncdotool`; no connection attempted at import time).

- [ ] **Step 3: Commit**

```bash
git add SheepShaver/e2e/sse2e/vnc.py
git commit -m "feat(e2e): vnc module — vncdotool wrapper (connect/click/key/capture)"
```

---

## Task 7: `scenario` — boot → shutdown → assert (orchestration)

**Files:**
- Create: `SheepShaver/e2e/sse2e/scenario.py`

Ties the modules together. The Special ▸ Shut Down menu coordinates are **calibrated in Task 8**
(they depend on the live 640×480 menu bar); here they are named constants with a documented
default to be confirmed.

- [ ] **Step 1: Implement `scenario.py`**

`SheepShaver/e2e/sse2e/scenario.py`:
```python
"""P1 lifecycle scenario: boot -> Special>Shut Down -> assert clean exit."""
from __future__ import annotations

import time
from dataclasses import dataclass
from pathlib import Path

from . import observe
from .runner import Runner
from .vnc import Vnc

# Menu-bar coordinates at pinned 640x480 — CALIBRATED in Task 8, confirm before trusting.
SPECIAL_MENU_XY = (388, 10)      # x of the "Special" title in the menu bar, y inside the bar
SHUTDOWN_ITEM_XY = (388, 130)    # x same, y of the "Shut Down" item in the dropped menu


@dataclass
class Result:
    ok: bool
    reason: str
    log: str


def run_lifecycle(
    *,
    emulator: str,
    prefs: str,
    vncport: int,
    boot_timeout: float = 90.0,
    shutdown_timeout: float = 30.0,
    artifact_dir: Path | None = None,
) -> Result:
    runner = Runner(argv=[emulator, "--config", prefs])
    runner.start()
    try:
        # 1. Wait for the deterministic boot-ready signal.
        ev = _await_boot_ready(runner, boot_timeout)
        if ev is None:
            return Result(False, "boot timed out (no [BOOT] idle within timeout)", runner.log_text())
        if not observe.is_desktop_ready(ev):
            return Result(False, f"boot blocked on dialog (frontApp={ev.front_app!r} modal={ev.modal})",
                          runner.log_text())

        # 2. Drive Special > Shut Down over VNC.
        time.sleep(2.0)            # small settle margin after idle
        vnc = Vnc(port=vncport)
        if artifact_dir:
            vnc.capture(str(artifact_dir / "01-desktop.png"))
        vnc.click(*SPECIAL_MENU_XY)
        time.sleep(0.5)
        vnc.click(*SHUTDOWN_ITEM_XY)
        vnc.close()

        # 3. Assert clean exit: process exits on its own, log shows both signatures.
        code = runner.wait(timeout=shutdown_timeout)
        if code is None:
            return Result(False, "shutdown timed out — process did not exit (had to kill)",
                          runner.log_text())
        text = runner.log_text()
        if code == 0 and observe.saw_clean_shutdown(text):
            return Result(True, "clean lifecycle: booted to Finder, clean shutdown, exit 0", text)
        return Result(False, f"unclean exit (code={code}, clean_signatures={observe.saw_clean_shutdown(text)})", text)
    finally:
        runner.terminate()


def _await_boot_ready(runner: Runner, timeout: float) -> observe.BootReady | None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for line in runner.log_text().splitlines():
            ev = observe.parse_boot_ready(line)
            if ev is not None:
                return ev
        time.sleep(0.5)
    return None
```

- [ ] **Step 2: Verify it imports (offline)**

Run: `cd SheepShaver/e2e && .venv/bin/python -c "from sse2e import scenario; print('ok')"`
Expected: `ok`.

- [ ] **Step 3: Commit**

```bash
git add SheepShaver/e2e/sse2e/scenario.py
git commit -m "feat(e2e): scenario — boot/await-ready/shutdown/assert orchestration"
```

---

## Task 8: [BOOT-GATED] Calibrate menu coordinates + first green E2E + wiring

**Files:**
- Create: `SheepShaver/e2e/run_smoke.py`, `SheepShaver/e2e/README.md`
- Modify: `SheepShaver/Makefile` (add `e2e` target)
- Modify: `SheepShaver/e2e/sse2e/scenario.py` (calibrated coordinates)

- [ ] **Step 1: Create the entry-point script**

`SheepShaver/e2e/run_smoke.py`:
```python
#!/usr/bin/env python3
"""P1 smoke entry point. Requires a logged-in macOS GUI session (SDL needs a WindowServer)."""
import sys
import tempfile
from pathlib import Path

from sse2e import disk, scenario

HERE = Path(__file__).parent
EMULATOR = HERE.parent / "src" / "Unix" / "SheepShaver"
ROM = Path("/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom")
MASTER_DISK = Path("/Users/Shared/macemu/e2e_master.dsk")   # pristine, clean-shutdown master (Step 2)
VNCPORT = 5950


def main() -> int:
    work = Path(tempfile.mkdtemp(prefix="ss-e2e-"))
    run_disk = disk.copy_pristine(MASTER_DISK, work)
    prefs = disk.render_prefs(HERE / "config" / "test.prefs.template", work / "test.prefs",
                              rom=str(ROM), disk=str(run_disk), vncport=VNCPORT)
    artifacts = HERE / "artifacts"
    artifacts.mkdir(exist_ok=True)
    res = scenario.run_lifecycle(emulator=str(EMULATOR), prefs=str(prefs), vncport=VNCPORT,
                                 artifact_dir=artifacts)
    print(f"{'PASS' if res.ok else 'FAIL'}: {res.reason}")
    if not res.ok:
        (artifacts / "fail.log").write_text(res.log)
    return 0 if res.ok else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: [BOOT-GATED] Create the pristine master disk via a CLEAN shutdown**

Copy the known-good install and boot it once, then Special ▸ Shut Down by hand so the volume is
"properly put away" (never dirty → no repair prompt). Ask the user to:
```bash
cp /Users/Shared/macemu/macos86_fresh.dsk /Users/Shared/macemu/e2e_master.dsk
# boot it once with the e2e prefs, then Special > Shut Down manually, confirm clean exit
```
Expected: `e2e_master.dsk` exists and was cleanly shut down. (Smaller dedicated disk is a future
optimization; the fresh install is the fallback per the spec.)

- [ ] **Step 3: [BOOT-GATED] Calibrate the Special ▸ Shut Down coordinates**

Boot once and capture the desktop, then read the menu coordinates:
```bash
cd SheepShaver/e2e
./src/Unix/SheepShaver --config <prefs> &      # or ask the user to launch
.venv/bin/python -c "from sse2e.vnc import Vnc; Vnc(port=5950).capture('artifacts/cal-desktop.png')"
```
Open `cal-desktop.png`, read the pixel x of the "Special" menu title and the y of the "Shut Down"
item when the menu is open (click Special first, recapture). Update `SPECIAL_MENU_XY` and
`SHUTDOWN_ITEM_XY` in `scenario.py` with the measured values. Commit:
```bash
git add SheepShaver/e2e/sse2e/scenario.py
git commit -m "feat(e2e): calibrate Special>Shut Down menu coordinates at 640x480"
```

- [ ] **Step 4: [BOOT-GATED] Run the full smoke and confirm PASS**

Run: `cd SheepShaver/e2e && .venv/bin/python run_smoke.py`
Expected: `PASS: clean lifecycle: booted to Finder, clean shutdown, exit 0`, and the emulator
process exits on its own (no stray `SheepShaver`). If FAIL, inspect `artifacts/fail.log` and
`artifacts/01-desktop.png`.

- [ ] **Step 5: Add the `make e2e` target + README**

In `SheepShaver/Makefile`, add (guard for macOS, mirror existing platform guards):
```make
e2e:
	cd e2e && .venv/bin/python run_smoke.py
```

`SheepShaver/e2e/README.md`:
```markdown
# SheepShaver E2E VNC harness (P1)

Boots SheepShaver in an isolated config, waits for the deterministic `[BOOT] idle frontApp='Finder'`
signal, drives Special > Shut Down over VNC, and asserts a clean exit.

## Requirements
- A **logged-in macOS GUI session** — SDL needs a live WindowServer; this does NOT run on a
  headless macOS CI agent (no Xvfb equivalent on macOS).
- `python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`
- A pristine master disk at `/Users/Shared/macemu/e2e_master.dsk`, created via a clean Shut Down.

## Run
    make e2e          # from SheepShaver/
    # or: cd e2e && .venv/bin/python run_smoke.py

Offline unit tests (no boot): `cd e2e && .venv/bin/pytest -q`
```

- [ ] **Step 6: Commit**

```bash
git add SheepShaver/Makefile SheepShaver/e2e/run_smoke.py SheepShaver/e2e/README.md
git commit -m "feat(e2e): make e2e target + run_smoke entry point + README (P1 complete)"
```

- [ ] **Step 7: Update trackers (per CONTRIBUTING Documentation Lifecycle)**

- Flip ROADMAP A5 marker to 🟡 in-progress with a "P1 landed" note.
- Add a `CHANGELOG.md` entry under `[SheepShaver] Testing & benchmarking`.
- Bump the spec's `Updated:` date and mark P1 ✅.
```bash
git add docs/planning/ROADMAP.md CHANGELOG.md docs/superpowers/specs/2026-06-04-e2e-vnc-harness-design.md
git commit -m "docs: record E2E harness P1 landed (ROADMAP A5, changelog, spec)"
```

---

## Self-review notes

- **Spec coverage:** §4 architecture → Tasks 3–7 (the four modules); §5 scenario → Task 7; §6
  isolation → Tasks 1 (`idlewait`), 5 (pristine copy + `--config`); §11 boot signal + 3-layer
  defense → Task 1 (signal + modal disambiguation), Task 5/8 Step 2 (pristine clean master),
  Task 7 (watchdog timeouts + blocked-dialog failure); §7 deps → Task 2; §9 process → Task 8 Step 7.
- **Boot-gated honesty:** every step that needs a real boot is tagged **[BOOT-GATED]** and asks the
  user; offline steps (Tasks 1 build, 2–7) are fully verifiable without booting.
- **Known calibration debt:** menu coordinates (Task 7/8) and the exact `frontApp`/window-kind
  offsets (Task 1 Step 6) are confirmed against a live boot before the harness is trusted — these
  are the only values that cannot be known statically.
```
