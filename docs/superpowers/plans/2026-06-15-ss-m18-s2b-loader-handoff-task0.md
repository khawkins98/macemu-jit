# SS_M18 Stage 2b — Trampoline loader + OF-CI handoff + /mmu: DEEP Task-0 recon (BINDING; gates the S2b-impl milestone)

> **Status:** rev-1 (2026-06-15) — red-team FOLDED (PROCESS + TECHNICAL + ADVERSARY, all GO-WITH-FIXES, no
> BLOCK). Five binding amendments folded (see "Red-team record" + inline ★rev-1 marks): (1) **CRITICAL** —
> `decode_parcels` does NOT surface `MacOS.elf` (it processes only `'rom '` parcels into a flat
> `ROMBaseHost`); the asset-free path is NEW code with wrong-bytes risk → **default to staging the
> md5-verified 9.0.1 `MacOS.elf` asset**; (2) Q-S2b.3 must pin the guest↔host CHRP-array marshalling + `ctx`
> binding + guest-callable r5 stub (a hidden G2b.a prerequisite — `of_ci_callback` is 2-arg/host-cell, NOT
> directly r5-wireable); (3) Q-S2b.4 caps at **RESIDUE-PASS pre-S1** (inherits S1 via the `/mmu` stub) +
> bind the predicate to `of_ci_unresolved_count()==0` sampled at the handoff PC; (4) the post-handoff NK
> fault is **EXPECTED pre-S1/S3** (severed EXT path + no live MMU) — not a Q-S2b.4 falsification; (5) re-point
> the `ppc-cpu.cpp:1991` env-gate cite to the real glue precedents. This plan is recon + design ONLY. It pins
> S2b's four blocking questions (loader source/placement, CHRP entry ABI + launch seam, OF-CI wiring,
> NanoKernelEntry handoff) and SPECs the loader+handoff acceptance; it writes NO `SheepShaver/src/**`.
> S2b implementation (loader-gating `rom_patches.cpp` `PatchROM_NW_trampoline`, the `sheepshaver_glue.cpp`
> launch seam, wiring the committed `of_ci_callback`) is the SEPARATE S2b-impl milestone, gated on these
> answers AND on S1 landing (acceptance serialized behind S1) AND coordinated with S3-impl (file-ownership
> collision, below).
>
> **Scope law (carried from the program):** Route A is SETTLED (`FINDINGS-trampoline-re.md` Q0-E) and NOT
> relitigated; the program shape (S1→S3→S4 critical path; S2a parallel/boot-disjoint; **S2b serialized
> behind S1**) is NOT relitigated; the S2a/S2b split is SETTLED (program rev-4 B6). The CHRP entry ABI
> (`r5`=OF-CI callback, `r2`=0x1001e8, entry `0x20f078`) and the program shape are SETTLED inputs — this
> Task-0 operationalizes S2b's *own* loader/handoff recon on top of them.

## What S2b is (binding one-paragraph frame)

S2b is the **loader that delivers control to the NanoKernel**. It (1) loads the real, md5-verified
Trampoline `MacOS.elf` (`ET_EXEC`, 2 `PT_LOAD` @ vaddr data `0x100000` / exec `0x200000`) into guest
memory under the gate; (2) sets the CHRP entry ABI (`r5`=OF-CI callback, `r2`=`0x1001e8` after entry,
`r3`/`r4` per the trace) and jumps to entry `0x20f078` from the `sheepshaver_glue.cpp` launch seam,
behind `SS_M18_TRAMPOLINE`; (3) wires the **committed, inert** `of_ci_callback`
(`machine/openfirmware_ci.cpp` + `include/openfirmware_ci.h`, S2a-impl `14c9384e`+`6c892840`, currently
compiled by the `machine/` unit-test Makefile but NOT linked into the binary) as the `r5` callback target,
so the Trampoline's bounded OF surface (21 direct services + `call-method` + the 3-word `interpret` shim)
resolves against the synthesized Core99 DT; and (4) lets the Trampoline complete its DT walk + `/mmu`
claim/translate/map page-map build and reach **`NanoKernelEntry` with 0 unresolved OF calls** — the point
at which control is handed to the real NK and S3's "the NK install runs" can begin. **S2b's win is
REACHING the NK; it does not run the NK install** (that is S3). The current `SS_NW_TRAMPOLINE`
register-fixup forge in `rom_patches.cpp` (`PatchROM_NW_trampoline`, the entry-vector synthesis at the
mirror) becomes **loader-gated** — disabled when S2b's real loader runs. **S2b Task-0's deliverable is the
RECON that pins the loader source + placement + launch seam + handoff contract — NOT S2b code.**

## Dependency-graph reconciliation (read before the goal)

- **S2b acceptance is serialized behind S1** (program blocking table): the `/mmu` `claim`/`translate`/`map`
  call-method backend is owed to **S1/S3's live paged MMU**. Pre-S1, `/mmu` is a **recording stub** (logs
  the claim/translate/map requests; build + unit only; flagged **NON-ACCEPTANCE** per Stop-rule #7). S2b
  "build-complete on the stub" ≠ S2b PASS. **S2b PASS requires the post-S1 real-`/mmu` handoff.**
