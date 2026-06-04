# Roadmap / Work Tracker — `macos-arm64`

The single place to **arrange and track outstanding work** so context survives across
pickups (when we focus on one task we don't lose the others). When you pick up or finish a
task, update its **Status** line. Deep analysis lives in the linked docs — this is the map.

**Legend:** 🔜 next up · 🟡 open · ⏸ deferred/optional · ✅ done

> **Cross-cutting theme — verification is the bottleneck, not the fix.** The `vsel` bug, the
> AltiVec `ev_mixed` bugs, and the "vacuous FP vectors" all came from the same root: the
> harness silently passed wrong codegen (results hid in VRs/FPRs/memory the REGDUMP doesn't
> check). Investing in test trustworthiness pays off across every other task below.

---

## 1. 🔜 Testing infrastructure — trustworthy JIT verification  ← *picking up first*

**Why:** the recurring failure mode (above). Three rounds of vacuous/masking AltiVec vectors
slipped the harness; FP vectors were vacuous for months. Fixing more codegen on top of a
harness that can't catch mistakes just produces the next silent bug.

**Scope:**
- **Vacuousness guard** on `SheepShaver/jit-test/run.sh` — reject any vector whose checked
  result can't actually move (result must reach a GPR the REGDUMP captures, or be otherwise
  asserted). The integrity preflight (duplicate-name detection) exists; the vacuousness guard
  is still deferred — it needs a sentinel/mutation design.
- **Confirm/extend `SS_JIT_VERIFY`** to compare **VR and FPR** state, not just GPRs — so the
  differential oracle catches vector/FP corruption directly (sidesteps the vacuous-vector
  problem entirely for boot-time verification).
- A **trustworthy AltiVec differential** built on the above.

**Verifiable here** (harness/build, no boot needed). **Unblocks #2.**
**Detail:** `docs/TESTING.md`; harness `SheepShaver/jit-test/run.sh`; generators `gen-*-vectors.py`.

---

## 2. 🟡 AltiVec `ev_mixed` correctness — finish the element-order class

**Why:** **silent data corruption in the emulator that works** (SheepShaver). AltiVec is live
(the emulator advertises a G4). Only triggers when guest software uses the affected ops
(media codecs, AltiVec-era apps) — latent, not a boot-blocker, but the worst failure mode.

**Status:** splats (`vspltb`/`vsplth`) FIXED + merged; `vspltw`/`vsldoi` were already correct.
**Still wrong:** merges (`vmrghb`/`vmrglb`/`vmrghw`/`vmrglw`), pack (`vpkuhum`), even/odd byte
multiplies (`vmuleub`/`vmuloub`). `vmuloub` *also* emits non-widening `MUL.8B` instead of `UMULL.8H`.

**Root cause:** VRs are stored in the interpreter's `ev_mixed` byte order (bytes reversed
within each word). `emit_load_vr` loads raw via `LDR Q`; the JIT then indexes NEON lanes with
the raw PPC element → wrong element for any sub-word-rearranging op.

**Approach (decision):** prefer **normalizing at the boundary** — `REV32.16B` in
`emit_load_vr`/`emit_store_vr` so all ops see natural order and the whole class is fixed at
once (vs. per-op remapping = whack-a-mole the harness can't police). Cost: 2 NEON ops per VR
load/store + revert the splat remap + re-verify every op. Do **after #1**; measure perf.

**Depends on:** #1. **Needs boot verification** (yours).
**Detail:** `CHANGELOG.md` [SheepShaver] 2026-06-04 (ev_mixed blast radius); `ppc-jit.cpp`.

---

## 3. ⏸ BasiliskII on macOS — OPTIONAL, independent of SheepShaver

**What B2 is for:** emulating **68K Macs / classic Mac OS 7.x–8.x** — a different machine class
from SheepShaver (PowerPC, Mac OS 8.x–9.x).

**Do we need it for SheepShaver? NO.** Separate build trees (`BasiliskII/src/Unix` vs
`SheepShaver/src/Unix`); SheepShaver compiles **none** of B2's 68K JIT (the broken
`compemu_support_arm.cpp` is B2-only). The only overlap is the shared Unix platform layer
(`ether_unix.cpp`, prefs, video, sigsegv), which builds fine. **B2 has been broken on macOS
for a while with zero effect on SheepShaver** — we just build/run SheepShaver directly. So B2
is a standalone "also support 68K Macs" deliverable, not a blocker.

**Status:** does not build on macOS. Configure host-routing + `main_unix.cpp` Linux-isms fixed;
the 68K JIT backend (`compemu_support_arm.cpp`) is unported (Linux `uc_mcontext`,
undeclared `uae_vm_jit_write_protect` W^X, `uae_vm_page_size`, `_XOPEN_SOURCE`).

**Two sub-paths (pick based on whether 68K perf matters — on M-series it largely doesn't):**
- **3a. Interpreter-only build** — cheap/mechanical; restores B2 usability on macOS *now*.
  68K on Apple Silicon doesn't need a JIT for speed. Likely just the mechanical guards + a
  `--disable-jit` config; confirm the interpreter path compiles clean.
- **3b. JIT-backend port** — optional performance; mirror SheepShaver's *proven* macOS W^X
  (`jit-target-cache.hpp`: MAP_JIT + `pthread_jit_write_protect_np` + `sys_icache_invalidate`).
  Higher effort (wire W^X into B2's larger code-cache lifecycle; unknown cascade depth) and
  needs heavy boot verification.

**Detail:** `BasiliskII/docs/MACOS-AARCH64-JIT-PORT.md` (remaining errors + pick-up plan).

---

## 4. 🟡 Validation gaps in shipped features

Shipped as build/boot-verified, **not exercised**:
- **VDE networking** — compiles + links, but never network-tested (does it actually move packets?).
- **Wayland fix** (`91d58b12`) — inert on 64-bit; needs a **32-bit ARM / real-addressing rig**
  to exercise (`docs/UPSTREAM-LINEAGE-SYNC.md` §6.1).
- **SDL3 default** — boots to Finder, but not put through real app use / mode changes / fullscreen.

---

## ✅ Done (recent — for context, newest first)
- Doc hygiene: fork-wide `CHANGELOG.md`, `docs/ARCHITECTURE.md` extracted, handoff docs retired
  (durables → DIAGNOSTICS/LEARNINGS), harness counts de-hardcoded, memories pruned.
- Upstream backports: **VDE networking**, **SDL3 default backend** (boot-verified), **Wayland
  detection** — all from kanjitalk755 (`docs/UPSTREAM-LINEAGE-SYNC.md`).
- JIT: AltiVec `vsel` + splat fixes; optimizations B1 (CR0 cleanup), B2 (LogicalImm), A3 (mullwo).
- BasiliskII: configure host-routing fix + `main_unix.cpp` guards (partial — see #3).
