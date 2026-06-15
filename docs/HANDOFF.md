# Project Handoff — resume entry point

> ## ▶ RIGHT NOW (read this first)
> - **Aim:** boot Mac OS 9.2 (NewWorld) by **running the Trampoline producer**, not forging its outputs (Operation NewSheep; the M8→M17 forge arc is closed).
> - **▶▶▶▶ KEYSTONE FINDING (2026-06-15, LATEST) — the real Trampoline RUNS in-tree (first time ever):** a
>   gated-ON both-gates probe boot (`SS_M18_TRAMPOLINE=1 SS_M18_NK_SUPERVISOR=1`, 9.0.1 ROM, slot protocol)
>   validated the ENTIRE S2b+S3 scaffolding end-to-end: S3 forge-retirement live (`[NK-SUP] T3+T4`), the
>   loader placed `MacOS.elf`, the launch seam set the CHRP ABI, and the `emul_ppc` override **entered the
>   real Trampoline at `0x20f078`** (forge bypassed) — which then EXECUTED (105M blocks in its
>   `0x180000–0x1f0000` code). **BUT `NanoKernelEntry 0x50310000` never fired (`iNK=0`), NO `[S2B-MMU-STUB]`
>   `/mmu` call ever issued, and it spun then trapped `twi @ pc=0x16c`** → **Outcome B / RESIDUE-PASS: no
>   live map produced.** ▶▶▶▶ **REFINED NEXT WALL: S1's live `/mmu` is NOT the immediate blocker — a
>   Trampoline EARLY-BRINGUP wall precedes it** (the Trampoline never reaches its OF-CI `/mmu` work). **Bringup RE (capstone) pinned it deeper:** the launch/glue/shim/return path is fully CORRECT (OF-CI IS
>   entered); the wall is **OF-CI ENVIRONMENT FIDELITY** — `claim()→0` + `/memory reg` size=0 + bootpath=0
>   (S2a-scope stubs in `machine/openfirmware_ci.cpp`). The Trampoline consumes `claim()=0` as an allocated
>   base → relocates/jumps to ~0 → wild-executes the unmapped `0x180000–0x1f0000` gap → `twi @ 0x16c`. The
>   r3/r4 sweep is LOW-value. **OF-CI fidelity LANDED (`af3f7717`): `claim` bump-allocator + `/memory`=RAMSize → the Trampoline now
>   blows past the zero-gap wall, makes 95 OF-CI calls, and issues `[S2B-MMU-STUB] /mmu translate
>   in[0]=0x01000000` — REACHED THE `/mmu` PHASE (legit relocation code at `pc=0x2026e0`, no wild jump).**
>   ▶▶▶▶▶ **S1's live `/mmu` is NOW the genuine immediate next wall** (Outcome shifted B→A): the `/mmu`
>   translate is the NON-ACCEPTANCE recording stub → returns degenerate. Minimally-real V=P `/mmu` LANDED (`fb0d0f19`, gated, NON-ACCEPTANCE-honest): correct + exercised (1
>   identity translate `0x01000000->0x01000000`) but did NOT move the wall. **NEW WALL (pinned upstream of
>   the NK): the Trampoline's own RELOCATION LOOP at guest `pc=0x2026e0` → SIGSEGV** — `stw r8,8(r31)` with
>   `r31=0x01fffff8` walking past the `0x180000` claim, loop vars garbage (`r30=0x3e18696d` ≈ ASCII → it's
>   reading the WRONG table). iNK=0 still. OF `/chosen` ihandles + `/mmu` node LANDED (`e58de6a4`): the relocation loop was over-running because
>   `/chosen` had no `"memory"` ihandle → `instance-to-package(0)→getprop(0,"reg")→-1` → count=0xFFFFFFFF.
>   Fixed → **the real Trampoline now completes its ENTIRE OpenFirmware phase** (OF-CI calls 183→722:
>   `/memory`, 13× `/mmu translate`+`map`, claim/release, then **`quiesce`+`exit`** — the OF→OS handoff
>   teardown). **▶▶▶▶▶▶ NEW WALL = the genuine S1 `/mmu`:** after `quiesce`/`exit` control wild-jumps into the
>   data segment (~`0x100000`) → SIGSEGV; `NanoKernelEntry 0x50310000` never fired (iNK=0). The `/mmu`
>   translate/map is still the **V=P identity recording stub** (NON-ACCEPTANCE) — the post-quiesce handoff
>   target depends on REAL `/mmu` translation/map. Recording `/mmu` + Apple map arg-order fix LANDED (`e1d4d944`). The OF→NK handoff is now fully RE'd: it's
>   a deliberate `rfi` to `translate(0x20f0b8)=0x20f0b8` (V=P, ALREADY correct, MSR=0x3000 translation-OFF) —
>   so `/mmu` is NOT the wall. **▶▶ NEW WALL (still pre-NK, pre-S1): the Trampoline's post-rfi PHYSICAL-MODE
>   RELOCATION ENGINE at guest `0x20f0b8+`** (clears all BATs, then relocates/copies driven by boot-info
>   structs `[r1+0x500]` (written by `0x202074`) + `[r1+0x2fc]` (by `0x20fe78`/the getprop chain),
>   `r27=[r4+0xc]`) → branches into the data segment `~0x100000` → SIGSEGV `pc=0x10031c`; iNK=0. `/mmu translate` IEEE-1275 success-flag fix LANDED (`07003fa1`): the boot-info structs were all-zero because
>   `translate` left `out[3]` (the `( virt -- false|phys mode true )` flag) unwritten → the Trampoline
>   discarded every translate → rfi-to-0. Fixed → the degenerate rfi-to-0 is GONE; the Trampoline advances
>   down the real handoff. **▶▶ NEW WALL: fault at guest `pc=0x2031e4` (`lwz r30,0(r24)`, `r24=0xffffffff`
>   from `[[r19]+0xa8]`) IMMEDIATELY AFTER the first non-identity `/mmu map` (virt `0x10000`→phys
>   `0xf3040000` MacIO/MMIO, mode 0x2a).** The Trampoline maps the MMIO aperture, walks a struct, derefs a
>   `-1` sentinel. **NEXT RE: the `0x2031e4` struct base (`[r19]+0xa8`) + `bl 0x207fe8(arg=-1)` → is it a DT
>   node/property returning -1 (cheap OF fidelity) OR does the MMIO map need a LIVE host remap (the genuine
>   S1 `/mmu` escalation / S3 co-land)?** iNK still 0. Each wall = bounded RE + small gated fix + measured
>   commit; trajectory unmistakable (real producer boots further each step). Process playbook:
>   `docs/MULTI-AGENT-FEATURE-WORKFLOW.md`. Plan + full ladder:
>   `docs/superpowers/plans/2026-06-15-ss-m18-s1-task0-live-mmu.md` §FINDING. **BOTH restructures (S2b-impl
>   T1–T5 + S3-impl T1–T4) are now validated running together gated-OFF-safe + gated-ON-executing.**
> - **State (2026-06-15):** both Task-0s DONE → **Route A GO but MONTHS**. Staged program planned (rev-4): **S1** paged MMU → **S2** loader+OF-CI+DT → **S3** two-supervisor reconciliation → **S4** disk IM-init→CGRP (critical path S1→S3→S4 ≈ quarters). Kickoff recon DONE: Discriminator-A=COARSE, donors extracted, 9.2 ISOs in hand.
> - **▶▶ NEWEST PROGRESS (2026-06-15 overnight, LATEST) — S2b-impl is BUILD-RESIDUE-COMPLETE (T1–T5 all
>   committed, gated-OFF, byte-identical):** the in-tree path that RUNS the real Trampoline now exists behind
>   `SS_M18_TRAMPOLINE` (default OFF). **T1** staged-asset `MacOS.elf` loader + 9.0.1-ROM-identity guard
>   (`31eafd59`); **T2** guest-callable r5 marshalling shim — EXEC_NATIVE bridge + pointer-arg descriptor
>   schema + BE-32↔host-cell (`d528a350`); **T3** CHRP launch seam — `emul_ppc` enters the Trampoline at
>   `0x20f078` (`b3c16ffa`); **T4** OF-CI wiring — `of_ci_callback` bound, `/mmu` recording stub
>   (NON-ACCEPTANCE) + `[S2B-SHIM-COLLIDE]` tripwire (`93545b6f`); **T5** forge loader-gated via the
>   additive-clause `if (!TrampolineLoaderGateEnabled())` with the S3-T4 rebase point documented (`4580651d`).
>   Plan rev-2 `47252b01` (red-team folded — ROM-identity guard cleared the ADVERSARY BLOCK). Each task:
>   `make test-jit`=100 + real `make e2e` PASS (byte-identical gate-OFF) + per-task micro-test. **GREEN-PASS
>   (the gated boot reaching `NanoKernelEntry 0x50310000` with `of_ci_unresolved_count()==0`) is DEFERRED
>   behind S1's real `/mmu`.** ▶▶ **NEXT: the S3 RESTRUCTURE remainder (T3 retire synthetic supervisor / T4
>   retire forge — rebased cleanly on S2b's additive-clause gate / both gated-OFF). **▶▶▶ S3-impl
>   RESTRUCTURE is ALSO COMPLETE (T1–T4): T3 retire synthetic supervisor (`475d53df`, gate-ON skips legacy
>   `SheepExcDeliverPending`, NK owns DEC `0x50313200`, scheduler yielded) + T4 retire forge (`f6dc02b0`,
>   additive-clause rebase, SDR1/HTAB/MSR + Execute68k retired, `sc`/program closure via live KDP+0x390/0x37c).
>   Each test-jit=100 + e2e PASS byte-identical, PEM untouched. BOTH supervisor-bringup halves now built.**
>   ▶▶▶ **NEXT (the sole remaining GREEN-PASS unlock for BOTH milestones): S1 — the live paged `/mmu`** (the
>   Dolphin SHM-arena window co-land, substrate PROVEN `acd89dce`+`cd7a62b6`). Open S1's deep Task-0
>   re-validating the two DEFERRED plans (`…s1-taskB-window-build.md` rev-2 + `…s1-softmmu-first.md` rev-2)
>   against the now-landed S2b+S3 restructure; then S3-T5 (live-MMU co-land) + the post-S1 gated boot deliver
>   GREEN-PASS. *(S3-impl T1+T2 history below.)*
> - **▶ PRIOR PROGRESS (2026-06-15 overnight) — S3-impl RESTRUCTURE half landed gated-OFF + the S2b
>   critical-path opened:** S3-impl plan **rev-2** re-scoped (`fa44698e`) to **RESTRUCTURE + GATED-MERGE-RESIDUE**
>   this milestone — **GREEN-PASS (T5 + the gate flip) is DEFERRED behind S2b** (T4's "S2 landed" precondition is
>   FALSIFIED: the forge substitutes the unbuilt loader). **S3-impl T1 + T2 COMMITTED green, gated-OFF:** T1
>   master gate `SS_M18_NK_SUPERVISOR` + inert scaffold (`3d057df2`); **T2 the load-bearing host→NK
>   EXT-injection shim** (`22db4818`, `exc_inject.{h,cpp}`: resolves the NK EXT vector from live **KDP+0x374**,
>   never hardcodes, builds a LOCAL table — never `g_exc_entry_table`, PEM untouched; micro-test 20/20,
>   test-jit=100, e2e PASS, bench codegen-identical). **S2b Task-0 recon DONE → RESIDUE-PASS** (`90e59fd1`,
>   `FINDINGS-s2b-loader-handoff.md`): NanoKernelEntry pinned **`0x50310000`**, loader source = **stage the
>   md5-verified 9.0.1 `MacOS.elf` asset** (`decode_parcels` does NOT surface it), CHRP seam + the
>   guest-callable r5 **marshalling-shim** contract pinned, `/mmu`→S1 (recording stub = NON-ACCEPTANCE).
>   **▶▶ ACTIVE NEXT ACTION: S2b-impl-FIRST** (decided — S2b lands first on the shared `rom_patches.cpp`+`glue`
>   per the file-ownership LAW; S3 GREEN-PASS is blocked behind it anyway). S2b-impl BUILD plan is drafting
>   (`…2026-06-15-ss-m18-s2b-impl-loader-launch.md`) → red-team → gated serial impl (staged-asset loader →
>   marshalling shim → CHRP launch seam → wire of_ci_callback + `/mmu` recording stub → loader-gate
>   `PatchROM_NW_trampoline`). Then return to **S3 T3/T4/T5** rebased on S2b's gate structure (the loader
>   replaces the forge → the install runs → GREEN-PASS becomes reachable). **S2b-impl PASS is gated on S1
>   (`/mmu`); its BUILD/UNIT is cleared now (no blocking residue on Q1/Q2).** *(S3 Task-0 history below.)*
> - **(SUPERSEDED 2026-06-15) S3 Task-0 DONE → GREEN-PASS, architecture = REPLACE; S3-IMPL was
>   USER-AUTHORIZED and is now IN FLIGHT (T1+T2 landed; T3+ paused behind S2b).** S3 recon (4-reviewer red-teamed):
>   `FINDINGS-s3-{two-supervisor,nk-ownership,ss-reusemap}.md`, plan `2026-06-15-ss-m18-s3-two-supervisor-task0.md`
>   rev-2. **Architecture = REPLACE** (retire SS's synthetic supervisor under `SS_M18_NK_SUPERVISOR ∧
>   MachineProfileIsNewWorld()`, run the real NK resident; CO-OWN/yield-fully scored out — co-residency
>   statically impossible; thin-shim = strictly-larger-surface of REPLACE). **The window substrate is
>   PROVEN** (`acd89dce` single + `cd7a62b6` multi-entry: atomic one-step `mach_vm_map(FIXED|OVERWRITE)`,
>   concurrent-reader-safe, 16K-coarse fork — the live-MMU's biggest risk de-risked). **S3-impl is
>   LAW-adjacent (supervisor handover) → needs the user's explicit GO; KEEP exc_core vectoring (PEM=LAW),
>   paravirtual provably unreachable, revert-on-red=branch revert.** Owed to S3-impl's own boot: the
>   device-IRQ→NK-EXT seam, the sc path, the G3.a/c positive probe (the QEMU rig stalls pre-NK-install).
>   *(S1 history below for context.)* S1 mechanism =
>   DONE: Task A `paged_mmu_translate()` + oracle test committed (`fc3ca256`/`2bf1526b`/`191fe67c`+fix;
>   `make test-jit`=100, `test_paged_mmu` 36/36) and the static NK-MMU constants derived
>   (`FINDINGS-s1-mmu-constants.md`, `f77702d9`). **Verdict:** there is no live `(SR/BAT/SDR1)` map until
>   the real NK install runs, which only S3 (two-supervisor reconciliation) provides — so a standalone S1
>   live MMU (window OR softmmu) is not buildable/validatable. **★ The full reasoning (why the live MMU ⟺
>   S3) is the canonical doc `docs/planning/newsheep/MMU-NANOKERNEL-INSEPARABILITY.md`** — read it before
>   any future S1 live-MMU attempt (the kill-switch for re-deriving this a fourth time). Both live-MMU
>   plans are DEFERRED (re-validate at S3 against the NK-installed tables):
>   `…2026-06-15-ss-m18-s1-taskB-window-build.md` rev-2 + `…2026-06-15-ss-m18-s1-softmmu-first.md` rev-2
>   (impl plan `…ss-m18-s1-impl-paged-mmu.md`). **The only truly-S1 work remaining pre-S3** is a cheap
>   `SS_PROBE_PC` forge-state instrumentation pass. S2a-impl is DONE (inert). Do NOT re-run the gating
>   Task-0 / Discriminator-A. Do NOT relitigate Route A. *(The prior softmmu-first/window-build next-action detail is preserved in the two DEFERRED plans + the canonical doc; removed here to keep the live box current.)*

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