- **S2b consumes the committed S2a-impl artifact.** `of_ci_callback` + the Core99 DT model exist and are
  unit-tested (inert); S2b's job is to **WIRE** them into the boot (link them into the binary behind the
  gate + hand the callback pointer in `r5`), not to re-author them. S2a's named residues land here:
  `r3`/`r4` at CHRP entry (S2a injected `r5` only), the `interrupt-map`/`-mask` tuple cell-shape +
  §5-Q8 PIC input numbers (S2a flagged-residue-to-S2b), and the +8/+2 dynamic DT query names
  (→ first integration boot).
- **S2b reaches, S3 runs.** The handoff (Q-S2b.4) is the seam between the two milestones: S2b's success
  predicate is "Trampoline reaches `NanoKernelEntry` with 0 unresolved OF calls"; S3-impl's start predicate
  is "the NK install runs under `SS_M18_NK_SUPERVISOR`." The probe-boot exposure (the QEMU rig stalls in
  OF→OS handoff PRE-NK-install, `FINDINGS-discriminator-a.md` Evidence B / `FINDINGS-s3-two-supervisor.md`
  §5) is precisely the gap S2b closes inside SheepShaver: **S2b IS the in-tree path that gets past that
  stall to the NK.**

## S2b-Task-0 item-0 — the committed inert OF-CI artifact (precondition input)

Re-verify at recon: `SheepShaver/src/machine/openfirmware_ci.cpp` + `SheepShaver/src/include/openfirmware_ci.h`
exist (S2a-impl `14c9384e` + fixes `6c892840`), compiled by `machine/Makefile` `TESTS` (the standalone
`test_openfirmware_ci.cpp` target), **NOT linked into the SheepShaver binary**. The DT model carries the
scaffolding nodes (`/chosen` `/aliases` `/options` `/rtas` `/cpus` `/rom/macos`) + the ADV-1 finddevice
binding (component-wise, unit-address-insensitive matching; SHORT static spellings). S2b folds this as the
artifact-to-wire; the recon CONFIRMS the of_ci_callback entry signature is callable from the launch seam.

## Goal (PASS-vs-DIAGNOSTIC)

**PASS (the Task-0 deliverable — falsifiable).** S2b's four blocking questions are closed in writing,
each evidence-tagged, AND the loader+handoff acceptance is SPEC'd to an I/O-contract level, in a committed
addendum (suggested `FINDINGS-s2b-loader-handoff.md`). This is a **DESIGN + contract Task-0**: the
deliverable is the loader-source decision, the placement/launch-seam contract, the OF-CI wiring contract,
and the handoff contract — the actual loader + launch seam is BUILT in S2b-impl. Task-0 PASSES when all of:

1. **Q-S2b.1 answered** — the **loader source + placement** is pinned: where `MacOS.elf` comes from (a ROM
   parcel extract via the existing `rom_decode.hpp` parcels-decode path vs the staged dump vs an asset) AND
   how the 2 `PT_LOAD` segments are placed at their ELF vaddrs (data `0x100000` / exec `0x200000`) into
   guest memory under the gate, with the PIC self-reloc stub (entry `0x20f078`) accounted.
2. **Q-S2b.2 answered** — the **CHRP entry ABI + launch seam** is pinned: `r5`=OF-CI callback CONFIRMED
   (carried from S2a); `r3`/`r4` CONFIRMED against the trace OR carried as the named S2a residue resolved
   here (NEVER a guessed ABI); `r2`=`0x1001e8` after entry; the jump to `0x20f078` is located at a concrete
   seam in `sheepshaver_glue.cpp`, gated behind `SS_M18_TRAMPOLINE ∧ MachineProfileIsNewWorld()`.
3. **Q-S2b.3 answered** — the **OF-CI wiring** is pinned: the committed `of_ci_callback` is wired as the
   `r5` callback target (linked into the binary behind the gate); the call-method backend ownership map is
   pinned — `/mmu` claim/translate/map → **S1/S3 (recording stub pre-S1, NON-ACCEPTANCE)**; disk
   `read-blocks`/`write-blocks`/`block-size` → **S4 (stub/deferred)**; the rest (DT queries, display no-op,
   RTAS stub, `interpret` shim) **resolve now**.
4. **Q-S2b.4 answered** — the **handoff** is pinned: what `NanoKernelEntry` is (the address/mechanism the
   Trampoline jumps to after its DT walk + `/mmu` build), the falsifiable "reaches `NanoKernelEntry` with
   0 unresolved OF calls" predicate, and how S2b's success hands to S3's "the NK install runs" (the seam
   between the two milestones), with the probe-boot OF→OS-stall exposure named.
5. **Gate-item-0 (parcel provenance):** NK-v02.27 md5 `61c176e90b6365e84e5c660d703e56af` (105280 B) +
   canonical ROM md5 `66210b4f71df8a580eb175f52b9d0f88` re-verified; the `MacOS.elf` source artifact's
   identity recorded (94144 B in `/tmp/newsheep/dump-9.0.1/`, the same md5-verified parcel set).

**PASS taxonomy (the floor).** The close-out MUST state which it is:
- **GREEN-PASS** = all four questions pinned with affirmative closure (incl. a concrete loader source +
  launch seam + a real handoff contract) → S2b-impl may start its **build/unit** work (acceptance still
  serialized behind S1).
