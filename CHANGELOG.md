# macemu-jit (macOS arm64) Changelog

Changes specific to the `macos-arm64` branch — this fork of kanjitalk755/macemu, which
adds AArch64 JIT backends. Covers **both emulators** plus shared/build/docs work.

Entries are tagged by component: **[SheepShaver]**, **[BasiliskII]**, **[shared]** (code
used by both, e.g. `ether_unix.cpp`, prefs), **[build]**, **[docs]**. Entries before
2026-06-04 predate this fork-wide reorganization and are SheepShaver-scoped unless noted
(BasiliskII history lives in `BasiliskII/docs/AARCH64_JIT_BRINGUP.md` and
`docs/planning/BasiliskII-MACOS-AARCH64-JIT-PORT.md`).

## 2026-06-06

### [SheepShaver][e2e] Guest UI introspection — window control-list items (non-dialog windows)

**Dogfooding fix.** Driving the CarbonLib installer over VNC exposed a real hole: the Apple Installer's
"Continue"/"Install" buttons live in a movable-modal/document window, but Backend A only emitted dialog
DITL `items` for `dialogKind` windows — so a non-dialog window's controls were absent from the JSON and
the driver had to fall back to blind Return. Now `serialize_window_controls()` walks
`WindowRecord.controlList` (+0x8C) → the `ControlRecord` chain (`nextControl`/`contrlRect`/`contrlHilite`/
`contrlValue`/`contrlTitle`, reusing Plan 2b's layout) for **non-dialog** windows too, emitting each
control as an `item` (type/rect/text/value/hilite, same schema) so `find_item`/`click_item` work on them
unchanged. **Verified by re-running the installer on the new build**: its Continue button is now clicked
**by name** via introspection (`click 'Continue'`) — a non-dialog window's controls now surface as `items`,
where before the feature they were absent and the driver fell back to blind Return (`no dialog button;
Return`, observed in earlier-session runs). `make test-jit` score=100; e2e offline suite 107 passed;
`ui-introspect-test` ALL OK — and the branch is now **offline-unit-tested** against a mock RAM (see the
serializer-harness entry below). Commit `ac4363a2`.

### [SheepShaver][test] Offline unit harness for the memory-walking UI serializers

Paid down tracked test-debt: the serializers (`serialize_snapshot`/`_window_controls`/`_dialog_items`/
`_menu_bar`) read guest memory via `ReadMacInt*` and were validated **boot-only**.
`ui_introspect_serialize_test.cpp` now compiles the **real** `ui_introspect.cpp` against a flat
big-endian **mock RAM** (stub `sysdeps.h`/`cpu_emulation.h` in `src/uitest/`, selected purely by `-I`
order — the real build is untouched) and asserts the JSON for hand-built Toolbox structures, so each
offset is a regression-tested fact. Fixtures: non-dialog `controlList` → items (titled button globalized,
dimmed/untitled control, degenerate-rect skip) + dialog DITL items (text/rect/refCon/defaultItem/modality/
default). Wired into `make ui-introspect-test`; standalone `make ui-introspect-serialize-test`. Commit `492607c4`.

### [e2e][docs] Real-world workload bring-up: Fractal Carbon install + CarbonLib via introspection

First steps toward the S4/S5 real-app workload library. Installed **AltiVec Fractal Carbon** onto the
E2E apps disk host-side (forks/type intact; `docs/HOST-SIDE-MAC-SOFTWARE-INSTALL.md`), then resolved its
**CarbonLib ≥1.3** dependency the hard way — **drove the Apple `.smi` installer over VNC with guest-UI
introspection** (`SheepShaver/e2e/run_carbonlib_install.py`): CarbonLib → 1.6 (`INIT/cbon … Jun 2002`).
Extracted the installed extension as a reusable MacBinary (`/Users/Shared/macemu/CarbonLib_1.6_extension.bin`)
so future installs are a one-line `hcopy -m` — no SMI, no boot. Procedure + the SMI/dialog-vs-document-window
gotchas documented in the install doc; saga in `LEARNINGS.md`. Discovery harness commit `3c0368c8`;
install driver committed with `ac4363a2`.

### [SheepShaver][e2e] Guest UI introspection — Plan 2c: menu bar + depth + desktop role

The last app-automation enabler: a top-level `menuBar` (menus + items + **Command-key equivalents** +
enabled + apple role) read by a research-backed, read-only `MenuList` ($0A1C) walk — **no Toolbox
traps** (the idle-hook reentrancy the repo already avoids), robust via **handle-anchoring** (each
entry must deref to a plausible `MenuInfo`, else stop). Offsets verified vs Carbon `Menus.h`
(`SheepShaver/e2e/MENUBAR-READ-SPEC.md`). The harness can now discover "File ▸ Open ⌘O" and fire it by
keystroke (`find_menu_item(snap, "Open").cmd_key`). Also: real `screen.depth` from the GDevice;
`role:"desktop"` on the Finder backdrop window. **Live-verified**: 7 Finder menus
(File/Edit/View/Special/Help…), File's New=N/Open=O/Close=W. A subtle bug fix along the way: extent-END
bounds checks must use a range-only guard (`guest_range_ok`) — `guest_ptr_ok` requires even alignment,
so odd-ended title/text/menu extents were spuriously rejected (this also hardens Plan 1/2a/2b reads).
Commits 4ca2da4d, 8aeb9a2f, ecb0ec42, 706ce769, d2fa9602, e73eb6a6, 5026c2b6.

### [docs] Stress-workload catalog → living capability matrix (stretch goals + frontier metric)

Extended `docs/MACOS9-STRESS-WORKLOADS.md` from a runnable-software list into a **capability matrix
that benchmarks reach over time**. Added a "Capability matrix & stretch goals" section that
**deliberately includes targets we can't run today** (Quake III / OpenGL games — no 3D accel;
FireWire capture; 9.2.x-only software) as **frontier markers**, scored not pass/fail but by *how far
they get* (🌑 won't launch → 🌒 quit → 🌓 menu → 🌔 one frame → 🌕 usable), measurable via the e2e
harness + heartbeat/HOT-PC/trace-ring/`SS_JIT_VERIFY`. Each gated row lists its unlock path (3D bridge,
virtual DV source, New-World ROM → D3). Names the 3D-acceleration bridge as the major unplanned
north-star and "how far Quake III gets per build" as its motivating proxy.

### [docs] Curated Mac OS 9 stress-test software catalog (downloadable, AltiVec-first)

Added `docs/MACOS9-STRESS-WORKLOADS.md` (cross-linked from `TESTING.md`): a linkable catalog of
demanding period-correct PowerPC Mac OS 9 software for stress-testing the JIT, prioritized by
AltiVec-relevance + emulation-feasibility, with download sources (Macintosh Garden / Repository /
archive.org) and per-title caveats. Web-researched, with myth-corrections carried: official
**SETI@home is NOT AltiVec**, **Photoshop 6.0 removed AltiVec** (5.5+AltiVecCore plug-in / 7 have it),
**stock POV-Ray Mac is NOT AltiVec**, **OpenGL games (Quake III) are non-starters** (no 3D HW) but
software-renderer games run, and **iMovie/FCP render-export works camera-free** (only capture needs
FireWire). Top zero-friction AltiVec picks: AltiVec Fractal Carbon (built-in scalar oracle),
SoundJam MP (AltiVec MP3 encode), POV-Ray 3.6 (scalar-FP). Ties to ROADMAP A2/A3/A5-V + B1.

### [docs] Fold in two external threads — JIT stress workloads + hot disk loading

Digested two user-shared sources and folded the actionable parts:
- **emaculation #7159** (the actual provenance of the "pin SheepShaver to one core" lore): specific
  titles crashed on **upstream** multicore (Royal Flush, The Dig, A-Train; Sandy Bridge, Mac OS
  7.5–9.0.4), single-core affinity "fixed" them. Added to `docs/TESTING.md` as documented
  emulator-breakers (concurrency + timing stress; Royal Flush also a timing/speed-calibration probe),
  and to `SheepShaver/docs/CONCURRENCY-MODEL.md` as the lore's provenance + an **empirical falsification
  test** of our "not multicore-sensitive" claim (run them on a busy multicore Apple Silicon host under
  SS_JIT_VERIFY; crashes = a real race to root-cause; the originals were the upstream x86 build).
- **Infinite Mac disk-streaming write-up** (persistent.info): folded a "Hot disk/CD insertion +
  software library" feature into `DESKTOP_INTEGRATION_PLAN.md` Tier 2. Validated that SheepShaver's
  `disk.cpp` already has runtime mount machinery (`DiskMountVolume`/`to_be_mounted`/`mount_mountable_volumes`)
  so hot insertion is feasible; Infinite Mac's runtime injection uses the **same `extfs.cpp`** we have +
  a Downloads/Uploads/Saved watched-folder convention, while its browser streaming (256 K chunks/service
  worker/IndexedDB) does **not** transfer to native (real files + APFS clonefile instead). Cross-linked
  from the Infinite Mac eval plan.

### [SheepShaver][e2e] Guest UI introspection — Plan 2a (dialog items) + 2b (control state) + harness integration

Backend A now emits each **dialog** window's DITL items — `button`/`checkbox`/`radio`/`staticText`/
`editText`/etc. with **globalized (VNC-clickable) rects**, titles, `enabled` (itemDisable), and the
`default` item — plus the window `refCon` + `defaultItem`. Python `uidump.py` gains `Item`,
`find_item`, and `click_item(vnc, snap, win, text="OK")` to click a named button. **Live-verified**
against the Speedometer choose-disk dialog (8 items, `OK`/`Cancel` titles, `refCon='sped'`, plausible
global coords). Review-driven Plan-1 hardening also landed: honest modality from the window *variant*
(not `windowKind`), `frontWindowIndex`=first-visible, junk-title sanity. E2E harness now boots with
`SS_UI_DUMP_DIR` and uses introspection in its gates — `run_lifecycle` logs the desktop window list,
and the benchmark's quit-to-Finder is now a definitive "Speedometer gone + Finder front" check
(heuristic kept as fallback). DX: `make ui-dump` CLI + `Snapshot.render()` ASCII layout,
`wait_for_window`, `click_point`. Canonical reference `SheepShaver/docs/UI-INTROSPECTION.md` updated;
action plan in `docs/planning/UI-INTROSPECTION-REVIEW-SYNTHESIS.md`. Plans: `…-p2a-dialog-items.md`.
**Plan 2b (control state):** control-type items also emit `value`, `hilite` (255 = dimmed/disabled),
and a self-checking `crect` (the `ControlRecord` rect, which must equal the item rect); text with
`^0`–`^3` is flagged `hasParams` (resolution deferred to Backend B). Python `Item.checked`/`.dimmed`/
`.has_params`. **Live-verified** on the choose-disk buttons: `crect_ok=True` on all controls, dimmed
buttons reported `hil=255`. Commits 81c3f9e6, e901beb2, 320a0eb6, 57a8b9af, 80491e28, 325501c2,
06b68fce, 39afb740, 4234bec0, 0299b22a, 31a3177c.

### [docs] Fold the "Developer Inspector" into SiliconSheep + correct DingusPPC license to GPL-3.0

- **Folded the Snow-inspired build/debug "chrome" recommendation into the SiliconSheep plan** as a
  tracked feature: `DESKTOP_INTEGRATION_PLAN.md` → "Developer Inspector / Debug Chrome". A live
  *observability inspector* (not a step-debugger) in the Tauri UI, fed by the **B1 profiler + existing
  diagnostics** (heartbeat/ring/HOT-PC/`SS_JIT_WATCH_ADDR`/window-title stats), riding the Tier-4
  Layer-A launcher↔emulator RPC. Cross-linked from the Snow plan (rationale), ROADMAP B1 (data layer),
  DingusPPC plan, and OPTIMIZATION-PLAN §P0. Unifies the three eval plans' debug-tooling crosswalks.
- **License re-validation (primary source): DingusPPC is GPL-3.0, NOT BSD-3-Clause.** Verified against
  the repo's root `LICENSE` (GPLv3 verbatim) and GitHub metadata (`spdx_id: GPL-3.0`). This **corrects
  the prior CHANGELOG/plan note (`9affe3a4`)** that said BSD-3-Clause — that was a bad scrape; the
  original plan's "similar GPL family" was right (same copyleft family as our GPL SheepShaver).

### [docs] Validate + sharpen the external-emulator evaluation plans (Snow / DingusPPC / Infinite Mac)

Web-verified the factual claims in the three eval plans (`b2bf2c0e`) and corrected them:
- **Snow** is **68K-only** (Mac 128K–II; 68000/020/030), **MIT**, Rust/egui, deliberately
  hardware-level — so its *emulation* lessons apply to **BasiliskII only, never SheepShaver's PPC**;
  only its *debugger/observability UX* transfers. Added a **"Build/debug presentation" recommendation**
  answering "should we adopt Snow-style chrome": build a **live observability *inspector*** (not a
  step-debugger) hosted in **Silicon Sheep (Tauri)**, fed by the **B1 profiler + existing diagnostics**
  (heartbeat / ring / HOT-PC / `SS_JIT_WATCH_ADDR` / window-title stats) — profiling-first, no
  emulator-core churn. Unify the three plans' separate debug-tooling crosswalks into one.
- **DingusPPC license corrected: BSD-3-Clause (permissive), not "GPL family"** — and flagged as the
  **most directly relevant** reference (PowerPC interpreter with real MMU + setjmp/longjmp exceptions);
  cross-linked to `MMU-NANOKERNEL-MP-PLAN.md`.
- **Infinite Mac** runs **our own SheepShaver lineage** (downstream WASM sibling) — best host-integration
  reference for Silicon Sheep; "includes Snow" is true-but-secondary; AppleTalk-over-Cloudflare-Durable-
  Objects zones, off by default; runtime media injection is a product feature not in repo docs.

### [SheepShaver][e2e] Guest UI introspection — Plan 1 walking skeleton (SS_UI_DUMP_DIR)

A read-only, env-gated host-side dump of the guest `WindowList`: front→back window list with
global (VNC-clickable) bounds, title (MacRoman→UTF-8), and dialog/active/visible flags, serviced
at the idle safe point and written as nonce-stamped JSON. Python consumer `sse2e/uidump.py`
provides the snapshot handshake and query/occlusion helpers. Live boot smoke verified: backend A,
screen 800×600, 2 windows (CD volume + Desktop), clean exit 0. Activate by setting
`SS_UI_DUMP_DIR` to a writable directory (feature is off when unset). Commits: `245d9fee`
(text transforms), `ee21de74` (shared guard), `7665d73a` (Backend-A walk + transport, hardened),
`13c0aa0c` + `07271080` (Python uidump consumer), `5d2bc4cf` (integration smoke). Spec:
`docs/superpowers/specs/2026-06-06-guest-ui-introspection-design.md`.

### [docs] Canonical concurrency-model reference — "SheepShaver is not multicore-sensitive, and why"

Added `SheepShaver/docs/CONCURRENCY-MODEL.md`: the citable, code-grounded answer to a recurring
question. States plainly that this fork runs the guest on one host thread (single logical guest CPU)
with synchronized host helpers (atomic `spcflags`/P0d, `atomic_or` `InterruptFlags`, mutex'd
framebuffer, signal-driven `SIGUSR2` interrupts) — so it is **not** multicore-sensitive and needs no
core-pinning (that folklore is upstream/older builds). Documents the one real caveat: the JIT cache
is single-writer *by construction, not by lock* (no locks on `jit_cache_wp`/`jit_bc_pool_next`/chain
pool; per-thread W^X), a property to preserve — adding a compiler thread needs R8→R9 first. Linked
from `docs/ARCHITECTURE.md` and the MULTICORE plan's concurrency baseline.

### [SheepShaver] LR-prediction icbi-safety fix + two stale-comment refreshes (code review)

A code-review pass (`72c5e525`) found one latent bug + two stale comments in `ppc-jit.cpp`:
- **icbi/SMC stale-translation bug:** the R1 LR-prediction fast-path emitted a raw `B chain_code`
  to the return block's chain entry **without `record_chain_site`**, so
  `ppc_jit_aarch64_invalidate_range` couldn't revert it — a RAM return-target that got SMC/icbi-
  invalidated left the predicting block branching into a stale translation (the historical icbi-hang
  class). Naive registration is unsafe (the B is mid-hit-path; the revert writes a lone LDP word →
  double epilogue). **Fix:** only direct-chain LR predictions to never-invalidated **ROM** targets;
  RAM returns fall back to the standard dispatcher (correct, unoptimized). Boot-verified (ISO→Finder,
  no hang), `make test-jit` 302/302. (= CROSS-EMULATOR-IDEATION finding #L, now ✅.) Restoring the
  RAM optimization via a registered revert-to-miss-path site is filed as a perf follow-up.
- **Stale comments:** the block-chaining header claimed "default OFF / flip to 1 to test" (it's ON
  + boot-verified — rewritten, with a do-not-disable note); the KNOWN-AltiVec-BUG (PARKED) block
  claimed "STILL BROKEN: vmuloub/vmuleub" (fixed long ago) — updated to current status + an explicit
  "approach A (global REV32 at load/store) REJECTED" guard so it isn't re-attempted.

### [docs] Fold external multicore analysis into the plans (+ code-verified concurrency baseline)

- Reconciled a web-sourced external analysis against our plans. It **independently converged** on the
  host-side-parallelism-not-guest-SMP thesis — validation for `MULTICORE-OFFLOAD-PLAN.md`. Folded in:
  - **Concurrency baseline (code-verified):** debunked the external "pin the JIT to one core" claim
    for this fork — the runtime is cleanly single-writer-guest (atomic spcflags, mutex'd framebuffer,
    signal-driven interrupts). But the JIT cache is single-writer-*by-assumption* (no locks on
    `jit_cache_wp`/`jit_bc_pool_next`/chain-site pool; per-thread `pthread_jit_write_protect_np`), so
    Tier-1 background compile has named hard prerequisites: **R8 dual-W^X → atomic cache allocation →
    cross-thread invalidation**. This makes the R8→R9 dependency mechanical, not aspirational.
  - **New Tier-2 device ideas:** **hardware cursor** (host overlay, decoupled from guest 60 Hz
    redraw — added as idea-bank grid #11) and the **"synthetic devices"** framing for the HLE layer.
  - **Deterministic event-queue corollary** for interrupt/device-completion (also the record/replay
    substrate).
- Cross-EMULATOR-IDEATION.md gets the external-corroboration note + grid #11; noted that the external
  "no-MMU ⇒ no 9.1+" line is common-wisdom — our New-World-ROM plan found parcels-ROM is the first wall.

### [SheepShaver] tools: jit-diff-sweep.py — differential op-sweep + auto-referee (instrumentation #11)

The unified-instrumentation payoff (ROADMAP A1 #11, Phase 0+2 in practical form): a committed,
reusable tool (`SheepShaver/tools/jit-diff-sweep.py`) that formalizes the throwaway sweep scripts +
by-hand `SS_TEST_HEX` triage that found the session's 27 codegen bugs. For each registered AltiVec/FP
op it injects one instruction (crafted lane-distinct, sign/saturation-crossing operands) through the
**real emulator twice** — interp (`SS_TEST_JIT=0`) vs JIT (`SS_TEST_JIT=1`) — and diffs the result
register. The interpreter IS the trusted oracle, so the tool is its own **auto-referee** (no second
pass): `interp == JIT` ⇒ correct; `!=` ⇒ a real divergence with a copy-paste repro.

- **Run-stamped JSONL output** under `$SS_RUN_DIR` (default `/tmp/macemu-runs/<ts>-sweep/` + a
  `latest` symlink), one record per op (`{schema,tool,kind,family,op,verdict,oracle,interp,jit,hex}`)
  + a `summary.json` — the #11 schema. FAIL records carry the exact `SS_TEST_HEX` repro.
- **Regression-tracks known-broken ops** (the pack family, A2) as `known-broken`, not `FAIL`, and
  **skips un-referee-able ops** (`fsqrt`/`fres`/`frsqrte`: interp lacks them / estimates).
- Lessons baked in (operands non-saturating so lane bugs can't hide; see
  `docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md`). Current run: **pass=38, FAIL=0, known-broken=3**.
- **Remaining for full #11:** Phase 1 orchestrator (`make test-session` aggregating all oracles)
  and the in-emulator C++ JSONL emitters (so `SS_JIT_VERIFY`/heartbeat feed the same run dir) — this
  tool delivers the high-value referee piece standalone. Spec:
  `docs/superpowers/specs/2026-06-06-unified-test-session-instrumentation-design.md`.

### [SheepShaver] FP differential sweep — fctiw/fctiwz conversion bug fixed; rest of FP clean

Pivoted the proven sweep method to the under-tested FP ops (vs the real interpreter, with
edge-case operands: signed zero, NaN, inf, rounding boundaries). **`fsel`, `fnabs`, and the single
fused ops `fmsubs`/`fnmadds`/`fnmsubs` all passed** — clean. One real bug found:

**`fctiw`/`fctiwz` (double→int32) were both wrong** (`ppc-jit.cpp` case 14/15): both emitted the
*same* `FCVTZS Xd` (64-bit, toward-zero), so (a) `fctiw` ignored its FPSCR-rounding contract and
behaved identically to `fctiwz`; (b) overflow mis-saturated (2³¹ → `0x80000000` instead of
`0x7FFFFFFF`) because of the 64-bit-convert-then-truncate; (c) NaN → 0 instead of `0x80000000`.
Fix: 32-bit `FCVTAS Wd` for `fctiw` (round-nearest-ties-**away** = PPC `frin` = FPSCR default RN=0)
and 32-bit `FCVTZS Wd` for `fctiwz` — the 32-bit form gives PPC's INT32 overflow saturation for
free — plus an `FCMP`/`CSEL` NaN→`0x80000000` fixup. Verified interp==JIT across 2.5/3.5/−2.5/2³¹/
inf/NaN/±1 (`fctiw` now correctly rounds half-away 2.5→3, distinct from `fctiwz` 2.5→2). All NEON
capstone-verified. **5 committed vectors** (`fp_fctiw_round`/`_ovf`/`_nan`, `fp_fctiwz_trunc`/`_nan`);
`make test-jit` **302/302**. **Known limitation (documented in-code):** only the default FPSCR
RN=0 is honored for `fctiw`; non-default dynamic rounding modes aren't read yet (still a strict
improvement — the old code was wrong for *all* rounding). `fsqrt`/`fres`/`frsqrte` remain
un-sweepable (interp doesn't implement them / they're estimates).

### [SheepShaver] AltiVec saturating add/sub + signed averages fixed — 14 more bugs (broad sweep)

A broad differential sweep (every untested VX-form AltiVec op, JIT vs real interpreter, distinct
operands) + a sub-agent audit surfaced a second large bug cluster, all written without differential
validation (scrambled comments again). Fixed in `ppc-jit.cpp`, all NEON capstone-verified:

- **Saturating add** `vaddubs/uhs/uws` (512/576/640), `vaddsbs/shs/sws` (768/832/896): emitted
  **SABA/UABA** (absolute-difference-accumulate) — a completely different op — instead of
  **UQADD/SQADD**. Also signedness was transposed vs the canonical XO.
- **Saturating sub** `vsububs/uhs` (1536/1600), `vsubsbs/shs/sws` (1792/1856/1920): correct opcode
  but **signed/unsigned swapped** (unsigned XOs emitted signed `SQSUB`, and vice versa) → wrong
  clamp. Now `UQSUB`/`SQSUB` per the canonical op.
- **Signed averages** `vavgsb/sh/sw` (1282/1346/1410): emitted **SMAXP** (pairwise max!) instead of
  the signed rounding average **SRHADD**. (Unsigned `vavgu*` were already correct.)

Validated against the real interpreter across mid-range + saturation-boundary + signed-overflow
operands (42/42 agree). **14 strong committed vectors** with boundary-crossing operands (new
`load_bytes`/`satop` generator helpers — `load_pattern`'s linear slope made subtraction vacuous);
`make test-jit` **296/296** (was 282). Sweep also confirmed integer **compares** all correct, and
flagged the pack/pixel/sum families (structural rework, overlap ROADMAP A2) + FP-conversion scale
handling as remaining — see `docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md`.

### [docs] Cross-emulator ideation capture + effort/payoff grid + R3 stale-note fix

- Added `docs/planning/sheepshaver-research/CROSS-EMULATOR-IDEATION.md`: a reality-checked capture
  of a 6-persona lateral-ideation workflow (Dolphin/RPCS3/Cemu = PowerPC; QEMU; Rosetta/FEX = same
  host; wildcard), 88 ideas distilled to an effort/payoff/blocked **grid**. Nominated first picks:
  `CopyBits` HLE (games/media, →Metal) and idle-skipping (battery/thermal/desktop citizenship);
  do-anyway: dual-W^X (R8). Records what's already shipped (chaining, AltiVec byte-mults) so it's
  not re-chased, defers the persistent-ROM-cache idea (re-file under Silicon Sheep snapshot/resume),
  and logs strategic anchors (TSO-for-SMP, record/replay time-travel, static AOT recomp).
- Wired: ROADMAP B4 + OPTIMIZATION-PLAN (HLE section) point to the idea bank; the two nominees are
  promoted to first-class tracked items **ROADMAP B5 (`CopyBits` HLE)** and **B6 (idle-skipping)**.
- **Fixed OPTIMIZATION-PLAN R3 stale note** — it claimed "no W^X toggling exists"; the code uses
  `MAP_JIT` + `pthread_jit_write_protect_np` (`jit-target-cache.hpp:34-39`), superseded by R8.
- Surfaced (for the JIT owner, not fixed here): two stale `ppc-jit.cpp` comments (chaining
  "default OFF"; AltiVec "STILL BROKEN: vmuloub") and a latent bug (unregistered LR-prediction
  `B chain_code` at ~:3052). Handoff prompt prepared.

### [SheepShaver] AltiVec shift/rotate family COMPLETE — halfword/word shifts + all rotates fixed

Finished the family started with the byte ops below. `ppc-jit.cpp` case 324/388 (`vsl{h,w}`),
580/644 (`vsr{h,w}` logical right), 836/900 (`vsra{h,w}` arith right), 4/68/132 (`vrl{b,h,w}`):

- **Halfword/word shifts:** same fix as the byte ops at `.8H`/`.4S` (mask mod 16/32, truncating
  `USHL`/`SSHL`, `+NEG` for right). Validated against byte-**asymmetric** lvx operands with amounts
  exceeding the element width — so the **ev_mixed byte order is empirically covered** (a non-issue
  for per-element shifts: `vslw` already passed asymmetric data pre-fix).
- **Rotates:** NEON has no vector rotate — synthesized `rol(x,k) = (x<<k)|(x>>>(w-k))` with
  `k=amt&(w-1)` (mask, copy, left `USHL`, `SUB` width, right `USHL` by the negative, `ORR`). The old
  code emitted a plain shift (dropped the wrapped bits) and didn't mask.

All capstone-verified. **9 more strong committed vectors** (`av_vsl{h,w}`/`av_vsr{h,w}`/`av_vsra{h,w}`
/`av_vrl{b,h,w}`); `make test-jit` **282/282** (was 273). The full 12-op variable shift/rotate
family is now correct and covered. `docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md` marked ✅ ALL FIXED.

### [SheepShaver] AltiVec variable BYTE shifts fixed — vslb/vsrb/vsrab (3 confirmed bugs)

Fixed the first family of the AltiVec shift bugs found below — the **byte** ops, which are
byte-order-safe (each lane independent). `ppc-jit.cpp` case 260/516/772, capstone-verified NEON:

- **Mask the shift amount mod element width** (`DUP #7`→v2, `AND`) before the shift — AltiVec wraps
  the amount mod 8; the old code passed it raw so amount ≥ 8 shifted the bits out (`vslb` by 9 gave
  0 instead of `<<1`).
- **Use truncating, correctly-signed shifts**: `vslb`→`USHL`; `vsrb` (logical right)→`NEG`+`USHL`
  (was a signed *left* shift — wrong direction); `vsrab` (arith right)→`NEG`+`SSHL` (was `SRSHL`,
  which *rounds* — AltiVec truncates). The old code used rounding/signed variants throughout.

Validated: the `SS_TEST_HEX` repros now agree interp==JIT; **3 strong committed vectors added**
(`av_vslb`/`av_vsrb`/`av_vsrab`, distinct high-bit data + amounts 0..15 exercising the mask — 11–12
distinct result bytes, non-vacuous), `make test-jit` **273/273** (was 270). **Still open** (tracked,
`docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md`): halfword/word variants (same fix, but the ev_mixed
byte order needs lvx-built byte-asymmetric validation) and the rotates `vrl{b,h,w}`.

### [SheepShaver] AltiVec shift/rotate codegen bugs found (hunt paid off) — repros recorded, fix pending (`c7b0c98c`)

Acting on the "AltiVec/FP is the highest residual-bug surface" strategy verdict, led a differential
hunt with the zero-coverage variable-shift family (where ARM64 NEON is *not* 1:1 with AltiVec).
Found **real, oracle-validated** codegen bugs in minutes — same lane/width-sensitive category that
produced the 9 `ev_mixed` bugs. Validated against the **real emulator** interp (`SS_TEST_HEX
SS_TEST_JIT={0,1}`), not the rom-harness subset interp:

- **Missing mod-element-width masking** — `vslb v2,v1,v3` with amount 9: interp `0x08` (9 mod 8 →
  <<1), JIT `0x00` (NEON `USHL` by 9 ≥ 8 shifts out). Affects all variable shifts/rotates at
  amount ≥ width.
- **Logical right shifts emit the wrong shift** — `vsrb` `0xF0>>1`: interp `0x78` (logical), JIT
  `0xe0` (shifted **left**!); `vsrh`/`vsrw` give arithmetic (sign-filled) results instead of
  logical. `vsra{b,h,w}` (arithmetic right) are correct.
- **Rotates `vrl{b,h,w}`** use a plain shift by inspection (lose wrapped bits) — suspected, needs an
  lvx-built repro.

The codegen's own inline comments are scrambled (`vsrb`↔`vsrab`), confirming this path was written
without differential validation. **Repros + fix plan: `docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md`.**
Fix deferred to a focused, differentially-validated pass (add xfail repros → fix `ppc-jit.cpp`
~3320–3331/3446–3447 op-by-op → `make test-jit` 270 + boot smoke). ROADMAP A1.

### [docs] Unified test-session instrumentation design + planning reconciliation (`88c3311d`)

- **New design spec** `docs/superpowers/specs/2026-06-06-unified-test-session-instrumentation-design.md`:
  one `SS_RUN_DIR` run-stamp convention + a JSONL record schema (with an `oracle`-trust field) so the
  four correctness/perf oracles (test-jit, rom-harness, `SS_JIT_VERIFY`, E2E/bench) + diagnostics
  write to one analyzable place; a reconciliation analyzer **auto-referees disagreements via
  `SS_TEST_HEX`** (automating the manual 2026-06-06 triage) and **joins HOT-PC "hot" with microbench
  "slow"** for optimization leads. Phased (0: schema + 2 tools; 1: orchestrator; 2: auto-referee;
  3: perf join), building on the benchmark-export run-stamp pattern + `jit-analyze.py`.
- **Planning reconciled:** ROADMAP A1 gains the unified-instrumentation bullet and reflects the
  rom-harness arc (span gate + triage ✅, integer path clean) and AltiVec/FP promoted to 🔜;
  OPTIMIZATION-PLAN P0 cross-links the perf-join half of the spec; ROADMAP "Updated" → 2026-06-06.

### [docs] Multi-core offload plan ("multithreading light") (`4e8cc8f8`)

- Added `docs/planning/MULTICORE-OFFLOAD-PLAN.md`: what guest/emulator work can move to other host
  cores. Core finding: the guest is one logical cooperative CPU, so the high-ROI wins are
  emulator-*internal* parallelism (background JIT compile R9, dual W^X R8, Metal compositing R10,
  async devices) — not splitting guest execution; and games/large apps benefit more from getting the
  emulator's housekeeping *off* the hot core than from any guest SMP. Tiered 0–3 with
  effort/payoff/risk; "true guest SMP" (MP tasks on separate cores) is Tier 3, gated on the
  supervisor-fidelity plan's MP work + a cross-core coherence project (real `lwarx`/`stwcx.` +
  PPC→ARM64 barriers).
- Wired bidirectionally: ROADMAP B4 pointer; back-pointer from `MMU-NANOKERNEL-MP-PLAN.md` sub-plan C
  (Tier 3 is its multi-core extension).

### [docs] Supervisor-fidelity plan (MMU / nanokernel / MP) + New World ROM "second wall" (`36e45a74`)

- Added `docs/planning/MMU-NANOKERNEL-MP-PLAN.md`: specs the three privileged layers SheepShaver
  deliberately *stubs* (MMU faked V=P; nanokernel exception/interrupt model bypassed; preemptive
  MP tasks absent), grounded in the actual stub sites (`rom_patches.cpp` `patch_nanokernel*`,
  `main_unix.cpp` `sigill_handler`/`tick_func`, `ppc-execute.cpp` `sc`→illegal). Key finding: the
  dependency order is *not* MMU→nanokernel→MP — MP rides on nanokernel exception/decrementer
  fidelity, which mostly does **not** need real MMU translation (identity mapping suffices), so the
  cheapest path to MP/timing fidelity skips the MMU. Includes effort/payoff/risk per sub-plan and a
  cheap "stub-pressure" probe to run before committing.
- Linked it as the possible "second wall" behind `NEW-WORLD-ROM-SUPPORT-PLAN.md` (Phase 3 pointer),
  and added **ROADMAP D3** ("break the 9.0.4 ceiling") tying the two plans together. New World ROM
  support was previously unlinked from the roadmap; now it is.

### [SheepShaver] rom-harness — span gate cleans the differential signal (~46 → ~7–8 failures/seed) (`fe378c5d`, triage `980cf4df`)

Follow-up to the skip-not-abort fix: the harness's failures were dominated by a **block-model
mismatch**, not codegen bugs — the scanner ends a block at `bc` (opcode 16) but the JIT runs
*past* it (bc is not a JIT terminator), so the two compared different instruction spans from the
same start PC (the same root cause as `SS_JIT_VERIFY` fix-(i)).

- **Span gate (`rom-harness.cpp`):** compare only when `jblk.n_insns == blk.n_insns`; dropped
  blocks are reported as **`Span mismatch`** (visible, not a silent cap). On the OldWorld ROM
  (`--count=10000`, seeds 1/7/42) this cut failures from ~43–49/seed to **~7–8 span-matched,
  trustworthy failures** (≈710 bc-terminated blocks skipped). The remaining failures are now
  genuine span-matched divergences (PC/CR-dominated — branch-target/condition), no longer cascade
  artifacts from the span mismatch, so they're worth refereeing via `SS_TEST_HEX`.
- **Tactical hardening:** `alarm(0)` now cancels the per-block timeout on all exit paths (normal,
  SIGSEGV, fallback longjmp). In-code block-model notes at the compare site + `is_block_terminator`.
- **Survivor triage done — integer path differentially CLEAN, zero real JIT bugs.** Refereed 3
  distinct span-matched survivors against the real emulator (`SS_TEST_HEX`/`SS_TEST_INIT`,
  `SS_TEST_JIT={0,1}`): pure `bl`, `extsh r3,r7`+`bl` (harness claimed `jit` clobbered source r7),
  `mr;addi;li;bl` (harness claimed `jit` zeroed untouched r9/r11). In all three real-interp == JIT
  == correct (e.g. `extsh r3,r7` → r3=`0000197d`, r7 unchanged). Every survivor is a **harness
  artifact**: all `b`/`bl`-terminated, the harness's *JIT* run follows the branch at runtime (into
  real ROM at the relative target, which mutates registers) while its reference interp stops at the
  block end — a *runtime* branch-follow the *compile-time* span gate doesn't catch.
- **Open follow-ups (ROADMAP A1, both rom-harness polish, neither a JIT bug):** stop the harness JIT
  following the branch (disable chaining / snapshot at block exit); and/or recover the span-gate
  coverage by running the interp for the JIT's instruction count. Higher-priority residual-bug
  surface remains AltiVec/FP operand vectors in `test-jit`.

### [SheepShaver] rom-harness — skip-not-abort on fallback blocks (broad sweeps unblocked) (`c1a10c0a`)

The standalone differential rom-harness `abort()`ed the entire run the moment a JIT block that
the compiler marked `complete == true` hit the inline-interp fallback bridge (`ppc_jit_interp_one`)
at runtime — which happens whenever the linear ROM scan picks up a mis-scanned data region or an
op handled only via fallback. Under random seeds this killed the run on the *first* such block
(no Score line printed), making broad differential sweeps impossible.

- **Fix (harness-only, `rom-harness.cpp`, zero emulator/boot risk):** the fallback bridge now
  `siglongjmp`s back to a per-block guard (a shared `sigsetjmp` buffer, mirroring the existing
  SIGSEGV protection) instead of `abort()`ing. Such blocks are counted as **`JIT fallback`**
  skips and the sweep continues to completion. Verified across 3 seeds (1/7/42, `--count=10000`):
  all now finish cleanly (~110 fallback blocks gracefully skipped/seed, was: abort on block 1).
- **Honest characterization of the now-visible failures (root cause found via review — block-model
  mismatch, not codegen bugs):** ~43–49 differential failures/seed are surfaced, but the dominant
  cause is **structural, not a bug**. The scanner ends a block at the first terminator and counts a
  conditional branch (`bc`, opcode 16) as one — but the JIT does **not** treat `bc` as a block
  terminator, so it compiles/runs *past* it. The harness then compares a short interp run against a
  longer JIT run from the same start PC; registers diverge because the two ran **different
  instruction spans** (the exact analog of the `SS_JIT_VERIFY` fix-(i) block-exit problem).
  Verified: the flagged single-instruction `bc` block `42424642` "fails" only because the JIT ran 5
  instructions past it while the harness interp ran 1; the `bc` codegen is **correct** — a
  non-vacuous real-emulator test (`SS_TEST_HEX="38600002 7C6903A6 42424642"` = li/mtctr/bc) gives
  identical interp and JIT REGDUMP (CTR 2→1, branch to the correct AA=1 target). So this is a
  harness block-comparison artifact, **not** a JIT bug and **not** a harness-interp ISA bug. The
  GPR-diff "minority" is largely **cascade** from the same span mismatch in multi-insn blocks, not
  independent bugs. A trustworthy differential would compare only when `jblk.n_insns == blk.n_insns`
  (or run the interp for the JIT's instruction count) — tracked as a follow-up. Treat the absolute
  count as a noisy regression-delta upper bound, **not** an "N JIT bugs" figure. README
  "Interpreting Results" updated with the block-model caveat + the worked `bc` example.
- **Corroborates fix-(ii) must-fix (a):** the root cause (`complete` not distinguishing
  fallback-ending blocks) is the same `ends_in_fallback` signal gap noted in `ppc-jit.cpp:4792`
  and `docs/superpowers/specs/2026-06-05-verify-memory-snapshot-fix-ii-design.md`.

### [SheepShaver] SS_JIT_VERIFY oracle — X1 fix (i): replay mirrors the JIT single-block exit (`5ac5e676`, dedup `7314bc9c`)

The differential oracle's interp replay used "stop when pc leaves [start,end)", which
re-iterated intra-block loops, followed blr returns, and ran into post-bc dead code — the
structural false-positive classes 2/4/5 (OPTIMIZATION-PLAN §0b-extra4). Root cause (from the
compiler): a JIT block runs LINEARLY until the first conditional branch (`bc`, opcode 16 —
**both** arms `emit_epilogue_with_pc` to the dispatcher, taken or not) or an unconditional
terminator/taken branch; `bc` is not a terminator, so the compiler emits dead code past it
that inflates `n_insns`.

- **Replay now mirrors the JIT's single-block exit** (`ppc-cpu.cpp`): execute each insn, stop
  when PC left the sequential path (`pc != cur+4`) OR the insn was a `bc` (opcode 16). Plus a
  no-op-skip guard (`memcmp(jit_state, pre_state)` for the entry-spcflags-poll bail) and a
  one-time warning when `SS_JIT_NO_CHAIN=1` isn't set. **Boot-validated** (`SS_JIT_NO_CHAIN=1`):
  the 7 known control-structural blocks now verify **clean boot-wide** (ARTIFACT-PC count 0,
  was 7); design adversarially challenged by a sub-agent across two rounds (it caught the
  not-taken-`bc`/opcode-16 case).
- **Per-block report dedup**: a memory-RMW block (e.g. `100fd0e0`, a `lwz/addi/stw` counter)
  diverges every visit and previously consumed the whole report budget, blinding the oracle to
  the rest of boot. Now each distinct block is reported once. The oracle now sees past it; the
  newly surfaced divergences are **all memory-contamination, no codegen bug**: SUSPECT
  `10106b50` (`lhz/addi/sth`), `1011e734` (stw to a just-loaded pointer slot), `1018b04c`
  (`lwzx/stwx`), plus memory-dependent-`bc` ARTIFACT-PC blocks.
- **Remaining**: only the memory-RMW confound (class 6) is left — the replay reads guest memory
  `fn()` already wrote. Fix (ii) (memory snapshot/restore, verify-gated) is the next step; it
  also doubles as the proof of "no real bug" (if it cleans them all, they were artifacts).
- **Verify sweep (2026-06-05, `SS_JIT_VERIFY=1 SS_JIT_NO_CHAIN=1`, HD boot) — honestly scoped:**
  with fix (i) + dedup, the oracle reports **exactly four `SUSPECT` blocks**
  (`100fd0e0`/`10106b50`/`1011e734`/`1018b04c`) and four `ARTIFACT-PC` blocks. **All four
  `SUSPECT`s are class-6 memory-RMW**: three *encoding-certified* (load+store same base reg &
  offset — `0(r8)`, `10(r31)`, `4(r3)`), and `1018b04c` most-likely (indexed `[r3+r4]`, `stwx`
  base reloaded → contingent; value-chain confirms artifact, broken-`lwzx` hypothesis refuted —
  the JIT's own load returns nonzero). **No real codegen bug in the covered region — but NOT
  whole-boot:** under verify the guest stalled in an early-boot spin ~10 s in (`comp=10826` frozen
  10 s→3 min, only 8 distinct block PCs ever verified ≈0.07 % of compiled blocks, never reached
  Finder — the documented timer-starvation trap; "nothing new after" = nothing executed). **Deeper
  differential coverage is cheaper offline** (`make test-jit`, `rom-harness --passes/--seed/--count`,
  no timer dependency) — preferred over more boot sweeps. Fix (ii) re-framed: **DEFER pending
  greenlight** — the tool that would *certify* the artifacts + unblock deeper boot-time verify,
  not mere polish. Ready-to-implement design with the two landing-blocker must-fixes
  (`ends_in_fallback` bit; 16384-entry journal) and the register-only caveat:
  `docs/superpowers/specs/2026-06-05-verify-memory-snapshot-fix-ii-design.md`.

- **P1a substantially strengthened — RA eviction path validated (targeted surfaces).** The
  >8-live-GPR `ra_evict` path (never hit by the harness's small vectors) is validated on
  **oracle-independent** evidence: the eviction battery (below) passes the harness JIT-vs-interp
  REGDUMP (separate clean executions, no replay confounds), and a full chaining-on boot reaches
  Finder with eviction firing continuously. **P8 (cross-block pinning) unblocked.** Calibration:
  targeted harness coverage is strong for mid-block, pure-register, single-exit, Rc=0 eviction;
  two surfaces have functional (boot) coverage but no targeted vector yet — eviction at
  control-flow exits/terminators, and eviction × deferred CR0/XER state under pressure (next
  cheap no-boot increment). Detail + residual: OPTIMIZATION-PLAN §P1a.
- **Eviction harness battery broadened (264 -> 270 vectors).** Six pure-register vectors
  widen `ra_evict` coverage beyond the single `lmw_stmw_wide` load/store-multiple case, each
  using 16 distinct GPRs > `RA_NUM_REGS=8` (capstone-verified encodings, no memory/chaining/loop
  confound). Mid-block, pure-register: `evict_wb16` (dirty spill+reload), `evict_rd_eq_ra`
  (ra_load-before-ra_store ordering under pressure), `evict_mixed_alu` (add/subf/and/or/xor).
  Edge surfaces: `evict_branch_exit` (a conditional `bc` **terminates the block** — verified
  `blocks=2` — exercising `ra_flush_all` on a branch exit, not just `blr`), `evict_rc1_cr0`
  (Rc=1 `add.` → `emit_update_cr0` under pressure, CR captured), `evict_adde_carry` (`addic.`+
  `adde` chain → XER carry under pressure). All pass `make test-jit` (270/270, score=100); each
  confirmed fully JIT-compiled (not an interp fallback) with hand-checked REGDUMPs. The
  confound-free, no-boot way to harden P1a -- the right instrument vs. the verify oracle. Not
  exhaustive by design (interp-fallback/spcflags/FP-VR/bclr-bail edges remain boot-only).
- **Mapped the `SS_JIT_VERIFY` differential oracle's 6 false-positive classes** across three
  whole-boot sweeps (chaining-on; `SS_JIT_NO_CHAIN=1`; raised-budget ~92k lines). Every reported
  divergence is structural, **zero real codegen bugs**: block chaining, blr/bclr returns,
  intra-block loops, mid-block conditional paths, PC bookkeeping, and **memory-RMW** (the replay
  restores registers but not guest memory, so a `lwz;addi;stw` counter shows a PC-matching,
  step-by-2 divergence that fools a naive PC-match filter — `100fd0e0`). A literal whole-boot
  clean verify is **unachievable** here (low budget → oracle goes dark mid-boot; high budget →
  verify-every-block starves the guest timer into an early-boot ROM spin). Full taxonomy + the
  X1 fix plan (replay mirrors JIT path/terminator; snapshot+restore memory; targeted/sampled
  verify): OPTIMIZATION-PLAN §0b-extra4, ROADMAP A1.
- **Diagnostic knobs added** (`ppc-jit.cpp`, `ppc-cpu.cpp`): `SS_JIT_NO_CHAIN=1` now logs a
  startup marker so a no-chain run self-confirms; new `SS_JIT_VERIFY_BUDGET=N` overrides the
  divergence report budget (default 20) for whole-boot coverage. The earlier "exactly one blr
  residual" baseline was itself a latch artifact (§0b-extra5) — superseded by the taxonomy above.
- **`SS_JIT_VERIFY` made triagable + targetable (partial X1, tooling).** Each divergence is now
  classified inline — **`[VERIFY] SUSPECT`** (interp/JIT agree on exit PC ⇒ same control flow ⇒
  real-bug candidate) vs **`[VERIFY] ARTIFACT-PC`** (exit PC differs ⇒ structural confound, not a
  codegen bug) — so a whole-boot log triages to `grep '[VERIFY] SUSPECT'` instead of a wall of
  noise (memory-RMW still lands in SUSPECT; disambiguate by value stepping by 2, not 1). New
  `SS_JIT_VERIFY_PC=LO:HI` scopes the oracle to one PC range to re-check a suspect block without
  the whole-boot slowdown. These make the *existing* confounded oracle usable; the replay-mirror
  + memory-snapshot fixes (to make it *clean*) remain the X1 follow-on. **Boot-validated against
  ground truth (2026-06-05):** two scoped `SS_JIT_NO_CHAIN=1 SS_JIT_VERIFY=1` boots tagged the 7
  known structural blocks `ARTIFACT-PC` (20/20) and the lone memory-RMW block `100fd0e0` `SUSPECT`
  (200/200, `GPR10` confirmed stepping by 2 — correct codegen, oracle double-apply); `SS_JIT_VERIFY_PC`
  scoping confirmed working. Harness unaffected (270/270).

### [SheepShaver] Guest OS version detection via SysVersion low-memory global (`ce725b75`)

- The emulator's idle hook now reads the guest's `SysVersion` ($015A) — a BCD-packed
  OS version set by Mac OS during boot. Emits `[SYSV] osVersion=0x0860 (8.6.0)` to
  stderr on first detection. Verified firing on Mac OS 8.6 (E2E ISO) and Mac OS 9.0
  (benchmark disk). This is a generic guest-state introspection hook, reusable for
  other runtime queries.

### [shared] SavePrefs now preserves comments, blank lines, and ordering (`99b9b720`)

- Previously `SavePrefs()` dumped the in-memory prefs list, stripping all `#` comments
  and user ordering. New `SavePrefsToStreamMerging()` reads the original file, passes
  through comments/blanks, replaces known-key lines with current values, appends new
  keys. Write-to-temp + rename avoids the truncation race. Verified: comments in
  `~/.sheepshaver_prefs` survive a full E2E boot+shutdown cycle.

### [docs] SiliconSheep — full feature set shipped and documented

SiliconSheep Tauri v2 launcher progressed from scaffold to functional app:

- **First-run wizard**: 4-screen flow (Welcome → ROM picker with SHA-256 verification →
  Disk creation + optional CD → Review & Boot). ROM validation accepts compressed/trimmed
  ROMs (512K–8MB), shows "proceed anyway" for unrecognized files.
- **VM library**: Card grid with live VNC screenshots (captured every ~10s while running,
  final frame on shutdown), OS version (from `[SYSV]` hook), last-booted date.
- **Complete settings panel**: 7 sections (General, Display, Storage, Network, Input,
  Advanced with Expert fold-out, Debug). All 30+ SheepShaver prefs configurable.
  Custom resolution support (any width×height). Explainer text on all expert settings.
- **Debug panel**: GUI controls for JIT diagnostics — SS_JIT_VERIFY, SS_JIT_NO_CHAIN,
  SS_JIT_NO_ROM, SS_USE_JIT, SS_JIT_TRACE_RING, watch addresses, skip opcodes. Env vars
  passed to the emulator child process on launch.
- **Log retention**: Timestamped logs in `<vm>.sheepvm/logs/`, last 10 kept, symlink to
  latest, "View Logs" button in Debug section.
- **Drag-and-drop**: ROM files, disk images, CD images, prefs files — drop on the window,
  routed to the right context (wizard step or import).
- **Import from prefs**: "Import Prefs" button or drag-drop a `sheepshaver_prefs` file to
  create a VM from an existing config.
- **VM duplicate**: APFS `clonefile` for instant zero-copy, warns on shared external disks.
- **Disk backup**: APFS clonefile snapshot of disk images (available when VM stopped).
- **19 Rust tests** (10 prefs parser + 9 VM integration). TypeScript strict mode. Vite 8 +
  TypeScript 6. Zero compiler warnings.
- **Deps**: Vite 8.0, TypeScript 6.0, all Tauri packages at latest (2.11.x).
- **Renamed** "Silicon Sheep" → "SiliconSheep" (one word, like SheepShaver).

### [SheepShaver][BasiliskII] JIT harness & diagnostic integrity hardening (`d52d4103`, `8d101b2e`, `c5cc8b0d`)

Tooling-only (no runtime/codegen change); hardens the signals used to judge the JIT:

- **[SheepShaver] SS_JIT_VERIFY suppression latch fixed.** The differential interp-vs-JIT
  oracle used a `bool verify_suppressed` that, once set on the first divergence, gated its
  own clearing branch — so verify went silent for the rest of the run after one report.
  Replaced with a `verify_suppress_blocks` countdown decremented only on in-range (RAM)
  blocks (execution is overwhelmingly ROM at `0x50xxxxxx`, so a per-block decrement would
  drain it before any RAM block is skipped). Checking now auto-resumes. Tunable with a
  symptom guide in the code comment. (`ppc-cpu.cpp`)
- **[SheepShaver] rom-harness bench no longer blanket-masks stderr.** `make bench` shows
  the JIT engine chatter by default; masking is opt-in via `make bench BENCH_QUIET=1`. The
  bench's own warnings/errors (baseline-stale, init-fail, compile-fail) are routed to
  **stdout** so they survive even under `BENCH_QUIET=1` — the previous unconditional
  `2>/dev/null` silently hid them, including the advertised "baseline OLDER than
  ppc-jit.cpp" warning.
- **[BasiliskII][SheepShaver] preflight vacuousness guard (shallow tier).** Both
  `jit-test/run.sh` preflights now reject a vector whose body is entirely NOPs (`4E71` /
  PPC `60000000`) — it exercises only decode/dispatch and asserts nothing under the
  differential. Intentional decode/dispatch sanity vectors (`nop`, `nop_triplet`) are
  allow-listed. Verified: rejects synthetic all-NOP vectors, zero false positives across
  the real tables (B2 452, SS 264). The **deep** tier (real-opcode body whose result hides
  in an FPR/VR/memory the REGDUMP can't see) remains deferred to the `gen-*-vectors.py`
  generators / a future sentinel-mutation redesign.

### [SheepShaver] E2E harness — first-run DX: shared entry scaffolding, GUI-session + configure preflight (`3ab8bd59`)

From a developer-advocate review of the onboarding cliff:

- **New `sse2e/harness.py`** — shared entry-point scaffolding so each run script (and any new
  scenario) is a thin "check preconditions, run the drive fn": `hard_exit(code)` (the os._exit that
  stops a stuck vncdotool reactor from hanging the process — previously duplicated verbatim with a
  long comment in each script) and `check_preconditions(need_iso/need_disk)` (build + GUI + assets,
  failing fast with actionable messages). `run_smoke.py`/`run_benchmark.py` now use it.
- **GUI-session fast-fail** (`runner.gui_session_ok`, `launchctl managername == "Aqua"`,
  conservative) — a headless/SSH run now fails immediately with "no GUI session" instead of waiting
  out the 90s boot timeout on a cryptic disk error. Surfaced in `make e2e-setup` too.
- **`configure`-not-run detection** (`runner.is_configured`) — `make build-ss` fails on an
  un-configured tree; the doctor + a tracked README **"Building the emulator"** section now carry
  the one-time `configure` incantation (it previously lived only in gitignored `CLAUDE.md`,
  invisible to a fresh clone). README also gains an env-var reference table. +8 unit tests.

### [SheepShaver] E2E harness — trustworthy gates: FakeRunner tests, settle-based gate, honest PASS (`daaa1965`)

Hardens the *integrity* of the benchmark's pass/fail signal (this session exposed runs printing
PASS when the automation hadn't actually worked unattended):

- **Drive/gate logic is now unit-tested offline.** `scenario.py`'s save/quit sequence was
  extracted into testable helpers (`_save_text_report`, `_quit_to_finder`, `_await_front_app`);
  new `tests/test_scenario.py` exercises them with a **`FakeRunner`** (scripted `[APP]` log lines)
  + **`FakeVnc`** (records keys, reacts to input) + a fake clock — 14 deterministic tests, no
  emulator/boot. The most error-prone part of the harness finally has coverage.
- **`back-to-finder` gate hardened against the noisy `frontApp` signal.** It used to fire on a
  single spurious `'Finder'` frame (false confidence). `_await_front_app` now requires Speedometer
  to be ABSENT across a window of consecutive `[APP]` frames (and Finder present) — it only
  succeeds once Speedometer has really quit. Unit-tested against the exact false positive.
- **Honest benchmark PASS.** A green `make e2e-bench` now requires the guest's real clean-shutdown
  signatures (`saw_clean_shutdown`: "Shutdown complete." + the atexit session line), not just a 0
  exit code — so PASS means the harness genuinely drove an unattended shutdown. 67 unit tests pass.

### [SheepShaver] E2E benchmark history export (`cad1af88`, SS_E2E_RUNS `0dc302cf`)

`make e2e-bench` now saves Speedometer's text report in-guest (Cmd-T → Return, accepting
the default name "Power Macintosh Report"), extracts it host-side via hfsutils (no extra
boot — reads the unmounted run-copy image directly, MacRoman-decoded, hfsutils state
isolated via a throwaway HOME), and archives each run under gitignored
`SheepShaver/e2e/artifacts/benchmark-history/<timestamp>/` (raw `report.txt` + `scores.csv`
+ result PNG) plus an append-only `history.csv`, printing the CPU/Graphics/Disk/Math delta
vs the previous run. Collect+report only — a save/extract failure never flips a PASS to FAIL.
The benchmark now **shuts down unattended**: since the Power-key hook only fires at the
Finder (not over a frontmost app), the harness quits Speedometer keyboard-only (Cmd-Q →
Return through the save dialogs) to the Finder first — keyboard is reliable. (VNC *clicks*
work fine too — verified on both SDL2 and SDL3; an earlier "clicks don't register" was a
misdiagnosis, see LEARNINGS.)
**Less-noisy measurement:** `SS_E2E_RUNS=N` runs N times (each in its own subprocess — vncdotool's
Twisted reactor can't restart in-process) and prints a batch summary — the **median** per metric
+ each metric's **CV%** (run-to-run noise), flagging >5%. CV% is host-state dependent (quiet host:
CPU/Math <1%, Disk noisy; under load: everything noisy) — the honest "is this batch trustworthy?"
signal. `PR`/PowerRating is **deliberately not trended** (disk-weighted composite → inherits
Disk's noise, misleads as a perf number; also panel-only). New `sse2e/bench_export.py` (+24 unit
tests); throwaway run-copies are now cleaned up; `make e2e-setup` gains an optional hfsutils
check. Design + plan:
`docs/superpowers/specs/2026-06-05-benchmark-result-export-design.md`,
`docs/superpowers/plans/2026-06-05-benchmark-result-export.md`.

### [docs] BasiliskII → SheepShaver JIT cross-pollination triage (`33ffbea6`)

- Added `docs/planning/sheepshaver-research/BASILISKII-CROSS-POLLINATION.md`: which techniques
  from the BasiliskII 68K JIT (the more mature, same-host ARM64 lineage) are worth borrowing for
  the SheepShaver PPC JIT, filtered by whether they come over clean or drag the heavy
  lazy-state / multi-PC / lifecycle machinery in. Top borrows: verify/bisection tooling (X1,
  Track A), intra-block backward CR0-liveness as the safety proof for re-enabling lazy CR0 (X2,
  §0g), and the light §R2 inline-cache form of guarded indirect-branch resolution for `bcctr`
  (X3, §P2/§P9) — explicitly *not* the heavy edge-profiling/jmpdep apparatus.
- Wired into the trackers (bidirectional, per the doc-lifecycle rule): ROADMAP A1/B2/Track-B
  intro, and back-pointers in OPTIMIZATION-PLAN §0g/§P2/§R2.
- Includes a **fast-target-hardware re-ranking** (two independent sub-agent reviews): on Apple
  Silicon the emulator already runs the 1990s guest faster than period hardware, so the four
  non-throughput borrows (X1 tooling, X4 latency, X5 clarity, X6 methodology) top the list and
  the two throughput borrows (X2 lazy-CR0, X3 bcctr) defer — also premature by our own rule
  (gated on the unbuilt B1 execution-weighted profiler). Re-ranked order: X1 → X6 → X4 → X5.

### [shared] Repository tidy-up — relocate the BasiliskII harness, remove the superseded VNC-QA scaffold, document `cxmon/` (`22710b39`)

- **Moved `jit-test/` → `BasiliskII/jit-test/`.** The root `jit-test/` was the *BasiliskII*
  68K opcode harness (distinct from `SheepShaver/jit-test/`, the PPC one — they only shared a
  name). Relocating it gives clean per-emulator symmetry. Fixed the scripts' internal relative
  paths (`run.sh`: `../BasiliskII/src/Unix` → `../src/Unix`; `rom-harness.sh`:
  `$DIR/BasiliskII/src/Unix` → `$DIR/src/Unix`) and repointed `Makefile` (`make test-jit`),
  `autoresearch.sh`, `JIT-STATUS.md`, and `docs/planning/BasiliskII-next-phase-plan.md`.
  SheepShaver-context `jit-test/` references (run from `SheepShaver/`) are unchanged.
- **Removed the repo-level VNC/Gherkin QA scaffold** (`qa/` and `BasiliskII/qa/`). It was an
  Xvfb/Linux-oriented story-runner experiment, superseded by the macOS VNC E2E harness at
  `SheepShaver/e2e/` (isolated prefs + pristine disk, `[BOOT]`/`[READY]` signals). `BasiliskII/qa/`
  depended on root `qa/tests/vnc/`, so the two were one system and went together; BasiliskII also
  doesn't build on macOS. Repointed the surviving references (`JIT-STATUS.md`, both
  `*/docs/AARCH64_JIT_GOLDEN_WORKLOADS.md`, `BasiliskII/docs/AARCH64_JIT_BRINGUP.md`,
  `docs/planning/sheepshaver-research/COMPATIBILITY-TESTING-PLAN.md`) at `SheepShaver/e2e/`.
- **Documented `cxmon/`** with `cxmon/README.macemu.md`: it's vendored upstream source (cxmon 3.2,
  the optional `mon` debugger), pinned at the repo root because all three `configure.ac` files
  hard-code `../../../cxmon/src` for `--with-mon`; currently compiled **off** (`config.h` →
  `/* #undef ENABLE_MON */`); frozen upstream (last change 2017) — keep as-is, don't relocate.

### [SheepShaver] SDL3 is now the genuinely-built default backend (+ the bugs that surfaced)

- **The build had been silently linking SDL2.** `configure.ac` defaults to SDL3, but the *generated*
  `configure` was stale (generated before the SDL3-default flip), so every build linked SDL2 — the
  E2E harness had been validating SDL2 the whole time. Re-bootstrapped (`NO_CONFIGURE=1 ./autogen.sh`)
  so the build actually links SDL3 (`otool -L` confirms `libSDL3`). See LEARNINGS / spec §17.
- **SDL3 boot regression fixed.** The live-JIT-stats title-bar update called `SDL_SetWindowTitle`
  from the redraw thread; on macOS that Cocoa call is main-thread-only and stalled the redraw thread
  → VBL stops → guest hangs in early boot (commit `5d87d713`, the "Live JIT stats" entry below).
- **SDL3 shutdown crash fixed.** `Quit()` calls `VideoExit()` twice (directly + via `ExitAll()`);
  `VideoExit()` destroyed `frame_buffer_lock`/`sdl_palette_lock`/`sdl_events_lock` without NULLing
  them, so the 2nd pass double-destroyed freed mutexes → `os_unfair_lock is corrupt` abort (SDL2
  tolerated it; SDL3's os_unfair_lock-backed mutexes don't). Fix: NULL after `SDL_DestroyMutex`
  (`video_sdl3.cpp`, commit `3daa9c98`). Diagnosed from a user-captured crash backtrace (the earlier
  Metal-deadlock theory was wrong — host VBL degradation had masked the real bug). Spec §18.
- **VNC server ported to SDL3** (`vnc_server.cpp`, `video_sdl3.cpp`, commit `01bc52fe`). It was
  SDL2-only (`#if SDL2 && !SDL3`; the SDL3 branch was empty stubs), so on SDL3 the benchmark +
  headless screenshots failed (VNC `ConnectionRefused`). Guard widened to
  `#if SDL_VERSION_ATLEAST(2,0,0)`; SDL3 adaptations for the renamed keymod/condition-variable
  symbols, `SDL_EVENT_KEY_*`/float mouse coords, and `SDL_GetPixelFormatDetails`; the 4 call sites
  wired into `video_sdl3.cpp` mirroring SDL2. `make e2e-bench` PASS on SDL3 (port 5950,
  pixel-correct capture). **SDL3 now has full harness parity.** Spec §20.

### [SheepShaver] E2E harness — signal-gated benchmark, richer signals, robustness fixes

- **Harness no longer hangs after PASS** (commit `f55dd7f6`). vncdotool starts a non-daemon Twisted
  reactor on connect; a swallowed screenshot-connect failure skipped `close()`, leaving the reactor
  to block interpreter exit (print PASS, then hang / lingering Python). Fix: entry points
  `flush` + `os._exit(code)`; the boot screenshot always `api.shutdown()` in a `finally`.
- **Richer `[APP]`/`[BOOT]` idle signals** carry the front-window pointer (`win=`) and **title**
  (`title=`); garbage frames (background-extension pseudo-windows under cooperative MT) are
  suppressed, cutting the benchmark's signal lines from hundreds to ~32 and making the log a readable
  state story. Guest reads are sanitized (a literal `'` → backtick so it can't break the
  `frontApp='…'`/`title='…'` parsers) and bounds-checked (a wild titleHandle no longer SIGSEGVs the
  host). (`emul_op.cpp`.)
- **Benchmark drive steps are signal-gated** (`_await_since`/`_drive_until`) instead of fixed sleeps:
  each step proceeds the instant the guest reaches the next window state, prints its elapsed time
  (`[gate] step: Xs`), and self-corrects (resends a key the splash silently dropped — one continuous
  watch, no cursor race). Done-detection gates on the `All Done!` alert title (unambiguous), not a
  modal count. (`observe.py`, `scenario.py`; commits `8972c0f3`, `b170415d`.)
- **`[READY]` settled-desktop signal** — emitted once the Finder has been frontmost + non-modal for a
  ~2 s dwell (more robust than the first `[BOOT] idle`, which can fire mid-draw); carries `MBarHeight`
  for evaluation. A better "desktop actually usable" marker. (`emul_op.cpp`, `observe.saw_desktop_ready`.)
- **Pre-flight disk-availability check** (`runner.preflight`): `make e2e`/`e2e-bench` now kill **and
  reap** stray SheepShaver instances (bare `pkill` returns before the OS releases file handles) and
  refuse to launch if the boot image is still held open — so a stray session can no longer make a run
  boot to the "?" no-boot-disk icon; it fails fast with a clear message instead.

### [docs] SiliconSheep plan — Tier 4 Automation & Scripting

- Added a **Tier 4 — Automation & Scripting** section to `docs/planning/DESKTOP_INTEGRATION_PLAN.md`:
  a launcher control surface (`siliconsheep` CLI / AppleScript / Shortcuts / MCP server / headless
  CI mode), a screenshot-free guest control bridge (structured input + observation + an AppleEvents
  bridge), and a "SiliconSheep Tools" guest agent for *managed* images that reframes part of the
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

### [SheepShaver] Live JIT stats in SDL window title bar — periodic update reverted (boot-breaking on macOS)

- New `ppc_jit_aarch64_get_stats()` API exposes block count, pool size, cache used/total;
  `set_window_name()` gained an optional `status_suffix`. **The periodic title update in
  `do_video_refresh()` was removed** (both SDL2 and SDL3 backends): `do_video_refresh()` runs on
  the "Redraw Thread", and `SDL_SetWindowTitle` calls into Cocoa, which asserts *"NSWindow … should
  only be modified on the main thread!"* — aborting the emulator (SDL2) or stalling the redraw
  thread so the 60 Hz VBL stops and the guest hangs in early boot (SDL2-default build; `SS_JIT_VERIFY`
  run reproduced the abort). The getter + param are retained for a future reimplementation that
  applies the title on the **main thread** (e.g. a pending-title mailbox drained by the main loop).

### [docs] SiliconSheep — Tauri v2 launcher scaffolded + plan expanded

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

### [docs] Doc hygiene sweep — stale-claim fixes, /workspace paths, archive (`61306fa5`)

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

### [docs] Documentation lifecycle convention + status/provenance headers (`e7b444c9`)

- **New `CONTRIBUTING.md` → "Documentation Lifecycle" section**: how to keep docs honest when
  work lands — log to `CHANGELOG.md` (component-tagged), flip the item's status marker instead
  of deleting it, bump the doc's `Updated:` date, and fold-and-retire (not silently delete)
  dated specs.
- **Standardized header on all 43 docs under `docs/planning/`**: a `Status / Created / Updated /
  Why this doc exists` block plus the canonical marker legend (✅ done · 🟡 in progress ·
  ⏸ blocked/deferred · ☐ todo). Created dates recovered from git history (following renames);
  existing rich intros (ROADMAP, NEW-WORLD plan) harmonized, not clobbered.

### [SheepShaver] AltiVec even/odd byte multiplies fix — ev_mixed class complete (vmuloub/vmuleub) (`ed1fb0bd`)

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

### [SheepShaver] AltiVec vpkuhum pack fix (ev_mixed) (`d43313ac`)

- **Bug fix.** `vpkuhum` (pack 8+8 halfwords to their low bytes, modulo) was doubly wrong: it
  **ignored vA entirely** (loaded only vB) and used the wrong NEON op. PPC keeps each halfword's
  low byte = PPC byte 2i+1 = the odd byte lane in natural order, so on `REV32.16B`-normalized
  inputs that is **`UZP2.16B`** (odd-lane deinterleave; vA → result high half). Reuses the merge
  family's `emit_vmrg` helper (`REV32.16B` → permute → `REV32.16B`). Verified `xfail→xpass` with
  distinct operands, promoted to the scored gate (**261→262, score=100**). Sibling `vpkuwum`
  (word→halfword modulo pack, case 78) has the same ignore-vA bug — flagged in code + ROADMAP A2,
  not yet vectored. Remaining in the `ev_mixed` class: the even/odd byte multiplies
  (`vmuleub`/`vmuloub`).

### [SheepShaver] AltiVec byte/halfword merge fix (vmrgh/l b,h) + distinct-operand test strengthening (`023870cd`)

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

### [SheepShaver] AltiVec word-merge codegen fix (vmrghw/vmrglw) (`b1bb6b52`)

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

### [SheepShaver] Wayland detection (upstream backport) (`f5a96e0e`)

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

### [shared] Networking (`3bfa680a`)

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

### [SheepShaver] Video backend (`dda61521`)

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
  a new differential vector. (`086226f3`)

- **AltiVec `vspltb`/`vsplth` element-order fix (ev_mixed)**: byte/halfword splats
  selected the WRONG element. VRs are stored in the interpreter's `ev_mixed` byte
  order (`byte_element(i)=(i&~3)+(3-(i&3))` — bytes reversed *within* each 32-bit
  word, word order preserved), but `emit_load_vr` loads it raw via `LDR Q`, so the
  JIT indexed NEON lanes with the raw PPC element. Fixed by remapping the DUP index
  through `ev_mixed` (verified across indices 0/3/15 and 0/3/7); `vspltw`/`vsldoi`/
  element-symmetric ops were already correct. The same mismatch still affects
  `vmrg*`/`vpk*`/even-odd multiplies — parked and signposted in code (`emit_load_vr`)
  and tracked as **P1b** in OPTIMIZATION-PLAN.md (two fix approaches documented).
  Repro vectors in `jit-test/gen-altivec-vectors.py`. (`9117e789`)

- **`emit_update_cr0` cleanup (B1)**: CR0 field construction reduced from 19 to
  11 ARM64 instructions.  Replaced 3x `emit_load_imm32` + 3x CSEL + LSL + AND +
  ORR with 3x CSET + 3x shifted ADD + BFI.  Every Rc=1 instruction benefits. (`fd616e5b`)

- **LogicalImm encoder (B2)**: ARM64 bitmask-immediate encoding for AND masks in
  rlwinm, rlwimi, rlwnm, andi., andis.  Saves 1-2 instructions per masked op
  (992/1024 PPC masks are encodable).  Encoder from `ppc-logical-imm.hpp`. (`c71cd0f2`)

- **mullwo overflow detection (A3)**: Case label was 715 (wrong XO), should be
  747.  The instruction was never JIT-compiled — silently fell to interpreter.
  Now uses SMULL + ASR/CMP to detect 32-bit overflow and sets XER OV/SO. (`c71cd0f2`)

- **SS_JIT_VERIFY cascade fix**: Reduced false divergences from 20+ to 1 per
  boot.  Skips verifying blocks ending with link-setting branches (bl/bctrl),
  and suppresses cascade after any divergence until a clean block is found.
  Mixed Mode Manager dispatch causes unavoidable interpreter/JIT path divergence
  that is not a codegen bug. (`0cda0257`)

- **Software link stack (R1, partial)**: Compile-time infrastructure for
  Dolphin/RPCS3-style blr return prediction added.  Finding: `bl` always
  terminates a block, so the compile-time stack is empty by the callee's `blr`
  — the fast-path never fires.  Runtime variant (R1b) documented in the
  optimization plan as a follow-up. (`e256c871`)

### Code Quality

- **Technique attribution**: Added source credits (Dolphin, RPCS3, MAME,
  upstream PERFORMANCE_AUDIT) to all major JIT optimizations as inline code
  comments — RA, CR0 cleanup, LogicalImm, ADCS carry, mullwo, atomic spcflags,
  link stack. (`2f2347cd`)

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
  logged-in macOS GUI session (SDL needs a WindowServer; no Xvfb equivalent). `SheepShaver/e2e/`. (`acd37114`)

- **18 real FP-arithmetic test vectors**: the pre-existing `fp_*` vectors were
  *vacuous* — they ended at `stfd` and never loaded the result into a GPR, but the
  harness REGDUMP captures GPRs, not FPRs, so a wrong FP result was invisible and
  the JIT-vs-interpreter diff passed trivially. FP arithmetic was effectively
  untested. The new vectors load the result back into a GPR (fadd/fsub/fmul/fdiv,
  the fma family, frsp/fctiwz/fneg/fabs/fmr, and single-precision forms). Generated
  by `jit-test/gen-fp-vectors.py` (documented, reproducible). The 9 vacuous `fp_*`
  originals were then removed (kept `fp_lfd_stfd`/`fp_lfs_stfs` — those *do* round-
  trip the value back into a GPR, so they are real load/store tests). (`c1e26ee2`)

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
  the `gen-*-vectors.py` generators are the practical defense there. (`f49df948`)

- **`make harness-count`**: single source of truth for the harness vector count,
  derived from `jit-test/run.sh` (the count had drifted across several docs). The
  *gate* references in the testing docs (TESTING.md, OPTIMIZATION-PLAN.md,
  CLAUDE.md, CONTRIBUTING.md) were de-hardcoded to reference it; dated historical
  snapshots in session logs and baseline tables are intentionally left as-is. (`797a9242`)

- **FP microbench kernels**: `make bench` gains `fp-add`/`fp-fma`. They measure the
  FPR store/load round-trip (the JIT has no FP register allocator), not raw FP-unit
  latency — useful as the baseline an FP register allocator would improve against. (`64258de7`)

### [docs] Documentation

- **Paranoia FP conformance**: concrete manual run steps documented in TESTING.md,
  with the honest caveat that automation needs a guest binary + boot. (`6ab3f194`)

- **IMPROVEMENT-CYCLE-1.md**: prioritized, collision-aware improvement plan from a
  multi-agent audit (read-only auditors → adversarial verification → synthesis). (`4053d111`)

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
