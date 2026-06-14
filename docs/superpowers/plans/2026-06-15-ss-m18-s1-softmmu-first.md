# SS_M18 Stage 1 — NewWorld paged MMU: S1 SOFTMMU-FIRST — the interpreter-chokepoint walker + map harvest — IMPLEMENTATION milestone

> **Status:** rev-1 (2026-06-15), pre-red-team. NEW CRITICAL PATH. Created by the coordinator
> disposition that DEFERRED the `vm_remap` window (`2026-06-15-ss-m18-s1-taskB-window-build.md` rev-2
> Red-team record: ADVERSARY WRONG-BUILD-ORDER · TECHNICAL V4 NO-GO-without-spike · PROCESS
> GO-WITH-FIXES). The window's `VM_FLAGS_OVERWRITE`-on-live-NATMEM primitive is unproven +
> topology-mismatched, and the inherited "softmmu-default-until-measured" discipline is un-implementable
> while softmmu is unwritten. This plan builds that missing softmmu first.
>
> **Binding inheritance (NOT relitigated):** Route A SETTLED; program shape S1→S3→S4 SETTLED;
> Discriminator-A COARSE SETTLED; window is the user-PREFERRED *end* mechanism (only the BUILD ORDER
> flipped); Task A is DONE/committed (`paged_mmu_translate()` + `test_paged_mmu.cpp`, commits
> `fc3ca256`/`2bf1526b`/`191fe67c`, real-mode/BAT bug fixed). This plan REUSES that core verbatim.

## ⚠ SCOPE NOTE (read first)

This milestone is deliberately the **low-risk** sibling of the window: **NO NATMEM surgery, NO
`vm_remap`, NO JIT emit-site edits.** It wires the committed, adversary-fixed `paged_mmu_translate()`
at a SINGLE interpreter chokepoint behind a double gate + adds a harvest dump. Two deliverables:
1. **Correctness reference / harvester** — a real paged NewWorld boot under the interpreter translating
   every guest RAM/ROM access through the classic MMU, at zero foundation risk.
2. **Data extraction** — HARVEST the live `(SR/BAT/SDR1, EA)` map + DBAT descriptors as measured
   constants. That DATA retires the deferred window's owed coverage predicate (DBAT-covers-RAM+ROM,
   BAT-priority sanity) AS DATA, and is the in-process PA oracle the window later A/B-validates against.

Division of labor (BINDING): **softmmu+interp = correctness reference + harvester; the window
(deferred) = the JIT-path performance solution.**

## Goal (PASS-vs-DIAGNOSTIC)

**PASS (falsifiable).** A gated softmmu walker at the interpreter chokepoint translates interpreter
guest RAM/ROM accesses through `paged_mmu_translate()` under a real NewWorld boot (`SS_USE_JIT=0`), is
non-regressing to paravirtual, and EMITS the harvest artifact. PASSES when all of:
1. **W1 GREEN** — `vm_do_get_real_address` (`vm.hpp:225`) routes through `paged_mmu_translate()` when
   `SS_M18_PAGED_MMU ∧ MachineProfileIsNewWorld()`, reading live `(sr[16],bat[16],sdr1,msr)` from an
   installed regs-provider + live guest HTAB via the phys-reader hook. The ~68 RMEMBASE JIT sites
   UNTOUCHED.
2. **W2** — the Vs/Vp-vs-`MSR[PR]` BAT valid-bit selection + PTE PP protection (paged_mmu adversary
   Finding 3) folded into `paged_mmu_translate()` (signature + `msr_pr`) + a `test_paged_mmu` row.
3. **All gates pass** — G-build (`make test-jit`=100 authoritative + machine test), G-paravirtual (red
   team settles inertness-substitute vs real e2e), G-harvest (interp-only paged boot reaches a
   translation landmark + emits the dump).
4. **The harvest is COMMITTED** — `docs/planning/newsheep/FINDINGS-s1-softmmu-harvest.md` with the live
   `(SR/BAT/SDR1,EA)` map + DBAT descriptors as constants. That DATA retires the deferred window's owed
   predicate.

**PASS taxonomy:**
- **GREEN-PASS** = all gates clear AND the interp-only paged boot reaches the `mtsrin` landmark AND the
  harvest dump is non-degenerate (non-identity, non-sentinel) → hand the constants to the deferred
  window milestone + S2/S3.
