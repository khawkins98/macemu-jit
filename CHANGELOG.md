# macemu-jit (macOS arm64) Changelog

Changes specific to the `macos-arm64` branch — this fork of kanjitalk755/macemu, which
adds AArch64 JIT backends. Covers **both emulators** plus shared/build/docs work.

Entries are tagged by component: **[SheepShaver]**, **[BasiliskII]**, **[shared]** (code
used by both, e.g. `ether_unix.cpp`, prefs), **[build]**, **[docs]**. Entries before
2026-06-04 predate this fork-wide reorganization and are SheepShaver-scoped unless noted
(BasiliskII history lives in `BasiliskII/docs/AARCH64_JIT_BRINGUP.md` and
`BasiliskII/docs/MACOS-AARCH64-JIT-PORT.md`).

## 2026-06-04

### [SheepShaver] Wayland detection (upstream backport)

- **Wayland detection without GTK** (backport of kanjitalk755/macemu `91d58b12`, Dave
  Vasilevsky): `init_sdl()` previously forced `SDL_VIDEODRIVER=x11` only under
  `#if REAL_ADDRESSING && defined(GDK_WINDOWING_WAYLAND)`, so a `--without-gtk`
  SDL build never got the XWayland workaround that avoids a Wayland mmap/fixed-
  low-address-mapping crash. The guard is now `#if REAL_ADDRESSING &&
  defined(__linux__)` plus a runtime `getenv("WAYLAND_DISPLAY")` check, so the
  workaround applies in non-GTK SDL builds. **Inert on macOS**: the block is
  gated on `defined(__linux__)`, which is never defined on Darwin (only
  `__APPLE__`/`__MACH__`), so it compiles out entirely on the macOS arm64 build
  (`REAL_ADDRESSING` is not defined here regardless — see `docs/UPSTREAM-LINEAGE-SYNC.md`
  §6.1). Brought in for a future
  Linux/Wayland target; runtime Wayland behavior is not verifiable on macOS.
  Harness unaffected: `make test-jit` 257/257, score=100.

### [shared] Networking

- **VDE virtual networking** (backport of kanjitalk755/macemu `06d8bc02`): SheepShaver can now use
  a VDE switch for Ethernet. The destination VDE link is configured directly in the
  `ether` pref via a new `vde:` prefix (e.g.
  `--ether 'vde:cmd://ssh root@server vde_plug tap://tap0'`), so it persists with the
  rest of the prefs. Two correctness fixes in the shared `ether_unix.cpp` send path:
  outgoing packets now send the actual frame length (was `sizeof(packet)`, which
  appended trailing garbage), and the infinite `do {} while (len < 0)` send-retry was
  replaced with a proper `excessCollsns` error return. SheepShaver's `configure` gains
  `--with-vdeplug` (default yes) and an `AC_CHECK_LIB(vdeplug, vde_close)` probe that
  defines `HAVE_LIBVDEPLUG` and links `-lvdeplug` when the library is present (Homebrew
  `vde`, header `libvdeplug.h`). The bare `vde` ether pref (no destination) still works.
  Boot/packet-flow on real hardware is unverified by this change.

### [SheepShaver] Video backend

- **SDL3 is now the default video backend** (was SDL2). `configure` selects SDL 3.x
  when no `--with-sdlN` flag is given; pass `--with-sdl2` to opt back to SDL 2.x.
  Requires the `sdl3` pkg-config module (Homebrew `sdl3`, tested with 3.4.10). The
  SheepShaver binary now links `libSDL3.0.dylib`.
- Picked up kanjitalk755/macemu `e596e215` ("SDL3: blit not required in `SDL_UnlockTexture()`"):
  the SDL3 texture is unified to `ARGB8888` and the big-endian→host swap is done in
  software (`__builtin_bswap32`) inside the `SDL_LockTexture`/`UnlockTexture` copy,
  removing the `SDL_GetMasksForPixelFormat` round-trip.
- macOS SDL3 build fixes (this backend had never been compiled on the fork before):
  `video_sdl3.cpp` used `dynamic_cast` (needs RTTI, but the build uses `-fno-rtti`) →
  changed to `static_cast` to match `video_sdl2.cpp`; the three macOS Objective-C++
  files (`prefs_macosx.mm`, `VMSettingsController.mm`, shared `utils_macosx.mm`) used
  a raw `#include <SDL.h>` that does not resolve under SDL3's `sdl3/SDL.h` layout →
  switched to the version-aware `my_sdl.h` shim.

> **Status:** SDL3 is **boot-verified** — Mac OS 8.6 boots to the Finder desktop on the
> SDL3-default build (2026-06-04). The JIT harness validates codegen, not video, so this
> confirmation is the boot test, not the harness. If you hit display problems on a future
> build, `--with-sdl2` falls back to the SDL2 backend.

