# Roadmap / Work Tracker — `macos-arm64`

> **Status:** 🟡 Active · **Created:** 2026-06-04 · **Updated:** 2026-06-04
> **Why this doc exists:** The single tracker for all outstanding work, arranged into four tracks so context survives across pickups.


The single place to **arrange and track outstanding work** so context survives across
pickups (when we focus on one task we don't lose the others). When you pick up or finish a
task, update its **Status** line. This is the **map**, not the territory — deep analysis,
design, and per-item detail live in the linked docs. Keep entries to a few lines + a pointer.

**Legend:** 🔜 next up · 🟡 open · ⏸ deferred/optional · ✅ done

---

## Workstreams at a glance

The project has grown four parallel tracks. They are largely independent — pick by appetite,
not by order. The only hard sequencing is *within* a track (noted per item).

| # | Track | What it buys | State | Lead doc |
|---|-------|--------------|-------|----------|
| **A** | **Correctness & verification** | Trust that the booting emulator isn't silently wrong | 🔜 active front | `docs/TESTING.md` |
| **B** | **Performance / JIT optimization** | Faster guest execution | 🟡 cheap wins landed; big levers open | `docs/planning/OPTIMIZATION-PLAN.md` |
| **C** | **Desktop integration ("Silicon Sheep")** | A Parallels-like, first-class macOS app experience | ⏸ researched, not started | `docs/planning/DESKTOP_INTEGRATION_PLAN.md` |
| **D** | **Platform breadth** | BasiliskII/68K on macOS; Linux/ARM re-convergence | ⏸ optional | per-item below |

> **Cross-cutting theme — verification is the bottleneck, not the fix.** The `vsel` bug, the
> AltiVec `ev_mixed` bugs, and the "vacuous FP vectors" all came from one root: the harness
> silently passed wrong codegen (results hid in VRs/FPRs/memory the REGDUMP doesn't check).
> Track **A** pays off across every other track — especially **B**, where a perf change that
> corrupts state must be caught, not shipped.

> **⚠️ Cross-cutting decision (gates C *and* D) — do we hard-fork?** Track C's
> `docs/planning/DESKTOP_INTEGRATION_PLAN.md` proposes a pruned, Apple-Silicon-only hard fork ("Silicon
> Sheep") that *removes* BeOS/AmigaOS/Windows/SDL1 and tracks upstream by cherry-pick only.
> That premise is **in direct tension with Track D**: a pruned macOS-only fork is unlikely to
> invest in D1 (68K BasiliskII) or D2 (Linux/ARM re-convergence + VDE test rig). Decide the
> repo's identity — *is this branch the product, or the seed of a separate fork?* — **before**
> starting either C-Tier-1 or any D backport, because the answer can invalidate the other track.

---

# Track A — Correctness & verification  ← *picking up first*

## A1. 🟡 Testing infrastructure — trustworthy JIT verification *(in progress)*

**Progress (2026-06-04):** ✅ REGDUMP now dumps all 32 **FPR** (raw 64-bit) + 32 **VR** (raw
128-bit); `run.sh` raw-diffs the line, so `make test-jit` finally compares FP/AltiVec results
(interp vs JIT). Still **255/255 score=100** — which now *proves* the current suite has no
hidden FP/VR divergence (commit `9439666a`). Side effect: FP/VR results are no longer
"vacuous" (they're captured), so the vacuousness-guard scope below narrows to memory-only /
otherwise-unobservable results.
✅ **Quarantine lane added + widened to the full worklist** (commits `4f5825bd`, `aef9fb34`):
`QUARANTINE_ORDER` vectors run in JIT mode but don't count toward score, so `score=100` stays
meaningful while confirmed bugs are *tracked*. Originally held **all 9** `ev_mixed` divergences;
all 9 (merges `vmrgh{b,h,w}`/`vmrgl{b,h,w}`, pack `vpkuhum`, byte multiplies `vmulo/eub`) have
since been fixed and promoted — **the quarantine lane is now empty**. The mechanism (a fix flips
its vector `xfail`→`xpass` and `make test-jit` prints "promote to TEST_ORDER") stays available for
the next confirmed divergence; it was also used to rule out approach A — see A2.
✅ **`SS_JIT_VERIFY` now compares FPR + VR** (commit `6837f642`): the boot-time oracle catches
FP/AltiVec divergence in real software (`SS_JIT_VERIFY=1 ./SheepShaver`), not just GPR/flags —
the in-the-wild validation path for A2.
**Remaining A1:** ✅ the merge vectors are now on **distinct operands** (vA=`00..0F`, vB=`10..1F`)
— done as part of the A2 byte/hw merge fix. Still open: the scored **arithmetic** word-op vectors
(`vadduwm`/`vsubuwm`/`vmaxsw`/…) use uniform palindrome operands (`0x05..`/`0x03..`) that can't
catch a byteswap/lane bug — regenerate with **distinct AND carry-inducing** operands before
trusting any *broad* VR-codegen change. Safe + here-verifiable.

**Why:** the recurring failure mode (above). Three rounds of vacuous/masking AltiVec vectors
slipped the harness; FP vectors were vacuous for months. Fixing more codegen on top of a
harness that can't catch mistakes just produces the next silent bug.

**Scope (✅ = landed this cycle):**
- ✅ **REGDUMP compares FPR + VR** (harness) and ✅ **`SS_JIT_VERIFY` compares FPR + VR** (boot
  oracle) — the two blind spots that hid every FP/AltiVec bug are closed.
- ✅ **Quarantine lane** tracks the `ev_mixed` divergences as `xfail` (the A2 gate); down to 3
  (pack + 2 multiplies) after the merge family landed.
- 🟡 **Stronger AltiVec operands** — ✅ done for the *merges* (distinct operands). Still open for
  the scored **arithmetic** word-ops: regenerate with **distinct AND carry-inducing** operands
  (small-distinct still masks byteswap-carry; see A2 testing-gap note). Needed before any *broad*
  VR change.
- 🟡 **Vacuousness guard** on `run.sh` — now narrowed to **memory-only / otherwise-unobservable**
  results (FP/VR are captured). Still needs a sentinel/mutation design; lower urgency now.
- 🟡 **Broaden AltiVec coverage** — the suite has ~13 scored AltiVec + 9 quarantined ops out of
  ~100+; most AltiVec ops are still untested. Grow `gen-altivec-vectors.py` alongside A2.
- 🟡 **Paranoia FP conformance** runner wiring + CI (18 in-harness FP vectors landed; the
  self-grading torture run is still manual — needs a boot rig + disk image).
- ⏸ **(stretch) golden-result oracle** — revive the PowerPC Emulator Tester against recovered
  G4 golden results (catches bugs shared by *both* interp and JIT). See IMPLEMENTATION-BACKLOG T1.

**Verifiable here** (harness/build, no boot needed). **Unblocks A2 and de-risks all of Track B.**
**Detail:** `docs/TESTING.md`; harness `SheepShaver/jit-test/run.sh`; generators `gen-*-vectors.py`.

---

## A2. ✅ AltiVec `ev_mixed` correctness — element-order class COMPLETE (tested ops; boot-pending)

**Why:** **silent data corruption in the emulator that works** (SheepShaver). AltiVec is live
(the emulator advertises a G4). Only triggers when guest software uses the affected ops
(media codecs, AltiVec-era apps) — latent, not a boot-blocker, but the worst failure mode.

**Status:** splats (`vspltb`/`vsplth`) FIXED + merged; `vspltw`/`vsldoi` were already correct.
✅ **ALL merges `vmrgh/l {b,h,w}` FIXED + boot-verified + promoted (2026-06-04).**
- *Word merges* (`vmrghw`/`vmrglw`): the 6 `vmrgh*`/`vmrgl*` encodings were garbage
  (`0x..C400`/`0x..C800` — not permute ops); replaced with correct `ZIP1`/`ZIP2`. Word merges
  are correct as a plain `ZIP.4S` (`word_element` is identity under `ev_mixed`, no byte remap).
- *Byte/halfword merges* (`vmrgh{b,h}`/`vmrgl{b,h}`): needed more than the encoding. The fix is
  the **`REV32.16B` normalize** sequence (`emit_vmrg` in `ppc-jit.cpp`): the `ev_mixed` layout
  (bytes reversed within each 32-bit word) is *exactly* `REV32.16B` vs natural element order, so
  `REV32.16B` both inputs → `ZIP1`/`ZIP2.{16B,8H}` → `REV32.16B` back. (This is the **per-op**
  version of approach A — a *local* normalize works where the *global* load/store one failed,
  see below.)
- Verified `xfail→xpass` with **distinct operands** (vA=`00..0F`, vB=`10..1F`), promoted to the
  scored gate (**255→261, score=100**), and boot-verified under `SS_JIT_VERIFY` (zero VR/FPR
  divergence; the lone GPR/LR/PC divergence is the documented `blr`-boundary false positive).

✅ **pack `vpkuhum` FIXED + promoted (2026-06-04, boot-pending):** it ignored vA *and* used the
wrong op; the low-byte modulo pack is `UZP2.16B` on the `REV32.16B`-normalized inputs (reuses
`emit_vmrg`). xfail→xpass with distinct operands, scored gate 261→262.

✅ **even/odd byte multiplies `vmuloub`/`vmuleub` FIXED + promoted (2026-06-04, boot-pending):**
two bugs — non-widening `MUL.8B` (must widen 8×8→16) and no ev_mixed even/odd select. Fix
(`emit_vmul_byte`): `REV32.16B` normalize → `UZP1`(even)/`UZP2`(odd)`.16B` select → `UMULL.8H`
widen → `REV32.8H` output. Distinct operands exercise BOTH bugs (even-lane products >255 catch a
non-widening op). xfail→xpass, scored gate 262→264. **The ev_mixed quarantine lane is now EMPTY.**

**Remaining siblings (untested, no test vector — NOT in quarantine):** halfword multiplies
`vmul{o,e}{u,s}h` (need the hw→word analogue: `UZP` on `.8H` + `[SU]MULL.4S`; `word_element` is
identity so no output rev), the word pack `vpkuwum` (ignores vA like `vpkuhum` did), and signed
byte multiplies `vmulosb`/`vmulesb` (share `emit_vmul_byte` with `SMULL`, emitted as *prospective*
— no signed test vector). All flagged in `ppc-jit.cpp` + tracked here. **Next A2 step:** write
distinct/signed test vectors for these and verify (overlaps A1 "broaden AltiVec coverage").

**Root cause:** VRs are stored in the interpreter's `ev_mixed` byte order (bytes reversed
within each word). `emit_load_vr` loads raw via `LDR Q`; the JIT then indexes NEON lanes with
the raw PPC element → wrong element for any sub-word-rearranging op.

**⛔ Approach A (REV32.16B normalize at load/store, GLOBAL) — RULED OUT (2026-06-04).**
Tried on a throwaway branch: added `REV32.16B` to `emit_load_vr`/`emit_store_vr` + reverted the
splat remap, built, ran the harness. Result: **all 9 quarantine vectors stayed `xfail`**, and
`score=100` was *misleading* (the masking gap below). A *blanket* load/store transform changes
the in-JIT VR convention for every op at once and re-breaks the already-correct ones; it is the
wrong granularity.

**✅ Approach B (per-op) is the path — CONFIRMED for the merges.** Fix each op's actual NEON
sequence locally. The merges proved the technique: a **per-op, local** `REV32.16B` normalize
(`emit_vmrg`) is exactly approach A applied *inside one op*, and it works — the global version
failed only because it was global. Remaining targets, same method (derive against the
interpreter, flip the quarantine vector `xfail→xpass`):
- ✅ **`vpkuhum`** (halfword→byte pack): DONE — `REV32.16B` normalize + `UZP2.16B` + `REV32.16B`
  back (low byte = odd lane). `vpkuwum` (word→halfword) is the same shape, un-vectored.
- ✅ **`vmuleub`/`vmuloub`** (even/odd byte multiply): DONE — `emit_vmul_byte` (REV32.16B → UZP1/
  UZP2 select → UMULL.8H → REV32.8H). Signed/halfword siblings remain untested (above).

**⚠️ Testing gap found (A1 follow-up):** the scored word-op vectors (`av_vadduwm`/`vsubuwm`/
`vmaxsw`/…) use **uniform operands** (`0x05050505`/`0x03030303`) which are byteswap-palindromes,
so they can't catch a byteswap/lane bug — that's why approach A falsely scored 100. Regenerate
them with **distinct AND carry-inducing** operands (small-distinct like `00..0F`/`10..1F` still
masks it — sums don't carry across byte boundaries, so byteswap stays invisible; use values that
force inter-byte carries). Only matters for *broad* VR-codegen changes, not per-op approach B.

**Order of attack (simplest → hardest):** ✅ word merges → ✅ byte/halfword merges → ✅ pack
(`vpkuhum`) → ✅ byte multiplies (`vmuleub`/`vmuloub`). **All tested ops done.** Leftover:
write test vectors for the untested siblings (halfword multiplies, `vpkuwum`, signed byte
multiplies) and verify — overlaps A1 "broaden AltiVec coverage".

**Done when:** ✅ all quarantine vectors `xpass`+promoted (quarantine now empty) — DONE for the
tested class. **Still pending:** (a) the `SS_JIT_VERIFY=1` boot for `vpkuhum` + the multiplies
(new `UMULL.8H`/`REV32.8H`/`UZP` instructions the merge boot never ran), and (b) real-AltiVec
software validation (→ A3). The harness flip is necessary but not sufficient — its input coverage
is one pattern per op.

**Depends on:** A1. **Needs boot verification** (yours) — the harness AltiVec coverage is partial.
**Detail:** `docs/planning/OPTIMIZATION-PLAN.md` §P1b/P5c; `CHANGELOG.md` [SheepShaver] 2026-06-04; `ppc-jit.cpp`.

---

## A3. 🟡 Validation gaps in shipped features

Shipped as build/boot-verified, **not exercised**:
- **VDE networking** — compiles + links, but never network-tested (does it actually move packets?).
- **SDL3 default** — boots to Finder, but not put through real app use / mode changes / fullscreen.
- **Wayland fix** (`91d58b12`) — inert on 64-bit; needs a **32-bit ARM / real-addressing rig**
  to exercise (`docs/UPSTREAM-LINEAGE-SYNC.md` §6.1; overlaps Track D).
- **AltiVec under real software** — run LAME/QuickTime/Photoshop under `SS_JIT_VERIFY` to catch
  real `ev_mixed` breakage in the wild (depends on A1's VR/FPR verify). See `docs/TESTING.md`.

---

## A4. 🟡 Diagnostics trustworthiness & build hygiene

Verification-adjacent: noisy/miscalibrated diagnostics undermine the boot-time oracle work in
A1. The heartbeat warning matrix is miscalibrated — `comp frozen` WARN **false-fires on a
healthy idle desktop**, training readers to ignore red — and its thresholds live un-synced in
three places. Plus a set of cheap build/diag-hygiene fixes (gate the interp-site heartbeat in
JIT mode, complete macOS `make clean`, fix the mislabeled `hb-test.cpp`, Linux RSS latch, window
the OTH rule, prune `/tmp` logs, quiet-mode env gate).
**Detail:** `docs/planning/sheepshaver-research/research/IMPLEMENTATION-BACKLOG.md` Tier D
(+ correctness items A6/A7 there: duplicate SMC-invalidation call, XO63 FP-control semantics).

---

## A5. 🔜 End-to-end VNC test harness — system-level boot/run/shutdown regression

**Why:** A1/A2 verify the JIT *per instruction*; nothing automatically checks the emulator *as a
running system* — "does it still boot to the Finder and shut down cleanly after a codegen
change?" That's today a manual human-at-VNC step. This is the highest-leverage item for **agent
autonomy in testing**: it turns the one check agents currently *can't* self-serve (boot/run) into
a scriptable pass/fail, so an agent can validate its own JIT change end-to-end instead of handing
every boot back to the user.

**Approach:** spawn SheepShaver with an *isolated* config (own prefs + pristine-disk-per-run),
drive it over the **built-in bidirectional VNC server** (`vnc_server.cpp` already wires
kbd/ptr→ADB), and assert with **hybrid observability** — host-log signals as the deterministic
spine (booted / exit 0 / atexit report) + masked perceptual-hash screenshots as a tolerant visual
gate (exact frame-hash rejected as flaky). Canonical v1: `boot → Special ▸ Shut Down → assert
clean exit` (also regression-tests the 2026-06-03 clean-shutdown feature).

**Effort:** P1 (lifecycle smoke) ~1–2 days; P2 (golden-diff + app launch) +1–2 days; P3 (scenario
framework) larger, only if earned. Needs a small pristine test disk image (asset) + `vncdotool`/
`imagehash` deps. **Policy:** relaxes the no-boot rule to "ask the user first" (done in
CONTRIBUTING + CLAUDE.md). **Complements** A1's boot-rig need (Paranoia FP conformance runner).
**Detail:** `docs/superpowers/specs/2026-06-04-e2e-vnc-harness-design.md`.

---

# Track B — Performance / JIT optimization

Full plan, with per-lever effort/payoff and measured baselines, lives in
**`docs/planning/OPTIMIZATION-PLAN.md`** (and the Dolphin/RPCS3/Box64/FEX survey items therein +
`docs/planning/sheepshaver-research/research/IMPLEMENTATION-BACKLOG.md`). The cheap, ready wins from the first
pass have largely **landed** (subfe/adde via ADCS, mullwo, CR0 cleanup/B1, LogicalImm/B2, code-cache
sizing, atomic spcflags). What remains is the bigger, measurement-gated work. Tracked here as
buckets so they don't fall off the map:

## B1. 🟡 P0 — Execution-weighted profiler (gates everything below)

**Why:** prioritize the remaining levers by *real* hot-block / instruction-mix data instead of
guessing. Nothing else in Track B should be tuned blind. **Unblocks B2–B4.**
**Detail:** `docs/planning/OPTIMIZATION-PLAN.md` §P0.

## B2. 🟡 Medium levers — fallback & branch handling

- **Native `bcctr`** (98.5% of JIT misses) — complex; needs Mixed-Mode-Manager RE; gated on B1.
- **Reduce interpreter fallbacks** (`lwarx`/`stwcx`/`mftb`/`isync` native) — ~5–10%.
- **`isync` inline BLR** (0c), **lazy CR0 re-enable** (0g — was disabled after a boot regression;
  needs A1's boot verify first).
**Detail:** `docs/planning/OPTIMIZATION-PLAN.md` §P2/P3/0c/0g.

## B3. 🟡 High-effort levers

Constant folding, FP register allocator, instruction scheduling, byte-swap opt, **cross-block
register pinning** (r1/SP, r2/RTOC — P8), **bclr indirect-branch chaining** (P9).
**Detail:** `docs/planning/OPTIMIZATION-PLAN.md` §P5–P9.

## B4. 🟡 Strategic / from-research levers

Runtime link stack (R1b), **dual W^X mapping** (R8 — ~27% compile speedup measured), async
background JIT compilation (R9, Cemu model), Metal framebuffer compositing (R10), JIT-residency
gate restructure (C1), selective HLE of hot routines.
**Detail:** `docs/planning/OPTIMIZATION-PLAN.md` (research section + HLE) + `IMPLEMENTATION-BACKLOG.md` Tier C.

**Regression tracking:** baselines + how to A/B → `docs/BENCHMARKS.md` (Speedometer baseline
recorded 2026-06-04, 1.88× over interp; MacBench 5.0 + app-launch timings still TODO) and
`SheepShaver/rom-harness/` (`make bench`).

---

# Track C — Desktop integration ("Silicon Sheep") — the Parallels-like experience

⏸ **Researched, not started.** A complete multi-tier plan for turning the raw emulator into a
first-class macOS app — guided first-run, VM library, hot-reloading prefs, multi-folder shared
volumes, and (research-grade) coherence/seamless windows. This is a **large, JIT-independent
work stream**; it can proceed in parallel with Tracks A/B once someone picks it up.

**Tiers (full detail in `docs/planning/DESKTOP_INTEGRATION_PLAN.md`):**
- **C1. Tier 1 — First-run & VM management:** ROM picker + SHA verify, disk creation wizard, OS
  selector, **VM profile library**, **prefs hot-reload** (kill the quit/relaunch cycle),
  fullscreen-escape affordance, dark mode, Gatekeeper signing/notarization.
- **C2. Tier 2 — Enhanced integration:** multi-folder shared volumes (extend `extfs` beyond one
  RootPath), live mount/unmount, Unicode (utxt/UT16) clipboard fix, clipboard status, network
  assistant, display-scaling controls.
- **C3. Tier 3 — Coherence Lite (novel research):** parse the guest `WindowList` global → host
  title-bar overlay, per-window Dock entries; **true seamless windows** is low-feasibility
  near-term (no guest agent exists for Mac OS 9).

**Framework decision (recorded in the plan):** extend the existing Cocoa/ObjC launcher — not
Tauri/Qt/SwiftUI/Electron. **Repo strategy:** the plan also proposes a pruned hard-fork
("Silicon Sheep") tracking `kanjitalk755` + `rcarmo` as cherry-pick remotes — revisit whether
that fork happens or this branch *is* it before starting Tier 1.
**Detail:** `docs/planning/DESKTOP_INTEGRATION_PLAN.md`.

---

# Track D — Platform breadth (optional)

## D1. ⏸ BasiliskII on macOS — independent of SheepShaver

**What B2 is for:** emulating **68K Macs / classic Mac OS 7.x–8.x** — a different machine class
from SheepShaver (PowerPC, 8.x–9.x). **Not needed for SheepShaver** (separate build trees; SS
compiles none of B2's 68K JIT). B2 has been broken on macOS for a while with zero effect on SS.

**Status:** does not build on macOS. Configure host-routing + `main_unix.cpp` Linux-isms fixed;
the 68K JIT backend (`compemu_support_arm.cpp`) is unported (Linux `uc_mcontext`, undeclared
`uae_vm_jit_write_protect` W^X, `uae_vm_page_size`, `_XOPEN_SOURCE`).

- **D1a. Interpreter-only build** — cheap/mechanical; restores B2 usability on macOS *now*. 68K
  on Apple Silicon doesn't need a JIT for speed. Likely just the mechanical guards + `--disable-jit`.
- **D1b. JIT-backend port** — optional perf; mirror SheepShaver's *proven* macOS W^X
  (`jit-target-cache.hpp`). Higher effort + heavy boot verification.

**Detail:** `docs/planning/BasiliskII-MACOS-AARCH64-JIT-PORT.md`.

## D2. ⏸ Linux / ARM re-convergence (deferred backlog)

Upstream backports left on the shelf until a dedicated Linux pass: sheep_net `module_init`
modernization (`650d3a82`), `linux/sched.h` guard (`6787dce8`), etherhelpertool malloc-leak fix
(`db897d0d`), REAL_ADDRESSING kernel-relative reads (`533cf6fa`), and a **Parallels Ubuntu ARM64
rig** to validate the Linux JIT + VDE (also exercises the Wayland fix from A3).
**Detail:** `docs/UPSTREAM-LINEAGE-SYNC.md` §6 / §6.1.

---

## ✅ Done (recent — for context, newest first)
- AltiVec `ev_mixed`: **entire tested element-order class fixed** — splats, merges `vmrgh/l
  {b,h,w}`, pack `vpkuhum`, byte multiplies `vmulo/eub` — promoted to the scored gate (**264/100**,
  quarantine empty; merges boot-verified, pack+multiplies boot-pending). Per-op `REV32.16B`
  normalize: merges=ZIP, pack=UZP2 (`emit_vmrg`); byte mults=UZP1/2+UMULL.8H+REV32.8H
  (`emit_vmul_byte`). Untested siblings (halfword mults, `vpkuwum`, signed byte mults) flagged.
- Doc hygiene: fork-wide `CHANGELOG.md`, `docs/ARCHITECTURE.md` extracted, handoff docs retired
  (durables → DIAGNOSTICS/LEARNINGS), harness counts de-hardcoded, memories pruned.
- Upstream backports: **VDE networking**, **SDL3 default backend** (boot-verified), **Wayland
  detection** — all from kanjitalk755 (`docs/UPSTREAM-LINEAGE-SYNC.md`).
- JIT correctness: AltiVec `vsel` + splat fixes; subfe/adde carry-out; mullwo.
- JIT perf (cheap wins): CR0 cleanup, LogicalImm encoder, code-cache sizing, atomic spcflags,
  trailing-MOV elimination, register allocator (P1). See `docs/planning/OPTIMIZATION-PLAN.md` §Completed.
- BasiliskII: configure host-routing fix + `main_unix.cpp` guards (partial — see D1).
