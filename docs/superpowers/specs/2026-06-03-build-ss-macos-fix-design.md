# Platform-Guard `build-ss` in SheepShaver/Makefile (Option A — minimal remediation)

**Date:** 2026-06-03
**Status:** Approved (Option A of A/B/C discussion); Option B is the eventual proper fix
**Component:** `SheepShaver/Makefile`

## Problem

`SheepShaver/Makefile` is upstream's (rcarmo) Linux dev harness. Its `build-ss` target:

1. Runs `make -j$(nproc)` — `nproc` doesn't exist on macOS
2. Hand-compiles `obj/ppc-jit.o` with minimal flags (missing the real include paths/defines)
3. Hand-links with GTK/X11 libraries that don't exist on macOS

On macOS the link step fails **and deletes the `SheepShaver` binary** (the linker removes
its partial output on failure). This bit us on 2026-06-03: `make test-opcodes` (which
depends on `build-ss`) destroyed the working binary mid-session.

Root cause of the upstream design: upstream's `configure` doesn't know about the AArch64
JIT, so the Makefile bolts it on after the autoconf build. On this fork's `macos-arm64`
branch, `configure.ac` was already fixed — `ppc-jit.cpp` is in `CPUSRCS`, so the inner
`src/Unix/Makefile` builds and links everything correctly by itself.

## Design (Option A — what this change does)

Platform-guard `build-ss` with `uname -s`:

- **Darwin**: delegate entirely to the autoconf build — `cd src/Unix && make -j$(sysctl -n hw.ncpu)`
- **Else (Linux)**: keep upstream's recipe byte-for-byte unchanged

Add a comment block explaining:
- why the split exists (upstream configure doesn't own the JIT build; this fork's does)
- that this is a minimal remediation
- what the proper fix (Option B) looks like, for an eventual upstream PR

## Option B (documented, not implemented)

Teach upstream's `configure.ac` to include `ppc-jit.cpp` in `CPUSRCS` on Linux ARM64 the
same way the macos-arm64 branch does. Then `build-ss` collapses to
`cd src/Unix && make -j<n>` on **both** platforms and the fragile hand-compile/hand-link
disappears entirely. This must be done in coordination with upstream (rcarmo) since only
they can test the Linux ARM64 path. It is the version of this fix that belongs in an
upstream contribution PR, riding along with the subfe/adde correctness fixes.

## Not Doing (deliberately)

- `run`/`run-jit`/`run-tmux`/`screenshot`/`ensure-xvfb` targets remain Linux-only
  (Xvfb, GDK, `/workspace` paths). The macOS run workflow is documented in CLAUDE.md
  (direct binary launch). Fixing those is Option C — rejected as unnecessary divergence
  from upstream.
- No change to `jit-test/run.sh` — it already handles macOS via the prebuilt-binary path.

## Verification

- `make -n build-ss` on macOS shows ONLY the inner-make delegation (no g++ hand-link,
  no GTK libs, no `nproc`)
- Full `make build-ss` + `make test-opcodes` end-to-end: deferred until the other
  agent's uncommitted WIP in ppc-jit.cpp/ppc-cpu.cpp is landed (building now would
  compile their half-finished code)

## Documentation Updates

- `LEARNINGS.md`: new entry — the build-ss trap, binary deletion failure mode, and the A/B fix shape
- `CLAUDE.md` (untracked, on-disk): SheepShaver build/test command notes