### [BasiliskII] Build

- **macOS arm64 build (partial; does not yet build).** The configure host-routing was
  fixed (Apple Silicon `arm-apple-darwin` → AArch64, not ARM32; `c71100d0`) and Linux-only
  code in `main_unix.cpp` guarded (`2f967f4b`), but the B2 AArch64 JIT backend
  (`compemu_support_arm.cpp`) is still unported — **BasiliskII does not yet build on macOS
  arm64.** Full detail, remaining errors, and the pick-up plan are in
  **`BasiliskII/docs/MACOS-AARCH64-JIT-PORT.md`**.

### [SheepShaver] JIT correctness

- **AltiVec `vsel` fix**: `vsel` (vector select) emitted ARM64 `BSL` with its two
  source operands swapped — it computed `(vC & vA) | (vB & ~vC)` instead of PPC's
  `(vB & vC) | (vA & ~vC)`, returning vA wherever the mask bit was set. This would
  silently corrupt any AltiVec software that uses `vsel` (the emulator advertises a
  G4, so AltiVec is live). One-token operand swap; caught and regression-tested by
  a new differential vector.

- **`emit_update_cr0` cleanup (B1)**: CR0 field construction reduced from 19 to
  11 ARM64 instructions.  Replaced 3x `emit_load_imm32` + 3x CSEL + LSL + AND +
  ORR with 3x CSET + 3x shifted ADD + BFI.  Every Rc=1 instruction benefits.

- **LogicalImm encoder (B2)**: ARM64 bitmask-immediate encoding for AND masks in
  rlwinm, rlwimi, rlwnm, andi., andis.  Saves 1-2 instructions per masked op
  (992/1024 PPC masks are encodable).  Encoder from `ppc-logical-imm.hpp`.

- **mullwo overflow detection (A3)**: Case label was 715 (wrong XO), should be
  747.  The instruction was never JIT-compiled — silently fell to interpreter.
  Now uses SMULL + ASR/CMP to detect 32-bit overflow and sets XER OV/SO.

- **SS_JIT_VERIFY cascade fix**: Reduced false divergences from 20+ to 1 per
  boot.  Skips verifying blocks ending with link-setting branches (bl/bctrl),
  and suppresses cascade after any divergence until a clean block is found.
  Mixed Mode Manager dispatch causes unavoidable interpreter/JIT path divergence
  that is not a codegen bug.

### [SheepShaver] Testing & benchmarking

- **18 real FP-arithmetic test vectors**: the pre-existing `fp_*` vectors were
  *vacuous* — they ended at `stfd` and never loaded the result into a GPR, but the
  harness REGDUMP captures GPRs, not FPRs, so a wrong FP result was invisible and
  the JIT-vs-interpreter diff passed trivially. FP arithmetic was effectively
  untested. The new vectors load the result back into a GPR (fadd/fsub/fmul/fdiv,
  the fma family, frsp/fctiwz/fneg/fabs/fmr, and single-precision forms). Generated
  by `jit-test/gen-fp-vectors.py` (documented, reproducible).

- **AltiVec coverage (corrected in review)**: an initial 15-vector AltiVec batch
  was added, but adversarial review found 14 were vacuous — VX-form ops carried a
  doubled XO field, decoding to no-ops, so their results never reached the checked
  GPRs. Those were removed; the one correctly-encoded vector (`vsel`) is kept. A
  correctly-encoded `vspltb` probe exposed a *separate* hidden interp-vs-JIT
  divergence, flagged for follow-up. A proper VX-form AltiVec batch is pending.

- **`make harness-count`**: single source of truth for the harness vector count,
  derived from `jit-test/run.sh` (the count had drifted across several docs). The
  *gate* references in the testing docs (TESTING.md, OPTIMIZATION-PLAN.md,
  CLAUDE.md, CONTRIBUTING.md) were de-hardcoded to reference it; dated historical
  snapshots in session logs and baseline tables are intentionally left as-is.

- **FP microbench kernels**: `make bench` gains `fp-add`/`fp-fma`. They measure the
  FPR store/load round-trip (the JIT has no FP register allocator), not raw FP-unit
  latency — useful as the baseline an FP register allocator would improve against.

### [docs] Documentation

- **Paranoia FP conformance**: concrete manual run steps documented in TESTING.md,
  with the honest caveat that automation needs a guest binary + boot.

- **IMPROVEMENT-CYCLE-1.md**: prioritized, collision-aware improvement plan from a
  multi-agent audit (read-only auditors → adversarial verification → synthesis).

## 2026-06-03

### Emulator Features

- **Clean shutdown**: Special > Shut Down now exits the host process cleanly,
  running all cleanup handlers (JIT miss report, cache free, SDL teardown).
  Previously the process hung with a black screen after the guest powered off.

