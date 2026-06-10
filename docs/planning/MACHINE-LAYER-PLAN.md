# The Machine Layer — a designed NewWorld fidelity profile

> **Status:** 🟢 Approved architecture — implementation not started · **Created:** 2026-06-10
> **Decision (2026-06-10):** Stop extending the ROM-patching/paravirtualization approach toward
> NewWorld and Mac OS 9.2.x one bug at a time. Instead, build the thing SheepShaver never had:
> a **real machine-model layer** — MMIO bus, device models, supervisor environment — as a
> first-class, testable subsystem, landed as a second **machine profile** beside the proven
> paravirtual path.
>
> **This is the new primary approach for ROADMAP D3** ("break the 9.0.4 ceiling").

---

## 0. Lineage — what this supersedes and what it builds on

This document is the synthesis of the Path A / Path B research arc. It **supersedes the
strategy** in the documents below; their analysis, data, and reverse-engineering remain the
foundation and are referenced throughout, not discarded.

| Ancestor doc | Relationship |
|---|---|
| `HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` (Path A) | **Superseded as a path, harvested as components.** Its §2.7 hybrid insight (LLE for boot, HLE for runtime) is this plan's core principle. Its §3 MMU rung-ladder becomes §2c here. Its §2.8 obstacle map remains the per-wall reference. The env-gated scaffolding (`SS_NW_TRAMPOLINE`, synthetic ECB stub, SegMap/PMDT spikes) are prototypes the Machine Layer replaces with designed components. |
| `UPGRADE-CARD-PATH.md` (Path B) | **Superseded as a strategy, kept as a tactical tool.** The 4-byte System-file gate bypass and the SYSTEM-BOOT-GATES RE are permanently useful. Its key finding — the post-splash **SCC serial stall** — is the Machine Layer's first concrete consumer (M1). The "upgrade card" metaphor survives in corrected form: the cards we slot in are **device models and supervisor components**, not identity shims (identity patches were measured insufficient — UPGRADE-CARD-PATH §2.1). |
| `NEW-WORLD-ROM-SUPPORT-PLAN.md` | Historical context for the NewWorld ROM work; staged parcels analysis still valid as reference. |
| `PATCH-68K-SHIM-INVENTORY.md` | Path A artifact; remains the HLE-shim reference for whichever shims the fidelity profile keeps (§2d). |
| `docs/planning/sheepshaver-research/SYSTEM-BOOT-GATES.md` | Active reference — gate anatomy + DSAT format, reusable across System versions. |
| `DINGUSPPC-EVALUATION-PLAN.md` | Resolved the license question (GPLv2-or-later + GPLv3 → combined GPLv3, feasible). DingusPPC is promoted from "ideas-first reference" to **primary device-model donor** (§4). |
| `MMU-NANOKERNEL-MP-PLAN.md`, `MMU-DEFERRAL-REDTEAM.md` | Deep-dive references for §2c; the deferral verdict stands (classic Mac OS is morally V=P — no translation engine needed). |
| `COMPATIBILITY-PAYOFF-DINGUSPPC-REVISIT.md` | Its "make 8.6–9.0.4 run wanted software usably" verdict is unaffected — that work proceeds on the **paravirtual** profile in parallel. |

