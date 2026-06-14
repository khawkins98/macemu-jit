# SS_M18 Stage 1 — NewWorld paged MMU: S1-IMPL (the paged-MMU build) — IMPLEMENTATION milestone

> **Status:** rev-2 (2026-06-14) — three-reviewer red-team folded (PROCESS GO-WITH-FIXES · TECHNICAL
> GO-WITH-FIXES · ADVERSARY OPTIMISTIC-BUT-OK). Binding amendments in the **Red-team record** at the
> bottom; load-bearing ones merged inline. **Task A (A1+A2) is AUTHORIZED** (reshaped — see below);
> **Task B (the window) is PARKED** pending a tlbie/HTAB-store interception design (ADV finding). This
> plan operationalizes the S1 Task-0 recon (`FINDINGS-s1-paged-mmu.md` rev-2, RESIDUE-PASS).
>
> **★ rev-2 reshaping (the two decisive red-team findings):**
> - **AD-2 (high BATs) DEMOTED to dead-code insurance — NOT a binding driver.** The S1-impl ADVERSARY
>   re-derived the parcel: the 24 SPR 560–575 writes (`0x14138`) are **feature-gated** by a PVR-indexed
>   word (bit `0x20` @`0x14114`); the gate table sets `0x20` for **no PVR SheepShaver can present**
>   (7400 `0x000c`→`0x1f`, 745x→`0x1f`; SS PVR `0x000c0000`). The writes are **dead code on the target
>   machine.** → keep the `high_bat[16]` field + `case 560…575` arm as **cheap harmless insurance**, but
>   **REMOVE the high-BAT battery rows as a decision input** (they test code that never executes) and the
>   V7 PearPC-can't-model-high-BATs problem is **MOOT**. The recon-adversary's AD-2 re-band over-weighted
>   a dead premise (an M8→M17 *false-positive*); the RESIDUE-PASS survives on AD-1 alone (strengthened).
> - **AD-1 (live PTE engine) CONFIRMED + deepened into the REAL window risk.** The engine changes
>   mappings via **plain HTAB stores + `tlbie`, NOT `mtspr`** (`0x50319af8`/`0x9b28`). The window's
>   `mtspr`/`mtsr`/`mtsrin` hook surface **does not intercept it** → any JIT-covered page remapped via the
>   PTE engine **silently desyncs** the shadow window. → **Task B PARKED:** the window needs a
>   tlbie/HTAB-store interception design OR a G1.e proof that JIT-covered RAM/ROM is BAT-only and
>   PTE-engine-untouched, BEFORE the window is locked. The window can only ever be **PROVISIONALLY**
>   authorized pre-boot; softmmu stays FULLY live through **G1.e** (not Gate A — Gate A is structurally
>   blind to live mappings).
>
> **Binding inheritance from the re-band:** the **window is PREFERRED but the walker/softmmu is the
> DEFAULT** until G1.e proves coarse adequacy on a real boot. **S1-impl does NOT bet on the window.**
> Task A (reshaped) builds the mechanism-agnostic translation core + dead-code insurance; the
> window-vs-softmmu decision is owed to G1.e.
>
> **Scope law (carried, NOT relitigated):** Route A SETTLED; program shape (S1→S3→S4) SETTLED;
> Discriminator-A COARSE→path a SETTLED; the Task-0 verdict (RESIDUE-PASS) is BINDING. This plan does
> not re-open any of them.

## Goal (PASS-vs-DIAGNOSTIC)

**PASS (the S1-impl deliverable — falsifiable).** A NewWorld paged-MMU translation path exists,
correct against an external oracle and proven non-regressing to the paravirtual build, **AND** the
window-vs-softmmu mechanism question is *decided by measurement, not assumption*. S1-impl PASSES when
all of:

1. **Task A lands GREEN** — the high-BAT state model + `mtspr` capture (SPR 560–575) is in place with
   a CLEAN PPC recompile, `make test-jit`=100 on the authoritative legacy run, and the G1.a-pure
   oracle test (`our_translate(SR[16],BAT[16],HIGH_BAT[16],SDR1,EA)→PA`) passes the full battery
   (two-context / COARSE-BAT / adversarial-FINE + high-BAT), with **the FINE/high-BAT host-page
   coverage result RECORDED** (the window-vs-softmmu decision gate).
