# SS_M18_TRAMPOLINE_LLE — implementation program (STAGED)

> **Scale (honest):** **multiple quarters.** Critical path = **S1 → S3 → S4, strictly sequential**
> (S1 gates S3 gates S4), each months (S1 & S3 possibly UNKNOWN-months). The roll-up is the SUM of the
> critical-path stages, NOT max(); S2a parallelizes only weeks of dispatch/DT work against S1's Task-0.
>
> **This is a PROGRAM of staged milestones**, each run through the milestone machine
> (`docs/MILESTONE-WORKFLOW.md` §2): **each Stage = one milestone** with its own Task-0 + red-team
> BEFORE any of its code. "Stage" is used throughout (never "milestone" for a sub-unit).
> Route A is SETTLED (`FINDINGS-trampoline-re.md` Q0-E) and is NOT relitigated — this plan is the HOW.
>
> **Status:** rev-4 (2026-06-14) — consolidated single-state. Supersedes the rev-1/2/3 layered draft
> (audit trail in Appendix A). Folds the gating-Task-0 amendments (A1–A9), the program red-team (B1–B10),
> the lateral-moves pass (C1–C4), and the technical-writer + developer-advocate reviews.

## STATUS (the single done-vs-todo board — update as stages move)

| Item | Status | Next gate / action | Blocked by |
|---|---|---|---|
| SS_M18 gating Task-0 | ✅ DONE | — (Route A GO but MONTHS) | — |
| Kickoff: Discriminator-A | ✅ DONE — COARSE → S1 path a | — | — |
| Kickoff: donor studies (`DONOR-NOTES.md`) | ✅ DONE | — | — |
| Kickoff: 9.2.x ISO | ✅ DONE — in hand (`ASSETS` R2) | copy into asset area at S4 | — |
| **S1 — paged MMU** | **▶ IN PROGRESS — S1-impl plan rev-2 (3-reviewer); Task A AUTHORIZED, Task B (window) PARKED** | **Task A (reshaped, building now):** `high_bat[16]` dead-code INSURANCE + `paged_mmu.cpp` mechanism-agnostic translation fn + `test_paged_mmu.cpp` (SPEC-derived oracle, segment+BAT+HTAB). **AD-2 high-BATs FALSIFIED = dead code (feature-gated, no presentable PVR enables); RESIDUE-PASS survives on AD-1.** **Task B PARKED:** live PTE engine remaps via HTAB-stores+`tlbie` not `mtspr` → window's mtspr-hooks desync; needs tlbie/HTAB interception design OR G1.e disjointness proof. Walker/softmmu = DEFAULT until G1.e. `…ss-m18-s1-impl-paged-mmu.md` | — (unblocked) |
| S2a — OF-CI + Core99 DT | **Task-0 recon DONE → RESIDUE-PASS (3-reviewer folded); S2a-impl CLEARED** | open S2a-impl: new `openfirmware_ci.{cpp,h}` + DT model (incl scaffolding nodes `/chosen` `/aliases` `/options` `/rtas` `/cpus` `/rom/macos`) + unit test. **Binding (ADV-1):** finddevice = component-wise unit-address-insensitive match; battery uses SHORT static spellings. interrupt-map value + DT-coverage closure = named hand-offs to first integration boot | — (unblocked) |
| S2b — loader + /mmu + handoff | blocked | — | S1 |
| S3 — two-supervisor reconciliation | blocked | S3 deep Task-0 | S1 + S2 |
| S4 — disk IM-init → CGRP | blocked | S4 Task-0 (live trace) | S3 (ISO in hand) |

## Orientation

- **Read first:** `docs/planning/newsheep/GLOSSARY.md` (terms) + `FINDINGS-trampoline-re.md` "SS_M18 gating
  Task-0" (the Q1/Q2/Q3 findings this program rests on).
- **Amendment-ID legend** (Appendix A holds the full text): **A\*** = gating-Task-0 rev-2 amendments
  (in `…-gating-task0.md`); **B\*** = this program's red-team amendments; **C\*** = the lateral-moves pass.
  This body IS the current truth; the IDs are provenance pointers, not live instructions.
- **Load-bearing terms** (first-use): **NK** = NanoKernel-v02.27 (the resident PPC supervisor parcel @ run
  base `0x50310000`). **IM-init** = the guest's Interrupt-Manager init (disk/CFM-loaded code that builds
  CGRP). **CGRP** = the interrupt-group descriptor at `*(KDP-0x338)` the whole M8→M17 arc fought.
  **OF-CI** = OpenFirmware client interface. **DT** = device tree. **V=P** = virtual==physical
  (`DIRECT_ADDRESSING`, host = `NATMEM_OFFSET + guest`) — the addressing mode the **paravirtual** profile
  uses. **CFM/PEF** = Code Fragment Manager / Preferred Executable Format (Mac shared-library code).
  **PEM masks** = the MSR entry/exit bitmasks in `exc_core.cpp` (treated as LAW — do not edit casually).

## Program goal (PASS-vs-DIAGNOSTIC)

**PASS (Program DoD — falsifiable):** the frozen-struct class (CGRP / NK dispatch state / the M16
ROM-absent handler PC) is populated by **real guest code** (the real `MacOS.elf` Trampoline + the real NK
+ a real disk System/Enabler IM-init) computing real values from a synthesized Core99 DT, under
`SS_M18_*` + `MachineProfileIsNewWorld()`, with the **paravirtual profile byte-identical** and
`make test-jit` = 100 (the score contract; the harness runs 353 vectors). Program PASSES when, with all
stage gates ON, a newworld boot reaches the **first non-IM-init wall** (expected: the Cuda IFR/IER wall,
charter §6 / M14-FINDINGS) — i.e. CGRP is guest-built, no longer frozen-zero. **Critical-path estimate:
S1 → S3 → S4 sequential, ≈ multiple quarters** (each months; S1/S3 possibly UNKNOWN-months until their
Task-0s resolve).

