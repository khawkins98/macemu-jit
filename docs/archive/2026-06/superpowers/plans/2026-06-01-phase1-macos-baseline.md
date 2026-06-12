> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# Phase 1: macOS 26 Baseline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build SheepShaver from this fork natively on Apple Silicon macOS 26.4.1, run its JIT test harnesses, boot Mac OS in interpreter and JIT modes, and record a performance baseline.

**Architecture:** No new code design — this phase makes the existing Linux-first build work on macOS arm64, exercises upstream's test harnesses (209-vector opcode harness, ROM harness), and produces a documented baseline in LEARNINGS.md. Build fixes are made minimally and committed individually so they can be PR'd upstream later.

**Tech Stack:** autotools (autoconf/automake), clang 17 (Xcode CLT), SDL2 2.32.10 (Homebrew), SheepShaver Unix build (`SheepShaver/src/Unix`), upstream harnesses (`SheepShaver/jit-test/`, `SheepShaver/rom-harness/`).

**Working repo:** `~/Documents/git/macemu-jit`, branch `macos-arm64`.

**Commit rule:** Never add a `Co-Authored-By` trailer. Keep messages in upstream's style: `fix:`/`docs:`/`build:` prefix, body explains root cause.

**Documentation rule:** Every task that discovers something non-obvious appends a dated entry to `LEARNINGS.md` under a `## 2026-06-01 — Phase 1 baseline` (or current date) section. Every task ends with a commit.

---

### Task 1: Install missing build tools

**Files:** none (system setup)

- [ ] **Step 1: Install automake (verified missing on this machine)**

```bash
brew install automake
```

- [ ] **Step 2: Verify the full toolchain is present**

```bash
which autoconf aclocal automake glibtoolize sdl2-config clang && sdl2-config --version
```

Expected: all six paths print (autoconf/aclocal/automake/glibtoolize under `/opt/homebrew/bin`), SDL version `2.32.10`, no "not found" lines.

- [ ] **Step 3: Record environment in LEARNINGS.md**

Append to `LEARNINGS.md`:

```markdown
## <today's date> — Phase 1 baseline

### Build environment
- macOS 26.4.1 arm64, Apple clang 17.0.0 (Command Line Tools, no full Xcode)
- Homebrew: autoconf, automake, libtool, pkgconf, sdl2 2.32.10, gmp, mpfr
```

- [ ] **Step 4: Commit**

```bash
cd ~/Documents/git/macemu-jit
git add LEARNINGS.md
git commit -m "docs: record Phase 1 macOS build environment"
```

---

### Task 2: Generate and run configure on macOS arm64

**Files:**
- Possibly modify: `SheepShaver/src/Unix/configure.ac`, `SheepShaver/src/Unix/autogen.sh`

- [ ] **Step 1: Run autogen.sh**

```bash
cd ~/Documents/git/macemu-jit/SheepShaver/src/Unix
NO_CONFIGURE=1 ./autogen.sh 2>&1 | tee /tmp/ss-autogen.log
```

Expected: exits 0, `configure` file generated. (If `NO_CONFIGURE` isn't honored, it may auto-run configure — that's fine, capture the log.)

Likely failure: `aclocal: command not found` (fixed by Task 1) or missing m4 macros (e.g. `AM_PATH_ESD`, GTK macros). If an m4 macro is missing, the minimal fix is installing the providing package (`brew install gtk+3` is NOT wanted — prefer stubbing/conditionalizing the macro since GTK is optional and we use SDL).

- [ ] **Step 2: Run configure for an SDL2 + JIT build**

```bash
cd ~/Documents/git/macemu-jit/SheepShaver/src/Unix
./configure --enable-sdl-video --enable-sdl-audio --enable-jit \
  --without-gtk --without-x --without-esd 2>&1 | tee /tmp/ss-configure.log
tail -40 /tmp/ss-configure.log
```

Expected: configure completes; summary shows SDL video/audio enabled. Check specifically what it prints about JIT — note whether the aarch64 JIT is wired in (look for `Enable JIT compiler ........ : yes`).

(If `--without-gtk/--without-x/--without-esd` are not recognized flag names, check `./configure --help | grep -iE 'gtk|x11|esd'` and use the actual spellings; record the correct invocation in LEARNINGS.md.)

- [ ] **Step 3: Verify what configure decided about the JIT**

```bash
grep -iE 'ENABLE_DYNGEN|USE_JIT|aarch64|AARCH64_JIT' SheepShaver/src/Unix/config.h | head -20
```

