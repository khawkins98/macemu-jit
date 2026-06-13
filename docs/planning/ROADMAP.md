# Roadmap / Work Tracker — `macos-arm64`

> **Status:** ⏸ PAUSED 2026-06-13 (resume entry: `docs/HANDOFF.md`) · **Created:** 2026-06-04
> **Current state (header budget = 5 lines):** SheepShaver boots 8.6 to Finder, full native
> JIT (stable). Machine Layer: M10+M11a COMPLETE — `SS_PROBE_68K=0x5000ed08` fires via CGRP
> delivery (3/3 clean), r24-stability confirmed by NK static RE. **Next: M11 framebuffer.**
> Live frontier: `docs/AGENT-CONTEXT.md`.

---

## Project arc

| Phase | Thrust | State |
|-------|--------|-------|
| **1. Foundation** | Native AArch64 JIT on macOS — SheepShaver boots Mac OS 8.6/9 to Finder | ✅ done |
| **2. Instrumentation** | Differential harness, E2E boot/workload harness, guest-UI introspection, benchmarks | ✅ done (maintained) |
| **3. Widen emulation** | Mac OS 9.2.x via Machine Layer (real device models); AltiVec reachable ✅ | 🟡 active |
| **4. Optimize** | Per-block overhead, cross-block pinning, HLE — gated by Phase-2 benchmarks | 🟡 levers open |
| **Silicon Sheep** | Tauri launcher/VM manager (Track C) | ⏸ deferred |