**DIAGNOSTIC (recorded, never a gate):** how far the all-on boot gets past CGRP; `nw-northstar`
`[NW-PROG verdict]`; the *identity* of the next wall (PASS only requires reaching *a* non-IM-init wall,
not specifically Cuda); per-stage probe-boot markers. Reaching Finder is the north star, NOT the DoD.

**Scope law:** no stage may forge a frozen output (re-hand-write CGRP), fake the MMU with identity-only
when translation is load-bearing, or claim a "handoff boundary" the recon proved fictional (Q1). See
Stop-rule.

## Authoritative inputs

| Doc / source | Role |
|---|---|
| `docs/MILESTONE-WORKFLOW.md` §2/§3/§4/§6/§6c | The machine each stage runs through (env-gated default-OFF; falsifiable-in-advance gates; acceptance env-on-first/flip-LAST/revert-on-red; file ownership; QEMU-before-static-RE). **External doc** — its "§N" cites are not sections of THIS plan. |
| `docs/planning/newsheep/FINDINGS-trampoline-re.md` "SS_M18 gating Task-0" + "SS-integration sketch" | The Q1/Q2/Q3 findings + cited addresses (all **re-verify before use**) + the reconciliation table. |
| `docs/superpowers/plans/2026-06-14-ss-m18-trampoline-lle-gating-task0.md` (rev-2, A1–A9) | The gating-Task-0 plan; its amendments are inherited (folded into the stages below). |
| `docs/AGENT-CONTEXT.md` — Constants, env-gate census, gate tiers, QEMU rig recipe | Standing facts; `MachineProfileIsNewWorld()`; the Execute68k pair. |
| `docs/planning/sheepshaver-research/MMU-WITHOUT-GUTTING-FLATMEM.md` | The S1 technique ranking (Dolphin Dynamic-BAT winner; Discriminators A/B). |
| `docs/archive/2026-06/planning/{MMU-NANOKERNEL-MP-PLAN,HANDOFF-NEWWORLD-SUPERVISOR-MMU}.md` | Path A reuse map (parked supervisor/MMU work; obstacle map). |
| `docs/archive/2026-06/machine/M3-PIC-CUDA-DONOR-STUDY.md` + `docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md` | S2/S4 donor specifics + the Core99 DT source. |

## Codebase facts (carried; implementers RE-VERIFY each site before use)

> Offsets PRE-PINNED for orientation. Live-verified 2026-06-14 where tagged.

- `MachineProfileIsNewWorld()` — runtime read of process-wide `g_profile`, set once at init:
  `machine/machine_profile.cpp:96`, decl `include/machine_profile.h:37`. **[verified]** Fine for boot-time
  *mode selection*; MUST NOT become a per-memory-access branch in the JIT hot path.
- Glue = **`SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp`** **[verified — NOT `src/Unix/`]**. Execute68k
  pair read live every excursion `:1504–1505`; written `:3140–3143`; `interrupt()` `:1002`;
  `deliver_pending_dec_exception()` `:1122` (newworld-only); EXT/DEC `ExcEnter` `:1254`/`:1342`;
  `execute_68k` `:1461`. `reset_supervisor_for_test()` is a TEST helper here `:560` (sole caller `:2485`)
  — **there is no `machine/reset_supervisor*` file.**
- **Supervisor state already stored (Path A Wave 0):** `sprg[4]/sdr1/bat[16]/sr[16]/srr0/srr1/msr` in
  `kpx_cpu/src/cpu/ppc/ppc-registers.hpp:258`; full `mtspr/mtsr/mtsrin/mfspr/mfsr` handlers in
  `kpx_cpu/src/cpu/ppc/ppc-execute.cpp:1403`. They store inertly — **no translation consumer / no
  page-table walker** (grep `tlbie|htab|get_physical_address|page_table` → none). A write-then-read-back
  check is a **false-clean** for any MMU verdict.
- **The production memory path is the JIT, which has NO translation chokepoint:** the aarch64 JIT at
  `kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp` inlines every access as `LDR/STR [RMEMBASE,X0]`, RMEMBASE =
  NATMEM_OFFSET, at **80 emit sites** **[verified: 80 RMEMBASE occurrences]**. The interpreter DOES have a
  chokepoint (`kpx_cpu/src/cpu/vm.hpp:225` `vm_do_get_real_address`, `VMBaseDiff=NATMEM_OFFSET` **[verified]**)
  — but it is not the boot path. NATMEM reservation/`vm_alloc` layer: `Unix/main_unix.cpp` (~:2079
  NATMEM setup) — the shadow-remap hook point. `Unix/sysdeps.h:96` `DIRECT_ADDRESSING`.
- **Supervisor surface is ALREADY profile-forked:** the newworld arm of `check_spcflags` is at
  `kpx_cpu/src/cpu/ppc/ppc-cpu.cpp:1986` **[verified]** (`deliver_pending_dec_exception` is newworld-only).
  So S3 toggles an existing arm, not a gut.
