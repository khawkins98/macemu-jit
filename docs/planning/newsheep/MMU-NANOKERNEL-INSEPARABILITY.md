# Why the NewWorld paged MMU is inseparable from the NanoKernel (S1 ⟂ S3)

> **Status:** ARCHITECTURAL FINDING (2026-06-15), load-bearing for program sequencing.
> **One line:** A live, correct paged MMU only exists once the **real NanoKernel runs its
> supervisor init** — and running that init *is* the S3 two-supervisor reconciliation. So S1's live
> MMU cannot be built or validated independently of S3; they are the same problem. The most S1 can
> deliver standalone is the translation **mechanism** (Task A) + a **static** derivation of what the
> NK would install. **Live** validation is owed to S3.
>
> **Why this doc exists:** during 2026-06-15, three independent explorations — the window de-risk, the
> window-vs-softmmu build-order red-team, and the softmmu-first harvest plan — *each separately
> rediscovered* this. It is written down so a fourth attempt at a standalone S1 live-MMU is not made.
> All file:line are `[STATIC-CAPSTONE]` / source-verified; re-verify before relying (drift ±1–5).

## The reasoning, step by step

**1. A paged MMU is meaningless without something that programs the translation tables.**
PowerPC classic translation resolves an EA→PA only through state the supervisor must install: the 16
segment registers (`mtsr`/`mtsrin`), the 8 BAT pairs (`mtspr {I,D}BATxx`), `SDR1` (the hashed page
table base/mask), and the HTAB itself (PTEs). With those zeroed, every translated access faults. A
"paged MMU" is therefore only as real as the code that fills these in.

**2. On a real NewWorld Mac that code is the NanoKernel's supervisor init — and SheepShaver does NOT
run it.** The current NewWorld boot runs the `SS_NW_TRAMPOLINE` **output-forge**, not the producer:
- `sheepshaver_glue.cpp:2743` — *"SheepShaver runs no Trampoline."* The register-fixup forge is
  `rom_patches.cpp:716`.
- `sheepshaver_glue.cpp:2806` — `SDR1` is assigned **in C** (to `htab_base`); the HTAB is a **zeroed**
  64 KB region (`:2805`), `HTABMASK = 0`.
- `sheepshaver_glue.cpp:563`/`:566` — the segment registers are reset to **0** and **never
  programmed** by guest code in the boot path.
- `sheepshaver_glue.cpp:2918` — the boot `MSR` is forged to `0x7072` (IR=1, **DR=1**) — translation is
  nominally "on" from instruction one, against the empty/forged tables.

The forge exists precisely *because* the real install does not run; it hand-writes the *outputs* so the
boot can limp forward under SheepShaver's V=P (`DIRECT_ADDRESSING`, host = `NATMEM_OFFSET + guest`)
model.

**3. Consequence: there is no live, correct `(SR/BAT/SDR1, EA→PA)` map anywhere in SheepShaver's
current execution.** The frontier the M8→M17 arc reached (EXT body `0x50314880`) is reached with
**forged** supervisor state, not via any MMU-install flow. Therefore:
- A **softmmu** wired at the interpreter chokepoint would translate against the zeroed forged tables
  (`SR=0`, `BAT=0`, empty HTAB) → **fault on essentially every guest RAM/ROM access** → kill the one
  working boot. (Verified: softmmu-first adversary, 2026-06-15.)
- A **window** (Dolphin-style shadow remap of NATMEM at `mtspr`-time) would have **nothing real to
  mirror** — the `mtspr SR/BAT/SDR1` writes that would trigger it never happen (the C forge does them).
- "Harvesting" the live map from today's boot would capture only the **forged** `SDR1`/`SR=0`/no-PTE
  state — a fabricated map, not the NK's real one.

**4. To get a real map, the real NanoKernel install must run — and that runs in a regime SheepShaver
structurally cannot host today.** The NK install is genuine paged-supervisor code:
- NK entry `0x50310000`: `mfmsr` → test `MSR[DR]` → `mtspr SRR0/SRR1` → `rfi` @`0x5031003c` **into
  translated supervisor mode**.
- The SR program (`mtsr 0..15` unrolled @`0x503104b4`) + the per-context `mtsrin` loop @`0x50315290`
  (256 MB/segment/iter), the BAT descriptor program @`0x503152c4`, `SDR1` install @`0x50310604`, and
  the live 4 KB PTE insert/invalidate engine @`0x50319af8` (HTAB stores + `tlbie`).