- **Restart**: Deferred — requires deeper ROM reset state management. Special >
  Restart still has no effect (same as upstream).

- **Prefs: K/M/G suffixes**: Integer prefs now accept human-readable sizes
  (e.g., `ramsize 256M`). Also supports `0x` hex prefix via `strtol` base 0.
  Shared by both BasiliskII and SheepShaver (symlink).

- **Prefs: comments**: `#` and `;` line comments were already supported by the
  parser but undocumented. Now noted in CLAUDE.md and prefs examples.

- **Networking**: `ether slirp` provides outbound NAT networking (web, FTP)
  with built-in DHCP. No host configuration required.

- **JIT code cache sizing**: Default increased from 64 MB to 256 MB, eliminating
  recompilation churn during boot and app launch (previously 2+ full flushes per
  session).  Configurable via `jitcachesize` pref or `SS_JIT_CACHE_KB` env var.

- **Startup prefs readout**: SheepShaver now prints each loaded pref value on
  startup, showing what configuration is active.

### JIT Performance

- **Register allocator (RA)**: Re-enabled and fully converted. All 32-bit GPR
  accesses go through `ra_load`/`ra_store`, operating directly on ARM64
  x21-x28 registers — no MOV bounce through temporaries. Benchmark Mix
  improved **+15.6%** (548 → 634), Dhrystones **+9.4%**, CPU **+2.2%**.

- **TBZ bclr optimization**: Mixed Mode guard on function returns uses single
  `TBZ` instruction instead of `AND` + `CBZ` (1 instruction saved per `bclr`).

- **Atomic spcflags (P0d)**: Replaced spinlock-based spcflags with
  `std::atomic`, eliminating lock contention between the 60 Hz VBL timer
  and the JIT dispatch loop. CPU score 65.2 (new high).

- **Trailing MOV elimination (P0f)**: Carry, overflow, and immediate-carry ops
  (subfc, addc, addco, subfco, addo, subfo, nego, addze, subfze, addic, addic.,
  subfic) now compute ADDS/SUBS directly into the RA destination register,
  eliminating a redundant MOV per instruction.  Mix 638 (new high).

- **RA eviction test**: New `lmw_stmw_wide` harness vector exercises 12 live
  GPRs, forcing mid-block RA eviction. Harness now 236/236.

- **JIT miss report via atexit**: The opcode coverage histogram now prints
  reliably on process exit (via `atexit` hook), regardless of how the guest
  shuts down. Includes session wall-clock time.

- **JIT coverage**: 98.4% of compiled instructions run natively. The remaining
  1.6% (opc=19: bcctr/isync) accounts for 98.5% of all misses — making native
  bcctr the single highest-impact remaining optimization.

### JIT Correctness

- **RA flush on bclr bail path**: Added `ra_flush_all()` before the Mixed Mode
  bail epilogue — prevents dirty cached GPR values from being lost when bclr
  bails to the interpreter mid-block.

- **RA ordering constraint**: `ra_load` for source operands must precede
  `ra_store` for the destination. Otherwise `ra_store` allocates without
  loading the old value, and a subsequent `ra_load` for the same GPR (when
  rD == rA) returns uninitialized data.

- **gpr64 coherence warning**: Documented that `emit_load_gpr64` /
  `emit_store_gpr64` bypass the RA cache for the low word. Safe today (PPC64
  ops unreachable from 32-bit guests), must be fixed before G5 support.

### Testing & Benchmarking

- **`make test-jit`**: New target that runs the harness in JIT equivalence mode
  (`SS_HARNESS_MODE=jit`), actually testing JIT codegen against the interpreter.
  The old `make test-opcodes` only tested interpreter determinism.

- **`make bench` (jit-bench)**: Microbenchmark for fast A/B testing of codegen
  changes — reports ns/insn for targeted kernels (carry-chain, rc1/CR0, ALU).
  Supports `--save-baseline` / `--compare` for differential timing. No boot needed.

- **TESTING.md maintenance contract**: Freshness rules, per-change checklist,
  harness mode awareness. Prevents test/doc rot.

### Debug Output

- **Disk driver**: Suppressed per-poll DiskStatus flood for csDriverGestaltCode
  (status 43). Replaced two-line output with single-line format showing the
  four-char gestalt selector: `csDriverGestaltCode 43: 'flus'`.

### Documentation

- **OPTIMIZATION-PLAN.md**: Updated with completed items (RA, TBZ), new
  entries (lazy CR0, trailing-MOV elimination, cross-block register pinning,
  indirect bclr chaining), post-RA benchmark baseline, and miss-data-driven
  reprioritization of P2 (native bcctr).

- **USER-HANDBOOK.md**: New user guide covering prefs reference, networking
  setup, environment variables, benchmarking, and troubleshooting.

- **CHANGELOG.md**: This file — tracks user-visible changes per session.