**Why the pivot from both paths:** Path A devolved into ROM-specific byte-pattern RE with no
unifying design (violating architecture-first); Path B proved that identity/gate patches cannot
close a *structural* gap (a 1998 ROM environment vs a 2001 System's expectations) — its own
frontier (the SCC stall) is literally a missing device model. Both paths independently arrived
at the same wall: **SheepShaver has no machine model.** This plan builds one.

---

## 1. Goal and governing decisions

**End state (Phase-3 "widen emulation," ROADMAP project arc):** a full-stack classic PowerPC Mac
emulation platform — Mac OS 8.6 → **9.2.2** — benefiting from the native AArch64 JIT, with
designed foundations for raw hardware handling, power management, and eventually Metal-mapped
graphics.

Decisions made 2026-06-10 (with Ken):

1. **Destination is B, gait is C.** The platform vision ("model the complete PowerPC Mac")
   justifies building permanent components — but we build them **in the order 9.2.x demands**,
   no speculative generality.
2. **Classic Mac OS only; door architecturally ajar for OS X.** Classic Mac OS is morally V=P,
   so the DIRECT_ADDRESSING/NATMEM identity-mapped JIT fast path stays. We do **not** build a
   softmmu. The memory-access seam (MMIO bus) is designed so a translation layer *could* slot
   in later; we never pay for it now.
3. **Strangler-fig dual profile (Approach A).** The new work lands as a second, pref-selected
   **machine profile**. Today's path is frozen as `paravirtual` and remains the shipping
   default with all gates green. The `newworld` fidelity profile grows beside it, component by
   component, A/B-testable against the known-good path.
4. **Chassis stays SheepShaver.** Evaluated and rejected: porting our JIT into DingusPPC
   (near-rewrite of the JIT's memory/dispatch model; abandons our host-integration and
   verification investment) and adopting QEMU as substrate (TCG replaces our JIT — the
   project's reason to exist). Both are demoted to supporting roles: DingusPPC = device-model
   donor, QEMU = behavioral oracle.

---

## 2. Architecture

```
┌─────────────────────────────────────────────────┐
│ Host integration (SDL3, VNC, E2E, prefs)        │  ← unchanged
├─────────────────────────────────────────────────┤
│ CPU core: AArch64 JIT + interpreter              │  ← unchanged (identity-mapped
│   memory access stays the fast path)             │     fast path; classic-only)
├──────────────────┬──────────────────────────────┤
│ Profile:         │ Profile: NEWWORLD FIDELITY    │
│ PARAVIRTUAL      │  (new)                        │
│ (today's path,   │  ┌─────────────────────────┐ │
│  frozen & green) │  │ MMIO bus (SIGSEGV-based)│ │
│                  │  │ Device models:           │ │
│  ROM patches +   │  │  SCC · VIA/Cuda · PIC ·  │ │
│  HLE shims +     │  │  NVRAM · MacIO           │ │
│  EMUL_OPs        │  │ Supervisor env:          │ │
│                  │  │  MMU bookkeeping · excp  │ │
│                  │  │  vectors · Trampoline    │ │
│                  │  │  handoff synthesis       │ │
│                  │  └─────────────────────────┘ │
│                  │  HLE shims kept for runtime   │
└──────────────────┴──────────────────────────────┘
```

New code lives in `SheepShaver/src/machine/` — explicit, testable components replacing the
diffuse patchwork of ROM patches, fake memory pages, and env-gated hacks.

### 2a. MMIO bus (the keystone)

A registry of guest physical address ranges → device objects with
`read(addr, size)` / `write(addr, size, value)` handlers.

- **Mechanism:** device pages are left **unmapped** inside the NATMEM reservation; the existing
  SIGSEGV machinery (`sigsegv.cpp` already decodes faulting accesses) dispatches to the bus
  instead of crashing. HANDOFF §2.7 step 4 sketched exactly this; it becomes real.
- **Cost model:** zero on the fast path. RAM/ROM accesses remain raw JIT loads/stores; only
  device-register touches fault. Boot-time device polling is not perf-critical.
- **What it replaces:** the fake-SCC page, the `[KDP-0x900]` fake-VIA/SCC experiments, and
  every future "pre-poison bytes in fake memory and hope the polling loop is satisfied" hack.
  A device *answers* each read; state machines instead of magic constants.
- **OS X door (decision 2):** the bus is the memory-access seam. If translation is ever
  needed, it slots in at this layer; nothing else assumes V=P beyond the JIT's existing
  contract.

### 2b. Device models

Initial set, dictated by what NewWorld boot actually probes (from the Path A/B wall analyses,
not speculation):

| Device | Why (evidence) | Source |
|---|---|---|
| **SCC 8530** (ESCC, serial) | The 9.2.1 post-splash stall (ROM serial init polling at 68k PC 0x500cc998 — SYSTEM-BOOT-GATES §5) AND the nanokernel `check_work` SCC poll (Path A `[KDP-0x900]` finding). Both current frontiers are this one missing device. | DingusPPC `escc` + Zilog 8530 datasheet |
| **VIA / Cuda (or PMU)** | Timer interrupts, ADB, RTC, NVRAM access path, power events — "energy management" starts here. | DingusPPC + Apple Cuda/PMU protocol docs |
| **Interrupt controller** (Heathrow mask regs / OpenPIC) | The designed replacement for the hand-rolled `[NW-INT]` host-timer injection hack in the working tree. Interrupts get **raised by devices** through a controller into the CPU's exception path, not injected by the host. | DingusPPC + OpenPIC spec |
| **NVRAM** (full 8 KB, partitioned) | Mac OS 9.x stores prefs in extended NVRAM; OldWorld shims covered only 3 of 7 ROM NVRAM routines (HANDOFF Phase-4 table). | DingusPPC |
| **MacIO glue** | The container the ROM expects these devices inside (address map, F3xxxxxx region — matches the observed r18/r19 = 0xF3016000/0xF3012000 SCC addresses). | DingusPPC `macio` |

Each device is a plain C++ object with no emulator-global dependencies → **unit-testable
host-side without booting** (feed register reads/writes, assert state-machine behavior).

### 2c. Supervisor environment (the rung-ladder, continued as components)

- **Rung 2 — SR/BAT as stored state** (S, general correctness): store what `mtspr`/`mtsr`
  writes, return it on read-back, like the SPRG0–3 fix already harvested from Path A.
- **Real exception vectoring:** `sc`, decrementer, and external interrupts dispatch through
  guest exception vectors (nanokernel handlers) instead of host-side `HandleInterrupt` special
  cases. Fixes the `sc` double-increment class of bug structurally and lets the PIC (§2b)
  deliver interrupts the way the nanokernel expects.
- **Synthesized Trampoline handoff:** the documented register/KDP/device-tree state Open
  Firmware leaves for the nanokernel, built by ABI contract (the SegMap/PMDT/ECB spike
  technique, promoted from env-gated experiment to a designed initializer).
- **MMU bookkeeping only:** rungs 3+ (pre-seeded HTAB, shadow arenas, softmmu) stay deferred
  per MMU-DEFERRAL-REDTEAM — build only if measurement demands it.

### 2d. What stays HLE on purpose

Video (→ the future Metal surface), disk, ethernet, sound, file system. These are *runtime*
paths where paravirtualization is the feature, not the compromise — it's where the speed is
and where host integration (and eventually Metal) hooks in. The hybrid split is permanent
architecture, not a transition state: **LLE for what the ROM probes, HLE for what the OS uses.**

---

## 3. Milestones

Each milestone is independently valuable and gated; the paravirtual profile's gates
(`make test-jit`, `make e2e`, bench history) stay green throughout — that is the standing
non-regression contract.

| # | Milestone | Definition of done | Effort |
|---|---|---|---|
| **M0** | **Profile plumbing** | `machine` pref (`paravirtual` default / `newworld`); the `SS_NW_*` env-gate sprawl consolidated under the profile; paravirtual path byte-identical in behavior, all gates green. | S |
| **M1** | **MMIO bus + SCC model** | SCC register state machine answers the ROM's serial-init polling. Two consumers prove it: (a) 9.2.1-on-1.1-ROM boot progresses past the post-splash stall (the Path B frontier); (b) nanokernel `check_work` runs with a real SCC base instead of null. Device behavior diffed against QEMU mac99. | M |
| **M2** | **Interrupt controller + VIA/Cuda** | The `[NW-INT]` host-injection hack deleted; decrementer/timer → PIC → guest exception vector delivery wakes the nanokernel idle loop on the 9.0.1 ROM diagnostic boot. ADB/RTC paths answer. | M–L |
| **M3** | **NVRAM + MacIO address map** | Full partitioned 8 KB NVRAM behind the bus at the MacIO-correct address; 9.x prefs read/write paths satisfied; unshimmed NVRAM routines hit the model, not nonexistent hardware. | S–M |
| **M4** | **Supervisor environment** | Rung 2 SR/BAT stored state; real exception vectoring for `sc`/DEC/external; synthesized Trampoline handoff replaces `SS_NW_TRAMPOLINE` ad-hoc writes. | M |
| **M5** | **Mac OS 9.2.2 boots on the fidelity profile** | End-to-end: NewWorld ROM (9.0.1 preferred — may satisfy 9.2.x natively, no System-file patch; the 4-byte gate bypass kept as fallback for the 1.1 ROM), boot to Finder, E2E lifecycle PASS on the `newworld` profile. | L (integration) |
| **M6+** | **Platform features** (separate designs when reached) | PMU power management; Metal-mapped video via the `.ndrv` HLE seam; fidelity profile becomes default once it dominates paravirtual on the E2E + bench matrix. | — |

**Sequencing notes:**
- M1 is deliberately first after plumbing: it is the smallest component that pays off **on both
  ROMs at once** (the 9.2-on-1.1 stall and the 9.0.1 nanokernel poll) — the first "card slotted
  in."
- M2 and M3 are parallelizable after M1 (independent devices on the same bus).
- The forcing-function discipline survives: every general PPC-correctness bug surfaced en route
  (the SPRG/fctiw/SDR1/sc class) is fixed on **both** profiles immediately.

---

## 4. Sources, oracle, and provenance

- **DingusPPC = primary device-model donor.** GPL-3.0; combining is license-feasible (our
  GPLv2-or-later + their GPLv3 → combined work GPLv3 — resolved in DINGUSPPC-EVALUATION-PLAN).
  Follow backport hygiene: cite the DingusPPC source file/commit SHA in code comments and
  CHANGELOG. **Constraint: DingusPPC does not accept AI contributions into their repo — never
  send AI-generated PRs upstream to them.** Downstream GPL use in this (openly AI-assisted)
  repo is normal GPL practice with attribution.
- **QEMU mac99 = behavioral oracle** (and secondary code reference — its ESCC/Cuda/OpenPIC/
  MacIO models are GPL). Each device model's "done" is defined by **behavior diffed against
  QEMU running the same ROM** — not by "the boot got further." This replaces hope-driven
  debugging with reference-driven implementation (UPGRADE-CARD-PATH Experiment 2, promoted
  from "nice to have" to standing methodology).
- **Datasheets = ground truth** where emulators disagree: Zilog SCC 8530, VIA 6522, Apple
  Cuda/PMU protocol, OpenPIC spec.

## 5. Testing strategy

1. **Device unit tests (no boot):** each device model is a pure object — drive its register
   interface from a host-side test binary, assert state-machine transitions. Lives beside the
   existing offline suites (cheap, CI-able anywhere).
2. **QEMU differential traces:** scripted boots of the same ROM under `qemu-system-ppc -M mac99
   -d` capturing device-register access sequences; compare our bus's access log (the bus
   naturally logs every MMIO touch). Divergence = the next work item, named.
3. **Existing gates, profile matrix:** `make test-jit` (CPU core unchanged — must stay 100 on
   both profiles), `make e2e` on paravirtual (non-regression contract), and a new
   `make e2e-newworld` lane as soon as the fidelity profile boots anything.
4. **The boot-stage ladder as a regression suite:** each previously-conquered wall (nanokernel
   init, idle-loop wake, DR Emulator entry, 68k dispatch, splash, Finder) becomes a named,
   asserted checkpoint — so a device-model change can't silently regress an earlier stage.

## 6. Error handling & risk

| Risk | Mitigation |
|---|---|
| SIGSEGV-dispatch correctness (wrong decode → silent bad device read) | Bus logs every access; unmapped-but-unregistered ranges abort loudly with PC + address (no silent zero-reads). |
| Device model fidelity rabbit holes (modeling more than the ROM probes) | YAGNI per device: implement registers the trace shows are touched; abort-loudly stubs for the rest. QEMU diff bounds "done." |
| Paravirtual regression while refactoring shared code (sigsegv.cpp, prefs) | M0 lands the seam first; profile checks at init, not scattered runtime conditionals; existing gates run per PR. |
| 9.0.1 ROM still rejects 9.2.x at some deeper gate | SYSTEM-BOOT-GATES methodology (DSAT parse + binary search) is reusable on any System version; 4-byte bypass remains the fallback. |
| Effort balloons past appetite | Milestones are individually shippable; the stop-rule discipline from HANDOFF §2.7.1 applies per wall: re-evaluate when a wall needs multi-day RE with no general payoff. |

## 7. Roadmap integration

- **ROADMAP D3** should point here as the primary approach (Upgrade Card demoted to tactical
  tool within M1/M5).
- Track B (perf) and the COMPATIBILITY-PAYOFF "make 8.6–9.0.4 usable" thrust continue on the
  paravirtual profile, unblocked and unaffected.
- Silicon Sheep (Track C) eventually surfaces the machine profile as a VM-library choice
  ("Power Mac 9500 (fast)" vs "Power Mac G3 (faithful)").