- **RESIDUE-PASS** = the interp-only paged boot STALLS before the MMU-install landmark (QEMU-rig
  failure mode), OR translation is reachable only through a path the chokepoint cannot see → cannot
  harvest a live map → **finding, not a green light**: STOP-and-surface; coordinator re-bands (harvest
  vehicle changes). "We wired the chokepoint and it compiles" is NOT a license to claim the harvest.

**DIAGNOSTIC:** translation fault counts; BAT-vs-PTE hit histograms; how far a stalled boot got;
interp+softmmu ns/insn (expected slow — NOT a gate). Unexpected live markers ESCALATED.

## ★ The load-bearing design fact (RED TEAM MUST VET)

**The aarch64 JIT does BARE RMEMBASE accesses that BYPASS `vm.hpp`** (RMEMBASE=`x19`,
`ppc-jit.cpp:445`, ~68 bare sites, no chokepoint). So a softmmu at `vm_do_get_real_address` translates
ONLY interpreter accesses → the paged harvest boot MUST run **interpreter-only (`SS_USE_JIT=0`)** —
slow but CORRECT — purely to harvest the map. This is why the milestone is lower-risk than the window
(no emit-site surgery), and its sharpest assumption: the interp path must route guest RAM/ROM access
through `vm_do_get_real_address` under newworld AND reach the NK MMU-install landmark while
interpreting. Verified at draft: interp accessors `vm_read/write_memory_*` (`ppc-execute.cpp`) call
`vm_do_get_real_address` with no direct-addressing bypass. Red team must confirm completeness +
landmark reachability.

## Authoritative inputs

