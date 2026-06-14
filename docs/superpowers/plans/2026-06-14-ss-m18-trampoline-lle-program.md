# SS_M18_TRAMPOLINE_LLE — implementation program (STAGED, months-scale)

> **For the coordinator:** this is a PROGRAM of staged milestones, each runnable through the milestone machine (`docs/MILESTONE-WORKFLOW.md` §2). It is NOT a single milestone. Route A is SETTLED (`FINDINGS-trampoline-re.md` Q0-E) and is NOT relitigated anywhere below — this plan is the HOW and the SEQUENCING. Two of the four stages (S1, S3) require their OWN gating Task-0 before any of their code; the other two inherit explicit residues from the SS_M18 gating Task-0 (`docs/superpowers/plans/2026-06-14-ss-m18-trampoline-lle-gating-task0.md`) that they must close first.

**Status:** PROGRAM DRAFT (2026-06-14) — staged from the gating Task-0 verdict "Route A GO but MONTHS" (three independent month-forcing findings Q1/Q2/Q3).

---

## Program goal (PASS-vs-DIAGNOSTIC)

**PASS (the program's definition of done — falsifiable):** the frozen-struct class (CGRP / NK dispatch state / the M16 ROM-absent handler PC) is populated by **real guest code** (the real `MacOS.elf` Trampoline + the real NanoKernel-v02.27 + a real disk System/Enabler IM-init) computing real values from a synthesized Core99 device tree, under `SS_M18_*` + `MachineProfileIsNewWorld()`, with the **paravirtual profile byte-identical** and `make test-jit` = 100%. The program PASSES when, with all stage env-gates ON, a newworld boot reaches the **first non-IM-init wall** (expected per charter §6 / M14-FINDINGS: the Cuda IFR/IER wall) — i.e. CGRP is built by guest code, no longer frozen-zero.

**DIAGNOSTIC (recorded, never a program gate):** how far the all-on boot gets past CGRP; `nw-northstar` `[NW-PROG verdict]`; any newly-named downstream wall; per-stage probe-boot progress markers. Reaching Finder remains the north star, NOT this program's DoD.

**Scope law:** Route A is decided. No stage may forge a frozen output (re-hand-write CGRP), fake the MMU with identity-only when translation is load-bearing, or claim a "handoff boundary" the recon proved fictional (Q1). See Stop-rule.

---

## Authoritative inputs

| Doc / source | What it fixes for this program |
|---|---|
| `docs/MILESTONE-WORKFLOW.md` §2 (each stage = env-gated default-OFF milestone, falsifiable-in-advance gates, acceptance env-on-first/flip-LAST/revert-on-red), §3 (file ownership / parallel layer), §4 (boot-disjoint split), §6/§6b (gate tiers, dispatch economics), §6c (QEMU-before-static-RE) | The machine each stage runs through; the program is many milestones. |
| `docs/planning/newsheep/FINDINGS-trampoline-re.md` — final section "SS_M18 gating Task-0 COMPLETE … Route A GO but MONTHS" (Q1/Q2/Q3 + cited addresses + per-surface effort bands A7) + the earlier "SS-integration sketch" reconciliation table | The authoritative inputs. ALL cited addresses are **re-verify before use**. |
| `docs/superpowers/plans/2026-06-14-ss-m18-trampoline-lle-gating-task0.md` (rev-2, A1–A9) | The format/discipline this program reuses; its blocking table + residues seed Stages 2/4; its amendments are LAW. |
| `docs/superpowers/plans/2026-06-11-nk-syscall-surface.md` | The canonical plan-format exemplar. |
| `docs/AGENT-CONTEXT.md` — Constants + env-gate state + gate tiers; `MachineProfileIsNewWorld()`; the slot protocol; the Execute68k pair | KDP/ECB/mirror constants; SS_NW_* census; `make nw-northstar` observe line. |
| `docs/planning/MACHINE-LAYER-PLAN.md` (esp. the M5 paged-MMU deferral Q2 forces un-deferring) + `docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md` (Core99 DT source for Stage 2) | Stage 1 un-defers M5; Stage 2's DT comes from CORE99-MACHINE-DESCRIPTION + the QEMU device-tree oracle. |

## Codebase facts (carried; implementers RE-VERIFY each site before use)

> All offsets/addresses PRE-PINNED for orientation — **re-verify before use** (re-read the file:line / re-disassemble the cited parcel offset). Verified live during this plan draft where noted.

- `MachineProfileIsNewWorld()` defined `SheepShaver/src/machine/machine_profile.cpp:96`; decl `SheepShaver/src/include/machine_profile.h:37`. Callers today: `rom_patches.cpp`, `Unix/main_unix.cpp`, `kpx_cpu/src/cpu/ppc/ppc-cpu.cpp`, `kpx_cpu/sheepshaver_glue.cpp`, `kpx_cpu/src/cpu/ppc/ppc-execute.cpp`, `machine/machine_profile.cpp`. **[verified 2026-06-14]**
- Glue path is **`SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp`** (NOT any `src/Unix/` prefix). Execute68k pair read live `:1504–1505`; written `:3140–3143`; `'Hnfo'`/PIC-rail `:2861–2904`; `[KDP+0xf2c]` TimebaseSpeed `:3099–3119`; mirror entry-vector publish `:3011`; `interrupt()` `:1002`; `deliver_pending_dec_exception()` `:1122ff` (XLM_RUN_MODE fence `:1139`); EXT/DEC `ExcEnter` `:1254`/`:1342`; `execute_68k` `:1461`.
- Trampoline forge: `rom_patches.cpp` `PatchROM_NW_trampoline` (table[0] gate @ ROM+0x46e8c0; seeds r31=ECB/r30=0x50460000/r29=0x50480000).
- Exception core: `machine/exc_core.cpp` `ExcEnter` `:12` / `ExcRfi` `:104`; `g_exc_entry_table` (PEM masks are LAW). Supervisor SPR/BAT/SDR1: `sheepshaver_cpu::reset_supervisor_for_test` `:560–569` (TEST helper, not a live MMU). Scheduler: `machine/event_sched.cpp`, `machine/virt_clock.cpp`.
- **V=P floor:** `Unix/sysdeps.h:91–99` `DIRECT_ADDRESSING 1` when `NATMEM_OFFSET` set; host = `NATMEM_OFFSET(0x400000000000) + guest` (`sheepshaver_glue.cpp:2388`). **No page-table walker** (grep `tlbie|get_physical_address|htab|page_table` over PPC cpu sources → none); BAT/SDR1/SR writable+readable **live but inertly** via interpreter `mtspr/mfspr` (JIT fallback `ppc-jit.cpp ~:1987/:2022 → ppc-execute.cpp ~:1607/1626/1539/1561`) — a "write-BAT then read-BAT" check passes inertly and is a **false-clean** for any MMU verdict.
- `SheepShaver/src/openfirmware_ci.cpp` does **NOT** exist yet (Stage 2 creates it). **[verified 2026-06-14]**
- NK constants (AGENT-CONTEXT): KDP=0x68ffe000, ECB=0x68fff000, mirror 0x50460000, DR table 0x50480000, ROMBase=0x50000000, NK run base 0x50310000, CGRP `*(KDP-0x338)` `[CGRP+0x38/0x3c/0x40/0x44]`, service 0x503148e0. NK exception entries: EXT 0x50314880, SC 0x50314ac0, DEC 0x50313200, PROGRAM 0x50314700. ROM EmulatorCode @ ROMBase+0x360000.
- **Q1 pinned addresses (re-verify):** NK entry 0x50310000 (`mfmsr`→test MSR[DR]→`mtspr SRR0/SRR1`→`rfi` @ 0x5031003c); DEC reschedule 0x50313200 (`mtspr DEC` @ 0x50313234). **Q2 pinned (re-verify):** SDR1 install `mtspr SDR1` @ 0x50310604; SR program `mtsr 0..15` @ 0x503104b4+; per-context `mtsrin` loop @ 0x50315290; MMIO segment-swap+DR-toggle 0x50325894/…9c/…a4; 13× `tlbie`+`tlbsync`. **Q3 pinned:** CGRP fully populated `*(KDP-0x338)` (+0x04='CGRP', +0x38/+0x3c/+0x40 arrays, +0x44=18 entries); builder + handlers in **disk-loaded low RAM 0x0045xxxx–0x0046xxxx** sharing one CFM/PEF TOC @ 0x00466ba0; the writer's PC + live DT read-order are **explicit residues** (writer ran physically/real-mode, defeating virtual watchpoints).
- Assets: no `macos921.dsk` in tree at draft path; `SheepShaver/e2e/assets/{bench,apps}.dsk` exist. The genuine 9.2.x install medium is a **parallel asset gap** (see "Parallel asset dependency"). **[verified 2026-06-14]**

---

## Program structure — the dependency graph

```
                 SS_M18 gating Task-0 (DONE; Route A GO/months)
                                 │
        ┌────────────────────────┼─────────────────────────────┐
        │ (boot-disjoint)        │                              │
   STAGE 2 Task-0           STAGE 1 Task-0                 (parallel recon)
 (close Q3-residue          (OWN deep Task-0:
  + DT-shape recon)          paged-MMU-in-hybrid)
        │                         │
   STAGE 2 impl  ───────────►  STAGE 1 impl (paged MMU, newworld profile)
 (loader+OF-CI+DT)            (PREREQUISITE; gates S3 & S4)
        │                         │
        └─────────────┬───────────┘
                      ▼
                 STAGE 3 Task-0 (OWN deep Task-0: two-supervisor reconciliation)
                      │
                 STAGE 3 impl (SS yields supervisor role to NK)
                      │
                      ▼
                 STAGE 4 impl (disk System/Enabler IM-init in the loop → CGRP)
                      │  HARD-BLOCKS on the genuine 9.2.x ISO asset
                      ▼
            program PASS (CGRP guest-built; next wall = Cuda IFR/IER)
```

**Edges (justification):**
- **S1 gates S3 and S4 via the MMU.** Q2 = requires-paged-MMU; the NK is a paged supervisor (Q1) and CGRP is built by disk code running under translation (Q3, low-RAM CFM). Neither the live NK nor the IM-init can run correctly without S1.
- **S2 is boot-disjoint and starts in parallel with S1's Task-0.** S2 loads `MacOS.elf` + services OF-CI + builds the DT — none of that needs the paged MMU to *exist* yet (the Trampoline runs pre-NK; OpenBIOS proves it runs at its ELF vaddr in real mode). S2's `/mmu` `call-method` backend, however, **stubs to S1** (see S2 task list) — so S2 can be built and unit-tested with a stub `/mmu`, but its acceptance (real handoff to NK) waits on S1.
- **S3 needs both S1 (paged MMU) and S2 (a live Trampoline+DT to hand off from).**
- **S4 needs S3 (a live NK regime to host IM-init) and the 9.2.x ISO.**

**Which stages need their OWN gating Task-0 before code:**
- **Stage 1 — YES, a deep Task-0** ("is a real paged MMU tractable in SS's hybrid model without breaking paravirtual?"). This is the hardest, most foundational unknown.
- **Stage 3 — YES, a deep Task-0** ("is 'SS yields supervisor role to the NK' coherent, or does it unravel the hybrid architecture?").
- **Stage 2 — a SMALL Task-0** to close the Q3-residue's DT-shape questions + pin the OF-CI ABI (mostly already bounded by Q0-A; this is a confirm-not-discover Task-0).
- **Stage 4 — inherits Q3 residues** (writer-PC + live DT-read-order) as its first gating items; it cannot start until S3 lands and the ISO exists.

---

## Blocking-answer / dependency table

| Stage / concern | Blocked by | Residue disposition (partial-findings-beat-stalling) |
|---|---|---|
| S1 paged-MMU mode for newworld profile | **S1-Task-0** (own) | If S1-Task-0 cannot show a paged MMU coexisting with paravirtual V=P byte-identical within its window → verdict = "paged-MMU-in-hybrid UNKNOWN → program-blocking-risk", recorded as the program's top open item; NEVER default to "identity mapping will do" (A8 #7). |
| S2 Trampoline loader + handoff re-use of `ExcEnter`/`execute_68k` | **S3-Task-0** (Q1) | S2 may BUILD against a stub `/mmu` + stub handoff; its real handoff acceptance waits on S3's REPLACE/CO-OWN verdict. Covered rows pinned; minor rows → S3-Task-0 residues. |
| S2 `/mmu` `claim/translate/map` backend | **S1** | Until S1 lands, `/mmu` is a recording stub (logs claim/translate/map requests; returns plausible identity for build/unit-test only, flagged NON-ACCEPTANCE). |
| S2 OF-CI callback + Core99 DT contract | **S2-Task-0** (Q3 mechanism + DT shape) | Q3 pins mechanism (which props, write order) per A6; property VALUES/SHAPES come from CORE99-MACHINE-DESCRIPTION + QEMU oracle (A6 defers shape). Captured-partial DT contract beats the current INFERRED one. |
| S3 two-supervisor reconciliation (does SS yield?) | **S3-Task-0** (Q1, own) | If no coherent yield/co-own model is found within the window → "supervisor-handoff UNKNOWN → architectural-risk; months", recorded as the program's structural open item; do NOT assert a fictional handoff boundary (Stop-rule #3). |
| S3 Execute68k pair re-bind | **S3-Task-0** (Q1 sub-row) | Q1 already pinned the pair is an SS synthetic absent from the parcel; S3-Task-0 decides whether SS's `execute_68k` path survives at all under NK ownership, or is replaced by the NK's ECB/exception path. |
| S4 disk IM-init in the loop → CGRP | **S3** + **9.2.x ISO** + **Q3 residues (writer-PC, DT-read-order)** | If the ISO is absent → S4 HARD-BLOCKED (asset gap; surface, do not forge). If Q3 writer-PC residue still open → S4-first-task is the live trace under S1+S3 (now translatable, so watchpoints work). |
| Program GO + PASS (CGRP guest-built) | **S1 ∧ S2 ∧ S3 ∧ S4** | The program verdict is conditional until all four land; each stage reports its own GO/months/UNKNOWN band. |

---

## The stages

### Stage 1 — NewWorld-profile paged MMU (PREREQUISITE per Q2)

**Effort band: MONTHS (the hardest, most foundational stage). Needs its OWN deep Task-0.**

**Goal (PASS-vs-DIAGNOSTIC).** PASS: a paged-MMU mode exists for the newworld profile (gated `SS_M18_PAGED_MMU` + `MachineProfileIsNewWorld()`) in which a guest `mtsr`/`mtsrin` + SDR1 install followed by a translated load/store **resolves to the SR/page-table-selected physical address, not the V=P address** — proven by a microtest where the same VA maps to two different PAs under two contexts (the Q2 POSITIVE-consumption #1 shape, `0x50315290`-class). The **paravirtual profile is byte-identical** (paravirtual stays V=P/DIRECT_ADDRESSING; the paged path is structurally unreachable when `!MachineProfileIsNewWorld()`); `make test-jit` = 100%. DIAGNOSTIC: TLB/walker hit-rate counters; how far a probe NK boot gets once translation is live.

**The gating Task-0 it still needs (OWN, deep).** Falsifiable questions before any code:
- Q-S1.1: Can a hashed/walked page table + SR/SDR1/BAT consumer coexist with `DIRECT_ADDRESSING` such that paravirtual is byte-identical? (i.e. is the MMU a *mode* switchable per profile, or does it contaminate the hot V=P path?)
- Q-S1.2: Does the host arm64 backend require a software walker on every translated access, or can large-window host mappings (mmap aliasing) realize the few segment swaps the NK actually performs (Q2 MMIO swap @ 0x50325894)? Cost model: per-access walker vs. window-remap.
- Q-S1.3: What is the minimal translation faithful enough for the NK — full hashed-PTE walk, or a "segment + context table" emulation that satisfies the per-context `mtsrin` reload (0x50315290) and the 13 `tlbie`?
- Provenance ritual (A5): re-verify the Q2 pinned addresses against the NanoKernel-v02.27 parcel md5 `61c176e90b6365e84e5c660d703e56af`, run base 0x50310000.

**Falsifiable gates (written in advance).**
- G1.a: microtest — two contexts, same VA, different PA via `mtsrin`; translated load returns the per-context PA. (Identity-only mapping FAILS this gate by construction — this is the anti-false-clean gate, A3.)
- G1.b: MMIO-via-segment-swap microtest — install I/O segment, DR-on, load resolves to the swapped target; restore SR. (Mirrors 0x50325894.)
- G1.c: `make e2e` paravirtual byte-identical A/B (paged path gated OFF) — the V=P hot path unchanged.
- G1.d: `make test-jit` = 100%; `make -C SheepShaver/src/machine test` green.

**Env-gate.** `SS_M18_PAGED_MMU` (default OFF) AND `MachineProfileIsNewWorld()`. Paravirtual untouched.

**File ownership (parallel safety).** New: `SheepShaver/src/machine/paged_mmu.cpp` + `test_paged_mmu.cpp` (+ Makefile target, pre-wired by coordinator). Edits (serialized, single-owner): the PPC translate path `kpx_cpu/src/cpu/ppc/ppc-execute.cpp` (the `mtspr/mfspr/mtsr/mtsrin` and load/store translation hook — LAW-module, strong tier), `Unix/sysdeps.h` (mode switch behind the gate), `machine/reset_supervisor*` consumer. **This stage owns the PPC translate hot path — no other stage may edit it concurrently.**

---

### Stage 2 — Trampoline loader + OF-CI callback + Core99 DT model

**Effort band: WEEKS each for {loader+CHRP entry; OF-CI dispatch; Core99 DT model} (boot-disjoint foundations; per A7 each plausibly weeks). Needs a SMALL confirm-Task-0.**

**Goal (PASS-vs-DIAGNOSTIC).** PASS: SS loads `MacOS.elf` at its ELF vaddrs (data `0x100000`, exec `0x200000`), sets the CHRP entry ABI (r5 = OF-CI callback ptr), jumps to entry `0x20f078`; the Trampoline runs through its 3 OF wrappers (`0x20dbec`/`0x20dcc0`/`0x20ddb4`) against the synthesized Core99 DT, resolving all 21 direct services + the `call-method` set + the 3-word `interpret` shim, and reaches its `NanoKernelEntry` handoff. Falsifiable: the Trampoline completes its DT walk + `/mmu` build sequence without an unresolved OF call (counter = 0 unresolved). DIAGNOSTIC: how many OF calls fired; which DT props were read; the concrete handoff PC.

**Gating Task-0 it still needs (SMALL — confirm, not discover).** Q0-A already bounded the call surface. This Task-0 closes:
- Q-S2.1 (Q3 residue, mechanism + DT shape per A6): the exact set/order of DT properties the loader's DT must answer for the Trampoline's walk (largely from the gating Task-0's [QEMU-BEHAVIORAL] trace + CORE99-MACHINE-DESCRIPTION).
- Q-S2.2: pin the CHRP entry register contract (r3/r4/r5) from `MacOS.elf` entry disasm + OpenBIOS behavior (Q0-E already established r5 = OF-CI callback; confirm r3/r4).
- Q-S2.3: the load-bearing DT property = `interrupt-map`/`interrupt-map-mask` on `/pci/mac-io/interrupt-controller` — pin its *shape* from CORE99-MACHINE-DESCRIPTION (NOT from the QEMU oracle value, A6).

**Falsifiable gates (written in advance).**
- G2.a: loader places both PT_LOAD segs at correct vaddrs inside the guest RAM aperture; PIC self-reloc stub at `0x20f078` runs; r2 = `0x1001e8` after entry (matches static + QEMU).
- G2.b: OF-CI dispatch resolves every one of the enumerated services (unit test drives all 21 + `call-method` names + `key?`/`key`/`reset-all`); unresolved-call counter = 0.
- G2.c: DT model answers every `finddevice` path + `getprop` key the Trampoline issues; `nextprop` tree-walk terminates.
- G2.d: `/mmu` `claim`/`translate`/`map` routed to the S1 paged MMU (until S1 lands: routed to the recording stub, flagged NON-ACCEPTANCE — build/unit only).
- G2.e: `make test-jit` = 100%; paravirtual byte-identical (all new code structurally inside newworld/env gates — inertness argument per §6).

**Env-gate.** `SS_M18_TRAMPOLINE` (loader+handoff) and `SS_M18_OFCI` (callback+DT) — default OFF, AND `MachineProfileIsNewWorld()`.

**File ownership.** New (no contention): `SheepShaver/src/openfirmware_ci.cpp` (OF-CI callback + Core99 DT model) + `SheepShaver/src/include/openfirmware_ci.h` + a unit test under `SheepShaver/src/machine/` (pre-wired Makefile target). `call-method` backends wire to existing SS subsystems via injected callbacks (the dev_cuda/adb_stub decoupling pattern, §3): `read-blocks`/`write-blocks`/`block-size` → SS boot disk; display (`dimensions`/`set-colors`/`fill-rectangle`/`draw-rectangle`) → framebuffer or no-op; `instantiate-rtas` → minimal stub; `/mmu` → S1. Edits (serialized): the newworld boot path that loads the ROM/launches the Trampoline — `rom_patches.cpp` (`PatchROM_NW_trampoline` becomes loader-gated) + `kpx_cpu/sheepshaver_glue.cpp` launch seam. **Stage 2 owns `openfirmware_ci.*` and the loader seam; it must NOT edit the PPC translate path (S1's).**

---

### Stage 3 — two-supervisor reconciliation (Q1, the architectural crux)

**Effort band: MONTHS / UNKNOWN-MONTHS (the weeks-vs-months crux). Needs its OWN deep Task-0.**

**Goal (PASS-vs-DIAGNOSTIC).** PASS: a coherent architecture in which the real NanoKernel-v02.27 owns the live 68k/exception/scheduler supervisor regime, and SheepShaver provides the bare PPC core + device models beneath it, such that the NK runs from its `rfi`-at-0x5031003c entry through its DEC reschedule loop (0x50313200) without contending with SS's `execute_68k`/`ExcEnter`/scheduler for the same live state. Falsifiable: the NK's EXT/SC/DEC/PROGRAM exception entries (0x50314880/0x50314ac0/0x50313200/0x50314700) are the live handlers (SS's `g_exc_entry_table` yields to them when gated ON), proven by a probe boot reaching the NK DEC loop with SS's scheduler quiescent. DIAGNOSTIC: which SS scaffolding pieces ended up REPLACED vs CO-OWNED; the re-bind status of the Execute68k pair.

**Gating Task-0 it still needs (OWN, deep — the architectural decision).** Q1 already pinned the headline: **no handoff boundary; all 5 ledger surfaces FIGHT; the Execute68k pair is an SS synthetic absent from the parcel.** This Task-0 decides the architecture, not whether there's a collision:
- Q-S3.1: Is "SS yields the supervisor role to the NK" coherent? Concretely — can SS's `execute_68k` (`sheepshaver_glue.cpp:1461/1504-1505`) be driven *by the NK's ECB/exception path* (NK calls into the 68k emulator the way real hardware would), rather than SS driving it? Or does the NK reach 68k entirely through its own EmulatorCode @ 0x360000, making SS's `execute_68k` dead under newworld?
- Q-S3.2: What happens to `machine/exc_core.cpp` `ExcEnter`/`ExcRfi` (PEM masks are LAW) and the SS scheduler (`event_sched.cpp`/`virt_clock.cpp`) when the NK owns DEC? Does SS's scheduler become a no-op under the gate, or does it co-own time-sharing with the NK DEC loop (the third "CO-OWN/TIME-SHARE" verdict, A4)?
- Q-S3.3: Is there ANY coherent yield point, or is the only coherent model "SS is the hardware, NK is the OS from `rfi`-at-0x5031003c onward" — i.e. SS stops being a supervisor entirely under newworld? (Q1 strongly implies the latter; this Task-0 must confirm it is *implementable* without unravelling the paravirtual hybrid.)
- Tension to resolve: does this unravel the whole hybrid architecture? (See Self-review — flagged for red team.)

**Falsifiable gates (written in advance).**
- G3.a: probe boot — NK reaches its DEC reschedule loop (0x50313200) with SS's `execute_68k`/scheduler structurally inert (counter: SS-scheduler-ticks = 0 while NK live).
- G3.b: the NK's exception entries are the live handlers (a guest exception routes to 0x50314880-class, not SS's `ExcEnter`), proven by a parked-PC assertion in the NK exception cluster.
- G3.c: Execute68k pair disposition resolved — either re-bound to ROM EmulatorCode @ 0x360000 (and SS's `execute_68k` dead under the gate) or proven still-driven; whichever, the `:1504-1505` live read is accounted for, not left dangling.
- G3.d: paravirtual byte-identical (the supervisor yield is structurally gated to newworld; paravirtual keeps SS as supervisor). `make test-jit` = 100%.

**Env-gate.** `SS_M18_NK_SUPERVISOR` (default OFF) AND `MachineProfileIsNewWorld()`. **This gate is the program's most dangerous flip** — it transfers the supervisor role; paravirtual must be provably unreachable.

**File ownership.** Edits (heavily serialized, strong tier, LAW-module): `machine/exc_core.cpp` (ExcEnter/ExcRfi yield), `kpx_cpu/sheepshaver_glue.cpp` (the `interrupt()`/`deliver_pending_dec_exception()`/`execute_68k` seams + Execute68k pair re-bind/retire), `machine/event_sched.cpp` + `machine/virt_clock.cpp` (scheduler yield), `rom_patches.cpp` (retire the entry-vector synthesis forge in favor of the live NK). **Stage 3 owns the entire SS supervisor surface — it MUST be the only stage in flight on these files.** Depends on S1 (translate path) and S2 (loader) already landed.

---

### Stage 4 — disk System/Enabler IM-init in the loop (Q3)

**Effort band: MONTHS (Q3-forced; also HARD-BLOCKED on the 9.2.x ISO asset). Inherits Q3 residues as its Task-0.**

**Goal (PASS-vs-DIAGNOSTIC).** PASS: a genuine Mac OS 9.2.x System/Enabler loads from disk, its CFM/PEF IM-init runs under the live NK regime (S3) with translation live (S1), and **populates CGRP `*(KDP-0x338)` from guest code** — `+0x04='CGRP'`, `+0x38/+0x3c/+0x40` handler/ptr/stack arrays in disk-loaded RAM (0x0045xxxx–0x0046xxxx, CFM TOC 0x00466ba0), `+0x44`=entry count — replacing the M16 frozen-zero forge. Falsifiable: a watchpoint on the CGRP descriptor fields fires from a **disk-RAM writer PC** (not a ROM/forge PC), and CGRP is non-zero and structurally valid (the gate that was previously frozen-zero). DIAGNOSTIC: the next wall after CGRP (expected: Cuda IFR/IER).

**Gating Task-0 it still needs (inherits Q3 explicit residues).** Q3 left two residues, now closable because S1+S3 make execution translatable/observable:
- Q-S4.1 (writer-PC residue): with S1's paged MMU live, virtual watchpoints work — trace the IM-init writer PC that builds CGRP (defeated the gdbstub watchpoint at gating-Task-0 time because it ran physically/real-mode).
- Q-S4.2 (live DT-read-order residue): the ordered list of what IM-init reads (from the DT the NK exposes) and writes when building CGRP — the contract S2's DT must satisfy.
- Q-S4.3 (asset): confirm a genuine 9.2.x System/Enabler medium exists and boots far enough to run IM-init (see Parallel asset dependency).

**Falsifiable gates (written in advance).**
- G4.a: CGRP non-zero and valid after boot (`+0x04='CGRP'`, `+0x44` = expected entry count) — written by a disk-RAM PC (watchpoint-confirmed), NOT a forge.
- G4.b: no SS-side CGRP forge writes occur under the gate (the M16 forge is retired/inert; counter: forge-writes = 0).
- G4.c: the boot reaches the **first non-IM-init wall** (program PASS condition; expected Cuda IFR/IER) — the GOOD outcome.
- G4.d: paravirtual byte-identical; `make test-jit` = 100%.

**Env-gate.** `SS_M18_IMINIT_DISK` (default OFF) AND `MachineProfileIsNewWorld()`.

**File ownership.** Edits (serialized): `rom_patches.cpp` / `kpx_cpu/sheepshaver_glue.cpp` (retire the CGRP forge: `*(KDP-0x338)` family, `[CGRP+0x38/0x3c/0x40/0x44]`, service 0x503148e0 binding) behind the gate; the disk/boot path that mounts the 9.2.x medium. New: an asset manifest entry for the 9.2.x medium. **Stage 4 owns the CGRP-forge retirement; depends on S1+S2+S3 landed and the ISO present.**

---

## Acceptance discipline (per stage — uniform)

Each stage runs the milestone-machine acceptance (§2):
1. **Env-on first.** Run the full gate battery with the stage's `SS_M18_*` gate(s) ON before flipping any default. Record every stage gate (G_.a…d) PASS with numbers.
2. **Flip is the LAST step.** The default-on flip (if any) is the final action; any gate failure post-flip ⇒ revert the flip in-task.
3. **Revert-on-red.** Any post-flip gate red ⇒ revert.
4. **Fix budget.** Telemetry freely; ONE small in-scope fix per falsified contract, full gates re-run; a SECOND falsification of the same contract ⇒ stop, re-scope, re-plan that stage.
5. **Paravirtual byte-identical is a gate, not a hope** — enumerated fields (jitter counters excluded), via `make e2e` on paravirtual OR the gated-off byte-identical A/B boot substitute when every new line is structurally inside newworld/env gates (state which in the commit, §6).
6. **`nw-northstar` observe line** (report-only) quoted at each stage close.
7. Gate tiers via `tools/gates.sh`; boot assertions via `ss-slot-boot.sh --expect/--absent`.

---

## Stop-rule (triggers named in advance; one-iteration mechanics)

1. **The tempting wrong fix — "re-forge CGRP instead of running IM-init."** If Stage 4 stalls on the ISO or the writer-PC residue and anyone reaches for the M16 hand-written CGRP table, STOP. The M8→M17 arc proved per-wall forging is structurally bankrupt (AGENT-CONTEXT M17 series tripwire). The deliverable is CGRP built by *real guest IM-init*, never a forged output. Route A is decided.
2. **The tempting wrong fix — "fake the MMU with identity-only when translation is load-bearing."** Q2 pinned POSITIVE consumption (per-context `mtsrin` @ 0x50315290; MMIO segment swap @ 0x50325894). G1.a is built to FAIL on identity-only by construction. A readback-consistency check is a **false-clean** (A3) — it is NOT evidence the MMU works. If Stage 1 is tempted to ship identity mapping, STOP: that defaults to UNKNOWN→months, not "fine" (A8 #7).
3. **The tempting wrong fix — "claim a handoff boundary that the recon proved fictional."** Q1 pinned: there is NO instruction sequence where NK init completes and yields to an SS-live-emulator steady state. If Stage 3 reaches for "re-inject post-handoff" as if a yield point exists, STOP — that is the falsified sketch. The coherent model is SS-as-hardware / NK-as-OS, or an explicitly-pinned CO-OWN/TIME-SHARE (A4); never a fictional handoff.
4. **Unbounded disasm in any stage Task-0.** Each Task-0 carries a function/instruction window (the proven kill-switch; two predecessor agents died in unbounded disasm). A surface not classifiable within the window is recorded as the conservative residue (FIGHT?/requires-paged-MMU-UNKNOWN/architectural-risk), never silently optimistic.
5. **QEMU-as-address-oracle.** No QEMU MMIO address cited as a reference value for our machine layer (A6/A8 #6); mechanism/order/shape-from-CORE99 only. Do not break on a SheepShaver NK address under QEMU — derive the live run address from the handoff first.
6. **"The verdict is months" is the answer, not a trigger to re-scope Route A.** A FIGHT/CO-OWN/paged-MMU finding is the priced outcome; record it, the next stage inherits the scope. Route A is not relitigated.

**One-iteration rule (operationalized).** A pinned answer falsified by a later step within a stage → (a) dated falsification addendum entry; (b) ONE bounded re-pin (≤1 disasm window or ≤1 boot from the stage's remaining cap); (c) resume. A SECOND falsification of the same answer ⇒ STOP, escalate to a re-plan of that stage (re-scope, not patch-on-patch).

---

## Parallel asset dependency (Stage 4 HARD-BLOCK)

Stage 4 requires a **genuine Mac OS 9.2.x install medium** (System + Enabler with the CFM/PEF IM-init that builds CGRP). The in-tree `macos921.dsk` is reportedly **actually 8.6** (wrong system version); the e2e assets in tree are `bench.dsk`/`apps.dsk` (not a 9.2.x installer). **[draft-verified: no `macos921.dsk` at the draft path; only `SheepShaver/e2e/assets/{bench,apps}.dsk` present.]** This is a **parallel asset-acquisition workstream** that must run AHEAD of Stage 4 (it gates nothing else, so it can start at program kickoff): source/verify a genuine 9.2.x medium, record its md5/SHA + provenance in the asset manifest, and confirm it boots far enough under the S1+S2+S3 stack to run IM-init. **Surface the gap; do NOT forge CGRP to work around a missing asset** (Stop-rule #1). If the genuine medium cannot be obtained, Stage 4 is BLOCKED and the program PASS is unreachable — that is a reportable program-level blocker, not a license to forge.

---

## Self-review record

Spec coverage: program structured as staged milestones each runnable through the machine (§2); top-level dependency graph (S1 gates S3+S4 via MMU; S2 boot-disjoint, parallel with S1 Task-0); per-stage PASS-vs-DIAGNOSTIC, own-Task-0 designation (S1/S3 deep; S2 small; S4 inherits Q3 residues), falsifiable gates written in advance, env-gates (`SS_M18_*` + `MachineProfileIsNewWorld()`, paravirtual byte-identical, `make test-jit`=100), file ownership for parallel safety, per-surface effort bands as stage gates (A7); blocking/dependency table with residue dispositions; uniform acceptance discipline (env-on-first/flip-LAST/revert-on-red/fix budget); stop-rule naming all three tempting-wrong-fixes + one-iteration mechanics; the 9.2.x ISO parallel asset hard-block. All pre-pinned static facts marked re-verify-before-use; live-verified facts tagged [verified 2026-06-14].

**Tensions flagged FOR the red team:**
1. **Stage 1 is the load-bearing unknown and I cannot derisk it on paper.** "A paged MMU mode for the newworld profile that leaves paravirtual V=P byte-identical" assumes the MMU is a clean per-profile *mode*. But the V=P assumption is structural (`sysdeps.h:96`; `host = NATMEM_OFFSET + guest`) and threads the PPC translate hot path — a mode switch may contaminate the paravirtual hot path or impose a per-access walker cost that makes the JIT untenable. RED TEAM: is a real paged MMU even tractable in SS's hybrid model without breaking paravirtual, and is "microtest two-context translation" (G1.a) a sufficient PASS, or does Stage 1 need a *full NK boot under translation* to be believed — pushing its band from months to UNKNOWN-months?
2. **Stage 3 may unravel the whole hybrid architecture.** "SS yields the supervisor role to the NK" is, taken to its conclusion (Q1: no handoff boundary), "SS stops being a supervisor under newworld; the NK is the OS from `rfi`-at-0x5031003c." That is coherent for newworld but the codebase is a *hybrid* — paravirtual keeps SS as supervisor, sharing `exc_core.cpp`/`sheepshaver_glue.cpp`/the scheduler. RED TEAM: can the supervisor role be cleanly profile-gated, or does newworld-yields-NK fork the supervisor surface so deeply that the "byte-identical paravirtual" claim becomes unfalsifiable / the two regimes can no longer share the LAW modules? Is there a third architecture (SS hosts the NK as a guest supervisor with a thin shim) the REPLACE/CO-OWN frame hides?
3. **The dependency graph assumes S2 can be built and unit-tested against a stub `/mmu` and stub handoff before S1/S3 land.** If the Trampoline's `/mmu` `claim/translate/map` sequence cannot even be *exercised* without real translation (because the Trampoline reads back what it mapped), the stub is a false-clean and S2 cannot be validated ahead of S1 — collapsing the parallelism the graph relies on. RED TEAM: is S2's stub-`/mmu` build/unit phase real, or does S2's acceptance fully serialize behind S1, making the "S2 parallel with S1 Task-0" edge cosmetic?
4. **Effort bands are per-surface but the program roll-up is sequential-months-times-four.** Each months stage gates the next (S1→S3→S4), so the program is not max(stage) but roughly sum of the critical-path months stages. RED TEAM: should the program advertise a critical-path estimate (S1 then S3 then S4, each months) rather than per-stage bands that read as if parallelizable?

## Red-team record

*(empty — to be filled by the two parallel reviewers: PROCESS + TECHNICAL/CONTRACTS)*
