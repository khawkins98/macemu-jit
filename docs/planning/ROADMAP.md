# Roadmap / Work Tracker — `macos-arm64`

> **Status:** ⏸ PAUSED 2026-06-12 (resume entry: `docs/HANDOFF.md`) · **Created:** 2026-06-04
> **Current state (header budget = 5 lines):** SheepShaver boots 8.6 to Finder, full native
> JIT (stable). Machine Layer: M7 interrupt injection DEFAULT-ON; M8 slot-4 consumption
> SHIPPED GATED-OFF-GREEN (`SS_NW_IRQ_CONSUME`). **Active critical path: M9 VIA-IFR** —
> the 68k handler rte's source-less. Live frontier: `docs/AGENT-CONTEXT.md`.

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

## M9: VIA-IFR → guest-claimed 68k ticks (ACTIVE — stalled)

- [x] **Infrastructure**: OP_IRQ_NW (frame-aware handler) + M68kRegisters.pc writeback (harness 353/353)
- [ ] **Blocker**: boot stall cause unknown — trampoline tp[25-26] vs ROM patch (need isolation boot)
- [ ] **Verify**: `SS_PROBE_68K=0x5000ed08` fires, dec_expiries ~2393, boot advances
- [ ] **Ship**: one-line CHANGELOG; retire pre-M7 default-ON gates

See `docs/HANDOFF.md` §Root cause for next step. Gate: `SS_NW_VIA_IFR`.

## M10: Framebuffer (recon done, not started)

Recon complete — a visible screen is itself an instrument.
See `docs/planning/machine/FRAMEBUFFER-RECON.md` (HOLD — verify M5 framebuffer status first).
Status: waiting on M9.

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
