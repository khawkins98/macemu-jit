# SS_M18 Stage 3 — two-supervisor reconciliation: S3-IMPL (REPLACE + EXT-injection shim) — IMPLEMENTATION milestone

> **Status:** rev-1 DRAFT (2026-06-15) — awaiting red-team. **This is the S3-impl build plan.** It
> executes the BINDING architecture verdict from `FINDINGS-s3-two-supervisor.md` (GREEN-PASS): **REPLACE +
> a permanent, bounded host→NK EXT-injection shim**. It writes `SheepShaver/src/**` under a default-OFF
> master gate, retires SS's synthetic supervisor / forge under that gate, and co-lands the live paged MMU.
>
> **★ The architecture is SETTLED, not relitigated.** Per the BINDING AMENDMENT
> (`FINDINGS-s3-two-supervisor.md` §★): pure REPLACE for the **synchronous** seams (NK→host: `bctr`@
> `0x5031a8b8` / `sc` / Execute68k — the no-yield property makes a shim redundant) **PLUS a permanent
> EXT-injection shim** for the **asynchronous** seam (host device-IRQ → NK EXT vector — pure REPLACE
> SEVERS this when it retires `deliver_pending_dec_exception()` + `g_exc_entry_table`). This plan does NOT
> re-pick the architecture; it sequences the build.
>
> **Scope law (carried, NOT relitigated):** Route A SETTLED; program shape (S1→S3→S4) SETTLED; Q1
> (resident supervisor, no handoff) SETTLED; co-residency-impossible SETTLED; REPLACE+EXT-shim is the
> BINDING pick. CO-OWN / yield-fully / thin-shim are scored-out and NOT reopened.
>
> **★ S3-impl START preconditions (BINDING, from the verdict §6 + Task-0 M4):** (1) S2 landed (loader +
> OF-CI + Core99 DT + `/mmu` handoff — the NK must be *reached* before it is *hosted*); (2) the
> multi-entry-transaction (atomicity) spike landed (`cd7a62b6`). T1–T4 may start on S2-landed alone; **T5
> (the live-MMU co-land) additionally requires the atomicity spike + a T4 install-runs pre-flight.**

## Goal (PASS-vs-DIAGNOSTIC)

**PASS (falsifiable).** SheepShaver STOPS forging the NanoKernel's outputs and RUNS the real,
md5-verified NanoKernel-v02.27 as the live, permanently-resident, paged, translated-mode PPC supervisor,
behind `SS_M18_NK_SUPERVISOR ∧ MachineProfileIsNewWorld()` (default OFF), paravirtual PROVABLY
byte-identical. PASSES when all of:
1. **T1–T4 land GREEN under the gate** — master gate exists (default OFF); the EXT-injection shim built +
   re-pointed at the NK's real KDP-installed vectors; the synthetic supervisor
   (`deliver_pending_dec_exception()` + `g_exc_entry_table`) retired under the gate; scheduler YIELDs
   (SS-scheduler-ticks=0 while NK live); the forge (`rom_patches.cpp:716` + `glue:2806/2804/2918`) retired
   so the NK's real install runs — each CLEAN PPC recompile, `make test-jit`=100, paravirtual
   byte-identical via REAL `make e2e`.
2. **T5 lands GREEN** — the live paged-MMU window co-lands via the ATOMIC one-step
   `mach_vm_map(FIXED|OVERWRITE)` per-region form (NOT Dolphin unmap-then-map), reusing
   `paged_mmu_translate()`; the gated newworld boot reaches G3.a–c.
