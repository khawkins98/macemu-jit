# NewSheep — S3 Task-0: two-supervisor reconciliation — the architecture VERDICT

**Status:** COMPLETE (2026-06-15) · READ-ONLY static recon + design. Closes the S3 Task-0 deliverable
(`docs/superpowers/plans/2026-06-15-ss-m18-s3-two-supervisor-task0.md`, rev-2). Writes NO `src/**`,
runs NO builds, spends NO boots (see §5 for why the owed probe is not gettable now).

**Inputs consumed:** `FINDINGS-s3-nk-ownership.md` (NK = sole resident owner; co-residency impossible),
`FINDINGS-s3-ss-reusemap.md` (the SS-side KEEP/RETIRE/YIELD map + 3 collisions), `FINDINGS-trampoline-re.md`
Q1 (resident supervisor, no handoff), `FINDINGS-discriminator-a.md` (NK MMU = SR/BAT/SR-swap; QEMU stalls
pre-install), the SHM-arena spike (commit `acd89dce`, PROVEN-for-construction),
`HANDOFF-NEWWORLD-SUPERVISOR-MMU.md:299-313` (the `sc` double-increment).

**Provenance re-verified (gate-item-0):** NK-v02.27 md5 `61c176e90b6365e84e5c660d703e56af` (105280 B);
canonical ROM md5 `66210b4f71df8a580eb175f52b9d0f88`. (Carried from the two committed findings; not
re-hashed here — no asset access required for the marginal Task-0 work.)

> **Gating honesty (rev-2 minor d).** Q-S3.1 and Q-S3.2 are already LARGELY CLOSED by the two committed
> findings. This addendum performs the MARGINAL remaining work the plan names: the **M3 static
> seam-necessity discriminator** (Q-S3.3), the **INFEASIBILITY test** with the M2 positive-evidence rule,
> the **`sc` sub-wall disposition**, the **conditional live-MMU co-land spec**, and the honest record that
> the **≤4-boot positive corroboration is OWED to S3-impl's own boot** (not gettable now). The grade rests
> on those, not on re-deriving the settled ownership findings.

---

## 1. Q-S3.3 — the architecture PICK (static seam-necessity discriminator)

### Candidate menu (CO-OWN / yield-fully scored OUT with evidence)

| Candidate | Hosts resident supervisor? | Verdict |
|---|---|---|
| **CO-OWN** | — | **SCORED OUT.** `FINDINGS-s3-nk-ownership.md`: the NK never `blr`-yields to a host caller (Q1 install-then-resident; every exception body returns via `rfi`). SS cannot co-own a supervisor the NK never delegates. Recorded scored-out, not silently dropped. |
| **yield-fully** | — | **SCORED OUT.** Q1 no-yield-point: there is no observed seam where the NK hands control back for SS to "fully yield" into. |
| **REPLACE** | YES | Live candidate (front-runner). |
| **thin-shim** | YES (in principle) | Live shortlist alternative. |

