# Upstream Lineage & Backport Sweep

**Date:** 2026-06-04
**Scope:** Trace the fork lineage of this repo, find upstream commits that never
made it down the inheritance line, and decide what (if anything) is worth importing
for **our** target: the macOS arm64 port of BasiliskII + SheepShaver with the AArch64 JIT
(Unix build, SDL video/audio, `--without-gtk --without-x`, emulated PPC).

> **TL;DR** — This fork is *remarkably current* with upstream. The raw git counts
> ("1,214 commits behind kanjitalk755") are **misleading**: rcarmo and this fork import
> upstream fixes as fresh cherry-picks/PR-merges with new SHAs, so git double-counts them.
> A **content** diff (by commit subject, hunk-verified) shows the genuine gap is **13
> commits**, all dated 2024+. After filtering for things that (a) compile in our build and
> (b) aren't already present by capability, **nothing is a required backport.** The only
> substantive optional pickup is **VDE networking for SheepShaver** (`06d8bc02`); SDL3
> video work (`e596e215`) is worth tracking for a future SDL2→SDL3 migration.

---

## 1. The lineage

```
cebix/macemu  ──────────────►  kanjitalk755/macemu  ──────────►  rcarmo/macemu-jit  ──►  khawkins98/macemu-jit
(Christian Bauer, original)    (active community fork)            (ARM64 JIT, Linux/Pi)   (THIS fork: macOS arm64)
SourceForge → GitHub           tip: 2026-05-20                    `upstream` remote        `origin` / HEAD
maintained through 2025-01     tracks + feeds cebix               tip: 2026-05-17          branch: macos-arm64
```

| Remote | Repo | Role | Tip (at time of writing) |
|--------|------|------|--------------------------|
| `cebix` | github.com/cebix/macemu | Original macemu (Christian Bauer) | 2025-01-06 |
| `kanjitalk755` | github.com/kanjitalk755/macemu | Active community fork; the de-facto upstream | 2026-05-20 |
| `upstream` | github.com/rcarmo/macemu-jit | Adds the ARM64 JIT (originally for Linux ARM64 / Orange Pi / Raspberry Pi) — **our direct parent** | 2026-05-17 |
| `origin` | github.com/khawkins98/macemu-jit | This fork — macOS arm64 port of the JIT | HEAD |

Relationships (via `git merge-base`):

- **cebix ↔ kanjitalk755** share history until **2020-11-22** and cross-pollinate
  (kanjitalk755 is 916 commits ahead of cebix; cebix only 31 ahead of kanjitalk755 — mostly
  README/links/CI). cebix is effectively the slower-moving mirror; kanjitalk755 is where the
  action is.
- **HEAD ↔ rcarmo (`upstream`)**: merge-base **= rcarmo's tip** → **we are 0 commits behind
  rcarmo.** This fork *is* rcarmo's master plus the macOS-arm64 work.