3. **The gate flips LAST** — only after T1–T5 are merged-green at default-OFF; **revert-on-red = BRANCH
   revert** (the restructure merges regardless of the flag's eventual default).

**The G3.a–d gates (falsifiable-in-advance):**
- **G3.a** — probe boot: NK reaches DEC `0x50313200` with the SS scheduler structurally inert
  (SS-scheduler-ticks=0; the NK re-arms its own decrementer `mtspr 0x16`@`0x50313234`). Identity/forge
  FAILS by construction. *(T3 + observed at T5 boot.)*
- **G3.b** — NK exception entries are the live handlers (parked-PC: EXT `0x50314880` / SC `0x50314ac0` /
  DEC `0x50313200` / PROGRAM `0x50314700`); `g_exc_entry_table` yields. *(T2 + T3, observed at T5 boot.)*
- **G3.c** — Execute68k-pair disposition resolved: NK reaches 68k via its own ECB/KDP dispatch through
  `bctr`@`0x5031a8b8` and SS `execute_68k` is dead under the gate (the `glue:1504-1505` read accounted
  for), OR proven still-driven. *(T4 retirement, observed at T5 boot — the positive probe Task-0 owed.)*
- **G3.d** — paravirtual byte-identical via REAL `make e2e` + multi-run soak (`SS_E2E_RUNS=N` median±CV%)
  — NOT the inertness substitute. **revert-on-red = BRANCH revert.** Checked at EVERY task gate T1–T5.

**PASS taxonomy:**
- **GREEN-PASS** = T1–T5 green AND the gated newworld boot reaches G3.a/b/c with paravirtual G3.d
  byte-identical → S3 complete, hand to S4.
- **RESIDUE-PASS** = the restructure lands + paravirtual byte-identical, but the gated boot stalls before a
  G3 landmark (e.g. pre-NK-install, the rig exposure Task-0 flagged) → a finding, not a green light: the
  restructure MERGES (branch-revert keeps paravirtual safe), the gate stays OFF, the stall is a named
  residue, coordinator re-bands the boot-bringup.

**DIAGNOSTIC (never a gate):** which SS pieces REPLACED vs YIELDED; the Execute68k disposition; how far the
gated NK boot got; remap counts at T5. `0xDEADBEEF` where a real PA/vector is expected is ESCALATED.

## Authoritative inputs

| Doc | Role |
|---|---|
| `FINDINGS-s3-two-supervisor.md` (verdict + ★ amendment) | Architecture: REPLACE + permanent EXT-injection shim; the device-IRQ→NK-EXT seam; the RETIRE/KEEP/YIELD list; the atomic `mach_vm_map(FIXED|OVERWRITE)` directive; the owed probe (G3.a/c → S3-impl's own boot). |
| `FINDINGS-s3-nk-ownership.md` | NK = sole resident owner of DEC/exceptions/68k; KDP+0x360 vector table (runtime-installed); `bctr`@0x5031a8b8. |
| `FINDINGS-s3-ss-reusemap.md` | KEEP/TOGGLE/RETIRE/YIELD map + the 3 collisions + the env-gate census; the exact file:line surface. |
| `2026-06-15-ss-m18-s3-two-supervisor-task0.md` rev-2 | Gates G3.a–d, env-gate, file-ownership LAW, Stop-rule, PEM-mask LAW, the `sc` sub-wall. |
| `2026-06-15-ss-m18-s1-shm-arena-spike.md` §5+appendix | The SHM-arena spike (PROVEN) + OWED O1–O6 (esp O4 mixed-perm 4×4KB, O1 concurrent-reader) — the T5 live-operation residual. |
| `MMU-NANOKERNEL-INSEPARABILITY.md` (CANONICAL) | Why the live MMU co-lands inside S3. |
| `2026-06-14-ss-m18-s1-impl-paged-mmu.md` (EXEMPLAR) | The template + the `paged_mmu_translate()` surface T5 reuses; NATMEM separate-allocation truth. |
| `MILESTONE-WORKFLOW.md` §2/§4/§6 | The machine; baseline-is-part-of-the-gate; env-on-first/flip-LAST/revert-on-red; QEMU-as-oracle LAW. |

## Codebase facts (carried — RE-VERIFY at impl; drift ±1–5)

- **Master gate to CREATE:** `SS_M18_NK_SUPERVISOR` (default OFF) ∧ `MachineProfileIsNewWorld()`
  (`machine/machine_profile.cpp:96-99`). Boot-time mode-select ONLY; MUST NOT become a per-block/per-access
  branch. Precedent: the newworld arm `ppc-cpu.cpp:1991` (`check_spcflags` body `:1972`).
- **`exc_core.cpp` — KEEP (PEM masks = LAW).** `ExcEnter:12`/`ExcRfi:104`/`ExcDeliveryDecision:58` survive.
  **Retirement at the CALL-SITE/gate, NEVER in mask math.** T2 re-USES `ExcEnter` re-pointed at the NK
  table.
- **`sheepshaver_glue.cpp`:** `g_exc_entry_table:142` (RETIRE/bypass under gate; consumed at EXT `:1254`);
  `deliver_pending_dec_exception():1122` (RETIRE; polled `:1689`; DEC ExcEnter `:1342`);
  `SheepExcExtSetPending`/host-IRQ latch `:270,325` (the device-IRQ source T2 re-routes — M14 wall);
  `execute_68k:1461`, pair read `:1504-1505` (forged `:3140-3141` → `0x50480000/0x50460000`, SS's own JIT
  emulator, NOT ROM `0x50360000`) RETIRE; forge `:2806/2804/2918` RETIRE; `interrupt():1002` MUTE;
  `ppc_cpu` static `:1683` KEEP; `SheepExcDeliverPending():1687` lives HERE not ppc-cpu.cpp.
- **`ppc-cpu.cpp` hook:** `check_spcflags()` body `:1972`; newworld arm `:1991`; the
  `SheepExcDeliverPending()` call site `:~2027` (the spcflag poll — EXT-injection poll + DEC-retirement gate).
- **Scheduler (YIELD):** `virt_clock.cpp:108 VirtClockDECPending` (advisory under NK), `:71
  VirtClockWriteDEC` (suppress host `on_dec_write`), `event_sched.cpp:66 process_timers` (quiescent). NK
  DEC loop `0x50313200` is the live rescheduler.
- **`rom_patches.cpp:716` `SS_NW_TRAMPOLINE` reg-fixup forge** (`:729-900+`): RETIRE under the gate. Census:
  `SS_NW_TRAMPOLINE` DEAD when master ON; `SS_NW_MM_SWITCH`/`MM_POOL` `:867` INHERIT iff NK respects
  `0x68ff5800`; `SS_NW_EE_RISER` `:2576` INHERIT/RETIRE the stub.
- **NK real vectors (G3.b):** EXT `0x50314880`/SC `0x50314ac0`/DEC `0x50313200`/PROGRAM `0x50314700`,
  installed into the runtime **KDP+0x360** at boot. SC `-1/-2/-3` fast-traps retire SS `sc`-as-illegal
  (Path A double-increment, `HANDOFF:299-313` — moot once the real vector owns `sc`; fixing the increment
  ALONE is NOT closure, Stop-rule #5).
- **`paged_mmu_translate()` (S1-mechanism DONE):** `machine/paged_mmu.cpp`, the
  `(SR[16],BAT[16],HIGH_BAT[16],SDR1,EA)→PA` core T5 reuses.
- **NATMEM = SEPARATE fixed mach allocations** (`main_unix.cpp`): RAM `:1974`, ROM `:2000`. T5 remaps the
  separate sub-ranges (Dolphin SEPARATE-arena, NOT MEM_BULK).
- **Parcel provenance:** NK md5 `61c176e90b6365e84e5c660d703e56af`; ROM md5 `66210b4f71df8a580eb175f52b9d0f88`.

## The seam contradiction the EXT-injection shim resolves (read before T2)

`FINDINGS-s3-two-supervisor.md` §★ flagged the apparent contradiction (RETIRE
`deliver_pending_dec_exception()` + `g_exc_entry_table` vs KEEP ExcEnter). The resolution is T2's spine:
**RETIRE the synthetic delivery-decision; KEEP the vectoring mechanism (PEM=LAW); ADD a bounded
EXT-injection poll** on exactly the async device→NK-EXT seam — poll the host-IRQ pending flag in the
spcflag path (`ppc-cpu.cpp:~2027`), resolve the NK's REAL EXT vector from `KDP+0x360`, inject via
`ExcEnter` **re-pointed at the NK table** (NOT `g_exc_entry_table`). Without T2, pure REPLACE severs the
only device-IRQ→CPU-EXT path (the bare core has no real EXT pin to the device models — that wiring IS the
polled host flag). **T2 is load-bearing; sequence it BEFORE any retirement (T3) that removes the old path.**

## Task breakdown (SERIALIZED, gated, LOWEST-RISK-FIRST; one task in flight per file set)

> **File-ownership LAW (sole-in-flight).** Owned set: `exc_core.cpp`, `sheepshaver_glue.cpp`,
> `event_sched.cpp`+`virt_clock.cpp`, `rom_patches.cpp`, `ppc-cpu.cpp` (+ `main_unix.cpp` for T5).
> T1→T2→T3→T4→T5 SERIALIZED, no two concurrently edit an owned file. Each env-gated default OFF, falsifiable
> gate, structurally inert when gated off (paravirtual byte-identical). env-on-first, flip-LAST,
> revert-on-red = BRANCH revert.

### T1 — the master gate + scaffold (LOWEST RISK; inert when off)
- [ ] Re-verify `machine_profile.cpp:96-99` + the arm `ppc-cpu.cpp:1991`/body `:1972`.
- [ ] Define `SS_M18_NK_SUPERVISOR` (default OFF) ∧ `MachineProfileIsNewWorld()`, read ONCE at boot (mirror
      `:1991`). NOT a per-access branch.
- [ ] Add an inert scaffold guard at `ppc-cpu.cpp:~2027` (`if (g_nk_supervisor) { /* T2/T3 land here */ }`
      empty body) — paravirtual byte-identical; the gated path is reachable-but-inert.
- [ ] Audit `MachineProfileIsNewWorld()==false` at the gate SITE. CLEAN PPC recompile.
- **G(T1):** `make test-jit`=100; real `make e2e` A/B confirms no paravirtual divergence. Satisfies none of
  G3.a–c yet; establishes the gate G3.d polices all tasks.

### T2 — the EXT-injection shim (LOAD-BEARING)
- [ ] Re-verify the host-IRQ source (`glue:270,325`), the EXT ExcEnter site (`:1254`), `g_exc_entry_table:142`.
- [ ] Under the gate, in the spcflag poll (`ppc-cpu.cpp:~2027`): poll the host-IRQ pending flag (the KEPT
      device-model latch) instead of routing through `deliver_pending_dec_exception()`.
- [ ] Resolve the NK's REAL EXT vector from the live KDP table (`KDP+0x360`) / installed MSR[IP] prefix;
      cache it. **Do NOT hardcode `0x50314880`** (resolve from the live table — hardcoding is
      QEMU-as-oracle-class). Sentinel guard: resolved vector `0x0`/`0xDEADBEEF` ⇒ STOP (install hasn't run).
- [ ] Inject EXT via the KEPT `ExcEnter(..., EXC_EXTERNAL, <NK table>)` re-pointed at the NK vector, NOT
      `g_exc_entry_table`. KEEP exc_core mask math (LAW). CLEAN PPC recompile.
- **G(T2):** `make test-jit`=100; paravirtual byte-identical (gated OFF) via real `make e2e` A/B + soak; a
  gated-ON micro-assertion (an asserted device IRQ resolves to parked-PC `0x50314880` via the shim, NOT
  `g_exc_entry_table`) — if the live install hasn't run yet (pre-T4), the LIVE confirmation is owed to T5
  (record the LIMIT, do not fabricate). Satisfies the host→NK-EXT half of G3.b.

### T3 — retire the synthetic supervisor under the gate (DEPENDS ON T2)
- [ ] Under the gate, STOP calling `deliver_pending_dec_exception()` at `:~2027` — the NK's own DEC handler
      `0x50313200` runs. Bypass/null `g_exc_entry_table` (`:142`); mute `interrupt():1002`.
- [ ] YIELD the scheduler under the gate: `process_timers` quiescent; `VirtClockWriteDEC` suppress the host
      `on_dec_write`; `VirtClockDECPending` advisory only (delivery gate must NOT consult it).
- [ ] PEM-mask LAW: retirement at the call-site/gate ONLY. CLEAN PPC recompile.
- **G(T3):** `make test-jit`=100; paravirtual byte-identical (gated OFF) via real `make e2e`+soak;
  instrumented gated-ON: SS-scheduler-ticks=0 while NK live. Satisfies G3.a + the table-bypass half of G3.b.

### T4 — retire the forge under the gate so the NK's real install runs (DEPENDS ON T3)
- [ ] Under the gate, retire `rom_patches.cpp:716` reg-fixup (`:729-900+`) so the NK's real install writes
      stick; INHERIT `SS_NW_MM_SWITCH`/`MM_POOL` iff NK respects `0x68ff5800`.
- [ ] Retire the SDR1/HTAB/MSR forge (`glue:2806/2804/2918`) — the NK programs SDR1/BAT/SR live
      (`mtsrin`@`0x50315290`, BAT@`0x503152c4`, SR-swap MMIO@`0x50325894`).
- [ ] Retire the Execute68k pair (`glue:3140-3141`) + `execute_68k:1461` — the NK reaches 68k via
      `bctr`@`0x5031a8b8`; account for the `:1504-1505` read (must not fire under the gate).
- [ ] Confirm NK SC `0x50314ac0` owns `sc`; retire SS `sc`-as-illegal (NOT the double-increment fix alone).
      CLEAN PPC recompile.
- **G(T4):** `make test-jit`=100; paravirtual byte-identical (gated OFF)+soak; gated-ON probe (co-checked at
  T5): the NK install runs (SDR1 ≠ forged C value; real `mtsrin`/BAT writes present), NOT a pre-install
  stall. Satisfies G3.c + the forge-retirement precondition T5 needs.

### T5 — the live-MMU co-land (HEAVIEST; DEPENDS ON T4 letting the install run; +atomicity spike + T4 pre-flight)
- [ ] Re-verify NATMEM (`main_unix.cpp` RAM `:1974`/ROM `:2000`) + the `paged_mmu_translate()` signature.
- [ ] Build the Dolphin SEPARATE-SHM-arena window via the ATOMIC one-step `mach_vm_map(FIXED|OVERWRITE)`
      per-region form (PROVEN `acd89dce`+`cd7a62b6`), NOT unmap-then-map. Assert region disjointness.
- [ ] Wire the window against the NK-installed live tables, reusing `paged_mmu_translate()`. Keep the JIT
      RMEMBASE sites bit-identical (remap paid at the NK's `mtspr`/`mtsrin`/HTAB-store time).
- [ ] Carry spike OWED O1–O6 as the live-operation residual (O1 concurrent-reader atomicity, O3 latency
      under live `mtspr` churn, O4 mixed-perm 4×4KB fork, O5 MAP_JIT coexistence). If the boot shows NK
      dependence on dense mixed-perm 4KB PTEs over JIT-covered regions → softmmu, re-band, STOP-and-surface.
      CLEAN PPC recompile.
- **G(T5) (the real boot gate):** `make test-jit`=100; paravirtual byte-identical via REAL `make e2e` A/B +
  `SS_E2E_RUNS=N` soak + `make bench` delta (GUI boots+builds AUTHORIZED); G3.a (NK DEC `0x50313200`,
  SS-ticks=0); G3.b (NK vectors live); G3.c (Execute68k disposition); the EXT-shim corroborated live. All
  together → GREEN-PASS (or RESIDUE-PASS if the boot stalls pre-landmark — restructure merges, gate OFF).

## Acceptance discipline (env-on-first, flip-LAST, revert-on-red = BRANCH revert)

env-on-first (every task default-OFF, structurally inert, merges that way); flip-LAST
(`SS_M18_NK_SUPERVISOR=ON` only after T1–T5 merged-green); **revert-on-red = BRANCH revert** (revert the
flag flip, not the merged restructure — what makes the dangerous flip safe); real `make e2e` paravirtual
A/B + soak at EVERY task gate (GUI boots+builds AUTHORIZED, slot boots only); probe-boot landmarks
G3.a/b/c + the EXT-shim live confirmation (the owed positive corroboration, now gettable); the
`0xDEADBEEF`=stop tripwire.

## Env-gate (the program's most dangerous flip)

`SS_M18_NK_SUPERVISOR` (default OFF) ∧ `MachineProfileIsNewWorld()`; once at boot (reuse `:1991`).
Transfers the supervisor role — paravirtual must be PROVABLY unreachable (audit
`MachineProfileIsNewWorld()==false` at the gate SITE + G3.d byte-identity at every gate). Not a per-access
branch.

## File ownership (LAW, sole-in-flight)

`machine/exc_core.cpp` (CONSUME ExcEnter ONLY; PEM=LAW never edited); `sheepshaver_glue.cpp` (EXT-inject /
host-IRQ re-route / deliver_pending gate-off / table bypass / interrupt mute / forge / Execute68k retire);
`machine/event_sched.cpp`+`virt_clock.cpp` (YIELD); `rom_patches.cpp` (forge retire); `ppc-cpu.cpp`
(`:1991`/`:~2027` hook); `Unix/main_unix.cpp` (T5 window). One task in flight per file set;
T1→T2→T3→T4→T5 SERIALIZED. Depends on S1-mechanism (DONE) + S2 landed (+ T5: atomicity spike + T4
pre-flight). **Fix budget: one ad-hoc fix to a PEM-mask LAW line = re-plan immediately.**

## Stop-rule (named in advance; one-iteration mechanics)

1. **#1 Re-forge a frozen output instead of running the real producer** ⇒ STOP. T4 retires the forge.
2. **PEM-mask LAW — one ad-hoc fix to an `exc_core.cpp` mask line = re-plan** (FIRST fix). T2/T3 retire at
   the call-site/gate.
3. **#3 Claim a handoff boundary the recon proved fictional** — CO-OWN/yield-fully/thin-shim stay
   scored-out; only the bounded async EXT-injection shim is sanctioned.
4. **No identity-only MMU when translation is load-bearing** (T5) — the live MMU hosts the NK's real tables.
5. **Fix the `sc` double-increment alone and claim closure** — closure = NK SC `0x50314ac0` live AND SS
   `sc`-as-illegal retired (T4).
6. **`0xDEADBEEF`/sentinel where a real PA/vector/KDP entry is expected ⇒ STOP.**
7. **Claim a fictional boot.** Gated boot stalls pre-NK-install ⇒ RESIDUE-PASS recorded honestly, NOT a
   green light, NOT fabricated landmarks.
8. **#5 Unbounded disasm** ⇒ STOP, conservative residue. No whole-parcel disasm.
9. **#6 QEMU/hardcode-as-address-oracle** — resolve the NK EXT vector from the live KDP+0x360, never a
   hardcoded constant (T2).
10. **Flip the gate before the restructure is merged-green at default-OFF** — flip-LAST is BINDING.
11. **Bet on the T5 window before the atomicity spike + the T4 install-runs pre-flight.** Mixed-perm 4KB
    PTE dependence on JIT-covered regions → softmmu, re-band, STOP-and-surface.

**One-iteration rule.** Falsified pin → dated entry → ONE bounded re-pin → resume. SECOND falsification of
the same answer ⇒ STOP, re-plan.

## Self-review record — tensions FLAGGED FOR THE RED TEAM

1. **Can T2's EXT-shim resolve the NK's real EXT vector STATICALLY, or is it owed a boot?** The table is
   runtime (KDP+0x360; no static cluster in the parcel). Is a gated-ON micro-assertion at T2 (live
   confirmation owed to T5) a sufficient T2 gate, or must T2+T5 be ONE indivisible task (untestable until
   the install runs)?
2. **Does T4 retiring the forge make the NK install actually RUN, or stall pre-install like the rig?**
   (Discriminator-A Evidence B: QEMU stalled in OF→OS handoff pre-install.) Is "S2 landed" SUFFICIENT, or
   does T4 need its OWN pre-flight probe (does the install run at all under the gate) before T5? **The
   sharpest sequencing risk.**
3. **Is T5 buildable BEFORE the install runs (S1⟂S3)?** The window CODE is buildable on S1-mechanism, but
   its GATE (the real boot reaching G3.a–c) is unmeasurable until T4 succeeds. Should T5 be PARKED (like
   S1-impl Task B) pending a T4 pre-flight boot + the atomicity spike?
4. **Does the EXT-shim survive paravirtual byte-identity?** The poll in the spcflag hot path — is gated-OFF
   structural inertness genuinely free, or does T2 need a `make bench` ns/insn check (not just G3.d)?

## Red-team record

*(empty — to be filled by the three-reviewer pre-implementation red-team: PROCESS + TECHNICAL + ADVERSARY. Voting list 1–11 submitted alongside.)*