Reference: [DingusPPC](https://github.com/dingusdev/dingusppc) for full PPC-Mac-stack
fidelity (interpreter-only; GPLv3 reuse feasible — cite per backport hygiene; never PR upstream).

---

## Workstreams at a glance

| # | Track | State | Lead doc |
|---|-------|-------|----------|
| **A** | Correctness & verification | 🔜 active | `docs/TESTING.md` |
| **B** | JIT perf optimization | 🟡 levers open | `docs/planning/OPTIMIZATION-PLAN.md` |
| **C** | Desktop integration (Silicon Sheep) | ⏸ deferred | `docs/planning/DESKTOP_INTEGRATION_PLAN.md` |
| **D** | Machine Layer / platform breadth | 🟡 active (M9) | `docs/planning/MACHINE-LAYER-PLAN.md` |

---

# Machine Layer milestones

## M9: VIA-IFR — PARTIALLY COMPLETE (2026-06-12 session 4)

- [x] **Infrastructure**: OP_IRQ_NW + M68kRegisters.pc writeback (harness 353/353)
- [x] **Stall fixed**: ROM patch at 0x5000ed08 was corrupting NK boot-time data. Removed.
  `dec_expiries≈1577, irq_fired≈376` on baseline with full env-on cluster.
- [ ] **Verify**: `SS_PROBE_68K=0x5000ed08` fires — **deferred to M10** (structural blocker)
- [ ] **Ship**: one-line CHANGELOG; retire pre-M7 default-ON gates

Gate: `SS_NW_VIA_IFR` (currently a no-op — kept for M10 use). See `docs/HANDOFF.md` §S4.

**Why the probe criterion moves to M10:** The NK EXT handler at 0x50314880 checks `r11.bit16`
(PR = user-mode bit) before routing to CGRP. The DR emulator runs in kernel mode
(SS_M6A_USER_MSR=0, quarantined), so PR=0 always → EXT always takes the fallback
(0x50313ab0), CGRP path unreachable. The 68k handler at 0x5000ed08 cannot fire until M10
fixes user-mode DR AND initializes CGRP. Full root cause: `docs/HANDOFF.md` §Session 4.

## M10: User-mode DR + CGRP initialization → 68k interrupt handler fires ✅ COMPLETE

**Completed:** 2026-06-13. Gate: `SS_M10_CGRP=1`.
**Acceptance:** `SS_PROBE_68K=0x5000ed08:5` fires — confirmed match=1/5, no crash
(run `m10-retry1`, 60s SIGTERM clean). Harness: 353/353. See CHANGELOG 2026-06-13.

**What shipped:**
- CGRP TABLE/STACK/DESC/STUB in guest RAM at 0x68ffc210–68 (rom_patches.cpp, gated `SS_M10_CGRP`)
- CGRP+0x20 = 0x503143a0 + *(KDP-0x338) = 0x68ffc1c0 armed at first DR68K dispatch (ppc-cpu.cpp)
- EXT shim re-syncs CGRP on every delivery (sheepshaver_glue.cpp)
- STUB: A7 guard + 68k exception frame push (SR=0, PC=interrupted PC from live r24) + DR_WARM branch

**Open tail resolved (M11a, 2026-06-13):** Static RE confirmed r24 is never clobbered by
NK between EXT entry and CGRP RFI. `mr r12, r24` in the STUB is always correct. The
non-deterministic crash was speculative; 3/3 × 90s acceptance runs produced probe match
and no SIGSEGV. No code change needed. M11a → COMPLETE.

## M11: Framebuffer (recon done, not started)

Recon complete — a visible screen is itself an instrument.
See `docs/planning/machine/FRAMEBUFFER-RECON.md` (HOLD — verify M5 framebuffer status first).
Status: M10 complete; M11 can begin.

## M11+: CFM / Process Manager / drivers (unscoped)

~3-4 milestone-class efforts remaining to Mac OS 9.2.x Finder. Measure the wall count
with the QEMU rig before scoping (the QEMU wall census — unblocked).

---

# Track A — Correctness & verification

## A1 open: strengthen AltiVec arithmetic vector operands

The scored arithmetic word-op vectors (`vadduwm`/`vsubuwm`/`vmaxsw`/…) still use
byteswap-palindrome operands that can't catch a byteswap/lane bug. Regenerate with
**distinct AND carry-inducing** operands before trusting any broad VR-codegen change.

## A2 open: signed byte multiplies

`vmulosb`/`vmulesb` — still prospective (no signed byte vector). Grow
`gen-altivec-vectors.py` alongside A1.

## A5-C: harness coverage gaps (no boot required)

Pixel + sum-across families need new vectors. These are permanent CI assets; grow the
suite here before relying on the E2E boot as a catch-all.

**Detail:** `docs/TESTING.md`, harness run: `SS_HARNESS_BATCH=1 make test-jit` (353/353).

---

# Track B — JIT performance

Full plan (per-lever effort/payoff + measured baselines): `docs/planning/OPTIMIZATION-PLAN.md`.
Implementation backlog from research: `docs/planning/sheepshaver-research/research/IMPLEMENTATION-BACKLOG.md`.

**Open medium levers (B2):**
- `bcctr` native (98.5% of JIT misses) or §R2 guarded inline direct-mapped cache
- `isync` inline BLR, lazy CR0 re-enable (12 a64/op — broadest remaining per-op lever;
  re-enabling needs divw/lwzx/mulhw/lwarx divw-`ra_store`-hoist first — see OPTIMIZATION-PLAN §0g)

**Open high-effort levers (B3):**
- Per-block prologue/epilogue overhead — the ~12-14 `a64/guest-op` ceiling is the new top lever
- Cross-block register pinning (r1/SP, r2/RTOC)
- bclr chaining (P9)

---

# Track C — Silicon Sheep (deferred)

Tauri v2 launcher/VM manager — first-run wizard, VM library, hot-reload, coherence-lite.
**Prerequisite decision:** the hard-fork question (pruned macOS-only vs upstream-tracking)
must be settled before starting C-Tier-1 (it gates Track D too).
Detail: `docs/planning/DESKTOP_INTEGRATION_PLAN.md`, `SiliconSheep/README.md`.

---

# Track D — Platform breadth

**D1. BasiliskII macOS AArch64 JIT port (deferred).**
Configure routing fixed; AArch64 JIT backend still needs porting.
Detail: `docs/planning/BasiliskII-MACOS-AARCH64-JIT-PORT.md`.

**D2. Linux/ARM re-convergence.** See `docs/UPSTREAM-LINEAGE-SYNC.md`.

---

## Done (recent highlights)

- AltiVec `ev_mixed`: entire tested element-order class fixed (splats, merges, pack, byte/
  halfword mults) — quarantine empty; 353/353.
- E2E harness + guest-UI introspection (Plans 1-4 shipped).
- M7 interrupt injection DEFAULT-ON; M8 slot-4 consumption GATED-OFF-GREEN.
- Upstream backports: VDE networking, SDL3 default, Wayland detection.
- JIT correctness: subfe/adde carry-out, vsel, crorc, mullwo.
- JIT perf: CR0 cleanup, LogicalImm encoder, FP register allocator, atomic spcflags.

Full history: `CHANGELOG.md`. Archived plans: `docs/archive/2026-06/superpowers/plans/`.
