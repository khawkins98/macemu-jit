> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone shipped
>

# SheepShaver ARM64 JIT for Apple Silicon + macOS 26 — Design

**Date:** 2026-06-01
**Author:** Ken Hawkins (khawkins98) with Claude
**Status:** Approved

## Goal

Make SheepShaver's PowerPC→ARM64 JIT excellent on Apple Silicon Macs running macOS 26+:
a native emulator that boots Mac OS 9 to the desktop with the JIT active, faster than the
x86_64-JIT-under-Rosetta-2 path, packaged as a signed, notarized macOS app.

Secondary goals: deep learning of JIT internals and macOS low-level APIs; share work
upstream (rcarmo/macemu-jit) if it proves out.

## Why this base

- **cebix/macemu**: dormant (~5 years), x86-only JIT. Reference only.
- **kanjitalk755/macemu**: actively maintained community standard, but arm64 = interpreter
  only ("macOS x86_64 JIT / arm64 non-JIT"), no MAP_JIT/W^X work.
- **rcarmo/macemu-jit** (chosen base): hand-written PPC→ARM64 JIT, 285+ opcodes, full FPU,
  AltiVec via NEON, 209/209 opcode tests, 98.6% ROM blocks, ~737 MIPS. But: Linux-ARM64-first
  (Orange Pi/RPi, VNC, `personality()` ASLR workaround), and JIT-mode boot stops at the
  "Welcome to Mac OS" splash (documented partial-block epilogue bug; lazy CR0/regalloc
  disabled as containment).

**Our niche is complementary, not duplicative: the macOS/Apple Silicon platform side, plus
JIT boot completion.**

## Repo strategy (done)

- Fork: https://github.com/khawkins98/macemu-jit ← rcarmo/macemu-jit
- Local: `~/Documents/git/macemu-jit`, working branch `macos-arm64`
- Remotes: `origin` (fork), `upstream` (rcarmo), `kanjitalk755`, `cebix`
- `master` stays clean tracking upstream; periodic `git merge upstream/master` into
  `macos-arm64` to avoid drift
- Cherry-picked from cebix: SLiRP buffer-overflow fix (`877782f3`). cebix's AARCH64 Mach
  exception support was already present in rcarmo's tree.

## Phased plan

### Phase 1 — Baseline on macOS 26 (build + measure)

1. Build SheepShaver on macOS 26 arm64: `cd SheepShaver/src/Unix && ./autogen.sh &&
   ./configure --enable-sdl-video --enable-sdl-audio --enable-jit && make`
2. Expect and fix macOS build breakage (upstream develops on Linux) — first contribution
3. Run upstream test harnesses locally: `SheepShaver/jit-test/run.sh` (209 opcode vectors),
   `SheepShaver/rom-harness/` against Ken's ROM
4. Boot interpreter mode with Ken's Mac OS ROM + Mac OS 9 media → desktop
5. Boot JIT mode; document exactly where it stops on macOS
6. Benchmark: native interpreter vs native JIT (as far as it goes) vs kanjitalk755
   x86_64-JIT-under-Rosetta (the number to beat). Use MacBench/Speedometer in-guest.

**Exit criteria:** builds reproducibly; harness results recorded; baseline benchmark table
in LEARNINGS.md.

**Status (2026-06-01): Substantially complete.** Builds reproducibly (3 portability fixes);
harnesses run (opcode 0/209, ROM harness EPERM — both blocked by the W^X issue, precisely
diagnosed); interpreter runtime confirmed working (boots into Mac OS SCSI scan; CD-open bug
found and fixed → boot now proceeds); JIT-mode failure point documented (code cache RWX mmap
EPERM). Remaining: in-guest benchmark vs Rosetta (needs Ken at the keyboard) and visual
boot-to-desktop confirmation (needs Screen Recording permission). See LEARNINGS.md
"Phase 1 baseline" section.

### Phase 2 — macOS-specific JIT correctness

