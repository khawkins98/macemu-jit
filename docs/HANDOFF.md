# Project Handoff — resume entry point

> ## > RIGHT NOW (read this first)
> - **Aim:** boot Mac OS 9.2 (NewWorld) by **running the Trampoline producer + NanoKernel**, not forging their outputs (Operation NewSheep; the M8->M17 forge arc is CLOSED).
> - ***** CURRENT FRONTIER (2026-06-15): the real NanoKernel BODY runs and programs its FULL live MMU map -- we are AT the S1 live-`/mmu` re-activation point (GO granted, oracle in hand).** A gated-ON boot (`SS_M18_TRAMPOLINE=1 SS_M18_NK_SUPERVISOR=1 SS_M18_PARCEL_FILE=<compressed prcl>`, 9.0.1 ROM) now runs end-to-end: BootX pre-stage -> real Trampoline (full OF phase: claim/translate/map, interrupt-controller masktable) -> `quiesce`/`exit` -> NK launch stub -> **NK body @`0xF10000`** -> the NK programs its **full SR/BAT/SDR1 map** (139-row oracle: MTSR 0-15, SDR1=`0x1fe0001f`, the real segment battery, the complete DBAT/IBAT program incl. DBAT1 v`0x68000000`->p`0x00f00000`, the MTSRIN per-context loop) -> **SIGSEGV at virtual `0x6806e8c0` INSIDE the `0x68000000` BAT mapping it JUST programmed -- because our SR/BAT writes are JIT/interp NO-OPS.** This is THE wall the whole program was built around: the live `(SR/BAT/SDR1)` paged MMU. Oracle saved: `docs/planning/newsheep/nk-faithful-oracle.log`.
> - **>> ACTIVE MILESTONE = S1 (live paged `/mmu`).** GO granted (user, standing). Inputs READY: the **harvested NK oracle** (the exact map to honor), the **B1 Dolphin shadow-arena standalone module** (`machine/dolphin_bat_arena.*`, 43-check unit-tested, behind `paged_mmu_translate`, inert until wired), the proven SHM-arena spikes (`acd89dce`/`cd7a62b6`), the S1 Task-0 (`docs/superpowers/plans/2026-06-15-ss-m18-s1-task0-live-mmu.md` -- window-vs-softmmu + the full ss-FINDING chain of this session's bringup). **The S1<->S3 integration is the SERIAL single-owned spine.** ** REALITY CHECK (do NOT mistake this for "S1 nearly done"):** what is READY is only the EASY part -- the translation LOGIC (the shadow-arena) + a correctness ORACLE (the 139-row trace: "given these writes, translate correctly"). The HARD, UNPARALLELIZABLE ~60% has NOT started: the SPINE -- (i) INTERCEPT the NK's live `mtsr`/`mtdbat*`/`mtibat*`/`mtspr SDR1` to actually MUTATE live translation (today they are JIT/interp no-ops), (ii) CONSULT the arena on every guest memory access, (iii) without wrecking JIT perf. The oracle says NOTHING about the live interception or the JIT integration. **PRIORITY S1-PREP (suggestion #5, NOT yet landed): spike the JIT fast-path design -- arena-consult vs today's V=P `NATMEM_OFFSET`, BENCHMARKED on synthetic mappings -- BEFORE wiring into the live boot, or S1 integration becomes perf-discovery at the same time.** This is where architecture-first matters most. Ground every step by watchpoint/disasm; settle UNBLOCK-vs-REPRODUCE by evidence not cost.
> - **How we got here (this session -- all gated-OFF + paravirtual byte-identical + e2e-verified):** S2b-impl T1-T5 (loader -> marshalling shim -> launch seam -> OF-CI wiring -> forge loader-gate) + S3-impl T1-T4 (master gate -> EXT-injection shim -> synthetic-supervisor retire -> forge retire) => the real Trampoline runs in-tree; then OF-CI environment fidelity (claim bump-allocator, `/memory`, `/chosen` ihandles, `/mmu` translate-arity, phys-0 RAM backing) => the Trampoline completes its whole OF phase; then the BootX pre-stage (`3446006b`, **NK executes, iNK>0**); then faithful compressed-container staging (`ad8012d6`, **NK body runs + full MMU map**).
> - **OWED follow-ups (NONE rollback-worthy -- tree is clean, builds, test-jit=100, e2e PASS, all 5 fan-out deliverables landed+verified):**
>   1. **S1 impl** (the active milestone) -- the live MMU (window/softmmu per the Task-0), validated against the harvested oracle + the B1 module + a QEMU differential.
>   2. **Review/gate B3's Cuda IFR/IER fix** -- a LIVE ungated change to `dev_cuda.cpp` (4794 unit checks + e2e pass, but "gated-OFF byte-identical" not strictly honored): confirm pure-correctness or gate it under the newworld path.
>   3. **Faithful-staging default (footgun -- fix before the gate is flipped on by anyone else).** The default path (no `SS_M18_PARCEL_FILE`) still silently stages decompressed->garbage. FIX = make the FAITHFUL path the DEFAULT whenever the gate is on, via **BOOT-TIME EXTRACTION** from the user's already-present ROM asset (the `.rom` at the prefs RomPath / `/Users/Shared/macemu`; slice the compressed `prcl` at offset `0x1bfc0` size `0x259c8c`, or parse it via `rom_decode.hpp`) inside `TrampolineStageParcels` (hook `trampoline_loader.cpp:296-305`; logic in `SheepShaver/tools/extract-toolbox-parcels.py`). **DO NOT commit the ~2.4MB `prcl` artifact to git** -- it is a slice of a copyrighted Mac OS ROM (licensing) + repo bloat, against the project's asset discipline. `SS_M18_PARCEL_FILE` stays as an override only.
>   4. **CD-1 probe gate (S3)** -- before S3 integration trusts the EXT-injection shim, dump `[SPRG3+0x14]` post-install + assert ==`0x50314880` (`FINDINGS-s3-nk-image-contract.md`); CD-3: the NK EXT handler reads an interrupt-source block (`KDP-0x338`), ties to B3's Cuda.
> - **Commit-hygiene note:** commit `9e4ae1d0` (titled "CD-1 GATE docs") ALSO carries A's ppc-execute recorder hooks + B3's full Cuda fix -- a `git commit -am` swept concurrent agents' tracked edits; `git blame` on `dev_cuda.cpp`/`ppc-execute.cpp` points there. Cosmetic, not worth a rebase. **Rule reaffirmed: never `git commit -a` with agents in flight -- explicit-path staging only.**

## Resume chain (read in this order; see CONTRIBUTING §0b for the discipline)

1. **This file** — the RIGHT NOW box above is the live state; the rest is pointers + standing rules.
2. **`docs/AGENT-CONTEXT.md`** — standing facts, constants, instrument caveats, gate tiers.
3. **`docs/planning/newsheep/README.md`** — the Operation NewSheep charter (§9 = status/next).
4. **The program plan's STATUS table** (`…/plans/2026-06-14-ss-m18-trampoline-lle-program.md`) — the
   per-stage done-vs-todo board; **this is the canonical "what's left" record** (the in-session task
   list does NOT persist).
5. **Findings the next action needs:** `FINDINGS-trampoline-re.md` (incl. "SS_M18 gating Task-0"),
   `FINDINGS-discriminator-a.md`, `DONOR-NOTES.md`.

Process: `docs/MILESTONE-WORKFLOW.md`. **Never push without being asked. Never global pkill — slot boots
only via `SheepShaver/tools/ss-slot-boot.sh`.** Baseline tag: `newsheep-baseline`.

## Frontier detail (the RIGHT NOW box, expanded)

- **Operation NewSheep** — run/reproduce the **producer** of the boot-time init (the Trampoline, an ELF
  parcel inside the Mac OS ROM file we hold + the NanoKernel) instead of forging the frozen CGRP/IM
  structures. Supersedes the forge *approach* (not the M15 verdict). Charter + scope + risks:
  `docs/planning/newsheep/README.md`.
- **Both Task-0s complete →** Trampoline RE: **Route A decided** (run the real Trampoline + NanoKernel vs
  a synthesized OF-CI + Core99 device tree). SS_M18 gating Task-0: **GO but MONTHS** — three independent
  month-forcing findings: **Q1** the real NanoKernel is a permanently-resident supervisor with no handoff
  boundary; **Q2** it requires a real paged MMU; **Q3** CGRP is built by disk/CFM IM-init, NOT the NK
  parcel (corrects Q0-F, confirms M16). Detail: `FINDINGS-trampoline-re.md`.
- **Staged program (rev-4)** + kickoff recon done: Discriminator-A=COARSE (→ S1 Dolphin shadow-arena),
  donors extracted (`DONOR-NOTES.md`), 9.2.1/9.2.2 ISOs in hand (`ASSETS-AND-TOOLING.md` R2). Per-stage
  status: the program plan's STATUS table.
- **Standing fact:** the ring-walk tool is **`tools/ring-walk.py`** (repo-root `tools/`, NOT
  `SheepShaver/tools/`).

## How we got here — the M8→M17 forge arc (CLOSED, banked NO-GO)

> **CANONICAL lineage = `docs/planning/newsheep/GLOSSARY.md` "The M8→M17 lineage" table** — read it for
> the per-milestone detail; it is NOT restated here (restating it caused the M14-wall contradiction). The
> arc proved forging the guest's interrupt/nanokernel structures is bankrupt; root cause = the Trampoline
> never runs. **Load-bearing survivors:** M13 — NewWorld 68k interrupt *delivery* works (don't re-chase
> it); M14-FINDINGS — the `[ALARM]` stall is a **Cuda IFR/IER device-model bug** (the expected
> post-NewSheep next wall), NOT a model-rejection gate. Banked FINDINGS:
> `M14-FINDINGS-cuda-delivery.md`, `M15-FINDINGS-consumption-recon.md`, `M16-FINDINGS-oracle-forge.md`.
> M10/M11/M11a COMPLETE; M12 PARTIAL; M13 retraction; M14–M17 = the forge arc (CLOSED).

## Standing operational notes

- Branch `macos-arm64`; never push unprompted.
- Parallel boots via `SheepShaver/tools/ss-slot-boot.sh` only (never global pkill); cleanup
  `SheepShaver/tools/ss-reap.sh`.
- All new code behind an env gate + `MachineProfileIsNewWorld()`; paravirtual byte-identical;
  `make test-jit` = 100. `0xDEADBEEF` (M10 DR-reentry) = immediate stop.
- **Gate cwd (canonical):** ALL `make build-ss|test-jit|e2e|nw-northstar` run from `SheepShaver/` (the
  353-vector authoritative harness); the repo-root `make test-jit` is the BasiliskII 301-vector harness —
  NOT this.
- Queued ideas: `docs/planning/BACKLOG.md`.
- Session logs / older history: `docs/archive/2026-06/LEARNINGS-2026-06.md`,
  `docs/archive/2026-06/HANDOFF-SESSIONS-M9-M12.md`; current interrupt-delivery diagnosis (reframes the
  old session log) = `docs/planning/M13-FINDINGS-interrupt-delivery.md`.

## ▶▶ PARALLEL ACCELERATION FAN-OUT (2026-06-15, user-directed) — oracles + zero-dep prep, serial S1↔S3 spine
The NK now executes → harvest its real behavior as OFFLINE ORACLES, fan out zero-dependency build/RE in
parallel, keep the S1↔S3 INTEGRATION a single serial evidence-grounded thread. Discipline gate (every
parallel deliverable): (a) findings doc w/ a falsifiable claim, (b) unit-tested standalone component behind
a pinned interface, or (c) an oracle/fixture — never "go implement S3"; test-jit=100 + gated-OFF byte-identical.
- **Track A (CRITICAL-PATH SPINE PREP):** the NK-MMU-write RECORDER (extend the `[ROMPATCH] sr_load`/BAT
  interception to log reg+value+PC+order to a file) + the NK-image STAGING fix (so the NK body runs) →
  HARVEST the full mtsr/mtsrin/mtdbat*/mtibat*/mtspr-SDR1 + sc/EXT trace = the S1/S3 ORACLE FIXTURE.
- **Track B (parallel, zero-dep, file-disjoint):** B1 Dolphin Dynamic-BAT shadow-arena standalone module
  (behind `paged_mmu_translate`, unit-tested vs synthetic + QEMU + the A oracle) — the bulk of S1, no live
  boot; B2 S3 static RE of the resident NK image (0xC00000 / the dump) → exception-vector/sc/EXT contract
  diff vs SS's synthetic supervisor; B3 S4 Cuda IFR/IER offline fix + unit test vs dev_cuda/dev_via6522
  (DingusPPC `ViaCuda::update_irq` donor, M14 bug pinned); B4 S4 disk IM-init→CGRP RE + 9.2 boot-target
  characterization (findings).
- **The SPINE stays serial + single-owned:** S1↔S3 have no live SR/BAT map until the real NK install runs,
  so their COMPOSITION is the milestone — the parallel agents produce validated components + oracles; the
  actual S1↔S3 integration is one careful disasm/watchpoint-grounded thread (UNBLOCK-vs-REPRODUCE settled by
  evidence, not cost). Do NOT let two agents both declare S1/S3 "done" against separate oracles.

### Fan-out coordination notes (2026-06-15)
- **B3 (S4 Cuda) DIED mid-task (API socket).** Its edits are isolated to `dev_cuda.cpp`/`dev_via6522.cpp`
  (+headers) — its own domain, no overlap with A (ppc-execute.cpp recorder + nk_mmu_trace.* + staging) or B1
  (dolphin_bat_arena.*). The tree still builds (B3's `test_dev_cuda` built). **Recovery:** when A+B1 land,
  commit each by EXPLICIT paths, REVERT B3's dead `dev_cuda/via` edits, then REDISPATCH B3 serially (so it
  edits the shared `machine/Makefile`/`Makefile.in` when nothing else is). **Lesson (file-ownership):** 3
  implementers sharing the Makefiles was under-managed — serialize shared-file (Makefile/ppc-execute) edits.
- **Staging fork (Track A): lean FAITHFUL** — stage the COMPRESSED prcl container + let the real Trampoline
  decompressor build a correct KernelCode image (the "let real code do it" principle), NOT a decompressed-
  contiguous patch (a stopgap; label it if taken). Steered A accordingly.

### Fan-out RECOVERED (2026-06-15) — all 5 deliverables landed + verified
B1 + B3 both FINISHED their work before dying on API errors (died during reporting). Recovery complete:
- **A** NK-MMU-write recorder (nk_mmu_trace.*, gated SS_M18_NK_TRACE) — test-jit=100; partial oracle (Trampoline
  BAT-clear only; full SR/BAT/SDR1 awaits the staging fix). Staging wall pinned watchpoint-grounded at
  pc=0x0159c37c (runtime parcel decompress/relocate COMPUTES garbage from our decompressed input — value
  0x3882009f exists in no static source). FAITHFUL fix (compressed container via SS_M18_PARCEL_FILE) owed +
  needs RE of the toolbox-parcels decompressor + a compressed MacROM.lzss artifact (not in the dump yet).
- **B1** Dolphin shadow-arena standalone module — 43 checks pass; INERT until S1 wiring; loads A's trace as a
  state-only replay (PROSPECTIVE until the NK body programs SR/BAT → `expect EA PA` oracle rows).
- **B2** S3 NK-image contract diff — REPLACE matches all sync seams; CD-1/CD-3 EXT-seam divergences (gated).
- **B3** Cuda IFR/IER fix (dev_cuda/via) — 4794+83 unit checks pass; e2e PASS (no paravirtual regression).
- **B4** S4 CGRP=coherence-group (disk krnl0 builder) + 9.2 boot target — genuine retail ISOs, R2 gap closed.
- **★ COORDINATOR LESSON:** a `git commit -am` swept concurrent agents' tracked edits (A's ppc-execute +
  B3's dev_cuda) into a docs commit, leaving HEAD referencing untracked files (broken from clean checkout).
  **NEVER `git commit -a` with concurrent agents in flight — ALWAYS explicit-path staging.** Fixed by
  committing the orphaned files + verifying build. Serialize shared-file (Makefile/ppc-execute) edits.