| Doc / source | Role |
|---|---|
| `docs/superpowers/plans/2026-06-14-ss-m18-s1-impl-paged-mmu.md` (rev-2) — parent | Gate philosophy, file ownership, Stop-rule, AD-1/AD-2, the softmmu-default discipline this finally makes implementable. Task A DONE. |
| `docs/superpowers/plans/2026-06-15-ss-m18-s1-taskB-window-build.md` (rev-2) — DEFERRED sibling | Why softmmu-first exists (WRONG-BUILD-ORDER); the owed predicates this harvest retires; what NOT to build (no `vm_remap`). |
| `docs/planning/newsheep/FINDINGS-s1-window-derisk.md` (rev-2) | The exact harvest targets: DBAT (independent of IBAT, strides `0x17,0x1b,0x1f,3` @`0x50315370-0x503153c8`) must cover RAM+ROM; BAT-priority sanity; region table @`r1+0x5e8`; SDR1 @`0x50310604`. |
| Task A artifacts (`fc3ca256`/`2bf1526b`/`191fe67c`) | `machine/paged_mmu.cpp :: paged_mmu_translate(sr[16],bat[16],sdr1,ea,msr_dr,*out_pa)` + `paged_mmu_set_phys_reader()`. REUSED; extended only for W2. |
| Oracle (external, UNLINKED): PearPC `ppc_effective_to_physical()` PRIMARY; QEMU `mmu_common.c` SECONDARY | PA-diff cross-check of `paged_mmu_translate()` incl. the privilege row. SOURCE+PIN repo+file+SHA. UNLINKED → no link propagation. NEVER port the walker (Stop-rule #6). |
| `docs/MILESTONE-WORKFLOW.md` §2/§4/§6 | The machine; baseline-is-part-of-the-gate; gate tiers; unbounded-disasm kill-switch. |

## Codebase facts (verified this session — RE-VERIFY at impl; drift ±1–5)

- **The chokepoint is REAL + is the interp memory path (✔).** `vm.hpp:225` `vm_do_get_real_address(addr)`
  → `(uint8*)(VMBaseDiff + a)`; `VMBaseDiff=NATMEM_OFFSET` (`:213`). Interp reads/writes
  `vm_read/write_memory_{1,2,4,8}` (`:241-259`) funnel through it; `ppc-execute.cpp` uses those macros.
  Existing Apple/x86_64 special-case at `:228-234` (`gZeroPage`/`gKernelData`) — the softmmu arm must
  be placed correctly relative to it (tension 4).
- **★ The JIT bypasses this (✔)** — interp-only by construction.
- **The regs-provider problem (✔ NEW, load-bearing).** `vm_do_get_real_address` is context-free (no
  cpu pointer). The live regs object is `static sheepshaver_cpu *ppc_cpu` in `sheepshaver_glue.cpp:1683`
  — **MUST NOT touch (S2/S3).** → install a regs-provider (callback yielding live
  `sr[16],bat[16],sdr1,msr`) from an ALLOWED file (`ppc-execute.cpp`/`ppc-cpu` init, where `regs()` is
  in scope). Red team: confirm soundness + single-active-cpu / no reentrancy hole.
- **`paged_mmu_translate()` (✔ `paged_mmu.h:44`):** `(sr[16],bat[16],sdr1,ea,msr_dr,*out_pa)→bool`;
  DBAT at `bat[8..15]`; phys-reader via `paged_mmu_set_phys_reader(fn,opaque)`. High BATs omitted
  (AD-2). **W2 adds `msr_pr` + Vs/Vp/PP.**
- **MSR[PR] available at the chokepoint** via the regs-provider → privilege gate now BUILDABLE.
- **Phys-reader reads GUEST HTAB via the SAME NATMEM base**, big-endian-correct (live HTAB is BE guest
  memory, unlike the synthetic host-native test — endianness IS in scope for the live walk; tension 5).
- **`MachineProfileIsNewWorld()`** = `machine/machine_profile.cpp:96–99`; precedent `ppc-cpu.cpp:1986`.
- **Provenance:** NK md5 `61c176e90b6365e84e5c660d703e56af`; ROM md5 `66210b4f71df8a580eb175f52b9d0f88`.

## Task breakdown (gated; lowest-risk-first; mechanism FIXED to softmmu)

### W1 — wire `paged_mmu_translate()` at the interpreter chokepoint
- [ ] Re-verify `vm.hpp:225` + the `:228-234` special-case + the `ppc-execute.cpp` accessors + the glue
      ownership boundary.
- [ ] Install a regs-provider from an ALLOWED file (`ppc-execute.cpp`/`ppc-cpu` init): yields live
      `sr[16],bat[16],sdr1,msr`; set on newworld init, NULL on paravirtual (gate structurally inert).
      **MUST NOT edit `sheepshaver_glue.cpp`.**
- [ ] Install the phys-reader reading guest HTAB at `VMBaseDiff + pa`, big-endian-correct.
- [ ] Gated softmmu arm in `vm_do_get_real_address`: when `SS_M18_PAGED_MMU ∧ newworld` + live provider,
      call `paged_mmu_translate(...)`; resolve → `VMBaseDiff + pa`; fault → defined action (tension 6).
      Gate off → BIT-IDENTICAL to today.
- [ ] Confirm placement vs the `gZeroPage`/`gKernelData` special-case (tension 4).

### W2 — fold the Vs/Vp-vs-MSR[PR] privilege gate
- [ ] Extend `paged_mmu_translate()` (header+impl): add `msr_pr`; PR=0 → test BAT **Vs**, PR=1 → **Vp**;
      enforce PTE **PP** (+ BAT PP) → fault on protection violation.
- [ ] `test_paged_mmu.cpp` privilege row: same EA/BAT, PR=0 vs PR=1 with Vs≠Vp → distinct resolve/fault;
      PP-protected PTE faults a problem-state write.
- [ ] W1 caller passes `regs().msr & MSR_PR`.

### W3 — the harvest dump (own env var)
- [ ] Harvest-dump routine gated by `SS_M18_HARVEST`: at/after the landmark, snapshot live
      `(SR[16],BAT[16],SDR1)` + DBAT descriptors + a sampled `(EA→PA)` map to stdout/log (NOT a
      source-tree write by the binary). Live home: an ALLOWED file.
- [ ] Author + COMMIT `docs/planning/newsheep/FINDINGS-s1-softmmu-harvest.md` from the dump: the live
      map + DBAT descriptors as constants + DBAT-covers-RAM+ROM + BAT-priority. **This DATA retires the
      deferred window's owed predicate.**

### Gates (falsifiable-in-advance)
- [ ] **G-build** — `make test-jit`=100 authoritative (softmmu arm gated-off there → no-op; <100 ⇒ STOP,
      gate leaked into hot path) + `make -C SheepShaver/src/machine test` GREEN incl `test_paged_mmu`
      (two-context/COARSE-BAT/adversarial-FINE/**privilege**, equal-PA + equal-fault vs the SPEC-derived
      reference + the external PearPC/QEMU oracle cross-check).
- [ ] **G-paravirtual** — paravirtual byte-identity. **OPEN for the red team:** the arm is structurally
      unreachable on `!newworld` (provider NULL + gate false) → inertness-substitute argument; BUT it
      edits a SHARED runtime path (`vm_do_get_real_address`) → real `make e2e` likely owed. Red team
      decides.
- [ ] **G-harvest** — an interpreter-only (`SS_USE_JIT=0`) paged boot reaches the translation landmark
      (`mtsrin` context switch live + RAM/ROM resolving non-identity) AND emits a non-degenerate dump.
      **Stall before the landmark ⇒ RESIDUE-PASS, STOP-and-surface** (tension 3).

> **Re-band trigger (binding):** interp-only paged boot can't reach the landmark, or harvest yields only
> identity/sentinel → STOP-and-surface; coordinator re-bands. Do not fabricate a map.

## Env-gate
`SS_M18_PAGED_MMU ∧ MachineProfileIsNewWorld()` (+ live regs-provider). Structurally unreachable in
paravirtual. JIT path UNTOUCHED → no per-access JIT branch. Harvest dump has its own `SS_M18_HARVEST`.
`SS_USE_JIT=0` for the harvest boot is operational, not a compile gate.

## File ownership (serialized, strong tier)
- **Edits:** `kpx_cpu/src/cpu/vm.hpp` (chokepoint arm, W1); `machine/paged_mmu.{cpp,h}` (privilege
  extension, W2); `machine/test_paged_mmu.cpp` (privilege row, W2); `ppc-execute.cpp`/a new `machine/`
  helper (regs-provider + phys-reader + harvest dump, W1/W3).
- **New (optional):** a `machine/` harvest helper TU + Makefile target.
- **MUST NOT touch:** `Unix/main_unix.cpp` / `vm_remap` / NATMEM surgery; the `ppc-jit.cpp` emit sites;
  `sheepshaver_glue.cpp` (S2/S3). These exclusions ARE the point.

## Stop-rule (carries the parent; one-iteration mechanics)
1. **No NATMEM surgery / no `vm_remap` / no JIT emit-site edit** ⇒ STOP (that's the deferred window).
2. **Identity-only MMU when translation is load-bearing** — fails the two-context battery; readback is
   a false-clean (A3 LAW).
3. **`0xDEADBEEF`/sentinel where a real PA/PTE/descriptor is expected ⇒ STOP.**
4. **Exclude any field from the JIT-verify memcmp to "fix" a divergence ⇒ STOP.**
5. **Edit `sheepshaver_glue.cpp`/`main_unix.cpp` to reach the cpu or remap ⇒ STOP** — provider/reader
   install from an allowed file, or the design is wrong.
6. **Port the oracle's per-access walker ⇒ STOP** — UNLINKED fixture generator only.
7. **Run the stalled QEMU NK-install rig to get the live map ⇒ STOP** — harvest from S1's OWN interp
   boot; if THAT stalls, RESIDUE-PASS + surface.
8. **Unbounded disasm ⇒ STOP**, conservative residue. No wholesale parcel disasm.
9. **Cite MEM_BULK / contiguous arena ⇒ STOP** — irrelevant (no remap substrate here).
10. **Claim the harvest retires the window predicate from synthetic data ⇒ STOP** — valid ONLY from a
    real interp-only paged boot reaching the landmark.

**One-iteration rule.** Falsified pin → dated entry → ONE bounded re-pin → resume. SECOND falsification
of the same answer ⇒ STOP, re-plan.

## Self-review record — tensions FLAGGED FOR THE RED TEAM
1. **Chokepoint completeness** — is `vm_do_get_real_address` on the FULL guest RAM/ROM interp access
   path under newworld (incl string/multiple/FP/`dcbz`/`lwarx`), or do exotic accesses bypass it
   (→ partial/wrong harvest map)?
2. **Regs-provider without glue edits** — sound + captures the ACTIVE cpu, no reentrancy/thread hole
   (device/DMA/SDL thread calling the chokepoint with a stale regs pointer)?
3. **Interp-only boot reachability (make-or-break)** — can an interp-only paged boot reach the NK
   MMU-install + `mtsrin` landmark, or stall like the QEMU rig? Contingency required before kickoff?
4. **Placement vs `gZeroPage`/`gKernelData`** (`vm.hpp:228-234`).
5. **Live HTAB endianness** — BE PTE-word handling on the real table.
6. **Fault contract** — deliver guest DSI/ISI vs abort+diagnostic vs identity-fallback (the last is a
   Stop-rule-#2 false-clean → reject).
7. **Privilege extension paravirtual byte-identity** — `paged_mmu_translate()` has no paravirtual caller
   (newworld-gated); confirm no test/caller perturbed.
8. **G-paravirtual** — inertness-substitute sufficient or real `make e2e` owed (shared runtime path)?

## Red-team record
*(empty — to be filled by the three-reviewer red-team: PROCESS + TECHNICAL + ADVERSARY. Voting list V1–V10 submitted alongside.)*