The decision reduces (per the plan's folded input) to **REPLACE vs thin-shim**.

### The discriminator (rev-2 M3): STATIC SEAM-NECESSITY — not G3.a

G3.a (`SS-scheduler-ticks=0 while NK live`) **cannot** tell REPLACE and thin-shim apart — both satisfy it.
The falsifiable discriminator is **whether the `sc`/ECB/Execute68k seams stay LIVE**:

- **thin-shim** REQUIRES live shim seams: it interposes at the lone 68k-entry `bctr`@`0x5031a8b8`
  (`mtctr r9; bctr`, r9 runtime-loaded; `FINDINGS-s3-nk-ownership.md` Q-S3.1) and adapts `sc`/ECB/Execute68k
  so SS-as-hardware mediates each NK->68k / NK->syscall crossing.
- **REPLACE** RETIRES those seams: the NK owns EXT/SC/DEC/PROGRAM live, reaches 68k via its own ECB/KDP
  task dispatch through `bctr`@`0x5031a8b8`, and SS's `execute_68k` / `g_exc_entry_table` / `sc`-as-illegal
  go dead under the gate.

### Scoring (the 6 axes the plan names)

| Axis | REPLACE | thin-shim |
|---|---|---|
| (i) **hosts a resident translated-mode PPC supervisor** | YES — NK `rfi`s @`0x5031003c`, owns the live vectors | YES, but only behind a mediation layer SS must keep correct |
| (ii) **reuses the gated arm** (`ppc-cpu.cpp:1991`, B4) | YES — toggles the EXISTING newworld arm; retires `SheepExcDeliverPending` at the `:2027` call-site | YES for the gate, but ADDS a standing shim that must live forever at `bctr`@`0x5031a8b8` |
| (iii) **live-MMU co-land** | NK programs SR/BAT/SDR1 live; SS hosts the regime (S4) — same deliverable | identical MMU substrate, but the shim adds an extra translated/host boundary per seam |
| (iv) **`sc` path** | NK's real SC handler `0x50314ac0` (`-1/-2/-3` fast-traps) becomes live; SS `sc`-as-illegal retires under the gate (S3) | SS must intercept + re-inject `sc` at the shim — keeps the dead `execute_illegal` path alive and adapted |
| (v) **paravirtual-provably-unreachable** | clean: `MachineProfileIsNewWorld()==false` audited at the gate site -> forge path untouched for paravirtual | harder: the shim seams sit on a path paravirtual could also touch; provable-unreachability is muddier |
| (vi) **effort** | retire-the-forge restructure (bounded: the reusemap's RETIRE/YIELD set) | the same retirement PLUS authoring + maintaining live shim seams (strictly more surface) |

### PICK: **REPLACE** (single architecture; thin-shim retired, not shortlisted)

REPLACE dominates on every axis. The NK-ownership finding makes the shim seams thin-shim depends on
**unnecessary**: there is no NK->host call-out for a shim to mediate (Q1 no-yield), and the NK already
reaches 68k by its own runtime-pointer dispatch (`bctr`@`0x5031a8b8`), so interposing SS-as-hardware there
adds a standing boundary with no reuse payoff. thin-shim is the strictly-larger-surface variant of the
same retirement, and its only differentiator (live seams) is the thing REPLACE correctly deletes.

This is a **single PICK, not a <=2 shortlist** -> it clears the rev-2 M3 bar without owing a deferred
S3-impl discriminator gate. (The static seam-necessity discriminator IS named above; it resolves at recon
in REPLACE's favor rather than being deferred.) Were a future finding to resurrect thin-shim, the named
discriminator (do the `sc`/ECB/Execute68k seams stay live?) is the falsifiable S3-impl test.

### SS-side reuse map carried (from `FINDINGS-s3-ss-reusemap.md`)

- **KEEP:** `exc_core.cpp` ExcEnter (`:12`) / ExcRfi (`:104`) / ExcDeliveryDecision (`:58`) — the
  hardware-vectoring mechanism + PEM masks = LAW (NK reuses the semantics; touching a mask line trips the
  re-plan stop-rule). KEEP the bare `ppc_cpu` core (`glue:1683`) + the M0–M13 device models beneath.
- **RETIRE (under the gate):** `deliver_pending_dec_exception()` (`glue:1122` — the synthetic supervisor
  body), `g_exc_entry_table` (`glue:142`, Collision 1), `execute_68k` + the Execute68k pair (`glue:1461`,
  forged at `:3140-3141`, Collision 2), the SDR1/HTAB/MSR forge (`glue:2806/2804/2918`), the
  `SS_NW_TRAMPOLINE` reg-fixup forge (`rom_patches.cpp:716`).
- **YIELD:** the SS scheduler — `virt_clock.cpp:108 VirtClockDECPending` (advisory), `:71
  VirtClockWriteDEC` (suppress host callback), `event_sched.cpp:66 process_timers` (quiescent). The NK's
  DEC loop `0x50313200` (re-arms its own decrementer @`mtspr 0x16`/`0x50313234`) is the live rescheduler.
  Falsifiable predicate: **SS-scheduler-ticks=0 while NK live** (G3.a).

---

## 2. The INFEASIBILITY test (rev-2 M2 — VERDICT only on POSITIVE no-seam evidence)

**Predicate:** for each surviving candidate, is there a concrete seam set (exception vectoring + DEC + 68k
dispatch + `sc` + live MMU install) under which the NK runs resident WITHOUT SS forging an output?

**Applied to REPLACE — the seam set EXISTS (positive evidence):**

| Seam | Concrete mechanism (statically pinned) | No-forge basis |
|---|---|---|
| exception vectoring | `exc_core.cpp` ExcEnter/ExcRfi SURVIVE as the hardware-vectoring mechanism; the NK installs the live handlers into its KDP+0x360 vector table at boot | SS substitutes nothing — it vectors; the NK decides |
| DEC / scheduler | NK DEC loop `0x50313200` re-arms its own decrementer (`mtspr 0x16`@`0x50313234`); SS scheduler YIELDs | the gated arm `ppc-cpu.cpp:1991` (B4) already exists — S3 toggles it; no synthetic DEC delivery |
| 68k dispatch | NK runtime-pointer dispatch via `bctr`@`0x5031a8b8` (ECB/KDP task dispatch) | the SS Execute68k pair is provably SS-forged (`glue:3140-3141`) and RETIRES; the NK's own door is live |
| `sc` | NK real SC handler `0x50314ac0` (`-1/-2/-3` fast-traps) | SS `sc`-as-illegal retires (S3) |
| live MMU install | NK programs SR/BAT/SDR1 (`FINDINGS-discriminator-a.md` Evidence A: `mtsrin`@`0x50315290`, BAT@`0x503152c4`, SR-swap MMIO@`0x50325894`) | the forge (`glue:2806/2804/2918`) retires; the NK's real writes stick |

Because REPLACE has a concrete no-forge seam set, **S3-supervisor is NOT infeasible.** The
INFEASIBILITY-VERDICT is **NOT emitted** — and correctly so per M2 (it may fire ONLY on positive evidence
that no seam set exists for ANY surviving candidate; this is the opposite — positive evidence that one DOES
exist for the front-runner).

**The REAL risk is NOT the supervisor menu.** Per the rev-2 Carried-risk re-aim: supervisor reconciliation
is well-pinned (REPLACE clearly hostable, B4). The carried risk is the live paged-MMU's **multi-entry
transactional atomicity** — the NK's `mtspr`-storm does multi-BAT table updates as a SET, and the SHM-arena
spike proved only the SINGLE-remap primitive (construction), not atomic-map-a-SET under live CPU-thread
access. That is a **carried residue (S4), NOT an infeasibility** — it gates S3-impl START, not the
architecture verdict (rev-2 M4).

---

## 3. The `sc` sub-wall disposition

**The Path A double-increment** (`HANDOFF-NEWWORLD-SUPERVISOR-MMU.md:299-313`): in SS today, PPC `sc`
@`0x50324fd8` is a complete no-op — `execute_syscall()` -> `execute_illegal()` (`ignoreillegal=true`:
`increment_pc(4); return;`) -> then `execute_syscall` does its OWN `increment_pc(4)` = **PC += 8**, skipping
the `cmpwi r3,0` and testing stale CR0 -> the NK idle task dead-loops.

**Under REPLACE:** the NK's real SC handler `0x50314ac0` (the `-1/-2/-3` fast-trap vector; restores
SRR1/LR from SPRG1/2, returns via `rfi`; `FINDINGS-s3-nk-ownership.md` Q-S3.2c) becomes LIVE, and SS's
`sc`-as-illegal path **retires under the gate**. The double-increment latent bug becomes moot on the
newworld path (the instruction is no longer routed through `execute_illegal` once the real vector owns it).

**Disposition — NAMED S3-impl sub-task, NOT in-scope-closed at Task-0.** Per Stop-rule #8: fixing the
double-increment alone is NOT closure (it would still not run the NK's handler). The closure is "make NK SC
`0x50314ac0` live (vector ownership under the gate) AND retire SS `sc`-as-illegal" — a named S3-impl
sub-task. Residue tag if it lands incomplete: **"sc-delivery-gap -> named S3-impl sub-task."**

---

## 4. Conditional live-MMU co-land spec (rev-2 minor c — DEFERRED, does NOT block GREEN-PASS)

A DEFERRED CONDITIONAL spec: a decision tree keyed on the SHM-spike state (now PROVEN-for-construction;
multi-entry transactional atomicity OWED) + the NK-table shape observed at first probe. It cannot be
decided at recon (the tables only exist once the architecture is built) — so it is **BARRED from blocking
the supervisor GREEN-PASS** and tracked as a separate residue gating S3-impl START.

**Decision tree (keyed on first-probe observation under the real NK):**

1. **Substrate (settled-for-construction):** the SHM-arena spike (commit `acd89dce`) proved the
   single-remap primitive on macOS arm64 (SHM segment + aliased RW views + 16KB host-page granularity +
   safe overwrite-under-reader). -> the window approach inherits a de-risked substrate.
2. **NK working-set shape (Discriminator-A, COARSE-settled):** the NK maps via SR (256MB segments,
   `mtsrin`@`0x50315290`) + BAT (>=256MB blocks) + SR-swap MMIO; HTAB is RAM-proportional but RESIDUAL.
   -> **front-runner = the window / Dolphin separate-arena approach** (Discriminator-A path a), because the
   NK working set is BAT/SR-class (the JIT fast path is preserved), NOT dense 4KB PTEs.
3. **Atomicity branch (the OWED decision, O4):** IF a first-probe / the follow-on multi-entry-transaction
   spike shows the NK depends on atomic map-a-SET (multi-BAT updates under live CPU-thread access) ->
   **port the window with the ATOMIC one-step `mach_vm_map(VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE)` per-region
   form** (spike-proven hole-free), **NOT Dolphin's unmap-then-map** (transient-unmapped fault window).
4. **Fallback:** IF the oracle/probe shows NK depending on dense mixed-perm 4KB PTEs for JIT-covered
   regions -> fall back to softmmu and re-band S1-live.

**Owed (carried by reference):** the spike's O1–O6 (esp. O4, the map-a-SET / mixed-perm 4×4KB fork) — the
next S3 platform-risk item (multi-entry-transaction spike), scheduled BEFORE the live window is built.
Residue tag: **"live-MMU-substrate UNKNOWN -> platform-risk."**

---

## 5. Probe-boot corroboration — the OWED residue (honest limit)

The positive-probe corroboration (gate G3.c: observe the NK loading r9 and dispatching through
`bctr`@`0x5031a8b8`; G3.a: NK as live DEC rescheduler; the `sc` fast-trap servicing) is **OWED to
S3-impl's own boot, NOT gettable now.**

**Evidence the probe is not gettable:** `FINDINGS-discriminator-a.md` Evidence B — two QEMU boots
(`qemu-rig.sh --gdbstub -S`, 9.0.1-ROM swap) showed the **NK MMU-install cluster never executed**: hw bps
at `0x50310604`/`0x503104b4`/`0x50310000` never fired; SDR1 stayed at OpenBIOS's `0x1f80003f` throughout;
the boot stalls in OF->OS handoff before any NK-equivalent install. This is the S1 G1.e exposure the plan
flags (runbook caveat / tension #3): **the architecture pick is owed a probe the rig cannot give.**

**Decision: NO boots spent here.** A third/fourth boot would recur the same OF-handoff stall (boots 3 & 4
were already reserved-unused in Discriminator-A for this exact reason) — spending them buys no signal and
the <=4 cap is shared program budget. Per the task card: "if a boot is attempted and stalls, THAT is the
residue" — Discriminator-A already attempted it and recorded the stall, so the residue is established
without re-spending. **Residue: Q-S3.1/.2 positive-probe corroboration (G3.a/G3.c) is OWED to S3-impl's
own boot, once the real NK install runs (the same boot S3-impl produces).** This does not weaken the
architecture pick — the pick rests on the static seam set (S1–S2), which is conclusive on mechanism; the
probe is corroboration, not the load-bearing evidence.

---

## 6. VERDICT

**GREEN-PASS** (supervisor architecture pinned).

- **Architecture PICKED: REPLACE** (single architecture — thin-shim retired via the named static
  seam-necessity discriminator, not carried as a shortlist; CO-OWN / yield-fully scored OUT with evidence).
- **Infeasibility cleared:** REPLACE has a concrete no-forge seam set (S2); INFEASIBILITY-VERDICT NOT
  emitted (and would be a stop-rule violation to emit — positive evidence points the other way).
- **`sc` sub-wall dispositioned:** named S3-impl sub-task (make NK SC `0x50314ac0` live + retire SS
  `sc`-as-illegal under the gate); fixing the double-increment alone is explicitly NOT closure.
- **Live-MMU co-land:** conditional spec recorded (window / Dolphin separate-arena front-runner, atomic
  one-step `mach_vm_map(FIXED|OVERWRITE)` form); DEFERRED, does NOT block GREEN-PASS (rev-2 M4 — verdict is
  INDEPENDENT of the spike).

**Per rev-2 M4 the supervisor verdict GREEN-PASSes INDEPENDENTLY of the SHM-arena spike.** The biggest
**carried residue** (separately tracked, gating S3-impl START — NOT the architecture grade) is the **live
paged-MMU MULTI-ENTRY TRANSACTIONAL ATOMICITY** (the NK `mtspr`-storm's atomic multi-BAT map-a-SET, owed
list O4) — de-risked by the next-scheduled multi-entry-transaction spike. Secondary residue: the
positive-probe corroboration (S5) owed to S3-impl's own boot (the rig stalls pre-NK-MMU-install).

**Unblocks:** S3-impl architecture (REPLACE under `SS_M18_NK_SUPERVISOR` AND `MachineProfileIsNewWorld()`,
default OFF; env-on-first, flip-LAST, revert-on-red = BRANCH revert). **S3-impl START additionally gated**
on: S2 landed + the multi-entry-transaction (atomicity) spike.
