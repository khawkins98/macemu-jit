# Roadmap / Work Tracker — `macos-arm64`

> **Status:** ⏸ PAUSED 2026-06-14 (resume entry: `docs/HANDOFF.md`) · **Created:** 2026-06-04
> **Current state (header budget = 5 lines):** SheepShaver boots 8.6 to Finder, full native
> JIT (stable). Machine Layer: M10+M11a+M11 COMPLETE; M12 PARTIAL; **M13 COMPLETE (2026-06-14)**.
> **M13 RESULT:** NewWorld 68k interrupt delivery WORKS — the M9→M13 "`0x5000ED08` never runs" thesis
> was a probe-granularity artifact (ed08-vs-ed0a). Dead `SS_NW_DR_AUTOVEC`/`SS_M10_CGRP` reverted.
> **Next = M14:** the model-rejection / pre-System gate (a ROM/OS-version issue, NOT interrupts).
> Findings: `docs/planning/M13-FINDINGS-interrupt-delivery.md` §C-pin.8. Frontier: `docs/AGENT-CONTEXT.md`.

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
| **D** | Machine Layer / platform breadth | 🟡 active (M14) | `docs/planning/MACHINE-LAYER-PLAN.md` |

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

## M10: User-mode DR + CGRP initialization → 68k interrupt handler fires ✅ COMPLETE (REVERTED by M13)