- **HEAD ↔ kanjitalk755**: merge-base is **2014-11-29** (`b1270264` "Changed page zero size
  to 4 kB to fix problem on OS X 10.10"). That is the point where rcarmo's ARM JIT branched
  off and grew its own (squashed/rebased) history. **This 2014 merge-base is what makes git
  report a 1,214-commit gap — it is an artifact of history shape, not of missing content.**

---

## 2. Why the raw counts lie — and the method that fixes it

`git rev-list --count kanjitalk755/master ^HEAD` reports **1,214**. But rcarmo (and this
fork) did not *merge* upstream — they **re-applied** upstream fixes as new commits (cherry-pick
or PR-merge), which get **new SHAs**. So SHA-based set difference counts every imported fix as
"missing."

**Method used here:** compare by **commit subject line** against the full set of HEAD subjects
(`git log --no-merges --format=%s HEAD`, 4,013 subjects), then **hunk-verify** every survivor.

- Spot-checks confirm the import pattern: `high precision timer`, `drag-and-drop on main
  thread`, `uninitialized SDL surface segfault`, `deadlock in timer`, `fres`, `unaligned
  lmw/stmw`, `areg7 reset`, `thread leak`, `array OOB`, `SLiRP buffer overflows` — **all
  present in HEAD** under different SHAs.
- After subject-matching, the genuine gap collapses from 1,214 to **13**, *all dated 2024+*.
  Everything older has been absorbed.

**Method limitation (stated for honesty):** exact-subject matching *over-reports* safely
(a reworded import shows as a false gap — caught at the hunk-verify step) but can *under-report*
on generic subjects (a kanjitalk755 "Fix warning" colliding with an unrelated HEAD "Fix
warning" would read as present). All 13 survivors have distinctive subjects and were
independently spot-checked, so the residual risk is contained — but "gap = 13" is a
well-supported estimate, not an absolute.

---

## 3. The genuine gap: 13 commits (all 2024+), triaged

Two of these (`e596e215` 2026-01, `964e8127` 2026-03) **predate** our rcarmo merge-base
(2026-05-17) — they were **skipped during import, not merely un-synced.** The import is
*selective*, which is exactly why the maintenance recommendation (§5) is a periodic
content-diff sweep rather than "rebase to a newer cutoff."

### Verdict legend
- ✅ **Take** — applies to our build, real benefit, low risk
- 🟡 **Optional** — feature or situational; take if you want the capability
- ⏳ **Track** — only relevant to a future change (e.g. SDL3 migration)
- ⛔ **Skip** — does not compile in our config, or capability already present, or wrong platform

| Commit | Date | Subject | Touches | Verdict | Why |
|--------|------|---------|---------|---------|-----|
| `06d8bc02` | 2026-05-06 | Add support for VDE to SheepShaver | `ether_unix.cpp`, SS `configure.ac`, `main_unix.cpp`, `main.h` | 🟡 **Optional** | Real, self-contained networking feature. Fixes VDE packet length (trailing garbage), adds SS configure support, and makes the VDE destination a savable `ether` pref — enables e.g. remote TAP-over-ssh. Works on macOS with `vdeplug4`. The only substantive pickup in the gap. |
| `e596e215` | 2026-01-31 | SDL3: blit not required in `SDL_UnlockTexture()` | `video_sdl3.cpp` | ⏳ **Track** | Touches **`video_sdl3.cpp` only**. We build **SDL2** (`video_sdl2.cpp`). No effect today; fold in *if/when* we migrate to SDL3 video. |
| `7d8a9fe1` | 2026-05-16 (Nov-'25 authored) | Disable stack protection for PPC signal handlers | SS `main_unix.cpp` | ⛔ **Skip** | Adds `__attribute__((no_stack_protector))` to `sigusr2_handler`/`sigsegv_handler`/`sigill_handler` — **all under `#if !EMULATED_PPC`**. Our build is `EMULATED_PPC 1`; these handlers are compiled out. This is a *native-PowerPC-host* fix, not for an emulated build. |
| `aff612c4` | 2026-05-19 | Fix incorrect printf/snprintf use | SS `main_unix.cpp` | ⛔ **Skip** | Fixes `printf("%s\n")`-with-missing-arg and `sprintf`→`printf` in the crash-register-dump code. Every hunk is inside `#if !EMULATED_PPC` (the `tick_func` native crash dumper + native sig handlers). Not compiled for us. (We already did our own `sprintf`→`snprintf` hardening in `prefs.cpp`, commit `d8a25819`.) |
| `964e8127` | 2026-03-25 | Fix macOS build: only use `--export-dynamic` on non-Darwin | BII `configure.ac` | ⛔ **Skip** | Guards the `--export-dynamic` flag with `case $host_os in darwin*) ;;` — but the flag is added **only inside the `WANT_GTK=GTK3` block**. We build `--without-gtk`, so the line never executes. Harmless to take, zero effect on our config. |
| `6c9a42d9` | 2026-05-15 | Fix linker script check | SS `configure.ac` | ⛔ **Skip** | `(char*)&main` → `(char*)(&main)` for **GCC 16** acceptance, inside `if [[ -n "$LINKER_SCRIPT_FLAGS" ]]`. On Darwin `LINKER_SCRIPT_FLAGS` is empty (the darwin linker-script line is commented out) and we use clang. No effect. Cheap future-proofing only. |
| `b4c6e913` | 2026-05-16 | Fix linker script check (BII) | BII `configure.ac` | ⛔ **Skip** | Same one-liner for BII. Same reasoning — skipped on Darwin, clang not GCC 16. |
| `26fffb6a` | 2026-05-16 | Only use `SDL_VERSION_ATLEAST` if building with SDL | SS `main_unix.cpp` | ⛔ **Skip** | Only matters for a **no-SDL** build. We always build with SDL. |
| `3dd2b524` | 2026-05-16 | Prevent miscompilation under -O2 by GCC 8 | core | ⛔ **Skip** | GCC 8 specific. We use clang. |
| `f95dc655` | 2026-05-16 | Fix detection of `loff_t` via `sys/types.h` | `configure.ac` | ⛔ **Skip** | Build portability; `loff_t` resolves fine on macOS. |
| `533cf6fa` | 2026-05-16 | Adjust reads to be kernel-relative | Linux `sheep_net` | ⛔ **Skip** | Linux kernel module. N/A on macOS. |
| `6787dce8` | 2026-05-16 | Add `linux/sched.h` for `CLONE_VM` | Linux `sheepthreads.c` | ⛔ **Skip** | Linux-only. |
| `91d58b12` | 2026-05-15 | Fix Wayland detection when there's no GTK | `configure.ac` | ⛔ **Skip** | Linux/Wayland. N/A on macOS. |

**Net: 1 optional feature (VDE), 1 to track (SDL3), 11 not applicable.**

---

## 4. cebix's unique commits — systematically checked, nothing required

cebix has 20 commits (by subject) not in HEAD. All are either irrelevant to us or
**already present by capability** under a different SHA/message:

| cebix commit | Subject | Status for us |
|--------------|---------|---------------|
| `e5be177f` | Add support for AARCH64 Mach exception | **Capability present.** Our `configure.ac` has `darwin*:arm) ac_cv_have_mach_exceptions=yes` (line 1307) and our CrossPlatform `sigsegv.cpp` has arm64 mach blocks — we run on macOS arm64, so fault handling demonstrably works. cebix's specific commit isn't in our history, but the function is. Re-verify only if you touch `sigsegv.cpp`. |
| `96691762` | Avoid conflict with MacOS 12.x (rename `slirp/VERSION`) | **Capability present.** The case-insensitive-APFS clash with the C++ `<version>` header is already avoided — our tree carries the file as `slirp/VERSION_`, and nothing `#include <version>`. |
| `15f7a351` | Fix `AudioDevice.cpp` compile on recent macOS | **N/A.** `AudioDevice.cpp` is the Cocoa/CoreAudio native-app audio path; our SDL build uses SDL audio. |
| `77944672`, `9abe3b32` | Added support for AARCH64 / Travis CI for AARCH64 | Superseded — our entire JIT is AArch64. |
| `b04dc5e5`, `c961b59e` | Extend prompt buffer 16→32; add `<stdlib.h>` | cxmon/debugger-adjacent; negligible, and we don't ship the Cocoa debugger path. |
| README / links / gettext / Xcode-project / SDK commits | — | Documentation/build-cosmetic; irrelevant to the Unix+SDL build. |

The SLiRP buffer-overflow hardening (`d26ae37e` on cebix) is **already in HEAD** as
`877782f3 "Fix potential buffer overflows in SLiRP"`.

---

## 5. Recommendations & decisions

> **Decision (2026-06-04):** import **VDE networking**, **SDL3 (with the `e596e215` fix)**,
> and the **Wayland-detection fix** now; defer the remaining Linux-only patches to the
> backlog in §6. Work tracked via subagents following `CONTRIBUTING.md`.

1. **Import VDE networking (`06d8bc02`) — IN PROGRESS.** The single substantive,
   applicable, self-contained item in the gap. Touches shared `ether_unix.cpp` plus
   SheepShaver `configure.ac`/`main_unix.cpp`/`main.h`. Requires `vdeplug` at build/run time
   (`vdeplug 2.3.3` confirmed installed via Homebrew `vde`). Also fixes two real bugs in the
   existing VDE path (packet length sent as `sizeof(packet)` → trailing garbage; an infinite
   send-retry loop). Verify it builds against our SDL/Unix config and doesn't disturb the
   harness (`make harness-count` / `make test-jit` score=100) or boot.

2. **Integrate SDL3 + the `e596e215` fix — IN PROGRESS.** We already carry `video_sdl3.cpp`
   (present, not compiled; build defaults to SDL2). Bring the `SDL_UnlockTexture()`
   blit-removal/format-unification fix and make the SDL3 backend buildable (`--with-sdl3`,
   requires `brew install sdl3`). SDL2 stays the default backend.

3. **Import the Wayland-detection fix (`91d58b12`) — IN PROGRESS.** Gated
   `#if REAL_ADDRESSING && defined(__linux__)`, so it is **inert on our macOS
   `DIRECT_ADDRESSING` build** (compiles out). Bringing it now means a future
   headless/SDL Linux deployment already has the fix in place. **Validation caveat:** it is
   inert on *any* 64-bit host — including 64-bit Linux ARM64 — because 64-bit builds use
   `DIRECT_ADDRESSING` (see §6.1). Exercising it requires a **32-bit ARM** (real-addressing)
   build or an explicit `--enable-addressing=real`. So it ships as "correct + free, validation
   deferred to a 32-bit ARM rig," not "verified."

4. **Maintenance going forward — content-diff sweeps, not rebases.** Because imports are
   selective cherry-picks with new SHAs, the git "behind" count will *always* look enormous
   and *always* be wrong. To find the real gap, re-run the subject-diff:

   ```bash
   git fetch kanjitalk755 cebix upstream --tags
   git log --no-merges --format='%s' HEAD > /tmp/head_subjects.txt
   git log --no-merges --since=<last-sweep-date> --format='%H|%ci|%s' kanjitalk755/master ^HEAD \
   | while IFS='|' read h d s; do
       grep -qxF "$s" /tmp/head_subjects.txt || printf '%s|%s|%s\n' "${h:0:9}" "${d:0:10}" "$s"
     done | sort -t'|' -k2 -r
   ```

   Then **hunk-verify each survivor** for (a) `#if !EMULATED_PPC` / Linux / GTK / SDL3 guards
   and (b) applicability to the Unix+SDL+arm64 config before importing. A quarterly sweep is
   ample given upstream's cadence (~13 relevant-ish commits in ~16 months).

---

## 6. Deferred backlog — Linux/Wayland re-convergence

**Why these are deferred, not dropped:** our direct parent `rcarmo/macemu-jit` is a
**Linux ARM64** project (Orange Pi / Raspberry Pi). The macOS-arm64 work is the divergence;
the AArch64 JIT itself is platform-agnostic. We still carry all the inherited Linux glue
(`BasiliskII/src/Unix/Linux/NetDriver/sheep_net.c`, `.../Linux/etherhelpertool.c`,
`SheepShaver/src/Unix/Linux/sheepthreads.c`), so these upstream fixes *would* apply to a
Linux build from this tree. We defer them because **we have no Linux ARM64 host to build or
test on** — importing platform code we can't exercise is untested drift. The right time to
take them is a dedicated "Linux re-convergence" pass, built and booted on real Pi/Orange Pi
hardware (matching rcarmo's targets).

**How these were found:** systematic content-diff (commit-subject vs HEAD) of
`kanjitalk755/master` and `cebix/master`, then per-commit hunk inspection. See §2 for the
method; re-run the sweep in §5.4 to refresh. All live in `kanjitalk755/master` as of
2026-05-20.

| Commit | Subject | What it fixes | When to take it |
|--------|---------|---------------|-----------------|
| `750d3a82` | sheep_net.c: use `module_init` instead of `init_module` | The Linux raw-ethernet **kernel module** won't build on modern kernels (`init_module`/`cleanup_module` were removed). Switches to `module_init()`/`module_exit()`. | When building native bridged ethernet (the `sheep_net` driver) on a modern Linux kernel. Not needed if relying on slirp + VDE. |
| `6787dce8` | Add `linux/sched.h` to sheepthreads.c for CLONE_VM | Header guard so SheepShaver's clone-based thread shim compiles on modern glibc/kernels. Adds `linux/sched.h` to the configure header probe. | First time we build SheepShaver on a recent Linux distro. Low-risk header guard. |
| `db897d0d` | Memory leak in Linux etherhelpertool.c | Plain bug: `free(outgoing)` should be `free(incoming)` on the malloc-failure path. Triggers only on allocation failure. | Whenever we build the Linux setuid ether helper. Trivial, take it with any Linux pass. |
| `533cf6fa` | Adjust reads to be kernel-relative | Rewrites interrupt-handler nanokernel reads to `KERNEL_DATA_BASE + offset` for **real addressing**. | **Lowest priority.** Inside `#if !EMULATED_PPC` *and* tied to `REAL_ADDRESSING` — i.e. native-PowerPC + real addressing. Our/rcarmo's Linux target is *emulated* PPC, so this likely never compiles there. Mainly for SheepShaver on actual PowerPC Linux. |

**Revisit-upstream note:** these are a snapshot. Before any Linux re-convergence, re-run the
§5.4 content-diff sweep against `kanjitalk755`, `cebix`, **and `rcarmo` (`upstream`)** — rcarmo
is the natural Linux ARM64 integration point and may have added or fixed Linux-platform code
since our last sync (we are currently 0 commits behind rcarmo; that will drift). Treat the
JIT as the shared core and keep it cleanly separable from the per-OS platform glue.

### 6.1 Linux testing strategy (Parallels + Ubuntu ARM64) and the addressing-mode gotcha

A **Parallels VM running aarch64 Ubuntu on Apple Silicon** is the most accessible Linux test
rig, and it is *not* merely a proxy: it is the real ISA (aarch64 — so JIT codegen correctness
transfers directly) and a real Linux userland. Standing it up turns the "no Linux host →
defer" rationale above into "testable now" for most of this backlog.

**The addressing-mode gotcha (decides what is even exercisable):** this fork selects the
addressing mode by host word size, not OS.
- BasiliskII default `ADDRESSING_TEST_ORDER="direct banks"` — `real` is never tried.
- SheepShaver (emulated PPC) computes a `NATMEM_OFFSET` (`0x400000000000`, a 48-bit address)
  and selects `direct,$NATMEM_OFFSET`. `REAL_ADDRESSING` is only forced for *native* PowerPC.
- `NATMEM_OFFSET` needs a 48-bit space → the `direct` path is fundamentally **64-bit-host**.

**Consequence:** *every* 64-bit host uses `DIRECT_ADDRESSING` — macOS arm64 **and** 64-bit
Linux aarch64 alike. `REAL_ADDRESSING` only appears on **32-bit ARM** (armv7/armhf) or
native-PPC. So a 64-bit Ubuntu build leaves all `#if REAL_ADDRESSING` code (e.g. the Wayland
fix `91d58b12`, and the deferred `533cf6fa`) **compiled out**, same as on macOS.
Empirical check on any build: `./configure` prints `addressing mode to use: …`; confirm with
`grep -E 'REAL_ADDRESSING|DIRECT_ADDRESSING' config.h`.

**What 64-bit Parallels Ubuntu ARM64 validates:**

| Target | Covered? |
|--------|----------|
| JIT codegen correctness (harness + boot on a 2nd platform) | ✅ same ARM64 ISA |
| VDE networking (`06d8bc02`) | ✅ Linux is VDE's native home |
| Deferred Linux glue: `750d3a82` (sheep_net `module_init`), `6787dce8` (`linux/sched.h`), `db897d0d` (etherhelper leak) | ✅ buildable/testable against Ubuntu's kernel |
| Linux `configure`/build paths | ✅ |
| Wayland fix (`91d58b12`) and `533cf6fa` (kernel-relative reads) | ❌ `REAL_ADDRESSING`-gated → compiled out on 64-bit |
| Performance / VBL-timer behavior | ⚠️ VM timing jitter — don't trust perf numbers; VBL-timer sensitivity (see CLAUDE.md) may differ under virtualization |
| macOS `MAP_JIT` / W^X path | ❌ macOS-only; Linux uses `mprotect` + `__builtin___clear_cache` — complementary, not a substitute |

**To validate the `REAL_ADDRESSING`-gated fixes** (Wayland, `533cf6fa`) you need a **32-bit
ARM Linux** target (32-bit Raspberry Pi OS — closest to rcarmo's original SBC target) or a
forced `--enable-addressing=real` build (which then also needs VOSF + working low-address
mapping, and may not function on aarch64). Note also that **Parallels' display integration may
not faithfully reproduce a bare-metal Wayland compositor**, so even a forced-real build there
is an imperfect repro of the specific Wayland mmap-collision — a real Pi is the honest rig.

**Net:** use Parallels Ubuntu ARM64 for the JIT, VDE, and the deferred Linux glue (most of the
value); use a 32-bit ARM / bare-metal Pi for anything `REAL_ADDRESSING`-gated. JIT inside the
VM runs natively (hardware virtualization; no nested translation), so functional/correctness
results are trustworthy even though timing is not.

---

## 7. Bottom line

This fork is in excellent sync with its upstreams. rcarmo's ARM64 JIT base *looks* ancient by
git merge-base (2014) but its **content** tracks kanjitalk755 to within the last few months,
and this fork is fully current with rcarmo. The honest finding is **not** "we're 1,214 commits
behind and need a big backport" — it is **"we're current; one optional networking feature and
one SDL3-migration item are the only upstream commits worth a second look."**
