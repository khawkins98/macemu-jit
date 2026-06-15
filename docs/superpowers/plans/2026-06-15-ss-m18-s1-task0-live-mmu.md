# SS_M18 Stage 1 — the LIVE paged /mmu — DEEP Task-0 (recon + design)

> **Status:** rev-1 (2026-06-15). Recon/design ONLY — writes NO `SheepShaver/src/**`, runs NO
> builds, spends NO boots (the one probe boot is DESIGNED here and RUN by the coordinator, §B).
> Opens the S1 live-MMU node, which `MMU-NANOKERNEL-INSEPARABILITY.md` (CANONICAL) places
> *co-landing with S3*. **The premise that deferred S1-live has CHANGED** and that change is the
> whole reason for this Task-0 — see "What changed" below.
>
> **Scope law (carried, NOT relitigated):** Route A SETTLED; program shape S1→S3→S4 SETTLED;
> Discriminator-A COARSE → Dolphin shadow-arena path SETTLED; the proven SHM-arena window
> (single `acd89dce` + multi-entry `cd7a62b6`) is the user-PREFERRED end mechanism; `paged_mmu_translate()`
> mechanism DONE (`fc3ca256/2bf1526b/191fe67c`). Do not re-derive any of these.

## What changed (why S1-live is re-openable now)

`MMU-NANOKERNEL-INSEPARABILITY.md` reasoned from a boot where **the NK MMU-install never runs**
(the `SS_NW_TRAMPOLINE` output-forge: SDR1 in C, SR=0, empty HTAB) → "no live `(SR/BAT/SDR1)` map to
mirror or harvest" → both window and softmmu deferred. **That premise is now superseded by two landings:**

- **S2b-impl** (gated `SS_M18_TRAMPOLINE`) LOADS + LAUNCHES the real `MacOS.elf` Trampoline and wires
  OF-CI, with the `/mmu` call-method backend as a RECORDING STUB (`[S2B-MMU-STUB]`, identity claims,
  declared NON-ACCEPTANCE, owed to S1).
- **S3-impl** (gated `SS_M18_NK_SUPERVISOR`, REPLACE architecture) RETIRES the SDR1/HTAB/MSR forge +
  the synthetic supervisor, so the **real NK programs SDR1/BAT/SR LIVE** (`mtsrin`@`0x50315290`,
  BAT@`0x503152c4`, SDR1@`0x50310604`, PTE engine@`0x50319af8`).

So for the FIRST time a **both-gates-ON** boot (9.0.1 ROM) can run loader→launch→OF-CI→NK-install until
it faults on the missing live `/mmu`. **The live `(SR/BAT/SDR1)` map the deferred plans said did not
exist is now PRODUCED by the gated-ON chain.** S1's job is to translate against it. This Task-0
re-validates the two DEFERRED plans against that new state and pins the fault that the design must target.

## Goal (PASS-vs-DIAGNOSTIC)

**PASS (the Task-0 deliverable — falsifiable).** A written, red-teamable design for the live paged `/mmu`
that (a) PICKS window vs softmmu with evidence re-validated against the now-real NK install,
(b) pins exactly what `claim`/`translate`/`map` must provide and which NK-programmed SR/BAT/SDR1/HTAB
state the chosen mechanism must honor, (c) specifies the integration seam that REUSES (not rebuilds)
`paged_mmu_translate()` + the proven SHM-arena primitive + S2b's OF-CI backend + S3's live supervisor
programming, and (d) records the gated-ON probe-boot's FIRST-FAULT location (run by the coordinator).
Each binding question closes GREEN / RESIDUE with a falsifiable predicate.

**PASS taxonomy:**
- **GREEN-PASS** = all four Q close with a pinned mechanism + seam + the probe-boot first-fault located,
  AND that fault is consistent with "live map exists, `/mmu` stub is what breaks" → S1-live-impl is
  authorizable (co-landing with S3).
