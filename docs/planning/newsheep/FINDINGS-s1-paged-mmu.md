# FINDINGS — SS_M18 Stage 1 (NewWorld paged MMU): DEEP Task-0 recon

**Date:** 2026-06-14   **Agent:** S1 paged-MMU gating-Task-0 (BINDING recon)
**Authority (spec):** `docs/superpowers/plans/2026-06-14-ss-m18-s1-paged-mmu-task0.md` (rev-2)
**Supporting inputs consumed:** `FINDINGS-discriminator-a.md` (COARSE → path a, SETTLED — NOT re-run),
`DONOR-NOTES.md` (Donor 1 Dolphin shadow-arena seam map), `docs/AGENT-CONTEXT.md`.
**Method:** static source reads + grep + capstone of the md5-verified NK parcel + md5 only.
**NO `SheepShaver/src/**` written. NO builds. NO boots. ZERO QEMU boots.** All file:line
re-verified this session (the plan's pinned numbers may drift ±5; refreshed values recorded below).

---

## VERDICT (rev-1 draft): GREEN-PASS — **RE-BANDED rev-2 → RESIDUE-PASS** by the three-reviewer red-team.

> **rev-2 (2026-06-14):** the ADVERSARY reviewer (re-deriving from the md5-verified parcel) produced
> **two FALSIFICATIONS** the spec/technical reviewers (both UPHOLD-GREEN) missed. They do NOT kill the
> window approach, but they make GREEN dishonest and re-band the verdict. **Binding amendments
> AD-1/AD-2 are folded into Q-S1.3 + the oracle SPEC below; full record at the bottom.**
>
> **Net verdict = RESIDUE-PASS (the anticipated FINE/high-BAT residue).** Q-S1.1 (coexistence) and the
> Q-S1.3 *segment-layer* mechanism stay AFFIRMATIVE. But Q-S1.2(2c)/Q-S1.3-FINE carries a **real
> carried residue**: the NK runs a **live 4 KB PTE engine** (HTAB is NOT "residual") AND programs
> **high/extended BATs SPR 560–575** that SheepShaver currently drops (`ppc-execute.cpp:1651`) and the
> oracle's `BAT[16]` contract cannot even express. → **The walker/softmmu stays the DEFAULT, not a
> contingency**, and S1-impl does NOT start by "betting on the window."
>
> **S1-impl gating first step (the re-band):** extend the supervisor register model to cover high BATs
> (SPR 560–575) + capture them in the `mtspr` handler, fix the oracle I/O contract to
> `(SR[16], BAT[16], HIGH_BAT[16], SDR1, EA)`, then run the FINE/high-BAT oracle battery + G1.e live
> coverage instrumentation. THAT measurement *earns* the window (or confirms softmmu). The window
> remap code is not written until the FINE/high-BAT coverage is measured.
>
> **⚠ rev-3 correction (2026-06-14, from the S1-IMPL plan ADVERSARY — read this):** **AD-2 was
> subsequently OPERATIONALLY FALSIFIED.** The high-BAT writes (SPR 560–575) are **feature-gated dead
> code** on every PVR SheepShaver can present (gate bit `0x20` @`0x14114`; the PVR feature tables
> @`0x118c`/`0x11cc` never set it; SS PVR `0x000c0000` = 7400, which architecturally has no extended
> BATs). So this rev-2 verdict's framing **over-weighted AD-2** (an M8→M17 *false-positive* — the mirror
> of the failure the re-band guarded against). **The RESIDUE-PASS still stands, but on AD-1 alone
> (strengthened):** the live 4 KB PTE engine remaps via **HTAB stores + `tlbie`, NOT `mtspr`**, so a
> window hooking only `mtspr`/`mtsr`/`mtsrin` silently desyncs on any PTE-engine remap of a JIT-covered
> page → window adequacy is owed to **G1.e** (a real boot), softmmu is the DEFAULT until then. The
> S1-impl first step is therefore **reshaped**: `high_bat[16]` is cheap dead-code INSURANCE (not a
> decision input); the real deliverable is the mechanism-agnostic translation function + oracle test;
> the window (Task B) is PARKED pending a tlbie/HTAB-store interception design. Full record:
> `docs/superpowers/plans/2026-06-14-ss-m18-s1-impl-paged-mmu.md` rev-2 Red-team record.

