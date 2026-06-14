# SS_M18 Stage 1 — NewWorld paged MMU: DEEP Task-0 recon (BINDING; gates ALL S1 implementation tasks)

> **Status:** rev-2 (2026-06-14) — draft folded with the two-reviewer pre-implementation red-team
> (PROCESS + TECHNICAL, both GO-WITH-FIXES). The rev-2 BINDING amendments are listed in the
> **Red-team record** at the bottom and are already merged into the body below. **This plan is recon
> + design ONLY.** It pins the three S1 blocking questions and SPECS the MMU-oracle unit test; it
> writes NO `SheepShaver/src/**`. S1 implementation (`machine/paged_mmu.cpp`, the NATMEM `vm_remap`
> shadow hook, the JIT-path wiring, the test body) is the SEPARATE S1-impl milestone, gated on these
> answers.
>
> **Scope law (carried from the program):** Route A is SETTLED and NOT relitigated; the program shape
> (S1→S3→S4) is NOT relitigated; the Discriminator-A verdict (COARSE → path a) is SETTLED
> (`FINDINGS-discriminator-a.md`) and is NOT re-run. This Task-0 operationalizes S1's *own* recon on
> top of those settled inputs.

## Goal (PASS-vs-DIAGNOSTIC)

**PASS (the Task-0 deliverable — falsifiable).** S1's three blocking questions are closed in writing,
each with an evidence tag, AND the MMU-oracle unit test is SPEC'd to an I/O-contract + pinned-fixture
level (NOT "build-ready" — see RT-C2), in a committed addendum (`FINDINGS-s1-paged-mmu.md`). Task-0
PASSES when all of:

1. **Q-S1.1 answered** — a falsifiable verdict on whether a shadow MMU can coexist with
   `DIRECT_ADDRESSING` (paravirtual) **byte-identical**, citing the **enumerated baseline-identity
   field set** (the existing `make e2e` A/B definition; jitter counters excluded — RT-M3) and the
   structural-unreachability gate point that makes the paged path unreachable when
   `!MachineProfileIsNewWorld()`.
2. **Q-S1.2 answered** — the **walker-vs-window** decision PINNED to *window* (host-side `vm_remap`
   aliasing of the separate NATMEM RAM/ROM reservations — **NOT** a "MEM_BULK arena"; see RT-C1),
   with the address-arithmetic disjointness proof (RAM `0x10000000` / ROM `0x50000000` / MacIO
   `0xF3000000`+`0x80000`), OR the conservative residue. **The walker is NOT retired — it stays a
   live fallback until the 16 KB/4 KB host-page adequacy is closed (RT-C3).**
3. **Q-S1.3 answered** — the minimal-faithful-translation verdict PINNED to segment+context-table
   (satisfying `mtsrin`@0x50315290 + 13× `tlbie` + the BAT program), NOT a full hashed-PTE walk, with
   the cited NK evidence; the FINE-falsifier explicitly **carried (conditional-close, RT-M1-tech)**,
   not declared retired.
4. **The MMU-oracle unit-test SPEC** — oracle choice + external-source dependency, I/O contract,
   equal-PA predicate, the **split** JIT-path question (RT-C2), fixture posture (SPEC-only, not
   generated here — RT-M2), file location, Makefile pre-wire note.
5. **Gate-item-0 (parcel provenance):** NK-v02.27 md5 `61c176e90b6365e84e5c660d703e56af` + canonical
   ROM md5 `66210b4f71df8a580eb175f52b9d0f88` re-verified and recorded.