1. Audit `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/` + `jit-cache.cpp` code-cache path
   for macOS W^X compliance: `mmap(MAP_JIT)`, `pthread_jit_write_protect_np()` bracketing
   all code writes, `sys_icache_invalidate()` after emission. (Upstream's W^X handling
   exists in the BasiliskII 68k compiler; verify/port for the SheepShaver PPC side.)
2. Memory layout: replace upstream's Linux-only `personality()` ASLR workaround with a
   macOS-appropriate approach for SheepShaver's address-space assumptions.
3. Verify Mach exception / SIGSEGV handling interoperates with JIT-generated code on arm64.

**Exit criteria:** JIT runs under default macOS security posture (no boot-args, no SIP
changes); harness still 209/209.

**Status (2026-06-01): Designed; implementation plan written** —
`docs/superpowers/plans/2026-06-01-phase2-wx-and-addressing.md`. Research findings (in
LEARNINGS.md "Phase 2 design research"): __PAGEZERO cannot be shrunk on arm64, so the plan
is DIRECT_ADDRESSING (already working for the interpreter) + MAP_JIT for the code cache +
base-register conversion of the JIT's load/store codegen. Item 2's "macOS-appropriate
approach" is therefore DIRECT_ADDRESSING, not a low-memory workaround.

### Phase 3 — JIT boot completion (the centerpiece)

1. Reproduce and root-cause the splash→desktop hang (upstream diagnosis: partial-block
   truncation epilogue corrupts state; see upstream JIT-STATUS.md and docs/planning/JIT-APPROACH-RESET.md)
2. Fix the 25 failing ROM-harness blocks (CR field interactions in multi-instruction
   blocks; complex branch BO patterns)
3. Re-enable lazy CR0 and register allocation behind correctness proofs, using the opcode +
   ROM harnesses as gates

**Exit criteria:** Mac OS 9 boots to desktop with JIT active on this Mac; ROM harness
≥ upstream's 98.6%; measurable speedup over interpreter recorded.

### Phase 4 — macOS 26 app polish

1. Proper .app bundle; code signing with `com.apple.security.cs.allow-jit` entitlement;
   notarization
2. SDL3 (or current SDL2 path), Retina/HiDPI, modern macOS niceties
3. Decide arm64-only vs universal binary

**Exit criteria:** a downloadable, signed app a non-developer could run.

### Phase 5 (optional) — Share

PRs upstream to rcarmo/macemu-jit for the pieces that fit his project; publish the fork
and/or builds for the community (emaculation.com).

## Working practices

- **Subagents:** delegate exploration, research, build-log debugging, and parallelizable
  work to subagents; keep the main session focused on decisions and verification.
- **LEARNINGS.md** at the repo root (matches upstream's root-doc convention): append
  dated entries for everything non-obvious discovered — macOS quirks, JIT internals, root
  causes, dead ends. Review at the start of each session.
- **Document and iterate:** update this design doc and a STATUS section as phases
  complete; treat docs as living artifacts. Each phase produces at least one doc update.
- **Regression discipline:** opcode harness + ROM harness must pass before any commit
  touching the JIT; boot test before any push.
- **Commits:** small, root-cause-explaining messages (follow upstream's style); preserve
  upstream authorship on cherry-picks.

## Risks

| Risk | Mitigation |
|---|---|
| Upstream moves fast / diverges | Regular merges from `upstream/master`; keep changes modular |
| macOS build badly broken (Linux-first upstream) | Phase 1 surfaces this first; fixes are themselves useful contributions |
| Partial-block epilogue bug runs deep | Upstream harnesses give a tight repro loop; worst case, keep containment (no lazy opts) and still ship correctness |
| W^X changes slow the JIT (write-protect toggling) | Batch code emission; measure in Phase 2 benchmarks |
| Mac OS 9 needs SheepShaver features beyond the JIT (sound, networking) | Out of scope until Phase 4; interpreter parity is the bar |

## Out of scope (for now)

- BasiliskII (68k) work — SheepShaver first
- Windows/Linux platform work — upstream covers Linux
- Upstreaming to kanjitalk755 — revisit after Phase 4