2. **The decision gate is resolved in writing (rev-2 — PROVISIONAL only).** Gate A's synthetic battery
   can establish **translator-correctness**, but is **structurally blind to live mappings** (it cannot
   observe whether the live NK maps JIT-covered RAM/ROM via BAT (window-OK) or via the live PTE engine
   (window-hostile)). So Gate A can only **PROVISIONALLY** authorize building the window behind the
   gate; the **affirmative** window-adequacy verdict is **G1.e-only** (a real paged boot proving the PTE
   engine never targets JIT-covered PAs). If G1.e shows PTE-engine dependence on JIT-covered regions →
   **softmmu**, STOP-and-surface. **softmmu stays FULLY live until G1.e closes coverage.**
3. **Task B lands GREEN at its mechanism** — the gated path (`SS_M18_PAGED_MMU ∧
   MachineProfileIsNewWorld()`) passes G1.a-under-JIT, G1.b, G1.c (real `make e2e` paravirtual A/B +
   soak + bench, paravirtual byte-identical), G1.d, and the G1.e probe NK boot reaches a
   translation-dependent landmark.

**PASS taxonomy (the floor, inherited from Task-0 RT-C1):**
- **GREEN-PASS** = Task A measurement earns the window AND Task B (window) clears all gates → S1
  complete, hand to S2/S3.
- **RESIDUE-PASS** = Task A measurement refutes the window (FINE/high-BAT dependence on JIT-covered
  regions) → Task B is **softmmu**, the program band changes, this is a **finding not a green light**:
  STOP, coordinator re-bands before any softmmu code is written. "We built the oracle test and a
  struct field" is NOT a license to ship a window.

**The window-vs-softmmu DECISION GATE (the spine of this plan).** Gate A is the binding fork. It is
falsifiable-in-advance: the adversarial-FINE + high-BAT battery row (3) plus the
`CanCreateHostMappingForGuestPages`-equivalent feasibility result MUST produce a recorded
coverage verdict BEFORE any `vm_remap` line is written. Task B's mechanism is **selected by Gate A's
output**, never assumed.

**DIAGNOSTIC (recorded, NEVER a gate):** remap/feasibility-fail counts; which Dolphin seam needed the
most adaptation; the high-BAT program's table shape. An unexpected live probe marker is ESCALATED, not
filed-and-ignored.

## Authoritative inputs