- Gating-Task-0 **Q1**: the real NanoKernel-v02.27 is a **permanently-resident paged supervisor with
  no handoff boundary** — it does not "set up and hand off"; it *owns* the live supervisor regime.

SheepShaver's paravirtual core is V=P with a synthetic supervisor surface; it cannot run a real,
resident, translated-mode PPC supervisor without reconciling who owns the 68k/exception/scheduler
supervisor state. **That reconciliation is the definition of S3.**

**5. The identity that closes the argument.**
> Running the NK MMU-install  ⟺  hosting the real NK as a resident supervisor  ⟺  **S3**.
> A live, correct paged MMU  **requires**  running the NK MMU-install.
> ∴ A live, correct paged MMU  ⟺  **S3.**

S1's live MMU and S3 are not sequential-but-separable; the live MMU is a *property that only exists
once S3 runs*. There is no intermediate boot in which the MMU is live but the NK is not.

## What this means for the program (corollaries)

- **S1's standalone ceiling is mechanism + static data, not a live MMU.** Deliverable and DONE:
  (a) the translation **mechanism** — `paged_mmu_translate()` + its oracle test (Task A); (b) the
  **static** derivation of what the NK *would* install — `FINDINGS-s1-mmu-constants.md` (HTABMASK
  closed-form confirmed; the 8 BAT descriptor *values* are RAMSize-derived runtime data → residue).
- **The window's coverage predicate (DBAT covers RAM+ROM) is retired only at G1.e, which only fires
  under S3.** Static analysis confirms the *mechanism* is DBAT-capable; the *numeric* answer needs the
  real install to run.
- **Build order between window and softmmu was a non-question.** Neither is buildable/validatable
  before S3, so debating which to build first (the 2026-06-15 red-team) resolved to "build neither
  yet." Both plans are preserved DEFERRED (`…s1-taskB-window-build.md`, `…s1-softmmu-first.md`) with
  their reusable analysis intact for the S3 era.
- **This is the original `S1 → S3 → S4` sequencing, now proven from the code** rather than assumed in
  the program plan. It strengthens, it does not change, the charter.

## Standing guard (for future agents / the S3 planner)

1. **Do NOT attempt a standalone S1 live MMU (window or softmmu) before S3.** There is no live map to
   translate, and arming translation against the forge kills the boot. Three independent attempts in
   one night each re-derived this; this doc is the kill-switch for a fourth.
2. **When S3 is opened, the live MMU comes *with* it, not after it.** Its own deep Task-0 should treat
   "the NK programs SR/BAT/SDR1 and we host the resulting regime" and "a paged MMU exists" as the same
   deliverable. The window vs softmmu choice is then a real (and measurable) decision, made against the
   *actual* NK-installed tables — at which point the deferred plans + the static constants + the
   committed `paged_mmu_translate()` core all become directly usable.
3. **The honest verdict to carry:** S1 mechanism = DONE; S1 live MMU = an S3 sub-deliverable, not an
   S1 gap.

## Source anchors (verify before relying)

- Forge / no-install: `sheepshaver_glue.cpp:2743/2806/2805/2918/563/566`, `rom_patches.cpp:716`.
- NK install path: `0x50310000` entry / `rfi`@`0x5031003c` / SR-unroll `0x503104b4` / `mtsrin`
  `0x50315290` / BAT `0x503152c4` / `SDR1` `0x50310604` / PTE engine `0x50319af8`
  (md5-verified parcel `61c176e90b6365e84e5c660d703e56af`).
- Q1 resident-supervisor / no-handoff: `FINDINGS-trampoline-re.md` "SS_M18 gating Task-0".
- Forge-state confirmation (M16): `GLOSSARY.md` lineage + `M16-FINDINGS-oracle-forge.md`.
- The three 2026-06-15 re-derivations: `FINDINGS-s1-window-derisk.md` rev-2,
  `2026-06-15-ss-m18-s1-taskB-window-build.md` rev-2 Red-team record,
  `2026-06-15-ss-m18-s1-softmmu-first.md` rev-2 Red-team record, `FINDINGS-s1-mmu-constants.md`.
