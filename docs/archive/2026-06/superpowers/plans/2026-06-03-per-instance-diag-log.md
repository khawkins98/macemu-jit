# Per-Instance JIT Diagnostic Log Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Each SheepShaver instance writes its JIT diagnostic log to a unique timestamped+PID file, with a `/tmp/jit_diag.log` symlink always pointing at the latest run, so concurrent instances can no longer truncate each other's logs.

**Architecture:** All code changes live inside one function, `jit_diag_log_open()` in `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp`. The `SS_JIT_DIAG_LOG` env var override is preserved exactly (no symlink in that case). CLAUDE.md documentation is updated to match.

**Tech Stack:** C++ (POSIX: `time`/`localtime_r`/`snprintf`/`getpid`/`unlink`/`symlink`), GNU make.

**Spec:** `docs/superpowers/specs/2026-06-03-per-instance-diag-log-design.md`

**Testing note:** There is no unit-test framework covering this file; the existing harnesses test PPC opcode semantics, not host-side logging. Verification is (a) a compile syntax check now, and (b) a deferred runtime checklist (Task 3) that must NOT be run until the other agent's emulator test run is finished — rebuilding the binary mid-run would contaminate their bisect. Check with `pgrep -x SheepShaver` before starting Task 3.

---

### Task 1: Rewrite `jit_diag_log_open()` with per-instance naming + latest symlink

**Files:**
- Modify: `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp:57` (includes) and `:65-79` (function)

**IMPORTANT — concurrent-edit guard:** Another agent may be editing this file. Before editing, run `git -C /Users/khawkins/Documents/git/macemu-jit status --short` and `git -C /Users/khawkins/Documents/git/macemu-jit diff --stat SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp`. If the file has uncommitted changes that are NOT yours, STOP and report back — do not edit over another agent's work in progress. Line numbers below were correct as of commit `e088bfc4`; if the file has changed, locate the function by searching for `jit_diag_log_open`.

- [ ] **Step 1: Add required includes**

The file already includes `<time.h>` (line 57) and `<stdlib.h>` (line 22). Add `<unistd.h>` (getpid, unlink, symlink), `<errno.h>`, and `<string.h>` (strerror) next to the existing `<time.h>` include inside the aarch64 JIT guard.

Current code at line 57:

```cpp
#include <time.h>
static double jit_elapsed_s() {
```

Change to:

```cpp
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
static double jit_elapsed_s() {
```

(Duplicate includes are harmless if `sysdeps.h` already pulls any of these in.)

- [ ] **Step 2: Replace the function and its comment block**

Current code (lines 65–79):

```cpp
static FILE *jit_log_file = nullptr;
/* Open the diagnostic log.  Path is SS_JIT_DIAG_LOG if set, else /tmp/jit_diag.log.
 *
 * WHY the env var: the path used to be hardcoded, and ANY concurrent SheepShaver
 * process (e.g. a jit-test harness vector running while a boot measurement is in
 * progress) would fopen(..., "w") the same file and truncate the measurement's log
 * mid-run — this silently corrupted a concurrent boot-time measurement.
 * Parallel agents must set SS_JIT_DIAG_LOG to distinct paths. */
static void jit_diag_log_open(void) {
	if (jit_log_file) return;
	const char *path = getenv("SS_JIT_DIAG_LOG");
	if (!path || !*path) path = "/tmp/jit_diag.log";
	jit_log_file = fopen(path, "w");
	fprintf(stderr, "[JIT] diagnostic log: %s\n", path);
}
```

Replace with:

