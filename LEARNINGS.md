# LEARNINGS — macOS ARM64 JIT work

Running log of non-obvious things learned while working on this fork.
Newest entries at the top of each section. Review at the start of each session.

## 2026-06-01 — Project setup & landscape research

### Fork landscape
- **cebix/macemu** (original) is dormant; 916 commits behind kanjitalk755. Its only unique
  value: a few post-2020 commits. The SLiRP buffer-overflow fix (`d26ae37e`) was NOT in
  rcarmo/kanjitalk755 trees — cherry-picked here as `877782f3`. The AARCH64 Mach exception
  commit (`e5be177f`) was already present (better integrated) in rcarmo's tree.
- **kanjitalk755/macemu** is the community-standard fork but is "arm64 non-JIT" by design;
  it has zero MAP_JIT / `pthread_jit_write_protect_np` usage anywhere.
- **Jagmn's 2021 M1 ARM64 JIT** (emaculation forum) was never published; his GitHub is
  empty. Not recoverable. His approach notes: dyngen ops regenerated with GCC-10, RX↔RW
  cache toggling for W^X, NATMEM_OFFSET instead of zero-page.
- **rcarmo/macemu-jit** (our upstream) is Linux-ARM64-first: developed on Orange Pi 6 Plus /
  RPi, tested over VNC (port 5999), CI builds .deb packages. macOS is not its focus —
  that's our niche.

### Upstream JIT state (from JIT-STATUS.md, 2026-05-17)
- SheepShaver PPC→ARM64 JIT: hand-written emitters (no dyngen),
  `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/` (~4.1K lines; `ppc-jit.cpp` is the core).
- Status: 209/209 opcode vectors, 1800/1825 ROM blocks (98.6%), ~737 MIPS tight-loop.
  JIT boot reaches "Welcome to Mac OS" splash; interpreter boots to desktop.
- Known root cause of boot hang: partial-block truncation epilogue corrupts state
  (upstream commit `98fd798`). Lazy CR0 + register allocation are scaffolded but DISABLED
  as containment. Re-enabling them = the big optimization opportunity after correctness.
- Remaining ROM harness failures: CR field interactions in multi-instruction blocks,
  complex branch BO patterns (CTR+condition combos).
- The 68k (BasiliskII) JIT in `BasiliskII/src/uae_cpu_2026/compiler/` is much larger
  (~60K lines) and has the MAP_JIT / W^X handling — the SheepShaver side may not. Audit
  needed (Phase 2).
- Upstream's ASLR workaround uses Linux-only `personality(ADDR_NO_RANDOMIZE)` + re-exec —
  this does not exist on macOS; we need a different approach.

### Old (cebix/kanjitalk755) JIT for reference
- The legacy SheepShaver JIT is dyngen-based (QEMU-derived), x86/x86_64 only, allocates
  one permanently-RWX cache via `vm_acquire(VM_MAP_32BIT)` — architecturally incompatible
  with modern macOS W^X. rcarmo's reset to hand-written emitters was the right call.

### Environment
- Dev machine: arm64, macOS 26.4.1 (Darwin 25). Ken has Mac OS ROM + OS 9 install media.
- kanjitalk755's x86_64-JIT-under-Rosetta-2 build is the performance bar to beat
  (historically ~271% MacBench vs ~96% for native interpreter; Jagmn's lost JIT hit ~470%).

## 2026-06-01 — Phase 1 baseline

### Build environment
- macOS 26.4.1 arm64, Apple clang 17.0.0 (Command Line Tools, no full Xcode)
- Homebrew: autoconf, automake 1.18.1, libtool, pkgconf, sdl2 2.32.10, gmp, mpfr
