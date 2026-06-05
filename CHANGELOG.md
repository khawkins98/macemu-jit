# macemu-jit (macOS arm64) Changelog

Changes specific to the `macos-arm64` branch — this fork of kanjitalk755/macemu, which
adds AArch64 JIT backends. Covers **both emulators** plus shared/build/docs work.

Entries are tagged by component: **[SheepShaver]**, **[BasiliskII]**, **[shared]** (code
used by both, e.g. `ether_unix.cpp`, prefs), **[build]**, **[docs]**. Entries before
2026-06-04 predate this fork-wide reorganization and are SheepShaver-scoped unless noted
(BasiliskII history lives in `BasiliskII/docs/AARCH64_JIT_BRINGUP.md` and
`docs/planning/BasiliskII-MACOS-AARCH64-JIT-PORT.md`).

## 2026-06-05

### [docs] Silicon Sheep plan — Tier 4 Automation & Scripting

- Added a **Tier 4 — Automation & Scripting** section to `docs/planning/DESKTOP_INTEGRATION_PLAN.md`:
  a launcher control surface (`siliconsheep` CLI / AppleScript / Shortcuts / MCP server / headless
  CI mode), a screenshot-free guest control bridge (structured input + observation + an AppleEvents
  bridge), and a "Silicon Sheep Tools" guest agent for *managed* images that reframes part of the
  Infeasible list. Grounded in prior art (UTM scripting, Lume's HTTP+MCP control, Tart, VirtualBuddy)
  with sources inline for future agents. Feeds the E2E harness (ROADMAP A5 / A5-V).
- **Built out with a per-layer integration design** (parallel codebase investigation): `file:line`
  anchors, RPC method additions, phased build orders + effort/risk per layer, and a "Shared
  architecture" synthesis (one wire protocol, one broker, one idle-hook command mailbox on the emul
  thread; the three execution contexts). Surfaced a premise correction — the bidirectional launcher
  RPC does **not** exist yet (emulator is an outbound-only RPC client; Tauri launcher stops VMs via
  SIGUSR1), so making that channel bidirectional is Layer A's true first task.

### [SheepShaver] E2E harness — host→guest shutdown hook + ISO medium + Speedometer benchmark (A5)

- **Host→guest shutdown hook.** `SIGUSR1` → the emulator's idle hook injects the ADB Power key
  (with dwell) → waits for the Shut Down dialog → Return (confirms) → real OS shutdown → clean
  exit. Replaces VNC menu-clicking; verified on Mac OS 8.6 and 9.0.4. (`emul_op.cpp`,
  `runner.request_shutdown()`.)
- **Read-only ISO is the default medium** — can't get dirty (no repair-prompt/dialog
  false-positives, no pristine-copy), reproducible. Disk-boot retained (`SS_E2E_MEDIUM=disk`,
  instant APFS clonefile copy) for the benchmark.