| Doc / source | What it fixes for S1-impl |
|---|---|
| `docs/planning/newsheep/FINDINGS-s1-paged-mmu.md` (rev-2) — **THE input** | The RESIDUE-PASS verdict + AD-1 (live 4 KB PTE engine) + AD-2 (high BATs SPR 560–575 dropped) + the MMU-oracle SPEC + I/O contract + the owed-falsifiers table. Window PREFERRED, walker DEFAULT until measured. |
| `docs/superpowers/plans/2026-06-14-ss-m18-s1-paged-mmu-task0.md` (rev-2) | The S1 charter operationalized: gates G1.a–e (falsifiable-in-advance), file ownership, env-gate, Stop-rule, zero-boot recon law (boots owed at G1.e/G1.c, AUTHORIZED now in isolated config). |
| `docs/planning/newsheep/DONOR-NOTES.md` — Donor 1 (Dolphin Dynamic-BAT shadow-arena) | The port map: arena remap `MemoryManager::UpdateDBATMappings()` `HW/Memmap.cpp` ~L233; BAT rebuild `MMU::DBATUpdated()` `MMU.cpp` ~L1535; 16K/4K fork `CanCreateHostMappingForGuestPages()` ~L343. Dolphin SHA `144d19433aa734c19c34e5978a1b817d2aa12663` (GPLv2). NOT `JitArm64/Memmap.cpp`. The remap is "new code in the NATMEM/`vm_alloc` layer (`main_unix.cpp`)". |
| `docs/superpowers/plans/2026-06-14-ss-m18-s1-paged-mmu-task0.md` (house-template EXEMPLAR) | The template for this plan (Goal/inputs/codebase-facts/tasks/gates/struct-hazard/env-gate/ownership/stop-rule/self-review/red-team). |
| `docs/MILESTONE-WORKFLOW.md` §2 / §4 / §6 | The machine; baseline-is-part-of-the-gate; gate tiers; dispatch economics; unbounded-disasm kill-switch. |
| Oracle (external, UNLINKED): PearPC `ppc_effective_to_physical()` (`ppc_mmu.cc`) PRIMARY; QEMU `target/ppc/mmu_common.c` SECONDARY | The PA-diff reference fixture generator. S1-impl SOURCES and PINS repo+file+full SHA in the fixture-gen comment. GPLv2, used UNLINKED → no link propagation. NEVER port the per-access walker (Stop-rule #6). |

## Codebase facts (carried from Task-0 — RE-VERIFY at impl before any edit)

> All file:line drift ±1–5; the fields/functions are correct. Re-verify on the edit-day. Several were
> independently re-checked while drafting this plan (noted ✔).

- **The struct-offset truth (✔ re-verified this session — resolves the AD-2 "where does high_bat go"
  conflict).** `ppc-registers.hpp:256-257` comment reads verbatim: *"MUST stay LAST: the JIT hardcodes
  byte offsets for gpr/lr/ctr/xer/reserve (e.g. PPCR_RESERVE_VALID=1060). **Appending here shifts
  nothing the JIT references.**"* The protected fields (`gpr/lr/ctr/xer/reserve`) sit ABOVE this
  comment. The supervisor fields `sprg[4]`, `sdr1`, `bat[16]`, `srr0`, `srr1`, `sr[16]`, `msr` are
  themselves the *appended-after* block and each carries "its exact offset is irrelevant to codegen"
  notes. → **The findings' phrasing "BEFORE this block" is misleading; the project memory "append LAST"
  is correct. The safe, unambiguous home for `high_bat[16]` is the very END, after `msr`.** A CLEAN PPC
  recompile is still MANDATORY (stale `.o` → `test-jit=0`), because any struct size change must
  propagate to every TU that includes the header.
- **The dropped high-BAT writes (✔ re-verified).** `ppc-execute.cpp` `execute_mtspr` switch: captures
  `SPR_SDR1`, `SPR_SRR0/1`, `SPRG0..3`, `SPR_IBAT0U ... SPR_DBAT3L` (`regs().bat[...] = s`), DEC(22),
  TBL(284)/TBU(285), and a **`default:` case that drops all other SPR writes** (`ss_stub_mtspr[...]++`
  stub). SPR 560–575 (`mtspr 0x230..0x23f`) fall into that default → silently discarded today. Task A
  adds a `case 560 ... 575:` arm storing into `high_bat[16]`.
- **The production memory path is the JIT, no translation chokepoint.** RMEMBASE = ARM64 `x19`
  (`ppc-jit.cpp:445`), loaded once via `emit_load_imm64(RMEMBASE, JIT_MEM_BASE)`. ~68 bare access
  sites + 1 address-form add; NO per-access MMIO/bounds guard (MMIO via Mach-fault on the PROT_NONE
  MacIO page). The window keeps these bit-identical; the remap is paid at `mtspr`-time. The JIT emit
  sites are touched ONLY if Gate A flips Task B to walker.
- **NATMEM = SEPARATE fixed mach allocations, NOT a MEM_BULK arena.** `config-macosx-aarch64.h:409`
  defines only `NATMEM_OFFSET 0x400000000000`; `sysdeps.h:96 #define DIRECT_ADDRESSING 1`. RAM
  (`RAM_BASE 0x10000000`, `main_unix.cpp:1974`) and ROM (`ROM_BASE 0x50000000`, `:2000`) are separate
  `vm_acquire_fixed`→mach `vm_allocate` calls. MacIO MMIO window = separate `mmap(MAP_FIXED, PROT_NONE)`
  at `NATMEM_OFFSET+0xF3000000`, size `0x80000` (`:2079–2086`), gated `MachineUsesMMIOBus()`. Shadow
  remap = NEW code using mach `vm_remap`/`mmap MAP_FIXED` on the separate sub-ranges (c4 spike verified
  `vm_remap` works on this machine). MEM_BULK is NOT compiled here (Stop-rule #9). Disjointness holds
  for all `RAMSize ≤ 0x40000000`; the remap routine MUST assert this (the bounded (2b) guard).
- **Supervisor state read by NAME, no struct change needed for a *consumer*.** Write handlers are
  store-only (A3 false-clean LAW): `execute_mtsr:1478`, `execute_mtsrin:1485`, `mtspr SDR1`,
  `mtspr BAT`. No existing JIT/interp path reads `sdr1`/`bat`/`sr`/`high_bat` for translation — the
  real consumer is the NEW `paged_mmu.cpp` translation function. A write-then-read-back test is a
  false-clean for any MMU verdict.
- **`MachineProfileIsNewWorld()`** = `machine/machine_profile.cpp:96–99`; O(1) read of `g_profile`
  (default `MACHINE_PARAVIRTUAL`). Boot-time mode-select ONLY; MUST NOT become a per-access JIT branch.
  Precedent for a boot-time-selected paged arm: `check_spcflags` newworld arm `ppc-cpu.cpp:1986`.
- **The `machine/` harness links NO `kpx_cpu`/`ppc-jit`/`ppc-execute`** (RT-C2). The two existing "JIT"
  tests hand-assemble a single access word and Mach-fault on it. → G1.a-pure is standalone (no boot, no
  kpx_cpu link). G1.a-under-JIT needs a NEW `kpx_cpu`-linked harness OR the real paged boot (G1.e).
- **Parcel provenance (gate-item-0):** NK-v02.27 md5 `61c176e90b6365e84e5c660d703e56af`; canonical ROM
  md5 `66210b4f71df8a580eb175f52b9d0f88`. Re-verify before trusting any disasm-derived constant.

## Task breakdown (gated; A is the gating-first-step, lowest-risk-first; B is mechanism-selected by Gate A)

### Task A — the gating-first-step: high-BAT state model + the G1.a-pure oracle measurement (earns or refutes the window)

> Lowest risk first. Task A writes NO host-mapping code. It makes the translator *expressible* (AD-2)
> and then MEASURES the FINE/high-BAT coverage that decides Task B's mechanism. It does NOT bet on the
> window.

**A1 — extend the supervisor register model for high BATs (SPR 560–575) + capture the dropped writes.**
- [ ] Re-verify (edit-day) `ppc-registers.hpp:256` "MUST stay LAST" comment and the `msr` field tail;
      re-verify the `ppc-execute.cpp` `execute_mtspr` switch + its `default:` drop arm.
- [ ] Append `uint32 high_bat[16];` to the supervisor block **at the very END, after `msr`** (the
      JIT-safe home per the struct fact above; document why with a field comment mirroring the existing
      "exact offset irrelevant to codegen" notes). **Do NOT insert it before `msr`/mid-block** unless
      the red team overrides — the safe answer is the tail.
- [ ] Add `case 560 ... 575:` (`SPR 0x230..0x23f`) to `execute_mtspr`, storing
      `regs().high_bat[spr - 560] = s;` (mirror the `bat[16]` arm). Add the symmetric `mfspr` read-back
      arm if the NK reads them back (re-verify against the parcel; AD-2 says writes are 24×, table-driven).
- [ ] CLEAN PPC recompile (mandatory): full rebuild of every TU including `ppc-registers.hpp`; do NOT
      reuse stale `.o`. Confirm no offset assertion / `test-jit` collapse.

**A2 — the G1.a-pure oracle test (the decision measurement).**
- [ ] SOURCE + PIN the external oracle: PearPC `ppc_effective_to_physical()` repo+file+full SHA in the
      fixture-generation comment (PRIMARY); QEMU `mmu_common.c` as SECONDARY cross-check. UNLINKED
      out-of-process fixture generator only — NEVER port the walker into our tree (Stop-rule #6).
- [ ] Implement `SheepShaver/src/machine/paged_mmu.cpp` :: `our_translate(SR[16], BAT[16],
      HIGH_BAT[16], SDR1, EA) → {PA, fault/resolve}` (segment + context-table + BAT + high-BAT; the
      adversarial-FINE rows exercise the 4 KB hashed-PTE path AD-1 proved live).
- [ ] Implement `SheepShaver/src/machine/test_paged_mmu.cpp` diffing `our_translate(row)` vs the
      oracle PA AND the same fault-vs-resolve outcome, for the full battery:
      1. **two-context** (`mtsrin`@0x50315290 shape: same EA, two SR programmings → two distinct PAs;
         identity-only FAILS by construction, Stop-rule #2);
      2. **COARSE-BAT** (256 MB-class block, the expected fast-path-safe case);
      3. **adversarial-FINE + high-BAT** (dense 4 KB hashed-PTEs with mixed per-page perms over a
         JIT-covered region, PLUS high-BAT-mapped ranges via the new `HIGH_BAT[16]` cells).
- [ ] Run the `CanCreateHostMappingForGuestPages`-equivalent feasibility check over battery (3) and
      **RECORD the coverage result** (how many JIT-covered-region rows are window-feasible vs fall to
      slow path). This recorded number IS the window-vs-softmmu decision input.

**Gate A (falsifiable-in-advance; the decision gate):**
- [ ] `make test-jit` = **100** on the **authoritative legacy run** (not just the batch harness) — the
      struct change must NOT regress. (If <100 → stale `.o`/offset-break suspected → STOP, clean rebuild,
      re-run; never paper over.)
- [ ] `make -C SheepShaver/src/machine test` GREEN including the new `test_paged_mmu` (battery 1–3 all
      equal-PA + equal-fault).
- [ ] **The FINE/high-BAT coverage result is RECORDED in the findings addendum**, and the decision is
      written: **(a) window-adequate → authorize Task B=window**, or **(b) JIT-covered FINE/high-BAT
      dependence → Task B=softmmu, re-band, STOP-and-surface to the coordinator.**

> **Gate A honesty note:** A2 retires the FINE-falsifier only as a *translator-correctness* property
> across coarse AND fine inputs (a conditional-close). It does NOT prove the live NK only uses coarse
> mappings on JIT-covered regions — that is G1.e on the real paged boot. If the battery cannot be built
> faithfully because the rig cannot reach the live NK mapping data (see Self-review tension 3), record
> that as a LIMIT on what Gate A can decide, do not fabricate coverage.

### Task B — the window (ONLY if Gate A earns it): the `vm_remap` shadow-remap hook

> Mechanism is SELECTED by Gate A. If Gate A=(b), Task B is softmmu and this section is replaced after
> the coordinator re-band. Do NOT write any `vm_remap` line before Gate A is recorded (Stop-rule #1).

**B1 — the shadow-remap routine in the NATMEM layer (`main_unix.cpp`).**
- [ ] New shadow-remap routine using mach `vm_remap`/`mmap MAP_FIXED` on the separate NATMEM RAM/ROM
      sub-ranges (port the Dolphin `UpdateDBATMappings` intersect-and-remap loop; cite Dolphin SHA
      `144d1943…` + file at the porting site, "needs validation" in comment AND CHANGELOG). Assert
      `RAMSize ≤ 0x40000000` (the (2b) disjointness guard) or fall back to a separate reservation.
- [ ] Hook the remap call at the `mtspr` BAT/SDR1/SR handlers (`ppc-execute.cpp`) + `mtsr`/`mtsrin`,
      env-gated `SS_M18_PAGED_MMU ∧ MachineProfileIsNewWorld()` — structurally unreachable in
      paravirtual. Keep the ~68 RMEMBASE JIT sites BIT-IDENTICAL.
- [ ] Wire `vm.hpp` (`vm_do_get_real_address`) for interpreter-path PARITY with the JIT remap.

**Gates G1.a-under-JIT / G1.b / G1.c / G1.d / G1.e (falsifiable-in-advance):**
- [ ] **G1.a-under-JIT** — bare `LDR/STR [RMEMBASE, EA]` lands on the remapped PA under the JIT.
      Vehicle: a NEW `kpx_cpu`-linked harness (W^X cache + dispatcher + `ppc-execute`) OR the real
      paged boot (G1.e). Identity-only FAILS by construction.
- [ ] **G1.b** — MMIO-via-SR-swap microtest mirroring `mtsrin`/`mfsrin` around the `0x50325894` shape.
- [ ] **G1.c** — real `make e2e` paravirtual A/B **byte-identical** (enumerated `make e2e` field set,
      jitter counters excluded) + `SS_E2E_RUNS=N` median±CV% soak + `make bench` ns/insn delta. GUI
      boots AUTHORIZED now in an ISOLATED config. The structural-inertness substitute is NOT allowed
      (S1 edits a shared runtime-gated path).
- [ ] **G1.d** — `make test-jit`=100 + `make -C SheepShaver/src/machine test` incl `test_paged_mmu`
      ("build-ready" confirmed here).
- [ ] **G1.e** — probe NK boot under translation reaches a translation-dependent landmark (the
      `mtsrin` context switch executing correctly live) on the REAL SheepShaver newworld boot under
      S1's own paged path. Carries AD-1's live FINE residual to its live retirement.

> **Task B re-band trigger (binding):** if at any point Task A or G1.e shows FINE/high-BAT dependence on
> JIT-covered regions, Task B IS softmmu → re-band, STOP-and-surface. Do not "make the window almost work".

## Struct-offset hazard (NAMED RISK — clean-rebuild requirement)

**Risk:** `ppc-registers.hpp` carries a "MUST stay LAST" block; the JIT hardcodes byte offsets for
`gpr/lr/ctr/xer/reserve` (e.g. `PPCR_RESERVE_VALID=1060`). A struct field inserted in the wrong place,
OR ANY field added without a CLEAN recompile, shifts offsets the JIT references → silent corruption →
`make test-jit`=0.

**Mitigation (binding):**
1. `high_bat[16]` goes at the very **END, after `msr`** — the appended-after region the comment
   declares JIT-irrelevant ("Appending here shifts nothing the JIT references"). NOT mid-block, NOT
   before `msr`, unless the red team explicitly overrides (Self-review tension 1).
2. **CLEAN PPC recompile is MANDATORY** — full rebuild of every TU including the header; no stale `.o`.
3. **Gate A's `make test-jit`=100 on the authoritative legacy run is the tripwire** — a regression here
   is the struct hazard manifesting; STOP and rebuild clean, never paper over.

## Env-gate

`SS_M18_PAGED_MMU ∧ MachineProfileIsNewWorld()`. The remap hook is structurally unreachable when
`!MachineProfileIsNewWorld()` (mirrors the `ppc-cpu.cpp:1986` newworld arm). Boot-time mode-select
ONLY; the gate read must NOT become a per-access JIT branch. Paravirtual stays V=P byte-identical
(proven at G1.c).

## File ownership (serialized, strong tier — S1 owns the memory-translation surface)

- **New:** `SheepShaver/src/machine/paged_mmu.cpp`, `SheepShaver/src/machine/test_paged_mmu.cpp`
  (Makefile target pre-wired by the coordinator at impl-kickoff; the G1.a-under-JIT vehicle may need a
  NEW `kpx_cpu`-linked harness target, RT-C2).
- **Edits (serialized):**
  - `kpx_cpu/src/cpu/ppc/ppc-registers.hpp` (Task A: append `high_bat[16]` at tail) — **clean rebuild**.
  - `kpx_cpu/src/cpu/ppc/ppc-execute.cpp` (Task A: capture SPR 560–575; Task B: remap-hook call at the
    `mtspr` BAT/SDR1/SR + `mtsr`/`mtsrin` handlers).
  - `Unix/main_unix.cpp` (Task B: the `vm_remap` shadow-remap routine).
  - `Unix/sysdeps.h` (Task B: mode switch behind the gate, if needed).
  - `kpx_cpu/src/cpu/vm.hpp` (Task B: interpreter chokepoint parity).
  - `ppc-jit.cpp` emit sites are touched ONLY if Gate A flips Task B to walker.
- **S1 must NOT touch `sheepshaver_glue.cpp` (owned by S2/S3).** No other stage edits the
  memory-translation surface concurrently.

## Stop-rule (triggers named in advance; one-iteration mechanics)

1. **"Bet on the window before measuring."** No `vm_remap`/shadow-remap line is written before Gate A
   records the FINE/high-BAT coverage. Window is PREFERRED, softmmu is DEFAULT until measured.
2. **#2 — identity-only MMU when translation is load-bearing.** G1.a-pure FAILS on identity-only by
   construction (two-context battery); readback-consistency is a false-clean (A3 LAW).
3. **Struct insert without a clean rebuild** (OR inserting `high_bat[16]` anywhere but the tail without
   red-team override). `make test-jit` < 100 after the struct change ⇒ STOP, clean rebuild, re-run;
   never paper over.
4. **`0xDEADBEEF` (or any sentinel) appearing where a real PA/PTE is expected ⇒ STOP** — it means a
   dropped/sentinel write reached the translator; do not "tolerate" it.
5. **#5 — unbounded disasm.** Any static thread past its function/instruction window ⇒ STOP, record the
   conservative residue. Do NOT disassemble the whole 105280 B parcel.
6. **#6 — port the oracle's per-access walker into our tree.** The oracle is an UNLINKED PA-fixture
   generator, never a runtime translator.
7. **#7 — proceed to a window on a RESIDUE-PASS.** Gate A=(b) (JIT-covered FINE/high-BAT dependence)
   BLOCKS the window; coordinator re-bands to softmmu first.
8. **#8 (rev-2 amended) — lock window while live coverage is UNKNOWN.** The walker/softmmu stays the
   live DEFAULT until **G1.e** (NOT Gate A — Gate A is structurally blind to live mappings) closes the
   coarse-adequacy coverage affirmatively on a real boot.
11. **(rev-2) — declare window-adequate from the synthetic battery alone.** Synthetic coverage ≠ live
    coverage; affirmative window retirement is G1.e-only.
12. **(rev-2) — exclude `high_bat` from the JIT-verify memcmp to "fix" a divergence.** A JIT-verify
    state-diff over the grown struct ⇒ STOP and find why (SPR 560–575 must route to the interp fallback
    in the JIT so both verified paths store identically); never paper over by trimming the compare.
13. **(rev-2) — hook only `mtspr` for the window and assume the PTE engine is covered.** The live PTE
    engine remaps via HTAB stores + `tlbie`, bypassing `mtspr`. Task B MUST either intercept
    tlbie/HTAB-stores OR carry a G1.e proof that JIT-covered RAM/ROM is BAT-only + PTE-engine-untouched.
9. **#9 — cite MEM_BULK / a single contiguous arena as the remap substrate.** It is not compiled here;
   the substrate is mach `vm_remap` on the separate NATMEM reservations.
10. **"Run stalled QEMU boots to see the live NK MMU install."** The rig stalls pre-install; the live
    residual is owed to G1.e on S1's own paged boot. (Boots ARE authorized now for G1.c in an isolated
    config — that is different from chasing the stalled NK-install rig.)

**One-iteration rule.** A pinned answer falsified within S1-impl → dated falsification entry → ONE
bounded re-pin (≤1 disasm window; respect the boot budget) → resume. A SECOND falsification of the same
answer ⇒ STOP, re-plan.

## Self-review record — tensions FLAGGED FOR THE RED TEAM

> These are deliberately left open for the red team to settle; they are the load-bearing uncertainties.

1. **Where EXACTLY does `high_bat[16]` go without breaking JIT offsets?** This plan resolves the
   findings-vs-memory conflict to **"the very END, after `msr`"**, on the strength of the verbatim
   comment ("Appending here shifts nothing the JIT references") and the existing supervisor fields all
   carrying "offset irrelevant to codegen" notes. **The findings said "BEFORE this block" — I believe
   that phrasing is wrong/ambiguous; the memory ("append LAST") wins.** Red team MUST confirm the tail
   is JIT-safe and that no TU computes `sizeof(powerpc_registers)` or a trailing offset that the new
   field would shift (e.g. any serialization/snapshot code, any assertion on struct size). If such a
   consumer exists, the tail is NOT safe and the field needs a different home + a recompile-wide audit.
2. **Is capturing previously-dropped SPR writes (SPR 560–575) paravirtual byte-identical?** Today those
   writes hit the `default:` drop arm (`ss_stub_mtspr[...]++`). Storing them into `high_bat[16]` changes
   only a counter-vs-store, with NO consumer in the paravirtual path (the consumer is gated
   `MachineProfileIsNewWorld()`). I assert this is paravirtual byte-identical, but the red team must
   confirm the stub counter is not itself observed by any e2e/identity assertion (RT-M3 jitter-exclusion
   territory) and that adding the `case 560 ... 575:` arm cannot perturb switch codegen on the hot path.
3. **Can the FINE/high-BAT battery actually be built without the live NK mapping data the rig can't
   reach?** AD-1 proves a live 4 KB PTE engine exists; the *actual* mappings are table-driven and
   UNMEASURED-LIVE (the QEMU rig stalls pre-NK-MMU-install; Discriminator-A Evidence B). The
   adversarial-FINE rows (battery 3) are therefore SYNTHETIC adversarial inputs, not captures of the
   real NK working set. **This means Gate A can decide translator-correctness across coarse+fine inputs,
   but it CANNOT by itself prove the live NK's JIT-covered regions are window-feasible** — that remains
   owed to G1.e. The red team must decide: is a synthetic-FINE Gate A a sufficient basis to AUTHORIZE
   Task B=window (with G1.e as the live backstop), or does the inability to reach live mapping data mean
   the window can only ever be *provisionally* authorized and softmmu must be kept fully live through
   G1.e? (This is the sharpest tension — it determines whether "earn the window" is achievable
   pre-boot at all.)

## Red-team record

Three reviewers, parallel. **PROCESS GO-WITH-FIXES · TECHNICAL GO-WITH-FIXES · ADVERSARY OPTIMISTIC-BUT-OK.** No NO-GO. Task A (reshaped) AUTHORIZED; Task B PARKED. Binding amendments folded:

**TECHNICAL (independent re-verify):**
- **V1 GO (decisive — confirmed):** `high_bat[16]` at the tail (after `msr`) is provably JIT-safe. The JIT hardcodes only `PPCR_SPCFLAGS 1056`/`RESERVE_VALID 1060`/`RESERVE_ADDR 1064` (`ppc-jit.cpp:425/428/429`), all guarded by `static_assert(offsetof(...)==...)` at `ppc-cpu.cpp:861/866/868`, all ABOVE the supervisor tail. The findings' "BEFORE this block" was wrong; project memory ("append LAST") wins. **M1 folded:** cite these existing static_asserts as the PRIMARY compile-time tripwire (demote `test-jit=100` to the behavioral check).
- **V2:** add `static_assert(offsetof(powerpc_registers, high_bat) == offsetof(..., msr)+4, "high_bat must stay last")` (cheap, in-pattern). Folded.
- **V3 GO:** SPR 560–575 fall through `default:` (`ppc-execute.cpp:1651`) today; adding `case 560…575:` mirrors the `bat[]` arm; tree-wide grep `high_bat` = 0 readers → paravirtual byte-identical. **M2 folded:** JIT-verify does `memcmp(sizeof(powerpc_registers))` (`ppc-cpu.cpp:2359…2913`) — now covers `high_bat`; a Gate A item must confirm SPR 560–575 routes to the interp fallback in the JIT so both verified paths store identically (Stop-rule #12).
- **V7 → MOOT (see ADV AD-2):** PearPC is a 4-BAT model and can't represent high BATs — but the high-BAT rows are dropped (dead code), so the oracle gap no longer bites. Path-drift minors fixed: JIT is `cpu/jit/aarch64/ppc-jit.cpp`; config is `MacOSX/config/config-macosx-aarch64.h`.

**PROCESS:**
- **C1 (CRITICAL, folded):** Gate A over-claimed a pre-boot affirmative window. Goal-2 now PROVISIONAL; affirmative adequacy is G1.e-only; Stop-rule #8 amended to reference G1.e; Stop-rule #11 added (synthetic≠live).
- **m1–m3 folded:** pin the gate predicate to `make test-jit METRIC score=100`; neutral-frame the A2 feasibility count ("count JIT-covered rows that REQUIRE FINE/PTE-engine mappings", not "window-feasible"); the two new stop-rules.

**ADVERSARY (primary re-derivation — the decisive round):**
- **AD-2 operationally FALSIFIED** — high-BAT writes are feature-gated dead code on every presentable PVR (gate bit `0x20` @`0x14114`; PVR tables @`0x118c`/`0x11cc` never set it). DEMOTED to dead-code insurance; high-BAT battery rows removed as a decision input; V7 moot. (Caught the recon-adversary's false-positive over-weighting.)
- **AD-1 CONFIRMED + deepened** — live PTE engine remaps via HTAB stores + `tlbie` (7 sites), NOT `mtspr`. The window's `mtspr`-only hook surface is **incomplete** → **Task B PARKED** pending a tlbie/HTAB-store interception design OR a G1.e PTE-engine-disjointness proof (Stop-rule #13; new G1.e sub-check). Good news: standard BATs 528–543 are programmed heavily/unconditionally, so coarse BAT IS the NK's primary tool.
- **V4 FALSIFIED-as-stated:** window cannot be earned at Task A; provisional + softmmu-live-through-G1.e is the only sound reading (folded into Goal-2 / #8).

**Coordinator disposition:** AUTHORIZE **Task A (reshaped):** A1 (high_bat insurance field + `case 560…575` + clean rebuild + static_asserts) + A2 (the mechanism-agnostic `paged_mmu.cpp` translation function + `test_paged_mmu.cpp` against a **SPEC-DERIVED** reference for segment+BAT+HTAB; high-BAT rows dropped; PearPC/QEMU cross-check deferred as owed). Task A is gated (build-ss + `make test-jit` score=100 authoritative + machine test) and structurally inert (new files + dead-code-insurance field). **PARK Task B (the window)** with the tlbie/HTAB-interception residue → its own focused design pass; surface to the user. One-iteration rule: this fold is the single bounded re-pin.