> ⚠️ **M13 close-out (2026-06-14):** M10's `SS_M10_CGRP` forged-CGRP-table mechanism was **reverted**.
> It targeted the M9→M13 non-problem (native interrupt delivery already works — the "handler never
> fires" claim was a probe-granularity artifact) and crashed the DR (0xDEADBEEF). Retained only as a
> negative-result record. See `docs/planning/M13-FINDINGS-interrupt-delivery.md` retraction banner.

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

## M11: Framebuffer aperture + OF node + SDL blit ✅ COMPLETE

Plan: `docs/archive/2026-06/superpowers/plans/2026-06-13-m11-framebuffer.md`. Tasks A–D done.
16 MB aperture at 0x81000000 (quiet mode: vm_mac_acquire_fixed; MMIO hull unchanged),
SDL the_buffer redirected to aperture, OF display node published (640×480×32, "cofb"),
MMIO_APERTURE non-hull contract verified by T-F6 unit test (13/13). [FB-DIRTY]=0 in
NW diagnostic boot (expected: boot exits at 0.3s before pixels; pixel gate deferred to M12).
Harness 353/353. Commits: 4ba87d8d (T-F6), 8f4197fa (Tasks A–D), 46c31ee7 (post-fix).

## M12: Display pixels — Wave0+Wave1 stable, pixel gate FAIL ⏸ PARTIAL (2026-06-13)

> ⚠️ **M13 close-out (2026-06-14):** the Task C diagnosis below ("pixel gate FAIL because `irq_fired=0`
> / interrupt never reaches the 68k handler") is subsumed by the M13 retraction — interrupt delivery
> was never the blocker (the "never reaches" was the ed08-vs-ed0a probe artifact). The real wall is the
> downstream model-rejection gate (M14). Wave0/Wave1 memory-map fixes remain valid.

Plan: `docs/archive/2026-06/superpowers/plans/2026-06-13-m12-display-pixels.md`.
- **Task A (Wave0)** ✅: NW lowmem extended 1MB → 32MB; fixes crash at ea=0x010020c8. Commit `ddbd8d79`.
- **Task B (VideoDriverStub)** ✅: VideoDriverStub verified; kOpenCommand path confirmed.
- **Task C (pixel gate)** ❌ FAIL: Boot stable 30+ seconds (dec_expiries=2000+) but irq_fired=0.
  CGRP (SS_M10_CGRP=1) routes EXT → ROM+0xED08, which immediately hits A-trap 0xA9A8 before
  the Mac OS Trap Dispatch Table is loaded. `bra.s *` stop stub → boot parks. **M13 input.**
- **Task D (Wave1)** ✅: Anonymous zero at 0xFF000000–0xFFFFFFFF fixes ea=0xFFFFEFD0 crash. Commit `348544cd`.

Harness: 353/353. Gates: machine tests ALL PASS, e2e-test 122 passed, make e2e PASS. `[FB-DIRTY]=0`.

## M13: NewWorld 68k interrupt delivery — ✅ COMPLETE (2026-06-14): the artifact retraction

**Result (the deliverable):** the milestone overturned its own premise. **Native NewWorld 68k
interrupt delivery WORKS.** The M9→M13 load-bearing thesis — "`0x5000ED08` never runs / interrupts
never delivered" — was a **probe-granularity artifact**: `SS_PROBE_68K=0x5000ed08` is exact-match,
but the DR's first `lhau` advances r24 `ed08→ed0a` before the dispatch hook samples. Re-targeting to
`ed0a` matched **8/8 in plain baseline** (HLE OFF), a genuine DR-built `$64` level-1 autovector frame
(`[a7+6]=0x0064`, saved `SR=0x2000` = IPL 0 → legitimate; saved PC `0x50034cae`); handler region
entered ~207×, full level-dispatch + VBL pass, scheduler healthy. Evidence:
`docs/planning/M13-FINDINGS-interrupt-delivery.md` §C-pin.7/8 (retraction banner at top).

**Close-out actions:** reverted both dead delivery mechanisms — `SS_NW_DR_AUTOVEC` (Task C HLE) and
`SS_M10_CGRP` (forged CGRP table). Every M9→M13 delivery effort (Task A injection, M10 CGRP forging,
the CGRP-registration "circular" thesis, Task C autovector HLE) targeted a non-problem.

**Meta-lesson banked (LEARNINGS 2026-06-14):** a load-bearing NEGATIVE ("X never runs") that rests on
an exact-match probe must be ring-confirmed (or word+2-corrected) before anything is built on it —
the SECOND five-milestone misdirection from a measurement artifact (cf. session-5 HOT-PC).

> The M13 strategy/plan docs (`NANOKERNEL-STRATEGY-DECISION.md`, the m13 plan) are now historical —
> they designed the non-problem. Do NOT re-chase interrupt delivery / CGRP registration / 68k injection.

## M14: Model-rejection / pre-System gate — 🔬 NEW FRONTIER (2026-06-14)

**The wall moved downstream.** Native delivery, the 68k handler, and the scheduler are all healthy;
the boot still parks ~15 s in at the `[ALARM]` **model-rejection / pre-System gate** — a
Gestalt/machine-ID or System-file boot gate rejecting this machine/ROM combo (`jDR` 68k-block counter
FREEZES ~10 s in while the NK spins on). This is a **ROM/OS-version** issue, not an interrupt one.

**Leverage (already-RE'd, gate-bypass-TOOLED — MORE tractable than NK internals):**
`docs/planning/sheepshaver-research/SYSTEM-BOOT-GATES.md` (`boot` id=3 anatomy, DSAT format, error
catalog, gate-bypass) + the archived 4-byte System-file bypass in
`docs/archive/2026-06/planning/UPGRADE-CARD-PATH.md`.

**Approach (per lead — characterize FIRST, do not act before understanding):**
1. Pin the gate: capture the last 68k subroutine the DR runs before `jDR` freezes (r24
   instruction-boundary trail in the final 1–2 s) and identify the device/gate it polls.
2. ONE early cross-version boot as a cheap version-locked check: a single boot of a different rev
   (staged 9.2.1 / 9.0.4 / 1.1 ROM at `/Users/Shared/macemu/`, or the QEMU 9.2.1 oracle) — same
   `[ALARM]` or different?
3. The full ROM/OS-version route-around sweep is **PROMOTED only if** that check shows the gate is
   version-locked. Held in reserve until then — do NOT jump to the sweep before characterizing.

## M11+: CFM / Process Manager / drivers (unscoped)

~3-4 milestone-class efforts remaining to Mac OS 9.2.x Finder. Measure the wall count
with the QEMU rig before scoping (the QEMU wall census — unblocked).

## NewWorld coherence tooling (infrastructure, 2026-06-13)

Closing the integration blind spot — the per-opcode harness + paravirtual e2e never
exercised the all-NW-gates-on stack, so an earlier milestone could be silently regressed
by a later one. Landed:
- **`make nw-northstar`** — the standing all-on boot-progress signal (`[NW-PROG]` readout
  + classified verdict, report-only). Spec: DIAGNOSTICS.md "[NW-PROG] readout".
- **Discrete `[NW-PROG]` readout** (was `[PROGRESS]`) + the **boot-non-determinism finding**
  (LEARNINGS 2026-06-13): a bare SIGSEGV is NOT a regression; classify by durable markers.
- **QEMU rig device-tree capture** (`info qtree`/`info mtree` → `device-tree.txt`) — the
  topology/wiring/NVRAM oracle, standing.
- Process-health: falsification tally + QEMU-oracle-first rule (MILESTONE-WORKFLOW §6c);
  load-bearing-caveat append-only rule (CONTRIBUTING archive pass).

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