```cpp
static FILE *jit_log_file = nullptr;
/* Open the diagnostic log.
 *
 * Path resolution:
 *   - SS_JIT_DIAG_LOG, if set and non-empty, is used verbatim (no symlink touched).
 *   - Otherwise a per-instance file /tmp/jit_diag.<YYYYMMDD-HHMMSS>.<pid>.log is
 *     created and the stable alias /tmp/jit_diag.log is re-pointed (symlink) at it.
 *
 * WHY per-instance files: the default used to be a single shared /tmp/jit_diag.log
 * opened with "w", so ANY concurrent SheepShaver process reaching a heartbeat would
 * truncate another instance's log mid-run — silently corrupting a concurrent
 * boot-time measurement.  Timestamp+pid naming makes each run's log unique; the
 * symlink keeps `tail -f /tmp/jit_diag.log` and jit-analyze.py defaults working.
 * Note: harness test vectors (SS_TEST_HEX) exit in milliseconds and never reach
 * the first 5s heartbeat, so they never create log files. */
static void jit_diag_log_open(void) {
	if (jit_log_file) return;
	const char *path = getenv("SS_JIT_DIAG_LOG");
	char ts_path[128];
	bool make_link = false;
	if (!path || !*path) {
		time_t now = time(NULL);
		struct tm tmv;
		localtime_r(&now, &tmv);
		snprintf(ts_path, sizeof(ts_path), "/tmp/jit_diag.%04d%02d%02d-%02d%02d%02d.%d.log",
		         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
		         tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)getpid());
		path = ts_path;
		make_link = true;
	}
	jit_log_file = fopen(path, "w");
	fprintf(stderr, "[JIT] diagnostic log: %s\n", path);
	if (jit_log_file && make_link) {
		/* Refresh the latest-run alias.  unlink() also replaces a stale regular
		 * file left behind by a pre-symlink build.  Failure is non-fatal: the
		 * real log still works, only the convenience alias is lost. */
		unlink("/tmp/jit_diag.log");
		if (symlink(path, "/tmp/jit_diag.log") != 0)
			fprintf(stderr, "[JIT] warning: could not update /tmp/jit_diag.log symlink: %s\n",
			        strerror(errno));
	}
}
```

- [ ] **Step 3: Syntax-only compile check (does NOT touch the binary or any .o file)**

This verifies the code compiles with the project's exact flags without producing output files, so the other agent's running binary and object files are untouched.

Run:

```bash
cd /Users/khawkins/Documents/git/macemu-jit/SheepShaver/src/Unix && \
g++ -I../MacOSX/Launcher -I../MacOSX -I../kpx_cpu/include -I../kpx_cpu/src \
    -DUSE_AARCH64_JIT -I../kpx_cpu/src/cpu/jit/aarch64 -DUSE_JIT -I../include -I. \
    -I../CrossPlatform -I../slirp -DHAVE_CONFIG_H -D_REENTRANT \
    -DDATADIR=\"/usr/local/share/SheepShaver\" -g -O2 -I/opt/homebrew/include \
    -I/opt/homebrew/include/SDL2 -D_THREAD_SAFE -mdynamic-no-pic \
    -fsyntax-only ../kpx_cpu/src/cpu/ppc/ppc-cpu.cpp
```

Expected: exits 0 with no error output (warnings about unused `-o`/`-c` arguments are fine; there are none here since we omit them).

If this command's include paths have drifted, regenerate it with:
`make -n -W ../kpx_cpu/src/cpu/ppc/ppc-cpu.cpp obj/ppc-cpu.o | head -1`, then replace `-c ... -o obj/ppc-cpu.o` with `-fsyntax-only <same source path>`.

- [ ] **Step 4: Commit**

```bash
cd /Users/khawkins/Documents/git/macemu-jit && \
git add SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp && \
git commit -m "feat: per-instance JIT diag log files + latest-run symlink

Default path is now /tmp/jit_diag.<timestamp>.<pid>.log; /tmp/jit_diag.log
becomes a symlink to the most recent run. SS_JIT_DIAG_LOG override unchanged.
Fixes concurrent instances truncating each other's logs.

NOTE: binary not rebuilt yet — deferred until the running emulator test
(other agent's opcode bisect) completes. See Task 3 of the plan."
```

---

### Task 2: Update CLAUDE.md documentation

**Files:**
- Modify: `/Users/khawkins/Documents/git/macemu-jit/CLAUDE.md:183`, `:187`, `:211-212`

- [ ] **Step 1: Update the stderr example (line 183)**

Old:

```
[JIT] diagnostic log: /tmp/jit_diag.log
```

New:

```
[JIT] diagnostic log: /tmp/jit_diag.20260603-141502.12345.log
```

- [ ] **Step 2: Update the log-file section heading (line 187)**

Old:

```
**`/tmp/jit_diag.log`** (heartbeat every ~5s, with per-region block counters):
```

New:

```
**`/tmp/jit_diag.<timestamp>.<pid>.log`** (heartbeat every ~5s, with per-region block
counters). `/tmp/jit_diag.log` is a symlink to the most recent run's log, so
`tail -f /tmp/jit_diag.log` and `jit-analyze.py` defaults always follow the latest run:
```

- [ ] **Step 3: Update the env var description (lines 211-212)**

Old:

```
SS_JIT_DIAG_LOG=/path        # Diagnostic log path (default /tmp/jit_diag.log) — parallel
                             # agents must use distinct paths
```

New:

```
SS_JIT_DIAG_LOG=/path        # Diagnostic log path override (no symlink touched when set).
                             # Default: per-instance /tmp/jit_diag.<timestamp>.<pid>.log,
                             # with /tmp/jit_diag.log symlinked to the latest run.
```

- [ ] **Step 4: Commit**

```bash
cd /Users/khawkins/Documents/git/macemu-jit && \
git add CLAUDE.md && \
git commit -m "docs: document per-instance JIT diag log naming in CLAUDE.md"
```

---

### Task 3: DEFERRED — Build and runtime verification

**⚠️ PRECONDITION: Do NOT start this task while another agent's emulator run is active.**
Check with:

```bash
pgrep -x SheepShaver && echo "EMULATOR RUNNING — DO NOT PROCEED" || echo "clear to proceed"
```

Also confirm with the user before running this task — per project rules, agents must not launch emulator instances; the user runs boots.

**Files:** none modified (build + verification only)

- [ ] **Step 1: Rebuild SheepShaver**

```bash
cd /Users/khawkins/Documents/git/macemu-jit/SheepShaver && make build-ss
```

Expected: clean build, binary at `SheepShaver/src/Unix/SheepShaver`.

- [ ] **Step 2: Harness regression — opcode tests still pass, no log spam**

```bash
cd /Users/khawkins/Documents/git/macemu-jit/SheepShaver && \
ls /tmp/jit_diag.* > /tmp/diag_files_before.txt 2>/dev/null; \
make test-opcodes; \
ls /tmp/jit_diag.* > /tmp/diag_files_after.txt 2>/dev/null; \
diff /tmp/diag_files_before.txt /tmp/diag_files_after.txt && echo "NO NEW LOG FILES — PASS"
```

Expected: `score=100` from the harness AND "NO NEW LOG FILES — PASS" (test vectors exit before the first heartbeat, so they create no logs).

- [ ] **Step 3: Stale-file migration check + boot run (USER runs the boot)**

```bash
# Simulate a stale regular file from a pre-change build:
rm -f /tmp/jit_diag.log && touch /tmp/jit_diag.log
```

Ask the user to start a boot (`cd SheepShaver && make run-jit`). After ~10 seconds of guest execution, verify:

```bash
# 1. stderr (in the run log) shows the timestamped path:
#    [JIT] diagnostic log: /tmp/jit_diag.<YYYYMMDD-HHMMSS>.<pid>.log
# 2. The symlink replaced the stale regular file and points at the new log:
ls -la /tmp/jit_diag.log          # expected: lrwxr-xr-x ... -> /tmp/jit_diag.<ts>.<pid>.log
# 3. Heartbeats flow through the symlink:
tail -2 /tmp/jit_diag.log         # expected: [JIT N.Ns] blocks=... lines
# 4. Analysis tool works with no args:
python3 /Users/khawkins/Documents/git/macemu-jit/SheepShaver/tools/jit-analyze.py diag
```

- [ ] **Step 4: Override regression — SS_JIT_DIAG_LOG still wins, no symlink touched**

Ask the user to run a boot with `SS_JIT_DIAG_LOG=/tmp/custom_diag.log`. Verify:

```bash
ls -la /tmp/custom_diag.log       # expected: regular file, receiving heartbeats
ls -la /tmp/jit_diag.log          # expected: UNCHANGED from step 3 (still points at the old run)
```

- [ ] **Step 5: Kill the boot, commit verification note**

```bash
pkill -9 -x SheepShaver
cd /Users/khawkins/Documents/git/macemu-jit && \
git commit --allow-empty -m "test: runtime verification of per-instance diag logs complete

Verified: timestamped log creation, symlink refresh over stale regular file,
heartbeats via symlink, jit-analyze.py default path, SS_JIT_DIAG_LOG override,
opcode harness score=100 with no new log files."
```