Expected: evidence the aarch64 JIT path is enabled (this fork's `--enable-jit` should select the aarch64 backend on arm64). If it instead disables JIT on Darwin/arm64, that's a configure.ac bug to fix in this task — find the architecture-selection block in `configure.ac`, add `aarch64-apple-darwin*`/`arm64*-apple-darwin*` to the case patterns that enable the JIT, re-run autogen + configure, and document the fix.

- [ ] **Step 4: Append findings to LEARNINGS.md and commit**

Append what configure detected (JIT enabled? which sources selected? Darwin-specific files used?) to `LEARNINGS.md`. Then:

```bash
cd ~/Documents/git/macemu-jit
git add -A
git commit -m "build: get SheepShaver configure working on macOS arm64"
```

(If no source changes were needed, the commit is just the LEARNINGS.md entry — use `docs:` prefix instead.)

---

### Task 3: Build SheepShaver (iterative breakage-fixing loop)

**Files:**
- Possibly modify: any source file that fails to compile on macOS/clang 17 — keep each fix minimal and separately committed

- [ ] **Step 1: Attempt the build, capturing the log**

```bash
cd ~/Documents/git/macemu-jit/SheepShaver/src/Unix
make -j8 2>&1 | tee /tmp/ss-make.log; echo "EXIT: $?"
```

Expected on first attempt: probably FAILS (upstream develops on Linux). That's the point of this task.

- [ ] **Step 2: Triage loop — repeat until `make` exits 0**

For each failure, follow this procedure:

1. Extract the first error: `grep -nE 'error:' /tmp/ss-make.log | head -10`
2. Root-cause it (read the failing file at the failing line; check whether it's a missing include, a Linux-only API like `personality()`/`linux/...` headers, an Obj-C/C++ issue, or a clang-17 strictness issue)
3. Apply the **minimal** fix:
   - Linux-only API → guard with `#ifdef __linux__` / provide Darwin path
   - Missing include on macOS → add the include guarded appropriately
   - clang strictness (e.g. mismatched prototypes) → fix the actual code
   - Do NOT disable warnings globally or add `-fpermissive`-style hacks
4. Rebuild: `make -j8 2>&1 | tee /tmp/ss-make.log; echo "EXIT: $?"`
5. Commit each logically-distinct fix separately:

```bash
cd ~/Documents/git/macemu-jit
git add <changed files>
git commit -m "fix: <what> on macOS arm64

<root cause: why this failed on Darwin / clang 17>"
```

6. Append a one-line entry per fix to `LEARNINGS.md` (what broke, why, how fixed).

Subagent guidance: dispatch one subagent per build error with the error text + failing file; have it return root cause + minimal patch; verify and commit in the main session.

- [ ] **Step 3: Verify the binary exists and links against expected libs**

```bash
ls -la ~/Documents/git/macemu-jit/SheepShaver/src/Unix/SheepShaver
file ~/Documents/git/macemu-jit/SheepShaver/src/Unix/SheepShaver
otool -L ~/Documents/git/macemu-jit/SheepShaver/src/Unix/SheepShaver | head -15
```

Expected: `Mach-O 64-bit executable arm64`, linked against SDL2 and system frameworks.

- [ ] **Step 4: Commit final LEARNINGS.md update for the build**

```bash
cd ~/Documents/git/macemu-jit
git add LEARNINGS.md
git commit -m "docs: record macOS arm64 build fixes for Phase 1"
```

---

### Task 4: Run the JIT opcode test harness

**Files:**
- Possibly modify: `SheepShaver/jit-test/run.sh` (if it has Linux-isms)

- [ ] **Step 1: Read the harness before running it**

```bash
cd ~/Documents/git/macemu-jit/SheepShaver
cat jit-test/run.sh | head -60
cat jit-test/README.md 2>/dev/null
```

Check for Linux-only assumptions (g++ vs clang++, `nproc`, Xvfb, `timeout` command — macOS needs `gtimeout` from coreutils or a rewrite).

- [ ] **Step 2: Run the harness**

```bash
cd ~/Documents/git/macemu-jit/SheepShaver
./jit-test/run.sh 2>&1 | tee /tmp/ss-jit-test.log
grep METRIC /tmp/ss-jit-test.log
```

Expected (matching upstream's Linux results): `METRIC pass=209 fail=0 total=209 score=100`

If the harness itself fails to run on macOS, fix it (same triage loop as Task 3, committed as `fix: make jit-test harness run on macOS`). If vectors FAIL that pass on Linux, **do not fix the JIT in this phase** — record exactly which vectors fail in LEARNINGS.md; that's Phase 2/3 input.

- [ ] **Step 3: Record results and commit**

Append to `LEARNINGS.md`: harness score on macOS arm64, any failing vectors, any harness changes needed.

```bash
cd ~/Documents/git/macemu-jit
git add -A
git commit -m "docs: record macOS jit-test harness baseline (X/209)"
```

---

### Task 5: Build and run the ROM harness

**Files:**
- Possibly modify: `SheepShaver/rom-harness/Makefile`, `SheepShaver/rom-harness/rom-harness.cpp`

- [ ] **Step 1: Ask the user for the ROM file path**

Ask: "Where is your Mac OS ROM file? (The 'Mac OS ROM' New World ROM file you mentioned having, or an OldWorld PowerMac ROM dump.)" Set `ROM_PATH` to the answer for the remaining steps.

- [ ] **Step 2: Build the ROM harness**

```bash
cd ~/Documents/git/macemu-jit/SheepShaver/rom-harness
make 2>&1 | tee /tmp/ss-rom-harness-build.log; echo "EXIT: $?"
```

Expected: builds. If it fails on macOS, apply the Task 3 triage loop (commit fixes as `fix: build rom-harness on macOS arm64`).

- [ ] **Step 3: Run the 10K-block scan**

```bash
cd ~/Documents/git/macemu-jit/SheepShaver/rom-harness
./rom-harness "$ROM_PATH" --count=10000 2>&1 | tee /tmp/ss-rom-harness.log
tail -5 /tmp/ss-rom-harness.log
```

Expected output format: `Score: X/Y`. Upstream's Linux baseline with a PowerMac 9500 OldWorld ROM is 1800/1825 (98.6%). Note: the user's ROM may be a NewWorld ROM, so the absolute numbers will differ — what matters is recording OUR baseline on OUR ROM.

- [ ] **Step 4: Record results and commit**

Append to `LEARNINGS.md`: ROM file used (name + size + MD5: `md5 "$ROM_PATH"`), score, failing block addresses if any.

```bash
cd ~/Documents/git/macemu-jit
git add -A
git commit -m "docs: record ROM harness baseline on macOS arm64"
```

---

### Task 6: Boot Mac OS in interpreter mode

**Files:**
- Create: `~/.sheepshaver_prefs` (outside repo)

- [ ] **Step 1: Ask the user for the disk image / install media path**

Ask: "Where is your Mac OS 9 disk image (or install media image)? And do you want a fresh install volume created, or boot an existing system disk image?" Set `DISK_PATH` accordingly.

- [ ] **Step 2: Create prefs file**

Write `~/.sheepshaver_prefs`:

```
rom <ROM_PATH>
disk <DISK_PATH>
ramsize 268435456
screen win/800/600
nosound false
nocdrom true
jit false
```

(`jit false` = interpreter mode for this task.)

- [ ] **Step 3: Launch SheepShaver (interpreter)**

```bash
cd ~/Documents/git/macemu-jit/SheepShaver/src/Unix
./SheepShaver 2>&1 | tee /tmp/ss-boot-interp.log
```

This opens an SDL window — the user needs to watch it. Expected: gray screen → Happy Mac → "Welcome to Mac OS" → desktop. Ask the user to confirm what they see and how long boot takes (wall clock).

If it crashes before drawing anything, capture the log + crash report (`ls -t ~/Library/Logs/DiagnosticReports/ | head -3`) and apply the triage loop — likely suspects on macOS 26: SDL window creation, Mach VM allocation of low memory, signal handler setup.

- [ ] **Step 4: Record results and commit**

Append to `LEARNINGS.md`: did it boot, how far, boot time, crashes + root causes, screenshots if useful (`screencapture -w /tmp/ss-interp-desktop.png` while the window is focused).

```bash
cd ~/Documents/git/macemu-jit
git add -A
git commit -m "docs: record interpreter-mode boot baseline on macOS 26"
```

---

### Task 7: Boot Mac OS in JIT mode

**Files:** none new (prefs change only)

- [ ] **Step 1: Enable JIT in prefs**

Edit `~/.sheepshaver_prefs`: change `jit false` → `jit true`. Also check whether this fork uses the `SS_USE_JIT` env var as an additional gate (search: `grep -rn "SS_USE_JIT" ~/Documents/git/macemu-jit/SheepShaver/src/ | head -5`) and set it if so.

- [ ] **Step 2: Launch SheepShaver (JIT)**

```bash
cd ~/Documents/git/macemu-jit/SheepShaver/src/Unix
SS_USE_JIT=1 ./SheepShaver 2>&1 | tee /tmp/ss-boot-jit.log
```

Expected based on upstream's Linux status: reaches "Welcome to Mac OS" splash, then hangs (does not reach desktop). On macOS it may not get that far — W^X/MAP_JIT issues could kill it at the first code-cache write (look for `EXC_BAD_ACCESS` with "executable region" or `mprotect` errors in the log/crash report).

**Do not fix JIT bugs in this task.** The deliverable is a precise record of where it stops on macOS and why (crash report, last log lines, hang vs crash).

- [ ] **Step 3: Capture diagnostics**

```bash
# If it hung: sample the process before killing it
sample SheepShaver 5 -file /tmp/ss-jit-hang-sample.txt 2>/dev/null
# If it crashed:
ls -t ~/Library/Logs/DiagnosticReports/ | head -3
```

- [ ] **Step 4: Record results and commit**

Append to `LEARNINGS.md`: exact stopping point (crash vs hang, where), diagnostics summary, comparison to upstream's Linux behavior. This is the primary input for Phase 2.

```bash
cd ~/Documents/git/macemu-jit
git add -A
git commit -m "docs: record JIT-mode boot behavior on macOS 26 arm64"
```

---

### Task 8: Benchmark vs the Rosetta 2 path

**Files:** none in repo (external comparison)

- [ ] **Step 1: Download the official kanjitalk755-based universal build**

Get the current emaculation.com SheepShaver universal build (the community build referenced from https://www.emaculation.com — ask the user if they already have it installed). Place it in `/Applications` or `~/Downloads`.

- [ ] **Step 2: Run it with the same prefs (forcing x86_64 under Rosetta)**

```bash
# Force the x86_64 slice so its JIT is active under Rosetta 2:
arch -x86_64 /Applications/SheepShaver.app/Contents/MacOS/SheepShaver
```

Confirm with the user it boots to desktop. Note: it reads the same `~/.sheepshaver_prefs`; set `jit true`.

- [ ] **Step 3: Run an in-guest benchmark on both setups**

The comparison matrix (3 configurations, same disk image):

| Config | Binary | JIT |
|---|---|---|
| A | Our fork, native arm64 | off (interpreter) |
| B | Our fork, native arm64 | on (as far as it works — if it can't reach desktop, mark N/A) |
| C | Official build, x86_64 under Rosetta | on |

In-guest benchmark: whatever Ken has available — MacBench 5.0 (preferred, matches historical numbers), Speedometer 4, or as a fallback a stopwatch boot-time + Finder responsiveness comparison. Ask the user to run it in each config and report numbers.

- [ ] **Step 4: Record the baseline table and commit**

Append to `LEARNINGS.md` a table with the three configs and their numbers + boot times. This is the number the ARM64 JIT must beat (config C) and the floor it must exceed (config A).

```bash
cd ~/Documents/git/macemu-jit
git add -A
git commit -m "docs: record Phase 1 performance baseline (native vs Rosetta)"
```

---

### Task 9: Phase 1 wrap-up

**Files:**
- Modify: `LEARNINGS.md`, `docs/superpowers/specs/2026-06-01-macos-arm64-jit-design.md`

- [ ] **Step 1: Verify Phase 1 exit criteria from the design doc**

Check each:
- [ ] Builds reproducibly on macOS 26 arm64 (run `make clean && make -j8` once more; expected exit 0)
- [ ] Harness results recorded (jit-test score + ROM harness score in LEARNINGS.md)
- [ ] Baseline benchmark table in LEARNINGS.md

- [ ] **Step 2: Update the design doc**

In `docs/superpowers/specs/2026-06-01-macos-arm64-jit-design.md`, under "### Phase 1", add a `**Status:** Complete <date>` line with a 2-3 sentence summary of outcomes and a pointer to the LEARNINGS.md section.

- [ ] **Step 3: Final commit and push**

```bash
cd ~/Documents/git/macemu-jit
git add -A
git commit -m "docs: Phase 1 baseline complete"
git push
```

- [ ] **Step 4: Report to user**

Summarize: build status, harness scores, boot results (interpreter vs JIT), benchmark table, and the recommended priorities for Phase 2 based on what was found.

---

## Self-review notes

- **Spec coverage:** Phase 1 items 1-6 from the design doc map to Tasks 2-3 (build), 4-5 (harnesses), 6 (interpreter boot), 7 (JIT boot), 8 (benchmark); exit criteria checked in Task 9.
- **Known unknowns:** build breakage specifics (Task 3 is a structured triage loop, not pretend-precise fixes); ROM/disk paths (asked from user at execution time); which in-guest benchmark Ken has (asked in Task 8).
- **Not in this phase:** any JIT code changes (correctness/W^X work is Phase 2-3); failures found by harnesses are recorded, not fixed.
