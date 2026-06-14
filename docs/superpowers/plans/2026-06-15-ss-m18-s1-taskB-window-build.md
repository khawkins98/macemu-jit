# SS_M18 Stage 1 — NewWorld paged MMU: S1 Task-B — the vm_remap "window" MMU build — IMPLEMENTATION milestone

> **Status:** rev-1 draft (2026-06-15), pre-red-team. This plan builds **Task B (the window)** — the
> host-side `vm_remap` shadow-remap of the NATMEM RAM/ROM reservations at `mtspr` BAT/SDR1/SR time —
> the part the parent S1-impl plan (`2026-06-14-ss-m18-s1-impl-paged-mmu.md` rev-2) **PARKED** pending
> a tlbie/HTAB-store interception design. **Task A is DONE/committed** (high_bat insurance + the
> mechanism-agnostic `paged_mmu.cpp` translation core + `test_paged_mmu.cpp`; commits
> `fc3ca256`/`2bf1526b`/`191fe67c`; `make test-jit` score=100; a real-mode/BAT bug was found+fixed by
> the Task-A adversary). This plan REUSES `paged_mmu_translate()` as the translation core — it does
> NOT re-derive translation.
>
> **★ Task B is now UN-PARKED — CONDITIONALLY — after a 2-stage adversary de-risk**
> (`docs/planning/newsheep/FINDINGS-s1-window-derisk.md` rev-2). Verdict = **WINDOW-SAFE-BY-MECHANISM,
> numeric coverage owed to G1.e**, on this binding chain:
> - The live PTE engine (HTAB-store + `tlbie`, 7 sites) is NOT intercepted by the window's `mtspr`
>   hook surface — BUT none of the 7 `tlbie` sites manipulates a BAT, and BAT writes are
>   `mtspr {I,D}BATxx`-EXCLUSIVE (hooked **wholesale by instruction**, regardless of call site). So the
>   "another path silently remaps a JIT page" gap is **structurally closed by mechanism**.
> - The residue is a numeric-coverage predicate owed to G1.e: the JIT does **data** accesses (RMEMBASE
>   loads/stores), so **DBAT** (not just IBAT) coverage of the full RAM aperture AND the ROM aperture
>   is the load-bearing predicate — a SECOND runtime unknown (IBAT/DBAT use INDEPENDENT descriptors,
>   strides IBAT `7,0xb,0xf,0x13` vs DBAT `0x17,0x1b,0x1f,3` @`0x50315370-0x503153c8`).
>
> **Binding consequence:** the window is built behind the gate but **PROVISIONALLY** live only; the
> affirmative window-adequacy verdict is **G1.e-only, TIGHTENED** to verify DBAT coverage of RAM+ROM +
> BAT-priority sanity. **softmmu / tlbie-HTAB-interception stays the re-band target** if any JIT EA is
> reachable only via the page table.
>
> **Scope law (carried, NOT relitigated):** Route A SETTLED; program shape SETTLED; Discriminator-A
> COARSE SETTLED; the Task-0 RESIDUE-PASS + the de-risk WINDOW-SAFE-BY-MECHANISM verdict are BINDING.

---

## ⚠ AUTHORIZATION GATE (read FIRST — this build is NOT auto-executable)

**This task edits the paravirtual-reachable NATMEM foundation** — the separate mach `vm_allocate`
reservations that back EVERY guest memory access, including the ~68 bare RMEMBASE JIT sites and the
live paravirtual build (which boots Mac OS 8.6, the working configuration). A defect here is not
contained to the gated newworld path until G1.c proves byte-identity; the blast radius is the whole
emulator's memory substrate.

**Therefore this plan MUST NOT be auto-executed.** It requires **explicit user authorization** before
the impl task runs. The Task-A authorization does NOT extend to Task B: Task A was structurally inert
(new files + a dead-code-insurance field); Task B writes live `vm_remap` calls into the shared NATMEM
layer and hooks them into the hot `mtspr` path. Authorization is a separate, explicit decision.

---

## Goal (PASS-vs-DIAGNOSTIC)