- **RESIDUE-PASS** (the expected-honest case) = the probe boot faults BEFORE the NK programs a live map
  (e.g. at the Trampoline's `/mmu translate` dereference of an identity claim) → the live map is still
  not produced end-to-end → S1-live remains an S3-co-land with the fault pinned as the next wall.
  A FINDING, not a green light; do not fabricate a map or "make the stub almost work."
- **NO-GO trigger:** if the probe shows the chain cannot reach the NK install for a structural reason
  (not just the `/mmu` stub) → STOP-and-surface; the co-land assumption itself needs re-planning.

**DIAGNOSTIC (never a gate):** how far the gated-ON boot gets; which probe PC last fired; `[S2B-*]`/
`[NK-SUP]` ladder depth; identity-claim counts from `[S2B-MMU-STUB]`.

## Authoritative inputs

| Doc / source | Role |
|---|---|
| `docs/planning/newsheep/MMU-NANOKERNEL-INSEPARABILITY.md` (CANONICAL) | Why live MMU co-lands with S3; the standing guard ("do NOT arm against the forge"). This Task-0 tests whether the gates-ON landing changes its premise — it does NOT relitigate the co-land conclusion, it OPERATES inside it. |
| `…/2026-06-15-ss-m18-s1-taskB-window-build.md` (rev-2, DEFERRED) | The window design + the two deferral reasons (overwrite-under-concurrency spike owed; softmmu-first build order). Re-validate both against current state. |
| `…/2026-06-15-ss-m18-s1-softmmu-first.md` (rev-2, DEFERRED) | The interp-chokepoint walker + harvest design + the JIT-bypass / regs-provider / TLS-guard tensions. Re-validate the "no live map to harvest" deferral. |
| `…/2026-06-14-ss-m18-s1-impl-paged-mmu.md` (DONE: Task A) | `paged_mmu_translate(sr[16],bat[16],sdr1,ea,msr_dr,*out_pa)` + `paged_mmu_set_phys_reader()` + `test_paged_mmu.cpp`. REUSE verbatim; W2 privilege extension (`msr_pr`/Vs/Vp/PP) still owed. |
| `FINDINGS-discriminator-a.md` | COARSE verdict → SR(256MB)/BAT(≥256MB)/SR-swap MMIO@`0x50325894`; JIT fast path preserved; HTAB residual. The FINE residual (PTE density) owed to the S1 oracle/live boot. |
| `FINDINGS-s2b-loader-handoff.md` | `/mmu` backend = recording stub (NON-ACCEPTANCE, → S1); the marshalling-shim I/O contract; `NanoKernelEntry=0x50310000`; handoff predicate `of_ci_unresolved_count()==0`; post-handoff NK fault EXPECTED. |
| `FINDINGS-s3-two-supervisor.md` (+ ★BINDING-AMENDMENT) | REPLACE retires the forge → NK programs SR/BAT/SDR1 live; the conditional live-MMU co-land spec (window front-runner, atomic `mach_vm_map(FIXED|OVERWRITE)` form); the EXT-injection shim. |
| `test_shm_arena_spike.cpp` (`acd89dce`) + `test_shm_arena_multientry.cpp` (`cd7a62b6`) | PROVEN: atomic one-step `mach_vm_map(FIXED\|OVERWRITE)`, concurrent-reader-safe, 16K-coarse fork, multi-region SET transaction. The window's platform foundation — both deferral-blocking spikes now PASS. |
| `docs/MILESTONE-WORKFLOW.md` §2/§4/§6 | The machine; gate tiers; baseline-is-the-gate; unbounded-disasm kill-switch. |

## Codebase facts (carried; RE-VERIFY at impl; drift ±1–5)

- NATMEM = SEPARATE fixed mach allocations (RAM `0x10000000`@`main_unix.cpp:1974`, ROM `0x50000000`@`:2000`),
  not a MEM_BULK arena. Window substrate = `mach_vm_map(FIXED|OVERWRITE)` on the separate sub-ranges
  (spike-proven). `RAMSize ≤ 0x40000000` disjointness assert.
- JIT does ~68 **bare RMEMBASE (`x19`) accesses** that BYPASS `vm.hpp` (`ppc-jit.cpp:445`) → softmmu at
  `vm_do_get_real_address` sees ONLY interpreter accesses → a softmmu harvest boot must be `SS_USE_JIT=0`.
- Interp chokepoint = `vm.hpp:225` `vm_do_get_real_address`; reached ALSO by `Mac2HostAddr`/`ReadMacInt`/
  `WriteMacInt` from the host-emulation layer + non-CPU threads → contamination/race → TLS
  `in_interp_translate` guard required (softmmu-first TECHNICAL V1).
- `MachineProfileIsNewWorld()` = `machine_profile.cpp:96–99`; boot-latched; precedent `ppc-cpu.cpp:1986`.
- The live SR/BAT/SDR1 regs object is `static sheepshaver_cpu *ppc_cpu` in `sheepshaver_glue.cpp:1683`
  — **S2/S3-owned, S1 MUST NOT edit** → regs-provider installed from an ALLOWED file.
- `/mmu` OF-CI backend lives in `machine/openfirmware_ci.cpp` (**S2b-owned**) → the real backend wiring
  is a FILE-OWNERSHIP seam, not a free S1 edit (Q-S1.3 / voting item 5).
- Provenance: NK md5 `61c176e90b6365e84e5c660d703e56af`; 9.0.1 ROM `66210b4f71df8a580eb175f52b9d0f88`
  (NOT the active 1.1 ROM `e0fc03faa589ee066c411b4603e0ac89` — the probe boot MUST use 9.0.1).

---

## Q-S1.1 — WINDOW vs SOFTMMU for the live `/mmu` (RE-VALIDATE the two deferred verdicts)

**The question.** Given COARSE (16K-coarse fork OK), the PROVEN SHM-arena window (BOTH spikes pass), the
JIT RMEMBASE fast-path-preservation requirement, and the now-landed S2b/S3 restructure that produces a
real NK install — which mechanism is the live `/mmu`, and in what build order?

**Re-validation of the two deferral reasons:**
1. **softmmu-first was DEFERRED because "no live map to harvest"** (`SS_NW_TRAMPOLINE` forge ran, NK did
   not). **That premise is now FALSE under both gates ON** — the real NK programs SR/BAT/SDR1 live (S3
   REPLACE). So the softmmu's harvest deliverable (snapshot the live `(SR/BAT/SDR1,EA)` map + DBAT
   descriptors as DATA) is RE-ACTIVATED **iff** the gated-ON chain reaches the NK install (Q-S1.4 / the
   probe boot decides this — falsifiable, NOT assumed).
2. **window was DEFERRED pending the `VM_FLAGS_OVERWRITE`-on-live-reservation-under-concurrency spike.**
   **That spike is now PROVEN** (single `acd89dce` + multi-entry `cd7a62b6`: atomic one-step form,
   concurrent-reader-safe, multi-region SET). The window's platform-foundation blocker is RETIRED — what
   remains owed is the *numeric* DBAT-covers-RAM+ROM coverage predicate (owed to G1.e/harvest DATA, not
   the platform).

**RECOMMENDATION (evidence-based, for the red team to ratify or refute).** **BOTH, in a fixed order:**
- **softmmu = the correctness reference + HARVESTER (build first, lowest risk).** Interp-only
  (`SS_USE_JIT=0`), no NATMEM surgery, no JIT emit-site edits, total coverage by construction. Its job is
  to RUN the now-real NK install under translation and HARVEST the live `(SR/BAT/SDR1,EA)` map + DBAT
  descriptors as measured constants — retiring the window's coverage predicate as DATA and serving as the
  in-process PA oracle.