- **`make e2e-bench` — Speedometer benchmark automation.** Boots a small stripped Mac OS 9.0.4 +
  Speedometer disk, drives the full suite over VNC (gated on a new `[APP] frontApp` change signal
  so it doesn't race the variable app launch), captures the results image (PR/CPU) + emulator log,
  and shuts down via the hook. Verified PASS (PR 29.375).
- **Benchmark-finished hook (no fixed sleep).** The idle hook's `[APP]` signal now also fires on a
  front-window **modal change**, so Speedometer's "tests are done!" dialog (modal 0→1) is a
  deterministic finish signal. `run_benchmark` waits for that dialog instead of a fixed 105 s sleep
  and reports the measured suite duration (a coarse perf signal). App-change `[APP]` emits are
  debounced to ~0.5 s (CurApName churns ~6/s among background extensions); modal changes are always
  emitted. (`emul_op.cpp`, `observe.is_app_dialog()`, `scenario.run_benchmark`.) Score *parsing*
  (Speedometer text export) is the next step.
- **Fixes found building it:** `Vnc.close()` calls `api.shutdown()` (vncdotool's reactor otherwise
  hung every capture ~2 min); host-FS (`extfs`) mount disabled in the test prefs; verbose `pytest`.
- Run guide: `SheepShaver/e2e/README.md`; design: `docs/superpowers/specs/2026-06-04-e2e-vnc-harness-design.md`
  (§12–§15).

### [SheepShaver] Live JIT stats in SDL window title bar

- Window title now shows JIT block count and code cache usage, updated every ~2s:
  `SheepShaver — JIT: 847 blocks, cache 1234K/262144K (0%)`. New
  `ppc_jit_aarch64_get_stats()` API exposes block count, pool size, cache used/total.
  Applied to both SDL2 and SDL3 video backends. Guarded by `USE_AARCH64_JIT`.
  Foundation for a future SDL overlay status bar (option 3 in the plan).

### [docs] Silicon Sheep — Tauri v2 launcher scaffolded + plan expanded

- **Framework pivot:** Desktop integration plan (`DESKTOP_INTEGRATION_PLAN.md`) revised from
  Cocoa/ObjC to **Tauri v2** (Rust + pnpm + TypeScript). Rationale: cross-platform door stays
  open, CLI-buildable (no Xcode.app needed), modern web UI for free.
- **Tauri scaffold:** `SiliconSheep/` at repo root — Rust backend with VM profile CRUD
  (`vm.rs`), Tauri command registrations (`main.rs`), web frontend with VM card grid UI
  (`main.ts`, `styles.css`), dark/light mode. Both `cargo check` and `vite build` pass.
- **Feature roadmap expanded** with competitive teardown (Parallels, VMware, UTM, Infinite Mac,
  DOSBox-X), detailed UX flows (4-screen first-run wizard, VM library cards, settings sidebar,
  error states, coach marks), Tauri sidecar architecture (IPC via existing `rpc_unix.cpp` UDS),
  APFS `clonefile` for instant VM duplication, and an explicit "Infeasible" tier.
- **Host-Guest Channels reference** (`HOST-GUEST-CHANNELS.md`): all existing host↔guest
  interaction channels, readable guest OS structures (WindowList, CurApName, MenuList,
  ScrnBase), achievable new channels, and hard limits.
- **ROADMAP Track C** updated from "⏸ Researched" to "🟡 Active — scaffolded".

## 2026-06-04

### [docs] Doc hygiene sweep — stale-claim fixes, /workspace paths, archive

- **Corrected misleading status claims.** SheepShaver `AARCH64_JIT_GOLDEN_WORKLOADS.md`
  Workload 3 said the JIT only "reaches the Mac OS Welcome splash" — updated to the verified
  reality (boots Mac OS 8.6 to Finder with the full native JIT, 2026-06-04), plus the
  Speedometer/blocker rows. Added Linux-lineage caveats to the BasiliskII
  `AARCH64_JIT_GOLDEN_WORKLOADS.md` and `AARCH64_JIT_BRINGUP.md` (their `301/301` figures are
  Linux history; B2 does not build on macOS arm64).
- **Fixed stale `/workspace/projects/macemu` paths** in both GOLDEN_WORKLOADS docs → `<repo>`.
- **Archived three one-off artifacts** into `docs/archive/` (with an index):
  `0x50467E00-CODEGEN-BUG-ANALYSIS.md`, `PR-SUBFE-FIX-DRAFT.md`, `JIT-OPCODE-TABLE.md` — their
  conclusions live in LEARNINGS/CHANGELOG / `make harness-count`.
- Added `docs/superpowers/README.md` namespace index; fixed OPTIMIZATION-PLAN reference links
  to point at local copies (`PERFORMANCE_AUDIT.md`, `JIT-FPU-PLAN.md`).

### [docs] Documentation lifecycle convention + status/provenance headers

- **New `CONTRIBUTING.md` → "Documentation Lifecycle" section**: how to keep docs honest when
  work lands — log to `CHANGELOG.md` (component-tagged), flip the item's status marker instead
  of deleting it, bump the doc's `Updated:` date, and fold-and-retire (not silently delete)
  dated specs.
- **Standardized header on all 43 docs under `docs/planning/`**: a `Status / Created / Updated /
  Why this doc exists` block plus the canonical marker legend (✅ done · 🟡 in progress ·
  ⏸ blocked/deferred · ☐ todo). Created dates recovered from git history (following renames);
  existing rich intros (ROADMAP, NEW-WORLD plan) harmonized, not clobbered.

### [SheepShaver] AltiVec even/odd byte multiplies fix — ev_mixed class complete (vmuloub/vmuleub)

- **Bug fix.** `vmuloub`/`vmuleub` (odd/even unsigned byte multiply → halfword products) had two
  bugs: they emitted a **non-widening `MUL.8B`** (must widen 8×8→16) and ignored the `ev_mixed`
  even/odd element selection. Fix (`emit_vmul_byte`): `REV32.16B` normalize → `UZP1`(even)/
  `UZP2`(odd)`.16B` select into the low 8 lanes → `UMULL.8H` widen → `REV32.8H` to ev_mixed
  halfword storage. Test vectors strengthened to distinct operands (vA=00..0F, vB=10..1F), which
  exercise **both** bugs at once — even-lane products like 0x0A×0x1A=260 exceed 255, so a
  non-widening op truncates visibly. `xfail→xpass`, promoted (**262→264, score=100**). **This
  empties the AltiVec ev_mixed quarantine lane** — the whole element-order class (splats, merges,
  pack, byte multiplies) is now fixed and scored.
- **Also corrected the scrambled multiply XO→op comment labels** (e.g. case 520 was labelled
  `vmulesb` but is `vmuleub`). Remaining, untested (no test vector, flagged in code + ROADMAP A2):
  halfword multiplies `vmul*h`, word pack `vpkuwum`, signed byte multiplies `vmulosb`/`vmulesb`
  (the latter share `emit_vmul_byte` with `SMULL` — emitted as *prospective*).
- Boot-verified (`SS_JIT_VERIFY`, Mac OS 8.6 clean exit): zero VR/FPR divergence for the pack +
  multiplies (only the documented `blr`-boundary GPR false positive). The whole ev_mixed class is
  now boot-confirmed.

### [SheepShaver] AltiVec vpkuhum pack fix (ev_mixed)

- **Bug fix.** `vpkuhum` (pack 8+8 halfwords to their low bytes, modulo) was doubly wrong: it
  **ignored vA entirely** (loaded only vB) and used the wrong NEON op. PPC keeps each halfword's
  low byte = PPC byte 2i+1 = the odd byte lane in natural order, so on `REV32.16B`-normalized
  inputs that is **`UZP2.16B`** (odd-lane deinterleave; vA → result high half). Reuses the merge
  family's `emit_vmrg` helper (`REV32.16B` → permute → `REV32.16B`). Verified `xfail→xpass` with
  distinct operands, promoted to the scored gate (**261→262, score=100**). Sibling `vpkuwum`
  (word→halfword modulo pack, case 78) has the same ignore-vA bug — flagged in code + ROADMAP A2,
  not yet vectored. Remaining in the `ev_mixed` class: the even/odd byte multiplies
  (`vmuleub`/`vmuloub`).

### [SheepShaver] AltiVec byte/halfword merge fix (vmrgh/l b,h) + distinct-operand test strengthening

- **Bug fix — completes the merge family.** Following the word-merge fix below, the byte and
  halfword merges (`vmrghb`/`vmrglb`/`vmrghh`/`vmrglh`) needed more than a correct ZIP
  encoding: the VR is stored `ev_mixed` (bytes reversed *within* each 32-bit word), which at
  the byte level is exactly `REV32.16B` relative to natural PPC element order. New
  `emit_vmrg()` helper normalizes both inputs with `REV32.16B`, merges with
  `ZIP1`/`ZIP2.{16B,8H}` (PPC element 0 = MSB = NEON's lowest lane after the rev, so PPC "high"
  merge = ZIP1 / "low" merge = ZIP2), then `REV32.16B` back before the store. Word merges keep
  the plain `ZIP.4S` — `word_element` is the identity under `ev_mixed`, so they need no rev.
- **Test strengthening (closes a masking gap).** All six merge vectors moved from self-operand
  (`v1,v1`) to **distinct operands** (vA=`00..0F`, vB=`10..1F`), so a wrong ZIP1↔ZIP2 or A↔B
  swap can no longer pass coincidentally; the full 128-bit result is diffed via the REGDUMP VR
  line. The four byte/halfword merges flipped `xfail→xpass` and were promoted to the scored
  gate (**257→261, score=100**); the word merges stayed green under distinct operands —
  retroactively proving that fix was not coincidental.
- **Boot-verified** under `SS_JIT_VERIFY` (Mac OS 8.6, 235K blocks, 98.7% JIT coverage): **zero
  VR/FPR divergence**. The single GPR/LR/PC divergence is the documented `blr`-block-boundary
  false positive (LEARNINGS, "SS_JIT_VERIFY false positives"), not a codegen bug.
- **Remaining in the `ev_mixed` class:** `vpkuhum` (halfword→byte pack) and the even/odd byte
  multiplies `vmuleub`/`vmuloub` (also need `UMULL.8H`, not `MUL.8B`) — still quarantined
  (xfail). See `docs/planning/ROADMAP.md` A2.

### [SheepShaver] AltiVec word-merge codegen fix (vmrghw/vmrglw)

- **Bug fix.** The JIT's AltiVec merge cases (`vmrgh*/vmrgl*`) emitted `0x..C400`/
  `0x..C800` — bit 15 set makes those three-same *arithmetic* encodings, not the
  ZIP1/ZIP2 permutes the inline comments claimed. Merges silently produced wrong
  results. Corrected all six cases to the real ZIP1/ZIP2 `{16B,8H,4S}` encodings.
  The word-granular `vmrghw`/`vmrglw` are now fully correct (word order is preserved
  under the interpreter's `ev_mixed` VR layout, so `ZIP.4S` needs no byte remap):
  verified by the differential harness (xfail→xpass) and an `SS_JIT_VERIFY` boot
  (zero VR divergence), then promoted from the quarantine lane to the scored gate
  (255→257, score=100). The byte/halfword merges, packs, and even/odd multiplies
  remain quarantined (`ev_mixed` byte-within-word reordering — see `ROADMAP` A2 and
  the in-code note above `case 12` in `ppc-jit.cpp`). The harness gained a
  quarantine lane (xfail/xpass, not scored) so known-diverging vectors are tracked
  as regressions-in-waiting rather than silently dropped.

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
  **`docs/planning/BasiliskII-MACOS-AARCH64-JIT-PORT.md`**.

### [SheepShaver] JIT correctness

- **AltiVec `vsel` fix**: `vsel` (vector select) emitted ARM64 `BSL` with its two
  source operands swapped — it computed `(vC & vA) | (vB & ~vC)` instead of PPC's
  `(vB & vC) | (vA & ~vC)`, returning vA wherever the mask bit was set. This would
  silently corrupt any AltiVec software that uses `vsel` (the emulator advertises a
  G4, so AltiVec is live). One-token operand swap; caught and regression-tested by
  a new differential vector.

- **AltiVec `vspltb`/`vsplth` element-order fix (ev_mixed)**: byte/halfword splats
  selected the WRONG element. VRs are stored in the interpreter's `ev_mixed` byte
  order (`byte_element(i)=(i&~3)+(3-(i&3))` — bytes reversed *within* each 32-bit
  word, word order preserved), but `emit_load_vr` loads it raw via `LDR Q`, so the
  JIT indexed NEON lanes with the raw PPC element. Fixed by remapping the DUP index
  through `ev_mixed` (verified across indices 0/3/15 and 0/3/7); `vspltw`/`vsldoi`/
  element-symmetric ops were already correct. The same mismatch still affects
  `vmrg*`/`vpk*`/even-odd multiplies — parked and signposted in code (`emit_load_vr`)
  and tracked as **P1b** in OPTIMIZATION-PLAN.md (two fix approaches documented).
  Repro vectors in `jit-test/gen-altivec-vectors.py`.

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

- **Software link stack (R1, partial)**: Compile-time infrastructure for
  Dolphin/RPCS3-style blr return prediction added.  Finding: `bl` always
  terminates a block, so the compile-time stack is empty by the callee's `blr`
  — the fast-path never fires.  Runtime variant (R1b) documented in the
  optimization plan as a follow-up.

### Code Quality

- **Technique attribution**: Added source credits (Dolphin, RPCS3, MAME,
  upstream PERFORMANCE_AUDIT) to all major JIT optimizations as inline code
  comments — RA, CR0 cleanup, LogicalImm, ADCS carry, mullwo, atomic spcflags,
  link stack.

### [SheepShaver] Testing & benchmarking

- **End-to-end VNC test harness (P1, ROADMAP A5)**: `make e2e` boots an isolated copy of
  Mac OS 8.6 in a checked-in config (not `~/.sheepshaver_prefs`), waits for a deterministic
  boot-ready signal, drives the Finder's Special ▸ Shut Down over VNC, and asserts a clean
  exit — the first *system-level* regression gate, complementing `make test-jit`/`SS_JIT_VERIFY`.
  A new one-shot `[BOOT] idle frontApp='Finder' modal=0` signal is emitted from the guest idle
  hook (`OP_IDLE_TIME`/`OP_IDLE_TIME_2` = the `SynchIdleTime` patch), enriched with `CurApName`
  (0x910) + a front-window modal check (0x9d6) so the harness distinguishes "idle at the desktop"
  from "idle blocked on a modal dialog". Python + `vncdotool`, env-resolved asset paths for CI,
  pristine-disk-per-run isolation, 12 offline unit tests. A host→guest shutdown *hook*
  (`ShutDwnPower` trap / ADB power-key) was explored and reverted — see spec §12. Requires a
  logged-in macOS GUI session (SDL needs a WindowServer; no Xvfb equivalent). `SheepShaver/e2e/`.

- **18 real FP-arithmetic test vectors**: the pre-existing `fp_*` vectors were
  *vacuous* — they ended at `stfd` and never loaded the result into a GPR, but the
  harness REGDUMP captures GPRs, not FPRs, so a wrong FP result was invisible and
  the JIT-vs-interpreter diff passed trivially. FP arithmetic was effectively
  untested. The new vectors load the result back into a GPR (fadd/fsub/fmul/fdiv,
  the fma family, frsp/fctiwz/fneg/fabs/fmr, and single-precision forms). Generated
  by `jit-test/gen-fp-vectors.py` (documented, reproducible). The 9 vacuous `fp_*`
  originals were then removed (kept `fp_lfd_stfd`/`fp_lfs_stfs` — those *do* round-
  trip the value back into a GPR, so they are real load/store tests).

- **AltiVec coverage rebuilt (corrected over several review rounds)**: an initial
  15-vector batch (14 vacuous — VX-form doubled-XO no-ops) *and* all 12 pre-existing
  `vec_*` vectors were found vacuous (results never reached a checked GPR). Replaced
  with correctly-encoded, verified-non-vacuous vectors via the documented
  `jit-test/gen-altivec-vectors.py` (VX-form XO is unshifted; operands must be
  distinct per-lane to avoid the masking trap). The `vspltb`/`vsplth` probe that
  exposed the ev_mixed divergence was **fixed** (see JIT correctness above), not
  merely flagged; the still-broken `vmrg*`/`vpk*`/multiply ops have repro vectors
  parked in the generator's bug set.

- **Harness integrity preflight**: the SheepShaver `jit-test/run.sh` had no
  self-validation (unlike BasiliskII's). Added a preflight that aborts on a
  malformed/missing/duplicate-name vector before the run. It immediately caught
  three real pre-existing **duplicate vector names** (`crand_basic`/`mcrf_basic`/
  `orc_basic`) where the second `T_` definition shadowed the first, so one vector of
  each pair never ran (silent lost coverage); fixed by renaming the shadowed ones.
  Vacuousness itself is not caught (a vacuous vector still touches scratch GPRs) —
  the `gen-*-vectors.py` generators are the practical defense there.

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