- Exception core: `machine/exc_core.cpp` `ExcEnter:12` / `ExcRfi:104` (PEM masks = LAW). Machine-layer
  M0–M13 device models: `machine/{mmio_bus,virt_clock,event_sched,dev_scc8530,dev_via6522,dev_openpic}.cpp`.
- `SheepShaver/src/openfirmware_ci.cpp` does **NOT** exist yet (Stage 2 creates it). **[verified]**
- NK/constants: KDP=0x68ffe000, ECB=0x68fff000, mirror 0x50460000, DR table 0x50480000,
  ROMBase=0x50000000, **NK run base 0x50310000**, ROM EmulatorCode 0x50360000. NK exception entries: EXT
  0x50314880, SC 0x50314ac0, DEC 0x50313200, PROGRAM 0x50314700. CGRP service 0x503148e0, CGRP `*(KDP-0x338)`,
  fields `[+0x04]='CGRP' [+0x38/0x3c/0x40] arrays [+0x44]=18 entries`.
- **Q1 pinned (re-verify):** NK entry 0x50310000 (`mfmsr`→test MSR[DR]→`mtspr SRR0/SRR1`→`rfi` @0x5031003c);
  DEC reschedule 0x50313200. **Q2 pinned:** SDR1 install `mtspr SDR1` @0x50310604; SR program @0x503104b4+;
  per-context `mtsrin` loop @0x50315290; MMIO segment-swap+DR-toggle @0x50325894; 13× `tlbie`. **Q3 pinned:**
  CGRP built by disk/CFM IM-init in low RAM 0x0045xxxx–0x0046xxxx (CFM TOC 0x00466ba0), NOT the NK parcel;
  writer-PC + live DT-read-order are residues (writer ran real-mode → defeated virtual watchpoints).
- **Parcel provenance** (Task-0 gate-item-0 for every stage): NanoKernel-v02.27 =
  `/tmp/newsheep/dump-9.0.1/Parcels.src/MacROM.src/NanoKernel-v02.27`, 105280 B, md5
  `61c176e90b6365e84e5c660d703e56af`; canonical ROM md5 `66210b4f71df8a580eb175f52b9d0f88`. Re-extract via
  `tbxi` (venv `/tmp/newsheep/venv`) if /tmp evaporated.

## Dependency graph

```
            SS_M18 gating Task-0 (DONE; Route A GO / quarters)
                              │
   ┌──────────────────────────┼───────────────────────────┐
   │ (boot-disjoint, weeks)    │ (sequential months ▼)      │ (zero-dep, start now)
  S2a Task-0 + impl       S1 Task-0 ─► S1 impl          9.2.x ISO sourcing
 (OF-CI + Core99 DT,     (paged MMU, PREREQUISITE;       + donor read-only
  unit-tested, no boot)   gates S3 & S4)                  studies (warm-up)
   │                          │
  S2b (loader + /mmu + handoff) ── acceptance serialized behind S1
                              │
                          S3 Task-0 ─► S3 impl   (sequential months ▼)
                       (two-supervisor reconciliation)
                              │
                          S4 impl   (sequential months ▼)
              (disk IM-init in the loop → CGRP; HARD-BLOCKS on 9.2.x ISO)
                              │
                   PASS: CGRP guest-built; next wall = Cuda IFR/IER
```

## Blocking / dependency table

| Concern | Blocked by | Residue disposition (partial-findings-beat-stalling) |
|---|---|---|
| S1 paged-MMU mode (newworld) | **S1-Task-0** | Can't show coexistence with paravirtual V=P byte-identical → "paged-MMU-in-hybrid UNKNOWN → program-blocking-risk"; NEVER default to "identity will do". |
| S2b loader/handoff re-use of `ExcEnter`/`execute_68k` | **S3-Task-0** (Q1) | S2a builds against a stub `/mmu`; S2b acceptance waits on S1+S3. |
| S2b `/mmu` claim/translate/map | **S1** | Until S1: recording stub (logs requests; build/unit ONLY, flagged NON-ACCEPTANCE). |
| S2a OF-CI + Core99 DT contract | **S2a-Task-0** | Mechanism from the Q3 [QEMU-BEHAVIORAL] trace; property VALUES/SHAPES from CORE99-MACHINE-DESCRIPTION (defer OpenBIOS shapes). |
| S3 supervisor reconciliation | **S3-Task-0** (Q1) | No coherent yield/co-own model in window → "supervisor-handoff UNKNOWN → architectural-risk; months"; never assert a fictional handoff. |
| S4 disk IM-init → CGRP | **S3 + 9.2.x ISO + Q3 residues** | ISO absent → S4 HARD-BLOCKED (surface, don't forge). Writer-PC residue → S4 first task is the live trace (now observable under S1's MMU). |
| Program PASS | **S1 ∧ S2 ∧ S3 ∧ S4** | Each stage reports its own band; the program verdict is conditional until all land. |

---

## Stage 1 — NewWorld-profile paged MMU (PREREQUISITE per Q2)

**Effort band: UNKNOWN-months, leaning tractable** (host-page aliasing may make it a bounded map-change
hook; a per-access JIT walker would be perf-fatal). **Discriminator-A run 2026-06-14 → COARSE on
mechanism** (`FINDINGS-discriminator-a.md`): static RE of the md5-verified NK is conclusive — `mtsrin`
context loop @0x50315290 is **segment-granular (one 256 MB segment/iter)**, all 8 IBAT/DBAT pairs
programmed from a descriptor table, MMIO via SR-swap @0x50325894; the HTAB is the residual pageable map,
not the JIT-covered RAM/ROM. → **path a (Dolphin shadow-arena) indicated.** Residual FINE-falsifier (dense
mixed-perm 4 KB PTEs on JIT-covered regions) could NOT be exercised live (QEMU stalled in the OF→OS handoff
before NK MMU-install) → routed to S1's MMU-oracle unit test (test-ladder item 2); if it ever shows the NK
depending on such PTEs, re-band to softmmu. **Still needs its OWN deep Task-0.**