**PASS (the Task-B deliverable — falsifiable).** A gated host-side shadow "window" remaps the NATMEM
RAM/ROM sub-ranges at `mtspr` BAT/SDR1/SR (+`mtsr`/`mtsrin`) time so a bare RMEMBASE access lands on
the SR/BAT-selected PA — **proven non-regressing to the paravirtual build (byte-identical), proven
structurally unreachable when `!MachineProfileIsNewWorld()`, and proven coarse-adequate on a real
paged boot whose DBAT covers RAM+ROM**. Task B PASSES when all of:

1. **B1 lands GREEN at its mechanism** — the shadow-remap routine (`main_unix.cpp`) ports the Dolphin
   `UpdateDBATMappings` intersect-and-remap loop onto mach `vm_remap` over the SEPARATE NATMEM RAM/ROM
   reservations; the `mtspr` BAT/SDR1/SR + `mtsr`/`mtsrin` hooks call it; the ~68 RMEMBASE JIT sites
   stay **bit-identical**; `vm.hpp` interpreter parity is wired.
2. **All gates pass** — `G1.a-under-JIT`, `G1.b`, `G1.c` (REAL `make e2e` A/B byte-identical + soak +
   bench, NO inertness substitute), `G1.d` (`make test-jit`=100 + machine test), and **`G1.e`
   TIGHTENED** (probe NK boot reaches a translation landmark AND verifies DBAT coverage of RAM+ROM +
   BAT-priority sanity).
3. **The OWED items are closed as gates** — the external PearPC/QEMU PA-oracle cross-check of Task A's
   translation core, and the Vs/Vp-vs-`MSR[PR]` privilege gate (paged_mmu adversary Finding 3) — wired
   into G1.d/G1.a, not left as TODOs.

**PASS taxonomy (the floor):**
- **GREEN-PASS** = all gates clear AND G1.e affirmatively shows DBAT covers RAM+ROM with no
  page-table-only JIT EA → window is the live mechanism, hand to S2/S3.