**PASS taxonomy (the floor — RT-C1).** The close-out MUST state which it is:
- **GREEN-PASS** = all three questions pinned with *affirmative* closure → S1-impl may start.
- **RESIDUE-PASS** = ≥1 question closed with a conservative residue (`…UNKNOWN→…risk`) → this is a
  **finding, not a green light**: it BLOCKS S1-impl start and triggers a program-plan re-band by the
  coordinator. (Stop-rule #7 below enforces this.) "Three residues + a test spec" is NOT a license to
  start coding.

**DIAGNOSTIC (recorded, NEVER a gate):** counter-design notes (remap/feasibility-fail counts) for the
future impl; which Dolphin seam needed the most adaptation. Live-NK MMU-install observation under
QEMU is UNAVAILABLE (the rig stalls pre-install) and is therefore NOT a Task-0 deliverable. **An
unexpected live probe marker, if one ever appears, is escalated — not filed-and-ignored (RT-m1).**

## Authoritative inputs

| Doc / source | What it fixes for this Task-0 |
|---|---|
| `docs/superpowers/plans/2026-06-14-ss-m18-trampoline-lle-program.md` — "Stage 1", "Discriminator-A runbook", "Donor acquisition", Stop-rule, "Acceptance discipline" | The S1 charter (Q-S1.1/.2/.3, gates G1.a–e, env-gate, file ownership) this plan operationalizes. **External doc — its "§N"/gate cites are not sections of THIS plan.** Route A + program shape NOT relitigated. |
| `docs/planning/newsheep/FINDINGS-discriminator-a.md` (verdict COARSE → path a) | SETTLED mechanism input: `mtsrin` loop @0x50315290 (256 MB/iter), 8 IBAT/DBAT pairs from a descriptor table, MMIO SR-swap @0x50325894, HTAB RAM-proportional but residual. The unmeasured residual FINE-falsifier (live HTAB PTE density/per-page-perm diversity) routed to S1's oracle test **and explicitly NOT retired live** (rig stalled). Do NOT re-run Discriminator-A. |
| `docs/planning/newsheep/DONOR-NOTES.md` — Donor 1 (Dolphin Dynamic-BAT shadow-arena) + "Mapping to OUR seams" | The port map: arena remap `MemoryManager::UpdateDBATMappings()` `Source/Core/Core/HW/Memmap.cpp` ~L233; BAT rebuild `MMU::DBATUpdated()` `MMU.cpp` ~L1535; 16K/4K fork `CanCreateHostMappingForGuestPages()` ~L343. **NOT `JitArm64/Memmap.cpp`.** DONOR-NOTES:110 already names "a new shadow-remap routine in the NATMEM/`vm_alloc` layer (`main_unix.cpp`)" — i.e. *new code*, not an existing vm_alloc entry point. Dolphin SHA `144d19433aa734c19c34e5978a1b817d2aa12663` (GPLv2). |
| `docs/MILESTONE-WORKFLOW.md` §2 / §4 / §6 / §6b / §6c | The machine; the rules+incidents (unbounded-disasm kill-switch; baseline-is-part-of-the-gate; QEMU topology-not-address caveat); gate tiers; dispatch economics. |
| `docs/AGENT-CONTEXT.md` — Constants, instrument caveats, gate tiers, standing rules | KDP/ECB/NK-base constants; QEMU caveats (behavioral-only, address-oracle LAW); `MachineProfileIsNewWorld()`; gate tiers. |
| `docs/superpowers/plans/2026-06-14-ss-m18-trampoline-lle-gating-task0.md` (rev-2, A1–A9) | The house-template EXEMPLAR. A3 (BAT/SDR1 readback = false-clean) is inherited LAW for Q-S1.3. |

## Codebase facts (PINNED 2026-06-14 — re-verified by the technical red-team; re-verify again at impl)

> All file:line below were live-verified at plan time AND independently re-verified by the technical
> red-team. Cited line numbers may drift ±1–5 lines (RT-m3) — the *fields/functions* are correct;
> refresh the numbers at impl. Each is "re-verify at impl" before any edit.

- **The production memory path is the JIT, which has NO translation chokepoint.** RMEMBASE = ARM64
  `x19`, defined `kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp:445`; loaded ONCE via
  `emit_load_imm64(RMEMBASE, JIT_MEM_BASE)` `:867`. The grep count `RMEMBASE`=80 **includes the
  `#define`, the address-form `a64_add_reg` (:2595), and comments**; the actual reg-form load/store
  sites are **~68 + 1 address-form add** (RT-m2 — say "~68 bare access sites", not "80
  instructions"). Verified NO per-access MMIO/bounds/slow-path guard at any site — MMIO is reached
  purely by Mach-faulting on the PROT_NONE MacIO page (`test_mmio_machfault.cpp`). → The port keeps
  these sites bit-identical; the remap is paid at `mtspr`-time, not access-time (Dolphin's whole
  point).
- **NATMEM reservation = SEPARATE fixed mach allocations, NOT a single MEM_BULK arena (RT-C1, the
  load-bearing correction):** `config-macosx-aarch64.h:409` defines `NATMEM_OFFSET 0x400000000000`
  and **does NOT define `MEM_BULK`**; `sysdeps.h:95` → `DIRECT_ADDRESSING`. So `vm_mac_acquire_fixed`
  (`main_unix.cpp:415`) → `vm_acquire_fixed` → mach `vm_allocate` at a FIXED address
  (`vm_alloc.cpp:378`). Guest RAM (`RAM_BASE 0x10000000`, `:1974`) and ROM (`ROM_BASE 0x50000000`,
  `:2000`) are **separate, non-contiguous** allocations. The MacIO MMIO window is a separate
  `mmap(MAP_FIXED, PROT_NONE)` at `NATMEM_OFFSET+0xF3000000`, size `0x80000` (`:2079–2086`). The
  MEM_BULK sub-range `vm_protect` path (`vm_alloc.cpp:431–442`) is a **different build config and is
  NOT compiled here.** → The shadow remap is **new code using mach `vm_remap` / `mmap MAP_FIXED`** on
  the separate NATMEM sub-ranges (a prior c4 spike verified `vm_remap` WORKS on this machine;
  `KERN_PROTECTION_FAILURE` is the MAP_JIT-only dead-end, irrelevant to the RW data shadow).
  **Disjointness holds:** `0x10000000` / `0x50000000` / `0xF3000000` are well separated — but the
  address-arithmetic proof across all `RAMSize` configs is a budgeted Q-S1.2 sub-question (RT-M1).
- **Supervisor state already stored (Wave 0; "honor-the-write"; fields read by NAME, no struct
  change needed for a translation consumer):** `kpx_cpu/src/cpu/ppc/ppc-registers.hpp`: `sprg[4]`,
  `sdr1`, `bat[16]`, `srr0`, `srr1`, `sr[16]`, `msr` — all in the **"MUST stay LAST" block**. The
  `sr[16]` comment states verbatim *"Stored state only: nothing consults SRs for translation
  (V=P)."* Any new field goes BEFORE this block.
- **Write handlers (store-only; NO translation consumer — A3 false-clean LAW):**
  `kpx_cpu/src/cpu/ppc/ppc-execute.cpp`: `execute_mtsr:1478`, `execute_mtsrin:1485`, `mtspr
  SDR1:1607–1609`, `mtspr SPRG:1616–1621`, `mtspr BAT:1626–1628`. A write-then-read-back check is a
  **false-clean** for any MMU verdict. The interpreter chokepoint is `vm.hpp:225
  vm_do_get_real_address` — NOT the boot path (the JIT is). No existing JIT path reads
  `sdr1`/`bat`/`sr` for translation (re-verified).
- **`MachineProfileIsNewWorld()`** = `machine/machine_profile.cpp:96–99`, decl
  `include/machine_profile.h:37`; O(1) read of process-wide `g_profile` (default
  `MACHINE_PARAVIRTUAL`, set once in `MachineProfileInit`). Cheap, currently boot-time-only → fine
  for **mode selection**, MUST NOT become a per-access JIT branch.
- **Supervisor surface already profile-forked:** `check_spcflags` newworld arm `ppc-cpu.cpp:1986`
  (`deliver_pending_dec_exception` is newworld-only) — the precedent for a boot-time-selected paged
  path.
- **`SheepShaver/src/openfirmware_ci.cpp` does NOT exist yet** (S2 creates it) — S1 does not depend
  on it.
- **Parcel provenance (gate-item-0):** NK-v02.27 = 105280 B, md5
  `61c176e90b6365e84e5c660d703e56af`; canonical ROM md5 `66210b4f71df8a580eb175f52b9d0f88`.
  Re-extract via `tbxi` (venv `/tmp/newsheep/venv`) if /tmp evaporated.

## Blocking-answer table (which S1-impl task blocks on which Task-0 question)

> Residue-disposition map, NOT a start-order license. Per RT-C1, a question closed with a residue is
> a RESIDUE-PASS → its dependent S1-impl task does NOT start until the coordinator re-scopes.

| S1-impl task (future plan) | Blocked by | Residue disposition (conservative; defaults to the harder path) |
|---|---|---|
| Gate the paged path behind `SS_M18_PAGED_MMU ∧ MachineProfileIsNewWorld()`, paravirtual provably byte-identical | **Q-S1.1** | Coexistence not shown byte-identical (enumerated set) → "paged-MMU-in-hybrid UNKNOWN → program-blocking-risk"; NEVER "identity will do". RESIDUE-PASS → blocks impl. |
| Host-side `vm_remap` shadow-remap of NATMEM RAM/ROM sub-ranges at `mtspr` BAT/SDR1/SR (window) | **Q-S1.2** + the **disjointness-arithmetic** sub-question (RT-M1) | Intersect-and-remap can't be shown to map onto mach `vm_remap` / disjointness unproven within box → "window-feasibility-UNKNOWN → walker-risk; keep walker fallback live; re-band". |
| The 16 KB-host / 4 KB-guest adequacy gate (the FINE detector) | **Q-S1.2 host-page sub-question** (RT-C3/RT-M1) | Adequacy unproven → "host-page-mismatch UNKNOWN → walker/softmmu-risk"; **do NOT lock window / retire walker** while this is open. |
| Translation faithfulness: segment+context-table vs full hashed-PTE walk | **Q-S1.3** | NK dependence unclassifiable statically → "faithfulness UNKNOWN → carry FINE-falsifier to the oracle test; softmmu-risk until cleared". |
| The MMU-oracle unit test (pure translation diff) | **Q-S1.1–.3 + the SPEC** | Oracle I/O contract unspecified → headline open item; the impl plan's first task resolves it. |
| The "under-the-JIT" two-context proof (G1.a) | **split out (RT-C2)** | No `kpx_cpu`-linked no-boot harness exists → this falsifier is owed to **a new harness OR SheepShaver's own paged boot**, NOT the pure unit test. State as OPEN, do not claim closed. |
| The whole S1 effort band | **Q-S1.2 ∧ Q-S1.3** | Window + segment-table + host-page-adequate ⇒ bounded (UNKNOWN-months leaning tractable); walker OR hashed-PTE OR host-page-inadequate ⇒ months/softmmu. The band is the verdict, not a dig-trigger. |

## The MMU-oracle unit-test SPEC (retires the residual FINE-falsifier — translator-correctness only)

> Task-0 produces the SPEC + I/O contract + pinned external-oracle dependency (no code, no fixture
> file generated — RT-M2). The S1-impl milestone builds it at `machine/test_paged_mmu.cpp`.

- **What it is.** A standalone, no-boot test that, for a battery of `(SR[16], BAT[16], SDR1, EA)`
  inputs, computes the PA two ways and asserts equality — **(1)** our shadow translation (the S1
  mechanism), **(2)** a reference PPC softmmu translator used purely as an **ORACLE** (PA-diff only).
- **The JIT-path question is SPLIT (RT-C2 — decisive technical finding).** The `machine/` harness
  does NOT link `kpx_cpu`/`ppc-jit`/`ppc-execute` (the machine `Makefile` links only device/mmio
  objects; the two "JIT" tests hand-assemble a single access word and Mach-fault on it — they do not
  stand up `powerpc_cpu`). Therefore:
  - **G1.a-pure (unit-testable NOW):** the translation function `our_translate(SR,BAT,SDR1,EA)→PA`
    diffs equal against the oracle across the battery. Feasible as a standalone test.
  - **G1.a-under-JIT (NOT unit-testable as first designed):** proving the bare `LDR/STR [RMEMBASE,
    EA]` lands on the remapped PA "under the JIT" needs **either** a new harness linking the full
    `kpx_cpu` (W^X code cache + dispatcher + `ppc-execute` — substantial) **or** SheepShaver's own
    paged boot (G1.e). The Task-0 deliverable is to SPEC G1.a-pure and to record G1.a-under-JIT as an
    OPEN owed-falsifier with its two candidate vehicles — NOT to claim G1.a is closeable by a unit
    test.
- **Oracle choice (PA-diff; never port a per-access walker — Stop-rule #6).** PearPC
  `ppc_effective_to_physical()` (`ppc_mmu.cc`) is the PRIMARY candidate (small, self-contained:
  hash+BAT+SR in one function) used as an **out-of-process, UNLINKED fixture generator** (avoids
  GPL-link entanglement and the ported-walker anti-pattern); QEMU `target/ppc/mmu_common.c` is the
  SECONDARY cross-check. **Neither is in-tree (RT-M2-tech):** the oracle is an explicit external
  dependency that S1-impl must SOURCE and PIN (repo + file + SHA in the fixture-generation comment).
- **Inputs (the battery).** (a) the two-context shape from `mtsrin`@0x50315290 — the SAME EA under
  two SR programmings yields two distinct PAs; (b) the BAT-block program (256 MB-class, the COARSE
  case, expected fast-path-safe); (c) the **adversarial FINE case** — dense 4 KB hashed-PTEs with
  mixed per-page perms over a JIT-covered region: if our `CanCreateHostMappingForGuestPages`-
  equivalent feasibility check fails pervasively on (c) AND the NK is later shown to depend on (c),
  S1 re-bands to softmmu.
- **Equal-PA predicate.** `our_translate(...) == oracle_PA(...)` for every row; permission/fault rows
  assert the same fault-vs-resolve outcome (not only the PA). The host-address-backing check (a bare
  access lands on the oracle's guest physical page) is part of **G1.a-under-JIT**, owed to the harness
  or boot per the split above.
- **What it retires vs does not.** It retires the FINE-falsifier as a *translator-correctness*
  property across coarse AND fine inputs — a **conditional-close** (RT-M1-tech: the Discriminator-A
  source explicitly says the live residual is NOT retired). It does NOT prove the live NK only uses
  coarse mappings — that is G1.e on the real boot.

## Budgets (caps with WRITTEN residue fallbacks — unbounded disasm killed predecessor agents)

> Caps bind; partial-findings-beat-stalling. Hard cap = the function/instruction window.

- **Q-S1.1 (static — SS source only):** ~45 min. Window: the gate sites only —
  `machine_profile.cpp:96–99`, `sysdeps.h` `DIRECT_ADDRESSING`, `ppc-cpu.cpp:1986`, NATMEM reserve
  `main_unix.cpp:1974/:2000/:2079`. Pin the structural-unreachability branch + cite the enumerated
  byte-identical field set. **Residue:** "coexistence-UNKNOWN → program-blocking-risk" (RESIDUE-PASS).
- **Q-S1.2 (static — DONOR-NOTES + `vm_alloc`/`main_unix.cpp`; NO NK disasm):** ~75 min, split into
  three budgeted sub-questions (RT-M1):
  - **(2a) remap mechanism:** confirm the Dolphin intersect-and-remap loop maps onto mach `vm_remap`
    / `mmap MAP_FIXED` on the separate NATMEM RAM/ROM reservations (NOT MEM_BULK). Residue:
    "remap-mechanism-UNKNOWN → walker-risk".
  - **(2b) disjointness arithmetic:** prove the remappable RAM/ROM sub-ranges are disjoint from the
    MacIO `MAP_FIXED` window (`:2079–2086`) across all `RAMSize` configs. Residue: "collision-risk →
    default to a separate reservation, not a sub-range remap".
  - **(2c) 16 KB/4 KB host-page adequacy:** assess whether the COARSE (256 MB SR/BAT) mapping is
    16 KB-host-safe AND whether the `CanCreateHostMappingForGuestPages` gate is needed as the FINE
    detector. Residue: "host-page-mismatch UNKNOWN → keep walker fallback live; do NOT lock window".
- **Q-S1.3 (static — re-reads Discriminator-A's captured evidence; NO new disasm unless cited addrs
  fail re-verify):** ~30 min. Confirm `mtsrin`@0x50315290 / SR-swap@0x50325894 / 13× `tlbie` /
  BAT-descriptor evidence still matches the md5-verified parcel; pin segment+context-table. Hard
  window if re-disasm needed: ≤2 call levels / ≤8 functions around 0x50315290. **Residue:**
  "faithfulness-UNKNOWN → softmmu-risk; carry FINE-falsifier to oracle test".
- **QEMU boots: ZERO.** Discriminator-A already spent ≤4 boots; the rig **stalls in the OF→OS
  handoff before NK MMU-install** (Evidence B — SDR1 stays at OpenBIOS's `0x1f80003f`, NK bps never
  fire). Live-NK MMU observation is UNAVAILABLE via this rig. Spending a boot here is a Stop-rule
  violation. **A boot IS owed later** — at S1 validation, once S1's own paged path can boot
  SheepShaver to the NK MMU-install cluster (G1.e); the program carries S1's irreducible "first
  live-NK proof is post-construction" exposure (RT zero-boot vote).
- **Unbounded-disasm kill-switch:** any static thread past its window without a verdict STOPS and
  records the conservative residue. Do NOT disassemble the whole 105280 B parcel.

## Gates G1.a–e (S1-IMPL gates restated falsifiable-in-advance; Task-0 ensures each is answerable)

> These run in the future S1 acceptance battery. The Task-0 produces the pinned contracts each
> checks; it does NOT run them.

- **G1.a-pure — two-context translation diff (unit-testable now).** Same EA → two distinct PAs under
  two SR contexts (the `mtsrin`@0x50315290 shape), oracle-diff equal. Identity-only FAILS by
  construction.
- **G1.a-under-JIT — the bare access lands on the remapped PA on the JIT path (OWED falsifier,
  split per RT-C2).** Vehicle: a new `kpx_cpu`-linked harness OR G1.e on the real boot. NOT closeable
  by the pure unit test.
- **G1.b — MMIO-via-SR-swap microtest** mirroring `mtsrin`/`mfsrin` around an access at the pattern
  of 0x50325894. Q-S1.3 pins the reference shape.
- **G1.c — `make e2e` paravirtual A/B (enumerated identity set) + multi-run soak unchanged**
  (`SS_E2E_RUNS=N` median±CV%) + a `make bench` ns/insn delta. S1 edits a shared runtime-gated path
  → the structural-inertness substitute is **NOT** allowed for S1.
- **G1.d — `make test-jit`=100** (353-vector harness) + `make -C SheepShaver/src/machine test` (incl.
  the new `test_paged_mmu` G1.a-pure). "Build-ready" of the test is confirmed HERE (G1.d), NOT in
  Task-0 (RT-C2).
- **G1.e — probe NK boot under translation reaches a translation-dependent landmark** (the `mtsrin`
  context switch executing correctly live). Runs on the **real SheepShaver newworld boot under S1's
  own paged path**, NOT the QEMU rig (which stalls pre-install). The one gate whose instrument differs;
  carries the FINE residual to its live retirement.

## File ownership (carried — for the S1-IMPL milestone, NOT this Task-0 which writes no src)

- **New (impl):** `SheepShaver/src/machine/paged_mmu.cpp` + `SheepShaver/src/machine/
  test_paged_mmu.cpp` (Makefile target pre-wired by the coordinator at impl-kickoff; note the harness
  currently links no `kpx_cpu` — the G1.a-under-JIT vehicle may require a NEW harness target, RT-C2).
- **Edits (impl; serialized, strong tier), per Q-S1.2 = window:** the host NATMEM layer —
  `SheepShaver/src/Unix/main_unix.cpp` (the new `vm_remap` shadow-remap routine; `BasiliskII/src/
  CrossPlatform/vm_alloc.cpp` only if a shared primitive is factored there); `Unix/sysdeps.h` (mode
  switch behind the gate); the interpreter chokepoint `kpx_cpu/src/cpu/vm.hpp` for parity; the `mtspr`
  BAT/SDR1/SR handlers in `ppc-execute.cpp:1607/1626/1478/1485` gain the remap-hook call. The JIT
  emit sites `ppc-jit.cpp` are touched ONLY if Q-S1.2 flips to walker (it does not, per
  Discriminator-A — the ~68 sites stay bit-identical).
- **S1 owns the memory-translation surface; no other stage edits it concurrently. S1 must NOT touch
  `sheepshaver_glue.cpp` (S2/S3).**

## Stop-rule (triggers named in advance; one-iteration mechanics)

1. **#2 — "Fake the MMU with identity-only when translation is load-bearing."** Q2 pinned positive
   consumption; G1.a-pure FAILS on identity-only by construction; readback-consistency is a
   **false-clean** (A3 LAW). Identity-only ⇒ default UNKNOWN→months, never "fine".
2. **#5 — Unbounded disasm.** Any Q-S1.x static thread past its function/instruction window ⇒ STOP,
   record the conservative residue.
3. **#6 — QEMU-as-address-oracle.** No QEMU MMIO address is a reference value for our machine layer
   (QEMU MacIO 0x80000000 vs ours 0xF3000000). The oracle is used for PA-diff of the translation
   function only; never break on a SheepShaver NK address under QEMU.
4. **"Run more stalled QEMU boots to see the live NK MMU install."** STOP — the rig provably stalls
   pre-install. The FINE-falsifier is conditionally-closed by the oracle SPEC, retired live at G1.e.
5. **"Start writing `machine/paged_mmu.cpp` / the remap hook / the test body mid-Task-0."** This
   Task-0 writes no `SheepShaver/src/**`; the test is SPEC'd here, BUILT in S1-impl.
6. **"Port the oracle's per-access walker into our tree."** The oracle is a reference-PA fixture
   generator, never a runtime translator — a ported per-access softmmu walker is the perf-fatal
   anti-pattern.
7. **"Proceed to S1-impl on a RESIDUE-PASS."** (RT-C1) A blocking question closed with a conservative
   residue BLOCKS its dependent impl task; the coordinator re-bands the program first. RESIDUE-PASS ≠
   green light.
8. **"Lock walker-vs-window to window while 16 KB/4 KB host-page adequacy is UNKNOWN."** (RT-C3)
   Prefer window, but the walker stays a live fallback until Q-S1.2(2c) closes affirmatively.
9. **"Cite MEM_BULK / a single contiguous arena as the remap substrate."** (RT-C1) It is not compiled
   in this build; the mechanism is mach `vm_remap` on the separate NATMEM reservations.

**One-iteration rule.** A pinned answer falsified within this Task-0 → dated falsification entry in
the addendum → ONE bounded re-pin (≤1 disasm window from the remaining cap; ZERO boots) → resume. A
SECOND falsification of the same answer ⇒ STOP, re-plan this Task-0.

## Self-review record

Spec coverage: all coordinator inputs consumed (program Stage-1 section; `FINDINGS-discriminator-a.md`
COARSE→path a with the FINE residual conditional; `DONOR-NOTES.md` Donor 1 + seam map; the
house-template exemplar; the PINNED codebase facts). Recon+design ONLY — no `SheepShaver/src/**`. The
Discriminator-A verdict is SETTLED (not re-run); Route A + program shape NOT relitigated; QEMU boots =
ZERO by design.

The four tensions originally flagged for the red team (16 KB/4 KB adequacy; JIT-in-unit-test
feasibility; FINE-falsifier retirement under a stalled rig; MEM_BULK-vs-MacIO safety) were ALL upheld
by the red team and are folded as the rev-2 amendments below — the most consequential being RT-C1
(MEM_BULK does not exist in this build → mach `vm_remap` on separate reservations) and RT-C2 (no
`kpx_cpu`-linked harness → split G1.a). No residual self-review tension remains open.

## Red-team record (rev-2 — BINDING amendments folded above)

Two reviewers, run in parallel, both **GO-WITH-FIXES**. All findings folded as binding amendments;
no re-plan required.

**TECHNICAL/CONTRACTS reviewer (independent re-verification against source):**
- **RT-C1 (CRITICAL, folded):** the plan's "remap NATMEM sub-ranges onto a vm_alloc MEM_BULK arena"
  is FALSE for this build — `config-macosx-aarch64.h:409` defines only `NATMEM_OFFSET`,
  `sysdeps.h:95`→`DIRECT_ADDRESSING`; RAM (`:1974`)/ROM (`:2000`) are separate `vm_allocate` calls,
  not one remappable reservation. Mechanism corrected to mach `vm_remap`/`mmap MAP_FIXED` on the
  separate sub-ranges (c4 spike verified working). Disjointness of `0x10000000`/`0x50000000`/
  `0xF3000000` holds. Stop-rule #9 added.
- **RT-C2 (CRITICAL, folded):** the `machine/` harness links no `kpx_cpu`/`ppc-jit`; "exercise the
  JIT path" in a no-boot unit test is not feasible as designed. G1.a SPLIT into G1.a-pure
  (unit-testable now) + G1.a-under-JIT (owed to a new harness OR G1.e). "Build-ready" deferred to
  G1.d.
- **RT-M1-tech (MAJOR, folded):** vm_alloc exposes no sub-range remap in this config; the remap is
  new code; address-arithmetic disjointness proof required before S1-impl (now Q-S1.2(2b)).
- **RT-M2-tech (MAJOR, folded):** no PearPC/QEMU source in tree — the oracle is an explicit external
  source-and-pin dependency.
- **RT-m1/2/3-tech (minor, folded):** "FINE retired" → conditional-close; "80 sites" → "~68 bare
  access sites"; cited line numbers drift ±1–5, fields correct.

**PROCESS reviewer (gating honesty / falsifiability / budgets / stop-rule coverage):**
- **RT-C1-proc (CRITICAL, folded):** no PASS floor — all-residue PASS was gameable. Added the
  GREEN-PASS / RESIDUE-PASS taxonomy + Stop-rule #7 (RESIDUE-PASS blocks impl, triggers re-band).
- **RT-C2-proc (CRITICAL, folded):** "build-ready test design" assumes the impl interface and is not
  Task-0-falsifiable. Downgraded the deliverable to test SPEC + I/O contract + pinned fixture;
  build-ready deferred to G1.d.
- **RT-C3-proc (CRITICAL, folded):** premature window-lock retires the walker before its known
  falsifier closes. Walker kept a live fallback; Stop-rule #8 added.
- **RT-M1-proc (MAJOR, folded):** the 16 KB/4 KB and disjointness risks had no budget line — promoted
  to Q-S1.2(2b)/(2c) budgeted sub-questions with conservative residues.
- **RT-M2-proc (MAJOR, folded):** fixture generation is a write — Task-0 specifies/records only; impl
  generates.
- **RT-M3-proc (MAJOR, folded):** "byte-identical" now cites the enumerated `make e2e` A/B field set
  (jitter counters excluded).
- **RT-m1-proc (minor, folded):** an unexpected live probe marker is escalated, not discarded.

**Convergent votes (both reviewers):** (1) host-page adequacy = accept window-as-bounded for COARSE
**with** the `CanCreateHostMappingForGuestPages` gate kept as the FINE detector; (2) JIT-in-unit-test
= split the gate (not closeable as a unit test); (3) FINE-falsifier = conditional-close; (4)
MEM_BULK-vs-MacIO = mechanism correction (`vm_remap`) + disjointness proof, disjointness holds; (5)
walker-vs-window = keep walker a live fallback; (6) oracle posture = endorse unlinked external
fixture generator + source-and-pin; (7) zero-boot = endorse for Task-0, a boot is owed at G1.e on
SheepShaver's own paged path.