**Goal.** PASS: a paged-MMU mode (gated `SS_M18_PAGED_MMU` + `MachineProfileIsNewWorld()`) in which a guest
`mtsr`/`mtsrin` + SDR1 install followed by a translated load/store **resolves to the SR/page-table-selected
PA, not the V=P address — proven UNDER THE JIT PATH** (not just the interpreter) by a microtest where the
same VA maps to two PAs under two contexts (the Q2 consumption-#1 shape @0x50315290). Paravirtual stays
V=P and byte-identical (the paged path is structurally unreachable when `!MachineProfileIsNewWorld()`).
DIAGNOSTIC: walker/remap counters; how far a probe NK boot gets under translation.

**Donor (C1) — details in `docs/planning/newsheep/DONOR-NOTES.md` (full pinned SHAs).** **PORT Dolphin
"Dynamic BAT" shadow-arena:** the arena remap is `MemoryManager::UpdateDBATMappings()` in
**`Source/Core/Core/HW/Memmap.cpp`** (~L233; the BAT-table rebuild is `MMU::DBATUpdated()` in `MMU.cpp`) —
**NOT** `JitArm64/Memmap.cpp` (the JIT is deliberately uninvolved — that's the point). Remap the NATMEM
arena at the rare `mtspr` BAT/SDR1/SR — the **80 JIT `LDR/STR` sites stay bit-identical**. The 16 KB-host /
4 KB-guest fork is Dolphin's `CanCreateHostMappingForGuestPages()` (~L343) — **this IS our Discriminator-A
risk** (it falls to the slow path unless the four constituent 4 KB pages are aligned+contiguous+same-perm).
**ORACLE only:** PearPC / QEMU softmmu translator to diff our PA outputs (never port). DingusPPC is NOT a
usable S1 oracle (no KeyLargo/OpenPIC). Dolphin master SHA `144d19433aa734c19c34e5978a1b817d2aa12663`.

**Reuse (C3).** Supervisor state + `mtspr/mtsr/mtsrin` handlers already exist (Wave 0; "honor-the-write"
largely done). Hook the shadow remap in the NATMEM/`vm_alloc` layer (`main_unix.cpp` ~:2079). Dead-ends
not to repeat: `vm_remap` from `MAP_JIT` (Apple-Silicon `KERN_PROTECTION_FAILURE` — irrelevant to the RW
data shadow); per-access softmmu (perf-fatal); HV.framework (no PPC guest).

**S1-Task-0 (own, deep).** Q-S1.1 can a shadow MMU coexist with `DIRECT_ADDRESSING` byte-identical?
Q-S1.2 (the **walker-vs-window** decision) host-page aliasing in the NATMEM reservation (preferred) vs ~80
JIT-site walker — informed by the **Discriminator-A runbook below**. Q-S1.3 minimal faithful translation
(full hashed-PTE walk vs segment+context-table satisfying `mtsrin`@0x50315290 + 13× `tlbie`). Gate-item-0:
parcel provenance (md5 `61c176e9…`).

**Test ladder (iterative-success contract).** (1) **Discriminator-A probe FIRST** (runbook below) — coarse
(segment/BAT) ⇒ shadow-arena 16 KB-host-safe; fine 4 KB mixed-perm ⇒ softmmu, fast path lost. (2)
standalone MMU unit test (no boot) diffing our shadow-arena vs the reference-translator oracle on identical
`(SR/BAT/SDR1, EA)` → equal PA, **exercising the JIT path**. (3) `make bench` ns/insn memory kernels = fast
path unchanged. (4) real `make e2e` paravirtual + multi-run soak (NOT the inertness substitute — S1 edits a
shared runtime-gated path).

**Gates.** G1.a two-context translation under the JIT (identity-only FAILS by construction). G1.b
MMIO-via-segment-swap microtest (mirrors 0x50325894). G1.c `make e2e` paravirtual A/B + soak unchanged.
G1.d `make test-jit`=100 + `make -C SheepShaver/src/machine test`. **G1.e** a probe NK boot under
translation reaches a translation-dependent landmark (the `mtsrin` context-switch executing correctly live)
— microtests alone are necessary-not-sufficient for the foundational stage.

**Env-gate.** `SS_M18_PAGED_MMU` (default OFF) AND `MachineProfileIsNewWorld()`; selected once at boot
(never a per-access branch).

**File ownership.** New: `machine/paged_mmu.cpp` + `machine/test_paged_mmu.cpp` (Makefile target pre-wired
by coordinator). Edits (serialized, strong tier): the **host NATMEM/`vm_alloc` reservation layer**
(`Unix/main_unix.cpp`) and/or the **JIT memory emit sites** (`kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`) per
the Q-S1.2 decision; `Unix/sysdeps.h` (mode switch behind the gate); the interpreter chokepoint
`kpx_cpu/src/cpu/vm.hpp` for parity. **S1 owns the memory-translation surface; no other stage edits it
concurrently. S1 must NOT touch `sheepshaver_glue.cpp` (S2/S3).**

---

## Stage 2 — Trampoline loader + OF-CI callback + Core99 DT (SPLIT: S2a parallel / S2b serial)

**Effort band: weeks** for {S2a OF-CI dispatch + DT model} (boot-disjoint, parallel with S1-Task-0) +
{S2b loader + handoff} (acceptance serialized behind S1). **S2a-Task-0 is small + one bounded discovery.**

**S2a — OF-CI callback + Core99 DT (parallel, unit-tested, no boot).**
- **Goal.** PASS: the `of_ci_callback()` resolves all 21 direct services + the `call-method` set + the
  3-word `interpret` shim against the synthesized Core99 DT; unit-test unresolved-call counter = 0.
- **Donor (C1).** QEMU OpenBIOS / `hw/misc/macio/macio.c` / `hw/intc/openpic.c` (SHA `de5d8bfd…`) as
  **oracle + reimplement-to-spec** (OpenPIC region map pinned in M3-DONOR-STUDY: `0x40000`, sub-regions
  glb/src/cpu). The captured Q3 [QEMU-BEHAVIORAL] Trampoline trace is the expected-call-sequence oracle.
- **S2a-Task-0.** Q-S2a.1 DT read set/order (from the Q3 trace + CORE99). Q-S2a.2 CHRP entry register
  contract (r5 = OF-CI callback confirmed; confirm r3/r4). **Q-S2a.3 (the one real discovery):** the Core99
  `interrupt-map`/`-mask` tuple SHAPE — pin from `CORE99-MACHINE-DESCRIPTION.md`, NOT the QEMU value; if the
  doc lacks it → flagged residue feeding S2b, NEVER a guessed shape.
- **Gates.** G2a.b unit test drives all 21 services + call-method names + `key?`/`key`/`reset-all` →
  unresolved = 0. G2a.c DT answers every finddevice/getprop/nextprop the Trampoline issues. G2a.e
  `make test-jit`=100; new-file inertness substitute OK (genuinely new code).
- **File ownership.** New: `src/openfirmware_ci.cpp` + `src/include/openfirmware_ci.h` + a unit test under
  `machine/` (Makefile target pre-wired). `call-method` backends wire via injected callbacks
  (dev_cuda/adb_stub decoupling pattern).

**S2b — loader + `/mmu` + NanoKernelEntry handoff (acceptance serialized behind S1).**
- **Goal.** PASS: SS loads `MacOS.elf` at its ELF vaddrs (data 0x100000, exec 0x200000), sets the CHRP
  entry ABI (r5 = OF-CI callback), jumps to entry 0x20f078, r2=0x1001e8 after entry; the Trampoline
  completes its DT walk + `/mmu` build and reaches `NanoKernelEntry` with **0 unresolved OF calls**.
- **Gates.** G2b.a loader vaddr placement + PIC self-reloc stub runs. G2b.d `/mmu` routed to S1's paged
  MMU (pre-S1: recording stub, **NON-ACCEPTANCE — build/unit only**). G2b.e paravirtual byte-identical.
- **File ownership.** Edits (serialized): the newworld boot path — `rom_patches.cpp`
  (`PatchROM_NW_trampoline` becomes loader-gated) + `sheepshaver_glue.cpp` launch seam. **S2 must NOT edit
  the memory-translation surface (S1's).**

**Env-gates.** `SS_M18_OFCI` (S2a) and `SS_M18_TRAMPOLINE` (S2b), default OFF, AND
`MachineProfileIsNewWorld()`.

---

## Stage 3 — two-supervisor reconciliation (Q1, the architectural crux)

**Effort band: months / UNKNOWN-months.** **Needs its OWN deep Task-0.** **Good news (B4):** the
supervisor surface is ALREADY newworld-gated (`ppc-cpu.cpp:1986`) → S3 toggles an existing arm, not a gut.

**Goal.** PASS: a coherent architecture in which the real NK owns the live 68k/exception/scheduler
supervisor regime and SheepShaver provides the bare PPC core + device models beneath it — the NK runs from
its `rfi`@0x5031003c through its DEC reschedule loop (0x50313200) without contending with SS's
`execute_68k`/`ExcEnter`/scheduler. Falsifiable: the NK's EXT/SC/DEC/PROGRAM entries are the live handlers
(SS's `g_exc_entry_table` yields when gated ON), proven by a probe boot reaching the NK DEC loop with SS's
scheduler quiescent. DIAGNOSTIC: which SS pieces ended REPLACED vs CO-OWNED; Execute68k-pair disposition.

**S3-Task-0 (own, deep — decides the architecture, not whether there's a collision).** Q-S3.1 can SS's
`execute_68k` (`glue:1461/1504-1505`) be driven BY the NK's ECB/exception path, or is it dead under
newworld (NK reaches 68k via ROM EmulatorCode @0x50360000)? Q-S3.2 what happens to `exc_core.cpp`
ExcEnter/ExcRfi (which **SURVIVE** as the hardware vectoring mechanism — B3) and the scheduler when the NK
owns DEC — no-op, or CO-OWN/TIME-SHARE? Q-S3.3 enumerate candidate architectures: REPLACE / CO-OWN /
yield-fully / **"SS hosts the NK as a guest supervisor behind a thin shim"** (the 4th candidate); cite
`ppc-cpu.cpp:1986` as the profile-gating precedent. The `sc` syscall-delivery gap (Path A double-increment)
is a known sub-wall.

**Reuse (C3).** `exc_core.cpp` PEM-mask math (DEC/EXT/SC/PROGRAM), `deliver_pending_dec_exception()`, the
machine-layer device models, the SegMap/PMDT "write-just-before-consumer" spike pattern. Retire (don't
reuse): SS's synthetic handler/scheduler substitution + the entry-vector synthesis forge.

**Test ladder.** QEMU oracle ("NK reaches DEC reschedule 0x50313200?"); `nw-northstar` markers; the
already-gated arm makes the A/B clean.

**Gates.** G3.a probe boot — NK reaches 0x50313200 with SS scheduler structurally inert
(SS-scheduler-ticks=0 while NK live). G3.b NK exception entries are the live handlers (parked-PC assertion
in the NK exception cluster). G3.c Execute68k-pair disposition resolved (re-bound to 0x50360000 + SS
`execute_68k` dead under the gate, OR proven still-driven; the `:1504-1505` live read accounted for).
G3.d paravirtual byte-identical via **real `make e2e` + multi-run soak** (NOT the inertness substitute —
LAW/timing-sensitive edits); **revert-on-red = branch revert, not a flag flip** (the restructure is merged
regardless of the flag).

**Env-gate.** `SS_M18_NK_SUPERVISOR` (default OFF) AND `MachineProfileIsNewWorld()`. **The program's most
dangerous flip** — it transfers the supervisor role; paravirtual must be provably unreachable.

**File ownership (LAW, sole-in-flight).** `machine/exc_core.cpp` (retire SS handler substitution; KEEP
ExcEnter/ExcRfi), `sheepshaver_glue.cpp` (interrupt/`deliver_pending_dec_exception`/`execute_68k` seams +
Execute68k-pair re-bind), `machine/{event_sched,virt_clock}.cpp` (scheduler yield), `rom_patches.cpp`
(retire entry-vector synthesis). Depends on S1+S2 landed. Fix budget: a single ad-hoc fix to a PEM-mask LAW
line trips re-plan immediately (not after a second falsification).

---

## Stage 4 — disk System/Enabler IM-init in the loop (Q3)

**Effort band: months.** HARD-BLOCKED on the 9.2.x ISO asset. Inherits Q3 residues as its Task-0.

**Goal.** PASS: a genuine 9.2.x System/Enabler loads from disk, its CFM/PEF IM-init runs under the live NK
(S3) with translation live (S1) and **populates CGRP `*(KDP-0x338)` from guest code** (+0x04='CGRP',
+0x38/0x3c/0x40 arrays, +0x44=count) — replacing the M16 frozen-zero forge. Falsifiable: a watchpoint on
the CGRP fields fires from a **disk-RAM writer PC** (not a forge PC), CGRP non-zero and valid. DIAGNOSTIC:
the next wall.

**Donor (C1).** **EXTRACT-protocol** from DingusPPC `devices/common/viacuda.cpp` (SHA `92bb6d10…`) for the
EXPECTED NEXT WALL (Cuda IFR/IER) — reimplement in our style; Dingus+QEMU as behavioral oracles (Dingus has
PRAM, QEMU lacks). Do NOT wrap their core.

**S4-Task-0 (start precondition: S1 ∧ S2 ∧ S3 landed AND the 9.2.x medium boots into IM-init).** Q-S4.1
writer-PC trace — now observable because S1's paged MMU exposes the writer's virtual addr (it ran real-mode
at gating-Task-0 time, defeating virtual watchpoints). Q-S4.2 live DT-read-order. Q-S4.3 asset confirm.

**Gates.** G4.a CGRP non-zero + valid, written by a disk-RAM PC (watchpoint-confirmed). G4.b no SS-side
CGRP forge writes under the gate (forge-writes=0). **G4.c** boot advances past the CGRP gate to ANY
non-IM-init wall (the wall's identity is DIAGNOSTIC; "Cuda IFR/IER" is the expectation, not a fail
criterion). G4.d paravirtual byte-identical; `make test-jit`=100.

**Env-gate.** `SS_M18_IMINIT_DISK` (default OFF) AND `MachineProfileIsNewWorld()`.

**File ownership.** Edits (serialized): retire the CGRP forge in `rom_patches.cpp`/`sheepshaver_glue.cpp`
behind the gate; the disk/boot path that mounts the 9.2.x medium; an asset-manifest entry.

---

## Discriminator-A runbook (S1's decisive first experiment)

**Question:** does the NK map memory **coarse** (segment/BAT, ≥256 MB → Dolphin shadow-arena is
16 KB-host-page-safe, S1 is bounded) or **fine** (4 KB pages with mixed per-page perms → softmmu, fast path
lost, S1 balloons)? This decides Q-S1.2 before any S1 code.

**Caveat (devrel C-1):** the legacy recipe in `MMU-WITHOUT-GUTTING-FLATMEM.md:191` cites `SS_STUB_TRACE`,
which **does not exist in the tree** — do not chase it. Use the QEMU rig + the gdbstub RSP client, which
observes the *real* NK on the *real* mapping (the 9.0.4 stub-trace was tainted by ROM-patch construction,
per C3 — this runbook avoids that confound by observing the genuine NK boot).

**Procedure (≤4 QEMU boots, mirrors the gating-Task-0 Q3 method):**
1. Boot the rig halted: `bash SheepShaver/tools/qemu-rig.sh --gdbstub --timeout 90` (the `--gdbstub` adds
   `-s`; add `-S` to halt at reset). Drive it with **`SheepShaver/tools/gdbcli.py`** (the hand-written RSP
   client, now version-controlled — md5 `f468451…`; the `/tmp/newsheep/gdbcli.py` copy is ephemeral).
2. Breakpoint the NK MMU-install cluster (Q2 pinned, re-verify): `mtspr SDR1` @0x50310604 and the SR
   program loop @0x503104b4+. Read SDR1 (HTABORG|HTABMASK) and SR0–15 at that point.
3. **Decision rule:**
   - **Coarse** if the NK relies on **segment registers / BAT** for the mappings it actually uses (the
     per-context `mtsrin` loop @0x50315290 reprogramming SRs; SR-granular MMIO swap @0x50325894) and the
     hashed page table is sparse/absent for the regions it touches → take Dolphin shadow-arena (path a).
   - **Fine** if the NK populates and depends on dense 4 KB hashed-PTE entries with differing per-page
     perms (walk the HTAB at SDR1's HTABORG and inspect PTE granularity) → softmmu (path b); re-band S1
     and escalate.
4. Record as `[QEMU-BEHAVIORAL]` in the S1-Task-0 findings; **no QEMU MMIO address is a reference value**
   for our machine layer (the address-oracle caveat). Residue fallback: if the HTAB granularity can't be
   classified in ≤4 boots → "Discriminator-A UNKNOWN → treat as fine/months-risk" (conservative).

## Donor acquisition + citation ritual (devrel M-2)

Donors are NOT vendored — clone read-only for study; we reimplement/port to our style, never import wholesale.
**Full pinned SHAs + the exact donor functions are in `docs/planning/newsheep/DONOR-NOTES.md` (the
authoritative donor doc; this list is the index).**
- **Dolphin** (GPLv2, master `144d19433aa734c19c34e5978a1b817d2aa12663`): `github.com/dolphin-emu/dolphin`
  → arena remap `MemoryManager::UpdateDBATMappings()` in **`Source/Core/Core/HW/Memmap.cpp`** + the 16K/4K
  fork `CanCreateHostMappingForGuestPages()`; BAT-table rebuild `MMU::DBATUpdated()` in `MMU.cpp`. (NOT
  `JitArm64/Memmap.cpp` — the JIT is deliberately uninvolved.) PR #9441 (W^X on Apple Silicon).
- **QEMU** (GPLv2, `de5d8bfd6105d3dd3ae668df9762df244a6d1506`): `github.com/qemu/qemu` → `hw/intc/openpic.c`,
  `hw/misc/macio/*`. Region map in M3-DONOR-STUDY.
- **DingusPPC** (GPLv3 → our dist becomes GPLv3, master `b2660e29201730efc2179a43ec6a0a5fb22ad120`):
  `github.com/dingusdev/dingusppc` → `devices/common/viacuda.cpp` (IFR/IER crux `ViaCuda::update_irq()`;
  the `92bb6d10…` from M3-DONOR-STUDY is now STALE). **Never PR anything back upstream (their AI ban).**
- **Citation ritual** (per the backport-hygiene memory): at the porting site in our source, cite the donor
  repo + file + **full pinned SHA** in a comment, and mark prospective/untested code "needs validation" in
  both the comment AND the CHANGELOG entry. License propagation: any Dingus-derived code makes the
  distributed binary GPLv3 — note it in `docs/UPSTREAM-LINEAGE-SYNC.md`.

## Parallel workstreams (start at kickoff; off the S1 critical path)

Each names its deliverable artifact so two contributors don't collide:
1. **S2a** — `src/openfirmware_ci.{cpp,h}` + unit test (boot-disjoint, weeks). *Still available.*
2. **9.2.x ISO sourcing** — ✅ **DONE** (genuine 9.2.1 + 9.2.2 ISOs in hand; `ASSETS-AND-TOOLING.md` R2).
3. **Donor read-only studies** — ✅ **DONE** → `docs/planning/newsheep/DONOR-NOTES.md` (Dolphin
   `UpdateDBATMappings` + Dingus `viacuda` extracted; S1/S4 start warm).
- Build hygiene: ccache (~12× warm rebuild; see CLAUDE.md) + `make -j`.

## Acceptance discipline (per stage — uniform)

Run the milestone-machine acceptance (`MILESTONE-WORKFLOW.md` §2). **Command cwd (devrel M-4):**
`tools/gates.sh <tier>` runs from the **repo root**; `make build-ss`/`make test-jit`/
`make -C src/machine test`/`make e2e`/`make nw-northstar` run from **`SheepShaver/`**; slot boots via
`SheepShaver/tools/ss-slot-boot.sh --expect 'PAT;;…'`.
1. **Env-on first** — full battery with the stage's `SS_M18_*` gate(s) ON before any default flip; record
   each G-gate PASS with numbers.
2. **Flip is the LAST step**; any post-flip gate failure ⇒ revert in-task (for S3, the revert is a **branch
   revert, not a flag flip**).
3. **Fix budget** — telemetry freely; ONE small in-scope fix per falsified contract, full gates re-run; a
   SECOND falsification of the same contract ⇒ stop, re-scope, re-plan that stage (LAW-module edits:
   re-plan on FIRST ad-hoc fix).
4. **Paravirtual byte-identical is a gate** — for **S1 and S3** it is **real `make e2e` + multi-run soak
   (`SS_E2E_RUNS=N` median±CV%)** + (S1) a `make bench` ns/insn delta; the §6 structural-inertness
   substitute is allowed ONLY for S2a / S4 (genuinely new files).
5. **`nw-northstar`** observe line quoted at each stage close (report-only, never blocks).

## Stop-rule (triggers named in advance; one-iteration mechanics)

1. **"Re-forge CGRP instead of running IM-init."** STOP — per-wall forging is banked bankrupt (M17 series
   tripwire). CGRP must be built by real guest IM-init.
2. **"Fake the MMU with identity-only when translation is load-bearing."** Q2 pinned positive consumption;
   G1.a FAILS on identity-only by construction; readback-consistency is a false-clean. Identity-only ⇒
   default UNKNOWN→months, not "fine".
3. **"Claim a handoff boundary the recon proved fictional."** Q1 pinned: no yield point exists. The
   coherent model is SS-as-hardware / NK-as-OS (or a pinned CO-OWN/thin-shim), never a fictional handoff.
4. **"Start writing a stage's code mid-Task-0."** Each stage opens with its Task-0 + red-team; no
   `SheepShaver/src/**` for that stage until its Task-0 closes.
5. **Unbounded disasm** in any Task-0 → conservative residue (the function/instruction window is the
   kill-switch; two predecessor agents died here).
6. **QEMU-as-address-oracle** — no QEMU MMIO address as a reference value; derive live run addresses from
   the handoff; never break on a SheepShaver NK address under QEMU.
7. **"S2 PASS on the stub `/mmu`."** S2b "build-complete on stub" ≠ S2 PASS; S2 PASS requires the post-S1
   real-`/mmu` handoff acceptance (G2b.a/G2b.d non-stub). Shipping S2 on the stub is a false-clean.
8. **"The verdict is months" is the answer, not a trigger to re-scope Route A.** Record it; the next stage
   inherits the scope.

**One-iteration rule.** A pinned answer falsified within a stage → dated falsification entry → ONE bounded
re-pin (≤1 disasm window or ≤1 boot from the stage cap) → resume. A SECOND falsification of the same answer
⇒ STOP, re-plan that stage.

## Parallel asset dependency (Stage 4 HARD-BLOCK)

S4 requires a **genuine Mac OS 9.2.x install medium** (System + Enabler with the CFM/PEF IM-init that builds
CGRP). In-tree `macos921.dsk` is actually 8.6; e2e assets are `bench.dsk`/`apps.dsk`, not a 9.2.x installer.
Run the sourcing workstream from kickoff; record md5/SHA + provenance in `ASSETS-AND-TOOLING.md`. **Surface
the gap; do NOT forge CGRP to work around a missing asset** (Stop-rule #1). If the medium can't be obtained,
S4 is BLOCKED and program PASS is unreachable — a reportable program-level blocker, not a license to forge.

---

## Appendix A — Amendment history (audit trail; superseded by the body above)

> Provenance for the milestone-machine record. The body above is the current truth; these are the layers
> that produced it.

**Gating-Task-0 rev-2 (A1–A9)** — folded into the stages: A1 Q3 re-target (memory-watchpoint the CGRP
fields; builder may be disk-IM-init); A2 PASS = traced-or-inferred; A3 Q2 asymmetric evidence
(readback = false-clean); A4 Q1 CO-OWN third verdict + handoff-boundary precondition; A5 provenance =
coordinator-serialized; A6 pin mechanism, defer shape; A7 per-surface effort bands; A8 stop-rules #6/#7;
A9 minors. Full text: `…-gating-task0.md` "Rev-2 BINDING amendments".

**Program red-team (B1–B10), both reviewers GO-WITH-FIXES** — B1 S1 hot path = the JIT (80 RMEMBASE sites,
no chokepoint) → host-page aliasing not a per-access walker; S1 = UNKNOWN-months. B2 drop fictional
`reset_supervisor*` file. B3 ExcEnter/ExcRfi SURVIVE (retire SS handler substitution). B4 supervisor arm
already newworld-gated (`ppc-cpu.cpp:1986`) — S3 toggles, not guts; + thin-shim 4th candidate. B5 honest
sequential roll-up = quarters. B6 split S2a/S2b + stop-rule #7. B7 add G1.e. B8 S1/S3 need real e2e+soak,
not inertness; S3 revert = branch. B9 S2a-Task-0 has one real discovery (interrupt-map shape). B10 minors
(S4 start precondition; per-stage provenance gate-item-0; G4.c wording).

**Lateral-moves pass (C1–C4)** — C1 per-stage donor map (Dolphin port / PearPC-QEMU oracle / QEMU
reimplement-to-spec / DingusPPC extract-protocol). C2 differential-oracle test ladder. C3 Path A reuse map.
C4 three zero-dependency parallel workstreams + build hygiene.

**Technical-writer + developer-advocate reviews (rev-4 trigger)** — TW: NEEDS-RESTRUCTURE (three-layer
accretion; Stage-1 body misleading; orphaned stop-rule #7 / G1.e / S2a-split) → consolidate to single-state
(this rev-4). DevRel: ACTIONABLE-WITH-FIXES (Discriminator-A not runnable / `SS_STUB_TRACE` non-existent;
donor acquisition + citation underspecified; MMU-oracle undefined; gate cwd unpinned) → added the
Discriminator-A runbook, the donor-acquisition block, the MMU-oracle test-ladder definition, and per-command
cwd. Both folded into the body above.

## Appendix B — Self-review & red-team records (historical)

The rev-1 Self-review tensions (Stage-1 tractability, Stage-3 hybrid-unravel, S2 stub-parallelism,
binary-vs-graded estimate) were all UPHELD by the red-team and closed by B1/B4/B6/B5 respectively. The
two-reviewer program red-team and the TW/DevRel reviews are summarized in Appendix A; full reviewer text is
in the session transcript / commit history (`9bec87ff` draft → `22d81566` rev-2 → `dbc5f76f` rev-3 → this
rev-4).