- **window = the JIT-path live `/mmu` (build second, A/B-validated against the harvest).** Because the
  real boot runs the JIT (bare RMEMBASE), only the window makes a live boot's JIT accesses land on the
  SR/BAT-selected PA. Foundation now de-risked (spikes); COARSE means BAT/SR coverage preserves the JIT
  fast path; the harvest DATA replaces the unmeasured coverage bet.

  **Falsifiable predicate (the fork):** if the harvest (or the probe boot, Q-S1.4) shows any JIT-touched
  EA is page-table-only (DBAT does NOT cover some JIT-touched RAM/ROM, or a PTE diverges from the BAT for
  a BAT-covered EA) → the pure-`mtspr` window is INSUFFICIENT → tlbie/HTAB interception OR softmmu-on-JIT
  → re-band. **GREEN** = harvest shows DBAT covers RAM+ROM, no page-table-only JIT EA → window is the live
  JIT mechanism. **Budget:** doc reasoning + the two spike files + the probe-boot reading (§B); ZERO new
  disasm, ZERO new boots beyond the single coordinator probe.

  **Stop-rule:** do NOT pick the window over the softmmu-harvest on the *spikes alone* — the spikes prove
  the PRIMITIVE, not numeric coverage of the live NK working set (that is the harvest's job). Picking the
  window without the harvest DATA is the un-measured bet the deferred plans correctly refused.

---

## Q-S1.2 — WHAT THE `/mmu` MUST PROVIDE (where identity breaks; which NK state to honor)

**The recording stub returns identity.** Per `FINDINGS-s2b-loader-handoff.md` Q-S2b.4, the Trampoline does
REAL `claim`/`translate`/`map` and **DEREFERENCES the results** — translation is *consumed* (per-context
SR reload + MMIO segment-swap), not merely installed. **Where identity breaks (the falsifiable claim S1
must verify against the probe boot):**
- `translate` returning EA==PA is only correct while the NK's intended map is V=P. The moment the
  Trampoline/NK installs a non-identity segment (the `mtsrin` context loop steps EA top-4-bits per 256MB
  segment) OR programs a BAT whose PA≠EA, an identity `translate` hands back a PA the consumer then
  dereferences WRONG → fault.
- `claim` returning the requested address as-granted is plausibly survivable; `map` of a region the
  Trampoline then *reads through* is where a non-identity expectation bites.

**What state the chosen mechanism must honor (NK-programmed, statically pinned, Discriminator-A):**
| NK action | Address | What the `/mmu`/window must reflect |
|---|---|---|
| SR program (unrolled `mtsr 0..15`) | `0x503104b4` | 16 segment registers; 256MB segment granularity |
| per-context `mtsrin` loop | `0x50315290` | EA top-4-bit-stepped segment reload (two-context battery: same EA → two PAs) |
| BAT descriptor program | `0x503152c4` | IBAT/DBAT pairs from the descriptor table; **DBAT (data) must cover the full RAM aperture AND ROM aperture** — the load-bearing predicate for the JIT (data accesses) |
| SDR1 install | `0x50310604` | HTABMASK from RAMSize (closed-form confirmed); HTAB base |
| PTE engine | `0x50319af8` | HTAB stores + `tlbie` — NOT `mtspr`; the window's hook surface does NOT see this (Stop-rule: residual must be BAT/SR-covered for JIT regions, else re-band) |
| SR-swap MMIO | `0x50325894` | `mfsrin`/`mtsrin` around the access — MMIO reached by whole-segment swap (G1.b microtest) |

**Closure predicate:** S1 design must state, for window AND softmmu, exactly how each of the six rows is
honored (window: remap on the `mtspr`/`mtsr`/`mtsrin` hook; softmmu: read live regs + walk). **GREEN** when
all six have a pinned honoring mechanism + the DBAT-covers-RAM+ROM predicate is bound to the harvest DATA.
**Budget:** the findings table above (already pinned); NO new disasm.

---

## Q-S1.3 — THE INTEGRATION SEAM (co-land; reuse, don't rebuild)

The live `/mmu` co-lands with three already-landed/DONE surfaces. The design must specify the WIRING, with
file-ownership serialization, NOT new translation logic:

1. **Replace S2b's `/mmu` recording stub with the real backend.** The OF-CI `/mmu` call-method backend
   lives in `machine/openfirmware_ci.cpp` (**S2b-owned**). The real `claim`/`translate`/`map` calls into
   S1's paged MMU: `translate` → `paged_mmu_translate(...)`; `map`/`claim` → the arena remap (window) or a
   recorded mapping table (softmmu-harvest). **SEAM DECISION owed:** does S1 own a new `machine/paged_mmu`
   backend that openfirmware_ci.cpp CALLS (clean ownership boundary), or does S2b expose a hook S1
   installs? Recommend the latter (callback-install from an S1-allowed file), mirroring the regs-provider
   pattern — avoids the S1↔S2b file collision.
2. **Read S3's live SR/BAT/SDR1.** S3 (REPLACE) makes the NK's `mtsr`/`mtsrin`/`mtspr BAT/SDR1` writes
   STICK in `regs()` (the forge retired). S1 installs a **regs-provider** (callback yielding live
   `sr[16],bat[16],sdr1,msr`) from an ALLOWED file (`ppc-execute.cpp`/`ppc-cpu` init) — **MUST NOT edit
   `sheepshaver_glue.cpp`** (S2/S3). Window hooks fire at S3's live `mtspr`/`mtsrin` sites.
3. **Reuse the DONE mechanism + proven primitive.** `paged_mmu_translate()` (translation core) + the
   SHM-arena atomic `mach_vm_map(FIXED|OVERWRITE)` form (spike-proven) + `test_paged_mmu` battery
   (extend additively for W2 privilege). NO re-derivation of translation; NO new remap primitive.

**Closure predicate:** a seam diagram naming, for each of the three, the OWNING file, the install
mechanism (callback, not direct edit), and the serialization order (S2b lands gate → S3 lands forge-retire
→ S1-live wires the backend + regs-provider + window hook on top). **GREEN** when no seam requires editing
an S2/S3-owned file's logic (only pre-wired hooks). **Budget:** the ownership facts above.

---

## Q-S1.4 — THE PROBE-BOOT FINDING (designed here, RUN by the coordinator — §B)

**The question.** Where does the both-gates-ON chain (9.0.1 ROM) FIRST fault on the missing live `/mmu`?
This is the single most valuable fact for S1's design: it tells us whether the chain reaches the NK
install (→ softmmu-harvest is viable, Q-S1.1 path) or faults earlier at the Trampoline `/mmu translate`
dereference (→ live map still not produced, RESIDUE-PASS / co-land confirmed).

**Falsifiable outcomes (pre-registered):**
- **Outcome A — faults at/after the NK MMU-install** (`mtsrin`@`0x50315290` or a post-install JIT access):
  the live map IS produced → softmmu-harvest re-activated → Q-S1.1 BOTH-in-order recommendation stands;
  the harvest retires the window predicate as DATA. **GREEN-leaning.**
- **Outcome B — faults at the Trampoline `/mmu translate` dereference** (before `0x50310000` NK entry, on
  an identity `[S2B-MMU-STUB]` claim): the live NK map is NOT yet produced end-to-end → INSEPARABILITY's
  co-land holds; S1-live must supply a *minimally-real* `/mmu` (claim/translate/map honoring the
  Trampoline's V=P expectation at that point) BEFORE the NK install is even reachable. **RESIDUE-PASS;
  the fault PC is the next wall.**
- **Outcome C — faults before `[S2B-LAUNCH]`** (loader/asset/ROM problem, e.g. `[S2B-ROM-MISMATCH]` or
  missing staged `MacOS.elf`): a setup defect, not an S1 finding → fix and re-run (does not consume the
  S1 budget).

**Closure:** record the first-fault PC + the last-fired probe + the deepest `[S2B-*]`/`[NK-SUP]` ladder
rung in this Task-0's findings doc. **The WIN is locating the first fault, not booting.**

---

## Env-gate
The live `/mmu` is `SS_M18_PAGED_MMU ∧ MachineProfileIsNewWorld()` (+ regs-provider installed), co-gated
in practice with `SS_M18_TRAMPOLINE` (loader) and `SS_M18_NK_SUPERVISOR` (live supervisor). Structurally
unreachable in paravirtual; JIT bare-RMEMBASE path untouched except by the window remap-at-`mtspr`-time.

## File ownership (serialized, strong tier)
- **S1 edits:** `machine/paged_mmu.{cpp,h}` (W2 privilege), `machine/test_paged_mmu.cpp`; the
  regs-provider + phys-reader + (window) the `vm_remap`/`mach_vm_map(FIXED|OVERWRITE)` routine in
  `Unix/main_unix.cpp`; the `mtspr`/`mtsr`/`mtsrin` hook in `ppc-execute.cpp`; `vm.hpp` (softmmu arm).
- **S1 MUST NOT edit (only install pre-wired hooks):** `sheepshaver_glue.cpp` (S2/S3),
  `machine/openfirmware_ci.cpp` logic (S2b — the `/mmu` backend hook must be a pre-wired install point).
- Serialization: S2b gate → S3 forge-retire → S1-live on top. Never concurrent on the shared files.

## Stop-rule (named in advance)
1. Pick the window on the spikes ALONE, without the harvest coverage DATA ⇒ STOP (un-measured bet).
2. Identity-only `/mmu` when the Trampoline/NK dereferences a real PA ⇒ STOP (two-context false-clean, A3 LAW).
3. `0xDEADBEEF`/sentinel where a real PA/PTE/claim is expected ⇒ STOP.
4. Edit `sheepshaver_glue.cpp`/`openfirmware_ci.cpp` logic to reach regs or the `/mmu` backend ⇒ STOP —
   provider/hook install from an allowed file, or the seam design is wrong.
5. Port the PearPC/QEMU oracle walker into the tree ⇒ STOP (UNLINKED fixture generator only).
6. Run the stalled QEMU NK-install rig to get the live map ⇒ STOP — harvest from S1's OWN interp boot;
   if THAT stalls, RESIDUE-PASS + surface.
7. Unbounded disasm / wholesale parcel disasm ⇒ STOP, conservative residue.
8. Arm/validate live translation against the FORGE state (gates OFF) ⇒ STOP (INSEPARABILITY standing guard).
9. Claim the harvest retires the window predicate from SYNTHETIC data ⇒ STOP — valid only from a real
   gated-ON boot reaching the `mtsrin` landmark.

**One-iteration rule.** Falsified pin → dated entry → ONE bounded re-pin → resume. Second falsification of
the same answer ⇒ STOP, re-plan.

## Self-review — tensions FLAGGED FOR THE RED TEAM
1. **Does the gated-ON boot actually reach the NK install, or fault at the `/mmu` stub first?** This is
   Outcome A vs B and it DECIDES whether softmmu-harvest is viable now (Q-S1.1) or whether S1-live must
   first supply a minimally-real `/mmu` just to let the Trampoline reach `0x50310000`. The probe boot (§B)
   is designed to answer it but the answer is genuinely unknown statically (S2b Q-S2b.4 said "unknowable
   statically").
2. **Softmmu harvest needs `SS_USE_JIT=0`, but does the interp-only boot reach the NK install** (or stall
   like the QEMU rig / hit the VBL-timer hang)? If interp-only can't reach the landmark, the harvest
   vehicle fails and the window's coverage predicate stays owed to a JIT boot (chicken-and-egg).
3. **Window foundation: are the two spikes sufficient** to retire the live-NATMEM overwrite-under-
   concurrency risk, given the spikes used a SYNTHETIC separate arena and the real NATMEM is a single
   in-use reservation with ~68 bare sites + non-CPU threads? (Deferred-window TECHNICAL V4 topology
   mismatch — is it truly retired or merely de-risked?)
4. **Is INSEPARABILITY now stale?** Its conclusion ("live MMU co-lands with S3") may be CONFIRMED-and-
   reached rather than superseded by the gates landing — i.e. S1-live is not a standalone milestone but
   the S1-contribution to the S3 co-land. The red team should rule whether this Task-0 opens a separate
   S1-live-impl milestone or an S3-co-land workstream.
5. **`/mmu` backend ownership** (S1 vs S2b file collision) — is the callback-install seam (Q-S1.3) clean,
   or does it force an S1 edit into `openfirmware_ci.cpp`?

## Red-team voting list (for the binding questions)

1. **Q-S1.1 order (decisive):** "softmmu-harvest first, then window" — correct now that the deferral
   premise flipped, or does the interp-only-reachability tension leave the window the only path
   (softmmu-first dead again)? VOTE: BOTH-in-order / window-only / softmmu-only / re-band.
2. **Probe-boot outcome dependency:** Should Q-S1.1's pick be BINDING before the probe boot, or
   CONDITIONED on Outcome A vs B (the plan currently conditions it)? Is conditioning on an unrun probe
   acceptable Task-0 discipline?
3. **Window foundation closure:** Do the two PROVEN spikes (synthetic separate arena) retire the
   live-NATMEM overwrite-under-concurrency risk for the SINGLE in-use reservation with ~68 bare sites +
   non-CPU threads — or is a real-NATMEM spike still owed as a gate? VOTE: retired / owed-as-gate.
4. **INSEPARABILITY status:** CONFIRMED-and-now-reachable (S1-live = the S1 contribution to the S3
   co-land) or SUPERSEDED (re-openable as a standalone milestone)?
5. **`/mmu` backend ownership seam:** Is the callback-install pattern clean, or does the real `/mmu`
   backend force an S1 edit into S2b-owned files (re-serialization)? VOTE.
6. **Regs-provider soundness:** Does reading S3's live `sr/bat/sdr1/msr` via a provider installed from
   `ppc-execute.cpp`/`ppc-cpu` (not glue) capture the ACTIVE cpu with no reentrancy/cross-thread stale
   hole? Is the TLS `in_interp_translate` guard still needed for the softmmu arm?
7. **Probe ROM/mode correctness:** Is the 9.0.1-ROM pin + JIT-default (with interp-only fallback) the
   right config to expose the FIRST fault — or do the VBL-timer/JIT-bypass caveats produce a misleading
   stall that masks the real `/mmu` fault?
8. **PASS taxonomy honesty:** Is "Outcome B → RESIDUE-PASS, fault PC is the next wall" correct — or is
   faulting at the Trampoline `/mmu` stub a NO-GO for softmmu-harvest, requiring S1-live to deliver a
   minimally-real `/mmu` as its FIRST sub-task rather than a residue?

## Probe-boot DESIGN (coordinator RUNS — §B) + the FINDING

*(The probe-boot invocation + log-ladder are recorded in the coordinator's run note below; the FIRST-FAULT
finding is appended here once the boot is run.)*

### Invocation (slot protocol — never global pkill)
```bash
SheepShaver/tools/ss-slot-boot.sh --label s1-live-mmu-firstfault --timeout 60 \
  --env 'SS_M18_TRAMPOLINE=1 SS_M18_NK_SUPERVISOR=1 \
         SS_PROBE_PC=0x50310000;0x50310604;0x503104b4;0x50315290;0x503152c4;0x50314880'
```
Default template = the NW diagnostic 9.0.1 config (ROM `66210b4f…`). Expected to FAULT (recording `/mmu`
stub + no live remap); the WIN is the first-fault PC + ladder depth. Optional interp-only re-run
(`SS_USE_JIT=0`) tests the softmmu-harvest reachability tension (self-review #2; expect the VBL caveat).

### Log ladder (deepest rung reached)
`[S2B-ROM-MISMATCH]`(→Outcome C) → `[S2B-LAUNCH]` → `[S2B-OFCI]` → `[S2B-MMU-STUB]`(identity claims;
fault here before `0x50310000` = **Outcome B**) → probe `0x50310000` (NK entry / handoff) → `[NK-SUP]`
T3/T4 → probes `0x503104b4`/`0x50315290`/`0x503152c4`/`0x50310604` firing (NK MMU-install executing →
**Outcome A**) → `[EXC-NK] FATAL` / sentinel abort (the fault PC) → `[DR68K] first instruction` (past the
wall). Cleanup: `SheepShaver/tools/ss-reap.sh`.

### FINDING (coordinator run, 2026-06-15, slot0 `20260615-094921`) — **OUTCOME B (refined), RESIDUE-PASS**

**The gated-ON chain RUNS THE REAL TRAMPOLINE for the first time in-tree** — a major end-to-end
validation of S2b-impl + S3-impl. The ladder (EXIT=134, abort at SIGTERM-adjacent):
- `[NK-SUP] T3+T4` all fired — S3 forge-retirement live: SDR1/HTAB forge retired (backing
  `[69000000..69010000)` mapped, SDR1/HTAB-zero left to the NK), boot-MSR forge retired, Execute68k pair
  retired, synthetic supervisor retired (g_exc_entry_table DEC/EXT/SC/PROGRAM nulled; sc/program resolve
  live from KDP+0x390/0x37c).
- `[S2B-LAUNCH] CHRP entry: pc=0x0020f078 r2=0x001001e8 r3=0 r4=0 r5=0x00211000 (shim opcode=0x180019c2)` —
  the loader placed `MacOS.elf` + the launch seam set the CHRP ABI + wrote the shim opcode at r5.
- `[S2B-OFCI] wired of_ci_callback: Core99 DT + 11 backends (/mmu=RECORDING STUB …)`.
- `[S2B-LAUNCH] entering Trampoline at 0x0020f078 (forge entry 0x50310000 bypassed)` — **the `emul_ppc`
  override worked; control entered the REAL Trampoline.**
- The Trampoline EXECUTED — interp/JIT ran its loaded code (`0x00180000…0x001f0000`); heartbeats climbed
  21M→105M blocks over 50s (`iNK=0 iDR=0 iRAM=105M`, `exc=0/…`, `mmio=S:0/V:0`).
- `[EXC-NK] EXT-injection STOP: KDP+0x374 unresolved` ×6 — the T2 EXT-shim correctly hit its sentinel.
- **FATAL: `twi` trap at `pc=0x0000016c`, KDP+0x37c (NK PROGRAM vector) unresolved → STOP (Stop-rule #6).**

**What it means (the refined wall):**
1. **`0x50310000` (NanoKernelEntry) NEVER fired** (`iNK=0`); **NO `[S2B-MMU-STUB]` call ever issued.** The
   Trampoline never reached its OF-CI `/mmu` work and never reached the NK install → **no live
   `(SR/BAT/SDR1)` map was produced.** This is **Outcome B**, not A.
2. **So S1's live `/mmu` is NOT the immediate next blocker.** An EARLIER wall sits in front of it: the
   real Trampoline spins in its own early code (105M blocks, no forward progress, no OF-CI `/mmu` call)
   and then traps `twi @ 0x16c` (a guest assertion/branch-to-low-address, vectoring to the unresolved NK
   PROGRAM handler). **The next action is Trampoline early-bringup, BEFORE S1.**
3. **Candidate causes (for the bringup sub-task, cheap → expensive):** (a) the provisional **r3/r4 = 0**
   CHRP ABI (env-overridable `SS_M18_R3`/`SS_M18_R4` — sweep first, no recompile); (b) whether the
   Trampoline's `bctrl` to r5=`0x211000` actually invokes the EXEC_NATIVE shim (no `[S2B-MMU-STUB]`
   suggests the OF-CI path is never entered — the shim opcode/invocation needs a probe); (c) an early
   Trampoline dependency (a memory/device value) that spins.

**Disposition: RESIDUE-PASS** (the honest expected case). The restructure (S2b+S3) is VALIDATED running
end-to-end; the live map is not produced; **the next wall is Trampoline early-progress (r3/r4 / r5-shim
invocation / early-init), pinned at `twi @ 0x16c`, which precedes S1's live `/mmu`.** S1-live remains
owed but is gated behind this earlier bringup. Re-run candidate: `SS_M18_R3`/`R4` sweep + an
`SS_PROBE_PC=0x211000` (shim entry) / `0x20f078` (ELF entry) trace to see how far early init gets and
whether r5 is ever called.

**Follow-up probe (2026-06-15, slot0 `20260615-095355`):** `SS_PROBE_PC=0x20f078;0x211000;0x20dbec;`
`0x20dcc0;0x20ddb4;0x21024c` (ELF entry + shim entry + the OF-CI call wrappers + indirect glue). **NONE
fired** — the boot runs **interpreter** (`i2j=0`, all `iRAM` interp blocks), and `SS_PROBE_PC` hooks JIT
block-entry, so it cannot observe interp-only execution; AND the Trampoline is spinning in
`0x180000–0x1f0000` without reaching `0x20f078`/the OF-CI wrappers as block entries. **Refinement:** the
Trampoline is stuck in EARLY code (`0x180000–0x1f0000` = inside its loaded exec segment) — likely PIC
self-relocation / an early clear/copy loop / a spin on an uninitialized value — BEFORE it ever calls r5
or reaches the NK. **The next wall is a dedicated Trampoline-early-bringup RE** (interp PC trace, not
SS_PROBE_PC which is JIT-only; or a targeted instruction dump of the `0x180000` spin region), which
PRECEDES both S1's `/mmu` AND the r3/r4/r5-shim questions. This is a NEW focused task, not S1-impl.

**Bringup RE (2026-06-15, capstone disasm of staged `MacOS.elf`) — the wall is OF-CI ENVIRONMENT FIDELITY,
pinned one layer deeper:**
- **The launch path is fully CORRECT** (validates S2b T1–T5 deeply): entry `0x20f078` stub → `r2=0x1001e8`
  from embedded const → stack `r1=0x116f30` (valid BSS) → CFM glue `0x21024c` `bctr` → **main `0x204d54`**;
  main stashes r5 (`0x211000`) into BSS scratch and calls the OF-CI wrapper `0x20dbec` within ~40 insns.
  The wrapper `bctr`s through `0x21024c` to `0x211000` = the shim opcode `0x180019c2` (decoded: EXEC_NATIVE
  selector `0x27`=NATIVE_OF_CI_SHIM, FN=1 → `pc()=lr()` correct function return). **OF-CI IS entered;** the
  early `finddevice`/`getprop` calls are serviced silently (the `[S2B-*]` logs only fire on refusal/`/mmu`).
- **The spin cause:** the Trampoline runs the classic CHRP boot-setup (`finddevice /chosen`→`getprop
  bootpath`→`getprop /memory reg`→…→AddMemoryRelocationEntry/RelocationEngine→`claim`). But the
  consequential OF-CI ops are S2a-scope STUBS in `machine/openfirmware_ci.cpp`: **`claim`/`read`/`seek`/
  `write` return `rets[0]=0`**, **`/memory reg` size=0** (`REG_MEMORY` base `0x10000000` size `0`, :150),
  **`bootpath`=0** (:183). The Trampoline consumes `claim()=0` as an allocated base → relocates/jumps to a
  zero-derived low address → wild execution through the unmapped `0x180000–0x1f0000` zero gap → `twi @
  0x16c`. This is BEFORE any `/mmu` call-method (hence `iNK=0`, no `[S2B-MMU-STUB]`).
- **`0x180000–0x1f0000` = the gap between BSS-top `0x119920` and exec-base `0x200000`** — covered by no
  PT_LOAD, uninitialized zero RAM; the loader correctly never touches it. Execution there = wild jump, not
  legit code (consistent with `exc=0`/`mmio=0`).
- **r3/r4 sweep is LOW value** (the wall is downstream of OF-CI `claim`/`/memory`, not the CHRP entry ABI).
- **CHEAPEST UNBLOCK (S2b-owned `openfirmware_ci.cpp`, gated, NOT S1):** (1) a real **`claim` bump-allocator**
  (honor non-zero `args[0]`, else hand out page-aligned addrs from a reserved arena) — the single
  highest-value fix (stops "allocate at 0 → jump to 0"); (2) **`/memory reg` size = real `RAMSize`** (+
  `available`); (3) `bootpath` + minimal `open`/`read`/`seek` (heavier — defer until a trace shows it's
  needed). **Diagnostic first:** instrument `of_ci_callback` to log every `(service, args, rets[0])`, boot
  gated-ON once, read the last calls before the spin (predict: `getprop /memory`/`claim`→0 immediately
  precede the wild jump). **NEXT TASK = OF-CI early-environment fidelity, which precedes S1's `/mmu`.**
  *(High confidence the wall is the OF-CI environment + before `/mmu`/NK; medium on which stub fires first —
  the trace resolves it cheaply.)*

### Relocation-loop RE (2026-06-15, capstone `0x2026a0`) — the wall after minimally-real /mmu
The SIGSEGV loop is `0x202698–0x202704`, a **range-coalesce / relocation-apply loop**:
- indexed by `r29` (stride +8) against a COUNT at `0xbc(r1)`; `r31` = output cursor (8-byte entries:
  `[r31+0]`,`[r31+4]`); `r30` accumulates a running total (`r30 += 0xcc(r1)` stride per iter); merges
  adjacent ranges (`r0=[r31+0]+[r31+4]; cmpw r0,0xc8(r1); bne → stw r8,8(r31)` = the faulting store).
- **Over-run mechanism:** the loop count (`0xbc(r1)`) / the range-entry stream is derived from the OF-CI
  memory map we publish (`/memory reg` = a SINGLE entry, size=RAMSize, base `0x10000000`). The Trampoline's
  range engine expects a richer structure (likely multiple `/memory` cells + an `available` property +
  reg/available consistency), so the count is wrong → `r29 < r4` stays true while `r31` walks `0x01000000 →
  0x01fffff8 →` off the end → SIGSEGV. `r30=0x3e18696d` is the runaway accumulated total, not ASCII.
- **NEXT FIX (OF memory-map fidelity):** make `/memory` (reg + `available`) + the `/chosen` memory props
  match what the CHRP range engine consumes (trace back from `0xbc(r1)`/`0xc8(r1)`/`0xcc(r1)` to the OF-CI
  getprop that fills them). Precedes the NK install. This is the next iterative bringup wall (Route A's
  "MONTHS" grind: each OF-CI/ABI input refined in turn until the Trampoline reaches `0x50310000`).

### Wall RE (2026-06-15) — `0x2031e4` is interrupt-controller setup, NOT S1 /mmu (Hypothesis A)
The fault at `pc=0x2031e4` is inside `0x202c58`, the NewWorld **interrupt-controller / interrupt-vector
initializer** (proven by its format strings `"SCSIIntVect"`/`"SCCAIntVect"`/`"VIAIntVect"`/`"ADBIntVect"`/
`"NMIIntVect"` + the sibling `"Can't add vectorlookuptable/vectormasktable/CascadeInfo/VectorPriorityTable
memory relocation entry"`).
- `r19 = [r2-0x98]` (TOC global, guest `0x11703c`) → `G = [r19]` = the **primary IC descriptor**.
  `G->[0x18]` = device handle (non-null, passes the guard); `G->[0xa8]` = the **vectormasktable /
  vector-lookup-table pointer**. The fault is `lwz r30,0(r24)` with `r24 = G->[0xa8] = 0xffffffff`.
- `ciMapRange(size=0x30000, virt=0x10000)` (at `0x20e160`) IS the `virt 0x10000 → phys 0xf3040000` MacIO
  map (mode 0x2a) in the log; `bl 0x207fe8(arg=-1)` is a device-register-configure helper (the `-1` is a
  hardcoded polled/no-interrupt call-site flag, NOT an error return).
- **Decisive: `G->[0xa8]` is a FIXED `0x68xx_xxxx` low-mem-mirror constant** set by the descriptor
  constructor `0x20c094` (`stw 0x68080000 → [r31+0xa8]`) / `0x205b28` (`[r26+0xa8]=0x68fefcfc`) — completely
  independent of the MMIO aperture's host backing. **Even a perfect S1 live `/mmu` leaves `G->[0xa8]==-1`.**
  → Hypothesis B (live host remap) RULED OUT.
- **Ordering smoking gun:** in `main`, the consumer `0x202c58` runs at `0x205750` but the descriptor
  constructor `0x20c094` runs LATER at `0x205784` — so the primary IC descriptor at the TOC global is
  expected **pre-initialized before `main`** (inherited from the preceding NewWorld boot environment's
  ConfigInfo / vector tables). In our synthesized OF-CI environment it's uninitialized → `0xa8 == -1`.
- **No fix landed (honest):** seeding `G->[0xa8]=0x68080000` only moves the fault to the empty
  vector/mask table at that address — it needs the actual tables, not just the pointer (Stop-rule #2/#3).
- **NEW WALL (precedes NK install + S1):** reproduce the inherited **primary interrupt-controller
  descriptor + its vector/mask/cascade/priority tables** in low memory (the "*memory relocation entry*"
  family) that the Trampoline expects pre-populated. **Next RE:** trace where the TOC global `[r2-0x98]`
  (`0x11703c`) descriptor + its `0xa8` table are meant to be seeded (an earlier constructor/ConfigInfo the
  boot environment provides). OF/device-environment fidelity, gated, NON-ACCEPTANCE.

### STRATEGY PIVOT (2026-06-15, user-directed) — QEMU-spec'd real environment, not reactive stubs
The wall-by-wall stub-walking kept producing subtly-wrong stubs (translate flag, map arg-order) that
surfaced walls later, and is now re-deriving public IEEE-1275/Apple-OF semantics by RE. **Pivot:** use
QEMU/OpenBIOS as a STRUCTURAL + SEMANTIC oracle (NOT an address oracle — LAW: QEMU MacIO `0x80000000` vs
ours `0xF3040000`; the Cuda IFR/IER runtime is the known gap QEMU misleads on) to spec the REAL OF/device
environment, backed by the existing Machine Layer device models (`dev_openpic` behind `SS_NW_PIC` @
`0xF3040000`, `dev_via6522`, `dev_cuda`), validated against the RE'd consumption contract. Keep the real
OF-CI infra we built (loader, dispatcher, marshalling shim, `claim`, `/chosen` ihandles); CONVERT the
stubs (the IC node's `interrupt-map`/`-mask` Q-S2a.3 PROVISIONAL placeholders → real; `/memory`/DT → real)
to the real thing. **User guardrails (BINDING):** (1) **PROVE THE LINK** — recon must show that populating
the real OpenPIC node + interrupt-map actually causes the guest to build the NK IC ConfigInfo at TOC-global
`0x11703c` / the vectormasktable at `0x68080000`, NOT a second hand-off seam (don't wire a beautiful IC
node and find the ConfigInfo still empty a wall later — the exact failure this pivot exists to stop). (2)
QEMU = node/property shapes + IRQ semantics ONLY; map QEMU PCI addrs → our fixed `0xF3040000`. (3) the
consumption contract is both spec AND bounding box — build real only for what the Trampoline genuinely
consumes, not a full Core99 machine. (4) **S1 live `/mmu` is OUT OF SCOPE** (the IC pointer is a fixed
`0x68xxxxxx` constant, NOT translation-dependent — `/mmu` stays as-is; live MMU defers to S3 with explicit
GO). (5) keep the IC node / `SS_NW_PIC` gated, paravirtual byte-identical, test-jit=100; don't land a
speculative seed.

**IC-descriptor RE (feeds the design):** the primary IC descriptor `G` is runtime-allocated in BSS, planted
at `[0x11703c]`, ≥0x364 bytes; int-vector NUMBERS runtime-copied from a source struct `[r17+0x30xxxx]` (not
image constants); `G->[0xa8]` = vectormasktable ptr → a POPULATED table (count word + 0x148-strided
records) built by the inherited vectorlookuptable/vectormasktable/CascadeInfo/VectorPriorityTable
memory-relocation subsystem. In the real boot `G->[0xa8]` is already valid at `0x205750` (consumer) — set
by an earlier inherited phase, BEFORE the constructor `0x20c094` (which builds a CHILD/cascade, not the
primary). Image int-vector names: ADB/NMI/SCCA/SCCB/SCSI/VIA IntVect (6). **Open unknowns for the recon:**
the int-vector NUMBER values (src `[r17+0x30xxxx]`), the 0x148-strided record format, `G->[0xa8]` =
`0x68080000` (child) vs `0x68fefcfc` (primary `0x205b28`). Donor: the OpenPIC/IC ConfigInfo path in
DONOR-NOTES.

### QEMU-spec recon (2026-06-15) — LINK NOT PROVEN: SECOND SEAM (the pivot's guardrail #1 paid off)
**A real IC `interrupt-map` does NOT move `0x2031e4`.** Direct register evidence (gated-ON boot slot0
`20260615-120728`): `r19=0x11703c`, `G=[r19]=0x01183000` (PRIMARY descriptor), `r24=[G+0xa8]=0xffffffff` →
deref-trap. But the descriptor `0x202c58` builds has `+0xa8 = a fresh claim 0x1587000` — a DIFFERENT
instance. So `G=0x01183000` was planted by an EARLIER inherited phase whose `[0xa8]` masktable was never
built. The IC node IS read (finddevice `/…/interrupt-controller@40000`→phandle 0x10 + full nextprop walk
incl. `interrupt-map` len 0x1c / `-mask` len 0x8 = our placeholders), and IC-init `0x202c58` claims its
tables successfully (backed RAM, V=P translate). **But the int-vector NUMBERS are IMAGE constants** (memcpy
0x40B from TOC `[r2-0x78]` via `0x20fe70`), NOT OF-derived. **Conclusion: the IC node is the CONTENT layer
(necessary-not-sufficient); the immediate wall is the unbuilt `G->[0xa8]` vectormasktable owned by the
inherited "memory-relocation entry" table-builder — a SEPARATE build.** Caught before a speculative IC node
was landed (exactly the failure the pivot prevents).

**Real IC-node design (build it as the content layer, QEMU shapes + OUR addrs):** keep
`/pci@f2000000/mac-io@c/interrupt-controller@40000` `device_type=open-pic` `compatible="chrp,open-pic"`
`reg=(0xF3040000,0x40000)` `#interrupt-cells=2`; add the presence-only `interrupt-controller` marker; leaf
`interrupts=(input,sense)`+`interrupt-parent=<0x10>`. **IRQ numbers (CORE99 §2 / QEMU semantic oracle, OUR
0xF3040000 OpenPIC):** SCCA=0x25, SCCB=0x24, VIA=0x19 (confirmed); ADB(rides Cuda/VIA), SCSI(MESH), NMI(GPIO)
= RESIDUE (cross-check DingusPPC/Apple — QEMU-only values; Cuda IFR/IER is the QEMU-misleads gap, an S4
concern). Backed by `dev_openpic` (already raises inputs by number @ `0xF3040000`). **vectormasktable
format:** `{ u32 count; record[count] each 0x148 bytes }` (from the `0x2031e4-0x203204` loop).

**▶ THE DECISIVE OPEN QUESTION (settle by disasm BEFORE any build — red-team items 1+3):** is the masktable
builder (a) a REACHABLE Trampoline function we're not reaching because an EARLIER input (ABI/claim/an OF
prop) is wrong → UNBLOCK it (cheap + genuinely real), or (b) genuinely INHERITED boot state we must
REPRODUCE? Settling disasm: trace the writer of TOC `[0x11703c]` / the phase that claims `0x01183000` +
where `G->[0xa8]` is meant to be set; check whether its masktable-fill reads phandle-0x10 `interrupt-map` or
only the image const `[r2-0x78]`/`[r17+0x30xxxx]`. This decides reproduce-vs-unblock + the build order.

### SETTLING DISASM (2026-06-15) — VERDICT: (a) UNBLOCK, not reproduce. "Inherited boot state" FALSIFIED.
Direct evidence: `main` (0x204d54) itself claims G (`0x2051cc bl claim(0xc0) -> 0x1183000`), plants
`[0x11703c]=G` (`0x2051d4 stw`), memsets it (`0x205200`, so `G->[0xa8]=0`). The consumer `0x202c58` builds
its OWN masktable: `0x202cd0 claim(0x204)`, fills it, then `0x202d08 bl 0x20e05c` = OF **call-method
"translate"** on the `/chosen` mmu ihandle (marshaller `0x20dcc0`, n_in=1 **n_out=3**) with `r4=&G->[0xa8]`
— it translates the mask-buffer virt and writes the result into `G->[0xa8]`. **No inherited table exists;
the guest claims+builds all four tables itself** (the `vectormasktable`/lookup/cascade/priority store sites
`0x205a34`/`0x20c1a0` all run AFTER the consumer). WATCHPOINT PROOF: `[WATCH] pc=00202d18 addr=011830a8
value=ffffffff (was 0)` — `G->[0xa8]` goes 0→0xffffffff exactly at the translate call; guard
`[WATCH] addr=01183018 value=f3040000` (G->[0x18]=MacIO handle) enters the deref path → `0x2031e4
lwz r30,0(0xffffffff)` traps.

**ROOT CAUSE = our `/mmu translate` CALL-METHOD out-vector arity.** Apple-OF `translate ( virt -- false |
phys mode true )`; the guest's call-method marshaller (`0x20dcc0`) distributes **n_out=3** to the caller's
out-pointer, expecting the cell that lands in `r4=&G->[0xa8]` to be **phys**. Our stub computes the right
phys (`translate virt=0x1587000 -> phys=0x1587000 mode=0x12 V=P`) but emits **n_out=4** (the out[3]
success-flag we added in `07003fa1` for the DIRECT translate path) — which MISALIGNS the cell, so
`G->[0xa8]` receives the `-1`/flag, not phys. **FIX (UNBLOCK, small, real — OF-CI contract fidelity, NOT
S1 live-MMU):** the `/mmu translate` backend must return the success out-cells in the layout each
marshaller expects — for the call-method path, the cell routed to the caller's out-pointer must be `phys`
(V=P → `phys=virt`), with `{phys, mode, true}` in the right order/arity; the direct-translate path keeps
its `( virt -- false|phys mode true )` layout. VERIFY: watch `011830a8` settles to `0x1587000`, `0x2031e4`
no longer traps. (The real IC node from the QEMU recon is still worth building as the content layer, but is
NOT what this wall needs.)