S1-impl may proceed **into that gating first step** (subject to coordinator review). The G1.a-under-JIT
proof and the empirical `make e2e` A/B run remain **owed-falsifiers** at their named gates (G1.d/G1.c/G1.e).

> Honesty note: Q-S1.1 is closed on **structural-unreachability** evidence + the enumerated identity
> field set (PASS-criterion #1); the empirical A/B is owed at G1.c. Q-S1.2(2b) carries one **bounded,
> non-blocking** arithmetic caveat (RAMSize > 1 GiB → impl-guard `RAMSize ≤ 0x40000000`).

---

## Gate-item-0 — parcel provenance (re-verified THIS session)

| Artifact | Path | md5 | Status |
|---|---|---|---|
| NanoKernel-v02.27 (105280 B) | `/tmp/newsheep/dump-9.0.1/Parcels.src/MacROM.src/NanoKernel-v02.27` | `61c176e90b6365e84e5c660d703e56af` | **MATCH** (present in /tmp) |
| Canonical 9.0.1 ROM | `/Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom` | `66210b4f71df8a580eb175f52b9d0f88` | **MATCH** |

Both match the recorded gate-item-0 values. The parcel is live in /tmp, so Q-S1.3 was re-verified by
capstone against the exact md5-matched bytes (below) rather than from recorded evidence.

---

## Q-S1.1 — Can a shadow MMU coexist with `DIRECT_ADDRESSING` (paravirtual) byte-identical?

**Answer: YES (affirmative).** Evidence tag: **[STATIC-GATE]** (structural unreachability) +
**[OWED-G1.c]** (empirical A/B). Budget: ~30 min, within the 45-min box. No residue.

### Structural-unreachability gate (the load-bearing evidence)
- `DIRECT_ADDRESSING` is the compiled addressing mode: `Unix/sysdeps.h:96` `#define DIRECT_ADDRESSING 1`
  (selected at `:95` `#elif defined(NATMEM_OFFSET) || defined(MEM_BULK)`; `NATMEM_OFFSET 0x400000000000`
  is defined at `config-macosx-aarch64.h:409` — file lives at
  `SheepShaver/src/MacOSX/config/config-macosx-aarch64.h`, **not** `Unix/Darwin/`). The plan's `:95`
  cite is the `#elif`; the `#define` lands at `:96` (drift +1, fields correct).
- The mode-selection predicate is `MachineProfileIsNewWorld()` = `machine/machine_profile.cpp:96–99`
  (re-verified: `return g_profile == MACHINE_NEWWORLD;`), an O(1) read of process-wide `g_profile`
  (default `MACHINE_PARAVIRTUAL`). Cheap; boot-time mode-select only — MUST NOT become a per-access
  JIT branch.
- **Precedent for a boot-time-selected paged path:** `ppc-cpu.cpp:1986` — the `check_spcflags`
  newworld arm (`if (MachineProfileIsNewWorld()) { … deliver_pending_dec_exception … }`) re-verified
  present. The same gate idiom makes the remap hook **structurally unreachable** when
  `!MachineProfileIsNewWorld()`: the impl gate is `SS_M18_PAGED_MMU ∧ MachineProfileIsNewWorld()`, so
  the paravirtual build never reaches the `vm_remap` shadow-remap call from the `mtspr` handlers.
- **Access path is untouched regardless of profile:** the ~68 bare `RMEMBASE` access sites
  (`ppc-jit.cpp`, `RMEMBASE 19` at `:445`, loaded once via `emit_load_imm64(RMEMBASE, JIT_MEM_BASE)`
  in the block prologue) are bit-identical between profiles — the remap is paid at `mtspr`-time, never
  at access-time (Dolphin's whole point; DONOR-NOTES "Mechanism summary"). grep `RMEMBASE`=80
  includes the `#define`, the address-form `a64_add_reg`, and comments → ~68 bare reg-form sites
  (RT-m2 wording adopted).

### Enumerated byte-identical field set (cited per PASS-criterion #1; empirical run owed at G1.c)
The baseline-identity set is the **existing `make e2e` A/B definition** (`SheepShaver/e2e/README.md`):
the signal-driven boot sequence (`[BOOT]`→`[READY]`→`[APP]` ordering), clean-exit assertion, and the
per-run REGDUMP-equivalent state — **jitter counters excluded** (RT-M3). Q-S1.1 cites this set; it does
**not** run it (boots/builds forbidden here). The empirical A/B + `SS_E2E_RUNS=N` median±CV% soak +
`make bench` ns/insn delta is **owed at G1.c** (the plan explicitly forbids the structural-inertness
substitute for S1 — G1.c must run because S1 edits a shared runtime-gated path).

**Residue:** none. (Were the gate not structurally unreachable, the disposition would be
"coexistence-UNKNOWN → program-blocking-risk" → RESIDUE-PASS. It is unreachable; affirmative.)

---

## Q-S1.2 — Walker vs window, in 3 budgeted sub-questions

**Answer: WINDOW (host-side `vm_remap` aliasing), affirmative — walker kept a LIVE fallback (Stop-rule
#8).** Evidence tag **[STATIC-SRC]**. Budget ~45 min, within the 75-min box. One bounded non-blocking
caveat in (2b).

### (2a) Remap mechanism — mach `vm_remap` / `mmap MAP_FIXED` on the SEPARATE NATMEM reservations
**Confirmed; NOT MEM_BULK.** Evidence:
- `config-macosx-aarch64.h:409` defines **only** `NATMEM_OFFSET` — **no `MEM_BULK`** (grep returned
  the single `NATMEM_OFFSET` line). `MEM_BULK` appears in-source solely as (i) the `#elif` condition in
  `sysdeps.h:95` and (ii) `#ifdef MEM_BULK` guards in `BasiliskII/src/CrossPlatform/vm_alloc.cpp`
  (`:86,:185,:219`) — **a different build config, not compiled here.** The `vm_protect` sub-range path
  (`vm_alloc.cpp:353`) lives inside that uncompiled region.
- Guest RAM and ROM are **separate, non-contiguous** fixed mach allocations:
  `RAM_BASE 0x10000000` (`main_unix.cpp:197`, acquired `:1974`), `ROM_BASE 0x50000000` (`:199`,
  acquired `:2000`), each via `vm_mac_acquire_fixed` (`:415`) → `vm_acquire_fixed` → mach `vm_allocate`
  at a FIXED address (`vm_alloc.cpp:298`).
- So the shadow remap is **new code using mach `vm_remap` / `mmap MAP_FIXED`** on the separate NATMEM
  sub-ranges (Dolphin's `UpdateDBATMappings` intersect-and-remap loop, DONOR-NOTES "The remap itself"),
  hooked at the `mtspr` BAT/SDR1/SR handlers — **not** a single contiguous arena. **Residue:** none.
  (Stop-rule #9 honored: MEM_BULK / contiguous-arena is NOT cited as the substrate.)

### (2b) Disjointness arithmetic — remappable RAM/ROM vs the MacIO `MAP_FIXED` window
**Disjoint for every real config.** Evidence:
- MacIO window: `mmap((void*)(NATMEM_OFFSET + 0xF3000000), 0x80000, PROT_NONE, MAP_FIXED…)` at
  `main_unix.cpp:2079–2081`, gated by `MachineUsesMMIOBus()` (`:2070`). Host span =
  `[NATMEM+0xF3000000, NATMEM+0xF3080000)`.
- All three live in the same NATMEM window (`Mac2HostAddr = guest + NATMEM_OFFSET`):
  - RAM: `[0x10000000, 0x10000000 + RAMSize)`
  - ROM: `[0x50000000, 0x50000000 + ROM_AREA_SIZE + SIG_STACK_SIZE)`
  - MacIO: `[0xF3000000, 0xF3080000)`
- Gap RAM→ROM = `0x50000000 − 0x10000000 = 0x40000000` (1024 MiB). MacIO sits **2.05 GiB above** ROM.
  `RAMSize` min is clamped to 16 MiB (`:1912–1914`); NewWorld configs use 256 MiB. Disjointness holds
  for all `RAMSize ≤ 0x40000000` (1 GiB) — i.e. every supported config.
- **Bounded non-blocking caveat (recorded, not a residue):** there is no hard upper `RAMSize` cap in
  `main_unix.cpp` (pref-driven). A pathological `RAMSize > 1 GiB` would make RAM overrun `ROM_BASE`.
  Disposition: the remap routine MUST assert `RAMSize ≤ 0x40000000` (or fall back to a separate
  reservation) — a one-line impl guard, **not** a program-blocking risk. Affirmative.

### (2c) 16 KB-host / 4 KB-guest adequacy — COARSE is 16 KB-safe; keep the FINE detector
- Discriminator-A SETTLED the NK working set as **COARSE** (256 MB-class SR/BAT blocks). A 256 MB block
  is trivially 16 KB-aligned and physically contiguous ⇒ Dolphin's
  `CanCreateHostMappingForGuestPages()` (DONOR-NOTES ~L343: alignment + 4-constituent-contiguity +
  shared-perm test) passes ⇒ the shadow-arena port is **16 KB-host-page-safe** on Apple Silicon (host
  page = 16 KB, Dolphin permanently on the `LargePages` path).
- **Per Stop-rule #8 / RT-C3: the walker is NOT retired.** Prefer window, but keep the
  `CanCreateHostMappingForGuestPages`-equivalent feasibility check as the **FINE detector** and the
  softmmu walker as a **live fallback** until (2c) closes affirmatively under the oracle test's
  adversarial-FINE battery. Affirmative-for-COARSE, walker retained.

**Q-S1.2 overall residue:** none blocking; window pinned; walker live; one bounded arithmetic guard owed
to impl.

---

## Q-S1.3 — Minimal faithful translation = segment + context-table (NOT a full hashed-PTE walk)

**Answer: segment + context-table, affirmative.** Evidence tag **[STATIC-CAPSTONE]** (re-verified vs
the md5-matched parcel this session). Budget ~25 min, within the 30-min box. No residue.

Capstone disasm (`CS_MODE_BIG_ENDIAN`, parcel base 0x50310000) of the md5-verified NK:

| Site | Addr | Disasm (verified) | Meaning |
|---|---|---|---|
| Context loop | `0x50315290` | `lwzu r30,-8(r28); addis r31,r31,-0x1000; or. r31,r31,r31; mtsrin r30,r31; bne 0x50315290` | per-iter `addis -0x1000` steps EA top 4 bits = **one 256 MB segment/iter** — context switch at segment granularity. Matches Discriminator-A exactly. |
| MMIO seg-swap | `0x50325894` | `mfsrin r21,r22; lwzx r24,r25,r24; mtsrin r24,r22; isync; mtmsr r20` | MMIO reached by swapping a whole **segment register** (SR-granular, not per-page). |
| SDR1 program | `0x50310604` | `mtspr 0x19,r12` (SPR 25 = SDR1) | HTAB base/mask set once. |
| SR unroll | `0x503104b4` | `mtsr 0,r12; mtsr 1,r0; mtsr 2,r0; …` | all 16 SRs programmed (unrolled). |

- **TLB management is bounded, not a per-page PTE walk.** Single-pass opcode-31 byte filter (bounded,
  non-recursive — NOT the prohibited unbounded/recursive disasm of Stop-rule #5; within the Q-S1.3
  window): **7 `tlbie`** (`0x50310b60, 0x503138dc, 0x50314f64, 0x503150c0, 0x503155f0, 0x50319af8, 0x5032253c`) +
  **6 `tlbsync`** (`0x503138e8, 0x50314f70, 0x503150cc, 0x50315608, 0x50319b04, 0x50322548`) = **13
  TLB-management ops total** — this refines the plan/charter's loose "13× `tlbie`" (it is 7 tlbie + 6
  tlbsync). A handful of bulk invalidations, **not** a hashed-PTE-density loop. Mechanism unchanged;
  this is a precision refinement, **not a falsification** (one-iteration rule not triggered).
- **Minimal faithful translation** is a **segment + context-table** for the SR/segment layer
  (`mtsrin`@0x50315290 + the SR-swap MMIO idiom + the BAT descriptor program + the 13 bounded TLB ops)
  — the segment-granular mechanism is COARSE and AFFIRMATIVE.
- **⚠ AD-1 (rev-2 binding amendment — ADVERSARY FALSIFICATION of "HTAB is residual"):** primary
  evidence shows the NK runs a **live, fine-grained 4 KB PTE insert/invalidate engine**, not an inert
  HTAB. At `0x50319af8`: `slwi r9,r4,0xc; sync; tlbie r9; tlbsync`, then `0x50319b28`:
  `stw r16,0(r15); stw r9,4(r14); eieio; stw r8,0(r14)` with `oris r8,r8,0x8000` setting the PTE **V
  bit** — a classic invalidate-then-rewrite PTE update at 4 KB granularity. The HTAB is **actively
  managed**. Whether any JIT-fast-path RAM/ROM range is backed by dense 4 KB PTEs with mixed perms is
  **unmeasured live** (Discriminator-A Evidence B: rig stalled) and **table-driven** (BAT ranges from a
  runtime descriptor `lwz r16,0x80(r8)`…), so "RAM/ROM are BAT-mapped" is **asserted, UNPROVABLE-NOW**.
  → the FINE risk is a **carried residue**, not a conditional-close; **walker stays default** until G1.e.
- **⚠ AD-2 (rev-2 binding amendment — ADVERSARY FALSIFICATION, state-modeling gap):** the NK programs
  the **high/extended BATs SPR 560–575 (`mtspr 0x230..0x23f`) 24×**, table-driven, in addition to the
  standard BAT writes. **SheepShaver models only `bat[16]` = SPR 528–543** (`ppc-registers.hpp:266`);
  `ppc-execute.cpp:1626` captures only `SPR_IBAT0U…SPR_DBAT3L`, and the `default` case (`:1651`)
  **silently drops** SPR 560–575. So the high-BAT writes are discarded today AND the Task-0 oracle's
  `(SR[16], BAT[16], SDR1, EA)` input contract **cannot represent them** — if any JIT-covered range is
  high-BAT-mapped, the translator is structurally blind to it. **Required S1-impl scope:** append
  `high_bat[16]` to the supervisor block (BEFORE the "MUST stay LAST" hazard — see below), capture SPR
  560–575 in the `mtspr` handler, and extend the oracle I/O contract (see SPEC).
- **A3 false-clean LAW honored:** the `mtspr`/`mtsr` handlers are **store-only** with no translation
  consumer — `execute_mtsr:1478`, `execute_mtsrin:1485`, `mtspr SDR1:1607–1609`, `mtspr
  SPRG:1616–1620`, `mtspr BAT (SPR_IBAT0U…SPR_DBAT3L):1626–1628` (re-verified: `regs().bat[…] = s;`).
  Supervisor state stored by NAME in `ppc-registers.hpp` "MUST stay LAST" block (`:256`): `sprg[4]:258`,
  `sdr1:264`, `bat[16]:266`, `srr0:269`, `srr1:270`, `sr[16]:271` — the `sr[16]` comment verbatim
  *"Stored state only: nothing consults SRs for translation (V=P)."* (`:275`). A
  write-then-read-back check would be a false-clean; the real consumer is the new translation
  function. Any new field goes BEFORE this block (struct-offset hazard).
- **FINE-falsifier:** explicitly **carried (conditional-close, RT-M1-tech)** to the oracle test's
  adversarial-FINE battery — **NOT retired live** (the QEMU rig stalls pre-NK-MMU-install; live
  retirement is owed at G1.e). Residue: none for the static verdict; the live residual is
  carried-by-design.

---

## MMU-oracle unit-test SPEC (Task-0 deliverable — SPEC + I/O contract + pinned external oracle)

> SPEC only. No code, no fixture file generated here (RT-M2). S1-impl builds it at
> `SheepShaver/src/machine/test_paged_mmu.cpp`; "build-ready" is confirmed at G1.d, not here.

### The split (RT-C2 — decisive)
The `machine/` Makefile links **only device/mmio objects** — **NO** `kpx_cpu`/`ppc-jit`/`ppc-execute`
(the two existing "JIT" tests hand-assemble a single access word and Mach-fault on it; they do not
stand up `powerpc_cpu`). Therefore:
- **G1.a-pure (unit-testable NOW):** `our_translate(SR[16], BAT[16], SDR1, EA) → PA` diffs **equal**
  against the oracle PA across the battery. Standalone, no boot, no `kpx_cpu` link. This is what the
  Task-0 SPECs.
- **G1.a-under-JIT (OWED — NOT unit-testable as first designed):** proving the bare
  `LDR/STR [RMEMBASE, EA]` lands on the remapped PA *under the JIT* needs **either** a NEW harness
  linking the full `kpx_cpu` (W^X code cache + dispatcher + `ppc-execute`) **or** SheepShaver's own
  paged boot (G1.e). Recorded **OPEN** with its two candidate vehicles — **not** claimed closeable by
  the unit test.

### Oracle choice (PA-diff; never port a per-access walker — Stop-rule #6)
- **PRIMARY:** PearPC `ppc_effective_to_physical()` (`ppc_mmu.cc`) — small, self-contained
  (hash+BAT+SR in one function), used as an **out-of-process, UNLINKED fixture generator** (avoids
  GPL-link entanglement and the ported-walker anti-pattern).
- **SECONDARY (cross-check):** QEMU `target/ppc/mmu_common.c`.
- **Neither is in-tree (RT-M2-tech):** the oracle is an explicit **external source-and-pin** dependency.
  S1-impl MUST record repo + file + full SHA in the fixture-generation comment (backport-hygiene). **Do
  NOT generate any fixture in Task-0** (that is a write). License: PearPC GPLv2 / QEMU GPLv2 — both
  used UNLINKED as fixture generators, so no link propagation.

### I/O contract
- **Input row (rev-2, AD-2 amended):** `(SR[16] uint32, BAT[16] uint32, HIGH_BAT[16] uint32, SDR1
  uint32, EA uint32)` — the `HIGH_BAT[16]` (SPR 560–575) cells are MANDATORY per AD-2: the rev-1 contract
  `(SR, BAT[16], SDR1, EA)` is **insufficient** (cannot express the NK's high-BAT mappings) and is
  superseded. The reference oracle (PearPC `ppc_effective_to_physical`) already models the full BAT set,
  so the oracle side is unaffected; our side must first gain high-BAT state (AD-2 impl scope).
- **Output:** `PA uint32` **plus** a fault/resolve flag (a row may legally fault).
- **Equal-PA predicate:** `our_translate(row) == oracle_PA(row)` for every row; permission/fault rows
  assert the **same fault-vs-resolve outcome** (not only the PA). The host-address-backing check (a bare
  access lands on the oracle's guest physical page) is part of **G1.a-under-JIT**, owed to the harness or
  boot — NOT this pure test.

### The battery
1. **Two-context** — the `mtsrin`@0x50315290 shape: the SAME EA under two SR programmings yields two
   distinct PAs (identity-only FAILS by construction → Stop-rule #2 enforced).
2. **COARSE-BAT** — the 256 MB-class block program (the expected fast-path-safe case).
3. **Adversarial-FINE** — dense 4 KB hashed-PTEs with mixed per-page perms over a JIT-covered region.
   If the `CanCreateHostMappingForGuestPages`-equivalent feasibility check fails pervasively on (3) AND
   the NK is later shown (G1.e) to depend on (3), S1 re-bands to softmmu.

### What it retires vs does not
Retires the FINE-falsifier as a **translator-correctness** property across coarse AND fine inputs — a
**conditional-close**. It does **NOT** prove the live NK only uses coarse mappings — that is G1.e on the
real SheepShaver newworld boot under S1's own paged path.

### File location + Makefile pre-wire
`SheepShaver/src/machine/test_paged_mmu.cpp` + a Makefile target pre-wired by the coordinator at
impl-kickoff. **Note:** the machine harness currently links no `kpx_cpu`; the **G1.a-under-JIT** vehicle
may require a NEW harness target (RT-C2). Build-ready confirmed at G1.d.

---

## Owed-falsifiers carried forward (by design — NOT Task-0 residues)

| Owed item | Owed to | Why not closeable in Task-0 |
|---|---|---|
| Empirical `make e2e` paravirtual A/B byte-identical + soak + bench | **G1.c** | boots/builds forbidden in recon; Q-S1.1 cites the field set + structural unreachability per PASS-criterion #1 |
| G1.a-under-JIT (bare access lands on remapped PA under the JIT) | **new `kpx_cpu`-linked harness OR G1.e** | the `machine/` harness links no `kpx_cpu` (RT-C2) |
| Live FINE residual (HTAB PTE density / per-page perm diversity on the real mapping) | **G1.e** (real paged boot) | QEMU rig stalls pre-NK-MMU-install; live observation unavailable (Discriminator-A Evidence B) |
| Oracle source-and-pin (PearPC/QEMU repo+file+SHA) | **S1-impl** | sourcing + pinning a fixture is a write |
| **AD-2: high-BAT (SPR 560–575) state model + `mtspr` capture + oracle-contract extension** | **S1-impl gating first step** | rev-1 missed it; `bat[16]`/`(SR,BAT,SDR1,EA)` can't express high BATs; MUST land before the window bet |
| **AD-1: live 4 KB-PTE / high-BAT coverage of JIT-fast-path RAM/ROM (FINE risk, re-banded to carried residue)** | **G1.e** (real paged boot) + the FINE oracle battery | live density unmeasured + table-driven; walker is DEFAULT until measured |

---

## Red-team record (rev-2 — three reviewers; binding amendments folded)

- **SPEC-compliance reviewer: UPHOLD-GREEN** (rev-1) — taxonomy honestly applied for the two flagged
  "honesty" items; 2 doc-hygiene fixes (clarify the Q-S1.3 "whole-parcel scan" is a bounded single-pass
  opcode-31 byte filter, NOT the prohibited unbounded/recursive disasm; route the RAMSize≤0x40000000
  guard into the owed-falsifiers table — DONE).
- **TECHNICAL reviewer: CLAIMS-HOLD** — independently re-disassembled the md5-verified parcel and
  reproduced the exact tlbie (7) + tlbsync (6) addresses, MEM_BULK-uncompiled, NATMEM disjointness, the
  no-`kpx_cpu` harness. Cosmetic drift only (mtsrin instr at `0x5031529c` vs loop-head `0x50315290`;
  path-prefix abbreviations).
- **ADVERSARY reviewer: GREEN-IS-OPTIMISTIC-BUT-OK → two FALSIFICATIONS (binding):**
  - **AD-1** — "HTAB is residual" FALSIFIED: a live 4 KB PTE engine exists (`0x50319af8`/`0x50319b28`,
    V-bit set). Folded into Q-S1.3; FINE re-banded from conditional-close to carried residue.
  - **AD-2** — oracle/state contract FALSIFIED: NK programs high BATs SPR 560–575 (24×) that SS drops
    (`ppc-execute.cpp:1651`) and `(SR,BAT[16],SDR1,EA)` can't express. Folded into Q-S1.3 + the SPEC I/O
    contract (now includes `HIGH_BAT[16]`); added to owed-falsifiers as the S1-impl gating first step.
  - Attacks 3 (GREEN-by-deferral) + 4 (vm_remap on AS, 256 MB-aligned RW data) SURVIVE.
- **Coordinator disposition:** re-band the verdict **GREEN-PASS → RESIDUE-PASS** (the anticipated
  FINE/high-BAT residue). Window remains the *preferred* mechanism; the **walker/softmmu is the
  DEFAULT** until the FINE/high-BAT coverage is measured. S1-impl's gating first step is the high-BAT
  model + oracle-contract fix + the measurement that earns the window. One-iteration rule: this fold IS
  the single bounded re-pin; no further adversary round on rev-2.

---

## Self-review

Recon-only: no `SheepShaver/src/**` written, no builds, no boots, ZERO QEMU boots. Every code claim
cites file:line re-verified this session (drifts noted: `config` path is `MacOSX/config/` not
`Unix/Darwin/`; `DIRECT_ADDRESSING` `#define` at `sysdeps.h:96` not `:95`). All three blocking
questions pinned affirmatively → **GREEN-PASS**. Stop-rules honored: #2 (identity-only FAILS G1.a-pure
by construction), #6 (oracle = PA-diff only), #8 (walker kept live), #9 (no MEM_BULK/arena cited). One
bounded non-blocking caveat (RAMSize > 1 GiB) recorded in Q-S1.2(2b) as an impl-guard, not a residue.
The "13× tlbie" charter figure refined to 7 tlbie + 6 tlbsync (precision, not falsification).
Discriminator-A NOT re-run; Route A + program shape NOT relitigated.