- **RESIDUE-PASS** = G1.e shows any JIT EA is page-table-only (DBAT does NOT cover some JIT-touched
  RAM/ROM, or a PTE diverges from the BAT for a BAT-covered EA) → the window is **insufficient as
  built**; fall to **tlbie/HTAB interception OR softmmu** and **re-band** (Stop-rule #13). A finding,
  not a green light.

**DIAGNOSTIC (never a gate):** remap/feasibility-fail counts; the live DBAT/IBAT descriptor values at
G1.e; `vm_remap` `KERN_*` histograms. An unexpected live probe marker is ESCALATED.

## Authoritative inputs

| Doc / source | Role |
|---|---|
| `docs/superpowers/plans/2026-06-14-ss-m18-s1-impl-paged-mmu.md` (rev-2) — **the parent** | Task B section, gate set, file ownership, Stop-rule (incl. #13), AD-1/AD-2, the PROVISIONAL-window/softmmu-default discipline. Task A DONE. |
| `docs/planning/newsheep/FINDINGS-s1-window-derisk.md` (rev-2) — **the un-park verdict** | WINDOW-SAFE-BY-MECHANISM; 7 tlbie sites avoid BATs; BAT writes mtspr-exclusive; owed predicate = DBAT covers RAM+ROM → G1.e TIGHTENED; else interception/softmmu re-band. |
| `docs/planning/newsheep/DONOR-NOTES.md` — Donor 1 (Dolphin shadow-arena) | `UpdateDBATMappings()` (`HW/Memmap.cpp` ~L233–305) intersect-and-remap; `CanCreateHostMappingForGuestPages()` (~L343); the seam table. SHA `144d19433aa734c19c34e5978a1b817d2aa12663` (GPLv2). NOT `JitArm64/Memmap.cpp`. |
| Task A artifacts (`fc3ca256`/`2bf1526b`/`191fe67c`) | `machine/paged_mmu.cpp :: paged_mmu_translate()` — the translation core Task B REUSES (feasibility input + post-remap verifier). real-mode/BAT fix already folded. |
| `docs/MILESTONE-WORKFLOW.md` §2/§4/§6 | The machine; baseline-is-part-of-the-gate (G1.c); gate tiers; backport hygiene (SHA + "needs validation" in comment AND CHANGELOG). |
| Oracle (external, UNLINKED): PearPC `ppc_effective_to_physical()` PRIMARY; QEMU `mmu_common.c` SECONDARY | The PA-diff cross-check of `paged_mmu_translate()`, deferred-as-owed by Task A → now a Task-B gate. SOURCE+PIN repo+file+SHA. UNLINKED → no link propagation. NEVER port the walker (Stop-rule #6). |

## Codebase facts (carried; RE-VERIFY at impl before any edit; drift ±1–5)

- **NATMEM = SEPARATE fixed mach allocations.** RAM (`RAM_BASE 0x10000000`, `main_unix.cpp:1974`), ROM
  (`ROM_BASE 0x50000000`, `:2000`), MacIO `mmap(MAP_FIXED,PROT_NONE)` `0xF3000000` size `0x80000`
  (`:2079–2086`, gated `MachineUsesMMIOBus()`). Shadow remap = NEW mach `vm_remap` on the separate
  sub-ranges (c4 spike: `vm_remap` WORKS here; RW data, not MAP_JIT). Assert `RAMSize ≤ 0x40000000`.
  MEM_BULK NOT compiled (Stop-rule #9).
- **JIT memory path, no chokepoint.** RMEMBASE = `x19` (`ppc-jit.cpp:445`); ~68 bare sites; MMIO via
  Mach-fault on the PROT_NONE page. Window keeps these bit-identical; remap is at `mtspr`-time.
- **Hook surface.** `ppc-execute.cpp`: `mtspr SDR1:1607`, `mtspr BAT:1626`, `mtsr:1478`, `mtsrin:1485`.
  BAT writes mtspr-EXCLUSIVE → hooking the instructions catches every BAT reprogram. The 7 tlbie sites
  are NOT hooked (need not be — they touch no BAT).
- **Task A core is the only translation consumer.** `paged_mmu_translate()`; write-then-read-back is a
  false-clean (A3 LAW).
- **`MachineProfileIsNewWorld()`** = `machine/machine_profile.cpp:96–99`; boot-time mode-select only.
  Precedent `ppc-cpu.cpp:1986`.
- **`machine/` harness links NO `kpx_cpu`** → G1.a-under-JIT needs a NEW kpx_cpu-linked harness OR the
  real boot (G1.e). (Self-review tension 1/2.)
- **Provenance:** NK md5 `61c176e90b6365e84e5c660d703e56af`; ROM md5 `66210b4f71df8a580eb175f52b9d0f88`.

## Task breakdown (window-build; gated; mechanism FIXED to window by the de-risk; fallback = re-band)

### B1 — the shadow-remap routine in the NATMEM layer (`main_unix.cpp`)
- [ ] Re-verify the NATMEM sites + the `mtspr`/`mtsr`/`mtsrin` lines + that `paged_mmu_translate()`
      exports the feasibility/PA interface B1 needs (Task A's committed `paged_mmu.cpp`).
- [ ] New shadow-remap routine using mach `vm_remap` on the separate NATMEM RAM/ROM sub-ranges. Port
      the Dolphin `UpdateDBATMappings` intersect-and-remap loop (`max(start)/min(end)`). **Cite Dolphin
      SHA `144d1943…` + `HW/Memmap.cpp` at the porting site, "needs validation" in comment AND
      CHANGELOG.** Assert `RAMSize ≤ 0x40000000` or fall back to a separate reservation.
- [ ] Port the `CanCreateHostMappingForGuestPages`-equiv 16 KB-host feasibility check (Apple Silicon =
      LargePages). Feasibility-fail → DIAGNOSTIC count + slow path; a PERVASIVE fail on a JIT-covered
      region is a re-band trigger (Stop-rule #13).
- [ ] **Live-mapping interaction safety (tension 3 — load-bearing):** remap sub-ranges of the EXISTING
      live reservation WITHOUT tearing down the backing the ~68 RMEMBASE sites depend on. Mirror
      Dolphin's unmap-old/remap-new bookkeeping (`m_dbat_mapped_entries`) so a context switch is
      reversible and never leaves a hole. Verify each `vm_remap` `KERN_*`; recurrence ⇒ Stop (#14).
- [ ] Post-remap verify (debug/gated): a probe EA resolves via `paged_mmu_translate()` to the PA the
      host page now backs (the in-process half of G1.a).

### B2 — the hook + parity wiring
- [ ] Hook the remap call at `mtspr` BAT/SDR1/SR (`:1607/:1626`) + `mtsr`/`mtsrin` (`:1478/:1485`),
      env-gated `SS_M18_PAGED_MMU ∧ MachineProfileIsNewWorld()` — structurally unreachable in
      paravirtual. Keep the ~68 RMEMBASE sites BIT-IDENTICAL (hook at write-time, never access-time).
- [ ] Wire `vm.hpp` (`vm_do_get_real_address`) for interpreter-path PARITY.
- [ ] `sysdeps.h` mode switch behind the gate ONLY if needed (no per-access branch).

### Gates (falsifiable-in-advance)
- [ ] **G1.a-under-JIT** — a bare `LDR/STR [RMEMBASE, EA]` lands on the remapped PA UNDER THE JIT.
      Vehicle: a NEW kpx_cpu-linked harness OR the real boot (G1.e). Identity-only FAILS by
      construction (two-context `mtsrin`@0x50315290: same EA, two SR programmings → two host PAs).
- [ ] **G1.b** — MMIO-via-SR-swap microtest mirroring `mtsrin`/`mfsrin` around `0x50325894`.
- [ ] **G1.c** — REAL `make e2e` paravirtual A/B byte-identical (enumerated field set, jitter
      excluded) + `SS_E2E_RUNS=N` soak + `make bench`. GUI boots AUTHORIZED in ISOLATED config. NO
      inertness substitute (shared runtime-gated foundation → byte-identity MEASURED, not argued).
- [ ] **G1.d** — `make test-jit`=100 (authoritative) + machine test incl `test_paged_mmu`. **Wires the
      OWED items:** (i) external PearPC/QEMU PA-oracle cross-check of `paged_mmu_translate()`; (ii) the
      Vs/Vp-vs-`MSR[PR]` privilege gate row (paged_mmu adversary Finding 3).
- [ ] **G1.e (TIGHTENED — retires the de-risk residue)** — the probe NK boot under S1's paged path
      reaches a translation landmark (`mtsrin` context switch live) AND verifies on the live
      `(SR/BAT/SDR1, EA)` map: (1) **DBAT covers the FULL RAM aperture AND the ROM aperture**; (2)
      **BAT-priority sanity** (no installed PTE diverges from the BAT for a BAT-covered EA); (3) no
      JIT-touched EA is page-table-only. **Any of 1–3 fails → tlbie/HTAB interception OR softmmu →
      re-band** (Stop-rule #13).

> **Re-band trigger (binding):** if G1.e (or the harness) shows DBAT does NOT cover RAM+ROM, or a JIT
> EA is page-table-only → the pure-`mtspr` window is INSUFFICIENT → interception/softmmu → re-band,
> STOP-and-surface. Do not "make the window almost work".

## Env-gate
`SS_M18_PAGED_MMU ∧ MachineProfileIsNewWorld()`; remap hook structurally unreachable in paravirtual
(mirror `ppc-cpu.cpp:1986`); boot-time mode-select only; paravirtual V=P byte-identical PROVEN at G1.c.

## File ownership (serialized, LAW-adjacent — S1 owns the memory-translation surface)
- **Edits (serialized):** `Unix/main_unix.cpp` (vm_remap routine, B1); `ppc-execute.cpp` (remap-hook,
  B2); `Unix/sysdeps.h` (gate switch if needed); `kpx_cpu/src/cpu/vm.hpp` (interp parity). `ppc-jit.cpp`
  emit sites ONLY if the window is abandoned for a walker (it is not).
- **Reuses (no edit):** `machine/paged_mmu.cpp` + `test_paged_mmu.cpp` (extended additively at G1.d).
- **New (G1.a-under-JIT vehicle, if chosen):** a NEW kpx_cpu-linked harness target.
- **MUST NOT touch `sheepshaver_glue.cpp` (S2/S3).**

## Hot-path hazard (carried)
The `high_bat[16]` struct change is ALREADY landed by Task A. Task B adds NO struct fields. The hazard:
the `mtspr` hook must not perturb hot-path switch codegen in `ppc-execute.cpp`, and must not regress
the JIT-verify `memcmp(sizeof(powerpc_registers))` (Stop-rule #12 — never trim the compare). G1.c+G1.d
are the tripwires.

## Stop-rule (carries the parent; #1 satisfied-by-de-risk; #13 is the live fallback)
1. **(satisfied-by-de-risk)** the window is no longer an unmeasured bet — BUT the numeric predicate
   (DBAT covers RAM+ROM) is still owed to G1.e; do not treat the de-risk as retiring G1.e.
2. **Identity-only MMU when translation is load-bearing** — G1.a FAILS on identity-only (two-context);
   readback-consistency is a false-clean (A3 LAW).
4. **`0xDEADBEEF`/sentinel where a real PA/PTE is expected ⇒ STOP.**
6. **Port the oracle's per-access walker ⇒ STOP** — UNLINKED fixture generator only.
9. **Cite MEM_BULK / contiguous arena as the substrate ⇒ STOP** — it is mach `vm_remap` on separate
   reservations.
12. **Exclude `high_bat`/any field from the JIT-verify memcmp to "fix" a divergence ⇒ STOP.**
13. **(LIVE) Hook only `mtspr` and assume the page table is covered.** Mechanism-closure holds ONLY
    while DBAT covers RAM+ROM (G1.e #1) and no JIT EA is page-table-only (#3). If G1.e refutes either →
    tlbie/HTAB interception OR softmmu → re-band. Do NOT ship a window that "almost" covers.
14. **(window-specific) `vm_remap` `KERN_*` recurrence ⇒ STOP** — the live NATMEM interaction is
    unsafe; do not retry-loop over a failing remap on the foundation every access depends on.
15. **(window-specific) Identity-only remap as a "good enough" stand-in** — passes a naive readback,
    FAILS the two-context battery; a false-clean.

**One-iteration rule.** Falsified pin → dated entry → ONE bounded re-pin (≤1 disasm/harness window;
respect the boot budget) → resume. SECOND falsification of the same answer ⇒ STOP, re-plan.

## Self-review record — tensions FLAGGED FOR THE RED TEAM
1. **Can G1.a-under-JIT be done WITHOUT a full boot?** The in-process post-remap verify (B1) proves
   the host page backs the PA via `paged_mmu_translate()`, but not that a JIT-emitted `LDR/STR
   [RMEMBASE,EA]` lands there under the live dispatcher. Vehicles: a NEW kpx_cpu-linked harness OR
   G1.e. Decide whether the harness is worth building or G1.a-under-JIT folds into G1.e.
2. **Is the new kpx_cpu-linked harness worth it vs going straight to G1.e?** The harness gives an
   isolated, identity-only-falsifiable proof BEFORE risking a boot on the live foundation; but it is
   real engineering partially duplicating G1.e.
3. **Does `vm_remap` of a SUB-RANGE interact safely with the LIVE NATMEM mapping at runtime?** The c4
   spike proved `vm_remap` WORKS in isolation — not while the ~68 RMEMBASE sites concurrently
   bare-access the SAME reservation being re-aliased. Does our mach `vm_remap` preserve atomicity, or
   can a context switch transiently expose a hole / stale alias an in-flight access reads? The sharpest
   live-safety tension and the reason for Stop-rule #14 + the AUTHORIZATION GATE.

## Red-team record
*(empty — to be filled by the three-reviewer pre-implementation red-team: PROCESS + TECHNICAL + ADVERSARY. Must-answer voting list V1–V10 submitted alongside.)*