- **RESIDUE-PASS** = ≥1 question closed with a conservative residue → a finding, not a green light. A
  residue on Q-S2b.1, .2, or .4 BLOCKS S2b-impl start. A Q-S2b.3 `/mmu`-stub residue is the **expected**
  case (the real backend is owed to S1) and does NOT block S2b's loader/launch build — but it BLOCKS S2b
  PASS (the stub is NON-ACCEPTANCE; full PASS needs the post-S1 real-`/mmu` handoff).
- **LOADER-INFEASIBILITY-VERDICT** = positive evidence that `MacOS.elf` is neither extractable from the
  in-tree/staged ROM nor placeable at its vaddrs within the guest aperture under the gate → a reportable
  program-level blocker, recorded with evidence, NOT a license to forge the Trampoline's outputs
  (Stop-rule #1). Budget-exhaustion yields LOADER-INFEASIBILITY-**UNKNOWN**, never the VERDICT.

**DIAGNOSTIC (recorded, NEVER a gate):** how far a gated probe boot gets toward `NanoKernelEntry`; the
count of OF calls resolved vs the stub set; which call-method backends S2b stubs vs S1/S4 own; the
`nw-northstar` observe line; whether the active project ROM (1.1, md5 `e0fc03faa589ee066c411b4603e0ac89`)
carries a `MacOS.elf` parcel compatible with the RE'd 9.0.1 one. Reaching the NK is the win, NOT Finder.

## Authoritative inputs

| Doc / source | What it fixes for this Task-0 |
|---|---|
| `docs/superpowers/plans/2026-06-14-ss-m18-trampoline-lle-program.md` — "Stage 2" (S2b section) | The S2b charter (the goal, gates G2b.a/.d/.e, env-gate `SS_M18_TRAMPOLINE`, file ownership = `rom_patches.cpp` loader-gating + `sheepshaver_glue.cpp` launch seam). **External doc.** Route A + program shape + S2a/S2b split NOT relitigated. |
| `docs/planning/newsheep/FINDINGS-trampoline-re.md` "SS_M18 gating Task-0" + Q0-A/B/E + SS-integration sketch | `MacOS.elf` = `ET_EXEC`, 2 `PT_LOAD` @ vaddr `0x100000`/`0x200000`, entry `0x20f078` (PIC self-reloc stub), OF entry at `[r2-0x60]`, `r2=0x1001e8`, indirect glue `0x21024c`; OpenBIOS proves launch at ELF vaddr; the 177/177+24/24 OF inventory; the `SS_NW_TRAMPOLINE`→Route-A disposition table. **All addresses re-verify.** |
| `docs/planning/newsheep/FINDINGS-s2a-ofci-dt.md` (RESIDUE-PASS) | The S2a OF-CI dispatch contract; the committed inert `openfirmware_ci.{cpp,h}` to WIRE; the Core99 DT read-set; the ADV-1 finddevice constraint; the **residues handed to S2b** (Q-S2a.3 tuple shape + §5-Q8 inputs; r3/r4; +8/+2 dynamic names). |
| `docs/planning/newsheep/FINDINGS-s3-two-supervisor.md` (GREEN-PASS, REPLACE) | What "reaching `NanoKernelEntry`" must hand to S3: the NK install runs under `SS_M18_NK_SUPERVISOR`; the file-ownership collision (`rom_patches.cpp`/`glue` are S3-impl's RETIRE surface too — §"File ownership"); the OF→OS-stall probe exposure (§5). |
| `docs/superpowers/plans/2026-06-14-ss-m18-s2a-ofci-dt-task0.md` + `…2026-06-15-ss-m18-s3-two-supervisor-task0.md` | The house-template EXEMPLARS (structure copied). **External docs.** |
| `docs/MILESTONE-WORKFLOW.md` §2/§4/§6/§6c | The machine; falsifiable-in-advance gates; env-on-first/flip-LAST/revert-on-red; file ownership; QEMU-before-RE + address-oracle LAW. **External doc.** |

## Codebase facts (PINNED 2026-06-15 — re-verify each at recon AND impl; drift ±1–5)

- **The committed OF-CI artifact is REAL + inert (verified):** `SheepShaver/src/machine/openfirmware_ci.cpp`
  + `SheepShaver/src/include/openfirmware_ci.h` exist; compiled by `machine/Makefile`'s `TESTS` target
  (`test_openfirmware_ci.cpp`), NOT linked into the SheepShaver binary. S2b links it behind the gate.
- **★rev-1 CORRECTED — the parcels-decode path does NOT surface `MacOS.elf`:** `rom_patches.cpp:46`
  includes `rom_decode.hpp`; `decode_parcels()` (`rom_decode.hpp:86-100`) walks the `prcl` chain but
  processes **only** parcels of type `FOURCC('r','o','m',' ')` (`:93`), LZSS-decompressing each into a
  single flat `dest` (= `ROMBaseHost`, via `decode_rom_image:168`). **`MacOS.elf` is the top-level
  Trampoline ELF — a SIBLING of the `Parcels` container in the tbxi dump, NOT a `'rom '` parcel — so the
  existing path NEVER writes it into `ROMBaseHost` and returns no per-parcel extent to locate it.** The
  "asset-free extract via decode_parcels" idea is therefore NEW extraction code, not an existing path.
  **★rev-1 DEFAULT FLIP:** stage the md5-verified 9.0.1 `MacOS.elf` (94144 B, `66210b4f…` parcel set) as an
  ASSET; ROM-extraction-in-tree is a later optimization gated on a parcel-identity check. **Wrong-bytes
  risk reinforces this:** the active boot ROM is the 1.1 ROM `e0fc03faa589ee066c411b4603e0ac89`, NOT the
  RE'd 9.0.1 — extracting from the loaded ROM could yield an un-characterized Trampoline. Q-S2b.1 weighs
  asset-staging (default) vs in-tree extraction (needs new code + identity guard).
- **The current forge (verified):** `rom_patches.cpp` `PatchROM_NW_trampoline` (~`:716`+, `SS_NW_TRAMPOLINE`)
  is the register-fixup trampoline + entry-vector synthesis at mirror `0x50429b40` (48-word budget). S2b
  makes it **loader-gated** — OFF (byte-identical to today) when `SS_M18_TRAMPOLINE` is OFF; bypassed when
  the real loader runs. **This is the S3-impl collision surface** (S3 RETIREs the reg-fixup forge `:716`).
- **The launch seam (verified, re-verify):** `kpx_cpu/sheepshaver_glue.cpp` holds the newworld boot/entry
  machinery (Execute68k pair `:1504-1505`/`:3140-3143`, `execute_68k:1461`, `interrupt():1002`,
  `deliver_pending_dec_exception():1122`). The CHRP-entry jump to `0x20f078` is sited here. **This is the
  S3-impl collision surface** (S3 RETIREs `deliver_pending_dec_exception`/`g_exc_entry_table`/Execute68k
  pair + forge `:2806/2804/2918`).
- **CHRP entry contract (carried, re-verify):** `r5`=OF-CI callback stashed at `[r2-0x60]`; `r2=0x1001e8`;
  entry `0x20f078`; glue `0x21024c`; `MacOS.elf` `ET_EXEC`, exec `0x200000` (filesz `0x10260`) / data
  `0x100000` (filesz `0x6bc0`, memsz `0x19920`); total < `0x210260`, inside the guest RAM aperture. `r3`/`r4`
  NOT pinned in the findings → Q-S2b.2 (the S2a residue resolved here).
- **`NanoKernelEntry` (carried, re-verify — DO NOT disasm the parcel to over-pin):** the Trampoline, after
  its DT walk + `/mmu` build, jumps to the NK; the NK run base is `0x50310000` (entry `mfmsr`→test
  MSR[DR]→`mtspr SRR0/SRR1`→`rfi`@`0x5031003c`). What address the Trampoline computes for `NanoKernelEntry`
  and how (from `/rom/macos` `AAPL,toolbox-parcels`? from the ROM component table?) is the Q-S2b.4 pin —
  bounded to the handoff window, not a full parcel walk.
- **Parcel provenance (gate-item-0):** NK-v02.27 md5 `61c176e90b6365e84e5c660d703e56af` (105280 B);
  canonical ROM md5 `66210b4f71df8a580eb175f52b9d0f88`; `MacOS.elf` 94144 B in
  `/tmp/newsheep/dump-9.0.1/`. Re-extract via `tbxi` (venv `/tmp/newsheep/venv`) if /tmp evaporated.

## Blocking-answer table (which S2b-impl task blocks on which Task-0 question)

> Residue-disposition map, NOT a start-order license. A question closed with a *blocking* residue → its
> dependent S2b-impl task does NOT start until the coordinator re-scopes. The Q-S2b.3 `/mmu`-stub residue
> is the *expected* case and is handed to S1 rather than blocking S2b's loader/launch build.

| S2b-impl task (future plan) | Blocked by | Residue disposition (conservative; defaults to the harder path) |
|---|---|---|
| ELF loader: place the 2 `PT_LOAD` at vaddr `0x100000`/`0x200000` from the parcel source under the gate | **Q-S2b.1** | `MacOS.elf` not extractable from the loaded ROM / not placeable in the aperture → "loader-source-UNKNOWN → LOADER-INFEASIBILITY" (with evidence) OR "needs-staged-asset → asset-manifest entry". RESIDUE-PASS → blocks impl. |
| CHRP launch: set `r5`/`r2`/`r3`/`r4`, jump to `0x20f078` from the glue seam, gated `SS_M18_TRAMPOLINE` | **Q-S2b.2** | r3/r4 unresolved → "CHRP-ABI-residue → confirm at first gated boot; do NOT guess". Blocks the launch claim, not the loader placement. |
| **★rev-1 — Guest-callable r5 marshalling shim** (single-arg PPC entry @ guest addr; bind `ctx`; marshal guest-32-BE CHRP array ↔ host `of_cell[]` incl. pointer translation) | **Q-S2b.3** | Hidden G2b.a prerequisite — without it `of_ci_callback` (2-arg/host-cell) cannot be the r5 target. Shim-contract-UNKNOWN → blocks G2b.a. |
| Wire `of_ci_callback` (via the shim above) as the `r5` target; link `openfirmware_ci` into the binary behind the gate | **Q-S2b.3** | A call-method without an owner → "backend-ownership-UNKNOWN"; `/mmu` stub = NON-ACCEPTANCE (expected, → S1); disk → S4. Never claim backend completeness. |
| The handoff: Trampoline reaches `NanoKernelEntry` w/ 0 unresolved OF calls; hand to S3 | **Q-S2b.4** | Handoff address/mechanism unpinned → "handoff-UNKNOWN → boot-path-risk". RESIDUE-PASS → blocks impl. |
| `/mmu` claim/translate/map real backend | **S1 (+ S3 co-land)** | Pre-S1 recording stub (build/unit ONLY, NON-ACCEPTANCE per Stop-rule #7). S2b PASS waits on the real `/mmu`. |
| Loader-gate the existing `PatchROM_NW_trampoline` forge | **Q-S2b.1 + S3-impl coordination** | Collision with S3's forge-retire (`rom_patches.cpp:716`) → serialize (see File ownership). Forge stays byte-identical when the gate is OFF. |
| The whole S2b effort band | **Q-S2b.1 ∧ .4** | Loader source pinned + handoff pinned ⇒ bounded (weeks) for loader/launch; `/mmu` real-backend acceptance ⇒ serialized behind S1. The band is the verdict, not a dig-trigger. |

## Q-S2b detail (the four blocking questions)

**Q-S2b.1 — loader source + placement.** WINDOW: `rom_decode.hpp` parcels-decode path + the
`FINDINGS-trampoline-re.md` ELF facts + the in-tree ROM identity. Decide: does `MacOS.elf` come from the
already-loaded ROM image (`ROMBaseHost`, via `decode_parcels` walking the `'rom '` chain to the Trampoline
parcel) — the preferred, asset-free path — or must it be a staged-dump asset (asset-manifest entry, the
9.0.1 `66210b4f…` extract)? Confirm the 2 `PT_LOAD` placement at their ELF vaddrs is honorable inside the
guest aperture (OpenBIOS proves it). Pin the PIC self-reloc stub behavior at `0x20f078`. **Hard stop:** do
NOT relocate the segments off their ELF vaddrs to "make it fit" (the Trampoline is `ET_EXEC` at fixed
vaddrs; OpenBIOS honors them). **Residue:** "loader-source-UNKNOWN → LOADER-INFEASIBILITY (with evidence)
OR needs-staged-asset."

**Q-S2b.2 — CHRP entry ABI + launch seam.** WINDOW: the `[r2-0x60]`/`r5`/`0x20f078`/`r2=0x1001e8` block +
the glue boot path. `r5`=OF-CI callback is CONFIRMED (S2a). Resolve the S2a `r3`/`r4` residue against the
trace OR carry it as "confirm at first gated boot, inject documented default, NEVER a hidden ABI". Locate
the concrete seam in `sheepshaver_glue.cpp` where the gated jump to `0x20f078` is emitted, behind
`SS_M18_TRAMPOLINE ∧ MachineProfileIsNewWorld()`. **Residue:** "r3/r4-UNKNOWN → first-gated-boot owner".

**Q-S2b.3 — wire `of_ci_callback` + backend ownership.** WINDOW: `openfirmware_ci.h` callback signature +
the program backend map. **★rev-1 — `of_ci_callback` is NOT directly r5-wireable; pin the marshalling
shim.** Signature (`openfirmware_ci.h:101`) is `int of_ci_callback(of_ci_context *ctx, of_cell *array)` —
**two** args, `of_cell = uint64_t` HOST width, `array[0]` = a host `const char*` cast to a cell. The real
CHRP `r5` callback is a **single-arg, guest-callable** entry receiving a **guest** pointer to a **big-endian
32-bit** cell array (strings are guest addresses). So S2b must build a **guest-address trampoline** that
(a) presents a single-arg PPC-callable entry at a guest address for `r5`, (b) binds `ctx`, and (c) marshals
the guest 32-bit-BE CHRP array ↔ host `of_cell[]`, translating every guest string/buffer pointer host↔guest.
**This marshalling shim is load-bearing S2b code and a hidden G2b.a prerequisite** (added to the blocking
table). Pin its I/O contract here, NOT just "link + pass pointer." Then the call-method backend ownership —
`/mmu` claim/translate/map →
**S1/S3 (recording stub pre-S1, NON-ACCEPTANCE)**; disk `read-blocks`/`write-blocks`/`block-size` →
**S4**; DT queries / display no-op / RTAS stub / `interpret` `key?`/`key`/`reset-all` **resolve now**. The
S2a residues (interrupt-map tuple shape + §5-Q8 inputs) are answered by the DT returning the provisional;
their VALUE confirmation is owed to the first integration boot. **Stop-rule #7:** the `/mmu` stub is NOT
acceptance.

**Q-S2b.4 — the handoff.** WINDOW: the Trampoline's tail (post-`/mmu`-build → `NanoKernelEntry` jump) +
the NK entry `0x50310000` + the S3 hand-off contract. Pin: what `NanoKernelEntry` is (the address +
how the Trampoline computes it), the falsifiable predicate, and the seam to S3 ("the NK install runs").
**★rev-1 — bind the predicate to an INSTRUMENT, cap pre-S1, declare the expected fault:**
- **Predicate instrument:** "reaches `NanoKernelEntry` with 0 unresolved OF calls" = `of_ci_unresolved_count()`
  (`openfirmware_ci.h:105`) reads `0` **sampled at the handoff PC**, not asserted narratively.
- **Q-S2b.4 caps at RESIDUE-PASS pre-S1.** It inherits S1's dependency through the `/mmu` stub: the
  Trampoline does REAL `claim`/`translate`/`map` and dereferences the result; a recording stub returning
  identity/plausible claims MAY let it progress to the jump OR it MAY fault on a `translate` before the
  handoff (unknowable statically). GREEN-closure waits on the post-S1 real-`/mmu` boot.
- **The post-handoff NK fault is EXPECTED pre-S1/S3, NOT a falsification.** Per `FINDINGS-s3-two-supervisor.md`
  §BINDING-AMENDMENT, pure REPLACE severs the device-IRQ→NK-EXT path (the shim is S3-T2) and the NK is a
  resident paged supervisor needing the live MMU SS lacks pre-S1. So "reached `NanoKernelEntry`" is
  immediately followed by an NK fault by construction — record it as EXPECTED, do NOT read the first gated
  boot's NK fault as a regression (the same honest-limit move S3 §5 made).
Name the probe-boot exposure: the QEMU rig stalls in OF→OS handoff PRE-NK-install
(`FINDINGS-discriminator-a.md` Evidence B) — S2b is the in-tree path that gets past it; positive
corroboration is owed to S2b-impl's own gated boot. **Hard stop:** do NOT disasm the whole 105280 B
parcel — bound to the handoff window. **Residue:** "handoff-UNKNOWN → boot-path-risk".

## Budgets (caps with WRITTEN residue fallbacks — unbounded disasm killed predecessor agents)

> Caps bind; partial-findings-beat-stalling. Hard cap = the function/instruction/doc-section window.

- **Q-S2b.1 (loader source — `rom_decode.hpp` + ELF facts + ROM identity):** ~45 min static + ≤1 confirm
  (md5/parcel-presence check; NOT a boot). Residue: "loader-source-UNKNOWN → LOADER-INFEASIBILITY/asset".
- **Q-S2b.2 (CHRP ABI + glue seam — re-read findings entry block + locate glue seam):** ~30 min static.
  Residue: "r3/r4-UNKNOWN → first-gated-boot owner".
- **Q-S2b.3 (OF-CI wiring + backend map — `openfirmware_ci.h` + program backend map):** ~30 min static.
  Residue: "backend-ownership-UNKNOWN".
- **Q-S2b.4 (handoff — Trampoline tail window + NK entry):** ~45 min static + ≤2 QEMU boots ONLY IF the
  static window cannot pin the `NanoKernelEntry` computation (INSIDE the ≤4 program cap; the rig is known to
  stall pre-NK-install, so a boot buys the *Trampoline-side* handoff observation, NOT the NK install).
  Residue: "handoff-UNKNOWN → boot-path-risk".
- **QEMU boots: ≤2 (Q-S2b.4 only), INSIDE the ≤4 program cap. NK disasm: bounded to the handoff window
  ONLY.** Spending a boot or a full-parcel disasm beyond these is a Stop-rule violation.
- **Unbounded-read kill-switch:** any question past its window without a verdict STOPS and records the
  conservative residue. Do NOT read the whole parcel or the whole ROM image.

## Gates G2b.a / G2b.d / G2b.e (S2b-IMPL gates restated falsifiable-in-advance; Task-0 ensures each is answerable)

> These run in the future S2b-impl acceptance battery. This Task-0 produces the pinned contracts each
> checks; it does NOT run them.

- **G2b.a — loader vaddr placement + PIC self-reloc stub runs.** Under the gate, the 2 `PT_LOAD` land at
  vaddr `0x100000`/`0x200000`, entry `0x20f078` executes its `mflr/bl .+8` self-reloc, and the launch
  reaches the first OF-CI call through a wrapper (`0x20dbec`/`0x20dcc0`/`0x20ddb4`). Identity/forge-state
  FAILS by construction (the real Trampoline bytes must be present and running).
- **G2b.d — `/mmu` routed to S1's paged MMU.** The `/mmu` `claim`/`translate`/`map` call-method is serviced
  by S1's live paged MMU. **Pre-S1: a recording stub (logs the requests) — build/unit ONLY, marked
  NON-ACCEPTANCE.** G2b.d PASS requires the real S1 `/mmu` backend; "build-complete on the stub" is a
  false-clean (Stop-rule #7).
- **G2b.e — paravirtual byte-identical.** `make test-jit` = 100 (353-vector harness) AND — because S2b
  edits SHARED runtime-gated paths (`rom_patches.cpp` + `sheepshaver_glue.cpp`, NOT genuinely-new files —
  the §6 structural-inertness substitute is NOT available) — **real `make e2e` + multi-run soak**
  (`SS_E2E_RUNS=N` median±CV%) with the gate OFF, byte-identical to today's forge path.

## Env-gate

`SS_M18_TRAMPOLINE` (default **OFF**) ∧ `MachineProfileIsNewWorld()`; selected once at boot (reuse a
profile-gating precedent — **★rev-1: the real precedents are `sheepshaver_glue.cpp:1082/1554/1984/2747`**
and the S3-T1 `NkSupervisorEnabled()` boot-latch in `kpx_cpu/src/cpu/ppc/ppc-cpu.cpp`; never a per-access
branch). When OFF, the existing
`PatchROM_NW_trampoline` forge runs byte-identical to today. **Proof obligation:** "paravirtual provably
unreachable" = `MachineProfileIsNewWorld()==false` audited at the gate site + G2b.e real-`make e2e`
byte-identity. Acceptance: env-on-first (full battery with the gate ON, each G-gate recorded with numbers),
**flip-LAST**, revert-on-red (since S2b edits shared paths, revert = in-task revert of the seam edit).

## File ownership (carried — for the S2b-IMPL milestone, NOT this Task-0 which writes no src)

- **Edits (serialized, strong tier):** `rom_patches.cpp` (`PatchROM_NW_trampoline` becomes loader-gated) +
  `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` (the CHRP-entry launch seam: load `MacOS.elf`, set
  `r5`/`r2`/`r3`/`r4`, jump to `0x20f078`). **Link** the existing `openfirmware_ci.{cpp,h}` into the binary
  behind the gate (Makefile edit).
- **S2b must NOT edit the memory-translation surface (S1's)** — `/mmu` routes to S1 via the injected-callback
  seam, not by S2b touching `paged_mmu.cpp` / the NATMEM layer.
- **★ File-ownership COLLISION with S3-impl (LAW — serialize).** Both `rom_patches.cpp` and
  `sheepshaver_glue.cpp` are **also S3-impl's RETIRE surface** (`FINDINGS-s3-two-supervisor.md` §"File
  ownership": S3 retires the reg-fixup forge `rom_patches.cpp:716`, the `deliver_pending_dec_exception`/
  `g_exc_entry_table`/Execute68k-pair/forge `glue:2806/2804/2918`). S2b and S3-impl **MUST NOT be in flight
  on these two files concurrently.** Serialization rule (coordinator-enforced): **S2b lands FIRST** (S2b is
  the unblocker that reaches the NK; S3 then retires the SS supervisor *under* the reached NK). S2b's
  loader-gating of `PatchROM_NW_trampoline` and S3's forge-retirement of the same function are sequenced —
  S2b gates the forge behind `SS_M18_TRAMPOLINE`; S3 later retires it behind `SS_M18_NK_SUPERVISOR`,
  rebasing on S2b's gate structure. Concurrent edits to either file = STOP, coordinator re-serializes.
- **S2b owns the loader + launch + OF-CI-wiring surface; depends on S1 landed (for `/mmu` acceptance) +
  S2a-impl committed (DONE).**

## Stop-rule (triggers named in advance; one-iteration mechanics)

1. **#1 (program) — Forge the Trampoline's output instead of running it.** STOP (M17 tripwire). S2b RUNS
   the real `MacOS.elf`; it does NOT re-synthesize the entry-vector forge as the answer. Loading
   `MacOS.elf` and letting it run is the whole point.
2. **#7 (program) — S2b PASS on the stub `/mmu`.** "Build-complete on the recording stub" ≠ S2b PASS. The
   `/mmu` backend is owed to S1 (not faked); S2b PASS requires the post-S1 real-`/mmu` handoff (G2b.d
   non-stub). Shipping S2b on the stub is a false-clean.
3. **#3 (program) — Claim a handoff boundary the recon proved fictional / guess `NanoKernelEntry`.** Q-S2b.4
   pins the handoff from the Trampoline tail + NK entry; if unpinnable in budget, it is a residue, NEVER a
   guessed address.
4. **#6 (program) — QEMU-as-address-oracle.** No QEMU MMIO address is a reference value; the rig is a
   *behavioral* oracle for the Trampoline-side handoff only (and is known to stall pre-NK-install).
5. **#5 (program) — Unbounded disasm.** Past the handoff window ⇒ STOP, conservative residue. Do NOT read
   the whole 105280 B parcel or the whole ROM image. Two predecessor agents died here.
6. **Relocate `MacOS.elf` off its ELF vaddrs to "make it fit".** `ET_EXEC` at fixed vaddrs; OpenBIOS honors
   them. If it doesn't fit the aperture under the gate, that is a Q-S2b.1 finding, not a relocation hack.
7. **Edit `rom_patches.cpp` / `sheepshaver_glue.cpp` concurrently with S3-impl.** Serialize (S2b first);
   concurrent in-flight on these files = STOP, coordinator re-serializes.
8. **Start writing the loader / launch seam / OF-CI wiring mid-Task-0.** No `SheepShaver/src/**`; the
   loader is SPEC'd here, BUILT in S2b-impl.
9. **"months/INFEASIBLE is the answer," not a trigger to relitigate Route A** (Stop-rule #8 program).
   Coordinator re-prices.
10. **Proceed to S2b-impl ACCEPTANCE on a `/mmu`-stub build** (vs build/unit only). Acceptance waits on S1.

**One-iteration rule.** A pinned answer falsified within this Task-0 → dated falsification entry in the
addendum → ONE bounded re-pin (≤1 doc/code window; ≤1 boot from the ≤2 Q-S2b.4 reserve, inside the ≤4
program cap) → resume. A SECOND falsification of the same answer ⇒ STOP, re-plan this Task-0.

## Self-review record

Spec coverage: all coordinator inputs consumed (program Stage-2 S2b section + blocking table + file
ownership; `FINDINGS-trampoline-re.md` ELF/CHRP/handoff facts + OF inventory; `FINDINGS-s2a-ofci-dt.md`
committed-artifact + residues-to-S2b; `FINDINGS-s3-two-supervisor.md` handoff target + file collision +
probe exposure; both Task-0 exemplars; the live `rom_decode.hpp`/`rom_patches.cpp`/`glue` facts). Recon +
design ONLY — no `SheepShaver/src/**`. Route A + program shape + S2a/S2b split NOT relitigated.

**Tensions FLAGGED FOR THE RED TEAM (unresolved by me; listed for adversarial review):**
1. **Where does `MacOS.elf` come from — and is it extractable from the ACTIVE ROM?** The active project boot
   ROM is the 1.1 ROM (`e0fc03faa589ee066c411b4603e0ac89`); the RE'd Trampoline is from the staged 9.0.1
   (`66210b4f…`). The asset-free path (extract via `decode_parcels` from `ROMBaseHost`) assumes the loaded
   ROM carries a compatible `MacOS.elf` parcel. If it does NOT, S2b needs a staged-asset path
   (asset-manifest entry) — or a forced 9.0.1-ROM boot. Is "extract from the loaded ROM" the right default,
   or should S2b stage the md5-verified 9.0.1 `MacOS.elf` as an asset to guarantee the RE'd bytes?
2. **Can the loader + handoff be tested WITHOUT the NK install running?** Is S2b PASS legitimately "reaches
   `NanoKernelEntry` with 0 unresolved OF calls" even though the NK then cannot run until S3 lands? I argue
   YES — S2b's contract is *delivery to the NK*, and the QEMU rig's OF→OS stall (`FINDINGS-discriminator-a.md`
   Evidence B) shows this IS the meaningful, separable boundary. But the positive corroboration is owed to
   S2b-impl's own gated boot, and the NK's *acceptance* of the handoff (does it run?) is only observable
   once S3 is on. Red team to confirm the boundary is real and not a false-green where "reached the NK" but
   the NK immediately faults on the two-supervisor collision.
3. **The `rom_patches.cpp`/`glue` file-ownership collision with S3-impl T4.** Both milestones edit the same
   two files. I propose S2b-first serialization (S2b gates the forge; S3 later retires it, rebasing on S2b's
   gate). Is that the right order, or should S2b and S3 co-land the forge disposition in one serialized
   window to avoid a churn cycle (S2b gates → S3 retires → the gate structure thrashes)?
4. **`/mmu` stub vs acceptance.** Is "S2b-impl build/unit on the recording stub, acceptance serialized
   behind S1" the right disposition, or does the loader/launch even *build* meaningfully without a `/mmu`
   that returns plausible page-map results to keep the Trampoline progressing toward `NanoKernelEntry`?
   (I argue the recording stub can return identity/plausible claims to let the Trampoline progress for the
   loader/launch build, with the real translation owed to S1 — but the Trampoline may fault on a bogus
   `translate` before reaching the handoff. Red team to confirm the stub is sufficient to reach the
   handoff, or flag that Q-S2b.4 is itself blocked behind S1.)

No residual self-review tension beyond these four.

## Red-team record (rev-1 FOLD — 2026-06-15; PROCESS + TECHNICAL + ADVERSARY, all GO-WITH-FIXES, no BLOCK)

The plan's scope discipline, budgets, stop-rules, and S2b-first serialization were endorsed unchanged
(adversary explicitly rejected co-landing the forge disposition; S2b-first keeps each milestone
independently revert-on-red). Five binding amendments FOLDED (inline ★rev-1 marks + the status header):

1. **CRITICAL (TECHNICAL + ADVERSARY) — Q-S2b.1 source.** `decode_parcels()` (`rom_decode.hpp:86-100`)
   processes ONLY `'rom '` parcels into a flat `ROMBaseHost`; `MacOS.elf` is the top-level Trampoline ELF
   (sibling of the `Parcels` container), never surfaced by the existing path. The "asset-free extract" was
   NEW code resting on a nonexistent path + carried 1.1-vs-9.0.1 wrong-bytes risk. **FOLDED:** default
   flipped to staging the md5-verified 9.0.1 `MacOS.elf` asset; in-tree extraction demoted to a later
   optimization behind a parcel-identity guard.
2. **MAJOR (TECHNICAL + ADVERSARY) — Q-S2b.3 marshalling shim.** `of_ci_callback(ctx, of_cell*)` is 2-arg,
   host-pointer/64-bit-cell — NOT directly r5-wireable (the real r5 entry is single-arg, guest-callable,
   guest 32-bit-BE array). **FOLDED:** Q-S2b.3 now pins the guest-callable r5 trampoline + `ctx` bind +
   guest↔host array marshalling as load-bearing S2b code + a hidden G2b.a prerequisite (added to the
   blocking table).
3. **MAJOR (PROCESS + ADVERSARY) — Q-S2b.4 caps at RESIDUE-PASS pre-S1 + instrument.** It inherits S1 via
   the `/mmu` stub (the Trampoline dereferences real `translate` results; the stub may fault before the
   handoff). **FOLDED:** Q-S2b.4 capped at RESIDUE-PASS pre-S1; predicate bound to
   `of_ci_unresolved_count()==0` (`openfirmware_ci.h:105`) sampled at the handoff PC.
4. **MAJOR (ADVERSARY) — post-handoff NK fault is EXPECTED.** Pure REPLACE severs device-IRQ→NK-EXT (shim
   is S3-T2) + no live MMU pre-S1, so "reached `NanoKernelEntry`" is immediately followed by an NK fault by
   construction. **FOLDED:** declared EXPECTED, not a Q-S2b.4 falsification.
5. **minor (TECHNICAL) — env-gate cite.** `ppc-cpu.cpp:1991` re-pointed to the real glue precedents
   (`:1082/1554/1984/2747`) + the S3-T1 `NkSupervisorEnabled()` boot-latch.

All other line cites + the inert-artifact / forge / file-collision claims verified accurate against source.
**Verdict: GO-WITH-FIXES folded → S2b Task-0 recon (the four Q-S2b questions) may dispatch.**
